#include "camera.h"

#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <cmath>

namespace OpenBully {

Camera::Camera(const glm::vec3& position, float yaw, float pitch)
    : m_position(position), m_yaw(yaw), m_pitch(pitch) {}

void Camera::Update(float dt, const Input& input, bool ignoreMove) {
    if (dt <= 0.0f) return;

    // --- Mouse look ----------------------------------------------------------
    const int mdx = input.MouseDX();
    const int mdy = input.MouseDY();
    if (mdx != 0 || mdy != 0) {
        m_yaw   += float(mdx) * m_mouseSensitivity;
        m_pitch -= float(mdy) * m_mouseSensitivity; // screen Y grows downward
        // Wrap yaw to keep the value bounded over long sessions.
        if (m_yaw >  glm::pi<float>()) m_yaw -= glm::two_pi<float>();
        if (m_yaw < -glm::pi<float>()) m_yaw += glm::two_pi<float>();
        m_pitch = std::clamp(m_pitch, -kPitchLimit, kPitchLimit);
        m_viewDirty = true;
    }

    // --- Movement (strafe plane is horizontal; Y handled by Up/Down) ---------
    glm::vec3 move(0.0f);
    const float cy = std::cos(m_yaw), sy = std::sin(m_yaw);
    if (!ignoreMove) {
    // Forward is horizontal-only (classic FPS walk), yaw=0 faces +Z.
    if (input.IsDown(Button::Forward)) move += glm::vec3(sy, 0.0f, cy);
    if (input.IsDown(Button::Back))    move -= glm::vec3(sy, 0.0f, cy);
    // Right = forward x up, i.e. (cos(yaw), 0, -sin(yaw)).
    if (input.IsDown(Button::Left))    move -= glm::vec3(cy, 0.0f, -sy);
    if (input.IsDown(Button::Right))   move += glm::vec3(cy, 0.0f, -sy);
    if (input.IsDown(Button::Up))      move += glm::vec3(0.0f, 1.0f, 0.0f);
    if (input.IsDown(Button::Down))    move -= glm::vec3(0.0f, 1.0f, 0.0f);
    }

    if (move.x != 0.0f || move.y != 0.0f || move.z != 0.0f) {
        move = glm::normalize(move);
        const float speed = m_moveSpeed *
            (input.IsDown(Button::Sprint) ? m_sprintMultiplier : 1.0f);
        m_position += move * (speed * dt);
        m_viewDirty = true;
    }
}

const glm::mat4& Camera::GetViewMatrix() {
    if (m_viewDirty) RebuildView();
    return m_view;
}

void Camera::RebuildView() const {
    // Forward from yaw/pitch (yaw=0 -> +Z, pitch=0 -> horizontal).
    const float cp = std::cos(m_pitch);
    const float cy = std::cos(m_yaw), sy = std::sin(m_yaw);
    const glm::vec3 dir(sy * cp, std::sin(m_pitch), cy * cp);
    const glm::vec3 target = m_position + dir;

    // LH look-at to match Gamebryo geometry handedness.
    m_view = glm::lookAtLH(m_position, target, glm::vec3(0.0f, 1.0f, 0.0f));
    m_viewDirty = false;
}

} // namespace OpenBully
