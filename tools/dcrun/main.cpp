// Runs a prepared Daytona.exe until the first thing that is not ported.
#include <mach-o/dyld.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>

#include "recomp/generated.hpp"
#include "recomp/host_files.hpp"
#include "recomp/loader.hpp"
#include "runtime/fail_closed.hpp"
#include "system/content.hpp"
#include "system/edition.hpp"
#include "system/exe_image.hpp"

namespace {

std::uintptr_t g_guest_base = 0;

// A guest load or store outside committed memory. Reports the guest address and
// exits like a fail-closed stop; only async-signal-safe calls.
void on_fault(int sig, siginfo_t* info, void* context) {
    const auto at = reinterpret_cast<std::uintptr_t>(info->si_addr);
    char buf[192];
    int n;
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
}

void usage() {
    std::fprintf(stderr, "usage: dcrun [--edition 2017|2019] [--content <folder>] <prepared-cache>\n");
}

}

int main(int argc, char** argv) {
    std::optional<dcusa::Edition> want;
    const char* path = nullptr;
    const char* content = nullptr;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--content") {
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
        dcusa::recomp::set_guest_files(install->install_root, install->game_dir.filename().string(), writable);
        std::fprintf(stderr, "content %s\n", install->game_dir.string().c_str());
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
