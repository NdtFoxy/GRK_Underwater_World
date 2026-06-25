#version 430 core
// Pass-through VS for the tessellated water. All wave work happens in the TES; this
// just forwards the patch corner's LOCAL position (the grid is camera-centred, so the
// local XZ magnitude is ~distance to the camera, used by the TCS for LOD).
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec2 aTexCoords;

out vec3 vLocalPos;
out vec2 vUV;

void main() {
    vLocalPos = aPos;
    vUV = aTexCoords;
}
