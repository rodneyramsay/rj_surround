# rj_span

Win32 + D3D11 prototype that captures the **primary desktop** using **Windows Graphics Capture (WGC)** and displays it across **three full-screen borderless windows** (one per monitor), each window showing a different horizontal “slice” of the captured frame.

This repository is intentionally small (single `.cpp` + CMake) so that capture/render issues can be debugged quickly.

## What it does

- Creates a hidden Win32 window used for:
  - Global hotkeys
  - Message pump
- Enumerates monitors and selects the **left-to-right first 3 monitors**.
- Creates 3 borderless topmost windows (`WS_POPUP`) positioned to cover each monitor.
- Creates a D3D11 device + context and a swapchain per output window.
- Starts WGC monitor capture for the **primary monitor**.
- Each render frame:
  - Copies the latest captured texture into an internal shader-readable BGRA texture
  - Draws a full-screen triangle into each output window
  - The pixel shader samples a slice of the captured texture based on `sliceIndex` (0/1/2)

## Why this exists / key design decisions

### 1) D3D11 everywhere (simple and compatible)
This project uses **Direct3D 11** because:

- WGC surfaces can be accessed as `ID3D11Texture2D` via `IDirect3DDxgiInterfaceAccess`.
- D3D11 is widely supported and easy to set up with CMake.
- A “single full-screen triangle” pipeline avoids vertex buffers and is minimal.

### 2) “Copy then sample” capture pipeline
WGC gives you a GPU texture that is owned by the frame pool. To render reliably and avoid lifetime hazards, the code:

- Stores the most recent WGC `ID3D11Texture2D` in `g_latestFrameTex`.
- Allocates its own `g_captureTex` (`DXGI_FORMAT_B8G8R8A8_UNORM`, `D3D11_BIND_SHADER_RESOURCE`).
- Copies (`CopyResource`) from the latest WGC texture into `g_captureTex`.

This makes the render path consistent: the pixel shader always samples a texture we own.

### 3) BGRA sampling / channel swizzle
On Windows, capture textures are commonly BGRA (`DXGI_FORMAT_B8G8R8A8_UNORM`). Depending on how the shader interprets channels, you can see “wrong colors”.

The current pixel shader includes a simple swizzle:

```hlsl
float4 c = capTex.Sample(capSamp, uv);
if (isBgra > 0.5) c = c.bgra;
return c;
```

The constant `isBgra` is currently set to `1.0f` in `RenderFrame()`.

### 4) Debug-first: 1×1 readback + format logging
The hardest issue we’re investigating is when WGC returns **placeholder frames** (e.g. solid green/gold, or a constant tinted frame).

To diagnose that, the app logs once per second:

- Whether programmatic capture access was granted
- How many frames arrived and how many were copied
- Source and owned DXGI formats
- A 1×1 pixel sample (`px`) from the sampled texture

This is useful to prove whether the capture content is changing.

## Build

### Requirements
- Windows 10/11
- Visual Studio 2022 (or Build Tools)
- CMake 3.20+
- A GPU + driver supporting D3D11

### CMake configure + build
From the repository root:

```powershell
cmake -S . -B build
cmake --build build --config Release
```

The executable will be at:

- `build\Release\rj_span.exe` (multi-config generators like Visual Studio)

## Run

Run the executable and use the global hotkeys.

### Hotkeys
- `Ctrl+Alt+S`
  - Toggle start/stop (“takeover”) mode
- `Ctrl+Alt+Q`
  - Emergency stop (stop takeover)
- `Ctrl+Alt+X`
  - Exit

A console window is allocated at startup and prints debug info.

## How it works (code walkthrough)

### High-level state
Key globals (simplified):

```cpp
std::vector<OutputWindow> g_outputs;   // 3 monitor windows + swapchains
D3DState g_d3d;                        // device, context, shaders

std::mutex g_captureMutex;
winrt::com_ptr<ID3D11Texture2D> g_latestFrameTex; // last WGC frame texture
ID3D11Texture2D* g_captureTex;                   // owned copy (BGRA)
ID3D11ShaderResourceView* g_captureSrv;          // SRV for shader sampling
```

### Startup (`wWinMain`)
- Initializes WinRT apartment (`multi_threaded`).
- Allocates a debug console.
- Requests permission for **programmatic capture**:

```cpp
auto access = GraphicsCaptureAccess::RequestAccessAsync(
    GraphicsCaptureAccessKind::Programmatic).get();

g_captureAccessAllowed.store(access == AppCapabilityAccessStatus::Allowed);
```

**Decision:** The app requests access up-front because without it, Windows may produce placeholder frames.

### Takeover mode (`StartTakeover`)
When you press `Ctrl+Alt+S`, the app:

- Enumerates monitors via `EnumDisplayMonitors`
- Picks the first 3 left-to-right
- Initializes D3D11 (`InitD3D()`)
- Creates 3 output windows + swapchains
- Starts capture (`StartCapturePrimary()`)

### Capturing the primary monitor (`StartCapturePrimary`)
The capture target is created via `IGraphicsCaptureItemInterop::CreateForMonitor`:

```cpp
HMONITOR primary = MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
auto interopFactory = get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
interopFactory->CreateForMonitor(primary, __uuidof(IGraphicsCaptureItem), put_abi(item));
```

Then a free-threaded frame pool is created and `FrameArrived` stores the latest texture:

```cpp
g_frameArrivedToken = g_framePool.FrameArrived([&](auto const& sender, auto const&) {
    auto frame = sender.TryGetNextFrame();
    auto surface = frame.Surface();

    com_ptr<IDirect3DDxgiInterfaceAccess> access;
    surface.as(access);

    ID3D11Texture2D* texRaw = nullptr;
    access->GetInterface(__uuidof(ID3D11Texture2D), (void**)&texRaw);

    com_ptr<ID3D11Texture2D> tex;
    tex.attach(texRaw);

    std::scoped_lock lk(g_captureMutex);
    g_latestFrameTex = tex;
});
```

### Rendering (`RenderFrame`)
`RenderFrame()` runs continuously in the main loop.

1) If a new capture frame is available, it allocates `g_captureTex` if needed and copies:

```cpp
if (g_captureTex) {
    g_d3d.ctx->CopyResource(g_captureTex, src.get());
}
```

2) It binds the capture SRV and renders each output window:

```cpp
for (auto& ow : g_outputs) {
    // set viewport, set RTV
    // update constants (sliceIndex)
    g_d3d.ctx->Draw(3, 0);
    ow.swapchain->Present(1, 0);
}
```

3) The pixel shader maps each monitor window to a different third of the captured image:

```hlsl
float2 uv = float2((i.uv.x + sliceIndex) / 3.0, i.uv.y);
float4 c = capTex.Sample(capSamp, uv);
```

### Cleanup (`StopTakeover` / `StopCapture`)
- `StopTakeover()` stops capture, destroys windows/swapchains, and releases D3D resources.
- `StopCapture()` releases SRVs/textures and resets capture counters.

## Known limitations / current investigation

- **Capture target is the primary monitor only.**
  - The long-term plan is to target a *virtual ultra-wide monitor* (e.g. via an Indirect Display Driver).
- **WGC may return placeholder frames** (observed as solid/tinted green/gold output).
  - The debug log’s `px` and `arrived/copied` counters are used to confirm.
- **Desktop Duplication fallback is stubbed** (`StartDesktopDuplicationOutput0`) but not fully integrated.
  - The intended next step is to switch to DXGI Desktop Duplication when WGC appears unusable.

## Troubleshooting

### I only see solid green/gold
- Ensure the console log shows `access=1`.
- Look at `px=`:
  - If `px` barely changes over time, WGC is likely not returning real desktop content.

### Hotkeys don’t work
- Another app may be using the same hotkeys.
- Run as admin only if you suspect global hotkey registration is blocked by policy.

### Only 1 or 2 monitors show output
- The app currently requires **at least 3 enabled monitors**.
- It takes the first 3 monitors when sorted left-to-right.

## Project layout

- `rj_span.cpp`
  - Entire app (Win32 + D3D11 + WGC)
- `CMakeLists.txt`
  - Minimal build configuration

## License

No license has been specified yet. If you intend to share this publicly, add a license file.
