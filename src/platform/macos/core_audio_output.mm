#include "platform/macos/core_audio_output.hpp"

#include <AudioToolbox/AudioToolbox.h>
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#import <AVFoundation/AVFoundation.h>
#endif

#include <cstdio>

namespace dcusa {
namespace {

class CoreAudioOutput : public AudioOutput {
public:
    ~CoreAudioOutput() override { stop(); }

    bool start(std::uint32_t sample_rate, Pull pull, void* user) override {
        if (unit_ != nullptr) return true;
        pull_ = pull;
        user_ = user;
        AudioComponentDescription desc{};
        desc.componentType = kAudioUnitType_Output;
#if TARGET_OS_IPHONE
        // iOS: the RemoteIO unit, in a playback session (sound with the ring /
        // silent switch off, as a game expects).
        AVAudioSession* session = [AVAudioSession sharedInstance];
        [session setCategory:AVAudioSessionCategoryPlayback error:nil];
        [session setPreferredSampleRate:sample_rate error:nil];
        [session setActive:YES error:nil];
        desc.componentSubType = kAudioUnitSubType_RemoteIO;
#else
        desc.componentSubType = kAudioUnitSubType_DefaultOutput;
#endif
        desc.componentManufacturer = kAudioUnitManufacturer_Apple;
        AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
        if (comp == nullptr || AudioComponentInstanceNew(comp, &unit_) != noErr) return fail("no output unit");
        AudioStreamBasicDescription f{};
        f.mSampleRate = sample_rate;
        f.mFormatID = kAudioFormatLinearPCM;
        f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        f.mChannelsPerFrame = 2;
        f.mBitsPerChannel = 32;
        f.mFramesPerPacket = 1;
        f.mBytesPerFrame = 8;
        f.mBytesPerPacket = 8;
        if (AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f) != noErr)
            return fail("stream format");
        AURenderCallbackStruct cb{render, this};
        if (AudioUnitSetProperty(unit_, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb) !=
            noErr)
            return fail("render callback");
        if (AudioUnitInitialize(unit_) != noErr) return fail("initialize");
        if (AudioOutputUnitStart(unit_) != noErr) return fail("start");
        return true;
    }

    void stop() override {
        if (unit_ == nullptr) return;
        AudioOutputUnitStop(unit_);
        AudioUnitUninitialize(unit_);
        AudioComponentInstanceDispose(unit_);
        unit_ = nullptr;
    }

private:
    bool fail(const char* what) {
        std::fprintf(stderr, "audio: Core Audio %s failed\n", what);
        if (unit_ != nullptr) AudioComponentInstanceDispose(unit_);
        unit_ = nullptr;
        return false;
    }

    static OSStatus render(void* self, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 frames,
                           AudioBufferList* io) {
        auto* out = static_cast<CoreAudioOutput*>(self);
        float* samples = static_cast<float*>(io->mBuffers[0].mData);
        out->pull_(out->user_, std::span<float>(samples, static_cast<std::size_t>(frames) * 2));
        return noErr;
    }

    AudioUnit unit_ = nullptr;
    Pull pull_ = nullptr;
    void* user_ = nullptr;
};

}  // namespace

std::unique_ptr<AudioOutput> make_core_audio_output() { return std::make_unique<CoreAudioOutput>(); }

}  // namespace dcusa
