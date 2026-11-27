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

std::optional<Edition> edition_from_environment() {
    const char* e = std::getenv("DCUSA_EDITION");
    if (e == nullptr || *e == 0) return std::nullopt;
    return edition_from_id(e);
}

}  // namespace dcusa
