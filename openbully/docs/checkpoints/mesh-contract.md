# Mesh Contract (Track 3 ↔ Track 4)

**Status**: Defined (Hour 24)

## CPU-side Mesh struct

Defined in `src/rendering/mesh.h`:

```cpp
struct Vertex {
    float position[3];  // XYZ
    float normal[3];    // unit normal
    float uv[2];        // texture coords
};

struct Mesh {
    std::vector<Vertex> vertices;
    std::vector<uint16_t> indices;  // triangle list
    const char* name;               // debug label
};
```

## Ownership

- **Track 3 (Data Loader)** creates `Mesh` from NIF, owns CPU-side data
- **Track 4 (Rendering)** reads `Mesh`, uploads to GPU, owns GPU buffers

## Coordinate system

- **Left-handed** (to match Gamebryo)
- Y-up world space
- Normals are **unit vectors** (Track 3 must normalize)

## Index format

- **uint16_t** (max 65,535 vertices per mesh)
- If NIF exceeds this, Track 3 must split mesh

## GPU upload

Track 4 (Renderer) is responsible for:
- Allocating bgfx vertex/index buffers
- Uploading Mesh data to GPU
- Releasing GPU resources on shutdown

Track 3 does NOT touch bgfx.

## Verification checkpoint (Hour 48)

Test: Load one NIF mesh (Track 3) → render with bgfx (Track 4)

Pass criteria:
- Mesh visible on screen
- No crash
- Vertex count matches NifSkope
