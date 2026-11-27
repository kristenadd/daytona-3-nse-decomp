#include "platform/macos/game_window.hpp"

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include <chrono>
#include <span>
#include <vector>
#include <cstdio>
#include <unistd.h>

#include "gpu/metal.hpp"
#include "platform/launcher/launcher_metal.hpp"
#include "platform/macos/core_audio_output.hpp"
#include "runtime/pad_mapping.hpp"

// While the launcher is up, the keys it takes (arrows, Return / Enter / Space,
// Esc / Backspace), queued for MacGameWindow::choose.
namespace {
bool g_launcher_keys = false;
std::vector<dcusa::launcher::Action> g_keys;
}  // namespace

// The content view: hosts the Metal layer and keeps keystrokes from beeping
// (the game's controls come from game controllers, not the keyboard).
@interface DcusaGameView : NSView
@end

@implementation DcusaGameView
- (BOOL)acceptsFirstResponder {
    return YES;
}
- (void)keyDown:(NSEvent*)event {
    if (!g_launcher_keys) return;
    using dcusa::launcher::Action;
    switch (event.keyCode) {
        case 126: g_keys.push_back(Action::Up); break;
        case 125: g_keys.push_back(Action::Down); break;
        case 123: g_keys.push_back(Action::Left); break;
        case 124: g_keys.push_back(Action::Right); break;
        case 36:  // Return
        case 76:  // Enter
        case 49:  // Space
            if (!event.isARepeat) g_keys.push_back(Action::Confirm);
            break;
        case 53:  // Esc
        case 51:  // Backspace
            if (!event.isARepeat) g_keys.push_back(Action::Back);
            break;
        default: break;
    }
}
- (void)keyUp:(NSEvent*)event {
    (void)event;
}
- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    self.layer.contentsScale = self.window.backingScaleFactor;
}
@end


// Window close and Quit both end the run.
@interface DcusaGameWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic) BOOL closed;
@end

@implementation DcusaGameWindowDelegate
- (void)windowWillClose:(NSNotification*)note {
    (void)note;
    self.closed = YES;
}
- (void)quit:(id)sender {
    (void)sender;
    self.closed = YES;
}
@end

namespace dcusa {
namespace {

NSMenu* make_menu(DcusaGameWindowDelegate* delegate, NSString* title) {
    NSMenu* bar = [NSMenu new];
    NSMenuItem* app_item = [NSMenuItem new];
    [bar addItem:app_item];
    NSMenu* app_menu = [NSMenu new];
    NSMenuItem* quit = [[NSMenuItem alloc] initWithTitle:[@"Quit " stringByAppendingString:title]
                                                  action:@selector(quit:)
                                           keyEquivalent:@"q"];
    quit.target = delegate;
    [app_menu addItem:quit];
    app_item.submenu = app_menu;

    NSMenuItem* view_item = [NSMenuItem new];
    [bar addItem:view_item];
    NSMenu* view_menu = [[NSMenu alloc] initWithTitle:@"View"];
    NSMenuItem* full = [[NSMenuItem alloc] initWithTitle:@"Toggle Full Screen"
                                                  action:@selector(toggleFullScreen:)
                                           keyEquivalent:@"f"];
    full.keyEquivalentModifierMask = NSEventModifierFlagControl | NSEventModifierFlagCommand;
    [view_menu addItem:full];
    view_item.submenu = view_menu;
    return bar;
}

class MacGameWindow : public GameWindow {
public:
    MacGameWindow(const char* title, unsigned width, unsigned height) {
        @autoreleasepool {
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
            NSString* name = @(title);
            delegate_ = [DcusaGameWindowDelegate new];
            NSApp.mainMenu = make_menu(delegate_, name);
            [NSApp finishLaunching];

            const NSRect frame = NSMakeRect(0, 0, width, height);
            window_ = [[NSWindow alloc]
                initWithContentRect:frame
                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                    NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                            backing:NSBackingStoreBuffered
                              defer:NO];
            window_.title = name;
            window_.delegate = delegate_;
            window_.releasedWhenClosed = NO;
            window_.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
            window_.backgroundColor = NSColor.blackColor;
            window_.contentAspectRatio = NSMakeSize(16, 9);

            layer_ = [CAMetalLayer layer];
            DcusaGameView* view = [[DcusaGameView alloc] initWithFrame:frame];
            view.layer = layer_;  // layer-hosting: set the layer, then ask for one
            view.wantsLayer = YES;
            layer_.contentsScale = window_.backingScaleFactor;
            layer_.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
            window_.contentView = view;
            [window_ makeFirstResponder:view];
            [window_ center];
            [window_ makeKeyAndOrderFront:nil];
            [NSApp activateIgnoringOtherApps:YES];
            pump();
        }
    }

    void* layer() override { return (__bridge void*)layer_; }

    int choose(const std::vector<launcher::LauncherEdition>& editions, int start, PadSource* pad) override {
        @autoreleasepool {
            using launcher::Launcher;
            launcher::MetalLauncherRenderer renderer((__bridge void*)layer_);
            int w = 0, h = 0;
            renderer.drawable_size(w, h);
            Launcher ui;
            if (!ui.init(editions, start, w, h, true)) return start;
            // Its sounds and music, through the speakers until the game's own start.
            std::unique_ptr<AudioOutput> audio = make_core_audio_output();
            const auto pull = [](void* user, std::span<float> out) { static_cast<Launcher*>(user)->mix(out.data(), out.size() / 2); };
            if (audio && !audio->start(Launcher::kRate, pull, &ui)) audio.reset();
            auto* gamepad = dynamic_cast<GamepadSource*>(pad);
            g_launcher_keys = true;
            g_keys.clear();
            while (ui.result() == Launcher::Result::Running) {
                @autoreleasepool {
                    while (NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                               untilDate:nil
                                                                  inMode:NSDefaultRunLoopMode
                                                                 dequeue:YES])
                        [NSApp sendEvent:event];
                }
                if (delegate_.closed) {
                    std::fprintf(stderr, "window closed\n");
                    _exit(0);
                }
                for (launcher::Action a : g_keys) ui.key(a);
                g_keys.clear();
                if (gamepad != nullptr) ui.pad(gamepad->read());
                renderer.drawable_size(w, h);
                ui.resize(w, h);
                renderer.draw(ui, ui.frame());  // waits for the display's next frame
            }
            g_launcher_keys = false;
            if (audio) audio->stop();
            pump();
            return ui.result() == Launcher::Result::Start ? ui.chosen() : -1;
        }
    }

    void drawable_size(std::uint32_t& width, std::uint32_t& height) override {
        const CGSize s = layer_.drawableSize;
        width = static_cast<std::uint32_t>(s.width);
        height = static_cast<std::uint32_t>(s.height);
    }

    // Once per frame: hand AppKit the events that arrived since the last one.
    void present() override {
        if (![NSThread isMainThread]) return;  // AppKit events belong to the main thread
        pump();
        // A minimised or covered window may get no drawables, and waiting for
        // one would stall the game; draw into it only while it is on screen.
        const bool visible = (window_.occlusionState & NSWindowOcclusionStateVisible) != 0;
        if (visible != attached_) {
            attached_ = visible;
            gpu::set_layer(visible ? layer() : nullptr);
        }
        if (delegate_.closed) {
            std::fprintf(stderr, "window closed\n");
            std::fflush(stderr);
            // Guest threads are still running; leave without unwinding them.
            _exit(0);
        }
    }

private:
    void pump() {
        @autoreleasepool {
            while (NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                       untilDate:nil
                                                          inMode:NSDefaultRunLoopMode
                                                         dequeue:YES])
                [NSApp sendEvent:event];
            [NSApp updateWindows];
        }
    }

    bool attached_ = true;  // dcrun attaches the layer when it creates the window
    NSWindow* __strong window_ = nil;
    CAMetalLayer* __strong layer_ = nil;
    DcusaGameWindowDelegate* __strong delegate_ = nil;
};

}  // namespace

std::unique_ptr<GameWindow> make_game_window(const char* title, unsigned width, unsigned height) {
    return std::make_unique<MacGameWindow>(title, width, height);
}

}  // namespace dcusa
