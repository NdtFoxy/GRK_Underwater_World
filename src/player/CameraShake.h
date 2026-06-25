#pragma once
#include <glm/glm.hpp>
#include <cmath>

// ----------------------------------------------------------------------
// CameraMotion (kept the file/class name for compatibility)
//
// NO jitter shake anymore. Instead this produces a gentle "floating in
// water" bob: a slow vertical rise/fall plus a tiny roll, strongest at
// the surface (where the waves are) and fading to nothing in the deep.
//
// The PlayerController feeds in `surfaceFactor` (1 near the surface,
// 0 deep down) each frame.
// ----------------------------------------------------------------------
class CameraShake {
public:
    // Wave bob tuning.
    float bobAmplitude = 0.35f;   // vertical world units at the surface
    float bobSpeed     = 1.1f;    // bob frequency
    float maxShakeAngle = 0.8f;   // gentle roll in degrees (slider-tunable)

    // 0 = deep/calm, 1 = at the surface where waves rock you.
    float surfaceFactor = 0.0f;

    // Underwater-storm turbulence 0..1 (set by main.cpp from the live
    // storm intensity): adds a slow chaotic roll + heave on top of the
    // surface bob, like being shoved around by surge.
    float turbulence = 0.0f;

    // Trauma kept as a no-op so old call sites still compile.
    void addTrauma(float) {}

    void update(float dt) { t += dt; }

    // Tiny roll only (degrees). No pitch/yaw jitter — keeps the view calm.
    glm::vec3 angularOffset() const {
        float roll = std::sin(t * bobSpeed * 0.6f) * maxShakeAngle * surfaceFactor;
        // Storm surge: two incommensurate slow rolls so it never loops.
        roll += (std::sin(t * 0.9f) + 0.6f * std::sin(t * 1.7f + 1.3f))
                * 1.6f * turbulence;
        return glm::vec3(0.0f, 0.0f, roll);
    }

    // Vertical wave bob applied to the eye position.
    glm::vec3 positionalOffset() const {
        float bob = std::sin(t * bobSpeed) * bobAmplitude * surfaceFactor;
        // A subtle secondary ripple for a less mechanical feel.
        bob += std::sin(t * bobSpeed * 2.3f + 1.1f) * bobAmplitude * 0.25f * surfaceFactor;
        // Storm heave (independent of the surface bob — works at depth).
        bob += std::sin(t * 1.3f + 0.7f) * 0.22f * turbulence;
        return glm::vec3(0.0f, bob, 0.0f);
    }

private:
    float t = 0.0f;
};
