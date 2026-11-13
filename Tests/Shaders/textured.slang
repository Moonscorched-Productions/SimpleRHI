// Fullscreen triangle sampling a texture at an explicit LOD, tinted by a uniform.
#include "SimpleRHI.slang"

SRHI_BINDING(0, 0) Texture2D<float4> gTexture;
SRHI_BINDING(1, 0) SamplerState gSampler;

struct Params
{
    float4 tint;
    float4 uvScaleOffset; // xy scale, zw offset
    float4 lod;           // x: LOD (negative: implicit)
};
SRHI_UNIFORMS(0) ConstantBuffer<Params> gParams;

struct VSOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VSOut vsFullscreen(uint vertexId : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    o.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}

[shader("fragment")]
float4 fsSample(VSOut input) : SV_Target0
{
    float2 uv = input.uv * gParams.uvScaleOffset.xy + gParams.uvScaleOffset.zw;
    float4 c = gParams.lod.x >= 0.0 ? gTexture.SampleLevel(gSampler, uv, gParams.lod.x) : gTexture.Sample(gSampler, uv);
    return c * gParams.tint;
}
