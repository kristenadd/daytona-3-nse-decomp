#pragma once

namespace dcusa {

// The frame-rate overlay (lower left: pictures a second and the build) on or
// off; it starts as DCUSA_SHOW_FPS (iOS: Settings, "Show frame rate") says.
// The controller's R3 (right stick press) calls this.
void toggle_fps_overlay();

}  // namespace dcusa
