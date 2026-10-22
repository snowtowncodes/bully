#pragma once

// OpenBully: NIF (Gamebryo/NetImmerse) mesh loader.
// NIF v20.3.0.9, Bully SE (PC). Parses:
//   - header (version check, little/big endianness)
//   - block table (types, sizes, strings) so unknown blocks are skipped
//   - NiTriShape / NiTriStrips geometry: vertex positions, normals, UV sets,
//     triangle indices (strips converted to triangles)
//
// Format notes (verified against bully-re/docs/nif.md and hex dumps of
// retail assets):
//   - header is a newline-terminated text line, NOT length-prefixed
//   - hex version 0x14030009 ("20.3.0.9"), user version 0
//   - block count is always little-endian; endianness applies from the type
//     count onward (247 CS_* cutscene files are big-endian)
//   - NiTriShape/NiTriStrips carry an NiObjectNET header; their data blocks
//     (NiTriShapeData/NiTriStripsData) start directly at NiGeometryData
//   - every block's declared size lets unknown blocks be skipped exactly
//
// Animations, controllers, skins, materials and textures are skipped, not
// interpreted (Track 3 scope).

#include <cstdint>
#include <string>
#include <vector>

namespace OpenBully {

// 0x14030009 == "20.3.0.9"
constexpr uint32_t NIF_VERSION_BULLY = 0x14030009u;

struct NifVector3 {
    float x, y, z;
};

struct NifVector2 {
    float u, v;
};

struct NifTriangle {
    uint16_t a, b, c;
};

// One geometry piece extracted from the file (one per NiTriShape/NiTriStrips
// data block, paired with its owning shape node).
struct NifGeometryData {
    std::string nodeName;        // name of the owning NiTriShape/NiTriStrips
    std::vector<NifVector3> vertices;
    std::vector<NifVector3> normals;
    std::vector<std::vector<NifVector2>> uvSets; // one inner vector per UV set
    std::vector<NifTriangle> triangles;          // strips already converted
};

struct NifLoadResult {
    bool ok = false;
    std::string error;                 // empty when ok
    uint32_t version = 0;              // hex NIF version, e.g. 0x14030009
    uint32_t numBlocks = 0;
    bool bigEndian = false;            // CS_* cutscene exports are big-endian
    std::vector<NifGeometryData> geometries;
    size_t totalVertices = 0;
    size_t totalTriangles = 0;
};

// Parse a NIF file from memory. `data` must remain valid only for the call.
NifLoadResult LoadNifFromMemory(const uint8_t* data, size_t size);

// Convenience: read the file at `path` and parse it.
NifLoadResult LoadNifFile(const std::string& path);

} // namespace OpenBully
