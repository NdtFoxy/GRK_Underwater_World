#pragma once
#include <GL/glew.h>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>

// ======================================================================
// ЗАЩИТА (обязательный метод: Underwater skybox / cubemap)
//   СУТЬ:   GL_TEXTURE_CUBE_MAP как фон. В шейдере сэмплится по
//           НАПРАВЛЕНИЮ взгляда (samplerCube). Рисуется с убранной
//           трансляцией из view (mat3(view)) => фон не двигается с камерой.
//   ГДЕ:    подводный градиент-«бездна» вокруг во все стороны.
//   СЛОВА:  samplerCube, mat3(view) убирает позицию, glDepthFunc(LEQUAL).
// ======================================================================
// ----------------------------------------------------------------------
// Cubemap  (mandatory method: underwater skybox / cubemap)
//
// Builds a GL_TEXTURE_CUBE_MAP for the underwater environment. Two ways:
//   * LoadFromFiles  — six face images (px,nx,py,ny,pz,nz)
//   * CreateProcedural — generate the six faces on the CPU from a simple
//                        underwater gradient (deep blue below, lighter
//                        teal toward the surface) so the project ships
//                        with a working cubemap even without art assets.
//
// The cubemap is sampled in the skybox shader by the view *direction*,
// and the skybox is drawn with the translation removed from the view
// matrix, so it stays fixed regardless of camera position.
// ----------------------------------------------------------------------
class Cubemap {
public:
    GLuint id = 0;

    bool CreateProcedural(int faceSize = 256) {
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_CUBE_MAP, id);

        // Face order matches GL_TEXTURE_CUBE_MAP_POSITIVE_X + i.
        for (int face = 0; face < 6; ++face) {
            std::vector<unsigned char> px(faceSize * faceSize * 3);
            for (int y = 0; y < faceSize; ++y) {
                for (int x = 0; x < faceSize; ++x) {
                    // Map face pixel -> direction vector.
                    float u = (x + 0.5f) / faceSize * 2.0f - 1.0f;
                    float v = (y + 0.5f) / faceSize * 2.0f - 1.0f;
                    float dx, dy, dz;
                    switch (face) {
                        case 0: dx =  1; dy = -v; dz = -u; break; // +X
                        case 1: dx = -1; dy = -v; dz =  u; break; // -X
                        case 2: dx =  u; dy =  1; dz =  v; break; // +Y (up)
                        case 3: dx =  u; dy = -1; dz = -v; break; // -Y (down)
                        case 4: dx =  u; dy = -v; dz =  1; break; // +Z
                        default:dx = -u; dy = -v; dz = -1; break; // -Z
                    }
                    float len = std::sqrt(dx*dx + dy*dy + dz*dz);
                    float ny = dy / len;   // -1 (down) .. +1 (up)

                    // Abyss gradient (per the brief): pure black looking
                    // straight DOWN into the deep, smoothly rising to a
                    // light greenish-blue "surface glow" looking UP.
                    float t = ny * 0.5f + 0.5f;            // 0 down .. 1 up

                    // Ease so the lower hemisphere stays very dark and the
                    // brightening is concentrated toward the top.
                    float up = t * t * t;                  // cubic bias

                    // Deep abyss colour ~ (0,0,0); surface colour is a
                    // light teal / greenish-blue.
                    float r = 0.00f + 0.12f * up;
                    float g = 0.01f + 0.55f * up;
                    float b = 0.02f + 0.62f * up;

                    // A faint horizon band of light just above the middle
                    // so the transition reads like scattered surface light.
                    float horizon = std::exp(-((ny - 0.05f) * (ny - 0.05f)) / 0.02f);
                    g += 0.06f * horizon;
                    b += 0.07f * horizon;

                    int idx = (y * faceSize + x) * 3;
                    px[idx + 0] = (unsigned char)(std::min(1.0f, r) * 255.0f);
                    px[idx + 1] = (unsigned char)(std::min(1.0f, g) * 255.0f);
                    px[idx + 2] = (unsigned char)(std::min(1.0f, b) * 255.0f);
                }
            }
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GL_RGB,
                         faceSize, faceSize, 0, GL_RGB, GL_UNSIGNED_BYTE, px.data());
        }

        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
        return true;
    }

    void Bind(GLenum unit) const {
        glActiveTexture(unit);
        glBindTexture(GL_TEXTURE_CUBE_MAP, id);
    }
};
