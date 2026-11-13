// A spinning vertex-colored triangle.
#include "SimpleRHI.slang"
#include "Common.slang"

struct Params
{
    float4 rotation; // cos, sin, aspect, brightness
    OutputParams output;
};
SRHI_UNIFORMS(0) ConstantBuffer<Params> gParams;

struct VSIn
{
    float2 position : POSITION;
    float3 color : COLOR;
};

struct VSOut
{
    float4 position : SV_Position;
    float3 color : COLOR;
};

[shader("vertex")]
VSOut vsMain(VSIn input)
{
    float c = gParams.rotation.x, s = gParams.rotation.y;
    float2 p = float2(input.position.x * c - input.position.y * s, input.position.x * s + input.position.y * c);
    VSOut o;
    o.position = float4(p.x / gParams.rotation.z, p.y, 0.0, 1.0);
    o.color = input.color;
    return o;
}

[shader("fragment")]
float4 fsMain(VSOut input) : SV_Target0
{
    return EncodeDisplay(input.color * gParams.rotation.w, gParams.output);
}
