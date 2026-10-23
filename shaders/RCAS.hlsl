// Based on AMD FidelityFX FSR RCAS (MIT).
// https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK

Texture2D<float4> ColorInput : register(t0);
RWTexture2D<float4> ColorOutput : register(u0);

cbuffer RcasConstants : register(b0)
{
    float sharpness;
    float3 pad;
};

#define FSR_RCAS_LIMIT (0.25 - (1.0 / 16.0))

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint2 size;
    ColorInput.GetDimensions(size.x, size.y);

    if (dtid.x >= size.x || dtid.y >= size.y)
        return;

    int2 p = int2(dtid.xy);
    int2 clampedMax = int2(size) - int2(1, 1);

    float3 b = ColorInput[clamp(p + int2(0, -1), int2(0, 0), clampedMax)].rgb;
    float3 d = ColorInput[clamp(p + int2(-1, 0), int2(0, 0), clampedMax)].rgb;
    float3 e = ColorInput[clamp(p, int2(0, 0), clampedMax)].rgb;
    float3 f = ColorInput[clamp(p + int2(1, 0), int2(0, 0), clampedMax)].rgb;
    float3 h = ColorInput[clamp(p + int2(0, 1), int2(0, 0), clampedMax)].rgb;

	float bL = b.b * 0.5 + (b.r * 0.5 + b.g);
	float dL = d.b * 0.5 + (d.r * 0.5 + d.g);
	float eL = e.b * 0.5 + (e.r * 0.5 + e.g);
	float fL = f.b * 0.5 + (f.r * 0.5 + f.g);
	float hL = h.b * 0.5 + (h.r * 0.5 + h.g);

	float nz = 0.25 * bL + 0.25 * dL + 0.25 * fL + 0.25 * hL - eL;
	nz = saturate(abs(nz) * rcp(max(max(max(bL, dL), max(eL, fL)), hL) - min(min(min(bL, dL), min(eL, fL)), hL)));
	nz = -0.5 * nz + 1.0;

	float mn4R = min(min(min(b.r, d.r), f.r), h.r);
	float mn4G = min(min(min(b.g, d.g), f.g), h.g);
	float mn4B = min(min(min(b.b, d.b), f.b), h.b);
	float mx4R = max(max(max(b.r, d.r), f.r), h.r);
	float mx4G = max(max(max(b.g, d.g), f.g), h.g);
	float mx4B = max(max(max(b.b, d.b), f.b), h.b);

	float2 peakC = float2(1.0, -1.0 * 4.0);

	float hitMinR = min(mn4R, e.r) * rcp(4.0 * mx4R);
	float hitMinG = min(mn4G, e.g) * rcp(4.0 * mx4G);
	float hitMinB = min(mn4B, e.b) * rcp(4.0 * mx4B);
	float hitMaxR = (peakC.x - max(mx4R, e.r)) * rcp(4.0 * mn4R + peakC.y);
	float hitMaxG = (peakC.x - max(mx4G, e.g)) * rcp(4.0 * mn4G + peakC.y);
	float hitMaxB = (peakC.x - max(mx4B, e.b)) * rcp(4.0 * mn4B + peakC.y);
	float lobeR = max(-hitMinR, hitMaxR);
	float lobeG = max(-hitMinG, hitMaxG);
	float lobeB = max(-hitMinB, hitMaxB);
	float lobe = max(-FSR_RCAS_LIMIT, min(max(lobeR, max(lobeG, lobeB)), 0.0)) * sharpness;

	lobe *= nz;

	float rcpL = rcp(4.0 * lobe + 1.0);
	float3 c = (lobe * b + lobe * d + lobe * h + lobe * f + e) * rcpL;

	ColorOutput[dtid.xy] = float4(c, 1.0);
}
