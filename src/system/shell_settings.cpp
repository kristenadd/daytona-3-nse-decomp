#include "system/shell_settings.hpp"

#include <fstream>
#include <vector>

namespace dcusa {
namespace {

const char* file_of(const std::string& section) {
    return section == "GameSettings" ? "GameSettings.ini" : "ShellData.ini";
}

std::string trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r") - a + 1);
}

std::vector<std::string> read_lines(const std::filesystem::path& p) {
    std::ifstream in(p);
    std::vector<std::string> lines;
    for (std::string l; std::getline(in, l);) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
    }
    return lines;
}

bool is_section(const std::string& line, const std::string& section) { return trim(line) == "[" + section + "]"; }

// The key of a "key = value" line, or empty.
std::string key_of(const std::string& line) {
    const auto eq = line.find('=');
    return eq == std::string::npos ? std::string() : trim(line.substr(0, eq));
}

}  // namespace

std::optional<std::string> shell_setting(const std::filesystem::path& install_root,
                                         const std::filesystem::path& container, const std::string& section,
                                         const std::string& key) {
    std::filesystem::path p = container / "ShellData" / file_of(section);
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) p = install_root / "ShellData" / file_of(section);
    bool in_section = false;
    for (const std::string& l : read_lines(p)) {
        const std::string t = trim(l);
        if (!t.empty() && t.front() == '[') {
            in_section = is_section(t, section);
            continue;
        }
        if (in_section && key_of(t) == key) return trim(t.substr(t.find('=') + 1));
    }
    return std::nullopt;
}

bool set_shell_setting(const std::filesystem::path& install_root, const std::filesystem::path& container,
                       const std::string& section, const std::string& key, const std::string& value) {
    const std::filesystem::path copy = container / "ShellData" / file_of(section);
    std::error_code ec;
    if (!std::filesystem::exists(copy, ec)) {
        std::filesystem::create_directories(copy.parent_path(), ec);
        std::filesystem::copy_file(install_root / "ShellData" / file_of(section), copy, ec);
    }
    std::vector<std::string> lines = read_lines(copy);
    // Replace the key in its section, or add it (and the section) at the end.
    bool in_section = false, done = false;
    for (std::size_t i = 0; i < lines.size() && !done; ++i) {
        const std::string t = trim(lines[i]);
        if (!t.empty() && t.front() == '[') {
            in_section = is_section(t, section);
            continue;
        }
        if (in_section && key_of(t) == key) {
            lines[i] = key + "=" + value;
            done = true;
        }
    }
    if (!done) {
        bool have = false;
        for (std::size_t i = 0; i < lines.size() && !have; ++i)
            if (is_section(lines[i], section)) {
                have = true;
                std::size_t at = i + 1;
                while (at < lines.size() && (trim(lines[at]).empty() || trim(lines[at]).front() != '[')) ++at;
                // Before the blank lines that end the section.
                while (at > i + 1 && trim(lines[at - 1]).empty()) --at;
                lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at), key + "=" + value);
            }
        if (!have) {
            lines.push_back("[" + section + "]");
            lines.push_back(key + "=" + value);
        }
    }
    std::ofstream out(copy, std::ios::binary);
    for (const std::string& l : lines) out << l << "\r\n";
    return static_cast<bool>(out);
}

}  // namespace dcusa
