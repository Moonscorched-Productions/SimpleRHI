// 3D vertex-colored geometry with a full transform matrix, plus MRT and fragment-storage variants.
#include "SimpleRHI.slang"

struct Params
{
    float4x4 transform;
    float4 tint;
};
SRHI_UNIFORMS(0) ConstantBuffer<Params> gParams;

struct VSIn
{
    float3 position : POSITION;
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
    o.position = mul(gParams.transform, float4(input.position, 1.0));
    o.color = input.color * gParams.tint;
    return o;
}

[shader("fragment")]
float4 fsMain(VSOut input) : SV_Target0
{
    return input.color;
}

struct MRTOut
{
    float4 color : SV_Target0;
    float4 inverted : SV_Target1;
    float4 position : SV_Target2;
};

[shader("fragment")]
MRTOut fsMRT(VSOut input)
{
    MRTOut o;
    o.color = input.color;
    o.inverted = float4(1.0 - input.color.rgb, 1.0);
    o.position = float4(frac(input.position.xy / 32.0), input.position.z, 1.0);
    return o;
}

// Every covered pixel records its packed color in a storage buffer indexed by pixel position.
SRHI_BINDING(0, 0) RWStructuredBuffer<uint> gPixels;

[shader("fragment")]
float4 fsStorage(VSOut input) : SV_Target0
{
    uint2 p = uint2(input.position.xy);
    uint4 c = uint4(saturate(input.color) * 255.0 + 0.5);
    gPixels[p.y * 64 + p.x] = c.r | (c.g << 8) | (c.b << 16) | (c.a << 24);
    return input.color;
}
