# GPL_SCREEN

## Problem: `rj_span` exits when GPL starts

When launching GPL (or other fullscreen / display-mode-changing apps), `rj_span` previously could stop showing capture and/or exit.

The debug console/log would show messages like:

```
[rj_span] DD: AcquireNextFrame[0] failed hr=0x887A0026
[rj_span] DD: AcquireNextFrame[1] failed hr=0x887A0026
[rj_span] DD: AcquireNextFrame[2] failed hr=0x887A0026
```

## Root cause: Desktop Duplication `DXGI_ERROR_ACCESS_LOST`

`0x887A0026` is `DXGI_ERROR_ACCESS_LOST`.

This is a normal Desktop Duplication failure mode: the OS invalidates the existing `IDXGIOutputDuplication` objects when the display pipeline changes.

Common triggers:

- Launching a game that switches into exclusive fullscreen
- Resolution / refresh-rate change
- Display reconfiguration (outputs enabled/disabled)
- GPU driver reset / TDR

In these cases, **Desktop Duplication must be recreated**.

## What `rj_span` does now

When `RenderFrame()` sees `DXGI_ERROR_ACCESS_LOST` from any `AcquireNextFrame()` call, it:

- Detects the access-lost condition
- Recreates Desktop Duplication for the same 3 monitors (left/middle/right)
- Continues rendering instead of exiting

Implementation notes:

- The selected monitors are cached when takeover starts:
  - `g_activeMons[3]`
  - `g_haveActiveMons`
- On access lost, `RenderFrame()` restarts DD via:
  - `StartDesktopDuplicationForMonitors(g_activeMons)`

If restart fails, DD is disabled so the app stays alive.

## What you should expect in practice

- It can still log `DXGI_ERROR_ACCESS_LOST` right when GPL starts.
- After that, `rj_span` should recover and keep running.

If it does not recover:

- Check if Windows is changing monitor topology/resolution at launch.
- Confirm all 3 monitors remain enabled.
- Consider testing GPL in borderless windowed mode (fewer display-mode transitions).

## Related logging

`rj_span` prints a 1Hz debug line that includes the capture backend and latency, e.g.:

```
[rj_span] backend=DD mode=slice Latency(uS)=... ...
```

The DD error lines above are emitted only when `AcquireNextFrame()` fails and the HRESULT changes (to avoid log spam).
