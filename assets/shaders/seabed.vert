#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec2 aTexCoords;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

out vec3 FragPos;
out vec2 TexCoords;
out vec3 WorldNormal;

void main() {
    FragPos = vec3(model * vec4(aPos, 1.0));
    TexCoords = aTexCoords * 8.0; // Tile the sand texture
    WorldNormal = vec3(0.0, 1.0, 0.0); // Flat plane pointing up
    gl_Position = projection * view * vec4(FragPos, 1.0);
}
