#include "system/files.hpp"

#include <fstream>
#include <system_error>

namespace dcusa {

namespace fs = std::filesystem;

bool read_file(const fs::path& path, std::vector<std::uint8_t>& out, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "cannot open " + path.string();
        return false;
    }
    in.seekg(0, std::ios::end);
    std::streamoff size = in.tellg();
    in.seekg(0);
    if (size < 0) {
        if (error) *error = "cannot size " + path.string();
        return false;
    }
    out.resize(std::size_t(size));
    if (size && !in.read(reinterpret_cast<char*>(out.data()), size)) {
        if (error) *error = "cannot read " + path.string();
        return false;
    }
    return true;
}

bool write_file_atomic(const fs::path& path, const std::vector<std::uint8_t>& bytes, std::string* error) {
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    fs::path temp = path;
    temp += ".partial";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out || !out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()))) {
            if (error) *error = "cannot write " + temp.string();
            fs::remove(temp, ec);
            return false;
        }
    }
    fs::rename(temp, path, ec);
    if (ec) {
        if (error) *error = "cannot replace " + path.string() + ": " + ec.message();
        fs::remove(temp, ec);
        return false;
    }
    return true;
}

}
