#pragma once
#include <GL/glew.h>
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <cmath>

struct HeightmapData;
struct LoadedMesh;

// ======================================================================
// ЗАЩИТА (метод A07: Instanced rendering with LOD)  — 30 баллов
//   СУТЬ:   тысячи растений рисуются ОДНИМ draw call на уровень LOD
//           (glDrawElementsInstanced) — позиция/поворот/масштаб каждого
//           экземпляра лежат в отдельном instance-VBO (glVertexAttribDivisor).
//   LOD:    3 версии меша (high/mid/low по треугольникам); близкие берут
//           LOD0, дальние LOD1/LOD2 — переключение по дистанции до камеры.
//   БОНУС:  стохастический прунинг — у каждого своя «дистанция смерти»,
//           поэтому плотность спадает плавно, без резкого «попа».
//   ГДЕ:    заросли водорослей / кораллов / травы на дне.
//   СЛОВА:  instancing, дискретный LOD, attrib divisor, draw calls.
// ======================================================================
// ----------------------------------------------------------------------
// VegetationSystem
//
// Renders thousands of kelp-like plants over the terrain using:
//   * GPU instancing  — one draw call per LOD level
//   * Discrete LOD    — 3 procedural meshes (high / mid / low triangle count)
//   * Stochastic pruning — each instance has a random "death distance"
//                          so the density falls off smoothly with range
//                          instead of popping out at a hard threshold
//
// Spawned once at scene init; per-frame the system buckets active
// instances into 3 GPU buffers and dispatches glDrawElementsInstanced.
// ----------------------------------------------------------------------
class VegetationSystem {
public:
    struct Instance {
        glm::vec3 position;   // world position of the plant base
        float     yaw;        // rotation around Y
        float     scale;      // size multiplier
        float     lifeDist;   // stochastic pruning radius (camera-distance)
    };

    // Build procedural LOD meshes. Default proportions make kelp-like
    // plants; pass e.g. (0.8, 0.35) for grass tufts or (2.3, 0.5) for
    // seagrass blades.
    bool init(float plantHeight = 8.0f, float plantWidth = 1.6f);
    void shutdown();

    // Replace the procedural meshes with a real model. The model is
    // used for LOD0 (full detail). LOD1 / LOD2 are derived by selecting
    // a stride of indices (cheap auto-decimation that works because
    // the source meshes are made of dense leaf clusters).
    //
    // baseScale: world-space height the mesh should map to. The loader
    // auto-fits the asset's Y range to this.
    // textureID: GL texture to bind when this mesh draws (0 = use the
    // shader's default procedural look).
    // zUp: if true, the source model is Z-up (common in Sketchfab /
    // 3ds Max exports) and will be rotated to the engine's Y-up.
    bool loadModel(const LoadedMesh& mesh, float baseScale, GLuint textureID,
                   bool zUp = false);

    // Scatter instances over the terrain. Picks only flat sandy cells in
    // the depth band [minWorldY, maxWorldY] (e.g. -25..-3 for shelf zone).
    void scatter(int targetCount,
                 const HeightmapData& hm,
                 float terrainSize,
                 float heightScale,
                 float minWorldY,
                 float maxWorldY);

    // Hand-placed extra instance (wreck overgrowth, set dressing). Call
    // after scatter(); position must already sit on the ground.
    void addInstance(const glm::vec3& pos, float scale) {
        Instance inst;
        inst.position = pos - glm::vec3(0.0f, groundSink, 0.0f);
        float h = pos.x * 12.9898f + pos.z * 78.233f;
        inst.yaw      = (h - std::floor(h)) * 6.2831853f;
        inst.scale    = scale;
        inst.lifeDist = pruneMax;
        instances.push_back(inst);
    }

    // Per-frame draw. The shader program must already be linked and use
    // the attribute slots described in the .vert file (locations 3,4 are
    // per-instance).
    void render(GLuint program,
                const glm::vec3& camPos,
                const glm::mat4& view,
                const glm::mat4& projection,
                const glm::vec3& sunDir,
                float time);

    // Depth-only render for the shadow pass. Uses the same instance
    // buckets but draws into the sun's depth map with a minimal program.
    void renderDepth(GLuint depthProgram,
                     const glm::vec3& camPos,
                     float time);

    // Stats for the most recent frame — handy for ImGui overlay.
    int totalInstances() const { return (int)instances.size(); }
    int drawnLOD0 = 0, drawnLOD1 = 0, drawnLOD2 = 0;

    // LOD switch distances. Public so the user can tune from outside.
    float lodDist0 = 30.0f;
    float lodDist1 = 80.0f;
    float pruneMin = 120.0f;   // closest possible "lifeDist"
    float pruneMax = 250.0f;   // furthest possible "lifeDist"
    float swayStrength = 1.0f; // 1.0 sways (kelp), 0.0 rigid (coral)
    float groundSink   = 0.05f;// how far the base is pushed into the seabed
    int   landMode     = 0;    // 1 = above-water plant (no underwater fog/glow)
    int   landCutout   = 0;    // landMode only: keep the procedural leaf cutout
                               // (grass blades) instead of forcing solid cards
    // Procedural plant colour gradient root->tip. Negative root.r means
    // "use the shader's built-in kelp/palm colours" (back-compat).
    glm::vec3 colorRoot = glm::vec3(-1.0f);
    glm::vec3 colorTip  = glm::vec3(-1.0f);
    // Patchy scatter: accept points only inside smooth-noise "meadows".
    // patchScale = noise frequency over the whole map (0 = everywhere),
    // patchThreshold = coverage cut (higher = rarer patches).
    float patchScale     = 0.0f;
    float patchThreshold = 0.5f;
    // Cluster scatter: each accepted sample spawns up to clusterCount
    // instances within clusterRadius metres. Vital when the valid area
    // (island tops) is a tiny fraction of the map — random sampling alone
    // accepts too few points for dense cover.
    int   clusterCount  = 1;
    float clusterRadius = 1.5f;

private:
    struct LODMesh {
        GLuint VAO         = 0;
        GLuint VBO         = 0;
        GLuint EBO         = 0;
        GLuint instanceVBO = 0;
        int    indexCount  = 0;
        int    triCount    = 0;
        // 6 floats per vertex for procedural LODs, 8 for loaded models.
        int    stride      = 6;
    };
    LODMesh lods[3];
    GLuint  modelTexture = 0;       // optional model albedo (0 = none)
    bool    usingLoadedModel = false;

    std::vector<Instance> instances;

    // Build one procedural plant LOD: `cardCount` flat strips arranged
    // in a star pattern around Y, each subdivided into `segments` rows
    // along its height. Proportions come from init().
    void buildLOD(LODMesh& out, int cardCount, int segments,
                  float height, float width);

    // Build a LOD from a loaded mesh, optionally taking every `stride`-th
    // triangle to reduce the index count. The vertex buffer is shared,
    // so this is a cheap way to derive lower LODs without a real
    // decimator.
    void buildModelLOD(LODMesh& out,
                       const LoadedMesh& src,
                       int triangleStride,
                       float scaleY,
                       bool zUp);
};
