/**
 * @ Author: Amélie Heinrich
 * @ Create Time: 2026-11-13 00:00:00
 * @ Copyright: Copyright (c) 2026 Amélie Heinrich. All rights reserved.
 */

#include "imgui_impl_srhi.h"
#ifndef IMGUI_DISABLE

#include "imgui_impl_srhi_shader.h"

#include <cstring>
#include <unordered_map>
#include <vector>

namespace
{
    struct TextureEntry
    {
        srhiTexture* texture = nullptr; // Owned for ImGui-managed textures (fonts), NULL for user views.
        srhiResourceSet* set = nullptr;
    };

    struct BackendData
    {
        ImGui_ImplSRHI_InitInfo info;
        srhiShaderModule* module = nullptr;
        srhiResourceSetLayout* layout = nullptr;
        srhiRenderPipeline* pipeline = nullptr;
        srhiSampler* sampler = nullptr;
        srhiBuffer* vertexBuffer = nullptr;
        srhiBuffer* indexBuffer = nullptr;
        uint64_t vertexCapacity = 0;
        uint64_t indexCapacity = 0;
        std::unordered_map<srhiTextureView*, TextureEntry> textures;
    };

    BackendData* GetData()
    {
        return ImGui::GetCurrentContext() ? static_cast<BackendData*>(ImGui::GetIO().BackendRendererUserData) : nullptr;
    }

    bool CreatePipeline(BackendData* bd)
    {
        if (bd->pipeline)
            srhiRenderPipelineDestroy(bd->pipeline);
        srhiRenderPipelineDesc desc = {};
        desc.module = bd->module;
        desc.vertexEntry = "vsMain";
        desc.fragmentEntry = "fsMain";
        desc.vertexLayout.bufferCount = 1;
        desc.vertexLayout.buffers[0].stride = sizeof(ImDrawVert);
        desc.vertexLayout.attributeCount = 3;
        desc.vertexLayout.attributes[0] = {0, SRHI_VERTEX_FORMAT_FLOAT2, (uint32_t)offsetof(ImDrawVert, pos), 0};
        desc.vertexLayout.attributes[1] = {1, SRHI_VERTEX_FORMAT_FLOAT2, (uint32_t)offsetof(ImDrawVert, uv), 0};
        desc.vertexLayout.attributes[2] = {2, SRHI_VERTEX_FORMAT_UBYTE4_NORM, (uint32_t)offsetof(ImDrawVert, col), 0};
        desc.colorTargetCount = 1;
        desc.colorTargets[0].format = bd->info.ColorFormat;
        desc.colorTargets[0].blend = {1,
                                      SRHI_BLEND_FACTOR_SRC_ALPHA,
                                      SRHI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                      SRHI_BLEND_OP_ADD,
                                      SRHI_BLEND_FACTOR_ONE,
                                      SRHI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                      SRHI_BLEND_OP_ADD};
        desc.depthStencil.format = bd->info.DepthFormat;
        desc.sampleCount = bd->info.SampleCount;
        desc.setLayouts[0] = bd->layout;
        desc.label = "ImGui";
        bd->pipeline = srhiRenderPipelineCreate(bd->info.Device, &desc);
        return bd->pipeline != nullptr;
    }

    srhiResourceSet* SetForView(BackendData* bd, srhiTextureView* view)
    {
        TextureEntry& entry = bd->textures[view];
        if (!entry.set) {
            srhiResourceBinding bindings[2] = {};
            bindings[0].binding = 0;
            bindings[0].textureView = view;
            bindings[1].binding = 1;
            bindings[1].sampler = bd->sampler;
            srhiResourceSetDesc desc = {bd->layout, bindings, 2, "ImGui texture"};
            entry.set = srhiResourceSetCreate(bd->info.Device, &desc);
        }
        return entry.set;
    }

    void DestroyEntry(TextureEntry& entry)
    {
        if (entry.set)
            srhiResourceSetDestroy(entry.set);
        if (entry.texture)
            srhiTextureDestroy(entry.texture);
        entry = {};
    }

    void UpdateTexture(BackendData* bd, ImTextureData* tex)
    {
        srhiDevice* device = bd->info.Device;
        if (tex->Status == ImTextureStatus_WantCreate) {
            IM_ASSERT(tex->Format == ImTextureFormat_RGBA32);
            srhiTextureDesc desc = {};
            desc.format = SRHI_FORMAT_RGBA8_UNORM;
            desc.width = uint32_t(tex->Width);
            desc.height = uint32_t(tex->Height);
            desc.usage = SRHI_TEXTURE_USAGE_SAMPLED;
            desc.label = "ImGui texture";
            srhiTexture* texture = srhiTextureCreate(device, &desc);
            srhiTextureWrite(device, texture, 0, 0, nullptr, tex->GetPixels(), uint32_t(tex->GetPitch()));
            srhiTextureView* view = srhiTextureGetDefaultView(texture);
            bd->textures[view].texture = texture;
            tex->SetTexID((ImTextureID)(intptr_t)view);
            tex->SetStatus(ImTextureStatus_OK);
        } else if (tex->Status == ImTextureStatus_WantUpdates) {
            srhiTextureView* view = (srhiTextureView*)(intptr_t)tex->GetTexID();
            srhiTexture* texture = bd->textures[view].texture;
            for (const ImTextureRect& r : tex->Updates) {
                srhiTextureRegion region = {r.x, r.y, 0, r.w, r.h, 1};
                srhiTextureWrite(device, texture, 0, 0, &region, tex->GetPixelsAt(r.x, r.y), uint32_t(tex->GetPitch()));
            }
            tex->SetStatus(ImTextureStatus_OK);
        } else if (tex->Status == ImTextureStatus_WantDestroy) {
            // Destroying after commit is safe: in-flight command buffers keep the resources alive.
            srhiTextureView* view = (srhiTextureView*)(intptr_t)tex->GetTexID();
            auto it = bd->textures.find(view);
            if (it != bd->textures.end()) {
                DestroyEntry(it->second);
                bd->textures.erase(it);
            }
            tex->SetTexID(ImTextureID_Invalid);
            tex->SetStatus(ImTextureStatus_Destroyed);
        }
    }

    float OutputMode(srhiFormat format, srhiColorSpace colorSpace)
    {
        if (format == SRHI_FORMAT_RGBA8_UNORM_SRGB || format == SRHI_FORMAT_BGRA8_UNORM_SRGB)
            return 1.0f;
        if (colorSpace == SRHI_COLOR_SPACE_EXTENDED_LINEAR_SRGB)
            return 2.0f;
        if (colorSpace == SRHI_COLOR_SPACE_HDR10_PQ)
            return 3.0f;
        return 0.0f;
    }
} // namespace

bool ImGui_ImplSRHI_Init(const ImGui_ImplSRHI_InitInfo* info)
{
    ImGuiIO& io = ImGui::GetIO();
    IMGUI_CHECKVERSION();
    IM_ASSERT(io.BackendRendererUserData == nullptr && "Already initialized a renderer backend!");
    IM_ASSERT(info && info->Device && info->ColorFormat != SRHI_FORMAT_UNDEFINED);

    BackendData* bd = IM_NEW(BackendData)();
    bd->info = *info;
    io.BackendRendererUserData = bd;
    io.BackendRendererName = "imgui_impl_srhi";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;

    srhiDevice* device = info->Device;
    bd->module = srhiShaderModuleCreate(device, g_imgui_srhi_shader, g_imgui_srhi_shader_size, "ImGui");
    srhiResourceSetLayoutEntry entries[2] = {};
    entries[0].binding = 0;
    entries[0].type = SRHI_BINDING_TYPE_TEXTURE;
    entries[0].stages = SRHI_SHADER_STAGE_FRAGMENT;
    entries[1].binding = 1;
    entries[1].type = SRHI_BINDING_TYPE_SAMPLER;
    entries[1].stages = SRHI_SHADER_STAGE_FRAGMENT;
    srhiResourceSetLayoutDesc layoutDesc = {entries, 2, "ImGui"};
    bd->layout = srhiResourceSetLayoutCreate(device, &layoutDesc);

    srhiSamplerDesc samplerDesc = {};
    samplerDesc.minFilter = samplerDesc.magFilter = samplerDesc.mipFilter = SRHI_FILTER_LINEAR;
    samplerDesc.addressU = samplerDesc.addressV = samplerDesc.addressW = SRHI_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerDesc.label = "ImGui";
    bd->sampler = srhiSamplerCreate(device, &samplerDesc);

    return bd->module && bd->layout && bd->sampler && CreatePipeline(bd);
}

void ImGui_ImplSRHI_Shutdown()
{
    BackendData* bd = GetData();
    IM_ASSERT(bd && "No renderer backend to shutdown, or already shutdown?");
    ImGuiIO& io = ImGui::GetIO();

    srhiDeviceWaitIdle(bd->info.Device);
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures)
        if (tex->RefCount == 1) {
            tex->SetTexID(ImTextureID_Invalid);
            tex->SetStatus(ImTextureStatus_Destroyed);
        }
    for (auto& [view, entry] : bd->textures)
        DestroyEntry(entry);
    srhiBufferDestroy(bd->vertexBuffer);
    srhiBufferDestroy(bd->indexBuffer);
    srhiRenderPipelineDestroy(bd->pipeline);
    srhiSamplerDestroy(bd->sampler);
    srhiResourceSetLayoutDestroy(bd->layout);
    srhiShaderModuleDestroy(bd->module);

    io.BackendRendererName = nullptr;
    io.BackendRendererUserData = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
    IM_DELETE(bd);
}

void ImGui_ImplSRHI_NewFrame()
{
    IM_ASSERT(GetData() && "Did you call ImGui_ImplSRHI_Init()?");
}

void ImGui_ImplSRHI_SetOutput(srhiFormat colorFormat, srhiColorSpace colorSpace, float paperWhiteNits)
{
    BackendData* bd = GetData();
    bool rebuild = bd->info.ColorFormat != colorFormat;
    bd->info.ColorFormat = colorFormat;
    bd->info.ColorSpace = colorSpace;
    bd->info.PaperWhiteNits = paperWhiteNits;
    if (rebuild)
        CreatePipeline(bd);
}

ImTextureID ImGui_ImplSRHI_TextureID(srhiTextureView* view) { return (ImTextureID)(intptr_t)view; }

void ImGui_ImplSRHI_ForgetTexture(srhiTextureView* view)
{
    BackendData* bd = GetData();
    auto it = bd->textures.find(view);
    if (it != bd->textures.end() && !it->second.texture) {
        DestroyEntry(it->second);
        bd->textures.erase(it);
    }
}

void ImGui_ImplSRHI_RenderDrawData(ImDrawData* drawData, srhiRenderEncoder* encoder)
{
    BackendData* bd = GetData();
    const float fbWidth = drawData->DisplaySize.x * drawData->FramebufferScale.x;
    const float fbHeight = drawData->DisplaySize.y * drawData->FramebufferScale.y;
    if (fbWidth <= 0.0f || fbHeight <= 0.0f || !bd->pipeline)
        return;

    if (drawData->Textures)
        for (ImTextureData* tex : *drawData->Textures)
            if (tex->Status != ImTextureStatus_OK)
                UpdateTexture(bd, tex);
    if (drawData->TotalVtxCount == 0)
        return;

    srhiDevice* device = bd->info.Device;

    // Grow and fill the shared vertex / index buffers. Writes are queue-ordered, so reusing one buffer
    // every frame is safe even while the previous frame is still executing.
    const uint64_t vtxBytes = ((uint64_t)drawData->TotalVtxCount * sizeof(ImDrawVert) + 3) & ~3ull;
    const uint64_t idxBytes = ((uint64_t)drawData->TotalIdxCount * sizeof(ImDrawIdx) + 3) & ~3ull;
    if (vtxBytes > bd->vertexCapacity) {
        srhiBufferDestroy(bd->vertexBuffer);
        bd->vertexCapacity = vtxBytes + 5000 * sizeof(ImDrawVert);
        srhiBufferDesc desc = {bd->vertexCapacity, SRHI_BUFFER_USAGE_VERTEX, 0, "ImGui vertices"};
        bd->vertexBuffer = srhiBufferCreate(device, &desc);
    }
    if (idxBytes > bd->indexCapacity) {
        srhiBufferDestroy(bd->indexBuffer);
        bd->indexCapacity = idxBytes + 10000 * sizeof(ImDrawIdx);
        srhiBufferDesc desc = {bd->indexCapacity, SRHI_BUFFER_USAGE_INDEX, 0, "ImGui indices"};
        bd->indexBuffer = srhiBufferCreate(device, &desc);
    }
    {
        std::vector<ImDrawVert> vertices;
        std::vector<ImDrawIdx> indices;
        vertices.reserve(drawData->TotalVtxCount + 1);
        indices.reserve(drawData->TotalIdxCount + 2);
        for (const ImDrawList* list : drawData->CmdLists) {
            vertices.insert(vertices.end(), list->VtxBuffer.Data, list->VtxBuffer.Data + list->VtxBuffer.Size);
            indices.insert(indices.end(), list->IdxBuffer.Data, list->IdxBuffer.Data + list->IdxBuffer.Size);
        }
        while ((indices.size() * sizeof(ImDrawIdx)) % 4)
            indices.push_back(0);
        srhiBufferWrite(device, bd->vertexBuffer, 0, vertices.data(), (uint64_t)vertices.size() * sizeof(ImDrawVert));
        srhiBufferWrite(device, bd->indexBuffer, 0, indices.data(), (uint64_t)indices.size() * sizeof(ImDrawIdx));
    }

    struct
    {
        float scaleTranslate[4];
        float output[4];
    } params;
    const float L = drawData->DisplayPos.x, R = drawData->DisplayPos.x + drawData->DisplaySize.x;
    const float T = drawData->DisplayPos.y, B = drawData->DisplayPos.y + drawData->DisplaySize.y;
    params.scaleTranslate[0] = 2.0f / (R - L);
    params.scaleTranslate[1] = 2.0f / (T - B);
    params.scaleTranslate[2] = (R + L) / (L - R);
    params.scaleTranslate[3] = (T + B) / (B - T);
    params.output[0] = OutputMode(bd->info.ColorFormat, bd->info.ColorSpace);
    params.output[1] = bd->info.PaperWhiteNits;
    params.output[2] = params.output[3] = 0.0f;

    auto setupState = [&] {
        srhiRenderEncoderSetPipeline(encoder, bd->pipeline);
        srhiRenderEncoderSetVertexBuffer(encoder, 0, bd->vertexBuffer, 0);
        srhiRenderEncoderSetIndexBuffer(encoder, bd->indexBuffer, sizeof(ImDrawIdx) == 2 ? SRHI_INDEX_TYPE_UINT16 : SRHI_INDEX_TYPE_UINT32, 0);
        srhiViewport viewport = {0, 0, fbWidth, fbHeight, 0, 1};
        srhiRenderEncoderSetViewport(encoder, &viewport);
        srhiRenderEncoderSetUniforms(encoder, 0, &params, sizeof(params));
    };
    srhiRenderEncoderPushDebugGroup(encoder, "ImGui");
    setupState();

    const ImVec2 clipOff = drawData->DisplayPos;
    const ImVec2 clipScale = drawData->FramebufferScale;
    uint32_t globalVtx = 0, globalIdx = 0;
    srhiResourceSet* boundSet = nullptr;
    for (const ImDrawList* list : drawData->CmdLists) {
        for (const ImDrawCmd& cmd : list->CmdBuffer) {
            if (cmd.UserCallback) {
                if (cmd.UserCallback == ImDrawCallback_ResetRenderState) {
                    setupState();
                    boundSet = nullptr;
                } else
                    cmd.UserCallback(list, &cmd);
                continue;
            }
            ImVec2 clipMin((cmd.ClipRect.x - clipOff.x) * clipScale.x, (cmd.ClipRect.y - clipOff.y) * clipScale.y);
            ImVec2 clipMax((cmd.ClipRect.z - clipOff.x) * clipScale.x, (cmd.ClipRect.w - clipOff.y) * clipScale.y);
            clipMin.x = clipMin.x < 0 ? 0 : clipMin.x;
            clipMin.y = clipMin.y < 0 ? 0 : clipMin.y;
            clipMax.x = clipMax.x > fbWidth ? fbWidth : clipMax.x;
            clipMax.y = clipMax.y > fbHeight ? fbHeight : clipMax.y;
            if (clipMax.x <= clipMin.x || clipMax.y <= clipMin.y)
                continue;
            srhiScissorRect scissor = {(uint32_t)clipMin.x, (uint32_t)clipMin.y, (uint32_t)(clipMax.x - clipMin.x),
                                       (uint32_t)(clipMax.y - clipMin.y)};
            srhiRenderEncoderSetScissor(encoder, &scissor);

            srhiResourceSet* set = SetForView(bd, (srhiTextureView*)(intptr_t)cmd.GetTexID());
            if (set != boundSet) {
                srhiRenderEncoderSetResourceSet(encoder, 0, set);
                boundSet = set;
            }
            srhiRenderEncoderDrawIndexed(encoder, cmd.ElemCount, 1, cmd.IdxOffset + globalIdx, int32_t(cmd.VtxOffset + globalVtx), 0);
        }
        globalIdx += list->IdxBuffer.Size;
        globalVtx += list->VtxBuffer.Size;
    }
    srhiRenderEncoderPopDebugGroup(encoder);
}

#endif // #ifndef IMGUI_DISABLE
