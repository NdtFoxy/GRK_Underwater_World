#version 330 core
layout (location = 0) in vec2 aPos;
out vec3 viewRay;

uniform mat4 invProjection;
uniform mat4 invView;

void main() {
    gl_Position = vec4(aPos.x, aPos.y, 0.9999, 1.0);
    vec4 clipPos = vec4(aPos.x, aPos.y, 1.0, 1.0);
    vec4 viewPos = invProjection * clipPos;
    viewPos = vec4(viewPos.xyz / viewPos.w, 0.0);
    vec4 worldDir = invView * viewPos;
    viewRay = worldDir.xyz;
}
