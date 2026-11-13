/**
 * @ Author: Amélie Heinrich
 * @ Create Time: 2026-11-13 00:00:00
 * @ Copyright: Copyright (c) 2026 Amélie Heinrich. All rights reserved.
 */

// Fixed-function pipeline state: depth, stencil, blending, culling, viewport/scissor, MSAA, MRT,
// topologies, matrices, fragment storage writes.

#include "test_helpers.h"

using namespace srhitest;

namespace
{
    struct Vertex3
    {
        float x, y, z;
        float r, g, b, a;
    };

    /// @brief Column-major 4x4 (m[column * 4 + row]), the layout glm uses and shaders expect.
    struct Mat4
    {
        float m[16];
    };

    Mat4 Identity4()
    {
        Mat4 r = {};
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
        return r;
    }

    Mat4 TranslateScale(float tx, float ty, float tz, float sx, float sy)
    {
        Mat4 r = Identity4();
        r.m[0] = sx;
        r.m[5] = sy;
        r.m[12] = tx;
        r.m[13] = ty;
        r.m[14] = tz;
        return r;
    }

    struct GeometryParams
    {
        Mat4 transform;
        float tint[4];
    };

    GeometryParams Params(const Mat4& m, float r = 1, float g = 1, float b = 1, float a = 1)
    {
        GeometryParams p;
        p.transform = m;
        p.tint[0] = r;
        p.tint[1] = g;
        p.tint[2] = b;
        p.tint[3] = a;
        return p;
    }

    // A quad at z (two CCW triangles in a list), colored.
    std::vector<Vertex3> Quad(float x0, float y0, float x1, float y1, float z, float r, float g, float b, float a = 1.0f)
    {
        return {{x0, y1, z, r, g, b, a}, {x0, y0, z, r, g, b, a}, {x1, y0, z, r, g, b, a},
                {x0, y1, z, r, g, b, a}, {x1, y0, z, r, g, b, a}, {x1, y1, z, r, g, b, a}};
    }

    srhiVertexLayout Vertex3Layout()
    {
        srhiVertexLayout layout = {};
        layout.bufferCount = 1;
        layout.buffers[0].stride = sizeof(Vertex3);
        layout.attributeCount = 2;
        layout.attributes[0] = {0, SRHI_VERTEX_FORMAT_FLOAT3, 0, 0};
        layout.attributes[1] = {1, SRHI_VERTEX_FORMAT_FLOAT4, 12, 0};
        return layout;
    }

    srhiRenderPipelineDesc GeometryDesc(GpuFixture& gpu, srhiFormat color, const char* fs = "fsMain")
    {
        srhiRenderPipelineDesc desc = {};
        desc.module = gpu.LoadShader("geometry.slang");
        desc.vertexEntry = "vsMain";
        desc.fragmentEntry = fs;
        desc.vertexLayout = Vertex3Layout();
        desc.colorTargetCount = color == SRHI_FORMAT_UNDEFINED ? 0 : 1;
        desc.colorTargets[0].format = color;
        return desc;
    }

    srhiRenderPipeline* Pipeline(GpuFixture& gpu, const srhiRenderPipelineDesc& desc)
    {
        return gpu.Own(srhiRenderPipelineCreate(gpu.Device(), &desc));
    }

    srhiBuffer* Vertices(GpuFixture& gpu, const std::vector<Vertex3>& v)
    {
        return gpu.CreateBuffer(v.size() * sizeof(Vertex3), SRHI_BUFFER_USAGE_VERTEX, v.data());
    }

    void Draw(srhiRenderEncoder* enc, srhiRenderPipeline* pipeline, srhiBuffer* vb, uint32_t first, uint32_t count,
              const GeometryParams& params)
    {
        srhiRenderEncoderSetPipeline(enc, pipeline);
        srhiRenderEncoderSetVertexBuffer(enc, 0, vb, 0);
        srhiRenderEncoderSetUniforms(enc, 0, &params, sizeof(params));
        srhiRenderEncoderDraw(enc, count, 1, first, 0);
    }

    template <typename... Ts> std::vector<Vertex3> Concat(Ts... parts)
    {
        std::vector<Vertex3> out;
        (out.insert(out.end(), parts.begin(), parts.end()), ...);
        return out;
    }
} // namespace

SRHI_TEST_TEXTURE(MatrixUniform, 64, 64)
{
    // Column-major translation: the quad must land in the top-right quadrant.
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);
    srhiTexture* target = CreateTarget(gpu, 64, 64);
    srhiRenderPipeline* pipeline = Pipeline(gpu, GeometryDesc(gpu, SRHI_FORMAT_RGBA8_UNORM));
    SRHI_EXPECT_NOT_NULL(pipeline);
    srhiBuffer* vb = Vertices(gpu, Quad(-1, -1, 1, 1, 0.5f, 1, 1, 0));
    gpu.Run([&](srhiCommandBuffer* cmd) {
        srhiRenderPassDesc pass = ClearPass(target, 0, 0, 0, 1);
        srhiRenderEncoder* enc = srhiRenderEncoderBegin(cmd, &pass);
        Draw(enc, pipeline, vb, 0, 6, Params(TranslateScale(0.5f, 0.5f, 0, 0.5f, 0.5f)));
        srhiRenderEncoderEnd(enc);
    });
    Image image = gpu.ReadImage(target);
    SRHI_EXPECT(image.pixels[(8 * 64 + 56) * 4 + 0] > 0.99f);  // top-right lit
    SRHI_EXPECT(image.pixels[(56 * 64 + 8) * 4 + 0] < 0.01f);  // bottom-left dark
    ExpectImageMatchesGolden(ctx, "MatrixUniform.png", image);
}

SRHI_TEST_TEXTURE(DepthTest, 64, 64)
{
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);
    srhiTexture* target = CreateTarget(gpu, 64, 64);
    srhiTexture* depth = gpu.CreateTexture2D(64, 64, SRHI_FORMAT_D32_FLOAT, SRHI_TEXTURE_USAGE_RENDER_TARGET);
    srhiRenderPipelineDesc desc = GeometryDesc(gpu, SRHI_FORMAT_RGBA8_UNORM);
    desc.depthStencil.format = SRHI_FORMAT_D32_FLOAT;
    desc.depthStencil.depthTestEnable = 1;
    desc.depthStencil.depthWriteEnable = 1;
    desc.depthStencil.depthCompare = SRHI_COMPARE_FUNCTION_LESS;
    srhiRenderPipeline* pipeline = Pipeline(gpu, desc);
    SRHI_EXPECT_NOT_NULL(pipeline);

    // Near red quad first, then a far green quad overlapping it, then a blue one at the clear depth (rejected).
    srhiBuffer* vb = Vertices(gpu, Concat(Quad(-0.8f, -0.8f, 0.3f, 0.3f, 0.2f, 1, 0, 0), Quad(-0.3f, -0.3f, 0.8f, 0.8f, 0.6f, 0, 1, 0),
                                          Quad(-1, -1, 1, 1, 1.0f, 0, 0, 1)));
    gpu.Run([&](srhiCommandBuffer* cmd) {
        srhiRenderPassDesc pass = ClearPass(target, 0, 0, 0, 1);
        pass.depthAttachment.view = srhiTextureGetDefaultView(depth);
        pass.depthAttachment.depthLoadAction = SRHI_LOAD_ACTION_CLEAR;
        pass.depthAttachment.depthStoreAction = SRHI_STORE_ACTION_DONT_CARE;
        pass.depthAttachment.clearDepth = 1.0f;
        srhiRenderEncoder* enc = srhiRenderEncoderBegin(cmd, &pass);
        Draw(enc, pipeline, vb, 0, 18, Params(Identity4()));
        srhiRenderEncoderEnd(enc);
    });
    ExpectImageMatchesGolden(ctx, "DepthTest.png", gpu.ReadImage(target));
}

SRHI_TEST_TEXTURE(DepthReadback, 64, 64)
{
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);
    srhiTexture* depth = gpu.CreateTexture2D(64, 64, SRHI_FORMAT_D32_FLOAT,
                                             SRHI_TEXTURE_USAGE_RENDER_TARGET | SRHI_TEXTURE_USAGE_SAMPLED | SRHI_TEXTURE_USAGE_COPY_SRC);
    srhiRenderPipelineDesc desc = GeometryDesc(gpu, SRHI_FORMAT_UNDEFINED, nullptr);
    desc.fragmentEntry = nullptr;
    desc.depthStencil.format = SRHI_FORMAT_D32_FLOAT;
    desc.depthStencil.depthTestEnable = 1;
    desc.depthStencil.depthWriteEnable = 1;
    desc.depthStencil.depthCompare = SRHI_COMPARE_FUNCTION_LESS_EQUAL;
    srhiRenderPipeline* pipeline = Pipeline(gpu, desc);
    SRHI_EXPECT_NOT_NULL(pipeline);

    // A quad sloping in depth from 0 (left) to 1 (right).
    std::vector<Vertex3> v = {{-1, 1, 0, 1, 1, 1, 1}, {-1, -1, 0, 1, 1, 1, 1}, {1, -1, 1, 1, 1, 1, 1},
                              {-1, 1, 0, 1, 1, 1, 1}, {1, -1, 1, 1, 1, 1, 1},  {1, 1, 1, 1, 1, 1, 1}};
    srhiBuffer* vb = Vertices(gpu, v);
    gpu.Run([&](srhiCommandBuffer* cmd) {
        srhiRenderPassDesc pass = {};
        pass.depthAttachment.view = srhiTextureGetDefaultView(depth);
        pass.depthAttachment.depthLoadAction = SRHI_LOAD_ACTION_CLEAR;
        pass.depthAttachment.depthStoreAction = SRHI_STORE_ACTION_STORE;
        pass.depthAttachment.clearDepth = 1.0f;
        srhiRenderEncoder* enc = srhiRenderEncoderBegin(cmd, &pass);
        Draw(enc, pipeline, vb, 0, 6, Params(Identity4()));
        srhiRenderEncoderEnd(enc);
    });
    Image image = gpu.ReadImage(depth);
    SRHI_EXPECT(image.Valid());
    SRHI_EXPECT(image.pixels[(32 * 64 + 0) * 4] < 0.05f && image.pixels[(32 * 64 + 63) * 4] > 0.95f);
    ExpectImageMatchesGolden(ctx, "DepthReadback.png", image);
}

SRHI_TEST_TEXTURE(StencilMask, 64, 64)
{
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);
    srhiTexture* target = CreateTarget(gpu, 64, 64);
    srhiTexture* ds = gpu.CreateTexture2D(64, 64, SRHI_FORMAT_D32_FLOAT_S8_UINT, SRHI_TEXTURE_USAGE_RENDER_TARGET);

    // Pass 1 state: write stencil = ref where the diamond covers, no color writes.
    srhiRenderPipelineDesc writeDesc = GeometryDesc(gpu, SRHI_FORMAT_RGBA8_UNORM);
    writeDesc.colorTargets[0].writeDisabled = 1;
    writeDesc.depthStencil.format = SRHI_FORMAT_D32_FLOAT_S8_UINT;
    writeDesc.depthStencil.stencilEnable = 1;
    writeDesc.depthStencil.stencilReadMask = 0xFF;
    writeDesc.depthStencil.stencilWriteMask = 0xFF;
    writeDesc.depthStencil.stencilFront = {SRHI_COMPARE_FUNCTION_ALWAYS, SRHI_STENCIL_OP_KEEP, SRHI_STENCIL_OP_KEEP, SRHI_STENCIL_OP_REPLACE};
    writeDesc.depthStencil.stencilBack = writeDesc.depthStencil.stencilFront;
    srhiRenderPipeline* write = Pipeline(gpu, writeDesc);

    // Pass 2 state: draw a full-screen quad only where stencil == ref.
    srhiRenderPipelineDesc testDesc = writeDesc;
    testDesc.colorTargets[0].writeDisabled = 0;
    testDesc.depthStencil.stencilWriteMask = 0;
    testDesc.depthStencil.stencilFront = {SRHI_COMPARE_FUNCTION_EQUAL, SRHI_STENCIL_OP_KEEP, SRHI_STENCIL_OP_KEEP, SRHI_STENCIL_OP_KEEP};
    testDesc.depthStencil.stencilBack = testDesc.depthStencil.stencilFront;
    srhiRenderPipeline* test = Pipeline(gpu, testDesc);
    SRHI_EXPECT(write && test);

    std::vector<Vertex3> diamond = {{0, 0.8f, 0, 1, 1, 1, 1}, {-0.8f, 0, 0, 1, 1, 1, 1}, {0.8f, 0, 0, 1, 1, 1, 1},
                                    {0.8f, 0, 0, 1, 1, 1, 1}, {-0.8f, 0, 0, 1, 1, 1, 1}, {0, -0.8f, 0, 1, 1, 1, 1}};
    srhiBuffer* vb = Vertices(gpu, Concat(diamond, Quad(-1, -1, 1, 1, 0, 1, 0.5f, 0)));
    gpu.Run([&](srhiCommandBuffer* cmd) {
        srhiRenderPassDesc pass = ClearPass(target, 0, 0, 0.3f, 1);
        pass.depthAttachment.view = srhiTextureGetDefaultView(ds);
        pass.depthAttachment.depthLoadAction = SRHI_LOAD_ACTION_CLEAR;
        pass.depthAttachment.clearDepth = 1.0f;
        pass.depthAttachment.stencilLoadAction = SRHI_LOAD_ACTION_CLEAR;
        pass.depthAttachment.clearStencil = 0;
        pass.depthAttachment.depthStoreAction = SRHI_STORE_ACTION_DONT_CARE;
        pass.depthAttachment.stencilStoreAction = SRHI_STORE_ACTION_DONT_CARE;
        srhiRenderEncoder* enc = srhiRenderEncoderBegin(cmd, &pass);
        srhiRenderEncoderSetStencilReference(enc, 7);
        Draw(enc, write, vb, 0, 6, Params(Identity4()));
        Draw(enc, test, vb, 6, 6, Params(Identity4()));
        srhiRenderEncoderEnd(enc);
    });
    ExpectImageMatchesGolden(ctx, "StencilMask.png", gpu.ReadImage(target));
}

SRHI_TEST_TEXTURE(BlendModes, 64, 64)
{
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);
    srhiTexture* target = CreateTarget(gpu, 64, 64);

    srhiRenderPipelineDesc alphaDesc = GeometryDesc(gpu, SRHI_FORMAT_RGBA8_UNORM);
    alphaDesc.colorTargets[0].blend = {1, SRHI_BLEND_FACTOR_SRC_ALPHA, SRHI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, SRHI_BLEND_OP_ADD,
                                       SRHI_BLEND_FACTOR_ONE, SRHI_BLEND_FACTOR_ZERO, SRHI_BLEND_OP_ADD};
    srhiRenderPipelineDesc addDesc = alphaDesc;
    addDesc.colorTargets[0].blend = {1, SRHI_BLEND_FACTOR_ONE, SRHI_BLEND_FACTOR_ONE, SRHI_BLEND_OP_ADD,
                                     SRHI_BLEND_FACTOR_ONE, SRHI_BLEND_FACTOR_ONE, SRHI_BLEND_OP_ADD};
    srhiRenderPipelineDesc constDesc = alphaDesc;
    constDesc.colorTargets[0].blend = {1, SRHI_BLEND_FACTOR_BLEND_COLOR, SRHI_BLEND_FACTOR_ONE_MINUS_BLEND_COLOR, SRHI_BLEND_OP_ADD,
                                       SRHI_BLEND_FACTOR_ONE, SRHI_BLEND_FACTOR_ZERO, SRHI_BLEND_OP_ADD};
    srhiRenderPipelineDesc minDesc = alphaDesc;
    minDesc.colorTargets[0].blend = {1, SRHI_BLEND_FACTOR_ONE, SRHI_BLEND_FACTOR_ONE, SRHI_BLEND_OP_MIN,
                                     SRHI_BLEND_FACTOR_ONE, SRHI_BLEND_FACTOR_ONE, SRHI_BLEND_OP_MAX};
    srhiRenderPipelineDesc maskDesc = GeometryDesc(gpu, SRHI_FORMAT_RGBA8_UNORM);
    maskDesc.colorTargets[0].writeMask = SRHI_COLOR_WRITE_GREEN | SRHI_COLOR_WRITE_ALPHA;
    srhiRenderPipeline* alpha = Pipeline(gpu, alphaDesc);
    srhiRenderPipeline* add = Pipeline(gpu, addDesc);
    srhiRenderPipeline* constant = Pipeline(gpu, constDesc);
    srhiRenderPipeline* minOp = Pipeline(gpu, minDesc);
    srhiRenderPipeline* mask = Pipeline(gpu, maskDesc);
    SRHI_EXPECT(alpha && add && constant && minOp && mask);

    srhiBuffer* vb = Vertices(gpu, Concat(Quad(-1, 0, 0, 1, 0, 1, 0, 0, 0.5f), Quad(0, 0, 1, 1, 0, 0, 0.25f, 0.5f, 1),
                                          Quad(-1, -1, 0, 0, 0, 1, 1, 0, 1), Quad(0, -1, 0.5f, 0, 0, 0.2f, 0.9f, 0.9f, 1),
                                          Quad(0.5f, -1, 1, 0, 0, 1, 1, 1, 1)));
    gpu.Run([&](srhiCommandBuffer* cmd) {
        srhiRenderPassDesc pass = ClearPass(target, 0.2f, 0.4f, 0.6f, 1);
        srhiRenderEncoder* enc = srhiRenderEncoderBegin(cmd, &pass);
        const float blendColor[4] = {0.25f, 0.5f, 0.75f, 1.0f};
        srhiRenderEncoderSetBlendColor(enc, blendColor);
        Draw(enc, alpha, vb, 0, 6, Params(Identity4()));
        Draw(enc, add, vb, 6, 6, Params(Identity4()));
        Draw(enc, constant, vb, 12, 6, Params(Identity4()));
        Draw(enc, minOp, vb, 18, 6, Params(Identity4()));
        Draw(enc, mask, vb, 24, 6, Params(Identity4()));
        srhiRenderEncoderEnd(enc);
    });
    ExpectImageMatchesGolden(ctx, "BlendModes.png", gpu.ReadImage(target));
}

SRHI_TEST_TEXTURE(CullAndWinding, 64, 64)
{
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);
    srhiTexture* target = CreateTarget(gpu, 64, 64);
    srhiRenderPipelineDesc backDesc = GeometryDesc(gpu, SRHI_FORMAT_RGBA8_UNORM);
    backDesc.raster.cullMode = SRHI_CULL_MODE_BACK;
    srhiRenderPipelineDesc cwDesc = backDesc;
    cwDesc.raster.frontFace = SRHI_FRONT_FACE_CLOCKWISE;
    srhiRenderPipeline* back = Pipeline(gpu, backDesc);
    srhiRenderPipeline* cw = Pipeline(gpu, cwDesc);
    SRHI_EXPECT(back && cw);

    // Top row: CCW (kept) then CW (culled) with CCW-front. Bottom row: same triangles with CW-front.
    std::vector<Vertex3> ccw = {{-0.9f, 0.9f, 0, 1, 0, 0, 1}, {-0.9f, 0.1f, 0, 1, 0, 0, 1}, {-0.1f, 0.1f, 0, 1, 0, 0, 1}};
    std::vector<Vertex3> clockwise = {{0.1f, 0.9f, 0, 0, 1, 0, 1}, {0.9f, 0.1f, 0, 0, 1, 0, 1}, {0.1f, 0.1f, 0, 0, 1, 0, 1}};
    srhiBuffer* vb = Vertices(gpu, Concat(ccw, clockwise));
    gpu.Run([&](srhiCommandBuffer* cmd) {
        srhiRenderPassDesc pass = ClearPass(target, 0, 0, 0, 1);
        srhiRenderEncoder* enc = srhiRenderEncoderBegin(cmd, &pass);
        Draw(enc, back, vb, 0, 6, Params(Identity4()));
        Draw(enc, cw, vb, 0, 6, Params(TranslateScale(0, -1.0f, 0, 1, 1)));
        srhiRenderEncoderEnd(enc);
    });
    Image image = gpu.ReadImage(target);
    SRHI_EXPECT(image.pixels[(8 * 64 + 4) * 4 + 0] > 0.99f);   // CCW kept with CCW front
    SRHI_EXPECT(image.pixels[(8 * 64 + 36) * 4 + 1] < 0.01f);  // CW culled with CCW front
    SRHI_EXPECT(image.pixels[(40 * 64 + 4) * 4 + 0] < 0.01f);  // CCW culled with CW front
    SRHI_EXPECT(image.pixels[(40 * 64 + 36) * 4 + 1] > 0.99f); // CW kept with CW front
    ExpectImageMatchesGolden(ctx, "CullAndWinding.png", image);
}

SRHI_TEST_TEXTURE(ViewportScissor, 64, 64)
{
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);
    srhiTexture* target = CreateTarget(gpu, 64, 64);
    srhiRenderPipeline* pipeline = Pipeline(gpu, GeometryDesc(gpu, SRHI_FORMAT_RGBA8_UNORM));
    SRHI_EXPECT_NOT_NULL(pipeline);
    srhiBuffer* vb = Vertices(gpu, Concat(Quad(-1, -1, 1, 1, 0, 1, 0, 0), Quad(-1, -1, 1, 1, 0, 0, 1, 0)));
    gpu.Run([&](srhiCommandBuffer* cmd) {
        srhiRenderPassDesc pass = ClearPass(target, 0, 0, 0, 1);
        srhiRenderEncoder* enc = srhiRenderEncoderBegin(cmd, &pass);
        srhiViewport vp = {8, 8, 24, 48, 0, 1};
        srhiRenderEncoderSetViewport(enc, &vp);
        Draw(enc, pipeline, vb, 0, 6, Params(Identity4()));
        srhiViewport full = {0, 0, 64, 64, 0, 1};
        srhiRenderEncoderSetViewport(enc, &full);
        srhiScissorRect sc = {40, 4, 20, 30};
        srhiRenderEncoderSetScissor(enc, &sc);
        Draw(enc, pipeline, vb, 6, 6, Params(Identity4()));
        srhiRenderEncoderEnd(enc);
    });
    ExpectImageMatchesGolden(ctx, "ViewportScissor.png", gpu.ReadImage(target));
}

SRHI_TEST_TEXTURE(MSAAResolve, 64, 64)
{
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);
    srhiTextureDesc td = {};
    td.format = SRHI_FORMAT_RGBA8_UNORM;
    td.width = td.height = 64;
    td.sampleCount = 4;
    td.usage = SRHI_TEXTURE_USAGE_RENDER_TARGET;
    srhiTexture* msaa = gpu.Own(srhiTextureCreate(gpu.Device(), &td));
    srhiTexture* resolved = CreateTarget(gpu, 64, 64);
    SRHI_EXPECT(msaa && resolved);
    srhiRenderPipelineDesc desc = GeometryDesc(gpu, SRHI_FORMAT_RGBA8_UNORM);
    desc.sampleCount = 4;
    srhiRenderPipeline* pipeline = Pipeline(gpu, desc);
    SRHI_EXPECT_NOT_NULL(pipeline);
    std::vector<Vertex3> tri = {{-0.9f, 0.7f, 0, 1, 1, 1, 1}, {-0.6f, -0.9f, 0, 1, 1, 1, 1}, {0.9f, -0.3f, 0, 1, 1, 1, 1}};
    srhiBuffer* vb = Vertices(gpu, tri);
    gpu.Run([&](srhiCommandBuffer* cmd) {
        srhiRenderPassDesc pass = ClearPass(msaa, 0, 0, 0, 1);
        pass.colorAttachments[0].storeAction = SRHI_STORE_ACTION_RESOLVE;
        pass.colorAttachments[0].resolveView = srhiTextureGetDefaultView(resolved);
        srhiRenderEncoder* enc = srhiRenderEncoderBegin(cmd, &pass);
        Draw(enc, pipeline, vb, 0, 3, Params(Identity4()));
        srhiRenderEncoderEnd(enc);
    });
    Image image = gpu.ReadImage(resolved);
    // Edge pixels must contain partial coverage.
    int partial = 0;
    for (size_t i = 0; i < image.pixels.size(); i += 4)
        partial += image.pixels[i] > 0.1f && image.pixels[i] < 0.9f;
    SRHI_EXPECT_MSG(partial > 20, "no partially covered pixels: MSAA resolve did not happen");
    ExpectImageMatchesGolden(ctx, "MSAAResolve.png", image);
}

SRHI_TEST_TEXTURE(MultipleRenderTargets, 64, 64)
{
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);
    srhiTexture* t0 = CreateTarget(gpu, 64, 64);
    srhiTexture* t1 = CreateTarget(gpu, 64, 64);
    srhiTexture* t2 = CreateTarget(gpu, 64, 64, SRHI_FORMAT_RGBA16_FLOAT);
    srhiRenderPipelineDesc desc = GeometryDesc(gpu, SRHI_FORMAT_RGBA8_UNORM, "fsMRT");
    desc.colorTargetCount = 3;
    desc.colorTargets[1].format = SRHI_FORMAT_RGBA8_UNORM;
    desc.colorTargets[2].format = SRHI_FORMAT_RGBA16_FLOAT;
    srhiRenderPipeline* pipeline = Pipeline(gpu, desc);
    SRHI_EXPECT_NOT_NULL(pipeline);
    srhiBuffer* vb = Vertices(gpu, Quad(-0.75f, -0.75f, 0.75f, 0.75f, 0.25f, 0.9f, 0.6f, 0.1f));
    gpu.Run([&](srhiCommandBuffer* cmd) {
        srhiRenderPassDesc pass = ClearPass(t0, 0, 0, 0, 1);
        pass.colorAttachmentCount = 3;
        pass.colorAttachments[1] = pass.colorAttachments[0];
        pass.colorAttachments[1].view = srhiTextureGetDefaultView(t1);
        pass.colorAttachments[2] = pass.colorAttachments[0];
        pass.colorAttachments[2].view = srhiTextureGetDefaultView(t2);
        srhiRenderEncoder* enc = srhiRenderEncoderBegin(cmd, &pass);
        Draw(enc, pipeline, vb, 0, 6, Params(Identity4()));
        srhiRenderEncoderEnd(enc);
    });
    // Stitch the three targets side by side (64 x 3 wide is too wide for the declared size, so stack rows).
    Image a = gpu.ReadImage(t0), b = gpu.ReadImage(t1), c = gpu.ReadImage(t2);
    SRHI_EXPECT(a.Valid() && b.Valid() && c.Valid());
    Image out = a;
    for (uint32_t y = 0; y < 64; ++y)
        for (uint32_t x = 0; x < 64; ++x) {
            const Image& src = y < 21 ? a : y < 42 ? b : c;
            for (int k = 0; k < 4; ++k)
                out.pixels[(y * 64 + x) * 4 + k] = src.pixels[(y * 64 + x) * 4 + k];
        }
    ExpectImageMatchesGolden(ctx, "MultipleRenderTargets.png", out);
}

SRHI_TEST_TEXTURE(LinesAndStrips, 64, 64)
{
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);
    srhiTexture* target = CreateTarget(gpu, 64, 64);
    srhiRenderPipelineDesc lineDesc = GeometryDesc(gpu, SRHI_FORMAT_RGBA8_UNORM);
    lineDesc.topology = SRHI_PRIMITIVE_TOPOLOGY_LINE_LIST;
    srhiRenderPipelineDesc stripDesc = lineDesc;
    stripDesc.topology = SRHI_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    srhiRenderPipelineDesc triStripDesc = lineDesc;
    triStripDesc.topology = SRHI_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    srhiRenderPipeline* lines = Pipeline(gpu, lineDesc);
    srhiRenderPipeline* strip = Pipeline(gpu, stripDesc);
    srhiRenderPipeline* triStrip = Pipeline(gpu, triStripDesc);
    SRHI_EXPECT(lines && strip && triStrip);

    // Pixel-center aligned coordinates avoid diamond-exit rule differences: x = (px + 0.5) / 32 - 1.
    auto px = [](float p) { return (p + 0.5f) / 32.0f - 1.0f; };
    std::vector<Vertex3> v = {
        {px(4), px(4), 0, 1, 0, 0, 1},   {px(60), px(4), 0, 1, 0, 0, 1},   // horizontal line
        {px(4), px(10), 0, 0, 1, 0, 1},  {px(4), px(60), 0, 0, 1, 0, 1},   // vertical line
        {px(10), px(20), 0, 1, 1, 0, 1}, {px(30), px(20), 0, 1, 1, 0, 1}, {px(30), px(40), 0, 1, 1, 0, 1}, // strip
        {0.2f, 0.9f, 0, 0, 0, 1, 1},     {0.2f, 0.3f, 0, 0, 0, 1, 1},     {0.9f, 0.9f, 0, 0, 1, 1, 1},     {0.9f, 0.3f, 0, 0, 1, 1, 1}};
    srhiBuffer* vb = Vertices(gpu, v);
    gpu.Run([&](srhiCommandBuffer* cmd) {
        srhiRenderPassDesc pass = ClearPass(target, 0, 0, 0, 1);
        srhiRenderEncoder* enc = srhiRenderEncoderBegin(cmd, &pass);
        Draw(enc, lines, vb, 0, 4, Params(Identity4()));
        Draw(enc, strip, vb, 4, 3, Params(Identity4()));
        Draw(enc, triStrip, vb, 7, 4, Params(Identity4()));
        srhiRenderEncoderEnd(enc);
    });
    ExpectImageMatchesGolden(ctx, "LinesAndStrips.png", gpu.ReadImage(target));
}

SRHI_TEST_BUFFER(FragmentStorageWrite)
{
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);
    srhiTexture* target = CreateTarget(gpu, 64, 64);
    srhiShaderModule* module = gpu.LoadShader("geometry.slang");
    srhiResourceSetLayout* layout = gpu.LayoutFromShader(module, 0);
    srhiRenderPipelineDesc desc = GeometryDesc(gpu, SRHI_FORMAT_RGBA8_UNORM, "fsStorage");
    desc.setLayouts[0] = layout;
    srhiRenderPipeline* pipeline = Pipeline(gpu, desc);
    SRHI_EXPECT_NOT_NULL(pipeline);

    std::vector<uint32_t> zero(64 * 64, 0);
    srhiBuffer* pixels = gpu.CreateBuffer(zero.size() * 4, SRHI_BUFFER_USAGE_STORAGE_RW, zero.data(), 4);
    srhiResourceBinding b = {};
    b.binding = 0;
    b.buffer = pixels;
    srhiResourceSetDesc sd = {layout, &b, 1, nullptr};
    srhiResourceSet* set = gpu.Own(srhiResourceSetCreate(gpu.Device(), &sd));
    srhiBuffer* vb = Vertices(gpu, Quad(-0.5f, -0.5f, 0.5f, 0.5f, 0, 1, 0.5f, 0.25f));
    gpu.Run([&](srhiCommandBuffer* cmd) {
        srhiRenderPassDesc pass = ClearPass(target, 0, 0, 0, 1);
        srhiRenderEncoder* enc = srhiRenderEncoderBegin(cmd, &pass);
        srhiRenderEncoderSetResourceSet(enc, 0, set);
        Draw(enc, pipeline, vb, 0, 6, Params(Identity4()));
        srhiRenderEncoderEnd(enc);
    });
    std::vector<uint8_t> bytes = gpu.ReadBuffer(pixels);
    std::vector<uint32_t> words(bytes.size() / 4);
    memcpy(words.data(), bytes.data(), bytes.size());
    uint32_t expected = 255u | (128u << 8) | (64u << 16) | (255u << 24);
    SRHI_EXPECT_EQ(words[32 * 64 + 32], expected);
    SRHI_EXPECT_EQ(words[2 * 64 + 2], 0u);
    ExpectBufferMatchesGolden(ctx, "FragmentStorageWrite.bin", bytes);
}
