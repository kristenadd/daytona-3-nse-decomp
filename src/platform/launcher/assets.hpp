#pragma once
// The launcher's fonts and logo, compiled into the binary (embed.cmake):
// "title_font" (Russo One), "body_font" (Titillium Web Bold), "logo" (PNG).
#include <cstddef>
#include <span>
#include <string_view>

namespace dcusa::launcher {
std::span<const unsigned char> asset(std::string_view name);  // empty when unknown
}  // namespace dcusa::launcher
