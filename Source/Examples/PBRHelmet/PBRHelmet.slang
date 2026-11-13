// Metal/rough PBR with image-based lighting, plus the skybox.
#include "SimpleRHI.slang"
#include "Common.slang"
#include "Material.slang"

SRHI_BINDING(0, 0) TextureCube<float4> gIrradiance;
SRHI_BINDING(1, 0) TextureCube<float4> gPrefiltered;
SRHI_BINDING(2, 0) Texture2D<float4> gBrdfLut;
SRHI_BINDING(3, 0) SamplerState gIblSampler;
SRHI_BINDING(4, 0) TextureCube<float4> gEnvironment;

struct FrameParams
{
    float4x4 viewProj;
    float4x4 invViewProj;
    float4x4 model;
    float4 cameraPos;   // xyz, w: prefiltered max mip
    float4 lighting;    // x exposure, y IBL intensity, z sun intensity, w debug view
    float4 sunDir;      // xyz towards the sun, w skybox LOD
    OutputParams output;
};
SRHI_UNIFORMS(0) ConstantBuffer<FrameParams> gFrame;

struct VSOut
{
    float4 position : SV_Position;
    float3 worldPos : TEXCOORD0;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD1;
    float4 tangent : TANGENT;
};

[shader("vertex")]
VSOut vsModel(ModelVertex input)
{
    VSOut o;
    float4 world = mul(gFrame.model, float4(input.position, 1.0));
    o.position = mul(gFrame.viewProj, world);
    o.worldPos = world.xyz;
    o.normal = mul(gFrame.model, float4(input.normal, 0.0)).xyz;
    o.tangent = float4(mul(gFrame.model, float4(input.tangent.xyz, 0.0)).xyz, input.tangent.w);
    o.uv = input.uv;
    return o;
}

float3 FresnelSchlick(float cosTheta, float3 f0)
{
    return f0 + (1.0 - f0) * pow(saturate(1.0 - cosTheta), 5.0);
}

float3 FresnelSchlickRoughness(float cosTheta, float3 f0, float roughness)
{
    return f0 + (max(1.0 - roughness, f0) - f0) * pow(saturate(1.0 - cosTheta), 5.0);
}

float D_GGX(float NdotH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

float G_SmithSchlick(float NdotV, float NdotL, float roughness)
{
    float r = roughness + 1.0;
    float k = r * r / 8.0;
    return (NdotV / (NdotV * (1.0 - k) + k)) * (NdotL / (NdotL * (1.0 - k) + k));
}

[shader("fragment")]
float4 fsModel(VSOut input) : SV_Target0
{
    SurfaceSample s;
    if (!SampleSurface(input.uv, input.normal, input.tangent, s))
        discard;

    float3 n = s.normal;
    float3 v = normalize(gFrame.cameraPos.xyz - input.worldPos);
    float NdotV = max(dot(n, v), 1e-4);
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), s.albedo, s.metallic);

    // Directional sun.
    float3 l = normalize(gFrame.sunDir.xyz);
    float3 h = normalize(v + l);
    float NdotL = saturate(dot(n, l));
    float3 F = FresnelSchlick(saturate(dot(h, v)), f0);
    float3 spec = D_GGX(saturate(dot(n, h)), s.roughness) * G_SmithSchlick(NdotV, NdotL, s.roughness) * F / (4.0 * NdotV * NdotL + 1e-4);
    float3 kd = (1.0 - F) * (1.0 - s.metallic);
    float3 direct = (kd * s.albedo / PI + spec) * NdotL * gFrame.lighting.z;

    // Image-based lighting.
    float3 Fr = FresnelSchlickRoughness(NdotV, f0, s.roughness);
    float3 kdIbl = (1.0 - Fr) * (1.0 - s.metallic);
    float3 diffuse = gIrradiance.SampleLevel(gIblSampler, n, 0.0).rgb * s.albedo;
    float3 r = reflect(-v, n);
    float3 prefiltered = gPrefiltered.SampleLevel(gIblSampler, r, s.roughness * gFrame.cameraPos.w).rgb;
    float2 brdf = gBrdfLut.SampleLevel(gIblSampler, float2(NdotV, 1.0 - s.roughness), 0.0).rg;
    float3 ambient = (kdIbl * diffuse + prefiltered * (Fr * brdf.x + brdf.y)) * s.occlusion * gFrame.lighting.y;

    float3 color = direct + ambient + s.emissive;

    int debugView = int(gFrame.lighting.w + 0.5);
    if (debugView == 1) return EncodeDisplay(s.albedo, gFrame.output);
    if (debugView == 2) return EncodeDisplay(n * 0.5 + 0.5, gFrame.output);
    if (debugView == 3) return EncodeDisplay(float3(s.metallic, s.metallic, s.metallic), gFrame.output);
    if (debugView == 4) return EncodeDisplay(float3(s.roughness, s.roughness, s.roughness), gFrame.output);
    if (debugView == 5) return EncodeDisplay(float3(s.occlusion, s.occlusion, s.occlusion), gFrame.output);
    if (debugView == 6) return EncodeDisplay(s.emissive, gFrame.output);
    if (debugView == 7) return EncodeOutput(ambient * gFrame.lighting.x, gFrame.output);
    return EncodeOutput(color * gFrame.lighting.x, gFrame.output);
}

// --- Skybox: fullscreen triangle at the far plane ---

struct SkyOut
{
    float4 position : SV_Position;
    float2 ndc : TEXCOORD0;
};

[shader("vertex")]
SkyOut vsSky(uint vertexId : SV_VertexID)
{
    SkyOut o;
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    o.ndc = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    o.position = float4(o.ndc, 1.0, 1.0);
    return o;
}

[shader("fragment")]
float4 fsSky(SkyOut input) : SV_Target0
{
    float4 world = mul(gFrame.invViewProj, float4(input.ndc, 1.0, 1.0));
    float3 dir = normalize(world.xyz / world.w - gFrame.cameraPos.xyz);
    float3 c = gEnvironment.SampleLevel(gIblSampler, dir, gFrame.sunDir.w).rgb;
    return EncodeOutput(c * gFrame.lighting.x, gFrame.output);
}
