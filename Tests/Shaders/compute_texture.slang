// Writes a deterministic pattern into an rgba8 storage texture.
#include "SimpleRHI.slang"

SRHI_BINDING(0, 0) [format("rgba8")] WTexture2D<float4> gOut; // write-only: WebGPU has no read_write rgba8unorm

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    gOut.GetDimensions(w, h);
    if (id.x >= w || id.y >= h)
        return;
    float2 uv = (float2(id.xy) + 0.5) / float2(w, h);
    bool checker = ((id.x / 8) + (id.y / 8)) % 2 == 0;
    gOut.Store(id.xy, float4(uv.x, uv.y, checker ? 1.0 : 0.25, 1.0));
}
