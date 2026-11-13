/**
 * @ Author: Amélie Heinrich
 * @ Create Time: 2026-11-13 00:00:00
 * @ Copyright: Copyright (c) 2026 Amélie Heinrich. All rights reserved.
 */

// dear imgui: Renderer Backend for SimpleRHI (D3D11 / Metal 3 / WebGPU).
// Use with a platform backend (e.g. imgui_impl_sdl3).
//
// Implemented features:
//  [X] Renderer: user textures. Pass ImGui_ImplSRHI_TextureID(view) as the ImTextureID.
//  [X] Renderer: large meshes (64k+ vertices) via ImGuiBackendFlags_RendererHasVtxOffset.
//  [X] Renderer: texture updates (ImGuiBackendFlags_RendererHasTextures).
//  [X] Renderer: correct output on sRGB, scRGB and HDR10 swapchains (see ImGui_ImplSRHI_SetOutput).
//  [ ] Renderer: multi-viewport.
//
// The shader is embedded (imgui_impl_srhi_shader.h), so no shader files are needed at runtime.

#pragma once
#include "imgui.h"
#ifndef IMGUI_DISABLE

#include <SimpleRHI.h>

struct ImGui_ImplSRHI_InitInfo
{
    srhiDevice* Device = nullptr;
    /// @brief Format of the color attachment ImGui renders into (e.g. srhiSwapchainGetFormat()).
    srhiFormat ColorFormat = SRHI_FORMAT_UNDEFINED;
    /// @brief Depth attachment of that pass, if any (ImGui does not test or write it).
    srhiFormat DepthFormat = SRHI_FORMAT_UNDEFINED;
    uint32_t SampleCount = 1;
    /// @brief How the target interprets values; drives the color conversion in the shader.
    srhiColorSpace ColorSpace = SRHI_COLOR_SPACE_SRGB;
    /// @brief Brightness of ImGui's white on HDR outputs, in nits.
    float PaperWhiteNits = 200.0f;
};

IMGUI_IMPL_API bool ImGui_ImplSRHI_Init(const ImGui_ImplSRHI_InitInfo* info);
IMGUI_IMPL_API void ImGui_ImplSRHI_Shutdown();
IMGUI_IMPL_API void ImGui_ImplSRHI_NewFrame();

/// @brief Records ImGui's draw data into an open render encoder whose attachments match the init info.
IMGUI_IMPL_API void ImGui_ImplSRHI_RenderDrawData(ImDrawData* drawData, srhiRenderEncoder* encoder);

/// @brief Call when the swapchain format / color space changes (e.g. toggling HDR).
IMGUI_IMPL_API void ImGui_ImplSRHI_SetOutput(srhiFormat colorFormat, srhiColorSpace colorSpace, float paperWhiteNits);

/// @brief ImTextureID for a texture view. The view must be a 2D view of a sampled float texture.
IMGUI_IMPL_API ImTextureID ImGui_ImplSRHI_TextureID(srhiTextureView* view);
/// @brief Drops the cached resource set for a view. Call before destroying a view used with ImGui.
IMGUI_IMPL_API void ImGui_ImplSRHI_ForgetTexture(srhiTextureView* view);

#endif // #ifndef IMGUI_DISABLE
