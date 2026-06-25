#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec2 aTexCoords;
layout (location = 2) in vec3 aNormal;
layout (location = 3) in vec3 aBiome; // (castle, lava, river) weights

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform float time;
uniform mat4 lightSpaceMatrix;   // sun's view-projection for shadow lookup

out vec3 FragPos;
out vec2 TexCoords;
out vec3 WorldNormal;
out vec3 BiomeWeights;
out float Depth; // Y position (negative = underwater)
out vec4 FragPosLight;

void main() {
    vec4 worldPos = model * vec4(aPos, 1.0);
    FragPos = worldPos.xyz;
    TexCoords = aTexCoords;
    WorldNormal = normalize(mat3(transpose(inverse(model))) * aNormal);
    BiomeWeights = aBiome;
    Depth = aPos.y;
    FragPosLight = lightSpaceMatrix * worldPos;
    gl_Position = projection * view * worldPos;
}
