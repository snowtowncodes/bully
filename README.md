# Bully Renderer / Mod Platform

Modern rendering and graphics mods on top of the native PC game
(Bully: Scholarship Edition, Steam).

Strategy: a `d3d9.dll` proxy in the game folder intercepts Gamebryo's
`Direct3DCreate9`, keeps the interception surface for future mods, and selects a
native D3D9, DXVK, or experimental D3D9On12 backend at runtime.

Current status (2026-09-16, this machine): native D3D9 is the dependency-free
default and control. Proxy-native visible runtime (`M1-V`) and native smoke
(`N-S`) passed (keyboard/mouse; controller not tested). Proxy-chainloaded DXVK
3.0.2 is operator-visible (game plus DXVK HUD) and completed a two-boot gameplay
smoke. That is not a `D-V` probe capture packet and not a fully-playable or
campaign claim. The retained
`20260814-175037-pid44752-dxvk-se-none_pi-none_od-i` run remains historical.
Later 20260814 blank-white selected `main-window` captures are
environment-specific; a white PNG is not proof of a white screen. On12 stays
parked. See the [DXVK evidence ledger](docs/dxvk_evidence_ledger.md).

The retained graphics-mod marker experiment is an opt-in `mods.test_marker`
applied through the stable device `Present` hook. D3D9 `ColorFill` proves the
marker reached the native in-process backbuffer, but the contaminated
active-window PNGs do not prove visible-window presentation. It remains disabled
by default.

See [docs/architecture.md](docs/architecture.md) for the full plan.
See [docs/playability_test_plan.md](docs/playability_test_plan.md) for the no-launch playability qualification plan.

## Layout
- `docs/` — architecture + RE notes
- `tools/` — python tooling (`tools/pe_scan.py`) and the render probe
- `src/` — C++ wrapper/mod platform
- `dump/` — generated analysis artifacts (gitignored)
- `Bully Scholarship Edition/` — local game copy (gitignored)

## Quick recon
```
python tools/pe_scan.py sections "Bully Scholarship Edition/Bully.exe"
python tools/pe_scan.py imports  "Bully Scholarship Edition/Bully.exe"
python tools/pe_scan.py strings  "Bully Scholarship Edition/Bully.exe" NiDX9Renderer D3D9.DLL
```
