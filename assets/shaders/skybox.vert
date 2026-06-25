#version 330 core
layout (location = 0) in vec3 aPos;

out vec3 TexDir;   // direction used to sample the cubemap

uniform mat4 view;        // view matrix WITH translation removed
uniform mat4 projection;

void main() {
    TexDir = aPos;
    vec4 pos = projection * view * vec4(aPos, 1.0);
    // Force z = w so the skybox is always at the far plane (depth 1.0)
    // and never occludes scene geometry.
    gl_Position = pos.xyww;
}
