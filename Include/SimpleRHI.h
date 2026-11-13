/**
 * @ Author: Amélie Heinrich
 * @ Create Time: 2026-11-13 00:00:00
 * @ Copyright: Copyright (c) 2026 Amélie Heinrich. All rights reserved.
 */

/**
 * @file SimpleRHI.h
 * @brief SimpleRHI: a D3D11-feature-level RHI with a Metal-shaped API, running on D3D11, Metal 3 and WebGPU.
 *
 * Single public header. It declares both the runtime (link `SimpleRHI`) and the offline shader compiler
 * (link `SimpleRHICompiler`, which pulls in Slang). The runtime never depends on Slang.
 *
 * Model in one paragraph:
 *   - One device, one queue. Commands are recorded into a command buffer through render, compute and blit
 *     encoders (one open encoder at a time), then committed. Command buffers execute in commit order.
 *   - There are no barriers. Hazards between passes are tracked by the backend.
 *   - Resources are bound in *resource sets* (immutable groups, WebGPU bind groups) at set indices 0..2.
 *     Set 3 is reserved for *transient uniforms*: small per-draw constants pushed with SetUniforms.
 *   - `srhiBufferWrite` / `srhiTextureWrite` are queue-ordered: the data is visible to every command buffer
 *     committed after the call, including one that was already being recorded when the call was made.
 *   - GPU → CPU readback is asynchronous only. Callbacks fire from `srhiDevicePoll` / `srhiDeviceWaitIdle`.
 *   - Lifetime: objects referenced by a command buffer must stay alive until that command buffer is committed.
 *     After the commit, destroying them is safe even while the GPU still uses them.
 *
 * Shader conventions (Slang, see Source/SimpleRHI/Shaders/SimpleRHI.slang):
 *   - Declare every resource as a global with `[vk::binding(binding, set)]`, set in 0..2.
 *   - Transient uniforms: `[vk::binding(slot, 3)] ConstantBuffer<T> name;`, slot in 0..SRHI_MAX_UNIFORM_SLOTS-1.
 *   - A render pipeline's vertex and fragment entry points must come from the same shader module, which
 *     guarantees a single resource layout across both stages.
 *   - Uniform and structured buffer layouts must be identical on every target: stick to 4-byte scalars,
 *     float2/float4 and float4x4, and pad float3 to float4. The compiler rejects layouts that differ.
 *   - Matrices are column-major in memory (glm-compatible), and `mul(M, v)` matches `M * v` on the CPU.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// =====================================================================================================
// Basic types
// =====================================================================================================

/// @brief Boolean type. 0 is false, anything else is true.
typedef uint32_t srhiBool;

#define SRHI_MAX_COLOR_ATTACHMENTS 8
#define SRHI_MAX_VERTEX_BUFFERS 8
#define SRHI_MAX_VERTEX_ATTRIBUTES 16
/// @brief Resource sets usable by the application (0..2). Set 3 is the transient uniform set.
#define SRHI_MAX_RESOURCE_SETS 3
#define SRHI_UNIFORM_SET_INDEX 3
#define SRHI_MAX_UNIFORM_SLOTS 4
/// @brief Maximum size of a single SetUniforms payload (the Metal setBytes limit).
#define SRHI_MAX_UNIFORM_SIZE 4096
/// @brief Required alignment of bytesPerRow for buffer <-> texture copies (WebGPU rule, applied everywhere).
#define SRHI_COPY_BYTES_PER_ROW_ALIGNMENT 256
#define SRHI_WHOLE_SIZE (~0ull)
#define SRHI_ALL_MIPS (~0u)
#define SRHI_ALL_LAYERS (~0u)
#define SRHI_QUERY_UNUSED (~0u)

typedef struct srhiDevice srhiDevice;
typedef struct srhiBuffer srhiBuffer;
typedef struct srhiTexture srhiTexture;
typedef struct srhiTextureView srhiTextureView;
typedef struct srhiSampler srhiSampler;
typedef struct srhiShaderModule srhiShaderModule;
typedef struct srhiResourceSetLayout srhiResourceSetLayout;
typedef struct srhiResourceSet srhiResourceSet;
typedef struct srhiRenderPipeline srhiRenderPipeline;
typedef struct srhiComputePipeline srhiComputePipeline;
typedef struct srhiQuerySet srhiQuerySet;
typedef struct srhiCommandBuffer srhiCommandBuffer;
typedef struct srhiRenderEncoder srhiRenderEncoder;
typedef struct srhiComputeEncoder srhiComputeEncoder;
typedef struct srhiBlitEncoder srhiBlitEncoder;
typedef struct srhiSwapchain srhiSwapchain;

// =====================================================================================================
// Enums
// =====================================================================================================

typedef enum srhiBackend {
    /// @brief D3D11 on Windows, Metal on Apple platforms, WebGPU on the web.
    SRHI_BACKEND_AUTO = 0,
    SRHI_BACKEND_D3D11,
    SRHI_BACKEND_METAL,
    SRHI_BACKEND_WEBGPU,
} srhiBackend;

typedef enum srhiLogSeverity {
    SRHI_LOG_SEVERITY_INFO,
    SRHI_LOG_SEVERITY_WARNING,
    /// @brief An operation failed. The object being created is NULL, or the command was dropped.
    SRHI_LOG_SEVERITY_ERROR,
} srhiLogSeverity;

typedef enum srhiFormat {
    SRHI_FORMAT_UNDEFINED = 0,

    SRHI_FORMAT_R8_UNORM,
    SRHI_FORMAT_RG8_UNORM,
    SRHI_FORMAT_RGBA8_UNORM,
    SRHI_FORMAT_RGBA8_UNORM_SRGB,
    SRHI_FORMAT_RGBA8_SNORM,
    SRHI_FORMAT_RGBA8_UINT,
    SRHI_FORMAT_BGRA8_UNORM,
    SRHI_FORMAT_BGRA8_UNORM_SRGB,
    SRHI_FORMAT_R16_FLOAT,
    SRHI_FORMAT_RG16_FLOAT,
    SRHI_FORMAT_RGBA16_FLOAT,
    SRHI_FORMAT_R16_UINT,
    SRHI_FORMAT_R32_FLOAT,
    SRHI_FORMAT_R32_UINT,
    SRHI_FORMAT_R32_SINT,
    SRHI_FORMAT_RG32_FLOAT,
    SRHI_FORMAT_RG32_UINT,
    SRHI_FORMAT_RGBA32_FLOAT,
    SRHI_FORMAT_RGBA32_UINT,
    SRHI_FORMAT_RGB10A2_UNORM,
    SRHI_FORMAT_RG11B10_FLOAT,

    SRHI_FORMAT_D16_UNORM,
    SRHI_FORMAT_D32_FLOAT,
    SRHI_FORMAT_D32_FLOAT_S8_UINT,

    SRHI_FORMAT_BC1_UNORM,
    SRHI_FORMAT_BC1_UNORM_SRGB,
    SRHI_FORMAT_BC3_UNORM,
    SRHI_FORMAT_BC3_UNORM_SRGB,
    SRHI_FORMAT_BC4_UNORM,
    SRHI_FORMAT_BC5_UNORM,
    SRHI_FORMAT_BC6H_UFLOAT,
    SRHI_FORMAT_BC7_UNORM,
    SRHI_FORMAT_BC7_UNORM_SRGB,

    /// @brief ASTC is only available where srhiDeviceFeatures::textureCompressionASTC is set.
    SRHI_FORMAT_ASTC_4x4_UNORM,
    SRHI_FORMAT_ASTC_4x4_UNORM_SRGB,
    /// @brief HDR ASTC (Apple GPUs). Requires srhiDeviceFeatures::textureCompressionASTCHDR.
    SRHI_FORMAT_ASTC_4x4_FLOAT,

    SRHI_FORMAT_COUNT
} srhiFormat;

/// @brief Capability bits returned by srhiDeviceGetFormatSupport.
typedef enum srhiFormatSupportFlags {
    SRHI_FORMAT_SUPPORT_SAMPLE = 1 << 0,
    SRHI_FORMAT_SUPPORT_FILTER = 1 << 1,
    SRHI_FORMAT_SUPPORT_RENDER_TARGET = 1 << 2,
    SRHI_FORMAT_SUPPORT_BLEND = 1 << 3,
    SRHI_FORMAT_SUPPORT_DEPTH_STENCIL = 1 << 4,
    SRHI_FORMAT_SUPPORT_STORAGE = 1 << 5,
    SRHI_FORMAT_SUPPORT_MSAA = 1 << 6,
    SRHI_FORMAT_SUPPORT_RESOLVE = 1 << 7,
} srhiFormatSupportFlags;

/// @brief Buffer usages. Portable combinations (D3D11 cannot express the others, and rejects them):
///          - UNIFORM cannot be combined with any other GPU usage (COPY_* are fine).
///          - STORAGE / STORAGE_RW cannot be combined with VERTEX, INDEX or INDIRECT. To feed compute results
///            to draws (e.g. GPU-written indirect arguments), copy with srhiBlitEncoderCopyBufferToBuffer.
typedef enum srhiBufferUsageFlags {
    SRHI_BUFFER_USAGE_VERTEX = 1 << 0,
    SRHI_BUFFER_USAGE_INDEX = 1 << 1,
    SRHI_BUFFER_USAGE_UNIFORM = 1 << 2,
    /// @brief Read-only structured buffer (StructuredBuffer<T>).
    SRHI_BUFFER_USAGE_STORAGE = 1 << 3,
    /// @brief Read-write structured buffer (RWStructuredBuffer<T>), compute and fragment only.
    SRHI_BUFFER_USAGE_STORAGE_RW = 1 << 4,
    SRHI_BUFFER_USAGE_INDIRECT = 1 << 5,
    SRHI_BUFFER_USAGE_COPY_SRC = 1 << 6,
    SRHI_BUFFER_USAGE_COPY_DST = 1 << 7,
} srhiBufferUsageFlags;

typedef enum srhiTextureUsageFlags {
    SRHI_TEXTURE_USAGE_SAMPLED = 1 << 0,
    /// @brief Write-only storage texture (RWTexture*), compute and fragment only.
    SRHI_TEXTURE_USAGE_STORAGE = 1 << 1,
    SRHI_TEXTURE_USAGE_RENDER_TARGET = 1 << 2,
    SRHI_TEXTURE_USAGE_COPY_SRC = 1 << 3,
    SRHI_TEXTURE_USAGE_COPY_DST = 1 << 4,
} srhiTextureUsageFlags;

typedef enum srhiTextureType {
    SRHI_TEXTURE_TYPE_2D = 0,
    SRHI_TEXTURE_TYPE_2D_ARRAY,
    SRHI_TEXTURE_TYPE_CUBE,
    SRHI_TEXTURE_TYPE_CUBE_ARRAY,
    SRHI_TEXTURE_TYPE_3D,
} srhiTextureType;

typedef enum srhiFilter {
    SRHI_FILTER_NEAREST = 0,
    SRHI_FILTER_LINEAR,
} srhiFilter;

typedef enum srhiAddressMode {
    SRHI_ADDRESS_MODE_REPEAT = 0,
    SRHI_ADDRESS_MODE_MIRROR_REPEAT,
    SRHI_ADDRESS_MODE_CLAMP_TO_EDGE,
} srhiAddressMode;

typedef enum srhiCompareFunction {
    SRHI_COMPARE_FUNCTION_NEVER = 0,
    SRHI_COMPARE_FUNCTION_LESS,
    SRHI_COMPARE_FUNCTION_EQUAL,
    SRHI_COMPARE_FUNCTION_LESS_EQUAL,
    SRHI_COMPARE_FUNCTION_GREATER,
    SRHI_COMPARE_FUNCTION_NOT_EQUAL,
    SRHI_COMPARE_FUNCTION_GREATER_EQUAL,
    SRHI_COMPARE_FUNCTION_ALWAYS,
} srhiCompareFunction;

typedef enum srhiShaderStageFlags {
    SRHI_SHADER_STAGE_VERTEX = 1 << 0,
    SRHI_SHADER_STAGE_FRAGMENT = 1 << 1,
    SRHI_SHADER_STAGE_COMPUTE = 1 << 2,
    SRHI_SHADER_STAGE_ALL_GRAPHICS = SRHI_SHADER_STAGE_VERTEX | SRHI_SHADER_STAGE_FRAGMENT,
    SRHI_SHADER_STAGE_ALL = SRHI_SHADER_STAGE_ALL_GRAPHICS | SRHI_SHADER_STAGE_COMPUTE,
} srhiShaderStageFlags;

typedef enum srhiBindingType {
    SRHI_BINDING_TYPE_UNIFORM_BUFFER = 0,
    SRHI_BINDING_TYPE_STORAGE_BUFFER,
    SRHI_BINDING_TYPE_STORAGE_BUFFER_RW,
    SRHI_BINDING_TYPE_TEXTURE,
    SRHI_BINDING_TYPE_STORAGE_TEXTURE,
    SRHI_BINDING_TYPE_SAMPLER,
} srhiBindingType;

typedef enum srhiTextureSampleType {
    SRHI_TEXTURE_SAMPLE_TYPE_FLOAT = 0,
    /// @brief 32-bit float textures (R32F, RGBA32F) that are only Load()ed or point-sampled.
    SRHI_TEXTURE_SAMPLE_TYPE_UNFILTERABLE_FLOAT,
    /// @brief Depth textures sampled with a comparison sampler or read as depth.
    SRHI_TEXTURE_SAMPLE_TYPE_DEPTH,
    SRHI_TEXTURE_SAMPLE_TYPE_SINT,
    SRHI_TEXTURE_SAMPLE_TYPE_UINT,
} srhiTextureSampleType;

typedef enum srhiSamplerBindingType {
    SRHI_SAMPLER_BINDING_TYPE_FILTERING = 0,
    SRHI_SAMPLER_BINDING_TYPE_NON_FILTERING,
    SRHI_SAMPLER_BINDING_TYPE_COMPARISON,
} srhiSamplerBindingType;

typedef enum srhiVertexFormat {
    SRHI_VERTEX_FORMAT_FLOAT = 0,
    SRHI_VERTEX_FORMAT_FLOAT2,
    SRHI_VERTEX_FORMAT_FLOAT3,
    SRHI_VERTEX_FORMAT_FLOAT4,
    SRHI_VERTEX_FORMAT_HALF2,
    SRHI_VERTEX_FORMAT_HALF4,
    SRHI_VERTEX_FORMAT_UINT,
    SRHI_VERTEX_FORMAT_UINT2,
    SRHI_VERTEX_FORMAT_UINT4,
    SRHI_VERTEX_FORMAT_SINT,
    SRHI_VERTEX_FORMAT_UBYTE4,
    SRHI_VERTEX_FORMAT_UBYTE4_NORM,
    SRHI_VERTEX_FORMAT_SHORT2_NORM,
    SRHI_VERTEX_FORMAT_SHORT4_NORM,
} srhiVertexFormat;

typedef enum srhiVertexStepMode {
    SRHI_VERTEX_STEP_MODE_VERTEX = 0,
    SRHI_VERTEX_STEP_MODE_INSTANCE,
} srhiVertexStepMode;

typedef enum srhiPrimitiveTopology {
    SRHI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST = 0,
    SRHI_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
    SRHI_PRIMITIVE_TOPOLOGY_LINE_LIST,
    SRHI_PRIMITIVE_TOPOLOGY_LINE_STRIP,
    SRHI_PRIMITIVE_TOPOLOGY_POINT_LIST,
} srhiPrimitiveTopology;

typedef enum srhiIndexType {
    SRHI_INDEX_TYPE_UINT16 = 0,
    SRHI_INDEX_TYPE_UINT32,
} srhiIndexType;

typedef enum srhiCullMode {
    SRHI_CULL_MODE_NONE = 0,
    SRHI_CULL_MODE_FRONT,
    SRHI_CULL_MODE_BACK,
} srhiCullMode;

typedef enum srhiFrontFace {
    SRHI_FRONT_FACE_COUNTER_CLOCKWISE = 0,
    SRHI_FRONT_FACE_CLOCKWISE,
} srhiFrontFace;

typedef enum srhiFillMode {
    SRHI_FILL_MODE_SOLID = 0,
    /// @brief Requires srhiDeviceFeatures::fillModeWireframe (not available on WebGPU).
    SRHI_FILL_MODE_WIREFRAME,
} srhiFillMode;

typedef enum srhiBlendFactor {
    SRHI_BLEND_FACTOR_ZERO = 0,
    SRHI_BLEND_FACTOR_ONE,
    SRHI_BLEND_FACTOR_SRC_COLOR,
    SRHI_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
    SRHI_BLEND_FACTOR_SRC_ALPHA,
    SRHI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
    SRHI_BLEND_FACTOR_DST_COLOR,
    SRHI_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
    SRHI_BLEND_FACTOR_DST_ALPHA,
    SRHI_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
    SRHI_BLEND_FACTOR_SRC_ALPHA_SATURATED,
    SRHI_BLEND_FACTOR_BLEND_COLOR,
    SRHI_BLEND_FACTOR_ONE_MINUS_BLEND_COLOR,
} srhiBlendFactor;

typedef enum srhiBlendOp {
    SRHI_BLEND_OP_ADD = 0,
    SRHI_BLEND_OP_SUBTRACT,
    SRHI_BLEND_OP_REVERSE_SUBTRACT,
    SRHI_BLEND_OP_MIN,
    SRHI_BLEND_OP_MAX,
} srhiBlendOp;

typedef enum srhiColorWriteMask {
    SRHI_COLOR_WRITE_RED = 1 << 0,
    SRHI_COLOR_WRITE_GREEN = 1 << 1,
    SRHI_COLOR_WRITE_BLUE = 1 << 2,
    SRHI_COLOR_WRITE_ALPHA = 1 << 3,
    SRHI_COLOR_WRITE_ALL = 0xF,
} srhiColorWriteMask;

typedef enum srhiStencilOp {
    SRHI_STENCIL_OP_KEEP = 0,
    SRHI_STENCIL_OP_ZERO,
    SRHI_STENCIL_OP_REPLACE,
    SRHI_STENCIL_OP_INCREMENT_CLAMP,
    SRHI_STENCIL_OP_DECREMENT_CLAMP,
    SRHI_STENCIL_OP_INVERT,
    SRHI_STENCIL_OP_INCREMENT_WRAP,
    SRHI_STENCIL_OP_DECREMENT_WRAP,
} srhiStencilOp;

typedef enum srhiLoadAction {
    SRHI_LOAD_ACTION_DONT_CARE = 0,
    SRHI_LOAD_ACTION_LOAD,
    SRHI_LOAD_ACTION_CLEAR,
} srhiLoadAction;

typedef enum srhiStoreAction {
    SRHI_STORE_ACTION_STORE = 0,
    SRHI_STORE_ACTION_DONT_CARE,
    /// @brief Resolve the multisampled attachment into resolveTexture and discard the MSAA contents.
    SRHI_STORE_ACTION_RESOLVE,
    /// @brief Resolve into resolveTexture and keep the MSAA contents.
    SRHI_STORE_ACTION_STORE_AND_RESOLVE,
} srhiStoreAction;

typedef enum srhiColorSpace {
    /// @brief 8-bit sRGB. Swapchain format is BGRA8_UNORM_SRGB: write linear values, the hardware encodes.
    SRHI_COLOR_SPACE_SRGB = 0,
    /// @brief HDR10: RGB10A2_UNORM, Rec.2020 primaries, the shader writes PQ-encoded values.
    SRHI_COLOR_SPACE_HDR10_PQ,
    /// @brief Extended linear sRGB (scRGB): RGBA16_FLOAT, 1.0 = SDR white (80 nits on Windows).
    SRHI_COLOR_SPACE_EXTENDED_LINEAR_SRGB,
} srhiColorSpace;

// =====================================================================================================
// Device
// =====================================================================================================

typedef void (*srhiLogFunction)(srhiLogSeverity severity, const char* message, void* userData);

typedef struct srhiDeviceDesc {
    srhiBackend backend;
    /// @brief Enables the D3D11 debug layer, Metal API validation where available, WebGPU validation,
    ///        plus SimpleRHI's own argument validation (which is always on in debug builds).
    srhiBool debug;
    /// @brief Prefer the low-power adapter on multi-GPU systems.
    srhiBool lowPower;
    srhiLogFunction log;
    void* logUserData;
} srhiDeviceDesc;

typedef struct srhiDeviceFeatures {
    srhiBool timestampQueries;
    srhiBool textureCompressionBC;
    srhiBool textureCompressionASTC;
    srhiBool textureCompressionASTCHDR;
    srhiBool fillModeWireframe;
    srhiBool depthClamp;
    /// @brief Indirect draws honour firstInstance.
    srhiBool indirectFirstInstance;
    /// @brief Cube array textures can be created and sampled.
    srhiBool cubeArrays;
} srhiDeviceFeatures;

typedef struct srhiDeviceLimits {
    uint32_t maxTextureDimension2D;
    uint32_t maxTextureDimension3D;
    uint32_t maxTextureArrayLayers;
    uint32_t maxColorAttachments;
    uint32_t maxComputeWorkgroupSizeX;
    uint32_t maxComputeWorkgroupSizeY;
    uint32_t maxComputeWorkgroupSizeZ;
    uint32_t maxComputeInvocationsPerWorkgroup;
    uint32_t maxComputeWorkgroupsPerDimension;
    uint32_t maxStorageBuffersPerStage;
    uint32_t maxStorageTexturesPerStage;
    uint32_t maxSampledTexturesPerStage;
    uint32_t maxSamplersPerStage;
    uint32_t uniformBufferOffsetAlignment;
    uint32_t storageBufferOffsetAlignment;
    uint64_t maxUniformBufferBindingSize;
    uint64_t maxStorageBufferBindingSize;
} srhiDeviceLimits;

typedef struct srhiDeviceInfo {
    srhiBackend backend;
    /// @brief "D3D11", "Metal 3" or "WebGPU". Owned by the device.
    const char* backendName;
    /// @brief Adapter name. Owned by the device.
    const char* adapterName;
    srhiDeviceFeatures features;
    srhiDeviceLimits limits;
} srhiDeviceInfo;

/// @brief Returns NULL if the requested backend is not compiled in or no suitable adapter exists.
srhiDevice* srhiDeviceCreate(const srhiDeviceDesc* desc);
/// @brief Waits for the GPU to go idle, then destroys the device. Every object must be destroyed first.
void srhiDeviceDestroy(srhiDevice* device);
void srhiDeviceGetInfo(const srhiDevice* device, srhiDeviceInfo* outInfo);
/// @brief Bitmask of srhiFormatSupportFlags.
uint32_t srhiDeviceGetFormatSupport(const srhiDevice* device, srhiFormat format);
/// @brief Retires finished command buffers and fires completion and readback callbacks. Non-blocking.
void srhiDevicePoll(srhiDevice* device);
/// @brief Blocks until all committed work finished, then fires every pending callback.
void srhiDeviceWaitIdle(srhiDevice* device);

// =====================================================================================================
// Buffers
// =====================================================================================================

typedef struct srhiBufferDesc {
    uint64_t size;
    /// @brief srhiBufferUsageFlags.
    uint32_t usage;
    /// @brief Element stride for STORAGE / STORAGE_RW buffers (the size of T in StructuredBuffer<T>).
    uint32_t structureStride;
    const char* label;
} srhiBufferDesc;

srhiBuffer* srhiBufferCreate(srhiDevice* device, const srhiBufferDesc* desc);
void srhiBufferDestroy(srhiBuffer* buffer);
void srhiBufferGetDesc(const srhiBuffer* buffer, srhiBufferDesc* outDesc);

/// @brief Queue-ordered upload. Visible to every command buffer committed after this call.
///        offset and size must be multiples of 4.
void srhiBufferWrite(srhiDevice* device, srhiBuffer* buffer, uint64_t offset, const void* data, uint64_t size);

// =====================================================================================================
// Textures and views
// =====================================================================================================

typedef struct srhiTextureDesc {
    srhiTextureType type;
    srhiFormat format;
    uint32_t width;
    uint32_t height;
    /// @brief Depth for 3D textures, array layers for arrays (6 per cube for cube arrays: pass the cube
    ///        count times 6). Ignored (treated as 1, or 6 for cubes) otherwise. 0 means 1.
    uint32_t depthOrLayers;
    /// @brief 0 means 1. SRHI_ALL_MIPS means the full chain.
    uint32_t mipLevels;
    /// @brief 0 or 1 for no MSAA, otherwise 4. 2D only.
    uint32_t sampleCount;
    /// @brief srhiTextureUsageFlags.
    uint32_t usage;
    const char* label;
} srhiTextureDesc;

typedef struct srhiTextureViewDesc {
    /// @brief UNDEFINED keeps the texture's format. Otherwise must only differ in sRGB-ness.
    srhiFormat format;
    srhiTextureType type;
    uint32_t baseMip;
    /// @brief 0 or SRHI_ALL_MIPS means every remaining mip.
    uint32_t mipCount;
    uint32_t baseLayer;
    /// @brief 0 or SRHI_ALL_LAYERS means every remaining layer.
    uint32_t layerCount;
    const char* label;
} srhiTextureViewDesc;

/// @brief A texel region. depthOrLayerCount covers depth for 3D textures, layers otherwise.
typedef struct srhiTextureRegion {
    uint32_t x, y, z;
    uint32_t width, height, depth;
} srhiTextureRegion;

srhiTexture* srhiTextureCreate(srhiDevice* device, const srhiTextureDesc* desc);
void srhiTextureDestroy(srhiTexture* texture);
void srhiTextureGetDesc(const srhiTexture* texture, srhiTextureDesc* outDesc);

/// @brief Queue-ordered upload into one mip of one layer (or a sub-region of it when region is non-NULL).
///        bytesPerRow 0 means tightly packed. For block-compressed formats rows are rows of blocks.
///        For 3D textures region->z/depth select slices and data holds depth consecutive slices.
void srhiTextureWrite(srhiDevice* device, srhiTexture* texture, uint32_t mip, uint32_t layer,
                      const srhiTextureRegion* region, const void* data, uint32_t bytesPerRow);

srhiTextureView* srhiTextureViewCreate(srhiTexture* texture, const srhiTextureViewDesc* desc);
void srhiTextureViewDestroy(srhiTextureView* view);
/// @brief The view covering the whole texture with its own format. Owned by the texture.
srhiTextureView* srhiTextureGetDefaultView(srhiTexture* texture);

// =====================================================================================================
// Samplers
// =====================================================================================================

typedef struct srhiSamplerDesc {
    srhiFilter minFilter;
    srhiFilter magFilter;
    srhiFilter mipFilter;
    srhiAddressMode addressU;
    srhiAddressMode addressV;
    srhiAddressMode addressW;
    float lodMinClamp;
    /// @brief 0 means unclamped.
    float lodMaxClamp;
    /// @brief 0 or 1 disables anisotropy. Requires all filters to be LINEAR.
    uint32_t maxAnisotropy;
    /// @brief Non-zero makes this a comparison sampler using compareFunction.
    srhiBool compareEnable;
    srhiCompareFunction compareFunction;
    const char* label;
} srhiSamplerDesc;

srhiSampler* srhiSamplerCreate(srhiDevice* device, const srhiSamplerDesc* desc);
void srhiSamplerDestroy(srhiSampler* sampler);

// =====================================================================================================
// Shader modules
// =====================================================================================================

/// @brief Loads a shader package (.srhs) produced by srhiCompileShader / srhic. The data is copied.
srhiShaderModule* srhiShaderModuleCreate(srhiDevice* device, const void* data, size_t size, const char* label);
void srhiShaderModuleDestroy(srhiShaderModule* module);
srhiBool srhiShaderModuleHasEntryPoint(const srhiShaderModule* module, const char* entryPoint);

typedef struct srhiResourceSetLayoutEntry {
    uint32_t binding;
    srhiBindingType type;
    /// @brief srhiShaderStageFlags. 0 means every stage.
    uint32_t stages;
    /// @brief TEXTURE / STORAGE_TEXTURE: the view type the shader declares.
    srhiTextureType textureType;
    /// @brief TEXTURE only.
    srhiTextureSampleType sampleType;
    /// @brief TEXTURE only: Texture2DMS.
    srhiBool multisampled;
    /// @brief STORAGE_TEXTURE only.
    srhiFormat storageFormat;
    /// @brief SAMPLER only.
    srhiSamplerBindingType samplerType;
} srhiResourceSetLayoutEntry;

/// @brief Fills a layout description for one set from the module's reflection. Pass NULL outEntries
///        to get the count. Sample types default to FLOAT and sampler types to FILTERING (or COMPARISON
///        for SamplerComparisonState); patch the result for unfilterable or depth textures.
uint32_t srhiShaderModuleGetResourceSetLayout(const srhiShaderModule* module, uint32_t set,
                                              srhiResourceSetLayoutEntry* outEntries, uint32_t capacity);

// =====================================================================================================
// Resource sets
// =====================================================================================================

typedef struct srhiResourceSetLayoutDesc {
    const srhiResourceSetLayoutEntry* entries;
    uint32_t entryCount;
    const char* label;
} srhiResourceSetLayoutDesc;

typedef struct srhiResourceBinding {
    uint32_t binding;
    /// @brief UNIFORM_BUFFER / STORAGE_BUFFER(_RW).
    srhiBuffer* buffer;
    uint64_t offset;
    /// @brief 0 or SRHI_WHOLE_SIZE means the rest of the buffer.
    uint64_t size;
    /// @brief TEXTURE / STORAGE_TEXTURE. Storage textures must be a view of exactly one mip.
    srhiTextureView* textureView;
    /// @brief SAMPLER.
    srhiSampler* sampler;
} srhiResourceBinding;

typedef struct srhiResourceSetDesc {
    srhiResourceSetLayout* layout;
    const srhiResourceBinding* bindings;
    uint32_t bindingCount;
    const char* label;
} srhiResourceSetDesc;

srhiResourceSetLayout* srhiResourceSetLayoutCreate(srhiDevice* device, const srhiResourceSetLayoutDesc* desc);
void srhiResourceSetLayoutDestroy(srhiResourceSetLayout* layout);

/// @brief Immutable. Every layout entry must be bound. Referenced resources must outlive the set.
srhiResourceSet* srhiResourceSetCreate(srhiDevice* device, const srhiResourceSetDesc* desc);
void srhiResourceSetDestroy(srhiResourceSet* set);

// =====================================================================================================
// Pipelines
// =====================================================================================================

typedef struct srhiVertexAttribute {
    /// @brief Index of the field in the vertex shader's input struct (its WGSL @location).
    uint32_t location;
    srhiVertexFormat format;
    uint32_t offset;
    uint32_t bufferIndex;
} srhiVertexAttribute;

typedef struct srhiVertexBufferLayout {
    uint32_t stride;
    srhiVertexStepMode stepMode;
} srhiVertexBufferLayout;

typedef struct srhiVertexLayout {
    srhiVertexBufferLayout buffers[SRHI_MAX_VERTEX_BUFFERS];
    uint32_t bufferCount;
    srhiVertexAttribute attributes[SRHI_MAX_VERTEX_ATTRIBUTES];
    uint32_t attributeCount;
} srhiVertexLayout;

typedef struct srhiBlendState {
    srhiBool enable;
    srhiBlendFactor srcColor;
    srhiBlendFactor dstColor;
    srhiBlendOp colorOp;
    srhiBlendFactor srcAlpha;
    srhiBlendFactor dstAlpha;
    srhiBlendOp alphaOp;
} srhiBlendState;

typedef struct srhiColorTargetState {
    srhiFormat format;
    srhiBlendState blend;
    /// @brief srhiColorWriteMask. 0 is treated as SRHI_COLOR_WRITE_ALL; use writeDisabled to mask everything.
    uint32_t writeMask;
    srhiBool writeDisabled;
} srhiColorTargetState;

typedef struct srhiStencilFaceState {
    srhiCompareFunction compare;
    srhiStencilOp failOp;
    srhiStencilOp depthFailOp;
    srhiStencilOp passOp;
} srhiStencilFaceState;

typedef struct srhiDepthStencilState {
    srhiFormat format;
    srhiBool depthTestEnable;
    srhiBool depthWriteEnable;
    srhiCompareFunction depthCompare;
    srhiBool stencilEnable;
    srhiStencilFaceState stencilFront;
    srhiStencilFaceState stencilBack;
    uint8_t stencilReadMask;
    uint8_t stencilWriteMask;
} srhiDepthStencilState;

typedef struct srhiRasterState {
    srhiCullMode cullMode;
    srhiFrontFace frontFace;
    srhiFillMode fillMode;
    int32_t depthBias;
    float depthBiasSlopeScale;
    float depthBiasClamp;
    /// @brief Clamp depth instead of clipping. Requires srhiDeviceFeatures::depthClamp.
    srhiBool depthClamp;
} srhiRasterState;

typedef struct srhiRenderPipelineDesc {
    srhiShaderModule* module;
    const char* vertexEntry;
    /// @brief NULL for depth-only pipelines.
    const char* fragmentEntry;
    srhiVertexLayout vertexLayout;
    srhiPrimitiveTopology topology;
    /// @brief Required for strip topologies used with indexed draws (primitive restart value).
    srhiIndexType stripIndexType;
    srhiRasterState raster;
    srhiDepthStencilState depthStencil;
    srhiColorTargetState colorTargets[SRHI_MAX_COLOR_ATTACHMENTS];
    uint32_t colorTargetCount;
    /// @brief 0 or 1 for no MSAA, otherwise 4.
    uint32_t sampleCount;
    srhiBool alphaToCoverage;
    /// @brief Layouts for sets 0..2. NULL for unused sets.
    srhiResourceSetLayout* setLayouts[SRHI_MAX_RESOURCE_SETS];
    const char* label;
} srhiRenderPipelineDesc;

typedef struct srhiComputePipelineDesc {
    srhiShaderModule* module;
    const char* entry;
    srhiResourceSetLayout* setLayouts[SRHI_MAX_RESOURCE_SETS];
    const char* label;
} srhiComputePipelineDesc;

srhiRenderPipeline* srhiRenderPipelineCreate(srhiDevice* device, const srhiRenderPipelineDesc* desc);
void srhiRenderPipelineDestroy(srhiRenderPipeline* pipeline);
srhiComputePipeline* srhiComputePipelineCreate(srhiDevice* device, const srhiComputePipelineDesc* desc);
void srhiComputePipelineDestroy(srhiComputePipeline* pipeline);
/// @brief The [numthreads] declared by the compute entry point.
void srhiComputePipelineGetThreadgroupSize(const srhiComputePipeline* pipeline, uint32_t outSize[3]);

// =====================================================================================================
// Queries
// =====================================================================================================

/// @brief A set of GPU timestamps. Requires srhiDeviceFeatures::timestampQueries.
srhiQuerySet* srhiQuerySetCreate(srhiDevice* device, uint32_t count, const char* label);
void srhiQuerySetDestroy(srhiQuerySet* querySet);

/// @brief Reads resolved timestamps in nanoseconds (relative to an arbitrary origin). Returns false
///        while the command buffer that wrote them has not completed and been polled; outNanoseconds is
///        then untouched. Results stay readable until the queries are written again.
srhiBool srhiQuerySetGetResults(srhiQuerySet* querySet, uint32_t first, uint32_t count, uint64_t* outNanoseconds);

/// @brief Timestamps are written at pass boundaries only (the portable subset of Metal, WebGPU and D3D11).
typedef struct srhiPassTimestampWrites {
    srhiQuerySet* querySet;
    /// @brief SRHI_QUERY_UNUSED to skip.
    uint32_t beginIndex;
    uint32_t endIndex;
} srhiPassTimestampWrites;

// =====================================================================================================
// Command buffers and encoders
// =====================================================================================================

typedef void (*srhiCompletionCallback)(void* userData);
/// @brief data is only valid for the duration of the callback.
typedef void (*srhiReadbackCallback)(const void* data, uint64_t size, void* userData);
/// @brief Called with the backend's native command recording object; see srhiCommandBufferEncodeNative.
typedef void (*srhiNativeEncodeCallback)(void* nativeCommandObject, void* userData);

typedef struct srhiColorAttachment {
    /// @brief The view selects the mip and layer to render to (exactly one mip, one layer for 2D views).
    srhiTextureView* view;
    srhiLoadAction loadAction;
    srhiStoreAction storeAction;
    float clearColor[4];
    /// @brief For RESOLVE / STORE_AND_RESOLVE.
    srhiTextureView* resolveView;
} srhiColorAttachment;

typedef struct srhiDepthAttachment {
    srhiTextureView* view;
    srhiLoadAction depthLoadAction;
    srhiStoreAction depthStoreAction;
    float clearDepth;
    srhiLoadAction stencilLoadAction;
    srhiStoreAction stencilStoreAction;
    uint8_t clearStencil;
    /// @brief The depth/stencil attachment is only tested, never written; it can be sampled in the same pass.
    srhiBool readOnly;
} srhiDepthAttachment;

typedef struct srhiRenderPassDesc {
    srhiColorAttachment colorAttachments[SRHI_MAX_COLOR_ATTACHMENTS];
    uint32_t colorAttachmentCount;
    /// @brief view == NULL for no depth attachment.
    srhiDepthAttachment depthAttachment;
    srhiPassTimestampWrites timestampWrites;
    const char* label;
} srhiRenderPassDesc;

typedef struct srhiComputePassDesc {
    srhiPassTimestampWrites timestampWrites;
    const char* label;
} srhiComputePassDesc;

typedef struct srhiViewport {
    float x, y, width, height;
    float minDepth, maxDepth;
} srhiViewport;

typedef struct srhiScissorRect {
    uint32_t x, y, width, height;
} srhiScissorRect;

/// @brief Matches D3D11, Metal and WebGPU: { vertexCount, instanceCount, firstVertex, firstInstance }.
///        Note: firstInstance offsets per-instance vertex attributes everywhere, but SV_InstanceID only includes it
///        on WebGPU; do not rely on SV_InstanceID + firstInstance in portable shaders.
typedef struct srhiDrawIndirectArgs {
    uint32_t vertexCount;
    uint32_t instanceCount;
    uint32_t firstVertex;
    uint32_t firstInstance;
} srhiDrawIndirectArgs;

typedef struct srhiDrawIndexedIndirectArgs {
    uint32_t indexCount;
    uint32_t instanceCount;
    uint32_t firstIndex;
    int32_t baseVertex;
    uint32_t firstInstance;
} srhiDrawIndexedIndirectArgs;

typedef struct srhiDispatchIndirectArgs {
    uint32_t x, y, z;
} srhiDispatchIndirectArgs;

srhiCommandBuffer* srhiCommandBufferBegin(srhiDevice* device, const char* label);
/// @brief Submits the command buffer and releases it. The callback (optional) fires from
///        srhiDevicePoll / srhiDeviceWaitIdle once the GPU finished executing it.
void srhiCommandBufferCommit(srhiCommandBuffer* cmd, srhiCompletionCallback onCompleted, void* userData);

void srhiCommandBufferPushDebugGroup(srhiCommandBuffer* cmd, const char* name);
void srhiCommandBufferPopDebugGroup(srhiCommandBuffer* cmd);

/// @brief Copies the range to the CPU once this command buffer completed. size SRHI_WHOLE_SIZE means to the end.
///        Must be recorded outside of any encoder.
void srhiCommandBufferReadBuffer(srhiCommandBuffer* cmd, srhiBuffer* buffer, uint64_t offset, uint64_t size,
                                 srhiReadbackCallback callback, void* userData);
/// @brief Copies one mip of one layer (one slice for 3D: layer is the z slice) to the CPU, tightly packed
///        (bytesPerRow = width * bytes per texel, rows of blocks for compressed formats).
void srhiCommandBufferReadTexture(srhiCommandBuffer* cmd, srhiTexture* texture, uint32_t mip, uint32_t layer,
                                  srhiReadbackCallback callback, void* userData);

/// @brief Escape hatch for native libraries (MetalFX, FSR, DLSS...). Must be recorded outside of any encoder.
///        The callback runs in command order with:
///          - D3D11:  ID3D11DeviceContext*  (at commit time, when the recorded stream is replayed)
///          - Metal:  id<MTLCommandBuffer>  (immediately, with no encoder open)
///          - WebGPU: WGPUCommandEncoder    (immediately, with no pass open)
///        State set by the callback is not tracked: restore anything SimpleRHI relies on.
void srhiCommandBufferEncodeNative(srhiCommandBuffer* cmd, srhiNativeEncodeCallback callback, void* userData);

/// @brief Presents the swapchain's acquired texture once this command buffer's work completes. Record it last,
///        outside any encoder. The acquired texture must not be used after this call.
void srhiCommandBufferPresent(srhiCommandBuffer* cmd, srhiSwapchain* swapchain);

// --- Render encoder ----------------------------------------------------------------------------------

srhiRenderEncoder* srhiRenderEncoderBegin(srhiCommandBuffer* cmd, const srhiRenderPassDesc* desc);
void srhiRenderEncoderEnd(srhiRenderEncoder* encoder);

void srhiRenderEncoderSetPipeline(srhiRenderEncoder* encoder, srhiRenderPipeline* pipeline);
void srhiRenderEncoderSetVertexBuffer(srhiRenderEncoder* encoder, uint32_t index, srhiBuffer* buffer, uint64_t offset);
void srhiRenderEncoderSetIndexBuffer(srhiRenderEncoder* encoder, srhiBuffer* buffer, srhiIndexType type, uint64_t offset);
void srhiRenderEncoderSetResourceSet(srhiRenderEncoder* encoder, uint32_t index, srhiResourceSet* set);
/// @brief Pushes size bytes (<= SRHI_MAX_UNIFORM_SIZE, multiple of 4) to uniform slot `slot` of set 3.
///        The data is copied immediately and stays bound until replaced or the encoder ends.
void srhiRenderEncoderSetUniforms(srhiRenderEncoder* encoder, uint32_t slot, const void* data, uint32_t size);
/// @brief Defaults to the full first attachment with depth 0..1.
void srhiRenderEncoderSetViewport(srhiRenderEncoder* encoder, const srhiViewport* viewport);
void srhiRenderEncoderSetScissor(srhiRenderEncoder* encoder, const srhiScissorRect* rect);
void srhiRenderEncoderSetBlendColor(srhiRenderEncoder* encoder, const float color[4]);
void srhiRenderEncoderSetStencilReference(srhiRenderEncoder* encoder, uint32_t reference);

void srhiRenderEncoderDraw(srhiRenderEncoder* encoder, uint32_t vertexCount, uint32_t instanceCount,
                           uint32_t firstVertex, uint32_t firstInstance);
void srhiRenderEncoderDrawIndexed(srhiRenderEncoder* encoder, uint32_t indexCount, uint32_t instanceCount,
                                  uint32_t firstIndex, int32_t baseVertex, uint32_t firstInstance);
/// @brief Reads one srhiDrawIndirectArgs at offset (4-byte aligned).
void srhiRenderEncoderDrawIndirect(srhiRenderEncoder* encoder, srhiBuffer* buffer, uint64_t offset);
/// @brief Reads one srhiDrawIndexedIndirectArgs at offset (4-byte aligned).
void srhiRenderEncoderDrawIndexedIndirect(srhiRenderEncoder* encoder, srhiBuffer* buffer, uint64_t offset);

void srhiRenderEncoderPushDebugGroup(srhiRenderEncoder* encoder, const char* name);
void srhiRenderEncoderPopDebugGroup(srhiRenderEncoder* encoder);
void srhiRenderEncoderInsertDebugMarker(srhiRenderEncoder* encoder, const char* name);

// --- Compute encoder ---------------------------------------------------------------------------------

/// @brief desc may be NULL.
srhiComputeEncoder* srhiComputeEncoderBegin(srhiCommandBuffer* cmd, const srhiComputePassDesc* desc);
void srhiComputeEncoderEnd(srhiComputeEncoder* encoder);

void srhiComputeEncoderSetPipeline(srhiComputeEncoder* encoder, srhiComputePipeline* pipeline);
void srhiComputeEncoderSetResourceSet(srhiComputeEncoder* encoder, uint32_t index, srhiResourceSet* set);
void srhiComputeEncoderSetUniforms(srhiComputeEncoder* encoder, uint32_t slot, const void* data, uint32_t size);
/// @brief Counts are threadgroups, not threads.
void srhiComputeEncoderDispatch(srhiComputeEncoder* encoder, uint32_t x, uint32_t y, uint32_t z);
void srhiComputeEncoderDispatchIndirect(srhiComputeEncoder* encoder, srhiBuffer* buffer, uint64_t offset);

void srhiComputeEncoderPushDebugGroup(srhiComputeEncoder* encoder, const char* name);
void srhiComputeEncoderPopDebugGroup(srhiComputeEncoder* encoder);
void srhiComputeEncoderInsertDebugMarker(srhiComputeEncoder* encoder, const char* name);

// --- Blit encoder ------------------------------------------------------------------------------------

typedef struct srhiTextureCopyLocation {
    srhiTexture* texture;
    uint32_t mip;
    uint32_t layer;
    uint32_t x, y, z;
} srhiTextureCopyLocation;

typedef struct srhiBufferCopyLocation {
    srhiBuffer* buffer;
    uint64_t offset;
    /// @brief Must be a multiple of SRHI_COPY_BYTES_PER_ROW_ALIGNMENT.
    uint32_t bytesPerRow;
    /// @brief Rows per 2D image, for 3D copies. 0 means the copy height.
    uint32_t rowsPerImage;
} srhiBufferCopyLocation;

/// @brief Note: D3D11 has no GPU buffer <-> texture copies; SimpleRHI emulates them through a CPU staging round trip
///        at submission, which stalls. Prefer srhiTextureWrite / srhiCommandBufferReadTexture for uploads/readbacks.
srhiBlitEncoder* srhiBlitEncoderBegin(srhiCommandBuffer* cmd, const char* label);
void srhiBlitEncoderEnd(srhiBlitEncoder* encoder);

void srhiBlitEncoderCopyBufferToBuffer(srhiBlitEncoder* encoder, srhiBuffer* src, uint64_t srcOffset,
                                       srhiBuffer* dst, uint64_t dstOffset, uint64_t size);
void srhiBlitEncoderCopyBufferToTexture(srhiBlitEncoder* encoder, const srhiBufferCopyLocation* src,
                                        const srhiTextureCopyLocation* dst, uint32_t width, uint32_t height,
                                        uint32_t depth);
void srhiBlitEncoderCopyTextureToBuffer(srhiBlitEncoder* encoder, const srhiTextureCopyLocation* src,
                                        const srhiBufferCopyLocation* dst, uint32_t width, uint32_t height,
                                        uint32_t depth);
/// @brief Formats must match (or differ only in sRGB-ness), as must sample counts.
void srhiBlitEncoderCopyTextureToTexture(srhiBlitEncoder* encoder, const srhiTextureCopyLocation* src,
                                         const srhiTextureCopyLocation* dst, uint32_t width, uint32_t height,
                                         uint32_t depth);
/// @brief Box-filters mip 0 into the remaining mips of every layer. The texture needs RENDER_TARGET usage
///        and a filterable, renderable color format.
void srhiBlitEncoderGenerateMipmaps(srhiBlitEncoder* encoder, srhiTexture* texture);

void srhiBlitEncoderPushDebugGroup(srhiBlitEncoder* encoder, const char* name);
void srhiBlitEncoderPopDebugGroup(srhiBlitEncoder* encoder);

// =====================================================================================================
// Swapchain
// =====================================================================================================

typedef struct srhiSwapchainDesc {
    /// @brief D3D11: HWND. Metal: CAMetalLayer*. WebGPU native: CAMetalLayer* (macOS) or HWND (Windows).
    ///        WebGPU on the web: NULL (use canvasSelector).
    void* nativeWindow;
    /// @brief WebGPU on the web: CSS selector of the canvas, e.g. "#canvas".
    const char* canvasSelector;
    uint32_t width;
    uint32_t height;
    srhiColorSpace colorSpace;
    srhiBool vsync;
    const char* label;
} srhiSwapchainDesc;

/// @brief Luminances are in nits; 0 means the platform does not report the value.
typedef struct srhiDisplayInfo {
    /// @brief The display can show HDR content (HDR enabled in the OS / EDR available).
    srhiBool hdrSupported;
    float minLuminance;
    float maxLuminance;
    float maxFullFrameLuminance;
    /// @brief Brightness of SDR white in nits, where the OS reports it.
    float sdrWhiteLevel;
    /// @brief Metal EDR: current maximum value of 1.0-relative linear color. 1.0 elsewhere.
    float edrHeadroom;
} srhiDisplayInfo;

srhiSwapchain* srhiSwapchainCreate(srhiDevice* device, const srhiSwapchainDesc* desc);
void srhiSwapchainDestroy(srhiSwapchain* swapchain);
/// @brief Call when the window size changed. Waits for the GPU on D3D11.
void srhiSwapchainResize(srhiSwapchain* swapchain, uint32_t width, uint32_t height);
/// @brief Changes color space and/or vsync. Returns false (and keeps the old settings) if unsupported.
srhiBool srhiSwapchainConfigure(srhiSwapchain* swapchain, srhiColorSpace colorSpace, srhiBool vsync);
srhiFormat srhiSwapchainGetFormat(const srhiSwapchain* swapchain);
void srhiSwapchainGetDisplayInfo(srhiSwapchain* swapchain, srhiDisplayInfo* outInfo);
/// @brief Returns the texture to render into this frame, or NULL if the window is minimized / occluded.
///        The texture is valid until the command buffer presenting it is committed.
srhiTexture* srhiSwapchainAcquire(srhiSwapchain* swapchain);

// =====================================================================================================
// Native access
// =====================================================================================================
// Every accessor returns a borrowed reference: SimpleRHI keeps ownership, do not release it.
//   Device:   ID3D11Device*         | id<MTLDevice>        | WGPUDevice
//   Queue:    ID3D11DeviceContext*  | id<MTLCommandQueue>  | WGPUQueue
//   Buffer:   ID3D11Buffer*         | id<MTLBuffer>        | WGPUBuffer
//   Texture:  ID3D11Resource*       | id<MTLTexture>       | WGPUTexture
// (Metal objects are returned as bridged void*: `(__bridge id<MTLDevice>)ptr`.)

void* srhiDeviceGetNative(srhiDevice* device);
void* srhiDeviceGetNativeQueue(srhiDevice* device);
void* srhiBufferGetNative(srhiBuffer* buffer);
void* srhiTextureGetNative(srhiTexture* texture);

// =====================================================================================================
// Shader compiler (link SimpleRHICompiler)
// =====================================================================================================

typedef enum srhiShaderTargetFlags {
    /// @brief D3D11 bytecode. On hosts without fxc (non-Windows) HLSL source is stored instead and
    ///        compiled at load time with D3DCompile.
    SRHI_SHADER_TARGET_D3D11 = 1 << 0,
    /// @brief metallib. Falls back to MSL source (compiled at load time) when no Metal toolchain exists.
    SRHI_SHADER_TARGET_METAL = 1 << 1,
    SRHI_SHADER_TARGET_WGSL = 1 << 2,
    SRHI_SHADER_TARGET_ALL = 0x7,
} srhiShaderTargetFlags;

typedef struct srhiShaderCompileDesc {
    /// @brief Path of the .slang file. Also used as the module name and for diagnostics.
    const char* sourcePath;
    /// @brief Optional source text. When NULL, sourcePath is read from disk.
    const char* source;
    /// @brief Entry points to compile. NULL compiles every function marked [shader("...")].
    const char* const* entryPoints;
    uint32_t entryPointCount;
    /// @brief "NAME" or "NAME=VALUE".
    const char* const* defines;
    uint32_t defineCount;
    const char* const* includeDirs;
    uint32_t includeDirCount;
    /// @brief srhiShaderTargetFlags. 0 means all.
    uint32_t targets;
    srhiBool debugInfo;
    /// @brief Skip optimizations (faster compiles, easier debugging).
    srhiBool noOptimize;
} srhiShaderCompileDesc;

typedef struct srhiShaderPackage {
    /// @brief The .srhs bytes, for srhiShaderModuleCreate or writing to disk.
    void* data;
    size_t size;
    /// @brief Warnings and errors, or NULL.
    char* diagnostics;
} srhiShaderPackage;

/// @brief Compiles a Slang file to a shader package. On failure returns false and fills diagnostics.
srhiBool srhiCompileShader(const srhiShaderCompileDesc* desc, srhiShaderPackage* outPackage);
void srhiShaderPackageFree(srhiShaderPackage* package);

#ifdef __cplusplus
}
#endif
