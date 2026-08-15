# Bully Renderer / Mod Platform

Modern rendering and graphics mods on top of the native PC game
(Bully: Scholarship Edition, Steam).

Strategy: a `d3d9.dll` proxy in the game folder intercepts Gamebryo's
`Direct3DCreate9`, keeps the interception surface for future mods, and selects a
native D3D9, DXVK, or experimental D3D9On12 backend at runtime.

Current status: `backend=dxvk` is the first verified translated path. Our proxy
chainloads an x86 DXVK DLL renamed to `dxvk_d3d9.dll`; Bully renders visibly
through Vulkan while the proxy's D3D9 wrappers and telemetry remain active.
Native is still the dependency-free default. The On12 path is parked because it
creates a D3D12-backed device and varied backbuffer but a white visible window.

See [docs/architecture.md](docs/architecture.md) for the full plan.

## Layout
- `docs/` — architecture + RE notes
- `tools/` — python tooling (`tools/pe_scan.py`, format parsers)
- `src/` — C++ wrapper/mod platform
- `dump/` — generated analysis artifacts (gitignored)
- `Bully Scholarship Edition/` — local game copy (gitignored)

## Quick recon
```
python tools/pe_scan.py sections "Bully Scholarship Edition/Bully.exe"
python tools/pe_scan.py imports  "Bully Scholarship Edition/Bully.exe"
python tools/pe_scan.py strings  "Bully Scholarship Edition/Bully.exe" NiDX9Renderer D3D9.DLL
```
