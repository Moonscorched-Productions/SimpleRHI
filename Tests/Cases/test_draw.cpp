/**
 * @ Author: Amélie Heinrich
 * @ Create Time: 2026-11-13 00:00:00
 * @ Copyright: Copyright (c) 2026 Amélie Heinrich. All rights reserved.
 */

#include "test_helpers.h"

using namespace srhitest;

namespace
{
    const ColorVertex kTriangle[3] = {
        {0.0f, 0.75f, 1.0f, 0.0f, 0.0f, 1.0f},
        {0.75f, -0.75f, 0.0f, 1.0f, 0.0f, 1.0f},
        {-0.75f, -0.75f, 0.0f, 0.0f, 1.0f, 1.0f},
    };

    const ColorVertex kQuad[4] = {
        {-0.5f, 0.5f, 1.0f, 1.0f, 0.0f, 1.0f},
        {0.5f, 0.5f, 0.0f, 1.0f, 1.0f, 1.0f},
        {0.5f, -0.5f, 1.0f, 0.0f, 1.0f, 1.0f},
        {-0.5f, -0.5f, 1.0f, 1.0f, 1.0f, 1.0f},
    };
} // namespace

SRHI_TEST_TEXTURE(ClearColor, 64, 64)
{
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);
    srhiTexture* target = CreateTarget(gpu, 64, 64);
    gpu.Run([&](srhiCommandBuffer* cmd) {
        srhiRenderPassDesc pass = ClearPass(target, 0.25f, 0.5f, 0.75f, 1.0f);
        srhiRenderEncoderEnd(srhiRenderEncoderBegin(cmd, &pass));
    });
    Image image = gpu.ReadImage(target);
    SRHI_EXPECT(image.Valid());
    SRHI_EXPECT(std::abs(image.pixels[0] - 64.0f / 255.0f) < 0.01f);
    ExpectImageMatchesGolden(ctx, "ClearColor.png", image);
}

SRHI_TEST_TEXTURE(DrawTriangle, 128, 128)
{
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);
    srhiTexture* target = CreateTarget(gpu, 128, 128);
    srhiRenderPipeline* pipeline = CreateColorPipeline(gpu, SRHI_FORMAT_RGBA8_UNORM);
    SRHI_EXPECT_NOT_NULL(pipeline);
    srhiBuffer* vb = gpu.CreateBuffer(sizeof(kTriangle), SRHI_BUFFER_USAGE_VERTEX, kTriangle);

    gpu.Run([&](srhiCommandBuffer* cmd) {
        srhiRenderPassDesc pass = ClearPass(target, 0.1f, 0.1f, 0.1f, 1.0f);
        srhiRenderEncoder* enc = srhiRenderEncoderBegin(cmd, &pass);
        srhiRenderEncoderSetPipeline(enc, pipeline);
        srhiRenderEncoderSetVertexBuffer(enc, 0, vb, 0);
        DrawParams params = Identity();
        srhiRenderEncoderSetUniforms(enc, 0, &params, sizeof(params));
        srhiRenderEncoderDraw(enc, 3, 1, 0, 0);
        srhiRenderEncoderEnd(enc);
    });
    ExpectImageMatchesGolden(ctx, "DrawTriangle.png", gpu.ReadImage(target));
}

static void DrawIndexedQuads(TestContext& ctx, srhiIndexType indexType, const char* golden)
{
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);
    srhiTexture* target = CreateTarget(gpu, 128, 128);
    srhiRenderPipeline* pipeline = CreateColorPipeline(gpu, SRHI_FORMAT_RGBA8_UNORM);
    SRHI_EXPECT_NOT_NULL(pipeline);

    // Two copies of the quad's vertices; the second draw uses baseVertex = 4 and firstIndex = 6.
    std::vector<ColorVertex> vertices(kQuad, kQuad + 4);
    for (const ColorVertex& v : kQuad)
        vertices.push_back({v.x, v.y, v.b, v.r, v.g, 1.0f});
    srhiBuffer* vb = gpu.CreateBuffer(vertices.size() * sizeof(ColorVertex), SRHI_BUFFER_USAGE_VERTEX, vertices.data());

    const uint32_t indices32[12] = {0, 1, 2, 0, 2, 3, 9, 9, 0, 1, 2, 0}; // 9s pad so firstIndex 8 starts at "0,1,2"
    uint16_t indices16[12];
    for (int i = 0; i < 12; ++i)
        indices16[i] = uint16_t(indices32[i]);
    srhiBuffer* ib = indexType == SRHI_INDEX_TYPE_UINT16 ? gpu.CreateBuffer(24, SRHI_BUFFER_USAGE_INDEX, indices16)
                                                         : gpu.CreateBuffer(48, SRHI_BUFFER_USAGE_INDEX, indices32);

    gpu.Run([&](srhiCommandBuffer* cmd) {
        srhiRenderPassDesc pass = ClearPass(target, 0.0f, 0.0f, 0.0f, 1.0f);
        srhiRenderEncoder* enc = srhiRenderEncoderBegin(cmd, &pass);
        srhiRenderEncoderSetPipeline(enc, pipeline);
        srhiRenderEncoderSetVertexBuffer(enc, 0, vb, 0);
        srhiRenderEncoderSetIndexBuffer(enc, ib, indexType, 0);
        DrawParams left = {{-0.45f, 0.0f, 0.8f, 0.8f}};
        srhiRenderEncoderSetUniforms(enc, 0, &left, sizeof(left));
        srhiRenderEncoderDrawIndexed(enc, 6, 1, 0, 0, 0);
        DrawParams right = {{0.45f, 0.0f, 0.8f, 0.8f}};
        srhiRenderEncoderSetUniforms(enc, 0, &right, sizeof(right));
        srhiRenderEncoderDrawIndexed(enc, 3, 1, 8, 4, 0); // first triangle only, second vertex set
        srhiRenderEncoderEnd(enc);
    });
    ExpectImageMatchesGolden(ctx, golden, gpu.ReadImage(target));
}

SRHI_TEST_TEXTURE(DrawIndexedU16, 128, 128) { DrawIndexedQuads(ctx, SRHI_INDEX_TYPE_UINT16, "DrawIndexed.png"); }
SRHI_TEST_TEXTURE(DrawIndexedU32, 128, 128) { DrawIndexedQuads(ctx, SRHI_INDEX_TYPE_UINT32, "DrawIndexed.png"); }

SRHI_TEST_TEXTURE(DrawInstanced, 128, 128)
{
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);
    srhiTexture* target = CreateTarget(gpu, 128, 128);

    srhiRenderPipelineDesc desc = {};
    desc.vertexLayout.bufferCount = 2;
    desc.vertexLayout.buffers[0].stride = sizeof(ColorVertex);
    desc.vertexLayout.buffers[1].stride = 8;
    desc.vertexLayout.buffers[1].stepMode = SRHI_VERTEX_STEP_MODE_INSTANCE;
    desc.vertexLayout.attributeCount = 3;
    desc.vertexLayout.attributes[0] = {0, SRHI_VERTEX_FORMAT_FLOAT2, 0, 0};
    desc.vertexLayout.attributes[1] = {1, SRHI_VERTEX_FORMAT_FLOAT4, 8, 0};
    desc.vertexLayout.attributes[2] = {2, SRHI_VERTEX_FORMAT_FLOAT2, 0, 1};
    desc.module = gpu.LoadShader("triangle.slang");
    SRHI_EXPECT_NOT_NULL(desc.module);
    desc.vertexEntry = "vsInstanced";
    desc.fragmentEntry = "fsMain";
    desc.colorTargetCount = 1;
    desc.colorTargets[0].format = SRHI_FORMAT_RGBA8_UNORM;
    srhiRenderPipeline* pipeline = gpu.Own(srhiRenderPipelineCreate(gpu.Device(), &desc));
    SRHI_EXPECT_NOT_NULL(pipeline);

    srhiBuffer* vb = gpu.CreateBuffer(sizeof(kTriangle), SRHI_BUFFER_USAGE_VERTEX, kTriangle);
    const float offsets[8] = {-0.5f, 0.5f, 0.5f, 0.5f, -0.5f, -0.5f, 0.5f, -0.5f};
    srhiBuffer* instances = gpu.CreateBuffer(sizeof(offsets), SRHI_BUFFER_USAGE_VERTEX, offsets);

    gpu.Run([&](srhiCommandBuffer* cmd) {
        srhiRenderPassDesc pass = ClearPass(target, 0.0f, 0.0f, 0.0f, 1.0f);
        srhiRenderEncoder* enc = srhiRenderEncoderBegin(cmd, &pass);
        srhiRenderEncoderSetPipeline(enc, pipeline);
        srhiRenderEncoderSetVertexBuffer(enc, 0, vb, 0);
        srhiRenderEncoderSetVertexBuffer(enc, 1, instances, 0);
        DrawParams params = {{0.0f, 0.0f, 0.4f, 0.4f}};
        srhiRenderEncoderSetUniforms(enc, 0, &params, sizeof(params));
        srhiRenderEncoderDraw(enc, 3, 3, 0, 1); // instances 1..3: skips the top-left one
        srhiRenderEncoderEnd(enc);
    });
    ExpectImageMatchesGolden(ctx, "DrawInstanced.png", gpu.ReadImage(target));
}

SRHI_TEST_TEXTURE(DrawIndirect, 128, 128)
{
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);
    srhiTexture* target = CreateTarget(gpu, 128, 128);
    srhiRenderPipeline* pipeline = CreateColorPipeline(gpu, SRHI_FORMAT_RGBA8_UNORM);
    SRHI_EXPECT_NOT_NULL(pipeline);
    srhiBuffer* vb = gpu.CreateBuffer(sizeof(kQuad), SRHI_BUFFER_USAGE_VERTEX, kQuad);
    const uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
    srhiBuffer* ib = gpu.CreateBuffer(sizeof(indices), SRHI_BUFFER_USAGE_INDEX, indices);

    struct
    {
        srhiDrawIndirectArgs draw;
        srhiDrawIndexedIndirectArgs indexed;
        uint32_t pad[3];
    } args = {{3, 1, 0, 0}, {3, 1, 3, 0, 0}, {}};
    srhiBuffer* indirect = gpu.CreateBuffer(sizeof(args), SRHI_BUFFER_USAGE_INDIRECT, &args);

    gpu.Run([&](srhiCommandBuffer* cmd) {
        srhiRenderPassDesc pass = ClearPass(target, 0.0f, 0.0f, 0.0f, 1.0f);
        srhiRenderEncoder* enc = srhiRenderEncoderBegin(cmd, &pass);
        srhiRenderEncoderSetPipeline(enc, pipeline);
        srhiRenderEncoderSetVertexBuffer(enc, 0, vb, 0);
        srhiRenderEncoderSetIndexBuffer(enc, ib, SRHI_INDEX_TYPE_UINT16, 0);
        DrawParams left = {{-0.45f, 0.0f, 0.8f, 0.8f}};
        srhiRenderEncoderSetUniforms(enc, 0, &left, sizeof(left));
        srhiRenderEncoderDrawIndirect(enc, indirect, 0);
        DrawParams right = {{0.45f, 0.0f, 0.8f, 0.8f}};
        srhiRenderEncoderSetUniforms(enc, 0, &right, sizeof(right));
        srhiRenderEncoderDrawIndexedIndirect(enc, indirect, sizeof(srhiDrawIndirectArgs));
        srhiRenderEncoderEnd(enc);
    });
    ExpectImageMatchesGolden(ctx, "DrawIndirect.png", gpu.ReadImage(target));
}
