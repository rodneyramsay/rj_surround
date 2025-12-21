
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((HANDLE)-4)
#endif

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11_3.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <dxgi1_3.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <mutex>
#include <vector>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace {

constexpr int kHotkeyToggle = 1;
constexpr int kHotkeyEmergencyStop = 2;
constexpr int kHotkeyExit = 3;

struct MonitorDesc {
    HMONITOR handle{};
    RECT rc{};
};

MonitorDesc g_activeMons[3]{};
bool g_haveActiveMons = false;

UINT g_expectedWideW = 0;
UINT g_expectedWideH = 0;
UINT g_expectedHz = 0;
bool g_haveExpectedMode = false;

struct OutputWindow {
    HWND hwnd{};
    RECT rc{};
    IDXGISwapChain1* swapchain{};
    ID3D11RenderTargetView* rtv{};
    int sliceIndex{}; // 0,1,2
    HANDLE frameLatencyWaitable{};
};

struct D3DState {
    ID3D11Device* device{};
    ID3D11DeviceContext* ctx{};
    ID3D11Device1* device1{};
    ID3D11Device3* device3{};
    ID3D11VideoDevice* videoDevice{};
    ID3D11VideoContext* videoCtx{};
    IDXGIFactory2* factory{};
    ID3D11VertexShader* vs{};
    ID3D11PixelShader* ps{};
    ID3D11Buffer* cb{};
    ID3D11SamplerState* sampler{};
};

struct alignas(16) Constants {
    float sliceIndex;
    float timeSeconds;
    float useCapture;
    float isNv12;
    float isBgra;
    float flipY;
    float sliceEnabled;
    float invViewW;
    float invViewH;
    float flipX;
    float pad0;
    float pad1;
    float pad2;
};

HINSTANCE g_hInstance{};
HWND g_hiddenHwnd{};
bool g_running{};

std::vector<OutputWindow> g_outputs;
D3DState g_d3d;

std::mutex g_captureMutex;
ID3D11ShaderResourceView* g_captureSrv{};
ID3D11ShaderResourceView* g_captureSrvY{};
ID3D11ShaderResourceView* g_captureSrvUV{};
// Captured frame resources.
//
// The app keeps an owned GPU texture (`g_captureTex`) that the renderer samples from.
// Depending on capture backend/format, we may:
// - Copy directly into BGRA/RGBA (`g_captureTex` + `g_captureSrv`)
// - Or copy NV12 and use the VideoProcessor to convert to BGRA (`g_captureNv12Tex` -> `g_captureRgbTex`)
//
// All of these are written from the capture thread and read from the render thread.
ID3D11Texture2D* g_captureTex{}; // BGRA/RGBA output texture OR NV12 copy texture (when using plane SRVs)
ID3D11Texture2D* g_captureNv12Tex{}; // NV12 copy texture used for VP conversion
ID3D11Texture2D* g_captureRgbTex{};  // BGRA output of VP conversion
winrt::com_ptr<ID3D11Texture2D> g_latestFrameTex;
UINT g_captureW{};
UINT g_captureH{};

winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice g_winrtD3DDevice{nullptr};
winrt::Windows::Graphics::Capture::GraphicsCaptureItem g_captureItem{nullptr};
winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool g_framePool{nullptr};
winrt::Windows::Graphics::Capture::GraphicsCaptureSession g_session{nullptr};
winrt::event_token g_frameArrivedToken{};

std::atomic<uint64_t> g_captureFrameCounter{0};
std::atomic<bool> g_captureAccessAllowed{false};
std::atomic<uint64_t> g_captureCopiedFrameCounter{0};

std::atomic<uint32_t> g_captureSrcFormat{0};
std::atomic<uint32_t> g_captureOwnedFormat{0};
std::atomic<bool> g_captureIsNv12{false};
std::atomic<bool> g_captureUsingVp{false};

ID3D11Texture2D* g_debugReadback1x1{};

// Desktop Duplication path.
//
// When running in "3 monitor mode" we use Desktop Duplication on each physical monitor and
// composite them into a single wide texture (e.g. 7680x1440), then the shader slices that
// wide texture across 3 output windows.
IDXGIOutputDuplication* g_ddDup[3]{};
std::atomic<bool> g_useDesktopDuplication{false};
std::atomic<uint64_t> g_ddFrameCounter{0};
uint32_t g_pxA = 0;
uint32_t g_pxB = 0;
int g_pxUniqueCount = 0;
int g_pxSampleCount = 0;

std::atomic<float> g_dbgFlipX{0.0f};
std::atomic<float> g_dbgFlipY{0.0f};

std::atomic<long long> g_lastCopyQpc{0};
long long g_qpcFreq = 0;

// Latency display smoothing: show worst latency once per second (max over last 1s window).
static long long g_latencyWindowStartQpc = 0;
static float g_latencyWindowMaxMs = -1.0f;
static float g_latencyPublishedMs = -1.0f;
static long long g_latencyLastSeenCopyQpc = 0;

bool g_consoleReady{false};

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
void StopTakeover();

bool CheckHr(const HRESULT hr, const wchar_t* what) {
    if (SUCCEEDED(hr)) return true;
    wchar_t buf[512];
    wsprintfW(buf, L"%s failed (hr=0x%08X)", what, static_cast<unsigned>(hr));
    MessageBoxW(nullptr, buf, L"rj_surround", MB_ICONERROR | MB_OK);
    return false;
}

static void EnsureQpcInit() {
    if (g_qpcFreq != 0) return;
    LARGE_INTEGER f{};
    if (QueryPerformanceFrequency(&f)) {
        g_qpcFreq = f.QuadPart;
    }
}

static void MarkCopyTimestampQpc() {
    EnsureQpcInit();
    LARGE_INTEGER t{};
    if (QueryPerformanceCounter(&t)) {
        g_lastCopyQpc.store(t.QuadPart, std::memory_order_relaxed);
    }
}

static double GetLatencyMsSinceLastCopy() {
    EnsureQpcInit();
    if (g_qpcFreq <= 0) return -1.0;
    const long long copied = g_lastCopyQpc.load(std::memory_order_relaxed);
    if (copied <= 0) return -1.0;
    LARGE_INTEGER now{};
    if (!QueryPerformanceCounter(&now)) return -1.0;
    const long long dt = now.QuadPart - copied;
    return (static_cast<double>(dt) * 1000.0) / static_cast<double>(g_qpcFreq);
}

static float GetLatencyUsSinceLastCopyF() {
    const double ms = GetLatencyMsSinceLastCopy();
    if (ms < 0.0) return -1.0f;
    const double us = ms * 1000.0;
    if (us > 9999.0) return 9999.0f;
    return static_cast<float>(us);
}

static float GetLatencyWorstOverLastSecondMs() {
    EnsureQpcInit();
    if (g_qpcFreq <= 0) return -1.0f;

    LARGE_INTEGER now{};
    if (!QueryPerformanceCounter(&now)) return g_latencyPublishedMs;

    if (g_latencyWindowStartQpc == 0) {
        g_latencyWindowStartQpc = now.QuadPart;
        g_latencyWindowMaxMs = -1.0f;
        g_latencyPublishedMs = -1.0f;
    }

    // Freeze-on-idle: only incorporate samples when a new frame was actually copied.
    const long long copiedQpc = g_lastCopyQpc.load(std::memory_order_relaxed);
    const bool newFrameCopied = (copiedQpc > 0 && copiedQpc != g_latencyLastSeenCopyQpc);
    float cur = -1.0f;
    if (newFrameCopied) {
        g_latencyLastSeenCopyQpc = copiedQpc;
        cur = GetLatencyUsSinceLastCopyF();
        if (cur >= 0.0f) {
            if (g_latencyWindowMaxMs < 0.0f || cur > g_latencyWindowMaxMs) g_latencyWindowMaxMs = cur;
        }
    }

    const long long elapsed = now.QuadPart - g_latencyWindowStartQpc;
    if (elapsed >= g_qpcFreq) {
        g_latencyPublishedMs = g_latencyWindowMaxMs;
        g_latencyWindowStartQpc = now.QuadPart;
        // Start the next window with the most recent sample if we got one; otherwise keep it empty.
        g_latencyWindowMaxMs = (cur >= 0.0f) ? cur : -1.0f;
    }
    return g_latencyPublishedMs;
}

static const char* DxgiFormatName(uint32_t fmt) {
    switch (static_cast<DXGI_FORMAT>(fmt)) {
        case DXGI_FORMAT_UNKNOWN:
            return "UNKNOWN";
        case DXGI_FORMAT_B8G8R8A8_UNORM:
            return "B8G8R8A8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return "B8G8R8A8_UNORM_SRGB";
        case DXGI_FORMAT_R8G8B8A8_UNORM:
            return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return "R8G8B8A8_UNORM_SRGB";
        case DXGI_FORMAT_NV12:
            return "NV12";
        default:
            return "OTHER";
    }
}

BOOL CALLBACK EnumMonProc(HMONITOR hMon, HDC, LPRECT, LPARAM user) {
    auto* mons = reinterpret_cast<std::vector<MonitorDesc>*>(user);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMon, &mi)) return TRUE;
    MonitorDesc d;
    d.handle = hMon;
    d.rc = mi.rcMonitor;
    mons->push_back(d);
    return TRUE;
}

std::vector<MonitorDesc> GetMonitorsSortedLeftToRight() {
    std::vector<MonitorDesc> mons;
    EnumDisplayMonitors(nullptr, nullptr, EnumMonProc, reinterpret_cast<LPARAM>(&mons));
    std::sort(mons.begin(), mons.end(), [](const MonitorDesc& a, const MonitorDesc& b) {
        return a.rc.left < b.rc.left;
    });
    return mons;
}

static bool TryGetMonitorCurrentMode(HMONITOR mon, UINT& outW, UINT& outH, UINT& outHz) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return false;

    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsExW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm, 0)) return false;

    if (dm.dmPelsWidth == 0 || dm.dmPelsHeight == 0) return false;
    outW = static_cast<UINT>(dm.dmPelsWidth);
    outH = static_cast<UINT>(dm.dmPelsHeight);
    outHz = static_cast<UINT>(dm.dmDisplayFrequency);
    return true;
}

static bool TryDeriveExpectedTripleWideModeFromMonitors(const MonitorDesc mons[3], UINT& outWideW, UINT& outWideH, UINT& outHz) {
    UINT w0 = 0, h0 = 0, hz0 = 0;
    if (!TryGetMonitorCurrentMode(mons[0].handle, w0, h0, hz0)) return false;

    for (int i = 1; i < 3; i++) {
        UINT wi = 0, hi = 0, hzi = 0;
        if (!TryGetMonitorCurrentMode(mons[i].handle, wi, hi, hzi)) return false;
        if (wi != w0 || hi != h0 || hzi != hz0) return false;
    }

    outWideW = w0 * 3;
    outWideH = h0;
    outHz = hz0;
    return true;
}

void SafeRelease(IUnknown*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

void ReleaseOutputResources(OutputWindow& ow) {
    IUnknown* rtv = ow.rtv;
    SafeRelease(rtv);
    ow.rtv = nullptr;
    IUnknown* sc = ow.swapchain;
    SafeRelease(sc);
    ow.swapchain = nullptr;
    if (ow.frameLatencyWaitable) {
        CloseHandle(ow.frameLatencyWaitable);
        ow.frameLatencyWaitable = nullptr;
    }
}

void DestroyOutputs() {
    for (auto& ow : g_outputs) {
        ReleaseOutputResources(ow);
        if (ow.hwnd) DestroyWindow(ow.hwnd);
        ow.hwnd = nullptr;
    }
    g_outputs.clear();
}

void DestroyD3D() {
    for (auto& ow : g_outputs) {
        ReleaseOutputResources(ow);
    }
    IUnknown* cb = g_d3d.cb;
    SafeRelease(cb);
    g_d3d.cb = nullptr;
    IUnknown* sampler = g_d3d.sampler;
    SafeRelease(sampler);
    g_d3d.sampler = nullptr;
    IUnknown* dev1 = g_d3d.device1;
    SafeRelease(dev1);
    g_d3d.device1 = nullptr;
    IUnknown* dev3 = g_d3d.device3;
    SafeRelease(dev3);
    g_d3d.device3 = nullptr;
    IUnknown* vctx = g_d3d.videoCtx;
    SafeRelease(vctx);
    g_d3d.videoCtx = nullptr;
    IUnknown* vdev = g_d3d.videoDevice;
    SafeRelease(vdev);
    g_d3d.videoDevice = nullptr;
    IUnknown* ps = g_d3d.ps;
    SafeRelease(ps);
    g_d3d.ps = nullptr;
    IUnknown* vs = g_d3d.vs;
    SafeRelease(vs);
    g_d3d.vs = nullptr;

    IUnknown* factory = g_d3d.factory;
    SafeRelease(factory);
    g_d3d.factory = nullptr;

    IUnknown* rb = g_debugReadback1x1;
    SafeRelease(rb);
    g_debugReadback1x1 = nullptr;

    for (auto& dd : g_ddDup) {
        IUnknown* p = dd;
        SafeRelease(p);
        dd = nullptr;
    }
    g_useDesktopDuplication.store(false, std::memory_order_relaxed);

    IUnknown* ctx = g_d3d.ctx;
    SafeRelease(ctx);
    g_d3d.ctx = nullptr;

    IUnknown* dev = g_d3d.device;
    SafeRelease(dev);
    g_d3d.device = nullptr;
}

void StopCapture() {
    {
        std::scoped_lock lk(g_captureMutex);
        IUnknown* srv = g_captureSrv;
        SafeRelease(srv);
        g_captureSrv = nullptr;
        IUnknown* srvY = g_captureSrvY;
        SafeRelease(srvY);
        g_captureSrvY = nullptr;
        IUnknown* srvUV = g_captureSrvUV;
        SafeRelease(srvUV);
        g_captureSrvUV = nullptr;
        IUnknown* tex = g_captureTex;
        SafeRelease(tex);
        g_captureTex = nullptr;
        IUnknown* nv12 = g_captureNv12Tex;
        SafeRelease(nv12);
        g_captureNv12Tex = nullptr;
        IUnknown* rgb = g_captureRgbTex;
        SafeRelease(rgb);
        g_captureRgbTex = nullptr;
        g_latestFrameTex = nullptr;
        g_captureW = 0;
        g_captureH = 0;
    }

    g_captureIsNv12.store(false, std::memory_order_relaxed);
    g_captureUsingVp.store(false, std::memory_order_relaxed);

    {
        for (auto& dd : g_ddDup) {
            IUnknown* p = dd;
            SafeRelease(p);
            dd = nullptr;
        }
    }
    g_useDesktopDuplication.store(false, std::memory_order_relaxed);
    g_ddFrameCounter.store(0, std::memory_order_relaxed);
    g_pxA = 0;
    g_pxB = 0;
    g_pxUniqueCount = 0;
    g_pxSampleCount = 0;
}

static bool StartDesktopDuplicationForMonitors(const MonitorDesc mons[3]) {
    if (!g_d3d.device) return false;

    StopCapture();

    IDXGIDevice* dxgiDevice = nullptr;
    HRESULT hr = g_d3d.device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
    if (FAILED(hr) || !dxgiDevice) return false;

    IDXGIAdapter* adapter = nullptr;
    hr = dxgiDevice->GetAdapter(&adapter);
    dxgiDevice->Release();
    if (FAILED(hr) || !adapter) return false;

    // Create one duplication per monitor (left/middle/right).
    for (int m = 0; m < 3; m++) {
        IDXGIOutput* output = nullptr;
        for (UINT i = 0;; i++) {
            IDXGIOutput* out = nullptr;
            hr = adapter->EnumOutputs(i, &out);
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(hr) || !out) continue;

            DXGI_OUTPUT_DESC od{};
            if (SUCCEEDED(out->GetDesc(&od)) && od.Monitor == mons[m].handle) {
                output = out;
                break;
            }
            out->Release();
        }

        if (!output) {
            adapter->Release();
            char buf[160];
            snprintf(buf, sizeof(buf), "[rj_surround] DD: could not find output for monitor[%d]\n", m);
            OutputDebugStringA(buf);
            if (g_consoleReady) {
                fputs(buf, stdout);
                fflush(stdout);
            }
            return false;
        }

        IDXGIOutput1* output1 = nullptr;
        hr = output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output1));
        output->Release();
        if (FAILED(hr) || !output1) {
            adapter->Release();
            return false;
        }

        hr = output1->DuplicateOutput(g_d3d.device, &g_ddDup[m]);
        output1->Release();
        if (FAILED(hr) || !g_ddDup[m]) {
            adapter->Release();
            char buf[160];
            snprintf(buf, sizeof(buf), "[rj_surround] DD: DuplicateOutput[%d] failed hr=0x%08X\n", m, static_cast<unsigned>(hr));
            OutputDebugStringA(buf);
            if (g_consoleReady) {
                fputs(buf, stdout);
                fflush(stdout);
            }
            return false;
        }
    }

    adapter->Release();

    g_useDesktopDuplication.store(true, std::memory_order_relaxed);
    g_ddFrameCounter.store(0, std::memory_order_relaxed);
    return true;
}

winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice CreateWinRTD3DDeviceFromD3D11(ID3D11Device* d3d11) {
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice out{nullptr};
    IDXGIDevice* dxgiDevice = nullptr;
    HRESULT hr = d3d11->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
    if (FAILED(hr) || !dxgiDevice) return out;

    IInspectable* insp = nullptr;
    hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice, &insp);
    dxgiDevice->Release();
    if (FAILED(hr) || !insp) return out;

    winrt::com_ptr<IInspectable> inspPtr;
    inspPtr.attach(insp);
    out = inspPtr.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
    return out;
}

winrt::Windows::Graphics::Capture::GraphicsCaptureItem CreateCaptureItemForPrimaryMonitor() {
    HMONITOR primary = MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
    if (!primary) return nullptr;

    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{nullptr};
    auto interopFactory = winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    HRESULT hr = interopFactory->CreateForMonitor(primary, __uuidof(ABI::Windows::Graphics::Capture::IGraphicsCaptureItem), winrt::put_abi(item));
    if (FAILED(hr)) return nullptr;
    return item;
}

static bool CreateSwapchainForWindow(OutputWindow& ow) {
    if (!g_d3d.factory || !g_d3d.device) return false;

    RECT cr{};
    GetClientRect(ow.hwnd, &cr);
    const UINT clientW = static_cast<UINT>(cr.right - cr.left);
    const UINT clientH = static_cast<UINT>(cr.bottom - cr.top);

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = clientW;
    desc.Height = clientH;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    HRESULT hr = g_d3d.factory->CreateSwapChainForHwnd(g_d3d.device, ow.hwnd, &desc, nullptr, nullptr, &ow.swapchain);
    if (FAILED(hr) || !ow.swapchain) return false;

    if (ow.sliceIndex == 0) {
        IDXGISwapChain2* sc2 = nullptr;
        if (SUCCEEDED(ow.swapchain->QueryInterface(__uuidof(IDXGISwapChain2), reinterpret_cast<void**>(&sc2))) && sc2) {
            (void)sc2->SetMaximumFrameLatency(1);
            ow.frameLatencyWaitable = sc2->GetFrameLatencyWaitableObject();
            sc2->Release();
        }
    }

    ID3D11Texture2D* back = nullptr;
    hr = ow.swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back));
    if (FAILED(hr) || !back) return false;

    hr = g_d3d.device->CreateRenderTargetView(back, nullptr, &ow.rtv);
    back->Release();
    return SUCCEEDED(hr) && ow.rtv;
}

bool InitD3D() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL flOut{};
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &g_d3d.device, &flOut, &g_d3d.ctx);
    if (FAILED(hr)) return CheckHr(hr, L"D3D11CreateDevice");

    (void)g_d3d.device->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&g_d3d.device1));
    (void)g_d3d.device->QueryInterface(__uuidof(ID3D11Device3), reinterpret_cast<void**>(&g_d3d.device3));
    (void)g_d3d.device->QueryInterface(__uuidof(ID3D11VideoDevice), reinterpret_cast<void**>(&g_d3d.videoDevice));
    (void)g_d3d.ctx->QueryInterface(__uuidof(ID3D11VideoContext), reinterpret_cast<void**>(&g_d3d.videoCtx));

    IDXGIDevice* dxgiDevice = nullptr;
    hr = g_d3d.device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
    if (FAILED(hr)) return CheckHr(hr, L"QueryInterface(IDXGIDevice)");
    IDXGIAdapter* adapter = nullptr;
    hr = dxgiDevice->GetAdapter(&adapter);
    dxgiDevice->Release();
    if (FAILED(hr)) return CheckHr(hr, L"IDXGIDevice::GetAdapter");
    hr = adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&g_d3d.factory));
    adapter->Release();
    if (FAILED(hr)) return CheckHr(hr, L"IDXGIAdapter::GetParent");

    // Diagnostics readback texture.
    {
        D3D11_TEXTURE2D_DESC rd{};
        rd.Width = 1;
        rd.Height = 1;
        rd.MipLevels = 1;
        rd.ArraySize = 1;
        rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        rd.SampleDesc.Count = 1;
        rd.Usage = D3D11_USAGE_STAGING;
        rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* t = nullptr;
        if (SUCCEEDED(g_d3d.device->CreateTexture2D(&rd, nullptr, &t)) && t) {
            g_debugReadback1x1 = t;
        }
    }

    // Shaders.
    //
    // We draw a single full-screen triangle (no vertex buffer) and compute UVs from
    // SV_Position so we are resilient to swapchain/client-size mismatches.
    static const char* kVsSrc =
        "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };"
        "VSOut main(uint vid : SV_VertexID) {"
        "  float2 p[3] = { float2(-1,-1), float2(-1,3), float2(3,-1) };"
        "  float2 u[3] = { float2(0,0), float2(0,2), float2(2,0) };"
        "  VSOut o; o.pos=float4(p[vid],0,1);"
        "  o.uv = u[vid];"
        "  return o;"
        "}";

    // Pixel shader source (HLSL) used for all output windows.
    //
    // How it's used:
    // - This string is compiled at startup in InitD3D() via D3DCompile(..., "ps_5_0").
    // - The resulting bytecode is turned into an ID3D11PixelShader (g_d3d.ps).
    // - Each frame, RenderFrame() updates the constant buffer (g_d3d.cb) with per-window
    //   values like slice index, viewport size, slice enablement, and latency.
    // - We then draw a full-screen triangle (Draw(3,0)) into each output swapchain backbuffer.
    //
    // Inputs:
    // - capTex (t0): shader resource view of the current captured/composited texture.
    // - capSamp (s0): clamp/linear sampler.
    // - C (b0): constants updated per output window.
    static const char* kPsSrc =
        "Texture2D capTex : register(t0);"
        "SamplerState capSamp : register(s0);"
        "cbuffer C : register(b0) { float sliceIndex; float timeSeconds; float useCapture; float isNv12; float isBgra; float flipY; float sliceEnabled; float invViewW; float invViewH; float flipX; float pad0; float pad1; float pad2; }"
        "struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; };"
        "float4 main(PSIn i) : SV_Target {"
        // UV mapping: derive 0..1 from SV_Position and viewport size, then optionally slice.
        "  float2 uv = float2(i.pos.x * invViewW, i.pos.y * invViewH);"
        "  if (sliceEnabled > 0.5) {"
        "    float localX = uv.x;"
        "    if (flipX > 0.5) localX = 1.0 - localX;"
        "    uv.x = (localX + sliceIndex) / 3.0;"
        "  } else {"
        "    if (flipX > 0.5) uv.x = 1.0 - uv.x;"
        "  }"
        "  if (flipY > 0.5) uv.y = 1.0 - uv.y;"
        "  float4 c = capTex.Sample(capSamp, uv);"
        "  if (useCapture < 0.5) return float4(uv.x, uv.y, 0.15, 1);"
        "  if (isBgra > 0.5) c = c.bgra;"
        "  return c;"
        "}";

    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* err = nullptr;
    hr = D3DCompile(kVsSrc, strlen(kVsSrc), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, &err);
    if (FAILED(hr)) return CheckHr(hr, L"D3DCompile(VS)");
    hr = D3DCompile(kPsSrc, strlen(kPsSrc), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, &err);
    if (FAILED(hr)) return CheckHr(hr, L"D3DCompile(PS)");

    hr = g_d3d.device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_d3d.vs);
    if (FAILED(hr)) return CheckHr(hr, L"CreateVertexShader");
    hr = g_d3d.device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_d3d.ps);
    if (FAILED(hr)) return CheckHr(hr, L"CreatePixelShader");
    vsBlob->Release();
    psBlob->Release();

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = (sizeof(Constants) + 15u) & ~15u;
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = g_d3d.device->CreateBuffer(&cbd, nullptr, &g_d3d.cb);
    if (FAILED(hr)) return CheckHr(hr, L"CreateBuffer(CB)");

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    hr = g_d3d.device->CreateSamplerState(&sd, &g_d3d.sampler);
    if (FAILED(hr)) return CheckHr(hr, L"CreateSamplerState");

    g_d3d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_d3d.ctx->VSSetShader(g_d3d.vs, nullptr, 0);
    g_d3d.ctx->PSSetShader(g_d3d.ps, nullptr, 0);
    g_d3d.ctx->VSSetConstantBuffers(0, 1, &g_d3d.cb);
    g_d3d.ctx->PSSetConstantBuffers(0, 1, &g_d3d.cb);
    g_d3d.ctx->PSSetSamplers(0, 1, &g_d3d.sampler);

    return true;
}

bool StartCapturePrimary() {
    // Minimal WGC capture start.
    StopCapture();
    g_captureFrameCounter.store(0, std::memory_order_relaxed);
    g_captureCopiedFrameCounter.store(0, std::memory_order_relaxed);
    g_captureSrcFormat.store(0, std::memory_order_relaxed);
    g_captureOwnedFormat.store(0, std::memory_order_relaxed);

    if (!winrt::Windows::Graphics::Capture::GraphicsCaptureSession::IsSupported()) return false;
    if (!g_captureAccessAllowed.load()) return false;
    if (!g_d3d.device) return false;

    g_winrtD3DDevice = CreateWinRTD3DDeviceFromD3D11(g_d3d.device);
    if (!g_winrtD3DDevice) return false;

    g_captureItem = CreateCaptureItemForPrimaryMonitor();
    if (!g_captureItem) return false;

    auto sz = g_captureItem.Size();
    g_captureW = static_cast<UINT>(sz.Width);
    g_captureH = static_cast<UINT>(sz.Height);

    g_framePool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
        g_winrtD3DDevice,
        winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
        2,
        sz);

    g_session = g_framePool.CreateCaptureSession(g_captureItem);
    try {
        g_session.IsCursorCaptureEnabled(false);
    } catch (...) {
    }

    g_frameArrivedToken = g_framePool.FrameArrived([&](auto const& sender, auto const&) {
        try {
            auto frame = sender.TryGetNextFrame();
            if (!frame) return;
            auto surface = frame.Surface();
            if (!surface) return;

            winrt::com_ptr<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess> access;
            surface.as(access);
            if (!access) return;

            ID3D11Texture2D* texRaw = nullptr;
            HRESULT hr = access->GetInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texRaw));
            if (FAILED(hr) || !texRaw) return;

            winrt::com_ptr<ID3D11Texture2D> tex;
            tex.attach(texRaw);
            D3D11_TEXTURE2D_DESC td{};
            tex->GetDesc(&td);
            g_captureSrcFormat.store(static_cast<uint32_t>(td.Format), std::memory_order_relaxed);

            {
                std::scoped_lock lk(g_captureMutex);
                g_latestFrameTex = tex;
                g_captureW = td.Width;
                g_captureH = td.Height;
            }
            g_captureFrameCounter.fetch_add(1, std::memory_order_relaxed);
        } catch (...) {
        }
    });

    try {
        g_session.StartCapture();
    } catch (...) {
        StopCapture();
        return false;
    }

    return true;
}

void RenderFrame() {
    if (!g_running || !g_d3d.device || !g_d3d.ctx) return;

    static uint64_t lastSeenFrame = 0;
    static uint64_t s_renderFrameCounter = 0;
    static long long s_lastLatencySeenCopyQpc = 0;
    static float s_lastCopyToPresentUs = -1.0f;
    static double s_accTotalMs = 0.0;
    static double s_accCaptureMs = 0.0;
    static double s_accRenderMs = 0.0;
    static double s_accPresentBlockMs = 0.0;
    static double s_maxPresentBlockMs = 0.0;
    static double s_maxTotalMs = 0.0;
    static double s_accWaitMs = 0.0;
    static double s_maxWaitMs = 0.0;
    static uint64_t s_accFrameCount = 0;
    const float timeSeconds = static_cast<float>(GetTickCount64()) / 1000.0f;

    EnsureQpcInit();
    LARGE_INTEGER qpcFrameStart{};
    (void)QueryPerformanceCounter(&qpcFrameStart);
    auto QpcToMs = [&](long long dtQpc) -> double {
        if (g_qpcFreq <= 0) return 0.0;
        return (static_cast<double>(dtQpc) * 1000.0) / static_cast<double>(g_qpcFreq);
    };

    double waitMsThisFrame = 0.0;
    if (!g_outputs.empty() && g_outputs[0].frameLatencyWaitable) {
        LARGE_INTEGER qpcWaitA{};
        LARGE_INTEGER qpcWaitB{};
        (void)QueryPerformanceCounter(&qpcWaitA);
        (void)WaitForSingleObject(g_outputs[0].frameLatencyWaitable, 100);
        (void)QueryPerformanceCounter(&qpcWaitB);
        waitMsThisFrame = QpcToMs(qpcWaitB.QuadPart - qpcWaitA.QuadPart);
    }

    auto EnsureCaptureTexture = [&](UINT w, UINT h) {
        bool needCreate = false;
        if (!g_captureTex || !g_captureSrv) {
            needCreate = true;
        } else {
            D3D11_TEXTURE2D_DESC cur{};
            g_captureTex->GetDesc(&cur);
            if (cur.Width != w || cur.Height != h) needCreate = true;
        }

        if (!needCreate) return;

        std::scoped_lock lk(g_captureMutex);
        IUnknown* oldSrv = g_captureSrv;
        SafeRelease(oldSrv);
        g_captureSrv = nullptr;
        IUnknown* oldTex = g_captureTex;
        SafeRelease(oldTex);
        g_captureTex = nullptr;

        D3D11_TEXTURE2D_DESC sd{};
        sd.Width = w;
        sd.Height = h;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_DEFAULT;
        sd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;

        ID3D11Texture2D* newTex = nullptr;
        if (SUCCEEDED(g_d3d.device->CreateTexture2D(&sd, nullptr, &newTex)) && newTex) {
            g_captureTex = newTex;
            ID3D11ShaderResourceView* newSrv = nullptr;
            if (SUCCEEDED(g_d3d.device->CreateShaderResourceView(g_captureTex, nullptr, &newSrv)) && newSrv) {
                g_captureSrv = newSrv;
            }
        }
        g_captureOwnedFormat.store(static_cast<uint32_t>(DXGI_FORMAT_B8G8R8A8_UNORM), std::memory_order_relaxed);
    };

    // Pull latest frame and copy to our own shader-readable texture.
    // Prefer Desktop Duplication when enabled; otherwise use WGC.
    LARGE_INTEGER qpcAfterCapture{};
    {
        const bool useDd = g_useDesktopDuplication.load(std::memory_order_relaxed);
        if (useDd && g_ddDup[0] && g_ddDup[1] && g_ddDup[2]) {
            // Composite 3 monitor frames into one wide texture.
            // IMPORTANT: Don't use window RECT sizes here (they can be DPI-logical). Use the actual
            // Desktop Duplication frame texture dimensions.
            static UINT s_tileW = 0;
            static UINT s_tileH = 0;

            bool ddAccessLost = false;

            bool anyFrame = false;
            for (int m = 0; m < 3; m++) {
                DXGI_OUTDUPL_FRAME_INFO info{};
                IDXGIResource* res = nullptr;
                HRESULT hr = g_ddDup[m]->AcquireNextFrame(0, &info, &res);
                if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
                    continue;
                }
                if (hr == DXGI_ERROR_ACCESS_LOST) {
                    ddAccessLost = true;
                }
                if (FAILED(hr) || !res) {
                    static HRESULT s_lastDdAcquireHr[3] = {S_OK, S_OK, S_OK};
                    if (hr != s_lastDdAcquireHr[m]) {
                        s_lastDdAcquireHr[m] = hr;
                        char buf[200];
                        snprintf(buf, sizeof(buf), "[rj_surround] DD: AcquireNextFrame[%d] failed hr=0x%08X\n", m, static_cast<unsigned>(hr));
                        OutputDebugStringA(buf);
                        if (g_consoleReady) {
                            fputs(buf, stdout);
                            fflush(stdout);
                        }
                    }
                    continue;
                }

                ID3D11Texture2D* tex2d = nullptr;
                hr = res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex2d));
                if (SUCCEEDED(hr) && tex2d) {
                    D3D11_TEXTURE2D_DESC td{};
                    tex2d->GetDesc(&td);
                    if (s_tileW != td.Width || s_tileH != td.Height) {
                        s_tileW = td.Width;
                        s_tileH = td.Height;
                    }

                    const UINT wideW = s_tileW * 3;
                    const UINT wideH = s_tileH;
                    g_captureSrcFormat.store(static_cast<uint32_t>(td.Format), std::memory_order_relaxed);
                    {
                        std::scoped_lock lk(g_captureMutex);
                        g_captureW = wideW;
                        g_captureH = wideH;
                    }
                    EnsureCaptureTexture(wideW, wideH);

                    if (g_captureTex) {
                        D3D11_BOX srcBox{0, 0, 0, s_tileW, s_tileH, 1};
                        g_d3d.ctx->CopySubresourceRegion(g_captureTex, 0, s_tileW * m, 0, 0, tex2d, 0, &srcBox);
                        anyFrame = true;
                    }
                }
                if (tex2d) tex2d->Release();
                res->Release();
                g_ddDup[m]->ReleaseFrame();
            }

            // Desktop Duplication can be invalidated by display mode changes (fullscreen apps,
            // resolution/refresh changes, driver resets). Recover by recreating the duplication
            // objects instead of exiting.
            if (ddAccessLost && g_haveActiveMons) {
                StopCapture();
                if (!StartDesktopDuplicationForMonitors(g_activeMons)) {
                    g_useDesktopDuplication.store(false, std::memory_order_relaxed);
                }
            }

            if (anyFrame) {
                const uint64_t ddCur = g_ddFrameCounter.fetch_add(1, std::memory_order_relaxed) + 1;
                MarkCopyTimestampQpc();
                g_captureCopiedFrameCounter.store(ddCur, std::memory_order_relaxed);
            }
        }

        if (!g_useDesktopDuplication.load(std::memory_order_relaxed)) {
            winrt::com_ptr<ID3D11Texture2D> src;
            UINT w = 0, h = 0;
            {
                std::scoped_lock lk(g_captureMutex);
                src = g_latestFrameTex;
                w = g_captureW;
                h = g_captureH;
            }

            const uint64_t curFrame = g_captureFrameCounter.load(std::memory_order_relaxed);
            if (src && curFrame != lastSeenFrame) {
                EnsureCaptureTexture(w, h);
                if (g_captureTex) {
                    g_d3d.ctx->CopyResource(g_captureTex, src.get());
                    MarkCopyTimestampQpc();
                    g_captureCopiedFrameCounter.store(curFrame, std::memory_order_relaxed);
                }
                lastSeenFrame = curFrame;
            }
        }
    }

    (void)QueryPerformanceCounter(&qpcAfterCapture);

    ID3D11ShaderResourceView* srvLocal = nullptr;
    {
        std::scoped_lock lk(g_captureMutex);
        srvLocal = g_captureSrv;
        if (srvLocal) srvLocal->AddRef();
    }
    ID3D11ShaderResourceView* srvs[1] = {srvLocal};
    g_d3d.ctx->PSSetShaderResources(0, 1, srvs);

    // Debug output once per second.
    {
        static ULONGLONG lastLogMs = 0;
        static uint64_t lastRenderedForFps = 0;
        static float lastFps = 0.0f;
        const ULONGLONG nowMs = GetTickCount64();
        if (lastLogMs == 0 || (nowMs - lastLogMs) >= 1000) {
            const ULONGLONG prevLogMs = lastLogMs;
            lastLogMs = nowMs;
            const bool usingDd = g_useDesktopDuplication.load(std::memory_order_relaxed);
            UINT w = 0, h = 0;
            {
                std::scoped_lock lk(g_captureMutex);
                w = g_captureW;
                h = g_captureH;
            }

            const uint64_t rendered = s_renderFrameCounter;
            const uint64_t dFrames = rendered - lastRenderedForFps;
            if (prevLogMs != 0 && nowMs > prevLogMs) {
                const double dtSec = static_cast<double>(nowMs - prevLogMs) / 1000.0;
                if (dtSec > 0.0) lastFps = static_cast<float>(static_cast<double>(dFrames) / dtSec);
            }
            lastRenderedForFps = rendered;

            const float fps = lastFps;
            const float latencyUs = s_lastCopyToPresentUs;

            double avgTotalMs = 0.0;
            double avgCaptureMs = 0.0;
            double avgRenderMs = 0.0;
            double avgPresentMs = 0.0;
            double avgWaitMs = 0.0;
            double maxPresentMs = 0.0;
            double maxWaitMs = 0.0;
            double maxTotalMs = 0.0;
            if (s_accFrameCount > 0) {
                const double denom = static_cast<double>(s_accFrameCount);
                avgTotalMs = s_accTotalMs / denom;
                avgCaptureMs = s_accCaptureMs / denom;
                avgRenderMs = s_accRenderMs / denom;
                avgPresentMs = s_accPresentBlockMs / denom;
                avgWaitMs = s_accWaitMs / denom;
                maxPresentMs = s_maxPresentBlockMs;
                maxWaitMs = s_maxWaitMs;
                maxTotalMs = s_maxTotalMs;
            }
            s_accTotalMs = 0.0;
            s_accCaptureMs = 0.0;
            s_accRenderMs = 0.0;
            s_accPresentBlockMs = 0.0;
            s_maxPresentBlockMs = 0.0;
            s_accWaitMs = 0.0;
            s_maxWaitMs = 0.0;
            s_maxTotalMs = 0.0;
            s_accFrameCount = 0;

            char buf[420];
            snprintf(
                buf,
                sizeof(buf),
                "[rj_surround] backend=%s fps=%.1f Latency(uS)=%.0f size=%ux%u expected=%ux%u@%u avg(ms) total=%.2f wait=%.2f cap=%.2f render=%.2f present=%.2f max(ms) wait=%.2f present=%.2f total=%.2f\n",
                usingDd ? "DD" : "WGC",
                static_cast<double>(fps),
                static_cast<double>(latencyUs),
                static_cast<unsigned>(w),
                static_cast<unsigned>(h),
                static_cast<unsigned>(g_haveExpectedMode ? g_expectedWideW : 0),
                static_cast<unsigned>(g_haveExpectedMode ? g_expectedWideH : 0),
                static_cast<unsigned>(g_haveExpectedMode ? g_expectedHz : 0),
                avgTotalMs,
                avgWaitMs,
                avgCaptureMs,
                avgRenderMs,
                avgPresentMs,
                maxWaitMs,
                maxPresentMs,
                maxTotalMs);
            OutputDebugStringA(buf);
            if (g_consoleReady) {
                fputs(buf, stdout);
                fflush(stdout);
            }
        }
    }

    LARGE_INTEGER qpcRenderStart{};
    (void)QueryPerformanceCounter(&qpcRenderStart);
    double presentBlockMsThisFrame = 0.0;

    for (auto& ow : g_outputs) {
        if (!ow.swapchain || !ow.rtv) continue;

        RECT cr{};
        GetClientRect(ow.hwnd, &cr);
        const UINT clientW = static_cast<UINT>(cr.right - cr.left);
        const UINT clientH = static_cast<UINT>(cr.bottom - cr.top);

        DXGI_SWAP_CHAIN_DESC1 scd{};
        if (SUCCEEDED(ow.swapchain->GetDesc1(&scd))) {
            if (scd.Width != clientW || scd.Height != clientH) {
                IUnknown* rtv = ow.rtv;
                SafeRelease(rtv);
                ow.rtv = nullptr;

                HRESULT hr = ow.swapchain->ResizeBuffers(0, clientW, clientH, DXGI_FORMAT_UNKNOWN, 0);
                if (SUCCEEDED(hr)) {
                    ID3D11Texture2D* back = nullptr;
                    hr = ow.swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back));
                    if (SUCCEEDED(hr) && back) {
                        hr = g_d3d.device->CreateRenderTargetView(back, nullptr, &ow.rtv);
                        back->Release();
                    }
                }

                if (!ow.rtv) continue;
            }
        }

        // Use backbuffer size for viewport to avoid mismatches that can manifest as corner-cropping.
        UINT bbW = clientW;
        UINT bbH = clientH;
        {
            ID3D11Texture2D* back = nullptr;
            if (SUCCEEDED(ow.swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) && back) {
                D3D11_TEXTURE2D_DESC bd{};
                back->GetDesc(&bd);
                bbW = bd.Width;
                bbH = bd.Height;
                back->Release();
            }
        }

        D3D11_VIEWPORT vp{};
        vp.TopLeftX = 0;
        vp.TopLeftY = 0;
        vp.Width = static_cast<float>(bbW);
        vp.Height = static_cast<float>(bbH);
        vp.MinDepth = 0;
        vp.MaxDepth = 1;
        g_d3d.ctx->RSSetViewports(1, &vp);

        float clear[4] = {0.02f, 0.02f, 0.02f, 1.0f};
        g_d3d.ctx->OMSetRenderTargets(1, &ow.rtv, nullptr);
        g_d3d.ctx->ClearRenderTargetView(ow.rtv, clear);

        D3D11_MAPPED_SUBRESOURCE map{};
        if (SUCCEEDED(g_d3d.ctx->Map(g_d3d.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
            auto* c = reinterpret_cast<Constants*>(map.pData);
            const bool usingDd = g_useDesktopDuplication.load(std::memory_order_relaxed);

            // When using the 3-monitor compositor, we already place monitors left-to-right in the
            // wide texture, so the slice order should match the window order.
            c->sliceIndex = static_cast<float>(ow.sliceIndex);
            c->timeSeconds = timeSeconds;
            c->useCapture = (g_captureCopiedFrameCounter.load(std::memory_order_relaxed) > 0) ? 1.0f : 0.0f;
            c->isNv12 = 0.0f;
            c->isBgra = 0.0f;

            // DD composite orientation controls (slice-local flipX in shader).
            c->flipY = usingDd ? 1.0f : 0.0f;

            // Only slice when the captured surface is actually ~3 monitors wide.
            UINT capW = 0;
            {
                std::scoped_lock lk(g_captureMutex);
                capW = g_captureW;
            }
            const UINT outW = static_cast<UINT>(ow.rc.right - ow.rc.left);
            const UINT expectedW = g_haveExpectedMode ? g_expectedWideW : (outW * 3);
            const bool sliceEnabled = capW >= (expectedW - 32);
            c->sliceEnabled = sliceEnabled ? 1.0f : 0.0f;

            // Provide viewport size so PS can compute UV from SV_Position robustly.
            c->invViewW = (clientW > 0) ? (1.0f / static_cast<float>(clientW)) : 0.0f;
            c->invViewH = (clientH > 0) ? (1.0f / static_cast<float>(clientH)) : 0.0f;

            // Temporarily force flips off so we can verify flip controls actually affect the output.
            c->flipX = 0.0f;
            c->flipY = 0.0f;

            if (ow.sliceIndex == 0) {
                g_dbgFlipX.store(c->flipX, std::memory_order_relaxed);
                g_dbgFlipY.store(c->flipY, std::memory_order_relaxed);
            }
            c->pad0 = 0.0f;
            c->pad1 = 0.0f;
            c->pad2 = 0.0f;
            g_d3d.ctx->Unmap(g_d3d.cb, 0);
        }

        g_d3d.ctx->Draw(3, 0);
        if (ow.sliceIndex == 0) {
            LARGE_INTEGER qpcBeforePresent{};
            LARGE_INTEGER qpcAfterPresent{};
            (void)QueryPerformanceCounter(&qpcBeforePresent);
            ow.swapchain->Present(1, 0);
            (void)QueryPerformanceCounter(&qpcAfterPresent);
            presentBlockMsThisFrame += QpcToMs(qpcAfterPresent.QuadPart - qpcBeforePresent.QuadPart);
        } else {
            ow.swapchain->Present(0, 0);
        }

        // Measure copy-to-present latency for the most recent copied frame.
        // We only update this when a new copy was observed (so idle periods don't spike the metric).
        if (ow.sliceIndex == 0) {
            EnsureQpcInit();
            const long long copyQpc = g_lastCopyQpc.load(std::memory_order_relaxed);
            if (copyQpc > 0 && copyQpc != s_lastLatencySeenCopyQpc && g_qpcFreq > 0) {
                LARGE_INTEGER now{};
                if (QueryPerformanceCounter(&now)) {
                    const long long dt = now.QuadPart - copyQpc;
                    double us = (static_cast<double>(dt) * 1000000.0) / static_cast<double>(g_qpcFreq);
                    if (us < 0.0) us = 0.0;
                    if (us > 50000.0) us = 50000.0;
                    s_lastCopyToPresentUs = static_cast<float>(us);
                    s_lastLatencySeenCopyQpc = copyQpc;
                }
            }
        }
    }

    LARGE_INTEGER qpcFrameEnd{};
    (void)QueryPerformanceCounter(&qpcFrameEnd);
    const double captureMs = QpcToMs(qpcAfterCapture.QuadPart - qpcFrameStart.QuadPart);
    const double renderMs = QpcToMs(qpcFrameEnd.QuadPart - qpcRenderStart.QuadPart);
    const double totalMs = QpcToMs(qpcFrameEnd.QuadPart - qpcFrameStart.QuadPart);
    s_accCaptureMs += captureMs;
    s_accRenderMs += renderMs;
    s_accTotalMs += totalMs;
    s_accPresentBlockMs += presentBlockMsThisFrame;
    s_accWaitMs += waitMsThisFrame;
    if (presentBlockMsThisFrame > s_maxPresentBlockMs) s_maxPresentBlockMs = presentBlockMsThisFrame;
    if (waitMsThisFrame > s_maxWaitMs) s_maxWaitMs = waitMsThisFrame;
    if (totalMs > s_maxTotalMs) s_maxTotalMs = totalMs;
    s_accFrameCount++;

    s_renderFrameCounter++;

    ID3D11ShaderResourceView* nullSrv = nullptr;
    g_d3d.ctx->PSSetShaderResources(0, 1, &nullSrv);
    if (srvLocal) srvLocal->Release();
}

HWND CreateOutputWindow(const RECT& rc, int sliceIndex) {
    constexpr DWORD style = WS_POPUP;
    constexpr DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;

    HWND hwnd = CreateWindowExW(
        exStyle,
        L"RJ_SURROUND_OUTPUT",
        L"rj_surround_output",
        style,
        rc.left,
        rc.top,
        rc.right - rc.left,
        rc.bottom - rc.top,
        nullptr,
        nullptr,
        g_hInstance,
        nullptr);

    if (!hwnd) return nullptr;

    // Layered+transparent+noactivate yields a click-through, non-focus-stealing overlay.
    // We still draw into it with DXGI flip-model swapchains.
    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);

    SetWindowPos(hwnd, HWND_TOPMOST, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);

    // Prevent recursive capture: hide these output windows from screen capture APIs.
    SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
    return hwnd;
}

bool StartTakeover() {
    if (g_running) return true;

    auto mons = GetMonitorsSortedLeftToRight();
    if (mons.size() < 3) {
        MessageBoxW(nullptr, L"Need at least 3 monitors enabled.", L"rj_surround", MB_OK | MB_ICONERROR);
        return false;
    }

    mons.resize(3);

    g_activeMons[0] = mons[0];
    g_activeMons[1] = mons[1];
    g_activeMons[2] = mons[2];
    g_haveActiveMons = true;

    if (!InitD3D()) {
        DestroyD3D();
        return false;
    }

    g_outputs.clear();
    g_outputs.resize(3);
    for (int i = 0; i < 3; i++) {
        g_outputs[i].rc = mons[i].rc;
        g_outputs[i].sliceIndex = i;
        g_outputs[i].hwnd = CreateOutputWindow(mons[i].rc, i);
        if (!g_outputs[i].hwnd) {
            MessageBoxW(nullptr, L"Failed to create output window.", L"rj_surround", MB_OK | MB_ICONERROR);
            DestroyOutputs();
            DestroyD3D();
            return false;
        }
    }

    for (auto& ow : g_outputs) {
        if (!CreateSwapchainForWindow(ow)) {
            DestroyOutputs();
            DestroyD3D();
            return false;
        }
    }

    // Primary runtime mode: Desktop Duplication per monitor -> composite to wide -> slice to 3 windows.
    const MonitorDesc monArr[3] = {mons[0], mons[1], mons[2]};
    if (!StartDesktopDuplicationForMonitors(monArr)) {
        MessageBoxW(nullptr, L"Failed to start Desktop Duplication.", L"rj_surround", MB_OK | MB_ICONERROR);
        StopTakeover();
        return false;
    }

    {
        UINT wideW = 0, wideH = 0, hz = 0;
        g_haveExpectedMode = TryDeriveExpectedTripleWideModeFromMonitors(monArr, wideW, wideH, hz);
        if (g_haveExpectedMode) {
            g_expectedWideW = wideW;
            g_expectedWideH = wideH;
            g_expectedHz = hz;
        } else {
            g_expectedWideW = 0;
            g_expectedWideH = 0;
            g_expectedHz = 0;
        }
    }

    g_running = true;
    return true;
}

void StopTakeover() {
    if (!g_running) return;
    g_running = false;
    g_haveActiveMons = false;
    StopCapture();
    DestroyOutputs();
    DestroyD3D();
}

LRESULT CALLBACK OutputWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_NCHITTEST:
            // Make the overlay click-through.
            return HTTRANSPARENT;
        case WM_MOUSEACTIVATE:
            // Ensure the overlay never steals focus.
            return MA_NOACTIVATE;
        case WM_CLOSE:
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xFFF0) == SC_CLOSE) return 0;
            break;
        case WM_DISPLAYCHANGE:
            StopTakeover();
            break;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_HOTKEY: {
            if (wParam == kHotkeyToggle) {
                if (g_running) StopTakeover();
                else StartTakeover();
                return 0;
            }
            if (wParam == kHotkeyEmergencyStop) {
                StopTakeover();
                return 0;
            }
            if (wParam == kHotkeyExit) {
                StopTakeover();
                PostQuitMessage(0);
                return 0;
            }
            break;
        }
        case WM_DESTROY:
            StopTakeover();
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool RegisterWindowClasses() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = g_hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);

    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = L"RJ_SURROUND_HIDDEN";
    if (!RegisterClassExW(&wc)) return false;

    wc.lpfnWndProc = OutputWndProc;
    wc.lpszClassName = L"RJ_SURROUND_OUTPUT";
    if (!RegisterClassExW(&wc)) return false;

    return true;
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    g_hInstance = hInstance;

    // Enable per-monitor DPI awareness to avoid DPI virtualization scaling/cropping our full-screen windows.
    {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32) {
            using Fn = BOOL(WINAPI*)(HANDLE);
            auto fn = reinterpret_cast<Fn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
            if (fn) {
                fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            }
        }
    }

    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    // Simple debug console so we can see capture state without attaching a debugger.
    if (AllocConsole()) {
        FILE* fp = nullptr;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
        g_consoleReady = true;
        SetConsoleTitleW(L"rj_surround debug");
        printf("rj_surround debug console\n");
        printf("Hotkeys: Ctrl+Alt+S toggle, Ctrl+Alt+Q stop, Ctrl+Alt+X exit\n");
    }

    // Request permission for programmatic capture. Without this, Windows may provide placeholder frames.
    try {
        auto access = winrt::Windows::Graphics::Capture::GraphicsCaptureAccess::RequestAccessAsync(
            winrt::Windows::Graphics::Capture::GraphicsCaptureAccessKind::Programmatic)
                          .get();
        g_captureAccessAllowed.store(
            access == winrt::Windows::Security::Authorization::AppCapabilityAccess::AppCapabilityAccessStatus::Allowed);
    } catch (...) {
        g_captureAccessAllowed.store(false);
    }

    if (!RegisterWindowClasses()) {
        MessageBoxW(nullptr, L"Failed to register window classes.", L"rj_surround", MB_OK | MB_ICONERROR);
        return 1;
    }

    g_hiddenHwnd = CreateWindowExW(
        0,
        L"RJ_SURROUND_HIDDEN",
        L"rj_surround",
        WS_OVERLAPPED,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        nullptr,
        nullptr,
        g_hInstance,
        nullptr);

    if (!g_hiddenHwnd) {
        MessageBoxW(nullptr, L"Failed to create hidden window.", L"rj_surround", MB_OK | MB_ICONERROR);
        return 1;
    }

    if (!RegisterHotKey(g_hiddenHwnd, kHotkeyToggle, MOD_CONTROL | MOD_ALT, 'S')) {
        MessageBoxW(nullptr, L"Failed to register Ctrl+Alt+S hotkey.", L"rj_surround", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (!RegisterHotKey(g_hiddenHwnd, kHotkeyEmergencyStop, MOD_CONTROL | MOD_ALT, 'Q')) {
        MessageBoxW(nullptr, L"Failed to register Ctrl+Alt+Q hotkey.", L"rj_surround", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (!RegisterHotKey(g_hiddenHwnd, kHotkeyExit, MOD_CONTROL | MOD_ALT, 'X')) {
        MessageBoxW(nullptr, L"Failed to register Ctrl+Alt+X hotkey.", L"rj_surround", MB_OK | MB_ICONERROR);
        return 1;
    }

    MSG msg{};
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                UnregisterHotKey(g_hiddenHwnd, kHotkeyToggle);
                UnregisterHotKey(g_hiddenHwnd, kHotkeyEmergencyStop);
                UnregisterHotKey(g_hiddenHwnd, kHotkeyExit);
                return static_cast<int>(msg.wParam);
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        RenderFrame();
        Sleep(g_running ? 0 : 10);
    }
}
