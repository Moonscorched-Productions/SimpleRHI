/**
 * @ Author: Amélie Heinrich
 * @ Create Time: 2026-11-13 00:00:00
 * @ Copyright: Copyright (c) 2026 Amélie Heinrich. All rights reserved.
 */

// Texture dimensions, comparison sampling, block compression and copies.

#include "test_helpers.h"

#include <initializer_list>

using namespace srhitest;

namespace
{
    /// @brief A layout with only the given bindings of the module's set 0 (sample_types.slang declares many).
    srhiResourceSetLayout* LayoutSubset(GpuFixture& gpu, srhiShaderModule* module, std::initializer_list<uint32_t> bindings,
                                        srhiTextureSampleType depthSampleType = SRHI_TEXTURE_SAMPLE_TYPE_DEPTH)
    {
        uint32_t count = srhiShaderModuleGetResourceSetLayout(module, 0, nullptr, 0);
        std::vector<srhiResourceSetLayoutEntry> all(count), picked;
        srhiShaderModuleGetResourceSetLayout(module, 0, all.data(), count);
        for (const srhiResourceSetLayoutEntry& e : all)
            for (uint32_t b : bindings)
                if (e.binding == b) {
                    picked.push_back(e);
                    if (b == 4) // gDepth: comparison-sampled depth texture
                        picked.back().sampleType = depthSampleType;
                }
        srhiResourceSetLayoutDesc desc = {picked.data(), uint32_t(picked.size()), "subset"};
        return gpu.Own(srhiResourceSetLayoutCreate(gpu.Device(), &desc));
    }

    srhiTexture* DrawFullscreen(GpuFixture& gpu, const char* fs, srhiResourceSetLayout* layout,
                                const std::vector<srhiResourceBinding>& bindings, uint32_t w, uint32_t h)
    {
        srhiRenderPipelineDesc desc = {};
        desc.module = gpu.LoadShader("sample_types.slang");
        desc.vertexEntry = "vsFullscreen";
        desc.fragmentEntry = fs;
        desc.colorTargetCount = 1;
        desc.colorTargets[0].format = SRHI_FORMAT_RGBA8_UNORM;
        desc.setLayouts[0] = layout;
        srhiRenderPipeline* pipeline = gpu.Own(srhiRenderPipelineCreate(gpu.Device(), &desc));
        srhiResourceSetDesc sd = {layout, bindings.data(), uint32_t(bindings.size()), fs};
        srhiResourceSet* set = gpu.Own(srhiResourceSetCreate(gpu.Device(), &sd));
        if (!pipeline || !set)
            return nullptr;
        srhiTexture* target = CreateTarget(gpu, w, h);
        gpu.Run([&](srhiCommandBuffer* cmd) {
            srhiRenderPassDesc pass = ClearPass(target, 0, 0, 0, 1);
            srhiRenderEncoder* enc = srhiRenderEncoderBegin(cmd, &pass);
            srhiRenderEncoderSetPipeline(enc, pipeline);
            srhiRenderEncoderSetResourceSet(enc, 0, set);
            srhiRenderEncoderDraw(enc, 3, 1, 0, 0);
            srhiRenderEncoderEnd(enc);
        });
        return target;
    }

    srhiSampler* PointSampler(GpuFixture& gpu)
    {
        srhiSamplerDesc sd = {};
        sd.addressU = sd.addressV = sd.addressW = SRHI_ADDRESS_MODE_CLAMP_TO_EDGE;
        return gpu.Own(srhiSamplerCreate(gpu.Device(), &sd));
    }

    /// @brief size x size RGBA8 tile: base color with a diagonal stripe and a corner marker.
    std::vector<uint8_t> Tile(uint32_t size, uint8_t r, uint8_t g, uint8_t b)
    {
        std::vector<uint8_t> p(size_t(size) * size * 4);
        for (uint32_t y = 0; y < size; ++y)
            for (uint32_t x = 0; x < size; ++x) {
                uint8_t* t = &p[(size_t(y) * size + x) * 4];
                bool stripe = (x + y) % 8 < 2;
                bool corner = x < size / 4 && y < size / 4;
                t[0] = corner ? 255 : stripe ? r / 2 : r;
                t[1] = corner ? 255 : stripe ? g / 2 : g;
                t[2] = corner ? 255 : stripe ? b / 2 : b;
                t[3] = 255;
            }
        return p;
    }

    const uint8_t kColors[6][3] = {{230, 40, 40}, {40, 200, 60}, {50, 80, 230}, {230, 200, 40}, {200, 60, 200}, {40, 200, 200}};
} // namespace

SRHI_TEST_TEXTURE(SampleTexture2DArray, 128, 32)
{
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);
    srhiTextureDesc td = {};
    td.type = SRHI_TEXTURE_TYPE_2D_ARRAY;
    td.format = SRHI_FORMAT_RGBA8_UNORM;
    td.width = td.height = 32;
    td.depthOrLayers = 4;
    td.usage = SRHI_TEXTURE_USAGE_SAMPLED;
    srhiTexture* tex = gpu.Own(srhiTextureCreate(gpu.Device(), &td));
    SRHI_EXPECT_NOT_NULL(tex);
    for (uint32_t layer = 0; layer < 4; ++layer) {
        std::vector<uint8_t> tile = Tile(32, kColors[layer][0], kColors[layer][1], kColors[layer][2]);
        srhiTextureWrite(gpu.Device(), tex, 0, layer, nullptr, tile.data(), 0);
    }
    srhiShaderModule* module = gpu.LoadShader("sample_types.slang");
    srhiResourceSetLayout* layout = LayoutSubset(gpu, module, {0, 3});
    srhiTexture* target = DrawFullscreen(gpu, "fsArray", layout, {{0, nullptr, 0, 0, srhiTextureGetDefaultView(tex)}, {3, nullptr, 0, 0, nullptr, PointSampler(gpu)}}, 128, 32);
    SRHI_EXPECT_NOT_NULL(target);
    ExpectImageMatchesGolden(ctx, "SampleTexture2DArray.png", gpu.ReadImage(target));
}

SRHI_TEST_TEXTURE(SampleTextureCube, 96, 64)
{
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);
    srhiTextureDesc td = {};
    td.type = SRHI_TEXTURE_TYPE_CUBE;
    td.format = SRHI_FORMAT_RGBA8_UNORM;
    td.width = td.height = 32;
    td.usage = SRHI_TEXTURE_USAGE_SAMPLED;
    srhiTexture* tex = gpu.Own(srhiTextureCreate(gpu.Device(), &td));
    SRHI_EXPECT_NOT_NULL(tex);
    for (uint32_t face = 0; face < 6; ++face) {
        std::vector<uint8_t> tile = Tile(32, kColors[face][0], kColors[face][1], kColors[face][2]);
        srhiTextureWrite(gpu.Device(), tex, 0, face, nullptr, tile.data(), 0);
    }
    srhiShaderModule* module = gpu.LoadShader("sample_types.slang");
    srhiResourceSetLayout* layout = LayoutSubset(gpu, module, {1, 3});
    srhiTexture* target = DrawFullscreen(gpu, "fsCube", layout, {{1, nullptr, 0, 0, srhiTextureGetDefaultView(tex)}, {3, nullptr, 0, 0, nullptr, PointSampler(gpu)}}, 96, 64);
    SRHI_EXPECT_NOT_NULL(target);
    ExpectImageMatchesGolden(ctx, "SampleTextureCube.png", gpu.ReadImage(target));
}

SRHI_TEST_TEXTURE(SampleTexture3D, 128, 32)
{
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);
    srhiTextureDesc td = {};
    td.type = SRHI_TEXTURE_TYPE_3D;
    td.format = SRHI_FORMAT_RGBA8_UNORM;
    td.width = td.height = 32;
    td.depthOrLayers = 4;
    td.usage = SRHI_TEXTURE_USAGE_SAMPLED;
    srhiTexture* tex = gpu.Own(srhiTextureCreate(gpu.Device(), &td));
    SRHI_EXPECT_NOT_NULL(tex);
    std::vector<uint8_t> volume;
    for (uint32_t z = 0; z < 4; ++z) {
        std::vector<uint8_t> tile = Tile(32, kColors[z + 2][0], kColors[z + 2][1], kColors[z + 2][2]);
        volume.insert(volume.end(), tile.begin(), tile.end());
    }
    srhiTextureWrite(gpu.Device(), tex, 0, 0, nullptr, volume.data(), 0);
    srhiShaderModule* module = gpu.LoadShader("sample_types.slang");
    srhiResourceSetLayout* layout = LayoutSubset(gpu, module, {2, 3});
    srhiTexture* target = DrawFullscreen(gpu, "fsVolume", layout, {{2, nullptr, 0, 0, srhiTextureGetDefaultView(tex)}, {3, nullptr, 0, 0, nullptr, PointSampler(gpu)}}, 128, 32);
    SRHI_EXPECT_NOT_NULL(target);
    ExpectImageMatchesGolden(ctx, "SampleTexture3D.png", gpu.ReadImage(target));
}

SRHI_TEST_TEXTURE(SampleDepthCompare, 64, 64)
{
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);
    srhiTexture* depth = gpu.CreateTexture2D(64, 64, SRHI_FORMAT_D32_FLOAT, SRHI_TEXTURE_USAGE_RENDER_TARGET | SRHI_TEXTURE_USAGE_SAMPLED);
    // Clear depth to 0.5: the reference ramps 0..1 left to right, so LESS passes on the left half.
    gpu.Run([&](srhiCommandBuffer* cmd) {
        srhiRenderPassDesc pass = {};
        pass.depthAttachment.view = srhiTextureGetDefaultView(depth);
        pass.depthAttachment.depthLoadAction = SRHI_LOAD_ACTION_CLEAR;
        pass.depthAttachment.depthStoreAction = SRHI_STORE_ACTION_STORE;
        pass.depthAttachment.clearDepth = 0.5f;
        srhiRenderEncoderEnd(srhiRenderEncoderBegin(cmd, &pass));
    });
    srhiSamplerDesc sd = {};
    sd.compareEnable = 1;
    sd.compareFunction = SRHI_COMPARE_FUNCTION_LESS;
    sd.addressU = sd.addressV = SRHI_ADDRESS_MODE_CLAMP_TO_EDGE;
    srhiSampler* compare = gpu.Own(srhiSamplerCreate(gpu.Device(), &sd));
    srhiShaderModule* module = gpu.LoadShader("sample_types.slang");
    srhiResourceSetLayout* layout = LayoutSubset(gpu, module, {4, 5});
    srhiTexture* target = DrawFullscreen(gpu, "fsCompare", layout, {{4, nullptr, 0, 0, srhiTextureGetDefaultView(depth)}, {5, nullptr, 0, 0, nullptr, compare}}, 64, 64);
    SRHI_EXPECT_NOT_NULL(target);
    Image image = gpu.ReadImage(target);
    SRHI_EXPECT(image.pixels[(32 * 64 + 8) * 4] > 0.99f && image.pixels[(32 * 64 + 56) * 4] < 0.01f);
    ExpectImageMatchesGolden(ctx, "SampleDepthCompare.png", image);
}

SRHI_TEST_TEXTURE(SampleBC1, 64, 64)
{
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);
    SRHI_REQUIRE_FEATURE(gpu, textureCompressionBC);
    // 16x16 BC1: 4x4 blocks. Each block interpolates between two 565 colors; indices form a gradient
    // along x (0, 2, 3, 1 = c0, 2/3 c0 + 1/3 c1, 1/3 c0 + 2/3 c1, c1).
    auto rgb565 = [](uint32_t r, uint32_t g, uint32_t b) { return uint16_t(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); };
    std::vector<uint8_t> blocks;
    for (uint32_t by = 0; by < 4; ++by)
        for (uint32_t bx = 0; bx < 4; ++bx) {
            uint16_t c0 = rgb565(255 - bx * 60, 40 + by * 60, 200), c1 = rgb565(20, 20 + bx * 50, 40 + by * 40);
            if (c0 <= c1)
                std::swap(c0, c1);
            uint32_t indices = 0;
            const uint32_t ramp[4] = {0, 2, 3, 1};
            for (uint32_t t = 0; t < 16; ++t)
                indices |= ramp[t % 4] << (t * 2);
            uint8_t block[8] = {uint8_t(c0), uint8_t(c0 >> 8), uint8_t(c1), uint8_t(c1 >> 8),
                                uint8_t(indices), uint8_t(indices >> 8), uint8_t(indices >> 16), uint8_t(indices >> 24)};
            blocks.insert(blocks.end(), block, block + 8);
        }
    srhiTexture* tex = gpu.CreateTexture2D(16, 16, SRHI_FORMAT_BC1_UNORM, SRHI_TEXTURE_USAGE_SAMPLED);
    SRHI_EXPECT_NOT_NULL(tex);
    srhiTextureWrite(gpu.Device(), tex, 0, 0, nullptr, blocks.data(), 0);

    srhiShaderModule* module = gpu.LoadShader("textured.slang");
    srhiResourceSetLayout* layout = gpu.LayoutFromShader(module, 0);
    srhiRenderPipelineDesc desc = {};
    desc.module = module;
    desc.vertexEntry = "vsFullscreen";
    desc.fragmentEntry = "fsSample";
    desc.colorTargetCount = 1;
    desc.colorTargets[0].format = SRHI_FORMAT_RGBA8_UNORM;
    desc.setLayouts[0] = layout;
    srhiRenderPipeline* pipeline = gpu.Own(srhiRenderPipelineCreate(gpu.Device(), &desc));
    srhiResourceBinding bindings[2] = {{0, nullptr, 0, 0, srhiTextureGetDefaultView(tex)}, {1, nullptr, 0, 0, nullptr, PointSampler(gpu)}};
    srhiResourceSetDesc sd = {layout, bindings, 2, nullptr};
    srhiResourceSet* set = gpu.Own(srhiResourceSetCreate(gpu.Device(), &sd));
    srhiTexture* target = CreateTarget(gpu, 64, 64);
    gpu.Run([&](srhiCommandBuffer* cmd) {
        srhiRenderPassDesc pass = ClearPass(target, 0, 0, 0, 1);
        srhiRenderEncoder* enc = srhiRenderEncoderBegin(cmd, &pass);
        srhiRenderEncoderSetPipeline(enc, pipeline);
        srhiRenderEncoderSetResourceSet(enc, 0, set);
        float params[12] = {1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0};
        srhiRenderEncoderSetUniforms(enc, 0, params, sizeof(params));
        srhiRenderEncoderDraw(enc, 3, 1, 0, 0);
        srhiRenderEncoderEnd(enc);
    });
    ExpectImageMatchesGolden(ctx, "SampleBC1.png", gpu.ReadImage(target));
}

SRHI_TEST_BUFFER(CopyTextureToBuffer)
{
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);
    std::vector<uint8_t> pixels = MakeCheckerRgba8(64);
    srhiTexture* tex = gpu.CreateTexture2D(64, 64, SRHI_FORMAT_RGBA8_UNORM, SRHI_TEXTURE_USAGE_SAMPLED);
    srhiTextureWrite(gpu.Device(), tex, 0, 0, nullptr, pixels.data(), 0);
    std::vector<uint8_t> fill(256 * 10, 0xCD);
    srhiBuffer* buffer = gpu.CreateBuffer(fill.size(), SRHI_BUFFER_USAGE_COPY_DST, fill.data());

    gpu.Run([&](srhiCommandBuffer* cmd) {
        srhiBlitEncoder* blit = srhiBlitEncoderBegin(cmd, "copy");
        srhiTextureCopyLocation src = {tex, 0, 0, 8, 4, 0};
        srhiBufferCopyLocation dst = {buffer, 256, 256, 0};
        srhiBlitEncoderCopyTextureToBuffer(blit, &src, &dst, 16, 8, 1);
        srhiBlitEncoderEnd(blit);
    });
    std::vector<uint8_t> bytes = gpu.ReadBuffer(buffer);
    for (uint32_t row = 0; row < 8; ++row)
        SRHI_EXPECT(memcmp(&bytes[256 + row * 256], &pixels[((4 + row) * 64 + 8) * 4], 64) == 0);
    SRHI_EXPECT_EQ(bytes[255], uint8_t(0xCD)); // untouched before the offset
    SRHI_EXPECT_EQ(bytes[256 + 64], uint8_t(0xCD)); // untouched row padding
    ExpectBufferMatchesGolden(ctx, "CopyTextureToBuffer.bin", bytes);
}

SRHI_TEST_TEXTURE(CopyBufferToTextureMip, 32, 32)
{
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);
    srhiTexture* tex = gpu.CreateTexture2D(64, 64, SRHI_FORMAT_RGBA8_UNORM, SRHI_TEXTURE_USAGE_SAMPLED, 2);
    std::vector<uint8_t> mip1 = MakeCheckerRgba8(32, 4);
    std::vector<uint8_t> padded(256 * 32, 0);
    for (uint32_t row = 0; row < 32; ++row)
        memcpy(&padded[row * 256], &mip1[row * 128], 128);
    srhiBuffer* buffer = gpu.CreateBuffer(padded.size(), SRHI_BUFFER_USAGE_COPY_SRC, padded.data());
    gpu.Run([&](srhiCommandBuffer* cmd) {
        srhiBlitEncoder* blit = srhiBlitEncoderBegin(cmd, "copy");
        srhiBufferCopyLocation src = {buffer, 0, 256, 0};
        srhiTextureCopyLocation dst = {tex, 1, 0, 0, 0, 0};
        srhiBlitEncoderCopyBufferToTexture(blit, &src, &dst, 32, 32, 1);
        srhiBlitEncoderEnd(blit);
    });
    std::vector<uint8_t> bytes = gpu.ReadTextureBytes(tex, 1);
    SRHI_EXPECT(bytes == mip1);
    ExpectImageMatchesGolden(ctx, "CopyBufferToTextureMip.png", gpu.ReadImage(tex, 1));
}

SRHI_TEST_TEXTURE(CopyTextureToTextureLayer, 32, 32)
{
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);
    std::vector<uint8_t> src = Tile(32, 230, 120, 40);
    srhiTexture* a = gpu.CreateTexture2D(32, 32, SRHI_FORMAT_RGBA8_UNORM, SRHI_TEXTURE_USAGE_SAMPLED);
    srhiTextureWrite(gpu.Device(), a, 0, 0, nullptr, src.data(), 0);
    srhiTextureDesc td = {};
    td.type = SRHI_TEXTURE_TYPE_2D_ARRAY;
    td.format = SRHI_FORMAT_RGBA8_UNORM;
    td.width = td.height = 32;
    td.depthOrLayers = 3;
    td.usage = SRHI_TEXTURE_USAGE_SAMPLED;
    srhiTexture* b = gpu.Own(srhiTextureCreate(gpu.Device(), &td));
    std::vector<uint8_t> black(32 * 32 * 4, 0);
    for (uint32_t l = 0; l < 3; ++l)
        srhiTextureWrite(gpu.Device(), b, 0, l, nullptr, black.data(), 0);
    gpu.Run([&](srhiCommandBuffer* cmd) {
        srhiBlitEncoder* blit = srhiBlitEncoderBegin(cmd, "copy");
        srhiTextureCopyLocation s = {a, 0, 0, 0, 0, 0};
        srhiTextureCopyLocation d = {b, 0, 2, 8, 8, 0};
        srhiBlitEncoderCopyTextureToTexture(blit, &s, &d, 16, 16, 1);
        srhiBlitEncoderEnd(blit);
    });
    std::vector<uint8_t> layer1 = gpu.ReadTextureBytes(b, 0, 1);
    SRHI_EXPECT(layer1 == black);
    ExpectImageMatchesGolden(ctx, "CopyTextureToTextureLayer.png", gpu.ReadImage(b, 0, 2));
}
