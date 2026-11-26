#include "platform/edition_choice.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "recomp/generated.hpp"
#include "system/content.hpp"

namespace dcusa {

const char* title_image_name(Edition edition) {
    return edition == Edition::Dcu2017 ? "title2017.png" : "title.png";
}

const char* container_name(Edition edition) {
    return edition == Edition::Dcu2017 ? "container-2017" : "container";
}

const char* edition_display_title(Edition edition) {
    return edition == Edition::Dcu2017 ? "Daytona Championship USA" : "New Season Edition";
}

std::vector<EditionChoice> available_editions(const std::filesystem::path& cache_dir,
                                              const std::filesystem::path& content,
                                              const std::vector<std::filesystem::path>& image_dirs,
                                              std::string* why) {
    std::vector<EditionChoice> out;
    std::optional<InstallScan> scan;
    if (!content.empty()) scan = find_installs(content);
    std::error_code ec;
    for (Edition edition : recomp::built_editions()) {
        const EditionInfo& info = edition_info(edition);
        const std::filesystem::path cache = cache_dir / std::string(info.cache_name);
        if (!std::filesystem::is_regular_file(cache, ec)) {
            if (why) *why += std::string(info.id) + ": no " + std::string(info.cache_name) + " in " + cache_dir.string() + "\n";
            continue;
        }
        if (scan && pick_install(*scan, edition) == nullptr) {
            if (why) *why += std::string(info.id) + ": no game folder under " + content.string() + "\n";
            continue;
        }
        EditionChoice c;
        c.edition = edition;
        c.cache = cache;
        c.title = edition_display_title(edition);
        for (const std::filesystem::path& dir : image_dirs) {
            const std::filesystem::path image = dir / title_image_name(edition);
            if (!dir.empty() && std::filesystem::is_regular_file(image, ec)) {
                c.image = image;
                break;
            }
        }
        out.push_back(std::move(c));
    }
    return out;
}

int arrange_for_picker(std::vector<EditionChoice>& choices) {
    std::stable_sort(choices.begin(), choices.end(), [](const EditionChoice& a, const EditionChoice& b) {
        return edition_info(a.edition).id < edition_info(b.edition).id;
    });
    for (std::size_t i = 0; i < choices.size(); ++i)
        if (choices[i].edition == Edition::Nse2019) return static_cast<int>(i);
    return 0;
}

std::optional<Edition> edition_from_environment() {
    const char* e = std::getenv("DCUSA_EDITION");
    if (e == nullptr || *e == 0) return std::nullopt;
    return edition_from_id(e);
}

bool PickerInput::pad(float steering, bool confirm_held) {
    const int dir = steering < -0.5f ? -1 : steering > 0.5f ? 1 : 0;
    if (dir != 0 && dir != stick_) select(selected_ + dir);
    stick_ = dir;
    if (!confirm_held) {
        confirm_armed_ = true;
        return false;
    }
    return confirm_armed_;
}

// The ring: cards 60 degrees apart (a ring of six, so two or three cards
// read as part of a carousel), seen from a camera 3.2 radii from its centre
// and a little above it, so cards farther round stand smaller and higher.
namespace {
constexpr float kSpacing = 3.14159265f / 3.0f;
constexpr float kCamera = 3.2f;      // camera distance, in ring radii
constexpr float kSpread = 0.40f;     // ring radius across the view, as a share of its width
constexpr float kFrontWidth = 0.44f; // the front card's width, as a share of the view's
}  // namespace

bool Carousel::step(double dt) {
    const float gap = target_ - pos_;
    if (std::fabs(gap) < 0.0005f) {
        pos_ = target_;
        return false;
    }
    // Eased: most of the way in about a quarter of a second.
    pos_ += gap * static_cast<float>(1.0 - std::exp(-dt * 11.0));
    return true;
}

CarouselCard Carousel::card(int index, float view_w, float view_h) const {
    const float theta = (static_cast<float>(index) - pos_) * kSpacing;
    const float c = std::cos(theta), s = std::sin(theta);
    const float scale = (kCamera - 1.0f) / (kCamera - c);  // 1 at the front
    CarouselCard k;
    k.w = std::min(view_w * kFrontWidth, view_h * 0.62f * 16.0f / 9.0f) * scale;
    k.h = k.w * 9.0f / 16.0f;
    k.cx = view_w * 0.5f + s * view_w * kSpread * scale;
    // The camera is above the ring: farther cards sit higher.
    k.cy = view_h * 0.46f - (1.0f - scale) * view_h * 0.45f;
    k.depth = (1.0f - c) * 0.5f;
    k.brightness = 1.0f - 0.85f * std::min(1.0f, k.depth * 2.5f);
    k.title_alpha = std::max(0.0f, 1.0f - std::fabs(static_cast<float>(index) - pos_) * 2.0f);
    return k;
}

std::vector<int> Carousel::draw_order(float view_w, float view_h) const {
    std::vector<int> order(static_cast<std::size_t>(count_));
    for (int i = 0; i < count_; ++i) order[static_cast<std::size_t>(i)] = i;
    std::sort(order.begin(), order.end(),
              [&](int a, int b) { return card(a, view_w, view_h).depth > card(b, view_w, view_h).depth; });
    return order;
}

int Carousel::hit(float x, float y, float view_w, float view_h) const {
    std::vector<int> order = draw_order(view_w, view_h);
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        const CarouselCard k = card(*it, view_w, view_h);
        if (std::fabs(x - k.cx) <= k.w * 0.5f && std::fabs(y - k.cy) <= k.h * 0.5f) return *it;
    }
    return -1;
}

}  // namespace dcusa
