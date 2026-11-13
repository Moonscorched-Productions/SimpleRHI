// Dear ImGui renderer shader for SimpleRHI. Compiled into imgui_impl_srhi_shader.h by srhic --header.
//
// ImGui colors are sRGB. Output modes:
//   0: UNORM target, sRGB values written as-is.
//   1: sRGB target, linearize (the hardware re-encodes).
//   2: extended linear sRGB (scRGB), linear * paperWhite / 80 nits.
//   3: HDR10, linear -> Rec.2020 -> nits -> PQ.
#include "SimpleRHI.slang"

SRHI_BINDING(0, 0) Texture2D<float4> gTexture;
SRHI_BINDING(1, 0) SamplerState gSampler;

struct Params
{
    float4 scaleTranslate;
    float4 output; // x: mode, y: paper white in nits
};
SRHI_UNIFORMS(0) ConstantBuffer<Params> gParams;

struct VSIn
{
    float2 position : POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR;
};

struct VSOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR;
};

[shader("vertex")]
VSOut vsMain(VSIn input)
{
    VSOut o;
    o.position = float4(input.position * gParams.scaleTranslate.xy + gParams.scaleTranslate.zw, 0.0, 1.0);
    o.uv = input.uv;
    o.color = input.color;
    return o;
}

float3 SrgbToLinear(float3 c)
{
    return select(c <= 0.04045, c / 12.92, pow((c + 0.055) / 1.055, 2.4));
}

float3 PQEncode(float3 nits)
{
    float3 y = saturate(nits / 10000.0);
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 ym = pow(y, m1);
    return pow((c1 + c2 * ym) / (1.0 + c3 * ym), m2);
}

[shader("fragment")]
float4 fsMain(VSOut input) : SV_Target0
{
    float4 c = input.color * gTexture.Sample(gSampler, input.uv);
    float mode = gParams.output.x;
    if (mode < 0.5)
        return c;
    float3 linear = SrgbToLinear(c.rgb);
    if (mode < 1.5)
        return float4(linear, c.a);
    if (mode < 2.5)
        return float4(linear * (gParams.output.y / 80.0), c.a);
    const float3x3 toRec2020 = float3x3(0.6274040, 0.3292820, 0.0433136,
                                        0.0690970, 0.9195400, 0.0113612,
                                        0.0163916, 0.0880132, 0.8955950);
    return float4(PQEncode(mul(toRec2020, linear) * gParams.output.y), c.a);
}
