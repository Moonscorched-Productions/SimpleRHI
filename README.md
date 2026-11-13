# Simple RHI - a basic RHI for indie game development

Simple RHI is a small rendering hardware interface for fast prototyping of game projects. It targets
D3D11-class hardware with a Metal-shaped API, and runs on **D3D11**, **Metal 3** and **WebGPU**.
Shaders are written once in Slang (HLSL syntax) and compiled offline for every backend.

It is the little sibling of AGFX: no bindless, ray tracing, mesh shaders or explicit barriers, just the
features a 2D or small 3D game needs, with as little ceremony as possible.

## At a glance

```c
#include <SimpleRHI.h>

srhiDevice* device = srhiDeviceCreate(&(srhiDeviceDesc){ .backend = SRHI_BACKEND_AUTO });

srhiCommandBuffer* cmd = srhiCommandBufferBegin(device, "frame");
srhiRenderEncoder* enc = srhiRenderEncoderBegin(cmd, &pass);   // load/store actions, MRT, MSAA resolve
srhiRenderEncoderSetPipeline(enc, pipeline);                    // baked PSO incl. depth-stencil state
srhiRenderEncoderSetResourceSet(enc, 0, materialSet);           // immutable bind groups (sets 0..2)
srhiRenderEncoderSetUniforms(enc, 0, &constants, sizeof(constants)); // transient uniforms (set 3)
srhiRenderEncoderDrawIndexed(enc, indexCount, 1, 0, 0, 0);
srhiRenderEncoderEnd(enc);
srhiCommandBufferPresent(cmd, swapchain);
srhiCommandBufferCommit(cmd, NULL, NULL);
```

- **Single header**: [Include/SimpleRHI.h](Include/SimpleRHI.h) declares the runtime and the shader compiler.
- **No barriers**: all three backends track hazards.
- **Queue-ordered uploads**: `srhiBufferWrite` / `srhiTextureWrite` are visible to every later commit.
- **Async readback** with callbacks fired from `srhiDevicePoll` / `srhiDeviceWaitIdle`.
- **Timestamps** at pass boundaries, **swapchain** with vsync, fullscreen and HDR (scRGB / HDR10).
- **Native escape hatch** (`srhi*GetNative`, `srhiCommandBufferEncodeNative`) for DLSS / FSR / MetalFX.
- **ImGui backend**: [Source/ImGui](Source/ImGui), shader embedded, HDR-aware.

## Shaders

```hlsl
#include "SimpleRHI.slang"

SRHI_BINDING(0, 0) Texture2D<float4> gAlbedo;     // [vk::binding(binding, set)]
SRHI_BINDING(1, 0) SamplerState gSampler;
SRHI_UNIFORMS(0) ConstantBuffer<DrawConstants> gDraw;   // set 3: transient uniforms
```

`srhic` (or `srhiCompileShader`) produces a `.srhs` package holding DXBC (or HLSL when built without fxc),
a metallib (or MSL) and WGSL, plus reflection. Every shader is compiled to WGSL during packaging, so a
shader that compiles is portable. See [SimpleRHI.slang](Source/SimpleRHI/Shaders/SimpleRHI.slang) for the
conventions (write-only storage textures, `DepthTexture2D` for comparison sampling, `Atomic<T>`).

## Building

Requires [xmake](https://xmake.io) and the [Slang](https://github.com/shader-slang/slang) SDK
(`--slang_dir=...`, defaults to `/usr/local` on macOS). Dependencies (SDL3, ImGui, cgltf, stb, glm,
wgpu-native) are fetched by xmake.

```sh
xmake f -m release --webgpu=y         # Windows: add --slang_dir=C:/path/to/slang (slang.dll on PATH)
xmake                        # runtime, compiler, tools, tests and examples
xmake run srhi_bake          # compress Content/ into Content/Cache (ANBC: BC7/BC5/BC6H)
xmake run srhi_tests         # --backend metal|d3d11|webgpu, --update-goldens, --filter <name>
xmake run Sprites2D          # --backend ..., --hdr, --no-vsync
```

### Browser build

Build natively first (the browser pages load the shader packages and baked assets from `Content/Cache`),
then configure for wasm with an [emsdk](https://emscripten.org) install:

```sh
xmake f -p wasm -m release --sdk=/path/to/emsdk
xmake
python3 -m http.server       # from the repository root, then open http://localhost:8000/build/web/
```

On Windows with a recent emsdk, also pass the tools explicitly (xmake looks for `emcc.bat`):
`--cc=.../emcc.exe --cxx=.../em++.exe --ld=.../em++.exe --sh=.../em++.exe --ar=.../emar.exe`
(all under `emsdk/upstream/emscripten`).

### Drop-in distribution

[dist/](dist/) is everything needed to use SimpleRHI in another project: a single-header `SimpleRHI.h`
(API, runtime and shader compiler), a one-file `srhic.cpp`, the shader include, the ImGui backend, a minimal
example with CMake and xmake build files, and Claude skills. See [dist/README.txt](dist/README.txt).
Regenerate the generated files after changing the sources with `python Tools/MakeDist/make_dist.py`.

Test results go to `Tests/Output/results.json`. To browse them with image diffs (NVIDIA FLIP), serve the
repository root (`python3 -m http.server`) and open `/Tools/TestReport/`.

## Layout

| Path | Contents |
|---|---|
| `Include/SimpleRHI.h` | Public API |
| `Source/SimpleRHI` | Common layer, backends (`srhi_metal.mm`, `srhi_d3d11.cpp`, `srhi_webgpu.cpp`), compiler |
| `Source/ImGui` | Dear ImGui renderer backend |
| `Source/Tools` | `srhic` shader compiler, `srhi_bake` asset baker |
| `Source/Examples` | HelloTriangle, Sprites2D, PBRHelmet, Bloom, ShadowMapping, Deferred |
| `Tests` | Golden-image / golden-buffer test suite |
| `Tools/TestReport` | Web viewer for test results |
| `Tools/MakeDist` | Generates the single-header distribution in `dist/` |
| `dist` | Drop-in distribution: single header, `srhic.cpp`, example, Claude skills |

## Portability notes

- D3D11 cannot combine `UNIFORM` buffers with other usages, nor `STORAGE` buffers with vertex / index /
  indirect usage. Buffer <-> texture blits are emulated on D3D11 with a CPU round trip.
- `SV_InstanceID` does not include `firstInstance` on D3D11 and Metal.
- Pass timestamps on Metal span start-of-fragment to end-of-fragment (vertex work of tile-based GPUs overlaps
  earlier passes). wgpu-native on Metal samples differently: its per-pass times overlap and can read ~0 ms.
- WebGPU has no wireframe, no HDR10 swapchain, and only BC/ASTC where the adapter supports them.
