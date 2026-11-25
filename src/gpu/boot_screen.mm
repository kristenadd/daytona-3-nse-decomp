#include "gpu/boot_screen.hpp"

#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <ImageIO/ImageIO.h>

#include <algorithm>
#include <cctype>

namespace dcusa::gpu {

namespace {

CGImageRef g_title = nullptr;
CGFontRef g_font = nullptr;  // boot_screen_font
std::string g_stamp;          // boot_screen_stamp
std::vector<std::uint8_t> g_overlay;

// One line of text at (x, baseline) in a top-down context, in the given
// font (or Helvetica Neue Bold) and colour; returns its width.
double draw_line(CGContextRef g, CGColorSpaceRef rgb, const std::string& text, CGFloat size, const CGFloat color[4],
                 CGFloat x, CGFloat baseline, bool measure_only = false) {
    CTFontRef font = g_font != nullptr ? CTFontCreateWithGraphicsFont(g_font, size, nullptr, nullptr)
                                       : CTFontCreateWithName(CFSTR("HelveticaNeue-Bold"), size, nullptr);
    CGColorRef c = CGColorCreate(rgb, color);
    const float kern_value = 1.0f;
    CFNumberRef kern = CFNumberCreate(nullptr, kCFNumberFloatType, &kern_value);
    const void* keys[3] = {kCTFontAttributeName, kCTForegroundColorAttributeName, kCTKernAttributeName};
    const void* values[3] = {font, c, kern};
    CFDictionaryRef attrs = CFDictionaryCreate(nullptr, keys, values, 3, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    CFStringRef str = CFStringCreateWithCString(nullptr, text.c_str(), kCFStringEncodingUTF8);
    CFAttributedStringRef astr = CFAttributedStringCreate(nullptr, str, attrs);
    CTLineRef line = CTLineCreateWithAttributedString(astr);
    const double w = CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr);
    if (!measure_only) {
        CGContextSaveGState(g);
        CGContextTranslateCTM(g, x, baseline);
        CGContextScaleCTM(g, 1, -1);
        CGContextSetTextPosition(g, 0, 0);
        CTLineDraw(line, g);
        CGContextRestoreGState(g);
    }
    CFRelease(line);
    CFRelease(astr);
    CFRelease(str);
    CFRelease(attrs);
    CFRelease(kern);
    CGColorRelease(c);
    CFRelease(font);
    return w;
}
std::vector<std::uint8_t> g_pixels;
constexpr std::uint32_t kWidth = 1920, kHeight = 1080;

// The title's colours: the swirl's red, orange and yellow.
const CGFloat kRed[4] = {0.91, 0.15, 0.29, 1}, kOrange[4] = {0.97, 0.58, 0.12, 1}, kYellow[4] = {1.0, 0.83, 0.0, 1};
const CGFloat kTrack[4] = {0.89, 0.89, 0.91, 1};

void rounded(CGContextRef g, CGRect r) {
    const CGFloat radius = r.size.height / 2;
    CGPathRef p = CGPathCreateWithRoundedRect(r, radius, radius, nullptr);
    CGContextAddPath(g, p);
    CGPathRelease(p);
}

}  // namespace

bool boot_screen_load(const std::string& path) {
    if (g_title != nullptr) return true;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(path.c_str()),
                                                           static_cast<CFIndex>(path.size()), false);
    if (url == nullptr) return false;
    CGImageSourceRef src = CGImageSourceCreateWithURL(url, nullptr);
    CFRelease(url);
    if (src == nullptr) return false;
    g_title = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
    CFRelease(src);
    return g_title != nullptr;
}

bool boot_screen_loaded() { return g_title != nullptr; }

void boot_screen_stamp(const std::string& text) { g_stamp = text; }

const std::vector<std::uint8_t>& overlay_compose(const std::string& text, std::uint32_t* width, std::uint32_t* height) {
    constexpr std::uint32_t kH = 48;
    constexpr CGFloat kSize = 28;
    CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
    const CGFloat white[4] = {1, 1, 1, 1};
    const double tw = draw_line(nullptr, rgb, text, kSize, white, 0, 0, true);
    constexpr std::uint32_t w = 640;  // fixed: the texture is made once; the pill fits the text
    const CGFloat pill = std::min<CGFloat>(w, static_cast<CGFloat>(tw) + 32);
    g_overlay.assign(static_cast<std::size_t>(w) * kH * 4, 0);
    CGContextRef g = CGBitmapContextCreate(g_overlay.data(), w, kH, 8, w * 4, rgb,
                                           kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    if (g != nullptr) {
        CGContextTranslateCTM(g, 0, kH);
        CGContextScaleCTM(g, 1, -1);
        CGContextSetRGBFillColor(g, 0, 0, 0, 0.55);
        CGPathRef p = CGPathCreateWithRoundedRect(CGRectMake(0, 0, pill, kH), 12, 12, nullptr);
        CGContextAddPath(g, p);
        CGContextFillPath(g);
        CGPathRelease(p);
        draw_line(g, rgb, text, kSize, white, 16, 34);
        CGContextRelease(g);
    }
    CGColorSpaceRelease(rgb);
    *width = w;
    *height = kH;
    return g_overlay;
}

bool boot_screen_font(const std::string& path) {
    if (g_font != nullptr) return true;
    CGDataProviderRef data = CGDataProviderCreateWithFilename(path.c_str());
    if (data == nullptr) return false;
    g_font = CGFontCreateWithDataProvider(data);
    CGDataProviderRelease(data);
    return g_font != nullptr;
}

const std::vector<std::uint8_t>& boot_screen_compose(int percent, const std::string& label, std::uint32_t* width,
                                                     std::uint32_t* height) {
    *width = kWidth;
    *height = kHeight;
    g_pixels.assign(static_cast<std::size_t>(kWidth) * kHeight * 4, 0);
    CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
    CGContextRef g = CGBitmapContextCreate(g_pixels.data(), kWidth, kHeight, 8, kWidth * 4, rgb,
                                           kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    if (g == nullptr) {
        CGColorSpaceRelease(rgb);
        return g_pixels;
    }
    // Drawn top-down, as the image is stored.
    CGContextTranslateCTM(g, 0, kHeight);
    CGContextScaleCTM(g, 1, -1);
    CGContextSetRGBFillColor(g, 1, 1, 1, 1);
    CGContextFillRect(g, CGRectMake(0, 0, kWidth, kHeight));
    if (g_title != nullptr) {
        // The image is upside down in a flipped context: flip it back locally.
        CGContextSaveGState(g);
        CGContextTranslateCTM(g, 0, kHeight);
        CGContextScaleCTM(g, 1, -1);
        CGContextDrawImage(g, CGRectMake(0, 0, kWidth, kHeight), g_title);
        CGContextRestoreGState(g);
    }

    // The bar, in the white band between "NEW SEASON EDITION" and the swirl.
    const CGRect track = CGRectMake(560, 842, 800, 18);
    CGContextSetFillColorSpace(g, rgb);
    CGContextSetFillColor(g, kTrack);
    rounded(g, track);
    CGContextFillPath(g);
    const double fraction = percent < 0 ? 1.0 : std::min(100, percent) / 100.0;
    if (fraction > 0) {
        CGRect fill = track;
        fill.size.width = std::max<CGFloat>(track.size.height, track.size.width * fraction);
        CGContextSaveGState(g);
        rounded(g, fill);
        CGContextClip(g);
        const CGFloat comps[12] = {kRed[0], kRed[1], kRed[2], 1, kOrange[0], kOrange[1], kOrange[2], 1,
                                   kYellow[0], kYellow[1], kYellow[2], 1};
        const CGFloat stops[3] = {0, 0.5, 1};
        CGGradientRef grad = CGGradientCreateWithColorComponents(rgb, comps, stops, 3);
        // The gradient spans the whole track, so the colour at a point stays put as the bar grows.
        CGContextDrawLinearGradient(g, grad, CGPointMake(track.origin.x, 0),
                                    CGPointMake(track.origin.x + track.size.width, 0), 0);
        CGGradientRelease(grad);
        CGContextRestoreGState(g);
    }

    // The label, centred under the bar, styled after the game's overlay text
    // ("PRESS START BUTTON"): wide bold white capitals (Helvetica Neue Bold
    // stretched 20% wider comes close to its face) with an outline, here a
    // lighter one than the game's navy (60% black) and a faint shadow.
    if (!label.empty()) {
        constexpr CGFloat kStretch = 1.2;
        // A given font (Eurostile, the game's face, is not a system font) is
        // a regular weight: its face is thickened with a stroke of its own colour.
        CTFontRef font = g_font != nullptr ? CTFontCreateWithGraphicsFont(g_font, 24, nullptr, nullptr)
                                           : CTFontCreateWithName(CFSTR("HelveticaNeue-Bold"), 22, nullptr);
        const float face_stroke = g_font != nullptr ? -4.0f : 0.0f;
        CGColorRef white = CGColorCreate(rgb, (const CGFloat[4]){1, 1, 1, 1});
        CGColorRef edge_color = CGColorCreate(rgb, (const CGFloat[4]){0.4, 0.4, 0.4, 1});
        CGColorRef shade = CGColorCreate(rgb, (const CGFloat[4]){0, 0, 0, 0.35});
        std::string upper = label;
        for (char& ch : upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        CFStringRef text = CFStringCreateWithCString(nullptr, upper.c_str(), kCFStringEncodingUTF8);
        // One line per pass: the outline (fill plus a stroke, as a % of the
        // font size) with the shadow, then the white face on top.
        auto make_line = [&](CGColorRef color, float stroke_width) {
            const float kern_value = 4.0f;
            CFNumberRef kern = CFNumberCreate(nullptr, kCFNumberFloatType, &kern_value);
            CFNumberRef stroke = CFNumberCreate(nullptr, kCFNumberFloatType, &stroke_width);
            const void* keys[5] = {kCTFontAttributeName, kCTForegroundColorAttributeName, kCTStrokeColorAttributeName,
                                   kCTStrokeWidthAttributeName, kCTKernAttributeName};
            const void* values[5] = {font, color, color, stroke, kern};
            CFDictionaryRef attrs = CFDictionaryCreate(nullptr, keys, values, 5, &kCFTypeDictionaryKeyCallBacks,
                                                       &kCFTypeDictionaryValueCallBacks);
            CFAttributedStringRef str = CFAttributedStringCreate(nullptr, text, attrs);
            CTLineRef line = CTLineCreateWithAttributedString(str);
            CFRelease(str);
            CFRelease(attrs);
            CFRelease(stroke);
            CFRelease(kern);
            return line;
        };
        CTLineRef edge = make_line(edge_color, -16.0f);
        CTLineRef face = make_line(white, face_stroke);
        const double w = CTLineGetTypographicBounds(face, nullptr, nullptr, nullptr) * kStretch;
        for (int pass = 0; pass < 2; ++pass) {
            // Text draws upward from its baseline: flip once more around the baseline.
            CGContextSaveGState(g);
            CGContextTranslateCTM(g, (kWidth - w) / 2, track.origin.y + track.size.height + 32);
            CGContextScaleCTM(g, kStretch, -1);
            if (pass == 0) CGContextSetShadowWithColor(g, CGSizeMake(0, -1), 4, shade);
            CGContextSetTextPosition(g, 0, 0);
            CTLineDraw(pass == 0 ? edge : face, g);
            CGContextRestoreGState(g);
        }
        CFRelease(edge);
        CFRelease(face);
        CFRelease(text);
        CGColorRelease(shade);
        CGColorRelease(edge_color);
        CGColorRelease(white);
        CFRelease(font);
    }
    // The build stamp, small and grey, centred under the label (the corners
    // are the swirl's).
    if (!g_stamp.empty()) {
        const CGFloat grey[4] = {0.55, 0.55, 0.55, 1};
        const double sw = draw_line(g, rgb, g_stamp, 15, grey, 0, 0, true);
        draw_line(g, rgb, g_stamp, 15, grey, (kWidth - sw) / 2, track.origin.y + track.size.height + 62);
    }
    CGContextRelease(g);
    CGColorSpaceRelease(rgb);
    return g_pixels;
}

}  // namespace dcusa::gpu
