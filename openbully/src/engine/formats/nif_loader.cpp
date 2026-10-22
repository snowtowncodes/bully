#include "nif_loader.h"

#include <cstdio>
#include <cstring>

namespace OpenBully {

namespace {

// ---------------------------------------------------------------------------
// Bounded reader: any read past `end` poisons the reader; callers check `ok`.
// Handles little- and big-endian files (Bully ships 247 big-endian CS_* NIFs;
// the block count itself is always little-endian).
// ---------------------------------------------------------------------------
class Reader {
public:
    Reader() = default;
    Reader(const uint8_t* data, size_t size)
        : m_p(data), m_end(data + size), m_ok(true), m_be(false) {}

    bool Ok() const { return m_ok; }
    void Fail() { m_ok = false; }
    void SetBigEndian(bool be) { m_be = be; }

    size_t BytesLeft() const { return static_cast<size_t>(m_end - m_p); }

    // Position the reader at an absolute pointer (used to jump to the next
    // declared block boundary; caller guarantees the pointer is in range).
    void SeekTo(const uint8_t* p) { m_p = p; }
    const uint8_t* Pos() const { return m_p; }

    bool Need(size_t n) {
        if (m_p + n > m_end) {
            m_ok = false;
            return false;
        }
        return true;
    }

    uint8_t U8() {
        if (!Need(1)) return 0;
        return *m_p++;
    }

    uint16_t U16() {
        if (!Need(2)) return 0;
        uint16_t v = m_be ? static_cast<uint16_t>((m_p[0] << 8) | m_p[1])
                          : static_cast<uint16_t>((m_p[1] << 8) | m_p[0]);
        m_p += 2;
        return v;
    }

    uint32_t U32() {
        if (!Need(4)) return 0;
        uint32_t v;
        if (m_be)
            v = (static_cast<uint32_t>(m_p[0]) << 24) |
                (static_cast<uint32_t>(m_p[1]) << 16) |
                (static_cast<uint32_t>(m_p[2]) << 8) | m_p[3];
        else
            v = (static_cast<uint32_t>(m_p[3]) << 24) |
                (static_cast<uint32_t>(m_p[2]) << 16) |
                (static_cast<uint32_t>(m_p[1]) << 8) | m_p[0];
        m_p += 4;
        return v;
    }

    int32_t I32() { return static_cast<int32_t>(U32()); }

    float F32() {
        uint32_t u = U32();
        float f;
        std::memcpy(&f, &u, 4);
        return f;
    }

    NifVector3 Vec3() {
        NifVector3 v;
        v.x = F32();
        v.y = F32();
        v.z = F32();
        return v;
    }

    NifVector2 Vec2() {
        NifVector2 v;
        v.u = F32();
        v.v = F32();
        return v;
    }

    // Pascal string: u32 length + bytes (NIF 20.3.0.9 header / strings table).
    std::string PStr() {
        uint32_t len = U32();
        if (len > 4096) {
            Fail();
            return {};
        }
        if (!Need(len)) return {};
        std::string s(reinterpret_cast<const char*>(m_p), len);
        m_p += len;
        return s;
    }

private:
    const uint8_t* m_p = nullptr;
    const uint8_t* m_end = nullptr;
    bool m_ok = false;
    bool m_be = false;
};

// NiAVObject body after NiObjectNET: flags u16, translation 3f, rotation 9f,
// scale f, properties (u32 count + refs), collision i32. (bully-re/docs/nif.md)
bool ReadAVObjectBody(Reader& r) {
    r.U16(); // flags
    r.Vec3(); // translation
    for (int k = 0; k < 9; k++) r.F32(); // rotation 3x3, row-major
    r.F32(); // scale
    int32_t numProps = r.I32();
    if (numProps < 0 || numProps > 10000) return false;
    for (int32_t p = 0; p < numProps && r.Ok(); p++) r.I32();
    r.I32(); // collision object
    return r.Ok();
}

// Geometry-data block layout, verified against bully-re/docs/nif.md
// ("NiGeometryData" for NIF 20.3.0.9):
//   groupId i32 | numVertices u16 | keepFlags u8 | compressFlags u8 |
//   hasVertices u8 + 12B each | dataFlags u16 (bits 0-5: UV sets, bit 12:
//   tangents+binormals) | hasNormals u8 + 12B each | [tangents+binormals] |
//   center 3f, radius f | hasColors u8 + 16B each | UVs (8B x sets x verts) |
//   consistency u16 | additionalData i32
void ReadGeometryData(Reader& r, NifGeometryData& out) {
    r.I32(); // group id
    uint16_t numVertices = r.U16();
    r.U8();  // keep flags
    r.U8();  // compress flags

    if (r.U8()) {
        out.vertices.reserve(numVertices);
        for (uint16_t i = 0; i < numVertices && r.Ok(); i++)
            out.vertices.push_back(r.Vec3());
    }

    uint16_t dataFlags = r.U16();
    uint16_t numUVSets = dataFlags & 0x3f;
    bool hasTangents = (dataFlags & 0x1000) != 0;

    if (r.U8()) {
        out.normals.reserve(numVertices);
        for (uint16_t i = 0; i < numVertices && r.Ok(); i++)
            out.normals.push_back(r.Vec3());
    }
    if (hasTangents) {
        for (int i = 0; i < 2 * numVertices && r.Ok(); i++)
            r.Vec3(); // tangents + binormals, skipped
    }

    r.Vec3(); // bounding sphere center
    r.F32();  // bounding sphere radius

    if (r.U8()) {
        for (uint16_t i = 0; i < numVertices && r.Ok(); i++) {
            r.F32(); r.F32(); r.F32(); r.F32(); // vertex colors, skipped
        }
    }

    out.uvSets.resize(numUVSets);
    for (uint16_t s = 0; s < numUVSets && r.Ok(); s++) {
        out.uvSets[s].reserve(numVertices);
        for (uint16_t i = 0; i < numVertices && r.Ok(); i++)
            out.uvSets[s].push_back(r.Vec2());
    }

    r.U16(); // consistency flags
    r.I32(); // additional data reference
}

// NiTriShapeData: geometry data + triangle list + matching groups.
void ReadTriShapeData(Reader& r, NifGeometryData& out) {
    ReadGeometryData(r, out);
    uint16_t numTriangles = r.U16();
    r.U32(); // number of triangle points (always 3 x triangles)
    if (r.U8()) {
        out.triangles.reserve(numTriangles);
        for (uint16_t i = 0; i < numTriangles && r.Ok(); i++) {
            NifTriangle t;
            t.a = r.U16();
            t.b = r.U16();
            t.c = r.U16();
            out.triangles.push_back(t);
        }
    }
    // Matching groups: u16 count, then per group u16 count + u16 indices.
    uint16_t numGroups = r.U16();
    for (uint16_t g = 0; g < numGroups && r.Ok(); g++) {
        uint16_t n = r.U16();
        for (uint16_t i = 0; i < n && r.Ok(); i++)
            r.U16();
    }
}

// NiTriStripsData: geometry data + triangle strips, converted to triangles
// with alternating orientation; degenerate (repeated-index) tris dropped.
void ReadTriStripsData(Reader& r, NifGeometryData& out) {
    ReadGeometryData(r, out);
    r.U16(); // declared triangle count
    uint16_t numStrips = r.U16();
    std::vector<uint16_t> lengths(numStrips);
    for (uint16_t s = 0; s < numStrips && r.Ok(); s++)
        lengths[s] = r.U16();
    if (r.U8() && r.Ok()) {
        for (uint16_t s = 0; s < numStrips && r.Ok(); s++) {
            uint16_t a = 0, b = 0;
            for (uint16_t i = 0; i < lengths[s] && r.Ok(); i++) {
                uint16_t c = r.U16();
                if (i >= 2 && a != b && b != c && a != c) {
                    NifTriangle t;
                    if (i & 1) { t.a = b; t.b = a; t.c = c; }
                    else      { t.a = a; t.b = b; t.c = c; }
                    out.triangles.push_back(t);
                }
                a = b;
                b = c;
            }
        }
    }
}

} // namespace

NifLoadResult LoadNifFromMemory(const uint8_t* data, size_t size) {
    NifLoadResult result;
    if (size < 40) {
        result.error = "file too small to be a NIF";
        return result;
    }

    Reader header(data, size);

    // --- Header ------------------------------------------------------------
    // NUL/newline-terminated text line "Gamebryo File Format, Version X.Y.Z.W\n"
    // (NOT length-prefixed in Bully's 20.3.0.9 files), then u32 version,
    // u8 endianness (1 = little), u32 user version.
    {
        const void* nl = std::memchr(data, '\n', size < 128 ? size : 128);
        if (!nl || std::memcmp(data, "Gamebryo File Format", 20) != 0) {
            result.error = "not a Gamebryo NIF (bad header line)";
            return result;
        }
        header.SeekTo(static_cast<const uint8_t*>(nl) + 1);
    }

    const uint32_t version = header.U32();
    result.version = version;
    if (version != NIF_VERSION_BULLY) {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "unsupported NIF version 0x%08x (Bully ships 0x14030009)",
                      version);
        result.error = buf;
        return result;
    }

    const uint8_t endianness = header.U8();
    result.bigEndian = (endianness == 0);
    header.U32(); // user version (0 for Bully)

    // Block count is ALWAYS little-endian; endianness applies from the type
    // count onward (verified on the 247 big-endian CS_* cutscene files and
    // the loose Models/*.nif — see bully-re/docs/nif.md).
    header.SetBigEndian(false);
    const uint32_t numBlocks = header.U32();
    header.SetBigEndian(result.bigEndian);
    result.numBlocks = numBlocks;
    if (numBlocks == 0 || numBlocks > 100000) {
        result.error = "implausible block count";
        return result;
    }

    const uint16_t numTypes = header.U16();
    if (numTypes == 0 || numTypes > 1000) {
        result.error = "implausible type count";
        return result;
    }

    std::vector<std::string> typeNames(numTypes);
    for (uint16_t i = 0; i < numTypes && header.Ok(); i++)
        typeNames[i] = header.PStr();

    std::vector<uint16_t> blockTypes(numBlocks);
    for (uint32_t i = 0; i < numBlocks && header.Ok(); i++)
        blockTypes[i] = header.U16();

    std::vector<uint32_t> blockSizes(numBlocks);
    for (uint32_t i = 0; i < numBlocks && header.Ok(); i++)
        blockSizes[i] = header.U32();

    // Strings table.
    const uint32_t numStrings = header.U32();
    header.U32(); // max string length
    std::vector<std::string> strings;
    strings.reserve(numStrings);
    for (uint32_t i = 0; i < numStrings && header.Ok(); i++)
        strings.push_back(header.PStr());

    const uint32_t numGroups = header.U32();
    for (uint32_t i = 0; i < numGroups && header.Ok(); i++)
        header.U32();

    if (!header.Ok()) {
        result.error = "header truncated";
        return result;
    }

    // --- Blocks ------------------------------------------------------------
    // Each block's declared size lets us skip unknown blocks exactly. Geometry
    // data blocks are decoded inside their declared range; shape nodes are
    // decoded far enough to capture the node name (matched with the geometry
    // that follows). Everything else is skipped by size.
    std::string lastShapeName;

    for (uint32_t i = 0; i < numBlocks && header.Ok(); i++) {
        const uint32_t blockSize = blockSizes[i];
        if (!header.Need(blockSize)) {
            result.error = "block stream truncated";
            return result;
        }
        const uint8_t* blockStart = header.Pos();
        const uint8_t* blockEnd = blockStart + blockSize;

        const std::string typeName =
            blockTypes[i] < typeNames.size() ? typeNames[blockTypes[i]] : "";

        bool decodeBlock =
            (typeName == "NiTriShape" || typeName == "NiTriStrips" ||
             typeName == "NiTriShapeData" || typeName == "NiTriStripsData");

        if (decodeBlock) {
            Reader block(blockStart, blockSize);
            block.SetBigEndian(result.bigEndian);

            if (typeName == "NiTriShape" || typeName == "NiTriStrips") {
                // NiTriShape/NiTriStrips = NiObjectNET + NiAVObject +
                // NiGeometry tail: name (i32 string index), extra data
                // (u32 count + refs), controller i32, then flags/transform/
                // properties, data ref, skin ref, materials, active material,
                // dirty flag.
                const int32_t nameIndex = block.I32();
                int32_t numExtra = block.I32();
                if (numExtra < 0 || numExtra > 100000) numExtra = 0;
                for (int32_t e = 0; e < numExtra && block.Ok(); e++)
                    block.I32();
                block.I32(); // controller

                if (ReadAVObjectBody(block)) {
                    block.I32(); // data reference
                    block.I32(); // skin reference
                    int32_t numMaterials = block.I32();
                    if (numMaterials < 0 || numMaterials > 10000) numMaterials = 0;
                    for (int32_t m = 0; m < numMaterials && block.Ok(); m++) {
                        block.I32(); // material name (string index)
                        block.I32(); // material extra data
                    }
                    block.I32(); // active material
                    block.U8();  // "dirty" flag
                    if (block.Ok() && nameIndex >= 0 &&
                        static_cast<uint32_t>(nameIndex) < strings.size())
                        lastShapeName = strings[nameIndex];
                }
            } else {
                // NiTriShapeData / NiTriStripsData start directly with the
                // NiGeometryData body — NO NiObjectNET header in 20.3.0.9
                // (verified on cube.nif; see bully-re LireTriShapeData).
                NifGeometryData geo;
                geo.nodeName = lastShapeName;
                if (typeName == "NiTriShapeData")
                    ReadTriShapeData(block, geo);
                else
                    ReadTriStripsData(block, geo);
                if (block.Ok()) {
                    result.totalVertices += geo.vertices.size();
                    result.totalTriangles += geo.triangles.size();
                    result.geometries.push_back(std::move(geo));
                }
            }
        }

        // Advance exactly one declared block, decode success or not.
        header.SeekTo(blockEnd);
    }

    result.ok = !result.geometries.empty();
    if (!result.ok)
        result.error = "no NiTriShape/NiTriStrips geometry found in file";
    return result;
}

NifLoadResult LoadNifFile(const std::string& path) {
    NifLoadResult result;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        result.error = "cannot open file: " + path;
        return result;
    }
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(f);
        result.error = "empty file: " + path;
        return result;
    }
    std::vector<uint8_t> buffer(static_cast<size_t>(size));
    const size_t got = std::fread(buffer.data(), 1, buffer.size(), f);
    std::fclose(f);
    if (got != buffer.size()) {
        result.error = "short read: " + path;
        return result;
    }
    return LoadNifFromMemory(buffer.data(), buffer.size());
}

} // namespace OpenBully
