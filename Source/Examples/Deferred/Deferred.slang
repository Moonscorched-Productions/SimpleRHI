// Deferred shading of Sponza with hundreds of point lights.
//   1. G-buffer (MRT): albedo+AO (RGBA8 sRGB), world normal (RGBA16F), metal/rough (RGBA8), depth (D32).
//   2. Compute: sun + ambient into an RGBA16F storage texture.
//   3. Lights: one screen-space quad per light (instanced), additive blending into the same texture.
//   4. Composite: tonemap / encode for the swapchain, or show a G-buffer channel.
#include "SimpleRHI.slang"
#include "Common.slang"
#include "Material.slang"
#include "DeferredPbr.slangh"

struct Frame
{
    float4x4 viewProj;
    float4x4 invViewProj;
    float4x4 view;
    float4 cameraPos;
    float4 sunDir;   // xyz toward the sun, w intensity
    float4 sunColor; // rgb, w ambient
    float4 screen;   // width, height, 1/width, 1/height
    float4 settings; // x debug view, y light intensity, z near plane, w far plane
    OutputParams output;
};
SRHI_UNIFORMS(0) ConstantBuffer<Frame> gFrame;

struct PointLight
{
    float4 positionRadius;
    float4 color;
};

SRHI_BINDING(0, 0) Texture2D<float4> gAlbedo;
SRHI_BINDING(1, 0) Texture2D<float4> gNormal;
SRHI_BINDING(2, 0) Texture2D<float4> gSurface;
SRHI_BINDING(3, 0) DepthTexture2D gDepth;
SRHI_BINDING(4, 0) [format("rgba16f")] WTexture2D<float4> gLitOut;
SRHI_BINDING(5, 0) StructuredBuffer<PointLight> gLights;
SRHI_BINDING(6, 0) Texture2D<float4> gLit;

// --- 1. G-buffer ---------------------------------------------------------------------------------------

struct GBufferVSOut
{
    float4 position : SV_Position;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 tangent : TANGENT;
};

[shader("vertex")]
GBufferVSOut vsGBuffer(ModelVertex v)
{
    GBufferVSOut o;
    o.position = mul(gFrame.viewProj, float4(v.position, 1.0));
    o.normal = v.normal;
    o.uv = v.uv;
    o.tangent = v.tangent;
    return o;
}

struct GBufferOut
{
    float4 albedo : SV_Target0;
    float4 normal : SV_Target1;
    float4 surface : SV_Target2;
};

[shader("fragment")]
GBufferOut fsGBuffer(GBufferVSOut input)
{
    SurfaceSample s;
    if (!SampleSurface(input.uv, input.normal, input.tangent, s))
        discard;
    GBufferOut o;
    o.albedo = float4(s.albedo, s.occlusion);
    o.normal = float4(s.normal, 1.0);
    o.surface = float4(s.metallic, s.roughness, 0.0, 1.0);
    return o;
}

// --- Shared ------------------------------------------------------------------------------------------

float3 WorldPosition(int2 pixel, float depth)
{
    float2 ndc = (float2(pixel) + 0.5) * gFrame.screen.zw * float2(2.0, -2.0) + float2(-1.0, 1.0);
    float4 w = mul(gFrame.invViewProj, float4(ndc, depth, 1.0));
    return w.xyz / w.w;
}

// --- 2. Sun + ambient (compute) -----------------------------------------------------------------------

[shader("compute")]
[numthreads(8, 8, 1)]
void csSun(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= uint(gFrame.screen.x) || id.y >= uint(gFrame.screen.y))
        return;
    int2 p = int2(id.xy);
    float depth = gDepth.Load(int3(p, 0));
    if (depth >= 1.0)
    {
        float t = float(id.y) * gFrame.screen.w;
        gLitOut.Store(id.xy, float4(lerp(float3(0.5, 0.65, 0.9), float3(0.85, 0.85, 0.9), t) * 1.5, 1.0));
        return;
    }
    float4 albedoAO = gAlbedo.Load(int3(p, 0));
    float3 N = normalize(gNormal.Load(int3(p, 0)).xyz);
    float2 mr = gSurface.Load(int3(p, 0)).xy;
    float3 world = WorldPosition(p, depth);
    float3 V = normalize(gFrame.cameraPos.xyz - world);
    float3 color = EvalBRDF(albedoAO.rgb, mr.x, mr.y, N, V, normalize(gFrame.sunDir.xyz)) * gFrame.sunColor.rgb * gFrame.sunDir.w;
    float3 sky = lerp(float3(0.35, 0.3, 0.25), float3(0.55, 0.65, 0.85), N.y * 0.5 + 0.5);
    color += albedoAO.rgb * albedoAO.a * sky * gFrame.sunColor.w;
    gLitOut.Store(id.xy, float4(color, 1.0));
}

// --- 3. Point lights (instanced screen-space quads) -----------------------------------------------------

struct LightVSOut
{
    float4 position : SV_Position;
    nointerpolation float4 positionRadius : TEXCOORD0;
    nointerpolation float4 color : TEXCOORD1;
};

[shader("vertex")]
LightVSOut vsLight(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    PointLight light = gLights[instanceId];
    float3 c = light.positionRadius.xyz;
    float r = light.positionRadius.w;

    // Screen bounds of the light's world AABB; fullscreen if it straddles the near plane, empty if behind.
    float2 mn = float2(1e9, 1e9), mx = float2(-1e9, -1e9);
    int behind = 0;
    for (int i = 0; i < 8; ++i)
    {
        float3 corner = c + r * float3((i & 1) ? 1.0 : -1.0, (i & 2) ? 1.0 : -1.0, (i & 4) ? 1.0 : -1.0);
        float4 clip = mul(gFrame.viewProj, float4(corner, 1.0));
        if (clip.w < gFrame.settings.z)
        {
            behind++;
            continue;
        }
        float2 ndc = clip.xy / clip.w;
        mn = min(mn, ndc);
        mx = max(mx, ndc);
    }
    if (behind == 8)
    {
        mn = float2(2.0, 2.0);
        mx = mn;
    }
    else if (behind > 0)
    {
        mn = float2(-1.0, -1.0);
        mx = float2(1.0, 1.0);
    }
    mn = clamp(mn, -1.0, 1.0);
    mx = clamp(mx, -1.0, 1.0);

    const float2 corners[6] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(0, 1), float2(1, 0), float2(1, 1) };
    LightVSOut o;
    o.position = float4(lerp(mn, mx, corners[vertexId]), 0.0, 1.0);
    o.positionRadius = light.positionRadius;
    o.color = light.color;
    return o;
}

[shader("fragment")]
float4 fsLight(LightVSOut input) : SV_Target0
{
    int2 p = int2(input.position.xy);
    float depth = gDepth.Load(int3(p, 0));
    if (depth >= 1.0)
        discard;
    float3 world = WorldPosition(p, depth);
    float3 toLight = input.positionRadius.xyz - world;
    float dist = length(toLight);
    float r = input.positionRadius.w;
    if (dist >= r)
        discard;
    float3 L = toLight / dist;
    float window = saturate(1.0 - pow(dist / r, 4.0));
    float attenuation = window * window / (dist * dist + 1.0);

    float4 albedoAO = gAlbedo.Load(int3(p, 0));
    float3 N = normalize(gNormal.Load(int3(p, 0)).xyz);
    float2 mr = gSurface.Load(int3(p, 0)).xy;
    float3 V = normalize(gFrame.cameraPos.xyz - world);
    float3 radiance = EvalBRDF(albedoAO.rgb, mr.x, mr.y, N, V, L) * input.color.rgb * (attenuation * gFrame.settings.y);
    return float4(radiance, 0.0);
}

// --- 4. Composite --------------------------------------------------------------------------------------

struct FullscreenOut
{
    float4 position : SV_Position;
};

[shader("vertex")]
FullscreenOut vsFullscreen(uint vertexId : SV_VertexID)
{
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    FullscreenOut o;
    o.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

[shader("fragment")]
float4 fsComposite(FullscreenOut input) : SV_Target0
{
    int3 p = int3(int2(input.position.xy), 0);
    int mode = int(gFrame.settings.x + 0.5);
    if (mode == 1)
        return EncodeDisplay(gAlbedo.Load(p).rgb, gFrame.output);
    if (mode == 2)
        return EncodeDisplay(pow(gNormal.Load(p).xyz * 0.5 + 0.5, 2.2), gFrame.output);
    if (mode == 3)
    {
        float2 mr = gSurface.Load(p).xy;
        return EncodeDisplay(float3(mr.x, mr.y, 0.0), gFrame.output);
    }
    if (mode == 4)
    {
        // Linear view depth, 0..far mapped to white..black.
        float d = gDepth.Load(p);
        float n = gFrame.settings.z, f = gFrame.settings.w;
        float linearDepth = n * f / (f - d * (f - n));
        float v = 1.0 - saturate(linearDepth / 40.0);
        return EncodeDisplay(float3(v * v), gFrame.output);
    }
    return EncodeOutput(gLit.Load(p).rgb, gFrame.output);
}
