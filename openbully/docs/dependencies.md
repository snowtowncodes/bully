# Dependencies

| Library | Version | How vendored | Notes |
|---------|---------|--------------|-------|
| SDL2 | 2.30.12 | `external/SDL2-2.30.12` (prebuilt VC dev package) | Windowing/input/audio device |
| bgfx | **v1.129.8958-499** (via `bgfx.cmake-archived` release, 2025-10-30) | `external/bgfx` (source, gitignored) | Rendering (Direct3D11 pinned on Windows for Phase 2); ships bgfx + bx + bimg |
| bx / bimg | matching bgfx.cmake release | `external/bgfx/bx`, `external/bgfx/bimg` | bgfx support libraries, built together |
| glm | 1.0.1 (header-only) | `external/glm` | Math |

## Audio backend (Track 21)

**Base SDL2 audio only — SDL2_mixer is NOT vendored.** Bully's real audio is
XACT banks (`.bin`/`.rsm`/`.SEA`, see `docs/asset_catalog.md`); decoding those
is deferred, so Track 21 ships procedural SFX + a small in-engine software
mixer built on `SDL_OpenAudioDevice` + the SDL audio callback
(`src/audio/audio.cpp`). SDL2_mixer's compressed-format decode adds nothing
here; revisit when XACT bank decoding starts. User-provided `.wav` files in
`data/audio/` (`footstep.wav` / `click.wav` / `wind.wav`) override the
procedural sounds as the extensibility path.

## bgfx

- Upstream: https://github.com/bkaradzic/bgfx
- The bgfx repo does not tag classic releases; versions are pinned through the
  CMake integration project `bgfx.cmake` (now `bgfx.cmake-archived`). We vendor
  the release asset `bgfx.cmake.v1.129.8958-499.zip`, which contains bgfx, bx,
  bimg and the CMake build scripts in one tree.
- Fetch with `scripts/fetch_bgfx.ps1` (idempotent):
  ```powershell
  powershell -File scripts/fetch_bgfx.ps1
  ```
- Build integration: `add_subdirectory(external/bgfx)` with examples/tests/
  extra tools disabled; only `shaderc` is built alongside the libraries to
  compile `src/rendering/shaders/*.sc` into per-backend binaries
  (`shaders/direct3d11/`, `shaders/opengl/`, `shaders/vulkan/` next to the exe).
- Backend policy (Phase 2): `bgfx::RendererType::Direct3D11` is pinned on
  Windows for determinism; multi-backend selection is deferred to Phase 3.
