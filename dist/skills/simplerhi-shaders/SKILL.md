---
name: simplerhi-shaders
description: Write Slang shaders for SimpleRHI and compile them into .srhs shader packages with srhic or srhiCompileShader. Use when creating or editing a .slang file in a SimpleRHI project, declaring shader resources (SRHI_BINDING, SRHI_UNIFORMS), fixing srhic compile errors, matching CPU structs and vertex layouts to a shader, or setting up shader compilation in a build.
---

# SimpleRHI shaders

Shaders are written once in Slang (HLSL syntax) and compiled offline by `srhic` into one `.srhs` package per
source file. A package holds every entry point for every backend (D3D11 bytecode, Metal, WGSL) plus reflection,
and is loaded at runtime with `srhiShaderModuleCreate`. Every shader is also compiled to WGSL while packaging,
so WebGPU's restrictions apply to all targets: a shader that compiles is portable.

## Skeleton

```hlsl
#include "SimpleRHI.slang"                       // SRHI_BINDING, SRHI_UNIFORMS

struct DrawConstants
{
    float4x4 mvp;
    float4 tint;
};
SRHI_UNIFORMS(0) ConstantBuffer<DrawConstants> gDraw;   // set 3, slot 0: srhiRenderEncoderSetUniforms(enc, 0, ...)

SRHI_BINDING(0, 0) Texture2D<float4> gAlbedo;           // (binding, set), set in 0..2: a resource set
SRHI_BINDING(1, 0) SamplerState gSampler;

struct VSIn
{
    float3 position : POSITION;    // location 0
    float2 uv : TEXCOORD0;         // location 1
};

struct VSOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VSOut vsMain(VSIn input)
{
    VSOut o;
    o.position = mul(gDraw.mvp, float4(input.position, 1.0));
    o.uv = input.uv;
    return o;
}

[shader("fragment")]
float4 fsMain(VSOut input) : SV_Target0
{
    return gAlbedo.Sample(gSampler, input.uv) * gDraw.tint;
}
```

## Rules

Entry points
- Mark every entry point with `[shader("vertex")]`, `[shader("fragment")]` or `[shader("compute")]`. Only these
  three stages exist. Compute entry points also need `[numthreads(x, y, z)]`.
- Entry points are looked up by function name at pipeline creation (`vertexEntry = "vsMain"`).
- Keep the vertex and fragment shader of a pipeline in the same file: they must come from the same module.
- A file can hold many entry points (several pipelines, compute and render together). Resources used by any of
  them share one layout.

Resources
- Every resource is a global with an explicit binding: `SRHI_BINDING(binding, set)` with set 0, 1 or 2.
- Set 3 is reserved for transient uniforms: `SRHI_UNIFORMS(slot) ConstantBuffer<T> name;` with slot 0..3 and a
  struct of at most 4096 bytes. Nothing else may live in set 3.
- A (set, binding) pair can be used once per file.
- Supported types and the binding type they reflect to:

  | Slang | `srhiBindingType` | Notes |
  |---|---|---|
  | `ConstantBuffer<T>` | `UNIFORM_BUFFER` | buffer with `UNIFORM` usage |
  | `StructuredBuffer<T>` | `STORAGE_BUFFER` | read-only; `STORAGE` usage, `structureStride = sizeof(T)` |
  | `RWStructuredBuffer<T>` | `STORAGE_BUFFER_RW` | fragment and compute only; `STORAGE_RW` usage |
  | `Texture2D`, `Texture2DArray`, `TextureCube`, `TextureCubeArray`, `Texture3D`, `Texture2DMS` | `TEXTURE` | |
  | `DepthTexture2D`, `DepthTexture2DArray` | `TEXTURE` (depth sample type) | for comparison sampling |
  | `WTexture2D<T>`, `WTexture2DArray<T>` | `STORAGE_TEXTURE` | write-only, needs `[format("...")]` |
  | `SamplerState` | `SAMPLER` | |
  | `SamplerComparisonState` | `SAMPLER` (comparison) | sampler created with `compareEnable` |

- Loose global uniforms (`uniform float4 x;`), `cbuffer` blocks without a binding and byte-address buffers are
  not supported. Put constants in a struct behind `ConstantBuffer<T>`.
- Writable resources are not allowed in vertex shaders.

Struct layout (uniform and structured buffers)
- The struct must have the same layout on every target, and the compiler rejects it otherwise. Use 4-byte
  scalars (`float`, `int`, `uint`), `float2`, `float4` and `float4x4`. **Use `float4` instead of `float3`.** Avoid `bool` and
  `float3x3`.
- Mirror the struct on the CPU with plain 4-byte fields in the same order, sized to a multiple of 16 bytes.
- Matrices are column-major in memory, the same as glm: upload a `glm::mat4` as is and write `mul(M, v)` in the
  shader for `M * v`.

Vertex inputs
- Every field of the vertex input struct needs a semantic (`POSITION`, `NORMAL`, `TEXCOORD0`, `COLOR`, ...).
- The attribute `location` in `srhiVertexLayout` is the index of the field in the input struct, in declaration
  order. The semantic name itself is free. Every input field needs a matching attribute.
- `SV_VertexID` and `SV_InstanceID` parameters are fine and are not attributes. `SV_InstanceID` does not include
  `firstInstance` on D3D11 and Metal.

Textures
- Storage textures are write-only and need a texel format:
  `SRHI_BINDING(0, 0) [format("rgba16f")] WTexture2D<float4> gOut;` then `gOut.Store(coord, value);`.
  To read and write the same image, bind it once as a sampled texture and write to another one.
- Comparison sampling needs a depth texture type:
  `DepthTexture2D gShadow; SamplerComparisonState gCmp; gShadow.SampleCmpLevelZero(gCmp, uv, depth)`.
- 32-bit float textures (`R32_FLOAT`, `RGBA32_FLOAT`) cannot be filtered on WebGPU: read them with `Load()` or a
  nearest sampler, and set the layout entry's `sampleType` to `SRHI_TEXTURE_SAMPLE_TYPE_UNFILTERABLE_FLOAT` (and
  the sampler's entry to `NON_FILTERING`). Reflection defaults to `FLOAT` / `FILTERING`.
- Integer textures (`Texture2D<uint>`) are read with `Load()`; set `sampleType` to `UINT` / `SINT`.
- In vertex and compute shaders use `SampleLevel` (there are no derivatives outside fragment shaders).

Other
- Atomics on storage buffers: `RWStructuredBuffer<Atomic<uint>> gCounter; gCounter[0].add(1u);` (compute only
  for portability; `Interlocked*` on plain `uint` is rejected by WGSL).
- Coordinates are identical on every backend: clip-space Y up, depth 0..1, UV and framebuffer origin top-left.
- sRGB render targets and swapchains encode on write: return linear color from the fragment shader.
- Shared code goes in a header that is `#include`d (pass its folder with `-I`). Use `#pragma once` in headers.

## Compiling

```
srhic input.slang -o output.srhs [-I dir]... [-D NAME[=VALUE]]... [-e entry]...
      [--targets d3d11,metal,wgsl] [-g] [--no-opt] [--header symbol]
srhic --dump package.srhs
```

- Always pass `-I <folder containing SimpleRHI.slang>`.
- With no `-e`, every function marked `[shader(...)]` is compiled.
- `--targets` limits the package to some backends (default: all). Packaging for all three works on every host:
  where the native toolchain is missing (fxc off Windows, the Metal compiler off macOS), source text is stored
  and compiled when the module is loaded.
- `-g` keeps debug info, `--no-opt` skips optimization.
- `--header name` writes a C header (`static const unsigned char name[]`, `name_size`) instead of a binary, to
  embed a shader in the executable: `srhiShaderModuleCreate(device, name, name_size, "label")`.
- `--dump` prints a package's bindings (with the D3D11 register and Metal index of each), entry points, vertex
  inputs and code blobs. Use it to see what `location` each vertex input got and which bindings an entry uses.
- A package is tied to the `srhic` version that built it: "not a valid shader package" at load time means the
  shaders must be recompiled.

Runtime compilation (tools, hot reload) uses the same compiler through the API, in a build that defines
`SRHI_COMPILER_IMPLEMENTATION` and links Slang:

```c
srhiShaderCompileDesc desc = {0};
desc.sourcePath = "shaders/triangle.slang";
const char* includes[] = {"path/to/simplerhi"};
desc.includeDirs = includes;
desc.includeDirCount = 1;
srhiShaderPackage package;
if (srhiCompileShader(&desc, &package))
    module = srhiShaderModuleCreate(device, package.data, package.size, "triangle");
if (package.diagnostics)
    fputs(package.diagnostics, stderr);
srhiShaderPackageFree(&package);
```

## Common compile errors

| Message | Fix |
|---|---|
| `unsupported global parameter ...` | Give the global a `SRHI_BINDING(b, s)` and a supported type; wrap loose uniforms in a `ConstantBuffer<T>` |
| `the struct layout differs between targets ...` | Replace `float3` with `float4`, remove `bool` / `float3x3`, keep 4-byte scalars |
| `set 3 is reserved for ConstantBuffer<T> transient uniforms ...` | Move textures, samplers and buffers to sets 0..2 |
| `... reuses set S binding B` | Two globals share a (binding, set) pair |
| `storage texture ... needs an explicit [format("...")]` | Add `[format("rgba8")]` (or `rgba16f`, `r32f`, ...) |
| `storage texture ... is read-write ...` | Declare `WTexture2D<T>` and use `.Store`, not `RWTexture2D` |
| `vertex input ... needs a semantic` | Add `: POSITION`, `: TEXCOORD0`, ... to the field |
| `entry point ... not found` | The function lacks `[shader("...")]`, or the name passed with `-e` is wrong |
| `no entry points` | No function is marked `[shader("...")]` |

After changing a shader's resources, update the matching CPU side: the resource set layout (regenerate from
reflection), the bindings passed to `srhiResourceSetCreate`, the uniform struct and the vertex layout.
