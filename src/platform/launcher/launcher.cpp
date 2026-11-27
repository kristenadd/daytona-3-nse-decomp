#include "platform/launcher/launcher.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wconversion"
#pragma clang diagnostic ignored "-Wshadow"
#pragma clang diagnostic ignored "-Wcast-qual"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#pragma clang diagnostic ignored "-Wdouble-promotion"
#endif
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "../../../third_party/stb/stb_truetype.h"
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "../../../third_party/stb/stb_image.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include "platform/launcher/assets.hpp"
#include "platform/launcher/sound_bank.hpp"
#include "system/content.hpp"
#include "system/shell_settings.hpp"

namespace dcusa::launcher {
namespace {

constexpr float kH = 800;
constexpr float kItalic = 0.21f;  // the lean of headings and menu items

// ---- colours ----

struct Color {
    float r, g, b, a;
};
Color rgb(unsigned hex, float a = 1.0f) {
    return {((hex >> 16) & 255) / 255.0f, ((hex >> 8) & 255) / 255.0f, (hex & 255) / 255.0f, a};
}
Color with_alpha(Color c, float a) { return {c.r, c.g, c.b, c.a * a}; }
const Color kWhite = rgb(0xFFFFFF), kBlack = rgb(0x000000), kYellow = rgb(0xFFD400), kOrange = rgb(0xF7931E),
            kRed = rgb(0xE3262F), kSkyTop = rgb(0x0E6FD6), kSkyBottom = rgb(0x041D4A), kGreen = rgb(0x26A54A),
            kPsBlue = rgb(0x6E8FE6), kPsRed = rgb(0xE5546B);

struct Pt {
    float x, y;
};

// ---- the game's rules (GameSettings.ini): the game's own table, default first ----

struct Rule {
    const char* key;
    std::vector<const char*> options;
};

const std::vector<Rule>& rules() {
    static const std::vector<Rule> r = {
        {"DIFFICULTY", {"MEDIUM", "VERY EASY", "EASY", "HARD", "VERY HARD"}},
        {"GAME LENGTH", {"MEDIUM", "VERY SHORT", "SHORT", "LONG", "VERY LONG", "CLASSIC MODE"}},
        {"CAMERA ENABLED", {"YES", "NO"}},
        {"INSTANT REMATCH", {"YES", "NO"}},
        {"START ON COIN ENTRY", {"NO", "YES"}},
        {"MENU TIMINGS", {"MEDIUM", "SHORT", "LONG"}},
        {"CAR SELECT", {"CHOOSE", "RANDOM", "1", "2", "3", "4", "5"}},
        {"TRACK SELECT", {"CHOOSE", "RANDOM", "EASY ONLY", "37_SPEED", "DINOSAUR", "SEASIDE", "DAYTONA", "LAKESIDE", "METRO"}},
        {"TRANSMISSION SELECT", {"ON", "OFF"}},
        {"CHAMPIONSHIP MODE", {"YES", "NO"}},
        {"CHAMPIONSHIP CONTINUE", {"YES", "NO"}},
        {"AUTO DRIVE", {"YES", "NO"}},
        {"KIDS ONLY MODE", {"NO", "YES"}},
        {"TOURNAMENT MODE", {"OFF", "ON", "EXCLUSIVE"}},
        {"TOURNAMENT DURATION", {"14", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "15",
                                 "16", "17", "18", "19", "20", "21", "22", "23", "24", "25", "26", "27", "28", "29", "30"}},
        {"TOURNAMENT TYPE", {"CUSTOM", "CHAMPIONSHIP"}},
        {"CUSTOM TOURNAMENT CAR", {"1", "2", "3", "4", "5"}},
        {"CUSTOM TOURNAMENT TRACK", {"37_SPEED", "DINOSAUR", "SEASIDE", "DAYTONA", "LAKESIDE", "METRO"}},
        {"CUSTOM TOURNAMENT TRANSMISSION", {"CHOOSE", "MANUAL", "AUTOMATIC"}},
        {"TOURNAMENT CONTINUE", {"NO", "YES"}},
        {"TOURNAMENT DATE FORMAT", {"MM/DD/YY", "YY/MM/DD", "DD/MM/YY"}},
        {"DRESS CODE", {"OFF", "ON"}},
        {"DEFAULT CAMERA", {"3", "1", "2", "4"}},
        {"LANGUAGE", {"ENGLISH", "FRENCH", "ITALIAN", "SPANISH", "RUSSIAN", "TURKISH", "PORTUGUESE"}},
    };
    return r;
}

const char* const kVolumeKeys[3] = {"MusicVolume", "SFXVolume", "VoiceVolume"};

// The settings OPTIONS edits, for one edition.
struct GameOptions {
    bool freeplay = true;
    bool has_audio = false;  // the 2017 file has no [Audio]
    int volume[3] = {80, 100, 90};
    std::vector<int> rule, rule_initial;
    std::vector<bool> rule_in_file;

    void load(const LauncherEdition& e) {
        const auto get = [&](const char* section, const char* key) {
            return shell_setting(e.install_root, e.container, section, key);
        };
        const auto fp = get("Credit", "Freeplay");
        freeplay = !fp || *fp != "0";
        has_audio = get("Audio", "MusicVolume").has_value();
        for (int i = 0; i < 3; ++i)
            if (const auto v = get("Audio", kVolumeKeys[i])) volume[i] = std::clamp(std::atoi(v->c_str()), 0, 100);
        const auto& r = rules();
        rule.assign(r.size(), 0);
        rule_in_file.assign(r.size(), false);
        for (std::size_t i = 0; i < r.size(); ++i) {
            const auto v = get("GameSettings", r[i].key);
            if (!v) continue;
            rule_in_file[i] = true;
            for (std::size_t k = 0; k < r[i].options.size(); ++k)
                if (*v == r[i].options[k]) rule[i] = static_cast<int>(k);
        }
        rule_initial = rule;
    }

    // A rule goes into the file when it is there already or was changed.
    bool save(const LauncherEdition& e) {
        const auto set = [&](const char* section, const std::string& key, const std::string& value) {
            return set_shell_setting(e.install_root, e.container, section, key, value);
        };
        bool ok = set("Credit", "Freeplay", freeplay ? "1" : "0");
        if (has_audio)
            for (int i = 0; i < 3; ++i) ok = set("Audio", kVolumeKeys[i], std::to_string(volume[i])) && ok;
        const auto& r = rules();
        for (std::size_t i = 0; i < r.size(); ++i) {
            if (!rule_in_file[i] && rule[i] == rule_initial[i]) continue;
            ok = set("GameSettings", r[i].key, r[i].options[static_cast<std::size_t>(rule[i])]) && ok;
            rule_in_file[i] = true;
        }
        return ok;
    }
};

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Friendlier names for the rule values.
std::string value_name(const std::string& key, const std::string& v) {
    static const struct {
        const char* id;
        const char* name;
    } tracks[] = {{"37_SPEED", "THREE SEVEN SPEEDWAY"}, {"DINOSAUR", "DINOSAUR CANYON"},
                  {"SEASIDE", "SEASIDE STREET GALAXY"}, {"DAYTONA", "DAYTONA SPEEDWAY"},
                  {"LAKESIDE", "LAKESIDE CASTLE"},       {"METRO", "METRO CITY"}};
    for (const auto& t : tracks)
        if (v == t.id) return t.name;
    if ((key == "CAR SELECT" || key == "CUSTOM TOURNAMENT CAR") && v.size() == 1) return "CAR " + v;
    if (key == "DEFAULT CAMERA") return "VIEW " + v;
    if (key == "TOURNAMENT DURATION") return v + (v == "1" ? " DAY" : " DAYS");
    return v;
}

// ---- the lists ----

enum class Ctl { None, FreePlay, Volume, Rule, PadBind, DefaultLayout };

struct Row {
    enum Kind { Header, Toggle, Choice, Slider, Button, Done } kind;
    std::string label;
    Ctl ctl = Ctl::None;
    int index = -1;
};

std::vector<Row> option_rows(const GameOptions& o) {
    std::vector<Row> rows;
    rows.push_back({Row::Header, "GAME"});
    rows.push_back({Row::Toggle, "FREE PLAY", Ctl::FreePlay});
    if (o.has_audio) {
        rows.push_back({Row::Slider, "MUSIC VOLUME", Ctl::Volume, 0});
        rows.push_back({Row::Slider, "EFFECTS VOLUME", Ctl::Volume, 1});
        rows.push_back({Row::Slider, "VOICE VOLUME", Ctl::Volume, 2});
    }
    rows.push_back({Row::Header, "RULES"});
    const auto& r = rules();
    for (std::size_t i = 0; i < r.size(); ++i) rows.push_back({Row::Choice, r[i].key, Ctl::Rule, static_cast<int>(i)});
    rows.push_back({Row::Done, "DONE"});
    return rows;
}

std::vector<Row> control_rows() {
    std::vector<Row> rows;
    rows.push_back({Row::Header, "CONTROLLER"});
    for (int i = 0; i < kPadActions; ++i)
        rows.push_back({Row::Choice, upper(pad_action_label(static_cast<PadAction>(i))), Ctl::PadBind, i});
    rows.push_back({Row::Button, "DEFAULT LAYOUT", Ctl::DefaultLayout});
    rows.push_back({Row::Done, "DONE"});
    return rows;
}

// ---- input ----

enum class Glyphs { Keyboard, Xbox, PlayStation, Nintendo };

// ---- sound ----

enum class Sound { Move, Decide, Cancel, Start, Count };

struct Voice {
    Pcm pcm;
    std::size_t pos = 0;
    bool playing = false;
};

std::vector<std::uint8_t> read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

// ---- fonts ----

struct Font {
    int texture = -1;
    stbtt_packedchar chars[96] = {};
    int atlas = 0;
    float size = 0, ascent = 0, inv = 1;
};

struct Launcher::State {
    std::vector<LauncherEdition> eds;
    int ed = 0;
    bool can_quit = true;

    // Display.
    int width_px = 1280, height_px = 800;
    float W = 1280, scale = 1, off_x = 0, off_y = 0;
    float baked_scale = 0;
    std::vector<Texture> textures;
    int generation = 0;
    Font title_font, heading_font, hint_font, row_font, small_font;
    int logo = -1;
    float logo_w = 0, logo_h = 0;
    DrawList list;
    int cur_texture = -2;
    bool clip = false;
    float clip_rect[4] = {};

    // Screens.
    enum Screen { Title, Options, Controls } screen = Title;
    int title_sel = 1;  // 0 edition, 1 START, 2 OPTIONS, 3 CONTROLS
    GameOptions opts;
    PadBindings bindings;
    bool dirty = false, bindings_dirty = false;
    std::vector<Row> rows;
    int row_sel = 1;
    float scroll = 0;
    std::string note;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    float fade_start = 0;
    bool starting = false, exiting = false;
    float start_at = 0, exit_at = 0;
    Result result = Result::Running;
    float quiet_until = 0;
    bool auto_start = false;  // testing: START after a second

    // Input.
    Glyphs glyphs = Glyphs::Keyboard;
    bool forced_glyphs = false;
    GamepadState prev;
    int repeat_dir = 0;
    float repeat_next = 0;
    // Press-to-map.
    int capture_row = -1;
    float capture_at = 0;
    GamepadState capture_base, capture_prev;

    // Sound.
    std::mutex audio;
    Voice voices[static_cast<int>(Sound::Count)];
    Pcm music;
    std::size_t music_pos = 0;
    std::uint32_t loop_start = 0, loop_end = 0;
    float music_gain = 0;

    float t() const {
        return std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
    }
    const LauncherEdition& in() const { return eds[static_cast<std::size_t>(ed)]; }
    bool can_switch() const { return eds.size() > 1; }

    // ---- display ----

    void layout() {
        const float aspect = height_px > 0 ? static_cast<float>(width_px) / static_cast<float>(height_px) : 1.6f;
        if (aspect >= 1280.0f / 800.0f) {
            scale = static_cast<float>(height_px) / kH;
            W = static_cast<float>(width_px) / scale;
            off_x = off_y = 0;
        } else {  // narrower than 16:10: letterboxed
            scale = static_cast<float>(width_px) / 1280.0f;
            W = 1280;
            off_x = 0;
            off_y = (static_cast<float>(height_px) - kH * scale) / 2;
        }
        if (std::fabs(scale - baked_scale) > 0.05f) bake();
    }

    bool load_font(Font& f, std::span<const unsigned char> ttf, float units) {
        if (ttf.empty()) return false;
        f.size = units;
        const float px = units * scale;
        f.inv = 1.0f / scale;
        f.atlas = px > 90 ? 4096 : px > 40 ? 2048 : 1024;
        std::vector<unsigned char> alpha(static_cast<std::size_t>(f.atlas) * static_cast<std::size_t>(f.atlas));
        stbtt_pack_context pc;
        if (!stbtt_PackBegin(&pc, alpha.data(), f.atlas, f.atlas, 0, 2, nullptr)) return false;
        stbtt_PackSetOversampling(&pc, 2, 2);
        stbtt_PackFontRange(&pc, ttf.data(), 0, px, 32, 96, f.chars);
        stbtt_PackEnd(&pc);
        stbtt_fontinfo info;
        stbtt_InitFont(&info, ttf.data(), 0);
        int asc = 0, desc = 0, gap = 0;
        stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
        f.ascent = static_cast<float>(asc) * stbtt_ScaleForPixelHeight(&info, px) * f.inv;
        Texture t;
        t.width = t.height = f.atlas;
        t.rgba.resize(alpha.size() * 4);
        for (std::size_t i = 0; i < alpha.size(); ++i) {
            t.rgba[4 * i] = t.rgba[4 * i + 1] = t.rgba[4 * i + 2] = 255;
            t.rgba[4 * i + 3] = alpha[i];
        }
        f.texture = static_cast<int>(textures.size());
        textures.push_back(std::move(t));
        return true;
    }

    void bake() {
        baked_scale = scale;
        textures.clear();
        const auto title = asset("title_font"), body = asset("body_font");
        load_font(title_font, title, 60);
        load_font(heading_font, title, 40);
        load_font(hint_font, title, 24);
        load_font(row_font, body, 30);
        load_font(small_font, body, 22);
        const auto png = asset("logo");
        int w = 0, h = 0, n = 0;
        if (unsigned char* px = stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &w, &h, &n, 4)) {
            Texture t;
            t.width = w;
            t.height = h;
            t.rgba.assign(px, px + static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
            stbi_image_free(px);
            logo = static_cast<int>(textures.size());
            logo_w = static_cast<float>(w);
            logo_h = static_cast<float>(h);
            textures.push_back(std::move(t));
        }
        ++generation;
    }

    // ---- drawing ----

    void use(int texture) {
        // A new batch for another texture; set_clip forces one too.
        if (list.batches.empty() || cur_texture != texture) {
            Batch b;
            b.texture = texture;
            b.first = static_cast<std::uint32_t>(list.indices.size());
            b.clipped = clip;
            if (clip) {
                b.clip_x = clip_rect[0];
                b.clip_y = clip_rect[1];
                b.clip_w = clip_rect[2];
                b.clip_h = clip_rect[3];
            }
            list.batches.push_back(b);
            cur_texture = texture;
        }
    }
    void set_clip(bool on, float x = 0, float y = 0, float w = 0, float h = 0) {
        clip = on;
        clip_rect[0] = off_x + x * scale;
        clip_rect[1] = off_y + y * scale;
        clip_rect[2] = w * scale;
        clip_rect[3] = h * scale;
        cur_texture = -2;  // a new batch
    }
    std::uint32_t vtx(float x, float y, Color c, float u = 0, float v = 0) {
        list.vertices.push_back({off_x + x * scale, off_y + y * scale, c.r, c.g, c.b, c.a, u, v});
        return static_cast<std::uint32_t>(list.vertices.size() - 1);
    }
    void tri(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        list.indices.push_back(a);
        list.indices.push_back(b);
        list.indices.push_back(c);
        list.batches.back().count += 3;
    }
    void quad(Pt p0, Pt p1, Pt p2, Pt p3, Color c0, Color c1, Color c2, Color c3) {
        use(-1);
        const std::uint32_t a = vtx(p0.x, p0.y, c0), b = vtx(p1.x, p1.y, c1), c = vtx(p2.x, p2.y, c2),
                            d = vtx(p3.x, p3.y, c3);
        tri(a, b, c);
        tri(a, c, d);
    }
    void para(float x, float y, float w, float h, float slant, Color left, Color right) {
        quad({x + slant, y}, {x + w + slant, y}, {x + w, y + h}, {x, y + h}, left, right, right, left);
    }
    void para(float x, float y, float w, float h, float slant, Color c) { para(x, y, w, h, slant, c, c); }
    void rect(float x, float y, float w, float h, Color c) { quad({x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}, c, c, c, c); }
    void vgradient(float x, float y, float w, float h, Color top, Color bottom) {
        quad({x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}, top, top, bottom, bottom);
    }
    void fan(float cx, float cy, float r, Color c, int n = 40) {
        use(-1);
        const std::uint32_t centre = vtx(cx, cy, c);
        std::uint32_t last = vtx(cx + r, cy, c);
        for (int i = 1; i <= n; ++i) {
            const float a = 6.2831853f * static_cast<float>(i) / static_cast<float>(n);
            const std::uint32_t v = vtx(cx + r * std::cos(a), cy + r * std::sin(a), c);
            tri(centre, last, v);
            last = v;
        }
    }
    void ring(float cx, float cy, float r, float thick, Color c, int n = 40) {
        for (int i = 0; i < n; ++i) {
            const float a0 = 6.2831853f * static_cast<float>(i) / static_cast<float>(n);
            const float a1 = 6.2831853f * static_cast<float>(i + 1) / static_cast<float>(n);
            quad({cx + r * std::cos(a0), cy + r * std::sin(a0)}, {cx + r * std::cos(a1), cy + r * std::sin(a1)},
                 {cx + (r - thick) * std::cos(a1), cy + (r - thick) * std::sin(a1)},
                 {cx + (r - thick) * std::cos(a0), cy + (r - thick) * std::sin(a0)}, c, c, c, c);
        }
    }
    void segment(float x0, float y0, float x1, float y1, float thick, Color c) {
        const float dx = x1 - x0, dy = y1 - y0, len = std::sqrt(dx * dx + dy * dy);
        const float nx = -dy / len * thick / 2, ny = dx / len * thick / 2;
        quad({x0 + nx, y0 + ny}, {x1 + nx, y1 + ny}, {x1 - nx, y1 - ny}, {x0 - nx, y0 - ny}, c, c, c, c);
    }
    void triangle(Pt a, Pt b, Pt c, Color col) {
        use(-1);
        tri(vtx(a.x, a.y, col), vtx(b.x, b.y, col), vtx(c.x, c.y, col));
    }
    void image(int texture, float x, float y, float w, float h) {
        if (texture < 0) return;
        use(texture);
        const std::uint32_t a = vtx(x, y, kWhite, 0, 0), b = vtx(x + w, y, kWhite, 1, 0),
                            c = vtx(x + w, y + h, kWhite, 1, 1), d = vtx(x, y + h, kWhite, 0, 1);
        tri(a, b, c);
        tri(a, c, d);
    }

    float text_width(const Font& f, const std::string& s) const {
        float x = 0, y = 0;
        for (unsigned char ch : s) {
            if (ch < 32 || ch >= 128) continue;
            stbtt_aligned_quad q;
            stbtt_GetPackedQuad(f.chars, f.atlas, f.atlas, ch - 32, &x, &y, &q, 0);
        }
        return x * f.inv;
    }
    // `top` is the top of the line; `skew` leans it right.
    void text(const Font& f, const std::string& s, float x, float top, Color c, float skew = 0) {
        if (f.texture < 0) return;
        use(f.texture);
        const float base = top + f.ascent;
        float px = 0, py = 0;
        for (unsigned char ch : s) {
            if (ch < 32 || ch >= 128) continue;
            stbtt_aligned_quad q;
            stbtt_GetPackedQuad(f.chars, f.atlas, f.atlas, ch - 32, &px, &py, &q, 0);
            const float x0 = x + q.x0 * f.inv, x1 = x + q.x1 * f.inv, y0 = base + q.y0 * f.inv, y1 = base + q.y1 * f.inv;
            const float k0 = skew * (base - y0), k1 = skew * (base - y1);
            const std::uint32_t a = vtx(x0 + k0, y0, c, q.s0, q.t0), b = vtx(x1 + k0, y0, c, q.s1, q.t0),
                                d = vtx(x1 + k1, y1, c, q.s1, q.t1), e = vtx(x0 + k1, y1, c, q.s0, q.t1);
            tri(a, b, d);
            tri(a, d, e);
        }
    }
    void shadowed(const Font& f, const std::string& s, float x, float top, Color c, float skew = 0, float off = 3) {
        text(f, s, x + off, top + off, rgb(0x000000, 0.55f * c.a), skew);
        text(f, s, x, top, c, skew);
    }

    // ---- the screens ----

    void background() {
        vgradient(0, 0, W, kH, kSkyTop, kSkyBottom);
        const float drift = std::fmod(t() * 60.0f, 400.0f);
        for (int i = -1; i < static_cast<int>(W / 400) + 2; ++i)
            para(static_cast<float>(i) * 400.0f + drift - 200, 0, 90, kH, 260, rgb(0xFFFFFF, 0.035f));
        const struct {
            Color c;
            float y;
        } stripes[] = {{kYellow, 0}, {kOrange, 18}, {kRed, 36}};
        for (const auto& s : stripes)
            quad({0, 704 + s.y * 0.45f}, {W, 684 + s.y * 0.45f}, {W, 691 + s.y * 0.45f}, {0, 711 + s.y * 0.45f}, s.c, s.c,
                 s.c, s.c);
        for (const auto& s : stripes)
            quad({W * 0.55f, s.y * 0.8f}, {W, s.y * 0.8f}, {W, 14 + s.y * 0.8f}, {W * 0.55f + 40, 14 + s.y * 0.8f},
                 with_alpha(s.c, 0), s.c, s.c, with_alpha(s.c, 0));
        const float cy = kH - 66, sq = 11;
        rect(0, cy, W, 2 * sq, kWhite);
        for (int i = 0; static_cast<float>(i) * sq < W; ++i)
            for (int r = 0; r < 2; ++r)
                if ((i + r) % 2 == 0) rect(static_cast<float>(i) * sq, cy + static_cast<float>(r) * sq, sq, sq, kBlack);
        rect(0, cy + 2 * sq, W, kH - cy - 2 * sq, rgb(0x08080C));
    }

    float hint(float x, float y, bool confirm, const char* word) {
        const float w = text_width(hint_font, word);
        x -= w;
        text(hint_font, word, x, y - 13, kWhite, kItalic * 0.5f);
        x -= 44;
        const float cx = x + 16, cy = y;
        switch (glyphs) {
            case Glyphs::Keyboard: {
                const char* key = confirm ? "ENTER" : "ESC";
                const float kw = text_width(small_font, key) + 18;
                const float kx = x + 32 - kw;
                rect(kx, cy - 15, kw, 30, rgb(0xEBEBEB));
                text(small_font, key, kx + 9, cy - 14, kBlack);
                return kx - 34;
            }
            case Glyphs::Xbox: {
                fan(cx, cy, 16, confirm ? kGreen : kRed);
                const char* l = confirm ? "A" : "B";
                text(small_font, l, cx - text_width(small_font, l) / 2, cy - 14, kWhite);
                break;
            }
            case Glyphs::Nintendo: {
                fan(cx, cy, 16, rgb(0x3A3A3A));
                const char* l = confirm ? "B" : "A";
                text(small_font, l, cx - text_width(small_font, l) / 2, cy - 14, kWhite);
                break;
            }
            case Glyphs::PlayStation:
                fan(cx, cy, 16, rgb(0x202024));
                if (confirm) {
                    segment(cx - 7, cy - 7, cx + 7, cy + 7, 3.5f, kPsBlue);
                    segment(cx - 7, cy + 7, cx + 7, cy - 7, 3.5f, kPsBlue);
                } else {
                    ring(cx, cy, 8.5f, 3.2f, kPsRed);
                }
                break;
        }
        return x - 34;
    }

    void footer(bool change) {
        float x = W - 40;
        const float y = kH - 22;
        if (screen != Title || can_quit) x = hint(x, y, false, screen == Title ? "EXIT" : "BACK");
        x = hint(x, y, true, "SELECT");
        if (change) {
            const float w = text_width(hint_font, "CHANGE");
            text(hint_font, "CHANGE", x - w, y - 13, kWhite, kItalic * 0.5f);
            const float ax = x - w - 40;
            triangle({ax, y}, {ax + 12, y - 9}, {ax + 12, y + 9}, kYellow);
            triangle({ax + 30, y}, {ax + 18, y - 9}, {ax + 18, y + 9}, kYellow);
        }
    }

    static const char* edition_name(Edition e) {
        return e == Edition::Nse2019 ? "NEW SEASON EDITION" : "2017 EDITION";
    }

    void draw_title() {
        const float band_y = 106, band_h = 306;
        para(-60, band_y - 14, W + 120, 10, 40, kRed);
        para(-60, band_y, W + 120, band_h, 40, kWhite);
        para(-60, band_y + band_h + 4, W + 120, 10, 40, kYellow);
        if (logo >= 0) {
            const float w = 640, h = w * logo_h / logo_w;
            image(logo, (W - w) / 2 + 20, band_y + (band_h - h) / 2, w, h);
        }
        const char* edition = edition_name(in().choice.edition);
        {
            const float w = text_width(heading_font, edition), x = (W - w) / 2 + 6, y = band_y + band_h + 26;
            const float my = y + 25;
            if (title_sel == 0) {
                para(x - 70, y - 4, w + 136, 56, 14, kYellow, kOrange);
                text(heading_font, edition, x, y, kBlack, kItalic);
                triangle({x - 46, my}, {x - 30, my - 12}, {x - 30, my + 12}, kBlack);
                triangle({x + w + 54, my}, {x + w + 38, my - 12}, {x + w + 38, my + 12}, kBlack);
            } else {
                shadowed(heading_font, edition, x, y, kYellow, kItalic);
                if (can_switch()) {
                    triangle({x - 40, my}, {x - 28, my - 9}, {x - 28, my + 9}, rgb(0xFFFFFF, 0.8f));
                    triangle({x + w + 46, my}, {x + w + 34, my - 9}, {x + w + 34, my + 9}, rgb(0xFFFFFF, 0.8f));
                }
            }
        }
        if (!note.empty()) text(small_font, note, 40, kH - 36, kYellow, kItalic * 0.5f);
        const char* items[3] = {"START", "OPTIONS", "CONTROLS"};
        for (int i = 0; i < 3; ++i) {
            const float y = 484 + static_cast<float>(i) * 64;
            const float w = text_width(title_font, items[i]);
            const float x = (W - w) / 2;
            if (title_sel == i + 1) {
                para(x - 54, y, w + 96, 62, 16, kYellow, kOrange);
                const bool pulse = i == 0 && !starting && std::fmod(t(), 1.0f) > 0.6f;
                const bool flash = starting && std::fmod(t() * 8, 1.0f) > 0.5f;
                text(title_font, items[i], x, y, flash ? kWhite : pulse ? kRed : kBlack, kItalic);
            } else {
                shadowed(title_font, items[i], x, y, kWhite, kItalic);
            }
        }
        footer(title_sel == 0);
    }

    std::string row_value(const Row& r) const {
        switch (r.ctl) {
            case Ctl::FreePlay: return opts.freeplay ? "ON" : "OFF";
            case Ctl::Rule: {
                const Rule& rule = rules()[static_cast<std::size_t>(r.index)];
                return value_name(rule.key, rule.options[static_cast<std::size_t>(opts.rule[static_cast<std::size_t>(r.index)])]);
            }
            case Ctl::PadBind: return upper(pad_binding_label(bindings.action[r.index]));
            default: return "";
        }
    }

    void draw_list() {
        para(40, 34, 420, 64, 18, kRed, rgb(0xB0141C));
        shadowed(heading_font, screen == Options ? "OPTIONS" : "CONTROLS", 84, 44, kWhite, kItalic);
        shadowed(small_font, edition_name(in().choice.edition), 500, 44, kYellow, kItalic, 2);
        if (logo >= 0) {
            para(W - 300, 28, 250, 82, 18, kWhite);
            const float w = 176, h = w * logo_h / logo_w;
            image(logo, W - 260, 28 + (82 - h) / 2, w, h);
        }
        const float top = 128, row_h = 52, visible = 10;
        const float target = std::clamp(static_cast<float>(row_sel) - visible / 2 + 0.5f, 0.0f,
                                        std::max(0.0f, static_cast<float>(rows.size()) - visible));
        scroll += (target - scroll) * 0.25f;
        set_clip(true, 0, top - 4, W, visible * row_h + 6);
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const float y = top + (static_cast<float>(i) - scroll) * row_h;
            if (y < top - row_h || y > top + visible * row_h) continue;
            const Row& r = rows[i];
            const bool sel = static_cast<int>(i) == row_sel;
            const float x = 110, w = W - 2 * x - 20;
            if (r.kind == Row::Header) {
                shadowed(heading_font, r.label, x + 4, y + 2, kYellow, kItalic);
                const float lw = text_width(heading_font, r.label);
                para(x + lw + 30, y + 26, w - lw - 30, 4, 2, kYellow, with_alpha(kYellow, 0));
                continue;
            }
            if (sel) para(x, y + 4, w, row_h - 8, 12, kYellow, kOrange);
            else para(x, y + 4, w, row_h - 8, 12, rgb(0x000000, 0.32f), rgb(0x000000, 0.12f));
            const Color tc = sel ? kBlack : kWhite;
            const float ty = y + (row_h - row_font.size) / 2 - 2;
            if (sel) text(row_font, r.label, x + 34, ty, tc, kItalic * 0.6f);
            else shadowed(row_font, r.label, x + 34, ty, tc, kItalic * 0.6f, 2);
            const float right = x + w - 30;
            if (r.kind == Row::Slider) {
                const int v = opts.volume[r.index];
                const std::string num = std::to_string(v);
                const float bw = 300, bx = right - bw - 90, by = y + row_h / 2 - 7;
                para(bx, by, bw, 14, 6, sel ? rgb(0x000000, 0.25f) : rgb(0xFFFFFF, 0.2f));
                para(bx, by, bw * static_cast<float>(v) / 100.0f, 14, 6, sel ? kRed : kYellow, sel ? rgb(0xB0141C) : kOrange);
                text(row_font, num, right - text_width(row_font, num), ty, tc, kItalic * 0.6f);
                if (sel) {
                    triangle({bx - 34, y + row_h / 2}, {bx - 20, y + row_h / 2 - 10}, {bx - 20, y + row_h / 2 + 10}, kBlack);
                    triangle({right - 72, y + row_h / 2}, {right - 86, y + row_h / 2 - 10}, {right - 86, y + row_h / 2 + 10}, kBlack);
                }
            } else if (r.kind == Row::Toggle || r.kind == Row::Choice) {
                const bool waiting = static_cast<int>(i) == capture_row;
                const std::string v = waiting ? (std::fmod(t() * 2, 1.0f) > 0.3f ? "PRESS..." : "") : row_value(r);
                const float vw = text_width(row_font, v);
                const float vx = right - vw - (sel ? 34 : 0);
                if (sel) {
                    text(row_font, v, vx, ty, tc, kItalic * 0.6f);
                    triangle({vx - 34, y + row_h / 2}, {vx - 20, y + row_h / 2 - 10}, {vx - 20, y + row_h / 2 + 10}, kBlack);
                    triangle({right, y + row_h / 2}, {right - 14, y + row_h / 2 - 10}, {right - 14, y + row_h / 2 + 10}, kBlack);
                } else {
                    shadowed(row_font, v, vx, ty, kYellow, kItalic * 0.6f, 2);
                }
            }
        }
        set_clip(false);
        const float track_h = visible * row_h - 8, frac = visible / static_cast<float>(rows.size());
        const float pos = scroll / std::max(1.0f, static_cast<float>(rows.size()) - visible);
        rect(W - 70, top + 4, 6, track_h, rgb(0xFFFFFF, 0.2f));
        rect(W - 70, top + 4 + pos * track_h * (1 - frac), 6, track_h * frac, kYellow);
        if (capture_row >= 0) {
            const int left = 8 - static_cast<int>(t() - capture_at);
            text(small_font,
                 "PRESS THE PAD BUTTON, OR MOVE THE STICK OR TRIGGER, TO USE  (" + std::to_string(std::max(left, 0)) +
                     ")  ESC CANCELS",
                 40, kH - 36, kYellow, kItalic * 0.5f);
            return;
        }
        if (!note.empty()) text(small_font, note, 40, kH - 36, kYellow, kItalic * 0.5f);
        const Row::Kind k = rows[static_cast<std::size_t>(row_sel)].kind;
        footer(k != Row::Done && k != Row::Button);
    }

    // ---- sound ----

    void play(Sound s) {
        std::lock_guard<std::mutex> g(audio);
        Voice& v = voices[static_cast<int>(s)];
        v.pos = 0;
        v.playing = !v.pcm.samples.empty();
    }
    float seconds(Sound s) {
        const Pcm& p = voices[static_cast<int>(s)].pcm;
        return p.channels == 0 ? 0.0f : static_cast<float>(p.samples.size() / static_cast<std::size_t>(p.channels)) / kRate;
    }
    float launch_s() { return std::max(0.7f, seconds(Sound::Start)); }
    float exit_s() { return std::clamp(seconds(Sound::Cancel), 0.25f, 0.9f); }

    void load_sounds() {
        for (const LauncherEdition& e : eds) {
            const auto bank = read_file(e.game_dir / "rom" / "sound" / "csb" / "SE.csb");
            if (bank.empty()) continue;
            Pcm pcm[kBankSounds];
            if (!decode_bank(bank.data(), bank.size(), pcm)) continue;
            for (int i = 0; i < kBankSounds; ++i) voices[i].pcm = std::move(pcm[i]);
            const auto adx = read_file(e.game_dir / "rom" / "sound" / "adx" / "d1a_bgm01_sel.adx");
            if (!adx.empty() && decode_adx_file(adx.data(), adx.size(), music) && !music.samples.empty()) {
                const auto frames = static_cast<std::uint32_t>(music.samples.size() / static_cast<std::size_t>(music.channels));
                loop_start = music.loop_end > music.loop_start ? music.loop_start : 0;
                loop_end = music.loop_end > music.loop_start ? music.loop_end : frames;
            }
            break;
        }
    }

    // The menu music: 80% of the game's own music volume, faded with the screen.
    void update_music_gain() {
        const float volume = opts.has_audio ? static_cast<float>(opts.volume[0]) / 100.0f : 0.8f;
        float fade = 1.0f;
        if (starting) fade = 1.0f - std::min(1.0f, (t() - start_at) / launch_s());
        if (exiting) fade = 1.0f - std::min(1.0f, (t() - exit_at) / exit_s());
        std::lock_guard<std::mutex> g(audio);
        music_gain = 0.8f * volume * fade;
    }

    // ---- actions ----

    void load_edition() {
        opts.load(in());
        bindings = PadBindings::load(in().container / "controls.ini");
        dirty = bindings_dirty = false;
    }

    void switch_edition(int dir) {
        if (!can_switch()) return;
        save();
        ed = (ed + dir + static_cast<int>(eds.size())) % static_cast<int>(eds.size());
        load_edition();
        note.clear();
    }

    bool save() {
        bool ok = true;
        if (dirty) ok = opts.save(in()) && ok;
        if (bindings_dirty) ok = bindings.save(in().container / "controls.ini") && ok;
        const bool any = dirty || bindings_dirty;
        dirty = bindings_dirty = false;
        if (any) note = ok ? "SETTINGS SAVED" : "SETTINGS COULD NOT BE SAVED";
        return ok;
    }

    void open_list(Screen which) {
        screen = which;
        rows = which == Options ? option_rows(opts) : control_rows();
        row_sel = 1;
        scroll = 0;
        note.clear();
        fade_start = t();
    }

    void leave_list() {
        save();
        title_sel = screen == Options ? 2 : 3;
        screen = Title;
        fade_start = t();
    }

    void move(int dir) {
        int i = row_sel;
        do i = (i + dir + static_cast<int>(rows.size())) % static_cast<int>(rows.size());
        while (rows[static_cast<std::size_t>(i)].kind == Row::Header);
        row_sel = i;
    }

    bool change(int dir) {
        Row& r = rows[static_cast<std::size_t>(row_sel)];
        switch (r.ctl) {
            case Ctl::FreePlay: opts.freeplay = !opts.freeplay; dirty = true; return true;
            case Ctl::Volume: opts.volume[r.index] = std::clamp(opts.volume[r.index] + 5 * dir, 0, 100); dirty = true; return true;
            case Ctl::Rule: {
                const int n = static_cast<int>(rules()[static_cast<std::size_t>(r.index)].options.size());
                int& v = opts.rule[static_cast<std::size_t>(r.index)];
                v = (v + dir + n) % n;
                dirty = true;
                return true;
            }
            case Ctl::PadBind: {
                const int nb = static_cast<int>(PadButton::Count), na = static_cast<int>(PadAxis::Count), n = 1 + nb + na;
                const PadBinding& b = bindings.action[r.index];
                int i = b.kind == PadBinding::Button ? 1 + b.index : b.kind == PadBinding::Axis ? 1 + nb + b.index : 0;
                i = (i + dir + n) % n;
                bindings.action[r.index] =
                    i == 0    ? PadBinding{}
                    : i <= nb ? PadBinding{PadBinding::Button, static_cast<std::uint8_t>(i - 1)}
                              : PadBinding{PadBinding::Axis, static_cast<std::uint8_t>(i - 1 - nb)};
                bindings_dirty = true;
                return true;
            }
            default: return false;
        }
    }

    void capture_start() {
        capture_row = row_sel;
        capture_at = t();
        capture_base = capture_prev = prev;
    }
    void capture_end(const char* message, bool quiet = true) {
        capture_row = -1;
        if (quiet) quiet_until = t() + 0.4f;  // the press that mapped is not also a move
        note = message;
    }
    void capture_poll(const GamepadState& s) {
        if (capture_row < 0) return;
        if (t() - capture_at > 8.0f) {
            play(Sound::Cancel);
            return capture_end("NOTHING PRESSED");
        }
        const int action = rows[static_cast<std::size_t>(capture_row)].index;
        if (t() - capture_at > 0.2f && s.connected) {
            for (int b = 0; b < static_cast<int>(PadButton::Count); ++b) {
                const auto pb = static_cast<PadButton>(b);
                if (action == 0 || !pad_button_down(s, pb) || pad_button_down(capture_prev, pb)) continue;
                bindings.action[action] = {PadBinding::Button, static_cast<std::uint8_t>(b)};
                bindings_dirty = true;
                play(Sound::Decide);
                return capture_end("SET");
            }
            for (int a = 0; a < static_cast<int>(PadAxis::Count); ++a) {
                const auto pa = static_cast<PadAxis>(a);
                if (std::fabs(pad_axis_value(s, pa) - pad_axis_value(capture_base, pa)) < 0.6f) continue;
                const bool trigger = pa == PadAxis::LeftTrigger || pa == PadAxis::RightTrigger;
                if ((action == 0) == trigger) continue;  // steering takes a stick, the rest a trigger
                bindings.action[action] = {PadBinding::Axis, static_cast<std::uint8_t>(a)};
                bindings_dirty = true;
                play(Sound::Decide);
                return capture_end("SET");
            }
        }
        capture_prev = s;
    }

    void act(Action a, bool from_keyboard) {
        if (a == Action::None || starting || exiting || result != Result::Running) return;
        if (capture_row >= 0) {
            // Only the keyboard cancels: any pad button may be the one to map.
            if (a == Action::Back && from_keyboard) {
                play(Sound::Cancel);
                capture_end("", false);
            }
            return;
        }
        if (t() < quiet_until) return;
        if (screen == Title) {
            const int before = title_sel;
            switch (a) {
                case Action::Up:
                    if (title_sel > (can_switch() ? 0 : 1)) --title_sel;
                    break;
                case Action::Down:
                    if (title_sel < 3) ++title_sel;
                    break;
                case Action::Left:
                case Action::Right:
                    if (title_sel == 0 && can_switch()) {
                        switch_edition(a == Action::Left ? -1 : 1);
                        play(Sound::Move);
                    }
                    break;
                case Action::Confirm:
                    if (title_sel == 0) {
                        if (can_switch()) {
                            switch_edition(1);
                            play(Sound::Move);
                        }
                    } else if (title_sel == 1) {
                        save();
                        play(Sound::Start);  // the engine revs as the game starts
                        starting = true;
                        start_at = t();
                    } else {
                        play(Sound::Decide);
                        open_list(title_sel == 2 ? Options : Controls);
                    }
                    break;
                case Action::Back:
                    if (!can_quit) break;
                    play(Sound::Cancel);
                    exiting = true;
                    exit_at = t();
                    break;
                default: break;
            }
            if ((a == Action::Up || a == Action::Down) && title_sel != before) play(Sound::Move);
            return;
        }
        switch (a) {
            case Action::Up: move(-1); play(Sound::Move); break;
            case Action::Down: move(1); play(Sound::Move); break;
            case Action::Left:
            case Action::Right:
                if (change(a == Action::Left ? -1 : 1)) play(Sound::Move);
                break;
            case Action::Confirm: {
                play(Sound::Decide);
                const Row& r = rows[static_cast<std::size_t>(row_sel)];
                if (r.kind == Row::Done) leave_list();
                else if (r.ctl == Ctl::PadBind) capture_start();
                else if (r.ctl == Ctl::DefaultLayout) {
                    bindings = PadBindings::defaults();
                    bindings_dirty = true;
                    note = "DEFAULT LAYOUT";
                } else change(1);
                break;
            }
            case Action::Back: play(Sound::Cancel); leave_list(); break;
            default: break;
        }
    }

    // The pad: edges of A / B / Start and the d-pad, the stick with repeat.
    void pad(const GamepadState& s) {
        if (s.connected && !forced_glyphs) {
            const bool any = s.a || s.b || s.start || s.dpad_up || s.dpad_down || s.dpad_left || s.dpad_right ||
                             std::fabs(s.left_x) > 0.5f || std::fabs(s.left_y) > 0.5f;
            if (any)
                glyphs = s.labels == GamepadState::Labels::PlayStation ? Glyphs::PlayStation
                         : s.labels == GamepadState::Labels::Nintendo  ? Glyphs::Nintendo
                                                                       : Glyphs::Xbox;
        }
        capture_poll(s);
        if (capture_row < 0 && s.connected) {
            if (s.a && !prev.a) act(Action::Confirm, false);
            if (s.start && !prev.start && screen == Title) act(Action::Confirm, false);
            if (s.b && !prev.b) act(Action::Back, false);
            int dir = 0;
            if (s.dpad_up || s.left_y < -0.5f) dir = 1;
            else if (s.dpad_down || s.left_y > 0.5f) dir = 2;
            else if (s.dpad_left || s.left_x < -0.5f) dir = 3;
            else if (s.dpad_right || s.left_x > 0.5f) dir = 4;
            const Action acts[5] = {Action::None, Action::Up, Action::Down, Action::Left, Action::Right};
            if (dir == 0) {
                repeat_dir = 0;
            } else if (dir != repeat_dir) {
                repeat_dir = dir;
                repeat_next = t() + 0.38f;
                act(acts[dir], false);
            } else if (t() >= repeat_next) {
                repeat_next = t() + 0.11f;
                act(acts[dir], false);
            }
        }
        prev = s;
    }

    void advance() {
        if (auto_start && screen == Title && !starting && t() > 1.0f) {
            auto_start = false;
            title_sel = 1;
            act(Action::Confirm, false);
        }
        if (starting && t() - start_at > launch_s() + 0.05f) result = Result::Start;
        if (exiting && t() - exit_at > exit_s()) result = Result::Quit;
        update_music_gain();
    }

    const DrawList& draw() {
        list = {};
        cur_texture = -2;
        clip = false;
        background();
        if (screen == Title) draw_title();
        else draw_list();
        float fade = 1.0f - std::min(1.0f, (t() - fade_start) / 0.25f);
        if (starting) fade = std::min(1.0f, (t() - start_at) / launch_s());
        if (exiting) fade = std::min(1.0f, (t() - exit_at) / exit_s());
        if (fade > 0) {
            // The whole drawable, letterbox bars included.
            const float x0 = -off_x / scale, y0 = -off_y / scale;
            rect(x0, y0, static_cast<float>(width_px) / scale, static_cast<float>(height_px) / scale, rgb(0x000000, fade));
        }
        return list;
    }
};

Launcher::Launcher() : s_(std::make_unique<State>()) {}
Launcher::~Launcher() = default;

bool Launcher::init(std::vector<LauncherEdition> editions, int start, int width_px, int height_px, bool can_quit) {
    if (editions.empty()) return false;
    s_->eds = std::move(editions);
    s_->ed = std::clamp(start, 0, static_cast<int>(s_->eds.size()) - 1);
    s_->can_quit = can_quit;
    s_->width_px = std::max(width_px, 1);
    s_->height_px = std::max(height_px, 1);
    s_->layout();
    s_->load_edition();
    s_->load_sounds();
    s_->fade_start = s_->t();
    // Testing: the prompts of a pad, and a list opened at once.
    if (const char* g = std::getenv("DCUSA_LAUNCHER_PROMPTS")) set_prompts(g);
    if (const char* st = std::getenv("DCUSA_LAUNCHER_START")) s_->auto_start = std::strcmp(st, "0") != 0;
    if (const char* sc = std::getenv("DCUSA_LAUNCHER_SCREEN")) {
        if (std::strcmp(sc, "options") == 0) s_->open_list(State::Options);
        if (std::strcmp(sc, "controls") == 0) s_->open_list(State::Controls);
    }
    return true;
}

void Launcher::resize(int width_px, int height_px) {
    if (width_px <= 0 || height_px <= 0 || (width_px == s_->width_px && height_px == s_->height_px)) return;
    s_->width_px = width_px;
    s_->height_px = height_px;
    s_->layout();
}

const std::vector<Texture>& Launcher::textures() const { return s_->textures; }
int Launcher::texture_generation() const { return s_->generation; }

void Launcher::key(Action a) {
    if (!s_->forced_glyphs) s_->glyphs = Glyphs::Keyboard;
    s_->act(a, true);
}

void Launcher::pad(const GamepadState& s) { s_->pad(s); }

const DrawList& Launcher::frame() {
    s_->advance();
    return s_->draw();
}

void Launcher::mix(float* stereo, std::size_t frames) {
    std::fill(stereo, stereo + frames * 2, 0.0f);
    std::lock_guard<std::mutex> g(s_->audio);
    for (Voice& v : s_->voices) {
        if (!v.playing || v.pcm.channels == 0) continue;
        const std::size_t ch = static_cast<std::size_t>(v.pcm.channels), total = v.pcm.samples.size() / ch;
        for (std::size_t f = 0; f < frames && v.pos < total; ++f, ++v.pos) {
            const float l = v.pcm.samples[v.pos * ch] / 32768.0f;
            const float r = ch > 1 ? v.pcm.samples[v.pos * ch + 1] / 32768.0f : l;
            stereo[2 * f] += 0.8f * l;
            stereo[2 * f + 1] += 0.8f * r;
        }
        if (v.pos >= total) v.playing = false;
    }
    Pcm& m = s_->music;
    if (m.channels > 0 && s_->music_gain > 0 && s_->loop_end > 0) {
        const std::size_t ch = static_cast<std::size_t>(m.channels);
        for (std::size_t f = 0; f < frames; ++f) {
            const float l = m.samples[s_->music_pos * ch] / 32768.0f;
            const float r = ch > 1 ? m.samples[s_->music_pos * ch + 1] / 32768.0f : l;
            stereo[2 * f] += s_->music_gain * l;
            stereo[2 * f + 1] += s_->music_gain * r;
            if (++s_->music_pos >= s_->loop_end) s_->music_pos = s_->loop_start;  // the seamless loop
        }
    }
    for (std::size_t i = 0; i < frames * 2; ++i) stereo[i] = std::clamp(stereo[i], -1.0f, 1.0f);
}

Launcher::Result Launcher::result() const { return s_->result; }
int Launcher::chosen() const { return s_->ed; }
const LauncherEdition* Launcher::edition(int i) const {
    return i >= 0 && i < static_cast<int>(s_->eds.size()) ? &s_->eds[static_cast<std::size_t>(i)] : nullptr;
}

void Launcher::set_prompts(const char* which) {
    if (which == nullptr) return;
    s_->forced_glyphs = true;
    s_->glyphs = which[0] == 'p' ? Glyphs::PlayStation : which[0] == 'x' ? Glyphs::Xbox
                 : which[0] == 'n' ? Glyphs::Nintendo : Glyphs::Keyboard;
}

// ---- the editions ----

std::vector<LauncherEdition> launcher_editions(const std::vector<EditionChoice>& choices,
                                               const std::filesystem::path& content,
                                               const std::filesystem::path& cache_dir, int* start) {
    std::vector<LauncherEdition> out;
    const InstallScan scan = find_installs(content);
    for (const EditionChoice& c : choices) {
        LauncherEdition e;
        e.choice = c;
        if (const GameInstall* install = pick_install(scan, c.edition)) {
            e.install_root = install->install_root;
            e.game_dir = install->game_dir;
        }
        e.container = cache_dir / container_name(c.edition);
        out.push_back(std::move(e));
    }
    // 2019 first, as the game's own order.
    std::stable_sort(out.begin(), out.end(), [](const LauncherEdition& a, const LauncherEdition& b) {
        return a.choice.edition == Edition::Nse2019 && b.choice.edition != Edition::Nse2019;
    });
    if (start != nullptr) {
        *start = 0;
        std::ifstream in(cache_dir / "launcher.ini");
        std::string line;
        while (std::getline(in, line))
            if (line.rfind("Edition=", 0) == 0)
                for (std::size_t i = 0; i < out.size(); ++i)
                    if (line.substr(8, 4) == edition_info(out[i].choice.edition).id) *start = static_cast<int>(i);
    }
    return out;
}

void remember_edition(const std::filesystem::path& cache_dir, Edition e) {
    std::ofstream out(cache_dir / "launcher.ini", std::ios::trunc);
    out << "[Launcher]\nEdition=" << edition_info(e).id << "\n";
}

PadBindings edition_pad_bindings(const std::filesystem::path& container) {
    return PadBindings::load(container / "controls.ini");
}

}  // namespace dcusa::launcher
