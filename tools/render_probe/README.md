# Bully Renderer Probe

`run.ps1` is a bounded, Windows-only runtime verification harness for the local Bully renderer proxy. Before a normal run mutates game files or starts the game, it requires a no-mutation display preflight: an interactive desktop, no remote-session indicator, a non-virtual primary display with valid bounds, and a harmless 1x1 desktop capture. It stages the generated proxy only while a run is active, captures the game window without foregrounding it when possible, evaluates deterministic pixel metrics, writes a self-contained report, and restores staged files in `finally`.

The harness uses only PowerShell and Windows/.NET APIs. It does not install packages, edit source files, build the proxy, or make network requests.

## Usage

From the repository root, use Windows PowerShell or PowerShell 7 on Windows:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\render_probe\run.ps1
powershell -ExecutionPolicy Bypass -File .\tools\render_probe\run.ps1 -Backend native
powershell -ExecutionPolicy Bypass -File .\tools\render_probe\run.ps1 -DurationSeconds 60 -CaptureAtSeconds 5,20,45
powershell -ExecutionPolicy Bypass -File .\tools\render_probe\run.ps1 -Backend on12 -On12Device explicit -ForceSwapEffect copy -ForcePresentInterval one
powershell -ExecutionPolicy Bypass -File .\tools\render_probe\run.ps1 -ValidateOnly -CaptureAtSeconds 5,15,30
powershell -ExecutionPolicy Bypass -File .\tools\render_probe\run.ps1 -ValidateHistory
powershell -ExecutionPolicy Bypass -File .\tools\render_probe\run.ps1 -ValidateDisplayOnly
powershell -ExecutionPolicy Bypass -File .\tools\render_probe\run.ps1 -ValidateIniOnly -On12Device explicit -ForceSwapEffect flip -ForcePresentInterval immediate
```

## Active Console Bridge

`run_active.ps1` is a separate wrapper for a controller that is running in session 0, WinDisc, or another noninteractive desktop. It resolves the active physical console session, verifies that its `explorer.exe` belongs to the same SID as the controller, then creates one short-lived scheduled task using that SID with interactive logon and the normal user token. The task runs `run.ps1` in the unlocked console desktop; `run.ps1` remains the only script that stages or restores game files.

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\render_probe\run_active.ps1 -ValidateBridgeOnly
powershell -ExecutionPolicy Bypass -File .\tools\render_probe\run_active.ps1 -ValidateDisplayOnly
powershell -ExecutionPolicy Bypass -File .\tools\render_probe\run_active.ps1 -Backend native -DurationSeconds 60 -CaptureAtSeconds 5,20,45 -AllowActiveDesktopLaunch
powershell -ExecutionPolicy Bypass -File .\tools\render_probe\run_active.ps1 -Backend on12 -On12Device explicit -ForceSwapEffect copy -ForcePresentInterval one -DurationSeconds 60 -CaptureAtSeconds 5,20,45 -AllowActiveDesktopLaunch
```

The bridge intentionally launches a runtime probe into the currently unlocked physical console desktop. Every non-validation invocation requires `-AllowActiveDesktopLaunch`; without it, the wrapper refuses to create a task or start the game. `-ValidateOnly`, `-ValidateHistory`, and `-ValidateDisplayOnly` remain no-launch modes and do not need that acknowledgement. A runtime invocation first runs the existing `run.ps1 -ValidateDisplayOnly` preflight inside the console session before it can invoke the runtime harness.

Each bridge invocation leaves its request, result, controller cleanup record, and child stdout/stderr under `dump/render-probe/bridge/<guid>/`. Runtime bridge metadata includes the normal `report.json` path parsed from that exact child output; it never guesses from the newest dump directory. The task name is unique and starts with `BullyRenderProbeActive-`. The wrapper unregisters only its exact task in `finally`. If a controller is forcibly interrupted, inspect tasks with that recognizable prefix and manually unregister the specific orphaned task name after confirming it belongs to the interrupted invocation. The task has a bounded execution limit and a disabled expiry trigger: it cannot later start by itself, while Task Scheduler has an expiry path for stale registrations.

`-ValidateBridgeOnly` creates no task and does not invoke the game or `run.ps1`; it validates argument normalization, per-invocation task-name construction, PowerShell parsing of the controller/worker/launcher, and the active-console interop helper compilation.

| Parameter | Default | Meaning |
| --- | --- | --- |
| `-Backend` | `native` | Renderer INI backend: `native` or explicitly requested `on12`. |
| `-On12Device` | `internal` | Explicit `[renderer] on12_device`: `internal` or `explicit`. Applied only when the harness stages its INI. |
| `-ForceSwapEffect` | `none` | Explicit `[renderer] force_swap_effect`: `none`, `discard`, `flip`, or `copy`. Applied only when the harness stages its INI. |
| `-ForcePresentInterval` | `none` | Explicit `[renderer] force_present_interval`: `none`, `immediate`, `default`, or `one`. Applied only when the harness stages its INI. |
| `-DurationSeconds` | `40` | Bounded runtime, from 1 to 3600 seconds. The harness terminates its own process after a successful timeout observation. |
| `-CaptureAtSeconds` | `5,15,30` | Non-negative integer capture times. Commas, semicolons, and whitespace separate values; comma-separated input works through both `powershell.exe -File` and direct PowerShell invocation. Times after the duration are recorded as unscheduled. |
| `-NoInstall` | off | Do not copy the built proxy or write an INI. Use the game folder as it already exists. |
| `-ForceInstall` | off | Permit replacement of a game-folder `d3d9.dll` that is not known to be a prior project-generated proxy. |
| `-ValidateOnly` | off | Normalize and print capture times plus the selected backend/on12-device/swap-effect/present-interval controls, then exit before creating a run directory, staging files, inspecting displays, or launching the game. |
| `-ValidateHistory` | off | Exercise proxy hash-history loading/appending/persistence against a temporary fixture, then exit. It never changes the real `proxy-hash-history.json`. |
| `-ValidateDisplayOnly` | off | Compile the helper, run the no-mutation display/capture preflight, print JSON, and exit. It never creates a run directory, stages game files, or starts the game. It returns nonzero when the host is not safe for visual capture. |
| `-ValidateIniOnly` | off | Generate the requested renderer INI into a temporary fixture under `tools/render_probe`, validate exactly one effective `backend`, `on12_device`, `force_swap_effect`, and `force_present_interval` key in `[renderer]`, print JSON, remove the fixture, and exit. It does not require an interactive display. |
| `-AllowVirtualDisplay` | off | Override a failed display preflight for diagnostic-only runs. The report marks host screen captures untrustworthy, and in-process BMP artifacts cannot establish a successful visual run. |

Do not expect `-Backend`, `-On12Device`, `-ForceSwapEffect`, or `-ForcePresentInterval` to change the renderer with `-NoInstall`: that option deliberately leaves the local game INI unchanged. The report records the requested values and `not-applied-no-install` rather than claiming they affected the active INI.

`-CaptureAtSeconds 5,15,30` is the recommended form for command-line use. Quote whitespace or semicolon-delimited forms so the invoking shell passes them as one argument, for example `-CaptureAtSeconds '5 15 30'` or `-CaptureAtSeconds '5;15;30'`. Invalid, out-of-range, or negative values fail before the launcher creates runtime output or starts `Bully.exe`.

`-ValidateHistory` is a no-launch, no-game-mutation check. By default it creates and removes a temporary fixture inside `tools/render_probe`; `-HistoryValidationPath` can target a separate fixture in that same directory for controlled tests. It refuses to use the real `proxy-hash-history.json` as a validation fixture.

`-ValidateIniOnly` is a no-launch, no-game-mutation check for renderer controls. It writes the generated requested INI only to a temporary fixture under `tools/render_probe`, verifies the four effective `[renderer]` keys, prints the fixture content as JSON, and removes the fixture before exit. It is safe to run under `WinDisc` or another noninteractive host.

## Display Preflight

Normal runs stop with `preflight-failed` before staging the proxy/INI or launching `Bully.exe` when the host is not safe for desktop capture. The checks record `Environment.UserInteractive` and the input desktop, `SM_REMOTESESSION` and `SESSIONNAME`, WinForms/native monitor and display-device names/bounds, virtual/disconnected names such as `WinDisc`, and a real in-memory 1x1 `Graphics.CopyFromScreen` probe. Arbitrary physical monitor resolutions are accepted; the harness does not expect a particular desktop size.

Use `-ValidateDisplayOnly` to inspect a host without touching the game. `-AllowVirtualDisplay` is deliberately diagnostic-only: it allows collection of process, proxy-log, and in-process front/backbuffer evidence, but records every failed capability and makes screen PNGs ineligible to prove a successful visual run.

## Installation Safety

Without `-NoInstall`, the launcher requires `build/proxy/Release/d3d9.dll`, `src/proxy/bully_d3d9proxy.ini`, and `Bully Scholarship Edition/Bully.exe`.

It records SHA-256 hashes before staging `d3d9.dll` and `bully_d3d9proxy.ini`, creates verified backups in the run report directory, and restores them in `finally`. The requested INI is produced with a section-aware line updater: it preserves comments and other sections, removes existing active renderer keys only inside `[renderer]`, and emits exactly one active `backend`, `on12_device`, `force_swap_effect`, and `force_present_interval` key. `proxy-hash-history.json`, ignored by Git, is a local allowlist of generated proxy SHA-256 values observed on prior runs.

An existing game-folder `d3d9.dll` is replaced only if it matches the current generated build or a hash in that local history. An unknown DLL causes the run to fail before mutation unless `-ForceInstall` is explicitly supplied. Cleanup restores a staged file only if it still matches the probe-installed hash; this avoids overwriting a file changed by another process during the run.

`-ForceInstall` is intentionally narrow. Use it only after confirming that the existing DLL is safe to replace. The original is backed up before staging.

## Captures And Interpretation

At each requested time the launcher resolves a visible, unowned top-level window for the launched `Bully.exe`, records its handle/title/rectangle, and tries:

1. `PrintWindow(PW_RENDERFULLCONTENT)` into a PNG.
2. `Graphics.CopyFromScreen` of the same rectangle if `PrintWindow` fails or its PNG is blank/low-information.

Neither API is asked to activate or focus the game. If no suitable game window exists, it captures the actual primary monitor rectangle and marks the record as `primary-monitor-fallback`. A fallback screenshot may include other desktop content, so treat it as a liveness/visual diagnostic rather than proof that the game itself rendered. When the host capability probe or a runtime screen copy fails due to unavailable desktop capture, the capture record uses `host-capture-unavailable` rather than a generic no-image result.

Every written PNG gets deterministic metrics: dimensions, mean/min/max RGB, luminance mean/variance/standard deviation, a 16-bin luminance histogram, 8x8x8 coarse-color occupancy, and near-white/near-black ratios. Thresholds are visible at the top of `run.ps1`:

- `blank-white`: at least 99.5% of pixels have every channel >= 245.
- `blank-black`: at least 99.5% of pixels have every channel <= 10.
- `low-information-uniform`: very low luminance standard deviation and coarse-color diversity.
- `nonblank`: everything else.

The optional proxy-created `bully_renderprobe_backbuffer.bmp` and `bully_renderprobe_frontbuffer.bmp` are copied and analyzed separately when they changed during the run. Frontbuffer readback is opt-in and disabled by default because it can interfere with presentation. Both artifacts include the same deterministic metrics, classification, hashes, and pre/post provenance in the report. They are diagnostic evidence only and do not satisfy the success requirement for window/desktop PNG captures.

## Output And Exit Status

Each invocation creates:

```text
dump/render-probe/<timestamp>-pid<id>-<backend>-se-<swap>_pi-<interval>_od-<device>/
  report.json
  summary.txt
  run.log
  captures/
  artifacts/
  state/
  wer-application-errors.json
```

The run ID retains the backend and adds concise renderer metadata, for example `...-on12-se-copy_pi-one_od-e` for `on12_device=explicit`, so A/B artifacts remain distinguishable without reading the report first.

`artifacts/bully_d3d9proxy.log` is the primary per-run proxy-log archive. The launcher records the pre-run byte length/SHA-256, then after the process terminates verifies that the post-run log preserves that exact prefix and archives only appended bytes. The report and summary include archive path, `[start,end)` byte offsets, archived length, pre/post/archive hashes, prefix verification, and extraction method/reason. When the log is missing, truncated, replaced, or its prefix changes, it archives the full post-run log instead and records why. A valid append-only case also preserves `artifacts/bully_d3d9proxy.full.log` as secondary evidence. The game-folder log is never truncated, renamed, or deleted.

The JSON and summary record requested backend/on12-device/swap-effect/present-interval controls, the requested INI artifact SHA-256 and effective keys, and whether the controls were applied to the active INI. They also record installation hashes and restoration status, process liveness observations, capture methods and metrics, front/backbuffer artifact hashes and classifications, proxy-log pre/post lengths plus extraction method, session/remote status, device names/bounds, preflight checks and errors, override/trust flags, display state before launch/after termination/after cleanup, restore decision/result, exit code, WER records, and known crash signatures. It specifically recognizes Application Error evidence for `Bully.exe`, exception `0xc0000005`, fault offset `0x3487DB`.

The launcher returns zero only when the launched process was alive at the duration deadline and at least one selected PNG was classified `nonblank`. It returns nonzero for early exit/crash evidence, no analyzable captures, or when all analyzable captures are blank or low-information. A process is killed after a successful deadline observation to keep the test bounded; that is recorded as `terminatedByHarness`, not classified as an early crash.

## Display Recovery

The primary display bounds and current display mode are recorded before launch and after termination. Cleanup compares primary position, bounds, size, and mode. When unchanged, it records `not-required-resolution-unchanged` and does not call `ChangeDisplaySettings`. It only attempts `ChangeDisplaySettings(NULL, 0)` after an actual launched process demonstrably changed display geometry/mode. When a snapshot is unavailable, it skips the API rather than blindly changing the desktop. The decision and any result remain in the report.
