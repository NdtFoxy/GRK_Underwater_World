#pragma once
#include <vector>
#include <cmath>
#include <glm/glm.hpp>
#include "../core/ModelLoader.h"

// ----------------------------------------------------------------------
// Procedural coconut palm (fallback when the FBX is too old for Assimp).
// Builds a tapered curved trunk + a crown of flat frond cards, in the
// 8-float vertex layout (pos, uv, normal) the vegetation system uses.
// Base sits at y=0; total height ~10 units (rescaled on load).
// ----------------------------------------------------------------------
namespace PalmMesh {

inline void pushV(std::vector<float>& v, glm::vec3 p, glm::vec2 uv, glm::vec3 n) {
    v.push_back(p.x); v.push_back(p.y); v.push_back(p.z);
    v.push_back(uv.x); v.push_back(uv.y);
    v.push_back(n.x); v.push_back(n.y); v.push_back(n.z);
}

inline LoadedMesh make() {
    LoadedMesh m;
    const float H = 9.0f;          // trunk height
    const int   SEG = 7;           // trunk vertical segments
    const int   SIDES = 6;         // trunk radial sides
    float lean = 1.2f;             // sideways curve of the trunk

    // --- trunk: stacked rings, slight curve + taper ---
    for (int s = 0; s <= SEG; ++s) {
        float t = (float)s / SEG;
        float y = t * H;
        float r = glm::mix(0.45f, 0.22f, t);          // taper
        float cx = lean * t * t;                       // curve
        for (int i = 0; i <= SIDES; ++i) {
            float a = (float)i / SIDES * 6.2831853f;
            glm::vec3 p(cx + std::cos(a) * r, y, std::sin(a) * r);
            glm::vec3 n = glm::normalize(glm::vec3(std::cos(a), 0.2f, std::sin(a)));
            // UV.y near 1 = trunk bark zone (texture's lower strip).
            pushV(m.vertices, p, glm::vec2((float)i / SIDES, 0.15f + 0.1f * t), n);
        }
    }
    int ring = SIDES + 1;
    for (int s = 0; s < SEG; ++s)
        for (int i = 0; i < SIDES; ++i) {
            unsigned int a = s * ring + i;
            unsigned int b = (s + 1) * ring + i;
            m.indices.push_back(a); m.indices.push_back(b); m.indices.push_back(a + 1);
            m.indices.push_back(a + 1); m.indices.push_back(b); m.indices.push_back(b + 1);
        }

    // --- crown: flat frond cards radiating from the top ---
    glm::vec3 top(lean, H, 0.0f);
    const int FRONDS = 9;
    float fl = 4.2f;     // frond length
    float fw = 0.9f;     // frond half-width at the base
    for (int f = 0; f < FRONDS; ++f) {
        float a = (float)f / FRONDS * 6.2831853f;
        glm::vec3 dir(std::cos(a), 0.0f, std::sin(a));
        glm::vec3 side = glm::normalize(glm::cross(dir, glm::vec3(0, 1, 0)));
        // Frond droops: tip lower than the base.
        glm::vec3 tip = top + dir * fl + glm::vec3(0.0f, -1.6f, 0.0f);
        glm::vec3 mid = top + dir * (fl * 0.5f) + glm::vec3(0.0f, 0.4f, 0.0f);
        glm::vec3 nrm = glm::normalize(glm::cross(side, tip - top));
        unsigned int base = (unsigned int)(m.vertices.size() / 8);
        // Quad-ish leaf (base wide -> tip narrow), UV.y in the leaf zone.
        pushV(m.vertices, top + side * fw, glm::vec2(0.0f, 0.55f), nrm);
        pushV(m.vertices, top - side * fw, glm::vec2(1.0f, 0.55f), nrm);
        pushV(m.vertices, mid - side * fw * 0.6f, glm::vec2(1.0f, 0.78f), nrm);
        pushV(m.vertices, mid + side * fw * 0.6f, glm::vec2(0.0f, 0.78f), nrm);
        pushV(m.vertices, tip, glm::vec2(0.5f, 1.0f), nrm);
        m.indices.push_back(base);   m.indices.push_back(base+1); m.indices.push_back(base+2);
        m.indices.push_back(base);   m.indices.push_back(base+2); m.indices.push_back(base+3);
        m.indices.push_back(base+3); m.indices.push_back(base+2); m.indices.push_back(base+4);
        // Back faces so fronds are two-sided.
        m.indices.push_back(base);   m.indices.push_back(base+2); m.indices.push_back(base+1);
        m.indices.push_back(base);   m.indices.push_back(base+3); m.indices.push_back(base+2);
        m.indices.push_back(base+3); m.indices.push_back(base+4); m.indices.push_back(base+2);
    }

    m.minY = 0.0f; m.maxY = H + 0.5f;
    return m;
}

} // namespace PalmMesh
