#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace cv {
class Mat;
}

// Shared frame parameters produced by main.cpp and consumed by D3D12 HLSL.
// All pixel filtering, mask compositing and presentation run on the GPU.
// OpenCV is used only to decode, convert and optionally resize video frames.
struct GpuFilterFrame {
    int filterId = 1;          // 1=None, 2=red wave, 3=dim, 4=blur, 5=blur wave
    int filterEye = 0;         // 0=left, 1=right (only for IDs 3 and 4)
    bool blurEnabled = true;
    bool waveEnabled = false;
    bool debugMarker = false;

    float roiX = 0.0f;
    float roiY = 0.0f;
    float roiRadius = 0.0f;

    float darkenBrightness = 0.25f;
    float blurSigma = 0.0f;        // IDs 2 and 4
    float baseBlurSigma = 3.0f;    // ID 5: constant, gaze independent
    float strongBlurSigma = 5.0f;  // ID 5: grows with gaze distance

    float ringWidthPixels = 0.0f;
    float redOpacity = 0.0f;
    std::array<float, 8> ringRadiiPixels{};
    unsigned int ringCount = 0;

    float gazeX01 = 0.5f;
    float gazeY01 = 0.5f;
};

// Native D3D12 + DXGI swap-chain viewer. No third-party D3D12 helper,
// CD3DX12 utility, OpenCV GUI or GPU-to-CPU readback is used.
class D3D12StereoRenderer {
public:
    D3D12StereoRenderer(
        int eyeWidth, int eyeHeight,
        float initialGazeX01, float initialGazeY01,
        const std::wstring& title
    );
    ~D3D12StereoRenderer();

    D3D12StereoRenderer(const D3D12StereoRenderer&) = delete;
    D3D12StereoRenderer& operator=(const D3D12StereoRenderer&) = delete;

    // Non-blocking Win32 message pump, returns the unconsumed keydown events.
    void pumpEvents(std::vector<int>& keys, bool& quit);
    void setWindowTitle(const std::wstring& title);

    float gazeX01() const noexcept;
    float gazeY01() const noexcept;

    // Input: two equally-sized CV_8UC3 (BGR) frames. The decoder provides
    // frames on the CPU; D3D12 handles all subsequent visual effects.
    void render(const cv::Mat& leftBgr, const cv::Mat& rightBgr,
                const GpuFilterFrame& frame);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
