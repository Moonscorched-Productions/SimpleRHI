/**
 * @ Author: Amélie Heinrich
 * @ Create Time: 2026-11-13 00:00:00
 * @ Copyright: Copyright (c) 2026 Amélie Heinrich. All rights reserved.
 */

// Deferred shading of Intel Sponza with up to 1024 animated point lights, and the timestamp-query
// showcase: every pass is wrapped in a GPU profiler scope.
//
//   G-buffer (render, MRT)  ->  Sun + ambient (compute, writes an RGBA16F storage texture)
//   ->  Point lights (render, one instanced screen-space quad per light, additive)  ->  Composite (render)

#include "../Common/App.h"
#include "../Common/Model.h"

#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <random>
#include <vector>

class Deferred : public App
{
    static constexpr uint32_t kMaxLights = 1024;

    struct Frame
    {
        glm::mat4 viewProj;
        glm::mat4 invViewProj;
        glm::mat4 view;
        glm::vec4 cameraPos;
        glm::vec4 sunDir;
        glm::vec4 sunColor;
        glm::vec4 screen;
        glm::vec4 settings;
        OutputParams output;
    };
    static_assert(sizeof(Frame) == 288, "matches the shader");

    struct PointLight
    {
        glm::vec4 positionRadius;
        glm::vec4 color;
    };

    struct LightAnim
    {
        glm::vec3 center;
        float orbitRadius, speed, phase, height;
    };

    srhiShaderModule* mModule = nullptr;
    srhiResourceSetLayout* mMaterialLayout = nullptr;
    srhiResourceSetLayout* mSunLayout = nullptr;
    srhiResourceSetLayout* mLightLayout = nullptr;
    srhiResourceSetLayout* mCompositeLayout = nullptr;
    srhiRenderPipeline* mGBufferPipeline = nullptr;
    srhiComputePipeline* mSunPipeline = nullptr;
    srhiRenderPipeline* mLightPipeline = nullptr;
    srhiRenderPipeline* mCompositePipeline = nullptr;

    srhiTexture* mAlbedo = nullptr;
    srhiTexture* mNormal = nullptr;
    srhiTexture* mSurface = nullptr;
    srhiTexture* mDepth = nullptr;
    srhiTexture* mLit = nullptr;
    srhiResourceSet* mSunSet = nullptr;
    srhiResourceSet* mLightSet = nullptr;
    srhiResourceSet* mCompositeSet = nullptr;
    srhiBuffer* mLightBuffer = nullptr;
    Model mSponza;

    std::vector<PointLight> mLights;
    std::vector<LightAnim> mAnim;
    int mLightCount = 512;
    float mLightRadius = 3.0f;
    float mLightIntensity = 6.0f;
    float mSunIntensity = 1.5f, mAmbient = 0.06f;
    bool mAnimate = true;
    int mDebugView = 0;
    float mLightTime = 0.0f;

    srhiResourceSetLayout* LayoutSubset(std::initializer_list<uint32_t> bindings)
    {
        uint32_t count = srhiShaderModuleGetResourceSetLayout(mModule, 0, nullptr, 0);
        std::vector<srhiResourceSetLayoutEntry> all(count), picked;
        srhiShaderModuleGetResourceSetLayout(mModule, 0, all.data(), count);
        for (const srhiResourceSetLayoutEntry& e : all)
            for (uint32_t b : bindings)
                if (e.binding == b)
                    picked.push_back(e);
        srhiResourceSetLayoutDesc desc = {picked.data(), uint32_t(picked.size()), "deferred"};
        return srhiResourceSetLayoutCreate(mDevice, &desc);
    }

    srhiResourceBinding View(uint32_t binding, srhiTexture* t)
    {
        srhiResourceBinding b = {};
        b.binding = binding;
        b.textureView = srhiTextureGetDefaultView(t);
        return b;
    }

    srhiTexture* Target(srhiFormat format, uint32_t usage, const char* label)
    {
        srhiTextureDesc td = {};
        td.format = format;
        td.width = mWidth;
        td.height = mHeight;
        td.usage = usage;
        td.label = label;
        return srhiTextureCreate(mDevice, &td);
    }

    void DestroyTargets()
    {
        srhiResourceSetDestroy(mSunSet);
        srhiResourceSetDestroy(mLightSet);
        srhiResourceSetDestroy(mCompositeSet);
        for (srhiTexture* t : {mAlbedo, mNormal, mSurface, mDepth, mLit})
            srhiTextureDestroy(t);
        mSunSet = mLightSet = mCompositeSet = nullptr;
        mAlbedo = mNormal = mSurface = mDepth = mLit = nullptr;
    }

    bool CreateTargets()
    {
        DestroyTargets();
        const uint32_t rt = SRHI_TEXTURE_USAGE_RENDER_TARGET | SRHI_TEXTURE_USAGE_SAMPLED;
        mAlbedo = Target(SRHI_FORMAT_RGBA8_UNORM_SRGB, rt, "gbuffer albedo");
        mNormal = Target(SRHI_FORMAT_RGBA16_FLOAT, rt, "gbuffer normal");
        mSurface = Target(SRHI_FORMAT_RGBA8_UNORM, rt, "gbuffer surface");
        mDepth = Target(SRHI_FORMAT_D32_FLOAT, rt, "gbuffer depth");
        mLit = Target(SRHI_FORMAT_RGBA16_FLOAT, rt | SRHI_TEXTURE_USAGE_STORAGE, "lit HDR");

        srhiResourceBinding sun[5] = {View(0, mAlbedo), View(1, mNormal), View(2, mSurface), View(3, mDepth), View(4, mLit)};
        srhiResourceSetDesc sunDesc = {mSunLayout, sun, 5, "sun set"};
        mSunSet = srhiResourceSetCreate(mDevice, &sunDesc);

        srhiResourceBinding light[5] = {View(0, mAlbedo), View(1, mNormal), View(2, mSurface), View(3, mDepth), {}};
        light[4].binding = 5;
        light[4].buffer = mLightBuffer;
        srhiResourceSetDesc lightDesc = {mLightLayout, light, 5, "light set"};
        mLightSet = srhiResourceSetCreate(mDevice, &lightDesc);

        srhiResourceBinding comp[5] = {View(0, mAlbedo), View(1, mNormal), View(2, mSurface), View(3, mDepth), View(6, mLit)};
        srhiResourceSetDesc compDesc = {mCompositeLayout, comp, 5, "composite set"};
        mCompositeSet = srhiResourceSetCreate(mDevice, &compDesc);
        return mSunSet && mLightSet && mCompositeSet;
    }

    bool CreateCompositePipeline()
    {
        srhiRenderPipelineDestroy(mCompositePipeline);
        srhiRenderPipelineDesc desc = {};
        desc.module = mModule;
        desc.vertexEntry = "vsFullscreen";
        desc.fragmentEntry = "fsComposite";
        desc.colorTargetCount = 1;
        desc.colorTargets[0].format = BackbufferFormat();
        desc.setLayouts[0] = mCompositeLayout;
        desc.label = "composite";
        mCompositePipeline = srhiRenderPipelineCreate(mDevice, &desc);
        return mCompositePipeline != nullptr;
    }

    bool CreatePipelines()
    {
        srhiRenderPipelineDesc g = {};
        g.module = mModule;
        g.vertexEntry = "vsGBuffer";
        g.fragmentEntry = "fsGBuffer";
        g.vertexLayout = Model::VertexLayout();
        g.colorTargetCount = 3;
        g.colorTargets[0].format = SRHI_FORMAT_RGBA8_UNORM_SRGB;
        g.colorTargets[1].format = SRHI_FORMAT_RGBA16_FLOAT;
        g.colorTargets[2].format = SRHI_FORMAT_RGBA8_UNORM;
        g.depthStencil.format = SRHI_FORMAT_D32_FLOAT;
        g.depthStencil.depthTestEnable = 1;
        g.depthStencil.depthWriteEnable = 1;
        g.depthStencil.depthCompare = SRHI_COMPARE_FUNCTION_LESS;
        g.setLayouts[1] = mMaterialLayout;
        g.label = "gbuffer";
        mGBufferPipeline = srhiRenderPipelineCreate(mDevice, &g);

        srhiComputePipelineDesc c = {};
        c.module = mModule;
        c.entry = "csSun";
        c.setLayouts[0] = mSunLayout;
        c.label = "sun";
        mSunPipeline = srhiComputePipelineCreate(mDevice, &c);

        srhiRenderPipelineDesc l = {};
        l.module = mModule;
        l.vertexEntry = "vsLight";
        l.fragmentEntry = "fsLight";
        l.colorTargetCount = 1;
        l.colorTargets[0].format = SRHI_FORMAT_RGBA16_FLOAT;
        l.colorTargets[0].blend = {1, SRHI_BLEND_FACTOR_ONE, SRHI_BLEND_FACTOR_ONE, SRHI_BLEND_OP_ADD,
                                   SRHI_BLEND_FACTOR_ZERO, SRHI_BLEND_FACTOR_ONE, SRHI_BLEND_OP_ADD};
        l.setLayouts[0] = mLightLayout;
        l.label = "point lights";
        mLightPipeline = srhiRenderPipelineCreate(mDevice, &l);
        return mGBufferPipeline && mSunPipeline && mLightPipeline && CreateCompositePipeline();
    }

    void InitLights()
    {
        std::mt19937 rng(42);
        std::uniform_real_distribution<float> u(0.0f, 1.0f);
        mLights.resize(kMaxLights);
        mAnim.resize(kMaxLights);
        const float* mn = mSponza.BoundsMin();
        const float* mx = mSponza.BoundsMax();
        for (uint32_t i = 0; i < kMaxLights; ++i) {
            LightAnim& a = mAnim[i];
            // Inside the building: trim the outer walls and keep below the upper gallery ceiling.
            a.center = glm::vec3(mn[0] + 2.0f + u(rng) * (mx[0] - mn[0] - 4.0f), 0.0f, mn[2] + 2.5f + u(rng) * (mx[2] - mn[2] - 5.0f));
            a.height = 0.3f + u(rng) * 7.5f;
            a.orbitRadius = 0.5f + u(rng) * 2.0f;
            a.speed = (u(rng) - 0.5f) * 1.5f;
            a.phase = u(rng) * 6.28f;
            float hue = u(rng) * 6.0f;
            glm::vec3 rgb(std::clamp(std::fabs(hue - 3.0f) - 1.0f, 0.0f, 1.0f), std::clamp(2.0f - std::fabs(hue - 2.0f), 0.0f, 1.0f),
                          std::clamp(2.0f - std::fabs(hue - 4.0f), 0.0f, 1.0f));
            mLights[i].color = glm::vec4(glm::mix(glm::vec3(1.0f), rgb, 0.8f), 1.0f);
        }
    }

    bool OnInit() override
    {
        mModule = LoadShader("Deferred");
        if (!mModule)
            return false;
        mMaterialLayout = Model::CreateMaterialLayout(mDevice);
        mSunLayout = LayoutSubset({0, 1, 2, 3, 4});
        mLightLayout = LayoutSubset({0, 1, 2, 3, 5});
        mCompositeLayout = LayoutSubset({0, 1, 2, 3, 6});
        if (!mSponza.Load(mDevice, "IntelSponza", mMaterialLayout))
            return false;

        srhiBufferDesc lb = {kMaxLights * sizeof(PointLight), SRHI_BUFFER_USAGE_STORAGE, sizeof(PointLight), "point lights"};
        mLightBuffer = srhiBufferCreate(mDevice, &lb);
        InitLights();

        mCamera.position = glm::vec3(9.0f, 2.5f, 0.3f);
        mCamera.yaw = -1.5708f;
        mCamera.pitch = 0.0f;
        mCamera.farZ = 200.0f;
        return mLightBuffer && CreatePipelines();
    }

    void OnResize(uint32_t, uint32_t) override { CreateTargets(); }
    void OnOutputChanged(srhiFormat) override { CreateCompositePipeline(); }

    void OnShutdown() override
    {
        DestroyTargets();
        mSponza.Destroy();
        srhiBufferDestroy(mLightBuffer);
        srhiRenderPipelineDestroy(mGBufferPipeline);
        srhiComputePipelineDestroy(mSunPipeline);
        srhiRenderPipelineDestroy(mLightPipeline);
        srhiRenderPipelineDestroy(mCompositePipeline);
        for (srhiResourceSetLayout* l : {mMaterialLayout, mSunLayout, mLightLayout, mCompositeLayout})
            srhiResourceSetLayoutDestroy(l);
    }

    void OnUpdate(float dt) override
    {
        if (mAnimate)
            mLightTime += dt;
        for (int i = 0; i < mLightCount; ++i) {
            const LightAnim& a = mAnim[size_t(i)];
            float t = mLightTime * a.speed + a.phase;
            glm::vec3 p = a.center + glm::vec3(std::cos(t) * a.orbitRadius, a.height + 0.3f * std::sin(t * 2.3f), std::sin(t) * a.orbitRadius);
            mLights[size_t(i)].positionRadius = glm::vec4(p, mLightRadius);
        }
    }

    void OnImGui() override
    {
        ImGui::SetNextWindowPos(ImVec2(10, 340), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(330, 0), ImGuiCond_FirstUseEver);
        ImGui::Begin("Deferred");
        ImGui::Text("Sponza: %u triangles, %u draws", mSponza.TriangleCount(), mSponza.SubmeshCount());
        ImGui::SliderInt("Point lights", &mLightCount, 0, int(kMaxLights));
        ImGui::SliderFloat("Light radius", &mLightRadius, 0.5f, 10.0f);
        ImGui::SliderFloat("Light intensity", &mLightIntensity, 0.0f, 40.0f);
        ImGui::Checkbox("Animate lights", &mAnimate);
        ImGui::SliderFloat("Sun", &mSunIntensity, 0.0f, 10.0f);
        ImGui::SliderFloat("Ambient", &mAmbient, 0.0f, 1.0f);
        ImGui::Combo("View", &mDebugView, "Lit\0Albedo\0Normals\0Metal / rough\0Depth\0");
        ImGui::TextDisabled("GPU timings per pass are in the SimpleRHI window.");
        ImGui::End();
    }

    void OnRender(srhiCommandBuffer* cmd, srhiTexture* backbuffer) override
    {
        if (mLightCount > 0)
            srhiBufferWrite(mDevice, mLightBuffer, 0, mLights.data(), uint64_t(mLightCount) * sizeof(PointLight));

        Frame frame = {};
        frame.view = mCamera.GetView();
        frame.viewProj = mCamera.GetProj() * frame.view;
        frame.invViewProj = glm::inverse(frame.viewProj);
        frame.cameraPos = glm::vec4(mCamera.position, 1.0f);
        frame.sunDir = glm::vec4(glm::normalize(glm::vec3(0.35f, 1.0f, 0.25f)), mSunIntensity);
        frame.sunColor = glm::vec4(1.0f, 0.92f, 0.8f, mAmbient);
        frame.screen = glm::vec4(float(mWidth), float(mHeight), 1.0f / float(mWidth), 1.0f / float(mHeight));
        frame.settings = glm::vec4(float(mDebugView), mLightIntensity, mCamera.nearZ, mCamera.farZ);
        frame.output = GetOutputParams();

        // 1. G-buffer
        {
            srhiRenderPassDesc pass = {};
            pass.colorAttachmentCount = 3;
            srhiTexture* targets[3] = {mAlbedo, mNormal, mSurface};
            for (uint32_t i = 0; i < 3; ++i) {
                pass.colorAttachments[i].view = srhiTextureGetDefaultView(targets[i]);
                pass.colorAttachments[i].loadAction = SRHI_LOAD_ACTION_CLEAR;
                pass.colorAttachments[i].storeAction = SRHI_STORE_ACTION_STORE;
            }
            pass.depthAttachment.view = srhiTextureGetDefaultView(mDepth);
            pass.depthAttachment.depthLoadAction = SRHI_LOAD_ACTION_CLEAR;
            pass.depthAttachment.depthStoreAction = SRHI_STORE_ACTION_STORE;
            pass.depthAttachment.clearDepth = 1.0f;
            pass.timestampWrites = mProfiler.Scope("G-buffer");
            pass.label = "gbuffer";
            srhiRenderEncoder* enc = srhiRenderEncoderBegin(cmd, &pass);
            srhiRenderEncoderSetPipeline(enc, mGBufferPipeline);
            srhiRenderEncoderSetUniforms(enc, 0, &frame, sizeof(frame));
            mSponza.Draw(enc, Model::kAll, true);
            srhiRenderEncoderEnd(enc);
        }

        // 2. Sun + ambient
        {
            srhiComputePassDesc pass = {};
            pass.timestampWrites = mProfiler.Scope("Sun + ambient (compute)");
            pass.label = "sun";
            srhiComputeEncoder* enc = srhiComputeEncoderBegin(cmd, &pass);
            srhiComputeEncoderSetPipeline(enc, mSunPipeline);
            srhiComputeEncoderSetResourceSet(enc, 0, mSunSet);
            srhiComputeEncoderSetUniforms(enc, 0, &frame, sizeof(frame));
            srhiComputeEncoderDispatch(enc, (mWidth + 7) / 8, (mHeight + 7) / 8, 1);
            srhiComputeEncoderEnd(enc);
        }

        // 3. Point lights
        {
            srhiRenderPassDesc pass = {};
            pass.colorAttachmentCount = 1;
            pass.colorAttachments[0].view = srhiTextureGetDefaultView(mLit);
            pass.colorAttachments[0].loadAction = SRHI_LOAD_ACTION_LOAD;
            pass.colorAttachments[0].storeAction = SRHI_STORE_ACTION_STORE;
            pass.timestampWrites = mProfiler.Scope("Point lights");
            pass.label = "point lights";
            srhiRenderEncoder* enc = srhiRenderEncoderBegin(cmd, &pass);
            if (mLightCount > 0) {
                srhiRenderEncoderSetPipeline(enc, mLightPipeline);
                srhiRenderEncoderSetResourceSet(enc, 0, mLightSet);
                srhiRenderEncoderSetUniforms(enc, 0, &frame, sizeof(frame));
                srhiRenderEncoderDraw(enc, 6, uint32_t(mLightCount), 0, 0);
            }
            srhiRenderEncoderEnd(enc);
        }

        // 4. Composite
        {
            srhiRenderPassDesc pass = {};
            pass.colorAttachmentCount = 1;
            pass.colorAttachments[0].view = srhiTextureGetDefaultView(backbuffer);
            pass.colorAttachments[0].loadAction = SRHI_LOAD_ACTION_DONT_CARE;
            pass.colorAttachments[0].storeAction = SRHI_STORE_ACTION_STORE;
            pass.timestampWrites = mProfiler.Scope("Composite");
            pass.label = "composite";
            srhiRenderEncoder* enc = srhiRenderEncoderBegin(cmd, &pass);
            srhiRenderEncoderSetPipeline(enc, mCompositePipeline);
            srhiRenderEncoderSetResourceSet(enc, 0, mCompositeSet);
            srhiRenderEncoderSetUniforms(enc, 0, &frame, sizeof(frame));
            srhiRenderEncoderDraw(enc, 3, 1, 0, 0);
            srhiRenderEncoderEnd(enc);
        }
    }
};

SRHI_EXAMPLE_MAIN(Deferred, "Deferred Shading")
