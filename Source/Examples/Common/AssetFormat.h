/**
 * @ Author: Amélie Heinrich
 * @ Create Time: 2026-11-13 00:00:00
 * @ Copyright: Copyright (c) 2026 Amélie Heinrich. All rights reserved.
 */

// Baked asset formats written by srhi_bake (Source/Tools/AssetBaker) into Content/Cache.
// Little-endian, tightly packed PODs.
//
// .srt (texture):  SrtHeader, then mipCount x { uint64 size, bytes } (rows of 4x4 blocks, tightly packed).
// .srm (model):    SrmHeader, SrmVertex[vertexCount], uint32 indices[indexCount], SrmSubmesh[submeshCount],
//                  SrmMaterial[materialCount], then textureCount x char[128] texture paths relative to
//                  Content/Cache/Textures (e.g. "IntelSponza/brickwall_01_BaseColor.bc7s.srt").

#pragma once

#include <cstdint>

namespace asset
{
    constexpr uint32_t kSrtMagic = 0x31545253; // 'SRT1'
    constexpr uint32_t kSrmMagic = 0x314D5253; // 'SRM1'
    constexpr uint32_t kSrmVersion = 1;
    constexpr int32_t kNoTexture = -1;

    struct SrtHeader
    {
        uint32_t magic;
        uint32_t format; ///< srhiFormat
        uint32_t width;
        uint32_t height;
        uint32_t mipCount;
        uint32_t reserved[3];
    };

    struct SrmVertex
    {
        float position[3];
        float normal[3];
        float uv[2];
        float tangent[4]; ///< xyz tangent, w bitangent sign
    };
    static_assert(sizeof(SrmVertex) == 48, "SrmVertex layout");

    struct SrmSubmesh
    {
        uint32_t indexOffset;
        uint32_t indexCount;
        uint32_t materialIndex;
        uint32_t pad;
        float boundsMin[4];
        float boundsMax[4];
    };

    enum AlphaMode : uint32_t
    {
        kAlphaOpaque = 0,
        kAlphaMask = 1,
        kAlphaBlend = 2,
    };

    struct SrmMaterial
    {
        float baseColorFactor[4];
        float emissiveFactor[3];
        float metallicFactor;
        float roughnessFactor;
        float alphaCutoff;
        uint32_t alphaMode;
        uint32_t doubleSided;
        int32_t baseColorTexture; ///< BC7 sRGB
        int32_t normalTexture;    ///< BC5 (X, Y; reconstruct Z)
        int32_t metalRoughTexture;///< BC7 linear, glTF layout (G roughness, B metallic, R occlusion if packed)
        int32_t emissiveTexture;  ///< BC7 sRGB
        int32_t occlusionTexture; ///< BC7 linear (R)
        int32_t pad[3];
    };

    struct SrmHeader
    {
        uint32_t magic;
        uint32_t version;
        uint32_t vertexCount;
        uint32_t indexCount;
        uint32_t submeshCount;
        uint32_t materialCount;
        uint32_t textureCount;
        uint32_t reserved;
        float boundsMin[4];
        float boundsMax[4];
    };

    constexpr uint32_t kTexturePathLength = 128;
} // namespace asset
