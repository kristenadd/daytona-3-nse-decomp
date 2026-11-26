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
#include <vector>

#include "recomp/overlay.hpp"
#include "runtime/pad_mapping.hpp"

// stb_image is compiled in gpu/boot_screen_stb.cpp; only its declarations here.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wall"
#pragma clang diagnostic ignored "-Wextra"
#pragma clang diagnostic ignored "-Wconversion"
#endif
#include "../../../third_party/stb/stb_image.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

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

    int choose(const std::vector<EditionChoice>& given, PadSource* pad) override;

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

// ---- the edition picker ----

SDL_Texture* load_texture(SDL_Renderer* r, const std::string& path) {
    if (path.empty()) return nullptr;
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (px == nullptr) return nullptr;
    SDL_Texture* t = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
    if (t != nullptr) {
        SDL_UpdateTexture(t, nullptr, px, w * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
    }
    stbi_image_free(px);
    return t;
}

int SdlGameWindow::choose(const std::vector<EditionChoice>& given, PadSource* pad) {
    std::vector<EditionChoice> items = given;
    const int start = arrange_for_picker(items);
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (want_fullscreen()) flags |= SDL_WINDOW_FULLSCREEN;
    SDL_Window* w = nullptr;
    SDL_Renderer* r = nullptr;
    if (!SDL_CreateWindowAndRenderer(title_.c_str(), static_cast<int>(width_), static_cast<int>(height_), flags, &w, &r)) {
        std::fprintf(stderr, "sdl: no picker window: %s\n", SDL_GetError());
        return 0;
    }
    SDL_RaiseWindow(w);
    SDL_SetRenderVSync(r, 1);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    std::vector<SDL_Texture*> images;
    for (const EditionChoice& item : items) images.push_back(load_texture(r, item.image.string()));
    PickerInput input(static_cast<int>(items.size()), start);
    Carousel carousel(static_cast<int>(items.size()), start);
    int selected = start, chosen = -1;
    auto last = std::chrono::steady_clock::now();
    while (chosen < 0) {
        int pw = 0, ph = 0;
        SDL_GetCurrentRenderOutputSize(r, &pw, &ph);
        const float vw = static_cast<float>(pw), vh = static_cast<float>(ph);
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (common_event(e, w)) continue;
            if (e.type == SDL_EVENT_KEY_DOWN) {
                if (e.key.key == SDLK_LEFT && selected > 0) --selected;
                else if (e.key.key == SDLK_RIGHT && selected + 1 < static_cast<int>(items.size())) ++selected;
                else if (e.key.key == SDLK_RETURN || e.key.key == SDLK_SPACE || e.key.key == SDLK_KP_ENTER) chosen = selected;
            } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                float x = e.button.x, y = e.button.y;
                SDL_RenderCoordinatesFromWindow(r, e.button.x, e.button.y, &x, &y);
                const int i = carousel.hit(x, y, vw, vh);
                if (i >= 0 && i == selected) chosen = i;
                else if (i >= 0) selected = i;
            }
        }
        input.select(selected);
        if (pad != nullptr) {
            const PadSnapshot s = pad->sample();
            const bool confirm = (s.buttons & (PadSnapshot::Start | PadSnapshot::View1 | PadSnapshot::View2 | PadSnapshot::View3 |
                                               PadSnapshot::View4)) != 0 ||
                                 s.accelerator > 0.5f;
            if (input.pad(s.steering, confirm)) chosen = input.selected();
            selected = input.selected();
        }
        const auto now = std::chrono::steady_clock::now();
        carousel.select(selected);
        carousel.step(std::chrono::duration<double>(now - last).count());
        last = now;

        // Black above, a dark floor below the cards' feet.
        SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
        SDL_RenderClear(r);
        const CarouselCard front = carousel.card(selected, vw, vh);
        const float floor_y = front.cy + front.h / 2 - vh * 0.25f;
        const SDL_FColor top{0, 0, 0, 1}, bottom{0.10f, 0.10f, 0.10f, 1};
        const SDL_Vertex quad[4] = {{{0, floor_y}, top, {0, 0}}, {{vw, floor_y}, top, {0, 0}},
                                    {{vw, vh}, bottom, {0, 0}}, {{0, vh}, bottom, {0, 0}}};
        const int order[6] = {0, 1, 2, 0, 2, 3};
        SDL_RenderGeometry(r, nullptr, quad, 4, order, 6);
        for (int i : carousel.draw_order(vw, vh)) {
            const CarouselCard k = carousel.card(i, vw, vh);
            const SDL_FRect rect{k.cx - k.w / 2, k.cy - k.h / 2, k.w, k.h};
            SDL_Texture* t = images[static_cast<std::size_t>(i)];
            if (t != nullptr) {
                // The reflection: the card upside down below it, fading out
                // over half its height (in strips, each fainter than the last).
                float tw = 0, th = 0;
                SDL_GetTextureSize(t, &tw, &th);
                constexpr int kStrips = 24;
                const float strip = rect.h * 0.5f / kStrips;
                for (int j = 0; j < kStrips; ++j) {
                    const float fade = 1.0f - (static_cast<float>(j) + 0.5f) / kStrips;
                    const SDL_FRect src{0, th - th * (static_cast<float>(j + 1) * strip / rect.h), tw, th * (strip / rect.h)};
                    const SDL_FRect dst{rect.x, rect.y + rect.h + 3 + static_cast<float>(j) * strip, rect.w, strip};
                    SDL_SetTextureAlphaModFloat(t, 0.28f * fade * fade * k.brightness);
                    SDL_RenderTextureRotated(r, t, &src, &dst, 0, nullptr, SDL_FLIP_VERTICAL);
                }
                SDL_SetTextureAlphaModFloat(t, 1.0f);
                SDL_RenderTexture(r, t, nullptr, &rect);
            } else {
                SDL_SetRenderDrawColor(r, 64, 64, 64, 255);
                SDL_RenderFillRect(r, &rect);
            }
            if (k.brightness < 1) {  // farther round the ring: darker
                SDL_SetRenderDrawColorFloat(r, 0, 0, 0, 1 - k.brightness);
                SDL_RenderFillRect(r, &rect);
            }
            if (k.title_alpha > 0) {
                SDL_SetRenderDrawColorFloat(r, 1, 1, 1, k.title_alpha);
                for (float inset = 1; inset <= 4; ++inset) {
                    const SDL_FRect f{rect.x - inset, rect.y - inset, rect.w + 2 * inset, rect.h + 2 * inset};
                    SDL_RenderRect(r, &f);
                }
                // The name under the front card (SDL's built-in 8 px font, scaled).
                const float scale = std::max(2.0f, vw * 0.024f / 8.0f);
                const std::string& name = items[static_cast<std::size_t>(i)].title;
                SDL_SetRenderScale(r, scale, scale);
                SDL_RenderDebugText(r, (vw / scale - static_cast<float>(name.size()) * 8) / 2, vh * 0.89f / scale, name.c_str());
                SDL_SetRenderScale(r, 1, 1);
            }
        }
        SDL_RenderPresent(r);
    }
    for (SDL_Texture* t : images)
        if (t != nullptr) SDL_DestroyTexture(t);
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(w);
    for (std::size_t i = 0; i < given.size(); ++i)
        if (given[i].edition == items[static_cast<std::size_t>(chosen)].edition) return static_cast<int>(i);
    return 0;
}

// ---- the controller ----

class SdlPad : public PadSource {
public:
    SdlPad() { init_subsystem(SDL_INIT_GAMEPAD); }

    PadSnapshot sample() override {
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
            // R3 (right stick press): the frame-rate overlay on or off.
            const bool r3 = button(SDL_GAMEPAD_BUTTON_RIGHT_STICK);
            if (r3 && !r3_) toggle_fps_overlay();
            r3_ = r3;
        }
        return map_gamepad(state);
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
