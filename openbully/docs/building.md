# Building OpenBully

OpenBully targets **Windows x64 first** (VS 2022), with cross-platform support
planned for later phases. Phase 1 uses a **vendored SDL 2.30.12** development
package — no package manager required.

## Prerequisites

- **Windows 10/11 x64**
- **Visual Studio 2022** (Community works) with:
  - "Desktop development with C++" workload
  - MSVC v143 toolset (build tools) — checked at `VC/Tools/MSVC/14.4x`
  - Windows 10/11 SDK
- **CMake 3.20+** (bundled with VS 2022, or standalone)

SDL2 is **vendored**: `external/SDL2-2.30.12/` (prebuilt x64 import libs +
DLL + headers). Nothing else to install.

## Quick start (presets, recommended)

From the `openbully/` directory:

```bat
cd C:\Users\Snowtown\Projects\bully\openbully

:: Configure (uses external/SDL2-2.30.12 automatically)
cmake --preset windows-release

:: Build
cmake --build build --config Release

:: Run
build\Release\openbully.exe
```

A dark blue-grey window (1280x720) opens with an RGB gradient triangle.
Press **Esc** or close the window to exit.

Debug preset:

```bat
cmake --preset windows-debug
cmake --build build --config Debug
build\Debug\openbully.exe
```

## Manual configure (no presets)

```bat
cd C:\Users\Snowtown\Projects\bully\openbully
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## Using vcpkg SDL2 instead (optional)

If you'd rather use vcpkg:

```bat
git clone https://github.com/microsoft/vcpkg C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
C:\vcpkg\vcpkg install sdl2:x64-windows
set VCPKG_ROOT=C:\vcpkg

:: Then configure with the vcpkg toolchain and delete/move the external/ dir
:: so the vendored config is not picked up:
move external external.bak
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release
```

## How SDL2 is resolved

`CMakeLists.txt` tries, in order:

1. **Vendored** `external/SDL2-2.30.12/cmake/sdl2-config.cmake` (Windows only,
   if present) — default path, zero external setup.
2. **System SDL2** via `find_package(SDL2 REQUIRED)` (module/config on Linux
  /macOS, or whatever `SDL2_DIR` points at).

Note on headers: the vendored VC package ships headers flat
(`include/SDL.h`), while OpenBully sources use the portable
`#include <SDL2/SDL.h>` spelling (matches vcpkg / Linux layouts). At
configure time CMake copies the vendored headers into
`<build>/sdl2-shim/SDL2/` and adds that directory to the include path, so
no source changes are needed. The build also copies `SDL2.dll` next to
`openbully.exe` automatically.

## Project layout

```
openbully/
├── CMakeLists.txt          # Top-level build script
├── CMakePresets.json       # VS 2022 presets (debug/release)
├── external/
│   ├── SDL2-VC.zip         # Downloaded dev archive (can be deleted)
│   └── SDL2-2.30.12/       # Vendored SDL2 (headers, x64 import libs, DLL)
├── src/
│   ├── main.cpp            # Entry point + main loop
│   ├── engine/
│   │   ├── engine.{h,cpp}      # Engine lifecycle + Phase 1 RGB triangle
│   │   └── resource_manager.{h,cpp} # Resource registry stub (Track 1)
│   └── platform/
│       ├── window.{h,cpp}  # SDL2 window + OpenGL 3.3 core context
│       ├── gl_core.{h,cpp} # Minimal GL function loader (SDL_GL_GetProcAddress)
│       └── logger.{h,cpp}  # printf-based logging
└── docs/
    └── building.md         # This file
```

## Verification checklist (Phase 1)

- [x] `cmake --preset windows-release` configures without errors
- [x] `cmake --build build --config Release` succeeds
- [x] `build\Release\openbully.exe` launches, window opens (1280x720)
- [x] RGB triangle visible
- [x] Esc / window close exits cleanly, no crashes (soak ≥ 10 s)

## Troubleshooting

- **`Could not find SDL2`** — ensure `external/SDL2-2.30.12/` exists. If not,
  download and extract it:

  ```bat
  mkdir external
  curl -L -o external\SDL2-VC.zip https://github.com/libsdl-org/SDL/releases/download/release-2.30.12/SDL2-devel-2.30.12-VC.zip
  tar -xf external\SDL2-VC.zip -C external
  ```
- **`SDL2.dll not found` at runtime** — the build copies it next to the exe;
  if it's missing, check the post-build step output or copy
  `external\SDL2-2.30.12\lib\x64\SDL2.dll` manually.
- **Linker errors on `SDL_main`** — Phase 1 uses plain `main()`, linked against
  `SDL2::SDL2` (not `SDL2::SDL2main`), so entry-point conflicts shouldn't occur.
  If you see `error LNK2019: SDL_main`, ensure the `WinMain` CRT startup wasn't
  forced.
- **OpenGL errors at startup** — Phase 1 requires OpenGL 3.3 core. Any GPU
  driver from the last decade supports this; update your GPU driver if the
  context creation fails.


## bgfx (Phase 2 rendering)

bgfx is **vendored** at `external/bgfx/` — the `bgfx.cmake` release
`v1.129.8958-499` (bgfx + bx + bimg + CMake integration in one tree). It is
gitignored, so a fresh checkout must fetch it once:

```powershell
powershell -File scripts/fetch_bgfx.ps1
```

The script is idempotent (skips when `external/bgfx/CMakeLists.txt` exists).
Configure/build are unchanged (`cmake --preset windows-release`). The build
also compiles the shaders in `src/rendering/shaders/*.sc` with bgfx's
`shaderc` into `shaders/<backend>/*.bin` next to the executable (Direct3D11
is the pinned Phase-2 backend; see docs/dependencies.md for the version
policy).
