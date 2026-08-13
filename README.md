# Bully DX12 Wrapper Platform

Modern rendering (DX12) and graphics mods on top of the native PC game
(Bully: Scholarship Edition, Steam).

Strategy: a `d3d9.dll` proxy in the game folder intercepts Gamebryo's
`Direct3DCreate9` and redirects device creation to Microsoft's open-source
**D3D9On12** translation layer, running the game's D3D9 on our D3D12 device.
Enhancement passes hook in later via `IDirect3DDevice9On12` interop.

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
