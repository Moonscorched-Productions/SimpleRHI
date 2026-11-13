/**
 * @ Author: Amélie Heinrich
 * @ Create Time: 2026-11-13 00:00:00
 * @ Copyright: Copyright (c) 2026 Amélie Heinrich. All rights reserved.
 */

// HDR bloom through a mip chain.
//
// The scene (the emissive DamagedHelmet under a bright HDRI sky) renders into RGBA16F. The bloom texture is
// a half-resolution RGBA16F with a mip chain; each mip is reached through a single-mip texture view, used
// as a render target in one pass and as a sampled input in the next:
//   down:  scene -> mip 0 (soft threshold + Karis average), mip i-1 -> mip i (13-tap filter)
//   up:    mip i -> mip i-1 (3x3 tent), blended additively on top of the downsampled content
//   final: scene + bloom mip 0 * intensity, tonemapped by EncodeOutput for SDR / scRGB / HDR10.

#include "../Common/App.h"
#include "../Common/Model.h"

#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <string>
#include <vector>

class Bloom : public App
{
    static constexpr uint32_t kMaxMips = 7;

    struct Params
    {
        glm::mat4 viewProj;
        glm::mat4 invViewProj;
        glm::mat4 model;
        glm::vec4 cameraPos;
        glm::vec4 scene;
        glm::vec4 bloom;
        glm::vec4 pass;
        OutputParams output;
    };
    static_assert(sizeof(Params) == 272, "matches the shader");

    srhiShaderModule* mShader = nullptr;
    srhiResourceSetLayout* mMaterialLayout = nullptr;
    srhiResourceSetLayout* mSkyLayout = nullptr;
    srhiResourceSetLayout* mFilterLayout = nullptr;
    srhiResourceSetLayout* mCompositeLayout = nullptr;
    srhiRenderPipeline* mModelPipeline = nullptr;
    srhiRenderPipeline* mSkyPipeline = nullptr;
    srhiRenderPipeline* mDownPipeline = nullptr;
    srhiRenderPipeline* mUpPipeline = nullptr;
    srhiRenderPipeline* mCompositePipeline = nullptr;
    srhiSampler* mClamp = nullptr;
    srhiSampler* mRepeat = nullptr;
    srhiTexture* mSkyTexture = nullptr;
    srhiResourceSet* mSkySet = nullptr;
    Model mHelmet;

    // Size-dependent resources.
    srhiTexture* mScene = nullptr;
    srhiTexture* mDepth = nullptr;
    srhiTexture* mBloom = nullptr;
    uint32_t mMipCount = 0;
    srhiTextureView* mMipViews[kMaxMips] = {};
    srhiResourceSet* mDownSets[kMaxMips] = {};   // input of the pass writing mip i
    srhiResourceSet* mUpSets[kMaxMips] = {};     // samples mip i (written into mip i-1)
    srhiResourceSet* mCompositeSets[kMaxMips] = {}; // scene + bloom mip i

    float mExposure = 1.0f, mEmissiveBoost = 8.0f, mSunIntensity = 2.0f, mSkyIntensity = 1.0f;
    float mThreshold = 1.0f, mKnee = 0.5f, mIntensity = 0.15f, mRadius = 1.0f;
    int mShowMip = -1;
    float mAngle = 0.0f;

    srhiResourceSetLayout* SubsetLayout(uint32_t set, std::initializer_list<uint32_t> bindings)
    {
        uint32_t count = srhiShaderModuleGetResourceSetLayout(mShader, set, nullptr, 0);
        std::vector<srhiResourceSetLayoutEntry> all(count), picked;
        srhiShaderModuleGetResourceSetLayout(mShader, set, all.data(), count);
        for (const srhiResourceSetLayoutEntry& e : all)
            for (uint32_t b : bindings)
                if (e.binding == b)
                    picked.push_back(e);
        srhiResourceSetLayoutDesc desc = {picked.data(), uint32_t(picked.size()), nullptr};
        return srhiResourceSetLayoutCreate(mDevice, &desc);
    }

    srhiRenderPipeline* Fullscreen(const char* fs, srhiResourceSetLayout* layout, srhiFormat format, bool additive, const char* label)
    {
        srhiRenderPipelineDesc d = {};
        d.module = mShader;
        d.vertexEntry = "vsFullscreen";
        d.fragmentEntry = fs;
        d.colorTargetCount = 1;
        d.colorTargets[0].format = format;
        if (additive)
            d.colorTargets[0].blend = {1, SRHI_BLEND_FACTOR_ONE, SRHI_BLEND_FACTOR_ONE, SRHI_BLEND_OP_ADD,
                                       SRHI_BLEND_FACTOR_ONE, SRHI_BLEND_FACTOR_ONE, SRHI_BLEND_OP_ADD};
        d.setLayouts[0] = layout;
        d.label = label;
        return srhiRenderPipelineCreate(mDevice, &d);
    }

    bool CreatePipelines()
    {
        srhiRenderPipelineDestroy(mCompositePipeline);
        mCompositePipeline = Fullscreen("fsComposite", mCompositeLayout, BackbufferFormat(), false, "composite");
        if (mModelPipeline)
            return mCompositePipeline != nullptr;

        srhiRenderPipelineDesc d = {};
        d.module = mShader;
        d.vertexEntry = "vsModel";
        d.fragmentEntry = "fsModel";
        d.vertexLayout = Model::VertexLayout();
        d.raster.cullMode = SRHI_CULL_MODE_BACK;
        d.depthStencil = {SRHI_FORMAT_D32_FLOAT, 1, 1, SRHI_COMPARE_FUNCTION_LESS};
        d.colorTargetCount = 1;
        d.colorTargets[0].format = SRHI_FORMAT_RGBA16_FLOAT;
        d.setLayouts[1] = mMaterialLayout;
        d.label = "scene";
        mModelPipeline = srhiRenderPipelineCreate(mDevice, &d);

        srhiRenderPipelineDesc sky = {};
        sky.module = mShader;
        sky.vertexEntry = "vsFullscreen";
        sky.fragmentEntry = "fsSky";
        sky.depthStencil = {SRHI_FORMAT_D32_FLOAT, 1, 0, SRHI_COMPARE_FUNCTION_LESS_EQUAL};
        sky.colorTargetCount = 1;
        sky.colorTargets[0].format = SRHI_FORMAT_RGBA16_FLOAT;
        sky.setLayouts[0] = mSkyLayout;
        sky.label = "sky";
        mSkyPipeline = srhiRenderPipelineCreate(mDevice, &sky);

        mDownPipeline = Fullscreen("fsDownsample", mFilterLayout, SRHI_FORMAT_RGBA16_FLOAT, false, "bloom down");
        mUpPipeline = Fullscreen("fsUpsample", mFilterLayout, SRHI_FORMAT_RGBA16_FLOAT, true, "bloom up");
        return mModelPipeline && mSkyPipeline && mDownPipeline && mUpPipeline && mCompositePipeline;
    }

    void DestroySized()
    {
        for (uint32_t i = 0; i < kMaxMips; ++i) {
            srhiResourceSetDestroy(mDownSets[i]);
            srhiResourceSetDestroy(mUpSets[i]);
            srhiResourceSetDestroy(mCompositeSets[i]);
            srhiTextureViewDestroy(mMipViews[i]);
            mDownSets[i] = mUpSets[i] = mCompositeSets[i] = nullptr;
            mMipViews[i] = nullptr;
        }
        srhiTextureDestroy(mScene);
        srhiTextureDestroy(mDepth);
        srhiTextureDestroy(mBloom);
        mScene = mDepth = mBloom = nullptr;
    }

    srhiResourceSet* FilterSet(srhiTextureView* source)
    {
        srhiResourceBinding b[2] = {};
        b[0] = {0, nullptr, 0, 0, source};
        b[1].binding = 1;
        b[1].sampler = mClamp;
        srhiResourceSetDesc sd = {mFilterLayout, b, 2, "bloom filter"};
        return srhiResourceSetCreate(mDevice, &sd);
    }

    void OnResize(uint32_t width, uint32_t height) override
    {
        DestroySized();
        srhiTextureDesc td = {};
        td.format = SRHI_FORMAT_RGBA16_FLOAT;
        td.width = width;
        td.height = height;
        td.usage = SRHI_TEXTURE_USAGE_RENDER_TARGET | SRHI_TEXTURE_USAGE_SAMPLED;
        td.label = "HDR scene";
        mScene = srhiTextureCreate(mDevice, &td);
        td.format = SRHI_FORMAT_D32_FLOAT;
        td.usage = SRHI_TEXTURE_USAGE_RENDER_TARGET;
        td.label = "depth";
        mDepth = srhiTextureCreate(mDevice, &td);

        uint32_t bw = std::max(width / 2, 1u), bh = std::max(height / 2, 1u);
        mMipCount = 1;
        while (mMipCount < kMaxMips && (bw >> mMipCount) >= 8 && (bh >> mMipCount) >= 8)
            ++mMipCount;
        td.format = SRHI_FORMAT_RGBA16_FLOAT;
        td.width = bw;
        td.height = bh;
        td.mipLevels = mMipCount;
        td.usage = SRHI_TEXTURE_USAGE_RENDER_TARGET | SRHI_TEXTURE_USAGE_SAMPLED;
        td.label = "bloom chain";
        mBloom = srhiTextureCreate(mDevice, &td);

        for (uint32_t i = 0; i < mMipCount; ++i) {
            srhiTextureViewDesc vd = {};
            vd.baseMip = i;
            vd.mipCount = 1;
            vd.label = "bloom mip";
            mMipViews[i] = srhiTextureViewCreate(mBloom, &vd);
        }
        mDownSets[0] = FilterSet(srhiTextureGetDefaultView(mScene));
        for (uint32_t i = 1; i < mMipCount; ++i) {
            mDownSets[i] = FilterSet(mMipViews[i - 1]);
            mUpSets[i] = FilterSet(mMipViews[i]);
        }
        for (uint32_t i = 0; i < mMipCount; ++i) {
            srhiResourceBinding b[3] = {};
            b[0].binding = 1;
            b[0].sampler = mClamp;
            b[1] = {2, nullptr, 0, 0, srhiTextureGetDefaultView(mScene)};
            b[2] = {3, nullptr, 0, 0, mMipViews[i]};
            srhiResourceSetDesc sd = {mCompositeLayout, b, 3, "composite"};
            mCompositeSets[i] = srhiResourceSetCreate(mDevice, &sd);
        }
        mShowMip = std::min(mShowMip, int(mMipCount) - 1);
    }

    bool OnInit() override
    {
        mShader = LoadShader("Bloom");
        if (!mShader)
            return false;
        mMaterialLayout = Model::CreateMaterialLayout(mDevice);
        mSkyLayout = SubsetLayout(0, {4, 5});
        mFilterLayout = SubsetLayout(0, {0, 1});
        mCompositeLayout = SubsetLayout(0, {1, 2, 3});

        srhiSamplerDesc sd = {};
        sd.minFilter = sd.magFilter = sd.mipFilter = SRHI_FILTER_LINEAR;
        sd.addressU = sd.addressV = sd.addressW = SRHI_ADDRESS_MODE_CLAMP_TO_EDGE;
        mClamp = srhiSamplerCreate(mDevice, &sd);
        sd.addressU = sd.addressV = sd.addressW = SRHI_ADDRESS_MODE_REPEAT;
        mRepeat = srhiSamplerCreate(mDevice, &sd);

        mSkyTexture = LoadBakedTexture(mDevice, "Skyboxes/lake_pier_4k.bc6h.srt");
        if (!mSkyTexture || !mHelmet.Load(mDevice, "DamagedHelmet", mMaterialLayout))
            return false;
        srhiResourceBinding b[2] = {};
        b[0] = {4, nullptr, 0, 0, srhiTextureGetDefaultView(mSkyTexture)};
        b[1].binding = 5;
        b[1].sampler = mRepeat;
        srhiResourceSetDesc setDesc = {mSkyLayout, b, 2, "sky"};
        mSkySet = srhiResourceSetCreate(mDevice, &setDesc);

        mCamera.position = glm::vec3(0.0f, 0.1f, 2.6f);
        mCamera.yaw = 3.14159265f;
        mCamera.nearZ = 0.01f;
        mCamera.farZ = 100.0f;
        mCamera.moveSpeed = 2.0f;
        return mSkySet && CreatePipelines();
    }

    void OnOutputChanged(srhiFormat) override { CreatePipelines(); }

    void OnShutdown() override
    {
        DestroySized();
        mHelmet.Destroy();
        srhiResourceSetDestroy(mSkySet);
        srhiTextureDestroy(mSkyTexture);
        for (srhiRenderPipeline* p : {mModelPipeline, mSkyPipeline, mDownPipeline, mUpPipeline, mCompositePipeline})
            srhiRenderPipelineDestroy(p);
        for (srhiResourceSetLayout* l : {mMaterialLayout, mSkyLayout, mFilterLayout, mCompositeLayout})
            srhiResourceSetLayoutDestroy(l);
        srhiSamplerDestroy(mClamp);
        srhiSamplerDestroy(mRepeat);
    }

    void OnUpdate(float dt) override { mAngle += dt * 0.3f; }

    void OnImGui() override
    {
        ImGui::SetNextWindowPos(ImVec2(10, 340), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(330, 0), ImGuiCond_FirstUseEver);
        ImGui::Begin("Bloom");
        ImGui::SliderFloat("Exposure", &mExposure, 0.05f, 8.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Emissive boost", &mEmissiveBoost, 0.0f, 50.0f);
        ImGui::SliderFloat("Sky intensity", &mSkyIntensity, 0.0f, 4.0f);
        ImGui::Separator();
        ImGui::SliderFloat("Threshold", &mThreshold, 0.0f, 10.0f);
        ImGui::SliderFloat("Soft knee", &mKnee, 0.0f, 1.0f);
        ImGui::SliderFloat("Intensity", &mIntensity, 0.0f, 1.0f);
        ImGui::SliderFloat("Radius", &mRadius, 0.25f, 3.0f);
        ImGui::SliderInt("Show mip (-1 = final)", &mShowMip, -1, int(mMipCount) - 1);
        ImGui::Text("Bloom chain: %u mips from %ux%u", mMipCount, std::max(mWidth / 2, 1u), std::max(mHeight / 2, 1u));
        ImGui::End();
    }

    void FullscreenPass(srhiCommandBuffer* cmd, srhiTextureView* target, srhiLoadAction load, srhiRenderPipeline* pipeline,
                        srhiResourceSet* set, const Params& params, const char* name)
    {
        srhiRenderPassDesc pass = {};
        pass.colorAttachmentCount = 1;
        pass.colorAttachments[0].view = target;
        pass.colorAttachments[0].loadAction = load;
        pass.colorAttachments[0].storeAction = SRHI_STORE_ACTION_STORE;
        pass.timestampWrites = mProfiler.Scope(name);
        pass.label = name;
        srhiRenderEncoder* enc = srhiRenderEncoderBegin(cmd, &pass);
        srhiRenderEncoderSetPipeline(enc, pipeline);
        srhiRenderEncoderSetResourceSet(enc, 0, set);
        srhiRenderEncoderSetUniforms(enc, 0, &params, sizeof(params));
        srhiRenderEncoderDraw(enc, 3, 1, 0, 0);
        srhiRenderEncoderEnd(enc);
    }

    void OnRender(srhiCommandBuffer* cmd, srhiTexture* backbuffer) override
    {
        Params p;
        p.viewProj = mCamera.GetProj() * mCamera.GetView();
        p.invViewProj = glm::inverse(p.viewProj);
        p.model = glm::rotate(glm::mat4(1.0f), mAngle, glm::vec3(0.0f, 1.0f, 0.0f));
        p.cameraPos = glm::vec4(mCamera.position, 1.0f);
        p.scene = glm::vec4(mExposure, mEmissiveBoost, mSunIntensity, mSkyIntensity);
        p.bloom = glm::vec4(mThreshold, mKnee, mIntensity, mRadius);
        p.pass = glm::vec4(0.0f);
        p.output = GetOutputParams();

        // Scene.
        srhiRenderPassDesc pass = {};
        pass.colorAttachmentCount = 1;
        pass.colorAttachments[0].view = srhiTextureGetDefaultView(mScene);
        pass.colorAttachments[0].loadAction = SRHI_LOAD_ACTION_DONT_CARE;
        pass.colorAttachments[0].storeAction = SRHI_STORE_ACTION_STORE;
        pass.depthAttachment.view = srhiTextureGetDefaultView(mDepth);
        pass.depthAttachment.depthLoadAction = SRHI_LOAD_ACTION_CLEAR;
        pass.depthAttachment.depthStoreAction = SRHI_STORE_ACTION_DONT_CARE;
        pass.depthAttachment.clearDepth = 1.0f;
        pass.timestampWrites = mProfiler.Scope("Scene");
        pass.label = "scene";
        srhiRenderEncoder* enc = srhiRenderEncoderBegin(cmd, &pass);
        srhiRenderEncoderSetUniforms(enc, 0, &p, sizeof(p));
        srhiRenderEncoderSetPipeline(enc, mModelPipeline);
        mHelmet.Draw(enc, Model::kAll);
        srhiRenderEncoderSetPipeline(enc, mSkyPipeline);
        srhiRenderEncoderSetResourceSet(enc, 0, mSkySet);
        srhiRenderEncoderDraw(enc, 3, 1, 0, 0);
        srhiRenderEncoderEnd(enc);

        // Downsample chain. pass.xy is the texel size of the *source*.
        srhiTextureDesc bd;
        srhiTextureGetDesc(mBloom, &bd);
        char name[32];
        for (uint32_t i = 0; i < mMipCount; ++i) {
            uint32_t sw = i == 0 ? mWidth : std::max(bd.width >> (i - 1), 1u);
            uint32_t sh = i == 0 ? mHeight : std::max(bd.height >> (i - 1), 1u);
            p.pass = glm::vec4(1.0f / float(sw), 1.0f / float(sh), i == 0 ? 1.0f : 0.0f, 0.0f);
            snprintf(name, sizeof(name), "Bloom down %u", i);
            FullscreenPass(cmd, mMipViews[i], SRHI_LOAD_ACTION_DONT_CARE, mDownPipeline, mDownSets[i], p, name);
        }
        // Upsample back up, accumulating into each larger mip.
        for (uint32_t i = mMipCount - 1; i >= 1; --i) {
            uint32_t sw = std::max(bd.width >> i, 1u), sh = std::max(bd.height >> i, 1u);
            p.pass = glm::vec4(1.0f / float(sw), 1.0f / float(sh), 0.0f, 0.0f);
            snprintf(name, sizeof(name), "Bloom up %u", i);
            FullscreenPass(cmd, mMipViews[i - 1], SRHI_LOAD_ACTION_LOAD, mUpPipeline, mUpSets[i], p, name);
        }

        // Composite + tonemap into the backbuffer.
        uint32_t shown = mShowMip >= 0 ? uint32_t(mShowMip) : 0;
        p.pass = glm::vec4(0.0f, 0.0f, 0.0f, mShowMip >= 0 ? 1.0f : 0.0f);
        FullscreenPass(cmd, srhiTextureGetDefaultView(backbuffer), SRHI_LOAD_ACTION_DONT_CARE, mCompositePipeline,
                       mCompositeSets[shown], p, "Composite");
    }
};

SRHI_EXAMPLE_MAIN(Bloom, "Bloom")
