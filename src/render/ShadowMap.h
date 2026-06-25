#pragma once
#include <GL/glew.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>

// ======================================================================
// ЗАЩИТА (обязательный метод: Shadow mapping)
//   СУТЬ:   1) рендерю сцену с точки зрения СОЛНЦА в depth-текстуру (FBO);
//           2) в основном шейдере перевожу фрагмент в light-space и
//              сравниваю его глубину с картой => в тени или нет.
//   АРТЕФАКТЫ: убраны через PCF 3x3 (мягкий край) + slope-scaled bias
//              (против shadow acne).
//   ГДЕ:    мягкие тени рельефа/растений на дне.
//   СЛОВА:  depth FBO, light-space matrix (ortho для солнца), PCF, bias.
// ======================================================================
// ----------------------------------------------------------------------
// ShadowMap  (mandatory method: shadow mapping + large-radius PCF)
//
// A single depth-only FBO rendered from the sun's point of view. The
// scene's opaque geometry (terrain, caves, vegetation, serpent) is
// drawn once into this depth texture; the main shaders then sample it
// with a wide PCF kernel for the very soft, diffuse shadows that suit an
// underwater scene (light is heavily scattered, so hard edges look wrong).
//
// The light-space matrix is an orthographic projection (directional sun)
// looking at the play area from along -sunDirection.
// ----------------------------------------------------------------------
class ShadowMap {
public:
    GLuint fbo = 0;
    GLuint depthTex = 0;
    int    size = 2048;
    glm::mat4 lightSpace = glm::mat4(1.0f);

    bool init(int resolution = 2048) {
        size = resolution;
        glGenFramebuffers(1, &fbo);
        glGenTextures(1, &depthTex);
        glBindTexture(GL_TEXTURE_2D, depthTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24,
                     size, size, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
        // Outside the light frustum = fully lit (border depth = 1.0).
        float border[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);

        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                               GL_TEXTURE_2D, depthTex, 0);
        // No colour buffer for a depth-only pass.
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
        bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return ok;
    }

    // Recompute the light-space matrix to cover a region of the world
    // centred at `center` with the given half-extent and a directional
    // sun. Call once per frame (cheap).
    void update(const glm::vec3& sunDir, const glm::vec3& center,
                float extent, float depthRange) {
        glm::vec3 dir = glm::normalize(sunDir);
        // Place the virtual light back along the sun direction.
        glm::vec3 eye = center + dir * (depthRange * 0.5f);
        glm::vec3 up = (std::abs(dir.y) > 0.99f) ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
        glm::mat4 lightView = glm::lookAt(eye, center, up);
        glm::mat4 lightProj = glm::ortho(-extent, extent, -extent, extent,
                                         1.0f, depthRange);
        lightSpace = lightProj * lightView;
    }

    // Bind for the depth pass (sets viewport + clears depth).
    void beginDepthPass() {
        glViewport(0, 0, size, size);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glClear(GL_DEPTH_BUFFER_BIT);
    }

    void destroy() {
        if (depthTex) glDeleteTextures(1, &depthTex);
        if (fbo) glDeleteFramebuffers(1, &fbo);
        depthTex = fbo = 0;
    }
};
