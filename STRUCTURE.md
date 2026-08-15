# Codebase Structure

## Directory Layout

```
bully/
├── src/
│   └── proxy/              # D3D9 proxy DLL (C++)
├── tools/
│   ├── render_probe/       # Visual verification harness (PowerShell)
│   └── pe_scan.py          # PE reconnaissance script
├── docs/
│   ├── architecture.md     # System design and renderer identification
│   └── m2_test_plan.md     # M2 verification procedure
├── build/                  # CMake build outputs (gitignored)
├── deps/                   # Read-only dependency sources (gitignored)
├── dump/                   # Analysis artifacts and test reports (gitignored)
├── Bully Scholarship Edition/  # Local game copy (gitignored)
├── README.md               # Project overview and quick start
└── .gitignore              # Exclusions: game copy, build, deps, dump
```

## Directory Purposes

**src/proxy:**
- Purpose: The d3d9.dll proxy that intercepts game rendering
- Contains: proxy.cpp (main implementation), exports.def (DLL exports), generate_device9_methods.py (codegen), device9_methods.generated.inc (116 IDirect3DDevice9 forwarding methods, vtable slots 3-118), CMakeLists.txt (build), bully_d3d9proxy.ini (runtime config template)
- Key files:
  - `proxy.cpp` — COM wrappers, Direct3DCreate9 hook, native/DXVK/9On12 backend selection, logging, frame capture
  - `exports.def` — 16 d3d9.dll exports mapped to proxy_ symbols
  - `generate_device9_methods.py` — generates device9_methods.generated.inc from IDirect3DDevice9 method list
  - `device9_methods.generated.inc` — 116 methods (TestCooperativeLevel through DrawIndexedPrimitiveUP) — vtable slots 3-118
  - `CMakeLists.txt` — x86-only build, links d3d12.lib + dxgi.lib, outputs d3d9.dll
  - `bully_d3d9proxy.ini` — [renderer] backend (native/dxvk/on12), on12_device/overrides, [diagnostics] trace/capture/frame settings

**tools/render_probe:**
- Purpose: Automated visual verification with desktop capture and pixel analysis
- Contains: run.ps1 (main harness), run_active.ps1 (session-0 bridge), run_dxvk.ps1 (DXVK staging helper), run_active_worker.ps1 (bridge worker), NativeMethods.cs (P/Invoke helpers), proxy-hash-history.json (local allowlist), README.md (usage, safety, interpretation)
- Key files:
  - `run.ps1` — display preflight, proxy/INI staging, game launch, timed capture, pixel metrics, report generation, restore
  - `run_active.ps1` — session-0 to active-console bridge via scheduled task
  - `run_dxvk.ps1` — x86/hash-checked DXVK DLL staging, bridge invocation, manifest, and restoration
  - `README.md` — comprehensive usage guide, parameter reference, preflight checks, exit status, artifact structure

**tools:**
- Purpose: Development utilities
- Contains: pe_scan.py (PE sections/imports/strings/context for Bully.exe), render_probe/ subdirectory
- Key files:
  - `pe_scan.py` — PE reconnaissance: sections with entropy, import DLLs, string search with offsets, raw hex context

**docs:**
- Purpose: Architecture documentation and test plans
- Contains: architecture.md (system design, renderer identification, strategy, milestones), m2_test_plan.md (M2 visual verification procedure)
- Key files:
  - `architecture.md` — goal, verified facts (Gamebryo NiDX9Renderer not RenderWare), proxy + native/DXVK/9On12 strategy, milestones, risks
  - `m2_test_plan.md` — preconditions, controls, test matrix (native/DXVK/On12), required evidence, failure interpretation

**build:**
- Purpose: CMake build outputs
- Contains: build/proxy/ for Win32 proxy build (CMakeCache.txt, .vcxproj, Release/d3d9.dll, Debug/d3d9.dll)
- Exclusion: Gitignored; created by `cmake -B build/proxy -A Win32 src/proxy`

**deps:**
- Purpose: Read-only dependency sources and third-party binaries for local inspection/testing
- Contains: D3D9On12/ (Microsoft's open-source reference implementation), dxvk-3.0.2/ (official archive extraction)
- Exclusion: Gitignored; dependencies are not built or bundled by the project

**dump:**
- Purpose: Generated analysis artifacts and test reports
- Contains: render-probe/<timestamp>-pid<id>-<backend>-se-<swap>_pi-<interval>_od-<device>/ (report.json, summary.txt, run.log, captures/, artifacts/, state/, wer-application-errors.json)
- Exclusion: Gitignored; each render probe run creates timestamped directory with full evidence

**Bully Scholarship Edition:**
- Purpose: Local game copy (Steam AppID 12200, v154)
- Contains: Bully.exe (8.2 MB, PE timestamp 2008-12-10), d3d9.dll (staged proxy), optional dxvk_d3d9.dll (renamed x86 DXVK), bully_d3d9proxy.ini (staged config), bully_d3d9proxy.log (append-only runtime log), game assets (Scripts/, Models/, Shaders/, ShaderBinaries/, TXD/, DAT/, Stream/, etc.)
- Exclusion: Gitignored; untouched by default; render probe stages/restores proxy + INI, while DXVK dependencies must be staged separately

## Key File Locations

**Entry Points:**
- `src/proxy/proxy.cpp` — proxy_Direct3DCreate9 (main interception), DllMain (no-op), ProxyIDirect3D9/ProxyIDirect3DDevice9/ProxyIDirect3DSwapChain9 (COM wrappers)
- `tools/render_probe/run.ps1` — param block line 1, main workflow starts ~line 800

**Configuration:**
- `src/proxy/bully_d3d9proxy.ini` — template INI with defaults (backend=native; optional dxvk/on12, on12_device=internal, trace_device=1, capture_frames=1, capture_frontbuffer=0, capture_frame=60, mods.test_marker=0)
- `src/proxy/CMakeLists.txt` — x86-only build, d3d12 + dxgi link, /DEF exports.def

**Core Logic:**
- `src/proxy/proxy.cpp` lines 1-823 — logging, INI parsing, D3D12 device creation (explicit mode), backend verification
- `src/proxy/proxy.cpp` lines 825-985 — export forwarding trampolines (asm naked functions)
- `src/proxy/proxy.cpp` lines 993-1087 — ProxyIDirect3DSwapChain9 (Present telemetry)
- `src/proxy/proxy.cpp` lines 1089-1931 — ProxyIDirect3DDevice9 (traffic logging, frame capture)
- `src/proxy/proxy.cpp` lines 1933-2132 — ProxyIDirect3D9 (CreateDevice interception)
- `src/proxy/device9_methods.generated.inc` — 116 forwarding method implementations (vtable slots 3-118)

**Tests:**
- `tools/render_probe/run.ps1` — automated visual verification
- `docs/m2_test_plan.md` — manual test matrix (native control, DXVK chainload, on12 internal/explicit, presentation variants)

**Documentation:**
- `README.md` — project overview, strategy summary, layout, quick recon commands
- `docs/architecture.md` — detailed system design, renderer identity (Gamebryo vs RenderWare), strategy justification, milestones
- `docs/m2_test_plan.md` — M2 verification plan with preconditions, controls, evidence requirements
- `tools/render_probe/README.md` — comprehensive probe usage, display preflight, installation safety, capture interpretation

## Naming Conventions

**Files:**
- C++ sources: `proxy.cpp` (monolithic implementation)
- Generated includes: `*.generated.inc` (codegen output, committed)
- Python scripts: `snake_case.py` (pe_scan.py, generate_device9_methods.py)
- PowerShell scripts: `PascalCase.ps1` or `snake_case.ps1` (run.ps1, run_active.ps1, run_active_worker.ps1)
- Configuration: `bully_d3d9proxy.ini`, `bully_d3d9proxy.log`
- Documentation: `snake_case.md` or `PascalCase.md`

**Directories:**
- Source modules: `lowercase/` (proxy/)
- Tools: `snake_case/` (render_probe/)
- Build/output: `lowercase/` (build/, dump/)

**Symbols:**
- Exported functions: `proxy_<D3D9ExportName>` (proxy_Direct3DCreate9, proxy_Direct3DCreate9Ex)
- COM wrappers: `Proxy<InterfaceName>` (ProxyIDirect3D9, ProxyIDirect3DDevice9, ProxyIDirect3DSwapChain9)
- Global state: `g_<name>` (g_realD3D9, g_Direct3DCreate9, g_log)
- Static functions: `PascalCase` (EnsureRealD3D9Loaded, ReadRendererBackend, VerifyDeviceBackend)
- Enums: `PascalCase` (RendererBackend, On12DeviceMode, D3D12DebugLayerMode)

## Where to Add New Code

**New proxy feature (logging, interception, diagnostics):**
- Location: `src/proxy/proxy.cpp` — add to appropriate section (config parsing ~lines 210-313, backend logic ~lines 428-815, wrapper classes ~lines 993-2132)
- Pattern: Follow existing INI config + wrapper method pattern
- Register: Update DeviceDiagnosticsConfig struct if adding INI key, ReadDeviceDiagnosticsConfig() for parsing

**New IDirect3DDevice9 method interception (beyond forwarding):**
- Location: `src/proxy/proxy.cpp` ProxyIDirect3DDevice9 class (~lines 1089-1931)
- Pattern: Override specific method in class definition, add special-case logic, call m_inner->Method() to forward
  - Example: Intercept_Present() already overridden for frame capture at lines 1244-1284

**New D3D9 export forwarding:**
- Location: `src/proxy/exports.def` — add export mapping
- Location: `src/proxy/proxy.cpp` — add FARPROC g_<ExportName>, GetProcAddress in InitRealD3D9Once(), naked trampoline function
  - Pattern: Follow existing export pattern (lines 825-985)

**New verification test:**
- Location: `tools/render_probe/run.ps1` — call from new PowerShell script or add to existing harness
- Pattern: Copy run.ps1 param block, invoke with -Backend/-On12Device/-ForceSwapEffect/-ForcePresentInterval
- Document: Add to `docs/m2_test_plan.md` test matrix

**New reconnaissance tool:**
- Location: `tools/<tool_name>.py` or `tools/<tool_name>/`
- Pattern: Follow pe_scan.py structure (argparse, subcommands, pefile dependency)
- Document: Add usage to tool docstring and reference in README.md

**New analysis script:**
- Location: `tools/render_probe/<script>.ps1` for probe-related, `tools/<script>.py` for general
- Pattern: Self-contained with inline usage doc
- Output: Write to `dump/` subdirectory

**Configuration change:**
- Location: `src/proxy/bully_d3d9proxy.ini` — edit template
- Code: Update DeviceDiagnosticsConfig struct and ReadDeviceDiagnosticsConfig() in proxy.cpp
- Document: Update `docs/m2_test_plan.md` controls table

**Enhancement pass (future M4):**
- Location: New `src/enhancements/` directory for D3D12-native passes
- Pattern: For a future D3D12 pass, access the On12 device via IDirect3DDevice9On12::GetD3D12Device(), submit extra command lists
- Integrate: Hook in ProxyIDirect3DDevice9::Present() before/after inner Present

**Mod platform (future M5):**
- Location: New `src/loader/` for ASI loader, `src/sdk/` for mod SDK headers
- Pattern: ThirteenAG ASI loader pattern, scan for .asi files, LoadLibrary + GetProcAddress("InitMod")
- Integrate: Call from proxy DllMain or lazy-init in first Direct3DCreate9

**Documentation update:**
- Location: `docs/architecture.md` for design changes, `docs/<milestone>_test_plan.md` for new test plans
- Pattern: Follow existing structure (facts, strategy, milestones, risks)
- Sync: Keep README.md layout section in sync with actual directory structure
