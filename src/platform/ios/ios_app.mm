// The game as an iOS / iPadOS app (iPhone and iPad).
//
// UIKit owns the main thread, so the game runs on a thread of its own with a
// large stack (translated code recurses deeply and keeps guest registers in
// host locals). The view's layer is the CAMetalLayer the graphics device draws
// each frame into (gpu::show, on the render thread).
//
// The player's files live in the app's Documents folder, which the Files app
// and Finder can reach (UIFileSharingEnabled): the prepared cache built on a Mac
// by dcprepare (DCUSA2019.cache), its translated shaders (shaders/msl, shaders/src)
// and the game install folder as found on the PC. Settings the game writes go to
// Documents/container. Shaders cannot be translated on the device (no Python, no
// processes), so the shaders folder must hold every one the game uses.
#import <AVFoundation/AVFoundation.h>
#import <QuartzCore/CAMetalLayer.h>
#import <UIKit/UIKit.h>

#include <mach/mach.h>
#include <pthread.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

#include "audio/ds_mixer.hpp"
#include "gpu/metal.hpp"
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

// The first file in Documents with a name ending in .cache.
std::filesystem::path find_cache(const std::filesystem::path& docs) {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(docs, ec))
        if (entry.is_regular_file() && entry.path().extension() == ".cache") return entry.path();
    return {};
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
    const std::filesystem::path cache = find_cache(docs);
    const std::filesystem::path shaders = docs / "shaders";
    std::error_code ec;
    if (cache.empty() || !std::filesystem::is_directory(shaders / "msl", ec)) {
        say("Copy into this app's folder (Files app, or Finder with the device connected):\n"
            "  DCUSA2019.cache (from dcprepare on a Mac)\n"
            "  shaders (with msl and src inside)\n"
            "  the Daytona Championship USA game folder");
        return nullptr;
    }
    setenv("DCUSA_METAL", "1", 0);
    setenv("DCUSA_SHADERS", shaders.c_str(), 0);
    // The boot screen (the Settings app's "Title boot screen", on unless turned
    // off): the attract title with a progress bar instead of "Data Loading".
    // The title is Documents/boot/title.png, else the one the build put in the
    // bundle (tools/bootscreen/capture_title.py, from the player's game on a Mac).
    {
        NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
        const bool on = [defaults objectForKey:@"boot_screen"] == nil || [defaults boolForKey:@"boot_screen"];
        std::string image = (docs / "boot" / "title.png").string();
        if (!std::filesystem::exists(image, ec)) {
            NSString* bundled = [NSBundle.mainBundle pathForResource:@"boot_title" ofType:@"png"];
            image = bundled != nil ? bundled.UTF8String : "";
        }
        if (on && !image.empty()) {
            setenv("DCUSA_BOOT_SCREEN", "1", 0);
            setenv("DCUSA_BOOT_SCREEN_IMAGE", image.c_str(), 0);
        }
    }
    std::string error;
    bool loaded = false;
    for (dcusa::Edition edition : {dcusa::Edition::Nse2019, dcusa::Edition::Dcu2017}) {
        std::string one;
        if (dcusa::exe_image_load_cache(cache.c_str(), edition, g.image, &one)) {
            loaded = true;
            break;
        }
        error += one + "\n";
    }
    if (!loaded) {
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
    const std::filesystem::path writable = docs / "container";
    std::filesystem::create_directories(writable, ec);
    dcusa::recomp::set_guest_files(install->install_root, install->game_dir.filename().string(), writable);
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
        g.process->start(dcusa::recomp::translated_entries());
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
