# M2 Renderer Proxy Verification Plan

This is the operational plan for proving visible Bully PC rendering through the
current D3D9 proxy. It is a visual verification plan, not a game-file editing
procedure. Sources: [render probe](../tools/render_probe/run.ps1),
[probe usage and safety notes](../tools/render_probe/README.md),
[proxy diagnostics](../src/proxy/proxy.cpp), and
[default proxy INI](../src/proxy/bully_d3d9proxy.ini).

## Preconditions

1. Run from the active physical user console session only. First run:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\tools\render_probe\run.ps1 -ValidateDisplayOnly
   ```

   It must exit zero with `preflight.status=passed`. It checks an interactive
   desktop, non-remote session, non-virtual display with valid bounds, and a
   1x1 desktop capture.

2. The current agent context is session 0 with `WinDisc`; it is not a valid
   visual host. A session-1 bridge is required. The bridge must run the build
   and probe in the active user console session and return the command exit
   code plus generated artifacts. Do not use `-AllowVirtualDisplay` to claim a
   visual pass.

3. Build the x86 proxy from the repository root:

   ```powershell
   cmake -B build/proxy -A Win32 src/proxy
   cmake --build build/proxy --config Release
   ```

   The expected staged DLL is `build/proxy/Release/d3d9.dll`. Let the harness
   stage and restore it; do not manually edit files under
   `Bully Scholarship Edition/`.

## Current Controls

| INI key | Values | Notes |
| --- | --- | --- |
| `renderer.backend` | `native`, `on12` | Backend selection. |
| `renderer.on12_device` | `internal`, `explicit` | Used for `on12`; `internal` is the default. |
| `renderer.force_swap_effect` | `none`, `discard`, `flip`, `copy` | `none` preserves the game's requested value. |
| `renderer.force_present_interval` | `none`, `immediate`, `default`, `one` | `none` preserves the game's requested value. |
| `diagnostics.trace_device` | `0`, `1` | Device-call trace logging. |
| `diagnostics.capture_frames` | `0`, `1` | Enables the pre-Present backbuffer BMP. |
| `diagnostics.capture_frontbuffer` | `0`, `1` | Opt-in post-Present readback; default `0` because it can interfere with presentation. |
| `diagnostics.capture_frame` | non-negative frame number | Current default is `60`. |

The probe writes the four renderer controls into a staged INI. Do not use
`-NoInstall` for an A/B case because requested controls are then not applied.

## Test Matrix

Use 35 seconds and captures at 5, 15, and 30 seconds for every listed case.
Run cases in this order and stop on a stop condition below.

1. Native control:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\tools\render_probe\run.ps1 -Backend native -DurationSeconds 35 -CaptureAtSeconds 5,15,30
   ```

2. On12 internal control:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\tools\render_probe\run.ps1 -Backend on12 -On12Device internal -DurationSeconds 35 -CaptureAtSeconds 5,15,30
   ```

3. On12 internal presentation variants:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\tools\render_probe\run.ps1 -Backend on12 -On12Device internal -ForceSwapEffect copy -ForcePresentInterval immediate -DurationSeconds 35 -CaptureAtSeconds 5,15,30
   powershell -ExecutionPolicy Bypass -File .\tools\render_probe\run.ps1 -Backend on12 -On12Device internal -ForceSwapEffect flip -ForcePresentInterval immediate -DurationSeconds 35 -CaptureAtSeconds 5,15,30
   powershell -ExecutionPolicy Bypass -File .\tools\render_probe\run.ps1 -Backend on12 -On12Device internal -ForceSwapEffect discard -ForcePresentInterval one -DurationSeconds 35 -CaptureAtSeconds 5,15,30
   ```

4. On12 explicit device cases, first default presentation settings, then COPY:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\tools\render_probe\run.ps1 -Backend on12 -On12Device explicit -DurationSeconds 35 -CaptureAtSeconds 5,15,30
   powershell -ExecutionPolicy Bypass -File .\tools\render_probe\run.ps1 -Backend on12 -On12Device explicit -ForceSwapEffect copy -DurationSeconds 35 -CaptureAtSeconds 5,15,30
   ```

5. Re-run the native control only when proxy source or the built DLL changed
   after the initial native control. Record the changed source/build identity
   in the result row.

## Required Evidence

A visual pass needs all applicable evidence below.

- At least one selected, active game-window PNG after startup is `nonblank`.
  Require `target=main-window`; `primary-monitor-fallback` is diagnostic only.
- `artifacts/bully_d3d9proxy.log` proves the requested/effective backend. For
  On12, retain the `IDirect3DDevice9On12`/D3D12 verification lines; for native,
  retain the native/unverified backend line.
- The proxy log records `CreateDevice` and `Present` HRESULTs. Retain both
  success/failure values and first-Present swap-chain parameters.
- The pre-Present backbuffer artifact is varied/non-uniform rather than a blank
  or uniform image. This shows rendering reached the backbuffer.
- When valid, retain the post-Present frontbuffer result and its
  `GetFrontBufferData` HRESULT as corroborating presentation evidence.

Never count an in-process `bully_renderprobe_backbuffer.bmp` or
`bully_renderprobe_frontbuffer.bmp` by itself as visible output. The qualifying
visual signal is the active-window PNG.

## Failure Interpretation

- A white game HWND with `Present` returning `S_OK` and a varied pre-Present
  backbuffer means rendering reached the presentation boundary but not visible
  output. Investigate swap/present/window presentation, not scene generation.
- In windowed mode, `GetFrontBufferData` needs a monitor/desktop-sized
  `D3DFMT_A8R8G8B8` destination surface. See `CaptureFrontbuffer` in
  `src/proxy/proxy.cpp`; a backbuffer-sized or wrong-format destination is not
  a valid frontbuffer test.
- `Bully.exe` address `0x00407430` is the internal renderer-readiness
  predicate. It is diagnostic context, not a current harness gate. The
  500-quad overflow guard is prepared but unapplied; do not apply it during
  this plan or hand-edit game files to do so.

## Artifacts And Cleanup

Each runtime case writes this run root:

```text
dump/render-probe/<timestamp>-pid<pid>-<backend>-se-<swap>_pi-<interval>_od-<i|e>/
  report.json
  summary.txt
  run.log
  wer-application-errors.json
  captures/capture-0005s-printwindow.png
  captures/capture-0005s-copy-from-screen.png
  captures/capture-0015s-...
  captures/capture-0030s-...
  artifacts/bully_d3d9proxy.log
  artifacts/bully_d3d9proxy.full.log             (append-only log case)
  artifacts/bully_d3d9proxy.active.ini
  artifacts/bully_renderprobe_backbuffer.bmp
  artifacts/bully_renderprobe_frontbuffer.bmp
  state/requested-bully_d3d9proxy.ini
  state/original-d3d9.dll                        (when one existed)
  state/original-bully_d3d9proxy.ini             (when one existed)
```

Use `report.json` as the authoritative record. It includes requested controls,
preflight, capture metrics, process/timeout state, WER evidence, hashes,
archive offsets, and display state. `artifacts/bully_d3d9proxy.log` is the
appended proxy-log range when the prior prefix is intact; otherwise it is the
full post-run log, with the extraction reason recorded in `report.json`.

The harness must restore its staged `Bully Scholarship Edition/d3d9.dll` and
`Bully Scholarship Edition/bully_d3d9proxy.ini` in `finally`. It restores only
when the current file still matches the probe-installed hash, preserving any
outside change. It does not truncate, rename, or delete the game-folder proxy
log. Confirm `installation.status=installed-and-restored` when staging occurred
and confirm display state after cleanup. Do not hand-edit game files outside
the harness.

## Current Result (2026-08-14)

- Current native control: `20260814-094923-pid44584-native-se-none_pi-none_od-i`, proxy SHA-256 `0d7acd...`, `capture_frontbuffer=0`; the process survived 35 seconds and the main-window captures included nonblank output. This is the valid native control for the current proxy wrapper.
- Matched On12 control: `20260814-095119-pid17544-on12-se-none_pi-none_od-i`, the same proxy SHA-256 and controls; all three main-window captures were blank-white. The log verified `IDirect3DDevice9On12`, `ID3D12Device`, `Present=S_OK`, and a nonblank pre-Present backbuffer.
- DXCap artifact: `dump/render-probe/dxcap-manual-20260814-115504/bully-on12-frame60.vsglog` is 4,120 bytes and `dxcap -p -toXML` reports no DirectX activity. The append-only proxy log shows that DXCap's process entered `Direct3DCreate9On12` but stopped before device creation returned, so this capture is non-diagnostic for the normal runtime path.
- Decision: park D3D9On12. Do not add presentation matrices, a custom 9On12 fork, or game patches without a new specific compatibility lead. Native is now the safe default; On12 remains an explicit experimental selection.

## Stop Conditions

Stop the matrix immediately and preserve the run directory when any condition
occurs:

1. Display/session invalid: `-ValidateDisplayOnly` fails, the bridge is not in
   session 1, or a virtual/remote/`WinDisc` display is reported.
2. Early crash: `Bully.exe` exits before 35 seconds, a scheduled capture is
   skipped because it exited, or WER reports matching crash evidence.
3. Unknown DLL replacement: the harness refuses an unrecognized existing
   `Bully Scholarship Edition/d3d9.dll`. Do not use `-ForceInstall` in an
   autonomous run; obtain an explicit ownership decision first.
4. Display restoration failure: a changed display is not restored, display
   cleanup records an error, or the after-cleanup display state is unavailable
   when restoration was required. Do not start another runtime case.

## Results Template

| Case | Build/source identity | Session and preflight | PNG at 5/15/30s | Backend/log proof | CreateDevice/Present | Back/frontbuffer | Result or stop reason |
| --- | --- | --- | --- | --- | --- | --- | --- |
|  |  |  |  |  |  |  |  |
