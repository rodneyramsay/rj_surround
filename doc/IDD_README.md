# IDD_README

Goal: make Windows 11 (and games) see **one large virtual monitor** (target: **7680x1440 @ 120Hz**) without using NVIDIA Surround.

`rj_span` today draws three click-through full-screen windows and shows a composited capture across 3 physical monitors. That does **not** change what resolutions games enumerate. Games learn available fullscreen modes from Windows’ display stack (WDDM/DXGI) and whatever display devices/drivers are present.

To expose a true `7680x1440` mode to games, you need a **virtual display device**. On Windows 11, the supported path is an **Indirect Display Driver (IDD)** implemented via the **IddCx** class extension.

## Recommended base: Microsoft `IddSampleDriver` (IddCx)

Use Microsoft’s official sample as the starting point:

- https://github.com/microsoft/Windows-driver-samples/tree/main/video/IndirectDisplay/IddSampleDriver

Useful entry points:

- Driver logic:
  - https://github.com/microsoft/Windows-driver-samples/blob/main/video/IndirectDisplay/IddSampleDriver/Driver.cpp
- INF:
  - https://github.com/microsoft/Windows-driver-samples/blob/main/video/IndirectDisplay/IddSampleDriver/IddSampleDriver.inf

The customization for this project is:

- Expose **exactly 1** virtual monitor.
- Advertise **7680x1440 @ 120Hz** as a supported mode.
- Give it a clear friendly name (so it’s easy to pick in Display Settings / game settings).

## Tooling prerequisites

Install:

- Visual Studio 2022
- Windows 11 SDK
- Windows Driver Kit (WDK) matching the SDK

After installing WDK, Visual Studio should be able to open/build the sample driver project.

## High-level “software surround” pipeline

1. Game renders to the **virtual** IDD monitor at `7680x1440`.
2. `rj_span` captures that virtual monitor.
3. `rj_span` slices/presents it across the 3 physical monitors.

## Driver customization tasks (what you change in the sample)

### 1) Expose exactly one monitor

The sample can be configured to enumerate multiple monitors. For this project, make it expose **one** target/monitor.

### 2) Add `7680x1440 @ 120Hz` to the mode list

Start minimal:

- `7680x1440 @ 120Hz`

Add more modes later only if necessary. Keeping the list small reduces games choosing unexpected defaults.

### 3) Rename hardware IDs / friendly name

In the INF, change the strings so the device is obvious in:

- Device Manager
- Settings → System → Display

Example: “RJ Virtual Surround 7680x1440”.

## Installing the driver (dev/test)

During development, most people use **test signing**.

High-level steps:

1. Enable test signing.
2. Reboot.
3. Build the driver (x64) in Visual Studio.
4. Install the INF as admin.

Installation methods:

- Device Manager (“Have Disk…”)
- `pnputil` (admin)

Expect reboots while iterating.

## Verify it worked

After installing:

- Settings → System → Display
  - You should see a new display.
  - You should be able to select/confirm `7680x1440`.

Then:

- In a game resolution selector, you should now see `7680x1440`.

## Make games see it as the only monitor

Practical options:

### A) Set the virtual monitor as the Main display

Settings → System → Display → select the virtual monitor → enable “Make this my main display”.

### B) Disable physical monitors temporarily

If you want games to effectively see only the virtual monitor for a session:

- Disable the physical displays in Windows Display settings.
- Leave only the virtual display enabled.

Re-enable the physical monitors afterwards.

## How this integrates with `rj_span` (next coding step)

Once the IDD is installed, update capture selection so `rj_span` captures the **virtual** output instead of the physical monitors.

Current behavior:

- Capture 3 physical monitors via Desktop Duplication
- Composite into 7680x1440
- Slice to 3 output windows

Desired post-IDD behavior:

- Capture 1 virtual 7680x1440 monitor
- Slice directly to 3 output windows

Implementation idea:

- Enumerate `IDXGIAdapter`/`IDXGIOutput`
- Prefer outputs associated with the IDD adapter/device name
- Fall back to the current 3-monitor physical capture path if the virtual monitor is not present

## Links

- Windows driver samples repo:
  - https://github.com/microsoft/Windows-driver-samples
- IddSampleDriver:
  - https://github.com/microsoft/Windows-driver-samples/tree/main/video/IndirectDisplay/IddSampleDriver

## Notes / gotchas

- Driver development is a different workflow than app dev: plan for signing and reboots.
- Keep the mode list minimal until everything is stable.
