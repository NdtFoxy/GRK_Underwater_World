// ----------------------------------------------------------------------
// Scene — static props (graded method A07: instanced rendering + LOD).
// Split out of Scene.cpp: cached texture loading, prop mesh build with
// triangle-reduction LOD index sets, per-submesh draw, prop placement and
// the frustum-culled / distance-LOD prop render pass. All methods belong
// to the Scene class declared in Scene.h.
// ----------------------------------------------------------------------
#include "Scene.h"
#include "SceneInternal.h"
#include "../render/GLUniform.h"
#include "../core/ModelLoader.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cmath>
#include <vector>
#include <random>
#include <iostream>

// ----------------------------------------------------------------------
// Static props (rocks / cliffs / foliage loaded from glTF). One shared
// GPU mesh per asset, many placements via the object shader.
// ----------------------------------------------------------------------
// Load a texture once and reuse it for every submesh that references the
// same file (rock packs share atlases; palms reuse trunk maps).
GLuint Scene::loadTextureCached(const std::string& path) {
    if (path.empty()) return 0;
    auto it = textureCache.find(path);
    if (it != textureCache.end()) return it->second;
    GLuint id = loadTexture(path.c_str());
    textureCache[path] = id;
    return id;
}

// Build a GPU prop mesh from an already-loaded LoadedMesh: normalize the
// geometry (centre XZ, base at y=0, fit horizontal extent to targetSize)
// and upload one VAO plus a PropSub per material slice with its own
// resolved textures.
int Scene::buildPropMesh(LoadedMesh mesh, float targetSize, bool allowTriangleLod) {
    if (!mesh.valid()) return -1;

    int vc = mesh.vertexCount();
    glm::vec3 mn(1e30f), mx(-1e30f);
    for (int i = 0; i < vc; ++i) {
        float* p = &mesh.vertices[i * 8];
        mn = glm::min(mn, glm::vec3(p[0], p[1], p[2]));
        mx = glm::max(mx, glm::vec3(p[0], p[1], p[2]));
    }
    glm::vec3 size = mx - mn;
    float horiz = glm::max(size.x, size.z);
    if (horiz < 1e-5f) horiz = 1.0f;
    float s = (targetSize > 0.0f) ? targetSize / horiz : 1.0f;
    glm::vec3 c((mn.x + mx.x) * 0.5f, mn.y, (mn.z + mx.z) * 0.5f);
    for (int i = 0; i < vc; ++i) {
        float* p = &mesh.vertices[i * 8];
        p[0] = (p[0] - c.x) * s;
        p[1] = (p[1] - c.y) * s;
        p[2] = (p[2] - c.z) * s;
    }

    PropMesh pm;
    pm.fitHeight = size.y * s;
    pm.radius = glm::max(glm::max(size.x, size.y), size.z) * s * 0.55f;
    pm.allowTriangleLod = allowTriangleLod;
    glGenVertexArrays(1, &pm.vao);
    glGenBuffers(1, &pm.vbo);
    glBindVertexArray(pm.vao);
    glBindBuffer(GL_ARRAY_BUFFER, pm.vbo);
    glBufferData(GL_ARRAY_BUFFER, mesh.vertices.size() * sizeof(float), mesh.vertices.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(5 * sizeof(float)));
    glEnableVertexAttribArray(2);

    // One source PropSub per glTF material slice, each binding its own
    // textures. Each LOD below rewrites the index offsets/counts while
    // preserving these material bindings.
    std::vector<PropSub> sourceSubs;
    for (const auto& sm : mesh.submeshes) {
        PropSub ps;
        ps.indexOffset = sm.indexOffset;
        ps.indexCount  = sm.indexCount;
        ps.albedo      = loadTextureCached(sm.baseColorPath);
        ps.normalTex   = loadTextureCached(sm.normalPath);
        ps.armTex      = loadTextureCached(sm.metalRoughPath);
        ps.baseFactor  = glm::vec3(sm.baseColorFactor[0],
                                   sm.baseColorFactor[1],
                                   sm.baseColorFactor[2]);
        ps.baseAlpha   = sm.baseColorFactor[3];
        ps.alphaMode      = sm.alphaMode;
        ps.armIsMR        = !sm.hasOcclusion;
        ps.metallicFactor = sm.metallicFactor;
        ps.emissive       = glm::clamp(glm::vec3(sm.emissiveFactor[0],
                                                 sm.emissiveFactor[1],
                                                 sm.emissiveFactor[2]),
                                       glm::vec3(0.0f), glm::vec3(1.5f));
        sourceSubs.push_back(ps);
    }
    // Fallback: if the model declared no submeshes, draw it whole.
    if (sourceSubs.empty()) {
        PropSub ps; ps.indexOffset = 0; ps.indexCount = (unsigned int)mesh.indices.size();
        sourceSubs.push_back(ps);
    }

    auto uploadLOD = [&](int lodIndex, int triangleStride) {
        triangleStride = allowTriangleLod ? triangleStride : 1;
        std::vector<unsigned int> lodIndices;
        std::vector<PropSub> lodSubs;
        lodIndices.reserve(mesh.indices.size() / triangleStride + 3);

        for (const auto& srcSub : sourceSubs) {
            PropSub dstSub = srcSub;
            dstSub.indexOffset = (unsigned int)lodIndices.size();
            dstSub.indexCount = 0;

            unsigned int triCount = srcSub.indexCount / 3;
            for (unsigned int t = 0; t < triCount; ++t) {
                // Stochastic-looking triangle thinning: deterministic per
                // submesh, stable across frames, cheap to build. This is the
                // same reduction idea as VegetationSystem LOD, generalized to
                // object meshes and their material slices.
                if (triangleStride > 1 && (t % (unsigned int)triangleStride) != 0) continue;
                unsigned int baseIndex = srcSub.indexOffset + t * 3;
                if (baseIndex + 2 >= mesh.indices.size()) break;
                lodIndices.push_back(mesh.indices[baseIndex + 0]);
                lodIndices.push_back(mesh.indices[baseIndex + 1]);
                lodIndices.push_back(mesh.indices[baseIndex + 2]);
                dstSub.indexCount += 3;
            }

            if (dstSub.indexCount > 0) lodSubs.push_back(dstSub);
        }

        auto& lod = pm.lods[lodIndex];
        lod.indexCount = (int)lodIndices.size();
        lod.triCount = lod.indexCount / 3;
        lod.subs = std::move(lodSubs);
        glGenBuffers(1, &lod.ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, lod.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     lodIndices.size() * sizeof(unsigned int),
                     lodIndices.data(),
                     GL_STATIC_DRAW);
    };

    uploadLOD(0, 1);  // full mesh
    uploadLOD(1, 3);  // medium distance: ~33% of triangles
    uploadLOD(2, 9);  // far distance: ~11% of triangles
    glBindVertexArray(0);

    propMeshes.push_back(pm);
    return (int)propMeshes.size() - 1;
}

// Load a glTF/OBJ as a single shared prop mesh using its own embedded
// material textures (multi-material aware).
int Scene::loadPropMesh(const char* gltf, float targetSize, bool allowTriangleLod) {
    LoadedMesh mesh = ModelLoader::Load(gltf);
    if (!mesh.valid()) {
        std::cout << "Prop FAILED: " << gltf << "\n";
        return -1;
    }
    int id = buildPropMesh(std::move(mesh), targetSize, allowTriangleLod);
    std::cout << "Prop loaded: " << gltf << " (mesh " << id << ")\n";
    return id;
}

Scene::ObjectShaderLocs Scene::queryObjectShaderLocs() const {
    ObjectShaderLocs l;
    l.tint        = glGetUniformLocation(objectProgram, "tint");
    l.useTex      = glGetUniformLocation(objectProgram, "useTexture");
    l.albedo      = glGetUniformLocation(objectProgram, "albedoTex");
    l.useNormal   = glGetUniformLocation(objectProgram, "useNormalMap");
    l.normal      = glGetUniformLocation(objectProgram, "normalTex");
    l.useArm      = glGetUniformLocation(objectProgram, "useArm");
    l.arm         = glGetUniformLocation(objectProgram, "armTex");
    l.armIsMR     = glGetUniformLocation(objectProgram, "armIsMR");
    l.metalFactor = glGetUniformLocation(objectProgram, "metallicFactor");
    l.emissive    = glGetUniformLocation(objectProgram, "emissiveBoost");
    l.useCut      = glGetUniformLocation(objectProgram, "useAlphaCut");
    l.useBlend    = glGetUniformLocation(objectProgram, "useAlphaBlend");
    l.matAlpha    = glGetUniformLocation(objectProgram, "materialAlpha");
    return l;
}

void Scene::drawPropSubmeshes(const PropMesh& pm, int lodIndex,
                              const glm::vec3& tint,
                              const ObjectShaderLocs& locs,
                              bool instanceAlphaCut) {
    if (lodIndex < 0) lodIndex = 0;
    if (lodIndex > 2) lodIndex = 2;
    const PropMesh::PropLOD& lod = pm.lods[lodIndex];
    if (lod.indexCount <= 0 || lod.ebo == 0) return;

    glBindVertexArray(pm.vao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, lod.ebo);

    GLboolean cullWasOn = glIsEnabled(GL_CULL_FACE);

    auto drawSub = [&](const PropSub& sub, bool blend) {
        glm::vec3 materialFactor = sanitizeMaterialFactor(sub.baseFactor, sub.albedo == 0);
        glm::vec3 subTint = tint * materialFactor;
        glUniform3fv(locs.tint, 1, glm::value_ptr(subTint));
        glUniform1i(locs.useTex, sub.albedo ? 1 : 0);
        if (sub.albedo) {
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, sub.albedo);
            glUniform1i(locs.albedo, 1);
        }
        glUniform1i(locs.useNormal, sub.normalTex ? 1 : 0);
        if (sub.normalTex) {
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, sub.normalTex);
            glUniform1i(locs.normal, 2);
        }
        glUniform1i(locs.useArm, sub.armTex ? 1 : 0);
        if (sub.armTex) {
            glActiveTexture(GL_TEXTURE3);
            glBindTexture(GL_TEXTURE_2D, sub.armTex);
            glUniform1i(locs.arm, 3);
        }
        glUniform1i(locs.armIsMR, sub.armIsMR ? 1 : 0);
        glUniform1f(locs.metalFactor, sub.metallicFactor);
        glUniform3fv(locs.emissive, 1, glm::value_ptr(sub.emissive));
        bool cut = instanceAlphaCut || sub.alphaMode == 1;
        glUniform1i(locs.useCut, cut ? 1 : 0);
        glUniform1i(locs.useBlend, blend ? 1 : 0);
        glUniform1f(locs.matAlpha, sub.baseAlpha);
        // Cutout/blended slices (glTF MASK/BLEND leaves, jellyfish bell) are
        // two-sided even when the host instance is back-culled rock.
        bool twoSided = cut || blend;
        if (twoSided && cullWasOn) glDisable(GL_CULL_FACE);
        glDrawElements(GL_TRIANGLES, sub.indexCount, GL_UNSIGNED_INT,
                       (void*)(sub.indexOffset * sizeof(unsigned int)));
        if (twoSided && cullWasOn) glEnable(GL_CULL_FACE);
    };

    // Opaque + alpha-cut slices first, blended slices last (over their own
    // opaque parts). Drawing MASK/BLEND slices opaque painted their
    // transparent texels as black patches (leafy rocks, jellyfish bell).
    bool anyBlend = false;
    for (const auto& sub : lod.subs) {
        // Glass-like slices (untextured, baseColor alpha near 0 — e.g. the tuna's
        // eye cornea at alpha 0.03) would render as solid blobs. Skip them.
        if (sub.albedo == 0 && sub.baseAlpha < 0.15f) continue;
        if (sub.alphaMode == 2) { anyBlend = true; continue; }
        drawSub(sub, false);
    }
    if (anyBlend) {
        GLboolean blendWasOn = glIsEnabled(GL_BLEND);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        for (const auto& sub : lod.subs) {
            if (sub.alphaMode != 2) continue;
            if (sub.albedo == 0 && sub.baseAlpha < 0.15f) continue;
            drawSub(sub, true);
        }
        glDepthMask(GL_TRUE);
        if (!blendWasOn) glDisable(GL_BLEND);
    }
    // Reset the blend flag so later object-shader users render opaque.
    glUniform1i(locs.useBlend, 0);
}

void Scene::initProps() {
    propMeshes.clear();
    propInstances.clear();

    // Load a rock pack as INDIVIDUAL objects (each its own textures) so we
    // can scatter distinct boulders instead of one merged blob. Returns
    // the list of mesh ids.
    auto loadParts = [&](const char* path, float targetSize) {
        std::vector<int> ids;
        auto parts = ModelLoader::LoadAssimpParts(path);
        for (auto& p : parts) {
            // Skip tiny fragments that add nothing.
            if (p.triangleCount() < 8) continue;
            int id = buildPropMesh(std::move(p), targetSize);
            if (id >= 0) ids.push_back(id);
        }
        return ids;
    };

    // --- Rock library: individual boulders from the packs ---
    std::vector<int> rockIds = loadParts("assets/3d/lowpoly_rocks_-_1/scene.gltf", 6.0f);
    {
        std::vector<int> desert = loadParts("assets/3d/desert__rock__fixed_pack/scene.gltf", 7.0f);
        rockIds.insert(rockIds.end(), desert.begin(), desert.end());
    }
    // Single-object props (kept whole; their own materials).
    int mossPile   = loadPropMesh("assets/3d/moss_covered_rock_pile/scene.gltf", 8.0f);
    int beachCliff = loadPropMesh("assets/3d/beach_cliff/scene.gltf", 30.0f);
    int rockGround = loadPropMesh("assets/3d/rock_on_ground/scene.gltf", 7.0f);
    int stoneIsle  = loadPropMesh("assets/3d/stone_rock_island/scene.gltf", 18.0f);
    {
        std::vector<int> stylized = loadParts("assets/3d/stylized_rock_generator_test_1/scene.gltf", 6.0f);
        rockIds.insert(rockIds.end(), stylized.begin(), stylized.end());
    }
    if (mossPile >= 0)   rockIds.push_back(mossPile);
    if (rockGround >= 0) rockIds.push_back(rockGround);

    // --- Foliage (alpha-cut) ---
    int palm = loadPropMesh("assets/3d/coconut_palm/scene.gltf", 10.0f);
    int fern = loadPropMesh("assets/3d/ferns_grass/scene.gltf", 3.0f,
                            /*allowTriangleLod*/ true);

    // Crabs scuttling on the seabed (untextured OBJ → reddish tint).
    int crab = loadPropMesh("assets/3d/animals/crab.obj", 0.7f);

    std::mt19937 rng(0x5EA);
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);

    auto pick = [&](const std::vector<int>& v) -> int {
        if (v.empty()) return -1;
        return v[(int)(uni(rng) * v.size()) % (int)v.size()];
    };

    auto placeOn = [&](int meshId, float wx, float wz, float scale,
                       float yaw, float sink, glm::vec3 tint, bool alphaCut) {
        if (meshId < 0) return;
        float h = SampleTerrainHeight(wx, wz);
        if (h < collisionNoTerrain + 1.0f) return;     // no ground here
        const PropMesh& pm = propMeshes[meshId];
        glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(wx, h - sink, wz));
        m = glm::rotate(m, yaw, glm::vec3(0, 1, 0));
        m = glm::scale(m, glm::vec3(scale));
        PropInstance pi;
        pi.meshId = meshId; pi.model = m; pi.tint = tint; pi.alphaCut = alphaCut;
        float sizeHint = glm::max(pm.radius * scale, pm.fitHeight * scale);
        float jitter = hash01(wx, wz, scale + (float)meshId);
        float baseLife = alphaCut ? 230.0f : 360.0f;
        if (sizeHint > 18.0f) baseLife = 620.0f;       // cliffs / big islands
        if (sizeHint > 35.0f) baseLife = 820.0f;
        if (h > 4.0f && alphaCut) baseLife += 80.0f;   // island foliage reads well
        pi.lifeDist = baseLife + jitter * baseLife * 0.65f;
        pi.lodBias = alphaCut ? 0.75f : 1.0f;
        if (sizeHint > 18.0f) pi.lodBias = 1.25f;      // keep silhouettes longer
        pi.cullRadius = glm::clamp(sizeHint * 1.8f, 8.0f, 90.0f);
        propInstances.push_back(pi);
    };

    // Scatter rocks across depth zones: shore, shallows, deep, and on the
    // islands. Sample random world positions and key off terrain height.
    int placed = 0, attempts = 0;
    while (placed < 560 && attempts < 18000) {
        ++attempts;
        float wx = (uni(rng) - 0.5f) * terrainSize * 0.9f;
        float wz = (uni(rng) - 0.5f) * terrainSize * 0.9f;
        float h = SampleTerrainHeight(wx, wz);
        if (h < collisionNoTerrain + 1.0f) continue;
        if (rockIds.empty()) break;

        float yaw = uni(rng) * 6.2831853f;
        if (h > 4.0f) {
            // On islands: rocks + lush palms/ferns for a green look.
            placeOn(pick(rockIds), wx, wz, 0.6f + uni(rng) * 1.2f, yaw, 0.4f, glm::vec3(1.0f), false);
            float r = uni(rng);
            if (r < 0.75f && palm >= 0) {
                // Small palm grove (1-3 trees) instead of a lone trunk.
                int np = 1 + (int)(uni(rng) * 2.9f);
                for (int k = 0; k < np; ++k)
                    placeOn(palm, wx + (uni(rng)-0.5f)*9.0f, wz + (uni(rng)-0.5f)*9.0f,
                            0.75f + uni(rng)*0.85f, uni(rng)*6.2831853f, 0.4f,
                            glm::vec3(1.0f), true);
            }
            if (r > 0.3f && fern >= 0) {
                // A small cluster of ferns for ground cover.
                int n = 2 + (int)(uni(rng) * 3.0f);
                for (int k = 0; k < n; ++k)
                    placeOn(fern, wx + (uni(rng)-0.5f)*6.0f, wz + (uni(rng)-0.5f)*6.0f,
                            0.7f + uni(rng)*0.7f, uni(rng)*6.28f, 0.05f, glm::vec3(1.0f), true);
            }
        } else if (h > -8.0f) {
            // Shoreline / shallows: beach cliffs + rocks.
            float r = uni(rng);
            if (r < 0.2f && beachCliff >= 0)
                placeOn(beachCliff, wx, wz, 0.7f + uni(rng)*0.8f, yaw, 1.5f, glm::vec3(1.0f), false);
            else if (r < 0.3f && stoneIsle >= 0)
                placeOn(stoneIsle, wx, wz, 0.7f + uni(rng)*0.8f, yaw, 1.0f, glm::vec3(1.0f), false);
            else
                placeOn(pick(rockIds), wx, wz, 0.7f + uni(rng) * 1.4f, yaw, 0.6f, glm::vec3(0.97f), false);
        } else if (h > -90.0f) {
            // Deeper seabed: boulder fields — often a small cluster around
            // a big anchor rock, like real reef rubble, instead of lone
            // evenly-spread stones.
            placeOn(pick(rockIds), wx, wz, 0.9f + uni(rng) * 2.2f, yaw, 0.8f,
                    glm::vec3(0.8f, 0.85f, 0.9f), false);
            if (uni(rng) < 0.55f) {
                int n = 1 + (int)(uni(rng) * 3.0f);
                for (int k = 0; k < n; ++k)
                    placeOn(pick(rockIds), wx + (uni(rng)-0.5f)*16.0f,
                            wz + (uni(rng)-0.5f)*16.0f,
                            0.5f + uni(rng) * 1.2f, uni(rng)*6.2831853f, 0.7f,
                            glm::vec3(0.8f, 0.85f, 0.9f), false);
            }
        }
        ++placed;
    }

    // --- Crabs: scatter a couple dozen over the sandy seabed (shallows to
    //     deep). Small, reddish, sitting flush on the ground. ---
    if (crab >= 0) {
        int crabs = 0, ctries = 0;
        while (crabs < 26 && ctries < 4000) {
            ++ctries;
            float wx = (uni(rng) - 0.5f) * terrainSize * 0.85f;
            float wz = (uni(rng) - 0.5f) * terrainSize * 0.85f;
            float h  = SampleTerrainHeight(wx, wz);
            if (h < -90.0f || h > -3.0f) continue;     // sandy seabed only
            glm::vec3 tint(0.62f, 0.27f, 0.18f);       // crab shell red-brown
            tint *= 0.85f + uni(rng) * 0.3f;
            // Small cluster so a found spot has a few crabs, not a lone one.
            int nc = 1 + (int)(uni(rng) * 2.9f);
            for (int k = 0; k < nc; ++k)
                placeOn(crab, wx + (uni(rng) - 0.5f) * 3.5f,
                        wz + (uni(rng) - 0.5f) * 3.5f,
                        0.7f + uni(rng) * 0.7f, uni(rng) * 6.2831853f, 0.05f,
                        tint * (0.9f + uni(rng) * 0.2f), false);
            ++crabs;
        }
        std::cout << "Crabs scattered: " << crabs << "\n";
    }

    // --- Sunken wreck site: pick a flat deep-sand spot and ring it with
    // boulders and overgrowth. (The site + POI marker; the buoyancy system
    // that once added a hull here was removed.)
    wreckPlaced = false;
    {
        int tries = 0;
        while (!wreckPlaced && tries++ < 600) {
            float wx = (uni(rng) - 0.5f) * terrainSize * 0.55f;
            float wz = (uni(rng) - 0.5f) * terrainSize * 0.55f;
            float h = SampleTerrainHeight(wx, wz);
            if (h > -20.0f || h < -42.0f) continue;
            // Flat enough? Probe 4 neighbours.
            float h1 = SampleTerrainHeight(wx + 8.0f, wz);
            float h2 = SampleTerrainHeight(wx - 8.0f, wz);
            float h3 = SampleTerrainHeight(wx, wz + 8.0f);
            float h4 = SampleTerrainHeight(wx, wz - 8.0f);
            float spread = glm::max(glm::max(h1, h2), glm::max(h3, h4))
                         - glm::min(glm::min(h1, h2), glm::min(h3, h4));
            if (spread > 3.5f) continue;
            wreckPos = glm::vec3(wx, h, wz);
            wreckPlaced = true;
            // Debris field: boulders strewn around the hull.
            for (int k = 0; k < 7; ++k)
                placeOn(pick(rockIds), wx + (uni(rng)-0.5f)*26.0f,
                        wz + (uni(rng)-0.5f)*26.0f,
                        0.4f + uni(rng) * 1.1f, uni(rng)*6.2831853f, 0.6f,
                        glm::vec3(0.75f, 0.82f, 0.85f), false);
            // A colony of crabs picking over the wreck (guaranteed group).
            if (crab >= 0)
                for (int k = 0; k < 9; ++k)
                    placeOn(crab, wx + (uni(rng)-0.5f)*20.0f,
                            wz + (uni(rng)-0.5f)*20.0f,
                            0.7f + uni(rng)*0.6f, uni(rng)*6.2831853f, 0.05f,
                            glm::vec3(0.62f, 0.27f, 0.18f) * (0.85f + uni(rng)*0.3f), false);
            // Overgrowth: coral colonised the wreck, seagrass ring around it.
            for (int k = 0; k < 26; ++k) {
                float ox = wx + (uni(rng)-0.5f) * 22.0f;
                float oz = wz + (uni(rng)-0.5f) * 22.0f;
                float oh = SampleTerrainHeight(ox, oz);
                if (oh < collisionNoTerrain + 1.0f) continue;
                if (k < 10) coral.addInstance(glm::vec3(ox, oh, oz), 0.6f + uni(rng) * 0.7f);
                else        seagrass.addInstance(glm::vec3(ox, oh, oz), 0.8f + uni(rng) * 0.7f);
            }
            std::cout << "Wreck site: (" << (int)wx << ", " << (int)h
                      << ", " << (int)wz << ")\n";
        }
    }

    // Dedicated palm pass: the main loop only plants palms next to island
    // rocks, which left the coastline bare. Seed standalone groves a few
    // metres above the beach so the islands read as lush from the water.
    if (palm >= 0) {
        int placedPalms = 0, palmTries = 0;
        while (placedPalms < 70 && palmTries < 8000) {
            ++palmTries;
            float wx = (uni(rng) - 0.5f) * terrainSize * 0.9f;
            float wz = (uni(rng) - 0.5f) * terrainSize * 0.9f;
            float h = SampleTerrainHeight(wx, wz);
            if (h < 2.5f || h > 24.0f) continue;
            int np = 1 + (int)(uni(rng) * 2.9f);
            for (int k = 0; k < np; ++k)
                placeOn(palm, wx + (uni(rng)-0.5f)*8.0f, wz + (uni(rng)-0.5f)*8.0f,
                        0.7f + uni(rng)*0.9f, uni(rng)*6.2831853f, 0.4f,
                        glm::vec3(1.0f), true);
            placedPalms += np;
        }
    }
    std::cout << "Props: " << propMeshes.size() << " meshes, "
              << propInstances.size() << " placed\n";
}

void Scene::renderProps(const glm::mat4& view, const glm::mat4& projection,
                        const glm::vec3& camPos, const glm::vec3& sunDir,
                        Camera& camera) {
    if (!objectProgram || propInstances.empty()) return;
    glUseProgram(objectProgram);
    setMat4(objectProgram, "view", view);
    setMat4(objectProgram, "projection", projection);
    setMat4(objectProgram, "lightSpaceMatrix", shadowMap.lightSpace);
    setVec3(objectProgram, "cameraPos", camPos);
    setVec3(objectProgram, "sunDirection", sunDir);
    setInt(objectProgram, "shadowEnabled", (sunDir.y > 0.05f) ? 1 : 0);
    setFloat(objectProgram, "weatherExposure", weather.get().exposure);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, shadowMap.depthTex);
    setInt(objectProgram, "shadowMap", 0);
    flashlight.setUniforms(objectProgram, camPos, camera.Front);

    // Cache uniform locations (avoid re-querying per submesh).
    GLint locModel   = glGetUniformLocation(objectProgram, "model");
    setFloat(objectProgram, "swim", 0.0f);  // props are rigid
    ObjectShaderLocs locs = queryObjectShaderLocs();

    propDrawnLOD0 = propDrawnLOD1 = propDrawnLOD2 = 0;
    propPrunedDistance = 0;
    propCulledFrustum = 0;

    Frustum fr; fr.extract(projection * view);
    for (const auto& pi : propInstances) {
        const PropMesh& pm = propMeshes[pi.meshId];
        // Cheap cull by the instance origin + a generous radius.
        glm::vec3 o = glm::vec3(pi.model[3]);
        float d = glm::length(camPos - o);
        // stochastic pruning for far props: each instance has a stable
        // lifeDist, so dense scenery thins out gradually with distance.
        if (d > pi.lifeDist) {
            ++propPrunedDistance;
            continue;
        }
        if (!fr.testAABB(o - glm::vec3(pi.cullRadius), o + glm::vec3(pi.cullRadius))) {
            ++propCulledFrustum;
            continue;
        }
        int lodIndex = chooseObjectLOD(d, pi.cullRadius, pi.lodBias);
        if (lodIndex == 0) ++propDrawnLOD0;
        else if (lodIndex == 1) ++propDrawnLOD1;
        else ++propDrawnLOD2;

        // Foliage (alpha-cut) is two-sided; solid rock is back-culled.
        if (pi.alphaCut) glDisable(GL_CULL_FACE);
        else { glEnable(GL_CULL_FACE); glCullFace(GL_BACK); }

        glUniformMatrix4fv(locModel, 1, GL_FALSE, glm::value_ptr(pi.model));
        drawPropSubmeshes(pm, lodIndex, pi.tint, locs, pi.alphaCut);
    }
    glBindVertexArray(0);
    glDisable(GL_CULL_FACE);
}

// Depth-only render of the static props into the sun shadow map. Rocks are
// solid casters; foliage (palms/ferns) uses the alpha-tested depth path so
// its leaf cutouts cast leaf-shaped shadows rather than solid quads.
void Scene::renderPropsDepth(const glm::vec3& camPos) {
    if (depthProgram == 0 || propInstances.empty()) return;

    GLint locModel = glGetUniformLocation(depthProgram, "model");
    GLint locAlpha = glGetUniformLocation(depthProgram, "alphaTest");
    GLint locTex   = glGetUniformLocation(depthProgram, "albedoTex");
    glUniform1i(glGetUniformLocation(depthProgram, "instanced"), 0);

    // Only rasterise props whose AABB can actually land in the light's map.
    Frustum lf; lf.extract(shadowMap.lightSpace);

    for (const auto& pi : propInstances) {
        glm::vec3 o = glm::vec3(pi.model[3]);
        if (!lf.testAABB(o - glm::vec3(pi.cullRadius), o + glm::vec3(pi.cullRadius)))
            continue;

        const PropMesh& pm = propMeshes[pi.meshId];
        // Shadows tolerate coarse geometry: pick a cheap LOD by distance.
        float d = glm::length(camPos - o);
        int lodIndex = (d < 120.0f) ? 1 : 2;
        if (pm.lods[lodIndex].indexCount <= 0 || pm.lods[lodIndex].ebo == 0)
            lodIndex = 0;
        const PropMesh::PropLOD& lod = pm.lods[lodIndex];
        if (lod.indexCount <= 0 || lod.ebo == 0) continue;

        glUniformMatrix4fv(locModel, 1, GL_FALSE, glm::value_ptr(pi.model));
        glBindVertexArray(pm.vao);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, lod.ebo);

        for (const auto& sub : lod.subs) {
            if (sub.alphaMode == 2) continue;                       // skip blended/glass
            if (sub.albedo == 0 && sub.baseAlpha < 0.15f) continue; // skip invisible slices
            bool cut = pi.alphaCut || sub.alphaMode == 1;
            if (cut && sub.albedo) {
                glUniform1i(locAlpha, 1);
                glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_2D, sub.albedo);
                glUniform1i(locTex, 1);
            } else {
                glUniform1i(locAlpha, 0);
            }
            glDrawElements(GL_TRIANGLES, sub.indexCount, GL_UNSIGNED_INT,
                           (void*)(sub.indexOffset * sizeof(unsigned int)));
        }
    }
    glUniform1i(locAlpha, 0);   // leave the program in the solid-caster state
}
