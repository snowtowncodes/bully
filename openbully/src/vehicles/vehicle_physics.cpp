// OpenBully: arcade vehicle physics (Phase 8, Track 32). See header.

#include "vehicle_physics.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace OpenBully {

namespace {
constexpr float kPi = 3.14159265358979f;

float WrapAngle(float a) {
    while (a >  kPi) a -= 2.0f * kPi;
    while (a < -kPi) a += 2.0f * kPi;
    return a;
}
} // namespace

glm::vec3 VehiclePhysics::Forward() const {
    // Matches Transform's yaw convention (engine.cpp: yaw 0 = +Z, +yaw = ...).
    return { std::sin(yaw), 0.0f, std::cos(yaw) };
}

glm::vec3 VehiclePhysics::Right() const {
    return { std::cos(yaw), 0.0f, -std::sin(yaw) };
}

void VehiclePhysics::Update(float dt, float steerInput, float throttle, float brake) {
    if (dt <= 0.0f) return;
    dt = std::min(dt, 0.05f); // hitch clamp: integrator never explodes

    // --- Wheel layout (fixed local offsets) ---
    const float hx = trackWidth * 0.5f, hz = wheelBase * 0.5f;
    wheels[WheelFL].localOffset = { -hx, 0.0f,  hz };
    wheels[WheelFR].localOffset = {  hx, 0.0f,  hz };
    wheels[WheelRL].localOffset = { -hx, 0.0f, -hz };
    wheels[WheelRR].localOffset = {  hx, 0.0f, -hz };

    UpdateWheels();
    ApplySuspension(dt);
    if (traceFrames > 0) {
        std::printf("[vtrace] after-susp vel=(%.3f,%.3f,%.3f) contacts=%d "
                    "lenFL=%.2f lenFR=%.2f lenRL=%.2f lenRR=%.2f\n",
                    velocity.x, velocity.y, velocity.z, wheelContacts,
                    wheels[0].suspensionLength, wheels[1].suspensionLength,
                    wheels[2].suspensionLength, wheels[3].suspensionLength);
    }
    ApplyDrive(dt, steerInput, throttle, brake);
    if (traceFrames > 0) {
        std::printf("[vtrace] after-drive vel=(%.3f,%.3f,%.3f) speedAlong=%.3f "
                    "thr=%.1f brk=%.1f steerIn=%.1f dt=%.4f\n",
                    velocity.x, velocity.y, velocity.z, speedAlongFacing,
                    throttle, brake, steerInput, dt);
    }
    Integrate(dt);
    if (traceFrames > 0) {
        std::printf("[vtrace] after-integ vel=(%.3f,%.3f,%.3f) pos=(%.2f,%.2f,%.2f)\n",
                    velocity.x, velocity.y, velocity.z,
                    position.x, position.y, position.z);
        --traceFrames;
    }
    UpdateOrientation(dt, throttle, brake);
}

// --- Wheels: raycast down from each wheel's world position -------------------
void VehiclePhysics::UpdateWheels() {
    wheelContacts = rearContacts = frontContacts = 0;
    const glm::vec3 fwd = Forward(), rgt = Right();

    for (int i = 0; i < 4; ++i) {
        WheelState& w = wheels[i];
        w.contact = false;
        w.groundNormal = glm::vec3(0.0f, 1.0f, 0.0f);

        // Ray origin above the chassis plane so hollow AABBs don't eat it.
        const glm::vec3 origin = position + rgt * w.localOffset.x
                                         + fwd * w.localOffset.z
                                         + glm::vec3(0.0f, 2.0f, 0.0f);
        Ray ray;
        ray.origin[0] = origin.x; ray.origin[1] = origin.y; ray.origin[2] = origin.z;
        ray.direction[0] = 0.0f; ray.direction[1] = -1.0f; ray.direction[2] = 0.0f;
        RayHit hit;
        if (!world || !world->Raycast(ray, hit)) {
            w.suspensionLength = restLength + wheelRadius + 0.1f; // full droop
            continue;
        }
        // Suspension length measured from the chassis plane down to ground.
        w.suspensionLength = (origin.y - hit.point[1]) - 2.0f;
        w.contactY = hit.point[1];
        w.groundNormal = glm::normalize(
            glm::vec3(hit.normal[0], hit.normal[1], hit.normal[2]));
        if (w.suspensionLength <= restLength + wheelRadius) {
            w.contact = true;
            ++wheelContacts;
            if (i >= WheelRL) ++rearContacts; else ++frontContacts;
        }
    }
}

// --- Suspension: spring-damper per contact wheel -----------------------------
// Contact model: a wheel is grounded whenever its down-ray hits within the
// full droop reach (restLength + wheelRadius). The spring compresses from the
// rest position: compression = (restLength + wheelRadius) - length, clamped
// so the force at the droop limit is zero and grows linearly as the chassis
// settles. This keeps the contact bit hysteresis-free (no flicker at the
// threshold) and gives a stable equilibrium INSIDE the contact band.
void VehiclePhysics::ApplySuspension(float dt) {
    for (int i = 0; i < 4; ++i) {
        WheelState& w = wheels[i];
        if (!w.contact) continue;

        const float compression = std::max(0.0f,
            (restLength + wheelRadius) - w.suspensionLength);
        float force = springK * compression - damperK * velocity.y;
        force = std::max(force, 0.0f); // springs push, never pull

        // Acceleration contribution (a = F/m); each wheel applies its share.
        velocity += w.groundNormal * (force / mass * dt);
    }
    // Settling: near-full compression bleeds downward velocity so the
    // chassis rests on the springs instead of micro-bouncing.
    float minLen = restLength * 10.0f;
    for (const WheelState& w : wheels)
        if (w.contact) minLen = std::min(minLen, w.suspensionLength);
    if (wheelContacts >= 2 && minLen < restLength * 0.8f && velocity.y < 0.0f)
        velocity.y *= std::max(0.0f, 1.0f - 14.0f * dt);
    // Any contact kills downward motion into the ground (hard stop) so the
    // vehicle can never sink through the road between spring frames.
    if (wheelContacts > 0 && velocity.y < 0.0f) velocity.y = 0.0f;
}

// --- Drive / brake / steer ----------------------------------------------------
void VehiclePhysics::ApplyDrive(float dt, float steerInput, float throttle, float brake) {
    const glm::vec3 fwd = Forward();
    speedAlongFacing = glm::dot(velocity, fwd);

    // Front-wheel steer angle ramps toward the input (±30 deg).
    const float targetSteer = std::clamp(steerInput, -1.0f, 1.0f) * maxSteerAngle;
    steerAngle += (targetSteer - steerAngle) * std::min(1.0f, 8.0f * dt);

    // Yaw rate proportional to speed * steer angle (bicycle model, grounded
    // only — no grip in the air). Division softens the turn-in at speed.
    const float speed = std::fabs(speedAlongFacing);
    const float grounded = Airborne() ? 0.0f : 1.0f;
    float yawTarget = grounded *
        (speed / (1.0f + speed * 0.10f)) *
        std::tan(steerAngle) / wheelBase;
    if (speedAlongFacing < -0.1f) yawTarget = -yawTarget; // reversing: inverted
    yawTarget = std::clamp(yawTarget, -maxYawRate, maxYawRate);
    yawRate += (yawTarget - yawRate) * std::min(1.0f, grounded * steerSpeed * dt * 3.0f);
    yaw = WrapAngle(yaw + yawRate * dt);

    // Drive force at the rear wheels (spec: force = accel * mass). Gated on
    // ANY wheel contact: the rear axle can hang over a mesh seam, and an
    // all-axles gate would strand the vehicle with throttle held.
    const bool onGround = wheelContacts > 0;
    if (throttle > 0.0f && onGround) {
        velocity += fwd * (driveAccel * throttle * dt);
    }
    // Brake opposing velocity; reverse slowly when stopped.
    if (brake > 0.0f && onGround) {
        if (speedAlongFacing > 0.4f) {
            const float decel = std::min(brakeAccel * dt, speedAlongFacing);
            velocity -= fwd * decel;
        } else if (speedAlongFacing > -maxReverse) {
            velocity -= fwd * (reverseAccel * dt);
        }
    }

    // Drag (slow decay = momentum feel) + rolling resistance on ground.
    velocity -= velocity * std::min(0.9f, dragCoeff * dt);
    if (!Airborne()) {
        const float sp = glm::length(velocity);
        if (sp > 1e-3f) velocity -= velocity / sp * std::min(rollingResist * dt, sp);
    }
    // Hard clamps (2-minute stability guarantee).
    const float sp2 = glm::length(velocity);
    if (sp2 > maxSpeed) velocity *= maxSpeed / sp2;
}

// --- Integrate + sphere collision with wall bounce ---------------------------
void VehiclePhysics::Integrate(float dt) {
    // Gravity always acts; the suspension counters it while grounded (with a
    // contact the spring + hard-stop below hold the chassis up). Gating
    // gravity on Airborne() would leave the spring unbalanced at rest.
    velocity.y += gravity * dt;

    position += velocity * dt;
    if (!world) return;

    // Sphere collision vs level AABBs. Walls (mostly-vertical normals)
    // bounce with restitution 0.3; ground-facing hits only push horizontally
    // — the vertical axis belongs to the suspension, otherwise the body
    // sphere would pin the chassis above wheel-contact range forever.
    const float in[3] = { position.x, position.y, position.z };
    float out[3], normal[3];
    if (world->ResolveSphere(in, bodyRadius, out, normal)) {
        const glm::vec3 n = glm::normalize(glm::vec3(normal[0], normal[1], normal[2]));
        if (n.y < 0.7f) {
            position = glm::vec3(out[0], out[1], out[2]) + n * 0.01f;
            const float into = glm::dot(velocity, n);
            if (into < 0.0f) {
                // Reflect the normal component (restitution), keep tangential
                // motion so the car slides/bounces off walls rather than sticking.
                velocity -= n * (into * (1.0f + restitution));
                velocity *= 0.85f; // crash energy loss
            }
        } else {
            position.x = out[0];
            position.z = out[2];
        }
    }
}

// --- Terrain adaptation + weight transfer ------------------------------------
void VehiclePhysics::UpdateOrientation(float dt, float throttle, float brake) {
    // Pitch/roll from suspension-length deltas: the chassis plane follows the
    // plane fit through the four wheel contact points. Works with AABB-level
    // stepped terrain where raycast normals are always axis-aligned up.
    float targetPitch = 0.0f, targetRoll = 0.0f;
    if (frontContacts > 0 && rearContacts > 0) {
        const float f = 0.5f * (wheels[WheelFL].suspensionLength +
                                wheels[WheelFR].suspensionLength);
        const float r = 0.5f * (wheels[WheelRL].suspensionLength +
                                wheels[WheelRR].suspensionLength);
        // Nose up/down from front-vs-rear suspension lengths: on an upslope
        // the front wheels sit on higher ground (shorter front springs) and
        // the nose must pitch UP (negative rotation.x in Transform's
        // convention, where positive X rotation tips +Z forward downward).
        targetPitch = std::clamp((f - r) / std::max(wheelBase, 0.1f), -0.6f, 0.6f);
    }
    if (wheels[WheelFL].contact && wheels[WheelFR].contact) {
        const float l = wheels[WheelFL].suspensionLength;
        const float rt = wheels[WheelFR].suspensionLength;
        targetRoll = std::clamp((l - rt) / std::max(trackWidth, 0.1f), -0.6f, 0.6f);
    } else if (wheels[WheelRL].contact && wheels[WheelRR].contact) {
        const float l = wheels[WheelRL].suspensionLength;
        const float rt = wheels[WheelRR].suspensionLength;
        targetRoll = std::clamp((l - rt) / std::max(trackWidth, 0.1f), -0.6f, 0.6f);
    }
    // Smooth toward the terrain targets (springy but stable).
    const float a = std::min(1.0f, 7.0f * dt);
    pitch += (targetPitch - pitch) * a;
    roll  += (targetRoll  - roll)  * a;

    // Weight transfer (cosmetic): nose dips on braking, lifts on accel.
    const float accel = throttle - brake;
    cosmeticPitch += (-accel * 0.05f - cosmeticPitch) * std::min(1.0f, 5.0f * dt);
    cosmeticPitch = std::clamp(cosmeticPitch, -0.10f, 0.10f);
}

} // namespace OpenBully
