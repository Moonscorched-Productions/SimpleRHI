/**
 * @ Author: Amélie Heinrich
 * @ Create Time: 2026-11-13 00:00:00
 * @ Copyright: Copyright (c) 2026 Amélie Heinrich. All rights reserved.
 */

// Shared example shell: SDL3 window, SimpleRHI device + swapchain, ImGui, fly camera, GPU profiler,
// HDR / vsync / fullscreen toggles, frame pacing.
//
//   class MyExample : public App { ... };
//   SRHI_EXAMPLE_MAIN(MyExample, "My example")
//
// Command line: --backend metal|d3d11|webgpu, --hdr, --no-vsync, --frames N (exit after N frames).

#pragma once

#include "Camera.h"
#include "GpuProfiler.h"

#include <SimpleRHI.h>

#include <glm/glm.hpp>

#include <string>
#include <vector>

struct SDL_Window;

/// @brief How a final pass should encode linear scene color for the current swapchain.
struct OutputParams
{
    /// @brief 0 = sRGB swapchain (write linear, hardware encodes), 1 = scRGB (linear * paperWhite / 80),
    ///        2 = HDR10 (Rec.2020 PQ).
    float mode;
    float paperWhiteNits;
    float maxNits;
    float pad;
};

class App
{
public:
    virtual ~App() = default;

    int Run(int argc, char** argv, const char* title);

    /// @brief Reads a whole file (fetched over HTTP in the browser). Returns false if missing.
    static bool ReadFile(const std::string& path, std::vector<uint8_t>& out);

protected:
    // --- Overridables ---------------------------------------------------------------------------------

    virtual bool OnInit() = 0;
    virtual void OnShutdown() {}
    /// @brief Window size in pixels changed (also called once after OnInit).
    virtual void OnResize(uint32_t width, uint32_t height) { (void)width; (void)height; }
    /// @brief The swapchain format / color space changed (HDR toggle): recreate output pipelines.
    virtual void OnOutputChanged(srhiFormat format) { (void)format; }
    virtual void OnUpdate(float dt) { (void)dt; }
    virtual void OnImGui() {}
    /// @brief Record the frame into cmd, ending with the final image in backbuffer. ImGui is drawn on top
    ///        by the shell afterwards.
    virtual void OnRender(srhiCommandBuffer* cmd, srhiTexture* backbuffer) = 0;

    // --- Helpers ----------------------------------------------------------------------------------------

    /// @brief Loads Content/Cache/Shaders/<name>.srhs (compiled at build time from the example's .slang).
    srhiShaderModule* LoadShader(const char* name);
    srhiResourceSetLayout* LayoutFromShader(srhiShaderModule* module, uint32_t set);
    OutputParams GetOutputParams() const;
    srhiFormat BackbufferFormat() const { return srhiSwapchainGetFormat(mSwapchain); }

    srhiDevice* mDevice = nullptr;
    srhiSwapchain* mSwapchain = nullptr;
    SDL_Window* mWindow = nullptr;
    uint32_t mWidth = 0, mHeight = 0;
    Camera mCamera;
    bool mCameraEnabled = true;
    GpuProfiler mProfiler;
    srhiDeviceInfo mInfo = {};
    float mTime = 0.0f;
    uint64_t mFrameIndex = 0;

private:
    void DrawShellUI();
    void ApplyOutput(srhiColorSpace colorSpace, bool vsync);

    srhiColorSpace mColorSpace = SRHI_COLOR_SPACE_SRGB;
    bool mVsync = true;
    bool mFullscreen = false;
    float mPaperWhite = 200.0f;
    uint32_t mFramesInFlight = 0;
    float mFrameMs = 0.0f;
    std::vector<srhiShaderModule*> mModules;
};

#define SRHI_EXAMPLE_MAIN(AppClass, Title)                                                                             \
    int main(int argc, char** argv)                                                                                    \
    {                                                                                                                  \
        AppClass app;                                                                                                  \
        return app.Run(argc, argv, Title);                                                                             \
    }
