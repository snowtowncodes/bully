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
#include <vector>

namespace OpenBully {

// Track 40: one mission row for the quest journal overlay (plain data; the
// Engine flattens MissionManager state into this each frame). Not-yet-started
// missions are filtered out; the list is ordered active, then complete,
// then failed.
struct JournalMissionEntry {
    // 0 = active (yellow), 1 = complete (green), 2 = failed (red).
    int state = 0;
    std::string name;                 // mission title
    std::string description;          // one-line blurb (first objective text)
    struct ObjectiveRow {
        std::string text;
        bool complete = false;
    };
    std::vector<ObjectiveRow> objectives;
    // Index of the highlighted "current" objective, or -1 when the mission is
    // done/failed (no current objective).
    int currentObjectiveIndex = -1;
    // Text of the current objective (Engine writes it alongside the index so
    // the journal can show the headline line without re-flattening).
    std::string currentObjective;
};

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
    // Track 37: type = 0 neutral (yellow), 1 hostile/prefect (red), 2 friendly (green).
    static constexpr int kMaxMinimapDots = 64;
    struct MinimapDot { float x = 0.0f; float z = 0.0f; int type = 0; };
    MinimapDot npcDots[kMaxMinimapDots] = {};
    int npcDotCount = 0;

    // Track 37: vehicle dots on the minimap (cyan squares).
    static constexpr int kMaxMinimapVehicles = 16;
    struct VehicleDot { float x = 0.0f; float z = 0.0f; };
    VehicleDot vehicleDots[kMaxMinimapVehicles] = {};
    int vehicleDotCount = 0;

    // Track 37: active mission objective marker (gold star on the minimap).
    float objectiveX = 0.0f;
    float objectiveZ = 0.0f;
    bool  objectiveActive = false;

    // Track 37: minimap visibility toggle (M key).
    bool  minimapVisible = true;

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

    // Track 39: slingshot aim mode (draws the crosshair) + reserve ammo for
    // the equipped slingshot slot.
    bool aiming = false;
    int  slingshotAmmo   = -1; // -1 = no slingshot equipped
    int  slingshotCap    = 30;
};

// Track 41: one class row for the schedule overlay (plain data; the Engine
// flattens ClassManager state into this each frame while the overlay is open).
struct ClassScheduleEntry {
    std::string name;                 // "English 1"
    std::string location;             // "Main School Building - Room 101"
    std::string days;                 // "MON/WED/FRI"
    std::string time;                 // "09:00-10:00"
    int  completions      = 0;
    int  sessionsRequired = 5;
    bool availableNow     = false;    // schedule window covers now (highlight)
    bool fullyComplete    = false;    // completions >= sessionsRequired
};

// Track 41: plain-data snapshot of the live class session (the placeholder
// minigame the Engine drives; Phase 12 replaces the sweep/band game).
struct ClassSessionView {
    std::string className;            // "English 1"
    std::string location;             // building + room
    float marker  = 0.5f;             // attention meter sweep position 0..1
    float bandLo  = 0.40f;
    float bandHi  = 0.60f;
    int   hits    = 0;                // completed beats (band presses)
    int   misses  = 0;
    int   hitGoal   = 3;              // pass condition
    int   missGoal  = 3;              // fail condition
    std::vector<std::string> objectives; // one row per beat
    int   objectivesDone = 0;
    int   result = 0;                 // 0 in progress, 1 pass, 2 fail
    std::string rewardSummary;        // reward list shown on pass
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

    // Track 30: dialogue box. Bottom-center panel (~800x200 at 1280x720,
    // scaled by the same convention as DrawHUD), semi-transparent black,
    // speaker name at the top, typewriter-revealed text below (word-wrapped
    // to the box width), and choices rendered as button rows when present
    // (selectedChoice highlighted; -1 = no selection). An advance hint shows
    // while text remains ("E: NEXT").
    void DrawDialogueBox(const char* speaker,
                         const std::string& text,
                         bool showAdvanceHint,
                         const std::vector<std::string>& choices,
                         int selectedChoice);

    // Track 40: quest journal overlay (J key). Centered ~70%-width panel,
    // semi-transparent dark backdrop over the whole screen, mission list
    // (active yellow / complete green / failed red) with objective checkmarks
    // and a progress counter, the selected mission highlighted with its full
    // description, divider lines between missions, and a "No active missions"
    // message when the list is empty. scrollPx offsets the virtual content
    // upward (mouse wheel / arrows drive it in the Engine); selected index
    // -1 = no selection.
    void DrawQuestJournal(const std::vector<JournalMissionEntry>& missions,
                          int selectedIndex, float scrollPx);

    // Hit-tests a screen pixel against the mission blocks at the given scroll.
    // Returns the mission index, or -1 when the pixel is outside the panel.
    int JournalHitTest(const std::vector<JournalMissionEntry>& missions,
                       float scrollPx, float mx, float my) const;

    // Clamps a journal scroll offset into the valid range for the current
    // screen size + snapshot. Shared with the Engine so arrow/wheel scrolling
    // stops at the same bounds the renderer uses.
    float ClampJournalScroll(const std::vector<JournalMissionEntry>& missions,
                             float scrollPx) const;

    // Track 41: "Press E to attend [Class]" prompt. Bottom-center strip above
    // the dialogue box slot; drawn only while the Engine reports a nearby
    // scheduled class.
    void DrawClassPrompt(const char* classLabel);

    // Track 41: class schedule overlay (C key). Lists every class with its
    // weekly window, location and per-class completion counter; the header
    // carries the day label + next-available countdown line, the footer the
    // respect/total line.
    void DrawClassSchedule(const std::vector<ClassScheduleEntry>& entries,
                           const std::string& headerLine,
                           const std::string& nextLine,
                           const std::string& infoLine);

    // Track 41: in-class overlay (the Phase-12 placeholder minigame): the
    // attention meter sweep with its target band, beat/objective tallies and
    // the pass/fail banner once the session resolves.
    void DrawClassSession(const ClassSessionView& view);

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
