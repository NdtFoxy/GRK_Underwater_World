#version 330 core
layout (location = 0) in vec3 aPos;     // body position (PTF-swept tube)
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUV;       // u = along body, v = around ring

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform float time;
uniform mat4 lightSpaceMatrix;   // for shadow receiving / casting

out vec3 FragPos;
out vec3 WorldNormal;
out vec2 UV;
out vec4 FragPosLight;

void main() {
    // Travelling-wave undulation so the serpent swims. The displacement
    // is applied in local space along X (lateral) and grows toward the
    // tail. Because the body was built with parallel-transport frames,
    // this lateral push stays coherent (no pinching/twisting).
    vec3 p = aPos;
    float along = aUV.x;                 // 0 head .. 1 tail
    float wave = sin(along * 12.0 - time * 4.0);
    float amp  = 1.4 * along;            // tail swings more than the head
    p.x += wave * amp;

    vec4 world = model * vec4(p, 1.0);
    FragPos     = world.xyz;
    WorldNormal = normalize(mat3(model) * aNormal);
    UV          = aUV;
    FragPosLight = lightSpaceMatrix * world;
    gl_Position = projection * view * world;
}
