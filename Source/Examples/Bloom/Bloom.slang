// HDR scene (helmet + HDRI sky) and a mip-chain bloom: 13-tap downsample with a soft threshold and Karis
// average on the first pass, tent-filter upsample with additive blending, then composite + tonemap.
#include "SimpleRHI.slang"
#include "Common.slang"
#include "Material.slang"

SRHI_BINDING(0, 0) Texture2D<float4> gSource;
SRHI_BINDING(1, 0) SamplerState gLinearClamp;
SRHI_BINDING(2, 0) Texture2D<float4> gScene;
SRHI_BINDING(3, 0) Texture2D<float4> gBloom;
SRHI_BINDING(4, 0) Texture2D<float4> gSky;
SRHI_BINDING(5, 0) SamplerState gLinearRepeat;

struct BloomParams
{
    float4x4 viewProj;
    float4x4 invViewProj;
    float4x4 model;
    float4 cameraPos;
    float4 scene;  // x exposure, y emissive boost, z sun intensity, w sky intensity
    float4 bloom;  // x threshold, y soft knee, z intensity, w upsample radius
    float4 pass;   // xy source texel size, z first downsample (threshold + Karis), w debug: show bloom only
    OutputParams output;
};
SRHI_UNIFORMS(0) ConstantBuffer<BloomParams> gParams;

// --- Scene ---------------------------------------------------------------------------------------------

struct ModelOut
{
    float4 position : SV_Position;
    float3 worldPos : TEXCOORD0;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD1;
    float4 tangent : TANGENT;
};

[shader("vertex")]
ModelOut vsModel(ModelVertex input)
{
    ModelOut o;
    float4 world = mul(gParams.model, float4(input.position, 1.0));
    o.position = mul(gParams.viewProj, world);
    o.worldPos = world.xyz;
    o.normal = mul(gParams.model, float4(input.normal, 0.0)).xyz;
    o.tangent = float4(mul(gParams.model, float4(input.tangent.xyz, 0.0)).xyz, input.tangent.w);
    o.uv = input.uv;
    return o;
}

[shader("fragment")]
float4 fsModel(ModelOut input) : SV_Target0
{
    SurfaceSample s;
    if (!SampleSurface(input.uv, input.normal, input.tangent, s))
        discard;
    float3 n = s.normal;
    float3 v = normalize(gParams.cameraPos.xyz - input.worldPos);
    float3 l = normalize(float3(0.4, 0.7, 0.5));
    float3 h = normalize(v + l);
    float NdotL = saturate(dot(n, l));
    float a = s.roughness * s.roughness;
    float d = saturate(dot(n, h));
    float D = a * a / (PI * pow(d * d * (a * a - 1.0) + 1.0, 2.0));
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), s.albedo, s.metallic);
    float3 F = f0 + (1.0 - f0) * pow(1.0 - saturate(dot(h, v)), 5.0);
    float3 direct = ((1.0 - s.metallic) * s.albedo / PI + D * F * 0.25) * NdotL * gParams.scene.z;
    float3 ambient = lerp(float3(0.15, 0.13, 0.12), float3(0.35, 0.45, 0.6), n.y * 0.5 + 0.5) * s.albedo * s.occlusion;
    return float4(direct + ambient + s.emissive * gParams.scene.y, 1.0);
}

struct FullscreenOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
FullscreenOut vsFullscreen(uint vertexId : SV_VertexID)
{
    FullscreenOut o;
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    o.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 1.0, 1.0); // far plane (sky)
    o.uv = uv;
    return o;
}

[shader("fragment")]
float4 fsSky(FullscreenOut input) : SV_Target0
{
    float2 ndc = input.uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    float4 world = mul(gParams.invViewProj, float4(ndc, 1.0, 1.0));
    float3 d = normalize(world.xyz / world.w - gParams.cameraPos.xyz);
    float2 uv = float2(atan2(d.z, d.x) / (2.0 * PI) + 0.5, acos(clamp(d.y, -1.0, 1.0)) / PI);
    return float4(gSky.SampleLevel(gLinearRepeat, uv, 0.0).rgb * gParams.scene.w, 1.0);
}

// --- Bloom ---------------------------------------------------------------------------------------------

float Luma(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// Soft-knee threshold (quadratic around the threshold).
float3 Threshold(float3 c)
{
    float threshold = gParams.bloom.x, knee = threshold * gParams.bloom.y + 1e-5;
    float br = max(c.r, max(c.g, c.b));
    float rq = clamp(br - threshold + knee, 0.0, 2.0 * knee);
    rq = rq * rq / (4.0 * knee);
    return c * max(rq, br - threshold) / max(br, 1e-5);
}

float3 KarisAverage(float3 a, float3 b, float3 c, float3 d)
{
    float wa = 1.0 / (1.0 + Luma(a)), wb = 1.0 / (1.0 + Luma(b)), wc = 1.0 / (1.0 + Luma(c)), wd = 1.0 / (1.0 + Luma(d));
    return (a * wa + b * wb + c * wc + d * wd) / (wa + wb + wc + wd);
}

float3 Tap(float2 uv)
{
    return gSource.SampleLevel(gLinearClamp, uv, 0.0).rgb;
}

// 13-tap downsample (Jimenez, "Next Generation Post Processing in Call of Duty: Advanced Warfare").
[shader("fragment")]
float4 fsDownsample(FullscreenOut input) : SV_Target0
{
    float2 t = gParams.pass.xy;
    float2 uv = input.uv;
    float3 a = Tap(uv + t * float2(-2, -2)), b = Tap(uv + t * float2(0, -2)), c = Tap(uv + t * float2(2, -2));
    float3 d = Tap(uv + t * float2(-2, 0)), e = Tap(uv), f = Tap(uv + t * float2(2, 0));
    float3 g = Tap(uv + t * float2(-2, 2)), h = Tap(uv + t * float2(0, 2)), i = Tap(uv + t * float2(2, 2));
    float3 j = Tap(uv + t * float2(-1, -1)), k = Tap(uv + t * float2(1, -1));
    float3 l = Tap(uv + t * float2(-1, 1)), m = Tap(uv + t * float2(1, 1));

    float3 result;
    if (gParams.pass.z > 0.5)
    {
        // First pass: threshold each group, then weight groups by 1 / (1 + luma) to kill fireflies.
        float3 g0 = KarisAverage(Threshold(j), Threshold(k), Threshold(l), Threshold(m));
        float3 g1 = KarisAverage(Threshold(a), Threshold(b), Threshold(d), Threshold(e));
        float3 g2 = KarisAverage(Threshold(b), Threshold(c), Threshold(e), Threshold(f));
        float3 g3 = KarisAverage(Threshold(d), Threshold(e), Threshold(g), Threshold(h));
        float3 g4 = KarisAverage(Threshold(e), Threshold(f), Threshold(h), Threshold(i));
        result = g0 * 0.5 + (g1 + g2 + g3 + g4) * 0.125;
    }
    else
    {
        result = e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
    }
    return float4(max(result, 0.0001), 1.0);
}

// 3x3 tent upsample; blended additively into the next larger mip.
[shader("fragment")]
float4 fsUpsample(FullscreenOut input) : SV_Target0
{
    float2 r = gParams.pass.xy * gParams.bloom.w;
    float2 uv = input.uv;
    float3 sum = Tap(uv) * 4.0;
    sum += (Tap(uv + float2(-r.x, 0)) + Tap(uv + float2(r.x, 0)) + Tap(uv + float2(0, -r.y)) + Tap(uv + float2(0, r.y))) * 2.0;
    sum += Tap(uv + float2(-r.x, -r.y)) + Tap(uv + float2(r.x, -r.y)) + Tap(uv + float2(-r.x, r.y)) + Tap(uv + float2(r.x, r.y));
    return float4(sum / 16.0, 1.0);
}

[shader("fragment")]
float4 fsComposite(FullscreenOut input) : SV_Target0
{
    float3 bloom = gBloom.SampleLevel(gLinearClamp, input.uv, 0.0).rgb;
    if (gParams.pass.w > 0.5)
        return EncodeOutput(bloom * gParams.scene.x, gParams.output);
    float3 scene = gScene.SampleLevel(gLinearClamp, input.uv, 0.0).rgb;
    return EncodeOutput((scene + bloom * gParams.bloom.z) * gParams.scene.x, gParams.output);
}
