#pragma once

// UIRenderer (Phase 6, Track 20): screen-space HUD rendering on bgfx view 10.
//
// Pixel coordinates with the origin at the top-left; an orthographic
// projection is pushed as the view transform each frame (BeginFrame). All
// primitives are single quads submitted with transient buffers through one
// tiny unlit program (vs_ui/fs_ui): texture * vertex color. Solid rects use
// a 1x1 white texture, text uses the built-in 5x7 bitmap font atlas, and
// arbitrary skins/textures pass their own handle. No depth test, alpha
// blended, drawn after all world views.
//
// The game-facing widgets (health bar, wanted stars, clock, money, minimap)
// consume a plain HUDState snapshot the Engine refreshes every frame via
// Engine::GetHUDState(); the UI layer never touches entities or SDL.

#include <bgfx/bgfx.h>

#include <cstdint>
#include <string>

namespace OpenBully {

// Plain-data HUD snapshot filled by the Engine each frame. Kept free of
// glm/SDL/entity types so the UI renderer only reads numbers.
struct HUDState {
    // Health (player HealthComponent).
    int health    = 100;
    int maxHealth = 100;
    // Wanted level 0..5.
    int wanted = 0;
    // Cash on hand (display only, debug-modified later).
    int money = 0;
    // Track 25: stamina pool (int-rounded for the bar) + combat state
    // (CombatState: 0 normal, 1 dodge, 2 block, 3 counter).
    int stamina = 100;
    int maxStamina = 100;
    int combatState = 0;
    // Game clock in minutes since midnight (480 = 08:00).
    int clockMinutes = 8 * 60;
    // Player world XZ + facing yaw (radians, 0 = +Z) for the minimap.
    float playerX   = 0.0f;
    float playerZ   = 0.0f;
    float playerYaw = 0.0f;
    // Track 24: grapple HUD status. 0 = none, 1 = grabbing (attacker),
    // 2 = grappled (target), 3 = breaking free.
    int   grappleState = 0;
    int   grappleMash  = 0;   // mash presses done toward the break-free count
    // Nearby NPC world XZ dots for the minimap.
    static constexpr int kMaxMinimapDots = 64;
    struct MinimapDot { float x = 0.0f; float z = 0.0f; };
    MinimapDot npcDots[kMaxMinimapDots] = {};
    int npcDotCount = 0;

    // Track 26: weapon inventory slots (bottom-left, above the health bar).
    static constexpr int kMaxWeaponSlots = 5;
    struct WeaponSlotInfo {
        bool valid = false;
        bool equipped = false;
        char name[16] = {};
        int  durability = 0;
        int  ammo = 0;
    };
    WeaponSlotInfo weaponSlots[kMaxWeaponSlots] = {};
};

class UIRenderer {
public:
    UIRenderer() = default;
    ~UIRenderer() = default;
    UIRenderer(const UIRenderer&) = delete;
    UIRenderer& operator=(const UIRenderer&) = delete;

    // Creates the UI program + textures. Call after Renderer::Init (bgfx
    // must be live). `shaderRoot` matches Renderer::Init's convention (the
    // engine passes SDL_GetBasePath()); shaders load from
    // <shaderRoot>shaders/<backend>/vs_ui.bin + fs_ui.bin. Failure is
    // non-fatal for the engine (HUD just stays off).
    bool Init(const char* shaderRoot);
    void Shutdown();
    bool IsInitialized() const { return m_initialized; }

    // Backbuffer size in pixels; also drives the ortho projection. Call at
    // init and whenever the window resizes.
    void SetScreenSize(uint32_t width, uint32_t height);

    // Per-frame setup: refreshes the view-10 transform/rect. Call once per
    // frame before the first Draw* (Engine::Render does it before DrawHUD).
    void BeginFrame();

    // --- Primitives (pixels, origin top-left, immediate submit) ------------

    // Solid colored rectangle.
    void DrawRect(float x, float y, float w, float h, uint32_t rgba);
    // Textured rectangle; tint multiplies the texture (0xffffffff = untinted).
    void DrawRectTextured(float x, float y, float w, float h,
                          bgfx::TextureHandle texture, uint32_t tint);
    // Bitmap-font text (built-in 5x7 face). Returns the advance width.
    float DrawText(float x, float y, const std::string& text, uint32_t rgba);
    float MeasureText(const std::string& text) const;

    // Draws the full HUD (health bar / stars / clock / money / minimap) from
    // the snapshot. Scales widget placement off the backbuffer height so the
    // 1280x720 reference layout holds at other resolutions.
    void DrawHUD(const HUDState& hud);

    // Glyph metrics (pixels).
    static constexpr float kGlyphWidth   = 5.0f;
    static constexpr float kGlyphHeight  = 7.0f;
    static constexpr float kGlyphAdvance = 6.0f;   // 5px glyph + 1px gap

private:
    // Packs one quad into transient buffers and submits it on view 10.
    void SubmitQuad(float x, float y, float w, float h,
                    float u0, float v0, float u1, float v1,
                    bgfx::TextureHandle texture, uint32_t rgba);

    bgfx::ProgramHandle m_program  = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_sampler  = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle m_fontTex  = BGFX_INVALID_HANDLE; // 5x7 glyph atlas
    bgfx::TextureHandle m_whiteTex = BGFX_INVALID_HANDLE; // 1x1 for solid rects
    uint32_t m_fontTexWidth  = 0;
    uint32_t m_fontTexHeight = 0;
    uint32_t m_screenWidth   = 0;
    uint32_t m_screenHeight  = 0;
    bool     m_initialized   = false;
};

} // namespace OpenBully
