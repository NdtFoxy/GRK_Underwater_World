#include "FishSchool.h"
#include "../render/GLUniform.h"
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cmath>
#include <random>

// ----------------------------------------------------------------------
// Procedural fish: a stretched octahedron body + a vertical tail fin.
// Local space: nose +Z, back +Y; ~0.55 m long. Vertex = pos(3)+normal(3).
// ----------------------------------------------------------------------
void FishSchool::buildMesh() {
    const glm::vec3 bodyC(0.0f, 0.0f, 0.02f);
    glm::vec3 P[6] = {
        { 0.00f,  0.00f,  0.30f},   // nose
        { 0.00f,  0.00f, -0.20f},   // tail root
        { 0.00f,  0.09f,  0.03f},   // back
        { 0.00f, -0.07f,  0.03f},   // belly
        {-0.045f, 0.00f,  0.03f},   // left
        { 0.045f, 0.00f,  0.03f},   // right
    };
    int F[8][3] = {
        {0,2,5}, {0,5,3}, {0,3,4}, {0,4,2},   // nose half
        {1,5,2}, {1,3,5}, {1,4,3}, {1,2,4},   // tail half
    };

    std::vector<float> v;
    std::vector<unsigned int> idx;
    auto pushV = [&](const glm::vec3& p, const glm::vec3& n) {
        v.push_back(p.x); v.push_back(p.y); v.push_back(p.z);
        v.push_back(n.x); v.push_back(n.y); v.push_back(n.z);
        return (unsigned int)(v.size() / 6 - 1);
    };
    // Smooth-ish body: per-vertex normal away from the body centre.
    unsigned int bi[6];
    for (int i = 0; i < 6; ++i)
        bi[i] = pushV(P[i], glm::normalize(P[i] - bodyC + glm::vec3(0, 0, 1e-3f)));
    for (auto& f : F) {
        idx.push_back(bi[f[0]]); idx.push_back(bi[f[1]]); idx.push_back(bi[f[2]]);
    }
    // Tail fin: flat quad in the YZ plane (two-sided lighting in shader).
    unsigned int t0 = pushV({0.0f,  0.085f, -0.20f}, {1, 0, 0});
    unsigned int t1 = pushV({0.0f, -0.075f, -0.20f}, {1, 0, 0});
    unsigned int t2 = pushV({0.0f,  0.110f, -0.36f}, {1, 0, 0});
    unsigned int t3 = pushV({0.0f, -0.095f, -0.36f}, {1, 0, 0});
    idx.push_back(t0); idx.push_back(t1); idx.push_back(t2);
    idx.push_back(t1); idx.push_back(t3); idx.push_back(t2);

    indexCount = (int)idx.size();

    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glGenBuffers(1, &ebo);
    glGenBuffers(1, &instanceVBO);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(float), v.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * sizeof(unsigned int), idx.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // Per-instance: pos(3) + vel(3) + phase(1) = 7 floats.
    glBindBuffer(GL_ARRAY_BUFFER, instanceVBO);
    glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(2);
    glVertexAttribDivisor(2, 1);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(3);
    glVertexAttribDivisor(3, 1);
    glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(4);
    glVertexAttribDivisor(4, 1);
    glBindVertexArray(0);
}

void FishSchool::init(int count, const glm::vec3& center, unsigned seed) {
    buildMesh();
    anchorHome = center;
    centerCache = center;

    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> uni(-1.0f, 1.0f);
    glm::vec3 schoolDir = glm::normalize(glm::vec3(uni(rng), 0.0f, uni(rng)) + glm::vec3(0.01f, 0, 0));

    fish.resize(count);
    for (auto& f : fish) {
        f.pos = center + glm::vec3(uni(rng) * 6.0f, uni(rng) * 2.5f, uni(rng) * 6.0f);
        f.vel = schoolDir * 2.5f + glm::vec3(uni(rng), uni(rng) * 0.3f, uni(rng)) * 0.8f;
        f.phase = uni(rng) * 3.14159f;
    }
    instanceData.reserve(count * 7);
}

// ----------------------------------------------------------------------
// Advance the school by one frame. This is the LIGHTWEIGHT replacement for
// classic boids: there are NO pairwise neighbour queries (which would be
// O(n^2)). Instead every fish is steered toward a personal point that
// orbits a shared, slowly-wandering "anchor". Because the anchor is common
// to the whole school the group stays together; because each fish's orbit
// is offset by its own phase, they spread out and read as a loose shoal.
// Cost is O(n) per school. On top of that base motion we layer two cheap
// behaviours: fleeing the diver and a vertical comfort band.
//   dt        : frame time (clamped, so a stutter can't fling fish away)
//   time      : global clock, drives the wander + orbit phases
//   playerPos : diver position the school flees from
//   floorY    : seabed height under the school (lower bound of the band)
//   waterLevel: sea level (upper bound of the band)
//   storm     : 0..1 sea state; agitates the school (raises top speed)
// ----------------------------------------------------------------------
void FishSchool::update(float dt, float time, const glm::vec3& playerPos,
                        float floorY, float waterLevel, float storm) {
    if (fish.empty()) return;
    dt = std::min(dt, 0.05f);   // physics guard: cap dt so hitches don't explode positions

    // Wandering anchor keeps the school roaming its home range instead of
    // sitting on a fixed point. Three out-of-phase sines trace a lazy,
    // never-repeating path around `anchorHome`.
    glm::vec3 anchor = anchorHome +
        glm::vec3(std::sin(time * 0.045f) * wanderRadius,
                  std::sin(time * 0.060f + 2.1f) * 4.0f,
                  std::cos(time * 0.038f + 0.7f) * wanderRadius);

    // Per-frame tuning constants (hoisted out of the loop so they're computed
    // once, not once per fish).
    const float maxSpeed = 4.6f + storm * 2.0f;   // storm agitates the school
    const float minSpeed = 1.6f;                   // never freeze in place
    const float bandLo   = airMode ? waterLevel + airBandLo : floorY + 2.5f;   // stay off the seabed
    const float bandHi   = airMode ? waterLevel + airBandHi : waterLevel - 3.0f; // stay under the surface
    const float climbCap = airMode ? 2.6f : 1.6f;  // air-mode flocks bank/climb more freely
    const float fleeR    = 8.0f;                    // diver scares fish within this radius (m)
    const float fleeR2   = fleeR * fleeR;           // squared, to compare without a sqrt

    glm::vec3 centroid(0.0f);
    size_t n = fish.size();
    for (size_t i = 0; i < n; ++i) {
        Fish& fi = fish[i];
        // Base motion: steer toward this fish's personal orbit point around
        // the anchor. `a` is its angle on the ring; the 4 m / 1.2 m radii give
        // a flat, slightly bobbing loop. (cos a, .., sin a) is the ring; the
        // `* 0.8f` is a soft spring so the fish eases toward the point.
        float a = time * 0.5f + fi.phase * 4.0f;
        glm::vec3 target = anchor + glm::vec3(std::cos(a) * 4.0f,
                                              std::sin(a * 0.5f) * 1.2f,
                                              std::sin(a) * 4.0f);
        glm::vec3 steer = (target - fi.pos) * 0.8f;

        // Flee the diver: push away (1/r falloff, strongest up close) so the
        // school visibly splits and reforms around the camera.
        glm::vec3 dp = fi.pos - playerPos;
        float pd2 = glm::dot(dp, dp);
        if (pd2 < fleeR2 && pd2 > 1e-4f) {
            float pd = std::sqrt(pd2);
            steer += (dp / pd) * (fleeR - pd) * 2.2f;
        }

        // Vertical comfort band: off the seabed, away from the surface.
        if (fi.pos.y < bandLo) steer.y += (bandLo - fi.pos.y) * 3.0f;
        if (fi.pos.y > bandHi) steer.y -= (fi.pos.y - bandHi) * 3.0f;

        // Integrate: accelerate by the accumulated steering, then constrain.
        fi.vel += steer * dt;
        // Mostly horizontal movers — cap the climb rate (looser in air mode).
        fi.vel.y = glm::clamp(fi.vel.y, -climbCap, climbCap);
        // Clamp the speed into [minSpeed, maxSpeed] without changing heading
        // (rescale the vector by the ratio). Keeps the swim lively but bounded.
        float sp = glm::length(fi.vel);
        if (sp > maxSpeed) fi.vel *= maxSpeed / sp;
        else if (sp < minSpeed && sp > 1e-4f) fi.vel *= minSpeed / sp;

        fi.pos += fi.vel * dt;
        centroid += fi.pos;
    }
    // Cache the school centre: render() uses it for a single cheap distance
    // test to cull the whole school when it's far from the camera.
    centerCache = centroid / (float)n;
}

// Draw the whole school in ONE instanced draw call (the A07 technique): the
// fish mesh is shared, and a per-instance buffer supplies each fish's
// pos/vel/phase so the vertex shader places and animates it on the GPU.
void FishSchool::render(GLuint program, const glm::mat4& view, const glm::mat4& projection,
                        const glm::vec3& camPos, const glm::vec3& sunDir, float time) {
    if (!program || fish.empty() || indexCount <= 0) return;
    // Distance cull: skip the entire school with one length test rather than
    // uploading and drawing ~200 invisible fish.
    if (glm::length(camPos - centerCache) > 420.0f) return;

    // Repack the live per-fish state into the flat instance array (7 floats
    // each). `instanceData` keeps its capacity between frames (reserved in
    // init), so this clear+refill does not reallocate.
    instanceData.clear();
    for (const auto& f : fish) {
        instanceData.push_back(f.pos.x); instanceData.push_back(f.pos.y); instanceData.push_back(f.pos.z);
        instanceData.push_back(f.vel.x); instanceData.push_back(f.vel.y); instanceData.push_back(f.vel.z);
        instanceData.push_back(f.phase);
    }

    glUseProgram(program);
    setMat4(program, "view", view);
    setMat4(program, "projection", projection);
    setVec3(program, "cameraPos", camPos);
    setVec3(program, "sunDirection", sunDir);
    setFloat(program, "time", time);
    setFloat(program, "uAir", airMode ? 1.0f : 0.0f);

    GLboolean cullWas = glIsEnabled(GL_CULL_FACE);
    glDisable(GL_CULL_FACE);   // tail fin is a two-sided card
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, instanceVBO);
    glBufferData(GL_ARRAY_BUFFER, instanceData.size() * sizeof(float),
                 instanceData.data(), GL_DYNAMIC_DRAW);
    glDrawElementsInstanced(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, 0,
                            (GLsizei)fish.size());
    glBindVertexArray(0);
    if (cullWas) glEnable(GL_CULL_FACE);
}

void FishSchool::shutdown() {
    if (vao) glDeleteVertexArrays(1, &vao);
    if (vbo) glDeleteBuffers(1, &vbo);
    if (ebo) glDeleteBuffers(1, &ebo);
    if (instanceVBO) glDeleteBuffers(1, &instanceVBO);
    vao = vbo = ebo = instanceVBO = 0;
    fish.clear();
}
