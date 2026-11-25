#include "platform/macos/gamecontroller_pad.hpp"

#import <Foundation/Foundation.h>
#import <GameController/GameController.h>

#include <cstdio>

#include "recomp/overlay.hpp"
#include "runtime/pad_mapping.hpp"

namespace dcusa {
namespace {

class GameControllerPad : public PadSource {
public:
    GameControllerPad() {
        // dcrun is not a foreground app: without this, input only reaches the
        // application in front.
        if (@available(macOS 11.3, *)) GCController.shouldMonitorBackgroundEvents = YES;
    }

    PadSnapshot sample() override {
        @autoreleasepool {
            // Connection notices and element updates are delivered on the main
            // queue, which the game's loop never returns to on its own.
            if ([NSThread isMainThread]) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true);
            GCController* chosen = nil;
            for (GCController* c in GCController.controllers)
                if (c.extendedGamepad != nil && (chosen == nil || c == GCController.current)) chosen = c;
            if (chosen != last_) {
                last_ = chosen;
                if (chosen != nil) {
                    const char* vendor = chosen.vendorName.UTF8String;
                    const char* category = chosen.productCategory.UTF8String;
                    std::fprintf(stderr, "pad: %s (%s)\n", vendor ? vendor : "controller", category ? category : "?");
                }
                else
                    std::fprintf(stderr, "pad: none\n");
            }
            GamepadState state;
            if (GCExtendedGamepad* g = chosen.extendedGamepad) {
                state.connected = true;
                state.left_x = g.leftThumbstick.xAxis.value;
                state.left_trigger = g.leftTrigger.value;
                state.right_trigger = g.rightTrigger.value;
                state.a = g.buttonA.pressed;
                state.b = g.buttonB.pressed;
                state.x = g.buttonX.pressed;
                state.y = g.buttonY.pressed;
                state.left_shoulder = g.leftShoulder.pressed;
                state.right_shoulder = g.rightShoulder.pressed;
                state.start = g.buttonMenu.pressed;
                state.back = g.buttonOptions != nil && g.buttonOptions.pressed;
                // R3 (right stick press): the frame-rate overlay on or off.
                const bool r3 = g.rightThumbstickButton != nil && g.rightThumbstickButton.pressed;
                if (r3 && !r3_) toggle_fps_overlay();
                r3_ = r3;
            }
            return map_gamepad(state);
        }
    }

private:
    GCController* __strong last_ = nil;
    bool r3_ = false;
};

}  // namespace

std::unique_ptr<PadSource> make_gamecontroller_pad() { return std::make_unique<GameControllerPad>(); }

}  // namespace dcusa
