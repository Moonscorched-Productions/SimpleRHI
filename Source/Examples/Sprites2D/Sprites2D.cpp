/**
 * @ Author: Amélie Heinrich
 * @ Create Time: 2026-11-13 00:00:00
 * @ Copyright: Copyright (c) 2026 Amélie Heinrich. All rights reserved.
 */

// 2D sprite batching: tens of thousands of animated sprites in a single instanced draw.
// Sprite data is rewritten every frame with srhiBufferWrite (queue-ordered, so one buffer is enough),
// read in the vertex shader from a StructuredBuffer, and textured from a mipmapped procedural atlas.

#include "../Common/App.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

class Sprites2D : public App
{
    struct Sprite
    {
        float position[2];
        float size[2];
        float color[4];
        float rotation;
        uint32_t frame;
        float pad[2];
    };
    static_assert(sizeof(Sprite) == 48, "matches the shader");

    struct Motion
    {
        float vx, vy, spin;
    };

    static constexpr uint32_t kMaxSprites = 200000;

    srhiShaderModule* mModule = nullptr;
    srhiResourceSetLayout* mLayout = nullptr;
    srhiRenderPipeline* mPipeline = nullptr;
    srhiBuffer* mSpriteBuffer = nullptr;
    srhiTexture* mAtlas = nullptr;
    srhiSampler* mSampler = nullptr;
    srhiResourceSet* mSet = nullptr;

    std::vector<Sprite> mSprites;
    std::vector<Motion> mMotion;
    int mCount = 20000;
    bool mAdditive = false;
    float mGravity = 400.0f;
    std::mt19937 mRng{1234};

    static void Shape(std::vector<uint8_t>& pixels, uint32_t size, uint32_t cell, int kind)
    {
        uint32_t half = size / 2, ox = (cell % 2) * half, oy = (cell / 2) * half;
        for (uint32_t y = 0; y < half; ++y)
            for (uint32_t x = 0; x < half; ++x) {
                float u = (x + 0.5f) / half * 2.0f - 1.0f, v = (y + 0.5f) / half * 2.0f - 1.0f;
                float r = std::sqrt(u * u + v * v), a = 0.0f, shade = 1.0f;
                float aa = 2.0f / half;
                switch (kind) {
                case 0: // soft ball
                    a = std::clamp((0.9f - r) / aa, 0.0f, 1.0f);
                    shade = 1.0f - 0.5f * std::sqrt(std::max(0.0f, (u + 0.4f) * (u + 0.4f) + (v + 0.4f) * (v + 0.4f)));
                    break;
                case 1: { // star
                    float angle = std::atan2(v, u);
                    float radius = 0.55f + 0.35f * std::cos(5.0f * angle);
                    a = std::clamp((radius - r) / aa, 0.0f, 1.0f);
                    break;
                }
                case 2: // ring
                    a = std::clamp((0.12f - std::fabs(r - 0.7f)) / aa, 0.0f, 1.0f);
                    break;
                default: // rounded square
                    a = std::clamp((0.8f - std::max(std::fabs(u), std::fabs(v))) / aa, 0.0f, 1.0f);
                    shade = 0.75f + 0.25f * (1.0f - v);
                    break;
                }
                uint8_t* p = &pixels[(size_t(oy + y) * size + ox + x) * 4];
                uint8_t c = uint8_t(std::clamp(shade, 0.0f, 1.0f) * 255.0f);
                p[0] = p[1] = p[2] = c;
                p[3] = uint8_t(a * 255.0f);
            }
    }

    void Spawn(size_t i)
    {
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        Sprite& s = mSprites[i];
        float size = 12.0f + unit(mRng) * 36.0f;
        s.position[0] = unit(mRng) * float(mWidth);
        s.position[1] = unit(mRng) * float(mHeight) * 0.5f;
        s.size[0] = s.size[1] = size;
        float hue = unit(mRng) * 6.0f;
        float rgb[3] = {std::clamp(std::fabs(hue - 3.0f) - 1.0f, 0.0f, 1.0f), std::clamp(2.0f - std::fabs(hue - 2.0f), 0.0f, 1.0f),
                        std::clamp(2.0f - std::fabs(hue - 4.0f), 0.0f, 1.0f)};
        for (int c = 0; c < 3; ++c)
            s.color[c] = 0.15f + 0.85f * rgb[c] * rgb[c];
        s.color[3] = 0.85f;
        s.rotation = unit(mRng) * 6.28f;
        s.frame = uint32_t(unit(mRng) * 4.0f) % 4;
        mMotion[i] = {(unit(mRng) - 0.5f) * 600.0f, (unit(mRng) - 0.5f) * 200.0f, (unit(mRng) - 0.5f) * 4.0f};
    }

    bool CreatePipeline()
    {
        srhiRenderPipelineDestroy(mPipeline);
        srhiRenderPipelineDesc desc = {};
        desc.module = mModule;
        desc.vertexEntry = "vsSprite";
        desc.fragmentEntry = "fsSprite";
        desc.colorTargetCount = 1;
        desc.colorTargets[0].format = BackbufferFormat();
        desc.colorTargets[0].blend = {1,
                                      SRHI_BLEND_FACTOR_ONE,
                                      mAdditive ? SRHI_BLEND_FACTOR_ONE : SRHI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                      SRHI_BLEND_OP_ADD,
                                      SRHI_BLEND_FACTOR_ONE,
                                      SRHI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                      SRHI_BLEND_OP_ADD};
        desc.setLayouts[0] = mLayout;
        desc.label = "sprites";
        mPipeline = srhiRenderPipelineCreate(mDevice, &desc);
        return mPipeline != nullptr;
    }

    bool OnInit() override
    {
        mCameraEnabled = false;
        mModule = LoadShader("Sprites2D");
        if (!mModule)
            return false;
        mLayout = LayoutFromShader(mModule, 0);

        srhiBufferDesc bd = {kMaxSprites * sizeof(Sprite), SRHI_BUFFER_USAGE_STORAGE, sizeof(Sprite), "sprites"};
        mSpriteBuffer = srhiBufferCreate(mDevice, &bd);

        const uint32_t atlasSize = 256;
        std::vector<uint8_t> pixels(atlasSize * atlasSize * 4, 0);
        for (int i = 0; i < 4; ++i)
            Shape(pixels, atlasSize, uint32_t(i), i);
        srhiTextureDesc td = {};
        td.format = SRHI_FORMAT_RGBA8_UNORM_SRGB;
        td.width = td.height = atlasSize;
        td.mipLevels = SRHI_ALL_MIPS;
        td.usage = SRHI_TEXTURE_USAGE_SAMPLED | SRHI_TEXTURE_USAGE_RENDER_TARGET;
        td.label = "sprite atlas";
        mAtlas = srhiTextureCreate(mDevice, &td);
        srhiTextureWrite(mDevice, mAtlas, 0, 0, nullptr, pixels.data(), 0);
        srhiCommandBuffer* cmd = srhiCommandBufferBegin(mDevice, "atlas mips");
        srhiBlitEncoder* blit = srhiBlitEncoderBegin(cmd, "mips");
        srhiBlitEncoderGenerateMipmaps(blit, mAtlas);
        srhiBlitEncoderEnd(blit);
        srhiCommandBufferCommit(cmd, nullptr, nullptr);

        srhiSamplerDesc sd = {};
        sd.minFilter = sd.magFilter = sd.mipFilter = SRHI_FILTER_LINEAR;
        sd.addressU = sd.addressV = SRHI_ADDRESS_MODE_CLAMP_TO_EDGE;
        mSampler = srhiSamplerCreate(mDevice, &sd);

        srhiResourceBinding bindings[3] = {};
        bindings[0].binding = 0;
        bindings[0].buffer = mSpriteBuffer;
        bindings[1].binding = 1;
        bindings[1].textureView = srhiTextureGetDefaultView(mAtlas);
        bindings[2].binding = 2;
        bindings[2].sampler = mSampler;
        srhiResourceSetDesc setDesc = {mLayout, bindings, 3, "sprites"};
        mSet = srhiResourceSetCreate(mDevice, &setDesc);

        mSprites.resize(kMaxSprites);
        mMotion.resize(kMaxSprites);
        for (size_t i = 0; i < kMaxSprites; ++i)
            Spawn(i);
        return mSet && CreatePipeline();
    }

    void OnOutputChanged(srhiFormat) override { CreatePipeline(); }

    void OnShutdown() override
    {
        srhiResourceSetDestroy(mSet);
        srhiSamplerDestroy(mSampler);
        srhiTextureDestroy(mAtlas);
        srhiBufferDestroy(mSpriteBuffer);
        srhiRenderPipelineDestroy(mPipeline);
        srhiResourceSetLayoutDestroy(mLayout);
    }

    void OnUpdate(float dt) override
    {
        dt = std::min(dt, 1.0f / 30.0f);
        const float w = float(mWidth), h = float(mHeight);
        for (int i = 0; i < mCount; ++i) {
            Sprite& s = mSprites[size_t(i)];
            Motion& m = mMotion[size_t(i)];
            m.vy += mGravity * dt;
            s.position[0] += m.vx * dt;
            s.position[1] += m.vy * dt;
            s.rotation += m.spin * dt;
            float r = s.size[0] * 0.5f;
            if (s.position[0] < r || s.position[0] > w - r) {
                m.vx = -m.vx;
                s.position[0] = std::clamp(s.position[0], r, w - r);
            }
            if (s.position[1] > h - r) {
                m.vy = -std::fabs(m.vy) * 0.92f;
                if (std::fabs(m.vy) < 60.0f)
                    m.vy = -(400.0f + float(i % 7) * 60.0f);
                s.position[1] = h - r;
            }
        }
    }

    void OnImGui() override
    {
        ImGui::SetNextWindowPos(ImVec2(10, 340), ImGuiCond_FirstUseEver);
        ImGui::Begin("Sprites");
        ImGui::SliderInt("Count", &mCount, 1, int(kMaxSprites));
        ImGui::SliderFloat("Gravity", &mGravity, 0.0f, 2000.0f);
        if (ImGui::Checkbox("Additive", &mAdditive))
            CreatePipeline();
        ImGui::Text("1 draw call, %d instances", mCount);
        ImGui::End();
    }

    void OnRender(srhiCommandBuffer* cmd, srhiTexture* backbuffer) override
    {
        srhiBufferWrite(mDevice, mSpriteBuffer, 0, mSprites.data(), uint64_t(mCount) * sizeof(Sprite));

        srhiRenderPassDesc pass = {};
        pass.colorAttachmentCount = 1;
        pass.colorAttachments[0].view = srhiTextureGetDefaultView(backbuffer);
        pass.colorAttachments[0].loadAction = SRHI_LOAD_ACTION_CLEAR;
        pass.colorAttachments[0].storeAction = SRHI_STORE_ACTION_STORE;
        pass.colorAttachments[0].clearColor[0] = 0.01f;
        pass.colorAttachments[0].clearColor[1] = 0.012f;
        pass.colorAttachments[0].clearColor[2] = 0.02f;
        pass.colorAttachments[0].clearColor[3] = 1.0f;
        pass.timestampWrites = mProfiler.Scope("Sprites");
        pass.label = "sprites";

        struct
        {
            float viewport[4];
            OutputParams output;
        } params = {{2.0f / float(mWidth), -2.0f / float(mHeight), 0, 0}, GetOutputParams()};

        srhiRenderEncoder* enc = srhiRenderEncoderBegin(cmd, &pass);
        srhiRenderEncoderSetPipeline(enc, mPipeline);
        srhiRenderEncoderSetResourceSet(enc, 0, mSet);
        srhiRenderEncoderSetUniforms(enc, 0, &params, sizeof(params));
        srhiRenderEncoderDraw(enc, 6, uint32_t(mCount), 0, 0);
        srhiRenderEncoderEnd(enc);
    }
};

SRHI_EXAMPLE_MAIN(Sprites2D, "Sprites 2D")
