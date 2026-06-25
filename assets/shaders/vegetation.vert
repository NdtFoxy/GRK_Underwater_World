#version 330 core

// Per-vertex
layout (location = 0) in vec3 aPos;     // local space, base at y=0
layout (location = 1) in vec2 aUV;
layout (location = 2) in float aSway;   // 0 at root, 1 at tip

// Per-instance (glVertexAttribDivisor=1)
layout (location = 3) in vec4 iPosYaw;  // xyz = world position, w = yaw
layout (location = 4) in vec2 iScalePhase;

uniform mat4 view;
uniform mat4 projection;
uniform float time;
uniform float swayStrength;   // 1.0 = full sway (kelp), 0.0 = rigid (coral)
uniform float uCurrent;       // 0=calm, 1=full storm current

out vec2  vUV;
out vec3  vWorldPos;
out float vSway;
out float vDistFade;     // 0..1 fade for stochastic pruning at the very edge

uniform vec3 cameraPos;

void main() {
    float yaw   = iPosYaw.w;
    float scale = iScalePhase.x;
    float phase = iScalePhase.y;

    // Rotate around Y, then scale, then translate to world.
    float c = cos(yaw), s = sin(yaw);
    vec3 lp = aPos * scale;
    vec3 rotated = vec3(
        lp.x * c + lp.z * s,
        lp.y,
        -lp.x * s + lp.z * c);
    vec3 worldPos = rotated + iPosYaw.xyz;

    // ---- Wind / current sway ---------------------------------------
    // Sway amplitude grows quadratically with the vertical position so
    // the tip moves the most. swayStrength lets rigid plants (coral)
    // opt out while flexible kelp sways fully.
    // uCurrent scales the amplitude up in a storm (calm=1x, full=2.5x).
    float swayAmp = aSway * aSway * 0.6 * swayStrength * (1.0 + 1.5 * uCurrent);
    float swayT = time * 0.9 + phase * 6.28
                + worldPos.x * 0.02
                + worldPos.z * 0.02;
    worldPos.x += sin(swayT)            * swayAmp;
    worldPos.z += cos(swayT * 0.7 + 1.3) * swayAmp * 0.7;
    // Subtle vertical bob from the surge motion of the water column
    worldPos.y += sin(swayT * 1.4) * 0.05 * aSway * swayStrength;
    // Steady directional lean into the current — grows linearly from
    // root (aSway=0, no offset) to tip (aSway=1, full offset).
    // Keeps the root anchored while bending the whole plant downstream.
    vec2 curDir = normalize(vec2(1.0, 0.35));
    worldPos.x += curDir.x * (0.6 * uCurrent * aSway);
    worldPos.z += curDir.y * (0.6 * uCurrent * aSway);

    vWorldPos = worldPos;
    vUV       = aUV;
    vSway     = aSway;

    // Soft fade in the last 10 m before the per-instance death distance.
    // Distance is recomputed in vert because we don't pass lifeDist.
    // Approximating with a generic "near max LOD distance" is good
    // enough; this just hides the very last ring of pop-out.
    float d = length(cameraPos - worldPos);
    vDistFade = clamp((220.0 - d) / 30.0, 0.0, 1.0);

    gl_Position = projection * view * vec4(worldPos, 1.0);
}
