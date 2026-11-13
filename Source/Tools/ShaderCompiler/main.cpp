/**
 * @ Author: Amélie Heinrich
 * @ Create Time: 2026-11-13 00:00:00
 * @ Copyright: Copyright (c) 2026 Amélie Heinrich. All rights reserved.
 */

// srhic: compiles a .slang file into a .srhs shader package.
//
//   srhic input.slang -o output.srhs [-I dir]... [-D NAME[=VALUE]]... [-e entry]...
//         [--targets d3d11,metal,wgsl] [-g] [--no-opt] [--header symbol]
//
// --header writes a C header (static const unsigned char symbol[] / symbol_size) instead of a binary
// package, for embedding shaders in libraries (e.g. the ImGui backend).
//   srhic --dump package.srhs

#include <SimpleRHI.h>

#include "../../SimpleRHI/srhi_package.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

static const char* kUsage =
    "usage: srhic <input.slang> -o <output.srhs> [-I dir] [-D NAME[=VALUE]] [-e entry]\n"
    "             [--targets d3d11,metal,wgsl] [-g] [--no-opt] [--header symbol]\n"
    "       srhic --dump <package.srhs>\n";

static const char* BindingTypeName(srhiBindingType t)
{
    switch (t) {
    case SRHI_BINDING_TYPE_UNIFORM_BUFFER: return "uniform";
    case SRHI_BINDING_TYPE_STORAGE_BUFFER: return "storage";
    case SRHI_BINDING_TYPE_STORAGE_BUFFER_RW: return "storage_rw";
    case SRHI_BINDING_TYPE_TEXTURE: return "texture";
    case SRHI_BINDING_TYPE_STORAGE_TEXTURE: return "storage_texture";
    case SRHI_BINDING_TYPE_SAMPLER: return "sampler";
    }
    return "?";
}

static const char* BlobKindName(srhi::BlobKind k)
{
    switch (k) {
    case srhi::BlobKind::DXBC: return "dxbc";
    case srhi::BlobKind::HLSL: return "hlsl";
    case srhi::BlobKind::MetalLib: return "metallib";
    case srhi::BlobKind::MSL: return "msl";
    case srhi::BlobKind::WGSL: return "wgsl";
    }
    return "?";
}

static int Dump(const char* path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        fprintf(stderr, "srhic: cannot read '%s'\n", path);
        return 1;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    srhi::Package pkg;
    if (!srhi::ReadPackage(bytes.data(), bytes.size(), pkg)) {
        fprintf(stderr, "srhic: '%s' is not a valid shader package\n", path);
        return 1;
    }
    static const char d3dClass[] = {'-', 'b', 't', 's', 'u'};
    static const char* metalClass[] = {"-", "buffer", "texture", "sampler"};
    printf("bindings:\n");
    for (const srhi::PkgBinding& b : pkg.bindings)
        printf("  set %u binding %u  %-16s %-20s d3d11 %c%u  metal %s(%u)  size %u stride %u\n", b.set, b.binding,
               BindingTypeName(b.type), b.name.c_str(), d3dClass[uint32_t(b.d3dClass)], b.d3dSlot,
               metalClass[uint32_t(b.metalClass)], b.metalSlot, b.dataSize, b.structureStride);
    printf("entry points:\n");
    for (const srhi::PkgEntry& e : pkg.entries) {
        printf("  %s (stage 0x%x, threadgroup %ux%ux%u)\n", e.name.c_str(), e.stage, e.threadgroupSize[0],
               e.threadgroupSize[1], e.threadgroupSize[2]);
        for (const srhi::PkgVertexInput& in : e.inputs)
            printf("    input location %u: %s%u\n", in.location, in.semantic.c_str(), in.semanticIndex);
        printf("    uses:");
        for (uint32_t u : e.usedBindings)
            printf(" %s", pkg.bindings[u].name.c_str());
        printf("\n");
        for (const srhi::PkgBlob& blob : e.blobs)
            printf("    %-8s %zu bytes (function '%s')\n", BlobKindName(blob.kind), blob.data.size(),
                   blob.functionName.c_str());
    }
    return 0;
}

int main(int argc, char** argv)
{
    if (argc >= 3 && strcmp(argv[1], "--dump") == 0)
        return Dump(argv[2]);

    const char* input = nullptr;
    const char* output = nullptr;
    const char* headerSymbol = nullptr;
    std::vector<const char*> includes, defines, entries;
    uint32_t targets = 0;
    srhiShaderCompileDesc desc = {};

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) {
                fprintf(stderr, "srhic: missing value after '%s'\n%s", a, kUsage);
                exit(1);
            }
            return argv[++i];
        };
        if (strcmp(a, "-o") == 0)
            output = next();
        else if (strcmp(a, "-I") == 0)
            includes.push_back(next());
        else if (strncmp(a, "-I", 2) == 0)
            includes.push_back(a + 2);
        else if (strcmp(a, "-D") == 0)
            defines.push_back(next());
        else if (strncmp(a, "-D", 2) == 0)
            defines.push_back(a + 2);
        else if (strcmp(a, "-e") == 0)
            entries.push_back(next());
        else if (strcmp(a, "-g") == 0)
            desc.debugInfo = 1;
        else if (strcmp(a, "--no-opt") == 0)
            desc.noOptimize = 1;
        else if (strcmp(a, "--header") == 0)
            headerSymbol = next();
        else if (strcmp(a, "--targets") == 0) {
            std::string list = next();
            if (list.find("d3d11") != std::string::npos)
                targets |= SRHI_SHADER_TARGET_D3D11;
            if (list.find("metal") != std::string::npos)
                targets |= SRHI_SHADER_TARGET_METAL;
            if (list.find("wgsl") != std::string::npos)
                targets |= SRHI_SHADER_TARGET_WGSL;
        } else if (a[0] == '-') {
            fprintf(stderr, "srhic: unknown option '%s'\n%s", a, kUsage);
            return 1;
        } else
            input = a;
    }
    if (!input || !output) {
        fputs(kUsage, stderr);
        return 1;
    }

    desc.sourcePath = input;
    desc.includeDirs = includes.data();
    desc.includeDirCount = uint32_t(includes.size());
    desc.defines = defines.data();
    desc.defineCount = uint32_t(defines.size());
    desc.entryPoints = entries.empty() ? nullptr : entries.data();
    desc.entryPointCount = uint32_t(entries.size());
    desc.targets = targets;

    srhiShaderPackage package;
    srhiBool ok = srhiCompileShader(&desc, &package);
    if (package.diagnostics)
        fputs(package.diagnostics, stderr);
    if (!ok) {
        srhiShaderPackageFree(&package);
        return 1;
    }

    std::ofstream file(output, std::ios::binary);
    if (!file) {
        fprintf(stderr, "srhic: cannot write '%s'\n", output);
        srhiShaderPackageFree(&package);
        return 1;
    }
    if (headerSymbol) {
        file << "// Generated by srhic from " << input << ". Do not edit.\n#pragma once\n\n";
        file << "static const unsigned int " << headerSymbol << "_size = " << package.size << "u;\n";
        file << "static const unsigned char " << headerSymbol << "[] = {";
        const unsigned char* bytes = static_cast<const unsigned char*>(package.data);
        char buf[16];
        for (size_t i = 0; i < package.size; ++i) {
            snprintf(buf, sizeof(buf), "%s%u,", i % 24 == 0 ? "\n    " : "", bytes[i]);
            file << buf;
        }
        file << "\n};\n";
    } else {
        file.write(static_cast<const char*>(package.data), std::streamsize(package.size));
    }
    srhiShaderPackageFree(&package);
    return 0;
}
