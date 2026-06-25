#pragma once

// ----------------------------------------------------------------------
// OxygenSystem
//
// Subnautica-style breath meter. Oxygen drains while the player is
// below the water surface and refills quickly at/above it. When the
// air runs out the player begins to drown — the PlayerController owns
// the death/respawn sequence, this class just reports "out of air".
// ----------------------------------------------------------------------
class OxygenSystem {
public:
    float maxOxygen  = 45.0f;   // total seconds of air at a full tank
    float drainRate  = 1.0f;    // air-seconds spent per real second underwater
    float refillRate = 20.0f;   // air-seconds restored per real second at surface

    float oxygen = maxOxygen;   // current air (seconds)

    bool  underwater = false;   // set each frame by the player controller
    bool  godMode    = false;   // admin: never drains

    void update(float dt, bool isUnderwater) {
        underwater = isUnderwater;

        if (godMode) { oxygen = maxOxygen; return; }

        if (isUnderwater) {
            oxygen -= drainRate * dt;
            if (oxygen < 0.0f) oxygen = 0.0f;
        } else {
            oxygen += refillRate * dt;
            if (oxygen > maxOxygen) oxygen = maxOxygen;
        }
    }

    void reset() { oxygen = maxOxygen; }

    float oxygenFraction() const {
        return (maxOxygen > 0.0f) ? (oxygen / maxOxygen) : 0.0f;
    }
    bool  isOutOfAir() const { return oxygen <= 0.0f; }
};
