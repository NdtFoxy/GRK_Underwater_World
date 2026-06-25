#pragma once
// ----------------------------------------------------------------------
// Scene-internal helpers shared across the Scene_*.cpp translation units.
// These used to live in a file-local anonymous namespace inside Scene.cpp;
// they were promoted to inline functions in this header when Scene.cpp was
// split into per-responsibility files (terrain / props / ...), so every
// unit gets one consistent definition.
// ----------------------------------------------------------------------
#include <glm/glm.hpp>
#include <cmath>

// ----------------------------------------------------------------------
// Frustum culling helper. Used to skip terrain chunks / props / creatures
// that fall entirely outside the camera view — identical pixels, fewer
// draws.
// ----------------------------------------------------------------------
struct Frustum {
    glm::vec4 planes[6];   // ax+by+cz+d = 0, normals point INWARD
    void extract(const glm::mat4& m) {
        // Gribb-Hartmann plane extraction from a view-projection matrix.
        for (int i = 0; i < 3; ++i) {
            planes[i*2+0] = glm::vec4(m[0][3] + m[0][i],
                                      m[1][3] + m[1][i],
                                      m[2][3] + m[2][i],
                                      m[3][3] + m[3][i]);
            planes[i*2+1] = glm::vec4(m[0][3] - m[0][i],
                                      m[1][3] - m[1][i],
                                      m[2][3] - m[2][i],
                                      m[3][3] - m[3][i]);
        }
        for (int i = 0; i < 6; ++i) {
            float len = glm::length(glm::vec3(planes[i]));
            if (len > 1e-6f) planes[i] /= len;
        }
    }
    // True if the AABB is at least partially inside the frustum.
    bool testAABB(const glm::vec3& mn, const glm::vec3& mx) const {
        for (int i = 0; i < 6; ++i) {
            const glm::vec4& p = planes[i];
            // Positive-vertex test: the AABB corner furthest along
            // the plane normal. If it's behind the plane the whole
            // box is outside.
            glm::vec3 pv(
                p.x >= 0.0f ? mx.x : mn.x,
                p.y >= 0.0f ? mx.y : mn.y,
                p.z >= 0.0f ? mx.z : mn.z);
            if (p.x * pv.x + p.y * pv.y + p.z * pv.z + p.w < 0.0f)
                return false;
        }
        return true;
    }
};

inline float hash01(float x, float y, float seed) {
    float v = std::sin(x * 12.9898f + y * 78.233f + seed * 37.719f) * 43758.5453f;
    return v - std::floor(v);
}

inline int chooseObjectLOD(float distance, float radius, float bias = 1.0f) {
    radius = glm::clamp(radius, 4.0f, 80.0f);
    bias = glm::clamp(bias, 0.35f, 2.0f);
    // Tighter than before: props drop to their cheaper index sets sooner
    // so distant heavy meshes (rocks / shark) cost far fewer triangles.
    float lod0 = (45.0f + radius * 2.2f) * bias;
    float lod1 = (120.0f + radius * 5.0f) * bias;
    if (distance < lod0) return 0;
    if (distance < lod1) return 1;
    return 2;
}

inline glm::vec3 sanitizeMaterialFactor(const glm::vec3& factor, bool missingAlbedo) {
    glm::vec3 f = glm::clamp(factor, glm::vec3(0.0f), glm::vec3(4.0f));
    if (missingAlbedo && glm::length(f) < 0.08f) {
        return glm::vec3(0.75f);
    }
    return f;
}
