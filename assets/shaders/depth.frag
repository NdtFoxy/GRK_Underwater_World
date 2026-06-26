#version 330 core
// Depth-only pass: by default the fragment shader writes nothing of its
// own and the fixed-function depth buffer captures the distance to the sun.
//
// Optional alpha test: foliage props (palm fronds, ferns) are textured
// cutout cards. Without testing the albedo alpha their full quads would
// cast solid rectangular shadows. When `alphaTest` is 1 we discard
// transparent texels so the shadow takes the leaf silhouette.
in vec2 vUV;

uniform int       alphaTest;
uniform sampler2D albedoTex;

void main() {
    if (alphaTest == 1 && texture(albedoTex, vUV).a < 0.5)
        discard;
}
