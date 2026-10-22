#include "nft_loader.h"

#include <cstdio>
#include <cstring>

namespace OpenBully {

namespace {

// ---------------------------------------------------------------------------
// Bounded reader: any read past `end` poisons the reader; callers check `ok`.
// Little/big-endian aware (131 CS_* NFTs are big-endian; block count stays
// little-endian regardless — see bully-re/docs/nft.md). Pixel payloads are
// ALWAYS read little-endian (the game never swaps image contents).
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

    // Skip a run of i32 block references (negative allowed, -1 = none).
    bool Refs(int32_t count) {
        if (count < 0) count = 0;
        if (!Need(static_cast<size_t>(count) * 4)) return false;
        m_p += static_cast<size_t>(count) * 4;
        return true;
    }

private:
    const uint8_t* m_p = nullptr;
    const uint8_t* m_end = nullptr;
    bool m_ok = false;
    bool m_be = false;
};

// --- DXT decoding (BC1 / BC3), verified against bully-re TextureDecode ------

// RGB 5:6:5 -> 8-bit with high-bit replication (hardware-decoder style).
void Color565(uint16_t c, uint8_t out[4]) {
    const uint8_t r = static_cast<uint8_t>((c >> 11) & 0x1f);
    const uint8_t g = static_cast<uint8_t>((c >> 5) & 0x3f);
    const uint8_t b = static_cast<uint8_t>(c & 0x1f);
    out[0] = static_cast<uint8_t>((r << 3) | (r >> 2));
    out[1] = static_cast<uint8_t>((g << 2) | (g >> 4));
    out[2] = static_cast<uint8_t>((b << 3) | (b >> 2));
    out[3] = 255;
}

// 4-color palette of a BC1 block. `dxt1`: the 3-color+transparent mode exists
// only when c0 <= c1 (never valid in a DXT5 color half).
void PaletteColors(uint16_t c0, uint16_t c1, bool dxt1, uint8_t pal[4][4]) {
    Color565(c0, pal[0]);
    Color565(c1, pal[1]);
    if (!dxt1 || c0 > c1) {
        for (int k = 0; k < 3; k++) {
            pal[2][k] = static_cast<uint8_t>((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = static_cast<uint8_t>((pal[0][k] + 2 * pal[1][k]) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (int k = 0; k < 3; k++)
            pal[2][k] = static_cast<uint8_t>((pal[0][k] + pal[1][k]) / 2);
        pal[3][0] = pal[3][1] = pal[3][2] = 0;
        pal[2][3] = 255;
        pal[3][3] = 0;
    }
}

// 32 index bits = two 16-bit words: first carries rows 0-1, second rows 2-3.
// `src` is always read little-endian (the game never swaps pixel payloads).
void ColorIndices(const uint8_t* p, const uint8_t pal[4][4], uint8_t out[64]) {
    const uint32_t idx = static_cast<uint32_t>(p[0]) |
                         (static_cast<uint32_t>(p[1]) << 8) |
                         (static_cast<uint32_t>(p[2]) << 16) |
                         (static_cast<uint32_t>(p[3]) << 24);
    for (int i = 0; i < 16; i++)
        std::memcpy(out + i * 4, pal[(idx >> (2 * i)) & 3], 4);
}

void DecodeDxt1Block(const uint8_t* b, uint8_t out[64]) {
    uint16_t c0 = static_cast<uint16_t>(b[0] | (b[1] << 8));
    uint16_t c1 = static_cast<uint16_t>(b[2] | (b[3] << 8));
    uint8_t pal[4][4];
    PaletteColors(c0, c1, true, pal);
    ColorIndices(b + 4, pal, out);
}

void DecodeDxt5Block(const uint8_t* b, uint8_t out[64]) {
    // Alpha: two reference values then 48 bits of 3-bit indices.
    const uint8_t a0 = b[0], a1 = b[1];
    uint8_t alpha[8];
    alpha[0] = a0;
    alpha[1] = a1;
    if (a0 > a1) {
        for (int k = 1; k < 7; k++)
            alpha[k + 1] = static_cast<uint8_t>(((7 - k) * a0 + k * a1) / 7);
    } else {
        for (int k = 1; k < 5; k++)
            alpha[k + 1] = static_cast<uint8_t>(((5 - k) * a0 + k * a1) / 5);
        alpha[6] = 0;
        alpha[7] = 255;
    }
    uint64_t bits = 0;
    for (int k = 0; k < 6; k++)
        bits |= static_cast<uint64_t>(b[2 + k]) << (8 * k);

    const uint16_t c0 = static_cast<uint16_t>(b[8] | (b[9] << 8));
    const uint16_t c1 = static_cast<uint16_t>(b[10] | (b[11] << 8));
    uint8_t pal[4][4];
    PaletteColors(c0, c1, false, pal);
    ColorIndices(b + 12, pal, out);
    for (int i = 0; i < 16; i++)
        out[i * 4 + 3] = alpha[(bits >> (3 * i)) & 7];
}

// Gamebryo PixelFormat enum (subset present in the retail archive).
enum NifPixelFormat : uint32_t {
    NIF_PX_RGB8  = 0,
    NIF_PX_RGBA8 = 1,
    NIF_PX_PAL8  = 2,
    NIF_PX_DXT1  = 4,
    NIF_PX_DXT3  = 5,
    NIF_PX_DXT5  = 6,
};

uint32_t MipLevelBytes(uint32_t fmt, uint32_t w, uint32_t h) {
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    switch (fmt) {
        case NIF_PX_RGB8:  return w * h * 3;
        case NIF_PX_RGBA8: return w * h * 4;
        case NIF_PX_PAL8:  return w * h;
        case NIF_PX_DXT1:  return bw * bh * 8;
        case NIF_PX_DXT3:
        case NIF_PX_DXT5:  return bw * bh * 16;
        default:           return 0;
    }
}

// Position of each R,G,B,A channel within a raw RGB/RGBA pixel, read from the
// NiPixelFormat channel types (0=red, 1=green, 2=blue, 3=alpha).
void ChannelOrder(const uint32_t types[4], int order[4]) {
    order[0] = 0; order[1] = 1; order[2] = 2; order[3] = 3;
    for (int k = 0; k < 4; k++)
        if (types[k] <= 3) order[types[k]] = k;
}

// Strip export-machine directories: "z:\bully\temp\school\wall01.tga" ->
// "wall01" (matches bully-re BaseNom; path separators '/' and '\' both ok).
std::string BaseTextureName(const std::string& path) {
    size_t start = 0;
    for (size_t i = 0; i < path.size(); i++)
        if (path[i] == '/' || path[i] == '\\') start = i + 1;
    size_t end = path.size();
    if (end > start && path[end - 1] == '.') end--; // tolerate stray dot
    const size_t dot = path.rfind('.', end);
    if (dot != std::string::npos && dot >= start) end = dot;
    return path.substr(start, end - start);
}

// Lowercased copy for case-insensitive name matching.
std::string LowerCopy(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

struct PixelBlockInfo {
    uint32_t pixelFormat = 0;
    uint32_t numMipmaps = 0;
    uint32_t mip0Width = 0;
    uint32_t mip0Height = 0;
    uint32_t mip0Offset = 0;
    uint32_t numPixels = 0;   // bytes of one face, all mips
    uint32_t numFaces = 1;
    int32_t paletteRef = -1;
    uint32_t channelTypes[4] = {0, 1, 2, 3};
    const uint8_t* pixels = nullptr;
};

// NiPixelData layout: 79 + 12*numMipmaps + numPixels*numFaces bytes,
// byte-verified on all 35,635 retail blocks (bully-re/docs/nft.md).
bool ReadPixelData(Reader& r, PixelBlockInfo& out) {
    out.pixelFormat = r.U32();
    r.U8();                    // bitsPerPixel
    r.I32();                   // rendererHint (-1)
    r.U32();                   // extraData value
    r.U8();                    // flags
    r.U32();                   // tiling
    r.U8();                    // srgb (20.3-specific)
    for (int k = 0; k < 4; k++) {
        out.channelTypes[k] = r.U32();
        r.U32();               // convention
        r.U8();                // bitsPerChannel
        r.U8();                // isSigned
    }
    out.paletteRef = r.I32();
    out.numMipmaps = r.U32();
    r.U32();                   // bytesPerPixel
    if (out.numMipmaps > 32) {
        r.Fail();
        return false;
    }
    for (uint32_t m = 0; m < out.numMipmaps && r.Ok(); m++) {
        const uint32_t w = r.U32();
        const uint32_t h = r.U32();
        const uint32_t off = r.U32();
        if (m == 0) {
            out.mip0Width = w;
            out.mip0Height = h;
            out.mip0Offset = off;
        }
    }
    out.numPixels = r.U32();
    out.numFaces = r.U32();
    if (out.numFaces == 0 || out.numFaces > 6) {
        r.Fail();
        return false;
    }
    const uint64_t total =
        static_cast<uint64_t>(out.numPixels) * out.numFaces;
    if (total > r.BytesLeft()) {
        r.Fail();
        return false;
    }
    out.pixels = r.Pos();
    r.Need(static_cast<size_t>(total));
    return r.Ok();
}

// NiPalette: u8 hasAlpha; u32 numEntries; numEntries x 4 bytes.
bool ReadPalette(Reader& r, std::vector<uint8_t>& entries, bool& hasAlpha) {
    hasAlpha = r.U8() != 0;
    const uint32_t n = r.U32();
    if (n > 65536 || !r.Need(static_cast<size_t>(n) * 4)) {
        r.Fail();
        return false;
    }
    entries.assign(r.Pos(), r.Pos() + static_cast<size_t>(n) * 4);
    return r.Ok();
}

// Decode mip level `mip` of `px` into rgba (w*h*4 bytes).
bool DecodeMipRGBA(const PixelBlockInfo& px, const uint8_t* palette,
                   size_t paletteEntries, bool paletteHasAlpha,
                   uint32_t mip, uint8_t* rgba) {
    // Rebuild this mip's dimensions by halving (NFT mipchains always descend
    // to 1x1; mip 0 dims are stored, so derive rather than re-read).
    uint32_t w = px.mip0Width >> mip;
    uint32_t h = px.mip0Height >> mip;
    if (w == 0) w = 1;
    if (h == 0) h = 1;

    // Offsets were captured per level during ReadPixelData; re-walk to get
    // this mip's offset (kept simple: only mip 0 is used by callers today,
    // but the math supports all levels).
    uint32_t offset = 0;
    {
        // Caller stored only mip 0 offset; recompute by summing level sizes
        // (standard Gamebryo mipchain packing).
        offset = 0;
        for (uint32_t m = 0; m < mip; m++) {
            uint32_t mw = px.mip0Width >> m, mh = px.mip0Height >> m;
            if (mw == 0) mw = 1;
            if (mh == 0) mh = 1;
            offset += MipLevelBytes(px.pixelFormat, mw, mh);
        }
    }

    const uint32_t levelBytes = MipLevelBytes(px.pixelFormat, w, h);
    if (levelBytes == 0) return false;
    if (static_cast<uint64_t>(offset) + levelBytes > px.numPixels) return false;
    const uint8_t* src = px.pixels + offset;

    switch (px.pixelFormat) {
        case NIF_PX_RGB8:
        case NIF_PX_RGBA8: {
            int order[4];
            ChannelOrder(px.channelTypes, order);
            const int n = (px.pixelFormat == NIF_PX_RGB8) ? 3 : 4;
            for (uint32_t i = 0; i < w * h; i++) {
                const uint8_t* s = src + i * n;
                rgba[i * 4 + 0] = s[order[0]];
                rgba[i * 4 + 1] = s[order[1]];
                rgba[i * 4 + 2] = s[order[2]];
                rgba[i * 4 + 3] = (n == 4) ? s[order[3]] : 255;
            }
            return true;
        }
        case NIF_PX_PAL8:
            if (!palette || paletteEntries == 0) return false;
            for (uint32_t i = 0; i < w * h; i++) {
                const uint32_t e = src[i];
                if (e >= paletteEntries) return false;
                std::memcpy(rgba + i * 4, palette + e * 4, 4);
                if (!paletteHasAlpha) rgba[i * 4 + 3] = 255;
            }
            return true;
        case NIF_PX_DXT1:
        case NIF_PX_DXT5: {
            const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
            const uint32_t stride = (px.pixelFormat == NIF_PX_DXT1) ? 8u : 16u;
            uint8_t block[64];
            for (uint32_t by = 0; by < bh; by++)
                for (uint32_t bx = 0; bx < bw; bx++) {
                    const uint8_t* b = src + (by * bw + bx) * stride;
                    if (px.pixelFormat == NIF_PX_DXT1)
                        DecodeDxt1Block(b, block);
                    else
                        DecodeDxt5Block(b, block);
                    // Sub-4x4 tail levels keep only the useful corner.
                    for (uint32_t y = 0; y < 4 && by * 4 + y < h; y++)
                        for (uint32_t x = 0; x < 4 && bx * 4 + x < w; x++)
                            std::memcpy(
                                rgba + ((by * 4 + y) * w + bx * 4 + x) * 4,
                                block + (y * 4 + x) * 4, 4);
                }
            return true;
        }
        default:
            return false;
    }
}

} // namespace

// Parsed container header + block table, shared by the pixel loader and the
// name-index scanner.
struct NftHeader {
    bool ok = false;
    std::string error;
    bool bigEndian = false;
    uint32_t numBlocks = 0;
    std::vector<uint16_t> blockTypes;
    std::vector<uint32_t> blockSizes;
    std::vector<std::string> strings;
    std::vector<std::string> typeNames;
    const uint8_t* blocksStart = nullptr;
    const uint8_t* dataEnd = nullptr;   // buffer end, for block bound checks
};

// Parses the NFT container header (identical to NIF) and leaves the reader
// positioned at the first block.
NftHeader ParseNftHeader(const uint8_t* data, size_t size) {
    NftHeader h;
    if (size < 64) {
        h.error = "file too small to be a NFT";
        return h;
    }
    Reader r(data, size);
    const void* nl = std::memchr(data, '\n', size < 128 ? size : 128);
    if (!nl || std::memcmp(data, "Gamebryo File Format", 20) != 0) {
        h.error = "not a Gamebryo NIF/NFT (bad header line)";
        return h;
    }
    r.SetBigEndian(false);
    r.SeekTo(static_cast<const uint8_t*>(nl) + 1);
    const uint32_t version = r.U32();
    if (version != 0x14030009u) {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "unsupported NFT version 0x%08x (expected 0x14030009)",
                      version);
        h.error = buf;
        return h;
    }
    const uint8_t endianness = r.U8();
    h.bigEndian = (endianness == 0);
    r.U32(); // user version
    // Block count is ALWAYS little-endian; endianness applies from the type
    // count onward.
    r.SetBigEndian(false);
    h.numBlocks = r.U32();
    r.SetBigEndian(h.bigEndian);
    if (h.numBlocks == 0 || h.numBlocks > 100000) {
        h.error = "implausible block count";
        return h;
    }
    const uint16_t numTypes = r.U16();
    if (numTypes == 0 || numTypes > 1000) {
        h.error = "implausible type count";
        return h;
    }
    h.typeNames.resize(numTypes);
    for (uint16_t i = 0; i < numTypes && r.Ok(); i++)
        h.typeNames[i] = r.PStr();
    h.blockTypes.resize(h.numBlocks);
    for (uint32_t i = 0; i < h.numBlocks && r.Ok(); i++)
        h.blockTypes[i] = r.U16();
    h.blockSizes.resize(h.numBlocks);
    for (uint32_t i = 0; i < h.numBlocks && r.Ok(); i++)
        h.blockSizes[i] = r.U32();
    const uint32_t numStrings = r.U32();
    r.U32(); // max string length
    h.strings.reserve(numStrings);
    for (uint32_t i = 0; i < numStrings && r.Ok(); i++)
        h.strings.push_back(r.PStr());
    const uint32_t numGroups = r.U32();
    for (uint32_t i = 0; i < numGroups && r.Ok(); i++)
        r.U32();
    if (!r.Ok()) {
        h.error = "header truncated";
        return h;
    }
    h.blocksStart = r.Pos();
    h.dataEnd = data + size;
    h.ok = true;
    return h;
}

// Walks the declared block ranges starting at `h.blocksStart`, invoking
// `visit(typeName, blockStart, blockSize)` per block. Mirrors the NIF
// loader's skip-by-size discipline. Offsets come from the header tables, so
// this never dereferences beyond what ParseNftHeader already validated.
template <typename Visit>
void ForEachNftBlock(const NftHeader& h, Visit visit) {
    const uint8_t* p = h.blocksStart;
    for (uint32_t i = 0; i < h.numBlocks; i++) {
        const uint32_t blockSize = h.blockSizes[i];
        // Truncation guard: stop when a block would run past the buffer.
        if (p + blockSize > h.dataEnd) break;
        const std::string typeName =
            h.blockTypes[i] < h.typeNames.size()
                ? h.typeNames[h.blockTypes[i]] : "";
        visit(i, typeName, p, blockSize);
        p += blockSize;
    }
}

NftTexture LoadNftFromMemory(const uint8_t* data, size_t size,
                             const std::string& wantName) {
    NftTexture tex;
    const NftHeader h = ParseNftHeader(data, size);
    if (!h.ok) {
        tex.error = h.error;
        return tex;
    }

    // --- Blocks: map NiSourceTexture refs, capture NiPixelData -------------
    // Pass 1: for each NiSourceTexture record its stripped name and its
    // pixelData block ref. Pass 2: decode the wanted pixel block (or the
    // first one when no name was requested).
    struct SrcTex { std::string name; int32_t pixelData = -1; };
    std::vector<SrcTex> srcTextures(h.numBlocks);
    std::vector<bool> pixelDone(h.numBlocks, false);
    std::vector<PixelBlockInfo> pixelBlocks(h.numBlocks);
    std::vector<uint8_t> palette;
    size_t paletteEntries = 0;
    bool paletteHasAlpha = false;
    int32_t firstPixelBlock = -1;
    int32_t wantedPixelBlock = -1;
    std::string wantKey = LowerCopy(wantName);

    ForEachNftBlock(h, [&](uint32_t bi, const std::string& typeName,
                           const uint8_t* blockStart, uint32_t blockSize) {
        if (typeName == "NiPixelData") {
            Reader br(blockStart, blockSize);
            br.SetBigEndian(h.bigEndian);
            PixelBlockInfo px;
            if (ReadPixelData(br, px) && br.Ok()) {
                pixelBlocks[bi] = px;
                pixelDone[bi] = true;
                if (firstPixelBlock < 0) firstPixelBlock =
                    static_cast<int32_t>(bi);
            }
        } else if (typeName == "NiPalette" && palette.empty()) {
            Reader br(blockStart, blockSize);
            br.SetBigEndian(h.bigEndian);
            if (ReadPalette(br, palette, paletteHasAlpha))
                paletteEntries = palette.size() / 4;
        } else if (typeName == "NiSourceTexture") {
            // name i32 | extra refs | controller i32 | u8 external |
            // fileName i32 | pixelData i32 | 3x u32 | 3x u8
            Reader br(blockStart, blockSize);
            br.SetBigEndian(h.bigEndian);
            br.I32();
            const int32_t numExtra = br.I32();
            if (numExtra >= 0 && numExtra <= 100000) br.Refs(numExtra);
            br.I32();
            br.U8(); // useExternal
            const int32_t nameIdx = br.I32();
            SrcTex st;
            if (br.Ok() && nameIdx >= 0 &&
                static_cast<uint32_t>(nameIdx) < h.strings.size())
                st.name = BaseTextureName(h.strings[nameIdx]);
            st.pixelData = br.I32();
            if (br.Ok()) srcTextures[bi] = st;
        }
    });

    // Resolve the wanted pixel block: exact source-name match first; fall
    // back to the first decodable NiPixelData.
    if (!wantKey.empty()) {
        for (uint32_t bi = 0; bi < h.numBlocks; bi++) {
            const SrcTex& st = srcTextures[bi];
            if (st.pixelData < 0) continue;
            if (LowerCopy(st.name) == wantKey) {
                wantedPixelBlock = st.pixelData;
                break;
            }
        }
    }
    if (wantedPixelBlock < 0 ||
        wantedPixelBlock >= static_cast<int32_t>(h.numBlocks) ||
        !pixelDone[wantedPixelBlock])
        wantedPixelBlock = firstPixelBlock;

    if (wantedPixelBlock < 0 || !pixelDone[wantedPixelBlock]) {
        tex.error = "no decodable NiPixelData in NFT";
        return tex;
    }
    const PixelBlockInfo& px = pixelBlocks[wantedPixelBlock];
    if (!wantKey.empty())
        tex.sourceName = wantName;

    tex.pixelFormat = px.pixelFormat;
    tex.numMipmaps = px.numMipmaps;
    tex.width = px.mip0Width;
    tex.height = px.mip0Height;
    if (tex.width == 0 || tex.height == 0 ||
        tex.width > 8192 || tex.height > 8192) {
        tex.error = "implausible texture dimensions";
        return tex;
    }

    tex.rgba.resize(static_cast<size_t>(tex.width) * tex.height * 4);
    if (!DecodeMipRGBA(px, palette.data(), paletteEntries, paletteHasAlpha,
                       0, tex.rgba.data())) {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "pixel decode failed (format %u, %ux%u)",
                      px.pixelFormat, tex.width, tex.height);
        tex.error = buf;
        return tex;
    }

    tex.error.clear(); // success
    return tex;
}

std::vector<std::string> ListNftTextureNames(const std::string& path) {
    std::vector<std::string> names;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return names;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(f);
        return names;
    }
    std::vector<uint8_t> buffer(static_cast<size_t>(size));
    const size_t got = std::fread(buffer.data(), 1, buffer.size(), f);
    std::fclose(f);
    if (got != buffer.size()) return names;

    const NftHeader h = ParseNftHeader(buffer.data(), buffer.size());
    if (!h.ok) return names;

    ForEachNftBlock(h, [&](uint32_t, const std::string& typeName,
                           const uint8_t* blockStart, uint32_t blockSize) {
        if (typeName != "NiSourceTexture") return;
        Reader br(blockStart, blockSize);
        br.SetBigEndian(h.bigEndian);
        br.I32(); // name
        int32_t numExtra = br.I32();
        if (numExtra >= 0 && numExtra <= 100000) br.Refs(numExtra);
        br.I32(); // controller
        br.U8();  // useExternal
        const int32_t nameIdx = br.I32();
        if (br.Ok() && nameIdx >= 0 &&
            static_cast<uint32_t>(nameIdx) < h.strings.size())
            names.push_back(BaseTextureName(h.strings[nameIdx]));
    });
    return names;
}

NftTexture LoadNftFile(const std::string& path,
                       const std::string& wantName) {
    NftTexture tex;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        tex.error = "cannot open file: " + path;
        return tex;
    }
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(f);
        tex.error = "empty file: " + path;
        return tex;
    }
    std::vector<uint8_t> buffer(static_cast<size_t>(size));
    const size_t got = std::fread(buffer.data(), 1, buffer.size(), f);
    std::fclose(f);
    if (got != buffer.size()) {
        tex.error = "short read: " + path;
        return tex;
    }
    return LoadNftFromMemory(buffer.data(), buffer.size(), wantName);
}

NftTexture MakePlaceholderTexture() {
    NftTexture tex;
    tex.error.clear(); // success
    tex.width = 4;
    tex.height = 4;
    tex.sourceName = "<placeholder>";
    tex.rgba.resize(4 * 4 * 4);
    // 2x2 checker of magenta/black, expanded to 4x4 pixels: unmissable.
    for (uint32_t y = 0; y < 4; y++) {
        for (uint32_t x = 0; x < 4; x++) {
            const bool magenta = ((x / 2) + (y / 2)) % 2 == 0;
            uint8_t* p = &tex.rgba[(y * 4 + x) * 4];
            if (magenta) {
                p[0] = 255; p[1] = 0;   p[2] = 255; p[3] = 255;
            } else {
                p[0] = 20;  p[1] = 20;  p[2] = 20;  p[3] = 255;
            }
        }
    }
    return tex;
}

} // namespace OpenBully
