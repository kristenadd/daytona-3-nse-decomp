#pragma once
// The launcher every platform shows before the game (the same as
// daytona-shim's full-screen launcher on Windows and the Steam Deck): the
// logo, the edition (left / right when both are installed), START, OPTIONS
// (free play, volumes and the game's rules: ShellData.ini / GameSettings.ini
// in the edition's container) and CONTROLS (the controller layout:
// controls.ini in the container, read by map_gamepad). Driven by a pad
// (d-pad or left stick; A / Cross selects, B / Circle goes back) or the keys
// (arrows, Enter, Esc / Backspace). Its sounds and music are the game's own
// (rom/sound/csb/SE.csb, rom/sound/adx/d1a_bgm01_sel.adx).
//
// Platform-free: it draws into a DrawList of coloured, textured triangles in
// the drawable's pixels (the platforms render it with SDL or Metal) and mixes
// its sound into float stereo the platform pulls. The layout is 800 units
// high and as wide as the display's shape (1280 on 16:10, 1422 on 16:9);
// narrower screens are letterboxed.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include "platform/edition_choice.hpp"
#include "runtime/pad_mapping.hpp"

namespace dcusa::launcher {

struct Vertex {
    float x, y;     // drawable pixels
    float r, g, b, a;
    float u, v;
};

struct Batch {
    int texture = -1;  // index into Launcher::textures(), -1 for none
    std::uint32_t first = 0, count = 0;  // into DrawList::indices
    bool clipped = false;
    float clip_x = 0, clip_y = 0, clip_w = 0, clip_h = 0;  // pixels
};

struct DrawList {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<Batch> batches;
};

// Straight-alpha RGBA8, sampled with linear filtering and clamped.
struct Texture {
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba;
};

struct LauncherEdition {
    EditionChoice choice;
    std::filesystem::path install_root;  // GameData/, ShellData/
    std::filesystem::path game_dir;      // Daytona.exe, rom/
    std::filesystem::path container;     // what the game and the launcher write
};

enum class Action { None, Up, Down, Left, Right, Confirm, Back };

class Launcher {
public:
    Launcher();
    ~Launcher();
    // `start`: the edition shown first. `can_quit`: B / Esc on the title ends
    // the launcher (not on iOS).
    bool init(std::vector<LauncherEdition> editions, int start, int width_px, int height_px, bool can_quit);
    void resize(int width_px, int height_px);
    // The textures the draw list uses; rebuilt (generation changes) when the
    // display's scale changes.
    const std::vector<Texture>& textures() const;
    int texture_generation() const;

    void key(Action a);                // a key press (repeats included)
    void pad(const GamepadState& s);   // the controller, once a frame
    const DrawList& frame();           // advances and draws

    static constexpr int kRate = 44100;
    void mix(float* stereo, std::size_t frames);  // any thread

    enum class Result { Running, Start, Quit };
    Result result() const;
    int chosen() const;  // the edition started (index into the list given)
    const LauncherEdition* edition(int i) const;

    // Testing: the prompts of this pad ("ps", "xbox", "nintendo", "keys").
    void set_prompts(const char* which);

    struct State;
private:
    std::unique_ptr<State> s_;
};

// The editions found, as the launcher wants them: their installs (from
// `content`) and containers (beside the caches). `start` gets the one to show
// first: the last chosen (launcher.ini in `cache_dir`), else 2019.
std::vector<LauncherEdition> launcher_editions(const std::vector<EditionChoice>& choices,
                                               const std::filesystem::path& content,
                                               const std::filesystem::path& cache_dir, int* start);
void remember_edition(const std::filesystem::path& cache_dir, Edition e);

// The chosen edition's controller layout (its container's controls.ini).
PadBindings edition_pad_bindings(const std::filesystem::path& container);

}  // namespace dcusa::launcher
