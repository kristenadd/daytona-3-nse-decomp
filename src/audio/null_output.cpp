#include "audio/null_output.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace dcusa::audio {
namespace {

class NullOutput : public AudioOutput {
public:
    explicit NullOutput(std::string wav) : wav_path_(std::move(wav)) {}
    ~NullOutput() override { stop(); }

    bool start(std::uint32_t sample_rate, Pull pull, void* user) override {
        if (running_) return true;
        rate_ = sample_rate;
        pull_ = pull;
        user_ = user;
        if (!wav_path_.empty()) {
            wav_ = std::fopen(wav_path_.c_str(), "wb");
            if (wav_ != nullptr) write_header(0);
        }
        running_ = true;
        thread_ = std::thread([this] { run(); });
        return true;
    }

    void stop() override {
        if (!running_) return;
        running_ = false;
        thread_.join();
        if (wav_ != nullptr) {
            write_header(frames_written_);
            std::fclose(wav_);
            wav_ = nullptr;
        }
    }

private:
    // 10 ms blocks, paced against the clock rather than the sleep.
    void run() {
        const std::size_t block = rate_ / 100;
        std::vector<float> buf(2 * block);
        std::vector<std::int16_t> pcm(2 * block);
        const auto period = std::chrono::nanoseconds(1'000'000'000ull * block / rate_);
        auto next = std::chrono::steady_clock::now();
        while (running_) {
            pull_(user_, buf);
            if (wav_ != nullptr) {
                for (std::size_t i = 0; i < buf.size(); ++i)
                    pcm[i] = static_cast<std::int16_t>(std::lround(std::clamp(buf[i], -1.0f, 1.0f) * 32767.0f));
                std::fwrite(pcm.data(), 2, pcm.size(), wav_);
                frames_written_ += block;
                // Keep the header current, so a run that is killed leaves a playable file.
                if (frames_written_ % (rate_ * 2) < block) {
                    write_header(frames_written_);
                    std::fseek(wav_, 0, SEEK_END);
                }
            }
            next += period;
            std::this_thread::sleep_until(next);
        }
    }

    void write_header(std::uint64_t frames) {
        const std::uint32_t data = static_cast<std::uint32_t>(std::min<std::uint64_t>(frames * 4, 0xFFFFFFF0u));
        std::uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0};
        auto put32 = [&](int at, std::uint32_t v) { std::memcpy(h + at, &v, 4); };
        auto put16 = [&](int at, std::uint16_t v) { std::memcpy(h + at, &v, 2); };
        put32(4, 36 + data);
        put32(24, rate_);
        put32(28, rate_ * 4);
        put16(32, 4);
        put16(34, 16);
        std::memcpy(h + 36, "data", 4);
        put32(40, data);
        std::fseek(wav_, 0, SEEK_SET);
        std::fwrite(h, 1, sizeof h, wav_);
        std::fflush(wav_);
    }

    std::string wav_path_;
    std::FILE* wav_ = nullptr;
    std::uint64_t frames_written_ = 0;
    std::uint32_t rate_ = 48000;
    Pull pull_ = nullptr;
    void* user_ = nullptr;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

}  // namespace

std::unique_ptr<AudioOutput> make_null_output(const std::string& wav_path) {
    return std::make_unique<NullOutput>(wav_path);
}

}  // namespace dcusa::audio
