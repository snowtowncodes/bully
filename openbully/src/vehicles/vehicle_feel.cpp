// Track 33: vehicle feel implementation — drive character, wheelie, crash/eject,
// sound hooks. See vehicle_feel.h.

#include "vehicles/vehicle_feel.h"

#include "audio/audio.h"
#include "platform/logger.h"

#include <cmath>
#include <algorithm>

namespace OpenBully {

static constexpr float kDegToRad = 3.14159265358979f / 180.0f;

// ---------------------------------------------------------------------------
// Sound hooks (module-level; no-op when audio not attached/initialized)
// ---------------------------------------------------------------------------

static AudioManager* s_vehicleAudio = nullptr;

void SetVehicleAudio(AudioManager* audio) { s_vehicleAudio = audio; }

void VehicleSfxEngineStart(VehicleType type, const glm::vec3& pos) {
    if (s_vehicleAudio && s_vehicleAudio->Initialized()) {
        // Ignition blip + one-shot hum puff. NOTE: deliberately NOT loop=true —
        // PlaySound has no stop handle, so a loop here would stack a permanent
        // voice per mount. A proper engine loop needs Voice-stop support in
        // AudioManager (follow-up for Track 21 API).
        s_vehicleAudio->PlaySound(SoundId::UiClick, 0.5f, &pos);
        s_vehicleAudio->PlaySound(SoundId::AmbientWind, 0.15f, &pos);
    }
    Logger::Info("VEHICLE: engine start (%s)", GetHandling(type).name);
}

void VehicleSfxCrash(float impactSpeed, const glm::vec3& pos) {
    if (s_vehicleAudio && s_vehicleAudio->Initialized()) {
        const float vol = std::min(1.0f, 0.3f + impactSpeed / 20.0f);
        s_vehicleAudio->PlaySound(SoundId::Footstep, vol, &pos); // low thud
    }
    Logger::Info("VEHICLE: crash sound (impact %.1f u/s)", impactSpeed);
}

void VehicleSfxWheelie(const glm::vec3& pos) {
    if (s_vehicleAudio && s_vehicleAudio->Initialized())
        s_vehicleAudio->PlaySound(SoundId::UiClick, 0.3f, &pos);
}

// ---------------------------------------------------------------------------
// Crash / eject
// ---------------------------------------------------------------------------

bool FeelImpact(FeelState& feel, const glm::vec3& pos, float yaw,
                float impactSpeed, FeelOut& out) {
    out.impactSpeed = impactSpeed;

    // flipFactor scales eject sensitivity: BIKE 1.6 -> ejects at ~6.3 u/s,
    // CAR 0.4 -> needs 25 u/s (practically never). Spec: eject > 10 u/s.
    const VehicleHandling& h = GetHandling(feel.type);
    const float threshold = kEjectImpactSpeed / h.flipFactor;

    if (impactSpeed < threshold || feel.crashed)
        return false;

    feel.crashed    = true;
    feel.crashTimer = 0.0f;
    // Lay the vehicle on its side; direction follows steering at impact.
    feel.tiltRoll = 80.0f * kDegToRad;
    feel.wheelieActive = false;
    feel.wheelieAngle  = 0.0f;
    feel.wheelieBalance = 0.0f;
    feel.wheelieTime   = 0.0f;
    feel.wheelieStuntLogged = false;
    // Track 46: the stoppie dies with the crash too.
    feel.stoppieActive  = false;
    feel.stoppieAngle   = 0.0f;
    feel.stoppieBalance = 0.0f;
    feel.stoppieTime    = 0.0f;

    out.crashBegan = true;
    // Rider lands beside the vehicle, perpendicular to heading (impact side).
    out.ejectRider = true;
    out.ejectPos = pos + glm::vec3(std::cos(yaw), 0.0f, -std::sin(yaw)) * 1.2f;
    return true;
}

// ---------------------------------------------------------------------------
// Main step
// ---------------------------------------------------------------------------

FeelOut StepFeel(FeelState& feel, const glm::vec3& pos, float yaw, float speed,
                 float dt) {
    FeelOut out;
    const VehicleHandling& h = GetHandling(feel.type);
    dt = std::min(dt, 0.1f);

    // --- Crashed: no control, auto-right after kAutoRightDelay ---------------
    if (feel.crashed) {
        feel.crashTimer += dt;
        if (feel.crashTimer >= kAutoRightDelay) {
            feel.crashed    = false;
            feel.crashTimer = 0.0f;
            feel.tiltRoll   = 0.0f;
            out.autoRighted = true;
            Logger::Info("VEHICLE: %s auto-righted after crash", h.name);
        }
        out.roll = feel.tiltRoll * kDegToRad;
        return out;
    }

    // --- Speed: profiles drive accel/topSpeed --------------------------------
    float newSpeed = speed;
    if (feel.inThrottle) {
        newSpeed += h.accel * dt;
    } else if (feel.inBrake) {
        newSpeed -= h.accel * 0.8f * dt; // reverse slower than forward
    } else {
        newSpeed -= newSpeed * kFeelRollingDrag * dt;
        if (std::fabs(newSpeed) < 0.05f) newSpeed = 0.0f;
    }
    newSpeed = std::clamp(newSpeed, -h.topSpeed * 0.4f, h.topSpeed);
    out.speedDelta = newSpeed - speed;

    // --- Steering: profiles drive rate; authority scales with speed ----------
    float steerInput = 0.0f;
    if (feel.inLeanL) steerInput += 1.0f;
    if (feel.inLeanR) steerInput -= 1.0f;
    if (steerInput != 0.0f && std::fabs(newSpeed) > 0.2f) {
        const float authority = std::clamp(
            std::fabs(newSpeed) / (h.topSpeed * 0.5f), 0.0f, 1.2f);
        out.yawDelta = steerInput * h.steerRate * kDegToRad * authority * dt *
                       (newSpeed < 0.0f ? -1.0f : 1.0f);
        // Lean into the turn (kart leans more: bouncy suspension feel).
        out.roll = -steerInput * 8.0f * h.suspension * kDegToRad;
    }

    // --- Moped wobble near top speed ----------------------------------------
    if (h.wobble > 0.0f && newSpeed > h.topSpeed * 0.85f) {
        const float phase = pos.x * 0.7f + pos.z * 1.3f;
        out.yawDelta += std::sin(phase * 3.7f) * h.wobble * 0.9f * kDegToRad *
                        dt * 10.0f;
    }

    // --- Wheelie (bike only, grounded) ---------------------------------------
    if (feel.type == VehicleType::Bike && feel.inGrounded) {
        // StepWheelie works in deg and per-frame units; scale torque by dt here
        // by pre-multiplying balance (kept simple: balance in deg/s, integrate
        // with dt below via direct call semantics).
        const float torque = kWheelieTorque * h.flipFactor * dt;
        const bool wantWheelie = feel.inThrottle && feel.inWheelie;

        if (!feel.wheelieActive && wantWheelie) {
            feel.wheelieActive = true;
            feel.wheelieTime = 0.0f;
            feel.wheelieStuntLogged = false;
            VehicleSfxWheelie(pos);
        }

        if (feel.wheelieActive) {
            if (feel.inLeanL) feel.wheelieBalance -= kWheelieTorque * h.flipFactor * 0.6f * dt;
            if (feel.inLeanR) feel.wheelieBalance += kWheelieTorque * h.flipFactor * 0.6f * dt;

            // Throttle applies constant lift torque; the restoring spring is
            // "gravity" pulling the front back to level, so the natural hold
            // equilibrium sits at torque/spring ≈ 35 deg. The longer the
            // wheelie is held, the stronger the over-rotation push grows
            // (progressive instability): holding forever still ends in a fall.
            const float torque = kWheelieTorque * h.flipFactor;
            const float lift = wantWheelie ? torque : -torque * 2.0f; // release drops fast
            const float spring = -kWheelieSpring * feel.wheelieAngle;
            const float instability = kWheelieInstability * feel.wheelieTime;
            feel.wheelieBalance += (lift + spring + instability) * dt;
            feel.wheelieBalance -= feel.wheelieBalance * kWheelieDamping * dt;

            feel.wheelieAngle += feel.wheelieBalance * dt;
            feel.wheelieTime  += dt;

            if (feel.wheelieAngle > kWheelieBalanceMax) {
                Logger::Info("STUNT FAILED: bike over-rotated (%.0f deg) — rider ejected",
                             feel.wheelieAngle);
                feel.wheelieAngle   = 0.0f;
                feel.wheelieBalance = 0.0f;
                feel.wheelieActive  = false;
                feel.wheelieTime    = 0.0f;
                FeelImpact(feel, pos, yaw, kEjectImpactSpeed + 1.0f, out);
            } else {
                if (!feel.wheelieStuntLogged && feel.wheelieTime > kWheelieStuntTime) {
                    feel.wheelieStuntLogged = true;
                    out.stuntLogged = true;
                    Logger::Info("STUNT: bike wheelie held %.1fs (angle %.0f deg)",
                                 feel.wheelieTime, feel.wheelieAngle);
                }
                if (!wantWheelie && feel.wheelieAngle <= 0.0f) {
                    feel.wheelieActive  = false;
                    feel.wheelieBalance = 0.0f;
                    feel.wheelieTime    = 0.0f;
                }
            }
        } else {
            feel.wheelieAngle = std::max(0.0f, feel.wheelieAngle - 120.0f * dt);
            feel.wheelieBalance = 0.0f;
        }
    }

    // --- Track 46: stoppie (bike only): brake hard while rolling forward -----
    // The endo mirrors the wheelie: brake torque pitches the nose DOWN (the
    // rear rises — positive stoppieAngle in the feel convention), the front
    // weight spring pulls it back, and the balance fades as speed bleeds so
    // a full brake stop settles the rear wheel instead of looping out.
    // Gated on the wheelie ANGLE (not the active latch): during a wheelie's
    // ~0.15 s release decay the stoppie may already trigger, and gating on
    // the latch would eat the whole >1.5 u/s speed window.
    if (feel.type == VehicleType::Bike && feel.wheelieAngle < 5.0f &&
        !feel.crashed && feel.inGrounded) {
        // Signed speed: a stoppie needs FORWARD motion. Once the brake stops
        // the bike (and it creeps into reverse) the torque goes off and the
        // spring settles the rear back down.
        const bool wantStoppie = feel.inBrake && !feel.inThrottle &&
                                 speed > kStoppieMinSpeed;
        if (!feel.stoppieActive && wantStoppie) {
            feel.stoppieActive  = true;
            feel.stoppieTime    = 0.0f;
            feel.stoppieBalance = 0.0f;
            VehicleSfxWheelie(pos); // same pitch-up cue, different pose
        }

        if (feel.stoppieActive) {
            feel.stoppieTime += dt;
            // Torque fades as the speed collapses: an endo from a rolling
            // brake lasts under ~1.2 s, then the rear settles back down.
            const float fade = std::clamp(1.2f - feel.stoppieTime, 0.0f, 1.0f);
            const float torque = wantStoppie
                ? kStoppieTorque * h.flipFactor * fade * dt
                : 0.0f;
            const float spring = -kStoppieSpring * feel.stoppieAngle;
            feel.stoppieBalance += (torque + spring) * dt;
            feel.stoppieBalance -= feel.stoppieBalance * kStoppieDamping * dt;
            feel.stoppieAngle   += feel.stoppieBalance * dt;

            if (feel.stoppieAngle > kStoppieBalanceMax) {
                // Rear flipped over the front: loop out -> crash (bail).
                Logger::Info("STUNT FAILED: stoppie looped out (%.0f deg)",
                             feel.stoppieAngle);
                feel.stoppieActive  = false;
                feel.stoppieAngle   = 0.0f;
                feel.stoppieBalance = 0.0f;
                feel.stoppieTime    = 0.0f;
                FeelImpact(feel, pos, yaw, kEjectImpactSpeed + 1.0f, out);
            } else if (!wantStoppie && feel.stoppieAngle <= 0.0f) {
                // Released and recovered to level.
                feel.stoppieActive  = false;
                feel.stoppieAngle   = 0.0f;
                feel.stoppieBalance = 0.0f;
                feel.stoppieTime    = 0.0f;
            }
        } else {
            feel.stoppieAngle   = 0.0f;
            feel.stoppieBalance = 0.0f;
        }
    } else if (feel.type == VehicleType::Bike &&
               (feel.crashed || feel.wheelieActive)) {
        // A wheelie (or a crash) takes the stoppie channel offline; fold any
        // residual stoppie pitch back to level without antic feedback.
        feel.stoppieActive  = false;
        feel.stoppieAngle   = 0.0f;
        feel.stoppieBalance = 0.0f;
        feel.stoppieTime    = 0.0f;
    }

    out.pitch = (feel.wheelieAngle + feel.stoppieAngle) * kDegToRad;
    if (feel.crashed) out.roll = feel.tiltRoll;
    return out;
}

} // namespace OpenBully
