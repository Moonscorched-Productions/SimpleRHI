// out[i] = in[i] * multiplier + i, plus an atomic counter of processed elements.
#include "SimpleRHI.slang"

struct Params
{
    uint count;
    uint multiplier;
    uint pad0;
    uint pad1;
};
SRHI_UNIFORMS(0) ConstantBuffer<Params> gParams;

SRHI_BINDING(0, 0) RWStructuredBuffer<uint> gOut;
SRHI_BINDING(1, 0) StructuredBuffer<uint> gIn;
SRHI_BINDING(2, 0) RWStructuredBuffer<Atomic<uint>> gCounter;

[shader("compute")]
[numthreads(64, 1, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gParams.count)
        return;
    gOut[id.x] = gIn[id.x] * gParams.multiplier + id.x;
    gCounter[0].add(1u);
}
