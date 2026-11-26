#pragma once
// Which game editions a launcher can offer, and where each one's files are.
// An edition is offered when this binary has its translated code
// (recomp/generated.hpp), its prepared cache is present (DCUSA2019.cache /
// DCUSA2017.cache) and, when a content folder is given, the game install is
// there too. Each edition has its own title image (the boot screen's: made by
// tools/bootscreen/capture_title.py) and its own container folder for what
// the game writes.
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "system/edition.hpp"

namespace dcusa {

struct EditionChoice {
    Edition edition{};
    std::filesystem::path cache;
    std::string title;                // what the picker shows under the image
    std::filesystem::path image;      // title image; empty when there is none
};

// The title image's file name: title.png (2019) or title2017.png (2017).
const char* title_image_name(Edition edition);
// The container folder's name: container (2019) or container-2017 (2017).
const char* container_name(Edition edition);
// The short name shown in the picker.
const char* edition_display_title(Edition edition);

// Offerable editions, 2019 first. `cache_dir` holds the caches; `content` is
// searched for game installs (empty: not checked); title images are looked for
// in each of `image_dirs` in order. Problems (an edition built in but missing
// its cache or game folder) are appended to *why.
std::vector<EditionChoice> available_editions(const std::filesystem::path& cache_dir,
                                              const std::filesystem::path& content,
                                              const std::vector<std::filesystem::path>& image_dirs,
                                              std::string* why);

// The picker's order: oldest first, left to right (2017, then 2019). Sorts
// `choices` that way and returns the index it starts on, 2019's (the
// default), or 0 when 2019 is not among them.
int arrange_for_picker(std::vector<EditionChoice>& choices);

// DCUSA_EDITION=2017|2019 (or a --edition argument): the edition asked for.
std::optional<Edition> edition_from_environment();

// A choice made by moving left / right and confirming, shared by the pickers.
// Returns the chosen index once a confirm press (Start, gas, any view button)
// follows a moment with none held, so a press carried over is not taken.
class PickerInput {
public:
    explicit PickerInput(int count, int selected = 0) : count_(count), selected_(selected) {}
    int selected() const { return selected_; }
    void select(int index) {
        if (index >= 0 && index < count_) selected_ = index;
    }
    // steering -1 .. +1; returns true when the selection is confirmed.
    bool pad(float steering, bool confirm_held);

private:
    int count_;
    int selected_;
    int stick_ = 0;            // -1, 0, +1: the stick's last direction
    bool confirm_armed_ = false;
};

// The picker's carousel: the cards stand on a ring seen in perspective, the
// selected one at the front, and always face the viewer. Turning the ring is
// animated (step); the platforms draw each card where card() puts it,
// farthest first (draw_order), with a faint reflection below.
struct CarouselCard {
    float cx = 0, cy = 0;     // centre, in view points
    float w = 0, h = 0;       // size (16:9)
    float brightness = 1;     // 1 at the front, darker behind
    float title_alpha = 0;    // the name under the front card
    float depth = 0;          // 0 front .. 1 back of the ring
};

class Carousel {
public:
    explicit Carousel(int count, int start = 0)
        : count_(count), pos_(static_cast<float>(start)), target_(static_cast<float>(start)) {}
    void select(int index) { target_ = static_cast<float>(index); }
    // Advances the turn by dt seconds; true while still moving.
    bool step(double dt);
    CarouselCard card(int index, float view_w, float view_h) const;
    std::vector<int> draw_order(float view_w, float view_h) const;  // back to front
    // The card under (x, y), front ones first; -1 for none.
    int hit(float x, float y, float view_w, float view_h) const;

private:
    int count_;
    float pos_ = 0, target_ = 0;  // the ring's turn, in cards
};

}  // namespace dcusa
