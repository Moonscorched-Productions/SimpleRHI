/**
 * @ Author: Amélie Heinrich
 * @ Create Time: 2026-11-13 00:00:00
 * @ Copyright: Copyright (c) 2026 Amélie Heinrich. All rights reserved.
 */

// The C API: validation, default resolution, and forwarding to the backend objects.

#include "srhi_internal.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace srhi
{
    // clang-format off
    static const FormatInfo kFormats[SRHI_FORMAT_COUNT] = {
        //  name                  bytes bw bh depth  stencil srgb   compr  int    twin
        { "UNDEFINED",            0,  1, 1, false, false, false, false, false, SRHI_FORMAT_UNDEFINED },
        { "R8_UNORM",             1,  1, 1, false, false, false, false, false, SRHI_FORMAT_UNDEFINED },
        { "RG8_UNORM",            2,  1, 1, false, false, false, false, false, SRHI_FORMAT_UNDEFINED },
        { "RGBA8_UNORM",          4,  1, 1, false, false, false, false, false, SRHI_FORMAT_RGBA8_UNORM_SRGB },
        { "RGBA8_UNORM_SRGB",     4,  1, 1, false, false, true,  false, false, SRHI_FORMAT_RGBA8_UNORM },
        { "RGBA8_SNORM",          4,  1, 1, false, false, false, false, false, SRHI_FORMAT_UNDEFINED },
        { "RGBA8_UINT",           4,  1, 1, false, false, false, false, true,  SRHI_FORMAT_UNDEFINED },
        { "BGRA8_UNORM",          4,  1, 1, false, false, false, false, false, SRHI_FORMAT_BGRA8_UNORM_SRGB },
        { "BGRA8_UNORM_SRGB",     4,  1, 1, false, false, true,  false, false, SRHI_FORMAT_BGRA8_UNORM },
        { "R16_FLOAT",            2,  1, 1, false, false, false, false, false, SRHI_FORMAT_UNDEFINED },
        { "RG16_FLOAT",           4,  1, 1, false, false, false, false, false, SRHI_FORMAT_UNDEFINED },
        { "RGBA16_FLOAT",         8,  1, 1, false, false, false, false, false, SRHI_FORMAT_UNDEFINED },
        { "R16_UINT",             2,  1, 1, false, false, false, false, true,  SRHI_FORMAT_UNDEFINED },
        { "R32_FLOAT",            4,  1, 1, false, false, false, false, false, SRHI_FORMAT_UNDEFINED },
        { "R32_UINT",             4,  1, 1, false, false, false, false, true,  SRHI_FORMAT_UNDEFINED },
        { "R32_SINT",             4,  1, 1, false, false, false, false, true,  SRHI_FORMAT_UNDEFINED },
        { "RG32_FLOAT",           8,  1, 1, false, false, false, false, false, SRHI_FORMAT_UNDEFINED },
        { "RG32_UINT",            8,  1, 1, false, false, false, false, true,  SRHI_FORMAT_UNDEFINED },
        { "RGBA32_FLOAT",         16, 1, 1, false, false, false, false, false, SRHI_FORMAT_UNDEFINED },
        { "RGBA32_UINT",          16, 1, 1, false, false, false, false, true,  SRHI_FORMAT_UNDEFINED },
        { "RGB10A2_UNORM",        4,  1, 1, false, false, false, false, false, SRHI_FORMAT_UNDEFINED },
        { "RG11B10_FLOAT",        4,  1, 1, false, false, false, false, false, SRHI_FORMAT_UNDEFINED },
        { "D16_UNORM",            2,  1, 1, true,  false, false, false, false, SRHI_FORMAT_UNDEFINED },
        { "D32_FLOAT",            4,  1, 1, true,  false, false, false, false, SRHI_FORMAT_UNDEFINED },
        { "D32_FLOAT_S8_UINT",    8,  1, 1, true,  true,  false, false, false, SRHI_FORMAT_UNDEFINED },
        { "BC1_UNORM",            8,  4, 4, false, false, false, true,  false, SRHI_FORMAT_BC1_UNORM_SRGB },
        { "BC1_UNORM_SRGB",       8,  4, 4, false, false, true,  true,  false, SRHI_FORMAT_BC1_UNORM },
        { "BC3_UNORM",            16, 4, 4, false, false, false, true,  false, SRHI_FORMAT_BC3_UNORM_SRGB },
        { "BC3_UNORM_SRGB",       16, 4, 4, false, false, true,  true,  false, SRHI_FORMAT_BC3_UNORM },
        { "BC4_UNORM",            8,  4, 4, false, false, false, true,  false, SRHI_FORMAT_UNDEFINED },
        { "BC5_UNORM",            16, 4, 4, false, false, false, true,  false, SRHI_FORMAT_UNDEFINED },
        { "BC6H_UFLOAT",          16, 4, 4, false, false, false, true,  false, SRHI_FORMAT_UNDEFINED },
        { "BC7_UNORM",            16, 4, 4, false, false, false, true,  false, SRHI_FORMAT_BC7_UNORM_SRGB },
        { "BC7_UNORM_SRGB",       16, 4, 4, false, false, true,  true,  false, SRHI_FORMAT_BC7_UNORM },
        { "ASTC_4x4_UNORM",       16, 4, 4, false, false, false, true,  false, SRHI_FORMAT_ASTC_4x4_UNORM_SRGB },
        { "ASTC_4x4_UNORM_SRGB",  16, 4, 4, false, false, true,  true,  false, SRHI_FORMAT_ASTC_4x4_UNORM },
        { "ASTC_4x4_FLOAT",       16, 4, 4, false, false, false, true,  false, SRHI_FORMAT_UNDEFINED },
    };
    // clang-format on

    const FormatInfo& GetFormatInfo(srhiFormat format)
    {
        return kFormats[uint32_t(format) < SRHI_FORMAT_COUNT ? format : SRHI_FORMAT_UNDEFINED];
    }

    bool FormatsViewCompatible(srhiFormat a, srhiFormat b) { return a == b || GetFormatInfo(a).srgbTwin == b; }

    uint32_t FullMipCount(uint32_t width, uint32_t height, uint32_t depth)
    {
        uint32_t m = std::max(width, std::max(height, depth)), count = 1;
        while (m > 1) {
            m >>= 1;
            ++count;
        }
        return count;
    }

    uint32_t RowBytes(srhiFormat format, uint32_t width)
    {
        const FormatInfo& f = GetFormatInfo(format);
        return (width + f.blockWidth - 1) / f.blockWidth * f.blockBytes;
    }

    uint32_t RowCount(srhiFormat format, uint32_t height)
    {
        const FormatInfo& f = GetFormatInfo(format);
        return (height + f.blockHeight - 1) / f.blockHeight;
    }

    uint32_t VertexFormatSize(srhiVertexFormat format)
    {
        switch (format) {
        case SRHI_VERTEX_FORMAT_FLOAT: return 4;
        case SRHI_VERTEX_FORMAT_FLOAT2: return 8;
        case SRHI_VERTEX_FORMAT_FLOAT3: return 12;
        case SRHI_VERTEX_FORMAT_FLOAT4: return 16;
        case SRHI_VERTEX_FORMAT_HALF2: return 4;
        case SRHI_VERTEX_FORMAT_HALF4: return 8;
        case SRHI_VERTEX_FORMAT_UINT: return 4;
        case SRHI_VERTEX_FORMAT_UINT2: return 8;
        case SRHI_VERTEX_FORMAT_UINT4: return 16;
        case SRHI_VERTEX_FORMAT_SINT: return 4;
        case SRHI_VERTEX_FORMAT_UBYTE4: return 4;
        case SRHI_VERTEX_FORMAT_UBYTE4_NORM: return 4;
        case SRHI_VERTEX_FORMAT_SHORT2_NORM: return 4;
        case SRHI_VERTEX_FORMAT_SHORT4_NORM: return 8;
        }
        return 0;
    }

    void Log(const srhiDevice* device, srhiLogSeverity severity, const char* fmt, ...)
    {
        char buffer[2048];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);
        if (device && device->desc.log)
            device->desc.log(severity, buffer, device->desc.logUserData);
        else if (severity != SRHI_LOG_SEVERITY_INFO)
            fprintf(stderr, "[SimpleRHI] %s: %s\n", severity == SRHI_LOG_SEVERITY_ERROR ? "error" : "warning", buffer);
    }

    static const char* Str(const char* s) { return s ? s : ""; }
} // namespace srhi

using namespace srhi;

#define SRHI_CHECK(device, cond, ret, ...)                                                                             \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            SRHI_ERROR(device, __VA_ARGS__);                                                                           \
            return ret;                                                                                                \
        }                                                                                                              \
    } while (0)

// =====================================================================================================
// Device
// =====================================================================================================

extern "C" srhiDevice* srhiDeviceCreate(const srhiDeviceDesc* desc)
{
    srhiDeviceDesc d = desc ? *desc : srhiDeviceDesc{};
    if (d.backend == SRHI_BACKEND_AUTO) {
#if defined(__EMSCRIPTEN__)
        d.backend = SRHI_BACKEND_WEBGPU;
#elif defined(_WIN32)
        d.backend = SRHI_BACKEND_D3D11;
#elif defined(__APPLE__)
        d.backend = SRHI_BACKEND_METAL;
#else
        d.backend = SRHI_BACKEND_WEBGPU;
#endif
    }

    srhiDevice* device = nullptr;
    switch (d.backend) {
    case SRHI_BACKEND_METAL:
#ifdef SRHI_HAS_METAL
        device = srhiCreateDeviceMetal(d);
#endif
        break;
    case SRHI_BACKEND_D3D11:
#ifdef SRHI_HAS_D3D11
        device = srhiCreateDeviceD3D11(d);
#endif
        break;
    case SRHI_BACKEND_WEBGPU:
#ifdef SRHI_HAS_WEBGPU
        device = srhiCreateDeviceWebGPU(d);
#endif
        break;
    default: break;
    }
    if (!device) {
        if (d.log)
            d.log(SRHI_LOG_SEVERITY_ERROR, "srhiDeviceCreate: the requested backend is unavailable", d.logUserData);
        else
            fprintf(stderr, "[SimpleRHI] error: srhiDeviceCreate: the requested backend is unavailable\n");
        return nullptr;
    }
    device->info.adapterName = device->adapterName.c_str();
    return device;
}

extern "C" void srhiDeviceDestroy(srhiDevice* device)
{
    if (!device)
        return;
    device->WaitIdle();
    if (device->liveObjects != 0)
        SRHI_WARN(device, "srhiDeviceDestroy: %lld object(s) were not destroyed", (long long)device->liveObjects);
    delete device;
}

extern "C" void srhiDeviceGetInfo(const srhiDevice* device, srhiDeviceInfo* outInfo)
{
    if (device && outInfo)
        *outInfo = device->info;
}

extern "C" uint32_t srhiDeviceGetFormatSupport(const srhiDevice* device, srhiFormat format)
{
    if (!device || format <= SRHI_FORMAT_UNDEFINED || format >= SRHI_FORMAT_COUNT)
        return 0;
    return const_cast<srhiDevice*>(device)->GetFormatSupport(format);
}

extern "C" void srhiDevicePoll(srhiDevice* device)
{
    if (device)
        device->Poll();
}

extern "C" void srhiDeviceWaitIdle(srhiDevice* device)
{
    if (device)
        device->WaitIdle();
}

extern "C" void* srhiDeviceGetNative(srhiDevice* device) { return device ? device->GetNative() : nullptr; }
extern "C" void* srhiDeviceGetNativeQueue(srhiDevice* device) { return device ? device->GetNativeQueue() : nullptr; }

// =====================================================================================================
// Buffers
// =====================================================================================================

extern "C" srhiBuffer* srhiBufferCreate(srhiDevice* device, const srhiBufferDesc* desc)
{
    if (!device || !desc)
        return nullptr;
    SRHI_CHECK(device, desc->size > 0, nullptr, "srhiBufferCreate(%s): size is 0", Str(desc->label));
    SRHI_CHECK(device, desc->usage != 0, nullptr, "srhiBufferCreate(%s): usage is 0", Str(desc->label));
    if (desc->usage & (SRHI_BUFFER_USAGE_STORAGE | SRHI_BUFFER_USAGE_STORAGE_RW))
        SRHI_CHECK(device, desc->structureStride > 0 && desc->structureStride % 4 == 0 && desc->size % desc->structureStride == 0,
                   nullptr, "srhiBufferCreate(%s): storage buffers need a structureStride (multiple of 4) dividing size",
                   Str(desc->label));

    srhiBufferDesc d = *desc;
    d.size = AlignUp(d.size, 4);
    srhiBuffer* buffer = device->CreateBuffer(d);
    if (!buffer)
        return nullptr;
    buffer->device = device;
    buffer->label = Str(desc->label);
    buffer->desc = d;
    buffer->desc.label = buffer->label.c_str();
    ++device->liveObjects;
    return buffer;
}

extern "C" void srhiBufferDestroy(srhiBuffer* buffer)
{
    if (!buffer)
        return;
    --buffer->device->liveObjects;
    delete buffer;
}

extern "C" void srhiBufferGetDesc(const srhiBuffer* buffer, srhiBufferDesc* outDesc)
{
    if (buffer && outDesc)
        *outDesc = buffer->desc;
}

extern "C" void srhiBufferWrite(srhiDevice* device, srhiBuffer* buffer, uint64_t offset, const void* data, uint64_t size)
{
    if (!device || !buffer || !data || size == 0)
        return;
    SRHI_CHECK(device, offset % 4 == 0 && size % 4 == 0, , "srhiBufferWrite(%s): offset and size must be multiples of 4",
               buffer->label.c_str());
    SRHI_CHECK(device, offset + size <= buffer->desc.size, , "srhiBufferWrite(%s): range [%llu, %llu) exceeds size %llu",
               buffer->label.c_str(), (unsigned long long)offset, (unsigned long long)(offset + size),
               (unsigned long long)buffer->desc.size);
    device->WriteBuffer(buffer, offset, data, size);
}

extern "C" void* srhiBufferGetNative(srhiBuffer* buffer) { return buffer ? buffer->GetNative() : nullptr; }

// =====================================================================================================
// Textures
// =====================================================================================================

static bool ResolveViewDesc(srhiTexture* texture, const srhiTextureViewDesc& in, srhiTextureViewDesc& out)
{
    srhiDevice* device = texture->device;
    const srhiTextureDesc& t = texture->desc;
    out = in;
    if (out.format == SRHI_FORMAT_UNDEFINED)
        out.format = t.format;
    SRHI_CHECK(device, FormatsViewCompatible(out.format, t.format), false,
               "srhiTextureViewCreate(%s): format %s is not view-compatible with %s", texture->label.c_str(),
               GetFormatInfo(out.format).name, GetFormatInfo(t.format).name);
    SRHI_CHECK(device, out.baseMip < t.mipLevels, false, "srhiTextureViewCreate(%s): baseMip out of range", texture->label.c_str());
    if (out.mipCount == 0 || out.mipCount == SRHI_ALL_MIPS)
        out.mipCount = t.mipLevels - out.baseMip;
    SRHI_CHECK(device, out.baseMip + out.mipCount <= t.mipLevels, false, "srhiTextureViewCreate(%s): mip range out of range",
               texture->label.c_str());

    uint32_t layers = texture->LayerCount();
    SRHI_CHECK(device, out.baseLayer < layers, false, "srhiTextureViewCreate(%s): baseLayer out of range", texture->label.c_str());
    if (out.layerCount == 0 || out.layerCount == SRHI_ALL_LAYERS)
        out.layerCount = layers - out.baseLayer;
    SRHI_CHECK(device, out.baseLayer + out.layerCount <= layers, false, "srhiTextureViewCreate(%s): layer range out of range",
               texture->label.c_str());

    bool ok = false;
    switch (out.type) {
    case SRHI_TEXTURE_TYPE_2D: ok = t.type != SRHI_TEXTURE_TYPE_3D && out.layerCount == 1; break;
    case SRHI_TEXTURE_TYPE_2D_ARRAY: ok = t.type != SRHI_TEXTURE_TYPE_3D; break;
    case SRHI_TEXTURE_TYPE_CUBE:
        ok = (t.type == SRHI_TEXTURE_TYPE_CUBE || t.type == SRHI_TEXTURE_TYPE_CUBE_ARRAY) && out.layerCount == 6 &&
             out.baseLayer % 6 == 0;
        break;
    case SRHI_TEXTURE_TYPE_CUBE_ARRAY:
        ok = t.type == SRHI_TEXTURE_TYPE_CUBE_ARRAY && out.layerCount % 6 == 0 && out.baseLayer % 6 == 0;
        break;
    case SRHI_TEXTURE_TYPE_3D: ok = t.type == SRHI_TEXTURE_TYPE_3D; break;
    }
    SRHI_CHECK(device, ok, false, "srhiTextureViewCreate(%s): view type %d incompatible with the texture type %d / layer range",
               texture->label.c_str(), int(out.type), int(t.type));
    return true;
}

static srhiTextureView* CreateViewInternal(srhiTexture* texture, const srhiTextureViewDesc& resolved, const char* label)
{
    srhiTextureView* view = texture->device->CreateTextureView(texture, resolved);
    if (!view)
        return nullptr;
    view->texture = texture;
    view->label = Str(label);
    view->desc = resolved;
    view->desc.label = view->label.c_str();
    return view;
}

extern "C" srhiTexture* srhiTextureCreate(srhiDevice* device, const srhiTextureDesc* desc)
{
    if (!device || !desc)
        return nullptr;
    srhiTextureDesc d = *desc;
    const char* label = Str(desc->label);
    const FormatInfo& fi = GetFormatInfo(d.format);
    SRHI_CHECK(device, d.format > SRHI_FORMAT_UNDEFINED && d.format < SRHI_FORMAT_COUNT, nullptr,
               "srhiTextureCreate(%s): invalid format", label);
    SRHI_CHECK(device, d.width > 0 && d.height > 0, nullptr, "srhiTextureCreate(%s): zero size", label);
    SRHI_CHECK(device, d.usage != 0, nullptr, "srhiTextureCreate(%s): usage is 0", label);
    SRHI_CHECK(device, device->GetFormatSupport(d.format) & SRHI_FORMAT_SUPPORT_SAMPLE ||
                           device->GetFormatSupport(d.format) & SRHI_FORMAT_SUPPORT_DEPTH_STENCIL,
               nullptr, "srhiTextureCreate(%s): format %s is not supported by this device", label, fi.name);

    switch (d.type) {
    case SRHI_TEXTURE_TYPE_2D: d.depthOrLayers = 1; break;
    case SRHI_TEXTURE_TYPE_CUBE: d.depthOrLayers = 6; break;
    case SRHI_TEXTURE_TYPE_CUBE_ARRAY:
        SRHI_CHECK(device, d.depthOrLayers > 0 && d.depthOrLayers % 6 == 0, nullptr,
                   "srhiTextureCreate(%s): cube arrays need depthOrLayers = 6 * cube count", label);
        SRHI_CHECK(device, device->info.features.cubeArrays, nullptr, "srhiTextureCreate(%s): cube arrays unsupported", label);
        break;
    default:
        if (d.depthOrLayers == 0)
            d.depthOrLayers = 1;
        break;
    }
    if (d.type == SRHI_TEXTURE_TYPE_CUBE || d.type == SRHI_TEXTURE_TYPE_CUBE_ARRAY)
        SRHI_CHECK(device, d.width == d.height, nullptr, "srhiTextureCreate(%s): cube faces must be square", label);

    uint32_t full = FullMipCount(d.width, d.height, d.type == SRHI_TEXTURE_TYPE_3D ? d.depthOrLayers : 1);
    if (d.mipLevels == 0)
        d.mipLevels = 1;
    if (d.mipLevels == SRHI_ALL_MIPS)
        d.mipLevels = full;
    SRHI_CHECK(device, d.mipLevels <= full, nullptr, "srhiTextureCreate(%s): too many mips", label);

    if (d.sampleCount == 0)
        d.sampleCount = 1;
    SRHI_CHECK(device, d.sampleCount == 1 || d.sampleCount == 4, nullptr, "srhiTextureCreate(%s): sampleCount must be 1 or 4", label);
    if (d.sampleCount > 1) {
        SRHI_CHECK(device, d.type == SRHI_TEXTURE_TYPE_2D && d.mipLevels == 1, nullptr,
                   "srhiTextureCreate(%s): MSAA textures must be 2D with one mip", label);
        SRHI_CHECK(device, d.usage & SRHI_TEXTURE_USAGE_RENDER_TARGET, nullptr,
                   "srhiTextureCreate(%s): MSAA textures need RENDER_TARGET usage", label);
        SRHI_CHECK(device, !(d.usage & SRHI_TEXTURE_USAGE_STORAGE), nullptr,
                   "srhiTextureCreate(%s): MSAA textures cannot be storage textures", label);
    }
    if (fi.compressed) {
        SRHI_CHECK(device, !(d.usage & (SRHI_TEXTURE_USAGE_RENDER_TARGET | SRHI_TEXTURE_USAGE_STORAGE)), nullptr,
                   "srhiTextureCreate(%s): compressed textures can only be sampled and copied", label);
        SRHI_CHECK(device, d.width % fi.blockWidth == 0 && d.height % fi.blockHeight == 0, nullptr,
                   "srhiTextureCreate(%s): compressed texture size must be a multiple of the block size", label);
    }
    if (d.usage & SRHI_TEXTURE_USAGE_RENDER_TARGET)
        SRHI_CHECK(device, device->GetFormatSupport(d.format) & (SRHI_FORMAT_SUPPORT_RENDER_TARGET | SRHI_FORMAT_SUPPORT_DEPTH_STENCIL),
                   nullptr, "srhiTextureCreate(%s): format %s is not renderable", label, fi.name);
    if (d.usage & SRHI_TEXTURE_USAGE_STORAGE)
        SRHI_CHECK(device, device->GetFormatSupport(d.format) & SRHI_FORMAT_SUPPORT_STORAGE, nullptr,
                   "srhiTextureCreate(%s): format %s cannot be a storage texture", label, fi.name);

    srhiTexture* texture = device->CreateTexture(d);
    if (!texture)
        return nullptr;
    texture->device = device;
    texture->label = label;
    texture->desc = d;
    texture->desc.label = texture->label.c_str();

    srhiTextureViewDesc vd = {};
    vd.type = d.type;
    srhiTextureViewDesc resolved;
    ResolveViewDesc(texture, vd, resolved);
    texture->defaultView = CreateViewInternal(texture, resolved, label);
    if (!texture->defaultView) {
        delete texture;
        return nullptr;
    }
    ++device->liveObjects;
    return texture;
}

extern "C" void srhiTextureDestroy(srhiTexture* texture)
{
    if (!texture)
        return;
    if (texture->isSwapchainImage) {
        SRHI_ERROR(texture->device, "srhiTextureDestroy: swapchain textures are owned by the swapchain");
        return;
    }
    --texture->device->liveObjects;
    delete texture->defaultView;
    delete texture;
}

extern "C" void srhiTextureGetDesc(const srhiTexture* texture, srhiTextureDesc* outDesc)
{
    if (texture && outDesc)
        *outDesc = texture->desc;
}

extern "C" void srhiTextureWrite(srhiDevice* device, srhiTexture* texture, uint32_t mip, uint32_t layer,
                                 const srhiTextureRegion* region, const void* data, uint32_t bytesPerRow)
{
    if (!device || !texture || !data)
        return;
    const srhiTextureDesc& t = texture->desc;
    const char* label = texture->label.c_str();
    const FormatInfo& fi = GetFormatInfo(t.format);
    SRHI_CHECK(device, mip < t.mipLevels && layer < texture->LayerCount(), , "srhiTextureWrite(%s): mip/layer out of range", label);
    SRHI_CHECK(device, t.sampleCount == 1 && !fi.depth, , "srhiTextureWrite(%s): cannot write MSAA or depth textures", label);

    uint32_t mw = MipDim(t.width, mip), mh = MipDim(t.height, mip), md = texture->Depth(mip);
    srhiTextureRegion r = region ? *region : srhiTextureRegion{0, 0, 0, mw, mh, md};
    if (r.depth == 0)
        r.depth = 1;
    SRHI_CHECK(device, r.width > 0 && r.height > 0 && r.x + r.width <= mw && r.y + r.height <= mh && r.z + r.depth <= md, ,
               "srhiTextureWrite(%s): region out of bounds", label);
    if (fi.compressed) {
        bool aligned = r.x % fi.blockWidth == 0 && r.y % fi.blockHeight == 0 &&
                       (r.width % fi.blockWidth == 0 || r.x + r.width == mw) && (r.height % fi.blockHeight == 0 || r.y + r.height == mh);
        SRHI_CHECK(device, aligned, , "srhiTextureWrite(%s): compressed regions must be block aligned", label);
    }
    uint32_t tight = RowBytes(t.format, r.width);
    if (bytesPerRow == 0)
        bytesPerRow = tight;
    SRHI_CHECK(device, bytesPerRow >= tight, , "srhiTextureWrite(%s): bytesPerRow %u < %u", label, bytesPerRow, tight);
    device->WriteTexture(texture, mip, layer, r, data, bytesPerRow, RowCount(t.format, r.height));
}

extern "C" srhiTextureView* srhiTextureViewCreate(srhiTexture* texture, const srhiTextureViewDesc* desc)
{
    if (!texture || !desc)
        return nullptr;
    srhiTextureViewDesc resolved;
    if (!ResolveViewDesc(texture, *desc, resolved))
        return nullptr;
    srhiTextureView* view = CreateViewInternal(texture, resolved, desc->label);
    if (view)
        ++texture->device->liveObjects;
    return view;
}

extern "C" void srhiTextureViewDestroy(srhiTextureView* view)
{
    if (!view)
        return;
    if (view == view->texture->defaultView) {
        SRHI_ERROR(view->texture->device, "srhiTextureViewDestroy: the default view is owned by its texture");
        return;
    }
    --view->texture->device->liveObjects;
    delete view;
}

extern "C" srhiTextureView* srhiTextureGetDefaultView(srhiTexture* texture) { return texture ? texture->defaultView : nullptr; }
extern "C" void* srhiTextureGetNative(srhiTexture* texture) { return texture ? texture->GetNative() : nullptr; }

// =====================================================================================================
// Samplers
// =====================================================================================================

extern "C" srhiSampler* srhiSamplerCreate(srhiDevice* device, const srhiSamplerDesc* desc)
{
    if (!device || !desc)
        return nullptr;
    srhiSamplerDesc d = *desc;
    if (d.maxAnisotropy == 0)
        d.maxAnisotropy = 1;
    if (d.lodMaxClamp == 0.0f)
        d.lodMaxClamp = 1000.0f;
    if (d.maxAnisotropy > 16)
        d.maxAnisotropy = 16;
    if (d.maxAnisotropy > 1)
        SRHI_CHECK(device, d.minFilter == SRHI_FILTER_LINEAR && d.magFilter == SRHI_FILTER_LINEAR && d.mipFilter == SRHI_FILTER_LINEAR,
                   nullptr, "srhiSamplerCreate(%s): anisotropic filtering requires linear filters", Str(desc->label));
    srhiSampler* sampler = device->CreateSampler(d);
    if (!sampler)
        return nullptr;
    sampler->device = device;
    sampler->label = Str(desc->label);
    sampler->desc = d;
    sampler->desc.label = sampler->label.c_str();
    ++device->liveObjects;
    return sampler;
}

extern "C" void srhiSamplerDestroy(srhiSampler* sampler)
{
    if (!sampler)
        return;
    --sampler->device->liveObjects;
    delete sampler;
}

// =====================================================================================================
// Shader modules
// =====================================================================================================

extern "C" srhiShaderModule* srhiShaderModuleCreate(srhiDevice* device, const void* data, size_t size, const char* label)
{
    if (!device || !data)
        return nullptr;
    Package package;
    SRHI_CHECK(device, ReadPackage(data, size, package), nullptr,
               "srhiShaderModuleCreate(%s): not a valid shader package (recompile it with this version of srhic)", Str(label));
    srhiShaderModule* module = device->CreateShaderModule(std::move(package), label);
    if (!module)
        return nullptr;
    module->device = device;
    module->label = Str(label);
    ++device->liveObjects;
    return module;
}

extern "C" void srhiShaderModuleDestroy(srhiShaderModule* module)
{
    if (!module)
        return;
    --module->device->liveObjects;
    delete module;
}

extern "C" srhiBool srhiShaderModuleHasEntryPoint(const srhiShaderModule* module, const char* entryPoint)
{
    return module && module->package.FindEntry(entryPoint) != nullptr;
}

extern "C" uint32_t srhiShaderModuleGetResourceSetLayout(const srhiShaderModule* module, uint32_t set,
                                                         srhiResourceSetLayoutEntry* outEntries, uint32_t capacity)
{
    if (!module)
        return 0;
    std::vector<srhiResourceSetLayoutEntry> entries;
    for (uint32_t i = 0; i < module->package.bindings.size(); ++i) {
        const PkgBinding& b = module->package.bindings[i];
        if (b.set != set)
            continue;
        srhiResourceSetLayoutEntry e = {};
        e.binding = b.binding;
        e.type = b.type;
        for (const PkgEntry& entry : module->package.entries)
            if (std::find(entry.usedBindings.begin(), entry.usedBindings.end(), i) != entry.usedBindings.end())
                e.stages |= entry.stage;
        if (e.stages == 0)
            e.stages = SRHI_SHADER_STAGE_ALL;
        e.textureType = b.textureType;
        e.sampleType = b.sampleType;
        e.multisampled = b.multisampled;
        e.storageFormat = b.storageFormat;
        e.samplerType = b.samplerType;
        entries.push_back(e);
    }
    std::sort(entries.begin(), entries.end(), [](auto& a, auto& b) { return a.binding < b.binding; });
    if (outEntries)
        for (uint32_t i = 0; i < std::min<uint32_t>(capacity, uint32_t(entries.size())); ++i)
            outEntries[i] = entries[i];
    return uint32_t(entries.size());
}

// =====================================================================================================
// Resource sets
// =====================================================================================================

extern "C" srhiResourceSetLayout* srhiResourceSetLayoutCreate(srhiDevice* device, const srhiResourceSetLayoutDesc* desc)
{
    if (!device || !desc)
        return nullptr;
    const char* label = Str(desc->label);
    std::vector<srhiResourceSetLayoutEntry> entries(desc->entries, desc->entries + desc->entryCount);
    std::sort(entries.begin(), entries.end(), [](auto& a, auto& b) { return a.binding < b.binding; });
    for (size_t i = 0; i < entries.size(); ++i) {
        srhiResourceSetLayoutEntry& e = entries[i];
        SRHI_CHECK(device, i == 0 || entries[i - 1].binding != e.binding, nullptr,
                   "srhiResourceSetLayoutCreate(%s): binding %u declared twice", label, e.binding);
        if (e.stages == 0)
            e.stages = SRHI_SHADER_STAGE_ALL;
        if (e.type == SRHI_BINDING_TYPE_STORAGE_BUFFER_RW || e.type == SRHI_BINDING_TYPE_STORAGE_TEXTURE)
            SRHI_CHECK(device, !(e.stages & SRHI_SHADER_STAGE_VERTEX), nullptr,
                       "srhiResourceSetLayoutCreate(%s): binding %u: writable resources are not allowed in vertex shaders",
                       label, e.binding);
        if (e.type == SRHI_BINDING_TYPE_STORAGE_TEXTURE)
            SRHI_CHECK(device, e.storageFormat != SRHI_FORMAT_UNDEFINED, nullptr,
                       "srhiResourceSetLayoutCreate(%s): binding %u: storage textures need storageFormat", label, e.binding);
    }
    srhiResourceSetLayout* layout = device->CreateResourceSetLayout(std::move(entries), label);
    if (!layout)
        return nullptr;
    layout->device = device;
    layout->label = label;
    ++device->liveObjects;
    return layout;
}

extern "C" void srhiResourceSetLayoutDestroy(srhiResourceSetLayout* layout)
{
    if (!layout)
        return;
    --layout->device->liveObjects;
    delete layout;
}

extern "C" srhiResourceSet* srhiResourceSetCreate(srhiDevice* device, const srhiResourceSetDesc* desc)
{
    if (!device || !desc || !desc->layout)
        return nullptr;
    const char* label = Str(desc->label);
    srhiResourceSetLayout* layout = desc->layout;
    std::vector<srhiResourceBinding> bindings(layout->entries.size());

    for (size_t i = 0; i < layout->entries.size(); ++i) {
        const srhiResourceSetLayoutEntry& e = layout->entries[i];
        const srhiResourceBinding* found = nullptr;
        for (uint32_t j = 0; j < desc->bindingCount; ++j)
            if (desc->bindings[j].binding == e.binding)
                found = &desc->bindings[j];
        SRHI_CHECK(device, found, nullptr, "srhiResourceSetCreate(%s): binding %u is not bound", label, e.binding);
        srhiResourceBinding b = *found;

        switch (e.type) {
        case SRHI_BINDING_TYPE_UNIFORM_BUFFER:
        case SRHI_BINDING_TYPE_STORAGE_BUFFER:
        case SRHI_BINDING_TYPE_STORAGE_BUFFER_RW: {
            SRHI_CHECK(device, b.buffer, nullptr, "srhiResourceSetCreate(%s): binding %u needs a buffer", label, e.binding);
            uint32_t need = e.type == SRHI_BINDING_TYPE_UNIFORM_BUFFER ? SRHI_BUFFER_USAGE_UNIFORM
                          : e.type == SRHI_BINDING_TYPE_STORAGE_BUFFER ? (SRHI_BUFFER_USAGE_STORAGE | SRHI_BUFFER_USAGE_STORAGE_RW)
                                                                       : SRHI_BUFFER_USAGE_STORAGE_RW;
            SRHI_CHECK(device, b.buffer->desc.usage & need, nullptr,
                       "srhiResourceSetCreate(%s): binding %u: buffer '%s' lacks the required usage", label, e.binding,
                       b.buffer->label.c_str());
            uint32_t align = e.type == SRHI_BINDING_TYPE_UNIFORM_BUFFER ? device->info.limits.uniformBufferOffsetAlignment
                                                                         : device->info.limits.storageBufferOffsetAlignment;
            SRHI_CHECK(device, b.offset % align == 0, nullptr,
                       "srhiResourceSetCreate(%s): binding %u: offset must be a multiple of %u", label, e.binding, align);
            SRHI_CHECK(device, b.offset < b.buffer->desc.size, nullptr, "srhiResourceSetCreate(%s): binding %u: offset out of range",
                       label, e.binding);
            if (b.size == 0 || b.size == SRHI_WHOLE_SIZE)
                b.size = b.buffer->desc.size - b.offset;
            SRHI_CHECK(device, b.offset + b.size <= b.buffer->desc.size, nullptr,
                       "srhiResourceSetCreate(%s): binding %u: range out of bounds", label, e.binding);
            if (e.type != SRHI_BINDING_TYPE_UNIFORM_BUFFER)
                SRHI_CHECK(device, b.size % b.buffer->desc.structureStride == 0 && b.offset % b.buffer->desc.structureStride == 0,
                           nullptr, "srhiResourceSetCreate(%s): binding %u: storage range must be whole elements", label, e.binding);
            break;
        }
        case SRHI_BINDING_TYPE_TEXTURE:
        case SRHI_BINDING_TYPE_STORAGE_TEXTURE: {
            SRHI_CHECK(device, b.textureView, nullptr, "srhiResourceSetCreate(%s): binding %u needs a texture view", label, e.binding);
            srhiTexture* tex = b.textureView->texture;
            uint32_t need = e.type == SRHI_BINDING_TYPE_TEXTURE ? SRHI_TEXTURE_USAGE_SAMPLED : SRHI_TEXTURE_USAGE_STORAGE;
            SRHI_CHECK(device, tex->desc.usage & need, nullptr,
                       "srhiResourceSetCreate(%s): binding %u: texture '%s' lacks the required usage", label, e.binding,
                       tex->label.c_str());
            SRHI_CHECK(device, b.textureView->desc.type == e.textureType, nullptr,
                       "srhiResourceSetCreate(%s): binding %u: view type %d does not match the layout's %d", label, e.binding,
                       int(b.textureView->desc.type), int(e.textureType));
            if (e.type == SRHI_BINDING_TYPE_STORAGE_TEXTURE) {
                SRHI_CHECK(device, b.textureView->desc.mipCount == 1, nullptr,
                           "srhiResourceSetCreate(%s): binding %u: storage texture views must cover one mip", label, e.binding);
                SRHI_CHECK(device, b.textureView->desc.format == e.storageFormat, nullptr,
                           "srhiResourceSetCreate(%s): binding %u: view format %s != layout storage format %s", label,
                           e.binding, GetFormatInfo(b.textureView->desc.format).name, GetFormatInfo(e.storageFormat).name);
            } else {
                SRHI_CHECK(device, (tex->desc.sampleCount > 1) == (e.multisampled != 0), nullptr,
                           "srhiResourceSetCreate(%s): binding %u: multisampled mismatch", label, e.binding);
            }
            break;
        }
        case SRHI_BINDING_TYPE_SAMPLER:
            SRHI_CHECK(device, b.sampler, nullptr, "srhiResourceSetCreate(%s): binding %u needs a sampler", label, e.binding);
            SRHI_CHECK(device, (b.sampler->desc.compareEnable != 0) == (e.samplerType == SRHI_SAMPLER_BINDING_TYPE_COMPARISON),
                       nullptr, "srhiResourceSetCreate(%s): binding %u: comparison sampler mismatch", label, e.binding);
            break;
        }
        bindings[i] = b;
    }

    srhiResourceSet* set = device->CreateResourceSet(layout, std::move(bindings), label);
    if (!set)
        return nullptr;
    set->device = device;
    set->layout = layout;
    set->label = label;
    ++device->liveObjects;
    return set;
}

extern "C" void srhiResourceSetDestroy(srhiResourceSet* set)
{
    if (!set)
        return;
    --set->device->liveObjects;
    delete set;
}

// =====================================================================================================
// Pipelines
// =====================================================================================================

/// @brief Matches the package bindings used by the given entry points against the pipeline's set layouts.
static bool BuildPipelineLayout(srhiDevice* device, const char* label, const Package& pkg, const PkgEntry* const* entries,
                                uint32_t entryCount, srhiResourceSetLayout* const* setLayouts, PipelineLayoutInfo& out)
{
    for (uint32_t s = 0; s < SRHI_MAX_RESOURCE_SETS; ++s) {
        out.setLayouts[s] = setLayouts[s];
        if (setLayouts[s])
            out.slots[s].resize(setLayouts[s]->entries.size());
    }

    for (uint32_t ei = 0; ei < entryCount; ++ei) {
        const PkgEntry* entry = entries[ei];
        for (uint32_t index : entry->usedBindings) {
            const PkgBinding& b = pkg.bindings[index];
            BindingSlot* slot = nullptr;
            if (b.set == SRHI_UNIFORM_SET_INDEX) {
                slot = &out.uniforms[b.binding];
                out.uniformSizes[b.binding] = b.dataSize;
            } else {
                srhiResourceSetLayout* layout = setLayouts[b.set];
                int idx = layout ? layout->FindEntry(b.binding) : -1;
                SRHI_CHECK(device, idx >= 0, false,
                           "pipeline '%s': entry point '%s' uses '%s' (set %u, binding %u), which is missing from the set layouts",
                           label, entry->name.c_str(), b.name.c_str(), b.set, b.binding);
                const srhiResourceSetLayoutEntry& le = layout->entries[idx];
                SRHI_CHECK(device, le.type == b.type, false, "pipeline '%s': '%s' (set %u, binding %u) has type %d in the shader but %d in the layout",
                           label, b.name.c_str(), b.set, b.binding, int(b.type), int(le.type));
                SRHI_CHECK(device, le.stages & entry->stage, false,
                           "pipeline '%s': '%s' (set %u, binding %u) is used by a stage its layout entry does not list", label,
                           b.name.c_str(), b.set, b.binding);
                if (b.type == SRHI_BINDING_TYPE_TEXTURE || b.type == SRHI_BINDING_TYPE_STORAGE_TEXTURE)
                    SRHI_CHECK(device, le.textureType == b.textureType, false,
                               "pipeline '%s': '%s' texture type mismatch between shader and layout", label, b.name.c_str());
                if (b.type == SRHI_BINDING_TYPE_STORAGE_TEXTURE)
                    SRHI_CHECK(device, le.storageFormat == b.storageFormat, false,
                               "pipeline '%s': '%s' storage format mismatch between shader and layout", label, b.name.c_str());
                if (b.type == SRHI_BINDING_TYPE_SAMPLER)
                    SRHI_CHECK(device, (le.samplerType == SRHI_SAMPLER_BINDING_TYPE_COMPARISON) ==
                                           (b.samplerType == SRHI_SAMPLER_BINDING_TYPE_COMPARISON),
                               false, "pipeline '%s': '%s' comparison sampler mismatch", label, b.name.c_str());
                slot = &out.slots[b.set][idx];
            }
            slot->used = true;
            slot->stages |= entry->stage;
            slot->type = b.type;
            slot->d3dClass = b.d3dClass;
            slot->d3dSlot = b.d3dSlot;
            slot->metalClass = b.metalClass;
            slot->metalSlot = b.metalSlot;
        }
    }
    return true;
}

extern "C" srhiRenderPipeline* srhiRenderPipelineCreate(srhiDevice* device, const srhiRenderPipelineDesc* desc)
{
    if (!device || !desc)
        return nullptr;
    const char* label = Str(desc->label);
    SRHI_CHECK(device, desc->module, nullptr, "srhiRenderPipelineCreate(%s): no shader module", label);
    const Package& pkg = desc->module->package;
    const PkgEntry* vs = pkg.FindEntry(desc->vertexEntry);
    SRHI_CHECK(device, vs && vs->stage == SRHI_SHADER_STAGE_VERTEX, nullptr,
               "srhiRenderPipelineCreate(%s): vertex entry point '%s' not found", label, Str(desc->vertexEntry));
    const PkgEntry* fs = nullptr;
    if (desc->fragmentEntry) {
        fs = pkg.FindEntry(desc->fragmentEntry);
        SRHI_CHECK(device, fs && fs->stage == SRHI_SHADER_STAGE_FRAGMENT, nullptr,
                   "srhiRenderPipelineCreate(%s): fragment entry point '%s' not found", label, desc->fragmentEntry);
    }

    srhiRenderPipelineDesc d = *desc;
    SRHI_CHECK(device, d.colorTargetCount <= SRHI_MAX_COLOR_ATTACHMENTS, nullptr, "srhiRenderPipelineCreate(%s): too many color targets", label);
    SRHI_CHECK(device, d.colorTargetCount > 0 || d.depthStencil.format != SRHI_FORMAT_UNDEFINED, nullptr,
               "srhiRenderPipelineCreate(%s): no color or depth target", label);
    for (uint32_t i = 0; i < d.colorTargetCount; ++i) {
        srhiColorTargetState& ct = d.colorTargets[i];
        SRHI_CHECK(device, device->GetFormatSupport(ct.format) & SRHI_FORMAT_SUPPORT_RENDER_TARGET, nullptr,
                   "srhiRenderPipelineCreate(%s): color target %u format %s is not renderable", label, i, GetFormatInfo(ct.format).name);
        if (ct.blend.enable)
            SRHI_CHECK(device, device->GetFormatSupport(ct.format) & SRHI_FORMAT_SUPPORT_BLEND, nullptr,
                       "srhiRenderPipelineCreate(%s): color target %u format %s is not blendable", label, i, GetFormatInfo(ct.format).name);
        ct.writeMask = ct.writeDisabled ? 0 : (ct.writeMask ? ct.writeMask : SRHI_COLOR_WRITE_ALL);
    }
    if (d.depthStencil.format != SRHI_FORMAT_UNDEFINED)
        SRHI_CHECK(device, GetFormatInfo(d.depthStencil.format).depth, nullptr, "srhiRenderPipelineCreate(%s): %s is not a depth format",
                   label, GetFormatInfo(d.depthStencil.format).name);
    if (d.depthStencil.stencilEnable)
        SRHI_CHECK(device, GetFormatInfo(d.depthStencil.format).stencil, nullptr,
                   "srhiRenderPipelineCreate(%s): stencil enabled without a stencil format", label);
    if (d.sampleCount == 0)
        d.sampleCount = 1;
    SRHI_CHECK(device, d.sampleCount == 1 || d.sampleCount == 4, nullptr, "srhiRenderPipelineCreate(%s): sampleCount must be 1 or 4", label);
    if (d.raster.fillMode == SRHI_FILL_MODE_WIREFRAME)
        SRHI_CHECK(device, device->info.features.fillModeWireframe, nullptr, "srhiRenderPipelineCreate(%s): wireframe unsupported", label);
    if (d.raster.depthClamp)
        SRHI_CHECK(device, device->info.features.depthClamp, nullptr, "srhiRenderPipelineCreate(%s): depth clamp unsupported", label);

    // Vertex layout vs shader inputs.
    SRHI_CHECK(device, d.vertexLayout.bufferCount <= SRHI_MAX_VERTEX_BUFFERS && d.vertexLayout.attributeCount <= SRHI_MAX_VERTEX_ATTRIBUTES,
               nullptr, "srhiRenderPipelineCreate(%s): too many vertex buffers / attributes", label);
    for (uint32_t i = 0; i < d.vertexLayout.attributeCount; ++i) {
        const srhiVertexAttribute& a = d.vertexLayout.attributes[i];
        SRHI_CHECK(device, a.bufferIndex < d.vertexLayout.bufferCount, nullptr,
                   "srhiRenderPipelineCreate(%s): attribute %u references vertex buffer %u", label, a.location, a.bufferIndex);
        SRHI_CHECK(device, a.offset % 4 == 0, nullptr, "srhiRenderPipelineCreate(%s): attribute offsets must be 4-byte aligned", label);
    }
    for (uint32_t i = 0; i < d.vertexLayout.bufferCount; ++i)
        SRHI_CHECK(device, d.vertexLayout.buffers[i].stride % 4 == 0 && d.vertexLayout.buffers[i].stride > 0, nullptr,
                   "srhiRenderPipelineCreate(%s): vertex buffer %u stride must be a non-zero multiple of 4", label, i);
    for (const PkgVertexInput& in : vs->inputs) {
        bool found = false;
        for (uint32_t i = 0; i < d.vertexLayout.attributeCount; ++i)
            found |= d.vertexLayout.attributes[i].location == in.location;
        SRHI_CHECK(device, found, nullptr, "srhiRenderPipelineCreate(%s): vertex input location %u (%s%u) has no attribute", label,
                   in.location, in.semantic.c_str(), in.semanticIndex);
    }

    PipelineLayoutInfo layout;
    const PkgEntry* entries[2] = {vs, fs};
    if (!BuildPipelineLayout(device, label, pkg, entries, fs ? 2 : 1, desc->setLayouts, layout))
        return nullptr;

    srhiRenderPipeline* pipeline = device->CreateRenderPipeline(d, std::move(layout));
    if (!pipeline)
        return nullptr;
    pipeline->device = device;
    pipeline->label = label;
    pipeline->desc = d;
    pipeline->desc.vertexEntry = nullptr;
    pipeline->desc.fragmentEntry = nullptr;
    pipeline->desc.module = nullptr;
    pipeline->desc.label = pipeline->label.c_str();
    ++device->liveObjects;
    return pipeline;
}

extern "C" void srhiRenderPipelineDestroy(srhiRenderPipeline* pipeline)
{
    if (!pipeline)
        return;
    --pipeline->device->liveObjects;
    delete pipeline;
}

extern "C" srhiComputePipeline* srhiComputePipelineCreate(srhiDevice* device, const srhiComputePipelineDesc* desc)
{
    if (!device || !desc)
        return nullptr;
    const char* label = Str(desc->label);
    SRHI_CHECK(device, desc->module, nullptr, "srhiComputePipelineCreate(%s): no shader module", label);
    const PkgEntry* cs = desc->module->package.FindEntry(desc->entry);
    SRHI_CHECK(device, cs && cs->stage == SRHI_SHADER_STAGE_COMPUTE, nullptr,
               "srhiComputePipelineCreate(%s): compute entry point '%s' not found", label, Str(desc->entry));

    PipelineLayoutInfo layout;
    if (!BuildPipelineLayout(device, label, desc->module->package, &cs, 1, desc->setLayouts, layout))
        return nullptr;

    srhiComputePipeline* pipeline = device->CreateComputePipeline(*desc, std::move(layout));
    if (!pipeline)
        return nullptr;
    pipeline->device = device;
    pipeline->label = label;
    pipeline->desc = *desc;
    pipeline->desc.entry = nullptr;
    pipeline->desc.module = nullptr;
    pipeline->desc.label = pipeline->label.c_str();
    for (int i = 0; i < 3; ++i)
        pipeline->threadgroupSize[i] = cs->threadgroupSize[i];
    ++device->liveObjects;
    return pipeline;
}

extern "C" void srhiComputePipelineDestroy(srhiComputePipeline* pipeline)
{
    if (!pipeline)
        return;
    --pipeline->device->liveObjects;
    delete pipeline;
}

extern "C" void srhiComputePipelineGetThreadgroupSize(const srhiComputePipeline* pipeline, uint32_t outSize[3])
{
    if (pipeline && outSize)
        for (int i = 0; i < 3; ++i)
            outSize[i] = pipeline->threadgroupSize[i];
}

// =====================================================================================================
// Queries
// =====================================================================================================

extern "C" srhiQuerySet* srhiQuerySetCreate(srhiDevice* device, uint32_t count, const char* label)
{
    if (!device)
        return nullptr;
    SRHI_CHECK(device, device->info.features.timestampQueries, nullptr, "srhiQuerySetCreate(%s): timestamps unsupported", Str(label));
    SRHI_CHECK(device, count > 0, nullptr, "srhiQuerySetCreate(%s): count is 0", Str(label));
    srhiQuerySet* qs = device->CreateQuerySet(count, label);
    if (!qs)
        return nullptr;
    qs->device = device;
    qs->count = count;
    qs->label = Str(label);
    ++device->liveObjects;
    return qs;
}

extern "C" void srhiQuerySetDestroy(srhiQuerySet* querySet)
{
    if (!querySet)
        return;
    --querySet->device->liveObjects;
    delete querySet;
}

extern "C" srhiBool srhiQuerySetGetResults(srhiQuerySet* querySet, uint32_t first, uint32_t count, uint64_t* outNanoseconds)
{
    if (!querySet || !outNanoseconds)
        return 0;
    SRHI_CHECK(querySet->device, first + count <= querySet->count, 0, "srhiQuerySetGetResults(%s): range out of bounds",
               querySet->label.c_str());
    return querySet->GetResults(first, count, outNanoseconds);
}

static bool ValidateTimestamps(srhiDevice* device, const srhiPassTimestampWrites& tw, const char* label)
{
    if (!tw.querySet)
        return true;
    SRHI_CHECK(device, tw.beginIndex == SRHI_QUERY_UNUSED || tw.beginIndex < tw.querySet->count, false,
               "pass '%s': timestamp beginIndex out of range", label);
    SRHI_CHECK(device, tw.endIndex == SRHI_QUERY_UNUSED || tw.endIndex < tw.querySet->count, false,
               "pass '%s': timestamp endIndex out of range", label);
    return true;
}

// =====================================================================================================
// Command buffers
// =====================================================================================================

#define SRHI_CHECK_NO_ENCODER(cmd, fn, ret)                                                                            \
    SRHI_CHECK((cmd)->device, !(cmd)->encoderOpen, ret, fn "(%s): an encoder is still open", (cmd)->label.c_str())

extern "C" srhiCommandBuffer* srhiCommandBufferBegin(srhiDevice* device, const char* label)
{
    if (!device)
        return nullptr;
    srhiCommandBuffer* cmd = device->BeginCommandBuffer(label);
    if (!cmd)
        return nullptr;
    cmd->device = device;
    cmd->label = Str(label);
    return cmd;
}

extern "C" void srhiCommandBufferCommit(srhiCommandBuffer* cmd, srhiCompletionCallback onCompleted, void* userData)
{
    if (!cmd)
        return;
    SRHI_CHECK_NO_ENCODER(cmd, "srhiCommandBufferCommit", );
    cmd->Commit(onCompleted, userData);
}

extern "C" void srhiCommandBufferPushDebugGroup(srhiCommandBuffer* cmd, const char* name)
{
    if (cmd && name) {
        SRHI_CHECK_NO_ENCODER(cmd, "srhiCommandBufferPushDebugGroup", );
        cmd->PushDebugGroup(name);
    }
}

extern "C" void srhiCommandBufferPopDebugGroup(srhiCommandBuffer* cmd)
{
    if (cmd) {
        SRHI_CHECK_NO_ENCODER(cmd, "srhiCommandBufferPopDebugGroup", );
        cmd->PopDebugGroup();
    }
}

extern "C" void srhiCommandBufferReadBuffer(srhiCommandBuffer* cmd, srhiBuffer* buffer, uint64_t offset, uint64_t size,
                                            srhiReadbackCallback callback, void* userData)
{
    if (!cmd || !buffer || !callback)
        return;
    SRHI_CHECK_NO_ENCODER(cmd, "srhiCommandBufferReadBuffer", );
    if (size == SRHI_WHOLE_SIZE)
        size = buffer->desc.size - offset;
    SRHI_CHECK(cmd->device, offset % 4 == 0 && size % 4 == 0 && size > 0 && offset + size <= buffer->desc.size, ,
               "srhiCommandBufferReadBuffer(%s): invalid range (must be in bounds and 4-byte aligned)", buffer->label.c_str());
    cmd->ReadBuffer(buffer, offset, size, callback, userData);
}

extern "C" void srhiCommandBufferReadTexture(srhiCommandBuffer* cmd, srhiTexture* texture, uint32_t mip, uint32_t layer,
                                             srhiReadbackCallback callback, void* userData)
{
    if (!cmd || !texture || !callback)
        return;
    SRHI_CHECK_NO_ENCODER(cmd, "srhiCommandBufferReadTexture", );
    uint32_t layers = texture->desc.type == SRHI_TEXTURE_TYPE_3D ? texture->Depth(mip) : texture->LayerCount();
    SRHI_CHECK(cmd->device, mip < texture->desc.mipLevels && layer < layers, , "srhiCommandBufferReadTexture(%s): mip/layer out of range",
               texture->label.c_str());
    SRHI_CHECK(cmd->device, texture->desc.sampleCount == 1, , "srhiCommandBufferReadTexture(%s): cannot read MSAA textures (resolve first)",
               texture->label.c_str());
    SRHI_CHECK(cmd->device, !GetFormatInfo(texture->desc.format).stencil, ,
               "srhiCommandBufferReadTexture(%s): cannot read depth-stencil textures", texture->label.c_str());
    cmd->ReadTexture(texture, mip, layer, callback, userData);
}

extern "C" void srhiCommandBufferEncodeNative(srhiCommandBuffer* cmd, srhiNativeEncodeCallback callback, void* userData)
{
    if (!cmd || !callback)
        return;
    SRHI_CHECK_NO_ENCODER(cmd, "srhiCommandBufferEncodeNative", );
    cmd->EncodeNative(callback, userData);
}

extern "C" void srhiCommandBufferPresent(srhiCommandBuffer* cmd, srhiSwapchain* swapchain)
{
    if (!cmd || !swapchain)
        return;
    SRHI_CHECK_NO_ENCODER(cmd, "srhiCommandBufferPresent", );
    cmd->Present(swapchain);
}

// --- Render encoder ----------------------------------------------------------------------------------

static bool ValidateAttachmentView(srhiDevice* device, const char* label, srhiTextureView* view, const char* what)
{
    srhiTexture* tex = view->texture;
    SRHI_CHECK(device, tex->desc.usage & SRHI_TEXTURE_USAGE_RENDER_TARGET, false,
               "render pass '%s': %s texture '%s' lacks RENDER_TARGET usage", label, what, tex->label.c_str());
    SRHI_CHECK(device, view->desc.mipCount == 1, false, "render pass '%s': %s view must cover exactly one mip", label, what);
    SRHI_CHECK(device, tex->desc.type == SRHI_TEXTURE_TYPE_3D || view->desc.layerCount == 1, false,
               "render pass '%s': %s view must cover exactly one layer (use a 2D view of the layer)", label, what);
    return true;
}

extern "C" srhiRenderEncoder* srhiRenderEncoderBegin(srhiCommandBuffer* cmd, const srhiRenderPassDesc* desc)
{
    if (!cmd || !desc)
        return nullptr;
    srhiDevice* device = cmd->device;
    const char* label = Str(desc->label);
    SRHI_CHECK_NO_ENCODER(cmd, "srhiRenderEncoderBegin", nullptr);
    SRHI_CHECK(device, desc->colorAttachmentCount <= SRHI_MAX_COLOR_ATTACHMENTS, nullptr, "render pass '%s': too many attachments", label);
    SRHI_CHECK(device, desc->colorAttachmentCount > 0 || desc->depthAttachment.view, nullptr, "render pass '%s': no attachments", label);

    uint32_t width = 0, height = 0, samples = 0;
    auto checkSize = [&](srhiTextureView* view) {
        uint32_t w = MipDim(view->texture->desc.width, view->desc.baseMip), h = MipDim(view->texture->desc.height, view->desc.baseMip);
        uint32_t s = view->texture->desc.sampleCount;
        if (width == 0) {
            width = w;
            height = h;
            samples = s;
            return true;
        }
        return w == width && h == height && s == samples;
    };

    for (uint32_t i = 0; i < desc->colorAttachmentCount; ++i) {
        const srhiColorAttachment& a = desc->colorAttachments[i];
        SRHI_CHECK(device, a.view, nullptr, "render pass '%s': color attachment %u has no view", label, i);
        if (!ValidateAttachmentView(device, label, a.view, "color"))
            return nullptr;
        SRHI_CHECK(device, !GetFormatInfo(a.view->desc.format).depth, nullptr, "render pass '%s': color attachment %u is a depth format", label, i);
        SRHI_CHECK(device, checkSize(a.view), nullptr, "render pass '%s': attachment sizes / sample counts differ", label);
        if (a.storeAction == SRHI_STORE_ACTION_RESOLVE || a.storeAction == SRHI_STORE_ACTION_STORE_AND_RESOLVE) {
            SRHI_CHECK(device, a.resolveView && a.view->texture->desc.sampleCount > 1, nullptr,
                       "render pass '%s': color attachment %u resolves without a resolve view or MSAA source", label, i);
            if (!ValidateAttachmentView(device, label, a.resolveView, "resolve"))
                return nullptr;
            SRHI_CHECK(device, a.resolveView->texture->desc.sampleCount == 1 && a.resolveView->desc.format == a.view->desc.format,
                       nullptr, "render pass '%s': resolve view %u must be single-sampled with the same format", label, i);
        }
    }
    const srhiDepthAttachment& da = desc->depthAttachment;
    if (da.view) {
        if (!ValidateAttachmentView(device, label, da.view, "depth"))
            return nullptr;
        SRHI_CHECK(device, GetFormatInfo(da.view->desc.format).depth, nullptr, "render pass '%s': depth attachment is not a depth format", label);
        SRHI_CHECK(device, checkSize(da.view), nullptr, "render pass '%s': attachment sizes / sample counts differ", label);
        SRHI_CHECK(device, da.depthStoreAction != SRHI_STORE_ACTION_RESOLVE && da.depthStoreAction != SRHI_STORE_ACTION_STORE_AND_RESOLVE,
                   nullptr, "render pass '%s': depth resolve is not supported", label);
    }
    if (desc->timestampWrites.querySet && !ValidateTimestamps(device, desc->timestampWrites, label))
        return nullptr;

    srhiRenderEncoder* enc = cmd->BeginRender(*desc);
    if (!enc)
        return nullptr;
    enc->cmd = cmd;
    enc->pipeline = nullptr;
    enc->indexBufferSet = false;
    enc->colorCount = desc->colorAttachmentCount;
    for (uint32_t i = 0; i < desc->colorAttachmentCount; ++i)
        enc->colorFormats[i] = desc->colorAttachments[i].view->desc.format;
    enc->depthFormat = da.view ? da.view->desc.format : SRHI_FORMAT_UNDEFINED;
    enc->sampleCount = samples;
    cmd->encoderOpen = true;
    return enc;
}

extern "C" void srhiRenderEncoderEnd(srhiRenderEncoder* encoder)
{
    if (!encoder)
        return;
    srhiCommandBuffer* cmd = encoder->cmd;
    encoder->End();
    cmd->encoderOpen = false;
}

extern "C" void srhiRenderEncoderSetPipeline(srhiRenderEncoder* encoder, srhiRenderPipeline* pipeline)
{
    if (!encoder || !pipeline)
        return;
    srhiDevice* device = encoder->cmd->device;
    const srhiRenderPipelineDesc& d = pipeline->desc;
    bool match = d.colorTargetCount == encoder->colorCount && d.depthStencil.format == encoder->depthFormat &&
                 d.sampleCount == encoder->sampleCount;
    for (uint32_t i = 0; match && i < d.colorTargetCount; ++i)
        match = d.colorTargets[i].format == encoder->colorFormats[i];
    SRHI_CHECK(device, match, , "srhiRenderEncoderSetPipeline: pipeline '%s' attachment formats / sample count do not match the pass",
               pipeline->label.c_str());
    encoder->pipeline = pipeline;
    encoder->SetPipeline(pipeline);
}

extern "C" void srhiRenderEncoderSetVertexBuffer(srhiRenderEncoder* encoder, uint32_t index, srhiBuffer* buffer, uint64_t offset)
{
    if (!encoder || !buffer)
        return;
    SRHI_CHECK(encoder->cmd->device, index < SRHI_MAX_VERTEX_BUFFERS && (buffer->desc.usage & SRHI_BUFFER_USAGE_VERTEX) && offset % 4 == 0, ,
               "srhiRenderEncoderSetVertexBuffer: invalid index, offset or buffer '%s' lacks VERTEX usage", buffer->label.c_str());
    encoder->SetVertexBuffer(index, buffer, offset);
}

extern "C" void srhiRenderEncoderSetIndexBuffer(srhiRenderEncoder* encoder, srhiBuffer* buffer, srhiIndexType type, uint64_t offset)
{
    if (!encoder || !buffer)
        return;
    SRHI_CHECK(encoder->cmd->device, (buffer->desc.usage & SRHI_BUFFER_USAGE_INDEX) && offset % (type == SRHI_INDEX_TYPE_UINT16 ? 2 : 4) == 0, ,
               "srhiRenderEncoderSetIndexBuffer: buffer '%s' lacks INDEX usage or offset is misaligned", buffer->label.c_str());
    encoder->indexBufferSet = true;
    encoder->indexType = type;
    encoder->SetIndexBuffer(buffer, type, offset);
}

extern "C" void srhiRenderEncoderSetResourceSet(srhiRenderEncoder* encoder, uint32_t index, srhiResourceSet* set)
{
    if (!encoder || !set)
        return;
    SRHI_CHECK(encoder->cmd->device, index < SRHI_MAX_RESOURCE_SETS, , "srhiRenderEncoderSetResourceSet: index %u >= %u", index,
               SRHI_MAX_RESOURCE_SETS);
    encoder->SetResourceSet(index, set);
}

static bool ValidateUniforms(srhiDevice* device, uint32_t slot, const void* data, uint32_t size)
{
    SRHI_CHECK(device, slot < SRHI_MAX_UNIFORM_SLOTS, false, "SetUniforms: slot %u >= %u", slot, SRHI_MAX_UNIFORM_SLOTS);
    SRHI_CHECK(device, data && size > 0 && size <= SRHI_MAX_UNIFORM_SIZE && size % 4 == 0, false,
               "SetUniforms: size %u must be a multiple of 4 in (0, %u]", size, SRHI_MAX_UNIFORM_SIZE);
    return true;
}

extern "C" void srhiRenderEncoderSetUniforms(srhiRenderEncoder* encoder, uint32_t slot, const void* data, uint32_t size)
{
    if (encoder && ValidateUniforms(encoder->cmd->device, slot, data, size))
        encoder->SetUniforms(slot, data, size);
}

extern "C" void srhiRenderEncoderSetViewport(srhiRenderEncoder* encoder, const srhiViewport* viewport)
{
    if (encoder && viewport)
        encoder->SetViewport(*viewport);
}

extern "C" void srhiRenderEncoderSetScissor(srhiRenderEncoder* encoder, const srhiScissorRect* rect)
{
    if (encoder && rect)
        encoder->SetScissor(*rect);
}

extern "C" void srhiRenderEncoderSetBlendColor(srhiRenderEncoder* encoder, const float color[4])
{
    if (encoder && color)
        encoder->SetBlendColor(color);
}

extern "C" void srhiRenderEncoderSetStencilReference(srhiRenderEncoder* encoder, uint32_t reference)
{
    if (encoder)
        encoder->SetStencilReference(reference);
}

static bool CanDraw(srhiRenderEncoder* encoder, bool indexed, const char* fn)
{
    SRHI_CHECK(encoder->cmd->device, encoder->pipeline, false, "%s: no pipeline set", fn);
    if (indexed)
        SRHI_CHECK(encoder->cmd->device, encoder->indexBufferSet, false, "%s: no index buffer set", fn);
    return true;
}

extern "C" void srhiRenderEncoderDraw(srhiRenderEncoder* encoder, uint32_t vertexCount, uint32_t instanceCount,
                                      uint32_t firstVertex, uint32_t firstInstance)
{
    if (encoder && vertexCount && instanceCount && CanDraw(encoder, false, "srhiRenderEncoderDraw"))
        encoder->Draw(vertexCount, instanceCount, firstVertex, firstInstance);
}

extern "C" void srhiRenderEncoderDrawIndexed(srhiRenderEncoder* encoder, uint32_t indexCount, uint32_t instanceCount,
                                             uint32_t firstIndex, int32_t baseVertex, uint32_t firstInstance)
{
    if (encoder && indexCount && instanceCount && CanDraw(encoder, true, "srhiRenderEncoderDrawIndexed"))
        encoder->DrawIndexed(indexCount, instanceCount, firstIndex, baseVertex, firstInstance);
}

static bool ValidateIndirect(srhiDevice* device, srhiBuffer* buffer, uint64_t offset, uint64_t size, const char* fn)
{
    SRHI_CHECK(device, buffer->desc.usage & SRHI_BUFFER_USAGE_INDIRECT, false, "%s: buffer '%s' lacks INDIRECT usage", fn,
               buffer->label.c_str());
    SRHI_CHECK(device, offset % 4 == 0 && offset + size <= buffer->desc.size, false, "%s: offset misaligned or out of range", fn);
    return true;
}

extern "C" void srhiRenderEncoderDrawIndirect(srhiRenderEncoder* encoder, srhiBuffer* buffer, uint64_t offset)
{
    if (encoder && buffer && CanDraw(encoder, false, "srhiRenderEncoderDrawIndirect") &&
        ValidateIndirect(encoder->cmd->device, buffer, offset, sizeof(srhiDrawIndirectArgs), "srhiRenderEncoderDrawIndirect"))
        encoder->DrawIndirect(buffer, offset);
}

extern "C" void srhiRenderEncoderDrawIndexedIndirect(srhiRenderEncoder* encoder, srhiBuffer* buffer, uint64_t offset)
{
    if (encoder && buffer && CanDraw(encoder, true, "srhiRenderEncoderDrawIndexedIndirect") &&
        ValidateIndirect(encoder->cmd->device, buffer, offset, sizeof(srhiDrawIndexedIndirectArgs), "srhiRenderEncoderDrawIndexedIndirect"))
        encoder->DrawIndexedIndirect(buffer, offset);
}

extern "C" void srhiRenderEncoderPushDebugGroup(srhiRenderEncoder* encoder, const char* name)
{
    if (encoder && name)
        encoder->PushDebugGroup(name);
}

extern "C" void srhiRenderEncoderPopDebugGroup(srhiRenderEncoder* encoder)
{
    if (encoder)
        encoder->PopDebugGroup();
}

extern "C" void srhiRenderEncoderInsertDebugMarker(srhiRenderEncoder* encoder, const char* name)
{
    if (encoder && name)
        encoder->InsertDebugMarker(name);
}

// --- Compute encoder ---------------------------------------------------------------------------------

extern "C" srhiComputeEncoder* srhiComputeEncoderBegin(srhiCommandBuffer* cmd, const srhiComputePassDesc* desc)
{
    if (!cmd)
        return nullptr;
    SRHI_CHECK_NO_ENCODER(cmd, "srhiComputeEncoderBegin", nullptr);
    srhiComputePassDesc d = desc ? *desc : srhiComputePassDesc{};
    if (!ValidateTimestamps(cmd->device, d.timestampWrites, Str(d.label)))
        return nullptr;
    srhiComputeEncoder* enc = cmd->BeginCompute(d);
    if (!enc)
        return nullptr;
    enc->cmd = cmd;
    enc->pipeline = nullptr;
    cmd->encoderOpen = true;
    return enc;
}

extern "C" void srhiComputeEncoderEnd(srhiComputeEncoder* encoder)
{
    if (!encoder)
        return;
    srhiCommandBuffer* cmd = encoder->cmd;
    encoder->End();
    cmd->encoderOpen = false;
}

extern "C" void srhiComputeEncoderSetPipeline(srhiComputeEncoder* encoder, srhiComputePipeline* pipeline)
{
    if (!encoder || !pipeline)
        return;
    encoder->pipeline = pipeline;
    encoder->SetPipeline(pipeline);
}

extern "C" void srhiComputeEncoderSetResourceSet(srhiComputeEncoder* encoder, uint32_t index, srhiResourceSet* set)
{
    if (!encoder || !set)
        return;
    SRHI_CHECK(encoder->cmd->device, index < SRHI_MAX_RESOURCE_SETS, , "srhiComputeEncoderSetResourceSet: index %u >= %u", index,
               SRHI_MAX_RESOURCE_SETS);
    encoder->SetResourceSet(index, set);
}

extern "C" void srhiComputeEncoderSetUniforms(srhiComputeEncoder* encoder, uint32_t slot, const void* data, uint32_t size)
{
    if (encoder && ValidateUniforms(encoder->cmd->device, slot, data, size))
        encoder->SetUniforms(slot, data, size);
}

extern "C" void srhiComputeEncoderDispatch(srhiComputeEncoder* encoder, uint32_t x, uint32_t y, uint32_t z)
{
    if (!encoder || !x || !y || !z)
        return;
    SRHI_CHECK(encoder->cmd->device, encoder->pipeline, , "srhiComputeEncoderDispatch: no pipeline set");
    encoder->Dispatch(x, y, z);
}

extern "C" void srhiComputeEncoderDispatchIndirect(srhiComputeEncoder* encoder, srhiBuffer* buffer, uint64_t offset)
{
    if (!encoder || !buffer)
        return;
    SRHI_CHECK(encoder->cmd->device, encoder->pipeline, , "srhiComputeEncoderDispatchIndirect: no pipeline set");
    if (ValidateIndirect(encoder->cmd->device, buffer, offset, sizeof(srhiDispatchIndirectArgs), "srhiComputeEncoderDispatchIndirect"))
        encoder->DispatchIndirect(buffer, offset);
}

extern "C" void srhiComputeEncoderPushDebugGroup(srhiComputeEncoder* encoder, const char* name)
{
    if (encoder && name)
        encoder->PushDebugGroup(name);
}

extern "C" void srhiComputeEncoderPopDebugGroup(srhiComputeEncoder* encoder)
{
    if (encoder)
        encoder->PopDebugGroup();
}

extern "C" void srhiComputeEncoderInsertDebugMarker(srhiComputeEncoder* encoder, const char* name)
{
    if (encoder && name)
        encoder->InsertDebugMarker(name);
}

// --- Blit encoder ------------------------------------------------------------------------------------

extern "C" srhiBlitEncoder* srhiBlitEncoderBegin(srhiCommandBuffer* cmd, const char* label)
{
    if (!cmd)
        return nullptr;
    SRHI_CHECK_NO_ENCODER(cmd, "srhiBlitEncoderBegin", nullptr);
    srhiBlitEncoder* enc = cmd->BeginBlit(label);
    if (!enc)
        return nullptr;
    enc->cmd = cmd;
    cmd->encoderOpen = true;
    return enc;
}

extern "C" void srhiBlitEncoderEnd(srhiBlitEncoder* encoder)
{
    if (!encoder)
        return;
    srhiCommandBuffer* cmd = encoder->cmd;
    encoder->End();
    cmd->encoderOpen = false;
}

extern "C" void srhiBlitEncoderCopyBufferToBuffer(srhiBlitEncoder* encoder, srhiBuffer* src, uint64_t srcOffset, srhiBuffer* dst,
                                                  uint64_t dstOffset, uint64_t size)
{
    if (!encoder || !src || !dst || !size)
        return;
    SRHI_CHECK(encoder->cmd->device,
               srcOffset % 4 == 0 && dstOffset % 4 == 0 && size % 4 == 0 && srcOffset + size <= src->desc.size &&
                   dstOffset + size <= dst->desc.size && (src != dst || srcOffset + size <= dstOffset || dstOffset + size <= srcOffset), ,
               "srhiBlitEncoderCopyBufferToBuffer: ranges must be 4-byte aligned, in bounds and not overlap");
    encoder->CopyBufferToBuffer(src, srcOffset, dst, dstOffset, size);
}

static bool ValidateBufferTextureCopy(srhiDevice* device, const srhiBufferCopyLocation& b, const srhiTextureCopyLocation& t,
                                      uint32_t width, uint32_t height, uint32_t depth, const char* fn)
{
    const srhiTextureDesc& td = t.texture->desc;
    const FormatInfo& fi = GetFormatInfo(td.format);
    SRHI_CHECK(device, b.bytesPerRow % SRHI_COPY_BYTES_PER_ROW_ALIGNMENT == 0, false, "%s: bytesPerRow must be a multiple of %u", fn,
               SRHI_COPY_BYTES_PER_ROW_ALIGNMENT);
    SRHI_CHECK(device, b.bytesPerRow >= RowBytes(td.format, width), false, "%s: bytesPerRow too small", fn);
    SRHI_CHECK(device, b.offset % fi.blockBytes == 0, false, "%s: buffer offset must be a multiple of the texel/block size", fn);
    SRHI_CHECK(device, t.mip < td.mipLevels && td.sampleCount == 1, false, "%s: invalid mip or MSAA texture", fn);
    SRHI_CHECK(device, !fi.stencil, false, "%s: depth-stencil textures cannot be copied to/from buffers", fn);
    uint32_t mw = MipDim(td.width, t.mip), mh = MipDim(td.height, t.mip);
    uint32_t md = td.type == SRHI_TEXTURE_TYPE_3D ? t.texture->Depth(t.mip) : t.texture->LayerCount();
    uint32_t zStart = td.type == SRHI_TEXTURE_TYPE_3D ? t.z : t.layer;
    SRHI_CHECK(device, t.x + width <= mw && t.y + height <= mh && zStart + depth <= md, false, "%s: region out of bounds", fn);
    uint32_t rows = b.rowsPerImage ? b.rowsPerImage : RowCount(td.format, height);
    uint64_t needed = b.offset + uint64_t(b.bytesPerRow) * rows * (depth - 1) + uint64_t(b.bytesPerRow) * (RowCount(td.format, height) - 1) +
                      RowBytes(td.format, width);
    SRHI_CHECK(device, needed <= b.buffer->desc.size, false, "%s: buffer too small for the copy", fn);
    return true;
}

extern "C" void srhiBlitEncoderCopyBufferToTexture(srhiBlitEncoder* encoder, const srhiBufferCopyLocation* src,
                                                   const srhiTextureCopyLocation* dst, uint32_t width, uint32_t height, uint32_t depth)
{
    if (!encoder || !src || !dst || !src->buffer || !dst->texture || !width || !height)
        return;
    if (depth == 0)
        depth = 1;
    SRHI_CHECK(encoder->cmd->device, !GetFormatInfo(dst->texture->desc.format).depth, ,
               "srhiBlitEncoderCopyBufferToTexture: cannot copy into depth textures");
    if (ValidateBufferTextureCopy(encoder->cmd->device, *src, *dst, width, height, depth, "srhiBlitEncoderCopyBufferToTexture"))
        encoder->CopyBufferToTexture(*src, *dst, width, height, depth);
}

extern "C" void srhiBlitEncoderCopyTextureToBuffer(srhiBlitEncoder* encoder, const srhiTextureCopyLocation* src,
                                                   const srhiBufferCopyLocation* dst, uint32_t width, uint32_t height, uint32_t depth)
{
    if (!encoder || !src || !dst || !dst->buffer || !src->texture || !width || !height)
        return;
    if (depth == 0)
        depth = 1;
    if (ValidateBufferTextureCopy(encoder->cmd->device, *dst, *src, width, height, depth, "srhiBlitEncoderCopyTextureToBuffer"))
        encoder->CopyTextureToBuffer(*src, *dst, width, height, depth);
}

extern "C" void srhiBlitEncoderCopyTextureToTexture(srhiBlitEncoder* encoder, const srhiTextureCopyLocation* src,
                                                    const srhiTextureCopyLocation* dst, uint32_t width, uint32_t height, uint32_t depth)
{
    if (!encoder || !src || !dst || !src->texture || !dst->texture || !width || !height)
        return;
    if (depth == 0)
        depth = 1;
    srhiDevice* device = encoder->cmd->device;
    const srhiTextureDesc& s = src->texture->desc;
    const srhiTextureDesc& d = dst->texture->desc;
    SRHI_CHECK(device, FormatsViewCompatible(s.format, d.format) && s.sampleCount == d.sampleCount, ,
               "srhiBlitEncoderCopyTextureToTexture: formats / sample counts differ");
    SRHI_CHECK(device, src->mip < s.mipLevels && dst->mip < d.mipLevels, , "srhiBlitEncoderCopyTextureToTexture: mip out of range");
    SRHI_CHECK(device, src->x + width <= MipDim(s.width, src->mip) && src->y + height <= MipDim(s.height, src->mip) &&
                           dst->x + width <= MipDim(d.width, dst->mip) && dst->y + height <= MipDim(d.height, dst->mip), ,
               "srhiBlitEncoderCopyTextureToTexture: region out of bounds");
    encoder->CopyTextureToTexture(*src, *dst, width, height, depth);
}

extern "C" void srhiBlitEncoderGenerateMipmaps(srhiBlitEncoder* encoder, srhiTexture* texture)
{
    if (!encoder || !texture || texture->desc.mipLevels < 2)
        return;
    srhiDevice* device = encoder->cmd->device;
    uint32_t support = device->GetFormatSupport(texture->desc.format);
    SRHI_CHECK(device, (texture->desc.usage & SRHI_TEXTURE_USAGE_RENDER_TARGET) && (support & SRHI_FORMAT_SUPPORT_FILTER) &&
                           (support & SRHI_FORMAT_SUPPORT_RENDER_TARGET), ,
               "srhiBlitEncoderGenerateMipmaps(%s): needs RENDER_TARGET usage and a filterable, renderable format", texture->label.c_str());
    SRHI_CHECK(device, texture->desc.type != SRHI_TEXTURE_TYPE_3D, , "srhiBlitEncoderGenerateMipmaps(%s): 3D textures unsupported",
               texture->label.c_str());
    encoder->GenerateMipmaps(texture);
}

extern "C" void srhiBlitEncoderPushDebugGroup(srhiBlitEncoder* encoder, const char* name)
{
    if (encoder && name)
        encoder->PushDebugGroup(name);
}

extern "C" void srhiBlitEncoderPopDebugGroup(srhiBlitEncoder* encoder)
{
    if (encoder)
        encoder->PopDebugGroup();
}

// =====================================================================================================
// Swapchain
// =====================================================================================================

static srhiFormat SwapchainFormat(srhiColorSpace cs)
{
    switch (cs) {
    case SRHI_COLOR_SPACE_HDR10_PQ: return SRHI_FORMAT_RGB10A2_UNORM;
    case SRHI_COLOR_SPACE_EXTENDED_LINEAR_SRGB: return SRHI_FORMAT_RGBA16_FLOAT;
    default: return SRHI_FORMAT_BGRA8_UNORM_SRGB;
    }
}

extern "C" srhiSwapchain* srhiSwapchainCreate(srhiDevice* device, const srhiSwapchainDesc* desc)
{
    if (!device || !desc)
        return nullptr;
    SRHI_CHECK(device, desc->nativeWindow || desc->canvasSelector, nullptr, "srhiSwapchainCreate: no native window");
    srhiSwapchainDesc d = *desc;
    if (d.width == 0)
        d.width = 1;
    if (d.height == 0)
        d.height = 1;
    srhiSwapchain* sc = device->CreateSwapchain(d);
    if (!sc)
        return nullptr;
    sc->device = device;
    sc->label = Str(desc->label);
    sc->desc = d;
    sc->desc.label = sc->label.c_str();
    sc->desc.canvasSelector = nullptr;
    if (sc->format == SRHI_FORMAT_UNDEFINED)
        sc->format = SwapchainFormat(d.colorSpace);
    ++device->liveObjects;
    return sc;
}

extern "C" void srhiSwapchainDestroy(srhiSwapchain* swapchain)
{
    if (!swapchain)
        return;
    swapchain->device->WaitIdle();
    --swapchain->device->liveObjects;
    delete swapchain;
}

extern "C" void srhiSwapchainResize(srhiSwapchain* swapchain, uint32_t width, uint32_t height)
{
    if (!swapchain || (width == swapchain->desc.width && height == swapchain->desc.height))
        return;
    swapchain->Resize(width, height);
    swapchain->desc.width = width;
    swapchain->desc.height = height;
}

extern "C" srhiBool srhiSwapchainConfigure(srhiSwapchain* swapchain, srhiColorSpace colorSpace, srhiBool vsync)
{
    if (!swapchain)
        return 0;
    if (!swapchain->Configure(colorSpace, vsync != 0))
        return 0;
    swapchain->desc.colorSpace = colorSpace;
    swapchain->desc.vsync = vsync;
    return 1;
}

extern "C" srhiFormat srhiSwapchainGetFormat(const srhiSwapchain* swapchain)
{
    return swapchain ? swapchain->format : SRHI_FORMAT_UNDEFINED;
}

extern "C" void srhiSwapchainGetDisplayInfo(srhiSwapchain* swapchain, srhiDisplayInfo* outInfo)
{
    if (!swapchain || !outInfo)
        return;
    *outInfo = {};
    outInfo->edrHeadroom = 1.0f;
    outInfo->sdrWhiteLevel = 80.0f;
    swapchain->GetDisplayInfo(*outInfo);
}

extern "C" srhiTexture* srhiSwapchainAcquire(srhiSwapchain* swapchain)
{
    if (!swapchain || swapchain->desc.width == 0 || swapchain->desc.height == 0)
        return nullptr;
    return swapchain->Acquire();
}
