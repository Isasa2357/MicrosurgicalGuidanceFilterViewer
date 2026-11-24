// Stereo Gaussian blur. All image processing is performed on the GPU.
// Clamp the horizontal footprint to the originating eye to prevent
// left/right frame bleeding at their shared boundary.
cbuffer BlurConstants : register(b0)
{
    uint EyeWidth;
    uint Width;
    uint Height;
    uint Horizontal;

    uint Radius;
    float3 Padding;
    float4 Weights[65];
};

Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Destination : register(u0);

[numthreads(16, 16, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchId.xy;
    if (pixel.x >= Width || pixel.y >= Height)
        return;

    int2 origin = int2(pixel);
    int eyeStart = int(pixel.x / EyeWidth) * int(EyeWidth);
    int eyeEnd = eyeStart + int(EyeWidth) - 1;

    float4 color = Source.Load(int3(origin, 0)) * Weights[0].x;
    [loop]
    for (uint offset = 1; offset <= Radius; ++offset)
    {
        int2 a = origin;
        int2 b = origin;

        if (Horizontal != 0)
        {
            a.x = max(eyeStart, origin.x - int(offset));
            b.x = min(eyeEnd, origin.x + int(offset));
        }
        else
        {
            a.y = max(0, origin.y - int(offset));
            b.y = min(int(Height) - 1, origin.y + int(offset));
        }

        float weight = Weights[offset].x;
        color += weight *
            (Source.Load(int3(a, 0)) + Source.Load(int3(b, 0)));
    }

    Destination[pixel] = float4(color.rgb, 1.0);
}
