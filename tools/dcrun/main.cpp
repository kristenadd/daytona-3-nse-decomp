// Runs a prepared Daytona.exe until the first thing that is not ported.
#include <mach-o/dyld.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/ucontext.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "audio/null_output.hpp"
#include "gpu/metal.hpp"
#include "platform/macos/core_audio_output.hpp"
#include "platform/macos/game_window.hpp"
#include "recomp/d3d11.hpp"
#include "recomp/generated.hpp"
#include "recomp/host_files.hpp"
#include "recomp/loader.hpp"
#include "recomp/win32.hpp"
#include "platform/macos/gamecontroller_pad.hpp"
#include "runtime/pad_script.hpp"
#include "runtime/fail_closed.hpp"
#include "system/content.hpp"
#include "system/edition.hpp"
#include "system/exe_image.hpp"

namespace {

std::uintptr_t g_guest_base = 0;

// DCUSA_WRITE_WATCH=<hex guest address>: who writes that dword. After
// DCUSA_WRITE_WATCH_AFTER seconds (default 30) the host page holding it is
// kept read-only (re-protected every 2 ms); a write to the page faults, and a
// write to the dword itself is logged once per host pc ("write watch: pc ...",
// for atos), then the page is opened and the write goes on. A debugging aid.
std::uint32_t g_watch_addr = 0;
std::uintptr_t g_watch_page = 0;
std::atomic<bool> g_watch_on{false};
std::uint64_t g_watch_seen[64];
std::atomic<int> g_watch_n{0};

// A guest load or store outside committed memory. Reports the guest address and
// exits like a fail-closed stop; only async-signal-safe calls.
void on_fault(int sig, siginfo_t* info, void* context) {
    const auto at = reinterpret_cast<std::uintptr_t>(info->si_addr);
    char buf[192];
    int n;
    if (g_watch_on.load(std::memory_order_relaxed) && (at & ~static_cast<std::uintptr_t>(16383)) == g_watch_page) {
#if defined(__aarch64__)
        const auto* uc = static_cast<const ucontext_t*>(context);
        const std::uint64_t pc = uc->uc_mcontext->__ss.__pc;
        const std::uint32_t guest = static_cast<std::uint32_t>(at - g_guest_base);
        if (guest >= g_watch_addr && guest < g_watch_addr + 4) {
            bool seen = false;
            const int count = g_watch_n.load(std::memory_order_relaxed);
            for (int i = 0; i < count && i < 64; ++i)
                if (g_watch_seen[i] == pc) seen = true;
            if (!seen && count < 64) {
                g_watch_seen[count] = pc;
                g_watch_n.store(count + 1, std::memory_order_relaxed);
                n = std::snprintf(buf, sizeof buf, "write watch: %08X written at host pc %llx lr %llx slide %llx\n", guest,
                                  static_cast<unsigned long long>(pc), static_cast<unsigned long long>(uc->uc_mcontext->__ss.__lr),
                                  static_cast<unsigned long long>(_dyld_get_image_vmaddr_slide(0)));
                if (n > 0) (void)!write(2, buf, static_cast<std::size_t>(n));
            }
        }
#endif
        mprotect(reinterpret_cast<void*>(g_watch_page), 16384, PROT_READ | PROT_WRITE);
        return;
    }
    if (g_guest_base != 0 && at >= g_guest_base && at - g_guest_base < (1ull << 32) + (1ull << 16)) {
        n = std::snprintf(buf, sizeof buf, "guest fault (signal %d): access to unmapped guest address %08llX\n", sig,
                          static_cast<unsigned long long>(at - g_guest_base));
    } else {
        n = std::snprintf(buf, sizeof buf, "host fault (signal %d) at %p\n", sig, info->si_addr);
    }
    if (n > 0) (void)!write(2, buf, static_cast<std::size_t>(n));
#if defined(__aarch64__)
    // For `atos -o dcrun -l <0x100000000 + slide> <pc> <lr>`: names the translated function.
    const auto* uc = static_cast<const ucontext_t*>(context);
    n = std::snprintf(buf, sizeof buf, "host pc %llx lr %llx slide %llx\n",
                      static_cast<unsigned long long>(uc->uc_mcontext->__ss.__pc),
                      static_cast<unsigned long long>(uc->uc_mcontext->__ss.__lr),
                      static_cast<unsigned long long>(_dyld_get_image_vmaddr_slide(0)));
    if (n > 0) (void)!write(2, buf, static_cast<std::size_t>(n));
#else
    (void)context;
#endif
    _exit(1);
}

void install_fault_report(const std::uint8_t* base) {
    g_guest_base = reinterpret_cast<std::uintptr_t>(base);
    // Worker threads have their own stacks; an alternate stack only covers this one.
    struct sigaction sa {};
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGSEGV, &sa, nullptr);
    if (const char* w = std::getenv("DCUSA_WRITE_WATCH")) {
        g_watch_addr = static_cast<std::uint32_t>(std::strtoul(w, nullptr, 16));
        g_watch_page = (g_guest_base + g_watch_addr) & ~static_cast<std::uintptr_t>(16383);
        const int after = std::getenv("DCUSA_WRITE_WATCH_AFTER") ? std::atoi(std::getenv("DCUSA_WRITE_WATCH_AFTER")) : 30;
        std::thread([after] {
            std::this_thread::sleep_for(std::chrono::seconds(after));
            g_watch_on = true;
            std::fprintf(stderr, "write watch: on for %08X\n", g_watch_addr);
            for (;;) {
                mprotect(reinterpret_cast<void*>(g_watch_page), 16384, PROT_READ);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }).detach();
    }
}

// The cabinet's operator menu belongs to the shell (Shell.exe), which does not
// run here; what it changes is ShellData.ini (credits / free play, volumes,
// calibration) and GameSettings.ini (difficulty, race length, car and track
// select; the game reads it as ../ShellData/GameSettings.ini).
// --shell-setting Section.Key=Value edits the container's copy (made from the
// install's the first time), which the game reads in place of the install's.
// Section GameSettings goes to GameSettings.ini, every other to ShellData.ini.
// Returns false with a message on a malformed setting.
bool apply_shell_setting(const std::filesystem::path& install_root, const std::filesystem::path& writable,
                         const std::string& setting) {
    const std::size_t dot = setting.find('.'), eq = setting.find('=');
    if (dot == std::string::npos || eq == std::string::npos || dot > eq || dot == 0 || eq == dot + 1) {
        std::fprintf(stderr, "--shell-setting wants Section.Key=Value, got %s\n", setting.c_str());
        return false;
    }
    const std::string section = setting.substr(0, dot), key = setting.substr(dot + 1, eq - dot - 1),
                      value = setting.substr(eq + 1);
    const char* const file = section == "GameSettings" ? "GameSettings.ini" : "ShellData.ini";
    const std::filesystem::path copy = writable / "ShellData" / file;
    std::error_code ec;
    if (!std::filesystem::exists(copy)) {
        std::filesystem::create_directories(copy.parent_path(), ec);
        std::filesystem::copy_file(install_root / "ShellData" / file, copy, ec);
    }
    std::ifstream in(copy);
    std::vector<std::string> lines;
    for (std::string l; std::getline(in, l);) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
    }
    in.close();
    // Replace the key in its section, or add it (and the section) at the end.
    bool in_section = false, done = false;
    std::size_t section_end = lines.size();
    for (std::size_t i = 0; i < lines.size() && !done; ++i) {
        const std::string& l = lines[i];
        if (!l.empty() && l.front() == '[') {
            if (in_section) section_end = i;
            in_section = l == "[" + section + "]";
            if (in_section) section_end = lines.size();
            continue;
        }
        if (in_section && l.compare(0, key.size() + 1, key + "=") == 0) {
            lines[i] = key + "=" + value;
            done = true;
        }
    }
    if (!done) {
        bool have = false;
        for (std::size_t i = 0; i < lines.size(); ++i)
            if (lines[i] == "[" + section + "]") {
                have = true;
                std::size_t at = i + 1;
                while (at < lines.size() && (lines[at].empty() || lines[at].front() != '[')) ++at;
                lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at), key + "=" + value);
                break;
            }
        if (!have) {
            lines.push_back("[" + section + "]");
            lines.push_back(key + "=" + value);
        }
    }
    (void)section_end;
    std::ofstream out(copy, std::ios::binary);
    for (const std::string& l : lines) out << l << "\r\n";
    std::fprintf(stderr, "shell setting %s [%s] %s=%s\n", file, section.c_str(), key.c_str(), value.c_str());
    return static_cast<bool>(out);
}

void usage() {
    std::fprintf(stderr,
                 "usage: dcrun [--edition 2017|2019] [--content <folder>] [--headless]\n"
                 "             [--shell-setting Section.Key=Value ...] <prepared-cache>\n"
                 "  Opens a window and plays; --headless (or DCUSA_WINDOW=0) runs offscreen.\n"
                 "  --shell-setting changes the operator settings in ShellData.ini, for example\n"
                 "  Credit.Freeplay=0 (coins) or 1 (free play), Audio.MusicVolume=80, and the\n"
                 "  game rules in GameSettings.ini: \"GameSettings.DIFFICULTY=HARD\",\n"
                 "  \"GameSettings.GAME LENGTH=LONG\".\n");
}

// A run needs Metal, a shader folder and translation of the shaders it has not
// seen; each is on unless the environment already says otherwise.
// The shader folder is <cache folder>/shaders: the game's HLSL is captured
// there on first use and translated during the run.
void windowed_defaults(const char* cache) {
    setenv("DCUSA_METAL", "1", 0);
    setenv("DCUSA_TRANSLATE", "1", 0);
    const std::filesystem::path shaders = std::filesystem::absolute(cache).parent_path() / "shaders";
    std::error_code ec;
    std::filesystem::create_directories(shaders, ec);
    if (!ec) setenv("DCUSA_SHADERS", shaders.c_str(), 0);
}

}

int main(int argc, char** argv) {
    // The game keeps hundreds of data files open. An app started from Finder
    // gets a soft limit of 256 open files (a shell, far more); past it opens
    // failed and the boot stopped. Raise it to what the system allows.
    if (rlimit lim{}; getrlimit(RLIMIT_NOFILE, &lim) == 0 && lim.rlim_cur < 10240) {
        lim.rlim_cur = std::min<rlim_t>(lim.rlim_max, 10240);
        setrlimit(RLIMIT_NOFILE, &lim);
    }
    std::optional<dcusa::Edition> want;
    const char* path = nullptr;
    const char* content = nullptr;
    bool headless = false;
    std::vector<std::string> shell_settings;
    if (const char* w = std::getenv("DCUSA_WINDOW"); w != nullptr && std::string(w) == "0") headless = true;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--headless") {
            headless = true;
        } else if (arg == "--shell-setting") {
            if (i + 1 >= argc) {
                usage();
                return 2;
            }
            shell_settings.push_back(argv[++i]);
        } else if (arg == "--content") {
            if (i + 1 >= argc) {
                usage();
                return 2;
            }
            content = argv[++i];
        } else if (arg == "--edition") {
            if (i + 1 >= argc) {
                usage();
                return 2;
            }
            want = dcusa::edition_from_id(argv[++i]);
            if (!want) {
                std::fprintf(stderr, "unknown edition (use 2017 or 2019)\n");
                return 2;
            }
        } else if (!path) {
            path = argv[i];
        } else {
            usage();
            return 2;
        }
    }
    if (!path) {
        usage();
        return 2;
    }
    windowed_defaults(path);  // headless runs draw with Metal too; DCUSA_METAL=0 selects the 2D reference

    dcusa::ExeImage image;
    std::string error;
    bool loaded = false;
    if (want) {
        loaded = dcusa::exe_image_load_cache(path, *want, image, &error);
        if (!loaded) std::fprintf(stderr, "%s\n", error.c_str());
    } else {
        for (dcusa::Edition edition : {dcusa::Edition::Nse2019, dcusa::Edition::Dcu2017}) {
            std::string one;
            if (dcusa::exe_image_load_cache(path, edition, image, &one)) {
                loaded = true;
                break;
            }
            error = std::string(dcusa::edition_info(edition).id) + ": " + one;
            std::fprintf(stderr, "%s\n", error.c_str());
        }
    }
    if (!loaded) return 1;

    dcusa::recomp::GuestProcess process;
    if (!process.load(image, &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    install_fault_report(process.cpu().mem);
    const dcusa::EditionInfo& info = dcusa::edition_info(image.edition);
    if (content) {
        const dcusa::InstallScan scan = dcusa::find_installs(content);
        const dcusa::GameInstall* install = dcusa::pick_install(scan, image.edition);
        if (!install) {
            std::fprintf(stderr, "no %.*s game folder under %s\n", static_cast<int>(info.id.size()), info.id.data(),
                         content);
            for (const std::string& problem : scan.problems) std::fprintf(stderr, "%s\n", problem.c_str());
            return 1;
        }
        const std::filesystem::path writable = std::filesystem::path(path).parent_path() / "container";
        std::error_code ec;
        std::filesystem::create_directories(writable, ec);
        for (const std::string& setting : shell_settings)
            if (!apply_shell_setting(install->install_root, writable, setting)) return 2;
        dcusa::recomp::set_guest_files(install->install_root, install->game_dir.filename().string(), writable);
        std::fprintf(stderr, "content %s\n", install->game_dir.string().c_str());
    }
    // The controls: DCUSA_PAD_SCRIPT=<file> plays a script (runtime/pad_script.hpp);
    // otherwise the first game controller, unless DCUSA_PAD=0.
    std::unique_ptr<dcusa::PadSource> pad;
    if (const char* script = std::getenv("DCUSA_PAD_SCRIPT")) {
        std::ifstream in(script);
        std::stringstream text;
        text << in.rdbuf();
        auto scripted = std::make_unique<dcusa::ScriptPad>();
        std::string why;
        if (!in || !scripted->parse(text.str(), &why)) {
            std::fprintf(stderr, "%s\n", why.empty() ? "cannot read DCUSA_PAD_SCRIPT" : why.c_str());
            return 1;
        }
        pad = std::move(scripted);
    } else if (const char* off = std::getenv("DCUSA_PAD"); off == nullptr || std::string(off) != "0") {
        pad = dcusa::make_gamecontroller_pad();
    }
    dcusa::recomp::set_pad_source(pad.get());
    std::unique_ptr<dcusa::GameWindow> window;
    if (!headless) {
        if (!dcusa::gpu::enabled()) {
            std::fprintf(stderr, "no Metal device or no translated shaders (DCUSA_SHADERS); use --headless\n");
            return 1;
        }
        window = dcusa::make_game_window("Daytona Championship USA", 1280, 720);
        dcusa::gpu::set_layer(window->layer());
        dcusa::recomp::set_presenter(window.get());
    }
    // Sound: the speakers for a windowed run. Headless, DCUSA_AUDIO=0 or
    // DCUSA_AUDIO_WAV=<file> use a device-less output paced in real time (the
    // last also records what the game played).
    std::unique_ptr<dcusa::AudioOutput> audio;
    {
        const char* wav = std::getenv("DCUSA_AUDIO_WAV");
        const char* on = std::getenv("DCUSA_AUDIO");
        const bool speakers = !headless && wav == nullptr && !(on != nullptr && std::string(on) == "0");
        if (speakers) {
            audio = dcusa::make_core_audio_output();
            if (!audio->start(dcusa::audio::DsMixer::kOutputRate, dcusa::audio::DsMixer::pull,
                              &dcusa::recomp::sound_mixer()))
                audio.reset();
        }
        if (!audio) {
            audio = dcusa::audio::make_null_output(wav != nullptr ? wav : "");
            audio->start(dcusa::audio::DsMixer::kOutputRate, dcusa::audio::DsMixer::pull, &dcusa::recomp::sound_mixer());
        }
    }
    std::fprintf(stderr, "%.*s entry %08X, %zu imports\n", static_cast<int>(info.id.size()), info.id.data(),
                 static_cast<unsigned>(process.entry()), process.imports().size());
    try {
        process.start(dcusa::recomp::translated_entries());
    } catch (const dcusa::FailClosed& stopped) {
        std::fprintf(stderr, "%s\n", stopped.report().describe().c_str());
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
    std::fprintf(stderr, "entry returned\n");
    return 0;
}
