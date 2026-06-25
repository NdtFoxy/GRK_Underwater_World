#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include "../scene/Camera.h"
#include "OxygenSystem.h"
#include "CameraShake.h"

// ----------------------------------------------------------------------
// PlayerController
//
// Subnautica-style swimming controller on top of the free-fly Camera.
//   * velocity-based movement with smooth acceleration + water drag
//   * Player mode  : oxygen drains underwater and refills when the head
//                    breaks the surface. You CAN surface to breathe —
//                    there's a soft height cap above the water, not a
//                    hard wall. Running out of air fades to black and
//                    respawns at the spawn point.
//   * Admin mode   : free noclip flight, no oxygen, no bounds.
//   * gentle wave bob at the surface (no jittery camera shake).
// ----------------------------------------------------------------------
class PlayerController {
public:
    enum class Mode { Player, Admin };
    Mode mode = Mode::Player;

    // Movement tuning (player mode) — calm and weighty.
    float playerMaxSpeed   = 12.0f;
    float playerAccel      = 8.0f;
    float playerDrag       = 3.0f;
    float sprintMultiplier = 2.0f;

    // Admin (noclip)
    float adminMaxSpeed = 60.0f;
    float adminAccel    = 14.0f;
    float adminDrag     = 6.0f;

    glm::vec3 velocity = glm::vec3(0.0f);

    OxygenSystem oxygen;
    CameraShake  shake;     // now a gentle wave-bob, not a shake

    struct Input {
        bool forward = false, backward = false;
        bool left = false, right = false;
        bool up = false, down = false;
        bool sprint = false;
    };

    // World bounds / surface behaviour.
    float waterLevel    = 0.0f;     // surface Y
    float breatheMargin = 2.5f;     // how far above the surface you may rise
    float floorLevel    = -300.0f;  // hard lower bound (deep abyss reachable)
    glm::vec3 spawnPoint = glm::vec3(0.0f, -6.0f, 0.0f);

    // Terrain collision: set each frame by main.cpp from the scene's
    // height query. The player can't descend below this + a small
    // clearance. -100000 means "no terrain here" (a hole) → no floor.
    float terrainFloor   = -100000.0f;
    float floorClearance = 1.5f;    // eye stays this far above the seabed

    // Storm current: set each frame by main.cpp (direction x strength).
    // Underwater in Player mode the water bodily carries the diver —
    // you must actively swim against it in a storm.
    glm::vec3 waterCurrent = glm::vec3(0.0f);

    // Death / respawn (fade to black).
    bool  isDead     = false;
    float deathFade  = 0.0f;     // 0 clear, 1 full black
    float fadeInSpeed  = 1.6f;
    float fadeOutSpeed = 1.2f;

    // Reported each frame so the HUD knows if the head is in air.
    bool headAboveWater = false;

    void respawn(Camera& camera) {
        camera.Position = spawnPoint;
        velocity = glm::vec3(0.0f);
        oxygen.reset();
        isDead = false;
    }

    void update(Camera& camera, const Input& in, float dt) {
        // ---- death fade ----
        if (isDead) {
            deathFade += fadeInSpeed * dt;
            if (deathFade >= 1.0f) {
                deathFade = 1.0f;
                camera.Position = spawnPoint;
                velocity = glm::vec3(0.0f);
                oxygen.reset();
                isDead = false;
            }
            shake.update(dt);
            return;
        } else if (deathFade > 0.0f) {
            deathFade -= fadeOutSpeed * dt;
            if (deathFade < 0.0f) deathFade = 0.0f;
        }

        // ---- desired direction ----
        glm::vec3 wish(0.0f);
        if (in.forward)  wish += camera.Front;
        if (in.backward) wish -= camera.Front;
        if (in.right)    wish += camera.Right;
        if (in.left)     wish -= camera.Right;
        if (in.up)       wish += camera.WorldUp;
        if (in.down)     wish -= camera.WorldUp;

        bool moving = glm::dot(wish, wish) > 1e-4f;
        if (moving) wish = glm::normalize(wish);

        float maxSpeed = (mode == Mode::Admin) ? adminMaxSpeed : playerMaxSpeed;
        float accel    = (mode == Mode::Admin) ? adminAccel    : playerAccel;
        float drag     = (mode == Mode::Admin) ? adminDrag     : playerDrag;

        if (in.sprint) maxSpeed *= sprintMultiplier;

        // ---- smooth velocity integration (framerate-independent) ----
        glm::vec3 targetVel = wish * maxSpeed;
        float accelT = 1.0f - std::exp(-accel * dt);
        velocity = glm::mix(velocity, targetVel, accelT);
        if (!moving) {
            float dragT = 1.0f - std::exp(-drag * dt);
            velocity = glm::mix(velocity, glm::vec3(0.0f), dragT);
        }

        camera.Position += velocity * dt;

        // Storm current carries the diver (Player mode, underwater only).
        if (mode == Mode::Player && camera.Position.y < waterLevel)
            camera.Position += waterCurrent * dt;

        // ---- soft surface behaviour (Player mode) ----
        if (mode == Mode::Player) {
            // You may rise up to `waterLevel + breatheMargin`. Above the
            // surface, buoyancy gently pushes you back down instead of
            // a hard wall — feels like bobbing at the surface.
            float hardCeiling = waterLevel + breatheMargin;
            if (camera.Position.y > hardCeiling) {
                camera.Position.y = hardCeiling;
                if (velocity.y > 0.0f) velocity.y = 0.0f;
            }
            // Buoyant resistance once the head is above the water line:
            // the further out, the stronger the gentle pull-down.
            if (camera.Position.y > waterLevel) {
                float over = camera.Position.y - waterLevel;
                float buoyancy = over / breatheMargin;        // 0..1
                velocity.y -= buoyancy * 9.0f * dt;           // soft gravity
            }
            if (camera.Position.y < floorLevel) {
                camera.Position.y = floorLevel;
                if (velocity.y < 0.0f) velocity.y = 0.0f;
            }

            // Terrain collision — don't sink through the seabed.
            if (terrainFloor > -90000.0f) {
                float minY = terrainFloor + floorClearance;
                if (camera.Position.y < minY) {
                    camera.Position.y = minY;
                    if (velocity.y < 0.0f) velocity.y = 0.0f;
                }
            }
        }

        // ---- wave bob: strongest near the surface, fades with depth ----
        float depthBelow = waterLevel - camera.Position.y;          // >0 underwater
        float surfaceFactor = glm::clamp(1.0f - depthBelow / 12.0f, 0.0f, 1.0f);
        shake.surfaceFactor = surfaceFactor;
        shake.update(dt);

        // ---- oxygen ----
        // Head is "above water" (breathing) when the eye rises above the
        // surface line. Refill there, drain below.
        headAboveWater = camera.Position.y >= waterLevel;
        oxygen.godMode = (mode == Mode::Admin);
        oxygen.update(dt, /*isUnderwater*/ !headAboveWater);

        if (mode == Mode::Player && oxygen.isOutOfAir() && !headAboveWater) {
            isDead = true;
        }
    }

    // View matrix with the gentle wave bob applied.
    glm::mat4 getShakenView(Camera& camera) {
        glm::vec3 ang    = shake.angularOffset();     // roll only
        glm::vec3 posOff = shake.positionalOffset();  // vertical bob

        float roll = glm::radians(ang.z);
        glm::vec3 up = glm::normalize(camera.Up + camera.Right * std::tan(roll));
        glm::vec3 eye = camera.Position + glm::vec3(0.0f, posOff.y, 0.0f);
        return glm::lookAt(eye, eye + camera.Front, up);
    }
};
