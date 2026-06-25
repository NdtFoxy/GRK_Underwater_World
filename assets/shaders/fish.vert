#version 330 core
// Boids school fish: orients each instance along its velocity and swings
// the tail. One instanced draw call for the whole school.
layout (location = 0) in vec3 aPos;      // local: nose +Z
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec3 iPos;      // per-instance
layout (location = 3) in vec3 iVel;
layout (location = 4) in float iPhase;

uniform mat4 view;
uniform mat4 projection;
uniform float time;

out vec3  WorldPos;
out vec3  WorldNormal;
out float BellyT;     // -1 belly .. +1 back, for the silver gradient

void main() {
    // Basis from the swim direction (velocity), kept roll-free.
    vec3 fwd = normalize(iVel + vec3(1e-4, 0.0, 0.0));
    vec3 right = normalize(cross(vec3(0.0, 1.0, 0.0), fwd));
    vec3 up = cross(fwd, right);

    vec3 lp = aPos;
    // Tail sway: amplitude grows toward the tail (z < 0), rate with speed.
    float sp = length(iVel);
    float tailW = clamp(-lp.z * 3.0 + 0.2, 0.0, 1.0);
    lp.x += sin(lp.z * 7.0 - time * (7.0 + sp * 2.5) + iPhase * 4.0) * 0.10 * tailW;

    vec3 wp = iPos + right * lp.x + up * lp.y + fwd * lp.z;
    WorldPos = wp;
    WorldNormal = normalize(right * aNormal.x + up * aNormal.y + fwd * aNormal.z);
    BellyT = clamp(aPos.y * 12.0, -1.0, 1.0);
    gl_Position = projection * view * vec4(wp, 1.0);
}
