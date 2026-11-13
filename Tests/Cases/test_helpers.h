/**
 * @ Author: Amélie Heinrich
 * @ Create Time: 2026-11-13 00:00:00
 * @ Copyright: Copyright (c) 2026 Amélie Heinrich. All rights reserved.
 */

// Shared building blocks for the test cases.

#pragma once

#include "../Framework/test_gpu.h"

#include <cstring>
#include <vector>

namespace srhitest
{
    struct ColorVertex
    {
        float x, y;
        float r, g, b, a;
    };

    struct DrawParams
    {
        float offsetScale[4];
    };

    inline DrawParams Identity() { return DrawParams{{0.0f, 0.0f, 1.0f, 1.0f}}; }

    /// @brief Vertex layout for ColorVertex in buffer 0.
    inline srhiVertexLayout ColorVertexLayout()
    {
        srhiVertexLayout layout = {};
        layout.bufferCount = 1;
        layout.buffers[0].stride = sizeof(ColorVertex);
        layout.attributeCount = 2;
        layout.attributes[0] = {0, SRHI_VERTEX_FORMAT_FLOAT2, 0, 0};
        layout.attributes[1] = {1, SRHI_VERTEX_FORMAT_FLOAT4, 8, 0};
        return layout;
    }

    /// @brief A pipeline drawing triangle.slang's vsMain/fsMain into one color target.
    inline srhiRenderPipeline* CreateColorPipeline(GpuFixture& gpu, srhiFormat format,
                                                   srhiPrimitiveTopology topology = SRHI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                                                   srhiFormat depthFormat = SRHI_FORMAT_UNDEFINED,
                                                   const srhiRenderPipelineDesc* overrides = nullptr)
    {
        srhiShaderModule* module = gpu.LoadShader("triangle.slang");
        if (!module)
            return nullptr;
        srhiRenderPipelineDesc desc = overrides ? *overrides : srhiRenderPipelineDesc{};
        desc.module = module;
        desc.vertexEntry = "vsMain";
        desc.fragmentEntry = "fsMain";
        if (!overrides)
            desc.vertexLayout = ColorVertexLayout();
        desc.topology = topology;
        desc.colorTargetCount = 1;
        desc.colorTargets[0].format = format;
        if (!overrides || desc.depthStencil.format == SRHI_FORMAT_UNDEFINED)
            desc.depthStencil.format = depthFormat;
        desc.label = "color pipeline";
        return gpu.Own(srhiRenderPipelineCreate(gpu.Device(), &desc));
    }

    /// @brief A render target of the given size, readable back.
    inline srhiTexture* CreateTarget(GpuFixture& gpu, uint32_t w, uint32_t h, srhiFormat format = SRHI_FORMAT_RGBA8_UNORM,
                                     uint32_t extraUsage = 0)
    {
        return gpu.CreateTexture2D(w, h, format, SRHI_TEXTURE_USAGE_RENDER_TARGET | SRHI_TEXTURE_USAGE_COPY_SRC | extraUsage, 1,
                                   "target");
    }

    inline srhiRenderPassDesc ClearPass(srhiTexture* target, float r, float g, float b, float a)
    {
        srhiRenderPassDesc pass = {};
        pass.colorAttachmentCount = 1;
        pass.colorAttachments[0].view = srhiTextureGetDefaultView(target);
        pass.colorAttachments[0].loadAction = SRHI_LOAD_ACTION_CLEAR;
        pass.colorAttachments[0].storeAction = SRHI_STORE_ACTION_STORE;
        pass.colorAttachments[0].clearColor[0] = r;
        pass.colorAttachments[0].clearColor[1] = g;
        pass.colorAttachments[0].clearColor[2] = b;
        pass.colorAttachments[0].clearColor[3] = a;
        pass.label = "test pass";
        return pass;
    }

    /// @brief An RGBA8 checkerboard with a colored gradient, size x size.
    inline std::vector<uint8_t> MakeCheckerRgba8(uint32_t size, uint32_t cell = 8)
    {
        std::vector<uint8_t> pixels(size_t(size) * size * 4);
        for (uint32_t y = 0; y < size; ++y)
            for (uint32_t x = 0; x < size; ++x) {
                uint8_t* p = &pixels[(size_t(y) * size + x) * 4];
                bool on = ((x / cell) + (y / cell)) % 2 == 0;
                p[0] = uint8_t(x * 255 / (size - 1));
                p[1] = uint8_t(y * 255 / (size - 1));
                p[2] = on ? 255 : 32;
                p[3] = 255;
            }
        return pixels;
    }

    template <typename T> std::vector<uint8_t> AsBytes(const std::vector<T>& v)
    {
        std::vector<uint8_t> out(v.size() * sizeof(T));
        memcpy(out.data(), v.data(), out.size());
        return out;
    }
} // namespace srhitest
