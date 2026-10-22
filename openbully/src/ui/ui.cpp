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

    // --- Track 37: minimap (top-right) + relocated stars/clock/money ---------
    const float right = W - pad;
    const float mapSize = 150.0f * s;
    const float hudTopY = pad + (hud.minimapVisible ? mapSize + 8.0f * s : 0.0f);

    if (hud.minimapVisible) {
        const float mx = W - pad - mapSize;
        const float my = pad;

        // Background: dark gray, alpha 0.7.
        DrawRect(mx - 2.0f * s, my - 2.0f * s, mapSize + 4.0f * s, mapSize + 4.0f * s,
                 PackRGBA(25, 25, 28, 200));
        DrawRect(mx, my, mapSize, mapSize, PackRGBA(40, 40, 45, 178));

        // Border frame.
        const uint32_t border = PackRGBA(210, 210, 215, 220);
        const float bw = 2.0f * s;
        DrawRect(mx - bw, my - bw, mapSize + 2.0f * bw, bw, border);
        DrawRect(mx - bw, my + mapSize, mapSize + 2.0f * bw, bw, border);
        DrawRect(mx - bw, my, bw, mapSize, border);
        DrawRect(mx + mapSize, my, bw, mapSize, border);

        const float cx = mx + mapSize * 0.5f;
        const float cy = my + mapSize * 0.5f;
        const float viewRadius = 25.0f;
        const float scale = (mapSize * 0.5f - 8.0f * s) / viewRadius;

        // Map rotates so player faces up (player-up mode).
        const float sinY = std::sin(hud.playerYaw);
        const float cosY = std::cos(hud.playerYaw);
        const float halfIn = mapSize * 0.5f - 4.0f * s;

        // NPCs: color-coded dots (yellow=neutral, red=hostile/prefect, green=friendly).
        for (int i = 0; i < hud.npcDotCount; ++i) {
            const float relX = hud.npcDots[i].x - hud.playerX;
            const float relZ = hud.npcDots[i].z - hud.playerZ;
            const float sx = relX * cosY - relZ * sinY;
            const float sy = relX * sinY + relZ * cosY;
            const float px = cx + sx * scale;
            const float py = cy - sy * scale;
            if (std::fabs(px - cx) > halfIn || std::fabs(py - cy) > halfIn)
                continue;
            uint32_t color;
            switch (hud.npcDots[i].type) {
                case 1:  color = PackRGBA(255, 80, 80, 255);   break; // red: hostile
                case 2:  color = PackRGBA(80, 220, 80, 255);   break; // green: friendly
                default: color = PackRGBA(255, 220, 80, 255);  break; // yellow: neutral
            }
            DrawRect(px - 2.0f, py - 2.0f, 4.0f, 4.0f, color);
        }

        // Vehicles: cyan squares.
        for (int i = 0; i < hud.vehicleDotCount; ++i) {
            const float relX = hud.vehicleDots[i].x - hud.playerX;
            const float relZ = hud.vehicleDots[i].z - hud.playerZ;
            const float sx = relX * cosY - relZ * sinY;
            const float sy = relX * sinY + relZ * cosY;
            const float px = cx + sx * scale;
            const float py = cy - sy * scale;
            if (std::fabs(px - cx) > halfIn || std::fabs(py - cy) > halfIn)
                continue;
            DrawRect(px - 3.0f, py - 3.0f, 6.0f, 6.0f, PackRGBA(0, 220, 255, 255));
        }

        // Mission objective: gold diamond star marker.
        if (hud.objectiveActive) {
            const float relX = hud.objectiveX - hud.playerX;
            const float relZ = hud.objectiveZ - hud.playerZ;
            const float sx = relX * cosY - relZ * sinY;
            const float sy = relX * sinY + relZ * cosY;
            const float px = cx + sx * scale;
            const float py = cy - sy * scale;
            if (std::fabs(px - cx) <= halfIn && std::fabs(py - cy) <= halfIn) {
                const float cell = 2.0f * s;
                static const int kRows[5] = { 1, 3, 5, 3, 1 };
                const uint32_t gold = PackRGBA(255, 215, 0, 255);
                for (int r = 0; r < 5; ++r) {
                    const float w = float(kRows[r]) * cell;
                    DrawRect(px - w * 0.5f, py - 2.5f * cell + float(r) * cell,
                             w, cell, gold);
                }
            }
        }

        // Player: white upward-pointing triangle (map rotates, so always faces up).
        const float ts = 2.0f * s;
        DrawRect(cx - 0.5f * ts, cy - 1.5f * ts, ts, ts,
                 PackRGBA(255, 255, 255, 255));
        DrawRect(cx - ts, cy - 0.5f * ts, ts * 2.0f, ts,
                 PackRGBA(255, 255, 255, 255));
        DrawRect(cx - 1.5f * ts, cy + 0.5f * ts, ts * 3.0f, ts,
                 PackRGBA(255, 255, 255, 255));
    }

    // --- Wanted stars: top-right, below minimap -----------------------------
    {
        const float cell   = 3.0f * s;
        const float starW  = 5.0f * cell;
        const float gap    = 5.0f * s;
        const float starY  = hudTopY;
        for (int i = 0; i < 5; ++i) {
            const float scx = right - (5.0f - float(i)) * (starW + gap) + starW * 0.5f;
            const float scy = starY + starW * 0.5f;
            static const int kRows[5] = { 1, 3, 5, 3, 1 };
            const uint32_t rgba = i < hud.wanted
                ? PackRGBA(255, 205, 60, 255)
                : PackRGBA(90, 90, 95, 170);
            for (int r = 0; r < 5; ++r) {
                const float w = float(kRows[r]) * cell;
                DrawRect(scx - w * 0.5f, scy - 2.5f * cell + float(r) * cell,
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
        const float y = hudTopY + 15.0f * s + 8.0f * s;
        DrawText(right - MeasureText(clock), y, clock, PackRGBA(255, 255, 255, 235));

        std::snprintf(buf, sizeof(buf), "$%d", hud.money);
        const std::string money(buf);
        const float y2 = y + kGlyphHeight * s + 6.0f * s;
        DrawText(right - MeasureText(money), y2, money, PackRGBA(120, 230, 120, 255));
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

    // --- Track 39: slingshot crosshair (aim mode) + ammo counter ------------
    if (hud.aiming) {
        const float cx = W * 0.5f, cy = H * 0.5f;
        const uint32_t cross = PackRGBA(255, 255, 255, 220);
        const uint32_t dot   = PackRGBA(255, 80, 80, 255);
        DrawRect(cx - 11.0f * s, cy - 1.0f * s, 8.0f * s, 2.0f * s, cross);
        DrawRect(cx + 3.0f * s, cy - 1.0f * s, 8.0f * s, 2.0f * s, cross);
        DrawRect(cx - 1.0f * s, cy - 11.0f * s, 2.0f * s, 8.0f * s, cross);
        DrawRect(cx - 1.0f * s, cy + 3.0f * s, 2.0f * s, 8.0f * s, cross);
        DrawRect(cx - 1.5f * s, cy - 1.5f * s, 3.0f * s, 3.0f * s, dot);
        if (hud.slingshotAmmo >= 0) {
            char ambuf[32];
            std::snprintf(ambuf, sizeof(ambuf), "ROCKS %d/%d",
                          hud.slingshotAmmo, hud.slingshotCap);
            const std::string am(ambuf);
            DrawText(cx - MeasureText(am) * 0.5f, cy + 20.0f * s, am,
                     PackRGBA(255, 230, 140, 240));
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

// --- Track 30: dialogue box ------------------------------------------------------

namespace {

// Word-wraps `text` into lines no wider than maxWidth (bitmap-font metrics).
// Single words longer than maxWidth overflow (no mid-word split).
std::vector<std::string> WrapDialogueText(const UIRenderer& ui,
                                          const std::string& text,
                                          float maxWidth) {
    std::vector<std::string> out;
    std::string line;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find(' ', pos);
        if (end == std::string::npos) end = text.size();
        const std::string word = text.substr(pos, end - pos);
        if (!word.empty()) {
            if (!line.empty()) {
                if (ui.MeasureText(line + " " + word) > maxWidth) {
                    out.push_back(line);
                    line = word;
                } else {
                    line += " " + word;
                }
            } else {
                line = word;
            }
        }
        if (end == text.size()) break;
        pos = end + 1;
    }
    if (!line.empty() || out.empty()) out.push_back(line);
    return out;
}

} // namespace

void UIRenderer::DrawDialogueBox(const char* speaker,
                                 const std::string& text,
                                 bool showAdvanceHint,
                                 const std::vector<std::string>& choices,
                                 int selectedChoice) {
    if (!m_initialized || m_screenWidth == 0 || m_screenHeight == 0) return;

    const float W = float(m_screenWidth);
    const float H = float(m_screenHeight);
    const float s = std::min(1.5f, std::max(0.75f, H / 720.0f));

    // Bottom-center panel: ~800x200 at the 1280x720 reference layout.
    const float boxW = std::min(800.0f * s, W - 32.0f * s);
    const float boxH = 200.0f * s;
    const float bx   = 0.5f * (W - boxW);
    const float by   = H - boxH - 24.0f * s;
    const float padX = 20.0f * s;

    // Semi-transparent black panel + subtle border.
    DrawRect(bx, by, boxW, boxH, PackRGBA(8, 8, 12, 200));
    const uint32_t border = PackRGBA(190, 190, 200, 160);
    const float bw = 2.0f * s;
    DrawRect(bx, by, boxW, bw, border);
    DrawRect(bx, by + boxH - bw, boxW, bw, border);
    DrawRect(bx, by, bw, boxH, border);
    DrawRect(bx + boxW - bw, by, bw, boxH, border);

    // Speaker name: top of the box on an accent strip.
    const std::string name(speaker ? speaker : "");
    const float nameY = by + 10.0f * s;
    if (!name.empty()) {
        const float nameW = MeasureText(name) + 12.0f * s;
        DrawRect(bx + padX - 6.0f * s, nameY - 4.0f * s, nameW,
                 kGlyphHeight + 8.0f * s, PackRGBA(70, 110, 170, 230));
        DrawText(bx + padX, nameY, name, PackRGBA(255, 255, 255, 255));
    }

    // Body text: word-wrapped under the speaker strip.
    const float textMaxW = boxW - 2.0f * padX;
    const float textY = nameY + (kGlyphHeight + 10.0f) * s;
    const float lineH = (kGlyphHeight + 4.0f) * s;
    const uint32_t white = PackRGBA(240, 240, 245, 255);
    const std::vector<std::string> wrapped = WrapDialogueText(*this, text, textMaxW);
    float y = textY;
    for (const std::string& ln : wrapped) {
        if (y + lineH > by + boxH - 6.0f * s) break; // box overflow guard
        DrawText(bx + padX, y, ln, white);
        y += lineH;
    }

    // Choices as button rows (numbered); the selected row is highlighted.
    if (!choices.empty()) {
        const float rowH   = 16.0f * s;
        const float rowGap = 4.0f * s;
        // Rows sit at the bottom of the box, below the body text area.
        float cy = by + boxH - 10.0f * s - rowH * float(choices.size())
                   - rowGap * float(std::max(0, int(choices.size()) - 1));
        for (int i = 0; i < int(choices.size()); ++i) {
            char prefix[8];
            std::snprintf(prefix, sizeof(prefix), "%d. ", i + 1);
            const std::string label = std::string(prefix) + choices[i];
            const bool selected = (i == selectedChoice);
            const float rowW = std::min(textMaxW, MeasureText(label) + 16.0f * s);
            if (selected)
                DrawRect(bx + padX - 4.0f * s, cy - 2.0f * s, rowW, rowH,
                         PackRGBA(60, 100, 160, 220));
            DrawText(bx + padX, cy + (rowH - kGlyphHeight) * 0.5f - 2.0f * s, label,
                     selected ? PackRGBA(255, 240, 160, 255)
                              : PackRGBA(210, 210, 220, 230));
            cy += rowH + rowGap;
        }
        DrawText(bx + boxW - padX - MeasureText("ARROWS+ENTER"),
                 by + boxH - (kGlyphHeight + 8.0f) * s, "ARROWS+ENTER",
                 PackRGBA(160, 160, 170, 200));
    } else if (showAdvanceHint) {
        DrawText(bx + boxW - padX - MeasureText("E: NEXT"),
                 by + boxH - (kGlyphHeight + 8.0f) * s, "E: NEXT",
                 PackRGBA(255, 220, 120, 220));
    }
}

// --- Track 40: quest journal overlay -----------------------------------------

namespace {

// Screen-space layout of the journal panel. `blocks` hold each mission block's
// Y range in viewport-local (pre-scroll) coordinates so the hit-test and the
// draw always agree.
struct JournalLayout {
    float panelX, panelY, panelW, panelH;
    float contentTop, contentBottom;   // scrolling viewport bounds (px)
    float padX, lineH, s;
    struct Block { float top; float bottom; };
    std::vector<Block> blocks;
    float totalH;                      // content height before clamping
};

JournalLayout ComputeJournalLayout(uint32_t W, uint32_t H,
                                   const std::vector<JournalMissionEntry>& missions) {
    JournalLayout L;
    L.s = std::min(1.5f, std::max(0.75f, float(H) / 720.0f));
    L.panelW = std::min(900.0f * L.s, float(W) - 48.0f * L.s);
    L.panelH = float(H) * 0.78f;
    L.panelX = 0.5f * (float(W) - L.panelW);
    L.panelY = 0.5f * (float(H) - L.panelH);
    L.padX = 24.0f * L.s;
    L.lineH = (UIRenderer::kGlyphHeight + 4.0f) * L.s;
    const float hintH = (UIRenderer::kGlyphHeight + 10.0f) * L.s;
    // Fixed footer under the viewport: a details pane (label + up to 3
    // wrapped lines) plus the hint row.
    const float detailsH = L.lineH * 4.0f + 16.0f * L.s;
    const float headerDiv = L.panelY + 14.0f * L.s +
                            (UIRenderer::kGlyphHeight + 10.0f) * L.s;
    L.contentTop = headerDiv + 10.0f * L.s;
    L.contentBottom = L.panelY + L.panelH - detailsH - hintH;

    float y = 0.0f;
    for (const JournalMissionEntry& e : missions) {
        const float top = y;
        y += L.lineH + 2.0f * L.s;                 // title row + gap
        if (!e.description.empty()) y += L.lineH;  // description blurb
        y += L.lineH * float(e.objectives.size()); // objective rows
        y += 6.0f * L.s;                           // block bottom padding
        L.blocks.push_back({top, y});
        y += 14.0f * L.s;                          // divider + gap to the next block
    }
    L.totalH = y;
    return L;
}

// Scroll bounds: [0, max(0, contentHeight - viewportHeight)].
float JournalScrollClamped(const JournalLayout& L, float scrollPx) {
    const float maxScroll = std::max(0.0f, L.totalH - (L.contentBottom - L.contentTop));
    return std::max(0.0f, std::min(scrollPx, maxScroll));
}

} // namespace

int UIRenderer::JournalHitTest(const std::vector<JournalMissionEntry>& missions,
                               float scrollPx, float mx, float my) const {
    if (!m_initialized || m_screenWidth == 0 || m_screenHeight == 0) return -1;
    const JournalLayout L = ComputeJournalLayout(m_screenWidth, m_screenHeight, missions);
    if (mx < L.panelX || mx >= L.panelX + L.panelW) return -1;
    const float sc = JournalScrollClamped(L, scrollPx);
    for (int i = 0; i < int(L.blocks.size()); ++i) {
        const float top = L.contentTop + L.blocks[size_t(i)].top - sc;
        const float bot = L.contentTop + L.blocks[size_t(i)].bottom - sc;
        if (my >= top && my < bot) return i;
    }
    return -1;
}

float UIRenderer::ClampJournalScroll(const std::vector<JournalMissionEntry>& missions,
                                     float scrollPx) const {
    if (!m_initialized || m_screenWidth == 0 || m_screenHeight == 0)
        return std::max(0.0f, scrollPx);
    const JournalLayout L =
        ComputeJournalLayout(m_screenWidth, m_screenHeight, missions);
    const float maxScroll =
        std::max(0.0f, L.totalH - (L.contentBottom - L.contentTop));
    return std::max(0.0f, std::min(scrollPx, maxScroll));
}

void UIRenderer::DrawQuestJournal(const std::vector<JournalMissionEntry>& missions,
                                  int selectedIndex, float scrollPx) {
    if (!m_initialized || m_screenWidth == 0 || m_screenHeight == 0) return;

    const JournalLayout L = ComputeJournalLayout(m_screenWidth, m_screenHeight, missions);
    scrollPx = JournalScrollClamped(L, scrollPx);
    const float s = L.s;
    const float px0 = L.panelX, py0 = L.panelY;
    const float panelW = L.panelW, panelH = L.panelH;
    const float padX = L.padX, lineH = L.lineH;

    // Full-screen dim backdrop so the panel reads over any scene.
    DrawRect(0.0f, 0.0f, float(m_screenWidth), float(m_screenHeight),
             PackRGBA(0, 0, 0, 150));

    // Centered panel with a parchment-gold border.
    DrawRect(px0, py0, panelW, panelH, PackRGBA(10, 10, 14, 225));
    const uint32_t border = PackRGBA(200, 190, 140, 180);
    const float bw = 2.0f * s;
    DrawRect(px0, py0, panelW, bw, border);
    DrawRect(px0, py0 + panelH - bw, panelW, bw, border);
    DrawRect(px0, py0, bw, panelH, border);
    DrawRect(px0 + panelW - bw, py0, bw, panelH, border);

    // Header.
    const float headerY = py0 + 14.0f * s;
    const std::string title = "QUEST JOURNAL";
    DrawRect(px0 + padX - 8.0f * s, headerY - 4.0f * s,
             MeasureText(title) + 16.0f * s, kGlyphHeight + 8.0f * s,
             PackRGBA(60, 50, 20, 220));
    DrawText(px0 + padX, headerY, title, PackRGBA(255, 220, 120, 255));
    DrawText(px0 + panelW - padX - MeasureText("J / ESC: CLOSE"), headerY,
             "J / ESC: CLOSE", PackRGBA(170, 170, 175, 210));
    DrawRect(px0 + padX, L.contentTop - 10.0f * s, panelW - 2.0f * padX, 1.5f * s,
             PackRGBA(120, 110, 70, 200));

    // --- Mission list -------------------------------------------------------
    if (missions.empty()) {
        const std::string msg = "No active missions";
        DrawText(px0 + 0.5f * (panelW - MeasureText(msg)),
                 0.5f * (py0 + L.contentTop + L.contentBottom - kGlyphHeight * s),
                 msg, PackRGBA(200, 200, 205, 220));
    } else {
        for (int mi = 0; mi < int(missions.size()); ++mi) {
            const JournalMissionEntry& e = missions[size_t(mi)];
            const bool selected = (mi == selectedIndex);
            const float topPx = L.contentTop + L.blocks[size_t(mi)].top - scrollPx;
            const float botPx = L.contentTop + L.blocks[size_t(mi)].bottom - scrollPx;
            // Skip blocks scrolled entirely out of the viewport.
            if (botPx < L.contentTop - lineH || topPx > L.contentBottom + lineH)
                continue;

            const uint32_t stateColor =
                e.state == 0 ? PackRGBA(255, 215, 70, 255) :   // active: yellow
                e.state == 1 ? PackRGBA(90, 220, 90, 255) :    // complete: green
                               PackRGBA(240, 80, 80, 255);     // failed: red

            // Selection highlight, drawn behind the block's text.
            if (selected)
                DrawRect(px0 + 6.0f * s, topPx - 3.0f * s,
                         panelW - 12.0f * s, (botPx - topPx) + 1.0f * s,
                         PackRGBA(255, 255, 255, 18));

            float y = topPx;
            // Title row: colored icon square, title, and the progress counter.
            DrawRect(px0 + padX, y + 1.0f * s, 8.0f * s, 8.0f * s, stateColor);
            const std::string label =
                e.state == 1 ? e.name + " - COMPLETE" :
                e.state == 2 ? e.name + " - FAILED" : e.name;
            DrawText(px0 + padX + 14.0f * s, y, label, stateColor);
            size_t done = 0;
            for (const JournalMissionEntry::ObjectiveRow& o : e.objectives)
                if (o.complete) ++done;
            char prog[16];
            std::snprintf(prog, sizeof(prog), "%zu/%zu", done, e.objectives.size());
            DrawText(px0 + panelW - padX - MeasureText(prog), y, prog, stateColor);
            y += lineH + 2.0f * s;

            // Description blurb.
            if (!e.description.empty()) {
                DrawText(px0 + padX + 14.0f * s, y, e.description,
                         PackRGBA(190, 190, 198, 220));
                y += lineH;
            }

            // Objective rows: checkmarks; the current objective highlighted.
            for (int oi = 0; oi < int(e.objectives.size()); ++oi) {
                const JournalMissionEntry::ObjectiveRow& o = e.objectives[size_t(oi)];
                const bool isCurrent = (oi == e.currentObjectiveIndex);
                const std::string row = std::string(o.complete ? "[x] " : "[ ] ") + o.text;
                if (isCurrent) {
                    DrawRect(px0 + padX + 8.0f * s, y - 2.0f * s,
                             MeasureText(row) + 10.0f * s, lineH,
                             PackRGBA(70, 60, 20, 200));
                    DrawText(px0 + padX + 14.0f * s, y, row,
                             PackRGBA(255, 255, 230, 255));
                } else {
                    DrawText(px0 + padX + 14.0f * s, y, row,
                             o.complete ? PackRGBA(120, 170, 120, 200)
                                        : PackRGBA(160, 160, 168, 200));
                }
                y += lineH;
            }

            // Divider between missions.
            if (mi + 1 < int(missions.size()))
                DrawRect(px0 + padX, botPx + 4.0f * s, panelW - 2.0f * padX, 1.5f * s,
                         PackRGBA(90, 85, 60, 170));
        }
    }

    // --- Details pane: the selected mission's full description -------------
    {
        const float dTop = L.contentBottom + 4.0f * s;
        DrawRect(px0 + padX, dTop, panelW - 2.0f * padX, 1.0f * s,
                 PackRGBA(90, 85, 60, 150));
        const float dY = dTop + 6.0f * s;
        if (selectedIndex >= 0 && selectedIndex < int(missions.size())) {
            const JournalMissionEntry& e = missions[size_t(selectedIndex)];
            const uint32_t col =
                e.state == 1 ? PackRGBA(90, 220, 90, 255) :
                e.state == 2 ? PackRGBA(240, 80, 80, 255) :
                               PackRGBA(255, 215, 70, 255);
            DrawText(px0 + padX, dY, "> " + e.name, col);
            float ty = dY + lineH;
            const float maxW = panelW - 2.0f * padX - 14.0f * s;
            const std::vector<std::string> wrapped =
                WrapDialogueText(*this, e.description, maxW);
            for (size_t i = 0; i < wrapped.size() && i < 3; ++i) {
                DrawText(px0 + padX + 14.0f * s, ty, wrapped[i],
                         PackRGBA(220, 220, 228, 240));
                ty += lineH;
            }
        } else {
            DrawText(px0 + padX, dY, "SELECT A MISSION",
                     PackRGBA(170, 170, 175, 200));
        }
    }

    // Footer hint.
    DrawText(px0 + padX, py0 + panelH - (kGlyphHeight + 10.0f) * s,
             "ARROWS / WHEEL: SCROLL   CLICK: SELECT   J / ESC: CLOSE",
             PackRGBA(150, 150, 155, 190));
}

// --- Track 41: class system UI ---------------------------------------------------

// "Press E to attend [Class]" strip, bottom-center above the dialogue slot.
void UIRenderer::DrawClassPrompt(const char* classLabel) {
    if (!m_initialized || m_screenWidth == 0 || m_screenHeight == 0 || !classLabel)
        return;
    const float W = float(m_screenWidth);
    const float H = float(m_screenHeight);
    const float s = std::min(1.5f, std::max(0.75f, H / 720.0f));

    const std::string text = std::string("Press E to attend ") + classLabel;
    const float textW = MeasureText(text) + 24.0f * s;
    const float boxW  = textW + 24.0f * s;
    const float boxH  = (kGlyphHeight + 10.0f) * s;
    const float bx    = 0.5f * (W - boxW);
    const float by    = H * 0.62f;

    DrawRect(bx, by, boxW, boxH, PackRGBA(8, 8, 12, 200));
    const uint32_t border = PackRGBA(190, 190, 200, 150);
    const float bw = 1.5f * s;
    DrawRect(bx, by, boxW, bw, border);
    DrawRect(bx, by + boxH - bw, boxW, bw, border);
    DrawRect(bx, by, bw, boxH, border);
    DrawRect(bx + boxW - bw, by, bw, boxH, border);

    // "Press E" highlighted, the rest plain — mirrors the dialogue advance hint.
    const std::string key("Press E");
    DrawText(bx + 12.0f * s, by + 5.0f * s, key, PackRGBA(255, 220, 120, 255));
    DrawText(bx + 12.0f * s + MeasureText(key) + 6.0f * s, by + 5.0f * s,
             std::string("to attend ") + classLabel, PackRGBA(240, 240, 245, 255));
}

// Class schedule overlay (C key): every class, its window, location and
// completion counter; gold highlight on the class in session right now.
void UIRenderer::DrawClassSchedule(const std::vector<ClassScheduleEntry>& entries,
                                   const std::string& headerLine,
                                   const std::string& nextLine,
                                   const std::string& infoLine) {
    if (!m_initialized || m_screenWidth == 0 || m_screenHeight == 0) return;

    const float W = float(m_screenWidth);
    const float H = float(m_screenHeight);
    const float s = std::min(1.5f, std::max(0.75f, H / 720.0f));
    const float lineH = (kGlyphHeight + 4.0f) * s;

    // Slightly wider than the journal: rows carry days + time + location.
    const float panelW = std::min(860.0f * s, W - 32.0f * s);
    const float rowsH  = lineH * float(std::max<size_t>(entries.size(), 1));
    const float panelH = std::min(H - 60.0f * s,
                                  (kGlyphHeight + 46.0f) * s +   // header + hints
                                  rowsH + lineH * 2.0f);
    const float px0 = 0.5f * (W - panelW);
    const float py0 = 0.5f * (H - panelH);
    const float padX = 20.0f * s;

    DrawRect(0.0f, 0.0f, W, H, PackRGBA(0, 0, 0, 150));
    DrawRect(px0, py0, panelW, panelH, PackRGBA(10, 10, 14, 225));
    const uint32_t border = PackRGBA(200, 190, 140, 180);
    const float bw = 2.0f * s;
    DrawRect(px0, py0, panelW, bw, border);
    DrawRect(px0, py0 + panelH - bw, panelW, bw, border);
    DrawRect(px0, py0, bw, panelH, border);
    DrawRect(px0 + panelW - bw, py0, bw, panelH, border);

    // Header: title strip + day/time line + next-class countdown.
    const float headerY = py0 + 12.0f * s;
    DrawRect(px0 + padX - 8.0f * s, headerY - 4.0f * s,
             MeasureText("CLASS SCHEDULE") + 16.0f * s, kGlyphHeight + 8.0f * s,
             PackRGBA(60, 50, 20, 220));
    DrawText(px0 + padX, headerY, "CLASS SCHEDULE", PackRGBA(255, 220, 120, 255));
    DrawText(px0 + panelW - padX - MeasureText("C / ESC: CLOSE"), headerY,
             "C / ESC: CLOSE", PackRGBA(170, 170, 175, 210));
    float y = headerY + (kGlyphHeight + 6.0f) * s;
    if (!headerLine.empty())
        DrawText(px0 + padX, y, headerLine, PackRGBA(200, 200, 208, 240));
    y += lineH;
    if (!nextLine.empty())
        DrawText(px0 + padX, y, nextLine, PackRGBA(150, 200, 255, 255));
    y += lineH;
    DrawRect(px0 + padX, y, panelW - 2.0f * padX, 1.5f * s,
             PackRGBA(120, 110, 70, 200));
    y += 6.0f * s;

    if (entries.empty()) {
        DrawText(px0 + 0.5f * (panelW - MeasureText("No classes defined")),
                 y + 0.5f * lineH, "No classes defined",
                 PackRGBA(200, 200, 205, 220));
    } else {
        // Column x positions: NAME | DAYS | TIME | LOCATION | DONE
        const float xName = px0 + padX;
        const float xDays = xName + MeasureText("Photography 1") + 16.0f * s;
        const float xTime = xDays + MeasureText("MON/TUE/WED/THU/FRI") + 16.0f * s;
        const float xLoc  = xTime + MeasureText("09:00-10:00") + 16.0f * s;
        for (const ClassScheduleEntry& e : entries) {
            const uint32_t nameCol =
                e.fullyComplete ? PackRGBA(90, 220, 90, 255) :
                e.availableNow  ? PackRGBA(255, 215, 70, 255) :
                                  PackRGBA(225, 225, 232, 235);
            if (e.availableNow)
                DrawRect(px0 + 6.0f * s, y - 2.0f * s, panelW - 12.0f * s, lineH,
                         PackRGBA(255, 220, 90, 26));
            DrawText(xName, y, e.name, nameCol);
            DrawText(xDays, y, e.days, PackRGBA(160, 160, 170, 210));
            DrawText(xTime, y, e.time, PackRGBA(160, 160, 170, 210));
            DrawText(xLoc,  y, e.location, PackRGBA(190, 190, 198, 220));
            char prog[24];
            std::snprintf(prog, sizeof(prog), "%d/%d%s", e.completions,
                          e.sessionsRequired, e.fullyComplete ? " OK" : "");
            const std::string progStr(prog);
            DrawText(px0 + panelW - padX - MeasureText(progStr), y, progStr, nameCol);
            y += lineH;
        }
    }

    // Footer: respect/progress info line + close hint.
    const float footY = py0 + panelH - (kGlyphHeight + 10.0f) * s;
    if (!infoLine.empty())
        DrawText(px0 + padX, footY, infoLine, PackRGBA(255, 200, 90, 230));
}

// In-class overlay: attention meter (sweep + band), beat tallies, objective
// checklist and the pass/fail banner. Drawn over a dimmed world (the player
// is "inside" the classroom during the session).
void UIRenderer::DrawClassSession(const ClassSessionView& view) {
    if (!m_initialized || m_screenWidth == 0 || m_screenHeight == 0) return;

    const float W = float(m_screenWidth);
    const float H = float(m_screenHeight);
    const float s = std::min(1.5f, std::max(0.75f, H / 720.0f));
    const float lineH = (kGlyphHeight + 4.0f) * s;

    // Full-screen dim (deeper than menus: the room transition).
    DrawRect(0.0f, 0.0f, W, H, PackRGBA(0, 0, 0, 170));

    const float panelW = std::min(700.0f * s, W - 32.0f * s);
    const float rowsH  = lineH * float(view.objectives.size() + 2);
    const float panelH = std::min(H - 60.0f * s,
                                  (kGlyphHeight + 92.0f) * s + rowsH);
    const float px0 = 0.5f * (W - panelW);
    const float py0 = 0.5f * (H - panelH);
    const float padX = 20.0f * s;
    const uint32_t border = PackRGBA(200, 190, 140, 180);
    const float bw = 2.0f * s;

    DrawRect(px0, py0, panelW, panelH, PackRGBA(12, 12, 18, 235));
    DrawRect(px0, py0, panelW, bw, border);
    DrawRect(px0, py0 + panelH - bw, panelW, bw, border);
    DrawRect(px0, py0, bw, panelH, border);
    DrawRect(px0 + panelW - bw, py0, bw, panelH, border);

    // Header: class name + room.
    float y = py0 + 14.0f * s;
    DrawText(px0 + padX, y, view.className, PackRGBA(255, 220, 120, 255));
    if (!view.location.empty()) {
        DrawText(px0 + panelW - padX - MeasureText(view.location), y,
                 view.location, PackRGBA(170, 170, 178, 210));
    }
    y += lineH + 4.0f * s;

    // Attention meter: full-width bar, target band highlighted, marker tick.
    const float meterX = px0 + padX;
    const float meterW = panelW - 2.0f * padX;
    const float meterH = 14.0f * s;
    DrawRect(meterX, y, meterW, meterH, PackRGBA(30, 30, 40, 255));
    const float bandX = meterX + view.bandLo * meterW;
    const float bandW = std::max(1.0f, (view.bandHi - view.bandLo) * meterW);
    DrawRect(bandX, y, bandW, meterH, PackRGBA(70, 160, 70, 220));
    const float markerX = meterX + view.marker * meterW;
    DrawRect(markerX - 1.5f * s, y - 3.0f * s, 3.0f * s, meterH + 6.0f * s,
             PackRGBA(255, 255, 255, 255));
    y += meterH + 6.0f * s;
    DrawText(meterX, y, "E when the marker is in the green zone",
             PackRGBA(180, 180, 188, 220));
    y += lineH;

    // Tallies: needs X / slipped Y.
    char tally[64];
    std::snprintf(tally, sizeof(tally), "Correct: %d/%d    Slips: %d/%d",
                  view.hits, view.hitGoal, view.misses, view.missGoal);
    DrawText(meterX, y, tally, PackRGBA(210, 210, 220, 240));
    y += lineH + 4.0f * s;

    // Objective beats (checked as they complete).
    for (size_t i = 0; i < view.objectives.size(); ++i) {
        const bool done = int(i) < view.objectivesDone;
        const std::string row = std::string(done ? "[x] " : "[ ] ") +
                                view.objectives[i];
        DrawText(meterX + 6.0f * s, y, row,
                 done ? PackRGBA(120, 200, 120, 230)
                      : PackRGBA(170, 170, 178, 210));
        y += lineH;
    }

    // Result banner.
    if (view.result == 1) {
        const std::string pass = "CLASS PASSED!";
        DrawRect(px0 + padX - 6.0f * s, y + 2.0f * s,
                 MeasureText(pass) + 12.0f * s, lineH, PackRGBA(30, 90, 30, 230));
        DrawText(px0 + padX, y + 2.0f * s, pass, PackRGBA(140, 255, 140, 255));
        if (!view.rewardSummary.empty()) {
            y += lineH + 2.0f * s;
            DrawText(px0 + padX, y, view.rewardSummary,
                     PackRGBA(255, 220, 120, 255));
        }
    } else if (view.result == 2) {
        const std::string fail = "CLASS FAILED - retry when scheduled";
        DrawRect(px0 + padX - 6.0f * s, y + 2.0f * s,
                 MeasureText(fail) + 12.0f * s, lineH, PackRGBA(90, 30, 30, 230));
        DrawText(px0 + padX, y + 2.0f * s, fail, PackRGBA(255, 120, 120, 255));
    }

    // Footer hint.
    DrawText(px0 + padX, py0 + panelH - (kGlyphHeight + 8.0f) * s,
             "E: FOCUS   ESC: LEAVE CLASS",
             PackRGBA(150, 150, 155, 190));
}

} // namespace OpenBully
