/**
 * @ Author: Amélie Heinrich
 * @ Create Time: 2026-11-13 00:00:00
 * @ Copyright: Copyright (c) 2026 Amélie Heinrich. All rights reserved.
 */

// Physically based rendering of the DamagedHelmet with image-based lighting.
//
// At startup, compute passes turn the BC6H equirectangular HDRI into an RGBA16F cubemap (writing through
// 2D-array storage views of the cube), mip it with the blit encoder, convolve a diffuse irradiance cube and
// a GGX-prefiltered specular cube (one dispatch per mip, each through a single-mip view), and render the
// split-sum BRDF lookup table into RG16F. Every frame draws the helmet (GGX / Smith / Schlick + IBL) and a
// skybox, tonemapped for whichever output the swapchain uses.

#include "../Common/App.h"
#include "../Common/Model.h"

#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include <cmath>
#include <initializer_list>
#include <vector>

class PBRHelmet : public App
{
    static constexpr uint32_t kEnvSize = 512;
    static constexpr uint32_t kIrradianceSize = 32;
    static constexpr uint32_t kPrefilterSize = 128;
    static constexpr uint32_t kPrefilterMips = 6;
    static constexpr uint32_t kLutSize = 256;

    struct FrameParams
    {
        glm::mat4 viewProj;
        glm::mat4 invViewProj;
        glm::mat4 model;
        glm::vec4 cameraPos;
        glm::vec4 lighting;
        glm::vec4 sunDir;
        OutputParams output;
    };
    static_assert(sizeof(FrameParams) == 256, "matches the shader");

    srhiShaderModule* mShader = nullptr;
    srhiResourceSetLayout* mFrameLayout = nullptr;
    srhiResourceSetLayout* mMaterialLayout = nullptr;
    srhiRenderPipeline* mModelPipeline = nullptr;
    srhiRenderPipeline* mSkyPipeline = nullptr;
    srhiResourceSet* mFrameSet = nullptr;
    srhiTexture* mDepth = nullptr;

    srhiTexture* mEnvironment = nullptr;
    srhiTexture* mIrradiance = nullptr;
    srhiTexture* mPrefiltered = nullptr;
    srhiTexture* mBrdfLut = nullptr;
    srhiSampler* mSampler = nullptr;
    Model mHelmet;

    float mExposure = 1.0f, mIblIntensity = 1.0f, mSunIntensity = 2.5f, mSkyLod = 0.0f;
    float mSunAzimuth = 0.6f, mSunElevation = 0.7f;
    bool mAutoRotate = true;
    float mModelAngle = 0.0f;
    int mDebugView = 0;
    float mIblMs = 0.0f;

    // A layout with only some bindings of a module's set (pipelines in one module use different subsets).
    srhiResourceSetLayout* SubsetLayout(srhiShaderModule* module, uint32_t set, std::initializer_list<uint32_t> bindings)
    {
        uint32_t count = srhiShaderModuleGetResourceSetLayout(module, set, nullptr, 0);
        std::vector<srhiResourceSetLayoutEntry> all(count), picked;
        srhiShaderModuleGetResourceSetLayout(module, set, all.data(), count);
        for (const srhiResourceSetLayoutEntry& e : all)
            for (uint32_t b : bindings)
                if (e.binding == b)
                    picked.push_back(e);
        srhiResourceSetLayoutDesc desc = {picked.data(), uint32_t(picked.size()), nullptr};
        return srhiResourceSetLayoutCreate(mDevice, &desc);
    }

    srhiTexture* CreateCube(uint32_t size, uint32_t mips, uint32_t extraUsage, const char* label)
    {
        srhiTextureDesc desc = {};
        desc.type = SRHI_TEXTURE_TYPE_CUBE;
        desc.format = SRHI_FORMAT_RGBA16_FLOAT;
        desc.width = desc.height = size;
        desc.mipLevels = mips;
        desc.usage = SRHI_TEXTURE_USAGE_SAMPLED | SRHI_TEXTURE_USAGE_STORAGE | extraUsage;
        desc.label = label;
        return srhiTextureCreate(mDevice, &desc);
    }

    srhiTextureView* ArrayView(srhiTexture* cube, uint32_t mip)
    {
        srhiTextureViewDesc vd = {};
        vd.type = SRHI_TEXTURE_TYPE_2D_ARRAY;
        vd.baseMip = mip;
        vd.mipCount = 1;
        vd.label = "cube face array";
        return srhiTextureViewCreate(cube, &vd);
    }

    /// @brief Runs all IBL precomputation in one command buffer.
    bool PrecomputeIBL()
    {
        srhiTexture* equirect = LoadBakedTexture(mDevice, "Skyboxes/lake_pier_4k.bc6h.srt");
        srhiShaderModule* ibl = LoadShader("PBRHelmetIBL");
        if (!equirect || !ibl)
            return false;

        mEnvironment = CreateCube(kEnvSize, SRHI_ALL_MIPS, SRHI_TEXTURE_USAGE_RENDER_TARGET, "environment");
        mIrradiance = CreateCube(kIrradianceSize, 1, 0, "irradiance");
        mPrefiltered = CreateCube(kPrefilterSize, kPrefilterMips, 0, "prefiltered");
        srhiTextureDesc lutDesc = {};
        lutDesc.format = SRHI_FORMAT_RG16_FLOAT;
        lutDesc.width = lutDesc.height = kLutSize;
        lutDesc.usage = SRHI_TEXTURE_USAGE_SAMPLED | SRHI_TEXTURE_USAGE_RENDER_TARGET;
        lutDesc.label = "BRDF LUT";
        mBrdfLut = srhiTextureCreate(mDevice, &lutDesc);
        if (!mEnvironment || !mIrradiance || !mPrefiltered || !mBrdfLut)
            return false;

        srhiResourceSetLayout* equirectLayout = SubsetLayout(ibl, 0, {0, 1, 3});
        srhiResourceSetLayout* convolveLayout = SubsetLayout(ibl, 0, {1, 2, 3});
        auto computePipeline = [&](const char* entry, srhiResourceSetLayout* layout) {
            srhiComputePipelineDesc pd = {};
            pd.module = ibl;
            pd.entry = entry;
            pd.setLayouts[0] = layout;
            pd.label = entry;
            return srhiComputePipelineCreate(mDevice, &pd);
        };
        srhiComputePipeline* toCube = computePipeline("csEquirectToCube", equirectLayout);
        srhiComputePipeline* irradiance = computePipeline("csIrradiance", convolveLayout);
        srhiComputePipeline* prefilter = computePipeline("csPrefilter", convolveLayout);

        srhiRenderPipelineDesc lutPipelineDesc = {};
        lutPipelineDesc.module = ibl;
        lutPipelineDesc.vertexEntry = "vsFullscreen";
        lutPipelineDesc.fragmentEntry = "fsBrdfLut";
        lutPipelineDesc.colorTargetCount = 1;
        lutPipelineDesc.colorTargets[0].format = SRHI_FORMAT_RG16_FLOAT;
        lutPipelineDesc.label = "BRDF LUT";
        srhiRenderPipeline* lutPipeline = srhiRenderPipelineCreate(mDevice, &lutPipelineDesc);
        if (!toCube || !irradiance || !prefilter || !lutPipeline)
            return false;

        // Resource sets for every dispatch.
        std::vector<srhiTextureView*> views;
        std::vector<srhiResourceSet*> sets;
        auto makeSet = [&](srhiResourceSetLayout* layout, uint32_t sourceBinding, srhiTextureView* source, srhiTextureView* out) {
            srhiResourceBinding b[3] = {};
            b[0].binding = sourceBinding;
            b[0].textureView = source;
            b[1].binding = 1;
            b[1].sampler = mSampler;
            b[2].binding = 3;
            b[2].textureView = out;
            srhiResourceSetDesc sd = {layout, b, 3, nullptr};
            sets.push_back(srhiResourceSetCreate(mDevice, &sd));
            return sets.back();
        };
        views.push_back(ArrayView(mEnvironment, 0));
        srhiResourceSet* toCubeSet = makeSet(equirectLayout, 0, srhiTextureGetDefaultView(equirect), views.back());
        views.push_back(ArrayView(mIrradiance, 0));
        srhiResourceSet* irradianceSet = makeSet(convolveLayout, 2, srhiTextureGetDefaultView(mEnvironment), views.back());
        std::vector<srhiResourceSet*> prefilterSets;
        for (uint32_t mip = 0; mip < kPrefilterMips; ++mip) {
            views.push_back(ArrayView(mPrefiltered, mip));
            prefilterSets.push_back(makeSet(convolveLayout, 2, srhiTextureGetDefaultView(mEnvironment), views.back()));
        }

        srhiTextureDesc envDesc;
        srhiTextureGetDesc(mEnvironment, &envDesc);
        srhiQuerySet* timing = mInfo.features.timestampQueries ? srhiQuerySetCreate(mDevice, 2, "IBL") : nullptr;

        srhiCommandBuffer* cmd = srhiCommandBufferBegin(mDevice, "IBL precompute");
        struct
        {
            float params[4];
        } p = {};
        srhiComputePassDesc cp = {};
        cp.label = "equirect to cube";
        if (timing)
            cp.timestampWrites = {timing, 0, SRHI_QUERY_UNUSED};
        srhiComputeEncoder* enc = srhiComputeEncoderBegin(cmd, &cp);
        srhiComputeEncoderSetPipeline(enc, toCube);
        srhiComputeEncoderSetResourceSet(enc, 0, toCubeSet);
        p = {{0.0f, float(kEnvSize), 0.0f, 0.0f}};
        srhiComputeEncoderSetUniforms(enc, 0, &p, sizeof(p));
        srhiComputeEncoderDispatch(enc, kEnvSize / 8, kEnvSize / 8, 6);
        srhiComputeEncoderEnd(enc);

        srhiBlitEncoder* blit = srhiBlitEncoderBegin(cmd, "environment mips");
        srhiBlitEncoderGenerateMipmaps(blit, mEnvironment);
        srhiBlitEncoderEnd(blit);

        cp.label = "convolve";
        if (timing)
            cp.timestampWrites = {timing, SRHI_QUERY_UNUSED, 1};
        enc = srhiComputeEncoderBegin(cmd, &cp);
        srhiComputeEncoderSetPipeline(enc, irradiance);
        srhiComputeEncoderSetResourceSet(enc, 0, irradianceSet);
        p = {{0.0f, float(kIrradianceSize), float(kEnvSize), float(envDesc.mipLevels)}};
        srhiComputeEncoderSetUniforms(enc, 0, &p, sizeof(p));
        srhiComputeEncoderDispatch(enc, kIrradianceSize / 8, kIrradianceSize / 8, 6);

        srhiComputeEncoderSetPipeline(enc, prefilter);
        for (uint32_t mip = 0; mip < kPrefilterMips; ++mip) {
            uint32_t size = kPrefilterSize >> mip;
            p = {{float(mip) / float(kPrefilterMips - 1), float(size), float(kEnvSize), float(envDesc.mipLevels)}};
            srhiComputeEncoderSetResourceSet(enc, 0, prefilterSets[mip]);
            srhiComputeEncoderSetUniforms(enc, 0, &p, sizeof(p));
            srhiComputeEncoderDispatch(enc, (size + 7) / 8, (size + 7) / 8, 6);
        }
        srhiComputeEncoderEnd(enc);

        srhiRenderPassDesc lutPass = {};
        lutPass.colorAttachmentCount = 1;
        lutPass.colorAttachments[0].view = srhiTextureGetDefaultView(mBrdfLut);
        lutPass.colorAttachments[0].loadAction = SRHI_LOAD_ACTION_DONT_CARE;
        lutPass.colorAttachments[0].storeAction = SRHI_STORE_ACTION_STORE;
        lutPass.label = "BRDF LUT";
        srhiRenderEncoder* lut = srhiRenderEncoderBegin(cmd, &lutPass);
        srhiRenderEncoderSetPipeline(lut, lutPipeline);
        srhiRenderEncoderDraw(lut, 3, 1, 0, 0);
        srhiRenderEncoderEnd(lut);
        srhiCommandBufferCommit(cmd, nullptr, nullptr);
        srhiDeviceWaitIdle(mDevice);

        uint64_t ns[2];
        if (timing && srhiQuerySetGetResults(timing, 0, 2, ns) && ns[1] > ns[0])
            mIblMs = float(double(ns[1] - ns[0]) / 1e6);
        printf("IBL precompute: %.2f ms GPU\n", mIblMs);

        // Everything below was only needed for the precompute; destroying after the commit is safe.
        srhiQuerySetDestroy(timing);
        for (srhiResourceSet* s : sets)
            srhiResourceSetDestroy(s);
        for (srhiTextureView* v : views)
            srhiTextureViewDestroy(v);
        srhiComputePipelineDestroy(toCube);
        srhiComputePipelineDestroy(irradiance);
        srhiComputePipelineDestroy(prefilter);
        srhiRenderPipelineDestroy(lutPipeline);
        srhiResourceSetLayoutDestroy(equirectLayout);
        srhiResourceSetLayoutDestroy(convolveLayout);
        srhiTextureDestroy(equirect);
        return true;
    }

    bool CreatePipelines()
    {
        srhiRenderPipelineDestroy(mModelPipeline);
        srhiRenderPipelineDestroy(mSkyPipeline);

        srhiRenderPipelineDesc desc = {};
        desc.module = mShader;
        desc.vertexEntry = "vsModel";
        desc.fragmentEntry = "fsModel";
        desc.vertexLayout = Model::VertexLayout();
        desc.raster.cullMode = SRHI_CULL_MODE_BACK;
        desc.depthStencil.format = SRHI_FORMAT_D32_FLOAT;
        desc.depthStencil.depthTestEnable = 1;
        desc.depthStencil.depthWriteEnable = 1;
        desc.depthStencil.depthCompare = SRHI_COMPARE_FUNCTION_LESS;
        desc.colorTargetCount = 1;
        desc.colorTargets[0].format = BackbufferFormat();
        desc.setLayouts[0] = mFrameLayout;
        desc.setLayouts[1] = mMaterialLayout;
        desc.label = "PBR model";
        mModelPipeline = srhiRenderPipelineCreate(mDevice, &desc);

        srhiRenderPipelineDesc sky = {};
        sky.module = mShader;
        sky.vertexEntry = "vsSky";
        sky.fragmentEntry = "fsSky";
        sky.depthStencil.format = SRHI_FORMAT_D32_FLOAT;
        sky.depthStencil.depthTestEnable = 1;
        sky.depthStencil.depthWriteEnable = 0;
        sky.depthStencil.depthCompare = SRHI_COMPARE_FUNCTION_LESS_EQUAL;
        sky.colorTargetCount = 1;
        sky.colorTargets[0].format = BackbufferFormat();
        sky.setLayouts[0] = mFrameLayout;
        sky.label = "skybox";
        mSkyPipeline = srhiRenderPipelineCreate(mDevice, &sky);
        return mModelPipeline && mSkyPipeline;
    }

    bool OnInit() override
    {
        srhiSamplerDesc sd = {};
        sd.minFilter = sd.magFilter = sd.mipFilter = SRHI_FILTER_LINEAR;
        sd.addressU = sd.addressV = sd.addressW = SRHI_ADDRESS_MODE_CLAMP_TO_EDGE;
        sd.label = "IBL";
        mSampler = srhiSamplerCreate(mDevice, &sd);

        if (!PrecomputeIBL())
            return false;

        mShader = LoadShader("PBRHelmet");
        if (!mShader)
            return false;
        mFrameLayout = LayoutFromShader(mShader, 0);
        mMaterialLayout = Model::CreateMaterialLayout(mDevice);
        if (!mHelmet.Load(mDevice, "DamagedHelmet", mMaterialLayout))
            return false;

        srhiResourceBinding b[5] = {};
        b[0] = {0, nullptr, 0, 0, srhiTextureGetDefaultView(mIrradiance)};
        b[1] = {1, nullptr, 0, 0, srhiTextureGetDefaultView(mPrefiltered)};
        b[2] = {2, nullptr, 0, 0, srhiTextureGetDefaultView(mBrdfLut)};
        b[3].binding = 3;
        b[3].sampler = mSampler;
        b[4] = {4, nullptr, 0, 0, srhiTextureGetDefaultView(mEnvironment)};
        srhiResourceSetDesc setDesc = {mFrameLayout, b, 5, "IBL"};
        mFrameSet = srhiResourceSetCreate(mDevice, &setDesc);

        // Frame the helmet.
        const float* mn = mHelmet.BoundsMin();
        const float* mx = mHelmet.BoundsMax();
        glm::vec3 center = (glm::vec3(mn[0], mn[1], mn[2]) + glm::vec3(mx[0], mx[1], mx[2])) * 0.5f;
        float radius = glm::length(glm::vec3(mx[0], mx[1], mx[2]) - center);
        mCamera.position = center + glm::vec3(0.0f, 0.0f, radius * 2.4f);
        mCamera.yaw = 3.14159265f;
        mCamera.pitch = 0.0f;
        mCamera.nearZ = 0.01f;
        mCamera.farZ = 100.0f;
        mCamera.moveSpeed = 2.0f;
        return mFrameSet && CreatePipelines();
    }

    void OnResize(uint32_t width, uint32_t height) override
    {
        srhiTextureDestroy(mDepth);
        srhiTextureDesc desc = {};
        desc.format = SRHI_FORMAT_D32_FLOAT;
        desc.width = width;
        desc.height = height;
        desc.usage = SRHI_TEXTURE_USAGE_RENDER_TARGET;
        desc.label = "depth";
        mDepth = srhiTextureCreate(mDevice, &desc);
    }

    void OnOutputChanged(srhiFormat) override { CreatePipelines(); }

    void OnShutdown() override
    {
        mHelmet.Destroy();
        srhiResourceSetDestroy(mFrameSet);
        srhiRenderPipelineDestroy(mModelPipeline);
        srhiRenderPipelineDestroy(mSkyPipeline);
        srhiResourceSetLayoutDestroy(mFrameLayout);
        srhiResourceSetLayoutDestroy(mMaterialLayout);
        srhiTextureDestroy(mDepth);
        srhiTextureDestroy(mEnvironment);
        srhiTextureDestroy(mIrradiance);
        srhiTextureDestroy(mPrefiltered);
        srhiTextureDestroy(mBrdfLut);
        srhiSamplerDestroy(mSampler);
    }

    void OnUpdate(float dt) override
    {
        ImGuiIO& io = ImGui::GetIO();
        if (!io.WantCaptureMouse && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            mModelAngle += io.MouseDelta.x * 0.01f;
        else if (mAutoRotate)
            mModelAngle += dt * 0.3f;
    }

    void OnImGui() override
    {
        ImGui::SetNextWindowPos(ImVec2(10, 340), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(330, 0), ImGuiCond_FirstUseEver);
        ImGui::Begin("PBR");
        ImGui::SliderFloat("Exposure", &mExposure, 0.05f, 8.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("IBL intensity", &mIblIntensity, 0.0f, 4.0f);
        ImGui::SliderFloat("Sun intensity", &mSunIntensity, 0.0f, 10.0f);
        ImGui::SliderFloat("Sun azimuth", &mSunAzimuth, -3.14f, 3.14f);
        ImGui::SliderFloat("Sun elevation", &mSunElevation, 0.0f, 1.57f);
        ImGui::SliderFloat("Sky blur (LOD)", &mSkyLod, 0.0f, 8.0f);
        ImGui::Checkbox("Auto rotate (drag with left mouse)", &mAutoRotate);
        const char* views[] = {"Lit", "Albedo", "Normal", "Metallic", "Roughness", "Occlusion", "Emissive", "IBL only"};
        ImGui::Combo("Debug view", &mDebugView, views, IM_ARRAYSIZE(views));
        ImGui::Text("IBL precompute: %.2f ms GPU", mIblMs);
        ImGui::Text("%u triangles", mHelmet.TriangleCount());
        ImGui::End();
    }

    void OnRender(srhiCommandBuffer* cmd, srhiTexture* backbuffer) override
    {
        glm::mat4 view = mCamera.GetView();
        glm::mat4 proj = mCamera.GetProj();
        FrameParams f;
        f.viewProj = proj * view;
        f.invViewProj = glm::inverse(f.viewProj);
        f.model = glm::rotate(glm::mat4(1.0f), mModelAngle, glm::vec3(0.0f, 1.0f, 0.0f));
        f.cameraPos = glm::vec4(mCamera.position, float(kPrefilterMips - 1));
        f.lighting = glm::vec4(mExposure, mIblIntensity, mSunIntensity, float(mDebugView));
        f.sunDir = glm::vec4(std::cos(mSunElevation) * std::cos(mSunAzimuth), std::sin(mSunElevation),
                             std::cos(mSunElevation) * std::sin(mSunAzimuth), mSkyLod);
        f.output = GetOutputParams();

        srhiRenderPassDesc pass = {};
        pass.colorAttachmentCount = 1;
        pass.colorAttachments[0].view = srhiTextureGetDefaultView(backbuffer);
        pass.colorAttachments[0].loadAction = SRHI_LOAD_ACTION_DONT_CARE;
        pass.colorAttachments[0].storeAction = SRHI_STORE_ACTION_STORE;
        pass.depthAttachment.view = srhiTextureGetDefaultView(mDepth);
        pass.depthAttachment.depthLoadAction = SRHI_LOAD_ACTION_CLEAR;
        pass.depthAttachment.depthStoreAction = SRHI_STORE_ACTION_DONT_CARE;
        pass.depthAttachment.clearDepth = 1.0f;
        pass.timestampWrites = mProfiler.Scope("Helmet + sky");
        pass.label = "PBR";

        srhiRenderEncoder* enc = srhiRenderEncoderBegin(cmd, &pass);
        srhiRenderEncoderSetUniforms(enc, 0, &f, sizeof(f));
        srhiRenderEncoderSetResourceSet(enc, 0, mFrameSet);
        srhiRenderEncoderSetPipeline(enc, mModelPipeline);
        mHelmet.Draw(enc, Model::kAll);
        srhiRenderEncoderSetPipeline(enc, mSkyPipeline);
        srhiRenderEncoderDraw(enc, 3, 1, 0, 0);
        srhiRenderEncoderEnd(enc);
    }
};

SRHI_EXAMPLE_MAIN(PBRHelmet, "PBR Helmet")
