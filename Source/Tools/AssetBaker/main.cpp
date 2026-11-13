/**
 * @ Author: Amélie Heinrich
 * @ Create Time: 2026-11-13 00:00:00
 * @ Copyright: Copyright (c) 2026 Amélie Heinrich. All rights reserved.
 */

// srhi_bake: converts Content/ into GPU-ready assets in Content/Cache.
//
//   Content/Models/<Name>/<file>.gltf|.glb  ->  Content/Cache/Models/<Name>.srm
//                                               Content/Cache/Textures/<Name>/<image>.<kind>.srt
//   Content/Skyboxes/<name>.hdr             ->  Content/Cache/Textures/Skyboxes/<name>.bc6h.srt
//
// Textures are compressed with ANBC (GPU): base color / emissive -> BC7 sRGB, normals -> BC5,
// metal-roughness / occlusion -> BC7, HDR environments -> BC6H. Full mip chains.
// Scene node transforms are baked into the vertices. Rebakes only what is older than its source
// unless --force is given.
//
//   srhi_bake [--force] [--only <substring>] [--content <dir>]

#include "../../Examples/Common/AssetFormat.h"
#include "../../ThirdParty/ANBC/anbc.h"

#include <SimpleRHI.h>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace asset;

namespace
{
    struct Options
    {
        bool force = false;
        std::string only;
        fs::path content = "Content";
    } g_options;

    anbcDevice* g_anbc = nullptr;

    bool UpToDate(const fs::path& output, const fs::path& source)
    {
        std::error_code ec;
        return !g_options.force && fs::exists(output, ec) && fs::last_write_time(output, ec) >= fs::last_write_time(source, ec);
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
            return uint16_t(sign | 0x7BFF); // clamp to max finite
        return uint16_t(sign | (exp << 10) | ((mant + 0x1000) >> 13));
    }

    enum class TexKind
    {
        Color,     // BC7 sRGB
        Linear,    // BC7 linear
        Normal,    // BC5
        HDR,       // BC6H
    };

    const char* KindSuffix(TexKind k)
    {
        switch (k) {
        case TexKind::Color: return "bc7s";
        case TexKind::Linear: return "bc7";
        case TexKind::Normal: return "bc5";
        case TexKind::HDR: return "bc6h";
        }
        return "?";
    }

    /// @brief Compresses RGBA pixels (RGBA8 or RGBA16F) with a full mip chain and writes an .srt.
    bool CompressAndWrite(const fs::path& output, const void* pixels, uint32_t w, uint32_t h, bool half, TexKind kind)
    {
        anbcTextureDesc desc = {};
        desc.width = w;
        desc.height = h;
        desc.pixels = pixels;
        desc.pixelFormat = half ? ANBC_PIXEL_FORMAT_RGBA16_FLOAT : ANBC_PIXEL_FORMAT_RGBA8_UNORM;
        desc.flags = ANBC_TEXTURE_FLAG_GENERATE_MIPS | (kind == TexKind::Color ? ANBC_TEXTURE_FLAG_SRGB : 0);
        anbcTexture* tex = anbcCreateTexture(g_anbc, &desc);
        if (!tex) {
            fprintf(stderr, "  ANBC could not create a %ux%u texture for %s\n", w, h, output.string().c_str());
            return false;
        }
        anbcTextureFormat format = kind == TexKind::Normal ? ANBC_TEXTURE_FORMAT_BC5
                                 : kind == TexKind::HDR    ? ANBC_TEXTURE_FORMAT_BC6H
                                                           : ANBC_TEXTURE_FORMAT_BC7;
        anbcResult result = anbcCompress(g_anbc, tex, format, nullptr);
        if (result != ANBC_OK) {
            fprintf(stderr, "  ANBC compression failed for %s: %s\n", output.string().c_str(), anbcResultString(result));
            anbcDestroyTexture(tex);
            return false;
        }

        SrtHeader header = {};
        header.magic = kSrtMagic;
        header.format = kind == TexKind::Color ? SRHI_FORMAT_BC7_UNORM_SRGB
                      : kind == TexKind::Linear ? SRHI_FORMAT_BC7_UNORM
                      : kind == TexKind::Normal ? SRHI_FORMAT_BC5_UNORM
                                                : SRHI_FORMAT_BC6H_UFLOAT;
        header.width = w;
        header.height = h;
        header.mipCount = anbcGetMipCount(tex);

        fs::create_directories(output.parent_path());
        std::ofstream file(output, std::ios::binary);
        file.write(reinterpret_cast<const char*>(&header), sizeof(header));
        for (uint32_t m = 0; m < header.mipCount; ++m) {
            anbcMipInfo mip;
            anbcGetMip(tex, m, &mip);
            uint64_t size = mip.sizeBytes;
            file.write(reinterpret_cast<const char*>(&size), 8);
            file.write(static_cast<const char*>(mip.data), std::streamsize(size));
        }
        anbcDestroyTexture(tex);
        return bool(file);
    }

    // =================================================================================================
    // Models
    // =================================================================================================

    struct TextureRequest
    {
        int image;
        TexKind kind;
        bool operator<(const TextureRequest& o) const { return image != o.image ? image < o.image : int(kind) < int(o.kind); }
    };

    class ModelBaker
    {
    public:
        bool Bake(const fs::path& source, const std::string& name);

    private:
        int32_t Texture(const cgltf_texture_view& view, TexKind kind);
        bool BakeTextures();
        void AddPrimitive(const cgltf_primitive& prim, const float world[16]);

        cgltf_data* mData = nullptr;
        fs::path mSourceDir;
        std::string mName;
        std::vector<SrmVertex> mVertices;
        std::vector<uint32_t> mIndices;
        std::vector<SrmSubmesh> mSubmeshes;
        std::vector<SrmMaterial> mMaterials;
        std::map<TextureRequest, int32_t> mTextureIndex;
        std::vector<TextureRequest> mTextureList;
        std::vector<std::string> mTexturePaths;
    };

    std::string ImageName(const cgltf_image& image, int index)
    {
        std::string base;
        if (image.uri && strncmp(image.uri, "data:", 5) != 0)
            base = fs::path(image.uri).stem().string();
        else if (image.name && *image.name)
            base = image.name;
        else
            base = "image" + std::to_string(index);
        for (char& c : base)
            if (!isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-')
                c = '_';
        return base;
    }

    int32_t ModelBaker::Texture(const cgltf_texture_view& view, TexKind kind)
    {
        if (!view.texture || !view.texture->image)
            return kNoTexture;
        TextureRequest req = {int(view.texture->image - mData->images), kind};
        auto it = mTextureIndex.find(req);
        if (it != mTextureIndex.end())
            return it->second;
        int32_t index = int32_t(mTextureList.size());
        mTextureIndex[req] = index;
        mTextureList.push_back(req);
        mTexturePaths.push_back(mName + "/" + ImageName(*view.texture->image, req.image) + "." + KindSuffix(kind) + ".srt");
        return index;
    }

    bool ModelBaker::BakeTextures()
    {
        fs::path texDir = g_options.content / "Cache" / "Textures";
        for (size_t i = 0; i < mTextureList.size(); ++i) {
            const TextureRequest& req = mTextureList[i];
            const cgltf_image& image = mData->images[req.image];
            fs::path output = texDir / mTexturePaths[i];

            fs::path imageSource = image.uri && strncmp(image.uri, "data:", 5) != 0 ? mSourceDir / image.uri : fs::path();
            std::error_code ec;
            if (!g_options.force && fs::exists(output, ec) && (imageSource.empty() || UpToDate(output, imageSource))) {
                printf("  [cached] %s\n", mTexturePaths[i].c_str());
                continue;
            }

            int w = 0, h = 0, comp = 0;
            stbi_uc* pixels = nullptr;
            if (image.buffer_view) {
                const uint8_t* bytes = static_cast<const uint8_t*>(image.buffer_view->buffer->data) + image.buffer_view->offset;
                pixels = stbi_load_from_memory(bytes, int(image.buffer_view->size), &w, &h, &comp, 4);
            } else if (!imageSource.empty()) {
                std::string decoded = imageSource.string();
                cgltf_decode_uri(decoded.data());
                pixels = stbi_load(decoded.c_str(), &w, &h, &comp, 4);
            }
            if (!pixels) {
                fprintf(stderr, "  failed to load image %d of %s\n", req.image, mName.c_str());
                return false;
            }
            auto start = std::chrono::steady_clock::now();
            bool ok = CompressAndWrite(output, pixels, uint32_t(w), uint32_t(h), false, req.kind);
            stbi_image_free(pixels);
            if (!ok)
                return false;
            printf("  [%5.0f ms] %s (%dx%d)\n",
                   std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(),
                   mTexturePaths[i].c_str(), w, h);
        }
        return true;
    }

    void Mul(const float m[16], const float v[3], float w, float out[3])
    {
        for (int r = 0; r < 3; ++r)
            out[r] = m[r] * v[0] + m[4 + r] * v[1] + m[8 + r] * v[2] + m[12 + r] * w;
    }

    void Normalize(float* v, int n)
    {
        float len = 0.0f;
        for (int i = 0; i < n; ++i)
            len += v[i] * v[i];
        len = std::sqrt(len);
        if (len > 1e-8f)
            for (int i = 0; i < n; ++i)
                v[i] /= len;
    }

    void ModelBaker::AddPrimitive(const cgltf_primitive& prim, const float world[16])
    {
        if (prim.type != cgltf_primitive_type_triangles)
            return;
        const cgltf_accessor *pos = nullptr, *nrm = nullptr, *uv = nullptr, *tan = nullptr;
        for (cgltf_size i = 0; i < prim.attributes_count; ++i) {
            const cgltf_attribute& a = prim.attributes[i];
            if (a.type == cgltf_attribute_type_position)
                pos = a.data;
            else if (a.type == cgltf_attribute_type_normal)
                nrm = a.data;
            else if (a.type == cgltf_attribute_type_texcoord && a.index == 0)
                uv = a.data;
            else if (a.type == cgltf_attribute_type_tangent)
                tan = a.data;
        }
        if (!pos)
            return;

        // Normal matrix: inverse transpose of the upper 3x3 (column-major).
        const float* m = world;
        float a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], k = m[10];
        float det = a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);
        float inv = std::fabs(det) > 1e-12f ? 1.0f / det : 0.0f;
        float n3[16] = {(e * k - f * h) * inv, (c * h - b * k) * inv, (b * f - c * e) * inv, 0,
                        (f * g - d * k) * inv, (a * k - c * g) * inv, (c * d - a * f) * inv, 0,
                        (d * h - e * g) * inv, (b * g - a * h) * inv, (a * e - b * d) * inv, 0,
                        0, 0, 0, 1};
        // n3 is the inverse in row-major order == inverse-transpose in column-major order.

        uint32_t base = uint32_t(mVertices.size());
        size_t count = pos->count;
        mVertices.resize(base + count);
        for (size_t i = 0; i < count; ++i) {
            SrmVertex& v = mVertices[base + i];
            float p[3] = {}, n[3] = {0, 1, 0}, t[4] = {1, 0, 0, 1};
            cgltf_accessor_read_float(pos, i, p, 3);
            if (nrm)
                cgltf_accessor_read_float(nrm, i, n, 3);
            if (uv)
                cgltf_accessor_read_float(uv, i, v.uv, 2);
            else
                v.uv[0] = v.uv[1] = 0.0f;
            if (tan)
                cgltf_accessor_read_float(tan, i, t, 4);
            Mul(world, p, 1.0f, v.position);
            Mul(n3, n, 0.0f, v.normal);
            Normalize(v.normal, 3);
            Mul(world, t, 0.0f, v.tangent);
            Normalize(v.tangent, 3);
            v.tangent[3] = t[3] < 0.0f ? -1.0f : 1.0f;
        }

        SrmSubmesh sub = {};
        sub.indexOffset = uint32_t(mIndices.size());
        if (prim.indices) {
            sub.indexCount = uint32_t(prim.indices->count);
            for (size_t i = 0; i < prim.indices->count; ++i)
                mIndices.push_back(base + uint32_t(cgltf_accessor_read_index(prim.indices, i)));
        } else {
            sub.indexCount = uint32_t(count);
            for (size_t i = 0; i < count; ++i)
                mIndices.push_back(base + uint32_t(i));
        }

        // Generate tangents from UVs when the file has none.
        if (!tan && uv) {
            std::vector<float> acc(count * 6, 0.0f);
            for (uint32_t i = sub.indexOffset; i + 2 < sub.indexOffset + sub.indexCount; i += 3) {
                const SrmVertex& v0 = mVertices[mIndices[i]];
                const SrmVertex& v1 = mVertices[mIndices[i + 1]];
                const SrmVertex& v2 = mVertices[mIndices[i + 2]];
                float e1[3], e2[3];
                for (int c = 0; c < 3; ++c) {
                    e1[c] = v1.position[c] - v0.position[c];
                    e2[c] = v2.position[c] - v0.position[c];
                }
                float du1 = v1.uv[0] - v0.uv[0], dv1 = v1.uv[1] - v0.uv[1];
                float du2 = v2.uv[0] - v0.uv[0], dv2 = v2.uv[1] - v0.uv[1];
                float r = du1 * dv2 - du2 * dv1;
                if (std::fabs(r) < 1e-12f)
                    continue;
                r = 1.0f / r;
                for (int j = 0; j < 3; ++j) {
                    float* T = &acc[(mIndices[i + j] - base) * 6];
                    for (int c = 0; c < 3; ++c) {
                        T[c] += (e1[c] * dv2 - e2[c] * dv1) * r;
                        T[3 + c] += (e2[c] * du1 - e1[c] * du2) * r;
                    }
                }
            }
            for (size_t i = 0; i < count; ++i) {
                SrmVertex& v = mVertices[base + i];
                float* T = &acc[i * 6];
                float* B = T + 3;
                float ndt = v.normal[0] * T[0] + v.normal[1] * T[1] + v.normal[2] * T[2];
                float t[3] = {T[0] - v.normal[0] * ndt, T[1] - v.normal[1] * ndt, T[2] - v.normal[2] * ndt};
                Normalize(t, 3);
                float cx = v.normal[1] * t[2] - v.normal[2] * t[1], cy = v.normal[2] * t[0] - v.normal[0] * t[2],
                      cz = v.normal[0] * t[1] - v.normal[1] * t[0];
                v.tangent[0] = t[0];
                v.tangent[1] = t[1];
                v.tangent[2] = t[2];
                v.tangent[3] = (cx * B[0] + cy * B[1] + cz * B[2]) < 0.0f ? -1.0f : 1.0f;
            }
        }

        sub.materialIndex = prim.material ? uint32_t(prim.material - mData->materials) : uint32_t(mData->materials_count);
        for (int c = 0; c < 3; ++c) {
            sub.boundsMin[c] = 1e30f;
            sub.boundsMax[c] = -1e30f;
        }
        for (uint32_t i = sub.indexOffset; i < sub.indexOffset + sub.indexCount; ++i)
            for (int c = 0; c < 3; ++c) {
                sub.boundsMin[c] = std::min(sub.boundsMin[c], mVertices[mIndices[i]].position[c]);
                sub.boundsMax[c] = std::max(sub.boundsMax[c], mVertices[mIndices[i]].position[c]);
            }
        mSubmeshes.push_back(sub);
    }

    bool ModelBaker::Bake(const fs::path& source, const std::string& name)
    {
        mName = name;
        mSourceDir = source.parent_path();
        fs::path output = g_options.content / "Cache" / "Models" / (name + ".srm");
        if (UpToDate(output, source)) {
            printf("[cached] %s\n", name.c_str());
            return true;
        }
        printf("[model] %s\n", source.string().c_str());

        cgltf_options options = {};
        if (cgltf_parse_file(&options, source.string().c_str(), &mData) != cgltf_result_success ||
            cgltf_load_buffers(&options, mData, source.string().c_str()) != cgltf_result_success) {
            fprintf(stderr, "  failed to parse %s\n", source.string().c_str());
            return false;
        }

        // Materials (plus a default one at the end for primitives without a material).
        for (cgltf_size i = 0; i < mData->materials_count; ++i) {
            const cgltf_material& src = mData->materials[i];
            SrmMaterial mat = {};
            const cgltf_pbr_metallic_roughness& pbr = src.pbr_metallic_roughness;
            memcpy(mat.baseColorFactor, pbr.base_color_factor, 16);
            memcpy(mat.emissiveFactor, src.emissive_factor, 12);
            if (src.has_emissive_strength)
                for (float& e : mat.emissiveFactor)
                    e *= src.emissive_strength.emissive_strength;
            mat.metallicFactor = pbr.metallic_factor;
            mat.roughnessFactor = pbr.roughness_factor;
            mat.alphaCutoff = src.alpha_cutoff;
            mat.alphaMode = src.alpha_mode == cgltf_alpha_mode_mask ? kAlphaMask : src.alpha_mode == cgltf_alpha_mode_blend ? kAlphaBlend : kAlphaOpaque;
            mat.doubleSided = src.double_sided;
            mat.baseColorTexture = Texture(pbr.base_color_texture, TexKind::Color);
            mat.normalTexture = Texture(src.normal_texture, TexKind::Normal);
            mat.metalRoughTexture = Texture(pbr.metallic_roughness_texture, TexKind::Linear);
            mat.emissiveTexture = Texture(src.emissive_texture, TexKind::Color);
            mat.occlusionTexture = Texture(src.occlusion_texture, TexKind::Linear);
            mMaterials.push_back(mat);
        }
        SrmMaterial def = {};
        def.baseColorFactor[0] = def.baseColorFactor[1] = def.baseColorFactor[2] = def.baseColorFactor[3] = 1.0f;
        def.metallicFactor = 0.0f;
        def.roughnessFactor = 0.8f;
        def.baseColorTexture = def.normalTexture = def.metalRoughTexture = def.emissiveTexture = def.occlusionTexture = kNoTexture;
        mMaterials.push_back(def);

        // Geometry: every mesh node of the default scene, with its world transform baked in.
        const cgltf_scene* scene = mData->scene ? mData->scene : mData->scenes_count ? &mData->scenes[0] : nullptr;
        std::vector<const cgltf_node*> stack;
        if (scene)
            for (cgltf_size i = 0; i < scene->nodes_count; ++i)
                stack.push_back(scene->nodes[i]);
        else
            for (cgltf_size i = 0; i < mData->nodes_count; ++i)
                if (!mData->nodes[i].parent)
                    stack.push_back(&mData->nodes[i]);
        while (!stack.empty()) {
            const cgltf_node* node = stack.back();
            stack.pop_back();
            for (cgltf_size i = 0; i < node->children_count; ++i)
                stack.push_back(node->children[i]);
            if (!node->mesh)
                continue;
            float world[16];
            cgltf_node_transform_world(node, world);
            for (cgltf_size p = 0; p < node->mesh->primitives_count; ++p)
                AddPrimitive(node->mesh->primitives[p], world);
        }

        if (!BakeTextures()) {
            cgltf_free(mData);
            return false;
        }

        SrmHeader header = {};
        header.magic = kSrmMagic;
        header.version = kSrmVersion;
        header.vertexCount = uint32_t(mVertices.size());
        header.indexCount = uint32_t(mIndices.size());
        header.submeshCount = uint32_t(mSubmeshes.size());
        header.materialCount = uint32_t(mMaterials.size());
        header.textureCount = uint32_t(mTexturePaths.size());
        for (int c = 0; c < 3; ++c) {
            header.boundsMin[c] = 1e30f;
            header.boundsMax[c] = -1e30f;
        }
        for (const SrmSubmesh& s : mSubmeshes)
            for (int c = 0; c < 3; ++c) {
                header.boundsMin[c] = std::min(header.boundsMin[c], s.boundsMin[c]);
                header.boundsMax[c] = std::max(header.boundsMax[c], s.boundsMax[c]);
            }

        fs::create_directories(output.parent_path());
        std::ofstream file(output, std::ios::binary);
        file.write(reinterpret_cast<const char*>(&header), sizeof(header));
        file.write(reinterpret_cast<const char*>(mVertices.data()), std::streamsize(mVertices.size() * sizeof(SrmVertex)));
        file.write(reinterpret_cast<const char*>(mIndices.data()), std::streamsize(mIndices.size() * 4));
        file.write(reinterpret_cast<const char*>(mSubmeshes.data()), std::streamsize(mSubmeshes.size() * sizeof(SrmSubmesh)));
        file.write(reinterpret_cast<const char*>(mMaterials.data()), std::streamsize(mMaterials.size() * sizeof(SrmMaterial)));
        for (const std::string& path : mTexturePaths) {
            char buf[kTexturePathLength] = {};
            snprintf(buf, sizeof(buf), "%s", path.c_str());
            file.write(buf, sizeof(buf));
        }
        printf("  -> %s: %u vertices, %u indices, %u submeshes, %u materials, %u textures\n", output.string().c_str(),
               header.vertexCount, header.indexCount, header.submeshCount, header.materialCount, header.textureCount);
        cgltf_free(mData);
        return bool(file);
    }

    bool BakeSkybox(const fs::path& source)
    {
        fs::path output = g_options.content / "Cache" / "Textures" / "Skyboxes" / (source.stem().string() + ".bc6h.srt");
        if (UpToDate(output, source)) {
            printf("[cached] %s\n", source.filename().string().c_str());
            return true;
        }
        printf("[skybox] %s\n", source.string().c_str());
        int w = 0, h = 0, comp = 0;
        float* pixels = stbi_loadf(source.string().c_str(), &w, &h, &comp, 4);
        if (!pixels) {
            fprintf(stderr, "  failed to load %s\n", source.string().c_str());
            return false;
        }
        std::vector<uint16_t> half(size_t(w) * h * 4);
        for (size_t i = 0; i < half.size(); ++i)
            half[i] = FloatToHalf(pixels[i]);
        stbi_image_free(pixels);
        auto start = std::chrono::steady_clock::now();
        bool ok = CompressAndWrite(output, half.data(), uint32_t(w), uint32_t(h), true, TexKind::HDR);
        printf("  [%5.0f ms] %s (%dx%d)\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(),
               output.string().c_str(), w, h);
        return ok;
    }

    bool Selected(const fs::path& p)
    {
        return g_options.only.empty() || p.string().find(g_options.only) != std::string::npos;
    }
} // namespace

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--force"))
            g_options.force = true;
        else if (!strcmp(argv[i], "--only") && i + 1 < argc)
            g_options.only = argv[++i];
        else if (!strcmp(argv[i], "--content") && i + 1 < argc)
            g_options.content = argv[++i];
        else {
            printf("usage: srhi_bake [--force] [--only <substring>] [--content <dir>]\n");
            return 1;
        }
    }

#if defined(__APPLE__)
    g_anbc = anbcCreateDevice(ANBC_DEVICE_BACKEND_METAL);
#else
    g_anbc = anbcCreateDevice(ANBC_DEVICE_BACKEND_VULKAN);
#endif
    if (!g_anbc) {
        fprintf(stderr, "srhi_bake: no ANBC device (needs Metal 4 on macOS, Vulkan elsewhere)\n");
        return 1;
    }
    anbcDeviceInfo info;
    anbcGetDeviceInfo(g_anbc, &info);
    printf("srhi_bake: ANBC on %s (%s)\n", info.name, info.backend);

    bool ok = true;
    std::error_code ec;
    for (const auto& dir : fs::directory_iterator(g_options.content / "Models", ec)) {
        if (!dir.is_directory())
            continue;
        for (const auto& file : fs::directory_iterator(dir.path())) {
            std::string ext = file.path().extension().string();
            if ((ext == ".gltf" || ext == ".glb") && Selected(file.path())) {
                ModelBaker baker;
                ok &= baker.Bake(file.path(), dir.path().filename().string());
                break; // one model per directory
            }
        }
    }
    for (const auto& file : fs::directory_iterator(g_options.content / "Skyboxes", ec))
        if (file.path().extension() == ".hdr" && Selected(file.path()))
            ok &= BakeSkybox(file.path());

    anbcDestroyDevice(g_anbc);
    return ok ? 0 : 1;
}
