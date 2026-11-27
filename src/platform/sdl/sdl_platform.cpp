#include "platform/sdl/sdl_platform.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

#include "recomp/overlay.hpp"
#include "runtime/pad_mapping.hpp"

namespace dcusa {
namespace {

bool init_subsystem(SDL_InitFlags flags) {
    if (SDL_WasInit(flags) == flags) return true;
    if (!SDL_InitSubSystem(flags)) {
        std::fprintf(stderr, "sdl: %s\n", SDL_GetError());
        return false;
    }
    return true;
}

bool want_fullscreen() {
    if (const char* v = std::getenv("DCUSA_FULLSCREEN")) return std::strcmp(v, "0") != 0;
    // A Steam Deck (gamescope shows every window full screen anyway).
    return std::getenv("SteamDeck") != nullptr || std::getenv("SteamGamepadUI") != nullptr;
}

// Window events common to the picker and the game: closing ends the run, F11
// or Alt+Return toggles full screen, Ctrl+Q quits.
bool common_event(const SDL_Event& e, SDL_Window* window) {
    switch (e.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        std::fprintf(stderr, "window closed\n");
        _exit(0);
    case SDL_EVENT_KEY_DOWN:
        if (e.key.key == SDLK_F11 || (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT))) {
            const bool full = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
            SDL_SetWindowFullscreen(window, !full);
            return true;
        }
        if (e.key.key == SDLK_Q && (e.key.mod & SDL_KMOD_CTRL)) {
            std::fprintf(stderr, "quit\n");
            _exit(0);
        }
        return false;
    default:
        return false;
    }
}

class SdlGameWindow : public GameWindow {
public:
    SdlGameWindow(const char* title, unsigned width, unsigned height) : title_(title), width_(width), height_(height) {
        init_subsystem(SDL_INIT_VIDEO);
    }

    void* layer() override { return window(); }

    int choose(const std::vector<launcher::LauncherEdition>& editions, int start, PadSource* pad) override;

    void drawable_size(std::uint32_t& width, std::uint32_t& height) override {
        int w = static_cast<int>(width_), h = static_cast<int>(height_);
        if (window_ != nullptr) SDL_GetWindowSizeInPixels(window_, &w, &h);
        width = static_cast<std::uint32_t>(std::max(1, w));
        height = static_cast<std::uint32_t>(std::max(1, h));
    }

    void present() override {
        if (!SDL_IsMainThread()) return;
        SDL_Event e;
        while (SDL_PollEvent(&e)) common_event(e, window_);
    }

private:
    SDL_Window* window() {
        if (window_ != nullptr) return window_;
        SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
        if (want_fullscreen()) flags |= SDL_WINDOW_FULLSCREEN;
        window_ = SDL_CreateWindow(title_.c_str(), static_cast<int>(width_), static_cast<int>(height_), flags);
        if (window_ == nullptr) {
            std::fprintf(stderr, "sdl: no window: %s\n", SDL_GetError());
        } else {
            SDL_HideCursor();
            SDL_RaiseWindow(window_);
        }
        return window_;
    }

    std::string title_;
    unsigned width_, height_;
    SDL_Window* window_ = nullptr;
};

// ---- the launcher ----

int SdlGameWindow::choose(const std::vector<launcher::LauncherEdition>& editions, int start, PadSource* pad) {
    using launcher::Action;
    using launcher::Launcher;
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (want_fullscreen()) flags |= SDL_WINDOW_FULLSCREEN;
    SDL_Window* w = nullptr;
    SDL_Renderer* r = nullptr;
    if (!SDL_CreateWindowAndRenderer(title_.c_str(), static_cast<int>(width_), static_cast<int>(height_), flags, &w, &r)) {
        std::fprintf(stderr, "sdl: no launcher window: %s\n", SDL_GetError());
        return start;
    }
    SDL_RaiseWindow(w);
    SDL_HideCursor();
    SDL_SetRenderVSync(r, 1);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    int pw = 0, ph = 0;
    SDL_GetCurrentRenderOutputSize(r, &pw, &ph);
    Launcher ui;
    if (!ui.init(editions, start, pw, ph, true)) {
        SDL_DestroyRenderer(r);
        SDL_DestroyWindow(w);
        return start;
    }
    std::unique_ptr<AudioOutput> audio = make_sdl_audio_output();
    const auto pull = [](void* user, std::span<float> out) { static_cast<Launcher*>(user)->mix(out.data(), out.size() / 2); };
    if (audio && !audio->start(Launcher::kRate, pull, &ui)) audio.reset();
    auto* gamepad = dynamic_cast<GamepadSource*>(pad);
    static_assert(sizeof(launcher::Vertex) == sizeof(SDL_Vertex), "the launcher's vertices are SDL_Vertex-shaped");
    std::vector<SDL_Texture*> textures;
    int generation = -1;
    while (ui.result() == Launcher::Result::Running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (common_event(e, w)) continue;
            if (e.type != SDL_EVENT_KEY_DOWN) continue;
            switch (e.key.key) {
                case SDLK_UP: ui.key(Action::Up); break;
                case SDLK_DOWN: ui.key(Action::Down); break;
                case SDLK_LEFT: ui.key(Action::Left); break;
                case SDLK_RIGHT: ui.key(Action::Right); break;
                case SDLK_RETURN:
                case SDLK_KP_ENTER:
                case SDLK_SPACE:
                    if (!e.key.repeat) ui.key(Action::Confirm);
                    break;
                case SDLK_ESCAPE:
                case SDLK_BACKSPACE:
                    if (!e.key.repeat) ui.key(Action::Back);
                    break;
                default: break;
            }
        }
        if (gamepad != nullptr) ui.pad(gamepad->read());
        SDL_GetCurrentRenderOutputSize(r, &pw, &ph);
        ui.resize(pw, ph);
        const launcher::DrawList& list = ui.frame();
        if (generation != ui.texture_generation()) {
            generation = ui.texture_generation();
            for (SDL_Texture* t : textures) SDL_DestroyTexture(t);
            textures.clear();
            for (const launcher::Texture& t : ui.textures()) {
                SDL_Texture* tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, t.width, t.height);
                if (tex != nullptr) {
                    SDL_UpdateTexture(tex, nullptr, t.rgba.data(), t.width * 4);
                    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
                    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
                }
                textures.push_back(tex);
            }
        }
        SDL_SetRenderDrawColor(r, 4, 20, 50, 255);
        SDL_RenderClear(r);
        const auto* verts = reinterpret_cast<const SDL_Vertex*>(list.vertices.data());
        for (const launcher::Batch& b : list.batches) {
            if (b.count == 0) continue;
            if (b.clipped) {
                const SDL_Rect clip{static_cast<int>(b.clip_x), static_cast<int>(b.clip_y), static_cast<int>(b.clip_w),
                                    static_cast<int>(b.clip_h)};
                SDL_SetRenderClipRect(r, &clip);
            } else {
                SDL_SetRenderClipRect(r, nullptr);
            }
            SDL_Texture* t = b.texture >= 0 && b.texture < static_cast<int>(textures.size())
                                 ? textures[static_cast<std::size_t>(b.texture)]
                                 : nullptr;
            SDL_RenderGeometry(r, t, verts, static_cast<int>(list.vertices.size()),
                               reinterpret_cast<const int*>(list.indices.data() + b.first), static_cast<int>(b.count));
        }
        SDL_SetRenderClipRect(r, nullptr);
        if (const char* shot = std::getenv("DCUSA_LAUNCHER_SHOT")) {  // testing: the latest frame
            static int frames = 0;
            if (++frames % 20 == 0)
                if (SDL_Surface* sf = SDL_RenderReadPixels(r, nullptr)) {
                    SDL_SaveBMP(sf, shot);
                    SDL_DestroySurface(sf);
                }
        }
        SDL_RenderPresent(r);
    }
    if (audio) audio->stop();
    for (SDL_Texture* t : textures) SDL_DestroyTexture(t);
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(w);
    return ui.result() == Launcher::Result::Start ? ui.chosen() : -1;
}

// ---- the controller ----

class SdlPad : public GamepadSource {
public:
    SdlPad() { init_subsystem(SDL_INIT_GAMEPAD); }

    GamepadState read() override {
        if (SDL_IsMainThread() && SDL_WasInit(SDL_INIT_VIDEO) == 0) SDL_PumpEvents();
        SDL_UpdateGamepads();
        if (pad_ != nullptr && !SDL_GamepadConnected(pad_)) {
            SDL_CloseGamepad(pad_);
            pad_ = nullptr;
            std::fprintf(stderr, "pad: none\n");
        }
        if (pad_ == nullptr) {
            const auto now = std::chrono::steady_clock::now();
            if (now - last_look_ > std::chrono::milliseconds(500)) {
                last_look_ = now;
                int n = 0;
                if (SDL_JoystickID* ids = SDL_GetGamepads(&n)) {
                    if (n > 0) pad_ = SDL_OpenGamepad(ids[0]);
                    SDL_free(ids);
                }
                if (pad_ != nullptr) {
                    const char* name = SDL_GetGamepadName(pad_);
                    std::fprintf(stderr, "pad: %s\n", name ? name : "controller");
                }
            }
        }
        GamepadState state;
        if (pad_ != nullptr) {
            auto axis = [&](SDL_GamepadAxis a) { return static_cast<float>(SDL_GetGamepadAxis(pad_, a)) / 32767.0f; };
            auto button = [&](SDL_GamepadButton b) { return SDL_GetGamepadButton(pad_, b); };
            state.connected = true;
            state.left_x = std::clamp(axis(SDL_GAMEPAD_AXIS_LEFTX), -1.0f, 1.0f);
            state.left_trigger = std::clamp(axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER), 0.0f, 1.0f);
            state.right_trigger = std::clamp(axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), 0.0f, 1.0f);
            // Positions, Xbox names (south A, east B, west X, north Y).
            state.a = button(SDL_GAMEPAD_BUTTON_SOUTH);
            state.b = button(SDL_GAMEPAD_BUTTON_EAST);
            state.x = button(SDL_GAMEPAD_BUTTON_WEST);
            state.y = button(SDL_GAMEPAD_BUTTON_NORTH);
            state.left_shoulder = button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
            state.right_shoulder = button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
            state.start = button(SDL_GAMEPAD_BUTTON_START);
            state.back = button(SDL_GAMEPAD_BUTTON_BACK);
            state.left_y = std::clamp(axis(SDL_GAMEPAD_AXIS_LEFTY), -1.0f, 1.0f);
            state.right_x = std::clamp(axis(SDL_GAMEPAD_AXIS_RIGHTX), -1.0f, 1.0f);
            state.right_y = std::clamp(axis(SDL_GAMEPAD_AXIS_RIGHTY), -1.0f, 1.0f);
            state.left_stick = button(SDL_GAMEPAD_BUTTON_LEFT_STICK);
            state.right_stick = button(SDL_GAMEPAD_BUTTON_RIGHT_STICK);
            state.dpad_up = button(SDL_GAMEPAD_BUTTON_DPAD_UP);
            state.dpad_down = button(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
            state.dpad_left = button(SDL_GAMEPAD_BUTTON_DPAD_LEFT);
            state.dpad_right = button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
            state.misc = button(SDL_GAMEPAD_BUTTON_MISC1);
            state.paddle[0] = button(SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1);
            state.paddle[1] = button(SDL_GAMEPAD_BUTTON_LEFT_PADDLE1);
            state.paddle[2] = button(SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2);
            state.paddle[3] = button(SDL_GAMEPAD_BUTTON_LEFT_PADDLE2);
            state.touchpad = button(SDL_GAMEPAD_BUTTON_TOUCHPAD);
            // The face buttons' names, for the launcher's prompts.
            switch (SDL_GetGamepadButtonLabel(pad_, SDL_GAMEPAD_BUTTON_SOUTH)) {
                case SDL_GAMEPAD_BUTTON_LABEL_CROSS: state.labels = GamepadState::Labels::PlayStation; break;
                case SDL_GAMEPAD_BUTTON_LABEL_B: state.labels = GamepadState::Labels::Nintendo; break;
                default: break;
            }
            // R3 (right stick press): the frame-rate overlay on or off.
            const bool r3 = button(SDL_GAMEPAD_BUTTON_RIGHT_STICK);
            if (r3 && !r3_) toggle_fps_overlay();
            r3_ = r3;
        }
        return state;
    }

private:
    SDL_Gamepad* pad_ = nullptr;
    bool r3_ = false;
    std::chrono::steady_clock::time_point last_look_{};
};

// ---- the sound ----

class SdlAudio : public AudioOutput {
public:
    bool start(std::uint32_t sample_rate, Pull pull, void* user) override {
        if (!init_subsystem(SDL_INIT_AUDIO)) return false;
        pull_ = pull;
        user_ = user;
        SDL_AudioSpec spec{SDL_AUDIO_F32, 2, static_cast<int>(sample_rate)};
        stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &SdlAudio::callback, this);
        if (stream_ == nullptr) {
            std::fprintf(stderr, "sdl: no sound output: %s\n", SDL_GetError());
            return false;
        }
        SDL_ResumeAudioStreamDevice(stream_);
        std::fprintf(stderr, "sound: %s at %u Hz\n", SDL_GetCurrentAudioDriver(), sample_rate);
        return true;
    }
    void stop() override {
        if (stream_ != nullptr) SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }
    ~SdlAudio() override { stop(); }

private:
    static void callback(void* self, SDL_AudioStream* stream, int additional, int) {
        auto* a = static_cast<SdlAudio*>(self);
        const std::size_t floats = static_cast<std::size_t>(additional) / sizeof(float) / 2 * 2;
        if (floats == 0) return;
        if (a->buffer_.size() < floats) a->buffer_.resize(floats);
        std::span<float> out(a->buffer_.data(), floats);
        a->pull_(a->user_, out);
        SDL_PutAudioStreamData(stream, out.data(), static_cast<int>(floats * sizeof(float)));
    }

    Pull pull_ = nullptr;
    void* user_ = nullptr;
    SDL_AudioStream* stream_ = nullptr;
    std::vector<float> buffer_;
};

}  // namespace

std::unique_ptr<GameWindow> make_sdl_game_window(const char* title, unsigned width, unsigned height) {
    return std::make_unique<SdlGameWindow>(title, width, height);
}
std::unique_ptr<PadSource> make_sdl_pad() { return std::make_unique<SdlPad>(); }
std::unique_ptr<AudioOutput> make_sdl_audio_output() { return std::make_unique<SdlAudio>(); }

}  // namespace dcusa
