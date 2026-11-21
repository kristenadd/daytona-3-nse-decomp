#include <algorithm>
#include <string>
#include <vector>

#include "check.hpp"
#include "system/sha256.hpp"

using namespace dcusa;

static std::string hash_of(const std::string& s) {
    return to_hex(sha256({reinterpret_cast<const std::uint8_t*>(s.data()), s.size()}));
}

int main() {
    // FIPS 180-2 / NIST vectors.
    CHECK(hash_of("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(hash_of("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(hash_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(hash_of(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    // Feeding in pieces of every size gives the same digest.
    std::vector<std::uint8_t> data(1000);
    for (std::size_t i = 0; i < data.size(); ++i) data[i] = std::uint8_t(i * 31 + 7);
    const Sha256Digest whole = sha256(data);
    for (std::size_t step : {1u, 3u, 63u, 64u, 65u, 127u, 999u}) {
        Sha256 h;
        for (std::size_t at = 0; at < data.size(); at += step)
            h.update(std::span<const std::uint8_t>(data).subspan(at, std::min(step, data.size() - at)));
        CHECK(h.finish() == whole);
    }

    CHECK(digest_from_hex(to_hex(whole)) == whole);
    CHECK(digest_from_hex("E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855").has_value());
    CHECK(!digest_from_hex("abc").has_value());
    CHECK(!digest_from_hex(std::string(64, 'g')).has_value());
    return check_result();
}
