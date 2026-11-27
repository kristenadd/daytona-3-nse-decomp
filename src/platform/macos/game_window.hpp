#pragma once
// The game's window on macOS: an AppKit window whose content is a CAMetalLayer
// the graphics device draws each frame into (gpu::show).
//
// The game keeps the main thread, as it does on Windows, so the window does not
// run its own event loop: present() drains pending AppKit events once per
// frame, the way the game's PeekMessage loop would. Closing the window or Quit
// (Cmd-Q) ends the process; Ctrl-Cmd-F toggles full screen.
#include <memory>
#include <vector>

#include "platform/launcher/launcher.hpp"
#include "runtime/host_platform.hpp"

namespace dcusa {

class GameWindow : public Presenter {
public:
    // The CAMetalLayer, as void*, for gpu::set_layer.
    virtual void* layer() = 0;
    // Before the game starts: the launcher (platform/launcher: the logo, the
    // edition, START, OPTIONS, CONTROLS), driven by the pad (a GamepadSource)
    // and the keys. Returns the index of the edition started, or -1 when the
    // player left it (B / Esc on the title); closing the window or Quit ends
    // the process.
    virtual int choose(const std::vector<launcher::LauncherEdition>& editions, int start, PadSource* pad) = 0;
};

// Must be called on the main thread before the game starts.
std::unique_ptr<GameWindow> make_game_window(const char* title, unsigned width, unsigned height);

}  // namespace dcusa
