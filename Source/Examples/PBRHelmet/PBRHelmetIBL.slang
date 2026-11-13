// Image-based lighting precomputation: equirect -> cube, irradiance, GGX prefilter, BRDF LUT.
#include "SimpleRHI.slang"
#include "Common.slang"

SRHI_BINDING(0, 0) Texture2D<float4> gEquirect;
SRHI_BINDING(1, 0) SamplerState gLinearClamp;
SRHI_BINDING(2, 0) TextureCube<float4> gEnvironment;
SRHI_BINDING(3, 0) [format("rgba16f")] WTexture2DArray<float4> gCubeOut;

struct IBLParams
{
    float4 params; // x: roughness, y: output face size, z: source cube size, w: source mip count
};
SRHI_UNIFORMS(0) ConstantBuffer<IBLParams> gIBL;

// Direction through texel (uv in [0,1]) of a cube face (D3D / Metal / WebGPU face order).
float3 CubeDirection(uint face, float2 uv)
{
    float2 l = uv * 2.0 - 1.0;
    float3 d;
    switch (face)
    {
    case 0: d = float3(1.0, -l.y, -l.x); break;
    case 1: d = float3(-1.0, -l.y, l.x); break;
    case 2: d = float3(l.x, 1.0, l.y); break;
    case 3: d = float3(l.x, -1.0, -l.y); break;
    case 4: d = float3(l.x, -l.y, 1.0); break;
    default: d = float3(-l.x, -l.y, -1.0); break;
    }
    return normalize(d);
}

float2 DirectionToEquirect(float3 d)
{
    return float2(atan2(d.z, d.x) / (2.0 * PI) + 0.5, acos(clamp(d.y, -1.0, 1.0)) / PI);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csEquirectToCube(uint3 id : SV_DispatchThreadID)
{
    uint size = uint(gIBL.params.y);
    if (id.x >= size || id.y >= size)
        return;
    float3 dir = CubeDirection(id.z, (float2(id.xy) + 0.5) / float(size));
    float3 c = gEquirect.SampleLevel(gLinearClamp, DirectionToEquirect(dir), 1.0).rgb;
    gCubeOut.Store(id, float4(min(c, 60000.0), 1.0));
}

void Basis(float3 n, out float3 t, out float3 b)
{
    float3 up = abs(n.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    t = normalize(cross(up, n));
    b = cross(n, t);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csIrradiance(uint3 id : SV_DispatchThreadID)
{
    uint size = uint(gIBL.params.y);
    if (id.x >= size || id.y >= size)
        return;
    float3 n = CubeDirection(id.z, (float2(id.xy) + 0.5) / float(size));
    float3 t, b;
    Basis(n, t, b);
    float3 sum = 0.0;
    float count = 0.0;
    const float step = 0.025;
    for (float phi = 0.0; phi < 2.0 * PI; phi += step)
        for (float theta = 0.0; theta < 0.5 * PI; theta += step)
        {
            float3 local = float3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
            float3 dir = local.x * t + local.y * b + local.z * n;
            sum += gEnvironment.SampleLevel(gLinearClamp, dir, 4.0).rgb * cos(theta) * sin(theta);
            count += 1.0;
        }
    gCubeOut.Store(id, float4(PI * sum / count, 1.0));
}

float2 Hammersley(uint i, uint n)
{
    return float2(float(i) / float(n), float(reversebits(i)) * 2.3283064365386963e-10);
}

float3 ImportanceSampleGGX(float2 xi, float3 n, float roughness)
{
    float a = roughness * roughness;
    float phi = 2.0 * PI * xi.x;
    float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    float3 h = float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
    float3 t, b;
    Basis(n, t, b);
    return normalize(t * h.x + b * h.y + n * h.z);
}

float DistributionGGX(float NdotH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csPrefilter(uint3 id : SV_DispatchThreadID)
{
    uint size = uint(gIBL.params.y);
    if (id.x >= size || id.y >= size)
        return;
    float roughness = gIBL.params.x;
    float3 n = CubeDirection(id.z, (float2(id.xy) + 0.5) / float(size));
    float3 v = n;
    const uint kSamples = 512;
    float sourceSize = gIBL.params.z;
    float saTexel = 4.0 * PI / (6.0 * sourceSize * sourceSize);
    float3 sum = 0.0;
    float weight = 0.0;
    for (uint i = 0; i < kSamples; ++i)
    {
        float3 h = ImportanceSampleGGX(Hammersley(i, kSamples), n, roughness);
        float3 l = normalize(2.0 * dot(v, h) * h - v);
        float NdotL = dot(n, l);
        if (NdotL > 0.0)
        {
            float NdotH = saturate(dot(n, h));
            float pdf = DistributionGGX(NdotH, roughness) * 0.25 + 0.0001;
            float saSample = 1.0 / (float(kSamples) * pdf + 0.0001);
            float lod = roughness == 0.0 ? 0.0 : clamp(0.5 * log2(saSample / saTexel) + 1.0, 0.0, gIBL.params.w - 1.0);
            sum += gEnvironment.SampleLevel(gLinearClamp, l, lod).rgb * NdotL;
            weight += NdotL;
        }
    }
    gCubeOut.Store(id, float4(sum / max(weight, 0.0001), 1.0));
}

// --- BRDF LUT (rendered with a fullscreen triangle into RG16F) ---

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

float GeometrySchlickIBL(float NdotX, float roughness)
{
    float k = roughness * roughness * 0.5;
    return NdotX / (NdotX * (1.0 - k) + k);
}

[shader("fragment")]
float4 fsBrdfLut(VSOut input) : SV_Target0
{
    float NdotV = max(input.uv.x, 1e-3);
    float roughness = max(1.0 - input.uv.y, 0.02); // v up = rough 0 at the top row
    float3 v = float3(sqrt(1.0 - NdotV * NdotV), 0.0, NdotV);
    float3 n = float3(0.0, 0.0, 1.0);
    float A = 0.0, B = 0.0;
    const uint kSamples = 512;
    for (uint i = 0; i < kSamples; ++i)
    {
        float3 h = ImportanceSampleGGX(Hammersley(i, kSamples), n, roughness);
        float3 l = normalize(2.0 * dot(v, h) * h - v);
        float NdotL = saturate(l.z);
        float NdotH = saturate(h.z);
        float VdotH = saturate(dot(v, h));
        if (NdotL > 0.0)
        {
            float g = GeometrySchlickIBL(NdotV, roughness) * GeometrySchlickIBL(NdotL, roughness);
            float gVis = g * VdotH / (NdotH * NdotV);
            float fc = pow(1.0 - VdotH, 5.0);
            A += (1.0 - fc) * gVis;
            B += fc * gVis;
        }
    }
    return float4(A / float(kSamples), B / float(kSamples), 0.0, 1.0);
}
