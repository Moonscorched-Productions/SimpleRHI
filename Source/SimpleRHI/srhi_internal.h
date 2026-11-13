/**
 * @ Author: Amélie Heinrich
 * @ Create Time: 2026-11-13 00:00:00
 * @ Copyright: Copyright (c) 2026 Amélie Heinrich. All rights reserved.
 */

// Internal object model.
//
// Every opaque handle in SimpleRHI.h is a C++ class here; backends derive from them. srhi_common.cpp
// implements the C API: it validates arguments, resolves defaults, and forwards to the virtual methods.
// Backends can therefore assume well-formed, fully resolved inputs.

#pragma once

#include <SimpleRHI.h>

#include "srhi_package.h"

#include <cstdarg>
#include <cstdint>
#include <string>
#include <vector>

namespace srhi
{
    // --- Formats ----------------------------------------------------------------------------------------

    struct FormatInfo
    {
        const char* name;
        /// @brief Bytes per texel, or per block for compressed formats.
        uint32_t blockBytes;
        uint32_t blockWidth;
        uint32_t blockHeight;
        bool depth;
        bool stencil;
        bool srgb;
        bool compressed;
        bool integer;
        /// @brief The sRGB / linear twin, or UNDEFINED.
        srhiFormat srgbTwin;
    };

    const FormatInfo& GetFormatInfo(srhiFormat format);
    bool FormatsViewCompatible(srhiFormat a, srhiFormat b);
    uint32_t FullMipCount(uint32_t width, uint32_t height, uint32_t depth);
    inline uint32_t MipDim(uint32_t dim, uint32_t mip) { return dim >> mip ? dim >> mip : 1; }
    /// @brief Bytes in one tightly packed row of a mip (rows of blocks for compressed formats).
    uint32_t RowBytes(srhiFormat format, uint32_t width);
    /// @brief Number of rows (of blocks) for a given height.
    uint32_t RowCount(srhiFormat format, uint32_t height);
    inline uint64_t AlignUp(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

    void Log(const srhiDevice* device, srhiLogSeverity severity, const char* fmt, ...);
#define SRHI_ERROR(device, ...) ::srhi::Log((device), SRHI_LOG_SEVERITY_ERROR, __VA_ARGS__)
#define SRHI_WARN(device, ...) ::srhi::Log((device), SRHI_LOG_SEVERITY_WARNING, __VA_ARGS__)
#define SRHI_INFO(device, ...) ::srhi::Log((device), SRHI_LOG_SEVERITY_INFO, __VA_ARGS__)

    // --- Pipeline binding tables ------------------------------------------------------------------------

    /// @brief Where one resource-set layout entry lands in a pipeline, per backend.
    struct BindingSlot
    {
        bool used = false;
        /// @brief srhiShaderStageFlags of the entry points that use it.
        uint32_t stages = 0;
        srhiBindingType type = SRHI_BINDING_TYPE_UNIFORM_BUFFER;
        D3DClass d3dClass = D3DClass::None;
        uint32_t d3dSlot = 0;
        MetalClass metalClass = MetalClass::None;
        uint32_t metalSlot = 0;
    };

    /// @brief Resolved binding information for a pipeline, built by the common layer from the shader
    ///        package and the pipeline's set layouts.
    struct PipelineLayoutInfo
    {
        srhiResourceSetLayout* setLayouts[SRHI_MAX_RESOURCE_SETS] = {};
        /// @brief slots[set][i] matches setLayouts[set]->entries[i].
        std::vector<BindingSlot> slots[SRHI_MAX_RESOURCE_SETS];
        BindingSlot uniforms[SRHI_MAX_UNIFORM_SLOTS];
        uint32_t uniformSizes[SRHI_MAX_UNIFORM_SLOTS] = {};
    };

    /// @brief Stride of one vertex attribute format in bytes.
    uint32_t VertexFormatSize(srhiVertexFormat format);
} // namespace srhi

// =====================================================================================================
// Objects
// =====================================================================================================

struct srhiBuffer
{
    virtual ~srhiBuffer() = default;
    virtual void* GetNative() = 0;

    srhiDevice* device = nullptr;
    srhiBufferDesc desc = {};
    std::string label;
};

struct srhiTextureView
{
    virtual ~srhiTextureView() = default;

    srhiTexture* texture = nullptr;
    /// @brief Fully resolved: format set, counts non-zero.
    srhiTextureViewDesc desc = {};
    std::string label;
};

struct srhiTexture
{
    virtual ~srhiTexture() = default;
    virtual void* GetNative() = 0;

    srhiDevice* device = nullptr;
    /// @brief Fully resolved: depthOrLayers, mipLevels and sampleCount are >= 1, cube layer counts include faces.
    srhiTextureDesc desc = {};
    std::string label;
    /// @brief Owned. NULL for swapchain textures until the backend creates it.
    srhiTextureView* defaultView = nullptr;
    /// @brief Swapchain images are owned by the swapchain.
    bool isSwapchainImage = false;

    uint32_t LayerCount() const { return desc.type == SRHI_TEXTURE_TYPE_3D ? 1 : desc.depthOrLayers; }
    uint32_t Depth(uint32_t mip) const { return desc.type == SRHI_TEXTURE_TYPE_3D ? srhi::MipDim(desc.depthOrLayers, mip) : 1; }
};

struct srhiSampler
{
    virtual ~srhiSampler() = default;

    srhiDevice* device = nullptr;
    srhiSamplerDesc desc = {};
    std::string label;
};

struct srhiShaderModule
{
    virtual ~srhiShaderModule() = default;

    srhiDevice* device = nullptr;
    srhi::Package package;
    std::string label;
};

struct srhiResourceSetLayout
{
    virtual ~srhiResourceSetLayout() = default;

    srhiDevice* device = nullptr;
    /// @brief Sorted by binding; stages resolved (never 0).
    std::vector<srhiResourceSetLayoutEntry> entries;
    std::string label;

    int FindEntry(uint32_t binding) const
    {
        for (size_t i = 0; i < entries.size(); ++i)
            if (entries[i].binding == binding)
                return int(i);
        return -1;
    }
};

struct srhiResourceSet
{
    virtual ~srhiResourceSet() = default;

    srhiDevice* device = nullptr;
    srhiResourceSetLayout* layout = nullptr;
    /// @brief bindings[i] matches layout->entries[i]; buffer sizes resolved.
    std::vector<srhiResourceBinding> bindings;
    std::string label;
};

struct srhiRenderPipeline
{
    virtual ~srhiRenderPipeline() = default;

    srhiDevice* device = nullptr;
    srhiRenderPipelineDesc desc = {};
    srhi::PipelineLayoutInfo layout;
    std::string label;
};

struct srhiComputePipeline
{
    virtual ~srhiComputePipeline() = default;

    srhiDevice* device = nullptr;
    srhiComputePipelineDesc desc = {};
    srhi::PipelineLayoutInfo layout;
    uint32_t threadgroupSize[3] = {1, 1, 1};
    std::string label;
};

struct srhiQuerySet
{
    virtual ~srhiQuerySet() = default;
    virtual bool GetResults(uint32_t first, uint32_t count, uint64_t* outNanoseconds) = 0;

    srhiDevice* device = nullptr;
    uint32_t count = 0;
    std::string label;
};

struct srhiRenderEncoder
{
    virtual ~srhiRenderEncoder() = default;
    virtual void End() = 0;
    virtual void SetPipeline(srhiRenderPipeline* pipeline) = 0;
    virtual void SetVertexBuffer(uint32_t index, srhiBuffer* buffer, uint64_t offset) = 0;
    virtual void SetIndexBuffer(srhiBuffer* buffer, srhiIndexType type, uint64_t offset) = 0;
    virtual void SetResourceSet(uint32_t index, srhiResourceSet* set) = 0;
    virtual void SetUniforms(uint32_t slot, const void* data, uint32_t size) = 0;
    virtual void SetViewport(const srhiViewport& viewport) = 0;
    virtual void SetScissor(const srhiScissorRect& rect) = 0;
    virtual void SetBlendColor(const float color[4]) = 0;
    virtual void SetStencilReference(uint32_t reference) = 0;
    virtual void Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) = 0;
    virtual void DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t baseVertex,
                             uint32_t firstInstance) = 0;
    virtual void DrawIndirect(srhiBuffer* buffer, uint64_t offset) = 0;
    virtual void DrawIndexedIndirect(srhiBuffer* buffer, uint64_t offset) = 0;
    virtual void PushDebugGroup(const char* name) = 0;
    virtual void PopDebugGroup() = 0;
    virtual void InsertDebugMarker(const char* name) = 0;

    srhiCommandBuffer* cmd = nullptr;
    srhiRenderPipeline* pipeline = nullptr;
    bool indexBufferSet = false;
    srhiIndexType indexType = SRHI_INDEX_TYPE_UINT16;
    /// @brief Attachment formats, to validate pipelines against the pass.
    srhiFormat colorFormats[SRHI_MAX_COLOR_ATTACHMENTS] = {};
    uint32_t colorCount = 0;
    srhiFormat depthFormat = SRHI_FORMAT_UNDEFINED;
    uint32_t sampleCount = 1;
};

struct srhiComputeEncoder
{
    virtual ~srhiComputeEncoder() = default;
    virtual void End() = 0;
    virtual void SetPipeline(srhiComputePipeline* pipeline) = 0;
    virtual void SetResourceSet(uint32_t index, srhiResourceSet* set) = 0;
    virtual void SetUniforms(uint32_t slot, const void* data, uint32_t size) = 0;
    virtual void Dispatch(uint32_t x, uint32_t y, uint32_t z) = 0;
    virtual void DispatchIndirect(srhiBuffer* buffer, uint64_t offset) = 0;
    virtual void PushDebugGroup(const char* name) = 0;
    virtual void PopDebugGroup() = 0;
    virtual void InsertDebugMarker(const char* name) = 0;

    srhiCommandBuffer* cmd = nullptr;
    srhiComputePipeline* pipeline = nullptr;
};

struct srhiBlitEncoder
{
    virtual ~srhiBlitEncoder() = default;
    virtual void End() = 0;
    virtual void CopyBufferToBuffer(srhiBuffer* src, uint64_t srcOffset, srhiBuffer* dst, uint64_t dstOffset,
                                    uint64_t size) = 0;
    virtual void CopyBufferToTexture(const srhiBufferCopyLocation& src, const srhiTextureCopyLocation& dst,
                                     uint32_t width, uint32_t height, uint32_t depth) = 0;
    virtual void CopyTextureToBuffer(const srhiTextureCopyLocation& src, const srhiBufferCopyLocation& dst,
                                     uint32_t width, uint32_t height, uint32_t depth) = 0;
    virtual void CopyTextureToTexture(const srhiTextureCopyLocation& src, const srhiTextureCopyLocation& dst,
                                      uint32_t width, uint32_t height, uint32_t depth) = 0;
    virtual void GenerateMipmaps(srhiTexture* texture) = 0;
    virtual void PushDebugGroup(const char* name) = 0;
    virtual void PopDebugGroup() = 0;

    srhiCommandBuffer* cmd = nullptr;
};

struct srhiCommandBuffer
{
    virtual ~srhiCommandBuffer() = default;
    /// @brief Each Begin* returns an encoder owned by the command buffer, valid until its End().
    virtual srhiRenderEncoder* BeginRender(const srhiRenderPassDesc& desc) = 0;
    virtual srhiComputeEncoder* BeginCompute(const srhiComputePassDesc& desc) = 0;
    virtual srhiBlitEncoder* BeginBlit(const char* label) = 0;
    /// @brief Submits and deletes the command buffer.
    virtual void Commit(srhiCompletionCallback callback, void* userData) = 0;
    virtual void PushDebugGroup(const char* name) = 0;
    virtual void PopDebugGroup() = 0;
    virtual void ReadBuffer(srhiBuffer* buffer, uint64_t offset, uint64_t size, srhiReadbackCallback callback,
                            void* userData) = 0;
    virtual void ReadTexture(srhiTexture* texture, uint32_t mip, uint32_t layer, srhiReadbackCallback callback,
                             void* userData) = 0;
    virtual void EncodeNative(srhiNativeEncodeCallback callback, void* userData) = 0;
    virtual void Present(srhiSwapchain* swapchain) = 0;

    srhiDevice* device = nullptr;
    /// @brief Set by the common layer while an encoder is open.
    bool encoderOpen = false;
    std::string label;
};

struct srhiSwapchain
{
    virtual ~srhiSwapchain() = default;
    virtual void Resize(uint32_t width, uint32_t height) = 0;
    virtual bool Configure(srhiColorSpace colorSpace, bool vsync) = 0;
    virtual void GetDisplayInfo(srhiDisplayInfo& out) = 0;
    virtual srhiTexture* Acquire() = 0;

    srhiDevice* device = nullptr;
    srhiSwapchainDesc desc = {};
    srhiFormat format = SRHI_FORMAT_UNDEFINED;
    std::string label;
};

struct srhiDevice
{
    virtual ~srhiDevice() = default;

    virtual uint32_t GetFormatSupport(srhiFormat format) = 0;
    virtual void Poll() = 0;
    virtual void WaitIdle() = 0;
    virtual void* GetNative() = 0;
    virtual void* GetNativeQueue() = 0;

    virtual srhiBuffer* CreateBuffer(const srhiBufferDesc& desc) = 0;
    /// @brief offset/size validated and 4-byte aligned.
    virtual void WriteBuffer(srhiBuffer* buffer, uint64_t offset, const void* data, uint64_t size) = 0;
    virtual srhiTexture* CreateTexture(const srhiTextureDesc& desc) = 0;
    /// @brief region fully resolved and validated; bytesPerRow is never 0; for 3D, rowsPerImage rows per slice.
    virtual void WriteTexture(srhiTexture* texture, uint32_t mip, uint32_t layer, const srhiTextureRegion& region,
                              const void* data, uint32_t bytesPerRow, uint32_t rowsPerImage) = 0;
    /// @brief desc fully resolved.
    virtual srhiTextureView* CreateTextureView(srhiTexture* texture, const srhiTextureViewDesc& desc) = 0;
    virtual srhiSampler* CreateSampler(const srhiSamplerDesc& desc) = 0;
    virtual srhiShaderModule* CreateShaderModule(srhi::Package&& package, const char* label) = 0;
    /// @brief Entries sorted, stages resolved.
    virtual srhiResourceSetLayout* CreateResourceSetLayout(std::vector<srhiResourceSetLayoutEntry>&& entries,
                                                           const char* label) = 0;
    /// @brief bindings[i] matches layout->entries[i].
    virtual srhiResourceSet* CreateResourceSet(srhiResourceSetLayout* layout, std::vector<srhiResourceBinding>&& bindings,
                                               const char* label) = 0;
    virtual srhiRenderPipeline* CreateRenderPipeline(const srhiRenderPipelineDesc& desc, srhi::PipelineLayoutInfo&& layout) = 0;
    virtual srhiComputePipeline* CreateComputePipeline(const srhiComputePipelineDesc& desc, srhi::PipelineLayoutInfo&& layout) = 0;
    virtual srhiQuerySet* CreateQuerySet(uint32_t count, const char* label) = 0;
    virtual srhiCommandBuffer* BeginCommandBuffer(const char* label) = 0;
    virtual srhiSwapchain* CreateSwapchain(const srhiSwapchainDesc& desc) = 0;

    srhiDeviceDesc desc = {};
    srhiDeviceInfo info = {};
    std::string adapterName;
    /// @brief Live object count, to report leaks at destruction.
    int64_t liveObjects = 0;
};

// =====================================================================================================
// Backend factories (return NULL when unavailable)
// =====================================================================================================

srhiDevice* srhiCreateDeviceMetal(const srhiDeviceDesc& desc);
srhiDevice* srhiCreateDeviceD3D11(const srhiDeviceDesc& desc);
srhiDevice* srhiCreateDeviceWebGPU(const srhiDeviceDesc& desc);
