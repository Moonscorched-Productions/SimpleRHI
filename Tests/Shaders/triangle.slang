// Vertex-colored geometry: position + color attributes, optional per-draw offset uniform.
#include "SimpleRHI.slang"

struct DrawParams
{
    float4 offsetScale; // xy offset, zw scale
};
SRHI_UNIFORMS(0) ConstantBuffer<DrawParams> gDraw;

struct VSIn
{
    float2 position : POSITION;
    float4 color : COLOR;
};

struct VSOut
{
    float4 position : SV_Position;
    float4 color : COLOR;
};

[shader("vertex")]
VSOut vsMain(VSIn input)
{
    VSOut o;
    o.position = float4(input.position * gDraw.offsetScale.zw + gDraw.offsetScale.xy, 0.0, 1.0);
    o.color = input.color;
    return o;
}

// Per-instance offset from a second vertex buffer.
struct VSInInstanced
{
    float2 position : POSITION;
    float4 color : COLOR;
    float2 instanceOffset : TEXCOORD0;
};

[shader("vertex")]
VSOut vsInstanced(VSInInstanced input)
{
    VSOut o;
    o.position = float4(input.position * gDraw.offsetScale.zw + input.instanceOffset, 0.0, 1.0);
    o.color = input.color;
    return o;
}

[shader("fragment")]
float4 fsMain(VSOut input) : SV_Target0
{
    return input.color;
}
