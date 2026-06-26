#include "Scene.h"
#include "SceneInternal.h"
#include "WaterGrid.h"
#include "../render/ShaderLoader.h"
#include "../render/GLUniform.h"
#include "../core/ModelLoader.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/constants.hpp>
#include <iostream>
#include <vector>
#include <algorithm>
#include <thread>
#include <random>
#include <cmath>
#include <cstdlib>
#include <chrono>
#define STB_IMAGE_IMPLEMENTATION
#include "../../external/stb_image.h"

Scene::Scene(int width, int height) : screenWidth(width), screenHeight(height), time(0.0f) {
    mainFBO = nullptr;
}

Scene::~Scene() {
    if (mainFBO) delete mainFBO;
    if (ssrFBO) delete ssrFBO;
    if (shoreDataTextureID) glDeleteTextures(1, &shoreDataTextureID);
    vegetation.shutdown();
    coral.shutdown();
    palms.shutdown();
    grassLand.shutdown();
    seagrass.shutdown();
    for (auto& s : fishSchools) s.shutdown();
    serpent.destroy();
    shadowMap.destroy();
    for (auto& pm : propMeshes) {
        for (auto& lod : pm.lods) {
            if (lod.ebo) glDeleteBuffers(1, &lod.ebo);
        }
        if (pm.vbo) glDeleteBuffers(1, &pm.vbo);
        if (pm.vao) glDeleteVertexArrays(1, &pm.vao);
    }
}

void Scene::Init() {
    glEnable(GL_DEPTH_TEST);
    mainFBO = new Framebuffer(scaledW(), scaledH());
    ssrFBO  = new Framebuffer(scaledW(), scaledH());        // opaque-scene snapshot for SSR (scaled)

    // Initialize the unified Gerstner wave field — drives BOTH the rendered
    // open-water surface (uploaded to the water tessellation shader) and the CPU
    // buoyancy/underwater queries, so floating objects sit exactly on the surface.
    waveField.initDefault();

    // Initialize weather presets (Tropical / Stormy / Sunset).
    weather.initPresets();

    // Tessellated water: VS pass-through -> TCS (distance LOD) -> TES (wave displacement
    // per tessellated vertex, real polygons on crests) -> water.frag. GL 4.0+ feature.
    waterProgram = ShaderLoader::CreateProgramTess("assets/shaders/water_tess.vert",
                                                   "assets/shaders/water_tess.tesc",
                                                   "assets/shaders/water_tess.tese",
                                                   "assets/shaders/water.frag");
    screenProgram = ShaderLoader::CreateProgram("assets/shaders/screen.vert", "assets/shaders/screen.frag");
    skyProgram = ShaderLoader::CreateProgram("assets/shaders/sky.vert", "assets/shaders/sky.frag");
    seabedProgram = ShaderLoader::CreateProgram("assets/shaders/seabed.vert", "assets/shaders/seabed.frag");
    terrainProgram = ShaderLoader::CreateProgram("assets/shaders/terrain.vert", "assets/shaders/terrain.frag");
    vegetationProgram = ShaderLoader::CreateProgram("assets/shaders/vegetation.vert", "assets/shaders/vegetation.frag");
    fishProgram = ShaderLoader::CreateProgram("assets/shaders/fish.vert", "assets/shaders/fish.frag");
    causticsProgram = ShaderLoader::CreateProgram("assets/shaders/caustics.vert", "assets/shaders/caustics.frag");
    skyboxProgram = ShaderLoader::CreateProgram("assets/shaders/skybox.vert", "assets/shaders/skybox.frag");
    depthProgram = ShaderLoader::CreateProgram("assets/shaders/depth.vert", "assets/shaders/depth.frag");
    splineProgram = ShaderLoader::CreateProgram("assets/shaders/spline.vert", "assets/shaders/spline.frag");
    objectProgram = ShaderLoader::CreateProgram("assets/shaders/object.vert", "assets/shaders/object.frag");

    // Build the underwater environment cubemap + a unit cube to draw it.
    envCubemap.CreateProcedural(256);
    createSkyboxCube();

    // Shadow map (depth pass from the sun) + soft PCF in receivers.
    shadowMap.init(1536);

    // Parallel-transport-frame sea-serpent body (swept tube).
    serpent.build(/*radialSegments*/ 16, /*pathSamples*/ 240, /*radius*/ 1.7f);
    waterNormalTextureID = loadTexture("assets/textures/water_normal.jpg");
    hdriTextureID = loadHDRTexture("assets/textures/sky_hdri.hdr");

    // === Sand (mid/shallow) — keep top-level path for fallback compatibility ===
    sandDiffuseID   = loadTexture("assets/textures/world/terrain/sand/aerial_beach_01_diff_8k.jpg");
    sandNormalID    = loadTexture("assets/textures/world/terrain/sand/aerial_beach_01_nor_gl_8k.jpg");
    sandRoughnessID = loadTexture("assets/textures/world/terrain/sand/aerial_beach_01_rough_8k.jpg");

    // === Mud (deep / silt) ===
    mudDiffuseID   = loadTexture("assets/textures/world/terrain/mud/brown_mud_diff_2k.jpg");
    mudNormalID    = loadTexture("assets/textures/world/terrain/mud/brown_mud_nor_gl_2k.jpg");
    mudRoughnessID = loadTexture("assets/textures/world/terrain/mud/brown_mud_rough_2k.jpg");

    // === Rock (steep slopes) — using rock_face_03 (ARM packs Ao/Rough/Metal) ===
    rockDiffuseID = loadTexture("assets/textures/world/terrain/rock/rock_face_03_diff_8k.jpg");
    rockNormalID  = loadTexture("assets/textures/world/terrain/rock/rock_face_03_nor_gl_8k.jpg");
    rockARMID     = loadTexture("assets/textures/world/terrain/rock/rock_face_03_arm_8k.jpg");

    // === Lava biome ===
    lavaDiffuseID   = loadTexture("assets/textures/world/terrain/lava/ground_0027_color_4k.jpg");
    lavaNormalID    = loadTexture("assets/textures/world/terrain/lava/ground_0027_normal_opengl_4k.png");
    lavaRoughnessID = loadTexture("assets/textures/world/terrain/lava/ground_0027_roughness_4k.jpg");
    lavaEmissiveID  = loadTexture("assets/textures/world/terrain/lava/ground_0027_emissive_4k.jpg");

    // === Biome masks ===
    castleMaskTexID = loadTextureNoFlip("assets/textures/world/M_Castle_Depth_Mask.png");
    lavaMaskTexID   = loadTextureNoFlip("assets/textures/world/M_Lava_Depth_Mask.png");
    riverMaskTexID  = loadTextureNoFlip("assets/textures/world/M_River_Depth_Mask.png");

    // Terrain settings
    terrainSize = 1600.0f;  // larger world
    heightScale = 0.40f;    // much deeper relief: depth ~290 -> ~116 world units

    createTerrainFromHeightmap();
    createShorelineDataTexture();
    createWaterGrid(480, terrainSize); // dense enough for waves, much cheaper than 700
    createSeabed(terrainSize * 1.2f, -80.0f); // far-below fallback (terrain covers most)
    createScreenQuad();
    createSkyQuad();

    // Kelp: tall procedural swaying fronds in the deeper band below the
    // coral shelf — fills the "empty blue" mid-depths with life. Uses the
    // built-in procedural kelp cards (no model) with full current sway.
    vegetation.init(/*height*/ 7.0f, /*width*/ 1.4f);
    vegetation.lodDist0     = 24.0f;   // aggressive LOD/prune: fog hides the deep band
    vegetation.lodDist1     = 60.0f;
    vegetation.pruneMin     = 80.0f;
    vegetation.pruneMax     = 150.0f;
    vegetation.swayStrength = 1.0f;
    vegetation.groundSink   = 0.25f;
    vegetation.patchScale     = 9.0f;     // loose kelp groves, not a carpet
    vegetation.patchThreshold = 0.55f;
    vegetation.clusterCount   = 4;
    vegetation.clusterRadius  = 3.0f;
    vegetation.scatter(
        /*count*/ 6000,
        heightmapData,
        terrainSize,
        heightScale,
        /*minWorldY*/ -55.0f,
        /*maxWorldY*/ -18.0f);

    // Coral: the hero vegetation. Loaded from OBJ, oriented upright,
    // grounded on the seabed, scattered on the shallow sandy shelf.
    coral.init();
    coral.lodDist0 = 26.0f;
    coral.lodDist1 = 64.0f;
    coral.pruneMin = 85.0f;
    coral.pruneMax = 160.0f;
    coral.swayStrength = 0.15f;   // coral is mostly rigid, only gentle drift
    coral.groundSink   = 0.4f;    // bury the base so it doesn't float
    {
        LoadedMesh coralMesh = ModelLoader::Load(
            "assets/3d/koral/source/coral_reef_plant_middle_05.obj");
        if (coralMesh.valid()) {
            GLuint coralTex = loadTexture(
                "assets/3d/koral/textures/coral_reef_plant_middle_05.png");
            // Coral OBJ is Z-up (Sketchfab export) — rotate to Y-up.
            coral.loadModel(coralMesh, 4.0f, coralTex, /*zUp*/ true);
        }
    }
    coral.scatter(
        /*count*/ 5500,
        heightmapData,
        terrainSize,
        heightScale,
        /*minWorldY*/ -22.0f,
        /*maxWorldY*/  -9.0f);

    // Palms: above-water foliage on the islands. Reuses the instanced
    // LOD vegetation system but in "land mode" (sky-lit, no fog/glow),
    // scattered on the island interior (a few metres above the shore).
    palms.init();
    palms.lodDist0   = 70.0f;     // gentler: palms are visible over water from afar
    palms.lodDist1   = 180.0f;
    palms.pruneMin   = 320.0f;
    palms.pruneMax   = 520.0f;
    palms.swayStrength = 0.0f;    // rigid trunk (no seaweed sway)
    palms.groundSink   = 0.5f;
    palms.landMode     = 1;
    {
        // Try the date palm first, then the coconut palm. Use a clean
        // space-free path (Assimp can choke on spaces in some setups).
        LoadedMesh palmMesh = ModelLoader::Load("assets/3d/date-palm/palm.fbx");
        const char* palmTexPath = "assets/3d/date-palm/textures/palm01.png";
        if (!palmMesh.valid()) {
            palmMesh = ModelLoader::Load("assets/3d/double-coconut-palm/palm.fbx");
            palmTexPath = "assets/3d/double-coconut-palm/textures/palm02.png";
        }
        if (!palmMesh.valid()) {
            // Old-format FBX that Assimp can't read → procedural palm.
            palmMesh = PalmMesh::make();
            palmTexPath = "assets/3d/date-palm/textures/palm01.png";
        }
        if (palmMesh.valid()) {
            // Procedural palm is self-coloured in the shader (landMode):
            // brown trunk, green fronds. No texture needed → pass 0.
            palms.loadModel(palmMesh, 11.0f, 0, /*zUp*/ false);
            std::cout << "Palm model loaded ("
                      << palmMesh.triangleCount() << " tris)\n";
        } else {
            std::cout << "Palm model FAILED to load\n";
        }
        (void)palmTexPath;
    }
    // Scatter on island interiors: above the beach (+3) up to peaks.
    // Procedural-palm instancing disabled — real glTF palms are placed
    // by the prop system (initProps) instead.
    palms.scatter(
        /*count*/ 0,
        heightmapData,
        terrainSize,
        heightScale,
        /*minWorldY*/  3.0f,
        /*maxWorldY*/ 40.0f);

    // Island grass: dense procedural tufts over the island tops, in
    // noise-driven meadow patches. Same A07 pipeline (instancing + LOD +
    // stochastic pruning) in land lighting mode with a blade cutout.
    grassLand.init(/*height*/ 0.85f, /*width*/ 0.38f);
    grassLand.lodDist0     = 20.0f;
    grassLand.lodDist1     = 50.0f;
    grassLand.pruneMin     = 70.0f;
    grassLand.pruneMax     = 140.0f;
    grassLand.swayStrength = 0.35f;   // light breeze
    grassLand.groundSink   = 0.06f;
    grassLand.landMode     = 1;
    grassLand.landCutout   = 1;       // keep blade-shaped cutout
    grassLand.colorRoot    = glm::vec3(0.10f, 0.28f, 0.07f);
    grassLand.colorTip     = glm::vec3(0.47f, 0.66f, 0.24f);
    grassLand.patchScale     = 16.0f;
    grassLand.patchThreshold = 0.33f;
    grassLand.clusterCount   = 6;      // each hit seeds a small tuft cluster
    grassLand.clusterRadius  = 2.2f;
    grassLand.scatter(
        /*count*/ 0,                 // island grass removed (the "plankton" tufts)
        heightmapData,
        terrainSize,
        heightScale,
        /*minWorldY*/  1.6f,
        /*maxWorldY*/ 32.0f);

    // Seagrass: tall swaying blades in patchy beds ("biomes") on the
    // sandy shelf. Underwater lighting path (fog + tip glow), full
    // current sway so the meadows ripple with the storm intensity.
    seagrass.init(/*height*/ 2.3f, /*width*/ 0.5f);
    seagrass.lodDist0     = 22.0f;
    seagrass.lodDist1     = 56.0f;
    seagrass.pruneMin     = 75.0f;
    seagrass.pruneMax     = 150.0f;
    seagrass.swayStrength = 1.25f;
    seagrass.groundSink   = 0.12f;
    seagrass.colorRoot    = glm::vec3(0.05f, 0.20f, 0.10f);
    seagrass.colorTip     = glm::vec3(0.22f, 0.55f, 0.25f);
    seagrass.patchScale     = 11.0f;
    seagrass.patchThreshold = 0.52f;
    seagrass.clusterCount   = 4;
    seagrass.clusterRadius  = 2.0f;
    seagrass.scatter(
        /*count*/ 16000,
        heightmapData,
        terrainSize,
        heightScale,
        /*minWorldY*/ -26.0f,
        /*maxWorldY*/  -4.0f);

    // Caustics render-to-texture setup
    initCaustics();

    // Static props (rocks, beach cliffs, palms, ferns) from glTF, placed
    // across the shore, shallows, deep seabed and islands.
    initProps();

    // Animated sea creatures (jellyfish drift + tuna fish swim).
    initCreatures();

    // Heightmap data was needed by the terrain mesh.
    // Now that it has been consumed, release the big CPU-side arrays.
    heightmapData.heights.clear();      heightmapData.heights.shrink_to_fit();
    heightmapData.castleMask.clear();   heightmapData.castleMask.shrink_to_fit();
    heightmapData.lavaMask.clear();     heightmapData.lavaMask.shrink_to_fit();
    heightmapData.riverMask.clear();    heightmapData.riverMask.shrink_to_fit();
}

void Scene::createWaterGrid(int resolution, float size) {
    WaterGridMesh m = buildRadialWaterGrid(193, 1.5f, 1.045f);  // base mesh; crests get GPU tessellation
    std::vector<float>& vertices = m.vertices;
    std::vector<unsigned int>& indices = m.indices;
    waterHalfExtent = m.halfExtent;   // Scene.h member added in Step 4
    waterIndicesCount = (int)indices.size();
    glGenVertexArrays(1, &waterVAO);
    glGenBuffers(1, &waterVBO);
    glGenBuffers(1, &waterEBO);
    glBindVertexArray(waterVAO);
    glBindBuffer(GL_ARRAY_BUFFER, waterVBO);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), vertices.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, waterEBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
}


void Scene::createScreenQuad() {
    float quadVertices[] = {
        -1.0f,  1.0f,  0.0f, 1.0f,
        -1.0f, -1.0f,  0.0f, 0.0f,
         1.0f, -1.0f,  1.0f, 0.0f,
        -1.0f,  1.0f,  0.0f, 1.0f,
         1.0f, -1.0f,  1.0f, 0.0f,
         1.0f,  1.0f,  1.0f, 1.0f
    };
    glGenVertexArrays(1, &quadVAO);
    glGenBuffers(1, &quadVBO);
    glBindVertexArray(quadVAO);
    glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), &quadVertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
}

void Scene::createSkyboxCube() {
    // 36-vertex unit cube (positions only) for the skybox.
    float v[] = {
        -1,1,-1, -1,-1,-1, 1,-1,-1, 1,-1,-1, 1,1,-1, -1,1,-1,
        -1,-1,1, -1,-1,-1, -1,1,-1, -1,1,-1, -1,1,1, -1,-1,1,
        1,-1,-1, 1,-1,1, 1,1,1, 1,1,1, 1,1,-1, 1,-1,-1,
        -1,-1,1, -1,1,1, 1,1,1, 1,1,1, 1,-1,1, -1,-1,1,
        -1,1,-1, 1,1,-1, 1,1,1, 1,1,1, -1,1,1, -1,1,-1,
        -1,-1,-1, -1,-1,1, 1,-1,-1, 1,-1,-1, -1,-1,1, 1,-1,1
    };
    glGenVertexArrays(1, &cubeVAO);
    glGenBuffers(1, &cubeVBO);
    glBindVertexArray(cubeVAO);
    glBindBuffer(GL_ARRAY_BUFFER, cubeVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glBindVertexArray(0);
}

// ----------------------------------------------------------------------
// renderShadowDepth — depth-only pass from the sun's point of view.
// Fills shadowMap.depthTex; terrain/serpent shaders then sample it
// with a wide PCF kernel for soft underwater shadows. Saves/restores all
// touched GL state so the main pass is unaffected.
// ----------------------------------------------------------------------
void Scene::renderShadowDepth(const glm::vec3& sunDir, const glm::vec3& camPos) {
    if (depthProgram == 0) return;
    // Only the upper hemisphere sun casts meaningful shadows.
    if (sunDir.y <= 0.05f) return;

    // ---- skip-work cache --------------------------------------------
    // Terrain is static, so the shadow map only needs re-rendering when
    // the SUN direction changes or the CAMERA moves far enough that the
    // camera-centred light frustum should follow. Otherwise we keep last
    // frame's depth texture — a big FPS win (the terrain is ~5M tris).
    static glm::vec3 lastSun(1e9f);
    static glm::vec3 lastCenter(1e9f);
    static bool      haveShadow = false;
    glm::vec3 center(camPos.x, glm::min(camPos.y, -4.0f), camPos.z);
    bool sunMoved = glm::distance(sunDir, lastSun) > 0.01f;
    bool camMoved = glm::distance(center, lastCenter) > 25.0f;  // re-snap frustum
    if (haveShadow && !sunMoved && !camMoved) return;
    lastSun = sunDir; lastCenter = center; haveShadow = true;

    // Cover a region around the player so shadows are sharp where it
    // matters. Centre slightly below the surface where geometry sits.
    shadowMap.update(sunDir, center, /*extent*/ 220.0f, /*depthRange*/ 700.0f);

    GLint prevViewport[4]; glGetIntegerv(GL_VIEWPORT, prevViewport);
    GLint prevFBO;         glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevFBO);
    GLboolean prevCull = glIsEnabled(GL_CULL_FACE);

    shadowMap.beginDepthPass();
    glEnable(GL_DEPTH_TEST);
    // Front-face culling during the depth pass reduces "peter-panning"
    // and shadow acne on the lit surfaces.
    glDisable(GL_CULL_FACE);

    glUseProgram(depthProgram);
    glUniformMatrix4fv(glGetUniformLocation(depthProgram, "lightSpaceMatrix"),
                       1, GL_FALSE, glm::value_ptr(shadowMap.lightSpace));
    setInt(depthProgram, "alphaTest", 0);   // terrain/coral are solid casters

    glm::mat4 model = glm::mat4(1.0f);

    // --- terrain (non-instanced) ---
    if (terrainIndicesCount > 0) {
        setInt(depthProgram, "instanced", 0);
        glUniformMatrix4fv(glGetUniformLocation(depthProgram, "model"),
                           1, GL_FALSE, glm::value_ptr(model));
        glBindVertexArray(terrainVAO);
        if (!terrainChunks.empty()) {
            // Cull chunks against the LIGHT frustum so the depth pass
            // only rasterises terrain that can actually cast into the map.
            Frustum lf; lf.extract(shadowMap.lightSpace);
            for (const auto& ch : terrainChunks) {
                if (!lf.testAABB(ch.aabbMin, ch.aabbMax)) continue;
                // Shadows tolerate coarser terrain — use mid/low LOD only.
                glm::vec3 cc = (ch.aabbMin + ch.aabbMax) * 0.5f;
                float dist = glm::length(camPos - cc);
                int lod = (dist < 120.0f) ? 1 : 2;
                if (ch.lodCount[lod] == 0) lod = 0;
                glDrawElements(GL_TRIANGLES, ch.lodCount[lod], GL_UNSIGNED_INT,
                               (const void*)ch.lodOffset[lod]);
            }
        } else {
            glDrawElements(GL_TRIANGLES, terrainIndicesCount, GL_UNSIGNED_INT, 0);
        }
    }
    // --- serpent: intentionally NOT a shadow caster. Its body is
    //     animated with a travelling wave in spline.vert that the
    //     generic depth.vert doesn't replicate, so casting it here would
    //     produce a mismatched shadow. It still RECEIVES shadows. ---
    // --- vegetation (instanced) ---
    coral.renderDepth(depthProgram, camPos, time);

    // --- static props (rocks + foliage): rocks cast solid shadows,
    //     palms/ferns cast alpha-tested leaf-shaped shadows. ---
    renderPropsDepth(camPos);

    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    if (prevCull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
}

void Scene::createSkyQuad() {
    float skyVertices[] = {
        -1.0f,  1.0f,
        -1.0f, -1.0f,
         1.0f, -1.0f,
        -1.0f,  1.0f,
         1.0f, -1.0f,
         1.0f,  1.0f
    };
    glGenVertexArrays(1, &skyVAO);
    glGenBuffers(1, &skyVBO);
    glBindVertexArray(skyVAO);
    glBindBuffer(GL_ARRAY_BUFFER, skyVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(skyVertices), &skyVertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
}

void Scene::Update(GLFWwindow* window) {
    float now = (float)glfwGetTime();
    float dt = (time > 0.0f) ? glm::clamp(now - time, 0.0f, 0.1f) : 0.0f;
    time = now;
    frameDt = dt;
    // Smoothly transition weather toward the selected preset. The final
    // wave amplitude (preset scale × UI Wave Height) is set in Render.
    weather.update(dt);
    stormIntensity = (stormOverride >= 0.0f)
                   ? stormOverride
                   : weather.get().stormIntensity;

    // Random lightning: rarer when calm, frequent in a full storm. The flash spikes
    // to 1 then decays in ~0.25 s. rand() is fine here (no determinism needed).
    flashTimer -= dt;
    float strikeChance = stormIntensity * stormIntensity;     // quadratic -> rare unless stormy
    if (lightningEnabled && flashTimer <= 0.0f &&
        (float(rand()) / float(RAND_MAX)) < strikeChance * dt * 1.5f) {
        lightningFlash = 1.0f;
        flashTimer = 2.0f + 6.0f * (float(rand()) / float(RAND_MAX));   // 2..8 s cooldown
    }
    // Off => no new strikes and the current flash is killed immediately (no flicker).
    lightningFlash = lightningEnabled ? glm::max(0.0f, lightningFlash - dt * 4.0f) : 0.0f;
}

// ----------------------------------------------------------------------
// Animated sea creatures: jellyfish drift up/down, tuna fish swim along
// looping circles with a vertex-shader tail sway (the object shader bends
// the mesh along an asset-specific local body axis based on `swim`). Each uses its own glTF
// material textures via the multi-material prop pipeline.
// ----------------------------------------------------------------------
void Scene::initCreatures() {
    creatures.clear();

    // Jellyfish bell ~2.2 m across. The asset is much taller than wide
    // (long tentacles), so fitting the old 4 m HORIZONTAL extent produced
    // ~10 m monsters whose bells broke the water surface.
    int jelly = loadPropMesh("assets/3d/animals/jellyfish/scene.gltf", 2.2f);
    // Tuna: the baked body runs along +Y (3.0 units tall vs 1.5/1.6 wide —
    // measured from the glTF node-baked vertices), so the horizontal fit
    // is its fin spread: 1.2 m here = ~2.3 m body length after c.orient
    // lays it down nose-forward.
    int tuna  = loadPropMesh("assets/3d/animals/tuna_fish/scene.gltf", 1.2f);

    // Real great-white model (multi-material OBJ). LoadOBJ ignores .mtl, so
    // go through Assimp to resolve the base-colour/normal textures into
    // submeshes; buildPropMesh then fits the body to ~6 m and uploads them.
    int sharkMeshId = -1;
    {
        LoadedMesh sm = ModelLoader::LoadAssimp("assets/3d/animals/shark/Shark_1.obj");
        if (sm.valid()) {
            glm::vec3 mn(1e30f), mx(-1e30f);
            for (int i = 0; i < sm.vertexCount(); ++i) {
                const float* p = &sm.vertices[i * 8];
                mn = glm::min(mn, glm::vec3(p[0], p[1], p[2]));
                mx = glm::max(mx, glm::vec3(p[0], p[1], p[2]));
            }
            glm::vec3 e = mx - mn;
            std::cout << "[shark] extents x=" << e.x << " y=" << e.y << " z=" << e.z
                      << " subs=" << sm.submeshes.size() << "\n";
            sharkMeshId = buildPropMesh(std::move(sm), 6.0f, /*lod*/ true);
        } else {
            std::cout << "[shark] LoadAssimp FAILED — using tuna stub\n";
        }
    }

    std::mt19937 rng(0xF15Eu);
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);

    // Jellyfish: loose blooms drifting in deep water. The mesh is
    // base-anchored (tentacle tips at local y=0) so the bell sits
    // fitHeight*scale ABOVE the translate point — the depth budget must
    // cover the full body height plus the bob amplitude, or the bell
    // pokes out of the sea and the jellyfish appear to fly.
    if (jelly >= 0) {
        const float bellH = propMeshes[jelly].fitHeight;
        int blooms = 0, attempts = 0;
        while (blooms < 8 && attempts < 600) {
            ++attempts;
            float wx = (uni(rng) - 0.5f) * terrainSize * 0.6f;
            float wz = (uni(rng) - 0.5f) * terrainSize * 0.6f;
            if (SampleTerrainHeight(wx, wz) > -18.0f) continue;  // bloom needs depth
            int n = 2 + (int)(uni(rng) * 4.0f);
            for (int i = 0; i < n; ++i) {
                float ox = wx + (uni(rng) - 0.5f) * 14.0f;
                float oz = wz + (uni(rng) - 0.5f) * 14.0f;
                float bed = SampleTerrainHeight(ox, oz);
                if (bed > -14.0f) continue;
                Creature c;
                c.meshId = jelly;
                c.scale  = 0.55f + uni(rng) * 0.6f;
                c.bob    = 1.2f + uni(rng) * 1.8f;
                float height = bellH * c.scale;
                float maxC = -4.0f - height - c.bob;   // bell stays >= 4 m under
                float minC = bed + 1.5f + c.bob;       // tentacles clear the bed
                if (maxC < minC) { c.bob = 0.8f; maxC = -4.0f - height - c.bob; minC = bed + 1.5f + c.bob; }
                if (maxC < minC) continue;
                c.center = glm::vec3(ox, minC + (maxC - minC) * (0.3f + uni(rng) * 0.6f), oz);
                c.radius = 1.2f + uni(rng) * 2.2f;     // barely drifts sideways
                c.speed  = 0.05f + uni(rng) * 0.08f;
                c.phase  = uni(rng) * 6.2831853f;
                c.isFish = false;
                creatures.push_back(c);
            }
            ++blooms;
        }
    }
    // Tuna: faster swimmers circling at depth in schools.
    if (tuna >= 0) {
        // Asset-space correction, measured from the node-baked glTF
        // vertices: the body runs along +Y (head +Y — the dense end; the
        // flat tail fin is -Y). Rx(+90°) lays the nose onto +Z and brings
        // the back upright (the bigger mid-body bulge at +Z turned out to
        // be the BELLY keel, not the dorsal fin — the extra Rz(180°) roll
        // had every fish swimming belly-up).
        glm::mat4 tunaOrient =
            glm::rotate(glm::mat4(1.0f), glm::half_pi<float>(), glm::vec3(1, 0, 0));
        for (int s = 0; s < 8; ++s) {
            float wx = (uni(rng) - 0.5f) * terrainSize * 0.6f;
            float wz = (uni(rng) - 0.5f) * terrainSize * 0.6f;
            float bed = SampleTerrainHeight(wx, wz);
            if (bed > -14.0f) continue;
            float baseY = glm::clamp(bed + 12.0f, -110.0f, -10.0f);
            float baseR = 14.0f + uni(rng) * 16.0f;
            float baseSpeed = 0.18f + uni(rng) * 0.12f;
            int n = 4 + (int)(uni(rng) * 5.0f);
            for (int i = 0; i < n; ++i) {
                Creature c;
                c.meshId = tuna;
                c.center = glm::vec3(wx, baseY + (uni(rng)-0.5f)*4.0f, wz);
                c.radius = baseR + (uni(rng)-0.5f)*4.0f;
                c.speed  = baseSpeed;
                c.phase  = uni(rng) * 6.2831853f;
                c.bob    = 1.5f;
                c.scale  = 0.8f + uni(rng) * 0.5f;
                c.isFish = true;
                c.orient = tunaOrient;
                creatures.push_back(c);
            }
        }

        // Apex predator: one shark — the tuna mesh scaled up ~6 m and
        // tinted slate-grey reads as a shark silhouette at depth. Its
        // movement is fully AI-driven (patrol/hunt) in updateCreatures.
        {
            Creature shark;
            shark.meshId = (sharkMeshId >= 0) ? sharkMeshId : tuna;
            int tries = 0;
            glm::vec3 home(60.0f, -30.0f, 40.0f);
            while (tries++ < 300) {
                float wx = (uni(rng) - 0.5f) * terrainSize * 0.5f;
                float wz = (uni(rng) - 0.5f) * terrainSize * 0.5f;
                float bed = SampleTerrainHeight(wx, wz);
                if (bed > -24.0f) continue;
                home = glm::vec3(wx, glm::clamp(bed + 10.0f, -70.0f, -12.0f), wz);
                break;
            }
            shark.center  = home;
            shark.pos     = home;
            shark.radius  = 26.0f;            // patrol circle
            shark.phase   = uni(rng) * 6.2831853f;
            shark.isShark = true;
            if (sharkMeshId >= 0) {
                // Real textured model: rigid glide (no fish tail-sway, which
                // is tuned for the +Y tuna body), full-colour texture (white
                // tint), already ~6 m from buildPropMesh.
                shark.scale  = 1.0f;
                shark.isFish = false;
                shark.tint   = glm::vec3(1.0f);
                shark.orient = glm::mat4(1.0f);   // tuned after the extents print
            } else {
                shark.scale  = 2.6f;             // ~6 m body
                shark.isFish = true;             // tail sway in the shader
                shark.tint   = glm::vec3(0.32f, 0.36f, 0.40f);
                shark.orient = tunaOrient;
            }
            creatures.push_back(shark);
            std::cout << "Shark home: (" << (int)home.x << ", " << (int)home.y
                      << ", " << (int)home.z << ")\n";
        }
    }

    // Boids schools of small fish (one instanced draw call each).
    for (auto& s : fishSchools) s.shutdown();
    fishSchools.clear();
    fishSchools.reserve(3);
    {
        int placedSchools = 0, tries = 0;
        while (placedSchools < 3 && tries++ < 400) {
            float wx = (uni(rng) - 0.5f) * terrainSize * 0.5f;
            float wz = (uni(rng) - 0.5f) * terrainSize * 0.5f;
            float bed = SampleTerrainHeight(wx, wz);
            if (bed > -16.0f) continue;
            float y = glm::clamp(bed + 8.0f, -60.0f, -8.0f);
            FishSchool school;
            school.init(190 + (int)(uni(rng) * 70.0f), glm::vec3(wx, y, wz),
                        0xF00Du + (unsigned)placedSchools * 77u);
            fishSchools.push_back(std::move(school));
            ++placedSchools;
        }
        // Resident school circling the wreck — life around the point of
        // interest (small wander radius keeps it near the hull).
        if (wreckPlaced) {
            FishSchool school;
            school.init(150, wreckPos + glm::vec3(0.0f, 7.0f, 0.0f), 0x5EAF00Du);
            school.wanderRadius = 9.0f;
            fishSchools.push_back(std::move(school));
        }
        int totalBoids = 0;
        for (auto& s : fishSchools) totalBoids += s.count();
        std::cout << "Fish schools: " << fishSchools.size()
                  << " (" << totalBoids << " fish)\n";
    }
    std::cout << "Creatures: " << creatures.size() << "\n";
}

void Scene::FireSonar(const glm::vec3& origin) {
    sonarActive = true;
    sonarStart  = time;
    sonarOrigin = origin;
}

void Scene::updateCreatures(const glm::vec3& playerPos) {
    sharkThreat = glm::max(0.0f, sharkThreat - frameDt * 0.5f);   // calm down
    for (auto& c : creatures) {
        // The shark runs its own steering AI below and exits the iteration
        // early. Only fish and jellyfish use the shared circular-swim angle,
        // so we compute it AFTER the shark branch to keep that path light.
        if (c.isShark) {
            // --- Shark AI: patrol a circle around home; when a diver is
            // underwater nearby, swing over and circle THEM instead. No
            // damage — pure menace (panic vignette ramps via sharkThreat).
            float dt = glm::min(frameDt, 0.05f);
            float distP = glm::length(playerPos - c.pos);
            bool diving = playerPos.y < -2.0f;
            if (c.state == 0 && diving && distP < 42.0f) c.state = 1;
            if (c.state == 1 && (!diving || distP > 60.0f)) c.state = 0;

            glm::vec3 target;
            float speed;
            if (c.state == 1) {
                // Orbit the diver; the radius breathes 6..13 m so passes
                // sometimes come uncomfortably close.
                float orbA = time * 0.45f + c.phase;
                float orbR = 9.5f + 3.5f * std::sin(time * 0.23f + c.phase);
                target = playerPos + glm::vec3(std::cos(orbA) * orbR,
                                               1.0f + std::sin(time * 0.4f) * 1.5f,
                                               std::sin(orbA) * orbR);
                speed = 4.4f;
                sharkThreat = glm::max(sharkThreat,
                                       glm::clamp(1.35f - distP / 30.0f, 0.0f, 1.0f));
            } else {
                float patA = time * 0.10f + c.phase;
                target = c.center + glm::vec3(std::cos(patA) * c.radius,
                                              2.0f * std::sin(time * 0.17f),
                                              std::sin(patA) * c.radius);
                speed = 2.3f;
            }

            glm::vec3 to = target - c.pos;
            float toLen = glm::length(to);
            if (toLen > 0.5f) {
                glm::vec3 desired = to / toLen;
                // Smoothly steer the heading (limited turn rate -> wide,
                // shark-like arcs instead of twitchy turns).
                float desYaw = std::atan2(desired.x, desired.z);
                float dyaw = desYaw - c.yaw;
                while (dyaw >  3.14159265f) dyaw -= 6.2831853f;
                while (dyaw < -3.14159265f) dyaw += 6.2831853f;
                float maxTurn = (c.state == 1 ? 1.3f : 0.7f) * dt;
                c.yaw += glm::clamp(dyaw, -maxTurn, maxTurn);
                float desPitch = glm::clamp(std::asin(glm::clamp(desired.y, -1.0f, 1.0f)),
                                            -0.35f, 0.35f);
                c.pitchCur = glm::mix(c.pitchCur, desPitch, glm::min(1.0f, 2.0f * dt));
            }
            glm::vec3 fwd(std::sin(c.yaw) * std::cos(c.pitchCur),
                          std::sin(c.pitchCur),
                          std::cos(c.yaw) * std::cos(c.pitchCur));
            c.pos += fwd * speed * dt;
            // Stay off the seabed and below the surface.
            float bed = SampleTerrainHeight(c.pos.x, c.pos.z);
            if (bed > -90000.0f) c.pos.y = glm::max(c.pos.y, bed + 3.0f);
            c.pos.y = glm::min(c.pos.y, -3.5f);

            glm::mat4 m = glm::translate(glm::mat4(1.0f), c.pos);
            m = glm::rotate(m, c.yaw, glm::vec3(0, 1, 0));
            m = glm::rotate(m, -c.pitchCur, glm::vec3(1, 0, 0));
            // Lean into the turn slightly (constant small bank looks right
            // for the wide arcs both states produce).
            m = glm::rotate(m, 0.12f, glm::vec3(0, 0, 1));
            m = m * c.orient;
            m = glm::scale(m, glm::vec3(c.scale));
            c.model = m;
            continue;   // shark fully handled; skip the kinematic-swim code
        }

        // --- Fish + jellyfish: cheap kinematic swim (no per-neighbour AI) ---
        // Both drift along a circle parameterised by this angle; the shark
        // above never reaches here, so it is only paid for swimmers.
        float ang = c.phase + time * c.speed;   // position along the ring
        if (c.isFish) {
            // Swim a circle whose centre slowly wanders (a fixed ring reads
            // as a carousel); face the tangent, pitch with the vertical
            // motion and bank into the turn like a real fish.
            float cx = c.center.x + std::sin(time * 0.06f + c.phase * 2.3f) * 4.0f;
            float cz = c.center.z + std::cos(time * 0.05f + c.phase * 1.7f) * 4.0f;
            float x = cx + std::cos(ang) * c.radius;
            float z = cz + std::sin(ang) * c.radius;
            float bobPhase = time * 0.4f + c.phase;
            float y = c.center.y + std::sin(bobPhase) * c.bob;
            // Velocity on this circle is (-sin ang, 0, cos ang); R_y(heading)
            // maps +Z (the nose) onto it when heading = atan2(v.x, v.z) = -ang.
            float heading = -ang;
            // Nose follows the vertical velocity (dy/dt vs horizontal speed).
            float dy = c.bob * 0.4f * std::cos(bobPhase);
            float horizSpeed = glm::max(c.radius * c.speed, 0.5f);
            float pitch = glm::clamp(std::atan2(dy, horizSpeed), -0.35f, 0.35f);
            // Bank into the (left) turn, harder when swimming faster.
            float bank = glm::clamp(horizSpeed * 0.05f, 0.06f, 0.28f);
            glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(x, y, z));
            m = glm::rotate(m, heading, glm::vec3(0, 1, 0));
            m = glm::rotate(m, -pitch, glm::vec3(1, 0, 0));
            m = glm::rotate(m, bank, glm::vec3(0, 0, 1));
            m = m * c.orient;
            m = glm::scale(m, glm::vec3(c.scale));
            c.model = m;
        } else {
            // Jellyfish: near-stationary drift, slow asymmetric rise/sink
            // and a breathing bell (contract fast, relax slow) instead of
            // the old flying-saucer circle.
            float x = c.center.x + std::cos(ang) * c.radius;
            float z = c.center.z + std::sin(ang) * c.radius;
            float ts = time * 0.35f + c.phase;
            float y = c.center.y + c.bob * (0.8f * std::sin(ts) + 0.2f * std::sin(ts * 2.0f + 1.3f));
            float pulse = std::sin(time * 1.6f + c.phase * 3.0f);
            float sxz = c.scale * (1.0f + 0.06f * pulse);
            float sy  = c.scale * (1.0f - 0.08f * pulse);
            glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(x, y, z));
            m = glm::rotate(m, ang * 0.5f, glm::vec3(0, 1, 0));            // lazy spin
            m = glm::rotate(m, 0.10f * std::sin(time * 0.3f + c.phase * 2.0f),
                            glm::vec3(1, 0, 0));                            // gentle sway
            m = m * c.orient;
            m = glm::scale(m, glm::vec3(sxz, sy, sxz));
            c.model = m;
        }
    }
}

void Scene::renderCreatures(const glm::mat4& view, const glm::mat4& projection,
                            const glm::vec3& camPos, const glm::vec3& sunDir,
                            Camera& camera) {
    if (!objectProgram || creatures.empty()) return;
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

    GLint locModel = glGetUniformLocation(objectProgram, "model");
    ObjectShaderLocs locs = queryObjectShaderLocs();
    GLint locSwim  = glGetUniformLocation(objectProgram, "swim");
    GLint locTime  = glGetUniformLocation(objectProgram, "objTime");
    GLint locSwimAxis = glGetUniformLocation(objectProgram, "swimAxis");
    GLint locSwimSide = glGetUniformLocation(objectProgram, "swimSide");

    glDisable(GL_CULL_FACE);   // animal meshes are often two-sided
    glUniform1f(locTime, time);
    creatureDrawnLOD0 = creatureDrawnLOD1 = creatureDrawnLOD2 = 0;
    Frustum fr; fr.extract(projection * view);
    for (const auto& c : creatures) {
        const PropMesh& pm = propMeshes[c.meshId];
        glm::vec3 o = glm::vec3(c.model[3]);
        float radius = glm::clamp(pm.radius * c.scale * 1.8f, 8.0f, 50.0f);
        float d = glm::length(camPos - o);
        if (d > 760.0f) continue;
        if (!fr.testAABB(o - glm::vec3(radius), o + glm::vec3(radius))) continue;
        int lodIndex = chooseObjectLOD(d, radius, c.isFish ? 1.1f : 0.9f);
        if (lodIndex == 0) ++creatureDrawnLOD0;
        else if (lodIndex == 1) ++creatureDrawnLOD1;
        else ++creatureDrawnLOD2;

        glUniformMatrix4fv(locModel, 1, GL_FALSE, glm::value_ptr(c.model));
        // Fish bend their bodies (tail sway); jellyfish stay rigid.
        glUniform1f(locSwim, c.isFish ? 1.0f : 0.0f);
        // The sway runs in the MESH's source space (object.vert deforms
        // aPos before c.orient): the tuna body is along +Y with the tail
        // at -Y, so axis -Y makes dot(pos, axis) grow toward the tail;
        // the side flap direction is the source X.
        glm::vec3 swimAxis = c.isFish ? glm::vec3(0, -1, 0) : glm::vec3(0, 0, 1);
        glm::vec3 swimSide = glm::vec3(1, 0, 0);
        glUniform3fv(locSwimAxis, 1, glm::value_ptr(swimAxis));
        glUniform3fv(locSwimSide, 1, glm::value_ptr(swimSide));
        drawPropSubmeshes(pm, lodIndex, c.tint, locs, false);
    }
    glUniform1f(locSwim, 0.0f);   // reset for other object-shader users
    glBindVertexArray(0);
}


void Scene::OnResize(int width, int height) {
    screenWidth = width; screenHeight = height;
    // Scene targets follow the render scale; the post pass upscales them.
    if (mainFBO) mainFBO->Resize(scaledW(), scaledH());
    if (ssrFBO) ssrFBO->Resize(scaledW(), scaledH());
}

void Scene::SetRenderScale(float s) {
    renderScale = (s < 0.4f) ? 0.4f : (s > 1.0f ? 1.0f : s);
    if (mainFBO) mainFBO->Resize(scaledW(), scaledH());
    if (ssrFBO) ssrFBO->Resize(scaledW(), scaledH());
}

namespace {
// Max texture side we upload. 8K/4K source art is overkill here and murders
// the 32-bit (Win32 ~2GB) address space — an 8K RGBA is ~268 MB in VRAM.
// Capping to 2K is imperceptible with mipmaps and saves enormous memory.
// Override with env UW_TEXCAP (e.g. 4096 or 1024).
int textureCap() {
    static int cap = -1;
    if (cap < 0) {
        cap = 2048;
        char b[16] = {0}; size_t n = 0; getenv_s(&n, b, sizeof(b), "UW_TEXCAP");
        if (n > 0) { int v = atoi(b); if (v >= 64) cap = v; }
    }
    return cap;
}
// Box-downscale an 8-bit image (1..4 ch) by repeated 2x halving until both
// sides are <= cap. malloc/free stay compatible with stbi_image_free (free()).
void capTextureUC(unsigned char*& data, int& w, int& h, int comp, int cap) {
    while (data && (w > cap || h > cap) && w > 1 && h > 1) {
        int nw = (w + 1) / 2, nh = (h + 1) / 2;
        unsigned char* dst = (unsigned char*)malloc((size_t)nw * nh * comp);
        if (!dst) break;
        for (int y = 0; y < nh; ++y) {
            int y0 = y * 2, y1 = (y * 2 + 1 < h) ? y * 2 + 1 : y0;
            for (int x = 0; x < nw; ++x) {
                int x0 = x * 2, x1 = (x * 2 + 1 < w) ? x * 2 + 1 : x0;
                for (int c = 0; c < comp; ++c) {
                    int s = (int)data[(y0 * w + x0) * comp + c] + data[(y0 * w + x1) * comp + c]
                          + data[(y1 * w + x0) * comp + c] + data[(y1 * w + x1) * comp + c];
                    dst[(y * nw + x) * comp + c] = (unsigned char)(s >> 2);
                }
            }
        }
        free(data);
        data = dst; w = nw; h = nh;
    }
}
// Float (HDR) variant — an 8K HDRI is ~800 MB of float, fatal on Win32.
void capTextureF(float*& data, int& w, int& h, int comp, int cap) {
    while (data && (w > cap || h > cap) && w > 1 && h > 1) {
        int nw = (w + 1) / 2, nh = (h + 1) / 2;
        float* dst = (float*)malloc((size_t)nw * nh * comp * sizeof(float));
        if (!dst) break;
        for (int y = 0; y < nh; ++y) {
            int y0 = y * 2, y1 = (y * 2 + 1 < h) ? y * 2 + 1 : y0;
            for (int x = 0; x < nw; ++x) {
                int x0 = x * 2, x1 = (x * 2 + 1 < w) ? x * 2 + 1 : x0;
                for (int c = 0; c < comp; ++c) {
                    float s = data[(y0 * w + x0) * comp + c] + data[(y0 * w + x1) * comp + c]
                            + data[(y1 * w + x0) * comp + c] + data[(y1 * w + x1) * comp + c];
                    dst[(y * nw + x) * comp + c] = s * 0.25f;
                }
            }
        }
        free(data);
        data = dst; w = nw; h = nh;
    }
}
} // namespace

GLuint Scene::loadTexture(const char* path) {
    GLuint textureID;
    glGenTextures(1, &textureID);

    int width, height, nrComponents;
    stbi_set_flip_vertically_on_load(true);
    unsigned char* data = stbi_load(path, &width, &height, &nrComponents, 0);
    if (data && nrComponents == 2) {
        // Grey+alpha images have no matching GL format below — uploading the
        // 2-channel buffer as GL_RGB produced skewed colour garbage. Re-expand.
        stbi_image_free(data);
        data = stbi_load(path, &width, &height, &nrComponents, 4);
        if (data) nrComponents = 4;
    }

    if (data) {
        capTextureUC(data, width, height, nrComponents, textureCap());  // 8K/4K -> cap
        GLenum format = GL_RGB;
        if (nrComponents == 1) format = GL_RED;
        else if (nrComponents == 3) format = GL_RGB;
        else if (nrComponents == 4) format = GL_RGBA;

        glBindTexture(GL_TEXTURE_2D, textureID);
        // stbi rows are tightly packed; GL's default 4-byte row alignment skews
        // RGB images whose row size isn't a multiple of 4 into diagonal garbage.
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, format, GL_UNSIGNED_BYTE, data);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glGenerateMipmap(GL_TEXTURE_2D);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        stbi_image_free(data);
    }
    else {
        std::cout << "Texture failed to load at path: " << path << std::endl;
        stbi_image_free(data);
    }
    return textureID;
}

GLuint Scene::loadHDRTexture(const char* path) {
    stbi_set_flip_vertically_on_load(true);
    int width, height, nrComponents;
    float *data = stbi_loadf(path, &width, &height, &nrComponents, 0);
    GLuint textureID;
    glGenTextures(1, &textureID);
    if (data) {
        capTextureF(data, width, height, nrComponents, textureCap());  // 8K HDRI -> cap
        GLenum format = GL_RGB;
        if (nrComponents == 1) format = GL_RED;
        else if (nrComponents == 3) format = GL_RGB;
        else if (nrComponents == 4) format = GL_RGBA;

        glBindTexture(GL_TEXTURE_2D, textureID);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, width, height, 0, format, GL_FLOAT, data);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        stbi_image_free(data);
    }
    else {
        std::cout << "Failed to load HDR image: " << path << std::endl;
    }
    return textureID;
}


GLuint Scene::loadTextureNoFlip(const char* path) {
    GLuint textureID;
    glGenTextures(1, &textureID);

    int width, height, nrComponents;
    stbi_set_flip_vertically_on_load(false);
    unsigned char* data = stbi_load(path, &width, &height, &nrComponents, 0);
    if (data && nrComponents == 2) {
        // See loadTexture: grey+alpha must be expanded to RGBA before upload.
        stbi_image_free(data);
        data = stbi_load(path, &width, &height, &nrComponents, 4);
        if (data) nrComponents = 4;
    }

    if (data) {
        capTextureUC(data, width, height, nrComponents, textureCap());  // 8K/4K -> cap
        GLenum format = GL_RGB;
        if (nrComponents == 1) format = GL_RED;
        else if (nrComponents == 3) format = GL_RGB;
        else if (nrComponents == 4) format = GL_RGBA;

        glBindTexture(GL_TEXTURE_2D, textureID);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);   // stbi rows are tightly packed
        glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, format, GL_UNSIGNED_BYTE, data);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glGenerateMipmap(GL_TEXTURE_2D);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        stbi_image_free(data);
    } else {
        std::cout << "Mask texture failed to load at path: " << path << std::endl;
        stbi_image_free(data);
    }
    return textureID;
}


// =====================================================================
// Caustics (render-to-texture)
// =====================================================================
// We rasterise a 256×256 grid of "ray samples" of the water surface
// into a 1024×1024 R16F texture. Each vertex of the grid:
//   * computes the wave height + normal at its UV position
//   * refracts the sun ray through that surface
//   * lands somewhere on the receiver plane (seabed Y)
//   * gl_Position = the receiver-plane UV  →  brightens that texel
// Additive blending accumulates many samples per texel where rays
// converge — the bright caustic filament.
//
// The resulting texture is sampled by terrain.frag in world UV space.
// =====================================================================
void Scene::initCaustics() {
    // 1) Build the grid VBO + EBO. We MUST render as triangles, not
    //    points, because the fragment shader uses dFdx/dFdy on the
    //    pre-refraction position to compute caustic intensity. Those
    //    derivatives are zero for isolated points.
    //    512x512 grid → fine caustic filaments (with the 9-tap blur in
    //    terrain.frag this is smooth; 768 was needlessly heavy).
    const int GRID = 512;
    std::vector<float>        grid;
    std::vector<unsigned int> indices;
    grid.reserve(GRID * GRID * 2);
    indices.reserve((GRID - 1) * (GRID - 1) * 6);

    for (int y = 0; y < GRID; ++y) {
        for (int x = 0; x < GRID; ++x) {
            grid.push_back((float)x / (float)(GRID - 1));
            grid.push_back((float)y / (float)(GRID - 1));
        }
    }
    for (int y = 0; y < GRID - 1; ++y) {
        for (int x = 0; x < GRID - 1; ++x) {
            unsigned int a =  y      * GRID + x;
            unsigned int b =  y      * GRID + x + 1;
            unsigned int c = (y + 1) * GRID + x;
            unsigned int d = (y + 1) * GRID + x + 1;
            indices.push_back(a); indices.push_back(b); indices.push_back(c);
            indices.push_back(b); indices.push_back(d); indices.push_back(c);
        }
    }
    causticsIndexCount = (int)indices.size();

    glGenVertexArrays(1, &causticsGridVAO);
    glGenBuffers(1, &causticsGridVBO);
    glGenBuffers(1, &causticsGridEBO);
    glBindVertexArray(causticsGridVAO);
    glBindBuffer(GL_ARRAY_BUFFER, causticsGridVBO);
    glBufferData(GL_ARRAY_BUFFER, grid.size() * sizeof(float),
                 grid.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, causticsGridEBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int),
                 indices.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    // 2) Caustics texture (4-channel float, 1024x1024). RGBA16F is the
    //    safest float format for blendable color attachments across
    //    drivers; we only use the R channel but keep RGBA for compat.
    glGenTextures(1, &causticsTex);
    glBindTexture(GL_TEXTURE_2D, causticsTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F,
                 causticsTexSize, causticsTexSize, 0,
                 GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    // 3) Caustics FBO.
    glGenFramebuffers(1, &causticsFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, causticsFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, causticsTex, 0);
    GLenum drawBufs[1] = { GL_COLOR_ATTACHMENT0 };
    glDrawBuffers(1, drawBufs);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        std::cout << "Caustics FBO incomplete, status=0x" << std::hex << st << std::dec << std::endl;
    } else {
        std::cout << "Caustics FBO ready: " << causticsTexSize << "x" << causticsTexSize
                  << ", grid indices=" << causticsIndexCount << std::endl;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Scene::renderCaustics(float windSpeed, const glm::vec3& sunDir) {
    if (!causticsProgram || !causticsFBO) return;

    // Skip when the sun is below the horizon — no caustics at night.
    if (sunDir.y < 0.05f) {
        // Clear the texture to zero so stale frames don't linger.
        glBindFramebuffer(GL_FRAMEBUFFER, causticsFBO);
        glViewport(0, 0, causticsTexSize, causticsTexSize);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }

    // Save state we'll change so the rest of the frame doesn't break.
    GLint  prevViewport[4]; glGetIntegerv(GL_VIEWPORT, prevViewport);
    GLint  prevFBO;         glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevFBO);
    GLboolean prevBlend     = glIsEnabled(GL_BLEND);
    GLboolean prevDepthTest = glIsEnabled(GL_DEPTH_TEST);
    GLboolean prevCull      = glIsEnabled(GL_CULL_FACE);

    glBindFramebuffer(GL_FRAMEBUFFER, causticsFBO);
    glViewport(0, 0, causticsTexSize, causticsTexSize);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);

    // Additive accumulation — that's how the bright filaments form.
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    glUseProgram(causticsProgram);
    setFloat(causticsProgram, "time", time);
    // Cap the wave speed FED TO the caustic waves: at high Wave Speed the Gerstner
    // amplitude here scales ~windSpeed^2, which over-focuses the caustics into a
    // harsh, chaotic, over-bright pattern. The render COST is fixed regardless; this
    // just keeps the caustics readable. (For real FPS, lower causticsTexSize / grid.)
    setFloat(causticsProgram, "windSpeed", glm::min(windSpeed, 1.5f));
    glUniform3fv(glGetUniformLocation(causticsProgram, "sunDirection"),
                 1, glm::value_ptr(sunDir));
    setFloat(causticsProgram, "terrainSize", terrainSize);
    setFloat(causticsProgram, "causticsTileSize", causticsTileSize);
    setFloat(causticsProgram, "waterY", 0.0f);
    // Receiver plane: rough average seabed depth. Caustics are
    // projected onto this virtual plane; the actual seabed is shaded
    // as if its caustic equals what the texture holds at its XZ.
    setFloat(causticsProgram, "floorY", -8.0f);

    glBindVertexArray(causticsGridVAO);
    // Render as triangles — derivatives needed by the frag shader
    // require a 2x2 quad of running fragments.
    glDrawElements(GL_TRIANGLES, causticsIndexCount, GL_UNSIGNED_INT, 0);

    // Regenerate mipmaps so distant seabed samples a smoothed caustic
    // (kills sparkle/aliasing far from the camera).
    glBindTexture(GL_TEXTURE_2D, causticsTex);
    glGenerateMipmap(GL_TEXTURE_2D);

    // Restore.
    glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    if (prevBlend)     glEnable(GL_BLEND);     else glDisable(GL_BLEND);
    if (prevDepthTest) glEnable(GL_DEPTH_TEST);else glDisable(GL_DEPTH_TEST);
    if (prevCull)      glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
}


// ----------------------------------------------------------------------
// Terrain collision queries
// ----------------------------------------------------------------------
