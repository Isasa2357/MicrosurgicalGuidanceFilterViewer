#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>

#include "D3D12StereoRenderer.hpp"

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace {

constexpr DXGI_FORMAT kTextureFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr unsigned kBackBufferCount = 2;
constexpr unsigned kMaxGaussianRadius = 64;
constexpr unsigned kMaxRings = 8;
constexpr unsigned kThreadGroup = 16;
constexpr unsigned kCbSlotStride = 1536; // aligned to 256; fits 65 float4s
constexpr unsigned kCbSlotCount = 6;
constexpr wchar_t kWindowClass[] = L"MicrosurgicalGuidanceFilterViewerD3D12";

constexpr D3D12_RESOURCE_STATES kShaderReadState =
    static_cast<D3D12_RESOURCE_STATES>(
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
    );

enum DescriptorSlot : unsigned {
    kSourceSrv = 0, kBaseSrv = 1, kWaveSrv = 2,
    kTempSrv = 3, kResultSrv = 4,
    kTempUav = 5, kBaseUav = 6, kWaveUav = 7, kResultUav = 8,
    kDescriptorCount = 9
};

struct alignas(16) Float4 {
    float x = 0, y = 0, z = 0, w = 0;
};

struct alignas(16) BlurCB {
    std::uint32_t eyeWidth = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t horizontal = 0;

    std::uint32_t radius = 0;
    float padding[3]{};

    Float4 weights[kMaxGaussianRadius + 1]{};
};

struct alignas(16) CompositeCB {
    std::uint32_t eyeWidth = 0;
    std::uint32_t imageHeight = 0;
    std::uint32_t filterId = 1;
    std::uint32_t filterEye = 0;

    float roiX = 0, roiY = 0, roiRadius = 0, darkenBrightness = 0.25f;
    float bandWidth = 0, opacity = 0, waveEnabled = 0, padding = 0;

    Float4 radii0{};
    Float4 radii1{};

    std::uint32_t ringCount = 0;
    float padding2[3]{};
};

struct alignas(16) PresentCB {
    float windowWidth = 0, windowHeight = 0;
    float imageWidth = 0, imageHeight = 0;
    float eyeWidth = 0, eyeHeight = 0;
    float gazeX = 0.5f, gazeY = 0.5f;
    std::uint32_t debug = 0;
    float padding[3]{};
};

static_assert(sizeof(BlurCB) <= kCbSlotStride);
static_assert(sizeof(CompositeCB) <= kCbSlotStride);
static_assert(sizeof(PresentCB) <= kCbSlotStride);
static_assert(kCbSlotStride % D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT == 0);

void check(HRESULT code, const char* operation) {
    if (SUCCEEDED(code)) return;
    std::ostringstream message;
    message << operation << " failed (HRESULT=0x"
            << std::hex << std::uppercase
            << static_cast<std::uint32_t>(code) << ")";
    throw std::runtime_error(message.str());
}

D3D12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES props{};
    props.Type = type;
    props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    props.CreationNodeMask = 1;
    props.VisibleNodeMask = 1;
    return props;
}

D3D12_RESOURCE_DESC bufferDescription(std::uint64_t bytes) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

D3D12_RESOURCE_DESC textureDescription(
    unsigned width, unsigned height, bool uav
) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = kTextureFormat;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = uav
        ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
        : D3D12_RESOURCE_FLAG_NONE;
    return desc;
}

ComPtr<ID3DBlob> compileShader(
    const std::filesystem::path& path,
    const char* entry, const char* profile
) {
    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> errors;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
    const HRESULT hr = D3DCompileFromFile(
        path.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entry, profile, flags, 0, &bytecode, &errors
    );
    if (FAILED(hr)) {
        std::string message = "HLSL compile error in " + path.string();
        if (errors) {
            message += ":\n";
            message.append(
                static_cast<const char*>(errors->GetBufferPointer()),
                errors->GetBufferSize()
            );
        }
        throw std::runtime_error(message);
    }
    return bytecode;
}

std::filesystem::path shaderPath(const wchar_t* filename) {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, path.data(), static_cast<DWORD>(path.size())
    );
    if (length == 0 || length >= path.size()) {
        throw std::runtime_error("Cannot determine executable directory");
    }
    path.resize(length);
    return std::filesystem::path(path).parent_path() / L"shaders" / filename;
}

BlurCB blurConstants(
    unsigned eyeWidth, unsigned height, bool horizontal, float sigma
) {
    BlurCB cb{};
    cb.eyeWidth = eyeWidth;
    cb.width = eyeWidth * 2;
    cb.height = height;
    cb.horizontal = horizontal ? 1u : 0u;

    if (sigma <= 0.001f) {
        cb.weights[0].x = 1.0f;
        return cb;
    }
    const unsigned radius = std::min(
        kMaxGaussianRadius,
        static_cast<unsigned>(std::ceil(3.0f * sigma))
    );
    cb.radius = radius;

    const double sig = static_cast<double>(sigma);
    double normalization = 1.0;
    for (unsigned i = 1; i <= radius; ++i) {
        const double t = static_cast<double>(i);
        normalization += 2.0 * std::exp(-0.5 * (t / sig) * (t / sig));
    }

    cb.weights[0].x = static_cast<float>(1.0 / normalization);
    for (unsigned i = 1; i <= radius; ++i) {
        const double t = static_cast<double>(i);
        cb.weights[i].x = static_cast<float>(
            std::exp(-0.5 * (t / sig) * (t / sig)) / normalization
        );
    }
    return cb;
}

} // namespace

struct D3D12StereoRenderer::Impl {
    struct Texture {
        ComPtr<ID3D12Resource> resource;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
        unsigned srv = 0, uav = 0;
    };

    unsigned eyeWidth;
    unsigned eyeHeight;
    unsigned stereoWidth;

    HWND window = nullptr;
    unsigned windowWidth = 1280, windowHeight = 720;
    bool resizePending = false, quitRequested = false;
    float gazeX01 = 0.5f, gazeY01 = 0.5f;
    std::vector<int> keyEvents;

    ComPtr<IDXGIFactory6> factory;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain3> swapchain;
    ComPtr<ID3D12CommandAllocator> commandAllocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ComPtr<ID3D12Fence> fence;
    HANDLE fenceEvent = nullptr;
    std::uint64_t fenceValue = 0;

    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12DescriptorHeap> shaderHeap;
    unsigned rtvIncrement = 0, shaderIncrement = 0;
    std::array<ComPtr<ID3D12Resource>, kBackBufferCount> backBuffers{};

    ComPtr<ID3D12RootSignature> blurRoot, compositeRoot, presentRoot;
    ComPtr<ID3D12PipelineState> blurPipeline, compositePipeline, presentPipeline;

    Texture source, intermediate, base, wave, result;

    ComPtr<ID3D12Resource> upload;
    ComPtr<ID3D12Resource> constantBuffer;
    std::uint8_t* uploadCpu = nullptr;
    std::uint8_t* constantsCpu = nullptr;
    unsigned rowPitch = 0;

    Impl(
        int requestedWidth, int requestedHeight,
        float initialX, float initialY,
        const std::wstring& title
    ) : eyeWidth(static_cast<unsigned>(requestedWidth)),
        eyeHeight(static_cast<unsigned>(requestedHeight)),
        stereoWidth(eyeWidth * 2),
        gazeX01(std::clamp(initialX, 0.0f, 1.0f)),
        gazeY01(std::clamp(initialY, 0.0f, 1.0f))
    {
        if (requestedWidth < 1 || requestedHeight < 1 ||
            requestedWidth > 8192 || requestedHeight > 8192) {
            throw std::runtime_error("Invalid input image dimensions for D3D12");
        }
        createWindow(title);
        createD3D();
        createPipelines();
        createTextures();
        createUploads();
    }

    ~Impl() {
        // Destructors must not throw, including on GPU device removal.
        if (fenceEvent && queue && fence) {
            try { waitForGpu(); } catch (...) {}
        }
        if (upload && uploadCpu) upload->Unmap(0, nullptr);
        if (constantBuffer && constantsCpu) constantBuffer->Unmap(0, nullptr);
        if (fenceEvent) CloseHandle(fenceEvent);
        if (window && IsWindow(window)) DestroyWindow(window);
    }

    static LRESULT CALLBACK windowProc(
        HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam
    ) {
        Impl* self = reinterpret_cast<Impl*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA)
        );
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<Impl*>(create->lpCreateParams);
            SetWindowLongPtrW(
                hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)
            );
        }

        if (self) {
            switch (message) {
                case WM_CLOSE:
                    self->quitRequested = true;
                    DestroyWindow(hwnd);
                    return 0;
                case WM_DESTROY:
                    self->quitRequested = true;
                    self->window = nullptr;
                    PostQuitMessage(0);
                    return 0;
                case WM_SIZE:
                    self->resizePending = true;
                    return 0;
                case WM_MOUSEMOVE:
                    self->updateMouse(
                        GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)
                    );
                    return 0;
                case WM_KEYDOWN:
                    if (!(lParam & (static_cast<LPARAM>(1) << 30))) {
                        self->keyEvents.push_back(static_cast<int>(wParam));
                    }
                    return 0;
            }
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    void createWindow(const std::wstring& title) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = windowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = kWindowClass;
        windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(
            GetStockObject(BLACK_BRUSH)
        );

        if (!RegisterClassExW(&windowClass) &&
            GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            throw std::runtime_error("RegisterClassExW failed");
        }

        RECT bounds{0, 0, 1280, 720};
        AdjustWindowRect(&bounds, WS_OVERLAPPEDWINDOW, FALSE);
        window = CreateWindowExW(
            0, kWindowClass, title.c_str(), WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT,
            bounds.right - bounds.left,
            bounds.bottom - bounds.top,
            nullptr, nullptr, GetModuleHandleW(nullptr), this
        );
        if (!window) throw std::runtime_error("CreateWindowExW failed");

        RECT client{};
        GetClientRect(window, &client);
        windowWidth = std::max<LONG>(1, client.right - client.left);
        windowHeight = std::max<LONG>(1, client.bottom - client.top);

        ShowWindow(window, SW_SHOW);
        UpdateWindow(window);
    }

    void updateMouse(int x, int y) {
        RECT rect{};
        if (!window || !GetClientRect(window, &rect)) return;
        const float width = float(rect.right - rect.left);
        const float height = float(rect.bottom - rect.top);
        if (width < 1 || height < 1) return;

        const float scale = std::min(
            width / float(stereoWidth), height / float(eyeHeight)
        );
        if (scale <= 0) return;
        const float displayWidth = float(stereoWidth) * scale;
        const float displayHeight = float(eyeHeight) * scale;
        const float originX = (width - displayWidth) * 0.5f;
        const float originY = (height - displayHeight) * 0.5f;

        float imageX = std::clamp(
            (float(x) - originX) / scale, 0.0f, float(stereoWidth - 1)
        );
        float imageY = std::clamp(
            (float(y) - originY) / scale, 0.0f, float(eyeHeight - 1)
        );
        if (imageX >= eyeWidth) imageX -= eyeWidth;
        gazeX01 = imageX / float(std::max(1u, eyeWidth - 1));
        gazeY01 = imageY / float(std::max(1u, eyeHeight - 1));
    }

    void pumpEvents(std::vector<int>& keys, bool& quit) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                quitRequested = true;
                continue;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        keys.swap(keyEvents);
        keyEvents.clear();
        quit = quitRequested;
    }

    void createD3D() {
        check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)),
              "CreateDXGIFactory2");

        // Prefer a real high-performance adapter; no helper library or WARP
        // fallback is required for this GPU processing viewer.
        bool found = false;
        for (unsigned index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> adapter;
            const HRESULT hr = factory->EnumAdapterByGpuPreference(
                index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                IID_PPV_ARGS(&adapter)
            );
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(hr)) check(hr, "EnumAdapterByGpuPreference");

            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

            if (SUCCEEDED(D3D12CreateDevice(
                adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                IID_PPV_ARGS(&device)
            ))) {
                std::wcout << L"D3D12 GPU: " << desc.Description << L'\n';
                found = true;
                break;
            }
        }
        if (!found) {
            throw std::runtime_error(
                "No hardware GPU supporting Direct3D 12 was found"
            );
        }

        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(device->CreateCommandQueue(
            &queueDesc, IID_PPV_ARGS(&queue)
        ), "CreateCommandQueue");

        check(device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&commandAllocator)
        ), "CreateCommandAllocator");

        check(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            commandAllocator.Get(), nullptr,
            IID_PPV_ARGS(&commandList)
        ), "CreateCommandList");
        check(commandList->Close(), "Close initial command list");

        DXGI_SWAP_CHAIN_DESC1 swapDesc{};
        swapDesc.Width = windowWidth;
        swapDesc.Height = windowHeight;
        swapDesc.Format = kTextureFormat;
        swapDesc.SampleDesc.Count = 1;
        swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDesc.BufferCount = kBackBufferCount;
        swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> swapchain1;
        check(factory->CreateSwapChainForHwnd(
            queue.Get(), window, &swapDesc,
            nullptr, nullptr, &swapchain1
        ), "CreateSwapChainForHwnd");
        check(swapchain1.As(&swapchain), "Query IDXGISwapChain3");
        factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);

        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
        rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvDesc.NumDescriptors = kBackBufferCount;
        check(device->CreateDescriptorHeap(
            &rtvDesc, IID_PPV_ARGS(&rtvHeap)
        ), "Create RTV heap");
        rtvIncrement = device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV
        );

        D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
        srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvDesc.NumDescriptors = kDescriptorCount;
        srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        check(device->CreateDescriptorHeap(
            &srvDesc, IID_PPV_ARGS(&shaderHeap)
        ), "Create shader descriptor heap");
        shaderIncrement = device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV
        );
        createRenderTargets();

        check(device->CreateFence(
            0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)
        ), "CreateFence");
        fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!fenceEvent) throw std::runtime_error("CreateEventW failed");
        resizePending = false;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle(unsigned index) const {
        auto handle = shaderHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += SIZE_T(index) * shaderIncrement;
        return handle;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle(unsigned index) const {
        auto handle = shaderHeap->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += UINT64(index) * shaderIncrement;
        return handle;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle(unsigned index) const {
        auto handle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += SIZE_T(index) * rtvIncrement;
        return handle;
    }

    void createRenderTargets() {
        for (unsigned i = 0; i < kBackBufferCount; ++i) {
            check(swapchain->GetBuffer(
                i, IID_PPV_ARGS(&backBuffers[i])
            ), "Get swapchain backbuffer");
            device->CreateRenderTargetView(
                backBuffers[i].Get(), nullptr, rtvHandle(i)
            );
        }
    }

    void waitForGpu() {
        const std::uint64_t value = ++fenceValue;
        check(queue->Signal(fence.Get(), value), "Signal fence");
        if (fence->GetCompletedValue() < value) {
            check(fence->SetEventOnCompletion(value, fenceEvent),
                  "SetEventOnCompletion");
            if (WaitForSingleObject(fenceEvent, INFINITE) != WAIT_OBJECT_0)
                throw std::runtime_error("Waiting for GPU fence failed");
        }
    }

    bool updateSwapchainSize() {
        RECT bounds{};
        if (!window || !GetClientRect(window, &bounds)) return false;
        const unsigned width = std::max<LONG>(0, bounds.right - bounds.left);
        const unsigned height = std::max<LONG>(0, bounds.bottom - bounds.top);
        if (width == 0 || height == 0) return false;
        if (!resizePending && width == windowWidth && height == windowHeight)
            return true;

        if (width != windowWidth || height != windowHeight) {
            waitForGpu();
            // A closed command list can keep indirect references to the
            // old backbuffers; reset it before releasing those resources.
            check(commandAllocator->Reset(), "Reset allocator for resize");
            check(commandList->Reset(
                commandAllocator.Get(), nullptr
            ), "Reset command list for resize");
            check(commandList->Close(), "Close command list for resize");
            for (auto& surface : backBuffers) surface.Reset();
            check(swapchain->ResizeBuffers(
                kBackBufferCount, width, height,
                kTextureFormat, 0
            ), "ResizeBuffers");
            windowWidth = width;
            windowHeight = height;
            createRenderTargets();
        }
        resizePending = false;
        return true;
    }

    ComPtr<ID3D12RootSignature> makeComputeRoot(unsigned srvCount) {
        D3D12_DESCRIPTOR_RANGE srvRange{};
        srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors = srvCount;
        srvRange.BaseShaderRegister = 0;
        srvRange.OffsetInDescriptorsFromTableStart =
            D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE uavRange{};
        uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRange.NumDescriptors = 1;
        uavRange.BaseShaderRegister = 0;
        uavRange.OffsetInDescriptorsFromTableStart =
            D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER params[3]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 1;
        params[0].DescriptorTable.pDescriptorRanges = &srvRange;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &uavRange;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[2].Descriptor.ShaderRegister = 0;

        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters = 3;
        desc.pParameters = params;

        ComPtr<ID3DBlob> serialized, errors;
        check(D3D12SerializeRootSignature(
            &desc, D3D_ROOT_SIGNATURE_VERSION_1,
            &serialized, &errors
        ), "Serialize compute root signature");
        ComPtr<ID3D12RootSignature> root;
        check(device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(&root)
        ), "Create compute root signature");
        return root;
    }

    void createPipelines() {
        blurRoot = makeComputeRoot(1);
        compositeRoot = makeComputeRoot(3);

        D3D12_COMPUTE_PIPELINE_STATE_DESC blurDesc{};
        auto blurBlob = compileShader(
            shaderPath(L"blur.hlsl"), "main", "cs_5_0"
        );
        blurDesc.pRootSignature = blurRoot.Get();
        blurDesc.CS = {
            blurBlob->GetBufferPointer(), blurBlob->GetBufferSize()
        };
        check(device->CreateComputePipelineState(
            &blurDesc, IID_PPV_ARGS(&blurPipeline)
        ), "Create Gaussian compute pipeline");

        D3D12_COMPUTE_PIPELINE_STATE_DESC compositeDesc{};
        auto compositeBlob = compileShader(
            shaderPath(L"composite.hlsl"), "main", "cs_5_0"
        );
        compositeDesc.pRootSignature = compositeRoot.Get();
        compositeDesc.CS = {
            compositeBlob->GetBufferPointer(),
            compositeBlob->GetBufferSize()
        };
        check(device->CreateComputePipelineState(
            &compositeDesc, IID_PPV_ARGS(&compositePipeline)
        ), "Create composite compute pipeline");

        D3D12_DESCRIPTOR_RANGE srvRange{};
        srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors = 1;
        srvRange.BaseShaderRegister = 0;
        srvRange.OffsetInDescriptorsFromTableStart =
            D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 1;
        params[0].DescriptorTable.pDescriptorRanges = &srvRange;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[1].Descriptor.ShaderRegister = 0;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rootDesc{};
        rootDesc.NumParameters = 2;
        rootDesc.pParameters = params;
        rootDesc.NumStaticSamplers = 1;
        rootDesc.pStaticSamplers = &sampler;
        rootDesc.Flags =
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> serialized, errors;
        check(D3D12SerializeRootSignature(
            &rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
            &serialized, &errors
        ), "Serialize present root signature");
        check(device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(&presentRoot)
        ), "Create present root signature");

        auto vsBlob = compileShader(
            shaderPath(L"present.hlsl"), "VSMain", "vs_5_0"
        );
        auto psBlob = compileShader(
            shaderPath(L"present.hlsl"), "PSMain", "ps_5_0"
        );

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = presentRoot.Get();
        pso.VS = {vsBlob->GetBufferPointer(), vsBlob->GetBufferSize()};
        pso.PS = {psBlob->GetBufferPointer(), psBlob->GetBufferSize()};
        pso.SampleMask = UINT_MAX;
        pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.RasterizerState.DepthClipEnable = TRUE;
        pso.DepthStencilState.DepthEnable = FALSE;
        pso.DepthStencilState.StencilEnable = FALSE;
        for (auto& target : pso.BlendState.RenderTarget) {
            target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        }
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = kTextureFormat;
        pso.SampleDesc.Count = 1;

        check(device->CreateGraphicsPipelineState(
            &pso, IID_PPV_ARGS(&presentPipeline)
        ), "Create swapchain presentation pipeline");
    }

    Texture makeTexture(bool needsUav, unsigned srv, unsigned uav) {
        Texture t{};
        t.srv = srv;
        t.uav = uav;
        auto props = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
        auto desc = textureDescription(
            stereoWidth, eyeHeight, needsUav
        );
        const auto initial = needsUav
            ? D3D12_RESOURCE_STATE_COMMON
            : D3D12_RESOURCE_STATE_COPY_DEST;
        check(device->CreateCommittedResource(
            &props, D3D12_HEAP_FLAG_NONE, &desc, initial,
            nullptr, IID_PPV_ARGS(&t.resource)
        ), "Create stereo GPU texture");
        t.state = initial;

        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = kTextureFormat;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(
            t.resource.Get(), &view, cpuHandle(srv)
        );

        if (needsUav) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uavView{};
            uavView.Format = kTextureFormat;
            uavView.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            device->CreateUnorderedAccessView(
                t.resource.Get(), nullptr,
                &uavView, cpuHandle(uav)
            );
        }
        return t;
    }

    void createTextures() {
        source = makeTexture(false, kSourceSrv, 0);
        base = makeTexture(true, kBaseSrv, kBaseUav);
        wave = makeTexture(true, kWaveSrv, kWaveUav);
        intermediate = makeTexture(true, kTempSrv, kTempUav);
        result = makeTexture(true, kResultSrv, kResultUav);
    }

    void createUploads() {
        const std::uint64_t rowBytes = std::uint64_t(stereoWidth) * 4;
        rowPitch = static_cast<unsigned>(
            (rowBytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
            ~std::uint64_t(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
        );
        const auto props = heapProperties(D3D12_HEAP_TYPE_UPLOAD);

        auto desc = bufferDescription(
            std::uint64_t(rowPitch) * eyeHeight
        );
        check(device->CreateCommittedResource(
            &props, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&upload)
        ), "Create video upload buffer");

        desc = bufferDescription(
            std::uint64_t(kCbSlotStride) * kCbSlotCount
        );
        check(device->CreateCommittedResource(
            &props, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&constantBuffer)
        ), "Create constant upload buffer");

        D3D12_RANGE noRead{0, 0};
        void* mapped = nullptr;
        check(upload->Map(0, &noRead, &mapped), "Map video upload buffer");
        uploadCpu = static_cast<std::uint8_t*>(mapped);
        mapped = nullptr;
        check(constantBuffer->Map(
            0, &noRead, &mapped
        ), "Map constant upload buffer");
        constantsCpu = static_cast<std::uint8_t*>(mapped);
    }

    void transition(
        Texture& texture, D3D12_RESOURCE_STATES next
    ) {
        if (texture.state == next) return;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture.resource.Get();
        barrier.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = texture.state;
        barrier.Transition.StateAfter = next;
        commandList->ResourceBarrier(1, &barrier);
        texture.state = next;
    }

    void writeConstants(unsigned slot, const void* data, std::size_t size) {
        if (slot >= kCbSlotCount || size > kCbSlotStride)
            throw std::runtime_error("Constant buffer slot overflow");
        std::memcpy(
            constantsCpu + std::size_t(slot) * kCbSlotStride,
            data, size
        );
    }

    D3D12_GPU_VIRTUAL_ADDRESS cbAddress(unsigned slot) const {
        return constantBuffer->GetGPUVirtualAddress() +
            std::uint64_t(slot) * kCbSlotStride;
    }

    void runBlur(
        Texture& from, Texture& to,
        float sigma, bool horizontal, unsigned constantsSlot
    ) {
        transition(from, kShaderReadState);
        transition(to, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        const BlurCB cb = blurConstants(
            eyeWidth, eyeHeight, horizontal, sigma
        );
        writeConstants(constantsSlot, &cb, sizeof(cb));
        commandList->SetComputeRootSignature(blurRoot.Get());
        commandList->SetPipelineState(blurPipeline.Get());
        commandList->SetComputeRootDescriptorTable(
            0, gpuHandle(from.srv)
        );
        commandList->SetComputeRootDescriptorTable(
            1, gpuHandle(to.uav)
        );
        commandList->SetComputeRootConstantBufferView(
            2, cbAddress(constantsSlot)
        );
        commandList->Dispatch(
            (stereoWidth + kThreadGroup - 1) / kThreadGroup,
            (eyeHeight + kThreadGroup - 1) / kThreadGroup, 1
        );
        // The subsequent SRV transition orders prior UAV writes.
        transition(to, kShaderReadState);
    }

    void runBlurPair(
        Texture& target, float sigma, unsigned slot0
    ) {
        runBlur(source, intermediate, sigma, true, slot0);
        runBlur(intermediate, target, sigma, false, slot0 + 1);
    }

    void uploadStereo(
        const cv::Mat& left, const cv::Mat& right
    ) {
        if (left.cols != int(eyeWidth) || right.cols != int(eyeWidth) ||
            left.rows != int(eyeHeight) || right.rows != int(eyeHeight) ||
            left.type() != CV_8UC3 || right.type() != CV_8UC3) {
            throw std::runtime_error(
                "D3D12 renderer expects two equally-sized CV_8UC3 frames"
            );
        }

        cv::Mat leftRgba, rightRgba;
        cv::cvtColor(left, leftRgba, cv::COLOR_BGR2RGBA);
        cv::cvtColor(right, rightRgba, cv::COLOR_BGR2RGBA);

        const std::size_t eyeRowBytes = std::size_t(eyeWidth) * 4;
        for (unsigned y = 0; y < eyeHeight; ++y) {
            auto* output = uploadCpu + std::size_t(y) * rowPitch;
            std::memcpy(output, leftRgba.ptr(y), eyeRowBytes);
            std::memcpy(
                output + eyeRowBytes, rightRgba.ptr(y), eyeRowBytes
            );
        }

        transition(source, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION to{};
        to.pResource = source.resource.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION from{};
        from.pResource = upload.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint.Offset = 0;
        from.PlacedFootprint.Footprint.Format = kTextureFormat;
        from.PlacedFootprint.Footprint.Width = stereoWidth;
        from.PlacedFootprint.Footprint.Height = eyeHeight;
        from.PlacedFootprint.Footprint.Depth = 1;
        from.PlacedFootprint.Footprint.RowPitch = rowPitch;

        commandList->CopyTextureRegion(
            &to, 0, 0, 0, &from, nullptr
        );
        transition(source, kShaderReadState);
    }

    void compose(const GpuFilterFrame& settings) {
        CompositeCB cb{};
        cb.eyeWidth = eyeWidth;
        cb.imageHeight = eyeHeight;
        cb.filterId = static_cast<std::uint32_t>(settings.filterId);
        cb.filterEye = static_cast<std::uint32_t>(settings.filterEye);
        cb.roiX = settings.roiX;
        cb.roiY = settings.roiY;
        cb.roiRadius = settings.roiRadius;
        cb.darkenBrightness = settings.darkenBrightness;
        cb.bandWidth = settings.ringWidthPixels;
        cb.opacity = settings.redOpacity;
        cb.waveEnabled = settings.waveEnabled ? 1.0f : 0.0f;
        cb.ringCount = std::min(
            settings.ringCount, kMaxRings
        );
        cb.radii0.x = settings.ringRadiiPixels[0];
        cb.radii0.y = settings.ringRadiiPixels[1];
        cb.radii0.z = settings.ringRadiiPixels[2];
        cb.radii0.w = settings.ringRadiiPixels[3];
        cb.radii1.x = settings.ringRadiiPixels[4];
        cb.radii1.y = settings.ringRadiiPixels[5];
        cb.radii1.z = settings.ringRadiiPixels[6];
        cb.radii1.w = settings.ringRadiiPixels[7];

        writeConstants(4, &cb, sizeof(cb));
        transition(result, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commandList->SetComputeRootSignature(compositeRoot.Get());
        commandList->SetPipelineState(compositePipeline.Get());
        commandList->SetComputeRootDescriptorTable(
            0, gpuHandle(kSourceSrv)
        );
        commandList->SetComputeRootDescriptorTable(
            1, gpuHandle(kResultUav)
        );
        commandList->SetComputeRootConstantBufferView(
            2, cbAddress(4)
        );
        commandList->Dispatch(
            (stereoWidth + kThreadGroup - 1) / kThreadGroup,
            (eyeHeight + kThreadGroup - 1) / kThreadGroup, 1
        );
        transition(result, kShaderReadState);
    }

    void present(const GpuFilterFrame& settings, bool fromSource) {
        PresentCB cb{};
        cb.windowWidth = static_cast<float>(windowWidth);
        cb.windowHeight = static_cast<float>(windowHeight);
        cb.imageWidth = static_cast<float>(stereoWidth);
        cb.imageHeight = static_cast<float>(eyeHeight);
        cb.eyeWidth = static_cast<float>(eyeWidth);
        cb.eyeHeight = static_cast<float>(eyeHeight);
        cb.gazeX = settings.gazeX01;
        cb.gazeY = settings.gazeY01;
        cb.debug = settings.debugMarker ? 1 : 0;
        writeConstants(5, &cb, sizeof(cb));

        const unsigned index = swapchain->GetCurrentBackBufferIndex();
        D3D12_RESOURCE_BARRIER before{};
        before.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        before.Transition.pResource = backBuffers[index].Get();
        before.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        before.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        before.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        commandList->ResourceBarrier(1, &before);

        const auto handle = rtvHandle(index);
        const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        commandList->OMSetRenderTargets(1, &handle, FALSE, nullptr);
        commandList->ClearRenderTargetView(handle, clear, 0, nullptr);

        D3D12_VIEWPORT viewport{
            0.0f, 0.0f,
            float(windowWidth), float(windowHeight),
            0.0f, 1.0f
        };
        D3D12_RECT scissor{
            0, 0, LONG(windowWidth), LONG(windowHeight)
        };
        commandList->RSSetViewports(1, &viewport);
        commandList->RSSetScissorRects(1, &scissor);
        commandList->SetGraphicsRootSignature(presentRoot.Get());
        commandList->SetPipelineState(presentPipeline.Get());
        commandList->SetGraphicsRootDescriptorTable(
            0, gpuHandle(fromSource ? kSourceSrv : kResultSrv)
        );
        commandList->SetGraphicsRootConstantBufferView(
            1, cbAddress(5)
        );
        commandList->IASetPrimitiveTopology(
            D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST
        );
        commandList->DrawInstanced(3, 1, 0, 0);

        D3D12_RESOURCE_BARRIER after = before;
        after.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        after.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        commandList->ResourceBarrier(1, &after);
    }

    void render(
        const cv::Mat& left, const cv::Mat& right,
        const GpuFilterFrame& settings
    ) {
        if (quitRequested || !updateSwapchainSize()) return;

        check(commandAllocator->Reset(), "Reset command allocator");
        check(commandList->Reset(
            commandAllocator.Get(), nullptr
        ), "Reset command list");
        ID3D12DescriptorHeap* heaps[] = {shaderHeap.Get()};
        commandList->SetDescriptorHeaps(1, heaps);

        uploadStereo(left, right);

        const bool needsProcessing =
            settings.filterId != 1 &&
            !(settings.filterId == 5 && !settings.blurEnabled);

        if (needsProcessing) {
            if (settings.filterId == 2 || settings.filterId == 4) {
                runBlurPair(
                    base,
                    settings.blurEnabled ? settings.blurSigma : 0.0f,
                    0
                );
            } else if (settings.filterId == 5) {
                runBlurPair(base, settings.baseBlurSigma, 0);
                if (settings.waveEnabled) {
                    runBlurPair(wave, settings.strongBlurSigma, 2);
                }
            }
            // For ID 3 no base/wave texture is read by the shader.
            compose(settings);
        }

        present(settings, !needsProcessing);
        check(commandList->Close(), "Close frame command list");
        ID3D12CommandList* lists[] = {commandList.Get()};
        queue->ExecuteCommandLists(1, lists);
        check(swapchain->Present(0, 0), "Present swapchain");
        waitForGpu(); // Synchronous low-queue-depth prototype.
    }
};

D3D12StereoRenderer::D3D12StereoRenderer(
    int width, int height, float gazeX, float gazeY,
    const std::wstring& title
) : impl_(std::make_unique<Impl>(
    width, height, gazeX, gazeY, title
)) {}

D3D12StereoRenderer::~D3D12StereoRenderer() = default;

void D3D12StereoRenderer::pumpEvents(
    std::vector<int>& keys, bool& quit
) {
    impl_->pumpEvents(keys, quit);
}

void D3D12StereoRenderer::setWindowTitle(
    const std::wstring& title
) {
    if (impl_->window && IsWindow(impl_->window))
        SetWindowTextW(impl_->window, title.c_str());
}

float D3D12StereoRenderer::gazeX01() const noexcept {
    return impl_->gazeX01;
}

float D3D12StereoRenderer::gazeY01() const noexcept {
    return impl_->gazeY01;
}

void D3D12StereoRenderer::render(
    const cv::Mat& left, const cv::Mat& right,
    const GpuFilterFrame& frame
) {
    impl_->render(left, right, frame);
}
