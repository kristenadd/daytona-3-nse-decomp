#include "gpu/boot_screen.hpp"

#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <ImageIO/ImageIO.h>

#include <algorithm>
#include <cctype>

namespace dcusa::gpu {

namespace {

CGImageRef g_title = nullptr;
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
    const CGRect track = CGRectMake(560, 862, 800, 18);
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

    // The label, centred above the bar, styled after the game's own overlay
    // text ("PRESS START BUTTON", "FREE PLAY"): wide bold capitals, white with
    // a dark navy outline and a soft dark glow. The game's face is not a
    // system font; Helvetica Neue Bold stretched 18% wider comes close.
    if (!label.empty()) {
        constexpr CGFloat kStretch = 1.2;
        const CGFloat outline[4] = {0.11, 0.16, 0.27, 1}, glow[4] = {0.05, 0.07, 0.12, 0.75};
        CTFontRef font = CTFontCreateWithName(CFSTR("HelveticaNeue-Bold"), 44, nullptr);
        CGColorRef white = CGColorCreate(rgb, (const CGFloat[4]){1, 1, 1, 1});
        CGColorRef navy = CGColorCreate(rgb, outline);
        CGColorRef shade = CGColorCreate(rgb, glow);
        std::string upper = label;
        for (char& ch : upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        CFStringRef text = CFStringCreateWithCString(nullptr, upper.c_str(), kCFStringEncodingUTF8);
        // One line per pass: the outline (a stroke, as a % of the font size)
        // with the glow, then the white face on top, so the outline stays outside.
        auto make_line = [&](CGColorRef color, float stroke_width) {
            const float kern_value = 2.0f;
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
        CTLineRef edge = make_line(navy, -14.0f);  // fill and a wide stroke: the outline
        CTLineRef face = make_line(white, 0.0f);
        const double w = CTLineGetTypographicBounds(face, nullptr, nullptr, nullptr) * kStretch;
        const CGFloat baseline = track.origin.y - 22;
        for (int pass = 0; pass < 2; ++pass) {
            // Text draws upward from its baseline: flip once more around the baseline.
            CGContextSaveGState(g);
            CGContextTranslateCTM(g, (kWidth - w) / 2, baseline);
            CGContextScaleCTM(g, kStretch, -1);
            if (pass == 0) CGContextSetShadowWithColor(g, CGSizeMake(0, -2), 7, shade);
            CGContextSetTextPosition(g, 0, 0);
            CTLineDraw(pass == 0 ? edge : face, g);
            CGContextRestoreGState(g);
        }
        CFRelease(edge);
        CFRelease(face);
        CFRelease(text);
        CGColorRelease(shade);
        CGColorRelease(navy);
        CGColorRelease(white);
        CFRelease(font);
    }
    CGContextRelease(g);
    CGColorSpaceRelease(rgb);
    return g_pixels;
}

}  // namespace dcusa::gpu
