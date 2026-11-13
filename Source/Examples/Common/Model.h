/**
 * @ Author: Amélie Heinrich
 * @ Create Time: 2026-11-13 00:00:00
 * @ Copyright: Copyright (c) 2026 Amélie Heinrich. All rights reserved.
 */

// Loads baked models (.srm) and textures (.srt) from Content/Cache (run srhi_bake first).
// Materials are resource sets for set 1 (see Material.slang).

#pragma once

#include "AssetFormat.h"

#include <SimpleRHI.h>

#include <string>
#include <vector>

/// @brief Loads a baked texture (Content/Cache/Textures/<relativePath>). NULL on failure.
srhiTexture* LoadBakedTexture(srhiDevice* device, const std::string& relativePath);

class Model
{
public:
    enum DrawFilter : uint32_t
    {
        kOpaque = 1 << 0,
        kMasked = 1 << 1,
        kBlended = 1 << 2,
        kAllOpaque = kOpaque | kMasked,
        kAll = kOpaque | kMasked | kBlended,
    };

    /// @brief Vertex layout of asset::SrmVertex in buffer 0 (locations 0..3: position, normal, uv, tangent).
    static srhiVertexLayout VertexLayout();
    /// @brief The set-1 material layout (create once, share between models and pipelines).
    static srhiResourceSetLayout* CreateMaterialLayout(srhiDevice* device);

    /// @brief Loads Content/Cache/Models/<name>.srm and its textures.
    bool Load(srhiDevice* device, const char* name, srhiResourceSetLayout* materialLayout);
    void Destroy();

    /// @brief Binds buffers and issues one draw per submesh matching the filter. With bindMaterials, sets each
    ///        submesh's material set at index 1.
    void Draw(srhiRenderEncoder* encoder, uint32_t filter, bool bindMaterials = true) const;

    const float* BoundsMin() const { return mHeader.boundsMin; }
    const float* BoundsMax() const { return mHeader.boundsMax; }
    uint32_t SubmeshCount() const { return uint32_t(mSubmeshes.size()); }
    uint32_t TriangleCount() const { return mHeader.indexCount / 3; }

private:
    srhiDevice* mDevice = nullptr;
    asset::SrmHeader mHeader = {};
    srhiBuffer* mVertexBuffer = nullptr;
    srhiBuffer* mIndexBuffer = nullptr;
    srhiBuffer* mMaterialBuffer = nullptr;
    srhiSampler* mSampler = nullptr;
    std::vector<asset::SrmSubmesh> mSubmeshes;
    std::vector<asset::SrmMaterial> mMaterials;
    std::vector<srhiResourceSet*> mMaterialSets;
    std::vector<srhiTexture*> mTextures; // index matches the model's texture table, NULL if it failed to load
    srhiTexture* mDefaults[3] = {};      // white, flat normal, black
};
