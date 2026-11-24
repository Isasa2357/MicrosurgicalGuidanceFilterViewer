// Fullscreen triangle + letterboxed stereo presentation directly into a
// DXGI swap chain (no OpenCV highgui, no GPU-to-CPU readback).
cbuffer PresentConstants : register(b0)
{
    float2 WindowSize;
    float2 ImageSize;
    float2 EyeSize;
    float2 Gaze01;
    uint DebugMarker;
    float3 Padding;
};

Texture2D<float4> StereoImage : register(t0);
SamplerState LinearClamp : register(s0);

struct VSOutput
{
    float4 position : SV_Position;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    VSOutput result;
    float2 xy;
    if (vertexId == 0) xy = float2(-1.0, -1.0);
    else if (vertexId == 1) xy = float2(-1.0, 3.0);
    else xy = float2(3.0, -1.0);

    result.position = float4(xy, 0.0, 1.0);
    return result;
}

float4 PSMain(VSOutput input) : SV_Target0
{
    float scale = min(
        WindowSize.x / ImageSize.x,
        WindowSize.y / ImageSize.y
    );
    float2 displayed = ImageSize * scale;
    float2 origin = (WindowSize - displayed) * 0.5;
    float2 uv = (input.position.xy - origin) / displayed;

    if (uv.x < 0 || uv.y < 0 || uv.x >= 1 || uv.y >= 1)
        return float4(0.0, 0.0, 0.0, 1.0);

    float4 color = StereoImage.SampleLevel(LinearClamp, uv, 0);

    if (DebugMarker != 0)
    {
        // Show the same mouse gaze in both eye images, including when the
        // mouse is over the opposite half of the stereo preview.
        float eyeU = frac(uv.x * 2.0);
        float2 current = float2(
            eyeU * EyeSize.x, uv.y * EyeSize.y
        );
        float2 gaze = Gaze01 * EyeSize;
        float2 d = current - gaze;

        float ellipse = length(float2(d.x / 16.0, d.y / 9.0));
        if (ellipse > 0.80 && ellipse < 1.16)
            color.rgb = float3(1.0, 1.0, 1.0);

        float radius = length(d);
        if (radius < 6.0) color.rgb = float3(1.0, 0.0, 0.0);
        if (radius < 3.0) color.rgb = float3(0.0, 0.0, 0.0);
    }

    return float4(color.rgb, 1.0);
}
