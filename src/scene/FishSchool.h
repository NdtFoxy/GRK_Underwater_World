#pragma once
#include <GL/glew.h>
#include <glm/glm.hpp>
#include <vector>

// ----------------------------------------------------------------------
// FishSchool — a school of small fish with a lightweight schooling model.
//
// Behaviour (see update()): instead of classic boids (which compare every
// fish to every other — O(N^2)), each fish is steered toward a personal
// point orbiting a shared, slowly-wandering anchor. The shared anchor keeps
// the group together; the per-fish phase offset spreads them out. On top of
// that: a vertical comfort band (off the seabed, below the surface) and
// player avoidance — swim into the school and it splits around you.
// CPU cost is O(N) per school, with NO pairwise neighbour checks.
//
// Rendering is ONE instanced draw call (the A07 technique): a tiny
// procedural fish mesh (~10 tris) plus a per-instance buffer of
// position + velocity + phase; the vertex shader orients each fish along
// its velocity and swings the tail. The school is distance-culled as a unit.
// ----------------------------------------------------------------------
class FishSchool {
public:
    void init(int count, const glm::vec3& center, unsigned seed);
    void update(float dt, float time, const glm::vec3& playerPos,
                float floorY, float waterLevel, float storm);
    void render(GLuint program, const glm::mat4& view, const glm::mat4& projection,
                const glm::vec3& camPos, const glm::vec3& sunDir, float time);
    void shutdown();

    const glm::vec3& centroid() const { return centerCache; }
    int  count() const { return (int)fish.size(); }

    // How far the school's anchor wanders from home (default: roaming;
    // small value = resident school, e.g. circling the wreck).
    float wanderRadius = 28.0f;

    // Air mode: the flock stays in a band ABOVE the waterline and renders
    // sky-lit instead of fogged. The comfort band becomes
    // [waterLevel+airBandLo, waterLevel+airBandHi].
    bool  airMode   = false;
    float airBandLo = 6.0f;
    float airBandHi = 34.0f;

private:
    // One fish. `phase` does double duty: it offsets the tail-sway in the
    // shader AND offsets this fish's orbit angle in update(), so neighbours
    // don't move in lockstep.
    struct Fish {
        glm::vec3 pos;
        glm::vec3 vel;
        float     phase;
    };
    std::vector<Fish> fish;
    glm::vec3 anchorHome  = glm::vec3(0.0f);  // home point the wander orbits around
    glm::vec3 centerCache = glm::vec3(0.0f);  // last-frame school centre (for the distance cull)

    // GPU resources: shared mesh (vao/vbo/ebo) + the per-instance buffer.
    GLuint vao = 0, vbo = 0, ebo = 0, instanceVBO = 0;
    int    indexCount = 0;
    std::vector<float> instanceData;   // CPU-side scratch buffer re-uploaded each frame

    void buildMesh();   // builds the procedural fish mesh + instance attributes
};
