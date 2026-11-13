// Model vertex input and material set (set 1) shared by the 3D examples. Matches Model.h.
#pragma once

struct ModelVertex
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 tangent : TANGENT;
};

struct MaterialConstants
{
    float4 baseColorFactor;
    float4 emissiveFactor; // xyz emissive, w alpha cutoff
    float4 params;         // x metallic, y roughness, z alpha mode (0 opaque, 1 mask, 2 blend), w unused
};

SRHI_BINDING(0, 1) ConstantBuffer<MaterialConstants> gMaterial;
SRHI_BINDING(1, 1) Texture2D<float4> gBaseColorMap;
SRHI_BINDING(2, 1) Texture2D<float4> gNormalMap;
SRHI_BINDING(3, 1) Texture2D<float4> gMetalRoughMap;
SRHI_BINDING(4, 1) Texture2D<float4> gEmissiveMap;
SRHI_BINDING(5, 1) Texture2D<float4> gOcclusionMap;
SRHI_BINDING(6, 1) SamplerState gMaterialSampler;

struct SurfaceSample
{
    float3 albedo;
    float alpha;
    float3 normal; // world space
    float metallic;
    float roughness;
    float occlusion;
    float3 emissive;
};

// Returns false (callers discard) for alpha-tested texels below the cutoff.
bool SampleSurface(float2 uv, float3 normal, float4 tangent, out SurfaceSample s)
{
    float4 base = gBaseColorMap.Sample(gMaterialSampler, uv) * gMaterial.baseColorFactor;
    s.albedo = base.rgb;
    s.alpha = base.a;

    // BC5 normal map: XY in RG, reconstruct Z.
    float2 xy = gNormalMap.Sample(gMaterialSampler, uv).rg * 2.0 - 1.0;
    float3 tn = float3(xy, sqrt(saturate(1.0 - dot(xy, xy))));
    float3 n = normalize(normal);
    float3 t = normalize(tangent.xyz - n * dot(n, tangent.xyz));
    float3 b = cross(n, t) * tangent.w;
    s.normal = normalize(t * tn.x + b * tn.y + n * tn.z);

    float4 mr = gMetalRoughMap.Sample(gMaterialSampler, uv);
    s.metallic = mr.b * gMaterial.params.x;
    s.roughness = clamp(mr.g * gMaterial.params.y, 0.04, 1.0);
    s.occlusion = gOcclusionMap.Sample(gMaterialSampler, uv).r;
    s.emissive = gEmissiveMap.Sample(gMaterialSampler, uv).rgb * gMaterial.emissiveFactor.xyz;
    return !(gMaterial.params.z > 0.5 && gMaterial.params.z < 1.5 && s.alpha < gMaterial.emissiveFactor.w);
}

// Alpha test only (shadow / depth passes).
bool AlphaTest(float2 uv)
{
    if (gMaterial.params.z < 0.5 || gMaterial.params.z > 1.5)
        return true;
    return gBaseColorMap.Sample(gMaterialSampler, uv).a * gMaterial.baseColorFactor.a >= gMaterial.emissiveFactor.w;
}
