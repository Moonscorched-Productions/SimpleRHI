/**
 * @ Author: Amélie Heinrich
 * @ Create Time: 2026-11-13 00:00:00
 * @ Copyright: Copyright (c) 2026 Amélie Heinrich. All rights reserved.
 */

#include "test_helpers.h"

using namespace srhitest;

namespace
{
    struct ComputeParams
    {
        uint32_t count, multiplier, pad0, pad1;
    };
} // namespace

SRHI_TEST_BUFFER(ComputeBufferUniformsAtomics)
{
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);

    srhiShaderModule* module = gpu.LoadShader("compute_buffer.slang");
    SRHI_EXPECT_NOT_NULL(module);
    srhiResourceSetLayout* layout = gpu.LayoutFromShader(module, 0);
    SRHI_EXPECT_NOT_NULL(layout);

    srhiComputePipelineDesc pd = {};
    pd.module = module;
    pd.entry = "csMain";
    pd.setLayouts[0] = layout;
    pd.label = "compute_buffer";
    srhiComputePipeline* pipeline = gpu.Own(srhiComputePipelineCreate(gpu.Device(), &pd));
    SRHI_EXPECT_NOT_NULL(pipeline);

    uint32_t tg[3];
    srhiComputePipelineGetThreadgroupSize(pipeline, tg);
    SRHI_EXPECT_EQ(tg[0], 64u);

    const uint32_t count = 1000;
    std::vector<uint32_t> input(1024);
    for (uint32_t i = 0; i < input.size(); ++i)
        input[i] = i + 7;
    std::vector<uint32_t> zero(1024, 0);
    srhiBuffer* in = gpu.CreateBuffer(4096, SRHI_BUFFER_USAGE_STORAGE, input.data(), 4, "in");
    srhiBuffer* out = gpu.CreateBuffer(4096, SRHI_BUFFER_USAGE_STORAGE_RW, zero.data(), 4, "out");
    srhiBuffer* counter = gpu.CreateBuffer(256, SRHI_BUFFER_USAGE_STORAGE_RW, zero.data(), 4, "counter");

    srhiResourceBinding bindings[3] = {};
    bindings[0].binding = 0;
    bindings[0].buffer = out;
    bindings[1].binding = 1;
    bindings[1].buffer = in;
    bindings[2].binding = 2;
    bindings[2].buffer = counter;
    srhiResourceSetDesc sd = {layout, bindings, 3, "compute set"};
    srhiResourceSet* set = gpu.Own(srhiResourceSetCreate(gpu.Device(), &sd));
    SRHI_EXPECT_NOT_NULL(set);

    gpu.Run([&](srhiCommandBuffer* cmd) {
        srhiComputeEncoder* enc = srhiComputeEncoderBegin(cmd, nullptr);
        srhiComputeEncoderSetPipeline(enc, pipeline);
        srhiComputeEncoderSetResourceSet(enc, 0, set);
        ComputeParams params = {count, 3, 0, 0};
        srhiComputeEncoderSetUniforms(enc, 0, &params, sizeof(params));
        srhiComputeEncoderDispatch(enc, (count + 63) / 64, 1, 1);
        // Second dispatch over the same buffers with new uniforms: counter keeps accumulating.
        params.multiplier = 5;
        srhiComputeEncoderSetUniforms(enc, 0, &params, sizeof(params));
        srhiComputeEncoderDispatch(enc, (count + 63) / 64, 1, 1);
        srhiComputeEncoderEnd(enc);
    });

    std::vector<uint8_t> outBytes = gpu.ReadBuffer(out);
    std::vector<uint8_t> counterBytes = gpu.ReadBuffer(counter, 0, 4);
    std::vector<uint32_t> expected(1024, 0);
    for (uint32_t i = 0; i < count; ++i)
        expected[i] = input[i] * 5 + i;
    SRHI_EXPECT(outBytes == AsBytes(expected));
    uint32_t total;
    memcpy(&total, counterBytes.data(), 4);
    SRHI_EXPECT_EQ(total, count * 2);
    ExpectBufferMatchesGolden(ctx, "ComputeBufferUniformsAtomics.bin", outBytes);
}

SRHI_TEST_TEXTURE(ComputeTextureWrite, 64, 64)
{
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);

    srhiShaderModule* module = gpu.LoadShader("compute_texture.slang");
    SRHI_EXPECT_NOT_NULL(module);
    srhiResourceSetLayout* layout = gpu.LayoutFromShader(module, 0);
    srhiComputePipelineDesc pd = {};
    pd.module = module;
    pd.entry = "csMain";
    pd.setLayouts[0] = layout;
    srhiComputePipeline* pipeline = gpu.Own(srhiComputePipelineCreate(gpu.Device(), &pd));
    SRHI_EXPECT_NOT_NULL(pipeline);

    srhiTexture* texture = gpu.CreateTexture2D(64, 64, SRHI_FORMAT_RGBA8_UNORM, SRHI_TEXTURE_USAGE_STORAGE | SRHI_TEXTURE_USAGE_COPY_SRC);
    SRHI_EXPECT_NOT_NULL(texture);
    srhiResourceBinding b = {};
    b.binding = 0;
    b.textureView = srhiTextureGetDefaultView(texture);
    srhiResourceSetDesc sd = {layout, &b, 1, nullptr};
    srhiResourceSet* set = gpu.Own(srhiResourceSetCreate(gpu.Device(), &sd));
    SRHI_EXPECT_NOT_NULL(set);

    gpu.Run([&](srhiCommandBuffer* cmd) {
        srhiComputeEncoder* enc = srhiComputeEncoderBegin(cmd, nullptr);
        srhiComputeEncoderSetPipeline(enc, pipeline);
        srhiComputeEncoderSetResourceSet(enc, 0, set);
        srhiComputeEncoderDispatch(enc, 8, 8, 1);
        srhiComputeEncoderEnd(enc);
    });
    ExpectImageMatchesGolden(ctx, "ComputeTextureWrite.png", gpu.ReadImage(texture));
}

SRHI_TEST_BUFFER(ComputeDispatchIndirect)
{
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);

    srhiShaderModule* module = gpu.LoadShader("compute_buffer.slang");
    srhiResourceSetLayout* layout = gpu.LayoutFromShader(module, 0);
    srhiComputePipelineDesc pd = {};
    pd.module = module;
    pd.entry = "csMain";
    pd.setLayouts[0] = layout;
    srhiComputePipeline* pipeline = gpu.Own(srhiComputePipelineCreate(gpu.Device(), &pd));
    SRHI_EXPECT_NOT_NULL(pipeline);

    std::vector<uint32_t> input(256, 1), zero(256, 0);
    srhiBuffer* in = gpu.CreateBuffer(1024, SRHI_BUFFER_USAGE_STORAGE, input.data(), 4);
    srhiBuffer* out = gpu.CreateBuffer(1024, SRHI_BUFFER_USAGE_STORAGE_RW, zero.data(), 4);
    srhiBuffer* counter = gpu.CreateBuffer(256, SRHI_BUFFER_USAGE_STORAGE_RW, zero.data(), 4);
    srhiDispatchIndirectArgs args = {2, 1, 1}; // 128 threads of the 256 elements
    srhiBuffer* indirect = gpu.CreateBuffer(sizeof(args), SRHI_BUFFER_USAGE_INDIRECT, &args);

    srhiResourceBinding bindings[3] = {};
    bindings[0] = {0, out};
    bindings[1] = {1, in};
    bindings[2] = {2, counter};
    srhiResourceSetDesc sd = {layout, bindings, 3, nullptr};
    srhiResourceSet* set = gpu.Own(srhiResourceSetCreate(gpu.Device(), &sd));

    gpu.Run([&](srhiCommandBuffer* cmd) {
        srhiComputeEncoder* enc = srhiComputeEncoderBegin(cmd, nullptr);
        srhiComputeEncoderSetPipeline(enc, pipeline);
        srhiComputeEncoderSetResourceSet(enc, 0, set);
        ComputeParams params = {256, 2, 0, 0};
        srhiComputeEncoderSetUniforms(enc, 0, &params, sizeof(params));
        srhiComputeEncoderDispatchIndirect(enc, indirect, 0);
        srhiComputeEncoderEnd(enc);
    });

    std::vector<uint8_t> bytes = gpu.ReadBuffer(out);
    std::vector<uint32_t> expected(256, 0);
    for (uint32_t i = 0; i < 128; ++i)
        expected[i] = 2 + i;
    SRHI_EXPECT(bytes == AsBytes(expected));
    ExpectBufferMatchesGolden(ctx, "ComputeDispatchIndirect.bin", bytes);
}
