// Compute-shader compositing for all five filter modes.
// Source/base/wave textures contain BOTH eyes laid out horizontally.
cbuffer CompositeConstants : register(b0)
{
    uint EyeWidth;
    uint ImageHeight;
    uint FilterId;
    uint FilterEye;        // 0=left, 1=right

    float RoiX;
    float RoiY;
    float RoiRadius;
    float DarkenBrightness;

    float BandWidthPixels;
    float RedOpacity;
    float WaveEnabled;
    float Padding0;

    float4 RingRadii0;
    float4 RingRadii1;

    uint RingCount;
    float3 Padding1;
};

Texture2D<float4> Original : register(t0);
Texture2D<float4> BaseBlur : register(t1);
Texture2D<float4> StrongBlur : register(t2);
RWTexture2D<float4> Destination : register(u0);

float closestRingDistance(float pixelRadius)
{
    float distance = 1e20;

    // HLSL FXC may unroll dynamic vector indexing and evaluate i - 4 for
    // i < 4 (unsigned underflow), even inside a conditional expression.
    // Select the packed float4 first, then use named components only.
    [loop]
    for (uint i = 0; i < min(RingCount, 8u); ++i)
    {
        float4 packedRadii = i < 4u ? RingRadii0 : RingRadii1;
        uint component = i & 3u;
        float ringRadius;
        if (component == 0u)      ringRadius = packedRadii.x;
        else if (component == 1u) ringRadius = packedRadii.y;
        else if (component == 2u) ringRadius = packedRadii.z;
        else                      ringRadius = packedRadii.w;

        distance = min(distance, abs(pixelRadius - ringRadius));
    }
    return distance;
}

[numthreads(16, 16, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchId.xy;
    if (pixel.x >= EyeWidth * 2 || pixel.y >= ImageHeight)
        return;

    uint eye = pixel.x < EyeWidth ? 0 : 1;
    int localX = int(pixel.x - eye * EyeWidth);
    float2 delta = float2(float(localX) - RoiX, float(pixel.y) - RoiY);
    float pixelRadius = length(delta);
    float4 original = Original.Load(int3(pixel, 0));

    // ROI pixels are always bit-identical to their original source.
    if (FilterId == 1 || pixelRadius <= RoiRadius)
    {
        Destination[pixel] = original;
        return;
    }

    // Mode 3: dim outside the ROI on ONE selected eye.
    if (FilterId == 3)
    {
        Destination[pixel] = eye == FilterEye
            ? float4(original.rgb * DarkenBrightness, 1.0)
            : original;
        return;
    }

    // Mode 4: dynamic Gaussian blur on ONE selected eye.
    if (FilterId == 4)
    {
        Destination[pixel] = eye == FilterEye
            ? BaseBlur.Load(int3(pixel, 0))
            : original;
        return;
    }

    float4 color = BaseBlur.Load(int3(pixel, 0));

    if (WaveEnabled > 0.5 && RingCount > 0)
    {
        float ringDistance = closestRingDistance(pixelRadius);

        // Mode 2 retains the previous blur + RED DynSWAVE scheme.
        if (FilterId == 2)
        {
            float halfWidth = 0.5 * BandWidthPixels;
            float coverage = 1.0 - smoothstep(
                max(0.0, halfWidth - 1.0), halfWidth + 1.0,
                ringDistance
            );
            color.rgb = lerp(
                color.rgb, float3(1.0, 0.0, 0.0),
                saturate(RedOpacity * coverage)
            );
        }
        // Mode 5 doubles the band width: the ORIGINAL width is the
        // fully blurred core and W/2 at EACH edge is a smooth transition.
        else if (FilterId == 5)
        {
            float coreHalfWidth = 0.5 * BandWidthPixels;
            float transitionWidth = 0.5 * BandWidthPixels;
            float alpha = 1.0 - smoothstep(
                coreHalfWidth, coreHalfWidth + transitionWidth,
                ringDistance
            );
            float3 wave = StrongBlur.Load(int3(pixel, 0)).rgb;
            color.rgb = lerp(color.rgb, wave, saturate(alpha));
        }
    }

    Destination[pixel] = float4(color.rgb, 1.0);
}
