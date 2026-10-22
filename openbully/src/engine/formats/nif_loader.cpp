#include "nif_loader.h"

#include <cstdio>
#include <cstring>

namespace OpenBully {

namespace {

// Strip export-machine directories: "z:\\bully\\temp\\school\\wall01.tga" ->
// "wall01" (matches bully-re BaseNom; '/' and '\\' separators both accepted).
std::string BaseTextureName(const std::string& path) {
    size_t start = 0;
    for (size_t i = 0; i < path.size(); i++)
        if (path[i] == '/' || path[i] == '\\') start = i + 1;
    size_t end = path.size();
    const size_t dot = path.rfind('.', end);
    if (dot != std::string::npos && dot >= start) end = dot;
    return path.substr(start, end - start);
}
// Handles little- and big-endian files (Bully ships 247 big-endian CS_* NIFs;
// the block count itself is always little-endian).
// ---------------------------------------------------------------------------
// Bounded reader: any read past `end` poisons the reader; callers check `ok`.
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
// When `out` is non-null the transform components are captured as well; when
// `outProps` is non-null the property block references are captured too.
bool ReadAVObjectBody(Reader& r, NifNode* out = nullptr,
                      std::vector<int32_t>* outProps = nullptr) {
    r.U16(); // flags
    const NifVector3 t = r.Vec3();
    float rot[9];
    for (int k = 0; k < 9; k++) rot[k] = r.F32(); // rotation 3x3, row-major
    const float s = r.F32();
    if (out) {
        out->translation = t;
        for (int k = 0; k < 9; k++) out->rotation[k] = rot[k];
        out->scale = s;
    }
    int32_t numProps = r.I32();
    if (numProps < 0 || numProps > 10000) return false;
    for (int32_t p = 0; p < numProps && r.Ok(); p++) {
        const int32_t ref = r.I32();
        if (outProps) outProps->push_back(ref);
    }
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

// NiTexDesc: source ref i32, flags u16 (low byte = UV set), then an optional
// 4x4 transform (u8 + 40 bytes). (bully-re NifFile.cpp LireTexDesc)
void ReadTexDesc(Reader& r, int32_t* sourceRef) {
    const int32_t s = r.I32();
    r.U16();
    if (r.U8()) {
        for (int k = 0; k < 9; k++) r.F32(); // transform: 2x2 + offset/scale
        r.U32();                              // transform method
        r.F32(); r.F32();                     // center
    }
    if (sourceRef) *sourceRef = s;
}

// NiTexturingProperty in 20.3.0.9: NiObjectNET header, flags u16, slot count
// u32, then per slot an u8 "present" + TexDesc; slot 5 (bump) carries 6 extra
// floats, slot 7 (parallax) 1 float; then shader-texture count u32 with 4+1
// bytes each. Returns the base-map (slot 0) NiSourceTexture block reference,
// or -1. (bully-re NifFile.cpp LireTexturing)
int32_t ReadTexturingProperty(Reader& r) {
    r.I32();                       // name
    int32_t numExtra = r.I32();
    if (numExtra < 0 || numExtra > 100000) return -1;
    for (int32_t e = 0; e < numExtra && r.Ok(); e++) r.I32();
    r.I32();                       // controller
    r.U16();                       // flags
    const uint32_t numSlots = r.U32();
    if (numSlots > 12) return -1;
    int32_t baseSource = -1;
    for (uint32_t s = 0; s < numSlots && r.Ok(); s++) {
        if (r.U8()) {
            int32_t src = -1;
            ReadTexDesc(r, &src);
            if (s == 0) baseSource = src;
            if (s == 5) { for (int k = 0; k < 6; k++) r.F32(); }
            if (s == 7) r.F32();
        }
    }
    uint32_t numShaderTex = r.U32();
    if (numShaderTex > 32) return -1;
    for (uint32_t s = 0; s < numShaderTex && r.Ok(); s++) {
        if (r.U8()) {
            ReadTexDesc(r, nullptr);
            r.U32();               // shader map id
        }
    }
    return r.Ok() ? baseSource : -1;
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
    // Each block's declared size lets us skip unknown blocks exactly. Node
    // blocks (NiNode / NiTriShape / NiTriStrips) build the hierarchy table;
    // geometry data blocks are decoded inside their declared range;
    // NiTexturingProperty / NiSourceTexture blocks are decoded to resolve
    // per-shape base textures. Everything else is skipped by size.
    std::string lastShapeName;
    std::vector<int32_t> lastShapeProps;
    int32_t lastShapeSkinRef = -1; // NiSkinInstance ref of the current shape
    // Block index -> base-map NiSourceTexture ref (from NiTexturingProperty).
    std::vector<int32_t> texturingBaseSource(numBlocks, -1);
    // Block index -> texture file name (from NiSourceTexture, stripped).
    std::vector<std::string> sourceTextureNames(numBlocks);
    // Track 17: raw skin payloads until cross-referenced post-pass.
    std::vector<int32_t> blockToNode(numBlocks, -1); // block -> nodes[] position
    struct RawSkinInstance { int32_t dataRef = -1; int32_t rootRef = -1;
                             std::vector<int32_t> boneRefs; };
    struct RawSkinData { struct Bone {
        float rotation[9] = {1,0,0,0,1,0,0,0,1}; float translation[3] = {0,0,0};
        float scale = 1.0f;
        std::vector<uint32_t> vi; std::vector<float> vw; } ;
        bool hasWeights = false; std::vector<Bone> bones; };
    std::vector<RawSkinInstance> skinInstances(numBlocks);
    std::vector<RawSkinData> skinDatas(numBlocks);
    int32_t pendingSkinRef = -1; // last shape's NiSkinInstance ref

    result.nodes.reserve(numBlocks);

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

        const bool isShape = (typeName == "NiTriShape" || typeName == "NiTriStrips");
        const bool isNode = isShape || typeName == "NiNode";
        const bool isGeomData = (typeName == "NiTriShapeData" || typeName == "NiTriStripsData");
        const bool isTexBlock = (typeName == "NiTexturingProperty" ||
                                 typeName == "NiSourceTexture");
        const bool isSkinInstance = (typeName == "NiSkinInstance");
        const bool isSkinData = (typeName == "NiSkinData");

        if (isTexBlock) {
            Reader block(blockStart, blockSize);
            block.SetBigEndian(result.bigEndian);
            if (typeName == "NiTexturingProperty") {
                const int32_t base = ReadTexturingProperty(block);
                if (block.Ok() && base >= 0 &&
                    static_cast<uint32_t>(base) < numBlocks)
                    texturingBaseSource[i] = base;
            } else {
                // NiSourceTexture: NiObjectNET (name, extra refs, controller),
                // then u8 useExternal, fileName i32, pixelData i32, 3x u32,
                // 3x u8. Identical size for internal (.nft pixels) and
                // external (file reference) textures.
                // (bully-re NifFile.cpp LireSourceTexture)
                const int32_t nameIndex = block.I32();
                int32_t numExtra = block.I32();
                if (numExtra < 0 || numExtra > 100000) numExtra = 0;
                for (int32_t e = 0; e < numExtra && block.Ok(); e++)
                    block.I32();
                block.I32(); // controller
                block.U8();  // useExternal
                const int32_t fileNameIdx = block.I32();
                if (block.Ok() && fileNameIdx >= 0 &&
                    static_cast<uint32_t>(fileNameIdx) < strings.size())
                    sourceTextureNames[i] =
                        BaseTextureName(strings[fileNameIdx]);
            }
        } else if (isSkinInstance) {
            // NiSkinInstance in 20.3.0.9: data ref, partition ref, skeleton
            // root ref, bone count i32, bone node refs. (bully-re NifFile.cpp
            // LireSkinInstance.)
            Reader block(blockStart, blockSize);
            block.SetBigEndian(result.bigEndian);
            RawSkinInstance& si = skinInstances[i];
            si.dataRef = block.I32();
            block.I32(); // partition ref (unused: weights come from NiSkinData)
            si.rootRef = block.I32();
            int32_t numBones = block.I32();
            if (numBones >= 0 && numBones <= 1024) {
                si.boneRefs.reserve(numBones);
                for (int32_t b = 0; b < numBones && block.Ok(); b++)
                    si.boneRefs.push_back(block.I32());
            }
        } else if (isSkinData) {
            // NiSkinData in 20.3.0.9: overall transform (row-major 3x3, 3f,
            // 1f), bone count i32, has-weights u8, then per bone a bind
            // transform (13 floats), bounding sphere (center 3f, radius 1f),
            // vertex count u16 and, when weighted, (u16 index, f weight) per
            // vertex. (bully-re LireSkinData.)
            Reader block(blockStart, blockSize);
            block.SetBigEndian(result.bigEndian);
            RawSkinData& sd = skinDatas[i];
            for (int k = 0; k < 13; k++) block.F32(); // overall transform
            int32_t numBones = block.I32();
            sd.hasWeights = block.U8() != 0;
            if (numBones < 0 || numBones > 1024) { block.Fail(); }
            else {
                sd.bones.resize(numBones);
                for (int32_t b = 0; b < numBones && block.Ok(); b++) {
                    RawSkinData::Bone& o = sd.bones[b];
                    for (int k = 0; k < 9; k++) o.rotation[k] = block.F32();
                    for (int k = 0; k < 3; k++) o.translation[k] = block.F32();
                    o.scale = block.F32();
                    for (int k = 0; k < 4; k++) block.F32(); // bounding sphere
                    const uint16_t numVerts = block.U16();
                    if (sd.hasWeights) {
                        o.vi.reserve(numVerts); o.vw.reserve(numVerts);
                        for (uint16_t v = 0; v < numVerts && block.Ok(); v++) {
                            o.vi.push_back(block.U16());
                            o.vw.push_back(block.F32());
                        }
                    }
                }
            }
        } else if (isNode) {
            Reader block(blockStart, blockSize);
            block.SetBigEndian(result.bigEndian);

            // Every node gets a hierarchy entry in block order, even when the
            // decode fails (children refs stay consistent).
            NifNode node;
            const int32_t nameIndex = block.I32();
            if (nameIndex >= 0 && static_cast<uint32_t>(nameIndex) < strings.size())
                node.name = strings[nameIndex];
            int32_t numExtra = block.I32();
            if (numExtra < 0 || numExtra > 100000) numExtra = 0;
            for (int32_t e = 0; e < numExtra && block.Ok(); e++)
                block.I32();
            block.I32(); // controller

            bool avOk = ReadAVObjectBody(block, &node, isShape ? &lastShapeProps : nullptr);

            if (!isShape) {
                // NiNode tail: child refs (i32 count + i32 refs).
                int32_t numChildren = block.I32();
                if (numChildren < 0 || numChildren > 100000) numChildren = 0;
                for (int32_t c = 0; c < numChildren && block.Ok(); c++) {
                    const int32_t ref = block.I32();
                    if (ref >= 0) node.children.push_back(static_cast<uint32_t>(ref));
                }
            } else {
                // NiTriShape/NiTriStrips tail: data ref, skin ref, materials,
                // active material, dirty flag.
                block.I32(); // data reference
                lastShapeSkinRef = block.I32(); // skin reference
                int32_t numMaterials = block.I32();
                if (numMaterials < 0 || numMaterials > 10000) numMaterials = 0;
                for (int32_t m = 0; m < numMaterials && block.Ok(); m++) {
                    block.I32(); // material name (string index)
                    block.I32(); // material extra data
                }
                block.I32(); // active material
                block.U8();  // "dirty" flag
            }

            if (avOk && block.Ok()) {
                if (isShape) {
                    lastShapeName = node.name;
                    // Property refs captured above (ReadAVObjectBody); clear
                    // any stale refs from a failed previous decode.
                    if (!avOk) lastShapeProps.clear();
                    // Track 17: this shape's NiSkinInstance ref (invalidated
                    // when the decode failed; reset per shape).
                    // lastShapeSkinRef was set in the tail read above.
                }
                node.isShape = isShape;
                blockToNode[i] = static_cast<int32_t>(result.nodes.size());
                result.nodes.push_back(std::move(node));
            } else {
                blockToNode[i] = static_cast<int32_t>(result.nodes.size());
                result.nodes.push_back(NifNode{}); // placeholder keeps indices
            }
            // Stash the skin ref for the geometry data block that follows.
            if (isShape) pendingSkinRef = lastShapeSkinRef;
        } else if (isGeomData) {
            Reader block(blockStart, blockSize);
            block.SetBigEndian(result.bigEndian);

            // NiTriShapeData / NiTriStripsData start directly with the
            // NiGeometryData body - NO NiObjectNET header in 20.3.0.9
            // (verified on cube.nif; see bully-re LireTriShapeData).
            NifGeometryData geo;
            geo.nodeName = lastShapeName;
            geo.nodeIndex = result.nodes.empty()
                ? 0 : static_cast<uint32_t>(result.nodes.size() - 1);
            // Track 17: the shape this data belongs to carried a skin ref.
            if (pendingSkinRef >= 0 &&
                static_cast<uint32_t>(pendingSkinRef) < numBlocks)
                geo.skinIndex = static_cast<uint32_t>(pendingSkinRef);
            // Resolve this shape's base texture through its properties:
            // shape -> NiTexturingProperty -> base NiSourceTexture -> name.
            for (int32_t prop : lastShapeProps) {
                if (prop < 0 || static_cast<uint32_t>(prop) >= numBlocks)
                    continue;
                const int32_t src = texturingBaseSource[prop];
                if (src < 0 || static_cast<uint32_t>(src) >= numBlocks)
                    continue;
                if (!sourceTextureNames[src].empty()) {
                    geo.textureName = sourceTextureNames[src];
                    break;
                }
            }
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

        // Advance exactly one declared block, decode success or not.
        header.SeekTo(blockEnd);
    }


    // --- Hierarchy index fix-up (Track 17) ----------------------------------
    result.blockToNode = std::move(blockToNode);
    // Node children were parsed as raw NiBlock references, but non-node
    // blocks (geometry data, skins, textures...) interleave with nodes, so
    // block index != nodes[] position on any NIF with such blocks. Remap
    // children to nodes[] positions; drop refs that do not map.
    for (NifNode& n : result.nodes) {
        if (n.children.empty()) continue;
        std::vector<uint32_t> mapped;
        mapped.reserve(n.children.size());
        for (uint32_t c : n.children) {
            const int32_t ni = (c < result.blockToNode.size())
                ? result.blockToNode[c] : -1;
            if (ni >= 0) mapped.push_back(static_cast<uint32_t>(ni));
        }
        n.children = std::move(mapped);
    }

    // Fill parent links from the (now nodes[]-indexed) children refs.
    for (uint32_t n = 0; n < result.nodes.size(); n++)
        for (uint32_t c : result.nodes[n].children)
            if (c < result.nodes.size() &&
                result.nodes[c].parent < 0)
                result.nodes[c].parent = static_cast<int32_t>(n);

    // --- Track 17: assemble skins from the raw blocks -----------------------
    // skins[blockIndex] exists for every parsed NiSkinInstance; skinned
    // geometries point at their instance via skinIndex and carry resolved
    // per-vertex weights.
    result.skins.resize(numBlocks);
    std::vector<bool> skinValid(numBlocks, false);
    for (uint32_t i = 0; i < numBlocks; i++) {
        const RawSkinInstance& si = skinInstances[i];
        if (si.dataRef < 0 || static_cast<uint32_t>(si.dataRef) >= numBlocks)
            continue;
        const RawSkinData& sd = skinDatas[static_cast<size_t>(si.dataRef)];
        if (!sd.hasWeights || sd.bones.size() != si.boneRefs.size()) continue;
        NifSkinData& out = result.skins[i];
        out.skeletonRoot = si.rootRef >= 0 ? static_cast<uint32_t>(si.rootRef)
                                           : 0xFFFFFFFF;
        out.bones.resize(si.boneRefs.size());
        for (size_t b = 0; b < si.boneRefs.size(); b++) {
            out.boneNodes.push_back(static_cast<uint32_t>(si.boneRefs[b]));
            NifSkinBone& ob = out.bones[b];
            const RawSkinData::Bone& ib = sd.bones[b];
            std::memcpy(ob.rotation, ib.rotation, sizeof ob.rotation);
            std::memcpy(ob.translation, ib.translation, sizeof ob.translation);
            ob.scale = ib.scale;
            ob.vertexIndices = ib.vi;
            ob.vertexWeights = ib.vw;
        }
        skinValid[i] = true;
    }
    // Resolve per-vertex weights for skinned geometries.
    for (NifGeometryData& geo : result.geometries) {
        if (geo.skinIndex >= numBlocks || !skinValid[geo.skinIndex]) {
            geo.skinIndex = 0xFFFFFFFF;
            continue;
        }
        const NifSkinData& skin = result.skins[geo.skinIndex];
        geo.weights.assign(geo.vertices.size(), NifVertexWeights{});
        // Per-vertex influence list, rebuilt from the per-bone weight lists.
        struct Influ { uint8_t idx[4]; float w[4]; uint8_t n; };
        std::vector<Influ> infl(geo.vertices.size());
        for (size_t b = 0; b < skin.bones.size(); b++) {
            const NifSkinBone& ob = skin.bones[b];
            for (size_t k = 0; k < ob.vertexIndices.size(); k++) {
                const uint32_t v = ob.vertexIndices[k];
                if (v >= infl.size()) continue;
                Influ& f = infl[v];
                if (f.n < 4) { f.idx[f.n] = static_cast<uint8_t>(b);
                               f.w[f.n] = ob.vertexWeights[k]; f.n++; }
            }
        }
        for (size_t v = 0; v < infl.size(); v++) {
            NifVertexWeights& w = geo.weights[v];
            w.count = infl[v].n;
            for (int k = 0; k < 4; k++) { w.index[k] = infl[v].idx[k]; w.weight[k] = infl[v].w[k]; }
        }
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

bool NifWorldTransform(const NifLoadResult& nif, uint32_t index, float out[16]) {
    if (index >= nif.nodes.size()) return false;

    // Local transform: T * R * S, column-major 4x4 (bgfx convention).
    // NIF stores the 3x3 rotation row-major; R_col[c*4+r] = rotation[r*3+c].
    auto local = [](const NifNode& n, float m[16]) {
        for (int k = 0; k < 16; k++) m[k] = 0.0f;
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                m[c * 4 + r] = n.rotation[r * 3 + c] * n.scale;
        m[12] = n.translation.x;
        m[13] = n.translation.y;
        m[14] = n.translation.z;
        m[15] = 1.0f;
    };

    auto mul = [](const float* a, const float* b, float* o) { // o = a * b
        float r[16];
        for (int c = 0; c < 4; c++)
            for (int row = 0; row < 4; row++) {
                float s = 0.0f;
                for (int k = 0; k < 4; k++)
                    s += a[k * 4 + row] * b[c * 4 + k];
                r[c * 4 + row] = s;
            }
        for (int k = 0; k < 16; k++) o[k] = r[k];
    };

    float acc[16];
    local(nif.nodes[index], acc);

    // Compose child-to-root up the parent chain (cycle-capped defensively).
    int32_t parent = nif.nodes[index].parent;
    int depth = 0;
    while (parent >= 0 &&
           static_cast<uint32_t>(parent) < nif.nodes.size() && depth < 64) {
        float pm[16];
        local(nif.nodes[parent], pm);
        float res[16];
        mul(pm, acc, res);
        for (int k = 0; k < 16; k++) acc[k] = res[k];
        parent = nif.nodes[parent].parent;
        ++depth;
    }

    for (int k = 0; k < 16; k++) out[k] = acc[k];
    return true;
}

} // namespace OpenBully
