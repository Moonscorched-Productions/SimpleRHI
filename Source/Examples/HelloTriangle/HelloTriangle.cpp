/**
 * @ Author: Amélie Heinrich
 * @ Create Time: 2026-11-13 00:00:00
 * @ Copyright: Copyright (c) 2026 Amélie Heinrich. All rights reserved.
 */

// The smallest SimpleRHI program: one vertex buffer, one pipeline, transient uniforms.

#include "../Common/App.h"

#include <imgui.h>

#include <cmath>

class HelloTriangle : public App
{
    struct Vertex
    {
        float x, y;
        float r, g, b;
    };

    srhiBuffer* mVertices = nullptr;
    srhiRenderPipeline* mPipeline = nullptr;
    float mSpeed = 0.5f, mBrightness = 1.0f, mAngle = 0.0f;

    bool CreatePipeline()
    {
        srhiRenderPipelineDestroy(mPipeline);
        srhiRenderPipelineDesc desc = {};
        desc.module = LoadShader("HelloTriangle");
        desc.vertexEntry = "vsMain";
        desc.fragmentEntry = "fsMain";
        desc.vertexLayout.bufferCount = 1;
        desc.vertexLayout.buffers[0].stride = sizeof(Vertex);
        desc.vertexLayout.attributeCount = 2;
        desc.vertexLayout.attributes[0] = {0, SRHI_VERTEX_FORMAT_FLOAT2, 0, 0};
        desc.vertexLayout.attributes[1] = {1, SRHI_VERTEX_FORMAT_FLOAT3, 8, 0};
        desc.colorTargetCount = 1;
        desc.colorTargets[0].format = BackbufferFormat();
        desc.label = "triangle";
        mPipeline = srhiRenderPipelineCreate(mDevice, &desc);
        return mPipeline != nullptr;
    }

    bool OnInit() override
    {
        mCameraEnabled = false;
        const Vertex vertices[3] = {
            {0.0f, 0.7f, 1.0f, 0.2f, 0.2f},
            {-0.6f, -0.45f, 0.2f, 1.0f, 0.2f},
            {0.6f, -0.45f, 0.2f, 0.3f, 1.0f},
        };
        srhiBufferDesc vb = {sizeof(vertices), SRHI_BUFFER_USAGE_VERTEX, 0, "triangle vertices"};
        mVertices = srhiBufferCreate(mDevice, &vb);
        srhiBufferWrite(mDevice, mVertices, 0, vertices, sizeof(vertices));
        return mVertices && CreatePipeline();
    }

    void OnOutputChanged(srhiFormat) override { CreatePipeline(); }

    void OnShutdown() override
    {
        srhiRenderPipelineDestroy(mPipeline);
        srhiBufferDestroy(mVertices);
    }

    void OnUpdate(float dt) override { mAngle += dt * mSpeed; }

    void OnImGui() override
    {
        ImGui::SetNextWindowPos(ImVec2(10, 340), ImGuiCond_FirstUseEver);
        ImGui::Begin("Triangle");
        ImGui::SliderFloat("Speed", &mSpeed, -3.0f, 3.0f);
        ImGui::SliderFloat("Brightness", &mBrightness, 0.0f, 4.0f);
        ImGui::End();
    }

    void OnRender(srhiCommandBuffer* cmd, srhiTexture* backbuffer) override
    {
        srhiRenderPassDesc pass = {};
        pass.colorAttachmentCount = 1;
        pass.colorAttachments[0].view = srhiTextureGetDefaultView(backbuffer);
        pass.colorAttachments[0].loadAction = SRHI_LOAD_ACTION_CLEAR;
        pass.colorAttachments[0].storeAction = SRHI_STORE_ACTION_STORE;
        pass.colorAttachments[0].clearColor[0] = 0.02f;
        pass.colorAttachments[0].clearColor[1] = 0.02f;
        pass.colorAttachments[0].clearColor[2] = 0.03f;
        pass.colorAttachments[0].clearColor[3] = 1.0f;
        pass.timestampWrites = mProfiler.Scope("Triangle");
        pass.label = "triangle";

        struct
        {
            float rotation[4];
            OutputParams output;
        } params = {{std::cos(mAngle), std::sin(mAngle), float(mWidth) / float(mHeight), mBrightness}, GetOutputParams()};

        srhiRenderEncoder* enc = srhiRenderEncoderBegin(cmd, &pass);
        srhiRenderEncoderSetPipeline(enc, mPipeline);
        srhiRenderEncoderSetVertexBuffer(enc, 0, mVertices, 0);
        srhiRenderEncoderSetUniforms(enc, 0, &params, sizeof(params));
        srhiRenderEncoderDraw(enc, 3, 1, 0, 0);
        srhiRenderEncoderEnd(enc);
    }
};

SRHI_EXAMPLE_MAIN(HelloTriangle, "Hello Triangle")
