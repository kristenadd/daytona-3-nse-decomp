#pragma once
// Draws the launcher (platform/launcher) into a CAMetalLayer, on macOS and
// iOS, before the game's own Metal device takes the layer over.
#include <memory>

#include "platform/launcher/launcher.hpp"

namespace dcusa::launcher {

class MetalLauncherRenderer {
public:
    // `layer`: the CAMetalLayer, as void*.
    explicit MetalLauncherRenderer(void* layer);
    ~MetalLauncherRenderer();
    // The layer's size in pixels (its drawableSize, kept to its bounds).
    void drawable_size(int& width, int& height);
    void draw(const Launcher& launcher, const DrawList& list);

    struct State;
private:
    std::unique_ptr<State> s_;
};

}  // namespace dcusa::launcher
