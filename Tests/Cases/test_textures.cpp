/**
 * @ Author: Amélie Heinrich
 * @ Create Time: 2026-11-13 00:00:00
 * @ Copyright: Copyright (c) 2026 Amélie Heinrich. All rights reserved.
 */

#include "test_helpers.h"

using namespace srhitest;

namespace
{
    struct SampleParams
    {
        float tint[4];
        float uvScaleOffset[4];
        float lod[4];
    };

    /// @brief Draws textured.slang's fullscreen triangle sampling `view` into a new target.
    srhiTexture* DrawTextured(GpuFixture& gpu, srhiTextureView* view, srhiSampler* sampler, uint32_t w, uint32_t h,
                              const SampleParams& params, srhiFormat targetFormat = SRHI_FORMAT_RGBA8_UNORM)
    {
        srhiShaderModule* module = gpu.LoadShader("textured.slang");
        if (!module)
            return nullptr;
        srhiResourceSetLayout* layout = gpu.LayoutFromShader(module, 0);
        srhiRenderPipelineDesc desc = {};
        desc.module = module;
        desc.vertexEntry = "vsFullscreen";
        desc.fragmentEntry = "fsSample";
        desc.colorTargetCount = 1;
        desc.colorTargets[0].format = targetFormat;
        desc.setLayouts[0] = layout;
        srhiRenderPipeline* pipeline = gpu.Own(srhiRenderPipelineCreate(gpu.Device(), &desc));
        if (!pipeline)
            return nullptr;

        srhiResourceBinding bindings[2] = {};
        bindings[0].binding = 0;
        bindings[0].textureView = view;
        bindings[1].binding = 1;
        bindings[1].sampler = sampler;
        srhiResourceSetDesc sd = {layout, bindings, 2, "textured set"};
        srhiResourceSet* set = gpu.Own(srhiResourceSetCreate(gpu.Device(), &sd));
        if (!set)
            return nullptr;

        srhiTexture* target = CreateTarget(gpu, w, h, targetFormat);
        gpu.Run([&](srhiCommandBuffer* cmd) {
            srhiRenderPassDesc pass = ClearPass(target, 0.0f, 0.0f, 0.0f, 1.0f);
            srhiRenderEncoder* enc = srhiRenderEncoderBegin(cmd, &pass);
            srhiRenderEncoderSetPipeline(enc, pipeline);
            srhiRenderEncoderSetResourceSet(enc, 0, set);
            srhiRenderEncoderSetUniforms(enc, 0, &params, sizeof(params));
            srhiRenderEncoderDraw(enc, 3, 1, 0, 0);
            srhiRenderEncoderEnd(enc);
        });
        return target;
    }

    srhiSampler* CreateSampler(GpuFixture& gpu, srhiFilter filter, srhiAddressMode address, srhiFilter mip = SRHI_FILTER_NEAREST)
    {
        srhiSamplerDesc sd = {};
        sd.minFilter = sd.magFilter = filter;
        sd.mipFilter = mip;
        sd.addressU = sd.addressV = sd.addressW = address;
        return gpu.Own(srhiSamplerCreate(gpu.Device(), &sd));
    }

    const SampleParams kPlain = {{1, 1, 1, 1}, {1, 1, 0, 0}, {0, 0, 0, 0}};
} // namespace

SRHI_TEST_TEXTURE(TextureWriteReadback, 64, 64)
{
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);
    std::vector<uint8_t> pixels = MakeCheckerRgba8(64);
    srhiTexture* tex = gpu.CreateTexture2D(64, 64, SRHI_FORMAT_RGBA8_UNORM, SRHI_TEXTURE_USAGE_SAMPLED | SRHI_TEXTURE_USAGE_COPY_SRC);
    srhiTextureWrite(gpu.Device(), tex, 0, 0, nullptr, pixels.data(), 0);
    std::vector<uint8_t> bytes = gpu.ReadTextureBytes(tex);
    SRHI_EXPECT(bytes == pixels);
    ExpectImageMatchesGolden(ctx, "TextureWriteReadback.png", gpu.ReadImage(tex));
}

SRHI_TEST_TEXTURE(TextureWriteRegion, 64, 64)
{
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);
    std::vector<uint8_t> pixels = MakeCheckerRgba8(64);
    srhiTexture* tex = gpu.CreateTexture2D(64, 64, SRHI_FORMAT_RGBA8_UNORM, SRHI_TEXTURE_USAGE_SAMPLED | SRHI_TEXTURE_USAGE_COPY_SRC);
    srhiTextureWrite(gpu.Device(), tex, 0, 0, nullptr, pixels.data(), 0);
    // Overwrite a 16x8 region at (40, 20) with solid red, from a padded source (row pitch 100 bytes).
    std::vector<uint8_t> red(100 * 8, 0);
    for (uint32_t y = 0; y < 8; ++y)
        for (uint32_t x = 0; x < 16; ++x) {
            red[y * 100 + x * 4 + 0] = 255;
            red[y * 100 + x * 4 + 3] = 255;
        }
    srhiTextureRegion region = {40, 20, 0, 16, 8, 1};
    srhiTextureWrite(gpu.Device(), tex, 0, 0, &region, red.data(), 100);
    ExpectImageMatchesGolden(ctx, "TextureWriteRegion.png", gpu.ReadImage(tex));
}

SRHI_TEST_TEXTURE(SampleTexture2D, 128, 128)
{
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);
    std::vector<uint8_t> pixels = MakeCheckerRgba8(32, 4);
    srhiTexture* tex = gpu.CreateTexture2D(32, 32, SRHI_FORMAT_RGBA8_UNORM, SRHI_TEXTURE_USAGE_SAMPLED);
    srhiTextureWrite(gpu.Device(), tex, 0, 0, nullptr, pixels.data(), 0);
    srhiSampler* sampler = CreateSampler(gpu, SRHI_FILTER_LINEAR, SRHI_ADDRESS_MODE_REPEAT);
    SampleParams params = {{1, 0.8f, 1, 1}, {2, 2, 0.1f, 0}, {0, 0, 0, 0}};
    srhiTexture* target = DrawTextured(gpu, srhiTextureGetDefaultView(tex), sampler, 128, 128, params);
    SRHI_EXPECT_NOT_NULL(target);
    ExpectImageMatchesGolden(ctx, "SampleTexture2D.png", gpu.ReadImage(target));
}

SRHI_TEST_TEXTURE(SamplerAddressModes, 128, 128)
{
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);
    std::vector<uint8_t> pixels = MakeCheckerRgba8(16, 4);
    srhiTexture* tex = gpu.CreateTexture2D(16, 16, SRHI_FORMAT_RGBA8_UNORM, SRHI_TEXTURE_USAGE_SAMPLED);
    srhiTextureWrite(gpu.Device(), tex, 0, 0, nullptr, pixels.data(), 0);
    SampleParams params = {{1, 1, 1, 1}, {3, 3, -1, -1}, {0, 0, 0, 0}};
    srhiSampler* mirror = CreateSampler(gpu, SRHI_FILTER_NEAREST, SRHI_ADDRESS_MODE_MIRROR_REPEAT);
    srhiSampler* clamp = CreateSampler(gpu, SRHI_FILTER_NEAREST, SRHI_ADDRESS_MODE_CLAMP_TO_EDGE);
    srhiTexture* a = DrawTextured(gpu, srhiTextureGetDefaultView(tex), mirror, 128, 128, params);
    srhiTexture* b = DrawTextured(gpu, srhiTextureGetDefaultView(tex), clamp, 128, 128, params);
    SRHI_EXPECT(a && b);
    // Left half mirror, right half clamp.
    Image ia = gpu.ReadImage(a), ib = gpu.ReadImage(b);
    for (uint32_t y = 0; y < 128; ++y)
        for (uint32_t x = 64; x < 128; ++x)
            for (int c = 0; c < 4; ++c)
                ia.pixels[(y * 128 + x) * 4 + c] = ib.pixels[(y * 128 + x) * 4 + c];
    ExpectImageMatchesGolden(ctx, "SamplerAddressModes.png", ia);
}

SRHI_TEST_TEXTURE(GenerateMipmaps, 64, 64)
{
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);
    std::vector<uint8_t> pixels = MakeCheckerRgba8(64, 1);
    srhiTexture* tex = gpu.CreateTexture2D(64, 64, SRHI_FORMAT_RGBA8_UNORM,
                                           SRHI_TEXTURE_USAGE_SAMPLED | SRHI_TEXTURE_USAGE_RENDER_TARGET | SRHI_TEXTURE_USAGE_COPY_SRC,
                                           SRHI_ALL_MIPS);
    srhiTextureWrite(gpu.Device(), tex, 0, 0, nullptr, pixels.data(), 0);
    gpu.Run([&](srhiCommandBuffer* cmd) {
        srhiBlitEncoder* blit = srhiBlitEncoderBegin(cmd, "mips");
        srhiBlitEncoderGenerateMipmaps(blit, tex);
        srhiBlitEncoderEnd(blit);
    });
    // Mip 2 (16x16) of a 1-texel checker averages to a smooth gradient with grey blue.
    Image mip2 = gpu.ReadImage(tex, 2);
    SRHI_EXPECT_EQ(mip2.width, 16u);
    SRHI_EXPECT(std::abs(mip2.pixels[2] - (255.0f + 32.0f) / 2.0f / 255.0f) < 0.05f);
    srhiSampler* sampler = CreateSampler(gpu, SRHI_FILTER_NEAREST, SRHI_ADDRESS_MODE_CLAMP_TO_EDGE, SRHI_FILTER_NEAREST);
    SampleParams params = kPlain;
    params.lod[0] = 2.0f;
    srhiTexture* target = DrawTextured(gpu, srhiTextureGetDefaultView(tex), sampler, 64, 64, params);
    SRHI_EXPECT_NOT_NULL(target);
    ExpectImageMatchesGolden(ctx, "GenerateMipmaps.png", gpu.ReadImage(target));
}

SRHI_TEST_TEXTURE(RenderToMipAndSample, 64, 64)
{
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);
    srhiTexture* tex = gpu.CreateTexture2D(64, 64, SRHI_FORMAT_RGBA8_UNORM, SRHI_TEXTURE_USAGE_SAMPLED | SRHI_TEXTURE_USAGE_RENDER_TARGET, 3);
    const float colors[3][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}};
    for (uint32_t mip = 0; mip < 3; ++mip) {
        srhiTextureViewDesc vd = {};
        vd.baseMip = mip;
        vd.mipCount = 1;
        srhiTextureView* view = gpu.Own(srhiTextureViewCreate(tex, &vd));
        SRHI_EXPECT_NOT_NULL(view);
        gpu.Run([&](srhiCommandBuffer* cmd) {
            srhiRenderPassDesc pass = {};
            pass.colorAttachmentCount = 1;
            pass.colorAttachments[0].view = view;
            pass.colorAttachments[0].loadAction = SRHI_LOAD_ACTION_CLEAR;
            memcpy(pass.colorAttachments[0].clearColor, colors[mip], 16);
            srhiRenderEncoderEnd(srhiRenderEncoderBegin(cmd, &pass));
        });
    }
    // Sample a view of mips 1..2 at LOD 1 (= mip 2, blue).
    srhiTextureViewDesc vd = {};
    vd.baseMip = 1;
    srhiTextureView* lower = gpu.Own(srhiTextureViewCreate(tex, &vd));
    srhiSampler* sampler = CreateSampler(gpu, SRHI_FILTER_NEAREST, SRHI_ADDRESS_MODE_CLAMP_TO_EDGE);
    SampleParams params = kPlain;
    params.lod[0] = 1.0f;
    srhiTexture* target = DrawTextured(gpu, lower, sampler, 64, 64, params);
    SRHI_EXPECT_NOT_NULL(target);
    Image image = gpu.ReadImage(target);
    SRHI_EXPECT(image.pixels[2] > 0.99f && image.pixels[0] < 0.01f);
    ExpectImageMatchesGolden(ctx, "RenderToMipAndSample.png", image);
}

SRHI_TEST_TEXTURE(SrgbView, 64, 64)
{
    // Writing linear 0.5 through an sRGB view stores ~188; reading it through the linear texture gives 188/255.
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);
    srhiTexture* tex = gpu.CreateTexture2D(64, 64, SRHI_FORMAT_RGBA8_UNORM, SRHI_TEXTURE_USAGE_RENDER_TARGET | SRHI_TEXTURE_USAGE_SAMPLED);
    srhiTextureViewDesc vd = {};
    vd.format = SRHI_FORMAT_RGBA8_UNORM_SRGB;
    srhiTextureView* srgb = gpu.Own(srhiTextureViewCreate(tex, &vd));
    SRHI_EXPECT_NOT_NULL(srgb);
    gpu.Run([&](srhiCommandBuffer* cmd) {
        srhiRenderPassDesc pass = {};
        pass.colorAttachmentCount = 1;
        pass.colorAttachments[0].view = srgb;
        pass.colorAttachments[0].loadAction = SRHI_LOAD_ACTION_CLEAR;
        pass.colorAttachments[0].clearColor[0] = 0.5f;
        pass.colorAttachments[0].clearColor[1] = 0.2f;
        pass.colorAttachments[0].clearColor[2] = 0.0f;
        pass.colorAttachments[0].clearColor[3] = 1.0f;
        srhiRenderEncoderEnd(srhiRenderEncoderBegin(cmd, &pass));
    });
    std::vector<uint8_t> bytes = gpu.ReadTextureBytes(tex);
    SRHI_EXPECT(std::abs(int(bytes[0]) - 188) <= 1 && std::abs(int(bytes[1]) - 124) <= 1);
    ExpectImageMatchesGolden(ctx, "SrgbView.png", gpu.ReadImage(tex));
}
