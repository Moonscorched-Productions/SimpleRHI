/**
 * @ Author: Amélie Heinrich
 * @ Create Time: 2026-11-13 00:00:00
 * @ Copyright: Copyright (c) 2026 Amélie Heinrich. All rights reserved.
 */

#include "App.h"

#include "../../ImGui/imgui_impl_srhi.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>

#include <stb_image_write.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <thread>

namespace
{
    constexpr uint32_t kMaxFramesInFlight = 2;

    void LogCallback(srhiLogSeverity severity, const char* message, void*)
    {
        const char* tag = severity == SRHI_LOG_SEVERITY_ERROR ? "error" : severity == SRHI_LOG_SEVERITY_WARNING ? "warning" : "info";
        fprintf(stderr, "[SimpleRHI %s] %s\n", tag, message);
    }

    const char* ColorSpaceName(srhiColorSpace cs)
    {
        switch (cs) {
        case SRHI_COLOR_SPACE_HDR10_PQ: return "HDR10 (PQ)";
        case SRHI_COLOR_SPACE_EXTENDED_LINEAR_SRGB: return "scRGB (linear)";
        default: return "SDR (sRGB)";
        }
    }
} // namespace

bool App::ReadFile(const std::string& path, std::vector<uint8_t>& out)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

srhiShaderModule* App::LoadShader(const char* name)
{
    std::vector<uint8_t> bytes;
    std::string path = std::string("Content/Cache/Shaders/") + name + ".srhs";
    if (!ReadFile(path, bytes)) {
        fprintf(stderr, "missing shader package %s (build the example target to compile its shaders)\n", path.c_str());
        return nullptr;
    }
    srhiShaderModule* module = srhiShaderModuleCreate(mDevice, bytes.data(), bytes.size(), name);
    if (module)
        mModules.push_back(module);
    return module;
}

srhiResourceSetLayout* App::LayoutFromShader(srhiShaderModule* module, uint32_t set)
{
    uint32_t count = srhiShaderModuleGetResourceSetLayout(module, set, nullptr, 0);
    std::vector<srhiResourceSetLayoutEntry> entries(count);
    srhiShaderModuleGetResourceSetLayout(module, set, entries.data(), count);
    srhiResourceSetLayoutDesc desc = {entries.data(), count, nullptr};
    return srhiResourceSetLayoutCreate(mDevice, &desc);
}

OutputParams App::GetOutputParams() const
{
    OutputParams p = {};
    p.mode = mColorSpace == SRHI_COLOR_SPACE_EXTENDED_LINEAR_SRGB ? 1.0f : mColorSpace == SRHI_COLOR_SPACE_HDR10_PQ ? 2.0f : 0.0f;
    p.paperWhiteNits = mPaperWhite;
    srhiDisplayInfo display;
    srhiSwapchainGetDisplayInfo(mSwapchain, &display);
    p.maxNits = display.maxLuminance > 0.0f ? display.maxLuminance : 1000.0f;
    return p;
}

void App::ApplyOutput(srhiColorSpace colorSpace, bool vsync)
{
    srhiDeviceWaitIdle(mDevice);
    if (!srhiSwapchainConfigure(mSwapchain, colorSpace, vsync)) {
        fprintf(stderr, "swapchain configuration not supported, keeping the previous one\n");
        return;
    }
    mColorSpace = colorSpace;
    mVsync = vsync;
    ImGui_ImplSRHI_SetOutput(BackbufferFormat(), mColorSpace, mPaperWhite);
    OnOutputChanged(BackbufferFormat());
}

void App::DrawShellUI()
{
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(330, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("SimpleRHI");
    ImGui::Text("%s on %s", mInfo.backendName, mInfo.adapterName);
    ImGui::Text("%ux%u  %.2f ms (%.0f fps)", mWidth, mHeight, mFrameMs, mFrameMs > 0 ? 1000.0f / mFrameMs : 0.0f);

    bool vsync = mVsync;
    if (ImGui::Checkbox("VSync", &vsync))
        ApplyOutput(mColorSpace, vsync);
    ImGui::SameLine();
    if (ImGui::Checkbox("Fullscreen (F11)", &mFullscreen))
        SDL_SetWindowFullscreen(mWindow, mFullscreen);

    srhiDisplayInfo display;
    srhiSwapchainGetDisplayInfo(mSwapchain, &display);
    if (ImGui::BeginCombo("Output", ColorSpaceName(mColorSpace))) {
        for (srhiColorSpace cs : {SRHI_COLOR_SPACE_SRGB, SRHI_COLOR_SPACE_EXTENDED_LINEAR_SRGB, SRHI_COLOR_SPACE_HDR10_PQ})
            if (ImGui::Selectable(ColorSpaceName(cs), cs == mColorSpace) && cs != mColorSpace)
                ApplyOutput(cs, mVsync);
        ImGui::EndCombo();
    }
    if (mColorSpace != SRHI_COLOR_SPACE_SRGB && ImGui::SliderFloat("Paper white (nits)", &mPaperWhite, 80.0f, 500.0f))
        ImGui_ImplSRHI_SetOutput(BackbufferFormat(), mColorSpace, mPaperWhite);
    ImGui::TextDisabled("Display: %s, max %.0f nits, EDR headroom %.2f", display.hdrSupported ? "HDR" : "SDR",
                        display.maxLuminance, display.edrHeadroom);

    if (ImGui::CollapsingHeader("GPU timings", ImGuiTreeNodeFlags_DefaultOpen))
        mProfiler.DrawUI();
    if (mCameraEnabled)
        ImGui::TextDisabled("Hold right mouse + WASD/QE to fly, shift to go fast");
    ImGui::End();
}

int App::Run(int argc, char** argv, const char* title)
{
    srhiDeviceDesc deviceDesc = {};
    deviceDesc.log = LogCallback;
    uint64_t maxFrames = 0;
    std::string screenshotPath;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--backend") && i + 1 < argc) {
            const char* b = argv[++i];
            deviceDesc.backend = !strcmp(b, "metal") ? SRHI_BACKEND_METAL : !strcmp(b, "d3d11") ? SRHI_BACKEND_D3D11
                               : !strcmp(b, "webgpu")                     ? SRHI_BACKEND_WEBGPU
                                                                          : SRHI_BACKEND_AUTO;
        } else if (!strcmp(argv[i], "--hdr"))
            mColorSpace = SRHI_COLOR_SPACE_EXTENDED_LINEAR_SRGB;
        else if (!strcmp(argv[i], "--no-vsync"))
            mVsync = false;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            maxFrames = strtoull(argv[++i], nullptr, 10);
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc)
            screenshotPath = argv[++i];
        else if (!strcmp(argv[i], "--debug"))
            deviceDesc.debug = 1;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#if defined(__APPLE__)
    flags |= SDL_WINDOW_METAL;
#endif
    mWindow = SDL_CreateWindow(title, 1600, 900, flags);
    if (!mWindow) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }

    mDevice = srhiDeviceCreate(&deviceDesc);
    if (!mDevice) {
        fprintf(stderr, "no SimpleRHI device for the requested backend\n");
        return 1;
    }
    srhiDeviceGetInfo(mDevice, &mInfo);
    printf("%s: %s on %s\n", title, mInfo.backendName, mInfo.adapterName);

    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(mWindow, &pw, &ph);
    mWidth = uint32_t(pw);
    mHeight = uint32_t(ph);

    srhiSwapchainDesc scDesc = {};
#if defined(__APPLE__)
    SDL_MetalView view = SDL_Metal_CreateView(mWindow);
    scDesc.nativeWindow = SDL_Metal_GetLayer(view);
#elif defined(_WIN32)
    scDesc.nativeWindow = SDL_GetPointerProperty(SDL_GetWindowProperties(mWindow), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#elif defined(__EMSCRIPTEN__)
    scDesc.canvasSelector = "#canvas";
#endif
    scDesc.width = mWidth;
    scDesc.height = mHeight;
    scDesc.colorSpace = mColorSpace;
    scDesc.vsync = mVsync;
    scDesc.label = "swapchain";
    mSwapchain = srhiSwapchainCreate(mDevice, &scDesc);
    if (!mSwapchain && mColorSpace != SRHI_COLOR_SPACE_SRGB) {
        mColorSpace = scDesc.colorSpace = SRHI_COLOR_SPACE_SRGB;
        mSwapchain = srhiSwapchainCreate(mDevice, &scDesc);
    }
    if (!mSwapchain) {
        fprintf(stderr, "swapchain creation failed\n");
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForOther(mWindow);
    ImGui_ImplSRHI_InitInfo imguiInfo;
    imguiInfo.Device = mDevice;
    imguiInfo.ColorFormat = BackbufferFormat();
    imguiInfo.ColorSpace = mColorSpace;
    imguiInfo.PaperWhiteNits = mPaperWhite;
    ImGui_ImplSRHI_Init(&imguiInfo);

    mProfiler.Init(mDevice);
    mCamera.aspect = float(mWidth) / float(mHeight);

    if (!OnInit()) {
        fprintf(stderr, "%s: initialization failed\n", title);
        return 1;
    }
    OnResize(mWidth, mHeight);

    auto last = std::chrono::steady_clock::now();
    bool running = true;
    bool mouseCaptured = false;
    while (running && (maxFrames == 0 || mFrameIndex < maxFrames)) {
        float mouseDx = 0.0f, mouseDy = 0.0f;
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            switch (event.type) {
            case SDL_EVENT_QUIT: running = false; break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED: running = false; break;
            case SDL_EVENT_KEY_DOWN:
                if (event.key.scancode == SDL_SCANCODE_ESCAPE)
                    running = false;
                if (event.key.scancode == SDL_SCANCODE_F11) {
                    mFullscreen = !mFullscreen;
                    SDL_SetWindowFullscreen(mWindow, mFullscreen);
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (event.button.button == SDL_BUTTON_RIGHT && mCameraEnabled && !io.WantCaptureMouse) {
                    mouseCaptured = true;
                    SDL_SetWindowRelativeMouseMode(mWindow, true);
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (event.button.button == SDL_BUTTON_RIGHT && mouseCaptured) {
                    mouseCaptured = false;
                    SDL_SetWindowRelativeMouseMode(mWindow, false);
                }
                break;
            case SDL_EVENT_MOUSE_MOTION:
                if (mouseCaptured) {
                    mouseDx += event.motion.xrel;
                    mouseDy += event.motion.yrel;
                }
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
                SDL_GetWindowSizeInPixels(mWindow, &pw, &ph);
                if (pw > 0 && ph > 0 && (uint32_t(pw) != mWidth || uint32_t(ph) != mHeight)) {
                    srhiDeviceWaitIdle(mDevice);
                    mWidth = uint32_t(pw);
                    mHeight = uint32_t(ph);
                    srhiSwapchainResize(mSwapchain, mWidth, mHeight);
                    mCamera.aspect = float(mWidth) / float(mHeight);
                    OnResize(mWidth, mHeight);
                }
                break;
            }
            default: break;
            }
        }

        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        mTime += dt;
        mFrameMs = mFrameMs * 0.95f + dt * 1000.0f * 0.05f;

        // Frame pacing: at most kMaxFramesInFlight command buffers queued on the GPU.
        srhiDevicePoll(mDevice);
        while (mFramesInFlight >= kMaxFramesInFlight) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            srhiDevicePoll(mDevice);
        }

        if (mouseCaptured)
            mCamera.Update(SDL_GetKeyboardState(nullptr), mouseDx, mouseDy, dt);

        ImGui_ImplSRHI_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        OnUpdate(dt);
        DrawShellUI();
        OnImGui();
        ImGui::Render();

        srhiTexture* backbuffer = srhiSwapchainAcquire(mSwapchain);
        if (!backbuffer) {
            SDL_Delay(16);
            continue;
        }

        mProfiler.BeginFrame();
        srhiCommandBuffer* cmd = srhiCommandBufferBegin(mDevice, "frame");
        OnRender(cmd, backbuffer);

        srhiRenderPassDesc ui = {};
        ui.colorAttachmentCount = 1;
        ui.colorAttachments[0].view = srhiTextureGetDefaultView(backbuffer);
        ui.colorAttachments[0].loadAction = SRHI_LOAD_ACTION_LOAD;
        ui.colorAttachments[0].storeAction = SRHI_STORE_ACTION_STORE;
        ui.timestampWrites = mProfiler.Scope("ImGui");
        ui.label = "ImGui";
        srhiRenderEncoder* enc = srhiRenderEncoderBegin(cmd, &ui);
        ImGui_ImplSRHI_RenderDrawData(ImGui::GetDrawData(), enc);
        srhiRenderEncoderEnd(enc);

        // --screenshot: capture the final frame of a --frames run (8-bit swapchains only).
        struct Shot
        {
            std::string path;
            uint32_t w, h;
            srhiFormat format;
        } shot = {screenshotPath, mWidth, mHeight, BackbufferFormat()};
        bool lastFrame = maxFrames && mFrameIndex + 1 == maxFrames;
        if (lastFrame && !screenshotPath.empty())
            srhiCommandBufferReadTexture(cmd, backbuffer, 0, 0,
                [](const void* data, uint64_t, void* user) {
                    Shot* s = static_cast<Shot*>(user);
                    std::vector<uint8_t> rgba(size_t(s->w) * s->h * 4);
                    const uint8_t* src = static_cast<const uint8_t*>(data);
                    bool bgra = s->format == SRHI_FORMAT_BGRA8_UNORM || s->format == SRHI_FORMAT_BGRA8_UNORM_SRGB;
                    for (size_t i = 0; i < size_t(s->w) * s->h; ++i) {
                        rgba[i * 4 + 0] = src[i * 4 + (bgra ? 2 : 0)];
                        rgba[i * 4 + 1] = src[i * 4 + 1];
                        rgba[i * 4 + 2] = src[i * 4 + (bgra ? 0 : 2)];
                        rgba[i * 4 + 3] = 255;
                    }
                    stbi_write_png(s->path.c_str(), int(s->w), int(s->h), 4, rgba.data(), int(s->w) * 4);
                    printf("wrote %s\n", s->path.c_str());
                },
                &shot);

        srhiCommandBufferPresent(cmd, mSwapchain);
        ++mFramesInFlight;
        srhiCommandBufferCommit(cmd, [](void* user) { --*static_cast<uint32_t*>(user); }, &mFramesInFlight);
        if (lastFrame)
            srhiDeviceWaitIdle(mDevice); // fire the screenshot callback while `shot` is alive
        ++mFrameIndex;
    }

    srhiDeviceWaitIdle(mDevice);
    OnShutdown();
    for (srhiShaderModule* m : mModules)
        srhiShaderModuleDestroy(m);
    mProfiler.Shutdown();
    ImGui_ImplSRHI_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    srhiSwapchainDestroy(mSwapchain);
    srhiDeviceDestroy(mDevice);
#if defined(__APPLE__)
    SDL_Metal_DestroyView(view);
#endif
    SDL_DestroyWindow(mWindow);
    SDL_Quit();
    return 0;
}
