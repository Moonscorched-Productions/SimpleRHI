/**
 * @ Author: Amélie Heinrich
 * @ Create Time: 2026-11-13 00:00:00
 * @ Copyright: Copyright (c) 2026 Amélie Heinrich. All rights reserved.
 */

// D3D11 backend (feature level 11_0, D3D11.1 runtime).
//
// - Command buffers RECORD into a software command stream and REPLAY on the immediate context at commit.
//   srhiBufferWrite / srhiTextureWrite go straight to the immediate context, so they are visible to every
//   command buffer committed afterwards -- including one being recorded at the time of the write.
// - Binding state (pipeline, resource sets, uniforms, vertex buffers) is applied lazily at replay time
//   right before each draw/dispatch, using the flattened b/t/s/u slots recorded in the pipeline layout.
// - Transient uniforms are copied into the command buffer at record time, then uploaded at replay into a
//   dynamic constant-buffer ring (MAP_WRITE_NO_OVERWRITE, wrapping with MAP_WRITE_DISCARD) and bound with
//   *SetConstantBuffers1 offsets.
// - Every pass starts and ends with SRV/UAV/RTV slots cleared, so a texture can move between "render
//   target" and "shader resource" across passes without debug-layer hazard warnings.
// - Completion tracking: one D3D11_QUERY_EVENT per committed command buffer.
//
// Known D3D11 restrictions (logged as errors at creation):
// - A buffer with UNIFORM usage cannot have any other GPU usage (D3D11 constant buffers are exclusive).
// - A buffer with STORAGE / STORAGE_RW usage cannot also be VERTEX / INDEX / INDIRECT (structured buffers
//   cannot carry those bind flags). Copy between buffers with the blit encoder instead.
// - Buffer <-> texture copies inside a command buffer go through a CPU round trip (D3D11 cannot copy
//   between buffers and textures on the GPU), stalling the replay until the GPU caught up.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "srhi_internal.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <unordered_map>

using Microsoft::WRL::ComPtr;
using namespace srhi;

namespace
{
    constexpr uint32_t kUniformRingSize = 4u << 20;
    constexpr uint32_t kMaxSrvSlots = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; // 128
    constexpr uint32_t kMaxUavSlots = D3D11_PS_CS_UAV_REGISTER_COUNT;              // 8
    constexpr uint32_t kMaxSamplerSlots = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;   // 16

    std::wstring ToWide(const char* s)
    {
        if (!s || !*s)
            return std::wstring();
        int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
        std::wstring w(size_t(n > 0 ? n - 1 : 0), L'\0');
        if (n > 1)
            MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
        return w;
    }

    std::string ToUtf8(const wchar_t* s)
    {
        if (!s || !*s)
            return std::string();
        int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
        std::string out(size_t(n > 0 ? n - 1 : 0), '\0');
        if (n > 1)
            WideCharToMultiByte(CP_UTF8, 0, s, -1, &out[0], n, nullptr, nullptr);
        return out;
    }

    void SetName(ID3D11DeviceChild* obj, const char* name)
    {
        if (obj && name && *name)
            obj->SetPrivateData(WKPDID_D3DDebugObjectName, UINT(strlen(name)), name);
    }

    // =================================================================================================
    // Format tables
    // =================================================================================================

    DXGI_FORMAT ToDxgi(srhiFormat f)
    {
        switch (f) {
        case SRHI_FORMAT_R8_UNORM: return DXGI_FORMAT_R8_UNORM;
        case SRHI_FORMAT_RG8_UNORM: return DXGI_FORMAT_R8G8_UNORM;
        case SRHI_FORMAT_RGBA8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case SRHI_FORMAT_RGBA8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case SRHI_FORMAT_RGBA8_SNORM: return DXGI_FORMAT_R8G8B8A8_SNORM;
        case SRHI_FORMAT_RGBA8_UINT: return DXGI_FORMAT_R8G8B8A8_UINT;
        case SRHI_FORMAT_BGRA8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case SRHI_FORMAT_BGRA8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        case SRHI_FORMAT_R16_FLOAT: return DXGI_FORMAT_R16_FLOAT;
        case SRHI_FORMAT_RG16_FLOAT: return DXGI_FORMAT_R16G16_FLOAT;
        case SRHI_FORMAT_RGBA16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case SRHI_FORMAT_R16_UINT: return DXGI_FORMAT_R16_UINT;
        case SRHI_FORMAT_R32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
        case SRHI_FORMAT_R32_UINT: return DXGI_FORMAT_R32_UINT;
        case SRHI_FORMAT_R32_SINT: return DXGI_FORMAT_R32_SINT;
        case SRHI_FORMAT_RG32_FLOAT: return DXGI_FORMAT_R32G32_FLOAT;
        case SRHI_FORMAT_RG32_UINT: return DXGI_FORMAT_R32G32_UINT;
        case SRHI_FORMAT_RGBA32_FLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case SRHI_FORMAT_RGBA32_UINT: return DXGI_FORMAT_R32G32B32A32_UINT;
        case SRHI_FORMAT_RGB10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case SRHI_FORMAT_RG11B10_FLOAT: return DXGI_FORMAT_R11G11B10_FLOAT;
        case SRHI_FORMAT_D16_UNORM: return DXGI_FORMAT_D16_UNORM;
        case SRHI_FORMAT_D32_FLOAT: return DXGI_FORMAT_D32_FLOAT;
        case SRHI_FORMAT_D32_FLOAT_S8_UINT: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        case SRHI_FORMAT_BC1_UNORM: return DXGI_FORMAT_BC1_UNORM;
        case SRHI_FORMAT_BC1_UNORM_SRGB: return DXGI_FORMAT_BC1_UNORM_SRGB;
        case SRHI_FORMAT_BC3_UNORM: return DXGI_FORMAT_BC3_UNORM;
        case SRHI_FORMAT_BC3_UNORM_SRGB: return DXGI_FORMAT_BC3_UNORM_SRGB;
        case SRHI_FORMAT_BC4_UNORM: return DXGI_FORMAT_BC4_UNORM;
        case SRHI_FORMAT_BC5_UNORM: return DXGI_FORMAT_BC5_UNORM;
        case SRHI_FORMAT_BC6H_UFLOAT: return DXGI_FORMAT_BC6H_UF16;
        case SRHI_FORMAT_BC7_UNORM: return DXGI_FORMAT_BC7_UNORM;
        case SRHI_FORMAT_BC7_UNORM_SRGB: return DXGI_FORMAT_BC7_UNORM_SRGB;
        default: return DXGI_FORMAT_UNKNOWN; // ASTC is not available on D3D11
        }
    }

    /// @brief The format the resource itself is created with: typeless where views need to reinterpret it.
    DXGI_FORMAT ResourceFormat(srhiFormat f, uint32_t usage)
    {
        switch (f) {
        case SRHI_FORMAT_D16_UNORM: return (usage & SRHI_TEXTURE_USAGE_SAMPLED) ? DXGI_FORMAT_R16_TYPELESS : DXGI_FORMAT_D16_UNORM;
        case SRHI_FORMAT_D32_FLOAT: return (usage & SRHI_TEXTURE_USAGE_SAMPLED) ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_D32_FLOAT;
        case SRHI_FORMAT_D32_FLOAT_S8_UINT:
            return (usage & SRHI_TEXTURE_USAGE_SAMPLED) ? DXGI_FORMAT_R32G8X24_TYPELESS : DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        case SRHI_FORMAT_RGBA8_UNORM:
        case SRHI_FORMAT_RGBA8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
        case SRHI_FORMAT_BGRA8_UNORM:
        case SRHI_FORMAT_BGRA8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
        case SRHI_FORMAT_BC1_UNORM:
        case SRHI_FORMAT_BC1_UNORM_SRGB: return DXGI_FORMAT_BC1_TYPELESS;
        case SRHI_FORMAT_BC3_UNORM:
        case SRHI_FORMAT_BC3_UNORM_SRGB: return DXGI_FORMAT_BC3_TYPELESS;
        case SRHI_FORMAT_BC7_UNORM:
        case SRHI_FORMAT_BC7_UNORM_SRGB: return DXGI_FORMAT_BC7_TYPELESS;
        default: return ToDxgi(f);
        }
    }

    /// @brief SRV format for a view (depth formats read as their color equivalent).
    DXGI_FORMAT SrvFormat(srhiFormat f)
    {
        switch (f) {
        case SRHI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
        case SRHI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
        case SRHI_FORMAT_D32_FLOAT_S8_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        default: return ToDxgi(f);
        }
    }

    DXGI_FORMAT ToDxgi(srhiVertexFormat f)
    {
        switch (f) {
        case SRHI_VERTEX_FORMAT_FLOAT: return DXGI_FORMAT_R32_FLOAT;
        case SRHI_VERTEX_FORMAT_FLOAT2: return DXGI_FORMAT_R32G32_FLOAT;
        case SRHI_VERTEX_FORMAT_FLOAT3: return DXGI_FORMAT_R32G32B32_FLOAT;
        case SRHI_VERTEX_FORMAT_FLOAT4: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case SRHI_VERTEX_FORMAT_HALF2: return DXGI_FORMAT_R16G16_FLOAT;
        case SRHI_VERTEX_FORMAT_HALF4: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case SRHI_VERTEX_FORMAT_UINT: return DXGI_FORMAT_R32_UINT;
        case SRHI_VERTEX_FORMAT_UINT2: return DXGI_FORMAT_R32G32_UINT;
        case SRHI_VERTEX_FORMAT_UINT4: return DXGI_FORMAT_R32G32B32A32_UINT;
        case SRHI_VERTEX_FORMAT_SINT: return DXGI_FORMAT_R32_SINT;
        case SRHI_VERTEX_FORMAT_UBYTE4: return DXGI_FORMAT_R8G8B8A8_UINT;
        case SRHI_VERTEX_FORMAT_UBYTE4_NORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case SRHI_VERTEX_FORMAT_SHORT2_NORM: return DXGI_FORMAT_R16G16_SNORM;
        case SRHI_VERTEX_FORMAT_SHORT4_NORM: return DXGI_FORMAT_R16G16B16A16_SNORM;
        }
        return DXGI_FORMAT_UNKNOWN;
    }

    D3D11_COMPARISON_FUNC ToD3D(srhiCompareFunction f)
    {
        switch (f) {
        case SRHI_COMPARE_FUNCTION_NEVER: return D3D11_COMPARISON_NEVER;
        case SRHI_COMPARE_FUNCTION_LESS: return D3D11_COMPARISON_LESS;
        case SRHI_COMPARE_FUNCTION_EQUAL: return D3D11_COMPARISON_EQUAL;
        case SRHI_COMPARE_FUNCTION_LESS_EQUAL: return D3D11_COMPARISON_LESS_EQUAL;
        case SRHI_COMPARE_FUNCTION_GREATER: return D3D11_COMPARISON_GREATER;
        case SRHI_COMPARE_FUNCTION_NOT_EQUAL: return D3D11_COMPARISON_NOT_EQUAL;
        case SRHI_COMPARE_FUNCTION_GREATER_EQUAL: return D3D11_COMPARISON_GREATER_EQUAL;
        case SRHI_COMPARE_FUNCTION_ALWAYS: return D3D11_COMPARISON_ALWAYS;
        }
        return D3D11_COMPARISON_ALWAYS;
    }

    D3D11_TEXTURE_ADDRESS_MODE ToD3D(srhiAddressMode m)
    {
        switch (m) {
        case SRHI_ADDRESS_MODE_REPEAT: return D3D11_TEXTURE_ADDRESS_WRAP;
        case SRHI_ADDRESS_MODE_MIRROR_REPEAT: return D3D11_TEXTURE_ADDRESS_MIRROR;
        case SRHI_ADDRESS_MODE_CLAMP_TO_EDGE: return D3D11_TEXTURE_ADDRESS_CLAMP;
        }
        return D3D11_TEXTURE_ADDRESS_WRAP;
    }

    /// @brief Blend factor; `alpha` maps color factors to their alpha equivalents (D3D11 rejects
    ///        *_COLOR factors in the alpha blend equation).
    D3D11_BLEND ToD3D(srhiBlendFactor f, bool alpha)
    {
        switch (f) {
        case SRHI_BLEND_FACTOR_ZERO: return D3D11_BLEND_ZERO;
        case SRHI_BLEND_FACTOR_ONE: return D3D11_BLEND_ONE;
        case SRHI_BLEND_FACTOR_SRC_COLOR: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
        case SRHI_BLEND_FACTOR_ONE_MINUS_SRC_COLOR: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
        case SRHI_BLEND_FACTOR_SRC_ALPHA: return D3D11_BLEND_SRC_ALPHA;
        case SRHI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA: return D3D11_BLEND_INV_SRC_ALPHA;
        case SRHI_BLEND_FACTOR_DST_COLOR: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
        case SRHI_BLEND_FACTOR_ONE_MINUS_DST_COLOR: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
        case SRHI_BLEND_FACTOR_DST_ALPHA: return D3D11_BLEND_DEST_ALPHA;
        case SRHI_BLEND_FACTOR_ONE_MINUS_DST_ALPHA: return D3D11_BLEND_INV_DEST_ALPHA;
        case SRHI_BLEND_FACTOR_SRC_ALPHA_SATURATED: return D3D11_BLEND_SRC_ALPHA_SAT;
        case SRHI_BLEND_FACTOR_BLEND_COLOR: return D3D11_BLEND_BLEND_FACTOR;
        case SRHI_BLEND_FACTOR_ONE_MINUS_BLEND_COLOR: return D3D11_BLEND_INV_BLEND_FACTOR;
        }
        return D3D11_BLEND_ONE;
    }

    D3D11_BLEND_OP ToD3D(srhiBlendOp op)
    {
        switch (op) {
        case SRHI_BLEND_OP_ADD: return D3D11_BLEND_OP_ADD;
        case SRHI_BLEND_OP_SUBTRACT: return D3D11_BLEND_OP_SUBTRACT;
        case SRHI_BLEND_OP_REVERSE_SUBTRACT: return D3D11_BLEND_OP_REV_SUBTRACT;
        case SRHI_BLEND_OP_MIN: return D3D11_BLEND_OP_MIN;
        case SRHI_BLEND_OP_MAX: return D3D11_BLEND_OP_MAX;
        }
        return D3D11_BLEND_OP_ADD;
    }

    D3D11_STENCIL_OP ToD3D(srhiStencilOp op)
    {
        switch (op) {
        case SRHI_STENCIL_OP_KEEP: return D3D11_STENCIL_OP_KEEP;
        case SRHI_STENCIL_OP_ZERO: return D3D11_STENCIL_OP_ZERO;
        case SRHI_STENCIL_OP_REPLACE: return D3D11_STENCIL_OP_REPLACE;
        case SRHI_STENCIL_OP_INCREMENT_CLAMP: return D3D11_STENCIL_OP_INCR_SAT;
        case SRHI_STENCIL_OP_DECREMENT_CLAMP: return D3D11_STENCIL_OP_DECR_SAT;
        case SRHI_STENCIL_OP_INVERT: return D3D11_STENCIL_OP_INVERT;
        case SRHI_STENCIL_OP_INCREMENT_WRAP: return D3D11_STENCIL_OP_INCR;
        case SRHI_STENCIL_OP_DECREMENT_WRAP: return D3D11_STENCIL_OP_DECR;
        }
        return D3D11_STENCIL_OP_KEEP;
    }

    D3D11_PRIMITIVE_TOPOLOGY ToD3D(srhiPrimitiveTopology t)
    {
        switch (t) {
        case SRHI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        case SRHI_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        case SRHI_PRIMITIVE_TOPOLOGY_LINE_LIST: return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
        case SRHI_PRIMITIVE_TOPOLOGY_LINE_STRIP: return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
        case SRHI_PRIMITIVE_TOPOLOGY_POINT_LIST: return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
        }
        return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }

    // =================================================================================================
    // Objects
    // =================================================================================================

    struct D3DDevice;
    struct Replay;
    using Command = std::function<void(Replay&)>;

    struct D3DBuffer : srhiBuffer
    {
        ComPtr<ID3D11Buffer> buffer;
        bool isConstant = false;
        /// @brief CPU copy of constant buffers: D3D11.1 partial constant buffer updates must be 16-byte
        ///        aligned, so unaligned writes are widened from this shadow.
        std::vector<uint8_t> shadow;
        std::map<std::pair<uint64_t, uint64_t>, ComPtr<ID3D11ShaderResourceView>> srvs;
        std::map<std::pair<uint64_t, uint64_t>, ComPtr<ID3D11UnorderedAccessView>> uavs;

        void* GetNative() override { return buffer.Get(); }
        ID3D11ShaderResourceView* GetSRV(D3DDevice* dev, uint64_t offset, uint64_t size);
        ID3D11UnorderedAccessView* GetUAV(D3DDevice* dev, uint64_t offset, uint64_t size);
    };

    struct D3DTexture : srhiTexture
    {
        ComPtr<ID3D11Resource> resource;
        DXGI_FORMAT resourceFormat = DXGI_FORMAT_UNKNOWN;
        void* GetNative() override { return resource.Get(); }
        UINT Subresource(uint32_t mip, uint32_t layer) const { return D3D11CalcSubresource(mip, layer, desc.mipLevels); }
    };

    struct D3DTextureView : srhiTextureView
    {
        ComPtr<ID3D11ShaderResourceView> srv;
        ComPtr<ID3D11UnorderedAccessView> uav;
        ComPtr<ID3D11RenderTargetView> rtv;
        ComPtr<ID3D11DepthStencilView> dsv;
        ComPtr<ID3D11DepthStencilView> dsvReadOnly;

        ID3D11ShaderResourceView* GetSRV();
        ID3D11UnorderedAccessView* GetUAV();
        ID3D11RenderTargetView* GetRTV();
        ID3D11DepthStencilView* GetDSV(bool readOnly);
        void ReleaseViews()
        {
            srv.Reset();
            uav.Reset();
            rtv.Reset();
            dsv.Reset();
            dsvReadOnly.Reset();
        }
    };

    struct D3DSampler : srhiSampler
    {
        ComPtr<ID3D11SamplerState> sampler;
    };

    struct D3DShaderModule : srhiShaderModule
    {
        std::unordered_map<std::string, ComPtr<ID3DBlob>> bytecode;
    };

    struct D3DResourceSetLayout : srhiResourceSetLayout
    {
    };

    /// @brief A binding resolved to native objects at set creation, so draws never create views.
    struct NativeBinding
    {
        ID3D11Buffer* cb = nullptr;
        UINT firstConstant = 0;
        UINT numConstants = 0;
        ID3D11ShaderResourceView* srv = nullptr;
        ID3D11UnorderedAccessView* uav = nullptr;
        ID3D11SamplerState* sampler = nullptr;
    };

    struct D3DResourceSet : srhiResourceSet
    {
        std::vector<NativeBinding> native;
        /// @brief Keeps the referenced views alive.
        std::vector<ComPtr<IUnknown>> refs;
    };

    struct D3DRenderPipeline : srhiRenderPipeline
    {
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps;
        ComPtr<ID3D11InputLayout> inputLayout;
        ComPtr<ID3D11RasterizerState> rasterizer;
        ComPtr<ID3D11BlendState> blend;
        ComPtr<ID3D11DepthStencilState> depthStencil;
        D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        UINT strides[SRHI_MAX_VERTEX_BUFFERS] = {};
        uint32_t vertexBufferCount = 0;
    };

    struct D3DComputePipeline : srhiComputePipeline
    {
        ComPtr<ID3D11ComputeShader> cs;
    };

    struct D3DQuerySet : srhiQuerySet
    {
        std::vector<ComPtr<ID3D11Query>> queries;
        std::vector<uint64_t> results;
        std::vector<uint8_t> valid;

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

    struct Readback
    {
        ComPtr<ID3D11Resource> staging;
        bool isTexture = false;
        uint64_t size = 0;      ///< Buffers: byte count.
        uint32_t rowBytes = 0;  ///< Textures: tight row size.
        uint32_t rowCount = 0;  ///< Textures: rows (of blocks).
        srhiReadbackCallback callback = nullptr;
        void* userData = nullptr;
    };

    struct QueryWrite
    {
        D3DQuerySet* querySet;
        uint32_t index;
    };

    struct InFlight
    {
        ComPtr<ID3D11Query> event;
        ComPtr<ID3D11Query> disjoint;
        srhiCompletionCallback callback = nullptr;
        void* userData = nullptr;
        std::vector<Readback> readbacks;
        std::vector<QueryWrite> queries;
    };

    struct D3DSwapchain;
    struct D3DCommandBuffer;

    // --- Replay state --------------------------------------------------------------------------------

    struct ResolveOp
    {
        D3DTextureView* src;
        D3DTextureView* dst;
    };

    struct Replay
    {
        D3DDevice* dev = nullptr;
        ID3D11DeviceContext1* ctx = nullptr;
        InFlight* flight = nullptr;
        const std::vector<uint8_t>* arena = nullptr;

        // Render pass
        uint32_t numRTV = 0;
        uint32_t width = 0, height = 0;
        std::vector<ResolveOp> resolves;
        std::vector<ID3D11View*> discardAtEnd;
        D3DQuerySet* endQuerySet = nullptr;
        uint32_t endQueryIndex = SRHI_QUERY_UNUSED;

        // Bound state
        D3DRenderPipeline* rp = nullptr;
        D3DComputePipeline* cp = nullptr;
        srhiResourceSet* sets[SRHI_MAX_RESOURCE_SETS] = {};
        bool setDirty[SRHI_MAX_RESOURCE_SETS] = {};
        size_t uniformOffset[SRHI_MAX_UNIFORM_SLOTS] = {};
        uint32_t uniformSize[SRHI_MAX_UNIFORM_SLOTS] = {};
        bool uniformDirty[SRHI_MAX_UNIFORM_SLOTS] = {};
        ID3D11Buffer* vbs[SRHI_MAX_VERTEX_BUFFERS] = {};
        UINT vbOffsets[SRHI_MAX_VERTEX_BUFFERS] = {};
        bool vbDirty = false;
        ID3D11UnorderedAccessView* psUavs[kMaxUavSlots] = {};
        bool psUavDirty = false;
        float blendColor[4] = {0, 0, 0, 0};
        UINT stencilRef = 0;

        void ResetBindings()
        {
            rp = nullptr;
            cp = nullptr;
            for (uint32_t i = 0; i < SRHI_MAX_RESOURCE_SETS; ++i) {
                sets[i] = nullptr;
                setDirty[i] = false;
            }
            for (uint32_t i = 0; i < SRHI_MAX_UNIFORM_SLOTS; ++i) {
                uniformSize[i] = 0;
                uniformDirty[i] = false;
            }
            for (uint32_t i = 0; i < SRHI_MAX_VERTEX_BUFFERS; ++i) {
                vbs[i] = nullptr;
                vbOffsets[i] = 0;
            }
            vbDirty = false;
            for (uint32_t i = 0; i < kMaxUavSlots; ++i)
                psUavs[i] = nullptr;
            psUavDirty = false;
            blendColor[0] = blendColor[1] = blendColor[2] = blendColor[3] = 0.0f;
            stencilRef = 0;
        }

        void MarkAllDirty()
        {
            for (uint32_t i = 0; i < SRHI_MAX_RESOURCE_SETS; ++i)
                setDirty[i] = sets[i] != nullptr;
            for (uint32_t i = 0; i < SRHI_MAX_UNIFORM_SLOTS; ++i)
                uniformDirty[i] = uniformSize[i] != 0;
            vbDirty = true;
        }

        bool Flush(bool compute);
        void UnbindAll();
    };

    // --- Encoders (recorders) ------------------------------------------------------------------------

    struct D3DRenderEncoder : srhiRenderEncoder
    {
        D3DCommandBuffer* owner = nullptr;

        void End() override;
        void SetPipeline(srhiRenderPipeline* p) override;
        void SetVertexBuffer(uint32_t index, srhiBuffer* buffer, uint64_t offset) override;
        void SetIndexBuffer(srhiBuffer* buffer, srhiIndexType type, uint64_t offset) override;
        void SetResourceSet(uint32_t index, srhiResourceSet* set) override;
        void SetUniforms(uint32_t slot, const void* data, uint32_t size) override;
        void SetViewport(const srhiViewport& viewport) override;
        void SetScissor(const srhiScissorRect& rect) override;
        void SetBlendColor(const float color[4]) override;
        void SetStencilReference(uint32_t reference) override;
        void Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) override;
        void DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t baseVertex,
                         uint32_t firstInstance) override;
        void DrawIndirect(srhiBuffer* buffer, uint64_t offset) override;
        void DrawIndexedIndirect(srhiBuffer* buffer, uint64_t offset) override;
        void PushDebugGroup(const char* name) override;
        void PopDebugGroup() override;
        void InsertDebugMarker(const char* name) override;
    };

    struct D3DComputeEncoder : srhiComputeEncoder
    {
        D3DCommandBuffer* owner = nullptr;
        D3DQuerySet* endQuerySet = nullptr;
        uint32_t endQueryIndex = SRHI_QUERY_UNUSED;

        void End() override;
        void SetPipeline(srhiComputePipeline* p) override;
        void SetResourceSet(uint32_t index, srhiResourceSet* set) override;
        void SetUniforms(uint32_t slot, const void* data, uint32_t size) override;
        void Dispatch(uint32_t x, uint32_t y, uint32_t z) override;
        void DispatchIndirect(srhiBuffer* buffer, uint64_t offset) override;
        void PushDebugGroup(const char* name) override;
        void PopDebugGroup() override;
        void InsertDebugMarker(const char* name) override;
    };

    struct D3DBlitEncoder : srhiBlitEncoder
    {
        D3DCommandBuffer* owner = nullptr;

        void End() override;
        void CopyBufferToBuffer(srhiBuffer* src, uint64_t srcOffset, srhiBuffer* dst, uint64_t dstOffset, uint64_t size) override;
        void CopyBufferToTexture(const srhiBufferCopyLocation& src, const srhiTextureCopyLocation& dst, uint32_t width,
                                 uint32_t height, uint32_t depth) override;
        void CopyTextureToBuffer(const srhiTextureCopyLocation& src, const srhiBufferCopyLocation& dst, uint32_t width,
                                 uint32_t height, uint32_t depth) override;
        void CopyTextureToTexture(const srhiTextureCopyLocation& src, const srhiTextureCopyLocation& dst, uint32_t width,
                                  uint32_t height, uint32_t depth) override;
        void GenerateMipmaps(srhiTexture* texture) override;
        void PushDebugGroup(const char* name) override;
        void PopDebugGroup() override;
    };

    struct D3DCommandBuffer : srhiCommandBuffer
    {
        D3DDevice* dev = nullptr;
        std::vector<Command> commands;
        /// @brief Uniform payloads copied at record time; referenced by offset from the commands.
        std::vector<uint8_t> arena;
        bool usesTimestamps = false;
        D3DRenderEncoder render;
        D3DComputeEncoder compute;
        D3DBlitEncoder blit;

        void Record(Command&& c) { commands.push_back(std::move(c)); }
        void MarkQuery(D3DQuerySet* qs, uint32_t index)
        {
            if (!qs || index == SRHI_QUERY_UNUSED)
                return;
            qs->valid[index] = 0;
            usesTimestamps = true;
        }

        srhiRenderEncoder* BeginRender(const srhiRenderPassDesc& desc) override;
        srhiComputeEncoder* BeginCompute(const srhiComputePassDesc& desc) override;
        srhiBlitEncoder* BeginBlit(const char* label) override;
        void Commit(srhiCompletionCallback callback, void* userData) override;
        void PushDebugGroup(const char* name) override;
        void PopDebugGroup() override;
        void ReadBuffer(srhiBuffer* buffer, uint64_t offset, uint64_t size, srhiReadbackCallback callback, void* userData) override;
        void ReadTexture(srhiTexture* texture, uint32_t mip, uint32_t layer, srhiReadbackCallback callback, void* userData) override;
        void EncodeNative(srhiNativeEncodeCallback callback, void* userData) override;
        void Present(srhiSwapchain* swapchain) override;
    };

    // --- Swapchain -----------------------------------------------------------------------------------

    struct D3DSwapchain : srhiSwapchain
    {
        D3DDevice* dev = nullptr;
        HWND hwnd = nullptr;
        ComPtr<IDXGISwapChain1> swapchain;
        D3DTexture* image = nullptr;
        D3DTextureView* imageView = nullptr;
        DXGI_FORMAT bufferFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
        UINT flags = 0;
        bool tearing = false;
        bool vsync = true;

        ~D3DSwapchain() override
        {
            delete imageView;
            delete image;
        }
        bool ApplyColorSpace(srhiColorSpace cs);
        void AcquireBuffer();
        void ReleaseBuffer()
        {
            if (imageView)
                imageView->ReleaseViews();
            if (image)
                image->resource.Reset();
        }
        void PresentNow();
        void Resize(uint32_t width, uint32_t height) override;
        bool Configure(srhiColorSpace colorSpace, bool vsync) override;
        void GetDisplayInfo(srhiDisplayInfo& out) override;
        srhiTexture* Acquire() override;
    };

    // --- Device --------------------------------------------------------------------------------------

    struct D3DDevice : srhiDevice
    {
        ComPtr<IDXGIFactory2> factory;
        ComPtr<IDXGIAdapter1> adapter;
        ComPtr<ID3D11Device1> device;
        ComPtr<ID3D11DeviceContext1> ctx;
        ComPtr<ID3DUserDefinedAnnotation> annotation;
        ComPtr<ID3D11InfoQueue> infoQueue;
        bool tearingSupported = false;

        ComPtr<ID3D11Buffer> uniformRing;
        /// @brief Starts "full" so the first upload maps with DISCARD (NO_OVERWRITE before any DISCARD is invalid).
        uint32_t uniformRingOffset = kUniformRingSize;

        std::deque<InFlight> inFlight;

        ~D3DDevice() override
        {
            WaitIdle();
            if (ctx) {
                ctx->ClearState();
                ctx->Flush();
            }
        }

        void DrainMessages()
        {
            if (!infoQueue)
                return;
            UINT64 count = infoQueue->GetNumStoredMessages();
            for (UINT64 i = 0; i < count; ++i) {
                SIZE_T length = 0;
                if (FAILED(infoQueue->GetMessage(i, nullptr, &length)) || length == 0)
                    continue;
                std::vector<uint8_t> storage(length);
                D3D11_MESSAGE* msg = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
                if (FAILED(infoQueue->GetMessage(i, msg, &length)))
                    continue;
                switch (msg->Severity) {
                case D3D11_MESSAGE_SEVERITY_CORRUPTION:
                case D3D11_MESSAGE_SEVERITY_ERROR: SRHI_ERROR(this, "D3D11: %.*s", int(msg->DescriptionByteLength), msg->pDescription); break;
                case D3D11_MESSAGE_SEVERITY_WARNING: SRHI_WARN(this, "D3D11: %.*s", int(msg->DescriptionByteLength), msg->pDescription); break;
                default: break;
                }
            }
            infoQueue->ClearStoredMessages();
        }

        /// @brief Uploads `size` bytes to the uniform ring; returns the 16-byte constant offset.
        UINT UploadUniforms(const void* data, uint32_t size, UINT& outNumConstants)
        {
            uint32_t aligned = uint32_t(AlignUp(size, 256));
            D3D11_MAP mapType = D3D11_MAP_WRITE_NO_OVERWRITE;
            if (uniformRingOffset + aligned > kUniformRingSize) {
                uniformRingOffset = 0;
                mapType = D3D11_MAP_WRITE_DISCARD;
            }
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            if (FAILED(ctx->Map(uniformRing.Get(), 0, mapType, 0, &mapped))) {
                SRHI_ERROR(this, "D3D11: failed to map the uniform ring");
                outNumConstants = 0;
                return 0;
            }
            memcpy(static_cast<uint8_t*>(mapped.pData) + uniformRingOffset, data, size);
            ctx->Unmap(uniformRing.Get(), 0);
            UINT first = uniformRingOffset / 16;
            outNumConstants = aligned / 16;
            uniformRingOffset += aligned;
            return first;
        }

        /// @brief Immediate buffer write; constant buffers go through their CPU shadow.
        void WriteBufferNow(D3DBuffer* b, uint64_t offset, const void* data, uint64_t size)
        {
            if (b->isConstant) {
                memcpy(b->shadow.data() + offset, data, size);
                uint64_t begin = offset / 16 * 16;
                uint64_t end = std::min<uint64_t>(AlignUp(offset + size, 16), b->shadow.size());
                D3D11_BOX box = {UINT(begin), 0, 0, UINT(end), 1, 1};
                ctx->UpdateSubresource1(b->buffer.Get(), 0, &box, b->shadow.data() + begin, 0, 0, 0);
            } else {
                D3D11_BOX box = {UINT(offset), 0, 0, UINT(offset + size), 1, 1};
                ctx->UpdateSubresource1(b->buffer.Get(), 0, &box, data, 0, 0, 0);
            }
        }

        ComPtr<ID3D11Buffer> CreateStagingBuffer(uint64_t size, bool read)
        {
            D3D11_BUFFER_DESC bd = {};
            bd.ByteWidth = UINT(AlignUp(size, 16));
            bd.Usage = D3D11_USAGE_STAGING;
            bd.CPUAccessFlags = read ? D3D11_CPU_ACCESS_READ : D3D11_CPU_ACCESS_WRITE;
            ComPtr<ID3D11Buffer> b;
            device->CreateBuffer(&bd, nullptr, &b);
            return b;
        }

        /// @brief A staging texture matching one region of `t` (width x height x depthOrLayers).
        ComPtr<ID3D11Resource> CreateStagingTexture(D3DTexture* t, uint32_t width, uint32_t height, uint32_t depthOrLayers)
        {
            ComPtr<ID3D11Resource> res;
            if (t->desc.type == SRHI_TEXTURE_TYPE_3D) {
                D3D11_TEXTURE3D_DESC td = {};
                td.Width = width;
                td.Height = height;
                td.Depth = depthOrLayers;
                td.MipLevels = 1;
                td.Format = t->resourceFormat;
                td.Usage = D3D11_USAGE_STAGING;
                td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                ComPtr<ID3D11Texture3D> tex;
                if (SUCCEEDED(device->CreateTexture3D(&td, nullptr, &tex)))
                    res = tex;
            } else {
                D3D11_TEXTURE2D_DESC td = {};
                td.Width = width;
                td.Height = height;
                td.MipLevels = 1;
                td.ArraySize = depthOrLayers;
                td.Format = t->resourceFormat;
                td.SampleDesc.Count = 1;
                td.Usage = D3D11_USAGE_STAGING;
                td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                ComPtr<ID3D11Texture2D> tex;
                if (SUCCEEDED(device->CreateTexture2D(&td, nullptr, &tex)))
                    res = tex;
            }
            if (!res)
                SRHI_ERROR(this, "D3D11: failed to create a staging texture");
            return res;
        }

        void Retire(InFlight& f)
        {
            for (Readback& r : f.readbacks) {
                if (!r.staging)
                    continue;
                D3D11_MAPPED_SUBRESOURCE mapped = {};
                if (FAILED(ctx->Map(r.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                    SRHI_ERROR(this, "D3D11: failed to map a readback staging resource");
                    continue;
                }
                if (!r.isTexture || mapped.RowPitch == r.rowBytes) {
                    r.callback(mapped.pData, r.isTexture ? uint64_t(r.rowBytes) * r.rowCount : r.size, r.userData);
                } else {
                    std::vector<uint8_t> tight(size_t(r.rowBytes) * r.rowCount);
                    for (uint32_t row = 0; row < r.rowCount; ++row)
                        memcpy(tight.data() + size_t(row) * r.rowBytes, static_cast<const uint8_t*>(mapped.pData) + size_t(row) * mapped.RowPitch,
                               r.rowBytes);
                    r.callback(tight.data(), tight.size(), r.userData);
                }
                ctx->Unmap(r.staging.Get(), 0);
            }

            if (f.disjoint && !f.queries.empty()) {
                D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
                HRESULT hr;
                while ((hr = ctx->GetData(f.disjoint.Get(), &dj, sizeof(dj), 0)) == S_FALSE)
                    YieldProcessor();
                if (hr == S_OK && !dj.Disjoint && dj.Frequency) {
                    for (QueryWrite& q : f.queries) {
                        UINT64 ticks = 0;
                        HRESULT qhr;
                        while ((qhr = ctx->GetData(q.querySet->queries[q.index].Get(), &ticks, sizeof(ticks), 0)) == S_FALSE)
                            YieldProcessor();
                        if (qhr == S_OK) {
                            // ns = ticks * 1e9 / frequency, without overflowing for large tick counts.
                            uint64_t seconds = ticks / dj.Frequency, remainder = ticks % dj.Frequency;
                            q.querySet->results[q.index] = seconds * 1000000000ull + remainder * 1000000000ull / dj.Frequency;
                            q.querySet->valid[q.index] = 1;
                        }
                    }
                }
            }
            if (f.callback)
                f.callback(f.userData);
        }

        // --- srhiDevice ---

        uint32_t GetFormatSupport(srhiFormat format) override;

        void Poll() override
        {
            DrainMessages();
            while (!inFlight.empty()) {
                BOOL done = FALSE;
                if (ctx->GetData(inFlight.front().event.Get(), &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK || !done)
                    break;
                InFlight f = std::move(inFlight.front());
                inFlight.pop_front();
                Retire(f);
            }
            DrainMessages();
        }

        void WaitIdle() override
        {
            if (!ctx)
                return;
            ctx->Flush();
            if (!inFlight.empty()) {
                BOOL done = FALSE;
                HRESULT hr;
                while ((hr = ctx->GetData(inFlight.back().event.Get(), &done, sizeof(done), 0)) == S_FALSE || (hr == S_OK && !done))
                    YieldProcessor();
            }
            Poll();
        }

        void* GetNative() override { return device.Get(); }
        void* GetNativeQueue() override { return ctx.Get(); }

        srhiBuffer* CreateBuffer(const srhiBufferDesc& d) override
        {
            const char* label = d.label ? d.label : "";
            const uint32_t storage = SRHI_BUFFER_USAGE_STORAGE | SRHI_BUFFER_USAGE_STORAGE_RW;
            const uint32_t gpuUsages = SRHI_BUFFER_USAGE_VERTEX | SRHI_BUFFER_USAGE_INDEX | SRHI_BUFFER_USAGE_UNIFORM |
                                       storage | SRHI_BUFFER_USAGE_INDIRECT;
            if ((d.usage & SRHI_BUFFER_USAGE_UNIFORM) && (d.usage & gpuUsages & ~SRHI_BUFFER_USAGE_UNIFORM)) {
                SRHI_ERROR(this, "srhiBufferCreate(%s): D3D11 constant buffers cannot have other GPU usages (use a separate buffer)", label);
                return nullptr;
            }
            if ((d.usage & storage) && (d.usage & (SRHI_BUFFER_USAGE_VERTEX | SRHI_BUFFER_USAGE_INDEX | SRHI_BUFFER_USAGE_INDIRECT))) {
                SRHI_ERROR(this,
                           "srhiBufferCreate(%s): D3D11 structured (storage) buffers cannot also be vertex/index/indirect buffers "
                           "(copy into a separate buffer with the blit encoder)",
                           label);
                return nullptr;
            }

            D3D11_BUFFER_DESC bd = {};
            bd.ByteWidth = UINT(d.size);
            bd.Usage = D3D11_USAGE_DEFAULT;
            if (d.usage & SRHI_BUFFER_USAGE_VERTEX)
                bd.BindFlags |= D3D11_BIND_VERTEX_BUFFER;
            if (d.usage & SRHI_BUFFER_USAGE_INDEX)
                bd.BindFlags |= D3D11_BIND_INDEX_BUFFER;
            if (d.usage & SRHI_BUFFER_USAGE_UNIFORM) {
                // Padded to 256 so every 256-aligned binding range rounded up to 16 constants stays in bounds.
                bd.BindFlags |= D3D11_BIND_CONSTANT_BUFFER;
                bd.ByteWidth = UINT(AlignUp(d.size, 256));
            }
            if (d.usage & storage) {
                bd.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
                if (d.usage & SRHI_BUFFER_USAGE_STORAGE_RW)
                    bd.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
                bd.MiscFlags |= D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
                bd.StructureByteStride = d.structureStride;
            }
            if (d.usage & SRHI_BUFFER_USAGE_INDIRECT)
                bd.MiscFlags |= D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;

            // Zero-initialize to match Metal/WebGPU (fresh buffers read as zero there).
            std::vector<uint8_t> zeros(bd.ByteWidth, 0);
            D3D11_SUBRESOURCE_DATA init = {zeros.data(), 0, 0};

            D3DBuffer* b = new D3DBuffer;
            if (FAILED(device->CreateBuffer(&bd, &init, &b->buffer))) {
                SRHI_ERROR(this, "srhiBufferCreate(%s): CreateBuffer failed", label);
                delete b;
                return nullptr;
            }
            SetName(b->buffer.Get(), d.label);
            b->isConstant = (d.usage & SRHI_BUFFER_USAGE_UNIFORM) != 0;
            if (b->isConstant)
                b->shadow.assign(bd.ByteWidth, 0);
            return b;
        }

        void WriteBuffer(srhiBuffer* buffer, uint64_t offset, const void* data, uint64_t size) override
        {
            WriteBufferNow(static_cast<D3DBuffer*>(buffer), offset, data, size);
        }

        srhiTexture* CreateTexture(const srhiTextureDesc& d) override
        {
            const FormatInfo& fi = GetFormatInfo(d.format);
            UINT bind = 0;
            if (d.usage & SRHI_TEXTURE_USAGE_SAMPLED)
                bind |= D3D11_BIND_SHADER_RESOURCE;
            if (d.usage & SRHI_TEXTURE_USAGE_STORAGE)
                bind |= D3D11_BIND_UNORDERED_ACCESS;
            if (d.usage & SRHI_TEXTURE_USAGE_RENDER_TARGET)
                bind |= fi.depth ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_RENDER_TARGET;
            UINT misc = 0;
            if (d.type == SRHI_TEXTURE_TYPE_CUBE || d.type == SRHI_TEXTURE_TYPE_CUBE_ARRAY)
                misc |= D3D11_RESOURCE_MISC_TEXTURECUBE;
            // srhiBlitEncoderGenerateMipmaps only requires RENDER_TARGET usage; GenerateMips needs an SRV too.
            if (!fi.depth && d.mipLevels > 1 && d.sampleCount == 1 && (bind & D3D11_BIND_RENDER_TARGET)) {
                bind |= D3D11_BIND_SHADER_RESOURCE;
                misc |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
            }

            D3DTexture* t = new D3DTexture;
            t->resourceFormat = ResourceFormat(d.format, d.usage);
            HRESULT hr;
            if (d.type == SRHI_TEXTURE_TYPE_3D) {
                D3D11_TEXTURE3D_DESC td = {};
                td.Width = d.width;
                td.Height = d.height;
                td.Depth = d.depthOrLayers;
                td.MipLevels = d.mipLevels;
                td.Format = t->resourceFormat;
                td.Usage = D3D11_USAGE_DEFAULT;
                td.BindFlags = bind;
                td.MiscFlags = misc;
                ComPtr<ID3D11Texture3D> tex;
                hr = device->CreateTexture3D(&td, nullptr, &tex);
                t->resource = tex;
            } else {
                D3D11_TEXTURE2D_DESC td = {};
                td.Width = d.width;
                td.Height = d.height;
                td.MipLevels = d.mipLevels;
                td.ArraySize = d.depthOrLayers;
                td.Format = t->resourceFormat;
                td.SampleDesc.Count = d.sampleCount;
                td.SampleDesc.Quality = 0;
                td.Usage = D3D11_USAGE_DEFAULT;
                td.BindFlags = bind;
                td.MiscFlags = misc;
                ComPtr<ID3D11Texture2D> tex;
                hr = device->CreateTexture2D(&td, nullptr, &tex);
                t->resource = tex;
            }
            if (FAILED(hr)) {
                SRHI_ERROR(this, "srhiTextureCreate(%s): creation failed (hr 0x%08x)", d.label ? d.label : "", unsigned(hr));
                delete t;
                return nullptr;
            }
            SetName(t->resource.Get(), d.label);
            return t;
        }

        void WriteTexture(srhiTexture* texture, uint32_t mip, uint32_t layer, const srhiTextureRegion& r, const void* data,
                          uint32_t bytesPerRow, uint32_t rowsPerImage) override
        {
            D3DTexture* t = static_cast<D3DTexture*>(texture);
            bool is3D = t->desc.type == SRHI_TEXTURE_TYPE_3D;
            D3D11_BOX box = {r.x, r.y, is3D ? r.z : 0, r.x + r.width, r.y + r.height, is3D ? r.z + r.depth : 1};
            ctx->UpdateSubresource(t->resource.Get(), t->Subresource(mip, is3D ? 0 : layer), &box, data, bytesPerRow,
                                   bytesPerRow * rowsPerImage);
        }

        srhiTextureView* CreateTextureView(srhiTexture*, const srhiTextureViewDesc&) override
        {
            // Native views are created lazily per use (SRV/UAV/RTV/DSV) and cached on the view.
            return new D3DTextureView;
        }

        srhiSampler* CreateSampler(const srhiSamplerDesc& d) override
        {
            D3D11_SAMPLER_DESC sd = {};
            bool comparison = d.compareEnable != 0;
            if (d.maxAnisotropy > 1)
                sd.Filter = comparison ? D3D11_FILTER_COMPARISON_ANISOTROPIC : D3D11_FILTER_ANISOTROPIC;
            else {
                auto type = [](srhiFilter f) { return f == SRHI_FILTER_LINEAR ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT; };
                sd.Filter = D3D11_ENCODE_BASIC_FILTER(type(d.minFilter), type(d.magFilter), type(d.mipFilter), comparison ? 1 : 0);
            }
            sd.AddressU = ToD3D(d.addressU);
            sd.AddressV = ToD3D(d.addressV);
            sd.AddressW = ToD3D(d.addressW);
            sd.MipLODBias = 0.0f;
            sd.MaxAnisotropy = d.maxAnisotropy;
            sd.ComparisonFunc = comparison ? ToD3D(d.compareFunction) : D3D11_COMPARISON_NEVER;
            sd.MinLOD = d.lodMinClamp;
            sd.MaxLOD = d.lodMaxClamp;
            D3DSampler* s = new D3DSampler;
            if (FAILED(device->CreateSamplerState(&sd, &s->sampler))) {
                SRHI_ERROR(this, "srhiSamplerCreate(%s): CreateSamplerState failed", d.label ? d.label : "");
                delete s;
                return nullptr;
            }
            SetName(s->sampler.Get(), d.label);
            return s;
        }

        srhiShaderModule* CreateShaderModule(Package&& package, const char* label) override
        {
            for (const PkgEntry& e : package.entries)
                if (!e.FindBlob(BlobKind::DXBC) && !e.FindBlob(BlobKind::HLSL)) {
                    SRHI_ERROR(this, "srhiShaderModuleCreate(%s): entry point '%s' has no D3D11 code (compile with the d3d11 target)",
                               label ? label : "", e.name.c_str());
                    return nullptr;
                }
            D3DShaderModule* m = new D3DShaderModule;
            m->package = std::move(package);
            return m;
        }

        ID3DBlob* GetBytecode(srhiShaderModule* module, const PkgEntry* entry)
        {
            D3DShaderModule* m = static_cast<D3DShaderModule*>(module);
            auto it = m->bytecode.find(entry->name);
            if (it != m->bytecode.end())
                return it->second.Get();

            ComPtr<ID3DBlob> blob;
            if (const PkgBlob* dxbc = entry->FindBlob(BlobKind::DXBC)) {
                if (SUCCEEDED(D3DCreateBlob(dxbc->data.size(), &blob)))
                    memcpy(blob->GetBufferPointer(), dxbc->data.data(), dxbc->data.size());
            } else {
                const PkgBlob* hlsl = entry->FindBlob(BlobKind::HLSL);
                const char* profile = entry->stage == SRHI_SHADER_STAGE_VERTEX   ? "vs_5_0"
                                    : entry->stage == SRHI_SHADER_STAGE_FRAGMENT ? "ps_5_0"
                                                                                 : "cs_5_0";
                UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
                flags |= desc.debug ? (D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION) : D3DCOMPILE_OPTIMIZATION_LEVEL3;
                ComPtr<ID3DBlob> errors;
                std::string name = m->label + ":" + entry->name;
                HRESULT hr = D3DCompile(hlsl->data.data(), hlsl->data.size(), name.c_str(), nullptr, nullptr,
                                        hlsl->functionName.c_str(), profile, flags, 0, &blob, &errors);
                if (FAILED(hr)) {
                    SRHI_ERROR(this, "shader module '%s': D3DCompile of '%s' failed: %s", m->label.c_str(), entry->name.c_str(),
                               errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown error");
                    blob.Reset();
                }
            }
            m->bytecode[entry->name] = blob;
            return blob.Get();
        }

        srhiResourceSetLayout* CreateResourceSetLayout(std::vector<srhiResourceSetLayoutEntry>&& entries, const char*) override
        {
            D3DResourceSetLayout* l = new D3DResourceSetLayout;
            l->entries = std::move(entries);
            return l;
        }

        srhiResourceSet* CreateResourceSet(srhiResourceSetLayout* layout, std::vector<srhiResourceBinding>&& bindings,
                                           const char* label) override
        {
            D3DResourceSet* s = new D3DResourceSet;
            s->native.resize(bindings.size());
            for (size_t i = 0; i < bindings.size(); ++i) {
                const srhiResourceSetLayoutEntry& e = layout->entries[i];
                const srhiResourceBinding& b = bindings[i];
                NativeBinding& n = s->native[i];
                switch (e.type) {
                case SRHI_BINDING_TYPE_UNIFORM_BUFFER: {
                    D3DBuffer* buf = static_cast<D3DBuffer*>(b.buffer);
                    if (!buf->isConstant) {
                        SRHI_ERROR(this, "srhiResourceSetCreate(%s): binding %u: buffer '%s' is not a UNIFORM buffer", label ? label : "",
                                   e.binding, buf->label.c_str());
                        delete s;
                        return nullptr;
                    }
                    n.cb = buf->buffer.Get();
                    n.firstConstant = UINT(b.offset / 16);
                    n.numConstants = UINT(std::min<uint64_t>(AlignUp(b.size, 256) / 16, D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT));
                    s->refs.push_back(buf->buffer);
                    break;
                }
                case SRHI_BINDING_TYPE_STORAGE_BUFFER:
                    n.srv = static_cast<D3DBuffer*>(b.buffer)->GetSRV(this, b.offset, b.size);
                    break;
                case SRHI_BINDING_TYPE_STORAGE_BUFFER_RW:
                    n.uav = static_cast<D3DBuffer*>(b.buffer)->GetUAV(this, b.offset, b.size);
                    break;
                case SRHI_BINDING_TYPE_TEXTURE: n.srv = static_cast<D3DTextureView*>(b.textureView)->GetSRV(); break;
                case SRHI_BINDING_TYPE_STORAGE_TEXTURE: n.uav = static_cast<D3DTextureView*>(b.textureView)->GetUAV(); break;
                case SRHI_BINDING_TYPE_SAMPLER: n.sampler = static_cast<D3DSampler*>(b.sampler)->sampler.Get(); break;
                }
                if (n.srv)
                    s->refs.push_back(ComPtr<IUnknown>(n.srv));
                if (n.uav)
                    s->refs.push_back(ComPtr<IUnknown>(n.uav));
                if (n.sampler)
                    s->refs.push_back(ComPtr<IUnknown>(n.sampler));
                bool ok = (e.type == SRHI_BINDING_TYPE_UNIFORM_BUFFER) || n.srv || n.uav || n.sampler;
                if (!ok) {
                    SRHI_ERROR(this, "srhiResourceSetCreate(%s): binding %u: failed to create the native view", label ? label : "", e.binding);
                    delete s;
                    return nullptr;
                }
            }
            s->bindings = std::move(bindings);
            return s;
        }

        srhiRenderPipeline* CreateRenderPipeline(const srhiRenderPipelineDesc& d, PipelineLayoutInfo&& layout) override
        {
            const char* label = d.label ? d.label : "";
            const Package& pkg = d.module->package;
            const PkgEntry* vsEntry = pkg.FindEntry(d.vertexEntry);
            ID3DBlob* vsCode = GetBytecode(d.module, vsEntry);
            if (!vsCode)
                return nullptr;
            D3DRenderPipeline* p = new D3DRenderPipeline;
            auto fail = [&](const char* what, HRESULT hr) -> srhiRenderPipeline* {
                SRHI_ERROR(this, "srhiRenderPipelineCreate(%s): %s failed (hr 0x%08x)", label, what, unsigned(hr));
                delete p;
                return nullptr;
            };
            HRESULT hr = device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &p->vs);
            if (FAILED(hr))
                return fail("CreateVertexShader", hr);
            if (d.fragmentEntry) {
                ID3DBlob* psCode = GetBytecode(d.module, pkg.FindEntry(d.fragmentEntry));
                if (!psCode) {
                    delete p;
                    return nullptr;
                }
                hr = device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &p->ps);
                if (FAILED(hr))
                    return fail("CreatePixelShader", hr);
            }

            // Input layout: attributes the shader declares, matched by location.
            std::vector<D3D11_INPUT_ELEMENT_DESC> elements;
            for (uint32_t i = 0; i < d.vertexLayout.attributeCount; ++i) {
                const srhiVertexAttribute& a = d.vertexLayout.attributes[i];
                const PkgVertexInput* input = nullptr;
                for (const PkgVertexInput& in : vsEntry->inputs)
                    if (in.location == a.location)
                        input = &in;
                if (!input)
                    continue;
                bool instanced = d.vertexLayout.buffers[a.bufferIndex].stepMode == SRHI_VERTEX_STEP_MODE_INSTANCE;
                D3D11_INPUT_ELEMENT_DESC e = {};
                e.SemanticName = input->semantic.c_str();
                e.SemanticIndex = input->semanticIndex;
                e.Format = ToDxgi(a.format);
                e.InputSlot = a.bufferIndex;
                e.AlignedByteOffset = a.offset;
                e.InputSlotClass = instanced ? D3D11_INPUT_PER_INSTANCE_DATA : D3D11_INPUT_PER_VERTEX_DATA;
                e.InstanceDataStepRate = instanced ? 1 : 0;
                elements.push_back(e);
            }
            if (!elements.empty()) {
                hr = device->CreateInputLayout(elements.data(), UINT(elements.size()), vsCode->GetBufferPointer(),
                                               vsCode->GetBufferSize(), &p->inputLayout);
                if (FAILED(hr))
                    return fail("CreateInputLayout", hr);
            }
            p->vertexBufferCount = d.vertexLayout.bufferCount;
            for (uint32_t i = 0; i < d.vertexLayout.bufferCount; ++i)
                p->strides[i] = d.vertexLayout.buffers[i].stride;

            D3D11_RASTERIZER_DESC rd = {};
            rd.FillMode = d.raster.fillMode == SRHI_FILL_MODE_WIREFRAME ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
            rd.CullMode = d.raster.cullMode == SRHI_CULL_MODE_NONE    ? D3D11_CULL_NONE
                        : d.raster.cullMode == SRHI_CULL_MODE_FRONT ? D3D11_CULL_FRONT
                                                                    : D3D11_CULL_BACK;
            rd.FrontCounterClockwise = d.raster.frontFace == SRHI_FRONT_FACE_COUNTER_CLOCKWISE;
            rd.DepthBias = d.raster.depthBias;
            rd.DepthBiasClamp = d.raster.depthBiasClamp;
            rd.SlopeScaledDepthBias = d.raster.depthBiasSlopeScale;
            rd.DepthClipEnable = d.raster.depthClamp ? FALSE : TRUE;
            rd.ScissorEnable = TRUE;
            rd.MultisampleEnable = d.sampleCount > 1;
            rd.AntialiasedLineEnable = FALSE;
            hr = device->CreateRasterizerState(&rd, &p->rasterizer);
            if (FAILED(hr))
                return fail("CreateRasterizerState", hr);

            D3D11_BLEND_DESC bd = {};
            bd.AlphaToCoverageEnable = d.alphaToCoverage;
            bd.IndependentBlendEnable = TRUE;
            for (uint32_t i = 0; i < SRHI_MAX_COLOR_ATTACHMENTS; ++i) {
                D3D11_RENDER_TARGET_BLEND_DESC& rt = bd.RenderTarget[i];
                if (i >= d.colorTargetCount) {
                    rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE;
                    rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_ZERO;
                    rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
                    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
                    continue;
                }
                const srhiColorTargetState& ct = d.colorTargets[i];
                rt.BlendEnable = ct.blend.enable;
                rt.SrcBlend = ToD3D(ct.blend.srcColor, false);
                rt.DestBlend = ToD3D(ct.blend.dstColor, false);
                rt.BlendOp = ToD3D(ct.blend.colorOp);
                rt.SrcBlendAlpha = ToD3D(ct.blend.srcAlpha, true);
                rt.DestBlendAlpha = ToD3D(ct.blend.dstAlpha, true);
                rt.BlendOpAlpha = ToD3D(ct.blend.alphaOp);
                rt.RenderTargetWriteMask = UINT8(ct.writeMask & 0xF); // same bit layout as D3D11_COLOR_WRITE_ENABLE
            }
            hr = device->CreateBlendState(&bd, &p->blend);
            if (FAILED(hr))
                return fail("CreateBlendState", hr);

            const srhiDepthStencilState& ds = d.depthStencil;
            D3D11_DEPTH_STENCIL_DESC dsd = {};
            dsd.DepthEnable = ds.depthTestEnable ? TRUE : FALSE;
            dsd.DepthWriteMask = (ds.depthTestEnable && ds.depthWriteEnable) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
            dsd.DepthFunc = ToD3D(ds.depthCompare);
            dsd.StencilEnable = ds.stencilEnable ? TRUE : FALSE;
            dsd.StencilReadMask = ds.stencilReadMask;
            dsd.StencilWriteMask = ds.stencilWriteMask;
            auto face = [](const srhiStencilFaceState& f) {
                D3D11_DEPTH_STENCILOP_DESC o = {};
                o.StencilFailOp = ToD3D(f.failOp);
                o.StencilDepthFailOp = ToD3D(f.depthFailOp);
                o.StencilPassOp = ToD3D(f.passOp);
                o.StencilFunc = ToD3D(f.compare);
                return o;
            };
            dsd.FrontFace = face(ds.stencilFront);
            dsd.BackFace = face(ds.stencilBack);
            hr = device->CreateDepthStencilState(&dsd, &p->depthStencil);
            if (FAILED(hr))
                return fail("CreateDepthStencilState", hr);

            p->topology = ToD3D(d.topology);
            p->layout = std::move(layout);
            return p;
        }

        srhiComputePipeline* CreateComputePipeline(const srhiComputePipelineDesc& d, PipelineLayoutInfo&& layout) override
        {
            ID3DBlob* code = GetBytecode(d.module, d.module->package.FindEntry(d.entry));
            if (!code)
                return nullptr;
            D3DComputePipeline* p = new D3DComputePipeline;
            HRESULT hr = device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &p->cs);
            if (FAILED(hr)) {
                SRHI_ERROR(this, "srhiComputePipelineCreate(%s): CreateComputeShader failed (hr 0x%08x)", d.label ? d.label : "",
                           unsigned(hr));
                delete p;
                return nullptr;
            }
            p->layout = std::move(layout);
            return p;
        }

        srhiQuerySet* CreateQuerySet(uint32_t count, const char* label) override
        {
            D3DQuerySet* q = new D3DQuerySet;
            q->queries.resize(count);
            D3D11_QUERY_DESC qd = {D3D11_QUERY_TIMESTAMP, 0};
            for (uint32_t i = 0; i < count; ++i)
                if (FAILED(device->CreateQuery(&qd, &q->queries[i]))) {
                    SRHI_ERROR(this, "srhiQuerySetCreate(%s): CreateQuery failed", label ? label : "");
                    delete q;
                    return nullptr;
                }
            q->results.assign(count, 0);
            q->valid.assign(count, 0);
            return q;
        }

        srhiCommandBuffer* BeginCommandBuffer(const char*) override
        {
            D3DCommandBuffer* c = new D3DCommandBuffer;
            c->dev = this;
            c->render.owner = c;
            c->compute.owner = c;
            c->blit.owner = c;
            return c;
        }

        srhiSwapchain* CreateSwapchain(const srhiSwapchainDesc& d) override;

        void Execute(D3DCommandBuffer* cmd, srhiCompletionCallback callback, void* userData);
    };

    // =================================================================================================
    // Views
    // =================================================================================================

    D3DDevice* DeviceOf(srhiDevice* d) { return static_cast<D3DDevice*>(d); }

    ID3D11ShaderResourceView* D3DBuffer::GetSRV(D3DDevice* dev, uint64_t offset, uint64_t size)
    {
        auto key = std::make_pair(offset, size);
        auto it = srvs.find(key);
        if (it != srvs.end())
            return it->second.Get();
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_UNKNOWN;
        sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sd.Buffer.FirstElement = UINT(offset / desc.structureStride);
        sd.Buffer.NumElements = UINT(size / desc.structureStride);
        ComPtr<ID3D11ShaderResourceView> srv;
        if (FAILED(dev->device->CreateShaderResourceView(buffer.Get(), &sd, &srv)))
            SRHI_ERROR(dev, "buffer '%s': CreateShaderResourceView failed", label.c_str());
        srvs[key] = srv;
        return srv.Get();
    }

    ID3D11UnorderedAccessView* D3DBuffer::GetUAV(D3DDevice* dev, uint64_t offset, uint64_t size)
    {
        auto key = std::make_pair(offset, size);
        auto it = uavs.find(key);
        if (it != uavs.end())
            return it->second.Get();
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_UNKNOWN;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.FirstElement = UINT(offset / desc.structureStride);
        ud.Buffer.NumElements = UINT(size / desc.structureStride);
        ud.Buffer.Flags = 0;
        ComPtr<ID3D11UnorderedAccessView> uav;
        if (FAILED(dev->device->CreateUnorderedAccessView(buffer.Get(), &ud, &uav)))
            SRHI_ERROR(dev, "buffer '%s': CreateUnorderedAccessView failed", label.c_str());
        uavs[key] = uav;
        return uav.Get();
    }

    ID3D11ShaderResourceView* D3DTextureView::GetSRV()
    {
        if (srv)
            return srv.Get();
        D3DTexture* t = static_cast<D3DTexture*>(texture);
        D3DDevice* dev = DeviceOf(t->device);
        const srhiTextureDesc& td = t->desc;
        bool msaa = td.sampleCount > 1;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = SrvFormat(desc.format);
        switch (desc.type) {
        case SRHI_TEXTURE_TYPE_2D:
            if (msaa)
                sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
            else if (td.type == SRHI_TEXTURE_TYPE_2D) {
                sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                sd.Texture2D.MostDetailedMip = desc.baseMip;
                sd.Texture2D.MipLevels = desc.mipCount;
            } else {
                sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
                sd.Texture2DArray.MostDetailedMip = desc.baseMip;
                sd.Texture2DArray.MipLevels = desc.mipCount;
                sd.Texture2DArray.FirstArraySlice = desc.baseLayer;
                sd.Texture2DArray.ArraySize = 1;
            }
            break;
        case SRHI_TEXTURE_TYPE_2D_ARRAY:
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            sd.Texture2DArray.MostDetailedMip = desc.baseMip;
            sd.Texture2DArray.MipLevels = desc.mipCount;
            sd.Texture2DArray.FirstArraySlice = desc.baseLayer;
            sd.Texture2DArray.ArraySize = desc.layerCount;
            break;
        case SRHI_TEXTURE_TYPE_CUBE:
            if (desc.baseLayer == 0) {
                sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
                sd.TextureCube.MostDetailedMip = desc.baseMip;
                sd.TextureCube.MipLevels = desc.mipCount;
            } else {
                // One cube out of a cube array: D3D11 can only express it as a 1-cube cube array.
                sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
                sd.TextureCubeArray.MostDetailedMip = desc.baseMip;
                sd.TextureCubeArray.MipLevels = desc.mipCount;
                sd.TextureCubeArray.First2DArrayFace = desc.baseLayer;
                sd.TextureCubeArray.NumCubes = 1;
            }
            break;
        case SRHI_TEXTURE_TYPE_CUBE_ARRAY:
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
            sd.TextureCubeArray.MostDetailedMip = desc.baseMip;
            sd.TextureCubeArray.MipLevels = desc.mipCount;
            sd.TextureCubeArray.First2DArrayFace = desc.baseLayer;
            sd.TextureCubeArray.NumCubes = desc.layerCount / 6;
            break;
        case SRHI_TEXTURE_TYPE_3D:
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
            sd.Texture3D.MostDetailedMip = desc.baseMip;
            sd.Texture3D.MipLevels = desc.mipCount;
            break;
        }
        if (FAILED(dev->device->CreateShaderResourceView(t->resource.Get(), &sd, &srv)))
            SRHI_ERROR(dev, "texture '%s': CreateShaderResourceView failed", t->label.c_str());
        return srv.Get();
    }

    ID3D11UnorderedAccessView* D3DTextureView::GetUAV()
    {
        if (uav)
            return uav.Get();
        D3DTexture* t = static_cast<D3DTexture*>(texture);
        D3DDevice* dev = DeviceOf(t->device);
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = ToDxgi(desc.format);
        if (t->desc.type == SRHI_TEXTURE_TYPE_3D) {
            ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
            ud.Texture3D.MipSlice = desc.baseMip;
            ud.Texture3D.FirstWSlice = 0;
            ud.Texture3D.WSize = UINT(-1);
        } else if (t->desc.type == SRHI_TEXTURE_TYPE_2D) {
            ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            ud.Texture2D.MipSlice = desc.baseMip;
        } else {
            ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
            ud.Texture2DArray.MipSlice = desc.baseMip;
            ud.Texture2DArray.FirstArraySlice = desc.baseLayer;
            ud.Texture2DArray.ArraySize = desc.layerCount;
        }
        if (FAILED(dev->device->CreateUnorderedAccessView(t->resource.Get(), &ud, &uav)))
            SRHI_ERROR(dev, "texture '%s': CreateUnorderedAccessView failed", t->label.c_str());
        return uav.Get();
    }

    ID3D11RenderTargetView* D3DTextureView::GetRTV()
    {
        if (rtv)
            return rtv.Get();
        D3DTexture* t = static_cast<D3DTexture*>(texture);
        D3DDevice* dev = DeviceOf(t->device);
        D3D11_RENDER_TARGET_VIEW_DESC rd = {};
        rd.Format = ToDxgi(desc.format);
        if (t->desc.sampleCount > 1)
            rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
        else if (t->desc.type == SRHI_TEXTURE_TYPE_3D) {
            rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE3D;
            rd.Texture3D.MipSlice = desc.baseMip;
            rd.Texture3D.FirstWSlice = desc.baseLayer;
            rd.Texture3D.WSize = 1;
        } else if (t->desc.type == SRHI_TEXTURE_TYPE_2D) {
            rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            rd.Texture2D.MipSlice = desc.baseMip;
        } else {
            rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            rd.Texture2DArray.MipSlice = desc.baseMip;
            rd.Texture2DArray.FirstArraySlice = desc.baseLayer;
            rd.Texture2DArray.ArraySize = 1;
        }
        if (FAILED(dev->device->CreateRenderTargetView(t->resource.Get(), &rd, &rtv)))
            SRHI_ERROR(dev, "texture '%s': CreateRenderTargetView failed", t->label.c_str());
        return rtv.Get();
    }

    ID3D11DepthStencilView* D3DTextureView::GetDSV(bool readOnly)
    {
        ComPtr<ID3D11DepthStencilView>& slot = readOnly ? dsvReadOnly : dsv;
        if (slot)
            return slot.Get();
        D3DTexture* t = static_cast<D3DTexture*>(texture);
        D3DDevice* dev = DeviceOf(t->device);
        D3D11_DEPTH_STENCIL_VIEW_DESC dd = {};
        dd.Format = ToDxgi(desc.format);
        if (readOnly)
            dd.Flags = D3D11_DSV_READ_ONLY_DEPTH | (GetFormatInfo(desc.format).stencil ? D3D11_DSV_READ_ONLY_STENCIL : 0);
        if (t->desc.sampleCount > 1)
            dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
        else if (t->desc.type == SRHI_TEXTURE_TYPE_2D) {
            dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            dd.Texture2D.MipSlice = desc.baseMip;
        } else {
            dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
            dd.Texture2DArray.MipSlice = desc.baseMip;
            dd.Texture2DArray.FirstArraySlice = desc.baseLayer;
            dd.Texture2DArray.ArraySize = 1;
        }
        if (FAILED(dev->device->CreateDepthStencilView(t->resource.Get(), &dd, &slot)))
            SRHI_ERROR(dev, "texture '%s': CreateDepthStencilView failed", t->label.c_str());
        return slot.Get();
    }

    // =================================================================================================
    // Replay
    // =================================================================================================

    void Replay::UnbindAll()
    {
        static ID3D11ShaderResourceView* const nullSrvs[kMaxSrvSlots] = {};
        static ID3D11UnorderedAccessView* const nullUavs[kMaxUavSlots] = {};
        static const UINT counts[kMaxUavSlots] = {UINT(-1), UINT(-1), UINT(-1), UINT(-1), UINT(-1), UINT(-1), UINT(-1), UINT(-1)};
        ctx->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 0, nullptr, nullptr);
        ctx->VSSetShaderResources(0, kMaxSrvSlots, nullSrvs);
        ctx->PSSetShaderResources(0, kMaxSrvSlots, nullSrvs);
        ctx->CSSetShaderResources(0, kMaxSrvSlots, nullSrvs);
        ctx->CSSetUnorderedAccessViews(0, kMaxUavSlots, nullUavs, counts);
    }

    bool Replay::Flush(bool compute)
    {
        const PipelineLayoutInfo* layout = compute ? (cp ? &cp->layout : nullptr) : (rp ? &rp->layout : nullptr);
        const char* label = compute ? (cp ? cp->label.c_str() : "") : (rp ? rp->label.c_str() : "");
        if (!layout) {
            SRHI_ERROR(dev, "draw/dispatch without a pipeline");
            return false;
        }

        if (!compute && vbDirty) {
            for (uint32_t i = 0; i < rp->vertexBufferCount; ++i)
                ctx->IASetVertexBuffers(i, 1, &vbs[i], &rp->strides[i], &vbOffsets[i]);
            vbDirty = false;
        }

        for (uint32_t s = 0; s < SRHI_MAX_RESOURCE_SETS; ++s) {
            const std::vector<BindingSlot>& slots = layout->slots[s];
            bool needed = false;
            for (const BindingSlot& slot : slots)
                needed |= slot.used;
            if (needed && !sets[s]) {
                SRHI_ERROR(dev, "draw/dispatch with pipeline '%s': resource set %u is not bound", label, s);
                return false;
            }
            if (!setDirty[s] || !sets[s])
                continue;
            setDirty[s] = false;
            if (!needed)
                continue;
            D3DResourceSet* set = static_cast<D3DResourceSet*>(sets[s]);
            srhiResourceSetLayout* expected = layout->setLayouts[s];
            bool compatible = set->layout == expected || set->layout->entries.size() == expected->entries.size();
            for (size_t i = 0; compatible && set->layout != expected && i < expected->entries.size(); ++i)
                compatible = set->layout->entries[i].binding == expected->entries[i].binding &&
                             set->layout->entries[i].type == expected->entries[i].type;
            if (!compatible) {
                SRHI_ERROR(dev, "pipeline '%s': resource set '%s' at index %u has an incompatible layout", label, set->label.c_str(), s);
                return false;
            }

            for (size_t i = 0; i < slots.size(); ++i) {
                const BindingSlot& slot = slots[i];
                if (!slot.used)
                    continue;
                const NativeBinding& n = set->native[i];
                bool vs = !compute && (slot.stages & SRHI_SHADER_STAGE_VERTEX);
                bool ps = !compute && (slot.stages & SRHI_SHADER_STAGE_FRAGMENT);
                UINT reg = slot.d3dSlot;
                switch (slot.d3dClass) {
                case D3DClass::B:
                    if (compute)
                        ctx->CSSetConstantBuffers1(reg, 1, &n.cb, &n.firstConstant, &n.numConstants);
                    if (vs)
                        ctx->VSSetConstantBuffers1(reg, 1, &n.cb, &n.firstConstant, &n.numConstants);
                    if (ps)
                        ctx->PSSetConstantBuffers1(reg, 1, &n.cb, &n.firstConstant, &n.numConstants);
                    break;
                case D3DClass::T:
                    if (compute)
                        ctx->CSSetShaderResources(reg, 1, &n.srv);
                    if (vs)
                        ctx->VSSetShaderResources(reg, 1, &n.srv);
                    if (ps)
                        ctx->PSSetShaderResources(reg, 1, &n.srv);
                    break;
                case D3DClass::S:
                    if (compute)
                        ctx->CSSetSamplers(reg, 1, &n.sampler);
                    if (vs)
                        ctx->VSSetSamplers(reg, 1, &n.sampler);
                    if (ps)
                        ctx->PSSetSamplers(reg, 1, &n.sampler);
                    break;
                case D3DClass::U:
                    if (reg >= kMaxUavSlots) {
                        SRHI_ERROR(dev, "pipeline '%s': UAV register u%u exceeds the D3D11 limit of %u", label, reg, kMaxUavSlots);
                        return false;
                    }
                    if (compute) {
                        UINT count = UINT(-1);
                        ctx->CSSetUnorderedAccessViews(reg, 1, &n.uav, &count);
                    } else {
                        if (reg < numRTV) {
                            SRHI_ERROR(dev, "pipeline '%s': UAV register u%u overlaps the pass's %u render targets", label, reg, numRTV);
                            return false;
                        }
                        psUavs[reg] = n.uav;
                        psUavDirty = true;
                    }
                    break;
                default: break;
                }
            }
        }

        for (uint32_t u = 0; u < SRHI_MAX_UNIFORM_SLOTS; ++u) {
            const BindingSlot& slot = layout->uniforms[u];
            if (!slot.used)
                continue;
            if (uniformSize[u] == 0) {
                SRHI_ERROR(dev, "draw/dispatch with pipeline '%s': uniform slot %u was never set", label, u);
                return false;
            }
            if (!uniformDirty[u])
                continue;
            uniformDirty[u] = false;
            if (uniformSize[u] < layout->uniformSizes[u]) {
                SRHI_ERROR(dev, "pipeline '%s': uniform slot %u needs %u bytes, %u were set", label, u, layout->uniformSizes[u],
                           uniformSize[u]);
                return false;
            }
            UINT numConstants = 0;
            UINT first = dev->UploadUniforms(arena->data() + uniformOffset[u], uniformSize[u], numConstants);
            ID3D11Buffer* ring = dev->uniformRing.Get();
            if (compute)
                ctx->CSSetConstantBuffers1(slot.d3dSlot, 1, &ring, &first, &numConstants);
            else {
                if (slot.stages & SRHI_SHADER_STAGE_VERTEX)
                    ctx->VSSetConstantBuffers1(slot.d3dSlot, 1, &ring, &first, &numConstants);
                if (slot.stages & SRHI_SHADER_STAGE_FRAGMENT)
                    ctx->PSSetConstantBuffers1(slot.d3dSlot, 1, &ring, &first, &numConstants);
            }
        }

        if (!compute && psUavDirty) {
            static const UINT counts[kMaxUavSlots] = {UINT(-1), UINT(-1), UINT(-1), UINT(-1), UINT(-1), UINT(-1), UINT(-1), UINT(-1)};
            ctx->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL, nullptr, nullptr, numRTV,
                                                           kMaxUavSlots - numRTV, &psUavs[numRTV], counts);
            psUavDirty = false;
        }
        return true;
    }

    void D3DDevice::Execute(D3DCommandBuffer* cmd, srhiCompletionCallback callback, void* userData)
    {
        InFlight f;
        D3D11_QUERY_DESC ed = {D3D11_QUERY_EVENT, 0};
        device->CreateQuery(&ed, &f.event);
        if (cmd->usesTimestamps) {
            D3D11_QUERY_DESC dd = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            device->CreateQuery(&dd, &f.disjoint);
        }
        f.callback = callback;
        f.userData = userData;

        Replay r;
        r.dev = this;
        r.ctx = ctx.Get();
        r.flight = &f;
        r.arena = &cmd->arena;
        r.ResetBindings();

        if (f.disjoint)
            ctx->Begin(f.disjoint.Get());
        for (Command& c : cmd->commands)
            c(r);
        if (f.disjoint)
            ctx->End(f.disjoint.Get());
        ctx->End(f.event.Get());
        inFlight.push_back(std::move(f));
        DrainMessages();
    }

    // =================================================================================================
    // Encoders
    // =================================================================================================

    void WriteTimestamp(Replay& r, D3DQuerySet* qs, uint32_t index)
    {
        if (!qs || index == SRHI_QUERY_UNUSED)
            return;
        r.ctx->End(qs->queries[index].Get());
        r.flight->queries.push_back({qs, index});
    }

    srhiRenderEncoder* D3DCommandBuffer::BeginRender(const srhiRenderPassDesc& desc)
    {
        srhiRenderPassDesc d = desc;
        d.label = nullptr;
        std::wstring label = ToWide(desc.label);
        D3DQuerySet* qs = static_cast<D3DQuerySet*>(desc.timestampWrites.querySet);
        MarkQuery(qs, desc.timestampWrites.beginIndex);
        MarkQuery(qs, desc.timestampWrites.endIndex);

        Record([d, label, qs](Replay& r) {
            if (r.dev->annotation && !label.empty())
                r.dev->annotation->BeginEvent(label.c_str());
            WriteTimestamp(r, qs, d.timestampWrites.beginIndex);
            r.endQuerySet = qs;
            r.endQueryIndex = d.timestampWrites.endIndex;

            r.UnbindAll();
            r.ResetBindings();
            r.resolves.clear();
            r.discardAtEnd.clear();

            ID3D11RenderTargetView* rtvs[SRHI_MAX_COLOR_ATTACHMENTS] = {};
            for (uint32_t i = 0; i < d.colorAttachmentCount; ++i) {
                const srhiColorAttachment& c = d.colorAttachments[i];
                D3DTextureView* v = static_cast<D3DTextureView*>(c.view);
                rtvs[i] = v->GetRTV();
                r.width = MipDim(v->texture->desc.width, v->desc.baseMip);
                r.height = MipDim(v->texture->desc.height, v->desc.baseMip);
                if (c.loadAction == SRHI_LOAD_ACTION_CLEAR)
                    r.ctx->ClearRenderTargetView(rtvs[i], c.clearColor);
                else if (c.loadAction == SRHI_LOAD_ACTION_DONT_CARE)
                    r.ctx->DiscardView(rtvs[i]);
                if ((c.storeAction == SRHI_STORE_ACTION_RESOLVE || c.storeAction == SRHI_STORE_ACTION_STORE_AND_RESOLVE) && c.resolveView)
                    r.resolves.push_back({v, static_cast<D3DTextureView*>(c.resolveView)});
                if (c.storeAction == SRHI_STORE_ACTION_DONT_CARE || c.storeAction == SRHI_STORE_ACTION_RESOLVE)
                    r.discardAtEnd.push_back(rtvs[i]);
            }
            ID3D11DepthStencilView* dsv = nullptr;
            const srhiDepthAttachment& da = d.depthAttachment;
            if (da.view) {
                D3DTextureView* v = static_cast<D3DTextureView*>(da.view);
                bool hasStencil = GetFormatInfo(v->desc.format).stencil;
                // Clears need a writable DSV even for read-only passes; clear first, then bind read-only.
                UINT clearFlags = 0;
                if (da.depthLoadAction == SRHI_LOAD_ACTION_CLEAR)
                    clearFlags |= D3D11_CLEAR_DEPTH;
                if (hasStencil && da.stencilLoadAction == SRHI_LOAD_ACTION_CLEAR)
                    clearFlags |= D3D11_CLEAR_STENCIL;
                if (clearFlags)
                    r.ctx->ClearDepthStencilView(v->GetDSV(false), clearFlags, da.clearDepth, da.clearStencil);
                dsv = v->GetDSV(da.readOnly != 0);
                if (!da.readOnly && da.depthLoadAction == SRHI_LOAD_ACTION_DONT_CARE &&
                    (!hasStencil || da.stencilLoadAction == SRHI_LOAD_ACTION_DONT_CARE))
                    r.ctx->DiscardView(dsv);
                if (!da.readOnly && da.depthStoreAction == SRHI_STORE_ACTION_DONT_CARE &&
                    (!hasStencil || da.stencilStoreAction == SRHI_STORE_ACTION_DONT_CARE))
                    r.discardAtEnd.push_back(dsv);
                r.width = MipDim(v->texture->desc.width, v->desc.baseMip);
                r.height = MipDim(v->texture->desc.height, v->desc.baseMip);
            }
            r.numRTV = d.colorAttachmentCount;
            r.ctx->OMSetRenderTargets(d.colorAttachmentCount, rtvs, dsv);

            D3D11_VIEWPORT vp = {0.0f, 0.0f, float(r.width), float(r.height), 0.0f, 1.0f};
            r.ctx->RSSetViewports(1, &vp);
            D3D11_RECT sc = {0, 0, LONG(r.width), LONG(r.height)};
            r.ctx->RSSetScissorRects(1, &sc);
        });
        return &render;
    }

    void D3DRenderEncoder::End()
    {
        owner->Record([](Replay& r) {
            for (const ResolveOp& op : r.resolves) {
                D3DTexture* src = static_cast<D3DTexture*>(op.src->texture);
                D3DTexture* dst = static_cast<D3DTexture*>(op.dst->texture);
                r.ctx->ResolveSubresource(dst->resource.Get(), dst->Subresource(op.dst->desc.baseMip, op.dst->desc.baseLayer),
                                          src->resource.Get(), src->Subresource(op.src->desc.baseMip, op.src->desc.baseLayer),
                                          ToDxgi(op.src->desc.format));
            }
            for (ID3D11View* v : r.discardAtEnd)
                r.ctx->DiscardView(v);
            r.resolves.clear();
            r.discardAtEnd.clear();
            r.UnbindAll();
            r.ResetBindings();
            r.numRTV = 0;
            WriteTimestamp(r, r.endQuerySet, r.endQueryIndex);
            r.endQuerySet = nullptr;
            if (r.dev->annotation)
                r.dev->annotation->EndEvent();
        });
    }

    void D3DRenderEncoder::SetPipeline(srhiRenderPipeline* p)
    {
        D3DRenderPipeline* rp = static_cast<D3DRenderPipeline*>(p);
        owner->Record([rp](Replay& r) {
            r.rp = rp;
            r.ctx->VSSetShader(rp->vs.Get(), nullptr, 0);
            r.ctx->PSSetShader(rp->ps.Get(), nullptr, 0);
            r.ctx->IASetInputLayout(rp->inputLayout.Get());
            r.ctx->IASetPrimitiveTopology(rp->topology);
            r.ctx->RSSetState(rp->rasterizer.Get());
            r.ctx->OMSetBlendState(rp->blend.Get(), r.blendColor, 0xFFFFFFFFu);
            r.ctx->OMSetDepthStencilState(rp->depthStencil.Get(), r.stencilRef);
            r.MarkAllDirty();
        });
    }

    void D3DRenderEncoder::SetVertexBuffer(uint32_t index, srhiBuffer* buffer, uint64_t offset)
    {
        ID3D11Buffer* b = static_cast<D3DBuffer*>(buffer)->buffer.Get();
        owner->Record([index, b, offset](Replay& r) {
            r.vbs[index] = b;
            r.vbOffsets[index] = UINT(offset);
            r.vbDirty = true;
        });
    }

    void D3DRenderEncoder::SetIndexBuffer(srhiBuffer* buffer, srhiIndexType type, uint64_t offset)
    {
        ID3D11Buffer* b = static_cast<D3DBuffer*>(buffer)->buffer.Get();
        owner->Record([b, type, offset](Replay& r) {
            r.ctx->IASetIndexBuffer(b, type == SRHI_INDEX_TYPE_UINT16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT, UINT(offset));
        });
    }

    void D3DRenderEncoder::SetResourceSet(uint32_t index, srhiResourceSet* set)
    {
        owner->Record([index, set](Replay& r) {
            r.sets[index] = set;
            r.setDirty[index] = true;
        });
    }

    void RecordUniforms(D3DCommandBuffer* owner, uint32_t slot, const void* data, uint32_t size)
    {
        size_t offset = owner->arena.size();
        owner->arena.insert(owner->arena.end(), static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
        owner->Record([slot, offset, size](Replay& r) {
            r.uniformOffset[slot] = offset;
            r.uniformSize[slot] = size;
            r.uniformDirty[slot] = true;
        });
    }

    void D3DRenderEncoder::SetUniforms(uint32_t slot, const void* data, uint32_t size) { RecordUniforms(owner, slot, data, size); }

    void D3DRenderEncoder::SetViewport(const srhiViewport& v)
    {
        owner->Record([v](Replay& r) {
            D3D11_VIEWPORT vp = {v.x, v.y, v.width, v.height, v.minDepth, v.maxDepth};
            r.ctx->RSSetViewports(1, &vp);
        });
    }

    void D3DRenderEncoder::SetScissor(const srhiScissorRect& s)
    {
        owner->Record([s](Replay& r) {
            D3D11_RECT rect = {LONG(s.x), LONG(s.y), LONG(s.x + s.width), LONG(s.y + s.height)};
            r.ctx->RSSetScissorRects(1, &rect);
        });
    }

    void D3DRenderEncoder::SetBlendColor(const float color[4])
    {
        float c[4] = {color[0], color[1], color[2], color[3]};
        owner->Record([c](Replay& r) {
            memcpy(r.blendColor, c, sizeof(c));
            if (r.rp)
                r.ctx->OMSetBlendState(r.rp->blend.Get(), r.blendColor, 0xFFFFFFFFu);
        });
    }

    void D3DRenderEncoder::SetStencilReference(uint32_t reference)
    {
        owner->Record([reference](Replay& r) {
            r.stencilRef = reference;
            if (r.rp)
                r.ctx->OMSetDepthStencilState(r.rp->depthStencil.Get(), r.stencilRef);
        });
    }

    void D3DRenderEncoder::Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
    {
        owner->Record([=](Replay& r) {
            if (r.Flush(false))
                r.ctx->DrawInstanced(vertexCount, instanceCount, firstVertex, firstInstance);
        });
    }

    void D3DRenderEncoder::DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t baseVertex,
                                       uint32_t firstInstance)
    {
        owner->Record([=](Replay& r) {
            if (r.Flush(false))
                r.ctx->DrawIndexedInstanced(indexCount, instanceCount, firstIndex, baseVertex, firstInstance);
        });
    }

    void D3DRenderEncoder::DrawIndirect(srhiBuffer* buffer, uint64_t offset)
    {
        ID3D11Buffer* b = static_cast<D3DBuffer*>(buffer)->buffer.Get();
        owner->Record([b, offset](Replay& r) {
            if (r.Flush(false))
                r.ctx->DrawInstancedIndirect(b, UINT(offset));
        });
    }

    void D3DRenderEncoder::DrawIndexedIndirect(srhiBuffer* buffer, uint64_t offset)
    {
        ID3D11Buffer* b = static_cast<D3DBuffer*>(buffer)->buffer.Get();
        owner->Record([b, offset](Replay& r) {
            if (r.Flush(false))
                r.ctx->DrawIndexedInstancedIndirect(b, UINT(offset));
        });
    }

    void RecordPushGroup(D3DCommandBuffer* owner, const char* name)
    {
        std::wstring w = ToWide(name);
        owner->Record([w](Replay& r) {
            if (r.dev->annotation)
                r.dev->annotation->BeginEvent(w.c_str());
        });
    }

    void RecordPopGroup(D3DCommandBuffer* owner)
    {
        owner->Record([](Replay& r) {
            if (r.dev->annotation)
                r.dev->annotation->EndEvent();
        });
    }

    void RecordMarker(D3DCommandBuffer* owner, const char* name)
    {
        std::wstring w = ToWide(name);
        owner->Record([w](Replay& r) {
            if (r.dev->annotation)
                r.dev->annotation->SetMarker(w.c_str());
        });
    }

    void D3DRenderEncoder::PushDebugGroup(const char* name) { RecordPushGroup(owner, name); }
    void D3DRenderEncoder::PopDebugGroup() { RecordPopGroup(owner); }
    void D3DRenderEncoder::InsertDebugMarker(const char* name) { RecordMarker(owner, name); }

    // --- Compute ---

    srhiComputeEncoder* D3DCommandBuffer::BeginCompute(const srhiComputePassDesc& desc)
    {
        D3DQuerySet* qs = static_cast<D3DQuerySet*>(desc.timestampWrites.querySet);
        MarkQuery(qs, desc.timestampWrites.beginIndex);
        MarkQuery(qs, desc.timestampWrites.endIndex);
        compute.endQuerySet = qs;
        compute.endQueryIndex = qs ? desc.timestampWrites.endIndex : SRHI_QUERY_UNUSED;
        std::wstring label = ToWide(desc.label);
        uint32_t begin = desc.timestampWrites.beginIndex;
        Record([qs, begin, label](Replay& r) {
            if (r.dev->annotation && !label.empty())
                r.dev->annotation->BeginEvent(label.c_str());
            WriteTimestamp(r, qs, begin);
            r.UnbindAll();
            r.ResetBindings();
        });
        return &compute;
    }

    void D3DComputeEncoder::End()
    {
        D3DQuerySet* qs = endQuerySet;
        uint32_t index = endQueryIndex;
        owner->Record([qs, index](Replay& r) {
            r.UnbindAll();
            r.ResetBindings();
            WriteTimestamp(r, qs, index);
            if (r.dev->annotation)
                r.dev->annotation->EndEvent();
        });
    }

    void D3DComputeEncoder::SetPipeline(srhiComputePipeline* p)
    {
        D3DComputePipeline* cp = static_cast<D3DComputePipeline*>(p);
        owner->Record([cp](Replay& r) {
            r.cp = cp;
            r.ctx->CSSetShader(cp->cs.Get(), nullptr, 0);
            r.MarkAllDirty();
        });
    }

    void D3DComputeEncoder::SetResourceSet(uint32_t index, srhiResourceSet* set)
    {
        owner->Record([index, set](Replay& r) {
            r.sets[index] = set;
            r.setDirty[index] = true;
        });
    }

    void D3DComputeEncoder::SetUniforms(uint32_t slot, const void* data, uint32_t size) { RecordUniforms(owner, slot, data, size); }

    void D3DComputeEncoder::Dispatch(uint32_t x, uint32_t y, uint32_t z)
    {
        owner->Record([x, y, z](Replay& r) {
            if (r.Flush(true))
                r.ctx->Dispatch(x, y, z);
        });
    }

    void D3DComputeEncoder::DispatchIndirect(srhiBuffer* buffer, uint64_t offset)
    {
        ID3D11Buffer* b = static_cast<D3DBuffer*>(buffer)->buffer.Get();
        owner->Record([b, offset](Replay& r) {
            if (r.Flush(true))
                r.ctx->DispatchIndirect(b, UINT(offset));
        });
    }

    void D3DComputeEncoder::PushDebugGroup(const char* name) { RecordPushGroup(owner, name); }
    void D3DComputeEncoder::PopDebugGroup() { RecordPopGroup(owner); }
    void D3DComputeEncoder::InsertDebugMarker(const char* name) { RecordMarker(owner, name); }

    // --- Blit ---

    srhiBlitEncoder* D3DCommandBuffer::BeginBlit(const char* label)
    {
        std::wstring w = ToWide(label);
        Record([w](Replay& r) {
            if (r.dev->annotation && !w.empty())
                r.dev->annotation->BeginEvent(w.c_str());
            r.UnbindAll();
        });
        return &blit;
    }

    void D3DBlitEncoder::End()
    {
        owner->Record([](Replay& r) {
            if (r.dev->annotation)
                r.dev->annotation->EndEvent();
        });
    }

    void D3DBlitEncoder::CopyBufferToBuffer(srhiBuffer* src, uint64_t srcOffset, srhiBuffer* dst, uint64_t dstOffset, uint64_t size)
    {
        D3DBuffer* s = static_cast<D3DBuffer*>(src);
        D3DBuffer* d = static_cast<D3DBuffer*>(dst);
        owner->Record([s, d, srcOffset, dstOffset, size](Replay& r) {
            D3D11_BOX box = {UINT(srcOffset), 0, 0, UINT(srcOffset + size), 1, 1};
            r.ctx->CopySubresourceRegion(d->buffer.Get(), 0, UINT(dstOffset), 0, 0, s->buffer.Get(), 0, &box);
            if (d->isConstant)
                SRHI_WARN(r.dev, "CopyBufferToBuffer into uniform buffer '%s': its CPU shadow is now stale (later partial writes may "
                                 "restore old bytes)", d->label.c_str());
        });
    }

    void D3DBlitEncoder::CopyBufferToTexture(const srhiBufferCopyLocation& src, const srhiTextureCopyLocation& dst, uint32_t width,
                                             uint32_t height, uint32_t depth)
    {
        // D3D11 cannot copy a buffer into a texture on the GPU: read the buffer back through a staging
        // buffer (stalls until the GPU caught up) and UpdateSubresource the texture.
        owner->Record([src, dst, width, height, depth](Replay& r) {
            D3DTexture* t = static_cast<D3DTexture*>(dst.texture);
            D3DBuffer* b = static_cast<D3DBuffer*>(src.buffer);
            uint32_t rows = RowCount(t->desc.format, height);
            uint32_t rowsPerImage = src.rowsPerImage ? src.rowsPerImage : rows;
            uint64_t bytesPerImage = uint64_t(src.bytesPerRow) * rowsPerImage;
            uint64_t total = bytesPerImage * (depth - 1) + uint64_t(src.bytesPerRow) * (rows - 1) + RowBytes(t->desc.format, width);
            ComPtr<ID3D11Buffer> staging = r.dev->CreateStagingBuffer(total, true);
            if (!staging) {
                SRHI_ERROR(r.dev, "CopyBufferToTexture: staging allocation failed");
                return;
            }
            D3D11_BOX box = {UINT(src.offset), 0, 0, UINT(src.offset + total), 1, 1};
            r.ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, b->buffer.Get(), 0, &box);
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            if (FAILED(r.ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                SRHI_ERROR(r.dev, "CopyBufferToTexture: staging map failed");
                return;
            }
            const uint8_t* data = static_cast<const uint8_t*>(mapped.pData);
            if (t->desc.type == SRHI_TEXTURE_TYPE_3D) {
                D3D11_BOX tb = {dst.x, dst.y, dst.z, dst.x + width, dst.y + height, dst.z + depth};
                r.ctx->UpdateSubresource(t->resource.Get(), t->Subresource(dst.mip, 0), &tb, data, src.bytesPerRow, UINT(bytesPerImage));
            } else {
                for (uint32_t i = 0; i < depth; ++i) {
                    D3D11_BOX tb = {dst.x, dst.y, 0, dst.x + width, dst.y + height, 1};
                    r.ctx->UpdateSubresource(t->resource.Get(), t->Subresource(dst.mip, dst.layer + i), &tb, data + bytesPerImage * i,
                                             src.bytesPerRow, UINT(bytesPerImage));
                }
            }
            r.ctx->Unmap(staging.Get(), 0);
        });
    }

    void D3DBlitEncoder::CopyTextureToBuffer(const srhiTextureCopyLocation& src, const srhiBufferCopyLocation& dst, uint32_t width,
                                             uint32_t height, uint32_t depth)
    {
        // Texture -> staging texture on the GPU, then map and write the rows into the buffer.
        owner->Record([src, dst, width, height, depth](Replay& r) {
            D3DTexture* t = static_cast<D3DTexture*>(src.texture);
            D3DBuffer* b = static_cast<D3DBuffer*>(dst.buffer);
            bool is3D = t->desc.type == SRHI_TEXTURE_TYPE_3D;
            bool fullOnly = GetFormatInfo(t->desc.format).depth; // depth copies cannot use a box
            uint32_t mw = MipDim(t->desc.width, src.mip), mh = MipDim(t->desc.height, src.mip);
            uint32_t sw = fullOnly ? mw : width, sh = fullOnly ? mh : height;
            ComPtr<ID3D11Resource> staging = r.dev->CreateStagingTexture(t, sw, sh, depth);
            if (!staging)
                return;
            if (is3D) {
                D3D11_BOX box = {src.x, src.y, src.z, src.x + width, src.y + height, src.z + depth};
                r.ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, t->resource.Get(), t->Subresource(src.mip, 0), &box);
            } else {
                for (uint32_t i = 0; i < depth; ++i) {
                    D3D11_BOX box = {src.x, src.y, 0, src.x + width, src.y + height, 1};
                    r.ctx->CopySubresourceRegion(staging.Get(), D3D11CalcSubresource(0, i, 1), 0, 0, 0, t->resource.Get(),
                                                 t->Subresource(src.mip, src.layer + i), fullOnly ? nullptr : &box);
                }
            }
            uint32_t rows = RowCount(t->desc.format, height), rowBytes = RowBytes(t->desc.format, width);
            uint32_t rowsPerImage = dst.rowsPerImage ? dst.rowsPerImage : rows;
            uint32_t xBytes = fullOnly ? RowBytes(t->desc.format, src.x) : 0;
            uint32_t yRows = fullOnly ? RowCount(t->desc.format, src.y) : 0;
            for (uint32_t i = 0; i < depth; ++i) {
                UINT sub = is3D ? 0 : D3D11CalcSubresource(0, i, 1);
                D3D11_MAPPED_SUBRESOURCE mapped = {};
                if (FAILED(r.ctx->Map(staging.Get(), sub, D3D11_MAP_READ, 0, &mapped))) {
                    SRHI_ERROR(r.dev, "CopyTextureToBuffer: staging map failed");
                    return;
                }
                const uint8_t* base = static_cast<const uint8_t*>(mapped.pData) + (is3D ? size_t(mapped.DepthPitch) * i : 0);
                for (uint32_t row = 0; row < rows; ++row) {
                    uint64_t dstOffset = dst.offset + (uint64_t(rowsPerImage) * i + row) * dst.bytesPerRow;
                    r.dev->WriteBufferNow(b, dstOffset, base + size_t(row + yRows) * mapped.RowPitch + xBytes, rowBytes);
                }
                r.ctx->Unmap(staging.Get(), sub);
            }
        });
    }

    void D3DBlitEncoder::CopyTextureToTexture(const srhiTextureCopyLocation& src, const srhiTextureCopyLocation& dst, uint32_t width,
                                              uint32_t height, uint32_t depth)
    {
        owner->Record([src, dst, width, height, depth](Replay& r) {
            D3DTexture* s = static_cast<D3DTexture*>(src.texture);
            D3DTexture* d = static_cast<D3DTexture*>(dst.texture);
            bool fullOnly = GetFormatInfo(s->desc.format).depth || s->desc.sampleCount > 1;
            bool s3D = s->desc.type == SRHI_TEXTURE_TYPE_3D, d3D = d->desc.type == SRHI_TEXTURE_TYPE_3D;
            if (s3D && d3D) {
                D3D11_BOX box = {src.x, src.y, src.z, src.x + width, src.y + height, src.z + depth};
                r.ctx->CopySubresourceRegion(d->resource.Get(), d->Subresource(dst.mip, 0), dst.x, dst.y, dst.z, s->resource.Get(),
                                             s->Subresource(src.mip, 0), &box);
                return;
            }
            for (uint32_t i = 0; i < depth; ++i) {
                D3D11_BOX box = {src.x, src.y, s3D ? src.z + i : 0, src.x + width, src.y + height, s3D ? src.z + i + 1 : 1};
                r.ctx->CopySubresourceRegion(d->resource.Get(), d->Subresource(dst.mip, d3D ? 0 : dst.layer + i), fullOnly ? 0 : dst.x,
                                             fullOnly ? 0 : dst.y, d3D ? dst.z + i : 0, s->resource.Get(),
                                             s->Subresource(src.mip, s3D ? 0 : src.layer + i), fullOnly ? nullptr : &box);
            }
        });
    }

    void D3DBlitEncoder::GenerateMipmaps(srhiTexture* texture)
    {
        D3DTexture* t = static_cast<D3DTexture*>(texture);
        owner->Record([t](Replay& r) {
            D3DTextureView* v = static_cast<D3DTextureView*>(t->defaultView);
            ID3D11ShaderResourceView* srv = v->GetSRV();
            if (srv)
                r.ctx->GenerateMips(srv);
        });
    }

    void D3DBlitEncoder::PushDebugGroup(const char* name) { RecordPushGroup(owner, name); }
    void D3DBlitEncoder::PopDebugGroup() { RecordPopGroup(owner); }

    // --- Command buffer ---

    void D3DCommandBuffer::PushDebugGroup(const char* name) { RecordPushGroup(this, name); }
    void D3DCommandBuffer::PopDebugGroup() { RecordPopGroup(this); }

    void D3DCommandBuffer::ReadBuffer(srhiBuffer* buffer, uint64_t offset, uint64_t size, srhiReadbackCallback callback, void* userData)
    {
        D3DBuffer* b = static_cast<D3DBuffer*>(buffer);
        Record([b, offset, size, callback, userData](Replay& r) {
            Readback rb;
            ComPtr<ID3D11Buffer> staging = r.dev->CreateStagingBuffer(size, true);
            if (!staging) {
                SRHI_ERROR(r.dev, "srhiCommandBufferReadBuffer: staging allocation failed");
                return;
            }
            D3D11_BOX box = {UINT(offset), 0, 0, UINT(offset + size), 1, 1};
            r.ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, b->buffer.Get(), 0, &box);
            rb.staging = staging;
            rb.size = size;
            rb.callback = callback;
            rb.userData = userData;
            r.flight->readbacks.push_back(std::move(rb));
        });
    }

    void D3DCommandBuffer::ReadTexture(srhiTexture* texture, uint32_t mip, uint32_t layer, srhiReadbackCallback callback, void* userData)
    {
        D3DTexture* t = static_cast<D3DTexture*>(texture);
        Record([t, mip, layer, callback, userData](Replay& r) {
            uint32_t w = MipDim(t->desc.width, mip), h = MipDim(t->desc.height, mip);
            ComPtr<ID3D11Resource> staging = r.dev->CreateStagingTexture(t, w, h, 1);
            if (!staging)
                return;
            if (t->desc.type == SRHI_TEXTURE_TYPE_3D) {
                D3D11_BOX box = {0, 0, layer, w, h, layer + 1};
                r.ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, t->resource.Get(), t->Subresource(mip, 0), &box);
            } else {
                // Whole-subresource copy (no box): also valid for depth textures.
                r.ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, t->resource.Get(), t->Subresource(mip, layer), nullptr);
            }
            Readback rb;
            rb.staging = staging;
            rb.isTexture = true;
            rb.rowBytes = RowBytes(t->desc.format, w);
            rb.rowCount = RowCount(t->desc.format, h);
            rb.callback = callback;
            rb.userData = userData;
            r.flight->readbacks.push_back(std::move(rb));
        });
    }

    void D3DCommandBuffer::EncodeNative(srhiNativeEncodeCallback callback, void* userData)
    {
        Record([callback, userData](Replay& r) {
            r.UnbindAll();
            callback(r.ctx, userData);
        });
    }

    void D3DCommandBuffer::Present(srhiSwapchain* swapchain)
    {
        D3DSwapchain* sc = static_cast<D3DSwapchain*>(swapchain);
        Record([sc](Replay& r) {
            r.UnbindAll();
            sc->PresentNow();
        });
    }

    void D3DCommandBuffer::Commit(srhiCompletionCallback callback, void* userData)
    {
        dev->Execute(this, callback, userData);
        delete this;
    }

    // =================================================================================================
    // Swapchain
    // =================================================================================================

    DXGI_FORMAT SwapchainBufferFormat(srhiColorSpace cs)
    {
        switch (cs) {
        case SRHI_COLOR_SPACE_HDR10_PQ: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case SRHI_COLOR_SPACE_EXTENDED_LINEAR_SRGB: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default: return DXGI_FORMAT_B8G8R8A8_UNORM;
        }
    }

    DXGI_COLOR_SPACE_TYPE DxgiColorSpace(srhiColorSpace cs)
    {
        switch (cs) {
        case SRHI_COLOR_SPACE_HDR10_PQ: return DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        case SRHI_COLOR_SPACE_EXTENDED_LINEAR_SRGB: return DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
        default: return DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        }
    }

    srhiFormat SwapchainSrhiFormat(srhiColorSpace cs)
    {
        switch (cs) {
        case SRHI_COLOR_SPACE_HDR10_PQ: return SRHI_FORMAT_RGB10A2_UNORM;
        case SRHI_COLOR_SPACE_EXTENDED_LINEAR_SRGB: return SRHI_FORMAT_RGBA16_FLOAT;
        default: return SRHI_FORMAT_BGRA8_UNORM_SRGB;
        }
    }

    bool D3DSwapchain::ApplyColorSpace(srhiColorSpace cs)
    {
        ComPtr<IDXGISwapChain3> sc3;
        if (FAILED(swapchain.As(&sc3)))
            return cs == SRHI_COLOR_SPACE_SRGB;
        UINT support = 0;
        DXGI_COLOR_SPACE_TYPE type = DxgiColorSpace(cs);
        if (FAILED(sc3->CheckColorSpaceSupport(type, &support)) || !(support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))
            return false;
        return SUCCEEDED(sc3->SetColorSpace1(type));
    }

    void D3DSwapchain::AcquireBuffer()
    {
        ComPtr<ID3D11Texture2D> buffer;
        if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&buffer)))) {
            SRHI_ERROR(dev, "swapchain '%s': GetBuffer failed", label.c_str());
            return;
        }
        D3D11_TEXTURE2D_DESC td = {};
        buffer->GetDesc(&td);
        image->resource = buffer;
        image->resourceFormat = td.Format;
        image->desc = {};
        image->desc.type = SRHI_TEXTURE_TYPE_2D;
        image->desc.format = format;
        image->desc.width = td.Width;
        image->desc.height = td.Height;
        image->desc.depthOrLayers = 1;
        image->desc.mipLevels = 1;
        image->desc.sampleCount = 1;
        image->desc.usage = SRHI_TEXTURE_USAGE_RENDER_TARGET | SRHI_TEXTURE_USAGE_COPY_SRC | SRHI_TEXTURE_USAGE_COPY_DST;
        image->desc.label = image->label.c_str();
        imageView->desc = {};
        imageView->desc.format = format; // BGRA8_UNORM_SRGB RTV on a B8G8R8A8_UNORM flip-model buffer is allowed
        imageView->desc.type = SRHI_TEXTURE_TYPE_2D;
        imageView->desc.mipCount = 1;
        imageView->desc.layerCount = 1;
    }

    void D3DSwapchain::PresentNow()
    {
        UINT sync = vsync ? 1 : 0;
        UINT presentFlags = (!vsync && tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0;
        HRESULT hr = swapchain->Present(sync, presentFlags);
        if (FAILED(hr))
            SRHI_ERROR(dev, "swapchain '%s': Present failed (hr 0x%08x)", label.c_str(), unsigned(hr));
    }

    void D3DSwapchain::Resize(uint32_t width, uint32_t height)
    {
        if (width == 0 || height == 0)
            return;
        dev->WaitIdle();
        ReleaseBuffer();
        dev->ctx->ClearState();
        dev->ctx->Flush();
        HRESULT hr = swapchain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, flags);
        if (FAILED(hr))
            SRHI_ERROR(dev, "swapchain '%s': ResizeBuffers failed (hr 0x%08x)", label.c_str(), unsigned(hr));
        AcquireBuffer();
    }

    bool D3DSwapchain::Configure(srhiColorSpace colorSpace, bool newVsync)
    {
        DXGI_FORMAT newFormat = SwapchainBufferFormat(colorSpace);
        if (newFormat != bufferFormat || colorSpace != desc.colorSpace) {
            dev->WaitIdle();
            ReleaseBuffer();
            dev->ctx->ClearState();
            dev->ctx->Flush();
            if (FAILED(swapchain->ResizeBuffers(0, 0, 0, newFormat, flags))) {
                AcquireBuffer();
                return false;
            }
            if (!ApplyColorSpace(colorSpace)) {
                swapchain->ResizeBuffers(0, 0, 0, bufferFormat, flags);
                ApplyColorSpace(desc.colorSpace);
                AcquireBuffer();
                return false;
            }
            bufferFormat = newFormat;
            format = SwapchainSrhiFormat(colorSpace);
            AcquireBuffer();
        }
        vsync = newVsync;
        return true;
    }

    void D3DSwapchain::GetDisplayInfo(srhiDisplayInfo& out)
    {
        ComPtr<IDXGIOutput> output;
        if (FAILED(swapchain->GetContainingOutput(&output)))
            return;
        ComPtr<IDXGIOutput6> output6;
        if (FAILED(output.As(&output6)))
            return;
        DXGI_OUTPUT_DESC1 od = {};
        if (FAILED(output6->GetDesc1(&od)))
            return;
        out.hdrSupported = od.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        out.minLuminance = od.MinLuminance;
        out.maxLuminance = od.MaxLuminance;
        out.maxFullFrameLuminance = od.MaxFullFrameLuminance;
    }

    srhiTexture* D3DSwapchain::Acquire()
    {
        if (IsIconic(hwnd) || !image->resource)
            return nullptr;
        return image;
    }

    srhiSwapchain* D3DDevice::CreateSwapchain(const srhiSwapchainDesc& d)
    {
        HWND hwnd = static_cast<HWND>(d.nativeWindow);
        if (!hwnd || !IsWindow(hwnd)) {
            SRHI_ERROR(this, "srhiSwapchainCreate: nativeWindow must be a valid HWND on D3D11");
            return nullptr;
        }
        D3DSwapchain* sc = new D3DSwapchain;
        sc->dev = this;
        sc->hwnd = hwnd;
        sc->label = d.label ? d.label : "swapchain";
        sc->tearing = tearingSupported;
        sc->vsync = d.vsync != 0;
        sc->flags = tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        sc->bufferFormat = SwapchainBufferFormat(d.colorSpace);

        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Width = d.width;
        sd.Height = d.height;
        sd.Format = sc->bufferFormat;
        sd.Stereo = FALSE;
        sd.SampleDesc.Count = 1;
        sd.SampleDesc.Quality = 0;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 3;
        sd.Scaling = DXGI_SCALING_STRETCH;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        sd.Flags = sc->flags;
        HRESULT hr = factory->CreateSwapChainForHwnd(device.Get(), hwnd, &sd, nullptr, nullptr, &sc->swapchain);
        if (FAILED(hr)) {
            SRHI_ERROR(this, "srhiSwapchainCreate: CreateSwapChainForHwnd failed (hr 0x%08x)", unsigned(hr));
            delete sc;
            return nullptr;
        }
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

        srhiColorSpace cs = d.colorSpace;
        if (cs != SRHI_COLOR_SPACE_SRGB && !sc->ApplyColorSpace(cs)) {
            SRHI_WARN(this, "srhiSwapchainCreate: requested HDR color space unsupported, falling back to sRGB");
            cs = SRHI_COLOR_SPACE_SRGB;
            sc->bufferFormat = SwapchainBufferFormat(cs);
            sc->swapchain->ResizeBuffers(0, 0, 0, sc->bufferFormat, sc->flags);
            sc->ApplyColorSpace(cs);
        }
        sc->format = SwapchainSrhiFormat(cs);

        sc->image = new D3DTexture;
        sc->image->device = this;
        sc->image->isSwapchainImage = true;
        sc->image->label = sc->label;
        sc->imageView = new D3DTextureView;
        sc->imageView->texture = sc->image;
        sc->imageView->label = sc->label;
        sc->image->defaultView = sc->imageView;
        sc->AcquireBuffer();
        return sc;
    }

    // =================================================================================================
    // Format support
    // =================================================================================================

    uint32_t D3DDevice::GetFormatSupport(srhiFormat format)
    {
        DXGI_FORMAT f = ToDxgi(format);
        if (f == DXGI_FORMAT_UNKNOWN)
            return 0;
        const FormatInfo& fi = GetFormatInfo(format);
        UINT support = 0;
        if (FAILED(device->CheckFormatSupport(f, &support)))
            return 0;
        uint32_t flags = 0;
        if (fi.depth) {
            UINT srvSupport = 0;
            device->CheckFormatSupport(SrvFormat(format), &srvSupport);
            if (support & D3D11_FORMAT_SUPPORT_DEPTH_STENCIL)
                flags |= SRHI_FORMAT_SUPPORT_DEPTH_STENCIL;
            if (srvSupport & (D3D11_FORMAT_SUPPORT_SHADER_LOAD | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE))
                flags |= SRHI_FORMAT_SUPPORT_SAMPLE;
            if (srvSupport & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE)
                flags |= SRHI_FORMAT_SUPPORT_FILTER;
            if (support & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET)
                flags |= SRHI_FORMAT_SUPPORT_MSAA;
            return flags;
        }
        if (support & (D3D11_FORMAT_SUPPORT_SHADER_LOAD | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE))
            flags |= SRHI_FORMAT_SUPPORT_SAMPLE;
        if (support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE)
            flags |= SRHI_FORMAT_SUPPORT_FILTER;
        if (support & D3D11_FORMAT_SUPPORT_RENDER_TARGET)
            flags |= SRHI_FORMAT_SUPPORT_RENDER_TARGET;
        if (support & D3D11_FORMAT_SUPPORT_BLENDABLE)
            flags |= SRHI_FORMAT_SUPPORT_BLEND;
        if (support & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET) {
            UINT quality = 0;
            if (SUCCEEDED(device->CheckMultisampleQualityLevels(f, 4, &quality)) && quality > 0)
                flags |= SRHI_FORMAT_SUPPORT_MSAA;
        }
        if (support & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE)
            flags |= SRHI_FORMAT_SUPPORT_RESOLVE;
        // Storage: the portable (WebGPU core) subset, where D3D11 supports typed UAV stores.
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
        case SRHI_FORMAT_RGBA32_UINT:
            if (support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW)
                flags |= SRHI_FORMAT_SUPPORT_STORAGE;
            break;
        default: break;
        }
        return flags;
    }
} // namespace

// =====================================================================================================
// Factory
// =====================================================================================================

srhiDevice* srhiCreateDeviceD3D11(const srhiDeviceDesc& desc)
{
    ComPtr<IDXGIFactory1> factory1;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory1)))) {
        srhi::Log(nullptr, SRHI_LOG_SEVERITY_ERROR, "D3D11: CreateDXGIFactory1 failed");
        return nullptr;
    }
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(factory1.As(&factory))) {
        srhi::Log(nullptr, SRHI_LOG_SEVERITY_ERROR, "D3D11: IDXGIFactory2 unavailable (Windows 8 or later required)");
        return nullptr;
    }

    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIFactory6> factory6;
    if (SUCCEEDED(factory.As(&factory6)))
        factory6->EnumAdapterByGpuPreference(0, desc.lowPower ? DXGI_GPU_PREFERENCE_MINIMUM_POWER : DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                             IID_PPV_ARGS(&adapter));
    if (!adapter)
        factory->EnumAdapters1(0, &adapter);
    if (!adapter) {
        srhi::Log(nullptr, SRHI_LOG_SEVERITY_ERROR, "D3D11: no adapter found");
        return nullptr;
    }

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (desc.debug)
        flags |= D3D11_CREATE_DEVICE_DEBUG;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    auto create = [&](UINT f, const D3D_FEATURE_LEVEL* l, UINT n) {
        return D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, f, l, n, D3D11_SDK_VERSION, &device, &level, &context);
    };
    HRESULT hr = create(flags, levels, 2);
    if (hr == E_INVALIDARG) // runtimes without 11_1 reject the array
        hr = create(flags, levels + 1, 1);
    if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG)) {
        srhi::Log(nullptr, SRHI_LOG_SEVERITY_WARNING, "D3D11: debug layer unavailable (install the Graphics Tools feature)");
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = create(flags, levels, 2);
        if (hr == E_INVALIDARG)
            hr = create(flags, levels + 1, 1);
    }
    if (FAILED(hr)) {
        srhi::Log(nullptr, SRHI_LOG_SEVERITY_ERROR, "D3D11: D3D11CreateDevice failed (hr 0x%08x)", unsigned(hr));
        return nullptr;
    }

    D3DDevice* d = new D3DDevice;
    d->desc = desc;
    d->factory = factory;
    d->adapter = adapter;
    if (FAILED(device.As(&d->device)) || FAILED(context.As(&d->ctx))) {
        srhi::Log(nullptr, SRHI_LOG_SEVERITY_ERROR, "D3D11: the D3D11.1 runtime is required");
        delete d;
        return nullptr;
    }
    context.As(&d->annotation);
    if (flags & D3D11_CREATE_DEVICE_DEBUG) {
        if (SUCCEEDED(device.As(&d->infoQueue))) {
            D3D11_MESSAGE_SEVERITY deny[] = {D3D11_MESSAGE_SEVERITY_INFO, D3D11_MESSAGE_SEVERITY_MESSAGE};
            D3D11_INFO_QUEUE_FILTER filter = {};
            filter.DenyList.NumSeverities = UINT(sizeof(deny) / sizeof(deny[0]));
            filter.DenyList.pSeverityList = deny;
            d->infoQueue->PushStorageFilter(&filter);
        }
    }

    D3D11_FEATURE_DATA_D3D11_OPTIONS options = {};
    d->device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options));
    if (!options.ConstantBufferOffsetting || !options.MapNoOverwriteOnDynamicConstantBuffer || !options.ConstantBufferPartialUpdate) {
        srhi::Log(nullptr, SRHI_LOG_SEVERITY_ERROR,
                  "D3D11: the driver lacks constant buffer offsetting / partial updates / no-overwrite maps (WDDM 1.2 required)");
        delete d;
        return nullptr;
    }

    ComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(factory.As(&factory5))) {
        BOOL allow = FALSE;
        if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))))
            d->tearingSupported = allow != FALSE;
    }

    D3D11_BUFFER_DESC rd = {};
    rd.ByteWidth = kUniformRingSize;
    rd.Usage = D3D11_USAGE_DYNAMIC;
    rd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    rd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(d->device->CreateBuffer(&rd, nullptr, &d->uniformRing))) {
        srhi::Log(nullptr, SRHI_LOG_SEVERITY_ERROR, "D3D11: failed to create the uniform ring");
        delete d;
        return nullptr;
    }
    SetName(d->uniformRing.Get(), "SimpleRHI uniform ring");

    DXGI_ADAPTER_DESC1 ad = {};
    adapter->GetDesc1(&ad);
    d->adapterName = ToUtf8(ad.Description);

    srhiDeviceInfo& info = d->info;
    info.backend = SRHI_BACKEND_D3D11;
    info.backendName = "D3D11";
    info.features.timestampQueries = 1;
    info.features.textureCompressionBC = 1;
    info.features.textureCompressionASTC = 0;
    info.features.textureCompressionASTCHDR = 0;
    info.features.fillModeWireframe = 1;
    info.features.depthClamp = 1;
    info.features.indirectFirstInstance = 1;
    info.features.cubeArrays = 1;

    srhiDeviceLimits& l = info.limits;
    l.maxTextureDimension2D = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;          // 16384
    l.maxTextureDimension3D = D3D11_REQ_TEXTURE3D_U_V_OR_W_DIMENSION;        // 2048
    l.maxTextureArrayLayers = D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION;      // 2048
    l.maxColorAttachments = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;          // 8
    l.maxComputeWorkgroupSizeX = D3D11_CS_THREAD_GROUP_MAX_X;                // 1024
    l.maxComputeWorkgroupSizeY = D3D11_CS_THREAD_GROUP_MAX_Y;                // 1024
    l.maxComputeWorkgroupSizeZ = D3D11_CS_THREAD_GROUP_MAX_Z;                // 64
    l.maxComputeInvocationsPerWorkgroup = D3D11_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP; // 1024
    l.maxComputeWorkgroupsPerDimension = D3D11_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION; // 65535
    l.maxStorageBuffersPerStage = kMaxUavSlots;
    l.maxStorageTexturesPerStage = kMaxUavSlots;
    l.maxSampledTexturesPerStage = kMaxSrvSlots;
    l.maxSamplersPerStage = kMaxSamplerSlots;
    l.uniformBufferOffsetAlignment = 256;
    l.storageBufferOffsetAlignment = 256;
    l.maxUniformBufferBindingSize = D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16; // 65536
    l.maxStorageBufferBindingSize = 1ull << 30;
    return d;
}
