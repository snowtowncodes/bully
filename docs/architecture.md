# Bully Renderer Wrapper Platform - Architecture v0.2

Status: active (2026-08-14) - Track: proxy-owned multi-backend renderer

## 1. Goal

Modern rendering (Vulkan now, D3D12 when viable) and graphics mods **on top of the existing native PC game**
(Bully: Scholarship Edition, Steam AppID 12200, v154). Decompilation only where the
renderer demands it — not the lead strategy.

## 2. Established facts (verified on this machine)

### Game
| Fact | Value |
|---|---|
| Working copy | `C:\Users\Snowtown\Projects\bully\Bully Scholarship Edition` |
| Steam install | `E:\SteamLibrary\steamapps\common\Bully Scholarship Edition` |
| Bully.exe (local) | 8,204,288 B · PE timestamp 2008-12-10 (original 2008 build) |
| Bully.exe (Steam) | 8.5 MB re-signed variant |
| Static imports | DDRAW, DINPUT8, d3dx9_38, XINPUT1_3, vcomp, msvcr71-family |
| D3D9 loading | **Dynamic**: `LoadLibrary("D3D9.DLL")` + `GetProcAddress("Direct3DCreate9")` (string pair at .rdata 0x59acb8) |
| PE | ImageBase 0x400000, EP 0x460e31, RELOCS_STRIPPED, no ASLR, unpacked, 4 sections |

### Renderer identity (critical)
Bully PC does **not** render through RenderWare. String census of Bully.exe:
`NiDX9Renderer` ×40, `NiD3D` ×80, `Gamebryo` ×12, `NiMaterial` ×14,
`RenderWare`/`rwcore`/`RpWorld` **×0**.

The PC renderer is **Gamebryo (Emergent) `NiDX9Renderer`** — the Scholarship Edition
lineage (Xbox 360 "Xenon" port by Mad Doc; shader fragments are literally titled
"Emergent Xenon Default Vertex Shader Fragments"). The Rockstar data formats
(`timecyc*.dat`, `GRIDS.DAT`, `TXD`, `DAT`, `act`) are game-layer assets carried over
from the PS2/RW world; they are consumed by Rockstar's game code but rasterized by
Gamebryo/D3D9.

Implications:
- Interception target is Gamebryo's device creation (`NiDX9Renderer::Initialize`,
  strings: `Create D3D9 instance...SUCCESSFUL/FAILED`, device types `HAL/REF/SW`,
  vertex modes `SWVP/MIXVP/HWVP/MT/PURE`).
- Gamebryo's D3D9 usage is a **documented** quantity (OpenMW / NIF community,
  Gamebryo SDK docs, niftools) — far better than RE-ing an unknown RW driver.
- Shader pipeline is Gamebryo's: `ShaderBinaries/{High,Med,Low,Off}/*.fxb`
  (compiled shader libraries per quality tier; `Off` = fixed-function fallback) and
  `Data\Shaders\Data\Fragments\Text\NDL_{VS,PS}Fragments.txt` (fragment sources).

### Toolchain
- MSVC 18.7.2 Community (`C:\Program Files\Microsoft Visual Studio\18\Community`)
- Python 3.12.9 (+ pefile), git 2.53, CMake 4.4, ninja (miniconda)
- No Ghidra/IDA installed yet
- Web research must go through hound-web MCP (opencode web search is disabled)

## 3. Chosen strategy

**One proxy DLL with native, DXVK, and experimental D3D9On12 backends.**

```
Bully.exe
  └─ LoadLibrary("D3D9.DLL")            ← resolves to OUR proxy (exe-dir search order)
       └─ our d3d9.dll (proxy)
            ├─ exports the system D3D9 surface
            └─ Direct3DCreate9 hook selects:
                 native → system d3d9.dll
                 dxvk   → sibling dxvk_d3d9.dll → Vulkan
                 on12   → Direct3DCreate9On12 → D3D12 (experimental)
  └─ NiDX9Renderer::Initialize → CreateDevice(...) → D3D9 commands
                 our IDirect3D9/Device9 wrappers retain telemetry and hook points
```

- **Injection**: drop `d3d9.dll` in the game folder (standard wrapper technique;
  no exe patching required for the renderer hook). ASI loader (ThirteenAG pattern)
  comes later for mod loading.
- **Opt-in translated backend path**: `backend=dxvk` absolute-loads an x86
  DXVK `d3d9.dll` renamed to `dxvk_d3d9.dll` beside `Bully.exe`. The module
  remains loaded for process lifetime so returned COM objects retain valid
  code.
- **D3D12 research door**: On12 still exposes `IDirect3DDevice9On12`, but its
  visible presentation failure must be fixed before native D3D12 enhancement
  passes are credible.
- **Failure behavior**: DXVK load/create failure and On12 creation failure both
  fall directly back to native D3D9 and record the reason in the proxy log.

## 4. Milestones

- **M0 — Recon and evidence boundary (done, this doc)**: game located, renderer
  identified as Gamebryo NiDX9Renderer, dynamic d3d9 load path found, and the
  limits of visible-output evidence documented.
- **M1 — Proxy skeleton (done)**: forwarding proxy and wrapped D3D9 traffic
  surface.
- **M2 — Backend evidence and qualification (active)**: native is the
  dependency-free control and default. The retained
  `20260814-175037-pid44752-dxvk-se-none_pi-none_od-i` run is historical and
  environment-specific visible DXVK proof. Later direct and proxy DXVK runs
  produced blank-white selected `main-window` captures; the later proxy runs
  retained varied pre-Present backbuffers. Current visible DXVK qualification
  is unresolved, so DXVK remains opt-in qualification work rather than a
  release claim.

### Next critical work

1. **Evidence integrity**: preserve authoritative reports, backend logs, capture
   targets, and contamination decisions.
2. **Bounded DXVK diagnosis**: isolate the white-window condition with matched,
   minimal cases; do not generalize from API or backbuffer success.
3. **Proxy contract**: define and verify supported load, fallback, wrapper,
   cleanup, and evidence behavior.
4. **Native gameplay smoke/campaign**: establish the native control across startup,
   smoke coverage, and a bounded campaign path.
5. **Optional DXVK qualification**: revisit only after the preceding evidence and
   contract gates pass.

On12, traffic profiling, and mod-platform work remain deferred; they are not
current next steps.

### Current verification status (2026-08-14)

Native remains the dependency-free default and control. The retained
`20260814-175037-pid44752-dxvk-se-none_pi-none_od-i` run is historical and
environment-specific visible DXVK proof: its proxy and DXVK records show
chainloading, successful presentation calls, and selected nonblank
`main-window` captures. Later direct and proxy DXVK runs produced blank-white
selected `main-window` captures; the later proxy runs also recorded varied
pre-Present backbuffers. Current visible DXVK qualification is unresolved, and
DXVK remains opt-in qualification work rather than a release claim. See the
[DXVK evidence ledger](dxvk_evidence_ledger.md) for the authoritative
classification. On12 remains parked.

The first mod vertical slice uses `mods.test_marker=1` at the existing device
`Present` boundary. Run `20260814-211207-pid31140-native-se-none_pi-none_od-i`
logged `ColorFill` success and its captured native backbuffer visibly contains
the magenta marker over the Bully title screen. This proves the marker was
applied to the native backbuffer, not that it reached the visible window. The
active-window PNGs from that run were rejected because the desktop capture
included a PowerShell terminal; no qualifying visible-marker proof is retained.

## 5. Risks & open questions

1. **DXVK packaging and support boundary**: the historical integration needs the
   x86 DXVK `d3d9.dll` renamed to `dxvk_d3d9.dll`. DXVK is zlib-licensed and must
   remain the final D3D9 implementation in the chain; the renamed load works in
   this project but is not an upstream-supported chainloading configuration.
2. **9On12 completeness for Gamebryo**: Gamebryo 2008-era D3D9 uses fixed-function
   fallbacks (`ShaderBinaries\Off`), state blocks, `D3DPOOL_MANAGED`, and possibly
   SW vertex processing. 9On12 is "complete and relatively performant" per MS but
   game-specific gaps are plausible. This work is deferred pending a new
   compatibility lead.
3. **Device creation parameters**: `NiDX9Renderer` may request specific
   D3DCREATE_* flags (PUREDEVICE, MULTITHREADED) that 9On12 rejects. We can sanitize
   flags in our CreateDevice passthrough hook if needed.
4. **d3dx9_38**: unaffected — CPU-side math/util; system DLL provides it.
5. **ddraw import**: Bully statically imports DDRAW.dll; need to confirm what for
   (cursor/window mgmt?). Our proxy only needs d3d9; ddraw continues to resolve
   natively. Verify at M1.
6. **Shader translation**: 9On12 converts SM1-3 shaders to SM4+ internally.
   Gamebryo `.fxb` libraries are pre-compiled bytecode — 9On12 handles the
   resulting D3D9 shaders; recompiling `.fxb` is out of scope while mod work is
   deferred.

## 6. References

- microsoft/D3D9On12 — https://github.com/microsoft/D3D9On12 (open source 2021; DDI mapping layer; build notes in README)
- doitsujin/dxvk — https://github.com/doitsujin/dxvk (DXVK 3.0.2 used in the archived x86 D3D9-to-Vulkan evidence)
- Direct3DCreate9On12 spec — https://microsoft.github.io/DirectX-Specs/d3d/TranslationLayerResourceInterop.html
- elishacloud/dxwrapper (MIT) — proxy/ASI/detours reference — https://github.com/elishacloud/dxwrapper
- CookiePLMonster/SilentPatchBully (MIT) — pattern-scan hook reference for this exe — https://github.com/CookiePLMonster/SilentPatchBully
- aap/rwd3d9 — RW-family D3D9 notes (PS2-era cousins only) — https://github.com/aap/rwd3d9
- reGTA / librw (re3 mirrors) — RenderWare reference, NOT our PC renderer — https://github.com/gmh5225/reGTA
- PCGamingWiki Bully — https://www.pcgamingwiki.com/wiki/Bully:_Scholarship_Edition
- Razcoina/Bully-SE-scripts — Lua script decomp (game layer, useful later) — https://github.com/Razcoina/Bully-SE-scripts

## 7. Repository layout

```
bully/
  docs/                architecture, RE notes
  tools/               python tooling (pe_scan.py, format parsers…)
  src/                 wrapper + mod platform code (C++)
  dump/                generated analysis artifacts (gitignored)
  Bully Scholarship Edition/   game copy (gitignored, untouched by default)
```
