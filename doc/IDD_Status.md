
# IDD_Status

## Current decision

We are implementing **Path A**:

- Windows sees **one virtual monitor** exposed by an **Indirect Display Driver (IddCx)** at **7680x1440 @ 120Hz**.
- Games/apps render to that virtual monitor normally (fullscreen/borderless).
- `rj_span` captures that **single** virtual output and **slices** it across the 3 physical monitors (3 full-screen borderless windows).

This keeps the “game integration” simple and uses the Windows display stack as intended.

## Repos / locations

- IDD sample base:
  - `Windows-Driver/video/IndirectDisplay/IddSampleDriver`
- App:
  - `src/rj_span.cpp`

## Status

### Completed

- `rj_span` performance/pacing is stable at **7680x1440 @ 120 FPS** (Desktop Duplication path), with instrumentation (wait/capture/render/present).
- Added **Ctrl+Alt+T** test-pattern mode in `rj_span` (`backend=TEST`) to validate slicing/pacing without capture/IDD.
- Verified the 3-monitor slice output looks correct/seamless when driven by the synthetic 7680x1440 source.
- Implemented Option B logic in `rj_span`: detect a single wide monitor when present and capture it via Desktop Duplication; otherwise fall back to 3-monitor DD composite.
- Added DD runtime visibility in the log: `ddmode=single_wide` vs `ddmode=triple_composite`, plus one-time logging of the selected wide monitor device name/mode.
- Verified current setup runs at **120 FPS** in `backend=DD ddmode=triple_composite` (no wide/IDD display detected yet).
- CMake updated to use `src/rj_span.cpp`.
- Documentation moved under `doc/`.

### In progress

- Pivot `rj_span` capture selection to prefer the **IDD virtual output** (single 7680x1440) and slice directly.

### Next milestones

1) IDD: advertise exactly one monitor and only `7680x1440@120`

- In `Driver.cpp`:
  - Set monitor count to 1.
  - In `IddSampleMonitorQueryModes`, return only `7680x1440@120` as a target mode.
  - Ensure the monitor description/default monitor modes also intersect to exactly this mode.

2) INF: rename device/friendly name

- Update the sample INF strings so the virtual display is clearly identifiable (e.g. “RJ Virtual Surround 7680x1440”).

3) Install/test

- Test-signing, install the driver, verify Windows Display Settings shows the new display.
- Verify games enumerate/select `7680x1440`.

4) `rj_span`: capture the virtual output

- Prefer the output associated with the IDD display.
- Fall back to current behavior if the IDD display isn’t present.

## Notes

- IDD swapchain processing (`SwapChainProcessor`) is where the OS-provided frames are consumed by the driver.
- For Path A, we do not need custom IPC from the game to the driver.
- For games that only render to the primary display, the intended approach is to set the IDD virtual display as **Main display** (with the caveat that the primary desktop may be “invisible” if `rj_span` is not running to present it).

## How close are we?

Close on the **app side** (slicing + 120Hz pacing is working and verified). The remaining work is mostly **driver install/bring-up**:

- Once the IDD driver is installed and Windows exposes the virtual `7680x1440@120` display, the core runtime loop becomes:
  - Game renders to the virtual display (ideally set as Main display)
  - `rj_span` captures that single wide output
  - `rj_span` slices to the 3 physical monitors

At this point, we have validated everything except the IDD installation and selecting/capturing that IDD output.

## Next Steps (tomorrow)

1) INF: rename device/friendly name

- Edit `Windows-Driver/video/IndirectDisplay/IddSampleDriver/IddSampleDriver.inf` to use a clear name (e.g. “RJ Virtual Surround 7680x1440”).

2) Build the IDD driver

- Open `Windows-Driver/video/IndirectDisplay/IddSampleDriver.sln` and build the x64 driver.

3) Install (test signing)

- Enable test signing, reboot, install the INF.
- Confirm the device appears correctly in Device Manager / Display Settings.

4) Verify modes

- In Settings → System → Display, confirm you can select `7680x1440` and `120Hz` on the new virtual display.

5) Validate capture path

- Run `rj_span` and ensure it selects the wide output (`backend=DD ddmode=single_wide`).
- Set the IDD virtual display as **Main display** and confirm the game renders there.

6) Tighten selection (optional hardening)

- Replace the current “wide heuristic” (`>=7600x1440`) with explicit IDD identification (device name/adapter/output), once we know the final friendly name and what it reports in DXGI/DisplayConfig.
