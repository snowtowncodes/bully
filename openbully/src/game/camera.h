#pragma once

// Camera: free-fly FPS-style camera (Phase 3 debug camera).
//
// Left-handed, Y-up to match Gamebryo geometry (NIF verts are Y-up LH; the
// renderer hands them to bgfx unmodified). Yaw rotates around +Y, pitch
// around the camera's local right axis; +pitch looks up.
//
// The view matrix is built with glm::lookAt (LH variant, matching the
// geometry handedness) and consumed by Renderer::DrawMesh3D.

#include "../game/input.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>

namespace OpenBully {

class Camera {
public:
    Camera() = default;

    // Position + initial orientation (radians). yaw=0 looks down +Z.
    Camera(const glm::vec3& position, float yaw, float pitch);

    // Integrates button movement and mouse look for `dt` seconds.
    void Update(float dt, const Input& input, bool ignoreMove = false);

    // 4x4 view transform (column-major float[16] layout, glm default).
    const glm::mat4& GetViewMatrix();

    // -- tuning ---------------------------------------------------------------
    void SetPosition(const glm::vec3& p) { m_position = p; m_viewDirty = true; }
    const glm::vec3& GetPosition() const { return m_position; }

    float GetYaw() const   { return m_yaw; }
    float GetPitch() const { return m_pitch; }

    // Re-aims yaw/pitch so the view direction points along `to` (a full
    // direction vector, e.g. focus - position). Used by the 3rd-person
    // follow camera (Track 15).
    void SetLookAt(const glm::vec3& to) {
        const float horiz = std::sqrt(to.x * to.x + to.z * to.z);
        m_yaw   = std::atan2(to.x, to.z);
        m_pitch = std::atan2(to.y, horiz);
        m_pitch = std::clamp(m_pitch, -kPitchLimit, kPitchLimit);
        m_viewDirty = true;
    }

    void SetMoveSpeed(float unitsPerSec)      { m_moveSpeed = unitsPerSec; }
    void SetSprintMultiplier(float m)         { m_sprintMultiplier = m; }
    void SetMouseSensitivity(float radPerPix) { m_mouseSensitivity = radPerPix; }
    float GetMouseSensitivity() const         { return m_mouseSensitivity; }

    static constexpr float kDefaultMoveSpeed      = 5.0f;    // units/sec
    static constexpr float kDefaultSprintMult     = 4.0f;
    static constexpr float kDefaultMouseSens      = 0.002f;  // rad/pixel
    static constexpr float kPitchLimit            = 1.5533f; // ~89 deg

private:
    void RebuildView() const;

    glm::vec3 m_position{0.0f, 5.0f, -10.0f};
    float     m_yaw              = 0.0f;
    float     m_pitch            = 0.0f;

    float     m_moveSpeed        = kDefaultMoveSpeed;
    float     m_sprintMultiplier = kDefaultSprintMult;
    float     m_mouseSensitivity = kDefaultMouseSens;

    mutable glm::mat4 m_view{1.0f};
    mutable bool      m_viewDirty = true;
};

} // namespace OpenBully
