#include "VegetationSystem.h"
#include "../core/HeightmapLoader.h"
#include "../core/ModelLoader.h"
#include "../render/GLUniform.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>

// ----------------------------------------------------------------------
// Procedural kelp LOD construction
// ----------------------------------------------------------------------
// Each "card" is a vertical quad (or column of quads) crossing the
// origin. We rotate `cardCount` cards evenly around Y to build a
// volumetric-looking plant from any angle. Higher LODs use more cards
// and more vertical segments.
//
// Vertex layout (per LOD mesh):
//   pos.xyz  (3 floats)   — local space, base at y=0
//   uv.xy    (2 floats)   — for the leaf-strip texture
//   sway     (1 float)    — 0 at root, 1 at tip; used by vert shader
// = 6 floats per vertex
//
// Per-instance attributes are uploaded into a separate VBO and bound at
// locations 3 (instance position+yaw, vec4) and 4 (instance scale+phase,
// vec2). See render().
// ----------------------------------------------------------------------
void VegetationSystem::buildLOD(LODMesh& out, int cardCount, int segments,
                                float height, float width) {
    const float HEIGHT = height; // total plant height (world units)
    const float WIDTH  = width;  // card width

    std::vector<float>        verts;
    std::vector<unsigned int> idx;

    for (int c = 0; c < cardCount; ++c) {
        float ang = (float)c / (float)cardCount * 3.14159265f; // 0..PI (pairs cover full)
        float cx = std::cos(ang);
        float cz = std::sin(ang);

        unsigned int base = (unsigned int)(verts.size() / 6);

        for (int s = 0; s <= segments; ++s) {
            float t = (float)s / (float)segments;       // 0=root, 1=tip
            float y = t * HEIGHT;
            // Card narrows toward the tip
            float halfW = WIDTH * 0.5f * (1.0f - 0.4f * t);

            // Left vertex
            verts.push_back(-cx * halfW);
            verts.push_back( y);
            verts.push_back(-cz * halfW);
            verts.push_back(0.0f);
            verts.push_back(1.0f - t);                  // V grows downward
            verts.push_back(t);                         // sway weight

            // Right vertex
            verts.push_back( cx * halfW);
            verts.push_back( y);
            verts.push_back( cz * halfW);
            verts.push_back(1.0f);
            verts.push_back(1.0f - t);
            verts.push_back(t);
        }

        // Stitch quads
        for (int s = 0; s < segments; ++s) {
            unsigned int a = base + (unsigned int)(s * 2);
            unsigned int b = a + 1;
            unsigned int c2 = a + 2;
            unsigned int d = a + 3;
            idx.push_back(a); idx.push_back(b);  idx.push_back(c2);
            idx.push_back(b); idx.push_back(d);  idx.push_back(c2);
        }
    }

    out.indexCount = (int)idx.size();
    out.triCount   = out.indexCount / 3;
    out.stride     = 6;

    glGenVertexArrays(1, &out.VAO);
    glGenBuffers(1, &out.VBO);
    glGenBuffers(1, &out.EBO);
    glGenBuffers(1, &out.instanceVBO);

    glBindVertexArray(out.VAO);

    glBindBuffer(GL_ARRAY_BUFFER, out.VBO);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float),
                 verts.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, out.EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * sizeof(unsigned int),
                 idx.data(), GL_STATIC_DRAW);

    int strideBytes = 6 * sizeof(float);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, strideBytes, (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, strideBytes,
                          (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, strideBytes,
                          (void*)(5 * sizeof(float)));
    glEnableVertexAttribArray(2);

    // Per-instance attributes — bound to locations 3 and 4. The buffer
    // is filled per-frame in render(); allocate it as DYNAMIC_DRAW.
    glBindBuffer(GL_ARRAY_BUFFER, out.instanceVBO);
    glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_DYNAMIC_DRAW);
    // Attr 3: vec4 = (posX, posY, posZ, yaw)
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE,
                          6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(3);
    glVertexAttribDivisor(3, 1);
    // Attr 4: vec2 = (scale, phase)
    glVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE,
                          6 * sizeof(float),
                          (void*)(4 * sizeof(float)));
    glEnableVertexAttribArray(4);
    glVertexAttribDivisor(4, 1);

    glBindVertexArray(0);
}

bool VegetationSystem::init(float plantHeight, float plantWidth) {
    // Procedural fallback cards (used by seagrass / island grass; coral & kelp
    // replace these via loadModel()). Each card = `segments` quads = 2*segments
    // triangles, with `cardCount` cards per LOD:
    //   LOD0 = 4 cards x 6 segments = 48 tris  (close detail)
    //   LOD1 = 2 cards x 4 segments = 16 tris
    //   LOD2 = 1 card  x 2 segments =  4 tris  (silhouette only)
    // Exact counts are printed at startup: "Vegetation LODs built: a / b / c".
    buildLOD(lods[0], 4, 6, plantHeight, plantWidth);
    buildLOD(lods[1], 2, 4, plantHeight, plantWidth);
    buildLOD(lods[2], 1, 2, plantHeight, plantWidth);
    std::cout << "Vegetation LODs built: "
              << lods[0].triCount << " / "
              << lods[1].triCount << " / "
              << lods[2].triCount << " tris" << std::endl;
    return true;
}

// ----------------------------------------------------------------------
// Build a LOD from a real loaded mesh.
//
// The model's Y range is normalised to a target world height. LODs are
// produced by skipping every Nth triangle from the source — works
// surprisingly well for kelp/coral because they're built from many
// independent leaf clusters; throwing away triangles thins the foliage
// but keeps the silhouette intact. This is *much* simpler than a real
// edge-collapse decimator and good enough for distance LODs.
// ----------------------------------------------------------------------
void VegetationSystem::buildModelLOD(LODMesh& out,
                                     const LoadedMesh& src,
                                     int triangleStride,
                                     float scaleY,
                                     bool zUp)
{
    if (!src.valid()) return;

    // When the model is Z-up, the "height" lives in Z, not Y. Rotate
    // -90° about X so Z becomes Y: (x, y, z) -> (x, z, -y). Recompute
    // the vertical bounds from the rotated coordinate.
    int srcVerts = src.vertexCount();

    auto vpos = [&](int i, float& x, float& y, float& z) {
        float ox = src.vertices[i * 8 + 0];
        float oy = src.vertices[i * 8 + 1];
        float oz = src.vertices[i * 8 + 2];
        if (zUp) { x = ox; y = oz; z = -oy; }   // Z-up -> Y-up
        else     { x = ox; y = oy; z =  oz; }
    };
    auto vnrm = [&](int i, float& nx, float& ny, float& nz) {
        float ox = src.vertices[i * 8 + 5];
        float oy = src.vertices[i * 8 + 6];
        float oz = src.vertices[i * 8 + 7];
        if (zUp) { nx = ox; ny = oz; nz = -oy; }
        else     { nx = ox; ny = oy; nz =  oz; }
    };

    // Find vertical (Y) bounds after the up-axis fix.
    float minY = 1e30f, maxY = -1e30f;
    for (int i = 0; i < srcVerts; ++i) {
        float x, y, z; vpos(i, x, y, z);
        if (y < minY) minY = y;
        if (y > maxY) maxY = y;
    }
    float srcH = maxY - minY;
    if (srcH < 1e-4f) srcH = 1.0f;
    float s = scaleY / srcH;

    // Repack into the procedural vertex layout: pos.xyz | uv.xy | sway.
    std::vector<float> verts;
    verts.reserve(srcVerts * 6);
    for (int i = 0; i < srcVerts; ++i) {
        float x, y, z; vpos(i, x, y, z);
        x *= s;
        y  = (y - minY) * s;   // base sits at y=0
        z *= s;
        float u = src.vertices[i * 8 + 3];
        float v = src.vertices[i * 8 + 4];
        float swayW = (scaleY < 1e-4f) ? 0.0f : (y / scaleY);
        verts.push_back(x);
        verts.push_back(y);
        verts.push_back(z);
        verts.push_back(u);
        verts.push_back(v);
        verts.push_back(swayW);
    }

    // Pick every Nth triangle.
    std::vector<unsigned int> idx;
    int triCount = (int)(src.indices.size() / 3);
    int kept = 0;
    for (int t = 0; t < triCount; ++t) {
        if (triangleStride > 1 && (t % triangleStride) != 0) continue;
        idx.push_back(src.indices[t * 3 + 0]);
        idx.push_back(src.indices[t * 3 + 1]);
        idx.push_back(src.indices[t * 3 + 2]);
        ++kept;
    }

    out.indexCount = (int)idx.size();
    out.triCount   = kept;
    out.stride     = 6;

    if (out.VAO == 0) {
        glGenVertexArrays(1, &out.VAO);
        glGenBuffers(1, &out.VBO);
        glGenBuffers(1, &out.EBO);
        glGenBuffers(1, &out.instanceVBO);
    }
    glBindVertexArray(out.VAO);
    glBindBuffer(GL_ARRAY_BUFFER, out.VBO);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float),
                 verts.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, out.EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * sizeof(unsigned int),
                 idx.data(), GL_STATIC_DRAW);

    int strideBytes = 6 * sizeof(float);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, strideBytes, (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, strideBytes,
                          (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, strideBytes,
                          (void*)(5 * sizeof(float)));
    glEnableVertexAttribArray(2);

    glBindBuffer(GL_ARRAY_BUFFER, out.instanceVBO);
    glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE,
                          6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(3);
    glVertexAttribDivisor(3, 1);
    glVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE,
                          6 * sizeof(float),
                          (void*)(4 * sizeof(float)));
    glEnableVertexAttribArray(4);
    glVertexAttribDivisor(4, 1);

    glBindVertexArray(0);
}

bool VegetationSystem::loadModel(const LoadedMesh& mesh,
                                 float baseScale,
                                 GLuint textureID,
                                 bool zUp)
{
    if (!mesh.valid()) {
        std::cerr << "VegetationSystem::loadModel: invalid mesh, "
                     "keeping procedural LODs.\n";
        return false;
    }

    // Drop the procedural meshes and rebuild from this asset.
    for (auto& lod : lods) {
        if (lod.VAO)         glDeleteVertexArrays(1, &lod.VAO);
        if (lod.VBO)         glDeleteBuffers(1, &lod.VBO);
        if (lod.EBO)         glDeleteBuffers(1, &lod.EBO);
        if (lod.instanceVBO) glDeleteBuffers(1, &lod.instanceVBO);
        lod = {};
    }

    // LOD0 = full mesh, LOD1 = every 3rd tri (~33%), LOD2 = every 9th (~11%).
    buildModelLOD(lods[0], mesh, 1, baseScale, zUp);
    buildModelLOD(lods[1], mesh, 3, baseScale, zUp);
    buildModelLOD(lods[2], mesh, 9, baseScale, zUp);

    modelTexture     = textureID;
    usingLoadedModel = true;

    std::cout << "Vegetation LODs from model: "
              << lods[0].triCount << " / "
              << lods[1].triCount << " / "
              << lods[2].triCount << " tris\n";
    return true;
}

void VegetationSystem::shutdown() {
    for (auto& lod : lods) {
        if (lod.VAO)         glDeleteVertexArrays(1, &lod.VAO);
        if (lod.VBO)         glDeleteBuffers(1, &lod.VBO);
        if (lod.EBO)         glDeleteBuffers(1, &lod.EBO);
        if (lod.instanceVBO) glDeleteBuffers(1, &lod.instanceVBO);
        lod = {};
    }
    instances.clear();
}

// ----------------------------------------------------------------------
// Smooth value noise in [0,1] for the patchy-meadow filter. Hash the
// integer lattice, bilinear-blend with a smoothstep — cheap and stable.
// ----------------------------------------------------------------------
namespace {
float latticeHash(int x, int y) {
    unsigned int h = (unsigned int)(x * 374761393 + y * 668265263);
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xFFFFFF) / (float)0xFFFFFF;
}
float valueNoise(float x, float y) {
    int ix = (int)std::floor(x), iy = (int)std::floor(y);
    float fx = x - ix, fy = y - iy;
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);
    float a = latticeHash(ix, iy),     b = latticeHash(ix + 1, iy);
    float c = latticeHash(ix, iy + 1), d = latticeHash(ix + 1, iy + 1);
    return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}
} // namespace

// ----------------------------------------------------------------------
// Scatter instances. We sample random points in [0,1]² UV space, look
// up the heightmap, and accept the point only if:
//   * it isn't NO_TERRAIN
//   * the world-space height is in [minWorldY, maxWorldY]
//   * the slope (height delta over a 2-texel step, see below) is under 0.6
//   * (optional) the point falls inside a noise "meadow" patch
// ----------------------------------------------------------------------
void VegetationSystem::scatter(int targetCount,
                               const HeightmapData& hm,
                               float terrainSize,
                               float heightScale,
                               float minWorldY,
                               float maxWorldY)
{
    instances.clear();
    instances.reserve(targetCount);

    if (hm.width == 0 || hm.height == 0) return;

    const float NO_TERRAIN = -90000.0f;
    std::mt19937 rng(0xC0FFEE);
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);
    std::uniform_real_distribution<float> uni2(-1.0f, 1.0f);

    int attempts = 0;
    // Generous attempt budget: when the valid band is a small fraction of
    // the map (island tops), the old 12x cap starved the scatter long
    // before reaching the target count.
    int maxAttempts = targetCount * 40;
    float halfSize = terrainSize * 0.5f;
    int cluster = std::max(1, clusterCount);

    // Validate a UV-space sample against terrain band / slope / meadow
    // noise; reports the accepted world height.
    auto sampleOK = [&](float u, float v, float& worldY) -> bool {
        if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) return false;
        int sx = std::min((int)(u * hm.width),  hm.width  - 1);
        int sy = std::min((int)(v * hm.height), hm.height - 1);
        float h = hm.heights[sy * hm.width + sx];
        if (h < NO_TERRAIN + 1.0f) return false;

        worldY = h * heightScale;
        if (worldY < minWorldY || worldY > maxWorldY) return false;

        // Reject steep slopes — plants grow on flat ground, not cliffs.
        int sxR = std::min(sx + 2, hm.width  - 1);
        int syD = std::min(sy + 2, hm.height - 1);
        float hR = hm.heights[sy * hm.width + sxR];
        float hD = hm.heights[syD * hm.width + sx];
        if (hR < NO_TERRAIN + 1.0f || hD < NO_TERRAIN + 1.0f) return false;
        float dx = (hR - h) * heightScale;
        float dy = (hD - h) * heightScale;
        if (std::sqrt(dx * dx + dy * dy) > 0.6f) return false;

        // Patchy meadows: only grow where the smooth noise field is above
        // the threshold, so grass/seagrass forms natural-looking beds
        // instead of a uniform carpet.
        if (patchScale > 0.0f &&
            valueNoise(u * patchScale, v * patchScale) < patchThreshold)
            return false;
        return true;
    };

    while ((int)instances.size() < targetCount && attempts++ < maxAttempts) {
        float u = uni(rng);
        float v = uni(rng);
        float worldY;
        if (!sampleOK(u, v, worldY)) continue;

        // One accepted sample seeds a small cluster — much denser cover
        // for the same number of (expensive) rejection samples.
        for (int k = 0; k < cluster && (int)instances.size() < targetCount; ++k) {
            float u2 = u, v2 = v, y2 = worldY;
            if (k > 0) {
                u2 = u + uni2(rng) * (clusterRadius / terrainSize);
                v2 = v + uni2(rng) * (clusterRadius / terrainSize);
                if (!sampleOK(u2, v2, y2)) continue;
            }
            Instance inst;
            inst.position = glm::vec3(
                u2 * terrainSize - halfSize,
                y2 - groundSink,                        // sink base into the ground
                v2 * terrainSize - halfSize);
            inst.yaw      = uni(rng) * 6.2831853f;
            inst.scale    = 0.7f + uni(rng) * 0.8f;
            inst.lifeDist = pruneMin + uni(rng) * (pruneMax - pruneMin);
            instances.push_back(inst);
        }
    }

    std::cout << "Vegetation scattered: " << instances.size()
              << " / " << targetCount
              << " (after " << attempts << " attempts)" << std::endl;
}

// ----------------------------------------------------------------------
// Per-frame render — bucket instances by camera distance, upload three
// instance VBOs, fire three glDrawElementsInstanced calls.
// ----------------------------------------------------------------------
void VegetationSystem::render(GLuint program,
                              const glm::vec3& camPos,
                              const glm::mat4& view,
                              const glm::mat4& projection,
                              const glm::vec3& sunDir,
                              float time)
{
    if (instances.empty()) return;

    // Per-instance attribute layout: vec4(pos.xyz, yaw) + vec2(scale, phase)
    // = 6 floats per instance.
    static thread_local std::vector<float> bucket0, bucket1, bucket2;
    bucket0.clear(); bucket1.clear(); bucket2.clear();
    bucket0.reserve(instances.size() * 6 / 4);
    bucket1.reserve(instances.size() * 6 / 4);
    bucket2.reserve(instances.size() * 6 / 4);

    for (const auto& inst : instances) {
        float d = glm::length(camPos - inst.position);

        // STOCHASTIC PRUNING — each plant has its own die-off radius
        // randomised in [pruneMin, pruneMax]. Plants vanish gradually
        // with range so there's no visible "wall" of disappearing
        // geometry as the camera moves.
        if (d > inst.lifeDist) continue;

        // Use a per-instance hash as the per-vertex sway phase so all
        // plants don't sway in lock-step.
        float phase = std::sin(inst.position.x * 13.7f + inst.position.z * 7.3f);

        std::vector<float>* dst;
        if      (d < lodDist0) dst = &bucket0;
        else if (d < lodDist1) dst = &bucket1;
        else                   dst = &bucket2;

        dst->push_back(inst.position.x);
        dst->push_back(inst.position.y);
        dst->push_back(inst.position.z);
        dst->push_back(inst.yaw);
        dst->push_back(inst.scale);
        dst->push_back(phase);
    }

    drawnLOD0 = (int)bucket0.size() / 6;
    drawnLOD1 = (int)bucket1.size() / 6;
    drawnLOD2 = (int)bucket2.size() / 6;

    glUseProgram(program);
    glUniformMatrix4fv(glGetUniformLocation(program, "view"),
                       1, GL_FALSE, glm::value_ptr(view));
    glUniformMatrix4fv(glGetUniformLocation(program, "projection"),
                       1, GL_FALSE, glm::value_ptr(projection));
    glUniform3fv(glGetUniformLocation(program, "cameraPos"),
                 1, glm::value_ptr(camPos));
    glUniform3fv(glGetUniformLocation(program, "sunDirection"),
                 1, glm::value_ptr(sunDir));
    setFloat(program, "time", time);
    setFloat(program, "swayStrength", swayStrength);
    setInt(program, "landMode", landMode);
    setInt(program, "landCutout", landCutout);
    glUniform3fv(glGetUniformLocation(program, "colorRoot"), 1, &colorRoot.x);
    glUniform3fv(glGetUniformLocation(program, "colorTip"),  1, &colorTip.x);

    // If a model texture is bound, the shader switches from its
    // procedural leaf shape to a real texture sample.
    glUniform1i(glGetUniformLocation(program, "useTexture"),
                modelTexture ? 1 : 0);
    if (modelTexture) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, modelTexture);
        setInt(program, "albedoTex", 0);
    }

    // Two-sided rendering — leaves are flat cards seen from both sides.
    // Save and restore GL state so we don't corrupt anything that comes
    // after us (the water pass relies on its own cull / blend setup).
    GLboolean prevBlend     = glIsEnabled(GL_BLEND);
    GLboolean prevCull      = glIsEnabled(GL_CULL_FACE);
    GLboolean prevDepthMask;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);

    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    // Keep depth writes ON so the kelp occludes itself correctly.
    glDepthMask(GL_TRUE);

    auto draw = [&](LODMesh& lod, std::vector<float>& bucket) {
        if (bucket.empty()) return;
        glBindBuffer(GL_ARRAY_BUFFER, lod.instanceVBO);
        glBufferData(GL_ARRAY_BUFFER,
                     bucket.size() * sizeof(float),
                     bucket.data(),
                     GL_DYNAMIC_DRAW);
        glBindVertexArray(lod.VAO);
        glDrawElementsInstanced(GL_TRIANGLES,
                                lod.indexCount,
                                GL_UNSIGNED_INT,
                                0,
                                (GLsizei)(bucket.size() / 6));
    };

    draw(lods[0], bucket0);
    draw(lods[1], bucket1);
    draw(lods[2], bucket2);

    // Restore previous GL state — water rendering after us depends on
    // the engine-wide defaults being intact.
    if (prevBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (prevCull)  glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    glDepthMask(prevDepthMask);
}

// ----------------------------------------------------------------------
// Depth-only render for the shadow map. Buckets every live instance into
// a single buffer (no LOD split needed for shadows — we just want
// occluders) and dispatches one instanced draw with the depth program.
// ----------------------------------------------------------------------
void VegetationSystem::renderDepth(GLuint depthProgram,
                                   const glm::vec3& camPos,
                                   float time)
{
    if (instances.empty()) return;

    static thread_local std::vector<float> bucket;
    bucket.clear();
    bucket.reserve(instances.size() * 6);

    for (const auto& inst : instances) {
        float d = glm::length(camPos - inst.position);
        if (d > inst.lifeDist) continue;       // same pruning as main pass
        // Only cast shadows for reasonably close plants — distant ones
        // contribute little and keep the depth pass cheap.
        if (d > lodDist1 * 1.5f) continue;
        float phase = std::sin(inst.position.x * 13.7f + inst.position.z * 7.3f);
        bucket.push_back(inst.position.x);
        bucket.push_back(inst.position.y);
        bucket.push_back(inst.position.z);
        bucket.push_back(inst.yaw);
        bucket.push_back(inst.scale);
        bucket.push_back(phase);
    }
    if (bucket.empty()) return;

    glUseProgram(depthProgram);
    setInt(depthProgram, "instanced", 1);
    setFloat(depthProgram, "time", time);

    // Use LOD0 geometry as the shadow caster.
    LODMesh& lod = lods[0];
    glBindBuffer(GL_ARRAY_BUFFER, lod.instanceVBO);
    glBufferData(GL_ARRAY_BUFFER, bucket.size() * sizeof(float),
                 bucket.data(), GL_DYNAMIC_DRAW);
    glBindVertexArray(lod.VAO);
    glDrawElementsInstanced(GL_TRIANGLES, lod.indexCount, GL_UNSIGNED_INT,
                            0, (GLsizei)(bucket.size() / 6));
    glBindVertexArray(0);
}
