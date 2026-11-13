/**
 * @ Author: Amélie Heinrich
 * @ Create Time: 2026-11-13 00:00:00
 * @ Copyright: Copyright (c) 2026 Amélie Heinrich. All rights reserved.
 */

// WebGPU backend (standard webgpu.h: wgpu-native on desktop, emdawnwebgpu on the web).
//
// - Resource sets are WGPUBindGroups at groups 0..2. Group 3 is a shared layout of SRHI_MAX_UNIFORM_SLOTS
//   dynamic-offset uniform buffers backed by per-command-buffer ring chunks: SetUniforms keeps a CPU copy,
//   and at draw/dispatch time every slot the pipeline uses is copied into the current chunk and bound with
//   its dynamic offset. Chunks are uploaded with one queue write at commit and recycled on retirement.
// - Pipeline layouts must have a bind group at every group index, so unused sets get an empty bind group.
// - Writes are wgpuQueueWrite*: applied before any later submit, which is SimpleRHI's write semantics.
// - Readbacks copy into MapRead buffers at record time and are mapped after submit. Retirement (in commit
//   order) waits for queue completion and every map, then fires callbacks.
// - Blit "encoders" record straight into the command encoder. GenerateMipmaps uses an internal WGSL blit.

#include "srhi_internal.h"

// --- webgpu.h shim -----------------------------------------------------------------------------------
#if defined(__EMSCRIPTEN__)
    #include <emscripten/emscripten.h>
    #include <webgpu/webgpu.h>
    #define SRHI_WGPU_NATIVE 0
#else
    #include <webgpu/webgpu.h>
    #include <webgpu/wgpu.h>
    #define SRHI_WGPU_NATIVE 1
#endif

#include <algorithm>
#include <cstring>
#include <deque>
#include <memory>
#include <unordered_map>

using namespace srhi;

namespace
{
    /// @brief True if the pipeline's shaders use any binding of set s.
    bool SetUsed(const srhi::PipelineLayoutInfo& layout, uint32_t s)
    {
        if (!layout.setLayouts[s])
            return false;
        for (const srhi::BindingSlot& slot : layout.slots[s])
            if (slot.used)
                return true;
        return false;
    }

    constexpr uint64_t kUniformChunkSize = 1ull << 20;

    WGPUStringView SV(const char* s) { return WGPUStringView{s, s ? WGPU_STRLEN : 0}; }
    WGPUStringView SV(const std::string& s) { return WGPUStringView{s.c_str(), s.size()}; }
    std::string ToString(WGPUStringView v)
    {
        if (!v.data)
            return {};
        return v.length == WGPU_STRLEN ? std::string(v.data) : std::string(v.data, v.length);
    }

    /// @brief Blocks until *flag becomes true, pumping WebGPU events.
    void WaitFor(WGPUInstance instance, WGPUDevice device, const bool* flag)
    {
        while (!*flag) {
#if SRHI_WGPU_NATIVE
            if (device)
                wgpuDevicePoll(device, true, nullptr);
            wgpuInstanceProcessEvents(instance);
#else
            (void)device;
            wgpuInstanceProcessEvents(instance);
            emscripten_sleep(0); // requires -sASYNCIFY
#endif
        }
    }

    // --- Conversions ---------------------------------------------------------------------------------

    WGPUTextureFormat ToWgpu(srhiFormat f)
    {
        switch (f) {
        case SRHI_FORMAT_R8_UNORM: return WGPUTextureFormat_R8Unorm;
        case SRHI_FORMAT_RG8_UNORM: return WGPUTextureFormat_RG8Unorm;
        case SRHI_FORMAT_RGBA8_UNORM: return WGPUTextureFormat_RGBA8Unorm;
        case SRHI_FORMAT_RGBA8_UNORM_SRGB: return WGPUTextureFormat_RGBA8UnormSrgb;
        case SRHI_FORMAT_RGBA8_SNORM: return WGPUTextureFormat_RGBA8Snorm;
        case SRHI_FORMAT_RGBA8_UINT: return WGPUTextureFormat_RGBA8Uint;
        case SRHI_FORMAT_BGRA8_UNORM: return WGPUTextureFormat_BGRA8Unorm;
        case SRHI_FORMAT_BGRA8_UNORM_SRGB: return WGPUTextureFormat_BGRA8UnormSrgb;
        case SRHI_FORMAT_R16_FLOAT: return WGPUTextureFormat_R16Float;
        case SRHI_FORMAT_RG16_FLOAT: return WGPUTextureFormat_RG16Float;
        case SRHI_FORMAT_RGBA16_FLOAT: return WGPUTextureFormat_RGBA16Float;
        case SRHI_FORMAT_R16_UINT: return WGPUTextureFormat_R16Uint;
        case SRHI_FORMAT_R32_FLOAT: return WGPUTextureFormat_R32Float;
        case SRHI_FORMAT_R32_UINT: return WGPUTextureFormat_R32Uint;
        case SRHI_FORMAT_R32_SINT: return WGPUTextureFormat_R32Sint;
        case SRHI_FORMAT_RG32_FLOAT: return WGPUTextureFormat_RG32Float;
        case SRHI_FORMAT_RG32_UINT: return WGPUTextureFormat_RG32Uint;
        case SRHI_FORMAT_RGBA32_FLOAT: return WGPUTextureFormat_RGBA32Float;
        case SRHI_FORMAT_RGBA32_UINT: return WGPUTextureFormat_RGBA32Uint;
        case SRHI_FORMAT_RGB10A2_UNORM: return WGPUTextureFormat_RGB10A2Unorm;
        case SRHI_FORMAT_RG11B10_FLOAT: return WGPUTextureFormat_RG11B10Ufloat;
        case SRHI_FORMAT_D16_UNORM: return WGPUTextureFormat_Depth16Unorm;
        case SRHI_FORMAT_D32_FLOAT: return WGPUTextureFormat_Depth32Float;
        case SRHI_FORMAT_D32_FLOAT_S8_UINT: return WGPUTextureFormat_Depth32FloatStencil8;
        case SRHI_FORMAT_BC1_UNORM: return WGPUTextureFormat_BC1RGBAUnorm;
        case SRHI_FORMAT_BC1_UNORM_SRGB: return WGPUTextureFormat_BC1RGBAUnormSrgb;
        case SRHI_FORMAT_BC3_UNORM: return WGPUTextureFormat_BC3RGBAUnorm;
        case SRHI_FORMAT_BC3_UNORM_SRGB: return WGPUTextureFormat_BC3RGBAUnormSrgb;
        case SRHI_FORMAT_BC4_UNORM: return WGPUTextureFormat_BC4RUnorm;
        case SRHI_FORMAT_BC5_UNORM: return WGPUTextureFormat_BC5RGUnorm;
        case SRHI_FORMAT_BC6H_UFLOAT: return WGPUTextureFormat_BC6HRGBUfloat;
        case SRHI_FORMAT_BC7_UNORM: return WGPUTextureFormat_BC7RGBAUnorm;
        case SRHI_FORMAT_BC7_UNORM_SRGB: return WGPUTextureFormat_BC7RGBAUnormSrgb;
        case SRHI_FORMAT_ASTC_4x4_UNORM: return WGPUTextureFormat_ASTC4x4Unorm;
        case SRHI_FORMAT_ASTC_4x4_UNORM_SRGB: return WGPUTextureFormat_ASTC4x4UnormSrgb;
        default: return WGPUTextureFormat_Undefined;
        }
    }

    WGPUTextureViewDimension ToWgpu(srhiTextureType t)
    {
        switch (t) {
        case SRHI_TEXTURE_TYPE_2D: return WGPUTextureViewDimension_2D;
        case SRHI_TEXTURE_TYPE_2D_ARRAY: return WGPUTextureViewDimension_2DArray;
        case SRHI_TEXTURE_TYPE_CUBE: return WGPUTextureViewDimension_Cube;
        case SRHI_TEXTURE_TYPE_CUBE_ARRAY: return WGPUTextureViewDimension_CubeArray;
        case SRHI_TEXTURE_TYPE_3D: return WGPUTextureViewDimension_3D;
        }
        return WGPUTextureViewDimension_2D;
    }

    WGPUCompareFunction ToWgpu(srhiCompareFunction f)
    {
        switch (f) {
        case SRHI_COMPARE_FUNCTION_NEVER: return WGPUCompareFunction_Never;
        case SRHI_COMPARE_FUNCTION_LESS: return WGPUCompareFunction_Less;
        case SRHI_COMPARE_FUNCTION_EQUAL: return WGPUCompareFunction_Equal;
        case SRHI_COMPARE_FUNCTION_LESS_EQUAL: return WGPUCompareFunction_LessEqual;
        case SRHI_COMPARE_FUNCTION_GREATER: return WGPUCompareFunction_Greater;
        case SRHI_COMPARE_FUNCTION_NOT_EQUAL: return WGPUCompareFunction_NotEqual;
        case SRHI_COMPARE_FUNCTION_GREATER_EQUAL: return WGPUCompareFunction_GreaterEqual;
        case SRHI_COMPARE_FUNCTION_ALWAYS: return WGPUCompareFunction_Always;
        }
        return WGPUCompareFunction_Always;
    }

    WGPUAddressMode ToWgpu(srhiAddressMode m)
    {
        switch (m) {
        case SRHI_ADDRESS_MODE_REPEAT: return WGPUAddressMode_Repeat;
        case SRHI_ADDRESS_MODE_MIRROR_REPEAT: return WGPUAddressMode_MirrorRepeat;
        case SRHI_ADDRESS_MODE_CLAMP_TO_EDGE: return WGPUAddressMode_ClampToEdge;
        }
        return WGPUAddressMode_Repeat;
    }

    WGPUVertexFormat ToWgpu(srhiVertexFormat f)
    {
        switch (f) {
        case SRHI_VERTEX_FORMAT_FLOAT: return WGPUVertexFormat_Float32;
        case SRHI_VERTEX_FORMAT_FLOAT2: return WGPUVertexFormat_Float32x2;
        case SRHI_VERTEX_FORMAT_FLOAT3: return WGPUVertexFormat_Float32x3;
        case SRHI_VERTEX_FORMAT_FLOAT4: return WGPUVertexFormat_Float32x4;
        case SRHI_VERTEX_FORMAT_HALF2: return WGPUVertexFormat_Float16x2;
        case SRHI_VERTEX_FORMAT_HALF4: return WGPUVertexFormat_Float16x4;
        case SRHI_VERTEX_FORMAT_UINT: return WGPUVertexFormat_Uint32;
        case SRHI_VERTEX_FORMAT_UINT2: return WGPUVertexFormat_Uint32x2;
        case SRHI_VERTEX_FORMAT_UINT4: return WGPUVertexFormat_Uint32x4;
        case SRHI_VERTEX_FORMAT_SINT: return WGPUVertexFormat_Sint32;
        case SRHI_VERTEX_FORMAT_UBYTE4: return WGPUVertexFormat_Uint8x4;
        case SRHI_VERTEX_FORMAT_UBYTE4_NORM: return WGPUVertexFormat_Unorm8x4;
        case SRHI_VERTEX_FORMAT_SHORT2_NORM: return WGPUVertexFormat_Snorm16x2;
        case SRHI_VERTEX_FORMAT_SHORT4_NORM: return WGPUVertexFormat_Snorm16x4;
        }
        return WGPUVertexFormat_Float32;
    }

    WGPUBlendFactor ToWgpu(srhiBlendFactor f)
    {
        switch (f) {
        case SRHI_BLEND_FACTOR_ZERO: return WGPUBlendFactor_Zero;
        case SRHI_BLEND_FACTOR_ONE: return WGPUBlendFactor_One;
        case SRHI_BLEND_FACTOR_SRC_COLOR: return WGPUBlendFactor_Src;
        case SRHI_BLEND_FACTOR_ONE_MINUS_SRC_COLOR: return WGPUBlendFactor_OneMinusSrc;
        case SRHI_BLEND_FACTOR_SRC_ALPHA: return WGPUBlendFactor_SrcAlpha;
        case SRHI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA: return WGPUBlendFactor_OneMinusSrcAlpha;
        case SRHI_BLEND_FACTOR_DST_COLOR: return WGPUBlendFactor_Dst;
        case SRHI_BLEND_FACTOR_ONE_MINUS_DST_COLOR: return WGPUBlendFactor_OneMinusDst;
        case SRHI_BLEND_FACTOR_DST_ALPHA: return WGPUBlendFactor_DstAlpha;
        case SRHI_BLEND_FACTOR_ONE_MINUS_DST_ALPHA: return WGPUBlendFactor_OneMinusDstAlpha;
        case SRHI_BLEND_FACTOR_SRC_ALPHA_SATURATED: return WGPUBlendFactor_SrcAlphaSaturated;
        case SRHI_BLEND_FACTOR_BLEND_COLOR: return WGPUBlendFactor_Constant;
        case SRHI_BLEND_FACTOR_ONE_MINUS_BLEND_COLOR: return WGPUBlendFactor_OneMinusConstant;
        }
        return WGPUBlendFactor_One;
    }

    WGPUBlendOperation ToWgpu(srhiBlendOp op)
    {
        switch (op) {
        case SRHI_BLEND_OP_ADD: return WGPUBlendOperation_Add;
        case SRHI_BLEND_OP_SUBTRACT: return WGPUBlendOperation_Subtract;
        case SRHI_BLEND_OP_REVERSE_SUBTRACT: return WGPUBlendOperation_ReverseSubtract;
        case SRHI_BLEND_OP_MIN: return WGPUBlendOperation_Min;
        case SRHI_BLEND_OP_MAX: return WGPUBlendOperation_Max;
        }
        return WGPUBlendOperation_Add;
    }

    WGPUBlendComponent ToWgpu(srhiBlendFactor src, srhiBlendFactor dst, srhiBlendOp op)
    {
        WGPUBlendComponent c = {};
        c.operation = ToWgpu(op);
        // WebGPU requires One/One for min/max (other APIs ignore the factors there).
        bool minMax = op == SRHI_BLEND_OP_MIN || op == SRHI_BLEND_OP_MAX;
        c.srcFactor = minMax ? WGPUBlendFactor_One : ToWgpu(src);
        c.dstFactor = minMax ? WGPUBlendFactor_One : ToWgpu(dst);
        return c;
    }

    WGPUStencilOperation ToWgpu(srhiStencilOp op)
    {
        switch (op) {
        case SRHI_STENCIL_OP_KEEP: return WGPUStencilOperation_Keep;
        case SRHI_STENCIL_OP_ZERO: return WGPUStencilOperation_Zero;
        case SRHI_STENCIL_OP_REPLACE: return WGPUStencilOperation_Replace;
        case SRHI_STENCIL_OP_INCREMENT_CLAMP: return WGPUStencilOperation_IncrementClamp;
        case SRHI_STENCIL_OP_DECREMENT_CLAMP: return WGPUStencilOperation_DecrementClamp;
        case SRHI_STENCIL_OP_INVERT: return WGPUStencilOperation_Invert;
        case SRHI_STENCIL_OP_INCREMENT_WRAP: return WGPUStencilOperation_IncrementWrap;
        case SRHI_STENCIL_OP_DECREMENT_WRAP: return WGPUStencilOperation_DecrementWrap;
        }
        return WGPUStencilOperation_Keep;
    }

    WGPUStoreOp ToWgpu(srhiStoreAction a)
    {
        return a == SRHI_STORE_ACTION_STORE || a == SRHI_STORE_ACTION_STORE_AND_RESOLVE ? WGPUStoreOp_Store : WGPUStoreOp_Discard;
    }

    WGPULoadOp ToWgpu(srhiLoadAction a)
    {
        // WebGPU has no "don't care" load: clear is the cheap equivalent.
        return a == SRHI_LOAD_ACTION_LOAD ? WGPULoadOp_Load : WGPULoadOp_Clear;
    }

    // =================================================================================================
    // Objects
    // =================================================================================================

    struct WgpuDevice;

    struct WgpuBuffer : srhiBuffer
    {
        WGPUBuffer buffer = nullptr;
        ~WgpuBuffer() override
        {
            if (buffer)
                wgpuBufferRelease(buffer);
        }
        void* GetNative() override { return buffer; }
    };

    struct WgpuTexture : srhiTexture
    {
        WGPUTexture texture = nullptr;
        ~WgpuTexture() override
        {
            if (texture && !isSwapchainImage)
                wgpuTextureRelease(texture);
        }
        void* GetNative() override { return texture; }
    };

    struct WgpuTextureView : srhiTextureView
    {
        /// @brief The view as declared (bind groups); depth-only aspect for depth-stencil formats.
        WGPUTextureView view = nullptr;
        /// @brief A single-mip, single-layer 2D (or 3D) view for render attachments, created on demand.
        WGPUTextureView renderView = nullptr;
        ~WgpuTextureView() override { Release(); }
        void Release()
        {
            if (view)
                wgpuTextureViewRelease(view);
            if (renderView)
                wgpuTextureViewRelease(renderView);
            view = renderView = nullptr;
        }
        WGPUTextureView RenderView();
    };

    struct WgpuSampler : srhiSampler
    {
        WGPUSampler sampler = nullptr;
        ~WgpuSampler() override
        {
            if (sampler)
                wgpuSamplerRelease(sampler);
        }
    };

    struct WgpuShaderModule : srhiShaderModule
    {
        std::unordered_map<std::string, WGPUShaderModule> modules;
        ~WgpuShaderModule() override
        {
            for (auto& [name, m] : modules)
                if (m)
                    wgpuShaderModuleRelease(m);
        }
    };

    struct WgpuResourceSetLayout : srhiResourceSetLayout
    {
        WGPUBindGroupLayout layout = nullptr;
        ~WgpuResourceSetLayout() override
        {
            if (layout)
                wgpuBindGroupLayoutRelease(layout);
        }
    };

    struct WgpuResourceSet : srhiResourceSet
    {
        WGPUBindGroup group = nullptr;
        ~WgpuResourceSet() override
        {
            if (group)
                wgpuBindGroupRelease(group);
        }
    };

    struct WgpuRenderPipeline : srhiRenderPipeline
    {
        WGPURenderPipeline pipeline = nullptr;
        uint32_t groupCount = 0;
        bool usesUniforms = false;
        ~WgpuRenderPipeline() override
        {
            if (pipeline)
                wgpuRenderPipelineRelease(pipeline);
        }
    };

    struct WgpuComputePipeline : srhiComputePipeline
    {
        WGPUComputePipeline pipeline = nullptr;
        uint32_t groupCount = 0;
        bool usesUniforms = false;
        ~WgpuComputePipeline() override
        {
            if (pipeline)
                wgpuComputePipelineRelease(pipeline);
        }
    };

    struct WgpuQuerySet : srhiQuerySet
    {
        WGPUQuerySet set = nullptr;
        WGPUBuffer resolve = nullptr;
        std::vector<uint64_t> results;
        std::vector<uint8_t> valid;
        ~WgpuQuerySet() override
        {
            if (set)
                wgpuQuerySetRelease(set);
            if (resolve)
                wgpuBufferRelease(resolve);
        }
        bool GetResults(uint32_t first, uint32_t count, uint64_t* out) override
        {
            for (uint32_t i = first; i < first + count; ++i)
                if (!valid[i])
                    return false;
            for (uint32_t i = 0; i < count; ++i)
                out[i] = results[first + i];
            return true;
        }
    };

    struct UniformChunk
    {
        WGPUBuffer buffer = nullptr;
        WGPUBindGroup group = nullptr;
        std::vector<uint8_t> shadow;
        uint64_t used = 0;
    };

    struct Readback
    {
        WGPUBuffer buffer = nullptr;
        uint64_t size = 0;
        srhiReadbackCallback callback = nullptr;
        void* userData = nullptr;
        /// @brief Texture readbacks: padded rows to repack.
        uint32_t paddedRow = 0, tightRow = 0, rows = 0;
        /// @brief Timestamp readbacks.
        WgpuQuerySet* querySet = nullptr;
        uint32_t firstQuery = 0;
        std::vector<uint32_t> queries;
        bool mapped = false;
        bool failed = false;
    };

    struct InFlight
    {
        bool done = false;
        srhiCompletionCallback callback = nullptr;
        void* userData = nullptr;
        std::vector<std::unique_ptr<Readback>> readbacks;
        std::vector<UniformChunk> chunks;
        bool Complete() const
        {
            if (!done)
                return false;
            for (auto& r : readbacks)
                if (!r->mapped && !r->failed)
                    return false;
            return true;
        }
    };

    struct WgpuCommandBuffer;
    struct WgpuSwapchain;

    // --- Binding state -------------------------------------------------------------------------------

    struct BindingState
    {
        srhiResourceSet* sets[SRHI_MAX_RESOURCE_SETS] = {};
        bool setDirty[SRHI_MAX_RESOURCE_SETS] = {};
        std::vector<uint8_t> uniforms[SRHI_MAX_UNIFORM_SLOTS];
        bool uniformsDirty = false;

        void Reset()
        {
            for (uint32_t i = 0; i < SRHI_MAX_RESOURCE_SETS; ++i) {
                sets[i] = nullptr;
                setDirty[i] = false;
            }
            for (auto& u : uniforms)
                u.clear();
            uniformsDirty = false;
        }
        void MarkAllDirty()
        {
            for (bool& d : setDirty)
                d = true;
            uniformsDirty = true;
        }
    };

    // --- Encoders ------------------------------------------------------------------------------------

    struct WgpuRenderEncoder : srhiRenderEncoder
    {
        WgpuCommandBuffer* owner = nullptr;
        WGPURenderPassEncoder pass = nullptr;
        BindingState bindings;
        uint32_t width = 0, height = 0;

        void End() override;
        void SetPipeline(srhiRenderPipeline* p) override
        {
            wgpuRenderPassEncoderSetPipeline(pass, static_cast<WgpuRenderPipeline*>(p)->pipeline);
            bindings.MarkAllDirty();
        }
        void SetVertexBuffer(uint32_t index, srhiBuffer* buffer, uint64_t offset) override
        {
            wgpuRenderPassEncoderSetVertexBuffer(pass, index, static_cast<WgpuBuffer*>(buffer)->buffer, offset, WGPU_WHOLE_SIZE);
        }
        void SetIndexBuffer(srhiBuffer* buffer, srhiIndexType type, uint64_t offset) override
        {
            wgpuRenderPassEncoderSetIndexBuffer(pass, static_cast<WgpuBuffer*>(buffer)->buffer,
                                                type == SRHI_INDEX_TYPE_UINT16 ? WGPUIndexFormat_Uint16 : WGPUIndexFormat_Uint32,
                                                offset, WGPU_WHOLE_SIZE);
        }
        void SetResourceSet(uint32_t index, srhiResourceSet* set) override
        {
            bindings.sets[index] = set;
            bindings.setDirty[index] = true;
        }
        void SetUniforms(uint32_t slot, const void* data, uint32_t size) override
        {
            bindings.uniforms[slot].assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
            bindings.uniformsDirty = true;
        }
        void SetViewport(const srhiViewport& v) override
        {
            wgpuRenderPassEncoderSetViewport(pass, v.x, v.y, v.width, v.height, v.minDepth, v.maxDepth);
        }
        void SetScissor(const srhiScissorRect& r) override
        {
            uint32_t x = std::min(r.x, width), y = std::min(r.y, height);
            wgpuRenderPassEncoderSetScissorRect(pass, x, y, std::min(r.width, width - x), std::min(r.height, height - y));
        }
        void SetBlendColor(const float c[4]) override
        {
            WGPUColor color = {c[0], c[1], c[2], c[3]};
            wgpuRenderPassEncoderSetBlendConstant(pass, &color);
        }
        void SetStencilReference(uint32_t ref) override { wgpuRenderPassEncoderSetStencilReference(pass, ref); }
        bool Flush();
        void Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) override
        {
            if (Flush())
                wgpuRenderPassEncoderDraw(pass, vertexCount, instanceCount, firstVertex, firstInstance);
        }
        void DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t baseVertex,
                         uint32_t firstInstance) override
        {
            if (Flush())
                wgpuRenderPassEncoderDrawIndexed(pass, indexCount, instanceCount, firstIndex, baseVertex, firstInstance);
        }
        void DrawIndirect(srhiBuffer* buffer, uint64_t offset) override
        {
            if (Flush())
                wgpuRenderPassEncoderDrawIndirect(pass, static_cast<WgpuBuffer*>(buffer)->buffer, offset);
        }
        void DrawIndexedIndirect(srhiBuffer* buffer, uint64_t offset) override
        {
            if (Flush())
                wgpuRenderPassEncoderDrawIndexedIndirect(pass, static_cast<WgpuBuffer*>(buffer)->buffer, offset);
        }
        void PushDebugGroup(const char* name) override { wgpuRenderPassEncoderPushDebugGroup(pass, SV(name)); }
        void PopDebugGroup() override { wgpuRenderPassEncoderPopDebugGroup(pass); }
        void InsertDebugMarker(const char* name) override { wgpuRenderPassEncoderInsertDebugMarker(pass, SV(name)); }
    };

    struct WgpuComputeEncoder : srhiComputeEncoder
    {
        WgpuCommandBuffer* owner = nullptr;
        WGPUComputePassEncoder pass = nullptr;
        BindingState bindings;

        void End() override;
        void SetPipeline(srhiComputePipeline* p) override
        {
            wgpuComputePassEncoderSetPipeline(pass, static_cast<WgpuComputePipeline*>(p)->pipeline);
            bindings.MarkAllDirty();
        }
        void SetResourceSet(uint32_t index, srhiResourceSet* set) override
        {
            bindings.sets[index] = set;
            bindings.setDirty[index] = true;
        }
        void SetUniforms(uint32_t slot, const void* data, uint32_t size) override
        {
            bindings.uniforms[slot].assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
            bindings.uniformsDirty = true;
        }
        bool Flush();
        void Dispatch(uint32_t x, uint32_t y, uint32_t z) override
        {
            if (Flush())
                wgpuComputePassEncoderDispatchWorkgroups(pass, x, y, z);
        }
        void DispatchIndirect(srhiBuffer* buffer, uint64_t offset) override
        {
            if (Flush())
                wgpuComputePassEncoderDispatchWorkgroupsIndirect(pass, static_cast<WgpuBuffer*>(buffer)->buffer, offset);
        }
        void PushDebugGroup(const char* name) override { wgpuComputePassEncoderPushDebugGroup(pass, SV(name)); }
        void PopDebugGroup() override { wgpuComputePassEncoderPopDebugGroup(pass); }
        void InsertDebugMarker(const char* name) override { wgpuComputePassEncoderInsertDebugMarker(pass, SV(name)); }
    };

    struct WgpuBlitEncoder : srhiBlitEncoder
    {
        WgpuCommandBuffer* owner = nullptr;
        WGPUCommandEncoder enc = nullptr;
        bool labeled = false;

        void End() override
        {
            if (labeled)
                wgpuCommandEncoderPopDebugGroup(enc);
            labeled = false;
        }
        void CopyBufferToBuffer(srhiBuffer* src, uint64_t srcOffset, srhiBuffer* dst, uint64_t dstOffset, uint64_t size) override
        {
            wgpuCommandEncoderCopyBufferToBuffer(enc, static_cast<WgpuBuffer*>(src)->buffer, srcOffset,
                                                 static_cast<WgpuBuffer*>(dst)->buffer, dstOffset, size);
        }
        static WGPUTexelCopyTextureInfo TexInfo(const srhiTextureCopyLocation& l)
        {
            WgpuTexture* t = static_cast<WgpuTexture*>(l.texture);
            WGPUTexelCopyTextureInfo info = {};
            info.texture = t->texture;
            info.mipLevel = l.mip;
            info.origin = {l.x, l.y, t->desc.type == SRHI_TEXTURE_TYPE_3D ? l.z : l.layer};
            info.aspect = WGPUTextureAspect_All;
            return info;
        }
        static WGPUTexelCopyBufferInfo BufInfo(const srhiBufferCopyLocation& l, srhiFormat format, uint32_t height)
        {
            WGPUTexelCopyBufferInfo info = {};
            info.buffer = static_cast<WgpuBuffer*>(l.buffer)->buffer;
            info.layout.offset = l.offset;
            info.layout.bytesPerRow = l.bytesPerRow;
            info.layout.rowsPerImage = l.rowsPerImage ? l.rowsPerImage : RowCount(format, height);
            return info;
        }
        static WGPUExtent3D Extent(srhiTexture* t, uint32_t w, uint32_t h, uint32_t d)
        {
            // Compressed copies cover whole blocks.
            const FormatInfo& fi = GetFormatInfo(t->desc.format);
            return {uint32_t(AlignUp(w, fi.blockWidth)), uint32_t(AlignUp(h, fi.blockHeight)), d};
        }
        void CopyBufferToTexture(const srhiBufferCopyLocation& src, const srhiTextureCopyLocation& dst, uint32_t width,
                                 uint32_t height, uint32_t depth) override
        {
            WGPUTexelCopyBufferInfo b = BufInfo(src, dst.texture->desc.format, height);
            WGPUTexelCopyTextureInfo t = TexInfo(dst);
            WGPUExtent3D e = Extent(dst.texture, width, height, depth);
            wgpuCommandEncoderCopyBufferToTexture(enc, &b, &t, &e);
        }
        void CopyTextureToBuffer(const srhiTextureCopyLocation& src, const srhiBufferCopyLocation& dst, uint32_t width,
                                 uint32_t height, uint32_t depth) override
        {
            WGPUTexelCopyTextureInfo t = TexInfo(src);
            WGPUTexelCopyBufferInfo b = BufInfo(dst, src.texture->desc.format, height);
            WGPUExtent3D e = Extent(src.texture, width, height, depth);
            wgpuCommandEncoderCopyTextureToBuffer(enc, &t, &b, &e);
        }
        void CopyTextureToTexture(const srhiTextureCopyLocation& src, const srhiTextureCopyLocation& dst, uint32_t width,
                                  uint32_t height, uint32_t depth) override
        {
            WGPUTexelCopyTextureInfo s = TexInfo(src), d = TexInfo(dst);
            WGPUExtent3D e = Extent(src.texture, width, height, depth);
            wgpuCommandEncoderCopyTextureToTexture(enc, &s, &d, &e);
        }
        void GenerateMipmaps(srhiTexture* texture) override;
        void PushDebugGroup(const char* name) override { wgpuCommandEncoderPushDebugGroup(enc, SV(name)); }
        void PopDebugGroup() override { wgpuCommandEncoderPopDebugGroup(enc); }
    };

    // --- Swapchain -----------------------------------------------------------------------------------

    struct WgpuSwapchain : srhiSwapchain
    {
        WgpuDevice* dev = nullptr;
        WGPUSurface surface = nullptr;
        WGPUTextureFormat surfaceFormat = WGPUTextureFormat_Undefined;
        WGPUTextureFormat viewFormat = WGPUTextureFormat_Undefined;
        WgpuTexture* image = nullptr;
        WgpuTextureView* imageView = nullptr;
        bool acquired = false;
        bool configured = false;

        ~WgpuSwapchain() override;
        bool Apply(srhiColorSpace colorSpace, bool vsync, uint32_t width, uint32_t height);
        void ReleaseImage()
        {
            imageView->Release();
            if (image->texture)
                wgpuTextureRelease(image->texture);
            image->texture = nullptr;
            acquired = false;
        }
        void Resize(uint32_t width, uint32_t height) override
        {
            if (acquired)
                ReleaseImage();
            Apply(desc.colorSpace, desc.vsync != 0, width, height);
        }
        bool Configure(srhiColorSpace colorSpace, bool vsync) override
        {
            if (acquired)
                ReleaseImage();
            return Apply(colorSpace, vsync, desc.width, desc.height);
        }
        void GetDisplayInfo(srhiDisplayInfo& out) override { out.hdrSupported = 0; }
        srhiTexture* Acquire() override;
    };

    // --- Command buffer ------------------------------------------------------------------------------

    struct WgpuCommandBuffer : srhiCommandBuffer
    {
        WgpuDevice* dev = nullptr;
        WGPUCommandEncoder enc = nullptr;
        WgpuRenderEncoder render;
        WgpuComputeEncoder compute;
        WgpuBlitEncoder blit;
        std::unique_ptr<InFlight> pending = std::make_unique<InFlight>();
        /// @brief Index into pending->chunks of the chunk being filled, or -1.
        int currentChunk = -1;
        std::vector<WgpuSwapchain*> presents;
        /// @brief Query indices written per query set, for resolution at commit.
        std::vector<std::pair<WgpuQuerySet*, std::vector<uint32_t>>> queryWrites;

        ~WgpuCommandBuffer() override
        {
            if (enc)
                wgpuCommandEncoderRelease(enc);
        }

        void NoteQuery(WgpuQuerySet* qs, uint32_t index)
        {
            if (index == SRHI_QUERY_UNUSED)
                return;
            qs->valid[index] = 0;
            for (auto& [set, indices] : queryWrites)
                if (set == qs) {
                    indices.push_back(index);
                    return;
                }
            queryWrites.push_back({qs, {index}});
        }

        /// @brief Copies every uniform slot the pipeline uses into one chunk; fills offsets and returns the group.
        WGPUBindGroup UploadUniforms(BindingState& state, const PipelineLayoutInfo& layout, const char* label,
                                     uint32_t offsets[SRHI_MAX_UNIFORM_SLOTS]);

        srhiRenderEncoder* BeginRender(const srhiRenderPassDesc& desc) override;
        srhiComputeEncoder* BeginCompute(const srhiComputePassDesc& desc) override;
        srhiBlitEncoder* BeginBlit(const char* label) override
        {
            blit.owner = this;
            blit.enc = enc;
            blit.labeled = label && *label;
            if (blit.labeled)
                wgpuCommandEncoderPushDebugGroup(enc, SV(label));
            return &blit;
        }
        void Commit(srhiCompletionCallback callback, void* userData) override;
        void PushDebugGroup(const char* name) override { wgpuCommandEncoderPushDebugGroup(enc, SV(name)); }
        void PopDebugGroup() override { wgpuCommandEncoderPopDebugGroup(enc); }
        void ReadBuffer(srhiBuffer* buffer, uint64_t offset, uint64_t size, srhiReadbackCallback callback, void* userData) override;
        void ReadTexture(srhiTexture* texture, uint32_t mip, uint32_t layer, srhiReadbackCallback callback, void* userData) override;
        void EncodeNative(srhiNativeEncodeCallback callback, void* userData) override { callback(enc, userData); }
        void Present(srhiSwapchain* swapchain) override
        {
            WgpuSwapchain* sc = static_cast<WgpuSwapchain*>(swapchain);
            if (!sc->acquired) {
                SRHI_ERROR(device, "srhiCommandBufferPresent: swapchain '%s' was not acquired", sc->label.c_str());
                return;
            }
            presents.push_back(sc);
        }
    };

    // --- Device --------------------------------------------------------------------------------------

    struct WgpuDevice : srhiDevice
    {
        WGPUInstance instance = nullptr;
        WGPUAdapter adapter = nullptr;
        WGPUDevice device = nullptr;
        WGPUQueue queue = nullptr;
        WGPULimits limits = {};
        std::vector<WGPUFeatureName> features;

        WGPUBindGroupLayout emptyLayout = nullptr;
        WGPUBindGroup emptyGroup = nullptr;
        WGPUBindGroupLayout uniformLayout = nullptr;
        std::vector<UniformChunk> freeChunks;
        std::deque<std::unique_ptr<InFlight>> inFlight;

        // Mipmap generation
        WGPUBindGroupLayout mipLayout = nullptr;
        WGPUPipelineLayout mipPipelineLayout = nullptr;
        WGPUShaderModule mipShader = nullptr;
        WGPUSampler mipSampler = nullptr;
        std::unordered_map<uint32_t, WGPURenderPipeline> mipPipelines;

        bool HasFeature(WGPUFeatureName f) const { return std::find(features.begin(), features.end(), f) != features.end(); }

        ~WgpuDevice() override
        {
            WaitIdle();
            for (UniformChunk& c : freeChunks) {
                wgpuBindGroupRelease(c.group);
                wgpuBufferRelease(c.buffer);
            }
            for (auto& [f, p] : mipPipelines)
                wgpuRenderPipelineRelease(p);
            if (mipSampler)
                wgpuSamplerRelease(mipSampler);
            if (mipShader)
                wgpuShaderModuleRelease(mipShader);
            if (mipPipelineLayout)
                wgpuPipelineLayoutRelease(mipPipelineLayout);
            if (mipLayout)
                wgpuBindGroupLayoutRelease(mipLayout);
            if (emptyGroup)
                wgpuBindGroupRelease(emptyGroup);
            if (emptyLayout)
                wgpuBindGroupLayoutRelease(emptyLayout);
            if (uniformLayout)
                wgpuBindGroupLayoutRelease(uniformLayout);
            if (queue)
                wgpuQueueRelease(queue);
            if (device)
                wgpuDeviceRelease(device);
            if (adapter)
                wgpuAdapterRelease(adapter);
            if (instance)
                wgpuInstanceRelease(instance);
        }

        // --- Error scopes: synchronous validation of object creation ---

        void PushScope() { wgpuDevicePushErrorScope(device, WGPUErrorFilter_Validation); }

        /// @brief Pops the validation scope; returns the error message, or an empty string.
        std::string PopScope()
        {
            struct Result
            {
                bool done = false;
                std::string message;
            } result;
            WGPUPopErrorScopeCallbackInfo info = {};
            info.mode = WGPUCallbackMode_AllowProcessEvents;
            info.callback = [](WGPUPopErrorScopeStatus status, WGPUErrorType type, WGPUStringView message, void* u1, void*) {
                Result* r = static_cast<Result*>(u1);
                if (status == WGPUPopErrorScopeStatus_Success && type != WGPUErrorType_NoError)
                    r->message = ToString(message);
                r->done = true;
            };
            info.userdata1 = &result;
            wgpuDevicePopErrorScope(device, info);
            WaitFor(instance, nullptr, &result.done);
            return result.message;
        }

        // --- Uniform chunks ---

        UniformChunk AcquireChunk()
        {
            if (!freeChunks.empty()) {
                UniformChunk c = std::move(freeChunks.back());
                freeChunks.pop_back();
                c.used = 0;
                return c;
            }
            UniformChunk c;
            WGPUBufferDescriptor bd = {};
            bd.label = SV("SimpleRHI uniforms");
            bd.size = kUniformChunkSize;
            bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
            c.buffer = wgpuDeviceCreateBuffer(device, &bd);
            WGPUBindGroupEntry entries[SRHI_MAX_UNIFORM_SLOTS] = {};
            for (uint32_t i = 0; i < SRHI_MAX_UNIFORM_SLOTS; ++i) {
                entries[i].binding = i;
                entries[i].buffer = c.buffer;
                entries[i].offset = 0;
                entries[i].size = SRHI_MAX_UNIFORM_SIZE;
            }
            WGPUBindGroupDescriptor gd = {};
            gd.layout = uniformLayout;
            gd.entryCount = SRHI_MAX_UNIFORM_SLOTS;
            gd.entries = entries;
            c.group = wgpuDeviceCreateBindGroup(device, &gd);
            c.shadow.resize(kUniformChunkSize);
            return c;
        }

        // --- Retirement ---

        void Retire(InFlight& f)
        {
            for (auto& r : f.readbacks) {
                if (r->mapped) {
                    const uint8_t* data = static_cast<const uint8_t*>(wgpuBufferGetConstMappedRange(r->buffer, 0, size_t(r->size)));
                    if (r->querySet) {
                        for (uint32_t idx : r->queries) {
                            uint64_t ns;
                            memcpy(&ns, data + (idx - r->firstQuery) * 8, 8);
                            r->querySet->results[idx] = ns;
                            r->querySet->valid[idx] = 1;
                        }
                    } else if (r->paddedRow != r->tightRow) {
                        std::vector<uint8_t> tight(size_t(r->tightRow) * r->rows);
                        for (uint32_t row = 0; row < r->rows; ++row)
                            memcpy(tight.data() + size_t(row) * r->tightRow, data + size_t(row) * r->paddedRow, r->tightRow);
                        r->callback(tight.data(), tight.size(), r->userData);
                    } else {
                        r->callback(data, uint64_t(r->tightRow) * r->rows ? uint64_t(r->tightRow) * r->rows : r->size, r->userData);
                    }
                    wgpuBufferUnmap(r->buffer);
                } else {
                    SRHI_ERROR(this, "readback mapping failed");
                }
                wgpuBufferRelease(r->buffer);
            }
            if (f.callback)
                f.callback(f.userData);
            for (UniformChunk& c : f.chunks)
                freeChunks.push_back(std::move(c));
        }

        void RetireCompleted()
        {
            while (!inFlight.empty() && inFlight.front()->Complete()) {
                std::unique_ptr<InFlight> f = std::move(inFlight.front());
                inFlight.pop_front();
                Retire(*f);
            }
        }

        // --- srhiDevice ---

        uint32_t GetFormatSupport(srhiFormat format) override;

        void Poll() override
        {
#if SRHI_WGPU_NATIVE
            wgpuDevicePoll(device, false, nullptr);
#endif
            wgpuInstanceProcessEvents(instance);
            RetireCompleted();
        }

        void WaitIdle() override
        {
            while (!inFlight.empty()) {
#if SRHI_WGPU_NATIVE
                wgpuDevicePoll(device, true, nullptr);
#else
                emscripten_sleep(0);
#endif
                wgpuInstanceProcessEvents(instance);
                RetireCompleted();
            }
        }

        void* GetNative() override { return device; }
        void* GetNativeQueue() override { return queue; }

        srhiBuffer* CreateBuffer(const srhiBufferDesc& d) override
        {
            WGPUBufferDescriptor bd = {};
            bd.label = SV(d.label);
            bd.size = d.size;
            bd.usage = WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst;
            if (d.usage & SRHI_BUFFER_USAGE_VERTEX)
                bd.usage |= WGPUBufferUsage_Vertex;
            if (d.usage & SRHI_BUFFER_USAGE_INDEX)
                bd.usage |= WGPUBufferUsage_Index;
            if (d.usage & SRHI_BUFFER_USAGE_UNIFORM)
                bd.usage |= WGPUBufferUsage_Uniform;
            if (d.usage & (SRHI_BUFFER_USAGE_STORAGE | SRHI_BUFFER_USAGE_STORAGE_RW))
                bd.usage |= WGPUBufferUsage_Storage;
            if (d.usage & SRHI_BUFFER_USAGE_INDIRECT)
                bd.usage |= WGPUBufferUsage_Indirect;
            PushScope();
            WGPUBuffer buffer = wgpuDeviceCreateBuffer(device, &bd);
            std::string error = PopScope();
            if (!buffer || !error.empty()) {
                SRHI_ERROR(this, "srhiBufferCreate(%s): %s", d.label ? d.label : "", error.c_str());
                if (buffer)
                    wgpuBufferRelease(buffer);
                return nullptr;
            }
            WgpuBuffer* b = new WgpuBuffer;
            b->buffer = buffer;
            return b;
        }

        void WriteBuffer(srhiBuffer* buffer, uint64_t offset, const void* data, uint64_t size) override
        {
            wgpuQueueWriteBuffer(queue, static_cast<WgpuBuffer*>(buffer)->buffer, offset, data, size_t(size));
        }

        srhiTexture* CreateTexture(const srhiTextureDesc& d) override
        {
            WGPUTextureDescriptor td = {};
            td.label = SV(d.label);
            td.dimension = d.type == SRHI_TEXTURE_TYPE_3D ? WGPUTextureDimension_3D : WGPUTextureDimension_2D;
            td.size = {d.width, d.height, d.depthOrLayers};
            td.format = ToWgpu(d.format);
            td.mipLevelCount = d.mipLevels;
            td.sampleCount = d.sampleCount;
            td.usage = WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst;
            if (d.usage & SRHI_TEXTURE_USAGE_SAMPLED)
                td.usage |= WGPUTextureUsage_TextureBinding;
            if (d.usage & SRHI_TEXTURE_USAGE_STORAGE)
                td.usage |= WGPUTextureUsage_StorageBinding;
            if (d.usage & SRHI_TEXTURE_USAGE_RENDER_TARGET)
                td.usage |= WGPUTextureUsage_RenderAttachment;
            WGPUTextureFormat twin = ToWgpu(GetFormatInfo(d.format).srgbTwin);
            if (twin != WGPUTextureFormat_Undefined) {
                td.viewFormatCount = 1;
                td.viewFormats = &twin;
            }
            PushScope();
            WGPUTexture texture = wgpuDeviceCreateTexture(device, &td);
            std::string error = PopScope();
            if (!texture || !error.empty()) {
                SRHI_ERROR(this, "srhiTextureCreate(%s): %s", d.label ? d.label : "", error.c_str());
                if (texture)
                    wgpuTextureRelease(texture);
                return nullptr;
            }
            WgpuTexture* t = new WgpuTexture;
            t->texture = texture;
            return t;
        }

        void WriteTexture(srhiTexture* texture, uint32_t mip, uint32_t layer, const srhiTextureRegion& r, const void* data,
                          uint32_t bytesPerRow, uint32_t rowsPerImage) override
        {
            WgpuTexture* t = static_cast<WgpuTexture*>(texture);
            bool is3D = t->desc.type == SRHI_TEXTURE_TYPE_3D;
            WGPUTexelCopyTextureInfo dst = {};
            dst.texture = t->texture;
            dst.mipLevel = mip;
            dst.origin = {r.x, r.y, is3D ? r.z : layer};
            dst.aspect = WGPUTextureAspect_All;
            WGPUTexelCopyBufferLayout layout = {};
            layout.bytesPerRow = bytesPerRow;
            layout.rowsPerImage = rowsPerImage;
            WGPUExtent3D size = WgpuBlitEncoder::Extent(texture, r.width, r.height, is3D ? r.depth : 1);
            uint64_t dataSize = uint64_t(bytesPerRow) * rowsPerImage * (size.depthOrArrayLayers - 1) +
                                uint64_t(bytesPerRow) * (rowsPerImage - 1) + RowBytes(t->desc.format, r.width);
            wgpuQueueWriteTexture(queue, &dst, data, size_t(dataSize), &layout, &size);
        }

        srhiTextureView* CreateTextureView(srhiTexture* texture, const srhiTextureViewDesc& d) override
        {
            WgpuTexture* t = static_cast<WgpuTexture*>(texture);
            WgpuTextureView* v = new WgpuTextureView;
            WGPUTextureViewDescriptor vd = {};
            vd.label = SV(d.label);
            vd.format = ToWgpu(d.format);
            vd.dimension = ToWgpu(d.type);
            vd.baseMipLevel = d.baseMip;
            vd.mipLevelCount = d.mipCount;
            vd.baseArrayLayer = d.baseLayer;
            vd.arrayLayerCount = d.layerCount;
            vd.aspect = WGPUTextureAspect_All;
            if (GetFormatInfo(d.format).stencil) {
                // Sampling a depth-stencil texture reads its depth aspect only.
                vd.format = WGPUTextureFormat_Depth32Float;
                vd.aspect = WGPUTextureAspect_DepthOnly;
            }
            v->view = wgpuTextureCreateView(t->texture, &vd);
            if (!v->view) {
                SRHI_ERROR(this, "srhiTextureViewCreate(%s): creation failed", t->label.c_str());
                delete v;
                return nullptr;
            }
            return v;
        }

        srhiSampler* CreateSampler(const srhiSamplerDesc& d) override
        {
            WGPUSamplerDescriptor sd = {};
            sd.label = SV(d.label);
            sd.addressModeU = ToWgpu(d.addressU);
            sd.addressModeV = ToWgpu(d.addressV);
            sd.addressModeW = ToWgpu(d.addressW);
            sd.magFilter = d.magFilter == SRHI_FILTER_LINEAR ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
            sd.minFilter = d.minFilter == SRHI_FILTER_LINEAR ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
            sd.mipmapFilter = d.mipFilter == SRHI_FILTER_LINEAR ? WGPUMipmapFilterMode_Linear : WGPUMipmapFilterMode_Nearest;
            sd.lodMinClamp = d.lodMinClamp;
            sd.lodMaxClamp = std::min(d.lodMaxClamp, 32.0f);
            sd.compare = d.compareEnable ? ToWgpu(d.compareFunction) : WGPUCompareFunction_Undefined;
            sd.maxAnisotropy = uint16_t(d.maxAnisotropy);
            WgpuSampler* s = new WgpuSampler;
            s->sampler = wgpuDeviceCreateSampler(device, &sd);
            return s;
        }

        srhiShaderModule* CreateShaderModule(Package&& package, const char* label) override
        {
            for (const PkgEntry& e : package.entries)
                if (!e.FindBlob(BlobKind::WGSL)) {
                    SRHI_ERROR(this, "srhiShaderModuleCreate(%s): entry point '%s' has no WGSL code (compile with the wgsl target)",
                               label ? label : "", e.name.c_str());
                    return nullptr;
                }
            WgpuShaderModule* m = new WgpuShaderModule;
            m->package = std::move(package);
            return m;
        }

        WGPUShaderModule GetModule(srhiShaderModule* module, const PkgEntry* entry)
        {
            WgpuShaderModule* m = static_cast<WgpuShaderModule*>(module);
            auto it = m->modules.find(entry->name);
            if (it != m->modules.end())
                return it->second;
            const PkgBlob* blob = entry->FindBlob(BlobKind::WGSL);
            std::string code(blob->data.begin(), blob->data.end());
            WGPUShaderSourceWGSL source = {};
            source.chain.sType = WGPUSType_ShaderSourceWGSL;
            source.code = SV(code);
            WGPUShaderModuleDescriptor md = {};
            md.nextInChain = &source.chain;
            std::string label = m->label + ":" + entry->name;
            md.label = SV(label);
            PushScope();
            WGPUShaderModule sm = wgpuDeviceCreateShaderModule(device, &md);
            std::string error = PopScope();
            if (!error.empty()) {
                SRHI_ERROR(this, "shader module '%s': WGSL for '%s' failed to compile: %s", m->label.c_str(), entry->name.c_str(),
                           error.c_str());
                if (sm)
                    wgpuShaderModuleRelease(sm);
                sm = nullptr;
            }
            m->modules[entry->name] = sm;
            return sm;
        }

        WGPUBindGroupLayout CreateBGL(const std::vector<srhiResourceSetLayoutEntry>& entries, const char* label)
        {
            std::vector<WGPUBindGroupLayoutEntry> out(entries.size());
            for (size_t i = 0; i < entries.size(); ++i) {
                const srhiResourceSetLayoutEntry& e = entries[i];
                WGPUBindGroupLayoutEntry& o = out[i];
                o = {};
                o.binding = e.binding;
                o.visibility = WGPUShaderStage_None;
                if (e.stages & SRHI_SHADER_STAGE_VERTEX)
                    o.visibility |= WGPUShaderStage_Vertex;
                if (e.stages & SRHI_SHADER_STAGE_FRAGMENT)
                    o.visibility |= WGPUShaderStage_Fragment;
                if (e.stages & SRHI_SHADER_STAGE_COMPUTE)
                    o.visibility |= WGPUShaderStage_Compute;
                switch (e.type) {
                case SRHI_BINDING_TYPE_UNIFORM_BUFFER: o.buffer.type = WGPUBufferBindingType_Uniform; break;
                case SRHI_BINDING_TYPE_STORAGE_BUFFER: o.buffer.type = WGPUBufferBindingType_ReadOnlyStorage; break;
                case SRHI_BINDING_TYPE_STORAGE_BUFFER_RW: o.buffer.type = WGPUBufferBindingType_Storage; break;
                case SRHI_BINDING_TYPE_TEXTURE:
                    switch (e.sampleType) {
                    case SRHI_TEXTURE_SAMPLE_TYPE_FLOAT: o.texture.sampleType = WGPUTextureSampleType_Float; break;
                    case SRHI_TEXTURE_SAMPLE_TYPE_UNFILTERABLE_FLOAT: o.texture.sampleType = WGPUTextureSampleType_UnfilterableFloat; break;
                    case SRHI_TEXTURE_SAMPLE_TYPE_DEPTH: o.texture.sampleType = WGPUTextureSampleType_Depth; break;
                    case SRHI_TEXTURE_SAMPLE_TYPE_SINT: o.texture.sampleType = WGPUTextureSampleType_Sint; break;
                    case SRHI_TEXTURE_SAMPLE_TYPE_UINT: o.texture.sampleType = WGPUTextureSampleType_Uint; break;
                    }
                    o.texture.viewDimension = ToWgpu(e.textureType);
                    o.texture.multisampled = e.multisampled ? 1 : 0;
                    break;
                case SRHI_BINDING_TYPE_STORAGE_TEXTURE:
                    o.storageTexture.access = WGPUStorageTextureAccess_WriteOnly;
                    o.storageTexture.format = ToWgpu(e.storageFormat);
                    o.storageTexture.viewDimension = ToWgpu(e.textureType);
                    break;
                case SRHI_BINDING_TYPE_SAMPLER:
                    o.sampler.type = e.samplerType == SRHI_SAMPLER_BINDING_TYPE_COMPARISON      ? WGPUSamplerBindingType_Comparison
                                   : e.samplerType == SRHI_SAMPLER_BINDING_TYPE_NON_FILTERING ? WGPUSamplerBindingType_NonFiltering
                                                                                              : WGPUSamplerBindingType_Filtering;
                    break;
                }
            }
            WGPUBindGroupLayoutDescriptor d = {};
            d.label = SV(label);
            d.entryCount = out.size();
            d.entries = out.data();
            return wgpuDeviceCreateBindGroupLayout(device, &d);
        }

        srhiResourceSetLayout* CreateResourceSetLayout(std::vector<srhiResourceSetLayoutEntry>&& entries, const char* label) override
        {
            PushScope();
            WGPUBindGroupLayout bgl = CreateBGL(entries, label);
            std::string error = PopScope();
            if (!error.empty()) {
                SRHI_ERROR(this, "srhiResourceSetLayoutCreate(%s): %s", label ? label : "", error.c_str());
                wgpuBindGroupLayoutRelease(bgl);
                return nullptr;
            }
            WgpuResourceSetLayout* l = new WgpuResourceSetLayout;
            l->entries = std::move(entries);
            l->layout = bgl;
            return l;
        }

        srhiResourceSet* CreateResourceSet(srhiResourceSetLayout* layout, std::vector<srhiResourceBinding>&& bindings,
                                           const char* label) override
        {
            std::vector<WGPUBindGroupEntry> entries(bindings.size());
            for (size_t i = 0; i < bindings.size(); ++i) {
                const srhiResourceBinding& b = bindings[i];
                WGPUBindGroupEntry& e = entries[i];
                e = {};
                e.binding = b.binding;
                switch (layout->entries[i].type) {
                case SRHI_BINDING_TYPE_UNIFORM_BUFFER:
                case SRHI_BINDING_TYPE_STORAGE_BUFFER:
                case SRHI_BINDING_TYPE_STORAGE_BUFFER_RW:
                    e.buffer = static_cast<WgpuBuffer*>(b.buffer)->buffer;
                    e.offset = b.offset;
                    e.size = b.size;
                    break;
                case SRHI_BINDING_TYPE_TEXTURE:
                case SRHI_BINDING_TYPE_STORAGE_TEXTURE: e.textureView = static_cast<WgpuTextureView*>(b.textureView)->view; break;
                case SRHI_BINDING_TYPE_SAMPLER: e.sampler = static_cast<WgpuSampler*>(b.sampler)->sampler; break;
                }
            }
            WGPUBindGroupDescriptor d = {};
            d.label = SV(label);
            d.layout = static_cast<WgpuResourceSetLayout*>(layout)->layout;
            d.entryCount = entries.size();
            d.entries = entries.data();
            PushScope();
            WGPUBindGroup group = wgpuDeviceCreateBindGroup(device, &d);
            std::string error = PopScope();
            if (!error.empty()) {
                SRHI_ERROR(this, "srhiResourceSetCreate(%s): %s", label ? label : "", error.c_str());
                wgpuBindGroupRelease(group);
                return nullptr;
            }
            WgpuResourceSet* s = new WgpuResourceSet;
            s->bindings = std::move(bindings);
            s->group = group;
            return s;
        }

        /// @brief Groups 0..2 from the set layouts (empty where NULL), group 3 the uniform layout if used.
        WGPUPipelineLayout CreatePipelineLayout(const PipelineLayoutInfo& layout, uint32_t& outGroupCount, bool& outUsesUniforms)
        {
            outUsesUniforms = false;
            for (const BindingSlot& u : layout.uniforms)
                outUsesUniforms |= u.used;
            // Sets the shaders never touch get the empty layout, so (like on Metal / D3D11) they need not be bound.
            uint32_t count = 0;
            for (uint32_t s = 0; s < SRHI_MAX_RESOURCE_SETS; ++s)
                if (SetUsed(layout, s))
                    count = s + 1;
            if (outUsesUniforms)
                count = SRHI_UNIFORM_SET_INDEX + 1;
            WGPUBindGroupLayout groups[SRHI_UNIFORM_SET_INDEX + 1] = {};
            for (uint32_t s = 0; s < count; ++s) {
                if (s == SRHI_UNIFORM_SET_INDEX)
                    groups[s] = uniformLayout;
                else
                    groups[s] = SetUsed(layout, s) ? static_cast<WgpuResourceSetLayout*>(layout.setLayouts[s])->layout : emptyLayout;
            }
            WGPUPipelineLayoutDescriptor d = {};
            d.bindGroupLayoutCount = count;
            d.bindGroupLayouts = groups;
            outGroupCount = count;
            return wgpuDeviceCreatePipelineLayout(device, &d);
        }

        srhiRenderPipeline* CreateRenderPipeline(const srhiRenderPipelineDesc& d, PipelineLayoutInfo&& layout) override
        {
            const Package& pkg = d.module->package;
            WGPUShaderModule vsModule = GetModule(d.module, pkg.FindEntry(d.vertexEntry));
            WGPUShaderModule fsModule = d.fragmentEntry ? GetModule(d.module, pkg.FindEntry(d.fragmentEntry)) : nullptr;
            if (!vsModule || (d.fragmentEntry && !fsModule))
                return nullptr;

            WgpuRenderPipeline* p = new WgpuRenderPipeline;
            WGPUPipelineLayout pipelineLayout = CreatePipelineLayout(layout, p->groupCount, p->usesUniforms);

            // Vertex buffers.
            std::vector<WGPUVertexAttribute> attributes[SRHI_MAX_VERTEX_BUFFERS];
            for (uint32_t i = 0; i < d.vertexLayout.attributeCount; ++i) {
                const srhiVertexAttribute& a = d.vertexLayout.attributes[i];
                WGPUVertexAttribute va = {};
                va.format = ToWgpu(a.format);
                va.offset = a.offset;
                va.shaderLocation = a.location;
                attributes[a.bufferIndex].push_back(va);
            }
            WGPUVertexBufferLayout buffers[SRHI_MAX_VERTEX_BUFFERS] = {};
            for (uint32_t i = 0; i < d.vertexLayout.bufferCount; ++i) {
                buffers[i].arrayStride = d.vertexLayout.buffers[i].stride;
                buffers[i].stepMode = d.vertexLayout.buffers[i].stepMode == SRHI_VERTEX_STEP_MODE_INSTANCE ? WGPUVertexStepMode_Instance
                                                                                                          : WGPUVertexStepMode_Vertex;
                buffers[i].attributeCount = attributes[i].size();
                buffers[i].attributes = attributes[i].data();
            }

            const std::string vsName = pkg.FindEntry(d.vertexEntry)->FindBlob(BlobKind::WGSL)->functionName;
            WGPURenderPipelineDescriptor pd = {};
            pd.label = SV(d.label);
            pd.layout = pipelineLayout;
            pd.vertex.module = vsModule;
            pd.vertex.entryPoint = SV(vsName);
            pd.vertex.bufferCount = d.vertexLayout.bufferCount;
            pd.vertex.buffers = buffers;

            bool strip = d.topology == SRHI_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP || d.topology == SRHI_PRIMITIVE_TOPOLOGY_LINE_STRIP;
            switch (d.topology) {
            case SRHI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST: pd.primitive.topology = WGPUPrimitiveTopology_TriangleList; break;
            case SRHI_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: pd.primitive.topology = WGPUPrimitiveTopology_TriangleStrip; break;
            case SRHI_PRIMITIVE_TOPOLOGY_LINE_LIST: pd.primitive.topology = WGPUPrimitiveTopology_LineList; break;
            case SRHI_PRIMITIVE_TOPOLOGY_LINE_STRIP: pd.primitive.topology = WGPUPrimitiveTopology_LineStrip; break;
            case SRHI_PRIMITIVE_TOPOLOGY_POINT_LIST: pd.primitive.topology = WGPUPrimitiveTopology_PointList; break;
            }
            pd.primitive.stripIndexFormat = strip ? (d.stripIndexType == SRHI_INDEX_TYPE_UINT16 ? WGPUIndexFormat_Uint16 : WGPUIndexFormat_Uint32)
                                                  : WGPUIndexFormat_Undefined;
            pd.primitive.frontFace = d.raster.frontFace == SRHI_FRONT_FACE_CLOCKWISE ? WGPUFrontFace_CW : WGPUFrontFace_CCW;
            pd.primitive.cullMode = d.raster.cullMode == SRHI_CULL_MODE_NONE    ? WGPUCullMode_None
                                  : d.raster.cullMode == SRHI_CULL_MODE_FRONT ? WGPUCullMode_Front
                                                                              : WGPUCullMode_Back;
            pd.primitive.unclippedDepth = d.raster.depthClamp ? 1 : 0;

            WGPUDepthStencilState ds = {};
            const srhiDepthStencilState& s = d.depthStencil;
            if (s.format != SRHI_FORMAT_UNDEFINED) {
                ds.format = ToWgpu(s.format);
                ds.depthWriteEnabled = s.depthTestEnable && s.depthWriteEnable ? WGPUOptionalBool_True : WGPUOptionalBool_False;
                ds.depthCompare = s.depthTestEnable ? ToWgpu(s.depthCompare) : WGPUCompareFunction_Always;
                auto face = [&](const srhiStencilFaceState& f) {
                    WGPUStencilFaceState o = {};
                    o.compare = s.stencilEnable ? ToWgpu(f.compare) : WGPUCompareFunction_Always;
                    o.failOp = s.stencilEnable ? ToWgpu(f.failOp) : WGPUStencilOperation_Keep;
                    o.depthFailOp = s.stencilEnable ? ToWgpu(f.depthFailOp) : WGPUStencilOperation_Keep;
                    o.passOp = s.stencilEnable ? ToWgpu(f.passOp) : WGPUStencilOperation_Keep;
                    return o;
                };
                ds.stencilFront = face(s.stencilFront);
                ds.stencilBack = face(s.stencilBack);
                ds.stencilReadMask = s.stencilEnable ? s.stencilReadMask : 0;
                ds.stencilWriteMask = s.stencilEnable ? s.stencilWriteMask : 0;
                ds.depthBias = d.raster.depthBias;
                ds.depthBiasSlopeScale = d.raster.depthBiasSlopeScale;
                ds.depthBiasClamp = d.raster.depthBiasClamp;
                pd.depthStencil = &ds;
            }

            pd.multisample.count = d.sampleCount;
            pd.multisample.mask = 0xFFFFFFFFu;
            pd.multisample.alphaToCoverageEnabled = d.alphaToCoverage ? 1 : 0;

            WGPUColorTargetState targets[SRHI_MAX_COLOR_ATTACHMENTS] = {};
            WGPUBlendState blends[SRHI_MAX_COLOR_ATTACHMENTS] = {};
            for (uint32_t i = 0; i < d.colorTargetCount; ++i) {
                const srhiColorTargetState& ct = d.colorTargets[i];
                targets[i].format = ToWgpu(ct.format);
                targets[i].writeMask = ct.writeMask;
                if (ct.blend.enable) {
                    blends[i].color = ToWgpu(ct.blend.srcColor, ct.blend.dstColor, ct.blend.colorOp);
                    blends[i].alpha = ToWgpu(ct.blend.srcAlpha, ct.blend.dstAlpha, ct.blend.alphaOp);
                    targets[i].blend = &blends[i];
                }
            }
            WGPUFragmentState fs = {};
            std::string fsName;
            if (fsModule) {
                fsName = pkg.FindEntry(d.fragmentEntry)->FindBlob(BlobKind::WGSL)->functionName;
                fs.module = fsModule;
                fs.entryPoint = SV(fsName);
                fs.targetCount = d.colorTargetCount;
                fs.targets = targets;
                pd.fragment = &fs;
            }

            PushScope();
            p->pipeline = wgpuDeviceCreateRenderPipeline(device, &pd);
            std::string error = PopScope();
            wgpuPipelineLayoutRelease(pipelineLayout);
            if (!error.empty()) {
                SRHI_ERROR(this, "srhiRenderPipelineCreate(%s): %s", d.label ? d.label : "", error.c_str());
                delete p;
                return nullptr;
            }
            p->layout = std::move(layout);
            return p;
        }

        srhiComputePipeline* CreateComputePipeline(const srhiComputePipelineDesc& d, PipelineLayoutInfo&& layout) override
        {
            const PkgEntry* entry = d.module->package.FindEntry(d.entry);
            WGPUShaderModule module = GetModule(d.module, entry);
            if (!module)
                return nullptr;
            WgpuComputePipeline* p = new WgpuComputePipeline;
            WGPUPipelineLayout pipelineLayout = CreatePipelineLayout(layout, p->groupCount, p->usesUniforms);
            const std::string name = entry->FindBlob(BlobKind::WGSL)->functionName;
            WGPUComputePipelineDescriptor pd = {};
            pd.label = SV(d.label);
            pd.layout = pipelineLayout;
            pd.compute.module = module;
            pd.compute.entryPoint = SV(name);
            PushScope();
            p->pipeline = wgpuDeviceCreateComputePipeline(device, &pd);
            std::string error = PopScope();
            wgpuPipelineLayoutRelease(pipelineLayout);
            if (!error.empty()) {
                SRHI_ERROR(this, "srhiComputePipelineCreate(%s): %s", d.label ? d.label : "", error.c_str());
                delete p;
                return nullptr;
            }
            p->layout = std::move(layout);
            return p;
        }

        srhiQuerySet* CreateQuerySet(uint32_t count, const char* label) override
        {
            WGPUQuerySetDescriptor qd = {};
            qd.label = SV(label);
            qd.type = WGPUQueryType_Timestamp;
            qd.count = count;
            WgpuQuerySet* q = new WgpuQuerySet;
            q->set = wgpuDeviceCreateQuerySet(device, &qd);
            WGPUBufferDescriptor bd = {};
            bd.size = AlignUp(uint64_t(count) * 8, 256);
            bd.usage = WGPUBufferUsage_QueryResolve | WGPUBufferUsage_CopySrc;
            q->resolve = wgpuDeviceCreateBuffer(device, &bd);
            q->results.assign(count, 0);
            q->valid.assign(count, 0);
            return q;
        }

        srhiCommandBuffer* BeginCommandBuffer(const char* label) override
        {
            WgpuCommandBuffer* c = new WgpuCommandBuffer;
            c->dev = this;
            WGPUCommandEncoderDescriptor ed = {};
            ed.label = SV(label);
            c->enc = wgpuDeviceCreateCommandEncoder(device, &ed);
            return c;
        }

        srhiSwapchain* CreateSwapchain(const srhiSwapchainDesc& d) override;

        WGPURenderPipeline MipPipeline(WGPUTextureFormat format);
    };

    // =================================================================================================
    // Out-of-line implementations
    // =================================================================================================

    WGPUTextureView WgpuTextureView::RenderView()
    {
        if (renderView)
            return renderView;
        WgpuTexture* t = static_cast<WgpuTexture*>(texture);
        WGPUTextureViewDescriptor vd = {};
        vd.format = ToWgpu(desc.format);
        bool is3D = t->desc.type == SRHI_TEXTURE_TYPE_3D;
        vd.dimension = is3D ? WGPUTextureViewDimension_3D : WGPUTextureViewDimension_2D;
        vd.baseMipLevel = desc.baseMip;
        vd.mipLevelCount = 1;
        vd.baseArrayLayer = is3D ? 0 : desc.baseLayer;
        vd.arrayLayerCount = 1;
        vd.aspect = WGPUTextureAspect_All;
        renderView = wgpuTextureCreateView(t->texture, &vd);
        return renderView;
    }

    uint32_t WgpuDevice::GetFormatSupport(srhiFormat format)
    {
        const FormatInfo& fi = GetFormatInfo(format);
        if (ToWgpu(format) == WGPUTextureFormat_Undefined)
            return 0;
        constexpr uint32_t S = SRHI_FORMAT_SUPPORT_SAMPLE, F = SRHI_FORMAT_SUPPORT_FILTER, R = SRHI_FORMAT_SUPPORT_RENDER_TARGET,
                           B = SRHI_FORMAT_SUPPORT_BLEND, D = SRHI_FORMAT_SUPPORT_DEPTH_STENCIL, ST = SRHI_FORMAT_SUPPORT_STORAGE,
                           M = SRHI_FORMAT_SUPPORT_MSAA, RS = SRHI_FORMAT_SUPPORT_RESOLVE;
        if (fi.compressed) {
            bool bc = format >= SRHI_FORMAT_BC1_UNORM && format <= SRHI_FORMAT_BC7_UNORM_SRGB;
            bool ok = bc ? HasFeature(WGPUFeatureName_TextureCompressionBC) : HasFeature(WGPUFeatureName_TextureCompressionASTC);
            return ok ? (S | F) : 0;
        }
        bool f32Filter = HasFeature(WGPUFeatureName_Float32Filterable);
        bool f32Blend = HasFeature(WGPUFeatureName_Float32Blendable);
        switch (format) {
        case SRHI_FORMAT_R8_UNORM:
        case SRHI_FORMAT_RG8_UNORM:
        case SRHI_FORMAT_RGBA8_UNORM_SRGB:
        case SRHI_FORMAT_BGRA8_UNORM_SRGB:
        case SRHI_FORMAT_R16_FLOAT:
        case SRHI_FORMAT_RG16_FLOAT:
        case SRHI_FORMAT_RGB10A2_UNORM: return S | F | R | B | M | RS;
        case SRHI_FORMAT_RGBA8_UNORM:
        case SRHI_FORMAT_RGBA16_FLOAT: return S | F | R | B | M | RS | ST;
        case SRHI_FORMAT_BGRA8_UNORM: return S | F | R | B | M | RS | (HasFeature(WGPUFeatureName_BGRA8UnormStorage) ? ST : 0);
        case SRHI_FORMAT_RGBA8_SNORM: return S | F | ST;
        case SRHI_FORMAT_RGBA8_UINT: return S | R | M | ST;
        case SRHI_FORMAT_R16_UINT: return S | R | M;
        case SRHI_FORMAT_R32_FLOAT: return S | R | M | ST | (f32Filter ? F : 0) | (f32Blend ? B : 0);
        case SRHI_FORMAT_R32_UINT:
        case SRHI_FORMAT_R32_SINT: return S | R | ST;
        case SRHI_FORMAT_RG32_FLOAT:
        case SRHI_FORMAT_RGBA32_FLOAT: return S | R | ST | (f32Filter ? F : 0) | (f32Blend ? B : 0);
        case SRHI_FORMAT_RG32_UINT:
        case SRHI_FORMAT_RGBA32_UINT: return S | R | ST;
        case SRHI_FORMAT_RG11B10_FLOAT: return S | F | (HasFeature(WGPUFeatureName_RG11B10UfloatRenderable) ? (R | B | M | RS) : 0);
        case SRHI_FORMAT_D16_UNORM:
        case SRHI_FORMAT_D32_FLOAT: return S | D | M;
        case SRHI_FORMAT_D32_FLOAT_S8_UINT: return HasFeature(WGPUFeatureName_Depth32FloatStencil8) ? (S | D | M) : 0;
        default: return 0;
        }
    }

    WGPURenderPipeline WgpuDevice::MipPipeline(WGPUTextureFormat format)
    {
        auto it = mipPipelines.find(uint32_t(format));
        if (it != mipPipelines.end())
            return it->second;
        if (!mipShader) {
            static const char* kSource = R"(
@group(0) @binding(0) var srcTexture : texture_2d<f32>;
@group(0) @binding(1) var srcSampler : sampler;
struct VSOut { @builtin(position) position : vec4<f32>, @location(0) uv : vec2<f32> };
@vertex fn vsMain(@builtin(vertex_index) id : u32) -> VSOut {
    let uv = vec2<f32>(f32((id << 1u) & 2u), f32(id & 2u));
    var o : VSOut;
    o.position = vec4<f32>(uv * vec2<f32>(2.0, -2.0) + vec2<f32>(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}
@fragment fn fsMain(i : VSOut) -> @location(0) vec4<f32> {
    return textureSampleLevel(srcTexture, srcSampler, i.uv, 0.0);
}
)";
            WGPUShaderSourceWGSL source = {};
            source.chain.sType = WGPUSType_ShaderSourceWGSL;
            source.code = SV(kSource);
            WGPUShaderModuleDescriptor md = {};
            md.nextInChain = &source.chain;
            md.label = SV("SimpleRHI mipgen");
            mipShader = wgpuDeviceCreateShaderModule(device, &md);

            WGPUBindGroupLayoutEntry entries[2] = {};
            entries[0].binding = 0;
            entries[0].visibility = WGPUShaderStage_Fragment;
            entries[0].texture.sampleType = WGPUTextureSampleType_Float;
            entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
            entries[1].binding = 1;
            entries[1].visibility = WGPUShaderStage_Fragment;
            entries[1].sampler.type = WGPUSamplerBindingType_Filtering;
            WGPUBindGroupLayoutDescriptor ld = {};
            ld.entryCount = 2;
            ld.entries = entries;
            mipLayout = wgpuDeviceCreateBindGroupLayout(device, &ld);
            WGPUPipelineLayoutDescriptor pld = {};
            pld.bindGroupLayoutCount = 1;
            pld.bindGroupLayouts = &mipLayout;
            mipPipelineLayout = wgpuDeviceCreatePipelineLayout(device, &pld);

            WGPUSamplerDescriptor sd = {};
            sd.addressModeU = sd.addressModeV = sd.addressModeW = WGPUAddressMode_ClampToEdge;
            sd.magFilter = sd.minFilter = WGPUFilterMode_Linear;
            sd.mipmapFilter = WGPUMipmapFilterMode_Nearest;
            sd.lodMaxClamp = 32.0f;
            sd.maxAnisotropy = 1;
            mipSampler = wgpuDeviceCreateSampler(device, &sd);
        }
        WGPUColorTargetState target = {};
        target.format = format;
        target.writeMask = WGPUColorWriteMask_All;
        WGPUFragmentState fs = {};
        fs.module = mipShader;
        fs.entryPoint = SV("fsMain");
        fs.targetCount = 1;
        fs.targets = &target;
        WGPURenderPipelineDescriptor pd = {};
        pd.label = SV("SimpleRHI mipgen");
        pd.layout = mipPipelineLayout;
        pd.vertex.module = mipShader;
        pd.vertex.entryPoint = SV("vsMain");
        pd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
        pd.primitive.frontFace = WGPUFrontFace_CCW;
        pd.primitive.cullMode = WGPUCullMode_None;
        pd.multisample.count = 1;
        pd.multisample.mask = 0xFFFFFFFFu;
        pd.fragment = &fs;
        WGPURenderPipeline p = wgpuDeviceCreateRenderPipeline(device, &pd);
        mipPipelines[uint32_t(format)] = p;
        return p;
    }

    void WgpuBlitEncoder::GenerateMipmaps(srhiTexture* texture)
    {
        WgpuDevice* dev = owner->dev;
        WgpuTexture* t = static_cast<WgpuTexture*>(texture);
        WGPUTextureFormat format = ToWgpu(t->desc.format);
        WGPURenderPipeline pipeline = dev->MipPipeline(format);
        for (uint32_t layer = 0; layer < t->LayerCount(); ++layer)
            for (uint32_t mip = 1; mip < t->desc.mipLevels; ++mip) {
                WGPUTextureViewDescriptor vd = {};
                vd.format = format;
                vd.dimension = WGPUTextureViewDimension_2D;
                vd.baseMipLevel = mip - 1;
                vd.mipLevelCount = 1;
                vd.baseArrayLayer = layer;
                vd.arrayLayerCount = 1;
                vd.aspect = WGPUTextureAspect_All;
                WGPUTextureView src = wgpuTextureCreateView(t->texture, &vd);
                vd.baseMipLevel = mip;
                WGPUTextureView dst = wgpuTextureCreateView(t->texture, &vd);

                WGPUBindGroupEntry entries[2] = {};
                entries[0].binding = 0;
                entries[0].textureView = src;
                entries[1].binding = 1;
                entries[1].sampler = dev->mipSampler;
                WGPUBindGroupDescriptor gd = {};
                gd.layout = dev->mipLayout;
                gd.entryCount = 2;
                gd.entries = entries;
                WGPUBindGroup group = wgpuDeviceCreateBindGroup(dev->device, &gd);

                WGPURenderPassColorAttachment ca = {};
                ca.view = dst;
                ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
                ca.loadOp = WGPULoadOp_Clear;
                ca.storeOp = WGPUStoreOp_Store;
                WGPURenderPassDescriptor rp = {};
                rp.colorAttachmentCount = 1;
                rp.colorAttachments = &ca;
                WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
                wgpuRenderPassEncoderSetPipeline(pass, pipeline);
                wgpuRenderPassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
                wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
                wgpuRenderPassEncoderEnd(pass);
                wgpuRenderPassEncoderRelease(pass);
                wgpuBindGroupRelease(group);
                wgpuTextureViewRelease(src);
                wgpuTextureViewRelease(dst);
            }
    }

    // --- Uniforms / binding flush ---

    WGPUBindGroup WgpuCommandBuffer::UploadUniforms(BindingState& state, const PipelineLayoutInfo& layout, const char* label,
                                                     uint32_t offsets[SRHI_MAX_UNIFORM_SLOTS])
    {
        uint32_t needed = 0;
        for (uint32_t u = 0; u < SRHI_MAX_UNIFORM_SLOTS; ++u) {
            offsets[u] = 0;
            if (!layout.uniforms[u].used)
                continue;
            if (state.uniforms[u].empty()) {
                SRHI_ERROR(device, "draw/dispatch with pipeline '%s': uniform slot %u was never set", label, u);
                return nullptr;
            }
            if (state.uniforms[u].size() < layout.uniformSizes[u]) {
                SRHI_ERROR(device, "pipeline '%s': uniform slot %u needs %u bytes, %zu were set", label, u, layout.uniformSizes[u],
                           state.uniforms[u].size());
                return nullptr;
            }
            ++needed;
        }
        if (currentChunk < 0 || pending->chunks[currentChunk].used + uint64_t(needed) * SRHI_MAX_UNIFORM_SIZE > kUniformChunkSize) {
            pending->chunks.push_back(dev->AcquireChunk());
            currentChunk = int(pending->chunks.size()) - 1;
        }
        UniformChunk& chunk = pending->chunks[currentChunk];
        for (uint32_t u = 0; u < SRHI_MAX_UNIFORM_SLOTS; ++u) {
            if (!layout.uniforms[u].used)
                continue;
            const std::vector<uint8_t>& data = state.uniforms[u];
            memcpy(chunk.shadow.data() + chunk.used, data.data(), data.size());
            offsets[u] = uint32_t(chunk.used);
            chunk.used = AlignUp(chunk.used + data.size(), 256);
        }
        return chunk.group;
    }

    template <typename SetBindGroupFn>
    bool FlushBindings(WgpuCommandBuffer* cmd, BindingState& state, const PipelineLayoutInfo& layout, const char* label,
                       uint32_t groupCount, bool usesUniforms, SetBindGroupFn&& setBindGroup)
    {
        for (uint32_t s = 0; s < std::min<uint32_t>(groupCount, SRHI_MAX_RESOURCE_SETS); ++s) {
            if (!state.setDirty[s])
                continue;
            if (!SetUsed(layout, s)) {
                setBindGroup(s, cmd->dev->emptyGroup, 0, nullptr);
                state.setDirty[s] = false;
                continue;
            }
            srhiResourceSet* set = state.sets[s];
            if (!set) {
                SRHI_ERROR(cmd->device, "draw/dispatch with pipeline '%s': resource set %u is not bound", label, s);
                return false;
            }
            setBindGroup(s, static_cast<WgpuResourceSet*>(set)->group, 0, nullptr);
            state.setDirty[s] = false;
        }
        if (usesUniforms && state.uniformsDirty) {
            uint32_t offsets[SRHI_MAX_UNIFORM_SLOTS];
            WGPUBindGroup group = cmd->UploadUniforms(state, layout, label, offsets);
            if (!group)
                return false;
            setBindGroup(SRHI_UNIFORM_SET_INDEX, group, SRHI_MAX_UNIFORM_SLOTS, offsets);
            state.uniformsDirty = false;
        }
        return true;
    }

    bool WgpuRenderEncoder::Flush()
    {
        WgpuRenderPipeline* p = static_cast<WgpuRenderPipeline*>(pipeline);
        WGPURenderPassEncoder e = pass;
        return FlushBindings(owner, bindings, p->layout, p->label.c_str(), p->groupCount, p->usesUniforms,
                             [e](uint32_t index, WGPUBindGroup group, size_t count, const uint32_t* offsets) {
                                 wgpuRenderPassEncoderSetBindGroup(e, index, group, count, offsets);
                             });
    }

    bool WgpuComputeEncoder::Flush()
    {
        WgpuComputePipeline* p = static_cast<WgpuComputePipeline*>(pipeline);
        WGPUComputePassEncoder e = pass;
        return FlushBindings(owner, bindings, p->layout, p->label.c_str(), p->groupCount, p->usesUniforms,
                             [e](uint32_t index, WGPUBindGroup group, size_t count, const uint32_t* offsets) {
                                 wgpuComputePassEncoderSetBindGroup(e, index, group, count, offsets);
                             });
    }

    // --- Render / compute passes ---

    srhiRenderEncoder* WgpuCommandBuffer::BeginRender(const srhiRenderPassDesc& desc)
    {
        WGPURenderPassColorAttachment colors[SRHI_MAX_COLOR_ATTACHMENTS] = {};
        uint32_t width = 0, height = 0;
        for (uint32_t i = 0; i < desc.colorAttachmentCount; ++i) {
            const srhiColorAttachment& c = desc.colorAttachments[i];
            WgpuTextureView* v = static_cast<WgpuTextureView*>(c.view);
            WGPURenderPassColorAttachment& a = colors[i];
            a.view = v->RenderView();
            a.depthSlice = c.view->texture->desc.type == SRHI_TEXTURE_TYPE_3D ? c.view->desc.baseLayer : WGPU_DEPTH_SLICE_UNDEFINED;
            a.loadOp = ToWgpu(c.loadAction);
            a.storeOp = ToWgpu(c.storeAction);
            if (c.loadAction == SRHI_LOAD_ACTION_CLEAR)
                a.clearValue = {c.clearColor[0], c.clearColor[1], c.clearColor[2], c.clearColor[3]};
            if (c.resolveView && (c.storeAction == SRHI_STORE_ACTION_RESOLVE || c.storeAction == SRHI_STORE_ACTION_STORE_AND_RESOLVE))
                a.resolveTarget = static_cast<WgpuTextureView*>(c.resolveView)->RenderView();
            width = MipDim(c.view->texture->desc.width, c.view->desc.baseMip);
            height = MipDim(c.view->texture->desc.height, c.view->desc.baseMip);
        }
        WGPURenderPassDepthStencilAttachment depth = {};
        const srhiDepthAttachment& d = desc.depthAttachment;
        if (d.view) {
            bool stencil = GetFormatInfo(d.view->desc.format).stencil;
            depth.view = static_cast<WgpuTextureView*>(d.view)->RenderView();
            if (d.readOnly) {
                depth.depthReadOnly = 1;
                depth.stencilReadOnly = stencil ? 1 : 0;
            } else {
                depth.depthLoadOp = ToWgpu(d.depthLoadAction);
                depth.depthStoreOp = ToWgpu(d.depthStoreAction);
                depth.depthClearValue = d.clearDepth;
                if (stencil) {
                    depth.stencilLoadOp = ToWgpu(d.stencilLoadAction);
                    depth.stencilStoreOp = ToWgpu(d.stencilStoreAction);
                    depth.stencilClearValue = d.clearStencil;
                }
            }
            width = MipDim(d.view->texture->desc.width, d.view->desc.baseMip);
            height = MipDim(d.view->texture->desc.height, d.view->desc.baseMip);
        }

        WGPURenderPassDescriptor rp = {};
        rp.label = SV(desc.label);
        rp.colorAttachmentCount = desc.colorAttachmentCount;
        rp.colorAttachments = colors;
        rp.depthStencilAttachment = d.view ? &depth : nullptr;
        WGPURenderPassTimestampWrites tw = {};
        if (desc.timestampWrites.querySet) {
            WgpuQuerySet* qs = static_cast<WgpuQuerySet*>(desc.timestampWrites.querySet);
            tw.querySet = qs->set;
            tw.beginningOfPassWriteIndex = desc.timestampWrites.beginIndex == SRHI_QUERY_UNUSED ? WGPU_QUERY_SET_INDEX_UNDEFINED
                                                                                                 : desc.timestampWrites.beginIndex;
            tw.endOfPassWriteIndex =
                desc.timestampWrites.endIndex == SRHI_QUERY_UNUSED ? WGPU_QUERY_SET_INDEX_UNDEFINED : desc.timestampWrites.endIndex;
            NoteQuery(qs, desc.timestampWrites.beginIndex);
            NoteQuery(qs, desc.timestampWrites.endIndex);
            rp.timestampWrites = &tw;
        }

        render.owner = this;
        render.pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
        render.bindings.Reset();
        render.width = width;
        render.height = height;
        return &render;
    }

    void WgpuRenderEncoder::End()
    {
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
        pass = nullptr;
        bindings.Reset();
    }

    srhiComputeEncoder* WgpuCommandBuffer::BeginCompute(const srhiComputePassDesc& desc)
    {
        WGPUComputePassDescriptor cp = {};
        cp.label = SV(desc.label);
        WGPUComputePassTimestampWrites tw = {};
        if (desc.timestampWrites.querySet) {
            WgpuQuerySet* qs = static_cast<WgpuQuerySet*>(desc.timestampWrites.querySet);
            tw.querySet = qs->set;
            tw.beginningOfPassWriteIndex = desc.timestampWrites.beginIndex == SRHI_QUERY_UNUSED ? WGPU_QUERY_SET_INDEX_UNDEFINED
                                                                                                 : desc.timestampWrites.beginIndex;
            tw.endOfPassWriteIndex =
                desc.timestampWrites.endIndex == SRHI_QUERY_UNUSED ? WGPU_QUERY_SET_INDEX_UNDEFINED : desc.timestampWrites.endIndex;
            NoteQuery(qs, desc.timestampWrites.beginIndex);
            NoteQuery(qs, desc.timestampWrites.endIndex);
            cp.timestampWrites = &tw;
        }
        compute.owner = this;
        compute.pass = wgpuCommandEncoderBeginComputePass(enc, &cp);
        compute.bindings.Reset();
        return &compute;
    }

    void WgpuComputeEncoder::End()
    {
        wgpuComputePassEncoderEnd(pass);
        wgpuComputePassEncoderRelease(pass);
        pass = nullptr;
        bindings.Reset();
    }

    // --- Readbacks ---

    WGPUBuffer CreateMapBuffer(WGPUDevice device, uint64_t size)
    {
        WGPUBufferDescriptor bd = {};
        bd.label = SV("SimpleRHI readback");
        bd.size = AlignUp(size, 4);
        bd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
        return wgpuDeviceCreateBuffer(device, &bd);
    }

    void WgpuCommandBuffer::ReadBuffer(srhiBuffer* buffer, uint64_t offset, uint64_t size, srhiReadbackCallback callback, void* userData)
    {
        auto r = std::make_unique<Readback>();
        r->buffer = CreateMapBuffer(dev->device, size);
        r->size = size;
        r->callback = callback;
        r->userData = userData;
        wgpuCommandEncoderCopyBufferToBuffer(enc, static_cast<WgpuBuffer*>(buffer)->buffer, offset, r->buffer, 0, size);
        pending->readbacks.push_back(std::move(r));
    }

    void WgpuCommandBuffer::ReadTexture(srhiTexture* texture, uint32_t mip, uint32_t layer, srhiReadbackCallback callback, void* userData)
    {
        WgpuTexture* t = static_cast<WgpuTexture*>(texture);
        uint32_t w = MipDim(t->desc.width, mip), h = MipDim(t->desc.height, mip);
        auto r = std::make_unique<Readback>();
        r->tightRow = RowBytes(t->desc.format, w);
        r->paddedRow = uint32_t(AlignUp(r->tightRow, SRHI_COPY_BYTES_PER_ROW_ALIGNMENT));
        r->rows = RowCount(t->desc.format, h);
        r->size = uint64_t(r->paddedRow) * r->rows;
        r->buffer = CreateMapBuffer(dev->device, r->size);
        r->callback = callback;
        r->userData = userData;

        WGPUTexelCopyTextureInfo src = {};
        src.texture = t->texture;
        src.mipLevel = mip;
        src.origin = {0, 0, layer};
        src.aspect = WGPUTextureAspect_All;
        WGPUTexelCopyBufferInfo dst = {};
        dst.buffer = r->buffer;
        dst.layout.bytesPerRow = r->paddedRow;
        dst.layout.rowsPerImage = r->rows;
        WGPUExtent3D size = WgpuBlitEncoder::Extent(texture, w, h, 1);
        wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &size);
        pending->readbacks.push_back(std::move(r));
    }

    void WgpuCommandBuffer::Commit(srhiCompletionCallback callback, void* userData)
    {
        // Resolve timestamps written by this command buffer.
        for (auto& [qs, indices] : queryWrites) {
            uint32_t lo = *std::min_element(indices.begin(), indices.end());
            uint32_t hi = *std::max_element(indices.begin(), indices.end());
            uint32_t count = hi - lo + 1;
            wgpuCommandEncoderResolveQuerySet(enc, qs->set, lo, count, qs->resolve, 0);
            auto r = std::make_unique<Readback>();
            r->size = uint64_t(count) * 8;
            r->buffer = CreateMapBuffer(dev->device, r->size);
            r->querySet = qs;
            r->firstQuery = lo;
            r->queries = indices;
            wgpuCommandEncoderCopyBufferToBuffer(enc, qs->resolve, 0, r->buffer, 0, r->size);
            pending->readbacks.push_back(std::move(r));
        }

        for (UniformChunk& c : pending->chunks)
            if (c.used)
                wgpuQueueWriteBuffer(dev->queue, c.buffer, 0, c.shadow.data(), size_t(c.used));

        WGPUCommandBufferDescriptor cd = {};
        cd.label = SV(label);
        WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, &cd);
        wgpuQueueSubmit(dev->queue, 1, &cb);
        wgpuCommandBufferRelease(cb);

        for (WgpuSwapchain* sc : presents) {
            wgpuSurfacePresent(sc->surface);
            sc->ReleaseImage();
        }

        InFlight* f = pending.get();
        f->callback = callback;
        f->userData = userData;
        WGPUQueueWorkDoneCallbackInfo done = {};
        done.mode = WGPUCallbackMode_AllowProcessEvents;
        done.callback = [](WGPUQueueWorkDoneStatus, void* u1, void*) { static_cast<InFlight*>(u1)->done = true; };
        done.userdata1 = f;
        wgpuQueueOnSubmittedWorkDone(dev->queue, done);
        for (auto& r : f->readbacks) {
            WGPUBufferMapCallbackInfo mi = {};
            mi.mode = WGPUCallbackMode_AllowProcessEvents;
            mi.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void* u1, void*) {
                Readback* rb = static_cast<Readback*>(u1);
                if (status == WGPUMapAsyncStatus_Success)
                    rb->mapped = true;
                else
                    rb->failed = true;
            };
            mi.userdata1 = r.get();
            wgpuBufferMapAsync(r->buffer, WGPUMapMode_Read, 0, size_t(AlignUp(r->size, 4)), mi);
        }
        dev->inFlight.push_back(std::move(pending));
        delete this;
    }

    // --- Swapchain ---

    WgpuSwapchain::~WgpuSwapchain()
    {
        if (acquired)
            ReleaseImage();
        delete imageView;
        delete image;
        if (surface) {
            if (configured)
                wgpuSurfaceUnconfigure(surface);
            wgpuSurfaceRelease(surface);
        }
    }

    bool WgpuSwapchain::Apply(srhiColorSpace colorSpace, bool vsync, uint32_t width, uint32_t height)
    {
        WGPUSurfaceCapabilities caps = {};
        wgpuSurfaceGetCapabilities(surface, dev->adapter, &caps);
        auto hasFormat = [&](WGPUTextureFormat f) {
            for (size_t i = 0; i < caps.formatCount; ++i)
                if (caps.formats[i] == f)
                    return true;
            return false;
        };
        WGPUTextureFormat surfaceFmt = WGPUTextureFormat_Undefined, viewFmt = WGPUTextureFormat_Undefined;
        srhiFormat srhiFmt = SRHI_FORMAT_UNDEFINED;
        if (colorSpace == SRHI_COLOR_SPACE_SRGB) {
            if (hasFormat(WGPUTextureFormat_BGRA8Unorm)) {
                surfaceFmt = WGPUTextureFormat_BGRA8Unorm;
                viewFmt = WGPUTextureFormat_BGRA8UnormSrgb;
                srhiFmt = SRHI_FORMAT_BGRA8_UNORM_SRGB;
            } else if (hasFormat(WGPUTextureFormat_RGBA8Unorm)) {
                surfaceFmt = WGPUTextureFormat_RGBA8Unorm;
                viewFmt = WGPUTextureFormat_RGBA8UnormSrgb;
                srhiFmt = SRHI_FORMAT_RGBA8_UNORM_SRGB;
            }
        } else if (colorSpace == SRHI_COLOR_SPACE_EXTENDED_LINEAR_SRGB && hasFormat(WGPUTextureFormat_RGBA16Float)) {
            surfaceFmt = viewFmt = WGPUTextureFormat_RGBA16Float;
            srhiFmt = SRHI_FORMAT_RGBA16_FLOAT;
        }
        bool immediate = false, mailbox = false;
        for (size_t i = 0; i < caps.presentModeCount; ++i) {
            immediate |= caps.presentModes[i] == WGPUPresentMode_Immediate;
            mailbox |= caps.presentModes[i] == WGPUPresentMode_Mailbox;
        }
        WGPUTextureUsage usage = WGPUTextureUsage_RenderAttachment | (caps.usages & (WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst));
        wgpuSurfaceCapabilitiesFreeMembers(caps);
        if (surfaceFmt == WGPUTextureFormat_Undefined) {
            SRHI_ERROR(device, "swapchain '%s': color space %d is not supported by the WebGPU surface", label.c_str(), int(colorSpace));
            return false;
        }

        WGPUSurfaceConfiguration config = {};
        config.device = dev->device;
        config.format = surfaceFmt;
        config.usage = usage;
        config.width = std::max(width, 1u);
        config.height = std::max(height, 1u);
        config.viewFormatCount = viewFmt != surfaceFmt ? 1 : 0;
        config.viewFormats = &viewFmt;
        config.alphaMode = WGPUCompositeAlphaMode_Auto;
        config.presentMode = vsync ? WGPUPresentMode_Fifo : immediate ? WGPUPresentMode_Immediate : mailbox ? WGPUPresentMode_Mailbox
                                                                                                             : WGPUPresentMode_Fifo;
        wgpuSurfaceConfigure(surface, &config);
        configured = true;
        surfaceFormat = surfaceFmt;
        viewFormat = viewFmt;
        format = srhiFmt;
        return true;
    }

    srhiTexture* WgpuSwapchain::Acquire()
    {
        if (acquired)
            return image;
        WGPUSurfaceTexture st = {};
        wgpuSurfaceGetCurrentTexture(surface, &st);
        if (st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
            st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
            if (st.texture)
                wgpuTextureRelease(st.texture);
            if (st.status == WGPUSurfaceGetCurrentTextureStatus_Outdated || st.status == WGPUSurfaceGetCurrentTextureStatus_Lost)
                Apply(desc.colorSpace, desc.vsync != 0, desc.width, desc.height);
            return nullptr;
        }
        image->texture = st.texture;
        image->desc = {};
        image->desc.type = SRHI_TEXTURE_TYPE_2D;
        image->desc.format = format;
        image->desc.width = wgpuTextureGetWidth(st.texture);
        image->desc.height = wgpuTextureGetHeight(st.texture);
        image->desc.depthOrLayers = 1;
        image->desc.mipLevels = 1;
        image->desc.sampleCount = 1;
        image->desc.usage = SRHI_TEXTURE_USAGE_RENDER_TARGET | SRHI_TEXTURE_USAGE_COPY_SRC | SRHI_TEXTURE_USAGE_COPY_DST;
        image->desc.label = image->label.c_str();

        WGPUTextureViewDescriptor vd = {};
        vd.format = viewFormat;
        vd.dimension = WGPUTextureViewDimension_2D;
        vd.mipLevelCount = 1;
        vd.arrayLayerCount = 1;
        vd.aspect = WGPUTextureAspect_All;
        imageView->view = wgpuTextureCreateView(st.texture, &vd);
        imageView->desc = {};
        imageView->desc.format = format;
        imageView->desc.type = SRHI_TEXTURE_TYPE_2D;
        imageView->desc.mipCount = 1;
        imageView->desc.layerCount = 1;
        acquired = true;
        return image;
    }

    srhiSwapchain* WgpuDevice::CreateSwapchain(const srhiSwapchainDesc& d)
    {
        WGPUSurfaceDescriptor sd = {};
        sd.label = SV(d.label);
#if defined(__EMSCRIPTEN__)
        WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas = {};
        canvas.chain.sType = WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector;
        canvas.selector = SV(d.canvasSelector ? d.canvasSelector : "#canvas");
        sd.nextInChain = &canvas.chain;
#elif defined(_WIN32)
        WGPUSurfaceSourceWindowsHWND hwnd = {};
        hwnd.chain.sType = WGPUSType_SurfaceSourceWindowsHWND;
        hwnd.hwnd = d.nativeWindow;
        hwnd.hinstance = GetModuleHandleW(nullptr);
        sd.nextInChain = &hwnd.chain;
#elif defined(__APPLE__)
        WGPUSurfaceSourceMetalLayer layer = {};
        layer.chain.sType = WGPUSType_SurfaceSourceMetalLayer;
        layer.layer = d.nativeWindow;
        sd.nextInChain = &layer.chain;
#else
        SRHI_ERROR(this, "srhiSwapchainCreate: unsupported platform for WebGPU surfaces");
        return nullptr;
#endif
        WGPUSurface surface = wgpuInstanceCreateSurface(instance, &sd);
        if (!surface) {
            SRHI_ERROR(this, "srhiSwapchainCreate: failed to create the WebGPU surface");
            return nullptr;
        }
        WgpuSwapchain* sc = new WgpuSwapchain;
        sc->dev = this;
        sc->device = this;
        sc->surface = surface;
        sc->label = d.label ? d.label : "swapchain";
        sc->desc = d;
        if (!sc->Apply(d.colorSpace, d.vsync != 0, d.width, d.height)) {
            delete sc;
            return nullptr;
        }
        sc->image = new WgpuTexture;
        sc->image->device = this;
        sc->image->isSwapchainImage = true;
        sc->image->label = sc->label;
        sc->imageView = new WgpuTextureView;
        sc->imageView->texture = sc->image;
        sc->imageView->label = sc->label;
        sc->image->defaultView = sc->imageView;
        return sc;
    }
} // namespace

// =====================================================================================================
// Factory
// =====================================================================================================

srhiDevice* srhiCreateDeviceWebGPU(const srhiDeviceDesc& desc)
{
    WGPUInstanceDescriptor id = {};
#if SRHI_WGPU_NATIVE
    WGPUInstanceExtras extras = {};
    extras.chain.sType = WGPUSType(WGPUSType_InstanceExtras);
    extras.backends = WGPUInstanceBackend_Primary;
    extras.flags = desc.debug ? (WGPUInstanceFlag_Validation | WGPUInstanceFlag_Debug) : WGPUInstanceFlag_Default;
    id.nextInChain = &extras.chain;
#endif
    WGPUInstance instance = wgpuCreateInstance(&id);
    if (!instance) {
        srhi::Log(nullptr, SRHI_LOG_SEVERITY_ERROR, "WebGPU: failed to create an instance");
        return nullptr;
    }

    struct AdapterResult
    {
        bool done = false;
        WGPUAdapter adapter = nullptr;
        std::string message;
    } adapterResult;
    WGPURequestAdapterOptions ao = {};
    ao.powerPreference = desc.lowPower ? WGPUPowerPreference_LowPower : WGPUPowerPreference_HighPerformance;
    WGPURequestAdapterCallbackInfo aci = {};
    aci.mode = WGPUCallbackMode_AllowProcessEvents;
    aci.callback = [](WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView message, void* u1, void*) {
        AdapterResult* r = static_cast<AdapterResult*>(u1);
        if (status == WGPURequestAdapterStatus_Success)
            r->adapter = adapter;
        else
            r->message = ToString(message);
        r->done = true;
    };
    aci.userdata1 = &adapterResult;
    wgpuInstanceRequestAdapter(instance, &ao, aci);
    WaitFor(instance, nullptr, &adapterResult.done);
    if (!adapterResult.adapter) {
        srhi::Log(nullptr, SRHI_LOG_SEVERITY_ERROR, "WebGPU: no adapter (%s)", adapterResult.message.c_str());
        wgpuInstanceRelease(instance);
        return nullptr;
    }

    WgpuDevice* d = new WgpuDevice;
    d->desc = desc;
    d->instance = instance;
    d->adapter = adapterResult.adapter;

    // Features: everything SimpleRHI can use that the adapter has.
    const WGPUFeatureName wanted[] = {
        WGPUFeatureName_TimestampQuery,        WGPUFeatureName_TextureCompressionBC,  WGPUFeatureName_TextureCompressionASTC,
        WGPUFeatureName_IndirectFirstInstance, WGPUFeatureName_Float32Filterable,     WGPUFeatureName_Float32Blendable,
        WGPUFeatureName_Depth32FloatStencil8,  WGPUFeatureName_DepthClipControl,      WGPUFeatureName_RG11B10UfloatRenderable,
        WGPUFeatureName_BGRA8UnormStorage,
    };
    for (WGPUFeatureName f : wanted)
        if (wgpuAdapterHasFeature(d->adapter, f))
            d->features.push_back(f);
    wgpuAdapterGetLimits(d->adapter, &d->limits);

    struct DeviceResult
    {
        bool done = false;
        WGPUDevice device = nullptr;
        std::string message;
    } deviceResult;
    WGPUDeviceDescriptor dd = {};
    dd.label = SV("SimpleRHI device");
    dd.requiredFeatureCount = d->features.size();
    dd.requiredFeatures = d->features.data();
    WGPULimits required = d->limits;
    required.nextInChain = nullptr;
    dd.requiredLimits = &required;
    dd.uncapturedErrorCallbackInfo.callback = [](WGPUDevice const*, WGPUErrorType type, WGPUStringView message, void* u1, void*) {
        srhi::Log(static_cast<srhiDevice*>(u1), SRHI_LOG_SEVERITY_ERROR, "WebGPU %s error: %s",
                  type == WGPUErrorType_Validation ? "validation" : "device", ToString(message).c_str());
    };
    dd.uncapturedErrorCallbackInfo.userdata1 = d;
    dd.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
    dd.deviceLostCallbackInfo.callback = [](WGPUDevice const*, WGPUDeviceLostReason reason, WGPUStringView message, void* u1, void*) {
        if (reason != WGPUDeviceLostReason_Destroyed && reason != WGPUDeviceLostReason_InstanceDropped)
            srhi::Log(static_cast<srhiDevice*>(u1), SRHI_LOG_SEVERITY_ERROR, "WebGPU device lost: %s", ToString(message).c_str());
    };
    dd.deviceLostCallbackInfo.userdata1 = d;
    WGPURequestDeviceCallbackInfo dci = {};
    dci.mode = WGPUCallbackMode_AllowProcessEvents;
    dci.callback = [](WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message, void* u1, void*) {
        DeviceResult* r = static_cast<DeviceResult*>(u1);
        if (status == WGPURequestDeviceStatus_Success)
            r->device = device;
        else
            r->message = ToString(message);
        r->done = true;
    };
    dci.userdata1 = &deviceResult;
    wgpuAdapterRequestDevice(d->adapter, &dd, dci);
    WaitFor(instance, nullptr, &deviceResult.done);
    if (!deviceResult.device) {
        srhi::Log(nullptr, SRHI_LOG_SEVERITY_ERROR, "WebGPU: device creation failed (%s)", deviceResult.message.c_str());
        delete d;
        return nullptr;
    }
    d->device = deviceResult.device;
    d->queue = wgpuDeviceGetQueue(d->device);

    WGPUAdapterInfo ai = {};
    wgpuAdapterGetInfo(d->adapter, &ai);
    d->adapterName = ToString(ai.device);
    if (d->adapterName.empty())
        d->adapterName = ToString(ai.description);
    if (d->adapterName.empty())
        d->adapterName = "WebGPU adapter";
    wgpuAdapterInfoFreeMembers(ai);

    // Shared layouts.
    WGPUBindGroupLayoutDescriptor eld = {};
    eld.label = SV("SimpleRHI empty");
    d->emptyLayout = wgpuDeviceCreateBindGroupLayout(d->device, &eld);
    WGPUBindGroupDescriptor egd = {};
    egd.layout = d->emptyLayout;
    d->emptyGroup = wgpuDeviceCreateBindGroup(d->device, &egd);
    WGPUBindGroupLayoutEntry uniforms[SRHI_MAX_UNIFORM_SLOTS] = {};
    for (uint32_t i = 0; i < SRHI_MAX_UNIFORM_SLOTS; ++i) {
        uniforms[i].binding = i;
        uniforms[i].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment | WGPUShaderStage_Compute;
        uniforms[i].buffer.type = WGPUBufferBindingType_Uniform;
        uniforms[i].buffer.hasDynamicOffset = 1;
    }
    WGPUBindGroupLayoutDescriptor uld = {};
    uld.label = SV("SimpleRHI uniforms");
    uld.entryCount = SRHI_MAX_UNIFORM_SLOTS;
    uld.entries = uniforms;
    d->uniformLayout = wgpuDeviceCreateBindGroupLayout(d->device, &uld);

    srhiDeviceInfo& info = d->info;
    info.backend = SRHI_BACKEND_WEBGPU;
    info.backendName = "WebGPU";
    info.features.timestampQueries = d->HasFeature(WGPUFeatureName_TimestampQuery);
    info.features.textureCompressionBC = d->HasFeature(WGPUFeatureName_TextureCompressionBC);
    info.features.textureCompressionASTC = d->HasFeature(WGPUFeatureName_TextureCompressionASTC);
    info.features.textureCompressionASTCHDR = 0;
    info.features.fillModeWireframe = 0;
    info.features.depthClamp = d->HasFeature(WGPUFeatureName_DepthClipControl);
    info.features.indirectFirstInstance = d->HasFeature(WGPUFeatureName_IndirectFirstInstance);
    info.features.cubeArrays = 1;

    const WGPULimits& l = d->limits;
    srhiDeviceLimits& o = info.limits;
    o.maxTextureDimension2D = l.maxTextureDimension2D;
    o.maxTextureDimension3D = l.maxTextureDimension3D;
    o.maxTextureArrayLayers = l.maxTextureArrayLayers;
    o.maxColorAttachments = std::min<uint32_t>(l.maxColorAttachments, SRHI_MAX_COLOR_ATTACHMENTS);
    o.maxComputeWorkgroupSizeX = l.maxComputeWorkgroupSizeX;
    o.maxComputeWorkgroupSizeY = l.maxComputeWorkgroupSizeY;
    o.maxComputeWorkgroupSizeZ = l.maxComputeWorkgroupSizeZ;
    o.maxComputeInvocationsPerWorkgroup = l.maxComputeInvocationsPerWorkgroup;
    o.maxComputeWorkgroupsPerDimension = l.maxComputeWorkgroupsPerDimension;
    o.maxStorageBuffersPerStage = l.maxStorageBuffersPerShaderStage;
    o.maxStorageTexturesPerStage = l.maxStorageTexturesPerShaderStage;
    o.maxSampledTexturesPerStage = l.maxSampledTexturesPerShaderStage;
    o.maxSamplersPerStage = l.maxSamplersPerShaderStage;
    o.uniformBufferOffsetAlignment = l.minUniformBufferOffsetAlignment;
    o.storageBufferOffsetAlignment = l.minStorageBufferOffsetAlignment;
    o.maxUniformBufferBindingSize = l.maxUniformBufferBindingSize;
    o.maxStorageBufferBindingSize = l.maxStorageBufferBindingSize;
    return d;
}
