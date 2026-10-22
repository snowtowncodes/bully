#pragma once

// OpenBully: NFT (NetImmerse/Gamebryo texture) loader.
//
// A `.nft` is NOT a DDS or raw pixel dump: it is a full Gamebryo NIF container
// ("Gamebryo File Format, Version 20.3.0.9") whose blocks are texture-only:
// NiSourceTexture / NiPixelData / NiPalette / NiStringExtraData /
// NiIntegerExtraData / NiSourceCubeMap (see bully-re/docs/nft.md, byte-verified
// on all 35,635 NiPixelData blocks of the retail archive).
//
// NiPixelData layout (79 + 12*numMipmaps + numPixels*numFaces bytes):
//   u32 pixelFormat; u8 bitsPerPixel; i32 rendererHint; u32 extraData;
//   u8 flags; u32 tiling; u8 srgb;
//   4x { u32 type; u32 convention; u8 bitsPerChannel; u8 isSigned; }
//   i32 palette; u32 numMipmaps; u32 bytesPerPixel;
//   numMipmaps x { u32 width; u32 height; u32 offset; }
//   u32 numPixels; u32 numFaces; u8 pixels[numPixels*numFaces]
//
// Formats present in the retail archive (pixelFormat enum):
//   0 = RGB8, 1 = RGBA8, 2 = PAL8 (+NiPalette), 4 = DXT1, 6 = DXT5
//   (5 = DXT3 never shipped). 131 CS_* files are big-endian NIFs, but their
//   DXT payloads stay little-endian (measured; only NIF fields are swapped).
//
// This loader extracts the pixel bytes and decodes level 0 to RGBA8. bgfx
// receives the RGBA8 blob via bgfx::createTexture2D (a BGFX_TEXTURE_FORMAT
// handle); no DXTc upload path is needed for the Track-11 milestone.

#include <cstdint>
#include <string>
#include <vector>

namespace OpenBully {

// Decoded texture: RGBA8, top-first rows, mip level 0 only.
struct NftTexture {
    std::string error;            // empty when ok
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;    // width * height * 4 bytes
    // Diagnostics
    uint32_t pixelFormat = 0;     // raw Gamebryo PixelFormat enum (see header)
    uint32_t numMipmaps = 0;
    std::string sourceName;       // NiSourceTexture file name (original .tga)
};

// Parse a NFT (Gamebryo texture container) from memory and decode mip level 0
// to RGBA8. `data` must remain valid only for the call.
// When `wantName` is non-empty, decodes the NiPixelData referenced by the
// NiSourceTexture whose stripped name matches (case-insensitive); when empty
// or unmatched, decodes the first NiPixelData in the file.
NftTexture LoadNftFromMemory(const uint8_t* data, size_t size,
                             const std::string& wantName = {});

// Convenience: read the file at `path` and parse it.
NftTexture LoadNftFile(const std::string& path,
                       const std::string& wantName = {});

// Quick scan: returns the stripped names of every NiSourceTexture in the file
// (the original .tga base names). Used to build the texture-name -> NFT-file
// index over the extracted dictionary; does NOT decode pixels.
std::vector<std::string> ListNftTextureNames(const std::string& path);

// Build the 4x4 magenta/black placeholder texture used when a texture name
// cannot be resolved (proves the sampling pipeline is live).
NftTexture MakePlaceholderTexture();

} // namespace OpenBully
