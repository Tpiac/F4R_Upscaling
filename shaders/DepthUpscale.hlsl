cbuffer FlareDepthConstants : register(b0)
{
    uint2 TargetSize;
    uint2 SourceSize;
};

Texture2D<float> SourceDepth : register(t0);
RWTexture2D<float> ResolvedDepth : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dispatchID : SV_DispatchThreadID)
{
    if (any(dispatchID.xy >= TargetSize))
        return;

    int2 srcCoord = dispatchID.xy * int2(SourceSize) / int2(TargetSize);
    float d = SourceDepth[srcCoord];
    ResolvedDepth[dispatchID.xy] = d;
}
