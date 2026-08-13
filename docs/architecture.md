# Bully DX12 Wrapper Platform — Architecture v0.1

Status: draft (2026-08-12) · Track: DX12 wrapper first (user-approved)

## 1. Goal

Modern rendering (DX12) and graphics mods **on top of the existing native PC game**
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

**Proxy DLL + Microsoft D3D9On12 (open source).**

```
Bully.exe
  └─ LoadLibrary("D3D9.DLL")            ← resolves to OUR proxy (exe-dir search order)
       └─ our d3d9.dll (proxy)
            ├─ exports Direct3DCreate9 (+ the handful of D3D9 exports we must pass through)
            └─ Direct3DCreate9 hook:
                 Create our D3D12 device + queues
                 Direct3DCreate9On12(SDKVersion, overrides)   ← OS d3d9on12.dll
                 return IDirect3D9* (9On12-backed)
  └─ NiDX9Renderer::Initialize → CreateDevice(...) → D3D9 commands
                 D3D9 runtime validates → D3D9 DDI → D3D9On12 → D3D12 driver
```

- **Injection**: drop `d3d9.dll` in the game folder (standard wrapper technique;
  no exe patching required for the renderer hook). ASI loader (ThirteenAG pattern)
  comes later for mod loading.
- **Enhancement door**: the 9On12 device exposes `IDirect3DDevice9On12`, giving us
  lightweight D3D9↔D3D12 interop — this is where post-processing / extra passes /
  mod rendering will hook in later (M3+).
- **Fallbacks if 9On12 is incomplete for Gamebryo**: (a) build our own D3D9On12 from
  source (MS permits local builds; needs WDK + D3D12TranslationLayer + DxbcSigner
  NuGet + WinPixEventRuntime.dll) and fix gaps; (b) DXVK (D3D9→Vulkan) as a
  proven-completeness reference or interim backend.

## 4. Milestones

- **M0 — Recon (done, this doc)**: game located, renderer identified as Gamebryo
  NiDX9Renderer, dynamic d3d9 load path found, strategy fixed.
- **M1 — Proxy skeleton**: minimal `d3d9.dll` forwarding proxy; log which D3D9
  exports Bully touches at startup; verify game still boots through proxy.
- **M2 — D3D9On12 redirect**: hook `Direct3DCreate9` → `Direct3DCreate9On12`;
  first successful in-game frame rendered via DX12 (user-visible: game runs).
- **M3 — Traffic profile**: capture the full D3D9 feature surface the game uses
  (render states, FVF, shader models, state blocks, queries, SWVP usage). Gate for
  deciding stock-9On12 vs custom build vs dxvk.
- **M4 — Enhancement passes**: first DX12-native pass via `IDirect3DDevice9On12`
  interop (e.g. tone map / post FX), then graphics-mod API.
- **M5 — Mod platform**: ASI loader integration, mod SDK, config system.

## 5. Risks & open questions

1. **9On12 completeness for Gamebryo**: Gamebryo 2008-era D3D9 uses fixed-function
   fallbacks (`ShaderBinaries\Off`), state blocks, `D3DPOOL_MANAGED`, and possibly
   SW vertex processing. 9On12 is "complete and relatively performant" per MS but
   game-specific gaps are plausible. Mitigation: M3 traffic profile, custom build.
2. **Device creation parameters**: `NiDX9Renderer` may request specific
   D3DCREATE_* flags (PUREDEVICE, MULTITHREADED) that 9On12 rejects. We can sanitize
   flags in our CreateDevice passthrough hook if needed.
3. **d3dx9_38**: unaffected — CPU-side math/util; system DLL provides it.
4. **ddraw import**: Bully statically imports DDRAW.dll; need to confirm what for
   (cursor/window mgmt?). Our proxy only needs d3d9; ddraw continues to resolve
   natively. Verify at M1.
5. **Shader translation**: 9On12 converts SM1-3 shaders to SM4+ internally.
   Gamebryo `.fxb` libraries are pre-compiled bytecode — 9On12 handles the
   resulting D3D9 shaders; recompiling `.fxb` is out of scope until M5 (mod API).

## 6. References

- microsoft/D3D9On12 — https://github.com/microsoft/D3D9On12 (open source 2021; DDI mapping layer; build notes in README)
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
