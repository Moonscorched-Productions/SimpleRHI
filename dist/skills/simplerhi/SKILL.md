---
name: simplerhi
description: Write and debug rendering code against SimpleRHI (the srhi* C API in SimpleRHI.h) - devices, buffers, textures, resource sets, render and compute pipelines, command buffers and encoders, swapchains, readback, timestamps and the ImGui backend. Use whenever code includes SimpleRHI.h or calls srhi* functions, when adding a render pass, pipeline or GPU resource to a SimpleRHI project, or when a "[SimpleRHI error]" message needs explaining.
---

# SimpleRHI

SimpleRHI is a small rendering hardware interface: a D3D11-feature-level renderer with a Metal-shaped API. One C
header (`SimpleRHI.h`) runs on D3D11 (Windows), Metal 3 (Apple) and WebGPU (browser, or desktop via wgpu-native).
Shaders are written once in Slang and compiled offline into `.srhs` packages.

Related skills: `simplerhi-shaders` (writing shaders, running `srhic`) and `simplerhi-integration` (adding the
library to a build). Read `recipes.md` next to this file for complete code for common tasks.

The header is the reference: every struct field and function has a doc comment. Read the relevant section of
`SimpleRHI.h` before using a function you have not used in this session; do not guess field names.

## The model

- **One device, one queue.** Commands are recorded into a command buffer through render, compute and blit
  encoders, then committed. Only one encoder may be open at a time. Command buffers execute in commit order.
- **Single-threaded.** Record and commit from one thread.
- **No barriers or resource states.** The backend tracks hazards between passes.
- **Descriptors are plain structs; zero is the default.** Always start with `= {}` and fill what you need. Zero
  means: triangle list, no culling, counter-clockwise front faces, no depth test, no blending, 1 mip, 1 sample.
- **Resource sets** are immutable groups of bindings (WebGPU bind groups) bound at set index 0, 1 or 2.
- **Set 3 is transient uniforms**: small per-draw constants pushed with `srhi*EncoderSetUniforms(enc, slot, data,
  size)`. There is no uniform buffer to manage for per-draw data.
- **Uploads are queue-ordered.** `srhiBufferWrite` / `srhiTextureWrite` are visible to every command buffer
  committed after the call, including one already being recorded.
- **Readback is asynchronous.** Callbacks fire from `srhiDevicePoll` or `srhiDeviceWaitIdle`, never inline.
- **Errors do not throw or abort.** A failed create returns `NULL`, a bad command is dropped, and the reason goes
  to the log callback. Always install `srhiDeviceDesc::log` and check created objects for `NULL`.
- **Lifetime.** Objects referenced by a command buffer must stay alive until it is committed. After the commit,
  destroying them is safe even if the GPU is still using them. A resource set does not keep its resources alive.
  Destroy every object before `srhiDeviceDestroy` (it reports leaks).

## A frame

```c
srhiDevicePoll(device);                               // fire completion / readback callbacks
srhiTexture* backbuffer = srhiSwapchainAcquire(swapchain);
if (!backbuffer) { /* minimized: sleep and try again next frame */ }

srhiCommandBuffer* cmd = srhiCommandBufferBegin(device, "frame");

srhiRenderPassDesc pass = {0};
pass.colorAttachmentCount = 1;
pass.colorAttachments[0].view = srhiTextureGetDefaultView(backbuffer);
pass.colorAttachments[0].loadAction = SRHI_LOAD_ACTION_CLEAR;
pass.colorAttachments[0].storeAction = SRHI_STORE_ACTION_STORE;
pass.colorAttachments[0].clearColor[3] = 1.0f;

srhiRenderEncoder* enc = srhiRenderEncoderBegin(cmd, &pass);
srhiRenderEncoderSetPipeline(enc, pipeline);
srhiRenderEncoderSetVertexBuffer(enc, 0, vertexBuffer, 0);
srhiRenderEncoderSetIndexBuffer(enc, indexBuffer, SRHI_INDEX_TYPE_UINT32, 0);
srhiRenderEncoderSetResourceSet(enc, 0, materialSet);
srhiRenderEncoderSetUniforms(enc, 0, &drawConstants, sizeof(drawConstants));
srhiRenderEncoderDrawIndexed(enc, indexCount, 1, 0, 0, 0);
srhiRenderEncoderEnd(enc);

srhiCommandBufferPresent(cmd, swapchain);             // last, outside any encoder
srhiCommandBufferCommit(cmd, onFrameDone, userData);  // callback optional
```

Pace frames with the completion callback: count frames in flight, and before starting a new frame poll until
fewer than two are queued. On resize call `srhiDeviceWaitIdle` then `srhiSwapchainResize`.

## Rules that are easy to get wrong

Buffers
- `UNIFORM` cannot be combined with any other GPU usage (`COPY_SRC` / `COPY_DST` are fine).
- `STORAGE` / `STORAGE_RW` cannot be combined with `VERTEX`, `INDEX` or `INDIRECT`. To draw with compute output,
  copy it with `srhiBlitEncoderCopyBufferToBuffer`.
- Storage buffers need `structureStride` (the size of `T` in `StructuredBuffer<T>`, a multiple of 4 that divides
  the size).
- `srhiBufferWrite` offset and size must be multiples of 4. Vertex strides and attribute offsets too.
- Uniform buffer binding offsets must be multiples of `limits.uniformBufferOffsetAlignment` (256 is portable).

Textures
- Usage must list everything the texture is used for: `SAMPLED`, `STORAGE`, `RENDER_TARGET`, `COPY_SRC`,
  `COPY_DST`. A texture that is rendered to and then sampled needs both flags.
- Depth formats are `D16_UNORM`, `D32_FLOAT`, `D32_FLOAT_S8_UINT`. There is no D24S8.
- MSAA is 1 or 4 samples, 2D, one mip, and needs `RENDER_TARGET`.
- Compressed formats (BC, ASTC) need sizes that are multiples of the block size and can only be sampled and
  copied. Check `srhiDeviceFeatures` before using BC or ASTC (WebGPU exposes them only where supported).
- `srhiTextureWrite` writes one mip of one layer. It cannot write MSAA or depth textures.
- Render passes attach **views**: exactly one mip and one layer. To render to a mip, a cube face or an array
  layer, create a 2D view of it with `srhiTextureViewCreate`.
- Storage texture bindings must be a view of exactly one mip, and storage textures are write-only in shaders.

Pipelines and passes
- A render pipeline's vertex and fragment entry points must come from the same shader module.
- Depth-stencil state lives in the pipeline (`depthStencil.format` must match the pass's depth attachment).
- `colorTargets[i].format`, `depthStencil.format` and `sampleCount` must match the pass the pipeline is used in.
  For the swapchain use `srhiSwapchainGetFormat`, never a hard-coded format.
- `vertexLayout.attributes[i].location` is the index of the field in the shader's vertex input struct.
- `writeMask` 0 means "all channels". Use `writeDisabled` to mask everything.
- Strip topologies drawn with an index buffer need `stripIndexType`.
- Wireframe, depth clamp and cube arrays are optional features: check `srhiDeviceFeatures`.

Binding
- Every entry of a resource set layout must be bound when the set is created.
- Build layouts from shader reflection with `srhiShaderModuleGetResourceSetLayout` instead of by hand. Patch
  `sampleType` for 32-bit float textures (`UNFILTERABLE_FLOAT`) and for depth textures declared as plain
  `Texture2D` (`DEPTH`); see `simplerhi-shaders`.
- `SetUniforms` data is at most `SRHI_MAX_UNIFORM_SIZE` (4096) bytes, a multiple of 4, in slot 0..3. It is copied
  immediately and stays bound until replaced or the encoder ends, so set it again in every encoder.
- Pipeline, vertex buffers, resource sets and uniforms do not carry over from one encoder to the next.

Commands
- `srhiCommandBufferReadBuffer`, `ReadTexture`, `EncodeNative` and `Present` are recorded outside any encoder.
- The acquired swapchain texture is only valid until the command buffer that presents it is committed. Acquire
  it every frame and never store it or put it in a long-lived resource set.
- Buffer <-> texture blits need `bytesPerRow` to be a multiple of 256. On D3D11 they are emulated with a CPU
  round trip that stalls: prefer `srhiTextureWrite` and `srhiCommandBufferReadTexture`.
- `srhiBlitEncoderGenerateMipmaps` needs `RENDER_TARGET` usage and a filterable, renderable format (no 3D).
- Timestamps are written at pass boundaries only (`timestampWrites` on the pass descriptor), and need
  `features.timestampQueries`.

Portability
- Coordinates are the same on every backend: clip-space Y up, depth 0..1, texture and framebuffer origin top-left.
- Matrices are column-major in memory (glm-compatible); `mul(M, v)` in the shader matches `M * v` on the CPU.
- `SV_InstanceID` does not include `firstInstance` on D3D11 and Metal: do not rely on it.
- WebGPU has no wireframe and no HDR10 swapchain. On the web the blocking calls (`srhiDeviceCreate`,
  `srhiDeviceWaitIdle`) need Asyncify, and the frame loop must yield to the browser every frame.
- The sRGB swapchain format has an `_SRGB` suffix: write linear color, the hardware encodes it.

## Debugging

- Set `srhiDeviceDesc::debug = 1` during development: it enables the D3D11 debug layer / Metal and WebGPU
  validation and SimpleRHI's own argument checks. Messages name the function and the object's `label`, so give
  every object a label.
- A `NULL` from a create function always comes with a logged reason. Fix the first error first: later errors are
  usually consequences.
- "pipeline ... attachment formats / sample count do not match the pass": the pipeline's color/depth formats or
  sample count differ from the views in the render pass.
- "not a valid shader package": the `.srhs` was built by a different version of `srhic`. Recompile shaders.
- Nothing drawn and no error: check the viewport/scissor, cull mode and winding, that the depth attachment is
  cleared to 1.0 with `LESS`, and that `SetUniforms` was called in this encoder.

## Native escape hatch

`srhiDeviceGetNative`, `srhiDeviceGetNativeQueue`, `srhiBufferGetNative`, `srhiTextureGetNative` return borrowed
native objects (`ID3D11Device*`, `id<MTLDevice>`, `WGPUDevice`, ...). `srhiCommandBufferEncodeNative` runs a
callback with the native command object for libraries such as DLSS, FSR or MetalFX. State changed there is not
tracked, so restore it.
