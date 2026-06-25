#pragma once
#include <GL/glew.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>
#include <cmath>

// ======================================================================
// ЗАЩИТА (обязательный метод: Parallel Transport Frames)
//   СУТЬ:   тело «змея» — труба, заметённая вдоль Catmull-Rom сплайна.
//           Ориентация каждого кольца считается через PTF (rotation-
//           minimizing frame): нормаль ПЕРЕНОСИТСЯ вдоль кривой
//           поворотом на угол между соседними касательными (axis=T0xT1,
//           angle=acos(T0·T1)). Поэтому труба не перекручивается.
//   ПОЧЕМУ НЕ FRENET: рамка Френе скачет/крутится на изгибах — PTF нет.
//   ГДЕ:    морской змей патрулирует риф.
//   СЛОВА:  parallel transport, rotation-minimizing frame, без Frenet.
// ======================================================================
// ----------------------------------------------------------------------
// SplinePath  (mandatory method: Parallel Transport Frames)
//
// Builds a smooth "sea-serpent" body by sweeping a tube along a
// Catmull-Rom spline. The orientation of each cross-section ring is
// computed with PARALLEL TRANSPORT FRAMES (rotation-minimizing frames),
// so the tube never twists or flips even where the curve bends sharply.
//
// PTF algorithm (parallel transport / minimal rotation):
//   N[0] = any unit vector perpendicular to the start tangent T[0].
//   For each subsequent sample i:
//     axis  = T[i-1] x T[i]
//     angle = acos(T[i-1] . T[i])
//     N[i]  = rotate(N[i-1], axis, angle)   // carry the normal along
//     B[i]  = T[i] x N[i]
//   The frame {T, N, B} is then used to place a ring of vertices.
//
// The mesh (pos, normal, uv) is generated once on the CPU. At render
// time the model matrix patrols the serpent around the reef (facing its
// travel direction) and the vertex shader adds a travelling-sine
// undulation so it swims. PTF guarantees the cross-section stays
// coherent for that swim deformation.
// ----------------------------------------------------------------------
class SplinePath {
public:
    GLuint VAO = 0, VBO = 0, EBO = 0;
    int    indexCount = 0;
    float  bodyLength = 0.0f;   // local-space length along +Z

    // Catmull-Rom interpolation of one segment (p1->p2), t in [0,1].
    static glm::vec3 catmullRom(const glm::vec3& p0, const glm::vec3& p1,
                                const glm::vec3& p2, const glm::vec3& p3, float t) {
        float t2 = t * t, t3 = t2 * t;
        return 0.5f * ((2.0f * p1) +
                       (-p0 + p2) * t +
                       (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                       (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
    }

    // Build the serpent body. radialSegments = ring resolution,
    // pathSamples = subdivisions along the whole curve.
    void build(int radialSegments = 14, int pathSamples = 220, float radius = 1.6f) {
        // --- 1. control points: a long body roughly along +Z with a
        //        gentle 3D meander so the PTF has real curvature to
        //        transport through. ----------------------------------
        std::vector<glm::vec3> ctrl;
        const int N = 12;
        for (int i = 0; i < N; ++i) {
            float z = (float)i * 7.0f;
            float x = std::sin(i * 0.9f) * 5.0f;
            float y = std::sin(i * 0.5f) * 2.0f;
            ctrl.push_back(glm::vec3(x, y, z));
        }
        bodyLength = (float)(N - 1) * 7.0f;

        // --- 2. sample the Catmull-Rom curve into points + tangents ---
        std::vector<glm::vec3> P, T;
        int segs = (int)ctrl.size() - 3;
        for (int s = 0; s < segs; ++s) {
            const glm::vec3& p0 = ctrl[s];
            const glm::vec3& p1 = ctrl[s + 1];
            const glm::vec3& p2 = ctrl[s + 2];
            const glm::vec3& p3 = ctrl[s + 3];
            int sub = pathSamples / segs;
            for (int j = 0; j < sub; ++j) {
                float t = (float)j / (float)sub;
                glm::vec3 pos = catmullRom(p0, p1, p2, p3, t);
                // Tangent = derivative approximated by a small step.
                glm::vec3 posN = catmullRom(p0, p1, p2, p3, std::min(t + 0.01f, 1.0f));
                glm::vec3 tan = glm::normalize(posN - pos + glm::vec3(0, 0, 1e-4f));
                P.push_back(pos);
                T.push_back(tan);
            }
        }
        int M = (int)P.size();
        if (M < 2) return;

        // --- 3. PARALLEL TRANSPORT FRAMES -----------------------------
        std::vector<glm::vec3> Nf(M), Bf(M);
        // Initial normal: perpendicular to T[0].
        glm::vec3 up = std::abs(T[0].y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
        Nf[0] = glm::normalize(glm::cross(up, T[0]));
        Bf[0] = glm::normalize(glm::cross(T[0], Nf[0]));
        for (int i = 1; i < M; ++i) {
            glm::vec3 t0 = T[i - 1];
            glm::vec3 t1 = T[i];
            glm::vec3 axis = glm::cross(t0, t1);
            float axisLen = glm::length(axis);
            if (axisLen < 1e-5f) {
                // Tangents nearly parallel — carry the frame unchanged.
                Nf[i] = Nf[i - 1];
            } else {
                axis /= axisLen;
                float dotv = glm::clamp(glm::dot(t0, t1), -1.0f, 1.0f);
                float angle = std::acos(dotv);
                glm::mat4 R = glm::rotate(glm::mat4(1.0f), angle, axis);
                Nf[i] = glm::normalize(glm::vec3(R * glm::vec4(Nf[i - 1], 0.0f)));
            }
            // Re-orthogonalise against the current tangent.
            Nf[i] = glm::normalize(Nf[i] - t1 * glm::dot(Nf[i], t1));
            Bf[i] = glm::normalize(glm::cross(t1, Nf[i]));
        }

        // --- 4. sweep a tapered ellipse along the frames -------------
        std::vector<float> verts;
        std::vector<unsigned int> idx;
        verts.reserve(M * (radialSegments + 1) * 8);
        for (int i = 0; i < M; ++i) {
            float u = (float)i / (float)(M - 1);          // 0..1 along body
            // Eel taper: thin head, fat middle, thin tail. Slight
            // dorsal-ventral flattening for a fish silhouette.
            float taper = std::sin(u * 3.14159265f);
            float rr = radius * (0.18f + 0.95f * taper);
            for (int j = 0; j <= radialSegments; ++j) {
                float a = (float)j / (float)radialSegments * 6.2831853f;
                float ca = std::cos(a), sa = std::sin(a);
                glm::vec3 radial = Nf[i] * (ca * rr) + Bf[i] * (sa * rr * 0.72f);
                glm::vec3 pos = P[i] + radial;
                glm::vec3 nrm = glm::normalize(radial);
                verts.push_back(pos.x); verts.push_back(pos.y); verts.push_back(pos.z);
                verts.push_back(nrm.x); verts.push_back(nrm.y); verts.push_back(nrm.z);
                verts.push_back(u);
                verts.push_back((float)j / (float)radialSegments);
            }
        }
        int ring = radialSegments + 1;
        for (int i = 0; i < M - 1; ++i) {
            for (int j = 0; j < radialSegments; ++j) {
                int a = i * ring + j;
                int b = (i + 1) * ring + j;
                idx.push_back(a); idx.push_back(b); idx.push_back(a + 1);
                idx.push_back(a + 1); idx.push_back(b); idx.push_back(b + 1);
            }
        }
        indexCount = (int)idx.size();

        // --- 5. upload ----------------------------------------------
        glGenVertexArrays(1, &VAO);
        glGenBuffers(1, &VBO);
        glGenBuffers(1, &EBO);
        glBindVertexArray(VAO);
        glBindBuffer(GL_ARRAY_BUFFER, VBO);
        glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * sizeof(unsigned int), idx.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(6 * sizeof(float)));
        glBindVertexArray(0);
    }

    // Patrol transform: orbit the serpent around a centre at the given
    // radius/height, facing its direction of travel. `t` is seconds.
    glm::mat4 patrolModel(float t, glm::vec3 center, float orbitR, float speed) const {
        float ang = t * speed;
        glm::vec3 pos = center + glm::vec3(std::cos(ang) * orbitR, 0.0f, std::sin(ang) * orbitR);
        // Travel direction (tangent of the circle).
        glm::vec3 dir = glm::normalize(glm::vec3(-std::sin(ang), 0.0f, std::cos(ang)));
        // Build a yaw matrix that maps local +Z (body axis) to dir.
        glm::vec3 zAxis = dir;
        glm::vec3 yAxis = glm::vec3(0, 1, 0);
        glm::vec3 xAxis = glm::normalize(glm::cross(yAxis, zAxis));
        yAxis = glm::cross(zAxis, xAxis);
        glm::mat4 rot(1.0f);
        rot[0] = glm::vec4(xAxis, 0.0f);
        rot[1] = glm::vec4(yAxis, 0.0f);
        rot[2] = glm::vec4(zAxis, 0.0f);
        // Centre the body on its midpoint so it orbits nicely.
        glm::mat4 toMid = glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, -bodyLength * 0.5f));
        return glm::translate(glm::mat4(1.0f), pos) * rot * toMid;
    }

    void destroy() {
        if (EBO) glDeleteBuffers(1, &EBO);
        if (VBO) glDeleteBuffers(1, &VBO);
        if (VAO) glDeleteVertexArrays(1, &VAO);
        VAO = VBO = EBO = 0;
    }
};
