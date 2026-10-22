# OpenBully

Open-source reimplementation of Bully's game engine.

**Status**: Bootstrap Phase 1 (Week 1/52)

## Current Milestone: Foundation Setup

**Goal**: Window opens, renders test triangle, asset extractor exists.

**Progress**:
- [x] CMake build system
- [x] SDL2 window creation
- [x] OpenGL context
- [x] Main loop
- [x] Logger
- [x] Test triangle rendering
- [x] Asset extractor (IMG archives)
- [x] NIF format research (Gamebryo, not RenderWare)

## Building

### Requirements
- CMake 3.20+
- C++17 compiler
- SDL2 (vendored in external/SDL2-2.30.12)

### Windows
```bash
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022"
cmake --build . --config Release
```

### Linux/macOS
```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

## Running

```bash
# Launch window (should show colored triangle)
./openbully

# Run test suite
./openbully --test-triangle
```

## Agentic Development

This project is being developed using autonomous AI agents following the plan in `docs/AGENTIC_PORT_PLAN.md`.

**Active Tracks**:
- Track 1: Foundation Engineer (main loop, platform layer) - **COMPLETE**
- Track 2: Asset Pipeline Engineer (IMG extraction, NIF research) - **COMPLETE**
- Track 3: Data Loader Engineer (NIF mesh loading) - **IN PROGRESS**
- Track 4: Rendering Engineer (bgfx integration) - **IN PROGRESS**

## Legal

Requires original Bully game files. No assets are distributed with this repository.

Reverse engineering for interoperability purposes. See LICENSE for details.
