#pragma once
// SimpleRHI shader conventions.
//
//   #include "SimpleRHI.slang"     (srhic adds Source/SimpleRHI/Shaders to the include path)
//
// Resources are globals with an explicit (binding, set). Sets 0..2 hold resource sets, set 3 holds
// transient uniforms pushed with srhi*EncoderSetUniforms:
//
//   SRHI_BINDING(0, 0) Texture2D<float4> gAlbedo;
//   SRHI_BINDING(1, 0) SamplerState gSampler;
//   SRHI_UNIFORMS(0) ConstantBuffer<DrawConstants> gDraw;
//
// Layout rules (enforced by the compiler): uniform and structured buffer structs must have the same
// layout on every target. Use 4-byte scalars, float2, float4 and float4x4; pad float3 to float4.
// Matrices are column-major in memory (glm-compatible): mul(M, v) == M * v on the CPU.
//
// Storage textures are write-only and need an explicit texel format (WebGPU rules):
//   SRHI_BINDING(0, 0) [format("rgba8")] WTexture2D<float4> gOut;   ...   gOut.Store(coord, value);
//
// Depth textures sampled with a comparison sampler use DepthTexture2D (reflected as a DEPTH sample type):
//   SRHI_BINDING(1, 0) DepthTexture2D gShadowMap;  SRHI_BINDING(2, 0) SamplerComparisonState gShadowSampler;
//
// Atomics on storage buffers use Atomic<T>: RWStructuredBuffer<Atomic<uint>> gCounter; gCounter[0].add(1);
//
// Every shader is also compiled to WGSL during packaging, so WebGPU's restrictions are enforced for every
// target: a shader that compiles is portable.


#define SRHI_BINDING(b_, s_) [vk::binding(b_, s_)]
#define SRHI_UNIFORMS(slot_) [vk::binding(slot_, 3)]
