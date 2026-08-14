# Architecture

## Pattern Overview

**Overall:** DLL Proxy Injection with D3D9-to-D3D12 Translation

**Key Characteristics:**
- Intercepts Gamebryo's D3D9 renderer via DLL search-order hijacking
- Routes D3D9 commands through Microsoft's D3D9On12 translation layer
- Provides transparent D3D12 backend without game modification
- Exposes interop surface for future enhancement passes

## Layers

**Game Layer (Bully.exe):**
- Purpose: Original 2008 game executable with Gamebryo NiDX9Renderer
- Location: `Bully Scholarship Edition/Bully.exe` (gitignored)
- Contains: Rockstar game logic, Gamebryo scene graph, asset loaders
- Depends on: Dynamic `LoadLibrary("D3D9.DLL")` resolution
- Used by: Not modified; consumes proxy d3d9.dll via DLL search order

**Proxy Layer:**
- Purpose: Intercept D3D9 creation, redirect to D3D9On12, log device traffic
- Location: `src/proxy/proxy.cpp`, `src/proxy/exports.def`
- Contains: Export forwarding, Direct3DCreate9 hook, IDirect3DDevice9/IDirect3D9/IDirect3DSwapChain9 wrappers
- Depends on: System d3d9.dll (loaded from `%SystemRoot%\System32`), d3d12.lib, dxgi.lib
- Used by: Game via LoadLibrary resolution (exe-directory search before system32)

**Translation Layer (D3D9On12):**
- Purpose: Translate D3D9 commands to D3D12 at runtime
- Location: System `d3d9.dll` with `Direct3DCreate9On12` export (Windows built-in)
- Contains: Microsoft's D3D9On12 implementation (open-source reference in `deps/D3D9On12/`)
- Depends on: D3D12 driver, DXGI
- Used by: Proxy when backend=on12

**Configuration Layer:**
- Purpose: Runtime behavior selection via INI
- Location: `bully_d3d9proxy.ini` (sibling to Bully.exe)
- Contains: Backend selection (native/on12), device mode (internal/explicit), presentation overrides, diagnostics
- Depends on: Win32 GetPrivateProfileString API
- Used by: Proxy initialization

**Verification Layer:**
- Purpose: Automated visual testing with desktop capture and pixel metrics
- Location: `tools/render_probe/run.ps1`, `tools/render_probe/run_active.ps1`
- Contains: PowerShell harness, display preflight, capture scheduling, pixel classification, report generation
- Depends on: .NET WinForms for desktop capture, Windows Task Scheduler for session-0 bridge
- Used by: Development workflow for A/B testing backends

## Data Flow

**Initialization (on12 backend, internal device mode):**

1. Game calls `LoadLibrary("D3D9.DLL")` → resolves to proxy in exe directory — `src/proxy/proxy.cpp` DllMain
2. Proxy loads real system d3d9.dll via absolute path `%SystemRoot%\System32\d3d9.dll` — `InitRealD3D9Once()`
3. Game calls `GetProcAddress("Direct3DCreate9")` → resolves to `proxy_Direct3DCreate9` — `src/proxy/exports.def`
4. Proxy reads INI → backend=on12, on12_device=internal — `ReadRendererBackend()`, `ReadOn12DeviceMode()`
5. Proxy calls system `Direct3DCreate9On12(SDKVersion, nullptr, 0)` with internal device — 9On12 creates D3D12 device internally
6. Proxy wraps returned IDirect3D9* in `ProxyIDirect3D9` for CreateDevice interception — `src/proxy/proxy.cpp:2199-2209`
7. Game enumerates adapters, formats → forwarded to 9On12 enumerator

**Device Creation:**

1. Game calls `IDirect3D9::CreateDevice(...)` → `ProxyIDirect3D9::CreateDevice()` — `src/proxy/proxy.cpp:1989-2076`
2. Proxy applies presentation parameter overrides (swap effect, present interval) if configured — `ApplyPresentationOverrides()`
3. Proxy forwards to inner 9On12 IDirect3D9::CreateDevice
4. Proxy wraps returned IDirect3DDevice9* in `ProxyIDirect3DDevice9` for Present/method telemetry — device9_methods.generated.inc
5. Proxy queries `IDirect3DDevice9On12` interface → verifies D3D12 backend active — `VerifyDeviceBackend()`
6. Proxy logs D3D12 device identity, feature level (observable), device removed reason — `LogVerifiedOn12RuntimeIdentity()`

**Rendering Loop:**

1. Game issues D3D9 state/draw calls → forwarded through `ProxyIDirect3DDevice9` vtable (119 methods) — `device9_methods.generated.inc`
2. 9On12 translates to D3D12 command lists + PSO changes
3. Game calls `IDirect3DDevice9::Present()` or `IDirect3DSwapChain9::Present()` → logged by proxy wrappers — `ProxyIDirect3DSwapChain9::Present()`
4. 9On12 executes D3D12 command queue, presents via DXGI swap chain
5. At configured frame (default 60), proxy captures backbuffer before Present and frontbuffer after Present → writes `bully_renderprobe_backbuffer.bmp`, `bully_renderprobe_frontbuffer.bmp`

**Initialization (on12 backend, explicit device mode):**

1. Steps 1-4 same as internal mode
2. Proxy creates explicit D3D12 device + command queue via DXGI adapter enumeration — `CreateExplicitOn12Context()`
3. Proxy matches DXGI adapter to native D3D9 adapter's monitor via `GetAdapterMonitor()` — `GetNativeD3D9AdapterMonitor()`, `AdapterHasOutputMonitor()`
4. Proxy calls `Direct3DCreate9On12(SDKVersion, &args, 1)` with explicit device/queue — args.pD3D12Device, args.ppD3D12Queues
5. Steps 6-7 same as internal mode; explicit D3D12 device is verified against supplied device

**Initialization (native backend):**

1. Steps 1-3 same as on12
2. Proxy reads backend=native from INI
3. Proxy calls system `Direct3DCreate9(SDKVersion)` directly — no 9On12 translation
4. Game uses native D3D9 driver → no D3D12 involvement

## Key Abstractions

**ProxyIDirect3D9:**
- Purpose: Wrap IDirect3D9 enumerator to intercept CreateDevice
- Location: `src/proxy/proxy.cpp:1885-2084`
- Pattern: COM wrapper with IUnknown + IDirect3D9 forwarding, special-case CreateDevice hook

**ProxyIDirect3DDevice9:**
- Purpose: Wrap IDirect3DDevice9 to log traffic and capture diagnostic frames
- Location: `src/proxy/proxy.cpp:1041-1883` (class) + `src/proxy/device9_methods.generated.inc` (116 forwarding methods, vtable slots 3-118)
- Pattern: COM wrapper with generated vtable forwarding via Python codegen (`src/proxy/generate_device9_methods.py`)

**ProxyIDirect3DSwapChain9:**
- Purpose: Wrap IDirect3DSwapChain9 to log Present calls and track frame count
- Location: `src/proxy/proxy.cpp:945-1039`
- Pattern: COM wrapper with Present telemetry

**ExplicitOn12Context:**
- Purpose: RAII container for proxy-owned D3D12 device + command queue in explicit mode
- Location: `src/proxy/proxy.cpp:385-403`
- Pattern: Non-copyable RAII with Release() cleanup

**DeviceDiagnosticsConfig:**
- Purpose: Structured INI configuration for tracing, capture, and overrides
- Location: `src/proxy/proxy.cpp:215-225`
- Pattern: POD struct populated from INI via GetPrivateProfileString/Int

## Entry Points

**proxy_Direct3DCreate9:**
- Location: `src/proxy/proxy.cpp:2113-2212` (callable via export at offset in exports.def)
- Triggers: Game's `GetProcAddress("Direct3DCreate9")` → game's call
- Responsibilities: Read INI backend, create D3D9 enumerator (native or on12 internal/explicit), wrap in ProxyIDirect3D9, log backend identity

**DllMain:**
- Location: `src/proxy/proxy.cpp:2215-2221`
- Triggers: LoadLibrary, FreeLibrary
- Responsibilities: No-op; all initialization deferred to lazy INIT_ONCE in EnsureRealD3D9Loaded

**Generate Device Methods Script:**
- Location: `src/proxy/generate_device9_methods.py:162-190` (main function)
- Triggers: Developer invocation (not build-time)
- Responsibilities: Emit 116-method forwarding block (vtable slots 3-118) to `device9_methods.generated.inc` matching IDirect3DDevice9 vtable order

**Render Probe Harness:**
- Location: `tools/render_probe/run.ps1:1-1800+`
- Triggers: Developer invocation for visual verification
- Responsibilities: Display preflight, proxy/INI staging, game launch, timed window capture, pixel metrics, report generation, restore

## Error Handling

**Strategy:** Fail-fast for critical proxy setup, log and forward for D3D9 API HRESULTs

- Missing system d3d9.dll exports → `FailFastMissingExport()` with `__fastfail(FAST_FAIL_FATAL_APP_EXIT)`
- Direct3DCreate9On12 unavailable when backend=on12 → log error, fall back to native Direct3DCreate9, return native enumerator
- Explicit D3D12 device creation failure → log detailed HRESULT, fall back to internal mode, return 9On12 enumerator with internal device
- D3D9 API call failures (CreateDevice, Present, etc.) → log HRESULT, forward to caller, no proxy-side error recovery
- Device removed → logged via `GetDeviceRemovedReason()` at post-verify and final device release, not trapped
- Log file open failure → silently no-op in `Log()`, no crash

## Cross-Cutting Concerns

**Logging:**
- Append-only file `bully_d3d9proxy.log` next to Bully.exe
- Thread-safe via CRITICAL_SECTION + per-call lock
- Lazy initialization via INIT_ONCE
- Render probe archives log delta per run, detects truncation/replacement

**Device Traffic Tracing:**
- IDirect3DDevice9 method entry/exit logged when `trace_device=1` (default)
- CreateDevice, Reset, Present, SwapChain creation logged with parameters + HRESULTs
- First 8 Present calls logged unconditionally, then only failures

**Frame Capture:**
- Pre-Present backbuffer copied to system-memory surface when `capture_frames=1` and frame == `capture_frame`
- Post-Present frontbuffer retrieved via GetFrontBufferData when `capture_frontbuffer=1`
- Both written as BMP to exe directory: `bully_renderprobe_backbuffer.bmp`, `bully_renderprobe_frontbuffer.bmp`
- Render probe copies, hashes, and classifies these artifacts (blank-white/blank-black/low-information-uniform/nonblank)

**Backend Verification:**
- After CreateDevice, proxy queries `IDirect3DDevice9On12` interface
- If successful, queries underlying `ID3D12Device` via `GetD3D12Device()`
- Logs verified D3D12 backend identity, d3d9.dll path, device pointer
- In explicit mode, compares returned D3D12 device against proxy-supplied device for identity match

**Configuration:**
- INI path resolved relative to Bully.exe via `GetModuleFileName(nullptr)` + sibling filename
- Section `[renderer]`: backend, on12_device, force_swap_effect, force_present_interval
- Section `[diagnostics]`: trace_device, capture_frames, capture_frontbuffer, capture_frame, d3d12_debug_layer
- Defaults: backend=native, on12_device=internal, no overrides, trace=1, capture=1, frontbuffer=0, frame=60; On12 is explicit experimental mode

**Render Probe:**
- Display preflight checks: interactive desktop, non-remote session, non-virtual monitor, 1x1 CopyFromScreen probe
- Timed window capture via PrintWindow + CopyFromScreen fallback at configured seconds (default 5,15,30)
- Deterministic pixel metrics: mean/min/max RGB, luminance stats, 16-bin histogram, 8x8x8 color occupancy
- Classification: blank-white (≥99.5% pixels all channels ≥245), blank-black (≥99.5% all channels ≤10), low-information-uniform (low luminance stddev + coarse-color diversity), nonblank
- Exit 0 only when process alive at deadline and ≥1 nonblank capture
- Session-0 bridge via run_active.ps1: schedules task in active console session with user SID, runs run.ps1, collects artifacts
