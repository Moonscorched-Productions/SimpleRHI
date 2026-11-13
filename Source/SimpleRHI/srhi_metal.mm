/**
 * @ Author: Amélie Heinrich
 * @ Create Time: 2026-11-13 00:00:00
 * @ Copyright: Copyright (c) 2026 Amélie Heinrich. All rights reserved.
 */

// Metal 3 backend (Objective-C++, ARC).
//
// - Resources are private-storage and tracked (default hazard tracking), so no barriers are needed.
// - srhiBufferWrite / srhiTextureWrite stage through shared buffers into an internal "upload" command
//   buffer that is committed right before the next user command buffer: queue order gives the
//   "visible to every later commit" semantics.
// - Resource sets have no native object: at draw/dispatch time each set's bindings are applied to the
//   flattened buffer/texture/sampler indices the pipeline's reflection table recorded.
// - Vertex buffers live at the top of the buffer index space (30 downwards), away from Slang's indices.
// - Transient uniforms are set*Bytes.

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#if TARGET_OS_OSX
#import <AppKit/AppKit.h>
#endif

#include "srhi_internal.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <unordered_map>

using namespace srhi;

namespace
{
    constexpr uint32_t kVertexBufferBase = 30; // vertex buffer i -> [[buffer(30 - i)]]
    constexpr uint64_t kStagingChunkSize = 4ull << 20;
    constexpr uint64_t kMaxPooledChunkSize = 64ull << 20;

    NSString* ToNS(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()]; }
    NSString* ToNS(const char* s) { return s ? [NSString stringWithUTF8String:s] : @""; }

    MTLPixelFormat ToMtl(srhiFormat f)
    {
        switch (f) {
        case SRHI_FORMAT_R8_UNORM: return MTLPixelFormatR8Unorm;
        case SRHI_FORMAT_RG8_UNORM: return MTLPixelFormatRG8Unorm;
        case SRHI_FORMAT_RGBA8_UNORM: return MTLPixelFormatRGBA8Unorm;
        case SRHI_FORMAT_RGBA8_UNORM_SRGB: return MTLPixelFormatRGBA8Unorm_sRGB;
        case SRHI_FORMAT_RGBA8_SNORM: return MTLPixelFormatRGBA8Snorm;
        case SRHI_FORMAT_RGBA8_UINT: return MTLPixelFormatRGBA8Uint;
        case SRHI_FORMAT_BGRA8_UNORM: return MTLPixelFormatBGRA8Unorm;
        case SRHI_FORMAT_BGRA8_UNORM_SRGB: return MTLPixelFormatBGRA8Unorm_sRGB;
        case SRHI_FORMAT_R16_FLOAT: return MTLPixelFormatR16Float;
        case SRHI_FORMAT_RG16_FLOAT: return MTLPixelFormatRG16Float;
        case SRHI_FORMAT_RGBA16_FLOAT: return MTLPixelFormatRGBA16Float;
        case SRHI_FORMAT_R16_UINT: return MTLPixelFormatR16Uint;
        case SRHI_FORMAT_R32_FLOAT: return MTLPixelFormatR32Float;
        case SRHI_FORMAT_R32_UINT: return MTLPixelFormatR32Uint;
        case SRHI_FORMAT_R32_SINT: return MTLPixelFormatR32Sint;
        case SRHI_FORMAT_RG32_FLOAT: return MTLPixelFormatRG32Float;
        case SRHI_FORMAT_RG32_UINT: return MTLPixelFormatRG32Uint;
        case SRHI_FORMAT_RGBA32_FLOAT: return MTLPixelFormatRGBA32Float;
        case SRHI_FORMAT_RGBA32_UINT: return MTLPixelFormatRGBA32Uint;
        case SRHI_FORMAT_RGB10A2_UNORM: return MTLPixelFormatRGB10A2Unorm;
        case SRHI_FORMAT_RG11B10_FLOAT: return MTLPixelFormatRG11B10Float;
        case SRHI_FORMAT_D16_UNORM: return MTLPixelFormatDepth16Unorm;
        case SRHI_FORMAT_D32_FLOAT: return MTLPixelFormatDepth32Float;
        case SRHI_FORMAT_D32_FLOAT_S8_UINT: return MTLPixelFormatDepth32Float_Stencil8;
        case SRHI_FORMAT_BC1_UNORM: return MTLPixelFormatBC1_RGBA;
        case SRHI_FORMAT_BC1_UNORM_SRGB: return MTLPixelFormatBC1_RGBA_sRGB;
        case SRHI_FORMAT_BC3_UNORM: return MTLPixelFormatBC3_RGBA;
        case SRHI_FORMAT_BC3_UNORM_SRGB: return MTLPixelFormatBC3_RGBA_sRGB;
        case SRHI_FORMAT_BC4_UNORM: return MTLPixelFormatBC4_RUnorm;
        case SRHI_FORMAT_BC5_UNORM: return MTLPixelFormatBC5_RGUnorm;
        case SRHI_FORMAT_BC6H_UFLOAT: return MTLPixelFormatBC6H_RGBUfloat;
        case SRHI_FORMAT_BC7_UNORM: return MTLPixelFormatBC7_RGBAUnorm;
        case SRHI_FORMAT_BC7_UNORM_SRGB: return MTLPixelFormatBC7_RGBAUnorm_sRGB;
        case SRHI_FORMAT_ASTC_4x4_UNORM: return MTLPixelFormatASTC_4x4_LDR;
        case SRHI_FORMAT_ASTC_4x4_UNORM_SRGB: return MTLPixelFormatASTC_4x4_sRGB;
        case SRHI_FORMAT_ASTC_4x4_FLOAT: return MTLPixelFormatASTC_4x4_HDR;
        default: return MTLPixelFormatInvalid;
        }
    }

    MTLTextureType ToMtl(srhiTextureType t, bool msaa)
    {
        switch (t) {
        case SRHI_TEXTURE_TYPE_2D: return msaa ? MTLTextureType2DMultisample : MTLTextureType2D;
        case SRHI_TEXTURE_TYPE_2D_ARRAY: return MTLTextureType2DArray;
        case SRHI_TEXTURE_TYPE_CUBE: return MTLTextureTypeCube;
        case SRHI_TEXTURE_TYPE_CUBE_ARRAY: return MTLTextureTypeCubeArray;
        case SRHI_TEXTURE_TYPE_3D: return MTLTextureType3D;
        }
        return MTLTextureType2D;
    }

    MTLCompareFunction ToMtl(srhiCompareFunction f)
    {
        switch (f) {
        case SRHI_COMPARE_FUNCTION_NEVER: return MTLCompareFunctionNever;
        case SRHI_COMPARE_FUNCTION_LESS: return MTLCompareFunctionLess;
        case SRHI_COMPARE_FUNCTION_EQUAL: return MTLCompareFunctionEqual;
        case SRHI_COMPARE_FUNCTION_LESS_EQUAL: return MTLCompareFunctionLessEqual;
        case SRHI_COMPARE_FUNCTION_GREATER: return MTLCompareFunctionGreater;
        case SRHI_COMPARE_FUNCTION_NOT_EQUAL: return MTLCompareFunctionNotEqual;
        case SRHI_COMPARE_FUNCTION_GREATER_EQUAL: return MTLCompareFunctionGreaterEqual;
        case SRHI_COMPARE_FUNCTION_ALWAYS: return MTLCompareFunctionAlways;
        }
        return MTLCompareFunctionAlways;
    }

    MTLSamplerAddressMode ToMtl(srhiAddressMode m)
    {
        switch (m) {
        case SRHI_ADDRESS_MODE_REPEAT: return MTLSamplerAddressModeRepeat;
        case SRHI_ADDRESS_MODE_MIRROR_REPEAT: return MTLSamplerAddressModeMirrorRepeat;
        case SRHI_ADDRESS_MODE_CLAMP_TO_EDGE: return MTLSamplerAddressModeClampToEdge;
        }
        return MTLSamplerAddressModeRepeat;
    }

    MTLVertexFormat ToMtl(srhiVertexFormat f)
    {
        switch (f) {
        case SRHI_VERTEX_FORMAT_FLOAT: return MTLVertexFormatFloat;
        case SRHI_VERTEX_FORMAT_FLOAT2: return MTLVertexFormatFloat2;
        case SRHI_VERTEX_FORMAT_FLOAT3: return MTLVertexFormatFloat3;
        case SRHI_VERTEX_FORMAT_FLOAT4: return MTLVertexFormatFloat4;
        case SRHI_VERTEX_FORMAT_HALF2: return MTLVertexFormatHalf2;
        case SRHI_VERTEX_FORMAT_HALF4: return MTLVertexFormatHalf4;
        case SRHI_VERTEX_FORMAT_UINT: return MTLVertexFormatUInt;
        case SRHI_VERTEX_FORMAT_UINT2: return MTLVertexFormatUInt2;
        case SRHI_VERTEX_FORMAT_UINT4: return MTLVertexFormatUInt4;
        case SRHI_VERTEX_FORMAT_SINT: return MTLVertexFormatInt;
        case SRHI_VERTEX_FORMAT_UBYTE4: return MTLVertexFormatUChar4;
        case SRHI_VERTEX_FORMAT_UBYTE4_NORM: return MTLVertexFormatUChar4Normalized;
        case SRHI_VERTEX_FORMAT_SHORT2_NORM: return MTLVertexFormatShort2Normalized;
        case SRHI_VERTEX_FORMAT_SHORT4_NORM: return MTLVertexFormatShort4Normalized;
        }
        return MTLVertexFormatInvalid;
    }

    MTLBlendFactor ToMtl(srhiBlendFactor f)
    {
        switch (f) {
        case SRHI_BLEND_FACTOR_ZERO: return MTLBlendFactorZero;
        case SRHI_BLEND_FACTOR_ONE: return MTLBlendFactorOne;
        case SRHI_BLEND_FACTOR_SRC_COLOR: return MTLBlendFactorSourceColor;
        case SRHI_BLEND_FACTOR_ONE_MINUS_SRC_COLOR: return MTLBlendFactorOneMinusSourceColor;
        case SRHI_BLEND_FACTOR_SRC_ALPHA: return MTLBlendFactorSourceAlpha;
        case SRHI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA: return MTLBlendFactorOneMinusSourceAlpha;
        case SRHI_BLEND_FACTOR_DST_COLOR: return MTLBlendFactorDestinationColor;
        case SRHI_BLEND_FACTOR_ONE_MINUS_DST_COLOR: return MTLBlendFactorOneMinusDestinationColor;
        case SRHI_BLEND_FACTOR_DST_ALPHA: return MTLBlendFactorDestinationAlpha;
        case SRHI_BLEND_FACTOR_ONE_MINUS_DST_ALPHA: return MTLBlendFactorOneMinusDestinationAlpha;
        case SRHI_BLEND_FACTOR_SRC_ALPHA_SATURATED: return MTLBlendFactorSourceAlphaSaturated;
        case SRHI_BLEND_FACTOR_BLEND_COLOR: return MTLBlendFactorBlendColor;
        case SRHI_BLEND_FACTOR_ONE_MINUS_BLEND_COLOR: return MTLBlendFactorOneMinusBlendColor;
        }
        return MTLBlendFactorOne;
    }

    MTLBlendOperation ToMtl(srhiBlendOp op)
    {
        switch (op) {
        case SRHI_BLEND_OP_ADD: return MTLBlendOperationAdd;
        case SRHI_BLEND_OP_SUBTRACT: return MTLBlendOperationSubtract;
        case SRHI_BLEND_OP_REVERSE_SUBTRACT: return MTLBlendOperationReverseSubtract;
        case SRHI_BLEND_OP_MIN: return MTLBlendOperationMin;
        case SRHI_BLEND_OP_MAX: return MTLBlendOperationMax;
        }
        return MTLBlendOperationAdd;
    }

    MTLStencilOperation ToMtl(srhiStencilOp op)
    {
        switch (op) {
        case SRHI_STENCIL_OP_KEEP: return MTLStencilOperationKeep;
        case SRHI_STENCIL_OP_ZERO: return MTLStencilOperationZero;
        case SRHI_STENCIL_OP_REPLACE: return MTLStencilOperationReplace;
        case SRHI_STENCIL_OP_INCREMENT_CLAMP: return MTLStencilOperationIncrementClamp;
        case SRHI_STENCIL_OP_DECREMENT_CLAMP: return MTLStencilOperationDecrementClamp;
        case SRHI_STENCIL_OP_INVERT: return MTLStencilOperationInvert;
        case SRHI_STENCIL_OP_INCREMENT_WRAP: return MTLStencilOperationIncrementWrap;
        case SRHI_STENCIL_OP_DECREMENT_WRAP: return MTLStencilOperationDecrementWrap;
        }
        return MTLStencilOperationKeep;
    }

    MTLLoadAction ToMtl(srhiLoadAction a)
    {
        switch (a) {
        case SRHI_LOAD_ACTION_LOAD: return MTLLoadActionLoad;
        case SRHI_LOAD_ACTION_CLEAR: return MTLLoadActionClear;
        default: return MTLLoadActionDontCare;
        }
    }

    MTLStoreAction ToMtl(srhiStoreAction a)
    {
        switch (a) {
        case SRHI_STORE_ACTION_STORE: return MTLStoreActionStore;
        case SRHI_STORE_ACTION_RESOLVE: return MTLStoreActionMultisampleResolve;
        case SRHI_STORE_ACTION_STORE_AND_RESOLVE: return MTLStoreActionStoreAndMultisampleResolve;
        default: return MTLStoreActionDontCare;
        }
    }

    // =================================================================================================
    // Objects
    // =================================================================================================

    struct MtlDevice;

    struct MtlBuffer : srhiBuffer
    {
        id<MTLBuffer> buffer;
        void* GetNative() override { return (__bridge void*)buffer; }
    };

    struct MtlTexture : srhiTexture
    {
        id<MTLTexture> texture;
        void* GetNative() override { return (__bridge void*)texture; }
    };

    struct MtlTextureView : srhiTextureView
    {
        /// @brief The view's own texture object (the parent's when the view covers it unchanged).
        id<MTLTexture> mtl;
    };

    struct MtlSampler : srhiSampler
    {
        id<MTLSamplerState> sampler;
    };

    struct MtlShaderModule : srhiShaderModule
    {
        std::unordered_map<std::string, id<MTLFunction>> functions;
    };

    struct MtlResourceSetLayout : srhiResourceSetLayout
    {
    };

    struct MtlResourceSet : srhiResourceSet
    {
    };

    struct MtlRenderPipeline : srhiRenderPipeline
    {
        id<MTLRenderPipelineState> pso;
        id<MTLDepthStencilState> depthStencil;
        MTLPrimitiveType primitive = MTLPrimitiveTypeTriangle;
    };

    struct MtlComputePipeline : srhiComputePipeline
    {
        id<MTLComputePipelineState> pso;
    };

    struct MtlQuerySet : srhiQuerySet
    {
        id<MTLCounterSampleBuffer> buffer;
        std::vector<uint64_t> results;
        std::vector<uint8_t> valid;
        /// @brief CPU/GPU timestamp pair taken at creation, for the GPU tick -> ns scale.
        MTLTimestamp cpuOrigin = 0, gpuOrigin = 0;
        double gpuTickToNs = 1.0;

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

    struct StagingChunk
    {
        id<MTLBuffer> buffer;
        uint64_t used = 0;
    };

    struct StagingAlloc
    {
        id<MTLBuffer> buffer;
        uint64_t offset;
        uint8_t* cpu;
    };

    struct Readback
    {
        id<MTLBuffer> buffer;
        uint64_t size;
        srhiReadbackCallback callback;
        void* userData;
        /// @brief Texture readbacks: rows to repack from rowPitch to tightPitch (Metal needs no padding, so equal).
    };

    struct QueryWrite
    {
        MtlQuerySet* querySet;
        uint32_t index;
    };

    struct InFlight
    {
        id<MTLCommandBuffer> cb;
        srhiCompletionCallback callback = nullptr;
        void* userData = nullptr;
        std::vector<Readback> readbacks;
        std::vector<QueryWrite> queries;
        std::vector<StagingChunk> chunks;
    };

    struct MtlSwapchain;
    struct MtlCommandBuffer;

    // --- Encoders ------------------------------------------------------------------------------------

    /// @brief Shared resource-set / uniform state for render and compute encoders.
    struct BindingState
    {
        srhiResourceSet* sets[SRHI_MAX_RESOURCE_SETS] = {};
        bool setDirty[SRHI_MAX_RESOURCE_SETS] = {};
        std::vector<uint8_t> uniforms[SRHI_MAX_UNIFORM_SLOTS];
        bool uniformDirty[SRHI_MAX_UNIFORM_SLOTS] = {};

        void Reset()
        {
            for (uint32_t i = 0; i < SRHI_MAX_RESOURCE_SETS; ++i) {
                sets[i] = nullptr;
                setDirty[i] = false;
            }
            for (uint32_t i = 0; i < SRHI_MAX_UNIFORM_SLOTS; ++i) {
                uniforms[i].clear();
                uniformDirty[i] = false;
            }
        }

        void MarkAllDirty()
        {
            for (uint32_t i = 0; i < SRHI_MAX_RESOURCE_SETS; ++i)
                setDirty[i] = sets[i] != nullptr;
            for (uint32_t i = 0; i < SRHI_MAX_UNIFORM_SLOTS; ++i)
                uniformDirty[i] = !uniforms[i].empty();
        }

        /// @brief Calls bind(stageMask, slot, binding) for every dirty, used binding, and
        ///        bindBytes(stageMask, slot, data, size) for every dirty, used uniform slot.
        template <typename BindFn, typename BytesFn>
        bool Flush(srhiDevice* device, const PipelineLayoutInfo& layout, const char* pipelineLabel, BindFn&& bind, BytesFn&& bindBytes)
        {
            for (uint32_t s = 0; s < SRHI_MAX_RESOURCE_SETS; ++s) {
                const std::vector<BindingSlot>& slots = layout.slots[s];
                bool needed = false;
                for (const BindingSlot& slot : slots)
                    needed |= slot.used;
                if (needed && !sets[s]) {
                    SRHI_ERROR(device, "draw/dispatch with pipeline '%s': resource set %u is not bound", pipelineLabel, s);
                    return false;
                }
                if (!setDirty[s] || !sets[s])
                    continue;
                setDirty[s] = false;
                if (!needed)
                    continue;
                srhiResourceSet* set = sets[s];
                srhiResourceSetLayout* expected = layout.setLayouts[s];
                bool compatible = set->layout == expected || set->layout->entries.size() == expected->entries.size();
                for (size_t i = 0; compatible && i < expected->entries.size() && set->layout != expected; ++i)
                    compatible = set->layout->entries[i].binding == expected->entries[i].binding &&
                                 set->layout->entries[i].type == expected->entries[i].type;
                if (!compatible) {
                    SRHI_ERROR(device, "pipeline '%s': resource set '%s' at index %u has an incompatible layout", pipelineLabel,
                               set->label.c_str(), s);
                    return false;
                }
                for (size_t i = 0; i < slots.size(); ++i)
                    if (slots[i].used)
                        bind(slots[i], set->bindings[i]);
            }
            for (uint32_t u = 0; u < SRHI_MAX_UNIFORM_SLOTS; ++u) {
                const BindingSlot& slot = layout.uniforms[u];
                if (!slot.used)
                    continue;
                if (uniforms[u].empty()) {
                    SRHI_ERROR(device, "draw/dispatch with pipeline '%s': uniform slot %u was never set", pipelineLabel, u);
                    return false;
                }
                if (!uniformDirty[u])
                    continue;
                uniformDirty[u] = false;
                if (uniforms[u].size() < layout.uniformSizes[u]) {
                    SRHI_ERROR(device, "pipeline '%s': uniform slot %u needs %u bytes, %zu were set", pipelineLabel, u,
                               layout.uniformSizes[u], uniforms[u].size());
                    return false;
                }
                bindBytes(slot, uniforms[u].data(), uint32_t(uniforms[u].size()));
            }
            return true;
        }
    };

    struct MtlRenderEncoder : srhiRenderEncoder
    {
        MtlCommandBuffer* owner = nullptr;
        id<MTLRenderCommandEncoder> enc;
        BindingState bindings;
        id<MTLBuffer> indexBuffer;
        uint64_t indexOffset = 0;
        uint32_t width = 0, height = 0;

        void End() override;
        void SetPipeline(srhiRenderPipeline* p) override;
        void SetVertexBuffer(uint32_t index, srhiBuffer* buffer, uint64_t offset) override
        {
            [enc setVertexBuffer:static_cast<MtlBuffer*>(buffer)->buffer offset:offset atIndex:kVertexBufferBase - index];
        }
        void SetIndexBuffer(srhiBuffer* buffer, srhiIndexType, uint64_t offset) override
        {
            indexBuffer = static_cast<MtlBuffer*>(buffer)->buffer;
            indexOffset = offset;
        }
        void SetResourceSet(uint32_t index, srhiResourceSet* set) override
        {
            bindings.sets[index] = set;
            bindings.setDirty[index] = true;
        }
        void SetUniforms(uint32_t slot, const void* data, uint32_t size) override
        {
            bindings.uniforms[slot].assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
            bindings.uniformDirty[slot] = true;
        }
        void SetViewport(const srhiViewport& v) override
        {
            [enc setViewport:(MTLViewport){v.x, v.y, v.width, v.height, v.minDepth, v.maxDepth}];
        }
        void SetScissor(const srhiScissorRect& r) override
        {
            uint32_t x = std::min(r.x, width), y = std::min(r.y, height);
            uint32_t w = std::min(r.width, width - x), h = std::min(r.height, height - y);
            [enc setScissorRect:(MTLScissorRect){x, y, w, h}];
        }
        void SetBlendColor(const float c[4]) override { [enc setBlendColorRed:c[0] green:c[1] blue:c[2] alpha:c[3]]; }
        void SetStencilReference(uint32_t ref) override { [enc setStencilReferenceValue:ref]; }
        bool Flush();
        void Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) override
        {
            if (!Flush())
                return;
            [enc drawPrimitives:static_cast<MtlRenderPipeline*>(pipeline)->primitive
                    vertexStart:firstVertex
                    vertexCount:vertexCount
                  instanceCount:instanceCount
                   baseInstance:firstInstance];
        }
        void DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t baseVertex,
                         uint32_t firstInstance) override
        {
            if (!Flush())
                return;
            uint32_t indexSize = indexType == SRHI_INDEX_TYPE_UINT16 ? 2 : 4;
            [enc drawIndexedPrimitives:static_cast<MtlRenderPipeline*>(pipeline)->primitive
                            indexCount:indexCount
                             indexType:indexType == SRHI_INDEX_TYPE_UINT16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
                           indexBuffer:indexBuffer
                     indexBufferOffset:indexOffset + uint64_t(firstIndex) * indexSize
                         instanceCount:instanceCount
                            baseVertex:baseVertex
                          baseInstance:firstInstance];
        }
        void DrawIndirect(srhiBuffer* buffer, uint64_t offset) override
        {
            if (!Flush())
                return;
            [enc drawPrimitives:static_cast<MtlRenderPipeline*>(pipeline)->primitive
                      indirectBuffer:static_cast<MtlBuffer*>(buffer)->buffer
                indirectBufferOffset:offset];
        }
        void DrawIndexedIndirect(srhiBuffer* buffer, uint64_t offset) override
        {
            if (!Flush())
                return;
            [enc drawIndexedPrimitives:static_cast<MtlRenderPipeline*>(pipeline)->primitive
                             indexType:indexType == SRHI_INDEX_TYPE_UINT16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
                           indexBuffer:indexBuffer
                     indexBufferOffset:indexOffset
                        indirectBuffer:static_cast<MtlBuffer*>(buffer)->buffer
                  indirectBufferOffset:offset];
        }
        void PushDebugGroup(const char* name) override { [enc pushDebugGroup:ToNS(name)]; }
        void PopDebugGroup() override { [enc popDebugGroup]; }
        void InsertDebugMarker(const char* name) override { [enc insertDebugSignpost:ToNS(name)]; }
    };

    struct MtlComputeEncoder : srhiComputeEncoder
    {
        MtlCommandBuffer* owner = nullptr;
        id<MTLComputeCommandEncoder> enc;
        BindingState bindings;

        void End() override;
        void SetPipeline(srhiComputePipeline* p) override
        {
            [enc setComputePipelineState:static_cast<MtlComputePipeline*>(p)->pso];
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
            bindings.uniformDirty[slot] = true;
        }
        bool Flush();
        void Dispatch(uint32_t x, uint32_t y, uint32_t z) override
        {
            if (!Flush())
                return;
            const uint32_t* tg = pipeline->threadgroupSize;
            [enc dispatchThreadgroups:MTLSizeMake(x, y, z) threadsPerThreadgroup:MTLSizeMake(tg[0], tg[1], tg[2])];
        }
        void DispatchIndirect(srhiBuffer* buffer, uint64_t offset) override
        {
            if (!Flush())
                return;
            const uint32_t* tg = pipeline->threadgroupSize;
            [enc dispatchThreadgroupsWithIndirectBuffer:static_cast<MtlBuffer*>(buffer)->buffer
                                   indirectBufferOffset:offset
                                  threadsPerThreadgroup:MTLSizeMake(tg[0], tg[1], tg[2])];
        }
        void PushDebugGroup(const char* name) override { [enc pushDebugGroup:ToNS(name)]; }
        void PopDebugGroup() override { [enc popDebugGroup]; }
        void InsertDebugMarker(const char* name) override { [enc insertDebugSignpost:ToNS(name)]; }
    };

    struct MtlBlitEncoder : srhiBlitEncoder
    {
        MtlCommandBuffer* owner = nullptr;
        id<MTLBlitCommandEncoder> enc;

        void End() override
        {
            [enc endEncoding];
            enc = nil;
        }
        void CopyBufferToBuffer(srhiBuffer* src, uint64_t srcOffset, srhiBuffer* dst, uint64_t dstOffset, uint64_t size) override
        {
            [enc copyFromBuffer:static_cast<MtlBuffer*>(src)->buffer
                   sourceOffset:srcOffset
                       toBuffer:static_cast<MtlBuffer*>(dst)->buffer
              destinationOffset:dstOffset
                           size:size];
        }
        void CopyBufferToTexture(const srhiBufferCopyLocation& src, const srhiTextureCopyLocation& dst, uint32_t width,
                                 uint32_t height, uint32_t depth) override
        {
            MtlTexture* tex = static_cast<MtlTexture*>(dst.texture);
            uint32_t rows = src.rowsPerImage ? src.rowsPerImage : RowCount(tex->desc.format, height);
            uint64_t bytesPerImage = uint64_t(src.bytesPerRow) * rows;
            bool is3D = tex->desc.type == SRHI_TEXTURE_TYPE_3D;
            for (uint32_t i = 0; i < (is3D ? 1 : depth); ++i)
                [enc copyFromBuffer:static_cast<MtlBuffer*>(src.buffer)->buffer
                           sourceOffset:src.offset + bytesPerImage * i
                      sourceBytesPerRow:src.bytesPerRow
                    sourceBytesPerImage:bytesPerImage
                             sourceSize:MTLSizeMake(width, height, is3D ? depth : 1)
                              toTexture:tex->texture
                       destinationSlice:is3D ? 0 : dst.layer + i
                       destinationLevel:dst.mip
                      destinationOrigin:MTLOriginMake(dst.x, dst.y, is3D ? dst.z : 0)];
        }
        void CopyTextureToBuffer(const srhiTextureCopyLocation& src, const srhiBufferCopyLocation& dst, uint32_t width,
                                 uint32_t height, uint32_t depth) override
        {
            MtlTexture* tex = static_cast<MtlTexture*>(src.texture);
            uint32_t rows = dst.rowsPerImage ? dst.rowsPerImage : RowCount(tex->desc.format, height);
            uint64_t bytesPerImage = uint64_t(dst.bytesPerRow) * rows;
            bool is3D = tex->desc.type == SRHI_TEXTURE_TYPE_3D;
            for (uint32_t i = 0; i < (is3D ? 1 : depth); ++i)
                [enc copyFromTexture:tex->texture
                                 sourceSlice:is3D ? 0 : src.layer + i
                                 sourceLevel:src.mip
                                sourceOrigin:MTLOriginMake(src.x, src.y, is3D ? src.z : 0)
                                  sourceSize:MTLSizeMake(width, height, is3D ? depth : 1)
                                    toBuffer:static_cast<MtlBuffer*>(dst.buffer)->buffer
                           destinationOffset:dst.offset + bytesPerImage * i
                      destinationBytesPerRow:dst.bytesPerRow
                    destinationBytesPerImage:bytesPerImage];
        }
        void CopyTextureToTexture(const srhiTextureCopyLocation& src, const srhiTextureCopyLocation& dst, uint32_t width,
                                  uint32_t height, uint32_t depth) override
        {
            MtlTexture* s = static_cast<MtlTexture*>(src.texture);
            MtlTexture* d = static_cast<MtlTexture*>(dst.texture);
            bool s3D = s->desc.type == SRHI_TEXTURE_TYPE_3D, d3D = d->desc.type == SRHI_TEXTURE_TYPE_3D;
            uint32_t slices = (s3D && d3D) ? 1 : depth;
            for (uint32_t i = 0; i < slices; ++i)
                [enc copyFromTexture:s->texture
                         sourceSlice:s3D ? 0 : src.layer + i
                         sourceLevel:src.mip
                        sourceOrigin:MTLOriginMake(src.x, src.y, s3D ? src.z + (d3D ? 0 : i) : 0)
                          sourceSize:MTLSizeMake(width, height, (s3D && d3D) ? depth : 1)
                           toTexture:d->texture
                    destinationSlice:d3D ? 0 : dst.layer + i
                    destinationLevel:dst.mip
                   destinationOrigin:MTLOriginMake(dst.x, dst.y, d3D ? dst.z + (s3D ? 0 : i) : 0)];
        }
        void GenerateMipmaps(srhiTexture* texture) override
        {
            [enc generateMipmapsForTexture:static_cast<MtlTexture*>(texture)->texture];
        }
        void PushDebugGroup(const char* name) override { [enc pushDebugGroup:ToNS(name)]; }
        void PopDebugGroup() override { [enc popDebugGroup]; }
    };

    // --- Swapchain -----------------------------------------------------------------------------------

    struct MtlSwapchain : srhiSwapchain
    {
        MtlDevice* dev = nullptr;
        CAMetalLayer* layer;
        id<CAMetalDrawable> drawable;
        MtlTexture* image = nullptr;
        MtlTextureView* imageView = nullptr;

        ~MtlSwapchain() override
        {
            delete imageView;
            delete image;
        }
        void Apply(srhiColorSpace colorSpace, bool vsync);
        void Resize(uint32_t width, uint32_t height) override
        {
            layer.drawableSize = CGSizeMake(width, height);
            drawable = nil;
        }
        bool Configure(srhiColorSpace colorSpace, bool vsync) override
        {
            Apply(colorSpace, vsync);
            return true;
        }
        void GetDisplayInfo(srhiDisplayInfo& out) override;
        srhiTexture* Acquire() override;
    };

    // --- Command buffer ------------------------------------------------------------------------------

    struct MtlCommandBuffer : srhiCommandBuffer
    {
        MtlDevice* dev = nullptr;
        id<MTLCommandBuffer> cb;
        MtlRenderEncoder render;
        MtlComputeEncoder compute;
        MtlBlitEncoder blit;
        InFlight pending;

        srhiRenderEncoder* BeginRender(const srhiRenderPassDesc& desc) override;
        srhiComputeEncoder* BeginCompute(const srhiComputePassDesc& desc) override;
        srhiBlitEncoder* BeginBlit(const char* label) override
        {
            blit.owner = this;
            blit.enc = [cb blitCommandEncoder];
            blit.enc.label = ToNS(label);
            return &blit;
        }
        void Commit(srhiCompletionCallback callback, void* userData) override;
        void PushDebugGroup(const char* name) override { [cb pushDebugGroup:ToNS(name)]; }
        void PopDebugGroup() override { [cb popDebugGroup]; }
        void ReadBuffer(srhiBuffer* buffer, uint64_t offset, uint64_t size, srhiReadbackCallback callback, void* userData) override;
        void ReadTexture(srhiTexture* texture, uint32_t mip, uint32_t layer, srhiReadbackCallback callback, void* userData) override;
        void EncodeNative(srhiNativeEncodeCallback callback, void* userData) override { callback((__bridge void*)cb, userData); }
        void Present(srhiSwapchain* swapchain) override
        {
            MtlSwapchain* sc = static_cast<MtlSwapchain*>(swapchain);
            if (!sc->drawable) {
                SRHI_ERROR(device, "srhiCommandBufferPresent: swapchain '%s' was not acquired", sc->label.c_str());
                return;
            }
            [cb presentDrawable:sc->drawable];
            sc->drawable = nil;
            sc->image->texture = nil;
            sc->imageView->mtl = nil;
        }
    };

    // --- Device --------------------------------------------------------------------------------------

    struct MtlDevice : srhiDevice
    {
        id<MTLDevice> device;
        id<MTLCommandQueue> queue;
        id<MTLCounterSet> timestampCounterSet;

        id<MTLCommandBuffer> uploadCB;
        id<MTLBlitCommandEncoder> uploadEncoder;
        std::vector<StagingChunk> uploadChunks; ///< Chunks referenced by the open upload command buffer.
        std::vector<StagingChunk> freeChunks;
        std::deque<InFlight> inFlight;

        ~MtlDevice() override { WaitIdle(); }

        // --- Staging ---

        StagingAlloc AllocStaging(uint64_t size, uint64_t align)
        {
            for (StagingChunk& c : uploadChunks) {
                uint64_t offset = AlignUp(c.used, align);
                if (offset + size <= c.buffer.length) {
                    c.used = offset + size;
                    return {c.buffer, offset, static_cast<uint8_t*>(c.buffer.contents) + offset};
                }
            }
            StagingChunk chunk;
            for (size_t i = 0; i < freeChunks.size(); ++i)
                if (freeChunks[i].buffer.length >= size) {
                    chunk = freeChunks[i];
                    freeChunks.erase(freeChunks.begin() + i);
                    break;
                }
            if (!chunk.buffer)
                chunk.buffer = [device newBufferWithLength:std::max<uint64_t>(kStagingChunkSize, AlignUp(size, 4096))
                                                   options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
            chunk.used = size;
            uploadChunks.push_back(chunk);
            return {chunk.buffer, 0, static_cast<uint8_t*>(chunk.buffer.contents)};
        }

        id<MTLBlitCommandEncoder> UploadEncoder()
        {
            if (!uploadCB) {
                uploadCB = [queue commandBuffer];
                uploadCB.label = @"SimpleRHI uploads";
                uploadEncoder = [uploadCB blitCommandEncoder];
            }
            return uploadEncoder;
        }

        void FlushUploads()
        {
            if (!uploadCB)
                return;
            [uploadEncoder endEncoding];
            [uploadCB commit];
            InFlight f;
            f.cb = uploadCB;
            f.chunks = std::move(uploadChunks);
            inFlight.push_back(std::move(f));
            uploadChunks.clear();
            uploadCB = nil;
            uploadEncoder = nil;
        }

        void Retire(InFlight& f)
        {
            if (f.cb.status == MTLCommandBufferStatusError)
                SRHI_ERROR(this, "command buffer '%s' failed: %s", f.cb.label.UTF8String ?: "",
                           f.cb.error.localizedDescription.UTF8String ?: "unknown error");
            for (Readback& r : f.readbacks)
                r.callback(r.buffer.contents, r.size, r.userData);
            if (!f.queries.empty()) {
                MTLTimestamp cpu = 0, gpu = 0;
                [device sampleTimestamps:&cpu gpuTimestamp:&gpu];
                for (QueryWrite& q : f.queries) {
                    MtlQuerySet* qs = q.querySet;
                    if (gpu > qs->gpuOrigin && cpu > qs->cpuOrigin)
                        qs->gpuTickToNs = double(cpu - qs->cpuOrigin) / double(gpu - qs->gpuOrigin); // CPU timestamps are ns
                    NSData* data = [qs->buffer resolveCounterRange:NSMakeRange(q.index, 1)];
                    if (data && data.length >= sizeof(MTLCounterResultTimestamp)) {
                        uint64_t ticks = static_cast<const MTLCounterResultTimestamp*>(data.bytes)[0].timestamp;
                        qs->results[q.index] = uint64_t(double(ticks) * qs->gpuTickToNs);
                        qs->valid[q.index] = ticks != MTLCounterErrorValue;
                    }
                }
            }
            if (f.callback)
                f.callback(f.userData);
            for (StagingChunk& c : f.chunks)
                if (c.buffer.length <= kMaxPooledChunkSize) {
                    c.used = 0;
                    freeChunks.push_back(c);
                }
        }

        // --- srhiDevice ---

        uint32_t GetFormatSupport(srhiFormat format) override;

        void Poll() override
        {
            while (!inFlight.empty()) {
                MTLCommandBufferStatus status = inFlight.front().cb.status;
                if (status != MTLCommandBufferStatusCompleted && status != MTLCommandBufferStatusError)
                    break;
                InFlight f = std::move(inFlight.front());
                inFlight.pop_front();
                Retire(f);
            }
        }

        void WaitIdle() override
        {
            FlushUploads();
            if (!inFlight.empty())
                [inFlight.back().cb waitUntilCompleted];
            Poll();
        }

        void* GetNative() override { return (__bridge void*)device; }
        void* GetNativeQueue() override { return (__bridge void*)queue; }

        srhiBuffer* CreateBuffer(const srhiBufferDesc& desc) override
        {
            MtlBuffer* b = new MtlBuffer;
            b->buffer = [device newBufferWithLength:desc.size options:MTLResourceStorageModePrivate];
            if (!b->buffer) {
                SRHI_ERROR(this, "srhiBufferCreate(%s): allocation of %llu bytes failed", desc.label ? desc.label : "",
                           (unsigned long long)desc.size);
                delete b;
                return nullptr;
            }
            b->buffer.label = ToNS(desc.label);
            return b;
        }

        void WriteBuffer(srhiBuffer* buffer, uint64_t offset, const void* data, uint64_t size) override
        {
            StagingAlloc s = AllocStaging(size, 16);
            memcpy(s.cpu, data, size);
            [UploadEncoder() copyFromBuffer:s.buffer
                               sourceOffset:s.offset
                                   toBuffer:static_cast<MtlBuffer*>(buffer)->buffer
                          destinationOffset:offset
                                       size:size];
        }

        srhiTexture* CreateTexture(const srhiTextureDesc& d) override
        {
            MTLTextureDescriptor* td = [MTLTextureDescriptor new];
            td.textureType = ToMtl(d.type, d.sampleCount > 1);
            td.pixelFormat = ToMtl(d.format);
            td.width = d.width;
            td.height = d.height;
            td.depth = d.type == SRHI_TEXTURE_TYPE_3D ? d.depthOrLayers : 1;
            td.arrayLength = d.type == SRHI_TEXTURE_TYPE_2D_ARRAY ? d.depthOrLayers
                           : d.type == SRHI_TEXTURE_TYPE_CUBE_ARRAY ? d.depthOrLayers / 6
                                                                    : 1;
            td.mipmapLevelCount = d.mipLevels;
            td.sampleCount = d.sampleCount;
            td.storageMode = MTLStorageModePrivate;
            MTLTextureUsage usage = 0;
            if (d.usage & SRHI_TEXTURE_USAGE_SAMPLED)
                usage |= MTLTextureUsageShaderRead;
            if (d.usage & SRHI_TEXTURE_USAGE_STORAGE)
                usage |= MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
            if (d.usage & SRHI_TEXTURE_USAGE_RENDER_TARGET)
                usage |= MTLTextureUsageRenderTarget;
            if (GetFormatInfo(d.format).srgbTwin != SRHI_FORMAT_UNDEFINED || d.type != SRHI_TEXTURE_TYPE_2D)
                usage |= MTLTextureUsagePixelFormatView;
            td.usage = usage;

            MtlTexture* t = new MtlTexture;
            t->texture = [device newTextureWithDescriptor:td];
            if (!t->texture) {
                SRHI_ERROR(this, "srhiTextureCreate(%s): creation failed", d.label ? d.label : "");
                delete t;
                return nullptr;
            }
            t->texture.label = ToNS(d.label);
            return t;
        }

        void WriteTexture(srhiTexture* texture, uint32_t mip, uint32_t layer, const srhiTextureRegion& r, const void* data,
                          uint32_t bytesPerRow, uint32_t rowsPerImage) override
        {
            MtlTexture* t = static_cast<MtlTexture*>(texture);
            const FormatInfo& fi = GetFormatInfo(t->desc.format);
            uint32_t tight = RowBytes(t->desc.format, r.width);
            uint64_t imageBytes = uint64_t(tight) * rowsPerImage;
            StagingAlloc s = AllocStaging(imageBytes * r.depth, std::max<uint32_t>(fi.blockBytes, 16));
            const uint8_t* src = static_cast<const uint8_t*>(data);
            for (uint32_t z = 0; z < r.depth; ++z)
                for (uint32_t row = 0; row < rowsPerImage; ++row)
                    memcpy(s.cpu + imageBytes * z + uint64_t(tight) * row, src + (uint64_t(z) * rowsPerImage + row) * bytesPerRow, tight);
            bool is3D = t->desc.type == SRHI_TEXTURE_TYPE_3D;
            [UploadEncoder() copyFromBuffer:s.buffer
                               sourceOffset:s.offset
                          sourceBytesPerRow:tight
                        sourceBytesPerImage:imageBytes
                                 sourceSize:MTLSizeMake(r.width, r.height, is3D ? r.depth : 1)
                                  toTexture:t->texture
                           destinationSlice:is3D ? 0 : layer
                           destinationLevel:mip
                          destinationOrigin:MTLOriginMake(r.x, r.y, is3D ? r.z : 0)];
        }

        srhiTextureView* CreateTextureView(srhiTexture* texture, const srhiTextureViewDesc& d) override
        {
            MtlTexture* t = static_cast<MtlTexture*>(texture);
            MtlTextureView* v = new MtlTextureView;
            const srhiTextureDesc& td = t->desc;
            bool whole = d.format == td.format && d.type == td.type && d.baseMip == 0 && d.mipCount == td.mipLevels &&
                         d.baseLayer == 0 && d.layerCount == t->LayerCount();
            if (whole) {
                v->mtl = t->texture;
            } else {
                v->mtl = [t->texture newTextureViewWithPixelFormat:ToMtl(d.format)
                                                           textureType:ToMtl(d.type, td.sampleCount > 1)
                                                                levels:NSMakeRange(d.baseMip, d.mipCount)
                                                                slices:NSMakeRange(d.baseLayer, d.layerCount)];
                if (!v->mtl) {
                    SRHI_ERROR(this, "srhiTextureViewCreate(%s): creation failed", td.label);
                    delete v;
                    return nullptr;
                }
            }
            return v;
        }

        srhiSampler* CreateSampler(const srhiSamplerDesc& d) override
        {
            MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
            sd.minFilter = d.minFilter == SRHI_FILTER_LINEAR ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
            sd.magFilter = d.magFilter == SRHI_FILTER_LINEAR ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
            sd.mipFilter = d.mipFilter == SRHI_FILTER_LINEAR ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
            sd.sAddressMode = ToMtl(d.addressU);
            sd.tAddressMode = ToMtl(d.addressV);
            sd.rAddressMode = ToMtl(d.addressW);
            sd.lodMinClamp = d.lodMinClamp;
            sd.lodMaxClamp = d.lodMaxClamp;
            sd.maxAnisotropy = d.maxAnisotropy;
            if (d.compareEnable)
                sd.compareFunction = ToMtl(d.compareFunction);
            sd.label = ToNS(d.label);
            MtlSampler* s = new MtlSampler;
            s->sampler = [device newSamplerStateWithDescriptor:sd];
            return s;
        }

        srhiShaderModule* CreateShaderModule(Package&& package, const char* label) override
        {
            for (const PkgEntry& e : package.entries)
                if (!e.FindBlob(BlobKind::MetalLib) && !e.FindBlob(BlobKind::MSL)) {
                    SRHI_ERROR(this, "srhiShaderModuleCreate(%s): entry point '%s' has no Metal code (compile with the metal target)",
                               label ? label : "", e.name.c_str());
                    return nullptr;
                }
            MtlShaderModule* m = new MtlShaderModule;
            m->package = std::move(package);
            return m;
        }

        id<MTLFunction> GetFunction(srhiShaderModule* module, const PkgEntry* entry)
        {
            MtlShaderModule* m = static_cast<MtlShaderModule*>(module);
            auto it = m->functions.find(entry->name);
            if (it != m->functions.end())
                return it->second;
            NSError* error = nil;
            id<MTLLibrary> library;
            const PkgBlob* blob = entry->FindBlob(BlobKind::MetalLib);
            if (blob) {
                dispatch_data_t data = dispatch_data_create(blob->data.data(), blob->data.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
                library = [device newLibraryWithData:data error:&error];
            } else {
                blob = entry->FindBlob(BlobKind::MSL);
                NSString* source = [[NSString alloc] initWithBytes:blob->data.data() length:blob->data.size() encoding:NSUTF8StringEncoding];
                library = [device newLibraryWithSource:source options:nil error:&error];
            }
            if (!library) {
                SRHI_ERROR(this, "shader module '%s': failed to load the Metal library for '%s': %s", m->label.c_str(),
                           entry->name.c_str(), error.localizedDescription.UTF8String ?: "");
                return nil;
            }
            id<MTLFunction> fn = [library newFunctionWithName:ToNS(blob->functionName)];
            if (!fn)
                SRHI_ERROR(this, "shader module '%s': function '%s' not found in its Metal library", m->label.c_str(),
                           blob->functionName.c_str());
            m->functions[entry->name] = fn;
            return fn;
        }

        srhiResourceSetLayout* CreateResourceSetLayout(std::vector<srhiResourceSetLayoutEntry>&& entries, const char*) override
        {
            MtlResourceSetLayout* l = new MtlResourceSetLayout;
            l->entries = std::move(entries);
            return l;
        }

        srhiResourceSet* CreateResourceSet(srhiResourceSetLayout*, std::vector<srhiResourceBinding>&& bindings, const char*) override
        {
            MtlResourceSet* s = new MtlResourceSet;
            s->bindings = std::move(bindings);
            return s;
        }

        srhiRenderPipeline* CreateRenderPipeline(const srhiRenderPipelineDesc& d, PipelineLayoutInfo&& layout) override
        {
            const Package& pkg = d.module->package;
            MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
            pd.label = ToNS(d.label);
            pd.vertexFunction = GetFunction(d.module, pkg.FindEntry(d.vertexEntry));
            if (!pd.vertexFunction)
                return nullptr;
            if (d.fragmentEntry) {
                pd.fragmentFunction = GetFunction(d.module, pkg.FindEntry(d.fragmentEntry));
                if (!pd.fragmentFunction)
                    return nullptr;
            }

            MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
            for (uint32_t i = 0; i < d.vertexLayout.attributeCount; ++i) {
                const srhiVertexAttribute& a = d.vertexLayout.attributes[i];
                vd.attributes[a.location].format = ToMtl(a.format);
                vd.attributes[a.location].offset = a.offset;
                vd.attributes[a.location].bufferIndex = kVertexBufferBase - a.bufferIndex;
            }
            for (uint32_t i = 0; i < d.vertexLayout.bufferCount; ++i) {
                const srhiVertexBufferLayout& b = d.vertexLayout.buffers[i];
                MTLVertexBufferLayoutDescriptor* l = vd.layouts[kVertexBufferBase - i];
                l.stride = b.stride;
                l.stepFunction = b.stepMode == SRHI_VERTEX_STEP_MODE_INSTANCE ? MTLVertexStepFunctionPerInstance : MTLVertexStepFunctionPerVertex;
                l.stepRate = 1;
            }
            if (d.vertexLayout.attributeCount)
                pd.vertexDescriptor = vd;

            for (uint32_t i = 0; i < d.colorTargetCount; ++i) {
                const srhiColorTargetState& ct = d.colorTargets[i];
                MTLRenderPipelineColorAttachmentDescriptor* ca = pd.colorAttachments[i];
                ca.pixelFormat = ToMtl(ct.format);
                ca.blendingEnabled = ct.blend.enable;
                ca.sourceRGBBlendFactor = ToMtl(ct.blend.srcColor);
                ca.destinationRGBBlendFactor = ToMtl(ct.blend.dstColor);
                ca.rgbBlendOperation = ToMtl(ct.blend.colorOp);
                ca.sourceAlphaBlendFactor = ToMtl(ct.blend.srcAlpha);
                ca.destinationAlphaBlendFactor = ToMtl(ct.blend.dstAlpha);
                ca.alphaBlendOperation = ToMtl(ct.blend.alphaOp);
                MTLColorWriteMask mask = 0;
                if (ct.writeMask & SRHI_COLOR_WRITE_RED)
                    mask |= MTLColorWriteMaskRed;
                if (ct.writeMask & SRHI_COLOR_WRITE_GREEN)
                    mask |= MTLColorWriteMaskGreen;
                if (ct.writeMask & SRHI_COLOR_WRITE_BLUE)
                    mask |= MTLColorWriteMaskBlue;
                if (ct.writeMask & SRHI_COLOR_WRITE_ALPHA)
                    mask |= MTLColorWriteMaskAlpha;
                ca.writeMask = mask;
            }
            const srhiDepthStencilState& ds = d.depthStencil;
            if (ds.format != SRHI_FORMAT_UNDEFINED) {
                pd.depthAttachmentPixelFormat = ToMtl(ds.format);
                if (GetFormatInfo(ds.format).stencil)
                    pd.stencilAttachmentPixelFormat = ToMtl(ds.format);
            }
            pd.rasterSampleCount = d.sampleCount;
            pd.alphaToCoverageEnabled = d.alphaToCoverage;
            switch (d.topology) {
            case SRHI_PRIMITIVE_TOPOLOGY_POINT_LIST: pd.inputPrimitiveTopology = MTLPrimitiveTopologyClassPoint; break;
            case SRHI_PRIMITIVE_TOPOLOGY_LINE_LIST:
            case SRHI_PRIMITIVE_TOPOLOGY_LINE_STRIP: pd.inputPrimitiveTopology = MTLPrimitiveTopologyClassLine; break;
            default: pd.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle; break;
            }

            NSError* error = nil;
            id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:pd error:&error];
            if (!pso) {
                SRHI_ERROR(this, "srhiRenderPipelineCreate(%s): %s", d.label ? d.label : "", error.localizedDescription.UTF8String ?: "");
                return nullptr;
            }

            MTLDepthStencilDescriptor* dsd = [MTLDepthStencilDescriptor new];
            dsd.depthCompareFunction = ds.depthTestEnable ? ToMtl(ds.depthCompare) : MTLCompareFunctionAlways;
            dsd.depthWriteEnabled = ds.depthTestEnable && ds.depthWriteEnable;
            if (ds.stencilEnable) {
                auto face = [&](const srhiStencilFaceState& f) {
                    MTLStencilDescriptor* s = [MTLStencilDescriptor new];
                    s.stencilCompareFunction = ToMtl(f.compare);
                    s.stencilFailureOperation = ToMtl(f.failOp);
                    s.depthFailureOperation = ToMtl(f.depthFailOp);
                    s.depthStencilPassOperation = ToMtl(f.passOp);
                    s.readMask = ds.stencilReadMask;
                    s.writeMask = ds.stencilWriteMask;
                    return s;
                };
                dsd.frontFaceStencil = face(ds.stencilFront);
                dsd.backFaceStencil = face(ds.stencilBack);
            }

            MtlRenderPipeline* p = new MtlRenderPipeline;
            p->pso = pso;
            p->depthStencil = [device newDepthStencilStateWithDescriptor:dsd];
            p->layout = std::move(layout);
            switch (d.topology) {
            case SRHI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST: p->primitive = MTLPrimitiveTypeTriangle; break;
            case SRHI_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: p->primitive = MTLPrimitiveTypeTriangleStrip; break;
            case SRHI_PRIMITIVE_TOPOLOGY_LINE_LIST: p->primitive = MTLPrimitiveTypeLine; break;
            case SRHI_PRIMITIVE_TOPOLOGY_LINE_STRIP: p->primitive = MTLPrimitiveTypeLineStrip; break;
            case SRHI_PRIMITIVE_TOPOLOGY_POINT_LIST: p->primitive = MTLPrimitiveTypePoint; break;
            }
            return p;
        }

        srhiComputePipeline* CreateComputePipeline(const srhiComputePipelineDesc& d, PipelineLayoutInfo&& layout) override
        {
            id<MTLFunction> fn = GetFunction(d.module, d.module->package.FindEntry(d.entry));
            if (!fn)
                return nullptr;
            MTLComputePipelineDescriptor* pd = [MTLComputePipelineDescriptor new];
            pd.computeFunction = fn;
            pd.label = ToNS(d.label);
            NSError* error = nil;
            id<MTLComputePipelineState> pso = [device newComputePipelineStateWithDescriptor:pd
                                                                                    options:MTLPipelineOptionNone
                                                                                 reflection:nil
                                                                                      error:&error];
            if (!pso) {
                SRHI_ERROR(this, "srhiComputePipelineCreate(%s): %s", d.label ? d.label : "", error.localizedDescription.UTF8String ?: "");
                return nullptr;
            }
            MtlComputePipeline* p = new MtlComputePipeline;
            p->pso = pso;
            p->layout = std::move(layout);
            return p;
        }

        srhiQuerySet* CreateQuerySet(uint32_t count, const char* label) override
        {
            MTLCounterSampleBufferDescriptor* d = [MTLCounterSampleBufferDescriptor new];
            d.counterSet = timestampCounterSet;
            d.sampleCount = count;
            d.storageMode = MTLStorageModeShared;
            d.label = label ? ToNS(label) : @"timestamps";
            NSError* error = nil;
            id<MTLCounterSampleBuffer> buffer = [device newCounterSampleBufferWithDescriptor:d error:&error];
            if (!buffer) {
                SRHI_ERROR(this, "srhiQuerySetCreate(%s): %s", label ? label : "", error.localizedDescription.UTF8String ?: "");
                return nullptr;
            }
            MtlQuerySet* q = new MtlQuerySet;
            q->buffer = buffer;
            q->results.assign(count, 0);
            q->valid.assign(count, 0);
            [device sampleTimestamps:&q->cpuOrigin gpuTimestamp:&q->gpuOrigin];
            return q;
        }

        srhiCommandBuffer* BeginCommandBuffer(const char* label) override
        {
            MtlCommandBuffer* c = new MtlCommandBuffer;
            c->dev = this;
            c->cb = [queue commandBuffer];
            c->cb.label = ToNS(label);
            c->pending.cb = c->cb;
            return c;
        }

        srhiSwapchain* CreateSwapchain(const srhiSwapchainDesc& d) override
        {
            id obj = (__bridge id)d.nativeWindow;
            if (![obj isKindOfClass:[CAMetalLayer class]]) {
                SRHI_ERROR(this, "srhiSwapchainCreate: nativeWindow must be a CAMetalLayer* on Metal");
                return nullptr;
            }
            MtlSwapchain* sc = new MtlSwapchain;
            sc->dev = this;
            sc->layer = (CAMetalLayer*)obj;
            sc->layer.device = device;
            sc->layer.framebufferOnly = NO;
            sc->layer.maximumDrawableCount = 3;
            sc->layer.drawableSize = CGSizeMake(d.width, d.height);
            sc->Apply(d.colorSpace, d.vsync != 0);

            sc->image = new MtlTexture;
            sc->image->device = this;
            sc->image->isSwapchainImage = true;
            sc->image->label = d.label ? d.label : "swapchain";
            sc->imageView = new MtlTextureView;
            sc->imageView->texture = sc->image;
            sc->imageView->label = sc->image->label;
            sc->image->defaultView = sc->imageView;
            return sc;
        }
    };

    // =================================================================================================
    // Out-of-line implementations
    // =================================================================================================

    uint32_t MtlDevice::GetFormatSupport(srhiFormat format)
    {
        const FormatInfo& fi = GetFormatInfo(format);
        if (ToMtl(format) == MTLPixelFormatInvalid)
            return 0;
        if (fi.compressed) {
            bool bc = format >= SRHI_FORMAT_BC1_UNORM && format <= SRHI_FORMAT_BC7_UNORM_SRGB;
            bool ok = bc ? info.features.textureCompressionBC
                    : format == SRHI_FORMAT_ASTC_4x4_FLOAT ? info.features.textureCompressionASTCHDR
                                                           : info.features.textureCompressionASTC;
            return ok ? (SRHI_FORMAT_SUPPORT_SAMPLE | SRHI_FORMAT_SUPPORT_FILTER) : 0;
        }
        if (fi.depth)
            return SRHI_FORMAT_SUPPORT_SAMPLE | SRHI_FORMAT_SUPPORT_DEPTH_STENCIL | SRHI_FORMAT_SUPPORT_MSAA |
                   (format == SRHI_FORMAT_D16_UNORM || device.supports32BitFloatFiltering ? SRHI_FORMAT_SUPPORT_FILTER : 0);

        uint32_t flags = SRHI_FORMAT_SUPPORT_SAMPLE | SRHI_FORMAT_SUPPORT_RENDER_TARGET;
        bool is32Float = format == SRHI_FORMAT_R32_FLOAT || format == SRHI_FORMAT_RG32_FLOAT || format == SRHI_FORMAT_RGBA32_FLOAT;
        if (!fi.integer && (!is32Float || device.supports32BitFloatFiltering))
            flags |= SRHI_FORMAT_SUPPORT_FILTER;
        if (!fi.integer && !is32Float)
            flags |= SRHI_FORMAT_SUPPORT_BLEND | SRHI_FORMAT_SUPPORT_MSAA | SRHI_FORMAT_SUPPORT_RESOLVE;
        // Storage: the portable (WebGPU core) subset.
        switch (format) {
        case SRHI_FORMAT_RGBA8_UNORM:
        case SRHI_FORMAT_RGBA8_SNORM:
        case SRHI_FORMAT_RGBA8_UINT:
        case SRHI_FORMAT_RGBA16_FLOAT:
        case SRHI_FORMAT_R32_FLOAT:
        case SRHI_FORMAT_R32_UINT:
        case SRHI_FORMAT_R32_SINT:
        case SRHI_FORMAT_RG32_FLOAT:
        case SRHI_FORMAT_RG32_UINT:
        case SRHI_FORMAT_RGBA32_FLOAT:
        case SRHI_FORMAT_RGBA32_UINT: flags |= SRHI_FORMAT_SUPPORT_STORAGE; break;
        default: break;
        }
        if (format == SRHI_FORMAT_RGBA8_SNORM)
            flags &= ~(SRHI_FORMAT_SUPPORT_RENDER_TARGET | SRHI_FORMAT_SUPPORT_BLEND | SRHI_FORMAT_SUPPORT_MSAA | SRHI_FORMAT_SUPPORT_RESOLVE);
        return flags;
    }

    void MtlSwapchain::Apply(srhiColorSpace colorSpace, bool vsync)
    {
        CFStringRef name = kCGColorSpaceSRGB;
        switch (colorSpace) {
        case SRHI_COLOR_SPACE_SRGB:
            layer.pixelFormat = MTLPixelFormatBGRA8Unorm_sRGB;
            layer.wantsExtendedDynamicRangeContent = NO;
            break;
        case SRHI_COLOR_SPACE_HDR10_PQ:
            layer.pixelFormat = MTLPixelFormatRGB10A2Unorm;
            layer.wantsExtendedDynamicRangeContent = YES;
            name = kCGColorSpaceITUR_2100_PQ;
            break;
        case SRHI_COLOR_SPACE_EXTENDED_LINEAR_SRGB:
            layer.pixelFormat = MTLPixelFormatRGBA16Float;
            layer.wantsExtendedDynamicRangeContent = YES;
            name = kCGColorSpaceExtendedLinearSRGB;
            break;
        }
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(name);
        layer.colorspace = cs;
        CGColorSpaceRelease(cs);
#if TARGET_OS_OSX
        layer.displaySyncEnabled = vsync;
#endif
        drawable = nil;
        format = colorSpace == SRHI_COLOR_SPACE_HDR10_PQ ? SRHI_FORMAT_RGB10A2_UNORM
               : colorSpace == SRHI_COLOR_SPACE_EXTENDED_LINEAR_SRGB ? SRHI_FORMAT_RGBA16_FLOAT
                                                                    : SRHI_FORMAT_BGRA8_UNORM_SRGB;
    }

    void MtlSwapchain::GetDisplayInfo(srhiDisplayInfo& out)
    {
#if TARGET_OS_OSX
        NSScreen* screen = nil;
        id delegate = layer.delegate;
        if ([delegate isKindOfClass:[NSView class]])
            screen = ((NSView*)delegate).window.screen;
        if (!screen)
            screen = NSScreen.mainScreen;
        CGFloat potential = screen.maximumPotentialExtendedDynamicRangeColorComponentValue;
        out.hdrSupported = potential > 1.0;
        out.edrHeadroom = float(screen.maximumExtendedDynamicRangeColorComponentValue);
        out.sdrWhiteLevel = 100.0f;
        if (out.hdrSupported) {
            out.maxLuminance = float(potential * 100.0);
            out.maxFullFrameLuminance = out.maxLuminance;
        }
#endif
    }

    srhiTexture* MtlSwapchain::Acquire()
    {
        if (!drawable)
            drawable = [layer nextDrawable];
        if (!drawable)
            return nullptr;
        image->texture = drawable.texture;
        image->desc = {};
        image->desc.type = SRHI_TEXTURE_TYPE_2D;
        image->desc.format = format;
        image->desc.width = uint32_t(drawable.texture.width);
        image->desc.height = uint32_t(drawable.texture.height);
        image->desc.depthOrLayers = 1;
        image->desc.mipLevels = 1;
        image->desc.sampleCount = 1;
        image->desc.usage = SRHI_TEXTURE_USAGE_RENDER_TARGET | SRHI_TEXTURE_USAGE_COPY_DST | SRHI_TEXTURE_USAGE_COPY_SRC;
        image->desc.label = image->label.c_str();
        imageView->mtl = drawable.texture;
        imageView->desc = {};
        imageView->desc.format = format;
        imageView->desc.type = SRHI_TEXTURE_TYPE_2D;
        imageView->desc.mipCount = 1;
        imageView->desc.layerCount = 1;
        return image;
    }

    // --- Render encoder ---

    srhiRenderEncoder* MtlCommandBuffer::BeginRender(const srhiRenderPassDesc& desc)
    {
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        auto attach = [](MTLRenderPassAttachmentDescriptor* a, srhiTextureView* view) {
            MtlTextureView* v = static_cast<MtlTextureView*>(view);
            MtlTexture* t = static_cast<MtlTexture*>(view->texture);
            if (v->desc.format == t->desc.format) {
                a.texture = t->texture;
                a.level = v->desc.baseMip;
                if (t->desc.type == SRHI_TEXTURE_TYPE_3D)
                    a.depthPlane = v->desc.baseLayer;
                else
                    a.slice = v->desc.baseLayer;
            } else {
                a.texture = v->mtl;
            }
        };
        uint32_t width = 0, height = 0;
        for (uint32_t i = 0; i < desc.colorAttachmentCount; ++i) {
            const srhiColorAttachment& c = desc.colorAttachments[i];
            MTLRenderPassColorAttachmentDescriptor* a = rp.colorAttachments[i];
            attach(a, c.view);
            a.loadAction = ToMtl(c.loadAction);
            a.storeAction = ToMtl(c.storeAction);
            a.clearColor = MTLClearColorMake(c.clearColor[0], c.clearColor[1], c.clearColor[2], c.clearColor[3]);
            if (c.resolveView) {
                MtlTextureView* rv = static_cast<MtlTextureView*>(c.resolveView);
                MtlTexture* rt = static_cast<MtlTexture*>(c.resolveView->texture);
                if (rv->desc.format == rt->desc.format) {
                    a.resolveTexture = rt->texture;
                    a.resolveLevel = rv->desc.baseMip;
                    a.resolveSlice = rv->desc.baseLayer;
                } else
                    a.resolveTexture = rv->mtl;
            }
            width = MipDim(c.view->texture->desc.width, c.view->desc.baseMip);
            height = MipDim(c.view->texture->desc.height, c.view->desc.baseMip);
        }
        const srhiDepthAttachment& d = desc.depthAttachment;
        if (d.view) {
            attach(rp.depthAttachment, d.view);
            rp.depthAttachment.loadAction = ToMtl(d.depthLoadAction);
            rp.depthAttachment.storeAction = ToMtl(d.depthStoreAction);
            rp.depthAttachment.clearDepth = d.clearDepth;
            if (GetFormatInfo(d.view->desc.format).stencil) {
                attach(rp.stencilAttachment, d.view);
                rp.stencilAttachment.loadAction = ToMtl(d.stencilLoadAction);
                rp.stencilAttachment.storeAction = ToMtl(d.stencilStoreAction);
                rp.stencilAttachment.clearStencil = d.clearStencil;
            }
            width = MipDim(d.view->texture->desc.width, d.view->desc.baseMip);
            height = MipDim(d.view->texture->desc.height, d.view->desc.baseMip);
        }
        if (desc.timestampWrites.querySet) {
            MtlQuerySet* qs = static_cast<MtlQuerySet*>(desc.timestampWrites.querySet);
            MTLRenderPassSampleBufferAttachmentDescriptor* sa = rp.sampleBufferAttachments[0];
            sa.sampleBuffer = qs->buffer;
            uint32_t b = desc.timestampWrites.beginIndex, e = desc.timestampWrites.endIndex;
            // Begin at the start of the fragment stage: on tile-based GPUs a pass's vertex stage can start long
            // before the previous passes finish, which would make per-pass timings overlap.
            sa.startOfVertexSampleIndex = MTLCounterDontSample;
            sa.endOfVertexSampleIndex = MTLCounterDontSample;
            sa.startOfFragmentSampleIndex = b == SRHI_QUERY_UNUSED ? MTLCounterDontSample : b;
            sa.endOfFragmentSampleIndex = e == SRHI_QUERY_UNUSED ? MTLCounterDontSample : e;
            for (uint32_t idx : {b, e})
                if (idx != SRHI_QUERY_UNUSED) {
                    qs->valid[idx] = 0;
                    pending.queries.push_back({qs, idx});
                }
        }

        render.owner = this;
        render.enc = [cb renderCommandEncoderWithDescriptor:rp];
        render.enc.label = ToNS(desc.label);
        render.bindings.Reset();
        render.indexBuffer = nil;
        render.indexOffset = 0;
        render.width = width;
        render.height = height;
        return &render;
    }

    void MtlRenderEncoder::End()
    {
        [enc endEncoding];
        enc = nil;
        indexBuffer = nil;
        bindings.Reset();
    }

    void MtlRenderEncoder::SetPipeline(srhiRenderPipeline* p)
    {
        MtlRenderPipeline* mp = static_cast<MtlRenderPipeline*>(p);
        const srhiRenderPipelineDesc& d = p->desc;
        [enc setRenderPipelineState:mp->pso];
        [enc setDepthStencilState:mp->depthStencil];
        [enc setCullMode:d.raster.cullMode == SRHI_CULL_MODE_NONE    ? MTLCullModeNone
                         : d.raster.cullMode == SRHI_CULL_MODE_FRONT ? MTLCullModeFront
                                                                     : MTLCullModeBack];
        [enc setFrontFacingWinding:d.raster.frontFace == SRHI_FRONT_FACE_CLOCKWISE ? MTLWindingClockwise : MTLWindingCounterClockwise];
        [enc setTriangleFillMode:d.raster.fillMode == SRHI_FILL_MODE_WIREFRAME ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];
        [enc setDepthBias:float(d.raster.depthBias) slopeScale:d.raster.depthBiasSlopeScale clamp:d.raster.depthBiasClamp];
        [enc setDepthClipMode:d.raster.depthClamp ? MTLDepthClipModeClamp : MTLDepthClipModeClip];
        bindings.MarkAllDirty();
    }

    bool MtlRenderEncoder::Flush()
    {
        id<MTLRenderCommandEncoder> e = enc;
        return bindings.Flush(
            cmd->device, pipeline->layout, pipeline->label.c_str(),
            [e](const BindingSlot& slot, const srhiResourceBinding& b) {
                bool vs = slot.stages & SRHI_SHADER_STAGE_VERTEX, fs = slot.stages & SRHI_SHADER_STAGE_FRAGMENT;
                switch (slot.metalClass) {
                case MetalClass::Buffer: {
                    id<MTLBuffer> buf = static_cast<MtlBuffer*>(b.buffer)->buffer;
                    if (vs)
                        [e setVertexBuffer:buf offset:b.offset atIndex:slot.metalSlot];
                    if (fs)
                        [e setFragmentBuffer:buf offset:b.offset atIndex:slot.metalSlot];
                    break;
                }
                case MetalClass::Texture: {
                    id<MTLTexture> tex = static_cast<MtlTextureView*>(b.textureView)->mtl;
                    if (vs)
                        [e setVertexTexture:tex atIndex:slot.metalSlot];
                    if (fs)
                        [e setFragmentTexture:tex atIndex:slot.metalSlot];
                    break;
                }
                case MetalClass::Sampler: {
                    id<MTLSamplerState> s = static_cast<MtlSampler*>(b.sampler)->sampler;
                    if (vs)
                        [e setVertexSamplerState:s atIndex:slot.metalSlot];
                    if (fs)
                        [e setFragmentSamplerState:s atIndex:slot.metalSlot];
                    break;
                }
                default: break;
                }
            },
            [e](const BindingSlot& slot, const void* data, uint32_t size) {
                if (slot.stages & SRHI_SHADER_STAGE_VERTEX)
                    [e setVertexBytes:data length:size atIndex:slot.metalSlot];
                if (slot.stages & SRHI_SHADER_STAGE_FRAGMENT)
                    [e setFragmentBytes:data length:size atIndex:slot.metalSlot];
            });
    }

    // --- Compute encoder ---

    srhiComputeEncoder* MtlCommandBuffer::BeginCompute(const srhiComputePassDesc& desc)
    {
        MTLComputePassDescriptor* cp = [MTLComputePassDescriptor computePassDescriptor];
        if (desc.timestampWrites.querySet) {
            MtlQuerySet* qs = static_cast<MtlQuerySet*>(desc.timestampWrites.querySet);
            MTLComputePassSampleBufferAttachmentDescriptor* sa = cp.sampleBufferAttachments[0];
            sa.sampleBuffer = qs->buffer;
            uint32_t b = desc.timestampWrites.beginIndex, e = desc.timestampWrites.endIndex;
            sa.startOfEncoderSampleIndex = b == SRHI_QUERY_UNUSED ? MTLCounterDontSample : b;
            sa.endOfEncoderSampleIndex = e == SRHI_QUERY_UNUSED ? MTLCounterDontSample : e;
            for (uint32_t idx : {b, e})
                if (idx != SRHI_QUERY_UNUSED) {
                    qs->valid[idx] = 0;
                    pending.queries.push_back({qs, idx});
                }
        }
        compute.owner = this;
        compute.enc = [cb computeCommandEncoderWithDescriptor:cp];
        compute.enc.label = ToNS(desc.label);
        compute.bindings.Reset();
        return &compute;
    }

    void MtlComputeEncoder::End()
    {
        [enc endEncoding];
        enc = nil;
        bindings.Reset();
    }

    bool MtlComputeEncoder::Flush()
    {
        id<MTLComputeCommandEncoder> e = enc;
        return bindings.Flush(
            cmd->device, pipeline->layout, pipeline->label.c_str(),
            [e](const BindingSlot& slot, const srhiResourceBinding& b) {
                switch (slot.metalClass) {
                case MetalClass::Buffer: [e setBuffer:static_cast<MtlBuffer*>(b.buffer)->buffer offset:b.offset atIndex:slot.metalSlot]; break;
                case MetalClass::Texture: [e setTexture:static_cast<MtlTextureView*>(b.textureView)->mtl atIndex:slot.metalSlot]; break;
                case MetalClass::Sampler: [e setSamplerState:static_cast<MtlSampler*>(b.sampler)->sampler atIndex:slot.metalSlot]; break;
                default: break;
                }
            },
            [e](const BindingSlot& slot, const void* data, uint32_t size) { [e setBytes:data length:size atIndex:slot.metalSlot]; });
    }

    // --- Command buffer ---

    void MtlCommandBuffer::ReadBuffer(srhiBuffer* buffer, uint64_t offset, uint64_t size, srhiReadbackCallback callback, void* userData)
    {
        id<MTLBuffer> staging = [dev->device newBufferWithLength:size options:MTLResourceStorageModeShared];
        id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        b.label = @"readback";
        [b copyFromBuffer:static_cast<MtlBuffer*>(buffer)->buffer sourceOffset:offset toBuffer:staging destinationOffset:0 size:size];
        [b endEncoding];
        pending.readbacks.push_back({staging, size, callback, userData});
    }

    void MtlCommandBuffer::ReadTexture(srhiTexture* texture, uint32_t mip, uint32_t layer, srhiReadbackCallback callback, void* userData)
    {
        MtlTexture* t = static_cast<MtlTexture*>(texture);
        uint32_t w = MipDim(t->desc.width, mip), h = MipDim(t->desc.height, mip);
        uint32_t rowBytes = RowBytes(t->desc.format, w), rows = RowCount(t->desc.format, h);
        uint64_t size = uint64_t(rowBytes) * rows;
        id<MTLBuffer> staging = [dev->device newBufferWithLength:size options:MTLResourceStorageModeShared];
        bool is3D = t->desc.type == SRHI_TEXTURE_TYPE_3D;
        id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        b.label = @"readback";
        [b copyFromTexture:t->texture
                         sourceSlice:is3D ? 0 : layer
                         sourceLevel:mip
                        sourceOrigin:MTLOriginMake(0, 0, is3D ? layer : 0)
                          sourceSize:MTLSizeMake(w, h, 1)
                            toBuffer:staging
                   destinationOffset:0
              destinationBytesPerRow:rowBytes
            destinationBytesPerImage:size];
        [b endEncoding];
        pending.readbacks.push_back({staging, size, callback, userData});
    }

    void MtlCommandBuffer::Commit(srhiCompletionCallback callback, void* userData)
    {
        dev->FlushUploads();
        [cb commit];
        pending.callback = callback;
        pending.userData = userData;
        dev->inFlight.push_back(std::move(pending));
        delete this;
    }
} // namespace

// =====================================================================================================
// Factory
// =====================================================================================================

srhiDevice* srhiCreateDeviceMetal(const srhiDeviceDesc& desc)
{
    @autoreleasepool {
        id<MTLDevice> device = nil;
#if TARGET_OS_OSX
        if (desc.lowPower)
            for (id<MTLDevice> d in MTLCopyAllDevices())
                if (d.isLowPower)
                    device = d;
#endif
        if (!device)
            device = MTLCreateSystemDefaultDevice();
        if (!device || ![device supportsFamily:MTLGPUFamilyMetal3]) {
            srhi::Log(nullptr, SRHI_LOG_SEVERITY_ERROR, "Metal: no Metal 3 capable device found");
            return nullptr;
        }

        MtlDevice* d = new MtlDevice;
        d->desc = desc;
        d->device = device;
        d->queue = [device newCommandQueue];
        d->queue.label = @"SimpleRHI queue";
        d->adapterName = device.name.UTF8String;

        if ([device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary])
            for (id<MTLCounterSet> set in device.counterSets)
                if ([set.name isEqualToString:MTLCommonCounterSetTimestamp])
                    d->timestampCounterSet = set;

        srhiDeviceInfo& info = d->info;
        info.backend = SRHI_BACKEND_METAL;
        info.backendName = "Metal 3";
        info.features.timestampQueries = d->timestampCounterSet != nil;
        info.features.textureCompressionBC = device.supportsBCTextureCompression;
        info.features.textureCompressionASTC = [device supportsFamily:MTLGPUFamilyApple2];
        info.features.textureCompressionASTCHDR = [device supportsFamily:MTLGPUFamilyApple6];
        info.features.fillModeWireframe = 1;
        info.features.depthClamp = 1;
        info.features.indirectFirstInstance = 1;
        info.features.cubeArrays = 1;

        srhiDeviceLimits& l = info.limits;
        l.maxTextureDimension2D = 16384;
        l.maxTextureDimension3D = 2048;
        l.maxTextureArrayLayers = 2048;
        l.maxColorAttachments = 8;
        l.maxComputeWorkgroupSizeX = l.maxComputeWorkgroupSizeY = l.maxComputeWorkgroupSizeZ = 1024;
        l.maxComputeInvocationsPerWorkgroup = uint32_t(device.maxThreadsPerThreadgroup.width);
        l.maxComputeWorkgroupsPerDimension = 65535;
        l.maxStorageBuffersPerStage = 16;
        l.maxStorageTexturesPerStage = 8;
        l.maxSampledTexturesPerStage = 64;
        l.maxSamplersPerStage = 16;
        l.uniformBufferOffsetAlignment = 256;
        l.storageBufferOffsetAlignment = 256;
        l.maxUniformBufferBindingSize = 65536;
        l.maxStorageBufferBindingSize = std::min<uint64_t>(device.maxBufferLength, 1ull << 31);
        return d;
    }
}
