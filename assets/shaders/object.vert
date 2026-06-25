#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec2 aUV;
layout (location = 2) in vec3 aNormal;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform mat4 lightSpaceMatrix;   // sun's view-projection for shadow lookup

// Fish swim animation: bends the mesh along an asset-specific local body axis.
// swim = 0 disables it (rigid props / jellyfish); 1 enables.
uniform float swim;
uniform float objTime;
uniform vec3 swimAxis;
uniform vec3 swimSide;

out vec3 FragPos;
out vec2 UV;
out vec3 WorldNormal;
out vec4 FragPosLight;

void main() {
    vec3 localPos = aPos;
    if (swim > 0.5) {
        // Sinusoidal lateral wave growing along the fish body. Assets do
        // not agree on their local forward axis, so Scene uploads the
        // correct body/sway axes per creature.
        vec3 bodyAxis = normalize(swimAxis);
        vec3 sideAxis = normalize(swimSide);
        float body = dot(localPos, bodyAxis);
        // ~1.5 wave periods over a ~1.2-unit tail half: a visible S-curve
        // travelling tailward (0.6 was near-constant phase over the body).
        float wave = sin(body * 2.2 - objTime * 6.0);
        localPos += sideAxis * wave * 0.10 * max(body, 0.0);
    }
    vec4 world = model * vec4(localPos, 1.0);
    FragPos = world.xyz;
    UV = aUV;
    WorldNormal = normalize(mat3(transpose(inverse(model))) * aNormal);
    FragPosLight = lightSpaceMatrix * world;
    gl_Position = projection * view * world;
}
