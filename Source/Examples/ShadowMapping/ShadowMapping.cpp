/**
 * @ Author: Amélie Heinrich
 * @ Create Time: 2026-11-13 00:00:00
 * @ Copyright: Copyright (c) 2026 Amélie Heinrich. All rights reserved.
 */

// Cascaded shadow maps on Intel Sponza.
//
// - 4 cascades in one D32_FLOAT 2D array; each cascade is its own depth-only render pass into a
//   single-layer 2D view of the array.
// - Practical split scheme (log/uniform blend), bounding-sphere cascades with texel snapping, so the
//   shadows do not shimmer when the camera moves.
// - Hardware depth bias + slope scale in the shadow pipelines, normal offset in the receiver, PCF with a
//   comparison sampler (DepthTexture2DArray + SamplerComparisonState).
// - Alpha-tested foliage/curtains: opaque geometry renders with a vertex-only pipeline, masked geometry
//   with a fragment shader that discards.

#include "../Common/App.h"
#include "../Common/Model.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstring>

class ShadowMapping : public App
{
    static constexpr uint32_t kCascades = 4;

    struct Frame
    {
        glm::mat4 viewProj;
        glm::mat4 view;
        glm::mat4 cascadeViewProj[kCascades];
        glm::vec4 cameraPos;
        glm::vec4 sunDir;
        glm::vec4 sunColor;
        glm::vec4 splits;
        glm::vec4 settings;
        OutputParams output;
    };
    static_assert(sizeof(Frame) == 480, "matches the shader");

    srhiShaderModule* mModule = nullptr;
    srhiResourceSetLayout* mMaterialLayout = nullptr;
    srhiResourceSetLayout* mShadowSetLayout = nullptr;
    srhiRenderPipeline* mShadowOpaque = nullptr;
    srhiRenderPipeline* mShadowMasked = nullptr;
    srhiRenderPipeline* mForward = nullptr;
    srhiTexture* mShadowMap = nullptr;
    srhiTextureView* mCascadeViews[kCascades] = {};
    srhiSampler* mShadowSampler = nullptr;
    srhiResourceSet* mShadowSet = nullptr;
    srhiTexture* mDepth = nullptr;
    Model mSponza;

    // Settings
    int mShadowMapSize = 2048;
    float mSunYaw = 0.6f, mSunPitch = 1.15f;
    float mSunIntensity = 10.0f, mAmbient = 0.35f;
    float mShadowDistance = 45.0f, mSplitLambda = 0.75f;
    int mDepthBias = 2;
    float mSlopeBias = 2.0f;
    float mNormalOffset = 0.02f;
    int mPcfRadius = 1;
    bool mShowCascades = false;
    bool mStabilize = true;
    bool mAnimateSun = false;
    float mSplits[kCascades] = {};

    bool CreateShadowResources()
    {
        for (srhiTextureView*& v : mCascadeViews) {
            srhiTextureViewDestroy(v);
            v = nullptr;
        }
        srhiResourceSetDestroy(mShadowSet);
        srhiTextureDestroy(mShadowMap);

        srhiTextureDesc td = {};
        td.type = SRHI_TEXTURE_TYPE_2D_ARRAY;
        td.format = SRHI_FORMAT_D32_FLOAT;
        td.width = td.height = uint32_t(mShadowMapSize);
        td.depthOrLayers = kCascades;
        td.usage = SRHI_TEXTURE_USAGE_RENDER_TARGET | SRHI_TEXTURE_USAGE_SAMPLED;
        td.label = "cascaded shadow map";
        mShadowMap = srhiTextureCreate(mDevice, &td);
        if (!mShadowMap)
            return false;
        for (uint32_t i = 0; i < kCascades; ++i) {
            srhiTextureViewDesc vd = {};
            vd.type = SRHI_TEXTURE_TYPE_2D;
            vd.baseLayer = i;
            vd.layerCount = 1;
            vd.label = "cascade";
            mCascadeViews[i] = srhiTextureViewCreate(mShadowMap, &vd);
        }
        srhiResourceBinding b[2] = {};
        b[0].binding = 0;
        b[0].textureView = srhiTextureGetDefaultView(mShadowMap);
        b[1].binding = 1;
        b[1].sampler = mShadowSampler;
        srhiResourceSetDesc sd = {mShadowSetLayout, b, 2, "shadow set"};
        mShadowSet = srhiResourceSetCreate(mDevice, &sd);
        return mShadowSet != nullptr;
    }

    bool CreateShadowPipelines()
    {
        srhiRenderPipelineDestroy(mShadowOpaque);
        srhiRenderPipelineDestroy(mShadowMasked);
        srhiRenderPipelineDesc desc = {};
        desc.module = mModule;
        desc.vertexEntry = "vsShadow";
        desc.vertexLayout = Model::VertexLayout();
        desc.depthStencil.format = SRHI_FORMAT_D32_FLOAT;
        desc.depthStencil.depthTestEnable = 1;
        desc.depthStencil.depthWriteEnable = 1;
        desc.depthStencil.depthCompare = SRHI_COMPARE_FUNCTION_LESS_EQUAL;
        desc.raster.cullMode = SRHI_CULL_MODE_NONE;
        desc.raster.depthBias = mDepthBias;
        desc.raster.depthBiasSlopeScale = mSlopeBias;
        desc.raster.depthClamp = mInfo.features.depthClamp; // "pancaking": casters behind the near plane still cast
        // The vertex-only pipeline uses no material: leave set 1 out of its layout (WebGPU requires every set
        // in a pipeline layout to be bound).
        desc.label = "shadow opaque";
        mShadowOpaque = srhiRenderPipelineCreate(mDevice, &desc);
        desc.setLayouts[1] = mMaterialLayout;
        desc.fragmentEntry = "fsShadowMasked";
        desc.label = "shadow masked";
        mShadowMasked = srhiRenderPipelineCreate(mDevice, &desc);
        return mShadowOpaque && mShadowMasked;
    }

    bool CreateForwardPipeline()
    {
        srhiRenderPipelineDestroy(mForward);
        srhiRenderPipelineDesc desc = {};
        desc.module = mModule;
        desc.vertexEntry = "vsMain";
        desc.fragmentEntry = "fsMain";
        desc.vertexLayout = Model::VertexLayout();
        desc.colorTargetCount = 1;
        desc.colorTargets[0].format = BackbufferFormat();
        desc.depthStencil.format = SRHI_FORMAT_D32_FLOAT;
        desc.depthStencil.depthTestEnable = 1;
        desc.depthStencil.depthWriteEnable = 1;
        desc.depthStencil.depthCompare = SRHI_COMPARE_FUNCTION_LESS;
        desc.raster.cullMode = SRHI_CULL_MODE_NONE;
        desc.setLayouts[0] = mShadowSetLayout;
        desc.setLayouts[1] = mMaterialLayout;
        desc.label = "forward";
        mForward = srhiRenderPipelineCreate(mDevice, &desc);
        return mForward != nullptr;
    }

    glm::vec3 SunDirection() const
    {
        return glm::normalize(glm::vec3(std::cos(mSunPitch) * std::sin(mSunYaw), std::sin(mSunPitch), std::cos(mSunPitch) * std::cos(mSunYaw)));
    }

    /// @brief Fills the cascade matrices and split distances for the current camera.
    void ComputeCascades(glm::mat4 outViewProj[kCascades])
    {
        const float nearZ = mCamera.nearZ, farZ = std::min(mShadowDistance, mCamera.farZ);
        for (uint32_t i = 0; i < kCascades; ++i) {
            float p = float(i + 1) / float(kCascades);
            float logSplit = nearZ * std::pow(farZ / nearZ, p);
            float uniSplit = nearZ + (farZ - nearZ) * p;
            mSplits[i] = mSplitLambda * logSplit + (1.0f - mSplitLambda) * uniSplit;
        }

        const glm::mat4 view = mCamera.GetView();
        const glm::vec3 sun = SunDirection();
        float prev = nearZ;
        for (uint32_t i = 0; i < kCascades; ++i) {
            // Slice frustum corners in world space.
            glm::mat4 proj = glm::perspectiveRH_ZO(mCamera.fovYRadians, mCamera.aspect, prev, mSplits[i]);
            glm::mat4 inv = glm::inverse(proj * view);
            glm::vec3 corners[8];
            glm::vec3 center(0.0f);
            for (int c = 0; c < 8; ++c) {
                glm::vec4 ndc((c & 1) ? 1.0f : -1.0f, (c & 2) ? 1.0f : -1.0f, (c & 4) ? 1.0f : 0.0f, 1.0f);
                glm::vec4 w = inv * ndc;
                corners[c] = glm::vec3(w) / w.w;
                center += corners[c];
            }
            center /= 8.0f;
            float radius = 0.0f;
            for (const glm::vec3& c : corners)
                radius = std::max(radius, glm::length(c - center));
            radius = std::ceil(radius * 16.0f) / 16.0f; // quantize: a stable size keeps texel snapping stable

            const float zPad = 40.0f; // room for casters between the sun and the slice
            glm::vec3 up = std::fabs(sun.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
            glm::mat4 lightView = glm::lookAtRH(center + sun * (radius + zPad), center, up);
            glm::mat4 lightProj = glm::orthoRH_ZO(-radius, radius, -radius, radius, 0.0f, 2.0f * radius + zPad);
            glm::mat4 vp = lightProj * lightView;
            if (mStabilize) {
                // Snap the projected world origin to a shadow texel.
                glm::vec4 origin = vp * glm::vec4(0, 0, 0, 1);
                float half = float(mShadowMapSize) * 0.5f;
                glm::vec2 texel = glm::vec2(origin) * half;
                glm::vec2 offset = (glm::round(texel) - texel) / half;
                lightProj[3][0] += offset.x;
                lightProj[3][1] += offset.y;
                vp = lightProj * lightView;
            }
            outViewProj[i] = vp;
            prev = mSplits[i];
        }
    }

    bool OnInit() override
    {
        mModule = LoadShader("ShadowMapping");
        if (!mModule)
            return false;
        mMaterialLayout = Model::CreateMaterialLayout(mDevice);
        mShadowSetLayout = LayoutFromShader(mModule, 0);
        if (!mSponza.Load(mDevice, "IntelSponza", mMaterialLayout))
            return false;

        srhiSamplerDesc sd = {};
        sd.minFilter = sd.magFilter = SRHI_FILTER_LINEAR;
        sd.addressU = sd.addressV = sd.addressW = SRHI_ADDRESS_MODE_CLAMP_TO_EDGE;
        sd.compareEnable = 1;
        sd.compareFunction = SRHI_COMPARE_FUNCTION_LESS_EQUAL;
        sd.label = "shadow compare";
        mShadowSampler = srhiSamplerCreate(mDevice, &sd);

        mCamera.position = glm::vec3(9.0f, 2.0f, 0.3f);
        mCamera.yaw = -1.5708f; // looking down -X, along the nave
        mCamera.pitch = 0.05f;
        mCamera.farZ = 200.0f;

        return CreateShadowResources() && CreateShadowPipelines() && CreateForwardPipeline();
    }

    void OnResize(uint32_t width, uint32_t height) override
    {
        srhiTextureDestroy(mDepth);
        srhiTextureDesc td = {};
        td.format = SRHI_FORMAT_D32_FLOAT;
        td.width = width;
        td.height = height;
        td.usage = SRHI_TEXTURE_USAGE_RENDER_TARGET;
        td.label = "scene depth";
        mDepth = srhiTextureCreate(mDevice, &td);
    }

    void OnOutputChanged(srhiFormat) override { CreateForwardPipeline(); }

    void OnShutdown() override
    {
        mSponza.Destroy();
        for (srhiTextureView* v : mCascadeViews)
            srhiTextureViewDestroy(v);
        srhiResourceSetDestroy(mShadowSet);
        srhiTextureDestroy(mShadowMap);
        srhiTextureDestroy(mDepth);
        srhiSamplerDestroy(mShadowSampler);
        srhiRenderPipelineDestroy(mShadowOpaque);
        srhiRenderPipelineDestroy(mShadowMasked);
        srhiRenderPipelineDestroy(mForward);
        srhiResourceSetLayoutDestroy(mShadowSetLayout);
        srhiResourceSetLayoutDestroy(mMaterialLayout);
    }

    void OnUpdate(float dt) override
    {
        if (mAnimateSun)
            mSunYaw += dt * 0.2f;
    }

    void OnImGui() override
    {
        ImGui::SetNextWindowPos(ImVec2(10, 340), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(330, 0), ImGuiCond_FirstUseEver);
        ImGui::Begin("Cascaded shadows");
        ImGui::Text("Sponza: %u triangles, %u draws", mSponza.TriangleCount(), mSponza.SubmeshCount());
        ImGui::SeparatorText("Sun");
        ImGui::SliderFloat("Yaw", &mSunYaw, -3.14159f, 3.14159f);
        ImGui::SliderFloat("Pitch", &mSunPitch, 0.1f, 1.55f);
        ImGui::Checkbox("Animate", &mAnimateSun);
        ImGui::SliderFloat("Intensity", &mSunIntensity, 0.0f, 20.0f);
        ImGui::SliderFloat("Ambient", &mAmbient, 0.0f, 1.0f);
        ImGui::SeparatorText("Cascades");
        ImGui::Checkbox("Visualize cascades", &mShowCascades);
        ImGui::Checkbox("Stabilize (texel snapping)", &mStabilize);
        ImGui::SliderFloat("Shadow distance", &mShadowDistance, 5.0f, 150.0f);
        ImGui::SliderFloat("Split lambda", &mSplitLambda, 0.0f, 1.0f);
        ImGui::Text("Splits: %.1f / %.1f / %.1f / %.1f", mSplits[0], mSplits[1], mSplits[2], mSplits[3]);
        static const int sizes[] = {512, 1024, 2048, 4096};
        int sizeIndex = int(std::find(sizes, sizes + 4, mShadowMapSize) - sizes);
        if (ImGui::Combo("Resolution", &sizeIndex, "512\0" "1024\0" "2048\0" "4096\0")) {
            mShadowMapSize = sizes[sizeIndex];
            srhiDeviceWaitIdle(mDevice);
            CreateShadowResources();
        }
        ImGui::SeparatorText("Bias");
        bool rebuild = ImGui::SliderInt("Depth bias", &mDepthBias, 0, 64);
        rebuild |= ImGui::SliderFloat("Slope scale", &mSlopeBias, 0.0f, 8.0f);
        if (rebuild)
            CreateShadowPipelines();
        ImGui::SliderFloat("Normal offset", &mNormalOffset, 0.0f, 0.2f);
        ImGui::SliderInt("PCF radius", &mPcfRadius, 0, 3);
        ImGui::End();
    }

    void OnRender(srhiCommandBuffer* cmd, srhiTexture* backbuffer) override
    {
        Frame frame = {};
        ComputeCascades(frame.cascadeViewProj);

        // --- Shadow passes: one depth-only pass per cascade layer ---
        for (uint32_t i = 0; i < kCascades; ++i) {
            char name[32];
            snprintf(name, sizeof(name), "Shadow cascade %u", i);
            srhiRenderPassDesc pass = {};
            pass.depthAttachment.view = mCascadeViews[i];
            pass.depthAttachment.depthLoadAction = SRHI_LOAD_ACTION_CLEAR;
            pass.depthAttachment.depthStoreAction = SRHI_STORE_ACTION_STORE;
            pass.depthAttachment.clearDepth = 1.0f;
            pass.timestampWrites = mProfiler.Scope(name);
            pass.label = name;
            srhiRenderEncoder* enc = srhiRenderEncoderBegin(cmd, &pass);
            srhiRenderEncoderSetUniforms(enc, 1, glm::value_ptr(frame.cascadeViewProj[i]), sizeof(glm::mat4));
            srhiRenderEncoderSetPipeline(enc, mShadowOpaque);
            mSponza.Draw(enc, Model::kOpaque, false);
            srhiRenderEncoderSetPipeline(enc, mShadowMasked);
            mSponza.Draw(enc, Model::kMasked, true);
            srhiRenderEncoderEnd(enc);
        }

        // --- Forward pass ---
        frame.view = mCamera.GetView();
        frame.viewProj = mCamera.GetProj() * frame.view;
        frame.cameraPos = glm::vec4(mCamera.position, 1.0f);
        frame.sunDir = glm::vec4(SunDirection(), mSunIntensity);
        frame.sunColor = glm::vec4(1.0f, 0.95f, 0.85f, mAmbient);
        frame.splits = glm::vec4(mSplits[0], mSplits[1], mSplits[2], mSplits[3]);
        frame.settings = glm::vec4(mShowCascades ? 1.0f : 0.0f, mNormalOffset, float(mPcfRadius), 1.0f / float(mShadowMapSize));
        frame.output = GetOutputParams();

        srhiRenderPassDesc pass = {};
        pass.colorAttachmentCount = 1;
        pass.colorAttachments[0].view = srhiTextureGetDefaultView(backbuffer);
        pass.colorAttachments[0].loadAction = SRHI_LOAD_ACTION_CLEAR;
        pass.colorAttachments[0].storeAction = SRHI_STORE_ACTION_STORE;
        pass.colorAttachments[0].clearColor[0] = 0.4f;
        pass.colorAttachments[0].clearColor[1] = 0.55f;
        pass.colorAttachments[0].clearColor[2] = 0.8f;
        pass.colorAttachments[0].clearColor[3] = 1.0f;
        pass.depthAttachment.view = srhiTextureGetDefaultView(mDepth);
        pass.depthAttachment.depthLoadAction = SRHI_LOAD_ACTION_CLEAR;
        pass.depthAttachment.depthStoreAction = SRHI_STORE_ACTION_DONT_CARE;
        pass.depthAttachment.clearDepth = 1.0f;
        pass.timestampWrites = mProfiler.Scope("Forward");
        pass.label = "forward";
        srhiRenderEncoder* enc = srhiRenderEncoderBegin(cmd, &pass);
        srhiRenderEncoderSetPipeline(enc, mForward);
        srhiRenderEncoderSetResourceSet(enc, 0, mShadowSet);
        srhiRenderEncoderSetUniforms(enc, 0, &frame, sizeof(frame));
        mSponza.Draw(enc, Model::kAllOpaque, true);
        srhiRenderEncoderEnd(enc);
    }
};

SRHI_EXAMPLE_MAIN(ShadowMapping, "Cascaded Shadow Maps")
