#pragma once
#include <GL/glew.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

// ----------------------------------------------------------------------
// Thin glUniform helpers.
//
// Collapse the ubiquitous
//     glUniform3fv(glGetUniformLocation(prog, "name"), 1, glm::value_ptr(v));
// boilerplate into
//     setVec3(prog, "name", v);
// so the render code reads as a list of bindings instead of GL noise.
// These keep the same per-call glGetUniformLocation lookup as before, so
// behaviour is byte-for-byte identical — only the surface is shorter.
// ----------------------------------------------------------------------
inline void setInt  (GLuint p, const char* n, int   v) { glUniform1i(glGetUniformLocation(p, n), v); }
inline void setFloat(GLuint p, const char* n, float v) { glUniform1f(glGetUniformLocation(p, n), v); }
inline void setVec2 (GLuint p, const char* n, const glm::vec2& v) { glUniform2fv(glGetUniformLocation(p, n), 1, glm::value_ptr(v)); }
inline void setVec3 (GLuint p, const char* n, const glm::vec3& v) { glUniform3fv(glGetUniformLocation(p, n), 1, glm::value_ptr(v)); }
inline void setVec4 (GLuint p, const char* n, const glm::vec4& v) { glUniform4fv(glGetUniformLocation(p, n), 1, glm::value_ptr(v)); }
inline void setMat4 (GLuint p, const char* n, const glm::mat4& m) { glUniformMatrix4fv(glGetUniformLocation(p, n), 1, GL_FALSE, glm::value_ptr(m)); }
