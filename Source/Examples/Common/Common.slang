// Shared helpers for the examples. #include "Common.slang" (after "SimpleRHI.slang").
#pragma once

// Matches OutputParams in App.h.
struct OutputParams
{
    float mode;           // 0: sRGB swapchain (write linear), 1: scRGB, 2: HDR10 PQ
    float paperWhiteNits; // brightness of 1.0 on HDR outputs
    float maxNits;        // display peak
    float pad;
};

static const float PI = 3.14159265359;

float3 PQEncode(float3 nits)
{
    float3 y = saturate(nits / 10000.0);
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 ym = pow(y, m1);
    return pow((c1 + c2 * ym) / (1.0 + c3 * ym), m2);
}

float3 Rec709ToRec2020(float3 c)
{
    const float3x3 m = float3x3(0.6274040, 0.3292820, 0.0433136,
                                0.0690970, 0.9195400, 0.0113612,
                                0.0163916, 0.0880132, 0.8955950);
    return mul(m, c);
}

// ACES fitted (Narkowicz) for SDR.
float3 TonemapACES(float3 x)
{
    return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}

// Reinhard on luminance with a peak, for HDR outputs (in units of paper white).
float3 TonemapHDR(float3 x, float peak)
{
    float l = dot(x, float3(0.2126, 0.7152, 0.0722));
    float mapped = l * (1.0 + l / (peak * peak)) / (1.0 + l);
    return l > 0.0 ? x * (mapped / l) : x;
}

// Scene-linear (Rec.709) color -> value to write to the swapchain.
float4 EncodeOutput(float3 linearColor, OutputParams o)
{
    if (o.mode < 0.5)
        return float4(TonemapACES(linearColor), 1.0);
    float3 hdr = TonemapHDR(linearColor, o.maxNits / o.paperWhiteNits);
    if (o.mode < 1.5)
        return float4(hdr * (o.paperWhiteNits / 80.0), 1.0);
    return float4(PQEncode(max(Rec709ToRec2020(hdr), 0.0) * o.paperWhiteNits), 1.0);
}

// Same, for UI-like content that is already display-referred (no tonemapping).
float4 EncodeDisplay(float3 linearColor, OutputParams o)
{
    if (o.mode < 0.5)
        return float4(linearColor, 1.0);
    if (o.mode < 1.5)
        return float4(linearColor * (o.paperWhiteNits / 80.0), 1.0);
    return float4(PQEncode(Rec709ToRec2020(linearColor) * o.paperWhiteNits), 1.0);
}
