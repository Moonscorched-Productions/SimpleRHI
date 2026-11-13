/**
 * @ Author: Amélie Heinrich
 * @ Create Time: 2026-11-13 00:00:00
 * @ Copyright: Copyright (c) 2026 Amélie Heinrich. All rights reserved.
 */

// Slang -> .srhs shader package compiler.
//
// One Slang session holds every requested target plus an internal WGSL target that is always present:
// WGSL is the target where [vk::binding(b, s)] survives verbatim, so its layout is the source of truth for
// (set, binding). The D3D11 and Metal layouts give the flattened register / index of the same parameter.
// All entry points are linked into a single program so every stage shares one resource layout.

#include "srhi_package.h"

#include <slang-com-ptr.h>
#include <slang.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

using Slang::ComPtr;

namespace
{
    struct Diagnostics
    {
        std::string text;

        void Add(slang::IBlob* blob)
        {
            if (blob && blob->getBufferSize())
                text.append(static_cast<const char*>(blob->getBufferPointer()), blob->getBufferSize());
        }

        void Error(const std::string& message) { text += "error: " + message + "\n"; }
        void Warning(const std::string& message) { text += "warning: " + message + "\n"; }
    };

    slang::IGlobalSession* GetGlobalSession()
    {
        static std::once_flag once;
        static ComPtr<slang::IGlobalSession> session;
        std::call_once(once, [] { slang::createGlobalSession(session.writeRef()); });
        return session.get();
    }

    bool ReadFile(const char* path, std::string& out)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return false;
        std::stringstream ss;
        ss << file.rdbuf();
        out = ss.str();
        return true;
    }

    std::string ModuleNameFromPath(const std::string& path)
    {
        size_t slash = path.find_last_of("/\\");
        std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
        size_t dot = name.find_last_of('.');
        if (dot != std::string::npos)
            name = name.substr(0, dot);
        for (char& c : name)
            if (!isalnum(static_cast<unsigned char>(c)))
                c = '_';
        return name;
    }

    srhiFormat FormatFromSlang(SlangImageFormat f)
    {
        switch (f) {
        case SLANG_IMAGE_FORMAT_rgba32f: return SRHI_FORMAT_RGBA32_FLOAT;
        case SLANG_IMAGE_FORMAT_rgba16f: return SRHI_FORMAT_RGBA16_FLOAT;
        case SLANG_IMAGE_FORMAT_rg32f: return SRHI_FORMAT_RG32_FLOAT;
        case SLANG_IMAGE_FORMAT_rg16f: return SRHI_FORMAT_RG16_FLOAT;
        case SLANG_IMAGE_FORMAT_r11f_g11f_b10f: return SRHI_FORMAT_RG11B10_FLOAT;
        case SLANG_IMAGE_FORMAT_r32f: return SRHI_FORMAT_R32_FLOAT;
        case SLANG_IMAGE_FORMAT_r16f: return SRHI_FORMAT_R16_FLOAT;
        case SLANG_IMAGE_FORMAT_rgb10_a2: return SRHI_FORMAT_RGB10A2_UNORM;
        case SLANG_IMAGE_FORMAT_rgba8: return SRHI_FORMAT_RGBA8_UNORM;
        case SLANG_IMAGE_FORMAT_rg8: return SRHI_FORMAT_RG8_UNORM;
        case SLANG_IMAGE_FORMAT_r8: return SRHI_FORMAT_R8_UNORM;
        case SLANG_IMAGE_FORMAT_rgba8_snorm: return SRHI_FORMAT_RGBA8_SNORM;
        case SLANG_IMAGE_FORMAT_r32i: return SRHI_FORMAT_R32_SINT;
        case SLANG_IMAGE_FORMAT_rgba32ui: return SRHI_FORMAT_RGBA32_UINT;
        case SLANG_IMAGE_FORMAT_rgba8ui: return SRHI_FORMAT_RGBA8_UINT;
        case SLANG_IMAGE_FORMAT_rg32ui: return SRHI_FORMAT_RG32_UINT;
        case SLANG_IMAGE_FORMAT_r32ui: return SRHI_FORMAT_R32_UINT;
        case SLANG_IMAGE_FORMAT_r16ui: return SRHI_FORMAT_R16_UINT;
        case SLANG_IMAGE_FORMAT_bgra8: return SRHI_FORMAT_BGRA8_UNORM;
        default: return SRHI_FORMAT_UNDEFINED;
        }
    }

    uint32_t StageFromSlang(SlangStage stage)
    {
        switch (stage) {
        case SLANG_STAGE_VERTEX: return SRHI_SHADER_STAGE_VERTEX;
        case SLANG_STAGE_FRAGMENT: return SRHI_SHADER_STAGE_FRAGMENT;
        case SLANG_STAGE_COMPUTE: return SRHI_SHADER_STAGE_COMPUTE;
        default: return 0;
        }
    }

    slang::TypeReflection::ScalarType ResultScalarType(slang::TypeLayoutReflection* typeLayout)
    {
        slang::TypeReflection* type = typeLayout->getResourceResultType();
        if (!type)
            return slang::TypeReflection::ScalarType::Float32;
        if (type->getKind() == slang::TypeReflection::Kind::Vector || type->getKind() == slang::TypeReflection::Kind::Matrix)
            type = type->getElementType();
        return type->getScalarType();
    }

    /// @brief Classifies one global parameter from the WGSL layout. Returns false (with an error) for
    ///        parameter kinds SimpleRHI does not support.
    bool ClassifyBinding(slang::VariableLayoutReflection* param, srhi::PkgBinding& out, Diagnostics& diag)
    {
        slang::TypeLayoutReflection* typeLayout = param->getTypeLayout();
        const char* name = param->getName() ? param->getName() : "<unnamed>";
        out.name = name;

        switch (typeLayout->getKind()) {
        case slang::TypeReflection::Kind::ConstantBuffer:
            out.type = SRHI_BINDING_TYPE_UNIFORM_BUFFER;
            return true;
        case slang::TypeReflection::Kind::SamplerState: {
            out.type = SRHI_BINDING_TYPE_SAMPLER;
            const char* typeName = typeLayout->getType()->getName();
            if (typeName && strstr(typeName, "Comparison"))
                out.samplerType = SRHI_SAMPLER_BINDING_TYPE_COMPARISON;
            return true;
        }
        case slang::TypeReflection::Kind::Resource: {
            SlangResourceShape shape = typeLayout->getResourceShape();
            SlangResourceAccess access = typeLayout->getResourceAccess();
            uint32_t base = shape & SLANG_RESOURCE_BASE_SHAPE_MASK;
            bool isArray = (shape & SLANG_TEXTURE_ARRAY_FLAG) != 0;

            if (base == SLANG_STRUCTURED_BUFFER) {
                out.type = access == SLANG_RESOURCE_ACCESS_READ ? SRHI_BINDING_TYPE_STORAGE_BUFFER
                                                                : SRHI_BINDING_TYPE_STORAGE_BUFFER_RW;
                return true;
            }

            switch (base) {
            case SLANG_TEXTURE_2D: out.textureType = isArray ? SRHI_TEXTURE_TYPE_2D_ARRAY : SRHI_TEXTURE_TYPE_2D; break;
            case SLANG_TEXTURE_CUBE: out.textureType = isArray ? SRHI_TEXTURE_TYPE_CUBE_ARRAY : SRHI_TEXTURE_TYPE_CUBE; break;
            case SLANG_TEXTURE_3D: out.textureType = SRHI_TEXTURE_TYPE_3D; break;
            default:
                diag.Error(std::string("parameter '") + name +
                           "': unsupported resource shape (supported: 2D, 2D array, cube, cube array, 3D textures and structured buffers)");
                return false;
            }
            out.multisampled = (shape & SLANG_TEXTURE_MULTISAMPLE_FLAG) != 0;

            if (access == SLANG_RESOURCE_ACCESS_READ) {
                out.type = SRHI_BINDING_TYPE_TEXTURE;
                switch (ResultScalarType(typeLayout)) {
                case slang::TypeReflection::ScalarType::Int32: out.sampleType = SRHI_TEXTURE_SAMPLE_TYPE_SINT; break;
                case slang::TypeReflection::ScalarType::UInt32: out.sampleType = SRHI_TEXTURE_SAMPLE_TYPE_UINT; break;
                default: out.sampleType = SRHI_TEXTURE_SAMPLE_TYPE_FLOAT; break;
                }
                // DepthTexture2D & co. (Slang's shadow textures) are depth-sampled.
                if (shape & SLANG_TEXTURE_SHADOW_FLAG)
                    out.sampleType = SRHI_TEXTURE_SAMPLE_TYPE_DEPTH;
            } else if (access == SLANG_RESOURCE_ACCESS_READ_WRITE) {
                diag.Error(std::string("storage texture '") + name +
                           "' is read-write, which WebGPU does not allow for most formats: declare it write-only "
                           "(WTexture2D<T> and .Store(coord, value))");
                return false;
            } else {
                out.type = SRHI_BINDING_TYPE_STORAGE_TEXTURE;
                out.storageFormat = FormatFromSlang(param->getImageFormat());
                if (out.storageFormat == SRHI_FORMAT_UNDEFINED) {
                    diag.Error(std::string("storage texture '") + name +
                               "' needs an explicit [format(\"...\")] attribute (WebGPU requires the texel format)");
                    return false;
                }
            }
            return true;
        }
        default:
            diag.Error(std::string("parameter '") + name +
                       "': unsupported global parameter. Use ConstantBuffer<T>, (RW)StructuredBuffer<T>, textures and samplers with [vk::binding(binding, set)]");
            return false;
        }
    }

    /// @brief The single binding slot a parameter occupies in a D3D11/Metal target layout.
    bool FlatSlot(slang::VariableLayoutReflection* param, uint32_t& outSlot, SlangParameterCategory& outCategory)
    {
        slang::TypeLayoutReflection* typeLayout = param->getTypeLayout();
        uint32_t count = typeLayout->getCategoryCount();
        for (uint32_t i = 0; i < count; ++i) {
            SlangParameterCategory cat = SlangParameterCategory(typeLayout->getCategoryByIndex(i));
            if (cat == SLANG_PARAMETER_CATEGORY_CONSTANT_BUFFER || cat == SLANG_PARAMETER_CATEGORY_SHADER_RESOURCE ||
                cat == SLANG_PARAMETER_CATEGORY_UNORDERED_ACCESS || cat == SLANG_PARAMETER_CATEGORY_SAMPLER_STATE) {
                outSlot = uint32_t(param->getOffset(cat));
                outCategory = cat;
                return true;
            }
        }
        return false;
    }

    /// @brief A struct's flattened (offset, size) signature, to compare layouts across targets.
    void LayoutSignature(slang::TypeLayoutReflection* type, size_t baseOffset, std::vector<size_t>& out)
    {
        if (type->getKind() == slang::TypeReflection::Kind::Struct) {
            for (uint32_t i = 0; i < type->getFieldCount(); ++i) {
                slang::VariableLayoutReflection* field = type->getFieldByIndex(i);
                LayoutSignature(field->getTypeLayout(), baseOffset + field->getOffset(), out);
            }
        } else {
            out.push_back(baseOffset);
            out.push_back(type->getSize());
        }
    }

    struct TargetInfo
    {
        SlangCompileTarget format;
        const char* profile;
        srhi::BlobKind kind;
    };
} // namespace

extern "C" srhiBool srhiCompileShader(const srhiShaderCompileDesc* desc, srhiShaderPackage* outPackage)
{
    if (!desc || !outPackage || !desc->sourcePath)
        return 0;
    *outPackage = {};
    Diagnostics diag;

    auto fail = [&]() -> srhiBool {
        if (!diag.text.empty())
            outPackage->diagnostics = strdup(diag.text.c_str());
        return 0;
    };

    slang::IGlobalSession* global = GetGlobalSession();
    if (!global) {
        diag.Error("failed to create the Slang global session");
        return fail();
    }

    std::string source;
    if (desc->source)
        source = desc->source;
    else if (!ReadFile(desc->sourcePath, source)) {
        diag.Error(std::string("cannot read '") + desc->sourcePath + "'");
        return fail();
    }

    const uint32_t targets = desc->targets ? desc->targets : SRHI_SHADER_TARGET_ALL;

    // Target 0 is always WGSL: the (set, binding) source of truth.
    std::vector<TargetInfo> targetInfos;
    targetInfos.push_back({SLANG_WGSL, nullptr, srhi::BlobKind::WGSL});
    int d3dIndex = -1, metalIndex = -1;
    if (targets & SRHI_SHADER_TARGET_D3D11) {
        d3dIndex = int(targetInfos.size());
#ifdef _WIN32
        targetInfos.push_back({SLANG_DXBC, "sm_5_0", srhi::BlobKind::DXBC});
#else
        targetInfos.push_back({SLANG_HLSL, "sm_5_0", srhi::BlobKind::HLSL});
#endif
    }
    if (targets & SRHI_SHADER_TARGET_METAL) {
        metalIndex = int(targetInfos.size());
#ifdef __APPLE__
        targetInfos.push_back({SLANG_METAL_LIB, nullptr, srhi::BlobKind::MetalLib});
#else
        targetInfos.push_back({SLANG_METAL, nullptr, srhi::BlobKind::MSL});
#endif
    }

    std::vector<slang::TargetDesc> targetDescs(targetInfos.size());
    for (size_t i = 0; i < targetInfos.size(); ++i) {
        targetDescs[i].format = targetInfos[i].format;
        if (targetInfos[i].profile)
            targetDescs[i].profile = global->findProfile(targetInfos[i].profile);
    }

    std::vector<std::string> macroStorage;
    std::vector<slang::PreprocessorMacroDesc> macros;
    macroStorage.reserve(desc->defineCount * 2);
    for (uint32_t i = 0; i < desc->defineCount; ++i) {
        std::string def = desc->defines[i];
        size_t eq = def.find('=');
        macroStorage.push_back(def.substr(0, eq));
        macroStorage.push_back(eq == std::string::npos ? "1" : def.substr(eq + 1));
    }
    for (size_t i = 0; i < macroStorage.size(); i += 2)
        macros.push_back({macroStorage[i].c_str(), macroStorage[i + 1].c_str()});

    // The source file's own directory is always searched.
    std::string sourceDir = desc->sourcePath;
    size_t slash = sourceDir.find_last_of("/\\");
    sourceDir = slash == std::string::npos ? std::string(".") : sourceDir.substr(0, slash);
    std::vector<const char*> searchPaths(desc->includeDirs, desc->includeDirs + desc->includeDirCount);
    searchPaths.push_back(sourceDir.c_str());

    std::vector<slang::CompilerOptionEntry> options;
    {
        slang::CompilerOptionEntry e;
        e.name = slang::CompilerOptionName::Optimization;
        e.value.intValue0 = desc->noOptimize ? SLANG_OPTIMIZATION_LEVEL_NONE : SLANG_OPTIMIZATION_LEVEL_DEFAULT;
        options.push_back(e);
        if (desc->debugInfo) {
            e.name = slang::CompilerOptionName::DebugInformation;
            e.value.intValue0 = SLANG_DEBUG_INFO_LEVEL_STANDARD;
            options.push_back(e);
        }
    }

    slang::SessionDesc sessionDesc;
    sessionDesc.targets = targetDescs.data();
    sessionDesc.targetCount = SlangInt(targetDescs.size());
    sessionDesc.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR;
    sessionDesc.searchPaths = searchPaths.data();
    sessionDesc.searchPathCount = SlangInt(searchPaths.size());
    sessionDesc.preprocessorMacros = macros.data();
    sessionDesc.preprocessorMacroCount = SlangInt(macros.size());
    sessionDesc.compilerOptionEntries = options.data();
    sessionDesc.compilerOptionEntryCount = uint32_t(options.size());

    ComPtr<slang::ISession> session;
    if (SLANG_FAILED(global->createSession(sessionDesc, session.writeRef()))) {
        diag.Error("failed to create a Slang session");
        return fail();
    }

    ComPtr<slang::IBlob> diagBlob;
    std::string moduleName = ModuleNameFromPath(desc->sourcePath);
    slang::IModule* module =
        session->loadModuleFromSourceString(moduleName.c_str(), desc->sourcePath, source.c_str(), diagBlob.writeRef());
    diag.Add(diagBlob);
    if (!module)
        return fail();

    // Entry points.
    std::vector<ComPtr<slang::IEntryPoint>> entryPoints;
    if (desc->entryPoints && desc->entryPointCount) {
        for (uint32_t i = 0; i < desc->entryPointCount; ++i) {
            ComPtr<slang::IEntryPoint> ep;
            if (SLANG_FAILED(module->findEntryPointByName(desc->entryPoints[i], ep.writeRef()))) {
                diag.Error(std::string("entry point '") + desc->entryPoints[i] +
                           "' not found (it needs a [shader(\"...\")] attribute)");
                return fail();
            }
            entryPoints.push_back(ep);
        }
    } else {
        for (SlangInt32 i = 0; i < module->getDefinedEntryPointCount(); ++i) {
            ComPtr<slang::IEntryPoint> ep;
            module->getDefinedEntryPoint(i, ep.writeRef());
            entryPoints.push_back(ep);
        }
    }
    if (entryPoints.empty()) {
        diag.Error("no entry points: mark functions with [shader(\"vertex\" | \"fragment\" | \"compute\")]");
        return fail();
    }

    std::vector<slang::IComponentType*> components = {module};
    for (auto& ep : entryPoints)
        components.push_back(ep.get());
    ComPtr<slang::IComponentType> composite;
    diagBlob = nullptr;
    if (SLANG_FAILED(session->createCompositeComponentType(components.data(), SlangInt(components.size()),
                                                           composite.writeRef(), diagBlob.writeRef()))) {
        diag.Add(diagBlob);
        return fail();
    }
    diag.Add(diagBlob);

    ComPtr<slang::IComponentType> linked;
    diagBlob = nullptr;
    if (SLANG_FAILED(composite->link(linked.writeRef(), diagBlob.writeRef()))) {
        diag.Add(diagBlob);
        return fail();
    }
    diag.Add(diagBlob);

    srhi::Package pkg;
    bool ok = true;

    // --- Global parameters ------------------------------------------------------------------------
    slang::ProgramLayout* wgslLayout = linked->getLayout(0, diagBlob.writeRef());
    slang::ProgramLayout* d3dLayout = d3dIndex >= 0 ? linked->getLayout(d3dIndex) : nullptr;
    slang::ProgramLayout* metalLayout = metalIndex >= 0 ? linked->getLayout(metalIndex) : nullptr;
    if (!wgslLayout) {
        diag.Error("failed to compute the program layout");
        return fail();
    }

    for (uint32_t i = 0; i < wgslLayout->getParameterCount(); ++i) {
        slang::VariableLayoutReflection* param = wgslLayout->getParameterByIndex(i);
        srhi::PkgBinding b;
        if (!ClassifyBinding(param, b, diag)) {
            ok = false;
            continue;
        }

        b.binding = uint32_t(param->getOffset(SLANG_PARAMETER_CATEGORY_DESCRIPTOR_TABLE_SLOT));
        b.set = uint32_t(param->getBindingSpace(SLANG_PARAMETER_CATEGORY_DESCRIPTOR_TABLE_SLOT));
        if (b.set > SRHI_UNIFORM_SET_INDEX) {
            diag.Error("parameter '" + b.name + "' uses set " + std::to_string(b.set) + ", sets must be 0..3");
            ok = false;
            continue;
        }
        if (b.set == SRHI_UNIFORM_SET_INDEX &&
            (b.type != SRHI_BINDING_TYPE_UNIFORM_BUFFER || b.binding >= SRHI_MAX_UNIFORM_SLOTS)) {
            diag.Error("parameter '" + b.name + "': set 3 is reserved for ConstantBuffer<T> transient uniforms in bindings 0.." +
                       std::to_string(SRHI_MAX_UNIFORM_SLOTS - 1));
            ok = false;
            continue;
        }

        // Layout sizes/strides, checked for equality across targets.
        std::vector<slang::ProgramLayout*> layouts = {wgslLayout};
        if (d3dLayout)
            layouts.push_back(d3dLayout);
        if (metalLayout)
            layouts.push_back(metalLayout);

        if (b.type == SRHI_BINDING_TYPE_UNIFORM_BUFFER || b.type == SRHI_BINDING_TYPE_STORAGE_BUFFER ||
            b.type == SRHI_BINDING_TYPE_STORAGE_BUFFER_RW) {
            std::vector<size_t> reference;
            for (size_t t = 0; t < layouts.size(); ++t) {
                slang::TypeLayoutReflection* element = layouts[t]->getParameterByIndex(i)->getTypeLayout()->getElementTypeLayout();
                std::vector<size_t> signature;
                LayoutSignature(element, 0, signature);
                size_t size = b.type == SRHI_BINDING_TYPE_UNIFORM_BUFFER ? element->getSize() : element->getStride();
                if (t == 0) {
                    reference = signature;
                    if (b.type == SRHI_BINDING_TYPE_UNIFORM_BUFFER)
                        b.dataSize = uint32_t(size);
                    else
                        b.structureStride = uint32_t(size);
                } else if (signature != reference) {
                    diag.Error("parameter '" + b.name +
                               "': the struct layout differs between targets. Use 4-byte scalars, float2/float4 and "
                               "float4x4 only, and pad float3 to float4");
                    ok = false;
                    break;
                }
            }
        }

        if (d3dLayout) {
            slang::VariableLayoutReflection* p = d3dLayout->getParameterByIndex(i);
            SlangParameterCategory cat;
            if (FlatSlot(p, b.d3dSlot, cat)) {
                switch (cat) {
                case SLANG_PARAMETER_CATEGORY_CONSTANT_BUFFER: b.d3dClass = srhi::D3DClass::B; break;
                case SLANG_PARAMETER_CATEGORY_SHADER_RESOURCE: b.d3dClass = srhi::D3DClass::T; break;
                case SLANG_PARAMETER_CATEGORY_UNORDERED_ACCESS: b.d3dClass = srhi::D3DClass::U; break;
                case SLANG_PARAMETER_CATEGORY_SAMPLER_STATE: b.d3dClass = srhi::D3DClass::S; break;
                default: break;
                }
            }
        }
        if (metalLayout) {
            slang::VariableLayoutReflection* p = metalLayout->getParameterByIndex(i);
            SlangParameterCategory cat;
            if (FlatSlot(p, b.metalSlot, cat)) {
                switch (b.type) {
                case SRHI_BINDING_TYPE_TEXTURE:
                case SRHI_BINDING_TYPE_STORAGE_TEXTURE: b.metalClass = srhi::MetalClass::Texture; break;
                case SRHI_BINDING_TYPE_SAMPLER: b.metalClass = srhi::MetalClass::Sampler; break;
                default: b.metalClass = srhi::MetalClass::Buffer; break;
                }
            }
        }

        if (pkg.FindBinding(b.set, b.binding)) {
            diag.Error("parameter '" + b.name + "' reuses set " + std::to_string(b.set) + " binding " +
                       std::to_string(b.binding));
            ok = false;
            continue;
        }
        pkg.bindings.push_back(b);
    }

    // --- Entry points -----------------------------------------------------------------------------
    for (uint32_t i = 0; i < wgslLayout->getEntryPointCount(); ++i) {
        slang::EntryPointReflection* ep = wgslLayout->getEntryPointByIndex(i);
        srhi::PkgEntry entry;
        entry.name = ep->getName();
        entry.stage = StageFromSlang(ep->getStage());
        if (!entry.stage) {
            diag.Error("entry point '" + entry.name + "': only vertex, fragment and compute stages are supported");
            ok = false;
            continue;
        }
        if (entry.stage == SRHI_SHADER_STAGE_COMPUTE) {
            SlangUInt size[3] = {1, 1, 1};
            ep->getComputeThreadGroupSize(3, size);
            for (int a = 0; a < 3; ++a)
                entry.threadgroupSize[a] = uint32_t(size[a]);
        }

        if (entry.stage == SRHI_SHADER_STAGE_VERTEX) {
            // Varying inputs: either loose parameters or the fields of a struct parameter.
            auto addInput = [&](slang::VariableLayoutReflection* v, size_t base) {
                const char* semantic = v->getSemanticName();
                if (semantic && (strncmp(semantic, "SV_", 3) == 0 || strncmp(semantic, "sv_", 3) == 0))
                    return;
                if (v->getTypeLayout()->getCategoryCount() == 0)
                    return;
                bool isVarying = false;
                for (uint32_t c = 0; c < v->getTypeLayout()->getCategoryCount(); ++c)
                    if (SlangParameterCategory(v->getTypeLayout()->getCategoryByIndex(c)) == SLANG_PARAMETER_CATEGORY_VARYING_INPUT)
                        isVarying = true;
                if (!isVarying)
                    return;
                if (!semantic) {
                    diag.Error("vertex input '" + std::string(v->getName() ? v->getName() : "?") + "' of '" + entry.name +
                               "' needs a semantic (e.g. POSITION, TEXCOORD0)");
                    ok = false;
                    return;
                }
                srhi::PkgVertexInput in;
                in.location = uint32_t(base + v->getOffset(SLANG_PARAMETER_CATEGORY_VARYING_INPUT));
                in.semantic = semantic;
                in.semanticIndex = uint32_t(v->getSemanticIndex());
                entry.inputs.push_back(in);
            };
            for (uint32_t p = 0; p < ep->getParameterCount(); ++p) {
                slang::VariableLayoutReflection* param = ep->getParameterByIndex(p);
                slang::TypeLayoutReflection* type = param->getTypeLayout();
                if (type->getKind() == slang::TypeReflection::Kind::Struct) {
                    size_t base = param->getOffset(SLANG_PARAMETER_CATEGORY_VARYING_INPUT);
                    for (uint32_t f = 0; f < type->getFieldCount(); ++f)
                        addInput(type->getFieldByIndex(f), base);
                } else {
                    addInput(param, 0);
                }
            }
        }

        // Statically used resources, from the WGSL target where (set, binding) is preserved.
        {
            ComPtr<slang::IMetadata> metadata;
            bool haveMetadata = SLANG_SUCCEEDED(linked->getEntryPointMetadata(SlangInt(i), 0, metadata.writeRef()));
            for (uint32_t b = 0; b < pkg.bindings.size(); ++b) {
                bool used = true;
                if (haveMetadata)
                    metadata->isParameterLocationUsed(SLANG_PARAMETER_CATEGORY_DESCRIPTOR_TABLE_SLOT, pkg.bindings[b].set,
                                                      pkg.bindings[b].binding, used);
                if (used)
                    entry.usedBindings.push_back(b);
            }
        }

        for (size_t t = 0; t < targetInfos.size(); ++t) {
            bool wanted = t == 0 ? (targets & SRHI_SHADER_TARGET_WGSL) != 0 : true;
            if (!wanted)
                continue;
            ComPtr<slang::IBlob> code;
            diagBlob = nullptr;
            if (SLANG_FAILED(linked->getEntryPointCode(SlangInt(i), SlangInt(t), code.writeRef(), diagBlob.writeRef()))) {
                diag.Add(diagBlob);
                diag.Error("code generation failed for entry point '" + entry.name + "'");
                ok = false;
                continue;
            }
            diag.Add(diagBlob);
            srhi::PkgBlob blob;
            blob.kind = targetInfos[t].kind;
            blob.functionName = entry.name;
            const uint8_t* bytes = static_cast<const uint8_t*>(code->getBufferPointer());
            blob.data.assign(bytes, bytes + code->getBufferSize());
            entry.blobs.push_back(std::move(blob));
        }

        pkg.entries.push_back(std::move(entry));
    }

    if (!ok)
        return fail();

    std::vector<uint8_t> bytes = srhi::WritePackage(pkg);
    outPackage->data = malloc(bytes.size());
    memcpy(outPackage->data, bytes.data(), bytes.size());
    outPackage->size = bytes.size();
    if (!diag.text.empty())
        outPackage->diagnostics = strdup(diag.text.c_str());
    return 1;
}

extern "C" void srhiShaderPackageFree(srhiShaderPackage* package)
{
    if (!package)
        return;
    free(package->data);
    free(package->diagnostics);
    *package = {};
}
