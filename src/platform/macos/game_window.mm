#include "platform/macos/game_window.hpp"

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include <cstdio>
#include <unistd.h>

#include "gpu/metal.hpp"

// The content view: hosts the Metal layer and keeps keystrokes from beeping
// (the controls come from game controllers, not the keyboard).
@interface DcusaGameView : NSView
@end

@implementation DcusaGameView
- (BOOL)acceptsFirstResponder {
    return YES;
}
- (void)keyDown:(NSEvent*)event {
    (void)event;
}
- (void)keyUp:(NSEvent*)event {
    (void)event;
}
- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    self.layer.contentsScale = self.window.backingScaleFactor;
}
@end

// The edition picker: each edition's title image with its name under it, the
// selected one at full brightness with a white frame. Keys and clicks are
// queued for MacGameWindow::choose.
@interface DcusaPickerView : NSView
@property(nonatomic) NSInteger selected;
@property(nonatomic) NSInteger confirmed;  // -1 until a choice is made
- (instancetype)initWithFrame:(NSRect)frame images:(NSArray<NSImage*>*)images titles:(NSArray<NSString*>*)titles;
@end

@implementation DcusaPickerView {
    NSArray<NSImage*>* _images;
    NSArray<NSString*>* _titles;
}
- (instancetype)initWithFrame:(NSRect)frame images:(NSArray<NSImage*>*)images titles:(NSArray<NSString*>*)titles {
    if ((self = [super initWithFrame:frame])) {
        _images = images;
        _titles = titles;
        _confirmed = -1;
        self.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:NSZeroRect
                                                           options:NSTrackingMouseMoved | NSTrackingActiveInKeyWindow |
                                                                   NSTrackingInVisibleRect
                                                             owner:self
                                                          userInfo:nil]];
    }
    return self;
}
- (BOOL)acceptsFirstResponder {
    return YES;
}
- (BOOL)isFlipped {
    return YES;
}
// Slot i: two 16:9 images side by side, a gap between, centred.
- (NSRect)slot:(NSInteger)i {
    const NSInteger n = (NSInteger)_titles.count;
    const CGFloat gap = self.bounds.size.width * 0.04;
    const CGFloat w = (self.bounds.size.width - gap * (n + 1)) / n;
    const CGFloat h = w * 9.0 / 16.0;
    const CGFloat y = (self.bounds.size.height - h) / 2 - h * 0.08;
    return NSMakeRect(gap + i * (w + gap), y, w, h);
}
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    [NSColor.blackColor setFill];
    NSRectFill(self.bounds);
    const CGFloat text_size = MAX(14.0, self.bounds.size.width * 0.022);
    for (NSInteger i = 0; i < (NSInteger)_titles.count; ++i) {
        const NSRect r = [self slot:i];
        const BOOL on = i == self.selected;
        NSImage* image = _images[i];
        if (image.size.width > 0) {
            [image drawInRect:r fromRect:NSZeroRect operation:NSCompositingOperationSourceOver
                     fraction:on ? 1.0 : 0.45 respectFlipped:YES hints:nil];
        } else {
            [[NSColor colorWithWhite:on ? 0.25 : 0.12 alpha:1] setFill];
            NSRectFill(r);
        }
        if (on) {
            [NSColor.whiteColor setStroke];
            NSBezierPath* frame = [NSBezierPath bezierPathWithRect:NSInsetRect(r, -4, -4)];
            frame.lineWidth = 4;
            [frame stroke];
        }
        NSMutableParagraphStyle* centre = [NSMutableParagraphStyle new];
        centre.alignment = NSTextAlignmentCenter;
        NSDictionary* attrs = @{
            NSFontAttributeName : [NSFont boldSystemFontOfSize:text_size],
            NSForegroundColorAttributeName : [NSColor colorWithWhite:1 alpha:on ? 1.0 : 0.5],
            NSParagraphStyleAttributeName : centre
        };
        [_titles[i] drawInRect:NSMakeRect(r.origin.x, NSMaxY(r) + text_size, r.size.width, text_size * 1.6)
                withAttributes:attrs];
    }
}
- (NSInteger)hit:(NSEvent*)event {
    const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    for (NSInteger i = 0; i < (NSInteger)_titles.count; ++i)
        if (NSPointInRect(p, [self slot:i])) return i;
    return -1;
}
- (void)mouseMoved:(NSEvent*)event {
    const NSInteger i = [self hit:event];
    if (i >= 0 && i != self.selected) {
        self.selected = i;
        self.needsDisplay = YES;
    }
}
- (void)mouseDown:(NSEvent*)event {
    const NSInteger i = [self hit:event];
    if (i >= 0) self.confirmed = i;
}
- (void)keyDown:(NSEvent*)event {
    NSString* k = event.charactersIgnoringModifiers;
    if (k.length == 0) return;
    const unichar ch = [k characterAtIndex:0];
    if (ch == NSLeftArrowFunctionKey && self.selected > 0) --self.selected;
    else if (ch == NSRightArrowFunctionKey && self.selected + 1 < (NSInteger)_titles.count) ++self.selected;
    else if (ch == '\r' || ch == ' ' || ch == 3) self.confirmed = self.selected;
    self.needsDisplay = YES;
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

    int choose(const std::vector<EditionChoice>& items, PadSource* pad) override {
        @autoreleasepool {
            NSMutableArray<NSImage*>* images = [NSMutableArray array];
            NSMutableArray<NSString*>* titles = [NSMutableArray array];
            for (const EditionChoice& item : items) {
                NSImage* image = item.image.empty() ? nil : [[NSImage alloc] initWithContentsOfFile:@(item.image.c_str())];
                [images addObject:image ?: [NSImage new]];
                [titles addObject:@(item.title.c_str())];
            }
            NSView* game_view = window_.contentView;
            DcusaPickerView* picker = [[DcusaPickerView alloc] initWithFrame:game_view.bounds images:images titles:titles];
            // The game view hosts the Metal layer and takes no subviews: the
            // picker stands in for it until a choice is made.
            window_.contentView = picker;
            [window_ makeFirstResponder:picker];
            PickerInput input(static_cast<int>(items.size()));
            int chosen = -1;
            while (chosen < 0) {
                @autoreleasepool {
                    NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                        untilDate:[NSDate dateWithTimeIntervalSinceNow:1.0 / 60]
                                                           inMode:NSDefaultRunLoopMode
                                                          dequeue:YES];
                    if (event != nil) [NSApp sendEvent:event];
                }
                if (delegate_.closed) {
                    std::fprintf(stderr, "window closed\n");
                    _exit(0);
                }
                input.select(static_cast<int>(picker.selected));
                if (pad != nullptr) {
                    const PadSnapshot s = pad->sample();
                    const bool confirm = (s.buttons & (PadSnapshot::Start | PadSnapshot::View1 | PadSnapshot::View2 |
                                                       PadSnapshot::View3 | PadSnapshot::View4)) != 0 ||
                                         s.accelerator > 0.5f;
                    const int before = input.selected();
                    if (input.pad(s.steering, confirm)) chosen = input.selected();
                    if (input.selected() != before) {
                        picker.selected = input.selected();
                        picker.needsDisplay = YES;
                    }
                }
                if (picker.confirmed >= 0) chosen = static_cast<int>(picker.confirmed);
            }
            window_.contentView = game_view;
            [window_ makeFirstResponder:game_view];
            pump();
            return chosen;
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
