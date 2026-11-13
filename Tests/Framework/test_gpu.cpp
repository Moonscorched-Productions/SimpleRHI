/**
 * @ Author: Amélie Heinrich
 * @ Create Time: 2026-11-13 00:00:00
 * @ Copyright: Copyright (c) 2026 Amélie Heinrich. All rights reserved.
 */

#include "test_gpu.h"
#include "test_runner.h"

#include <cmath>
#include <cstring>
#include <map>
#include <mutex>

namespace srhitest
{
    srhiBackend SelectedBackend()
    {
        const std::string& b = RunnerOptions().backend;
        if (b == "metal")
            return SRHI_BACKEND_METAL;
        if (b == "d3d11")
            return SRHI_BACKEND_D3D11;
        if (b == "webgpu")
            return SRHI_BACKEND_WEBGPU;
#if defined(_WIN32)
        return SRHI_BACKEND_D3D11;
#elif defined(__APPLE__)
        return SRHI_BACKEND_METAL;
#else
        return SRHI_BACKEND_WEBGPU;
#endif
    }

    const char* BackendName()
    {
        switch (SelectedBackend()) {
        case SRHI_BACKEND_METAL: return "metal";
        case SRHI_BACKEND_D3D11: return "d3d11";
        case SRHI_BACKEND_WEBGPU: return "webgpu";
        default: return "unknown";
        }
    }

    srhiDevice* CreateTestDevice(srhiLogFunction log, void* logUserData)
    {
        srhiDeviceDesc desc = {};
        desc.backend = SelectedBackend();
        desc.debug = 1;
        desc.log = log;
        desc.logUserData = logUserData;
        return srhiDeviceCreate(&desc);
    }

    // --- Shader cache ------------------------------------------------------------------------------------

    namespace
    {
        std::mutex g_shaderMutex;
        std::map<std::string, std::vector<uint8_t>> g_shaderCache;

        uint32_t TargetFor(srhiBackend backend)
        {
            switch (backend) {
            case SRHI_BACKEND_METAL: return SRHI_SHADER_TARGET_METAL;
            case SRHI_BACKEND_D3D11: return SRHI_SHADER_TARGET_D3D11;
            default: return SRHI_SHADER_TARGET_WGSL;
            }
        }
    } // namespace

    GpuFixture::GpuFixture(TestContext& ctx) : mCtx(ctx)
    {
        mDevice = CreateTestDevice(&GpuFixture::LogCallback, this);
        if (mDevice)
            srhiDeviceGetInfo(mDevice, &mInfo);
    }

    GpuFixture::~GpuFixture()
    {
        if (mDevice)
            srhiDeviceWaitIdle(mDevice);
        for (auto it = mOwned.rbegin(); it != mOwned.rend(); ++it)
            (*it)();
        for (srhiShaderModule* m : mModules)
            srhiShaderModuleDestroy(m);
        if (mDevice)
            srhiDeviceDestroy(mDevice);
        if (mErrors.size() > mExpectedErrors) {
            std::string message = "SimpleRHI reported " + std::to_string(mErrors.size() - mExpectedErrors) + " unexpected error(s):";
            for (size_t i = mExpectedErrors; i < mErrors.size(); ++i)
                message += "\n  " + mErrors[i];
            mCtx.Fail(__FILE__, __LINE__, message);
        }
    }

    void GpuFixture::LogCallback(srhiLogSeverity severity, const char* message, void* userData)
    {
        GpuFixture* self = static_cast<GpuFixture*>(userData);
        if (severity == SRHI_LOG_SEVERITY_ERROR)
            self->mErrors.push_back(message);
        else if (severity == SRHI_LOG_SEVERITY_WARNING)
            fprintf(stderr, "       [warning] %s\n", message);
    }

    void GpuFixture::ExpectErrors(size_t count)
    {
        if (mErrors.size() - mExpectedErrors < count)
            mCtx.Fail(__FILE__, __LINE__, "expected " + std::to_string(count) + " validation error(s), got " +
                                              std::to_string(mErrors.size() - mExpectedErrors));
        mExpectedErrors = mErrors.size();
    }

    srhiShaderModule* GpuFixture::LoadShader(const char* name)
    {
        const std::string path = std::string("Tests/Shaders/") + name;
        const uint32_t targets = TargetFor(SelectedBackend());
        const std::string key = path + "#" + std::to_string(targets);
        std::vector<uint8_t> bytes;
        {
            std::lock_guard<std::mutex> lock(g_shaderMutex);
            auto it = g_shaderCache.find(key);
            if (it != g_shaderCache.end()) {
                bytes = it->second;
            } else {
                const char* includes[] = {"Source/SimpleRHI/Shaders"};
                srhiShaderCompileDesc desc = {};
                desc.sourcePath = path.c_str();
                desc.includeDirs = includes;
                desc.includeDirCount = 1;
                desc.targets = targets;
                srhiShaderPackage package;
                if (!srhiCompileShader(&desc, &package)) {
                    mErrors.push_back("shader compilation failed for " + path + ":\n" +
                                      (package.diagnostics ? package.diagnostics : ""));
                    srhiShaderPackageFree(&package);
                    return nullptr;
                }
                bytes.assign(static_cast<uint8_t*>(package.data), static_cast<uint8_t*>(package.data) + package.size);
                srhiShaderPackageFree(&package);
                g_shaderCache[key] = bytes;
            }
        }
        srhiShaderModule* module = srhiShaderModuleCreate(mDevice, bytes.data(), bytes.size(), name);
        if (module)
            mModules.push_back(module);
        return module;
    }

    void GpuFixture::Run(const std::function<void(srhiCommandBuffer*)>& record)
    {
        srhiCommandBuffer* cmd = srhiCommandBufferBegin(mDevice, mCtx.Case().name);
        record(cmd);
        srhiCommandBufferCommit(cmd, nullptr, nullptr);
        srhiDeviceWaitIdle(mDevice);
    }

    namespace
    {
        void CopyOut(const void* data, uint64_t size, void* user)
        {
            auto* out = static_cast<std::vector<uint8_t>*>(user);
            out->assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
        }
    } // namespace

    std::vector<uint8_t> GpuFixture::ReadBuffer(srhiBuffer* buffer, uint64_t offset, uint64_t size)
    {
        std::vector<uint8_t> out;
        Run([&](srhiCommandBuffer* cmd) { srhiCommandBufferReadBuffer(cmd, buffer, offset, size, CopyOut, &out); });
        return out;
    }

    std::vector<uint8_t> GpuFixture::ReadTextureBytes(srhiTexture* texture, uint32_t mip, uint32_t layer)
    {
        std::vector<uint8_t> out;
        Run([&](srhiCommandBuffer* cmd) { srhiCommandBufferReadTexture(cmd, texture, mip, layer, CopyOut, &out); });
        return out;
    }

    Image GpuFixture::ReadImage(srhiTexture* texture, uint32_t mip, uint32_t layer)
    {
        srhiTextureDesc desc;
        srhiTextureGetDesc(texture, &desc);
        std::vector<uint8_t> bytes = ReadTextureBytes(texture, mip, layer);
        Image image;
        image.width = desc.width >> mip ? desc.width >> mip : 1;
        image.height = desc.height >> mip ? desc.height >> mip : 1;
        const size_t count = size_t(image.width) * image.height;
        image.pixels.assign(count * 4, 0.0f);
        if (bytes.empty())
            return Image{};

        for (size_t i = 0; i < count; ++i) {
            float* p = &image.pixels[i * 4];
            p[3] = 1.0f;
            switch (desc.format) {
            case SRHI_FORMAT_RGBA8_UNORM:
            case SRHI_FORMAT_RGBA8_UNORM_SRGB:
            case SRHI_FORMAT_RGBA8_UINT:
                for (int c = 0; c < 4; ++c)
                    p[c] = bytes[i * 4 + c] / 255.0f;
                break;
            case SRHI_FORMAT_BGRA8_UNORM:
            case SRHI_FORMAT_BGRA8_UNORM_SRGB:
                p[0] = bytes[i * 4 + 2] / 255.0f;
                p[1] = bytes[i * 4 + 1] / 255.0f;
                p[2] = bytes[i * 4 + 0] / 255.0f;
                p[3] = bytes[i * 4 + 3] / 255.0f;
                break;
            case SRHI_FORMAT_R8_UNORM: p[0] = p[1] = p[2] = bytes[i] / 255.0f; break;
            case SRHI_FORMAT_RG8_UNORM:
                p[0] = bytes[i * 2] / 255.0f;
                p[1] = bytes[i * 2 + 1] / 255.0f;
                break;
            case SRHI_FORMAT_RGBA16_FLOAT: {
                const uint16_t* h = reinterpret_cast<const uint16_t*>(bytes.data()) + i * 4;
                for (int c = 0; c < 4; ++c)
                    p[c] = HalfToFloat(h[c]);
                break;
            }
            case SRHI_FORMAT_RG16_FLOAT: {
                const uint16_t* h = reinterpret_cast<const uint16_t*>(bytes.data()) + i * 2;
                p[0] = HalfToFloat(h[0]);
                p[1] = HalfToFloat(h[1]);
                break;
            }
            case SRHI_FORMAT_R16_FLOAT: p[0] = p[1] = p[2] = HalfToFloat(reinterpret_cast<const uint16_t*>(bytes.data())[i]); break;
            case SRHI_FORMAT_RGBA32_FLOAT: memcpy(p, bytes.data() + i * 16, 16); break;
            case SRHI_FORMAT_R32_FLOAT:
            case SRHI_FORMAT_D32_FLOAT: {
                float v;
                memcpy(&v, bytes.data() + i * 4, 4);
                p[0] = p[1] = p[2] = v;
                break;
            }
            case SRHI_FORMAT_D16_UNORM: {
                uint16_t v;
                memcpy(&v, bytes.data() + i * 2, 2);
                p[0] = p[1] = p[2] = v / 65535.0f;
                break;
            }
            case SRHI_FORMAT_RGB10A2_UNORM: {
                uint32_t v;
                memcpy(&v, bytes.data() + i * 4, 4);
                p[0] = (v & 1023) / 1023.0f;
                p[1] = ((v >> 10) & 1023) / 1023.0f;
                p[2] = ((v >> 20) & 1023) / 1023.0f;
                p[3] = (v >> 30) / 3.0f;
                break;
            }
            default:
                mErrors.push_back("ReadImage: unsupported format for image conversion");
                return Image{};
            }
        }
        return image;
    }

    srhiBuffer* GpuFixture::CreateBuffer(uint64_t size, uint32_t usage, const void* data, uint32_t stride, const char* label)
    {
        srhiBufferDesc desc = {};
        desc.size = size;
        desc.usage = usage;
        desc.structureStride = stride;
        desc.label = label;
        srhiBuffer* buffer = Own(srhiBufferCreate(mDevice, &desc));
        if (buffer && data)
            srhiBufferWrite(mDevice, buffer, 0, data, size);
        return buffer;
    }

    srhiTexture* GpuFixture::CreateTexture2D(uint32_t width, uint32_t height, srhiFormat format, uint32_t usage, uint32_t mips,
                                             const char* label)
    {
        srhiTextureDesc desc = {};
        desc.type = SRHI_TEXTURE_TYPE_2D;
        desc.format = format;
        desc.width = width;
        desc.height = height;
        desc.mipLevels = mips;
        desc.usage = usage;
        desc.label = label;
        return Own(srhiTextureCreate(mDevice, &desc));
    }

    srhiResourceSetLayout* GpuFixture::LayoutFromShader(srhiShaderModule* module, uint32_t set)
    {
        uint32_t count = srhiShaderModuleGetResourceSetLayout(module, set, nullptr, 0);
        std::vector<srhiResourceSetLayoutEntry> entries(count);
        srhiShaderModuleGetResourceSetLayout(module, set, entries.data(), count);
        srhiResourceSetLayoutDesc desc = {};
        desc.entries = entries.data();
        desc.entryCount = count;
        return Own(srhiResourceSetLayoutCreate(mDevice, &desc));
    }

    uint16_t FloatToHalf(float f)
    {
        uint32_t x;
        memcpy(&x, &f, 4);
        uint32_t sign = (x >> 16) & 0x8000;
        int32_t exp = int32_t((x >> 23) & 0xFF) - 127 + 15;
        uint32_t mant = x & 0x7FFFFF;
        if (exp <= 0)
            return uint16_t(sign);
        if (exp >= 31)
            return uint16_t(sign | 0x7C00);
        return uint16_t(sign | (exp << 10) | ((mant + 0x1000) >> 13));
    }

    float HalfToFloat(uint16_t h)
    {
        uint32_t sign = (h & 0x8000u) << 16;
        uint32_t exp = (h >> 10) & 0x1F;
        uint32_t mant = h & 0x3FF;
        uint32_t x;
        if (exp == 0) {
            if (mant == 0)
                x = sign;
            else {
                float f = std::ldexp(float(mant), -24);
                return sign ? -f : f;
            }
        } else if (exp == 31)
            x = sign | 0x7F800000 | (mant << 13);
        else
            x = sign | ((exp - 15 + 127) << 23) | (mant << 13);
        float f;
        memcpy(&f, &x, 4);
        return f;
    }
} // namespace srhitest
