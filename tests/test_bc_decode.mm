// The CPU BC decoder (gpu/bc_decode) against the GPU's own: random blocks of
// each format go into a Metal BC texture, a compute kernel reads every pixel
// back, and the two must agree. BC7 is tested mode by mode (mode 7 is one
// random block in 256 otherwise). Skips (77) without a Metal device or BC support.
#import <Metal/Metal.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "check.hpp"
#include "gpu/bc_decode.hpp"

namespace {

const char* kKernel = R"(#include <metal_stdlib>
using namespace metal;
kernel void read_all(texture2d<float, access::read> tex [[texture(0)]], device float4* out [[buffer(0)]],
                     uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= tex.get_width() || gid.y >= tex.get_height()) return;
    out[gid.y * tex.get_width() + gid.x] = tex.read(gid);
}
)";

struct Format {
    std::uint32_t dxgi;
    MTLPixelFormat metal;
    const char* name;
    int bc7_mode;  // -1: random blocks
};

}  // namespace

int main() {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (dev == nil || !dev.supportsBCTextureCompression) {
            std::fprintf(stderr, "no Metal device with BC support; skipped\n");
            return SkipTest;
        }
        NSError* error = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:@(kKernel) options:nil error:&error];
        REQUIRE(lib != nil);
        id<MTLComputePipelineState> pipe = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"read_all"]
                                                                              error:&error];
        REQUIRE(pipe != nil);
        id<MTLCommandQueue> queue = [dev newCommandQueue];

        std::vector<Format> formats = {
            {71, MTLPixelFormatBC1_RGBA, "BC1", -1}, {74, MTLPixelFormatBC2_RGBA, "BC2", -1},
            {77, MTLPixelFormatBC3_RGBA, "BC3", -1}, {80, MTLPixelFormatBC4_RUnorm, "BC4", -1},
            {83, MTLPixelFormatBC5_RGUnorm, "BC5", -1}, {98, MTLPixelFormatBC7_RGBAUnorm, "BC7", -1},
        };
        for (int mode = 0; mode < 8; ++mode) formats.push_back({98, MTLPixelFormatBC7_RGBAUnorm, "BC7 mode", mode});
        // BC6H, unsigned and signed: random blocks, then each of the 14 modes.
        formats.push_back({95, MTLPixelFormatBC6H_RGBUfloat, "BC6H U", -1});
        formats.push_back({96, MTLPixelFormatBC6H_RGBFloat, "BC6H S", -1});
        for (int mode = 0; mode < 14; ++mode) {
            formats.push_back({95, MTLPixelFormatBC6H_RGBUfloat, "BC6H U mode", mode});
            formats.push_back({96, MTLPixelFormatBC6H_RGBFloat, "BC6H S mode", mode});
        }

        std::mt19937 rng(1234);
        constexpr std::uint32_t kBw = 64, kBh = 64, kW = kBw * 4, kH = kBh * 4;
        for (const Format& f : formats) {
            const std::uint32_t bb = f.dxgi == 71 || f.dxgi == 80 ? 8 : 16;
            const std::uint32_t bpp = dcusa::gpu::bc_decoded_bytes(f.dxgi);
            std::vector<std::uint8_t> blocks(static_cast<std::size_t>(kBw) * kBh * bb);
            for (auto& b : blocks) b = static_cast<std::uint8_t>(rng());
            const bool bc6 = f.dxgi == 95 || f.dxgi == 96;
            // The mode bits of BC6H modes 1-14 (two bits for 1 and 2, five for the rest).
            static constexpr std::uint8_t kBc6Mode[14] = {0x00, 0x01, 0x02, 0x06, 0x0A, 0x0E, 0x12,
                                                          0x16, 0x1A, 0x1E, 0x03, 0x07, 0x0B, 0x0F};
            if (bc6 && f.bc7_mode >= 0)
                for (std::size_t i = 0; i < blocks.size(); i += 16) {
                    const int n = f.bc7_mode < 2 ? 2 : 5;
                    blocks[i] = static_cast<std::uint8_t>((blocks[i] & ~((1u << n) - 1u)) | kBc6Mode[f.bc7_mode]);
                }
            else if (f.bc7_mode >= 0)
                for (std::size_t i = 0; i < blocks.size(); i += 16) {
                    const std::uint8_t low = static_cast<std::uint8_t>(1u << f.bc7_mode);
                    blocks[i] = static_cast<std::uint8_t>((blocks[i] & ~(2u * low - 1u)) | low);
                }
            MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f.metal width:kW
                                                                                         height:kH mipmapped:NO];
            td.usage = MTLTextureUsageShaderRead;
            td.storageMode = MTLStorageModeShared;
            id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
            REQUIRE(tex != nil);
            [tex replaceRegion:MTLRegionMake2D(0, 0, kW, kH) mipmapLevel:0 withBytes:blocks.data() bytesPerRow:kBw * bb];
            id<MTLBuffer> out = [dev newBufferWithLength:sizeof(float) * 4 * kW * kH options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> cmd = [queue commandBuffer];
            id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
            [enc setComputePipelineState:pipe];
            [enc setTexture:tex atIndex:0];
            [enc setBuffer:out offset:0 atIndex:0];
            [enc dispatchThreads:MTLSizeMake(kW, kH, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
            [enc endEncoding];
            [cmd commit];
            [cmd waitUntilCompleted];

            std::vector<std::uint8_t> cpu(static_cast<std::size_t>(kW) * kH * bpp);
            CHECK(dcusa::gpu::bc_decode(f.dxgi, blocks.data(), blocks.size(), kBw * bb, kW, kH, cpu.data(), kW * bpp));
            const float* gpu = static_cast<const float*>(out.contents);
            // The GPU interpolates without rounding to 8 bits; the decoder must
            // give the nearest 8-bit value.
            double worst = 0;
            std::size_t off = 0, first = ~std::size_t{0};
            for (std::uint32_t p = 0; p < kW * kH; ++p)
                for (std::uint32_t c = 0; c < (bc6 ? 4u : bpp); ++c) {
                    double d;
                    if (bc6) {  // half floats: exact
                        __fp16 h;
                        std::memcpy(&h, &cpu[p * bpp + 2 * c], 2);
                        const float want = h, got = gpu[4 * p + c];
                        d = want == got || (std::isnan(want) && std::isnan(got)) ? 0.0 : 1.0;
                    } else {
                        d = std::fabs(static_cast<double>(gpu[4 * p + c]) * 255.0 - cpu[p * bpp + c]);
                    }
                    if (d > 0.5 + 1e-3) {
                        ++off;
                        if (first == ~std::size_t{0}) first = p;
                    }
                    worst = std::max(worst, d);
                }
            std::printf("%-9s", f.name);
            if (f.bc7_mode >= 0) std::printf(" %d", f.bc7_mode);
            std::printf(": %zu of %u values differ, worst by %.3f\n", off, kW * kH * bpp, worst);
            if (first != ~std::size_t{0}) {
                const std::uint32_t x = static_cast<std::uint32_t>(first % kW), y = static_cast<std::uint32_t>(first / kW);
                const std::uint8_t* blk = &blocks[(static_cast<std::size_t>(y / 4) * kBw + x / 4) * bb];
                std::printf("  first at (%u,%u) block", x, y);
                for (std::uint32_t i = 0; i < bb; ++i) std::printf(" %02x", blk[i]);
                std::printf("\n  gpu");
                for (std::uint32_t c = 0; c < (bc6 ? 4u : bpp); ++c)
                    std::printf(" %g", bc6 ? gpu[4 * first + c] : gpu[4 * first + c] * 255.0f);
                std::printf("  cpu");
                for (std::uint32_t c = 0; c < (bc6 ? 4u : bpp); ++c) {
                    if (!bc6) {
                        std::printf(" %d", cpu[first * bpp + c]);
                        continue;
                    }
                    __fp16 h;
                    std::memcpy(&h, &cpu[first * bpp + 2 * c], 2);
                    std::printf(" %g", static_cast<float>(h));
                }
                std::printf("\n");
            }
            CHECK(off == 0);
        }
    }
    return check_result();
}
