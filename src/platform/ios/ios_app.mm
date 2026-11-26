// The game as an iOS / iPadOS app (iPhone and iPad).
//
// UIKit owns the main thread, so the game runs on a thread of its own with a
// large stack (translated code recurses deeply and keeps guest registers in
// host locals). The view's layer is the CAMetalLayer the graphics device draws
// each frame into (gpu::show, on the render thread).
//
// The player's files live in the app's Documents folder, which the Files app
// and Finder can reach (UIFileSharingEnabled): the prepared caches built on a
// Mac by dcprepare (DCUSA2019.cache, DCUSA2017.cache), the translated shaders
// (shaders/msl, shaders/src) and the game install folders as found on the PC.
// With both editions there, the app first shows their title images to choose
// from (controller or tap). Settings the game writes go to Documents/container
// (2019) or Documents/container-2017. Shaders cannot be translated on the
// device (no Python, no processes), so the shaders folder must hold every one
// the game uses.
#import <GameController/GameController.h>
#import <AVFoundation/AVFoundation.h>
#import <QuartzCore/CAMetalLayer.h>
#import <UIKit/UIKit.h>

#include <mach/mach.h>
#include <pthread.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "audio/ds_mixer.hpp"
#include "gpu/metal.hpp"
#include "platform/edition_choice.hpp"
#include "platform/macos/core_audio_output.hpp"
#include "platform/macos/gamecontroller_pad.hpp"
#include "recomp/d3d11.hpp"
#include "recomp/generated.hpp"
#include "recomp/host_files.hpp"
#include "recomp/loader.hpp"
#include "recomp/win32.hpp"
#include "runtime/fail_closed.hpp"
#include "runtime/pad_script.hpp"
#include "system/content.hpp"
#include "system/edition.hpp"
#include "system/exe_image.hpp"

@interface DCMetalView : UIView
@end

@implementation DCMetalView
+ (Class)layerClass {
    return [CAMetalLayer class];
}
@end

namespace {

// The game draws into the layer from the render thread; present() has nothing to do.
class LayerPresenter : public dcusa::Presenter {
public:
    explicit LayerPresenter(CAMetalLayer* layer) : layer_(layer) {}
    void drawable_size(std::uint32_t& width, std::uint32_t& height) override {
        const CGSize s = layer_.drawableSize;
        width = static_cast<std::uint32_t>(s.width);
        height = static_cast<std::uint32_t>(s.height);
    }
    void present() override {}

private:
    __weak CAMetalLayer* layer_;
};

struct Game {
    CAMetalLayer* layer = nil;
    std::string documents;
    std::unique_ptr<LayerPresenter> presenter;
    std::unique_ptr<dcusa::PadSource> pad;
    std::unique_ptr<dcusa::AudioOutput> audio;
    std::optional<dcusa::EditionChoice> choice;  // the edition to run (set before the game thread starts)
    std::string no_choice;                        // why none could be offered
    dcusa::ExeImage image;
    std::unique_ptr<dcusa::recomp::GuestProcess> process;
    void (^report)(NSString*) = nil;  // a message on screen
};

Game& game() {
    static Game* g = new Game;  // never destroyed: the game thread outlives everything
    return *g;
}

void say(const std::string& text) {
    std::fprintf(stderr, "%s\n", text.c_str());
    NSString* message = @(text.c_str());
    void (^report)(NSString*) = game().report;
    if (report != nil) dispatch_async(dispatch_get_main_queue(), ^{ report(message); });
}

// The editions this app can run from Documents. Title images: Documents/boot,
// else the ones the build put in the bundle (boot_title.png, boot_title2017.png).
std::vector<dcusa::EditionChoice> find_choices(const std::filesystem::path& docs, std::string* why) {
    std::vector<dcusa::EditionChoice> choices = dcusa::available_editions(docs, docs, {docs / "boot"}, why);
    for (dcusa::EditionChoice& c : choices) {
        if (!c.image.empty()) continue;
        NSString* name = c.edition == dcusa::Edition::Dcu2017 ? @"boot_title2017" : @"boot_title";
        if (NSString* bundled = [NSBundle.mainBundle pathForResource:name ofType:@"png"]) c.image = bundled.UTF8String;
    }
    return choices;
}

void* run_game(void*) {
    Game& g = game();
    const std::filesystem::path docs = g.documents;
    // stderr (the runtime's messages) goes to Documents/log.txt, which the
    // Files app and devicectl can read back from a device.
    if (std::freopen((docs / "log.txt").c_str(), "w", stderr) != nullptr) setvbuf(stderr, nullptr, _IOLBF, 0);
    std::fprintf(stderr, "ios: game thread started\n");
    // Relative paths (DCUSA_FRAME=f, from devicectl --environment-variables) land in Documents.
    chdir(docs.c_str());
    // The footprint iOS kills an app over, every 10 s, in log.txt.
    std::thread([] {
        for (int s = 10;; s += 10) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            task_vm_info_data_t info{};
            mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
            // Thermal state too: a phone that is serious or critical runs slower.
            static const char* const kThermal[] = {"nominal", "fair", "serious", "critical"};
            const NSInteger heat = NSProcessInfo.processInfo.thermalState;
            if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &n) == KERN_SUCCESS)
                std::fprintf(stderr, "ios: %d s footprint %llu MB, thermal %s\n", s,
                             static_cast<unsigned long long>(info.phys_footprint >> 20),
                             heat >= 0 && heat < 4 ? kThermal[heat] : "?");
        }
    }).detach();
    const std::filesystem::path shaders = docs / "shaders";
    std::error_code ec;
    if (!g.choice || !std::filesystem::is_directory(shaders / "msl", ec)) {
        say("Copy into this app's folder (Files app, or Finder with the device connected):\n"
            "  DCUSA2019.cache and / or DCUSA2017.cache (from dcprepare on a Mac)\n"
            "  shaders (with msl and src inside)\n"
            "  the Daytona Championship USA game folder(s)\n\n" + g.no_choice);
        return nullptr;
    }
    const dcusa::EditionChoice choice = *g.choice;
    const std::filesystem::path cache = choice.cache;
    setenv("DCUSA_METAL", "1", 0);
    setenv("DCUSA_SHADERS", shaders.c_str(), 0);
    // The boot screen (the Settings app's "Title boot screen", on unless turned
    // off): the attract title with a progress bar instead of "Data Loading".
    // The title is Documents/boot/title.png, else the one the build put in the
    // bundle (tools/bootscreen/capture_title.py, from the player's game on a Mac).
    {
        NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
        // "Show frame rate": pictures per second and the build, lower left.
        if ([defaults boolForKey:@"show_fps"]) setenv("DCUSA_SHOW_FPS", "1", 0);
        const bool on = [defaults objectForKey:@"boot_screen"] == nil || [defaults boolForKey:@"boot_screen"];
        const std::string image = choice.image.string();  // the chosen edition's title
        if (on && !image.empty()) {
            setenv("DCUSA_BOOT_SCREEN", "1", 0);
            setenv("DCUSA_BOOT_SCREEN_IMAGE", image.c_str(), 0);
            if (NSString* font = [NSBundle.mainBundle pathForResource:@"boot_font" ofType:@"ttf"])
                setenv("DCUSA_BOOT_SCREEN_FONT", font.UTF8String, 0);
        }
    }
    std::string error;
    if (!dcusa::exe_image_load_cache(cache.c_str(), choice.edition, g.image, &error)) {
        say("Cannot read " + cache.filename().string() + ":\n" + error);
        return nullptr;
    }
    std::fprintf(stderr, "ios: cache loaded\n");
    g.process = std::make_unique<dcusa::recomp::GuestProcess>();
    if (!g.process->load(g.image, &error)) {
        say("Cannot load the game: " + error);
        return nullptr;
    }
    std::fprintf(stderr, "ios: guest loaded\n");
    const dcusa::InstallScan scan = dcusa::find_installs(docs);
    const dcusa::GameInstall* install = dcusa::pick_install(scan, g.image.edition);
    if (install == nullptr) {
        std::string why = "No Daytona Championship USA game folder found in this app's folder.";
        for (const std::string& problem : scan.problems) why += "\n" + problem;
        say(why);
        return nullptr;
    }
    const std::filesystem::path writable = docs / dcusa::container_name(choice.edition);
    std::filesystem::create_directories(writable, ec);
    dcusa::recomp::set_guest_files(install->install_root, install->game_dir.filename().string(), writable);
    if (!install->base_root.empty()) {  // a slim install: its shared files come from the base
        dcusa::recomp::set_guest_base(install->base_root, install->shared);
        std::fprintf(stderr, "shared files from %s: %zu\n", install->base_root.c_str(), install->shared.size());
    }
    if (!dcusa::gpu::enabled()) {
        say("No Metal device, or no translated shaders in the shaders folder.");
        return nullptr;
    }
    // DCUSA_PAD_SCRIPT=<file in Documents> plays a script (runtime/pad_script.hpp),
    // as dcrun does; otherwise the first game controller.
    if (const char* script = std::getenv("DCUSA_PAD_SCRIPT")) {
        std::ifstream in(script);
        std::stringstream text;
        text << in.rdbuf();
        auto scripted = std::make_unique<dcusa::ScriptPad>();
        std::string why;
        if (!in || !scripted->parse(text.str(), &why)) {
            say(why.empty() ? "cannot read DCUSA_PAD_SCRIPT" : why);
            return nullptr;
        }
        g.pad = std::move(scripted);
    } else {
        g.pad = dcusa::make_gamecontroller_pad();
    }
    dcusa::recomp::set_pad_source(g.pad.get());
    dcusa::gpu::set_layer((__bridge void*)g.layer);
    g.presenter = std::make_unique<LayerPresenter>(g.layer);
    dcusa::recomp::set_presenter(g.presenter.get());
    g.audio = dcusa::make_core_audio_output();
    if (!g.audio->start(dcusa::audio::DsMixer::kOutputRate, dcusa::audio::DsMixer::pull, &dcusa::recomp::sound_mixer()))
        g.audio.reset();
    std::fprintf(stderr, "ios: starting the game\n");
    try {
        g.process->start(dcusa::recomp::translated_entries(choice.edition));
        say("The game ended.");
    } catch (const dcusa::FailClosed& stopped) {
        say("The game stopped:\n" + stopped.report().describe());
    } catch (const std::exception& e) {
        say(std::string("The game stopped: ") + e.what());
    }
    return nullptr;
}

void start_game_thread() {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64u << 20);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);  // the game's main thread: a performance core
    pthread_t thread;
    pthread_create(&thread, &attr, run_game, nullptr);
    pthread_attr_destroy(&attr);
}

}  // namespace

@interface DCViewController : UIViewController
@end

@implementation DCViewController {
    UILabel* _message;
    BOOL _started;
    // The edition picker, while it is up.
    std::vector<dcusa::EditionChoice> _choices;
    std::unique_ptr<dcusa::PickerInput> _input;
    std::unique_ptr<dcusa::Carousel> _carousel;
    UIView* _picker;
    CAGradientLayer* _floor;
    NSMutableArray<UIImageView*>* _images;
    NSMutableArray<UIImageView*>* _mirrors;
    NSMutableArray<UIView*>* _dims;
    NSMutableArray<UILabel*>* _titles;
    CADisplayLink* _poll;
}

- (void)loadView {
    DCMetalView* view = [DCMetalView new];
    view.backgroundColor = UIColor.blackColor;
    self.view = view;
    _message = [UILabel new];
    _message.textColor = UIColor.whiteColor;
    _message.numberOfLines = 0;
    _message.textAlignment = NSTextAlignmentCenter;
    _message.font = [UIFont monospacedSystemFontOfSize:14 weight:UIFontWeightRegular];
    _message.hidden = YES;
    [view addSubview:_message];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    _message.frame = CGRectInset(self.view.bounds, 24, 24);
    [self layoutPicker];
    CAMetalLayer* layer = (CAMetalLayer*)self.view.layer;
    const CGFloat scale = self.view.window.windowScene.screen.nativeScale ?: 2.0;
    layer.contentsScale = scale;
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    if (_started) return;
    _started = YES;
    Game& g = game();
    g.layer = (CAMetalLayer*)self.view.layer;
    NSURL* docs = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
    g.documents = docs.path.UTF8String;
    UILabel* label = _message;
    g.report = ^(NSString* text) {
        label.text = text;
        label.hidden = NO;
    };
    _choices = find_choices(g.documents, &g.no_choice);
    if (_choices.size() > 1 && !std::getenv("DCUSA_EDITION")) {
        [self showPicker];
        return;
    }
    // One edition, or DCUSA_EDITION (devicectl --environment-variables) picks it.
    const std::optional<dcusa::Edition> want = dcusa::edition_from_environment();
    for (const dcusa::EditionChoice& c : _choices)
        if (!want || c.edition == *want) {
            g.choice = c;
            break;
        }
    start_game_thread();
}

// The editions' title images on a carousel (dcusa::Carousel: a ring seen in
// perspective, the cards always facing the viewer), each with a reflection
// on a dark floor; the front card framed, its name below.
- (void)showPicker {
    _input = std::make_unique<dcusa::PickerInput>(static_cast<int>(_choices.size()));
    _carousel = std::make_unique<dcusa::Carousel>(static_cast<int>(_choices.size()));
    _picker = [UIView new];
    _picker.backgroundColor = UIColor.blackColor;
    [_picker addGestureRecognizer:[[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(tapped:)]];
    _floor = [CAGradientLayer layer];
    _floor.colors = @[ (id)UIColor.blackColor.CGColor, (id)[UIColor colorWithWhite:0.10 alpha:1].CGColor ];
    [_picker.layer addSublayer:_floor];
    _images = [NSMutableArray array];
    _mirrors = [NSMutableArray array];
    _dims = [NSMutableArray array];
    _titles = [NSMutableArray array];
    for (std::size_t i = 0; i < _choices.size(); ++i) {
        UIImage* picture = _choices[i].image.empty() ? nil : [UIImage imageWithContentsOfFile:@(_choices[i].image.c_str())];
        UIImageView* image = [[UIImageView alloc] initWithImage:picture];
        image.contentMode = UIViewContentModeScaleToFill;
        image.backgroundColor = [UIColor colorWithWhite:0.25 alpha:1];
        image.layer.borderColor = UIColor.whiteColor.CGColor;
        UIView* dim = [UIView new];  // farther round the ring: darker
        dim.backgroundColor = UIColor.blackColor;
        [image addSubview:dim];
        // The reflection: upside down, fading out over half its height.
        UIImageView* mirror = [[UIImageView alloc] initWithImage:picture];
        mirror.contentMode = UIViewContentModeScaleToFill;
        mirror.transform = CGAffineTransformMakeScale(1, -1);
        CAGradientLayer* fade = [CAGradientLayer layer];
        fade.colors = @[ (id)[UIColor colorWithWhite:0 alpha:0].CGColor, (id)[UIColor colorWithWhite:0 alpha:0.3].CGColor ];
        fade.locations = @[ @0.5, @1.0 ];  // flipped: the card's foot is at the bottom of this layer
        mirror.layer.mask = fade;
        UILabel* title = [UILabel new];
        title.text = @(_choices[i].title.c_str());
        title.textColor = UIColor.whiteColor;
        title.textAlignment = NSTextAlignmentCenter;
        [_picker addSubview:mirror];
        [_picker addSubview:image];
        [_picker addSubview:title];
        [_images addObject:image];
        [_mirrors addObject:mirror];
        [_dims addObject:dim];
        [_titles addObject:title];
    }
    [self.view addSubview:_picker];
    [self layoutPicker];
    _poll = [CADisplayLink displayLinkWithTarget:self selector:@selector(pollPad:)];
    [_poll addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
}

// Every card where the carousel puts it now, back to front.
- (void)layoutPicker {
    if (_picker == nil) return;
    const CGRect b = self.view.bounds;
    const float vw = static_cast<float>(b.size.width), vh = static_cast<float>(b.size.height);
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _picker.frame = b;
    const dcusa::CarouselCard front = _carousel->card(_input->selected(), vw, vh);
    const CGFloat floor_y = front.cy + front.h / 2 - vh * 0.25;
    _floor.frame = CGRectMake(0, floor_y, vw, vh - floor_y);
    const CGFloat text = MAX(14.0, b.size.width * 0.024);
    CGFloat z = 0;
    for (int i : _carousel->draw_order(vw, vh)) {
        const dcusa::CarouselCard k = _carousel->card(i, vw, vh);
        const CGRect r = CGRectMake(k.cx - k.w / 2, k.cy - k.h / 2, k.w, k.h);
        UIImageView* image = _images[static_cast<NSUInteger>(i)];
        UIImageView* mirror = _mirrors[static_cast<NSUInteger>(i)];
        image.frame = r;
        image.layer.zPosition = z + 2;
        image.layer.borderWidth = 4 * k.title_alpha;
        _dims[static_cast<NSUInteger>(i)].frame = image.bounds;
        _dims[static_cast<NSUInteger>(i)].alpha = 1 - k.brightness;
        mirror.bounds = CGRectMake(0, 0, r.size.width, r.size.height);
        mirror.center = CGPointMake(CGRectGetMidX(r), CGRectGetMaxY(r) + 3 + r.size.height / 2);
        mirror.layer.mask.frame = mirror.bounds;
        mirror.alpha = k.brightness;
        mirror.layer.zPosition = z + 1;
        UILabel* title = _titles[static_cast<NSUInteger>(i)];
        title.font = [UIFont boldSystemFontOfSize:text];
        title.frame = CGRectMake(0, vh * 0.88, vw, text * 1.6);
        title.alpha = k.title_alpha;
        title.layer.zPosition = 100;
        z += 3;
    }
    [CATransaction commit];
}

// A tap on the front card chooses it; on another, turns the ring to it.
- (void)tapped:(UITapGestureRecognizer*)tap {
    const CGPoint p = [tap locationInView:_picker];
    const CGRect b = _picker.bounds;
    const int i = _carousel->hit(static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(b.size.width),
                                 static_cast<float>(b.size.height));
    if (i < 0) return;
    if (i == _input->selected()) [self choose:i];
    else _input->select(i);
}

// Every frame: the controller (left / right on the d-pad or stick, then A,
// Menu or the right trigger) and the ring's turn.
- (void)pollPad:(CADisplayLink*)link {
    GCExtendedGamepad* pad = GCController.current.extendedGamepad;
    if (pad == nil) {
        for (GCController* c in GCController.controllers)
            if (c.extendedGamepad != nil) {
                pad = c.extendedGamepad;
                break;
            }
    }
    if (pad != nil) {
        const float x = std::fabs(pad.dpad.xAxis.value) > std::fabs(pad.leftThumbstick.xAxis.value)
                            ? pad.dpad.xAxis.value
                            : pad.leftThumbstick.xAxis.value;
        const bool confirm = pad.buttonA.pressed || pad.buttonMenu.pressed || pad.rightTrigger.value > 0.5f;
        if (_input->pad(x, confirm)) {
            [self choose:_input->selected()];
            return;
        }
    }
    _carousel->select(_input->selected());
    if (_carousel->step(link.targetTimestamp - link.timestamp)) [self layoutPicker];
}

- (void)choose:(int)index {
    if (_picker == nil || index < 0 || index >= static_cast<int>(_choices.size())) return;
    [_poll invalidate];
    _poll = nil;
    [_picker removeFromSuperview];
    _picker = nil;
    game().choice = _choices[static_cast<std::size_t>(index)];
    std::fprintf(stderr, "ios: edition %s chosen\n", dcusa::edition_info(_choices[static_cast<std::size_t>(index)].edition).id.data());
    start_game_thread();
}

- (BOOL)prefersStatusBarHidden {
    return YES;
}

- (BOOL)prefersHomeIndicatorAutoHidden {
    return YES;
}

- (UIInterfaceOrientationMask)supportedInterfaceOrientations {
    return UIInterfaceOrientationMaskLandscape;
}

@end

@interface DCSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow* window;
@end

@implementation DCSceneDelegate
- (void)scene:(UIScene*)scene willConnectToSession:(UISceneSession*)session options:(UISceneConnectionOptions*)options {
    if (![scene isKindOfClass:[UIWindowScene class]]) return;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene*)scene];
    self.window.rootViewController = [DCViewController new];
    [self.window makeKeyAndVisible];
}
@end

@interface DCAppDelegate : UIResponder <UIApplicationDelegate>
@end

@implementation DCAppDelegate
- (BOOL)application:(UIApplication*)application didFinishLaunchingWithOptions:(NSDictionary*)options {
    application.idleTimerDisabled = YES;  // a game the player drives with a pad
    return YES;
}

- (UISceneConfiguration*)application:(UIApplication*)application
    configurationForConnectingSceneSession:(UISceneSession*)session
                                   options:(UISceneConnectionOptions*)options {
    UISceneConfiguration* config = [[UISceneConfiguration alloc] initWithName:@"Default" sessionRole:session.role];
    config.delegateClass = [DCSceneDelegate class];
    return config;
}
@end

int main(int argc, char** argv) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([DCAppDelegate class]));
    }
}
