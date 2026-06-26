#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec2 aUV;

// Depth-only pass from the sun's point of view. Each shadow caster is
// drawn with its own `model`; instanced casters (vegetation) supply a
// per-instance transform through the extra attributes below.
uniform mat4 lightSpaceMatrix;
uniform mat4 model;

// --- optional instanced path (vegetation) -------------------------
// When `instanced` is 1 we build the world transform from the
// per-instance attributes instead of `model`.
uniform int  instanced;
uniform float time;
layout (location = 3) in vec4 iPosYaw;     // xyz world pos, w yaw
layout (location = 4) in vec2 iScalePhase; // x scale, y phase

// Forwarded only for the alpha-tested path (foliage props) so leaf
// cutouts cast leaf-shaped shadows instead of solid quads.
out vec2 vUV;

void main() {
    vUV = aUV;
    vec3 world;
    if (instanced == 1) {
        float yaw = iPosYaw.w;
        float scale = iScalePhase.x;
        float c = cos(yaw), s = sin(yaw);
        vec3 lp = aPos * scale;
        world = vec3(lp.x*c + lp.z*s, lp.y, -lp.x*s + lp.z*c) + iPosYaw.xyz;
    } else {
        world = (model * vec4(aPos, 1.0)).xyz;
    }
    gl_Position = lightSpaceMatrix * vec4(world, 1.0);
}
