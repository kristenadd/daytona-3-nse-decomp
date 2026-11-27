#include "platform/launcher/launcher_metal.hpp"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <TargetConditionals.h>

#import <ImageIO/ImageIO.h>
#if TARGET_OS_IPHONE
#import <MobileCoreServices/MobileCoreServices.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace dcusa::launcher {
namespace {

// Coloured triangles, optionally textured (straight alpha, blended over).
NSString* const kShader = @R"(
#include <metal_stdlib>
using namespace metal;
struct V { float2 pos [[attribute(0)]]; float4 color [[attribute(1)]]; float2 uv [[attribute(2)]]; };
struct F { float4 pos [[position]]; float4 color; float2 uv; };
vertex F vs(V v [[stage_in]], constant float2& size [[buffer(1)]]) {
    F f;
    f.pos = float4(v.pos.x / size.x * 2.0 - 1.0, 1.0 - v.pos.y / size.y * 2.0, 0.0, 1.0);
    f.color = v.color;
    f.uv = v.uv;
    return f;
}
fragment float4 fs(F f [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
    return t.sample(s, f.uv) * f.color;
}
)";

}  // namespace

struct MetalLauncherRenderer::State {
    CAMetalLayer* __strong layer = nil;
    id<MTLDevice> __strong device = nil;
    id<MTLCommandQueue> __strong queue = nil;
    id<MTLRenderPipelineState> __strong pipeline = nil;
    id<MTLSamplerState> __strong sampler = nil;
    id<MTLTexture> __strong white = nil;  // for untextured triangles
    std::vector<id<MTLTexture>> textures;
    int generation = -1;
    const char* shot = std::getenv("DCUSA_LAUNCHER_SHOT");  // testing: the latest frame, as a PNG
    int frames = 0;
};

namespace {

// The drawable's pixels (BGRA) to a PNG.
void save_png(const char* path, const void* bgra, std::size_t width, std::size_t height) {
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(const_cast<void*>(bgra), width, height, 8, width * 4, cs,
                                             kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    CGImageRef image = CGBitmapContextCreateImage(ctx);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(path),
                                                           static_cast<CFIndex>(std::strlen(path)), false);
    if (CGImageDestinationRef dest = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr)) {
        CGImageDestinationAddImage(dest, image, nullptr);
        CGImageDestinationFinalize(dest);
        CFRelease(dest);
    }
    CFRelease(url);
    CGImageRelease(image);
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
}

}  // namespace

MetalLauncherRenderer::MetalLauncherRenderer(void* layer) : s_(std::make_unique<State>()) {
    State& s = *s_;
    s.layer = (__bridge CAMetalLayer*)layer;
    s.device = s.layer.device != nil ? s.layer.device : MTLCreateSystemDefaultDevice();
    s.layer.device = s.device;
    s.layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    if (s.shot != nullptr) s.layer.framebufferOnly = NO;
    s.queue = [s.device newCommandQueue];
    NSError* error = nil;
    id<MTLLibrary> lib = [s.device newLibraryWithSource:kShader options:nil error:&error];
    MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
    vd.attributes[0].format = MTLVertexFormatFloat2;
    vd.attributes[0].offset = offsetof(Vertex, x);
    vd.attributes[1].format = MTLVertexFormatFloat4;
    vd.attributes[1].offset = offsetof(Vertex, r);
    vd.attributes[2].format = MTLVertexFormatFloat2;
    vd.attributes[2].offset = offsetof(Vertex, u);
    for (NSUInteger i = 0; i < 3; ++i) vd.attributes[i].bufferIndex = 0;
    vd.layouts[0].stride = sizeof(Vertex);
    MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"vs"];
    pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
    pd.vertexDescriptor = vd;
    MTLRenderPipelineColorAttachmentDescriptor* ca = pd.colorAttachments[0];
    ca.pixelFormat = MTLPixelFormatBGRA8Unorm;
    ca.blendingEnabled = YES;
    ca.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    ca.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    ca.sourceAlphaBlendFactor = MTLBlendFactorOne;
    ca.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    s.pipeline = [s.device newRenderPipelineStateWithDescriptor:pd error:&error];
    if (s.pipeline == nil) NSLog(@"launcher: no pipeline: %@", error);
    MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
    sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    s.sampler = [s.device newSamplerStateWithDescriptor:sd];
    MTLTextureDescriptor* wd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:1
                                                                                 height:1
                                                                              mipmapped:NO];
    s.white = [s.device newTextureWithDescriptor:wd];
    const std::uint32_t px = 0xFFFFFFFFu;
    [s.white replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&px bytesPerRow:4];
}

MetalLauncherRenderer::~MetalLauncherRenderer() = default;

void MetalLauncherRenderer::drawable_size(int& width, int& height) {
    State& s = *s_;
    CGSize size = s.layer.bounds.size;
    const CGFloat scale = s.layer.contentsScale > 0 ? s.layer.contentsScale : 1;
    size.width *= scale;
    size.height *= scale;
    if (size.width >= 1 && size.height >= 1 &&
        (s.layer.drawableSize.width != size.width || s.layer.drawableSize.height != size.height))
        s.layer.drawableSize = size;
    width = static_cast<int>(s.layer.drawableSize.width);
    height = static_cast<int>(s.layer.drawableSize.height);
}

void MetalLauncherRenderer::draw(const Launcher& launcher, const DrawList& list) {
    State& s = *s_;
    @autoreleasepool {
        if (s.pipeline == nil) return;
        if (s.generation != launcher.texture_generation()) {
            s.generation = launcher.texture_generation();
            s.textures.clear();
            for (const Texture& t : launcher.textures()) {
                MTLTextureDescriptor* d =
                    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                       width:static_cast<NSUInteger>(t.width)
                                                                      height:static_cast<NSUInteger>(t.height)
                                                                   mipmapped:NO];
                id<MTLTexture> tex = [s.device newTextureWithDescriptor:d];
                [tex replaceRegion:MTLRegionMake2D(0, 0, static_cast<NSUInteger>(t.width), static_cast<NSUInteger>(t.height))
                       mipmapLevel:0
                         withBytes:t.rgba.data()
                       bytesPerRow:static_cast<NSUInteger>(t.width) * 4];
                s.textures.push_back(tex);
            }
        }
        id<CAMetalDrawable> drawable = [s.layer nextDrawable];
        if (drawable == nil) return;
        const float w = static_cast<float>(drawable.texture.width), h = static_cast<float>(drawable.texture.height);
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = drawable.texture;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0.016, 0.078, 0.196, 1);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLCommandBuffer> cb = [s.queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        if (!list.vertices.empty() && !list.indices.empty()) {
            id<MTLBuffer> vb = [s.device newBufferWithBytes:list.vertices.data()
                                                     length:list.vertices.size() * sizeof(Vertex)
                                                    options:MTLResourceStorageModeShared];
            id<MTLBuffer> ib = [s.device newBufferWithBytes:list.indices.data()
                                                     length:list.indices.size() * sizeof(std::uint32_t)
                                                    options:MTLResourceStorageModeShared];
            [enc setRenderPipelineState:s.pipeline];
            [enc setVertexBuffer:vb offset:0 atIndex:0];
            const float size[2] = {w, h};
            [enc setVertexBytes:size length:sizeof size atIndex:1];
            [enc setFragmentSamplerState:s.sampler atIndex:0];
            for (const Batch& b : list.batches) {
                if (b.count == 0) continue;
                id<MTLTexture> tex = b.texture >= 0 && b.texture < static_cast<int>(s.textures.size())
                                         ? s.textures[static_cast<std::size_t>(b.texture)]
                                         : s.white;
                [enc setFragmentTexture:tex atIndex:0];
                MTLScissorRect sc{0, 0, static_cast<NSUInteger>(w), static_cast<NSUInteger>(h)};
                if (b.clipped) {
                    const float x0 = std::clamp(b.clip_x, 0.0f, w), y0 = std::clamp(b.clip_y, 0.0f, h);
                    const float x1 = std::clamp(b.clip_x + b.clip_w, x0, w), y1 = std::clamp(b.clip_y + b.clip_h, y0, h);
                    sc = {static_cast<NSUInteger>(x0), static_cast<NSUInteger>(y0), static_cast<NSUInteger>(x1 - x0),
                          static_cast<NSUInteger>(y1 - y0)};
                }
                if (sc.width == 0 || sc.height == 0) continue;
                [enc setScissorRect:sc];
                [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                indexCount:b.count
                                 indexType:MTLIndexTypeUInt32
                               indexBuffer:ib
                         indexBufferOffset:b.first * sizeof(std::uint32_t)];
            }
        }
        [enc endEncoding];
        const bool shoot = s.shot != nullptr && ++s.frames % 20 == 0;
        id<MTLBuffer> pixels = nil;
        if (shoot) {
            pixels = [s.device newBufferWithLength:drawable.texture.width * drawable.texture.height * 4
                                           options:MTLResourceStorageModeShared];
            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            [blit copyFromTexture:drawable.texture
                             sourceSlice:0
                             sourceLevel:0
                            sourceOrigin:MTLOriginMake(0, 0, 0)
                              sourceSize:MTLSizeMake(drawable.texture.width, drawable.texture.height, 1)
                                toBuffer:pixels
                       destinationOffset:0
                  destinationBytesPerRow:drawable.texture.width * 4
                destinationBytesPerImage:drawable.texture.width * drawable.texture.height * 4];
            [blit endEncoding];
        }
        [cb presentDrawable:drawable];
        [cb commit];
        if (shoot) {
            [cb waitUntilCompleted];
            save_png(s.shot, pixels.contents, drawable.texture.width, drawable.texture.height);
        }
    }
}

}  // namespace dcusa::launcher
