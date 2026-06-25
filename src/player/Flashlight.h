#pragma once
#include <GL/glew.h>
#include <glm/glm.hpp>

// ----------------------------------------------------------------------
// Flashlight
//
// A camera-attached spotlight (toggled with F). Holds the light state
// and a helper to upload its uniforms to any lit shader program. The
// actual lighting math lives in the fragment shaders (terrain/coral/
// cave) inside an applyFlashlight() helper — see those .frag files.
//
// Realistic underwater torch:
//   * cone with smooth inner/outer falloff
//   * distance attenuation (water absorbs light fast)
//   * slight warm-white colour, flicker option
// ----------------------------------------------------------------------
class Flashlight {
public:
    bool  enabled = false;

    // Cone half-angles (cosines precomputed in setUniforms).
    float innerDeg = 16.0f;   // full-bright cone
    float outerDeg = 30.0f;   // fades to zero by here
    float range    = 90.0f;   // world units the beam reaches
    float intensity = 6.0f;   // strong — it must pierce the deep darkness
    glm::vec3 color = glm::vec3(1.0f, 0.96f, 0.85f);

    void toggle() { enabled = !enabled; }

    // Upload the flashlight uniforms to `program`. `pos` is the camera
    // position, `dir` the camera forward (both world space). Call after
    // glUseProgram(program).
    void setUniforms(GLuint program,
                     const glm::vec3& pos,
                     const glm::vec3& dir) const {
        glUniform1i(glGetUniformLocation(program, "flashOn"), enabled ? 1 : 0);
        if (!enabled) return;
        glUniform3fv(glGetUniformLocation(program, "flashPos"), 1, &pos[0]);
        glUniform3fv(glGetUniformLocation(program, "flashDir"), 1, &dir[0]);
        glUniform3fv(glGetUniformLocation(program, "flashColor"), 1, &color[0]);
        glUniform1f(glGetUniformLocation(program, "flashInnerCos"),
                    cosf(glm::radians(innerDeg)));
        glUniform1f(glGetUniformLocation(program, "flashOuterCos"),
                    cosf(glm::radians(outerDeg)));
        glUniform1f(glGetUniformLocation(program, "flashRange"), range);
        glUniform1f(glGetUniformLocation(program, "flashIntensity"), intensity);
    }
};
