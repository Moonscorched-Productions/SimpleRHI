// Minimal SimpleRHI program: SDL3 window, swapchain, one pipeline, transient uniforms.
// Usage: example [frameCount]   (frameCount: exit after that many frames, for smoke tests)

#include "SimpleRHI.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static void Log(srhiLogSeverity severity, const char* message, void*)
{
    const char* tag = severity == SRHI_LOG_SEVERITY_ERROR ? "error" : severity == SRHI_LOG_SEVERITY_WARNING ? "warning" : "info";
    fprintf(stderr, "[SimpleRHI %s] %s\n", tag, message);
}

static std::vector<uint8_t> ReadFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

struct Vertex
{
    float x, y;
    float r, g, b;
};

int main(int argc, char** argv)
{
    const unsigned long long maxFrames = argc > 1 ? strtoull(argv[1], nullptr, 10) : 0;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#if defined(__APPLE__)
    flags |= SDL_WINDOW_METAL;
#endif
    SDL_Window* window = SDL_CreateWindow("SimpleRHI example", 1280, 720, flags);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }

    // --- Device ---------------------------------------------------------------------------------------
    srhiDeviceDesc deviceDesc = {};
    deviceDesc.backend = SRHI_BACKEND_AUTO; // D3D11 on Windows, Metal on Apple platforms
    deviceDesc.log = Log;
#if !defined(NDEBUG)
    deviceDesc.debug = 1; // API validation: every mistake is reported through the log callback
#endif
    srhiDevice* device = srhiDeviceCreate(&deviceDesc);
    if (!device)
        return 1;
    srhiDeviceInfo info;
    srhiDeviceGetInfo(device, &info);
    printf("%s on %s\n", info.backendName, info.adapterName);

    // --- Swapchain ------------------------------------------------------------------------------------
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(window, &pw, &ph);
    uint32_t width = uint32_t(pw), height = uint32_t(ph);

    srhiSwapchainDesc swapchainDesc = {};
#if defined(__APPLE__)
    SDL_MetalView metalView = SDL_Metal_CreateView(window);
    swapchainDesc.nativeWindow = SDL_Metal_GetLayer(metalView); // CAMetalLayer*
#elif defined(_WIN32)
    swapchainDesc.nativeWindow =
        SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr); // HWND
#endif
    swapchainDesc.width = width;
    swapchainDesc.height = height;
    swapchainDesc.colorSpace = SRHI_COLOR_SPACE_SRGB;
    swapchainDesc.vsync = 1;
    srhiSwapchain* swapchain = srhiSwapchainCreate(device, &swapchainDesc);
    if (!swapchain)
        return 1;

    // --- Shader, pipeline, vertex buffer --------------------------------------------------------------
    // triangle.srhs is compiled from triangle.slang by srhic at build time and placed next to the executable.
    std::vector<uint8_t> package = ReadFile(std::string(SDL_GetBasePath()) + "triangle.srhs");
    srhiShaderModule* module = srhiShaderModuleCreate(device, package.data(), package.size(), "triangle");
    if (!module)
        return 1;

    srhiRenderPipelineDesc pipelineDesc = {}; // zero means: triangle list, no culling, no depth, no blending
    pipelineDesc.module = module;
    pipelineDesc.vertexEntry = "vsMain";
    pipelineDesc.fragmentEntry = "fsMain";
    pipelineDesc.vertexLayout.bufferCount = 1;
    pipelineDesc.vertexLayout.buffers[0].stride = sizeof(Vertex);
    pipelineDesc.vertexLayout.attributeCount = 2;
    pipelineDesc.vertexLayout.attributes[0] = {0, SRHI_VERTEX_FORMAT_FLOAT2, 0, 0}; // location, format, offset, buffer
    pipelineDesc.vertexLayout.attributes[1] = {1, SRHI_VERTEX_FORMAT_FLOAT3, 8, 0};
    pipelineDesc.colorTargetCount = 1;
    pipelineDesc.colorTargets[0].format = srhiSwapchainGetFormat(swapchain);
    pipelineDesc.label = "triangle";
    srhiRenderPipeline* pipeline = srhiRenderPipelineCreate(device, &pipelineDesc);
    if (!pipeline)
        return 1;

    const Vertex vertices[3] = {
        {0.0f, 0.7f, 1.0f, 0.2f, 0.2f},
        {-0.6f, -0.45f, 0.2f, 1.0f, 0.2f},
        {0.6f, -0.45f, 0.2f, 0.3f, 1.0f},
    };
    srhiBufferDesc vertexBufferDesc = {sizeof(vertices), SRHI_BUFFER_USAGE_VERTEX, 0, "triangle vertices"};
    srhiBuffer* vertexBuffer = srhiBufferCreate(device, &vertexBufferDesc);
    srhiBufferWrite(device, vertexBuffer, 0, vertices, sizeof(vertices));

    // --- Frame loop -----------------------------------------------------------------------------------
    uint32_t framesInFlight = 0; // decremented by the completion callback, fired from srhiDevicePoll
    unsigned long long frame = 0;
    bool running = true;
    while (running && (maxFrames == 0 || frame < maxFrames)) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                running = false;
            if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                SDL_GetWindowSizeInPixels(window, &pw, &ph);
                if (pw > 0 && ph > 0 && (uint32_t(pw) != width || uint32_t(ph) != height)) {
                    width = uint32_t(pw);
                    height = uint32_t(ph);
                    srhiDeviceWaitIdle(device);
                    srhiSwapchainResize(swapchain, width, height);
                }
            }
        }

        // Frame pacing: never queue more than two frames on the GPU.
        srhiDevicePoll(device);
        while (framesInFlight >= 2) {
            SDL_Delay(1);
            srhiDevicePoll(device);
        }

        srhiTexture* backbuffer = srhiSwapchainAcquire(swapchain);
        if (!backbuffer) { // minimized or occluded
            SDL_Delay(16);
            continue;
        }

        srhiCommandBuffer* cmd = srhiCommandBufferBegin(device, "frame");

        srhiRenderPassDesc pass = {};
        pass.colorAttachmentCount = 1;
        pass.colorAttachments[0].view = srhiTextureGetDefaultView(backbuffer);
        pass.colorAttachments[0].loadAction = SRHI_LOAD_ACTION_CLEAR;
        pass.colorAttachments[0].storeAction = SRHI_STORE_ACTION_STORE;
        pass.colorAttachments[0].clearColor[0] = 0.02f;
        pass.colorAttachments[0].clearColor[1] = 0.02f;
        pass.colorAttachments[0].clearColor[2] = 0.03f;
        pass.colorAttachments[0].clearColor[3] = 1.0f;
        pass.label = "main";

        const float angle = float(SDL_GetTicks()) * 0.001f;
        const float params[4] = {std::cos(angle), std::sin(angle), float(width) / float(height), 0.0f};

        srhiRenderEncoder* enc = srhiRenderEncoderBegin(cmd, &pass);
        srhiRenderEncoderSetPipeline(enc, pipeline);
        srhiRenderEncoderSetVertexBuffer(enc, 0, vertexBuffer, 0);
        srhiRenderEncoderSetUniforms(enc, 0, params, sizeof(params)); // set 3, slot 0: gParams
        srhiRenderEncoderDraw(enc, 3, 1, 0, 0);
        srhiRenderEncoderEnd(enc);

        srhiCommandBufferPresent(cmd, swapchain); // last, outside any encoder
        ++framesInFlight;
        srhiCommandBufferCommit(cmd, [](void* user) { --*static_cast<uint32_t*>(user); }, &framesInFlight);
        ++frame;
    }

    // --- Shutdown: wait for the GPU, then destroy every object before the device ----------------------
    srhiDeviceWaitIdle(device);
    srhiBufferDestroy(vertexBuffer);
    srhiRenderPipelineDestroy(pipeline);
    srhiShaderModuleDestroy(module);
    srhiSwapchainDestroy(swapchain);
    srhiDeviceDestroy(device);
#if defined(__APPLE__)
    SDL_Metal_DestroyView(metalView);
#endif
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
