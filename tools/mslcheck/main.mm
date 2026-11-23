// Compiles every .metal file in a folder with the system Metal device, the way
// the backend will at run time (newLibraryWithSource), and looks up main0.
// Exit 0 when all compile, 1 when any fails, 77 (CTest skip) when the folder is
// missing or empty: prepared shaders exist only where the player's files are.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cstdio>

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc != 2) {
            std::fprintf(stderr, "usage: mslcheck <folder of .metal files>\n");
            return 2;
        }
        NSString* dir = [NSString stringWithUTF8String:argv[1]];
        NSArray<NSString*>* names = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:dir error:nil];
        NSArray<NSString*>* shaders = [[names filteredArrayUsingPredicate:
            [NSPredicate predicateWithFormat:@"self ENDSWITH '.metal'"]] sortedArrayUsingSelector:@selector(compare:)];
        if (shaders.count == 0) {
            std::fprintf(stderr, "no .metal files in %s\n", argv[1]);
            return 77;
        }
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil) {
            std::fprintf(stderr, "no Metal device\n");
            return 77;
        }
        MTLCompileOptions* options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion2_1;
        // Relaxed: fast arithmetic that still honours NaN and infinity, which the
        // D3D11 min / max rules in the translated shaders rely on (translate_shaders.py).
        options.mathMode = MTLMathModeRelaxed;
        unsigned failed = 0;
        for (NSString* name in shaders) {
            NSString* path = [dir stringByAppendingPathComponent:name];
            NSString* source = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
            NSError* error = nil;
            id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
            id<MTLFunction> fn = library ? [library newFunctionWithName:@"main0"] : nil;
            if (fn == nil) {
                ++failed;
                NSString* why = error ? error.localizedDescription : @"no main0";
                std::fprintf(stderr, "%s: %s\n", name.UTF8String, [why substringToIndex:MIN(why.length, 400u)].UTF8String);
            }
        }
        std::printf("%lu shaders, %u failed (%s)\n", static_cast<unsigned long>(shaders.count), failed,
                    device.name.UTF8String);
        return failed ? 1 : 0;
    }
}
