/**
 * @ Author: Amélie Heinrich
 * @ Create Time: 2026-11-13 00:00:00
 * @ Copyright: Copyright (c) 2026 Amélie Heinrich. All rights reserved.
 */

#pragma once

#include "test_compare.h"
#include "test_framework.h"

#include <SimpleRHI.h>

#include <functional>
#include <string>
#include <vector>

namespace srhitest
{
    /// @brief The backend selected with --backend, resolved ("metal", "d3d11" or "webgpu").
    const char* BackendName();
    srhiBackend SelectedBackend();

    /// @brief Creates a device on the selected backend (debug validation on). NULL if unavailable.
    srhiDevice* CreateTestDevice(srhiLogFunction log = nullptr, void* logUserData = nullptr);

    inline void DestroyObject(srhiBuffer* o) { srhiBufferDestroy(o); }
    inline void DestroyObject(srhiTexture* o) { srhiTextureDestroy(o); }
    inline void DestroyObject(srhiTextureView* o) { srhiTextureViewDestroy(o); }
    inline void DestroyObject(srhiSampler* o) { srhiSamplerDestroy(o); }
    inline void DestroyObject(srhiResourceSetLayout* o) { srhiResourceSetLayoutDestroy(o); }
    inline void DestroyObject(srhiResourceSet* o) { srhiResourceSetDestroy(o); }
    inline void DestroyObject(srhiRenderPipeline* o) { srhiRenderPipelineDestroy(o); }
    inline void DestroyObject(srhiComputePipeline* o) { srhiComputePipelineDestroy(o); }
    inline void DestroyObject(srhiQuerySet* o) { srhiQuerySetDestroy(o); }

    /// @brief A device for one test. Any error SimpleRHI logs while the fixture is alive fails the test,
    /// unless the test expects it (ExpectErrors). Tests are synchronous: every helper waits for the GPU.
    class GpuFixture
    {
    public:
        explicit GpuFixture(TestContext& ctx);
        ~GpuFixture();

        GpuFixture(const GpuFixture&) = delete;
        GpuFixture& operator=(const GpuFixture&) = delete;

        bool Valid() const { return mDevice != nullptr; }
        srhiDevice* Device() const { return mDevice; }
        const srhiDeviceInfo& Info() const { return mInfo; }

        /// @brief Compiles Tests/Shaders/<name> for the selected backend (cached per process).
        srhiShaderModule* LoadShader(const char* name);

        /// @brief Records via record(cmd), commits, and waits for the GPU.
        void Run(const std::function<void(srhiCommandBuffer*)>& record);

        std::vector<uint8_t> ReadBuffer(srhiBuffer* buffer, uint64_t offset = 0, uint64_t size = SRHI_WHOLE_SIZE);
        std::vector<uint8_t> ReadTextureBytes(srhiTexture* texture, uint32_t mip = 0, uint32_t layer = 0);
        /// @brief Reads one mip/layer and converts it to an RGBA float Image (sRGB values stay encoded,
        ///        like the PNG goldens; depth becomes grey).
        Image ReadImage(srhiTexture* texture, uint32_t mip = 0, uint32_t layer = 0);

        /// @brief Destroys obj (in reverse order of ownership) when the fixture goes away. Returns obj.
        template <typename T> T* Own(T* obj)
        {
            if (obj)
                mOwned.push_back([obj] { DestroyObject(obj); });
            return obj;
        }

        // --- Convenience creators (owned by the fixture) (all fail the test via the error log on failure) ---

        srhiBuffer* CreateBuffer(uint64_t size, uint32_t usage, const void* data = nullptr, uint32_t stride = 0,
                                 const char* label = nullptr);
        srhiTexture* CreateTexture2D(uint32_t width, uint32_t height, srhiFormat format, uint32_t usage,
                                     uint32_t mips = 1, const char* label = nullptr);
        srhiResourceSetLayout* LayoutFromShader(srhiShaderModule* module, uint32_t set);

        /// @brief Number of errors logged so far. Tests checking that invalid usage is rejected call
        /// ExpectErrors() afterwards to consume them.
        size_t ErrorCount() const { return mErrors.size(); }
        void ExpectErrors(size_t count);
        const std::vector<std::string>& Errors() const { return mErrors; }

    private:
        static void LogCallback(srhiLogSeverity severity, const char* message, void* userData);

        TestContext& mCtx;
        srhiDevice* mDevice = nullptr;
        srhiDeviceInfo mInfo = {};
        std::vector<std::string> mErrors;
        size_t mExpectedErrors = 0;
        std::vector<srhiShaderModule*> mModules;
        std::vector<std::function<void()>> mOwned;
    };

    /// @brief Float -> half conversion for test data.
    uint16_t FloatToHalf(float f);
    float HalfToFloat(uint16_t h);
} // namespace srhitest

/// @brief Skips the test when the fixture has no device, or a required feature is missing.
#define SRHI_REQUIRE_DEVICE(gpu)                                                                                       \
    do {                                                                                                               \
        if (!(gpu).Valid()) {                                                                                          \
            ctx.Skip("no device for the selected backend");                                                            \
            return;                                                                                                    \
        }                                                                                                              \
    } while (0)

#define SRHI_REQUIRE_FEATURE(gpu, feature)                                                                             \
    do {                                                                                                               \
        if (!(gpu).Info().features.feature) {                                                                          \
            ctx.Skip("feature unsupported: " #feature);                                                                \
            return;                                                                                                    \
        }                                                                                                              \
    } while (0)
