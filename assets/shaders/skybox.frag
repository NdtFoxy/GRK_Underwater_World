#version 330 core
in vec3 TexDir;
out vec4 FragColor;

uniform samplerCube skybox;     // the underwater environment cubemap
uniform float exposure;         // day/night brightness
uniform vec3  tint;             // time-of-day colour tint

void main() {
    vec3 col = texture(skybox, normalize(TexDir)).rgb;
    col *= tint * exposure;
    FragColor = vec4(col, 1.0);
}
