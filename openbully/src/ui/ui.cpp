// UIRenderer implementation (Phase 6, Track 20).
//
// Everything here is intentionally plain: one program, two textures, one
// quad helper. bgfx transient buffers keep per-frame allocations off the
// heap; the HUD issues on the order of a few dozen quads per frame.

#include "ui.h"

#include "../platform/logger.h"
#include "../rendering/renderer.h"

#include <bx/math.h>

#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>

namespace OpenBully {

namespace {

// UI draws on view 10: above every world view (0 triangle overlay, 1 3D
// world, 2 debug boxes).
constexpr bgfx::ViewId kViewUi = bgfx::ViewId(10);

// UI vertex: pixel position, atlas UV, packed RGBA color. Color0 is 4x
// normalized uint8; the packed uint32 stores bytes in R,G,B,A memory order
// (little-endian packing: a<<24 | b<<16 | g<<8 | r), matching bgfx examples.
struct UIVertex {
    float    x, y;
    float    u, v;
    uint32_t rgba;
};

const bgfx::VertexLayout& UiVertexLayout() {
    static bgfx::VertexLayout layout = [] {
        bgfx::VertexLayout l;
        l.begin()
            .add(bgfx::Attrib::Position,  2, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Color0,    4, bgfx::AttribType::Uint8, true)
            .end();
        return l;
    }();
    return layout;
}

// Packs component bytes into the Color0 uint32 (see UIVertex note).
uint32_t PackRGBA(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return uint32_t(a) << 24 | uint32_t(b) << 16 | uint32_t(g) << 8 | uint32_t(r);
}

uint32_t ScaleAlpha(uint32_t rgba, float alpha) {
    const uint8_t a = uint8_t(float(rgba >> 24) * alpha);
    return (rgba & 0x00FFFFFFu) | (uint32_t(a) << 24);
}

// --- Built-in 5x7 bitmap font ------------------------------------------------
//
// Classic public-domain 5x7 glyph shapes for ASCII 0x20..0x5F (64 glyphs).
// One row byte per scanline, top row first; bit 0 = leftmost pixel column.
// Lowercase input maps to uppercase at draw time; characters outside the
// table render as blanks. (The game's proprietary Arial*.NFF faces stay a
// future upgrade — this satisfies the HUD text contract without a parser.)

struct GlyphRow { uint8_t rows[7]; };

const GlyphRow kFont5x7[64] = {
    {{0x00,0x00,0x00,0x00,0x00,0x00,0x00}}, // 0x20 ' '
    {{0x00,0x00,0x5F,0x00,0x00,0x00,0x00}}, // 0x21 '!'
    {{0x00,0x07,0x00,0x07,0x00,0x00,0x00}}, // 0x22 '"'
    {{0x14,0x7F,0x14,0x7F,0x14,0x00,0x00}}, // 0x23 '#'
    {{0x24,0x2A,0x7F,0x2A,0x12,0x00,0x00}}, // 0x24 '$'
    {{0x23,0x13,0x08,0x64,0x62,0x00,0x00}}, // 0x25 '%'
    {{0x36,0x49,0x55,0x22,0x50,0x00,0x00}}, // 0x26 '&'
    {{0x00,0x05,0x03,0x00,0x00,0x00,0x00}}, // 0x27 '\''
    {{0x00,0x1C,0x22,0x41,0x00,0x00,0x00}}, // 0x28 '('
    {{0x00,0x41,0x22,0x1C,0x00,0x00,0x00}}, // 0x29 ')'
    {{0x14,0x08,0x3E,0x08,0x14,0x00,0x00}}, // 0x2A '*'
    {{0x08,0x08,0x3E,0x08,0x08,0x00,0x00}}, // 0x2B '+'
    {{0x00,0x50,0x30,0x00,0x00,0x00,0x00}}, // 0x2C ','
    {{0x08,0x08,0x08,0x08,0x08,0x00,0x00}}, // 0x2D '-'
    {{0x00,0x60,0x60,0x00,0x00,0x00,0x00}}, // 0x2E '.'
    {{0x20,0x10,0x08,0x04,0x02,0x00,0x00}}, // 0x2F '/'
    {{0x3E,0x51,0x49,0x45,0x3E,0x00,0x00}}, // 0x30 '0'
    {{0x00,0x42,0x7F,0x40,0x00,0x00,0x00}}, // 0x31 '1'
    {{0x42,0x61,0x51,0x49,0x46,0x00,0x00}}, // 0x32 '2'
    {{0x21,0x41,0x45,0x4B,0x31,0x00,0x00}}, // 0x33 '3'
    {{0x18,0x14,0x12,0x7F,0x10,0x00,0x00}}, // 0x34 '4'
    {{0x27,0x45,0x45,0x45,0x39,0x00,0x00}}, // 0x35 '5'
    {{0x3C,0x4A,0x49,0x49,0x30,0x00,0x00}}, // 0x36 '6'
    {{0x01,0x71,0x09,0x05,0x03,0x00,0x00}}, // 0x37 '7'
    {{0x36,0x49,0x49,0x49,0x36,0x00,0x00}}, // 0x38 '8'
    {{0x06,0x49,0x49,0x29,0x1E,0x00,0x00}}, // 0x39 '9'
    {{0x00,0x36,0x36,0x00,0x00,0x00,0x00}}, // 0x3A ':'
    {{0x00,0x56,0x36,0x00,0x00,0x00,0x00}}, // 0x3B ';'
    {{0x08,0x14,0x22,0x41,0x00,0x00,0x00}}, // 0x3C '<'
    {{0x14,0x14,0x14,0x14,0x14,0x00,0x00}}, // 0x3D '='
    {{0x41,0x22,0x14,0x08,0x00,0x00,0x00}}, // 0x3E '>'
    {{0x02,0x01,0x51,0x09,0x06,0x00,0x00}}, // 0x3F '?'
    {{0x32,0x49,0x79,0x41,0x3E,0x00,0x00}}, // 0x40 '@'
    {{0x7E,0x11,0x11,0x11,0x7E,0x00,0x00}}, // 0x41 'A'
    {{0x7F,0x49,0x49,0x49,0x36,0x00,0x00}}, // 0x42 'B'
    {{0x3E,0x41,0x41,0x41,0x22,0x00,0x00}}, // 0x43 'C'
    {{0x7F,0x41,0x41,0x22,0x1C,0x00,0x00}}, // 0x44 'D'
    {{0x7F,0x49,0x49,0x49,0x41,0x00,0x00}}, // 0x45 'E'
    {{0x7F,0x09,0x09,0x09,0x01,0x00,0x00}}, // 0x46 'F'
    {{0x3E,0x41,0x49,0x49,0x7A,0x00,0x00}}, // 0x47 'G'
    {{0x7F,0x08,0x08,0x08,0x7F,0x00,0x00}}, // 0x48 'H'
    {{0x00,0x41,0x7F,0x41,0x00,0x00,0x00}}, // 0x49 'I'
    {{0x20,0x40,0x41,0x3F,0x01,0x00,0x00}}, // 0x4A 'J'
    {{0x7F,0x08,0x14,0x22,0x41,0x00,0x00}}, // 0x4B 'K'
    {{0x7F,0x40,0x40,0x40,0x40,0x00,0x00}}, // 0x4C 'L'
    {{0x7F,0x02,0x0C,0x02,0x7F,0x00,0x00}}, // 0x4D 'M'
    {{0x7F,0x04,0x08,0x10,0x7F,0x00,0x00}}, // 0x4E 'N'
    {{0x3E,0x41,0x41,0x41,0x3E,0x00,0x00}}, // 0x4F 'O'
    {{0x7F,0x09,0x09,0x09,0x06,0x00,0x00}}, // 0x50 'P'
    {{0x3E,0x41,0x51,0x21,0x5E,0x00,0x00}}, // 0x51 'Q'
    {{0x7F,0x09,0x19,0x29,0x46,0x00,0x00}}, // 0x52 'R'
    {{0x46,0x49,0x49,0x49,0x31,0x00,0x00}}, // 0x53 'S'
    {{0x01,0x01,0x7F,0x01,0x01,0x00,0x00}}, // 0x54 'T'
    {{0x3F,0x40,0x40,0x40,0x3F,0x00,0x00}}, // 0x55 'U'
    {{0x1F,0x20,0x40,0x20,0x1F,0x00,0x00}}, // 0x56 'V'
    {{0x7F,0x20,0x18,0x20,0x7F,0x00,0x00}}, // 0x57 'W'
    {{0x63,0x14,0x08,0x14,0x63,0x00,0x00}}, // 0x58 'X'
    {{0x03,0x04,0x78,0x04,0x03,0x00,0x00}}, // 0x59 'Y'
    {{0x61,0x51,0x49,0x45,0x43,0x00,0x00}}, // 0x5A 'Z'
    {{0x00,0x7F,0x41,0x41,0x00,0x00,0x00}}, // 0x5B '['
    {{0x02,0x04,0x08,0x10,0x20,0x00,0x00}}, // 0x5C '\\'
    {{0x00,0x41,0x41,0x7F,0x00,0x00,0x00}}, // 0x5D ']'
    {{0x04,0x02,0x01,0x02,0x04,0x00,0x00}}, // 0x5E '^'
    {{0x40,0x40,0x40,0x40,0x40,0x00,0x00}}, // 0x5F '_'
};

// Atlas layout: one 6x8 cell per glyph (5x7 bitmap + 1px gap right/bottom)
// in a single row of 64 cells -> 384x8 RGBA8. White pixels where a bit is
// set; alpha carries the shape so tinting works per quad.
constexpr uint32_t kAtlasCells   = 64;
constexpr uint32_t kAtlasCellW   = 6;
constexpr uint32_t kAtlasCellH   = 8;
constexpr uint32_t kAtlasWidth   = kAtlasCells * kAtlasCellW;   // 384
constexpr uint32_t kAtlasHeight  = kAtlasCellH;                 // 8

char GlyphIndexFor(char c) {
    if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A'); // lowercase -> uppercase
    if (c < 0x20 || c > 0x5F) return 0;                // unknown -> blank
    return char(c - 0x20);
}

} // namespace

// --- Lifecycle ----------------------------------------------------------------

bool UIRenderer::Init(const char* shaderRoot) {
    if (m_initialized) return true;
    if (!shaderRoot) {
        Logger::Error("UIRenderer::Init: null shader root");
        return false;
    }

    // Materialize the vertex layout before anything samples it.
    (void)UiVertexLayout();

    // Compiled UI shader binaries live next to the world shaders:
    // <shaderRoot>shaders/<backend>/vs_ui.bin + fs_ui.bin (shaderc targets
    // in CMakeLists.txt).
    const bgfx::Caps* caps = bgfx::getCaps();
    const char* subdir = nullptr;
    switch (caps->rendererType) {
        case bgfx::RendererType::Direct3D11: subdir = "direct3d11"; break;
        case bgfx::RendererType::Direct3D12: subdir = "direct3d12"; break;
        case bgfx::RendererType::Vulkan:     subdir = "vulkan";     break;
        case bgfx::RendererType::OpenGL:     subdir = "opengl";     break;
        default:                             subdir = nullptr;      break;
    }
    if (!subdir) {
        Logger::Error("UIRenderer::Init: no shader dir for renderer type %d",
                      int(caps->rendererType));
        return false;
    }
    const std::string dir = std::string(shaderRoot) + "shaders/" + subdir;
    m_program = Renderer::LoadProgramFiles(dir.c_str(), "vs_ui", "fs_ui");
    if (!bgfx::isValid(m_program)) {
        Logger::Error("UIRenderer::Init: failed to load UI program from %s",
                      dir.c_str());
        return false;
    }

    m_sampler = bgfx::createUniform("s_texUi", bgfx::UniformType::Sampler);
    if (!bgfx::isValid(m_sampler)) {
        Logger::Error("UIRenderer::Init: sampler uniform creation failed");
        return false;
    }

    // --- Font atlas (built-in 5x7) -----------------------------------------
    {
        std::vector<uint8_t> rgba(size_t(kAtlasWidth) * kAtlasHeight * 4u, 0);
        for (uint32_t g = 0; g < kAtlasCells; ++g) {
            for (uint32_t row = 0; row < 7; ++row) {
                const uint8_t bits = kFont5x7[g].rows[row];
                for (uint32_t col = 0; col < 5; ++col) {
                    if (!((bits >> col) & 0x1)) continue;
                    const uint32_t x = g * kAtlasCellW + col;
                    const uint32_t y = row;
                    uint8_t* px = &rgba[(size_t(y) * kAtlasWidth + x) * 4u];
                    px[0] = 255; px[1] = 255; px[2] = 255; px[3] = 255;
                }
            }
        }
        const bgfx::Memory* mem =
            bgfx::copy(rgba.data(), uint32_t(rgba.size()));
        // Point sampling: glyph pixels must stay crisp.
        m_fontTex = bgfx::createTexture2D(
            kAtlasWidth, kAtlasHeight, false, 1,
            bgfx::TextureFormat::RGBA8,
            BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT, mem);
        m_fontTexWidth  = kAtlasWidth;
        m_fontTexHeight = kAtlasHeight;
        if (!bgfx::isValid(m_fontTex)) {
            Logger::Error("UIRenderer::Init: font atlas upload failed");
            return false;
        }
    }

    // --- 1x1 white texture for solid rects ----------------------------------
    {
        const uint8_t white[4] = { 255, 255, 255, 255 };
        const bgfx::Memory* mem = bgfx::copy(white, sizeof(white));
        m_whiteTex = bgfx::createTexture2D(
            1, 1, false, 1, bgfx::TextureFormat::RGBA8,
            BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT, mem);
        if (!bgfx::isValid(m_whiteTex)) {
            Logger::Error("UIRenderer::Init: white texture upload failed");
            return false;
        }
    }

    m_initialized = true;
    Logger::Info("UIRenderer initialized (view %d, font atlas %ux%u)",
                 int(kViewUi), m_fontTexWidth, m_fontTexHeight);
    return true;
}

void UIRenderer::Shutdown() {
    if (bgfx::isValid(m_program))  bgfx::destroy(m_program);
    if (bgfx::isValid(m_sampler))  bgfx::destroy(m_sampler);
    if (bgfx::isValid(m_fontTex))  bgfx::destroy(m_fontTex);
    if (bgfx::isValid(m_whiteTex)) bgfx::destroy(m_whiteTex);
    m_program  = BGFX_INVALID_HANDLE;
    m_sampler  = BGFX_INVALID_HANDLE;
    m_fontTex  = BGFX_INVALID_HANDLE;
    m_whiteTex = BGFX_INVALID_HANDLE;
    m_initialized = false;
}

void UIRenderer::SetScreenSize(uint32_t width, uint32_t height) {
    m_screenWidth  = width;
    m_screenHeight = height;
}

void UIRenderer::BeginFrame() {
    if (!m_initialized || m_screenWidth == 0 || m_screenHeight == 0) return;

    bgfx::setViewRect(kViewUi, 0, 0, bgfx::BackbufferRatio::Equal);

    // Orthographic pixel projection, origin top-left. ZO (D3D) maps depth
    // [0,1], NO (GL/Vulkan) [-1,1]; y grows downward in both.
    const bgfx::Caps* caps = bgfx::getCaps();
    float ortho[16];
    if (caps->homogeneousDepth) {
        bx::mtxOrtho(ortho, 0.0f, float(m_screenWidth),
                            float(m_screenHeight), 0.0f,
                     -1.0f, 1.0f, 0.0f, caps->homogeneousDepth);
    } else {
        bx::mtxOrtho(ortho, 0.0f, float(m_screenWidth),
                            float(m_screenHeight), 0.0f,
                     0.0f, 1.0f, 0.0f, caps->homogeneousDepth);
    }
    bgfx::setViewTransform(kViewUi, nullptr, ortho); // identity view
}

// --- Primitives ----------------------------------------------------------------

void UIRenderer::SubmitQuad(float x, float y, float w, float h,
                            float u0, float v0, float u1, float v1,
                            bgfx::TextureHandle texture, uint32_t rgba) {
    if (!m_initialized || !bgfx::isValid(texture)) return;
    if (w <= 0.0f || h <= 0.0f) return;

    const UIVertex verts[4] = {
        { x,       y,       u0, v0, rgba },
        { x + w,   y,       u1, v0, rgba },
        { x + w,   y + h,   u1, v1, rgba },
        { x,       y + h,   u0, v1, rgba },
    };
    const uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 };

    bgfx::TransientVertexBuffer tvb;
    bgfx::TransientIndexBuffer  tib;
    bgfx::allocTransientBuffers(&tvb, UiVertexLayout(), 4, &tib, 6);
    std::memcpy(tvb.data, verts, sizeof(verts));
    std::memcpy(tib.data, indices, sizeof(indices));

    const uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
                         | BGFX_STATE_DEPTH_TEST_ALWAYS
                         | BGFX_STATE_BLEND_ALPHA
                         | BGFX_STATE_MSAA;
    bgfx::setState(state);
    bgfx::setVertexBuffer(0, &tvb);
    bgfx::setIndexBuffer(&tib);
    bgfx::setTexture(0, m_sampler, texture);
    bgfx::submit(kViewUi, m_program);
}

void UIRenderer::DrawRect(float x, float y, float w, float h, uint32_t rgba) {
    // 1x1 white texture; vertex color supplies the tint.
    SubmitQuad(x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f, m_whiteTex, rgba);
}

void UIRenderer::DrawRectTextured(float x, float y, float w, float h,
                                  bgfx::TextureHandle texture, uint32_t tint) {
    SubmitQuad(x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f, texture, tint);
}

float UIRenderer::MeasureText(const std::string& text) const {
    return float(text.size()) * kGlyphAdvance - (kGlyphAdvance - kGlyphWidth);
}

float UIRenderer::DrawText(float x, float y, const std::string& text, uint32_t rgba) {
    if (!m_initialized) return 0.0f;
    const float uStep = float(kAtlasCellW) / float(kAtlasWidth);
    const float v1    = float(kGlyphHeight) / float(kAtlasHeight);
    for (size_t i = 0; i < text.size(); ++i) {
        const char glyph = GlyphIndexFor(text[i]);
        const float u0 = float(uint8_t(glyph)) * uStep;
        SubmitQuad(x + float(i) * kGlyphAdvance, y,
                   kGlyphWidth, kGlyphHeight,
                   u0, 0.0f, u0 + float(kGlyphWidth) / float(kAtlasWidth), v1,
                   m_fontTex, rgba);
    }
    return MeasureText(text);
}

// --- HUD widgets -----------------------------------------------------------------

void UIRenderer::DrawHUD(const HUDState& hud) {
    if (!m_initialized || m_screenWidth == 0 || m_screenHeight == 0) return;

    const float W = float(m_screenWidth);
    const float H = float(m_screenHeight);
    // 1280x720 reference layout, mildly scale-locked for other resolutions.
    const float s   = std::min(1.5f, std::max(0.75f, H / 720.0f));
    const float pad = 24.0f * s;

    // --- Health bar: bottom-left ------------------------------------------
    {
        const float barW = 220.0f * s;
        const float barH = 20.0f * s;
        const float bx = pad;
        const float by = H - pad - barH;
        DrawRect(bx - 2.0f * s, by - 2.0f * s, barW + 4.0f * s, barH + 4.0f * s,
                 PackRGBA(25, 25, 28, 200));
        DrawRect(bx, by, barW, barH, PackRGBA(0, 0, 0, 180));
        float ratio = hud.maxHealth > 0
            ? float(hud.health) / float(hud.maxHealth) : 0.0f;
        ratio = std::min(1.0f, std::max(0.0f, ratio));
        // Green -> red as health drains.
        const uint8_t r = uint8_t(220.0f * (1.0f - ratio) + 20.0f);
        const uint8_t g = uint8_t(200.0f * ratio + 20.0f);
        DrawRect(bx + 1.0f * s, by + 1.0f * s,
                 std::max(0.0f, (barW - 2.0f * s) * ratio), barH - 2.0f * s,
                 PackRGBA(r, g, 30, 235));
    }

    // --- Track 25: stamina bar (under the health bar) -------------------------
    {
        const float barW = 220.0f * s;
        const float barH = 10.0f * s;
        const float bx = pad;
        const float by = H - pad - 20.0f * s - 2.0f * s - 2.0f * s - barH;
        DrawRect(bx - 2.0f * s, by - 2.0f * s, barW + 4.0f * s, barH + 4.0f * s,
                 PackRGBA(25, 25, 28, 200));
        DrawRect(bx, by, barW, barH, PackRGBA(0, 0, 0, 180));
        float ratio = hud.maxStamina > 0
            ? float(hud.stamina) / float(hud.maxStamina) : 0.0f;
        ratio = std::min(1.0f, std::max(0.0f, ratio));
        // Yellow pool; flashes red when empty-ish.
        DrawRect(bx + 1.0f * s, by + 1.0f * s,
                 std::max(0.0f, (barW - 2.0f * s) * ratio), barH - 2.0f * s,
                 PackRGBA(235, 200, 60, 235));
    }

    // --- Track 25: combat state label (under the stamina bar) -----------------
    {
        const char* label = nullptr;
        uint32_t color = PackRGBA(255, 255, 255, 220);
        switch (hud.combatState) {
            case 1: label = "DODGE";  color = PackRGBA(120, 200, 255, 255); break;
            case 2: label = "BLOCK";  color = PackRGBA(150, 150, 255, 255); break;
            case 3: label = "COUNTER!"; color = PackRGBA(255, 220, 80, 255); break;
            default: break; // Normal: no label
        }
        if (label)
            DrawText(pad, H - pad - 20.0f * s - 2.0f * s - 2.0f * s - 10.0f * s
                          - 2.0f * s - 2.0f * s - 2.0f * s - 8.0f * s,
                     label, color);
    }

    // --- Wanted stars: top-right -------------------------------------------
    const float right = W - pad;
    {
        const float cell   = 3.0f * s;
        const float starW  = 5.0f * cell;
        const float gap    = 5.0f * s;
        const float starY  = pad;
        for (int i = 0; i < 5; ++i) {
            const float cx = right - (5.0f - float(i)) * (starW + gap)
                             + starW * 0.5f;
            const float cy = starY + starW * 0.5f;
            // 5-row diamond (row widths 1,3,5,3,1 cells) reads as a star
            // badge at HUD size; filled = wanted level reached.
            static const int kRows[5] = { 1, 3, 5, 3, 1 };
            const uint32_t rgba = i < hud.wanted
                ? PackRGBA(255, 205, 60, 255)    // filled: gold
                : PackRGBA(90, 90, 95, 170);     // empty: dim outline
            for (int r = 0; r < 5; ++r) {
                const float w = float(kRows[r]) * cell;
                DrawRect(cx - w * 0.5f, cy - 2.5f * cell + float(r) * cell,
                         w, cell, rgba);
            }
        }
    }

    // --- Clock + money: top-right, under the stars --------------------------
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%02d:%02d",
                      (hud.clockMinutes / 60) % 24, hud.clockMinutes % 60);
        const std::string clock(buf);
        const float y = pad + 15.0f * s + 8.0f * s;
        DrawText(right - MeasureText(clock), y, clock, PackRGBA(255, 255, 255, 235));

        std::snprintf(buf, sizeof(buf), "$%d", hud.money);
        const std::string money(buf);
        const float y2 = y + kGlyphHeight * s + 6.0f * s;
        DrawText(right - MeasureText(money), y2, money, PackRGBA(120, 230, 120, 255));
    }

    // --- Minimap: bottom-right ----------------------------------------------
    {
        const float mapSize = 160.0f * s;
        const float mx = W - pad - mapSize;
        const float my = H - pad - mapSize;
        DrawRect(mx - 2.0f * s, my - 2.0f * s, mapSize + 4.0f * s, mapSize + 4.0f * s,
                 PackRGBA(25, 25, 28, 200));
        DrawRect(mx, my, mapSize, mapSize, PackRGBA(10, 14, 18, 170));
        // Border frame (4 rects).
        const uint32_t border = PackRGBA(210, 210, 215, 220);
        const float bw = 2.0f * s;
        DrawRect(mx - bw, my - bw, mapSize + 2.0f * bw, bw, border);
        DrawRect(mx - bw, my + mapSize, mapSize + 2.0f * bw, bw, border);
        DrawRect(mx - bw, my, bw, mapSize, border);
        DrawRect(mx + mapSize, my, bw, mapSize, border);

        const float cx = mx + mapSize * 0.5f;
        const float cy = my + mapSize * 0.5f;
        const float viewRadius = 25.0f; // world units shown edge-to-edge
        const float scale = (mapSize * 0.5f - 8.0f * s) / viewRadius;

        // Screen-up = Jimmy's facing direction (yaw 0 = +Z world).
        const float sinY = std::sin(hud.playerYaw);
        const float cosY = std::cos(hud.playerYaw);
        const float halfIn = mapSize * 0.5f - 4.0f * s;

        for (int i = 0; i < hud.npcDotCount; ++i) {
            const float relX = hud.npcDots[i].x - hud.playerX;
            const float relZ = hud.npcDots[i].z - hud.playerZ;
            const float sx = relX * cosY - relZ * sinY;  // right component
            const float sy = relX * sinY + relZ * cosY;  // forward component
            const float px = cx + sx * scale;
            const float py = cy - sy * scale;
            if (std::fabs(px - cx) > halfIn || std::fabs(py - cy) > halfIn)
                continue; // outside the box (no wrap-around yet)
            DrawRect(px - 2.0f, py - 2.0f, 4.0f, 4.0f,
                     PackRGBA(110, 200, 255, 255)); // NPC: cyan dot
        }

        // Player dot at the center (white).
        DrawRect(cx - 2.5f * s, cy - 2.5f * s, 5.0f * s, 5.0f * s,
                 PackRGBA(255, 255, 255, 255));
    }

    // --- Track 24: grapple status banner (top-center) -----------------------
    if (hud.grappleState > 0) {
        const char* label = "";
        uint32_t rgba = PackRGBA(255, 210, 80, 255);
        switch (hud.grappleState) {
        case 1: label = "GRAPPLE: U=HEADLOCK I=KNEE O=THROW G=RELEASE"; break;
        case 2: label = "GRAPPLED - MASH U/I/O/SPACE!"; rgba = PackRGBA(255, 90, 90, 255); break;
        case 3: label = "BREAKING FREE!"; rgba = PackRGBA(120, 255, 120, 255); break;
        default: break;
        }
        std::string text(label);
        const float tx = W * 0.5f - MeasureText(text) * 0.5f;
        const float ty = pad;
        DrawRect(tx - 4.0f * s, ty - 4.0f * s,
                 MeasureText(text) + 8.0f * s, kGlyphHeight + 8.0f * s,
                 PackRGBA(20, 20, 24, 190));
        DrawText(tx, ty, text, rgba);
        if (hud.grappleState == 2) {
            char mb[32];
            std::snprintf(mb, sizeof(mb), "MASH %d/10", hud.grappleMash);
            const std::string mash(mb);
            const float my2 = ty + (kGlyphHeight + 8.0f) * s + 6.0f * s;
            DrawText(W * 0.5f - MeasureText(mash) * 0.5f, my2, mash,
                     PackRGBA(255, 255, 255, 235));
        }
    }

    // --- Weapon slots: bottom-left, above the health bar (Track 26) ---------
    {
        const float y = H - pad - 20.0f * s - kGlyphHeight * s - 8.0f * s;
        float x = pad;
        for (int i = 0; i < HUDState::kMaxWeaponSlots; ++i) {
            const HUDState::WeaponSlotInfo& slot = hud.weaponSlots[i];
            char buf[24];
            if (slot.valid) {
                if (slot.ammo > 0)
                    std::snprintf(buf, sizeof(buf), "%d:%s %d", i + 1, slot.name, slot.ammo);
                else
                    std::snprintf(buf, sizeof(buf), "%d:%s %d", i + 1, slot.name, slot.durability);
            } else {
                std::snprintf(buf, sizeof(buf), "%d:-", i + 1);
            }
            const std::string label(buf);
            const uint32_t rgba = slot.valid
                ? (slot.equipped ? PackRGBA(255, 210, 60, 255)   // equipped: gold
                                 : PackRGBA(200, 200, 205, 220))
                : PackRGBA(110, 110, 115, 140);                  // empty: dim
            DrawText(x, y, label, rgba);
            x += MeasureText(label) + 10.0f * s;
        }
    }
}

} // namespace OpenBully
