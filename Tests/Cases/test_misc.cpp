/**
 * @ Author: Amélie Heinrich
 * @ Create Time: 2026-11-13 00:00:00
 * @ Copyright: Copyright (c) 2026 Amélie Heinrich. All rights reserved.
 */

#include "test_helpers.h"

using namespace srhitest;

SRHI_TEST_VALIDATION(DeviceInfo)
{
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);
    const srhiDeviceInfo& info = gpu.Info();
    SRHI_EXPECT(info.adapterName && info.adapterName[0]);
    SRHI_EXPECT(info.backendName && info.backendName[0]);
    SRHI_EXPECT(info.limits.maxTextureDimension2D >= 8192);
    SRHI_EXPECT(info.limits.uniformBufferOffsetAlignment > 0);
    SRHI_EXPECT(srhiDeviceGetFormatSupport(gpu.Device(), SRHI_FORMAT_RGBA8_UNORM) & SRHI_FORMAT_SUPPORT_RENDER_TARGET);
    SRHI_EXPECT(srhiDeviceGetFormatSupport(gpu.Device(), SRHI_FORMAT_D32_FLOAT) & SRHI_FORMAT_SUPPORT_DEPTH_STENCIL);
}

SRHI_TEST_VALIDATION(TimestampQueries)
{
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);
    SRHI_REQUIRE_FEATURE(gpu, timestampQueries);

    srhiQuerySet* qs = gpu.Own(srhiQuerySetCreate(gpu.Device(), 4, "timestamps"));
    SRHI_EXPECT_NOT_NULL(qs);
    srhiTexture* target = CreateTarget(gpu, 256, 256);
    uint64_t ns[4] = {};
    SRHI_EXPECT(!srhiQuerySetGetResults(qs, 0, 4, ns));

    gpu.Run([&](srhiCommandBuffer* cmd) {
        srhiRenderPassDesc pass = ClearPass(target, 1, 0, 0, 1);
        pass.timestampWrites = {qs, 0, 1};
        srhiRenderEncoderEnd(srhiRenderEncoderBegin(cmd, &pass));
        srhiComputePassDesc cp = {};
        cp.timestampWrites = {qs, 2, 3};
        srhiComputeEncoderEnd(srhiComputeEncoderBegin(cmd, &cp));
    });
    SRHI_EXPECT(srhiQuerySetGetResults(qs, 0, 4, ns));
    SRHI_EXPECT_MSG(ns[1] >= ns[0], "render pass end before begin");
    SRHI_EXPECT_MSG(ns[3] >= ns[2], "compute pass end before begin");
    SRHI_EXPECT_MSG(ns[1] - ns[0] < 1000000000ull, "render pass took over a second");
}

SRHI_TEST_VALIDATION(CompletionCallbackAndPoll)
{
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);
    int fired = 0;
    srhiCommandBuffer* cmd = srhiCommandBufferBegin(gpu.Device(), "callback");
    srhiCommandBufferCommit(cmd, [](void* user) { ++*static_cast<int*>(user); }, &fired);
    for (int i = 0; i < 1000 && !fired; ++i)
        srhiDevicePoll(gpu.Device());
    srhiDeviceWaitIdle(gpu.Device());
    SRHI_EXPECT_EQ(fired, 1);
}

SRHI_TEST_VALIDATION(RejectsMismatchedLayout)
{
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);
    srhiShaderModule* module = gpu.LoadShader("textured.slang");
    SRHI_EXPECT_NOT_NULL(module);

    // Layout declares binding 0 as a sampler: the shader's texture at binding 0 does not match.
    srhiResourceSetLayoutEntry entries[2] = {};
    entries[0].binding = 0;
    entries[0].type = SRHI_BINDING_TYPE_SAMPLER;
    entries[1].binding = 1;
    entries[1].type = SRHI_BINDING_TYPE_SAMPLER;
    srhiResourceSetLayoutDesc ld = {entries, 2, "bad layout"};
    srhiResourceSetLayout* layout = gpu.Own(srhiResourceSetLayoutCreate(gpu.Device(), &ld));
    srhiRenderPipelineDesc desc = {};
    desc.module = module;
    desc.vertexEntry = "vsFullscreen";
    desc.fragmentEntry = "fsSample";
    desc.colorTargetCount = 1;
    desc.colorTargets[0].format = SRHI_FORMAT_RGBA8_UNORM;
    desc.setLayouts[0] = layout;
    SRHI_EXPECT(srhiRenderPipelineCreate(gpu.Device(), &desc) == nullptr);
    gpu.ExpectErrors(1);

    // Missing layout entirely.
    desc.setLayouts[0] = nullptr;
    SRHI_EXPECT(srhiRenderPipelineCreate(gpu.Device(), &desc) == nullptr);
    gpu.ExpectErrors(1);
}

SRHI_TEST_VALIDATION(RejectsEncoderMisuse)
{
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);
    srhiTexture* target = CreateTarget(gpu, 16, 16);
    srhiCommandBuffer* cmd = srhiCommandBufferBegin(gpu.Device(), "misuse");
    srhiRenderPassDesc pass = ClearPass(target, 0, 0, 0, 1);
    srhiRenderEncoder* enc = srhiRenderEncoderBegin(cmd, &pass);
    SRHI_EXPECT(srhiComputeEncoderBegin(cmd, nullptr) == nullptr); // encoder already open
    srhiRenderEncoderDraw(enc, 3, 1, 0, 0);                        // no pipeline
    srhiRenderEncoderEnd(enc);
    srhiCommandBufferCommit(cmd, nullptr, nullptr);
    srhiDeviceWaitIdle(gpu.Device());
    gpu.ExpectErrors(2);
}

SRHI_TEST_VALIDATION(UnusedSetNeedNotBeBound)
{
    // A pipeline may declare a set layout its shaders never touch; drawing without binding it is valid.
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);
    srhiResourceSetLayoutEntry entry = {};
    entry.binding = 0;
    entry.type = SRHI_BINDING_TYPE_SAMPLER;
    srhiResourceSetLayoutDesc ld = {&entry, 1, "unused"};
    srhiResourceSetLayout* layout = gpu.Own(srhiResourceSetLayoutCreate(gpu.Device(), &ld));
    srhiRenderPipelineDesc desc = {};
    desc.setLayouts[1] = layout;
    srhiRenderPipeline* pipeline = CreateColorPipeline(gpu, SRHI_FORMAT_RGBA8_UNORM, SRHI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                                                       SRHI_FORMAT_UNDEFINED, nullptr);
    (void)pipeline;
    srhiShaderModule* module = gpu.LoadShader("triangle.slang");
    desc.module = module;
    desc.vertexEntry = "vsMain";
    desc.fragmentEntry = "fsMain";
    desc.vertexLayout = ColorVertexLayout();
    desc.colorTargetCount = 1;
    desc.colorTargets[0].format = SRHI_FORMAT_RGBA8_UNORM;
    srhiRenderPipeline* withUnused = gpu.Own(srhiRenderPipelineCreate(gpu.Device(), &desc));
    SRHI_EXPECT_NOT_NULL(withUnused);
    const ColorVertex tri[3] = {{0, 0.5f, 1, 1, 1, 1}, {0.5f, -0.5f, 1, 1, 1, 1}, {-0.5f, -0.5f, 1, 1, 1, 1}};
    srhiBuffer* vb = gpu.CreateBuffer(sizeof(tri), SRHI_BUFFER_USAGE_VERTEX, tri);
    srhiTexture* target = CreateTarget(gpu, 16, 16);
    gpu.Run([&](srhiCommandBuffer* cmd) {
        srhiRenderPassDesc pass = ClearPass(target, 0, 0, 0, 1);
        srhiRenderEncoder* enc = srhiRenderEncoderBegin(cmd, &pass);
        srhiRenderEncoderSetPipeline(enc, withUnused);
        srhiRenderEncoderSetVertexBuffer(enc, 0, vb, 0);
        DrawParams params = Identity();
        srhiRenderEncoderSetUniforms(enc, 0, &params, sizeof(params));
        srhiRenderEncoderDraw(enc, 3, 1, 0, 0);
        srhiRenderEncoderEnd(enc);
    });
    std::vector<uint8_t> bytes = gpu.ReadTextureBytes(target);
    SRHI_EXPECT_EQ(int(bytes[(8 * 16 + 8) * 4]), 255);
}
