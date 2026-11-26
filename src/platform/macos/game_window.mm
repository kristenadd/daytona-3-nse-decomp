#include "platform/macos/game_window.hpp"

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include <chrono>
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

// The edition picker: the editions' title images on a carousel
// (dcusa::Carousel: a ring seen in perspective, the cards always facing the
// viewer), each with a faint reflection on a dark floor. The front card has a
// white frame and its name below. Keys and clicks are queued for
// MacGameWindow::choose: a click on the front card chooses it, on another
// turns the ring to it.
@interface DcusaPickerView : NSView
@property(nonatomic) NSInteger selected;
@property(nonatomic) NSInteger confirmed;  // -1 until a choice is made
@property(nonatomic) dcusa::Carousel* carousel;
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
    }
    return self;
}
- (BOOL)acceptsFirstResponder {
    return YES;
}
- (BOOL)isFlipped {
    return YES;
}
- (NSRect)rectOf:(const dcusa::CarouselCard&)k {
    return NSMakeRect(k.cx - k.w / 2, k.cy - k.h / 2, k.w, k.h);
}
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    if (_carousel == nullptr) return;
    const NSRect b = self.bounds;
    const float vw = static_cast<float>(b.size.width), vh = static_cast<float>(b.size.height);
    // Black above, a dark floor fading in below the cards' feet.
    [NSColor.blackColor setFill];
    NSRectFill(b);
    const dcusa::CarouselCard front = _carousel->card(static_cast<int>(self.selected), vw, vh);
    const CGFloat floor_y = front.cy + front.h / 2;
    NSGradient* floor = [[NSGradient alloc] initWithStartingColor:[NSColor colorWithWhite:0.0 alpha:1]
                                                      endingColor:[NSColor colorWithWhite:0.10 alpha:1]];
    [floor drawInRect:NSMakeRect(0, floor_y - vh * 0.25, vw, vh - floor_y + vh * 0.25) angle:90];
    for (int i : _carousel->draw_order(vw, vh)) {
        const dcusa::CarouselCard k = _carousel->card(i, vw, vh);
        const NSRect r = [self rectOf:k];
        NSImage* image = _images[static_cast<NSUInteger>(i)];
        const bool has_image = image.size.width > 0;
        // The reflection: the card upside down below it, fading out over half
        // its height (drawn in strips, each fainter than the last).
        if (has_image) {
            NSRect m = r;
            m.origin.y = NSMaxY(r) + 3;
            [NSGraphicsContext saveGraphicsState];
            NSAffineTransform* flip = [NSAffineTransform transform];
            [flip translateXBy:0 yBy:NSMaxY(m) + m.origin.y];
            [flip scaleXBy:1 yBy:-1];
            [flip concat];
            constexpr int kStrips = 24;
            const CGFloat strip = m.size.height * 0.5 / kStrips;
            for (int j = 0; j < kStrips; ++j) {
                // Strip j of the mirror, counted from the card's foot; in the
                // flipped drawing it is the image's bottom rows.
                const NSRect dst = NSMakeRect(m.origin.x, NSMaxY(m) - (j + 1) * strip, m.size.width, strip);
                const NSRect src = NSMakeRect(0, image.size.height * (j * strip / m.size.height), image.size.width,
                                              image.size.height * (strip / m.size.height));
                const CGFloat fade = 1.0 - (j + 0.5) / kStrips;
                [image drawInRect:dst fromRect:src operation:NSCompositingOperationSourceOver
                         fraction:0.28 * fade * fade * k.brightness respectFlipped:NO hints:nil];
            }
            [NSGraphicsContext restoreGraphicsState];
        }
        if (has_image) {
            [image drawInRect:r fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1.0
                     respectFlipped:YES hints:nil];
        } else {
            [[NSColor colorWithWhite:0.25 alpha:1] setFill];
            NSRectFill(r);
        }
        if (k.brightness < 1) {  // farther round the ring: darker
            [[NSColor colorWithWhite:0 alpha:1 - k.brightness] setFill];
            NSRectFillUsingOperation(r, NSCompositingOperationSourceOver);
        }
        if (k.title_alpha > 0) {
            [[NSColor colorWithWhite:1 alpha:k.title_alpha] setStroke];
            NSBezierPath* frame = [NSBezierPath bezierPathWithRect:NSInsetRect(r, -4, -4)];
            frame.lineWidth = 4;
            [frame stroke];
            const CGFloat text_size = MAX(14.0, vw * 0.024);
            NSMutableParagraphStyle* centre = [NSMutableParagraphStyle new];
            centre.alignment = NSTextAlignmentCenter;
            NSDictionary* attrs = @{
                NSFontAttributeName : [NSFont boldSystemFontOfSize:text_size],
                NSForegroundColorAttributeName : [NSColor colorWithWhite:1 alpha:k.title_alpha],
                NSParagraphStyleAttributeName : centre
            };
            [_titles[static_cast<NSUInteger>(i)] drawInRect:NSMakeRect(0, vh * 0.88, vw, text_size * 1.6)
                                              withAttributes:attrs];
        }
    }
}
- (void)mouseDown:(NSEvent*)event {
    if (_carousel == nullptr) return;
    const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    const NSRect b = self.bounds;
    const int i = _carousel->hit(static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(b.size.width),
                                 static_cast<float>(b.size.height));
    if (i < 0) return;
    if (i == self.selected) self.confirmed = i;
    else self.selected = i;
}
- (void)keyDown:(NSEvent*)event {
    NSString* k = event.charactersIgnoringModifiers;
    if (k.length == 0) return;
    const unichar ch = [k characterAtIndex:0];
    if (ch == NSLeftArrowFunctionKey && self.selected > 0) --self.selected;
    else if (ch == NSRightArrowFunctionKey && self.selected + 1 < (NSInteger)_titles.count) ++self.selected;
    else if (ch == '\r' || ch == ' ' || ch == 3) self.confirmed = self.selected;
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

    int choose(const std::vector<EditionChoice>& given, PadSource* pad) override {
        @autoreleasepool {
            // Shown oldest first, left to right, starting on 2019; the result
            // is an index into `given`.
            std::vector<EditionChoice> items = given;
            const int start = arrange_for_picker(items);
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
            PickerInput input(static_cast<int>(items.size()), start);
            Carousel carousel(static_cast<int>(items.size()), start);
            picker.selected = start;
            picker.carousel = &carousel;
            int chosen = -1;
            auto last = std::chrono::steady_clock::now();
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
                    if (input.selected() != before) picker.selected = input.selected();
                }
                if (picker.confirmed >= 0) chosen = static_cast<int>(picker.confirmed);
                // Turn the ring towards the selection, redrawing while it moves.
                const auto now = std::chrono::steady_clock::now();
                carousel.select(static_cast<int>(picker.selected));
                if (carousel.step(std::chrono::duration<double>(now - last).count())) picker.needsDisplay = YES;
                last = now;
            }
            picker.carousel = nullptr;
            window_.contentView = game_view;
            [window_ makeFirstResponder:game_view];
            pump();
            for (std::size_t i = 0; i < given.size(); ++i)
                if (given[i].edition == items[static_cast<std::size_t>(chosen)].edition) return static_cast<int>(i);
            return 0;
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
