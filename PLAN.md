# SimpleRHI — Implementation Plan

## Context

AGFX (`~/dev/libs/agfx`) is a modern-GPU RHI (Vulkan 1.4 / D3D12 / Metal 4, bindless, mesh shaders, ray tracing). That is too much for small games and rules out older hardware. SimpleRHI is its counterpart:

- **D3D11 feature level** (target FL 11_0).
- **Metal-shaped API**: command buffer → render/compute/blit encoders, render pass descriptors with load/store actions, baked pipeline states.
- **Three backends built together**: D3D11, Metal 3, WebGPU (native via wgpu-native for testing, emscripten for the browser).
- **Shaders**: Slang, compiled offline into one package per shader file.
- **Out of scope**: multithreaded recording, bindless, ray tracing, mesh shaders, tessellation, Linux/Android (DXVK covers Vulkan).

Decisions taken:
- **API**: a pure C API in a single public header, `Include/SimpleRHI.h`. It covers both the runtime and the shader compiler interface.
- **Build**: xmake, matching AGFX. SDL3 is used only by examples and tools; the RHI takes native handles.
- **WebGPU constraints shape the design**: resource sets (bind groups), depth-stencil state inside the PSO, a transient uniform ring, async readback, and timestamps at pass boundaries.

## Layout

```
Include/SimpleRHI.h                  single public header (runtime + compiler API), prefix srhi / SRHI_
Source/SimpleRHI/
  srhi_internal.h                    backend vtable, shared validation, format tables
  srhi_common.c                      dispatch, validation, package parsing, logging
  srhi_metal.mm                      Metal 3 backend (Obj-C++, ARC)
  srhi_d3d11.cpp                     D3D11 backend (Windows only)
  srhi_webgpu.cpp                    WebGPU backend (standard webgpu.h; wgpu-native / emdawnwebgpu)
  srhi_compiler.cpp                  Slang-based compiler (separate lib: SimpleRHICompiler)
Source/SimpleRHI/Shaders/SimpleRHI.slang   shared include: binding macros, uniform-slot convention
Source/ImGui/imgui_impl_srhi.{h,c}   ImGui renderer backend (shaders embedded as package bytes)
Source/Tools/ShaderCompiler/         srhic CLI → Content/Cache/Shaders/*.srhs
Source/Tools/AssetBaker/             cgltf + stb_image + ANBC → Content/Cache/{Models,Textures}
Source/Examples/Common/              SDL3 app shell, camera, model loader (.srm), GPU profiler
Source/Examples/<Name>/
Tests/Framework/                     ported from agfx_tests (test_framework, test_compare, FLIP)
Tests/Cases/                         test_*.c(pp)
Tests/Golden/                        shared goldens (+ optional Golden/<backend>/ overrides)
Tools/TestReport/index.html          adapted from agfx tools/test_report
Content/Cache/                       generated, gitignored
```

## Reuse from AGFX (same author, copy and adapt)

| From `~/dev/libs/agfx/` | To |
|---|---|
| `src/agfx/agfx_tests/test_framework.{h,cpp}`, `test_compare.{h,cpp}`, `FLIP/` | `Tests/Framework/` (drop `TestApi`; add `backend` to results.json) |
| `tools/test_report/index.html` | `Tools/TestReport/index.html` (add backend filter/column) |
| `src/agfx/agfx_demo/camera.*`, `gltf_scene.*`, `csm.*`, `deferred_renderer.*`, `gpu_profiler.*`, `imgui_impl_sdl3.*` | `Source/Examples/Common/` + examples, rewritten against srhi |
| `src/agfx/agfx_imgui/imgui_impl_agfx.*` | structure for `imgui_impl_srhi` |
| `agfx_native.h` doc conventions (borrowed refs, no ownership transfer) | native-access section of `SimpleRHI.h` |
| Doc-comment style (`/// @brief`), `agfxLogFunction` / allocator callbacks | same in srhi |

## Public API (SimpleRHI.h) — shape

Opaque handles: `srhiDevice`, `srhiBuffer`, `srhiTexture`, `srhiTextureView`, `srhiSampler`, `srhiShaderModule`, `srhiRenderPipeline`, `srhiComputePipeline`, `srhiResourceSetLayout`, `srhiResourceSet`, `srhiCommandBuffer`, `srhiRenderEncoder`, `srhiComputeEncoder`, `srhiBlitEncoder`, `srhiQuerySet`, `srhiSwapchain`.

- **Device**: `srhiDeviceCreate({backend: AUTO|D3D11|METAL|WEBGPU, debug, log, alloc})` and `srhiDeviceGetInfo` (name, backend, limits, format caps, BC/ASTC support, timestamp support). One queue. A fixed number of frames in flight (default 2) handled internally. `srhiDevicePoll` fires async callbacks, and `srhiDeviceWaitIdle` blocks until the GPU is idle.
- **Buffers**: usage flags VERTEX|INDEX|UNIFORM|STORAGE|INDIRECT|COPY_SRC|COPY_DST. `srhiBufferWrite(device, buf, off, data, size)` has queue-ordered upload semantics (WebGPU `writeBuffer`, D3D11 `UpdateSubresource`, Metal staging + blit). Readback is async only: `srhiCommandBufferReadBuffer(cb, buf, off, size, callback, user)`.
- **Textures**: 2D, 2D array, cube, 3D; mips; MSAA (1/4). Usage SAMPLED|STORAGE|RENDER_TARGET. `srhiTextureWrite(device, tex, mip, slice, region, data, rowPitch)` and an async `srhiCommandBufferReadTexture`. `srhiTextureView` covers mip/slice subranges and format reinterpretation (sRGB↔linear).
- **Formats**:
  - Color: R8, RG8, RGBA8(+sRGB), BGRA8(+sRGB), R16F, RG16F, RGBA16F, R32F/U/I, RG32F, RGBA32F, RGB10A2, RG11B10F
  - Depth: D16, D32F, D32F_S8 (no D24S8, since Apple Silicon lacks it)
  - Compressed: BC1–7; ASTC 4x4 behind a capability flag
- **Samplers**: filter, mip filter, address modes, anisotropy, compare func, LOD clamp.
- **Shaders**: `srhiShaderModuleCreate(device, packageBytes, size)` loads a `.srhs` package; entry points are looked up by name (Metal library style).
- **Resource sets** (bind groups):
  - `srhiResourceSetLayoutCreate(entries[]: {binding, type: UNIFORM|STORAGE_RO|STORAGE_RW|TEXTURE|STORAGE_TEXTURE|SAMPLER, stages})`, plus `srhiShaderModuleGetSetLayoutDesc` to fill it from reflection.
  - `srhiResourceSetCreate(layout, bindings[])` is immutable.
  - Encoders call `SetResourceSet(index 0..2, set)`.
  - Set 3 is reserved for transient uniforms.
- **Transient uniforms**: `srhiRenderEncoderSetUniforms(enc, slot, data, size)` and the compute equivalent. They map to Metal `set*Bytes`, D3D11 `MAP_NO_OVERWRITE` ring + `*SetConstantBuffers1`, and WebGPU group 3 with dynamic offsets.
- **Render pipeline desc**: vertex + fragment entry, vertex layout (buffers/attributes/step rate), primitive topology, cull/front-face/fill/depth bias/depth clamp, depth-stencil state, MRT color formats with blend state, depth format, sample count, set layouts.
- **Compute pipeline desc**: entry point + set layouts. Threadgroup size comes from reflection.
- **Command buffer**: `srhiCommandBufferBegin(device)`, then `srhiCommandBufferCommit(cb)` (optional completion callback). Encoders:
  - Render: `srhiRenderEncoderBegin(cb, passDesc)`, where `passDesc` lists color/depth attachments as `{texture|view, mip, slice, load: LOAD|CLEAR|DONT_CARE, store: STORE|DONT_CARE|RESOLVE, resolveTarget, clear}` plus optional `timestampWrites {querySet, begin, end}`. Supports SetPipeline, SetVertexBuffer, SetIndexBuffer, SetResourceSet, SetUniforms, SetViewport, SetScissor, SetBlendColor, SetStencilRef, Draw, DrawIndexed (with base vertex and first instance), DrawIndirect, DrawIndexedIndirect, and End.
  - Compute: Begin(with timestampWrites), SetPipeline, SetResourceSet, SetUniforms, Dispatch, DispatchIndirect, End.
  - Blit: copy buffer↔buffer/texture, texture↔texture, GenerateMipmaps, End.
  - Debug groups and markers on all encoders.
- **Hazards**: no barriers in the API. D3D11, Metal 3 (tracked resources) and WebGPU all track them automatically.
- **Timestamps**: `srhiQuerySetCreate(count)`, written only at pass boundaries via `timestampWrites`. Results are read with `srhiQuerySetGetResults(qs, first, count, uint64_t* ns) → bool ready`.
  - D3D11: TIMESTAMP + DISJOINT queries, `GetData`.
  - Metal: `MTLCounterSampleBuffer` at stage boundaries.
  - WebGPU: `resolveQuerySet` + async map (feature-gated).
- **Swapchain**:
  - Created from a native handle: `HWND`, `CAMetalLayer*`, or an HTML canvas selector.
  - Options: `{width, height, colorSpace: SRGB|HDR10_PQ|SCRGB_LINEAR, vsync, allowTearing}`.
  - Calls: `srhiSwapchainAcquire → srhiTexture*` (returns NULL when minimized), `srhiCommandBufferPresent`, `srhiSwapchainResize`, `srhiSwapchainGetDisplayInfo` (HDR support, max luminance, EDR headroom).
  - Fullscreen is borderless via SDL plus a resize; the D3D11 backend uses a flip-model swapchain (`FLIP_DISCARD`, tearing flag).
- **Native access**: `srhiGetNative*` functions return borrowed `void*` for the device, context/queue, command buffer (only valid when no encoder is open), texture and buffer. That's `ID3D11Device*`/`ID3D11DeviceContext*`, `id<MTLDevice>`/`id<MTLCommandBuffer>`/`id<MTLTexture>`, and `WGPUDevice`/`WGPUTexture`. This is enough for DLSS/FSR (D3D11) and MetalFX (Metal).
- **Compiler API**, implemented in the separate `SimpleRHICompiler` lib that links libslang:
  - `srhiCompileShader(&desc, &outPackage)`: source path, entry points, defines, include dirs, targets bitmask (DXBC|METAL|WGSL), debug info. `srhiPackageFree` releases the output.
  - Metal: Slang → MSL, then `xcrun metal`/`metallib` when available; otherwise the MSL source is stored and compiled at load time with `newLibraryWithSource`.
  - D3D11: Slang DXBC on Windows; otherwise the HLSL source is stored and compiled at load time with `D3DCompile`. Every host can therefore produce packages for every target.

### Shader package (.srhs)
- Header with version and target mask.
- Per entry: name, stage (VS/FS/CS), threadgroup size, and code per target (DXBC / metallib / MSL text / HLSL text / WGSL).
- Per entry reflection:
  - Resources: `(set, binding, type)` → D3D11 register (t/s/b/u) and Metal index (buffer/texture/sampler).
  - Vertex inputs: location → semantic.
  - Uniform slot sizes.
- Metal vertex buffers use the top buffer indices (30 downward) so they never collide with resource buffers.

## Phases

0. **Scaffolding**
   - Add `xmake.lua` with SDL3, imgui (docking), cgltf, stb, glm and wgpu-native; libslang from `/usr/local` on macOS.
   - Add `.gitignore` (build/, Content/Cache/, Tests/Output/), the `SimpleRHI.h` skeleton, and `srhi_internal.h` with the backend vtable.
   - **Slang spike**: compile one shader using `[vk::binding(b,s)]` globals to HLSL/DXBC, MSL and WGSL, and confirm the register/index assignment is readable from reflection. This settles the binding convention in `SimpleRHI.slang`.
1. **Compiler**: `srhi_compiler.cpp`, `.srhs` writer/reader, the `srhic` CLI, and an xmake rule that compiles `Content/Shaders/*.slang` and `Tests/Shaders/*.slang` into `Content/Cache/Shaders/`.
2. **Metal 3 backend plus test framework**
   - Backend: device, buffers, textures, views, samplers, modules, pipelines, resource sets, uniforms ring, encoders, readback.
   - Test framework: port it and add the `--backend` / `--update-goldens` / `--filter` flags.
3. **WebGPU backend** on wgpu-native (macOS, native), brought to the same tests passing. Goldens are shared across backends with a FLIP threshold; a backend-specific override is added only when unavoidable.
4. **D3D11 backend**: written in this repo, but it can only be built and validated on a Windows machine (see Verification).
5. **Swapchain, HDR/fullscreen, timestamps, native access** across all three backends.
6. **ImGui backend** `imgui_impl_srhi`: dynamic VB/IB via `srhiBufferWrite`, a font texture, and one pipeline.
7. **Asset baker** (`srhi_bake`):
   - cgltf loads `.gltf`/`.glb` (Intel Sponza, DamagedHelmet). Textures are compressed with ANBC:
     - BaseColor/Emissive → BC7 sRGB
     - Normal → BC5
     - Metal/Roughness/ORM → BC7 linear
     - `lake_pier_4k.hdr` → BC6H
     - `--astc` additionally emits ASTC 4x4
   - Output goes to `Content/Cache/`:
     - `.srm`: an interleaved vertex stream, a u32 index buffer, submeshes with bounds, and a material table
     - `.srt`: a header plus the mip chain
   - The baker is incremental (skips files whose source mtime is unchanged).
   - Notes: ANBC needs Metal 4 on macOS (so the baker compiles as `.mm`) and Vulkan on Windows. Its 4096² limit when generating mips matches Sponza's 4K textures.
8. **Examples** (SDL3 shell, shared camera, ImGui overlay, backend chosen from the command line):
   - `Triangle` (smoke test)
   - `Sprites2D`: a batched quad renderer, the 2D use case
   - `PBRHelmet`: IBL from the HDRI via a compute equirect→cube pass plus prefilter/irradiance and a BRDF LUT
   - `ShadowMapping`: cascaded shadow maps on Sponza (D32F array, compare sampler)
   - `Deferred`: Sponza G-buffer (MRT), compute or fullscreen lighting, many point lights
   - `Bloom`: HDR scene, downsample/upsample mip chain through texture views, tonemap, HDR swapchain toggle
   - `GPUProfiler`: per-pass timestamp queries shown in ImGui, used by the other examples too
9. **Web build**: emscripten toolchain via xmake (`xmake f -p wasm`), examples served as static pages, and Content/Cache preloaded into the virtual FS.

## Tests (golden-backed, per backend)

- **Buffer tests** (memcmp): copy buffer↔buffer, `srhiBufferWrite`, compute write/atomics/shared memory, uniforms ring across many draws, storage RO/RW, readback of texture→buffer for mip and slice, indirect args, timestamp monotonicity (validation).
- **Texture tests** (FLIP):
  - Draws: triangle, indexed u16/u32, base vertex/first instance, instancing, lines/points, winding/cull, viewport/scissor, depth test/clamp/bias, stencil, blend factors/ops, MRT.
  - Pass actions: load/clear/dont-care, MSAA resolve.
  - Render targets: render to mip, slice, cube face.
  - Sampling: 2D, array, cube, 3D, BC formats, comparison samplers, address/filter modes, sRGB views.
  - Mipmaps: GenerateMipmaps.
  - Compute: storage texture write.
  - HDR: RGBA16F output (HDR FLIP).
- **Output**: `Tests/Output/results.json` plus artifacts, viewed with `Tools/TestReport/index.html` through `python3 -m http.server`.

## Verification

- `xmake f -m debug && xmake` builds the RHI, compiler, srhic, baker, tests and examples on macOS.
- `xmake run srhi_tests --backend metal` and `--backend webgpu` should pass everything. To view the report, run `python3 -m http.server` and open `/Tools/TestReport/`.
- `xmake run srhi_bake` should populate `Content/Cache`. Then each example should run under `--backend metal|webgpu` with ImGui showing GPU timings. Metal validation is on in debug builds (`MTL_DEBUG_LAYER=1`, `MTL_SHADER_VALIDATION=1`).
- D3D11 can't be built on this Mac. On Windows: `xmake && xmake run srhi_tests --backend d3d11` with the debug layer enabled, then run the examples. The backend is considered done only after that run.
- Web: `xmake f -p wasm && xmake`, serve the build directory, and open the examples in Chrome or Safari.

## Slang spike results (Slang 2026.1, done)

- **Convention**: shaders declare globals with `[vk::binding(binding, set)]`. Set 3 is the transient-uniform set.
- **WGSL** emits `@group(set) @binding(binding)` verbatim.
- **HLSL SM5** flattens to per-type registers (`b`/`t`/`s`/`u`) in declaration order. In a pixel shader, `u` starts after the render targets (`u1` with one RT). StructuredBuffer maps to `t` and RWStructuredBuffer to `u`.
- **MSL** flattens to per-type `[[buffer]]`/`[[texture]]`/`[[sampler]]` indices; constant buffers and structured buffers share the buffer index space.
- `-reflection-json` (and the API reflection) reports the flattened index per parameter. The compiler stores a `(set, binding) → {d3d11 register, metal index}` table per module.
- **All entry points of a module must be compiled in one session** so the layout is shared across VS/FS. A render pipeline's VS and FS must therefore come from the same module (validated at pipeline creation).
- **DXBC** requires fxc, which is unavailable on macOS, so the HLSL-text fallback is mandatory there.
- WGSL rejects HLSL `Interlocked*` on plain `uint` buffers in fragment shaders. Tests use compute-only atomics.

## Risks

- **Fallback if Slang's flattening changes between versions**: explicit per-target binding attributes generated by macros in `SimpleRHI.slang`. Slang is pinned to 2026.1.
- **webgpu.h churn** between wgpu-native and emdawnwebgpu. Pin both versions and keep a thin compatibility shim in `srhi_webgpu.cpp`.
- **Metal 3 timestamps** are only available at stage boundaries on Apple Silicon. That's why the API exposes pass-boundary timestamps only.

## Status (2026-11-13)

| Phase | State |
|---|---|
| 0 Scaffolding + Slang spike | Done |
| 1 Compiler, `.srhs`, `srhic` (+ `--header`) | Done |
| 2 Metal 3 backend + test framework | Done: 46/46 tests |
| 3 WebGPU backend (wgpu-native) | Done: 46/46 tests against the shared Metal goldens, no overrides |
| 4 D3D11 backend | Written, **not yet built or run** (needs a Windows machine) |
| 5 Swapchain / HDR / fullscreen / timestamps / native access | Done on Metal + WebGPU (HDR10 unsupported on WebGPU) |
| 6 ImGui backend | Done (sRGB / scRGB / HDR10-aware) |
| 7 Asset baker (`srhi_bake`, ANBC) | Done: Sponza, DamagedHelmet, lake_pier HDRI |
| 8 Examples | HelloTriangle, Sprites2D, PBRHelmet (IBL), Bloom, ShadowMapping (CSM), Deferred: all verified on Metal + WebGPU |
| 9 Web build | Not started: needs emsdk installed |

Follow-ups:
- Run the D3D11 backend on Windows (`srhi_tests --backend d3d11`); the checklist is in the D3D11 agent report / commit notes.
- `Model` has no CPU culling (Sponza: 448 draws per pass, 5.7M triangles); expose submesh bounds and cull per view/cascade.
- Web build: install emsdk, add an `xmake f -p wasm` path (emdawnwebgpu, `-sASYNCIFY` for blocking waits).
