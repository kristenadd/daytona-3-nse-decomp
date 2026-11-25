#pragma once
// The boot screen (DCUSA_BOOT_SCREEN): while the game boots (boot state below
// 8: "Data Loading", then the network check) the display shows the attract
// title instead, with a progress bar in its colours under the logo. The title
// image is made from the player's game by tools/bootscreen/capture_title.py.
#include <cstdint>
#include <string>
#include <vector>

namespace dcusa::gpu {

// Loads the title image (PNG). False when there is none.
bool boot_screen_load(const std::string& path);
bool boot_screen_loaded();
// A font file (TTF / OTF) for the label; without one, Helvetica Neue Bold.
bool boot_screen_font(const std::string& path);

// A small text shown in the boot screen's lower-right corner (the build stamp).
void boot_screen_stamp(const std::string& text);

// A label on a translucent dark pill, premultiplied RGBA8 (the frame-rate
// overlay, gpu::set_overlay).
const std::vector<std::uint8_t>& overlay_compose(const std::string& text, std::uint32_t* width, std::uint32_t* height);

// The picture at `width` x `height`, RGBA8 rows: the title, a bar filled to
// `percent` (0 to 100; below 0 a full bar) and `label` above it.
const std::vector<std::uint8_t>& boot_screen_compose(int percent, const std::string& label, std::uint32_t* width,
                                                     std::uint32_t* height);

}
