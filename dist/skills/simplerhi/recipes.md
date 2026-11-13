# SimpleRHI recipes

Complete snippets for common tasks. They compile as C++ (and as C with `{0}` initializers). Error checks are
omitted for brevity: in real code check every created object for `NULL`.

## Contents

1. Device with logging
2. Texture with data, mip chain and sampler
3. Resource set from shader reflection
4. Per-draw constants (transient uniforms)
5. Offscreen pass with depth, then sample the result
6. MSAA with resolve
7. Rendering to a mip, array layer or cube face
8. Shadow map with a comparison sampler
9. Compute pass
10. Reading pixels and buffers back
11. GPU timings
12. HDR output
13. Dear ImGui
14. Shutdown order

## 1. Device with logging

```c
static void Log(srhiLogSeverity severity, const char* message, void* user)
{
    fprintf(stderr, "[SimpleRHI %s] %s\n",
            severity == SRHI_LOG_SEVERITY_ERROR ? "error" : severity == SRHI_LOG_SEVERITY_WARNING ? "warning" : "info", message);
}

srhiDeviceDesc desc = {};
desc.backend = SRHI_BACKEND_AUTO;   // D3D11 on Windows, Metal on Apple, WebGPU on the web
desc.debug = 1;                     // validation; turn off for release builds
desc.log = Log;
srhiDevice* device = srhiDeviceCreate(&desc);

srhiDeviceInfo info;
srhiDeviceGetInfo(device, &info);   // info.features.*, info.limits.*, info.backendName
```

## 2. Texture with data, mip chain and sampler

```c
srhiTextureDesc td = {};
td.type = SRHI_TEXTURE_TYPE_2D;
td.format = SRHI_FORMAT_RGBA8_UNORM_SRGB;        // color textures: sRGB. Normal maps / data: RGBA8_UNORM
td.width = width;
td.height = height;
td.mipLevels = SRHI_ALL_MIPS;                    // full chain
td.usage = SRHI_TEXTURE_USAGE_SAMPLED | SRHI_TEXTURE_USAGE_RENDER_TARGET; // RENDER_TARGET for GenerateMipmaps
td.label = "albedo";
srhiTexture* texture = srhiTextureCreate(device, &td);

srhiTextureWrite(device, texture, 0 /*mip*/, 0 /*layer*/, NULL /*whole mip*/, pixels, 0 /*tightly packed*/);

srhiCommandBuffer* cmd = srhiCommandBufferBegin(device, "mips");
srhiBlitEncoder* blit = srhiBlitEncoderBegin(cmd, "mips");
srhiBlitEncoderGenerateMipmaps(blit, texture);
srhiBlitEncoderEnd(blit);
srhiCommandBufferCommit(cmd, NULL, NULL);

srhiSamplerDesc sd = {};
sd.minFilter = sd.magFilter = sd.mipFilter = SRHI_FILTER_LINEAR;
sd.addressU = sd.addressV = sd.addressW = SRHI_ADDRESS_MODE_REPEAT;
sd.maxAnisotropy = 8;                            // requires all three filters to be LINEAR
srhiSampler* sampler = srhiSamplerCreate(device, &sd);
```

Pre-compressed textures (BC7 and friends): create with the BC format, no `RENDER_TARGET`, and call
`srhiTextureWrite` once per mip with the block data. Cube maps: `type = SRHI_TEXTURE_TYPE_CUBE`, write layers
0..5 (+X, -X, +Y, -Y, +Z, -Z).

## 3. Resource set from shader reflection

Shader (set 0):

```hlsl
SRHI_BINDING(0, 0) Texture2D<float4> gAlbedo;
SRHI_BINDING(1, 0) SamplerState gSampler;
SRHI_BINDING(2, 0) ConstantBuffer<Material> gMaterial;
```

```c
// Layout: ask the module what set 0 contains.
srhiResourceSetLayoutEntry entries[16];
uint32_t count = srhiShaderModuleGetResourceSetLayout(module, 0, entries, 16);
srhiResourceSetLayoutDesc ld = {entries, count, "material layout"};
srhiResourceSetLayout* layout = srhiResourceSetLayoutCreate(device, &ld);

// Set: one binding per layout entry, all of them.
srhiResourceBinding b[3] = {};
b[0].binding = 0; b[0].textureView = srhiTextureGetDefaultView(texture);
b[1].binding = 1; b[1].sampler = sampler;
b[2].binding = 2; b[2].buffer = materialBuffer;   // UNIFORM usage; offset multiple of 256, size 0 = rest
srhiResourceSetDesc sd = {layout, b, 3, "material"};
srhiResourceSet* set = srhiResourceSetCreate(device, &sd);

// The pipeline lists the layout at the same set index, and the encoder binds the set there.
pipelineDesc.setLayouts[0] = layout;
srhiRenderEncoderSetResourceSet(enc, 0, set);
```

Sets are immutable: to change a texture, create another set. A good split is set 0 = per-frame/per-pass
resources, set 1 = per-material, per-draw data in transient uniforms.

## 4. Per-draw constants (transient uniforms)

```hlsl
struct DrawConstants { float4x4 mvp; float4 tint; };
SRHI_UNIFORMS(0) ConstantBuffer<DrawConstants> gDraw;   // set 3, slot 0
```

```c
struct DrawConstants { float mvp[16]; float tint[4]; } draw = ...;   // same layout as the shader struct
srhiRenderEncoderSetUniforms(enc, 0, &draw, sizeof(draw));           // <= 4096 bytes, multiple of 4
srhiRenderEncoderDrawIndexed(enc, indexCount, 1, 0, 0, 0);
```

Call it before each draw that needs new values. Nothing has to be created or bound for set 3.

## 5. Offscreen pass with depth, then sample the result

```c
srhiTextureDesc cd = {};
cd.format = SRHI_FORMAT_RGBA16_FLOAT;
cd.width = w; cd.height = h;
cd.usage = SRHI_TEXTURE_USAGE_RENDER_TARGET | SRHI_TEXTURE_USAGE_SAMPLED;
srhiTexture* color = srhiTextureCreate(device, &cd);

srhiTextureDesc dd = cd;
dd.format = SRHI_FORMAT_D32_FLOAT;
dd.usage = SRHI_TEXTURE_USAGE_RENDER_TARGET;
srhiTexture* depth = srhiTextureCreate(device, &dd);

// Pipeline for this pass
srhiRenderPipelineDesc pd = {};
pd.module = module; pd.vertexEntry = "vsMain"; pd.fragmentEntry = "fsMain";
/* vertex layout ... */
pd.raster.cullMode = SRHI_CULL_MODE_BACK;
pd.depthStencil.format = SRHI_FORMAT_D32_FLOAT;
pd.depthStencil.depthTestEnable = 1;
pd.depthStencil.depthWriteEnable = 1;
pd.depthStencil.depthCompare = SRHI_COMPARE_FUNCTION_LESS;
pd.colorTargetCount = 1;
pd.colorTargets[0].format = SRHI_FORMAT_RGBA16_FLOAT;

// The pass
srhiRenderPassDesc pass = {};
pass.colorAttachmentCount = 1;
pass.colorAttachments[0].view = srhiTextureGetDefaultView(color);
pass.colorAttachments[0].loadAction = SRHI_LOAD_ACTION_CLEAR;
pass.colorAttachments[0].storeAction = SRHI_STORE_ACTION_STORE;
pass.depthAttachment.view = srhiTextureGetDefaultView(depth);
pass.depthAttachment.depthLoadAction = SRHI_LOAD_ACTION_CLEAR;
pass.depthAttachment.depthStoreAction = SRHI_STORE_ACTION_DONT_CARE;
pass.depthAttachment.clearDepth = 1.0f;
```

A later pass samples `color` through a resource set holding `srhiTextureGetDefaultView(color)`. No barrier or
transition is needed. Recreate the textures (and the sets that reference them) when the window size changes.

Alpha blending for a color target:

```c
pd.colorTargets[0].blend.enable = 1;
pd.colorTargets[0].blend.srcColor = SRHI_BLEND_FACTOR_SRC_ALPHA;
pd.colorTargets[0].blend.dstColor = SRHI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
pd.colorTargets[0].blend.srcAlpha = SRHI_BLEND_FACTOR_ONE;
pd.colorTargets[0].blend.dstAlpha = SRHI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
```

## 6. MSAA with resolve

```c
srhiTextureDesc md = {};
md.format = format; md.width = w; md.height = h;
md.sampleCount = 4;
md.usage = SRHI_TEXTURE_USAGE_RENDER_TARGET;
srhiTexture* msaa = srhiTextureCreate(device, &md);

pipelineDesc.sampleCount = 4;

pass.colorAttachments[0].view = srhiTextureGetDefaultView(msaa);
pass.colorAttachments[0].loadAction = SRHI_LOAD_ACTION_CLEAR;
pass.colorAttachments[0].storeAction = SRHI_STORE_ACTION_RESOLVE;                 // resolve, discard samples
pass.colorAttachments[0].resolveView = srhiTextureGetDefaultView(resolvedTexture); // single-sampled, same format
```

A depth attachment in the same pass must also have 4 samples. Depth cannot be resolved.

## 7. Rendering to a mip, array layer or cube face

```c
srhiTextureViewDesc vd = {};
vd.type = SRHI_TEXTURE_TYPE_2D;   // a 2D view even when the texture is an array or a cube
vd.baseMip = mip;   vd.mipCount = 1;
vd.baseLayer = layer; vd.layerCount = 1;   // cube faces are layers 0..5
srhiTextureView* target = srhiTextureViewCreate(texture, &vd);
pass.colorAttachments[0].view = target;
// ... destroy the view with srhiTextureViewDestroy when no longer needed
```

`vd.format` can switch between the sRGB and linear twin of the texture's format (for example to render linear
values into an `_SRGB` texture without encoding).

## 8. Shadow map with a comparison sampler

```hlsl
SRHI_BINDING(0, 0) DepthTexture2D gShadowMap;
SRHI_BINDING(1, 0) SamplerComparisonState gShadowSampler;
float lit = gShadowMap.SampleCmpLevelZero(gShadowSampler, uv, depth);
```

```c
td.format = SRHI_FORMAT_D32_FLOAT;
td.usage = SRHI_TEXTURE_USAGE_RENDER_TARGET | SRHI_TEXTURE_USAGE_SAMPLED;

srhiSamplerDesc sd = {};
sd.minFilter = sd.magFilter = SRHI_FILTER_LINEAR;
sd.addressU = sd.addressV = sd.addressW = SRHI_ADDRESS_MODE_CLAMP_TO_EDGE;
sd.compareEnable = 1;
sd.compareFunction = SRHI_COMPARE_FUNCTION_LESS_EQUAL;
```

The shadow pass uses a depth-only pipeline: `fragmentEntry = NULL`, `colorTargetCount = 0`, and
`raster.depthBias` / `depthBiasSlopeScale` to fight acne.

## 9. Compute pass

```hlsl
SRHI_BINDING(0, 0) StructuredBuffer<Particle> gIn;
SRHI_BINDING(1, 0) RWStructuredBuffer<Particle> gOut;
[shader("compute")] [numthreads(64, 1, 1)]
void csMain(uint3 id : SV_DispatchThreadID) { ... }
```

```c
srhiBufferDesc bd = {};
bd.size = count * sizeof(Particle);
bd.usage = SRHI_BUFFER_USAGE_STORAGE_RW | SRHI_BUFFER_USAGE_COPY_SRC;   // STORAGE for the read-only one
bd.structureStride = sizeof(Particle);
srhiBuffer* out = srhiBufferCreate(device, &bd);

srhiComputePipelineDesc cp = {};
cp.module = module; cp.entry = "csMain"; cp.setLayouts[0] = layout;
srhiComputePipeline* pipeline = srhiComputePipelineCreate(device, &cp);

srhiComputeEncoder* ce = srhiComputeEncoderBegin(cmd, NULL);
srhiComputeEncoderSetPipeline(ce, pipeline);
srhiComputeEncoderSetResourceSet(ce, 0, set);
srhiComputeEncoderSetUniforms(ce, 0, &params, sizeof(params));
srhiComputeEncoderDispatch(ce, (count + 63) / 64, 1, 1);   // threadgroups, not threads
srhiComputeEncoderEnd(ce);
```

Writing a texture from compute: usage `STORAGE`, shader type `WTexture2D<float4>` with `[format("rgba8")]`, bound
through a view of exactly one mip.

## 10. Reading pixels and buffers back

```c
static void OnPixels(const void* data, uint64_t size, void* user)
{
    // data is only valid during the callback: copy it out. Rows are tightly packed (width * bytes per texel).
    memcpy(user, data, size);
}

srhiCommandBuffer* cmd = srhiCommandBufferBegin(device, "readback");
/* ... passes that produce the texture ... */
srhiCommandBufferReadTexture(cmd, texture, 0 /*mip*/, 0 /*layer*/, OnPixels, destination);  // outside any encoder
srhiCommandBufferReadBuffer(cmd, buffer, 0, SRHI_WHOLE_SIZE, OnBytes, user);
srhiCommandBufferCommit(cmd, NULL, NULL);

srhiDeviceWaitIdle(device);   // blocking: callbacks have fired when this returns
// or keep calling srhiDevicePoll(device) each frame and react when the callback arrives
```

MSAA and depth-stencil textures cannot be read (resolve MSAA first).

## 11. GPU timings

```c
if (info.features.timestampQueries) {
    srhiQuerySet* queries = srhiQuerySetCreate(device, 2, "timings");

    pass.timestampWrites.querySet = queries;
    pass.timestampWrites.beginIndex = 0;
    pass.timestampWrites.endIndex = 1;      // SRHI_QUERY_UNUSED to skip one of them
    /* record and commit the frame */

    uint64_t ns[2];
    if (srhiQuerySetGetResults(queries, 0, 2, ns))     // false until that frame completed and was polled
        printf("pass: %.3f ms\n", (ns[1] - ns[0]) / 1e6);
}
```

A pass without timestamps must leave `timestampWrites.querySet` NULL. Use one query set per frame in flight so a
set is not rewritten before its results were read.

## 12. HDR output

```c
srhiDisplayInfo display;
srhiSwapchainGetDisplayInfo(swapchain, &display);
if (display.hdrSupported && srhiSwapchainConfigure(swapchain, SRHI_COLOR_SPACE_EXTENDED_LINEAR_SRGB, vsync)) {
    // The format is now RGBA16_FLOAT: recreate pipelines that render to the swapchain with
    // srhiSwapchainGetFormat(swapchain). Write linear values where 1.0 = SDR white (80 nits on Windows),
    // so multiply scene color by paperWhiteNits / 80.
}
```

`SRHI_COLOR_SPACE_HDR10_PQ` gives RGB10A2 with Rec.2020 primaries and PQ-encoded values written by the shader
(not available on WebGPU). Call `srhiDeviceWaitIdle` before `srhiSwapchainConfigure`.

## 13. Dear ImGui

Add `imgui/imgui_impl_srhi.cpp` to the build next to a platform backend such as `imgui_impl_sdl3`.

```cpp
ImGui_ImplSRHI_InitInfo init;
init.Device = device;
init.ColorFormat = srhiSwapchainGetFormat(swapchain);
init.ColorSpace = SRHI_COLOR_SPACE_SRGB;
ImGui_ImplSRHI_Init(&init);

// Each frame
ImGui_ImplSRHI_NewFrame();
ImGui_ImplSDL3_NewFrame();
ImGui::NewFrame();
/* ... UI ... */
ImGui::Render();

srhiRenderPassDesc ui = {};
ui.colorAttachmentCount = 1;
ui.colorAttachments[0].view = srhiTextureGetDefaultView(backbuffer);
ui.colorAttachments[0].loadAction = SRHI_LOAD_ACTION_LOAD;      // draw on top of the scene
ui.colorAttachments[0].storeAction = SRHI_STORE_ACTION_STORE;
srhiRenderEncoder* enc = srhiRenderEncoderBegin(cmd, &ui);
ImGui_ImplSRHI_RenderDrawData(ImGui::GetDrawData(), enc);
srhiRenderEncoderEnd(enc);

// Shutdown
ImGui_ImplSRHI_Shutdown();
```

Show a texture with `ImGui::Image(ImGui_ImplSRHI_TextureID(view), size)`; call `ImGui_ImplSRHI_ForgetTexture(view)`
before destroying that view. After an HDR toggle call `ImGui_ImplSRHI_SetOutput`.

## 14. Shutdown order

```c
srhiDeviceWaitIdle(device);
// resource sets, pipelines, layouts, shader modules, views, textures, buffers, samplers, query sets ...
srhiSwapchainDestroy(swapchain);
srhiDeviceDestroy(device);          // logs how many objects were leaked, if any
```

Default views (`srhiTextureGetDefaultView`) and swapchain textures are owned by their texture / swapchain: do
not destroy them.
