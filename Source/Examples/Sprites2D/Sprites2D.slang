// Instanced sprite batch: one draw for every sprite, per-sprite data in a structured buffer.
#include "SimpleRHI.slang"
#include "Common.slang"

struct Sprite
{
    float2 position;   // pixels, center
    float2 size;       // pixels
    float4 color;      // linear, premultiplied in the shader
    float rotation;    // radians
    uint frame;        // atlas cell (2x2 atlas)
    float2 pad;
};

SRHI_BINDING(0, 0) StructuredBuffer<Sprite> gSprites;
SRHI_BINDING(1, 0) Texture2D<float4> gAtlas;
SRHI_BINDING(2, 0) SamplerState gSampler;

struct Params
{
    float4 viewport; // 2 / width, -2 / height, unused, unused
    OutputParams output;
};
SRHI_UNIFORMS(0) ConstantBuffer<Params> gParams;

struct VSOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR;
};

[shader("vertex")]
VSOut vsSprite(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    // Two triangles: (0,0) (1,0) (0,1) / (0,1) (1,0) (1,1)
    const float2 corners[6] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(0, 1), float2(1, 0), float2(1, 1) };
    Sprite s = gSprites[instanceId];
    float2 corner = corners[vertexId];
    float2 local = (corner - 0.5) * s.size;
    float c = cos(s.rotation), sn = sin(s.rotation);
    float2 p = s.position + float2(local.x * c - local.y * sn, local.x * sn + local.y * c);

    VSOut o;
    o.position = float4(p * gParams.viewport.xy + float2(-1.0, 1.0), 0.0, 1.0);
    float2 cell = float2(s.frame % 2, s.frame / 2);
    o.uv = (cell + corner) * 0.5;
    o.color = s.color;
    return o;
}

[shader("fragment")]
float4 fsSprite(VSOut input) : SV_Target0
{
    float4 texel = gAtlas.Sample(gSampler, input.uv);
    float4 c = texel * input.color;
    float4 encoded = EncodeDisplay(c.rgb, gParams.output);
    return float4(encoded.rgb * c.a, c.a); // premultiplied
}
