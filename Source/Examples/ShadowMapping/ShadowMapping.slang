// Cascaded shadow maps + forward PBR shading of Sponza.
#include "SimpleRHI.slang"
#include "Common.slang"
#include "Material.slang"
#include "ShadowPbr.slangh"

struct Frame
{
    float4x4 viewProj;
    float4x4 view;
    float4x4 cascadeViewProj[4];
    float4 cameraPos;
    float4 sunDir;   // xyz toward the sun, w intensity
    float4 sunColor; // rgb, w ambient intensity
    float4 splits;   // view-space far distance of each cascade
    float4 settings; // x show cascades, y normal offset (world units), z PCF radius (texels), w 1 / shadow map size
    OutputParams output;
};
SRHI_UNIFORMS(0) ConstantBuffer<Frame> gFrame;

struct ShadowParams
{
    float4x4 lightViewProj;
};
SRHI_UNIFORMS(1) ConstantBuffer<ShadowParams> gShadow;

SRHI_BINDING(0, 0) DepthTexture2DArray gShadowMap;
SRHI_BINDING(1, 0) SamplerComparisonState gShadowSampler;

// --- Shadow pass ---------------------------------------------------------------------------------------

struct ShadowOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
ShadowOut vsShadow(ModelVertex v)
{
    ShadowOut o;
    o.position = mul(gShadow.lightViewProj, float4(v.position, 1.0));
    o.uv = v.uv;
    return o;
}

[shader("fragment")]
void fsShadowMasked(ShadowOut input)
{
    if (!AlphaTest(input.uv))
        discard;
}

// --- Forward pass --------------------------------------------------------------------------------------

struct VSOut
{
    float4 position : SV_Position;
    float3 worldPos : TEXCOORD0;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD1;
    float4 tangent : TANGENT;
};

[shader("vertex")]
VSOut vsMain(ModelVertex v)
{
    VSOut o;
    o.position = mul(gFrame.viewProj, float4(v.position, 1.0));
    o.worldPos = v.position;
    o.normal = v.normal;
    o.uv = v.uv;
    o.tangent = v.tangent;
    return o;
}

float ShadowFactor(float3 worldPos, float3 N, out int cascade)
{
    float viewDepth = -mul(gFrame.view, float4(worldPos, 1.0)).z;
    cascade = 3;
    for (int i = 0; i < 4; ++i)
    {
        if (viewDepth < gFrame.splits[i])
        {
            cascade = i;
            break;
        }
    }
    if (viewDepth > gFrame.splits[3])
        return 1.0;

    // Normal offset grows with the cascade's texel size.
    float3 p = worldPos + N * gFrame.settings.y * float(1 << cascade);
    float4 sc = mul(gFrame.cascadeViewProj[cascade], float4(p, 1.0));
    float3 ndc = sc.xyz / sc.w;
    float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    float texel = gFrame.settings.w;
    int r = int(gFrame.settings.z);
    float sum = 0.0;
    for (int y = -r; y <= r; ++y)
        for (int x = -r; x <= r; ++x)
            sum += gShadowMap.SampleCmpLevelZero(gShadowSampler, float3(uv + float2(x, y) * texel, float(cascade)), ndc.z);
    float taps = float((2 * r + 1) * (2 * r + 1));
    return sum / taps;
}

[shader("fragment")]
float4 fsMain(VSOut input) : SV_Target0
{
    SurfaceSample s;
    if (!SampleSurface(input.uv, input.normal, input.tangent, s))
        discard;

    float3 N = s.normal;
    float3 V = normalize(gFrame.cameraPos.xyz - input.worldPos);
    float3 L = normalize(gFrame.sunDir.xyz);
    int cascade;
    float shadow = ShadowFactor(input.worldPos, normalize(input.normal), cascade);

    float3 color = EvalBRDF(s.albedo, s.metallic, s.roughness, N, V, L) * gFrame.sunColor.rgb * gFrame.sunDir.w * shadow;
    float3 sky = lerp(float3(0.35, 0.3, 0.25), float3(0.55, 0.65, 0.85), N.y * 0.5 + 0.5);
    color += s.albedo * s.occlusion * sky * gFrame.sunColor.w;
    color += s.emissive;

    if (gFrame.settings.x > 0.5)
    {
        const float3 tints[4] = { float3(1.0, 0.3, 0.3), float3(0.3, 1.0, 0.3), float3(0.3, 0.4, 1.0), float3(1.0, 1.0, 0.3) };
        color = lerp(color, color * tints[cascade] * 2.0, 0.6);
    }
    return EncodeOutput(color, gFrame.output);
}
