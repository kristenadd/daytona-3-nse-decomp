// The boot screen and the frame-rate overlay (boot_screen.hpp) without Core
// Graphics: the Linux build's, and the Mac's SDL test build's. The title is
// decoded with stb_image and the text drawn with stb_truetype; the picture
// follows boot_screen.mm (the title, a rounded bar in the swirl's colours, the
// label in wide outlined capitals, the build stamp).
//
// The label's font: font.ttf beside the title (boot_screen_font), else the
// first of a few common bold sans faces found on the system; with none, the
// screen has no text.
#include "gpu/boot_screen.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wall"
#pragma clang diagnostic ignored "-Wextra"
#pragma clang diagnostic ignored "-Wconversion"
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wshadow"
#pragma clang diagnostic ignored "-Wpedantic"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#pragma clang diagnostic ignored "-Wcast-qual"
#pragma clang diagnostic ignored "-Wdouble-promotion"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO_WARNINGS
#include "../../third_party/stb/stb_image.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "../../third_party/stb/stb_truetype.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace dcusa::gpu {

namespace {

constexpr int kWidth = 1920, kHeight = 1080;

struct Image {
    int w = 0, h = 0;
    std::vector<std::uint8_t> rgba;
};
Image g_title;

struct Font {
    std::vector<unsigned char> data;
    stbtt_fontinfo info{};
    bool ok = false;
    bool custom = false;  // given by the player: a regular weight, thickened
};
Font g_font;
bool g_font_tried = false;
std::string g_stamp;
std::vector<std::uint8_t> g_pixels, g_overlay;

bool load_font(const std::string& path, bool custom) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    Font f;
    f.data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    const int offset = stbtt_GetFontOffsetForIndex(f.data.data(), 0);
    if (offset < 0 || !stbtt_InitFont(&f.info, f.data.data(), offset)) return false;
    f.ok = true;
    f.custom = custom;
    g_font = std::move(f);
    return true;
}

const Font& font() {
    if (!g_font.ok && !g_font_tried) {
        g_font_tried = true;
        for (const char* p : {"/usr/share/fonts/TTF/DejaVuSans-Bold.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                              "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/noto/NotoSans-Bold.ttf",
                              "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf", "/usr/share/fonts/TTF/LiberationSans-Bold.ttf",
                              "/usr/share/fonts/liberation/LiberationSans-Bold.ttf",
                              "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
                              "/System/Library/Fonts/Supplemental/Arial Bold.ttf"})
            if (load_font(p, false)) break;
    }
    return g_font;
}

// One line of text as coverage (0..255), `size` the em size in pixels,
// stretched `stretch` times wider, `kern` extra pixels after each glyph.
struct Mask {
    int w = 0, h = 0, baseline = 0;
    std::vector<std::uint8_t> a;
};

Mask text_mask(const std::string& text, float size, float stretch, float kern, int pad) {
    Mask m;
    const Font& f = font();
    if (!f.ok || text.empty()) return m;
    const float scale = stbtt_ScaleForMappingEmToPixels(&f.info, size);
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(&f.info, &ascent, &descent, &gap);
    float width = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&f.info, static_cast<unsigned char>(text[i]), &adv, &lsb);
        width += static_cast<float>(adv) * scale * stretch + kern;
        if (i + 1 < text.size())
            width += static_cast<float>(stbtt_GetCodepointKernAdvance(&f.info, static_cast<unsigned char>(text[i]),
                                                                      static_cast<unsigned char>(text[i + 1]))) *
                     scale * stretch;
    }
    m.baseline = static_cast<int>(std::ceil(static_cast<float>(ascent) * scale)) + pad;
    m.w = static_cast<int>(std::ceil(width)) + 2 * pad;
    m.h = static_cast<int>(std::ceil(static_cast<float>(ascent - descent) * scale)) + 2 * pad;
    m.a.assign(static_cast<std::size_t>(m.w) * m.h, 0);
    float x = static_cast<float>(pad);
    for (std::size_t i = 0; i < text.size(); ++i) {
        const int ch = static_cast<unsigned char>(text[i]);
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&f.info, ch, &adv, &lsb);
        int x0, y0, x1, y1;
        const float shift = x - std::floor(x);
        stbtt_GetCodepointBitmapBoxSubpixel(&f.info, ch, scale * stretch, scale, shift, 0, &x0, &y0, &x1, &y1);
        const int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0) {
            std::vector<std::uint8_t> g(static_cast<std::size_t>(gw) * gh);
            stbtt_MakeCodepointBitmapSubpixel(&f.info, g.data(), gw, gh, gw, scale * stretch, scale, shift, 0, ch);
            const int ox = static_cast<int>(std::floor(x)) + x0, oy = m.baseline + y0;
            for (int yy = 0; yy < gh; ++yy)
                for (int xx = 0; xx < gw; ++xx) {
                    const int px = ox + xx, py = oy + yy;
                    if (px < 0 || py < 0 || px >= m.w || py >= m.h) continue;
                    std::uint8_t& dst = m.a[static_cast<std::size_t>(py) * m.w + px];
                    dst = std::max(dst, g[static_cast<std::size_t>(yy) * gw + xx]);
                }
        }
        x += static_cast<float>(adv) * scale * stretch + kern;
        if (i + 1 < text.size())
            x += static_cast<float>(stbtt_GetCodepointKernAdvance(&f.info, ch, static_cast<unsigned char>(text[i + 1]))) * scale *
                 stretch;
    }
    return m;
}

// The mask grown by `r` pixels (a round brush): an outline or a heavier face.
Mask dilate(const Mask& m, float r) {
    Mask out = m;
    if (r <= 0) return out;
    const int ri = static_cast<int>(std::ceil(r));
    for (int y = 0; y < m.h; ++y)
        for (int x = 0; x < m.w; ++x) {
            int best = 0;
            for (int dy = -ri; dy <= ri; ++dy)
                for (int dx = -ri; dx <= ri; ++dx) {
                    const float dist = std::sqrt(static_cast<float>(dx * dx + dy * dy));
                    if (dist > r + 0.5f) continue;
                    const int sx = x + dx, sy = y + dy;
                    if (sx < 0 || sy < 0 || sx >= m.w || sy >= m.h) continue;
                    const float weight = std::min(1.0f, r + 0.5f - dist);
                    best = std::max(best, static_cast<int>(static_cast<float>(m.a[static_cast<std::size_t>(sy) * m.w + sx]) * weight));
                }
            out.a[static_cast<std::size_t>(y) * m.w + x] = static_cast<std::uint8_t>(best);
        }
    return out;
}

Mask blur(const Mask& m, int r) {
    Mask out = m;
    if (r <= 0) return out;
    std::vector<int> tmp(m.a.size());
    for (int y = 0; y < m.h; ++y)
        for (int x = 0; x < m.w; ++x) {
            int s = 0, n = 0;
            for (int dx = -r; dx <= r; ++dx)
                if (x + dx >= 0 && x + dx < m.w) s += m.a[static_cast<std::size_t>(y) * m.w + x + dx], ++n;
            tmp[static_cast<std::size_t>(y) * m.w + x] = s / n;
        }
    for (int y = 0; y < m.h; ++y)
        for (int x = 0; x < m.w; ++x) {
            int s = 0, n = 0;
            for (int dy = -r; dy <= r; ++dy)
                if (y + dy >= 0 && y + dy < m.h) s += tmp[static_cast<std::size_t>(y + dy) * m.w + x], ++n;
            out.a[static_cast<std::size_t>(y) * m.w + x] = static_cast<std::uint8_t>(s / n);
        }
    return out;
}

// Premultiplied "source over" of one colour with coverage into RGBA8 rows.
void paint(std::vector<std::uint8_t>& px, int pw, int ph, int x, int y, const float c[4], float coverage) {
    if (x < 0 || y < 0 || x >= pw || y >= ph || coverage <= 0) return;
    const float a = c[3] * std::min(1.0f, coverage);
    std::uint8_t* p = &px[(static_cast<std::size_t>(y) * pw + x) * 4];
    for (int i = 0; i < 3; ++i) p[i] = static_cast<std::uint8_t>(std::lround(c[i] * 255.0f * a + p[i] * (1.0f - a)));
    p[3] = static_cast<std::uint8_t>(std::lround(255.0f * a + p[3] * (1.0f - a)));
}

void paint_mask(std::vector<std::uint8_t>& px, int pw, int ph, const Mask& m, int x, int y, const float c[4]) {
    for (int yy = 0; yy < m.h; ++yy)
        for (int xx = 0; xx < m.w; ++xx) {
            const std::uint8_t v = m.a[static_cast<std::size_t>(yy) * m.w + xx];
            if (v) paint(px, pw, ph, x + xx, y + yy, c, static_cast<float>(v) / 255.0f);
        }
}

// Coverage of a pill (a rounded rectangle with radius h / 2) at a pixel centre.
float pill_coverage(float px, float py, float x, float y, float w, float h, float radius) {
    const float cx = std::clamp(px, x + radius, x + w - radius);
    const float cy = std::clamp(py, y + radius, y + h - radius);
    const float dist = std::sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy)) - radius;
    return std::clamp(0.5f - dist, 0.0f, 1.0f);
}

}  // namespace

bool boot_screen_load(const std::string& path) {
    if (!g_title.rgba.empty()) return true;
    int w = 0, h = 0, n = 0;
    stbi_uc* data = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (data == nullptr) return false;
    g_title.w = w;
    g_title.h = h;
    g_title.rgba.assign(data, data + static_cast<std::size_t>(w) * h * 4);
    stbi_image_free(data);
    return true;
}

bool boot_screen_loaded() { return !g_title.rgba.empty(); }

bool boot_screen_font(const std::string& path) {
    if (g_font.ok && g_font.custom) return true;
    return load_font(path, true);
}

void boot_screen_stamp(const std::string& text) { g_stamp = text; }

const std::vector<std::uint8_t>& overlay_compose(const std::string& text, std::uint32_t* width, std::uint32_t* height) {
    constexpr int kH = 48, w = 1024;
    const Mask m = text_mask(text, 28, 1.0f, 1.0f, 0);
    const float pill = std::min<float>(w, static_cast<float>(m.w) + 32);
    g_overlay.assign(static_cast<std::size_t>(w) * kH * 4, 0);
    const float shade[4] = {0, 0, 0, 0.55f}, white[4] = {1, 1, 1, 1};
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < static_cast<int>(pill) + 1; ++x)
            paint(g_overlay, w, kH, x, y, shade, pill_coverage(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, 0, 0, pill, kH, 12));
    paint_mask(g_overlay, w, kH, m, 16, 34 - m.baseline, white);
    *width = w;
    *height = kH;
    return g_overlay;
}

const std::vector<std::uint8_t>& boot_screen_compose(int percent, const std::string& label, std::uint32_t* width,
                                                     std::uint32_t* height) {
    *width = kWidth;
    *height = kHeight;
    g_pixels.assign(static_cast<std::size_t>(kWidth) * kHeight * 4, 255);
    if (!g_title.rgba.empty()) {  // scaled to fill, bilinear
        for (int y = 0; y < kHeight; ++y) {
            const float sy = (static_cast<float>(y) + 0.5f) * static_cast<float>(g_title.h) / kHeight - 0.5f;
            const int y0 = std::clamp(static_cast<int>(std::floor(sy)), 0, g_title.h - 1), y1 = std::min(y0 + 1, g_title.h - 1);
            const float fy = std::clamp(sy - static_cast<float>(y0), 0.0f, 1.0f);
            for (int x = 0; x < kWidth; ++x) {
                const float sx = (static_cast<float>(x) + 0.5f) * static_cast<float>(g_title.w) / kWidth - 0.5f;
                const int x0 = std::clamp(static_cast<int>(std::floor(sx)), 0, g_title.w - 1), x1 = std::min(x0 + 1, g_title.w - 1);
                const float fx = std::clamp(sx - static_cast<float>(x0), 0.0f, 1.0f);
                auto at = [&](int xx, int yy, int c) {
                    return static_cast<float>(g_title.rgba[(static_cast<std::size_t>(yy) * g_title.w + xx) * 4 + c]);
                };
                std::uint8_t* p = &g_pixels[(static_cast<std::size_t>(y) * kWidth + x) * 4];
                float rgb[3], alpha;
                for (int c = 0; c < 4; ++c) {
                    const float v = (at(x0, y0, c) * (1 - fx) + at(x1, y0, c) * fx) * (1 - fy) +
                                    (at(x0, y1, c) * (1 - fx) + at(x1, y1, c) * fx) * fy;
                    if (c < 3) rgb[c] = v;
                    else alpha = v / 255.0f;
                }
                for (int c = 0; c < 3; ++c) p[c] = static_cast<std::uint8_t>(std::lround(rgb[c] * alpha + 255.0f * (1 - alpha)));
            }
        }
    }
    // The bar, in the white band between "NEW SEASON EDITION" and the swirl.
    const float tx = 560, ty = 842, tw = 800, th = 18;
    const float track[4] = {0.89f, 0.89f, 0.91f, 1};
    const float red[3] = {0.91f, 0.15f, 0.29f}, orange[3] = {0.97f, 0.58f, 0.12f}, yellow[3] = {1.0f, 0.83f, 0.0f};
    const double fraction = percent < 0 ? 1.0 : std::min(100, percent) / 100.0;
    const float fill_w = fraction > 0 ? std::max<float>(th, tw * static_cast<float>(fraction)) : 0;
    for (int y = static_cast<int>(ty) - 1; y <= static_cast<int>(ty + th) + 1; ++y)
        for (int x = static_cast<int>(tx) - 1; x <= static_cast<int>(tx + tw) + 1; ++x) {
            const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
            paint(g_pixels, kWidth, kHeight, x, y, track, pill_coverage(px, py, tx, ty, tw, th, th / 2));
            if (fill_w > 0) {
                // The gradient spans the whole track, so a point's colour stays put as the bar grows.
                const float t = std::clamp((px - tx) / tw, 0.0f, 1.0f);
                float c[4] = {0, 0, 0, 1};
                for (int i = 0; i < 3; ++i)
                    c[i] = t < 0.5f ? red[i] + (orange[i] - red[i]) * (t / 0.5f) : orange[i] + (yellow[i] - orange[i]) * ((t - 0.5f) / 0.5f);
                paint(g_pixels, kWidth, kHeight, x, y, c, pill_coverage(px, py, tx, ty, fill_w, th, th / 2));
            }
        }
    // The label, centred under the bar: wide bold white capitals, outlined.
    if (!label.empty() && font().ok) {
        std::string upper = label;
        for (char& ch : upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        const float size = g_font.custom ? 24.0f : 22.0f;
        Mask face = text_mask(upper, size, 1.2f, 4.0f * 1.2f, 6);
        if (g_font.custom) face = dilate(face, 0.5f);
        const Mask edge = dilate(face, size * 0.08f);
        const Mask shadow = blur(edge, 3);
        const int x = (kWidth - face.w) / 2, y = static_cast<int>(ty + th + 32) - face.baseline;
        const float shade[4] = {0, 0, 0, 0.35f}, edge_color[4] = {0.4f, 0.4f, 0.4f, 1}, white[4] = {1, 1, 1, 1};
        paint_mask(g_pixels, kWidth, kHeight, shadow, x, y + 1, shade);
        paint_mask(g_pixels, kWidth, kHeight, edge, x, y, edge_color);
        paint_mask(g_pixels, kWidth, kHeight, face, x, y, white);
    }
    if (!g_stamp.empty() && font().ok) {
        const Mask m = text_mask(g_stamp, 15, 1.0f, 1.0f, 0);
        const float grey[4] = {0.55f, 0.55f, 0.55f, 1};
        paint_mask(g_pixels, kWidth, kHeight, m, (kWidth - m.w) / 2, static_cast<int>(ty + th + 62) - m.baseline, grey);
    }
    return g_pixels;
}

}  // namespace dcusa::gpu
