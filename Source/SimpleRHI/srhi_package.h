/**
 * @ Author: Amélie Heinrich
 * @ Create Time: 2026-11-13 00:00:00
 * @ Copyright: Copyright (c) 2026 Amélie Heinrich. All rights reserved.
 */

// Shader package (.srhs) format, shared by the runtime and the compiler.
//
// Little-endian, all integers u32 unless noted. Strings are u32 length + bytes (no terminator).
//
//   u32 magic 'SRHS', u32 version
//   u32 bindingCount, then bindings:
//     str name, u32 set, binding, type, textureType, sampleType, multisampled, storageFormat, samplerType,
//     d3dClass, d3dSlot, metalClass, metalSlot, dataSize, structureStride
//   u32 entryCount, then entries:
//     str name, u32 stage, tg[3], u32 inputCount, inputs { u32 location, str semantic, u32 semanticIndex },
//     u32 usedCount, u32 usedBindings[] (indices into the binding table),
//     u32 blobCount, blobs { u32 kind, str functionName, u32 size, bytes }

#pragma once

#include <SimpleRHI.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace srhi
{
    constexpr uint32_t kPackageMagic = 0x53484853; // 'SRHS'
    constexpr uint32_t kPackageVersion = 1;

    enum class BlobKind : uint32_t
    {
        DXBC = 1,     ///< D3D11 bytecode.
        HLSL = 2,     ///< HLSL source for D3DCompile (profile from stage, sm 5.0).
        MetalLib = 3, ///< Compiled metallib.
        MSL = 4,      ///< Metal source for newLibraryWithSource.
        WGSL = 5,
    };

    /// @brief D3D11 register class.
    enum class D3DClass : uint32_t
    {
        None = 0,
        B,
        T,
        S,
        U
    };

    enum class MetalClass : uint32_t
    {
        None = 0,
        Buffer,
        Texture,
        Sampler
    };

    struct PkgBinding
    {
        std::string name;
        uint32_t set = 0;
        uint32_t binding = 0;
        srhiBindingType type = SRHI_BINDING_TYPE_UNIFORM_BUFFER;
        srhiTextureType textureType = SRHI_TEXTURE_TYPE_2D;
        srhiTextureSampleType sampleType = SRHI_TEXTURE_SAMPLE_TYPE_FLOAT;
        uint32_t multisampled = 0;
        srhiFormat storageFormat = SRHI_FORMAT_UNDEFINED;
        srhiSamplerBindingType samplerType = SRHI_SAMPLER_BINDING_TYPE_FILTERING;
        D3DClass d3dClass = D3DClass::None;
        uint32_t d3dSlot = 0;
        MetalClass metalClass = MetalClass::None;
        uint32_t metalSlot = 0;
        /// @brief Uniform buffers: size of the struct in bytes.
        uint32_t dataSize = 0;
        /// @brief Structured buffers: element stride.
        uint32_t structureStride = 0;
    };

    struct PkgVertexInput
    {
        uint32_t location = 0;
        std::string semantic;
        uint32_t semanticIndex = 0;
    };

    struct PkgBlob
    {
        BlobKind kind = BlobKind::WGSL;
        /// @brief Name of the entry function inside the blob's code.
        std::string functionName;
        std::vector<uint8_t> data;
    };

    struct PkgEntry
    {
        std::string name;
        /// @brief One srhiShaderStageFlags bit.
        uint32_t stage = 0;
        uint32_t threadgroupSize[3] = {1, 1, 1};
        std::vector<PkgVertexInput> inputs;
        /// @brief Indices into Package::bindings of the resources this entry point statically uses.
        std::vector<uint32_t> usedBindings;
        std::vector<PkgBlob> blobs;

        const PkgBlob* FindBlob(BlobKind kind) const
        {
            for (const PkgBlob& blob : blobs)
                if (blob.kind == kind)
                    return &blob;
            return nullptr;
        }
    };

    struct Package
    {
        std::vector<PkgBinding> bindings;
        std::vector<PkgEntry> entries;

        const PkgEntry* FindEntry(const char* name) const
        {
            if (!name)
                return nullptr;
            for (const PkgEntry& entry : entries)
                if (entry.name == name)
                    return &entry;
            return nullptr;
        }

        const PkgBinding* FindBinding(uint32_t set, uint32_t binding) const
        {
            for (const PkgBinding& b : bindings)
                if (b.set == set && b.binding == binding)
                    return &b;
            return nullptr;
        }
    };

    // --- Serialization ------------------------------------------------------------------------------

    class PackageWriter
    {
    public:
        void U32(uint32_t v) { Raw(&v, 4); }
        void Str(const std::string& s)
        {
            U32(uint32_t(s.size()));
            Raw(s.data(), s.size());
        }
        void Bytes(const std::vector<uint8_t>& b)
        {
            U32(uint32_t(b.size()));
            Raw(b.data(), b.size());
        }
        void Raw(const void* p, size_t n)
        {
            const uint8_t* b = static_cast<const uint8_t*>(p);
            mData.insert(mData.end(), b, b + n);
        }
        std::vector<uint8_t>& Data() { return mData; }

    private:
        std::vector<uint8_t> mData;
    };

    class PackageReader
    {
    public:
        PackageReader(const void* data, size_t size) : mData(static_cast<const uint8_t*>(data)), mSize(size) {}

        bool U32(uint32_t& v) { return Raw(&v, 4); }
        template <typename E> bool Enum(E& e)
        {
            uint32_t v;
            if (!U32(v))
                return false;
            e = static_cast<E>(v);
            return true;
        }
        bool Str(std::string& s)
        {
            uint32_t n;
            if (!U32(n) || mPos + n > mSize)
                return false;
            s.assign(reinterpret_cast<const char*>(mData + mPos), n);
            mPos += n;
            return true;
        }
        bool Bytes(std::vector<uint8_t>& b)
        {
            uint32_t n;
            if (!U32(n) || mPos + n > mSize)
                return false;
            b.assign(mData + mPos, mData + mPos + n);
            mPos += n;
            return true;
        }
        bool Raw(void* p, size_t n)
        {
            if (mPos + n > mSize)
                return false;
            std::memcpy(p, mData + mPos, n);
            mPos += n;
            return true;
        }

    private:
        const uint8_t* mData;
        size_t mSize;
        size_t mPos = 0;
    };

    inline std::vector<uint8_t> WritePackage(const Package& pkg)
    {
        PackageWriter w;
        w.U32(kPackageMagic);
        w.U32(kPackageVersion);
        w.U32(uint32_t(pkg.bindings.size()));
        for (const PkgBinding& b : pkg.bindings) {
            w.Str(b.name);
            w.U32(b.set);
            w.U32(b.binding);
            w.U32(uint32_t(b.type));
            w.U32(uint32_t(b.textureType));
            w.U32(uint32_t(b.sampleType));
            w.U32(b.multisampled);
            w.U32(uint32_t(b.storageFormat));
            w.U32(uint32_t(b.samplerType));
            w.U32(uint32_t(b.d3dClass));
            w.U32(b.d3dSlot);
            w.U32(uint32_t(b.metalClass));
            w.U32(b.metalSlot);
            w.U32(b.dataSize);
            w.U32(b.structureStride);
        }
        w.U32(uint32_t(pkg.entries.size()));
        for (const PkgEntry& e : pkg.entries) {
            w.Str(e.name);
            w.U32(e.stage);
            for (uint32_t s : e.threadgroupSize)
                w.U32(s);
            w.U32(uint32_t(e.inputs.size()));
            for (const PkgVertexInput& in : e.inputs) {
                w.U32(in.location);
                w.Str(in.semantic);
                w.U32(in.semanticIndex);
            }
            w.U32(uint32_t(e.usedBindings.size()));
            for (uint32_t u : e.usedBindings)
                w.U32(u);
            w.U32(uint32_t(e.blobs.size()));
            for (const PkgBlob& blob : e.blobs) {
                w.U32(uint32_t(blob.kind));
                w.Str(blob.functionName);
                w.Bytes(blob.data);
            }
        }
        return std::move(w.Data());
    }

    inline bool ReadPackage(const void* data, size_t size, Package& pkg)
    {
        PackageReader r(data, size);
        uint32_t magic = 0, version = 0, count = 0;
        if (!r.U32(magic) || magic != kPackageMagic || !r.U32(version) || version != kPackageVersion)
            return false;
        if (!r.U32(count))
            return false;
        pkg.bindings.resize(count);
        for (PkgBinding& b : pkg.bindings) {
            if (!r.Str(b.name) || !r.U32(b.set) || !r.U32(b.binding) || !r.Enum(b.type) || !r.Enum(b.textureType) ||
                !r.Enum(b.sampleType) || !r.U32(b.multisampled) || !r.Enum(b.storageFormat) ||
                !r.Enum(b.samplerType) || !r.Enum(b.d3dClass) || !r.U32(b.d3dSlot) || !r.Enum(b.metalClass) ||
                !r.U32(b.metalSlot) || !r.U32(b.dataSize) || !r.U32(b.structureStride))
                return false;
        }
        if (!r.U32(count))
            return false;
        pkg.entries.resize(count);
        for (PkgEntry& e : pkg.entries) {
            uint32_t n = 0;
            if (!r.Str(e.name) || !r.U32(e.stage) || !r.U32(e.threadgroupSize[0]) || !r.U32(e.threadgroupSize[1]) ||
                !r.U32(e.threadgroupSize[2]) || !r.U32(n))
                return false;
            e.inputs.resize(n);
            for (PkgVertexInput& in : e.inputs)
                if (!r.U32(in.location) || !r.Str(in.semantic) || !r.U32(in.semanticIndex))
                    return false;
            if (!r.U32(n))
                return false;
            e.usedBindings.resize(n);
            for (uint32_t& u : e.usedBindings)
                if (!r.U32(u) || u >= pkg.bindings.size())
                    return false;
            if (!r.U32(n))
                return false;
            e.blobs.resize(n);
            for (PkgBlob& blob : e.blobs)
                if (!r.Enum(blob.kind) || !r.Str(blob.functionName) || !r.Bytes(blob.data))
                    return false;
        }
        return true;
    }
} // namespace srhi
