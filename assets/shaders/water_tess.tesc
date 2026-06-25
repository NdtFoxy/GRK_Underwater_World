#version 430 core
// Tessellation control: subdivide water patches near the camera (more polygons on
// crests), coarse toward the horizon. Each outer edge level is computed from that
// edge's midpoint distance so neighbouring patches agree -> no cracks.
layout(vertices = 3) out;

in vec3 vLocalPos[];
in vec2 vUV[];
out vec3 tcLocalPos[];
out vec2 tcUV[];

uniform float uTessMax;   // base subdivision near the camera

float edgeLevel(vec3 a, vec3 b) {
    // Grid is camera-centred, so local XZ magnitude ~ distance from camera.
    // Pure distance LOD: dense near the camera, coarse toward the horizon.
    // Midpoint sampling keeps neighbouring patches in agreement -> crack-free.
    vec3 mid = (a + b) * 0.5;
    float d = length(mid.xz);
    float level = uTessMax * exp(-d * 0.020);
    return clamp(level, 1.0, 32.0);
}

void main() {
    tcLocalPos[gl_InvocationID] = vLocalPos[gl_InvocationID];
    tcUV[gl_InvocationID]       = vUV[gl_InvocationID];

    if (gl_InvocationID == 0) {
        vec3 v0 = vLocalPos[0], v1 = vLocalPos[1], v2 = vLocalPos[2];
        gl_TessLevelOuter[0] = edgeLevel(v1, v2);   // edge opposite v0
        gl_TessLevelOuter[1] = edgeLevel(v2, v0);   // edge opposite v1
        gl_TessLevelOuter[2] = edgeLevel(v0, v1);   // edge opposite v2
        gl_TessLevelInner[0] = (gl_TessLevelOuter[0] + gl_TessLevelOuter[1] + gl_TessLevelOuter[2]) / 3.0;
    }
}
