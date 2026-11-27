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
    std::string title;                // its name, for messages
    std::filesystem::path image;      // title image; empty when there is none
};

// The title image's file name: title.png (2019) or title2017.png (2017).
const char* title_image_name(Edition edition);
// The container folder's name: container (2019) or container-2017 (2017).
const char* container_name(Edition edition);
// The edition's short name.
const char* edition_display_title(Edition edition);

// Offerable editions, 2019 first. `cache_dir` holds the caches; `content` is
// searched for game installs (empty: not checked); title images are looked for
// in each of `image_dirs` in order (the boot screen's). Problems (an edition
// built in but missing its cache or game folder) are appended to *why.
std::vector<EditionChoice> available_editions(const std::filesystem::path& cache_dir,
                                              const std::filesystem::path& content,
                                              const std::vector<std::filesystem::path>& image_dirs,
                                              std::string* why);

// DCUSA_EDITION=2017|2019 (or a --edition argument): the edition asked for.
std::optional<Edition> edition_from_environment();

}  // namespace dcusa
