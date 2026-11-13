/**
 * @ Author: Amélie Heinrich
 * @ Create Time: 2026-11-13 00:00:00
 * @ Copyright: Copyright (c) 2026 Amélie Heinrich. All rights reserved.
 */

#include "Model.h"

#include "App.h"

#include <cstdio>
#include <cstring>

using namespace asset;

namespace
{
    srhiTexture* SolidTexture(srhiDevice* device, uint8_t r, uint8_t g, uint8_t b, const char* label)
    {
        srhiTextureDesc desc = {};
        desc.format = SRHI_FORMAT_RGBA8_UNORM;
        desc.width = desc.height = 4;
        desc.usage = SRHI_TEXTURE_USAGE_SAMPLED;
        desc.label = label;
        srhiTexture* t = srhiTextureCreate(device, &desc);
        uint8_t pixels[4 * 4 * 4];
        for (int i = 0; i < 16; ++i) {
            pixels[i * 4 + 0] = r;
            pixels[i * 4 + 1] = g;
            pixels[i * 4 + 2] = b;
            pixels[i * 4 + 3] = 255;
        }
        srhiTextureWrite(device, t, 0, 0, nullptr, pixels, 0);
        return t;
    }
} // namespace

srhiTexture* LoadBakedTexture(srhiDevice* device, const std::string& relativePath)
{
    std::vector<uint8_t> bytes;
    std::string path = "Content/Cache/Textures/" + relativePath;
    if (!App::ReadFile(path, bytes) || bytes.size() < sizeof(SrtHeader)) {
        fprintf(stderr, "missing texture %s (run srhi_bake)\n", path.c_str());
        return nullptr;
    }
    SrtHeader header;
    memcpy(&header, bytes.data(), sizeof(header));
    if (header.magic != kSrtMagic)
        return nullptr;

    srhiTextureDesc desc = {};
    desc.format = srhiFormat(header.format);
    desc.width = header.width;
    desc.height = header.height;
    desc.mipLevels = header.mipCount;
    desc.usage = SRHI_TEXTURE_USAGE_SAMPLED;
    desc.label = relativePath.c_str();
    srhiTexture* texture = srhiTextureCreate(device, &desc);
    if (!texture)
        return nullptr;
    size_t offset = sizeof(SrtHeader);
    for (uint32_t mip = 0; mip < header.mipCount && offset + 8 <= bytes.size(); ++mip) {
        uint64_t size;
        memcpy(&size, bytes.data() + offset, 8);
        offset += 8;
        srhiTextureWrite(device, texture, mip, 0, nullptr, bytes.data() + offset, 0);
        offset += size_t(size);
    }
    return texture;
}

srhiVertexLayout Model::VertexLayout()
{
    srhiVertexLayout layout = {};
    layout.bufferCount = 1;
    layout.buffers[0].stride = sizeof(SrmVertex);
    layout.attributeCount = 4;
    layout.attributes[0] = {0, SRHI_VERTEX_FORMAT_FLOAT3, offsetof(SrmVertex, position), 0};
    layout.attributes[1] = {1, SRHI_VERTEX_FORMAT_FLOAT3, offsetof(SrmVertex, normal), 0};
    layout.attributes[2] = {2, SRHI_VERTEX_FORMAT_FLOAT2, offsetof(SrmVertex, uv), 0};
    layout.attributes[3] = {3, SRHI_VERTEX_FORMAT_FLOAT4, offsetof(SrmVertex, tangent), 0};
    return layout;
}

srhiResourceSetLayout* Model::CreateMaterialLayout(srhiDevice* device)
{
    srhiResourceSetLayoutEntry entries[7] = {};
    entries[0].binding = 0;
    entries[0].type = SRHI_BINDING_TYPE_UNIFORM_BUFFER;
    entries[0].stages = SRHI_SHADER_STAGE_ALL_GRAPHICS;
    for (uint32_t i = 1; i <= 5; ++i) {
        entries[i].binding = i;
        entries[i].type = SRHI_BINDING_TYPE_TEXTURE;
        entries[i].stages = SRHI_SHADER_STAGE_FRAGMENT;
    }
    entries[6].binding = 6;
    entries[6].type = SRHI_BINDING_TYPE_SAMPLER;
    entries[6].stages = SRHI_SHADER_STAGE_FRAGMENT;
    srhiResourceSetLayoutDesc desc = {entries, 7, "material"};
    return srhiResourceSetLayoutCreate(device, &desc);
}

bool Model::Load(srhiDevice* device, const char* name, srhiResourceSetLayout* materialLayout)
{
    mDevice = device;
    std::vector<uint8_t> bytes;
    std::string path = std::string("Content/Cache/Models/") + name + ".srm";
    if (!App::ReadFile(path, bytes) || bytes.size() < sizeof(SrmHeader)) {
        fprintf(stderr, "missing model %s (run srhi_bake)\n", path.c_str());
        return false;
    }
    memcpy(&mHeader, bytes.data(), sizeof(mHeader));
    if (mHeader.magic != kSrmMagic || mHeader.version != kSrmVersion) {
        fprintf(stderr, "%s: bad or outdated model file (rebake with srhi_bake --force)\n", path.c_str());
        return false;
    }

    const uint8_t* p = bytes.data() + sizeof(SrmHeader);
    const SrmVertex* vertices = reinterpret_cast<const SrmVertex*>(p);
    p += size_t(mHeader.vertexCount) * sizeof(SrmVertex);
    const uint32_t* indices = reinterpret_cast<const uint32_t*>(p);
    p += size_t(mHeader.indexCount) * 4;
    mSubmeshes.assign(reinterpret_cast<const SrmSubmesh*>(p), reinterpret_cast<const SrmSubmesh*>(p) + mHeader.submeshCount);
    p += size_t(mHeader.submeshCount) * sizeof(SrmSubmesh);
    mMaterials.assign(reinterpret_cast<const SrmMaterial*>(p), reinterpret_cast<const SrmMaterial*>(p) + mHeader.materialCount);
    p += size_t(mHeader.materialCount) * sizeof(SrmMaterial);

    srhiBufferDesc vb = {uint64_t(mHeader.vertexCount) * sizeof(SrmVertex), SRHI_BUFFER_USAGE_VERTEX, 0, name};
    mVertexBuffer = srhiBufferCreate(device, &vb);
    srhiBufferDesc ib = {uint64_t(mHeader.indexCount) * 4, SRHI_BUFFER_USAGE_INDEX, 0, name};
    mIndexBuffer = srhiBufferCreate(device, &ib);
    if (!mVertexBuffer || !mIndexBuffer)
        return false;
    srhiBufferWrite(device, mVertexBuffer, 0, vertices, vb.size);
    srhiBufferWrite(device, mIndexBuffer, 0, indices, ib.size);

    mTextures.resize(mHeader.textureCount, nullptr);
    for (uint32_t i = 0; i < mHeader.textureCount; ++i) {
        char texPath[kTexturePathLength + 1] = {};
        memcpy(texPath, p + size_t(i) * kTexturePathLength, kTexturePathLength);
        mTextures[i] = LoadBakedTexture(device, texPath);
    }

    mDefaults[0] = SolidTexture(device, 255, 255, 255, "white");
    mDefaults[1] = SolidTexture(device, 128, 128, 255, "flat normal");
    mDefaults[2] = SolidTexture(device, 0, 0, 0, "black");

    srhiSamplerDesc sd = {};
    sd.minFilter = sd.magFilter = sd.mipFilter = SRHI_FILTER_LINEAR;
    sd.maxAnisotropy = 8;
    sd.label = "material";
    mSampler = srhiSamplerCreate(device, &sd);

    // One uniform buffer, 256 bytes per material (the portable uniform offset alignment).
    const uint32_t stride = 256;
    std::vector<uint8_t> constants(size_t(mHeader.materialCount) * stride, 0);
    for (uint32_t i = 0; i < mHeader.materialCount; ++i) {
        const SrmMaterial& m = mMaterials[i];
        float c[12] = {m.baseColorFactor[0], m.baseColorFactor[1], m.baseColorFactor[2], m.baseColorFactor[3],
                       m.emissiveFactor[0], m.emissiveFactor[1], m.emissiveFactor[2], m.alphaCutoff,
                       m.metallicFactor, m.roughnessFactor, float(m.alphaMode), 0.0f};
        memcpy(&constants[size_t(i) * stride], c, sizeof(c));
    }
    srhiBufferDesc mb = {constants.size(), SRHI_BUFFER_USAGE_UNIFORM, 0, "materials"};
    mMaterialBuffer = srhiBufferCreate(device, &mb);
    srhiBufferWrite(device, mMaterialBuffer, 0, constants.data(), constants.size());

    auto view = [&](int32_t index, int fallback) {
        srhiTexture* t = index >= 0 && uint32_t(index) < mTextures.size() && mTextures[index] ? mTextures[index] : mDefaults[fallback];
        return srhiTextureGetDefaultView(t);
    };
    for (uint32_t i = 0; i < mHeader.materialCount; ++i) {
        const SrmMaterial& m = mMaterials[i];
        srhiResourceBinding b[7] = {};
        b[0].binding = 0;
        b[0].buffer = mMaterialBuffer;
        b[0].offset = uint64_t(i) * stride;
        b[0].size = 48;
        b[1] = {1, nullptr, 0, 0, view(m.baseColorTexture, 0)};
        b[2] = {2, nullptr, 0, 0, view(m.normalTexture, 1)};
        b[3] = {3, nullptr, 0, 0, view(m.metalRoughTexture, 0)};
        b[4] = {4, nullptr, 0, 0, view(m.emissiveTexture, m.emissiveFactor[0] + m.emissiveFactor[1] + m.emissiveFactor[2] > 0 ? 0 : 2)};
        b[5] = {5, nullptr, 0, 0, view(m.occlusionTexture, 0)};
        b[6].binding = 6;
        b[6].sampler = mSampler;
        srhiResourceSetDesc sd2 = {materialLayout, b, 7, "material"};
        mMaterialSets.push_back(srhiResourceSetCreate(device, &sd2));
    }
    printf("loaded %s: %u triangles, %u submeshes, %u materials\n", name, mHeader.indexCount / 3, mHeader.submeshCount,
           mHeader.materialCount);
    return true;
}

void Model::Destroy()
{
    for (srhiResourceSet* s : mMaterialSets)
        srhiResourceSetDestroy(s);
    for (srhiTexture* t : mTextures)
        srhiTextureDestroy(t);
    for (srhiTexture* t : mDefaults)
        srhiTextureDestroy(t);
    srhiSamplerDestroy(mSampler);
    srhiBufferDestroy(mMaterialBuffer);
    srhiBufferDestroy(mVertexBuffer);
    srhiBufferDestroy(mIndexBuffer);
    *this = Model();
}

void Model::Draw(srhiRenderEncoder* encoder, uint32_t filter, bool bindMaterials) const
{
    srhiRenderEncoderSetVertexBuffer(encoder, 0, mVertexBuffer, 0);
    srhiRenderEncoderSetIndexBuffer(encoder, mIndexBuffer, SRHI_INDEX_TYPE_UINT32, 0);
    for (const SrmSubmesh& s : mSubmeshes) {
        uint32_t mode = mMaterials[s.materialIndex].alphaMode;
        uint32_t bit = mode == kAlphaMask ? kMasked : mode == kAlphaBlend ? kBlended : kOpaque;
        if (!(filter & bit))
            continue;
        if (bindMaterials)
            srhiRenderEncoderSetResourceSet(encoder, 1, mMaterialSets[s.materialIndex]);
        srhiRenderEncoderDrawIndexed(encoder, s.indexCount, 1, s.indexOffset, 0, 0);
    }
}
