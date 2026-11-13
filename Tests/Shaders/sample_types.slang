// Fullscreen sampling of array, cube, 3D and depth-comparison textures.
#include "SimpleRHI.slang"

SRHI_BINDING(0, 0) Texture2DArray<float4> gArray;
SRHI_BINDING(1, 0) TextureCube<float4> gCube;
SRHI_BINDING(2, 0) Texture3D<float4> gVolume;
SRHI_BINDING(3, 0) SamplerState gSampler;
SRHI_BINDING(4, 0) DepthTexture2D gDepth; // texture_depth_2d in WGSL (required by comparison sampling)
SRHI_BINDING(5, 0) SamplerComparisonState gCompare;

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

// Four vertical strips, one per array layer.
[shader("fragment")]
float4 fsArray(VSOut input) : SV_Target0
{
    float layer = floor(input.uv.x * 4.0);
    float2 uv = float2(frac(input.uv.x * 4.0), input.uv.y);
    return gArray.SampleLevel(gSampler, float3(uv, layer), 0.0);
}

// A 3x2 grid of the six cube faces (+X -X +Y / -Y +Z -Z).
[shader("fragment")]
float4 fsCube(VSOut input) : SV_Target0
{
    float2 grid = input.uv * float2(3.0, 2.0);
    int face = int(floor(grid.y)) * 3 + int(floor(grid.x));
    float2 l = frac(grid) * 2.0 - 1.0;
    float3 dir;
    switch (face)
    {
    case 0: dir = float3(1.0, -l.y, -l.x); break;
    case 1: dir = float3(-1.0, -l.y, l.x); break;
    case 2: dir = float3(l.x, 1.0, l.y); break;
    case 3: dir = float3(l.x, -1.0, -l.y); break;
    case 4: dir = float3(l.x, -l.y, 1.0); break;
    default: dir = float3(-l.x, -l.y, -1.0); break;
    }
    return gCube.SampleLevel(gSampler, dir, 0.0);
}

// Four vertical strips, one per depth slice (sampled at slice centers).
[shader("fragment")]
float4 fsVolume(VSOut input) : SV_Target0
{
    float slice = floor(input.uv.x * 4.0);
    float3 uvw = float3(frac(input.uv.x * 4.0), input.uv.y, (slice + 0.5) / 4.0);
    return gVolume.SampleLevel(gSampler, uvw, 0.0);
}

// Comparison against a reference that ramps from 0 (left) to 1 (right).
[shader("fragment")]
float4 fsCompare(VSOut input) : SV_Target0
{
    float v = gDepth.SampleCmpLevelZero(gCompare, input.uv, input.uv.x);
    return float4(v, v * 0.5, 1.0 - v, 1.0);
}
