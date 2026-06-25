#pragma once
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include "../render/Framebuffer.h"
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <unordered_map>
#include "Camera.h"
#include "../core/ModelLoader.h"
#include "../core/HeightmapLoader.h"
#include "VegetationSystem.h"
#include "../player/Flashlight.h"
#include "../render/Cubemap.h"
#include "../render/ShadowMap.h"
#include "SplinePath.h"
#include "WaveField.h"
#include "Weather.h"
#include "PalmMesh.h"
#include "FishSchool.h"

class Scene {
public:
    Scene(int width, int height);
    ~Scene();
    GLuint waterNormalTextureID;
    GLuint hdriTextureID;
    GLuint loadTexture(const char* path);
    GLuint loadTextureNoFlip(const char* path);
    GLuint loadHDRTexture(const char* path);
    void Init();
    void Update(GLFWwindow* window);
    void Render(Camera& camera, float waveSpeed, float cloudSpeed, float waveAmp, float timeOfDay, glm::vec3 sunDir);
    void OnResize(int width, int height);

    // Render-resolution scale: the 3D scene renders into a smaller target
    // (scale<1) and the post pass upscales it to the full window; the UI
    // stays crisp at native res. The single biggest fill-rate lever.
    void  SetRenderScale(float s);
    float GetRenderScale() const { return renderScale; }

    // Stats accessor for the in-game ImGui overlay
    const VegetationSystem& GetVegetation() const { return vegetation; }
    const VegetationSystem& GetCoral()      const { return coral; }

    // Terrain frustum-culling stats for the overlay.
    int  GetTerrainChunksDrawn() const { return terrainChunksDrawn; }
    int  GetTerrainChunksTotal() const { return (int)terrainChunks.size(); }
    int  GetPropDrawnLOD0() const { return propDrawnLOD0; }
    int  GetPropDrawnLOD1() const { return propDrawnLOD1; }
    int  GetPropDrawnLOD2() const { return propDrawnLOD2; }
    int  GetPropPruned() const { return propPrunedDistance; }
    int  GetPropFrustumCulled() const { return propCulledFrustum; }
    int  GetCreatureDrawnLOD0() const { return creatureDrawnLOD0; }
    int  GetCreatureDrawnLOD1() const { return creatureDrawnLOD1; }
    int  GetCreatureDrawnLOD2() const { return creatureDrawnLOD2; }

    // Weather control for the ImGui panel.
    void  SelectWeather(int idx) { weather.select(idx); }
    int   GetWeatherIndex() const { return weather.current; }
    int   GetWeatherCount() const { return (int)weather.presets.size(); }
    const char* GetWeatherName(int i) const {
        return (i >= 0 && i < (int)weather.presets.size())
                   ? weather.presets[i].name.c_str() : "";
    }
    void  SetStormOverride(float v) { stormOverride = v; }   // -1 to release
    float GetStormIntensity() const { return stormIntensity; }
    // Lightning flicker on/off (the random sky flash). Off forces it dark.
    void  SetLightningEnabled(bool e) { lightningEnabled = e; }
    bool  GetLightningEnabled() const { return lightningEnabled; }
    // Live tuning of the current preset from the panel (Req 9.4). We edit
    // the preset (not the lerped 'active'), so changes persist as update()
    // converges 'active' toward them.
    float* WeatherShallowPtr() { return &weather.presets[weather.current].waterShallow.x; }
    float* WeatherDeepPtr()    { return &weather.presets[weather.current].waterDeep.x; }
    float* WeatherCloudDensityPtr() { return &weather.presets[weather.current].cloudDensity; }

    // Prevailing wind direction (degrees), matching the Gerstner WaveField's
    // constant wind. Drives the underwater current pull in main.cpp.
    float GetWindDirDeg() const { return 19.29f; }   // atan2(0.35, 1.0)

    // --- Terrain collision query ---------------------------------
    // Returns the world-space terrain height (Y) at world (x,z), or a
    // very low value if there is no terrain there (a hole / outside).
    // Used by the player controller to stop the camera sinking through
    // the seabed.
    float SampleTerrainHeight(float worldX, float worldZ) const;

    // --- Points of interest for the locator UI -------------------
    bool      HasWreck() const { return wreckPlaced; }
    glm::vec3 GetWreckPos() const { return wreckPos; }
    glm::vec3 GetSharkPos() const {
        for (const auto& c : creatures) if (c.isShark) return c.pos;
        return glm::vec3(0.0f);
    }
    // Current world position of the patrolling sea-serpent (PTF body),
    // updated each frame in Scene_Render — used by the teleport UI.
    glm::vec3 GetSerpentPos() const { return serpentPos; }
    // 0..1 while the shark is hunting nearby — drives the panic post FX.
    float GetSharkThreat() const { return sharkThreat; }

    // Sonar ping (Q): emits an expanding shell from `origin` that reveals
    // world geometry in the depth pass and lights up creatures by type.
    void FireSonar(const glm::vec3& origin);

    // Camera-attached flashlight (toggled with F in main.cpp).
    Flashlight flashlight;

private:
    // Unified Gerstner wave field — drives the rendered water surface (GPU water
    // shader) AND the CPU buoyancy/underwater queries, so boats sit exactly on
    // the surface they see (see WaveField.h).
    WaveField waveField;
    float waterLevel = 0.0f;   // sea level (Y), fixed across subsystems

    // Weather presets driving water + cloud colour and wave amplitude.
    WeatherSystem weather;

    float stormIntensity = 0.15f;   // live sea-state (weather target, optionally overridden by input)
    float stormOverride  = -1.0f;   // >=0 forces a manual intensity (interaction); <0 = follow weather

    // === Sunken wreck POI + shark threat ======================
    glm::vec3 wreckPos = glm::vec3(0.0f);
    bool      wreckPlaced = false;
    float     sharkThreat = 0.0f;   // 0..1, decays when the shark loses interest

    // === Sonar ping (Q) ======================================
    bool      sonarActive = false;
    float     sonarStart  = 0.0f;            // Scene `time` at ping launch
    glm::vec3 sonarOrigin = glm::vec3(0.0f); // world-space ping centre

    float flashTimer      = 0.0f;   // cooldown until the next possible strike
    float lightningFlash  = 0.0f;   // 0..1 current flash brightness (decays fast)
    bool  lightningEnabled = true;  // UI toggle: off = no flicker (forces flash to 0)

    int screenWidth;
    int screenHeight;
    float renderScale = 0.85f;   // 3D scene internal-resolution scale (0.5..1)
    int scaledW() const { int w = (int)(screenWidth  * renderScale + 0.5f); return w < 1 ? 1 : w; }
    int scaledH() const { int h = (int)(screenHeight * renderScale + 0.5f); return h < 1 ? 1 : h; }
    float time;
    float frameDt = 0.0f;   // last frame delta (seconds), for particle updates

    Framebuffer* mainFBO;
    Framebuffer* ssrFBO = nullptr;   // opaque-scene snapshot (color+depth) for SSR

    // Шейдеры
    GLuint waterProgram;
    GLuint screenProgram;
    GLuint skyProgram;
    GLuint seabedProgram;
    GLuint terrainProgram;

    // Текстуры песка
    GLuint sandDiffuseID;
    GLuint sandNormalID;
    GLuint sandRoughnessID;

    // Биомные текстуры (terrain)
    GLuint mudDiffuseID, mudNormalID, mudRoughnessID;
    GLuint rockDiffuseID, rockNormalID, rockARMID; // ARM: AO/Rough/Metallic
    GLuint lavaDiffuseID, lavaNormalID, lavaRoughnessID, lavaEmissiveID;

    // Маски биомов (RGBA, альфа=0 = биом неактивен)
    GLuint castleMaskTexID;
    GLuint lavaMaskTexID;
    GLuint riverMaskTexID;

    // Сетка воды
    GLuint waterVAO, waterVBO, waterEBO;
    int waterIndicesCount;
    float waterHalfExtent = 0.0f;   // radial grid local half-size (recenter + shader edge-fade)
    GLuint shoreDataTextureID = 0;  // RGBA32F: height, valid, distance-to-land, land mask
    int shoreDataResolution = 0;


    // Дно (seabed) — legacy flat plane
    GLuint seabedVAO, seabedVBO, seabedEBO;
    int seabedIndicesCount;

    // Terrain (heightmap-based world)
    GLuint terrainVAO, terrainVBO, terrainEBO;
    int terrainIndicesCount;
    HeightmapData heightmapData;
    float terrainSize;    // world units
    float heightScale;    // vertical scale factor

    // --- Terrain chunking for frustum culling --------------------
    // The terrain mesh is one static VBO, but the index buffer is laid
    // out in spatial tiles. Each chunk stores its world-space AABB and
    // its slice of the shared EBO, so at render time we draw only the
    // chunks inside the camera frustum. Graphics are identical — we just
    // skip submitting triangles that are off-screen.
    struct TerrainChunk {
        glm::vec3 aabbMin;
        glm::vec3 aabbMax;
        int       indexCount = 0;
        size_t    indexOffset = 0;   // byte offset into terrainEBO (== LOD0)
        // Discrete distance LODs: full grid, every-2nd, every-4th vertex.
        // Same vertex buffer, coarser index sets → far chunks draw far
        // fewer triangles with no visible change through the fog.
        int       lodCount[3]  = { 0, 0, 0 };
        size_t    lodOffset[3] = { 0, 0, 0 };
    };
    std::vector<TerrainChunk> terrainChunks;
    int terrainChunksDrawn = 0;      // stat for the ImGui overlay

    // --- Collision height field (downsampled terrain heights) ----
    std::vector<float> collisionHeights;  // row-major, collisionRes x collisionRes
    std::vector<unsigned char> collisionValid;
    int   collisionRes = 0;               // grid resolution
    float collisionNoTerrain = -100000.0f;

    // Квадрат экрана (для FBO)
    GLuint quadVAO, quadVBO;

    // Полноэкранный квад для неба
    GLuint skyVAO, skyVBO;

    // === Underwater cubemap skybox ========================
    GLuint    skyboxProgram = 0;
    GLuint    cubeVAO = 0, cubeVBO = 0;
    Cubemap   envCubemap;
    void createSkyboxCube();

    // === Shadow mapping (depth pass from the sun + PCF) =====
    GLuint    depthProgram = 0;
    ShadowMap shadowMap;
    void      renderShadowDepth(const glm::vec3& sunDir, const glm::vec3& camPos);

    // === Parallel Transport Frames sea-serpent ==============
    GLuint     splineProgram = 0;
    SplinePath serpent;
    glm::vec3  serpentPos = glm::vec3(0.0f);   // current patrol position (for teleport UI)

    // === Shared object shader (static props + animated creatures) ==========
    GLuint                       objectProgram = 0;

    // === Boids fish schools (instanced, A07) =================
    GLuint                  fishProgram = 0;
    std::vector<FishSchool> fishSchools;
    // Previous-frame camera, used by the fish/creature update that runs before
    // camPos is recomputed.
    glm::vec3      prevCamPos = glm::vec3(0.0f);
    bool           prevCamValid = false;

    // === Static props (rocks / cliffs / palms from glTF) ====
    // Each prop is a loaded mesh instance placed on the terrain. Drawn
    // through the object shader (albedo + normal + ARM). One GPU mesh is
    // shared by many placements (cheap), each with its own transform.
    //
    // glTF assets carry SEVERAL materials (rock packs, palms, fish), each
    // with its own textures. We keep one VAO per mesh and a list of
    // submesh slices, each binding its own resolved textures. This is the
    // fix for the "black / white camouflage" look of multi-material meshes.
    struct PropSub {
        unsigned int indexOffset = 0;   // element offset into the EBO
        unsigned int indexCount  = 0;
        GLuint albedo = 0, normalTex = 0, armTex = 0;
        glm::vec3 baseFactor = glm::vec3(1.0f);
        float baseAlpha = 1.0f;   // glTF baseColorFactor.a — near-0 glass slices are skipped at draw
        int   alphaMode = 0;      // 0 opaque, 1 alpha-cut (MASK), 2 alpha-blend
        bool  armIsMR = false;    // armTex has no AO in R (pure metallicRoughness)
        float metallicFactor = 1.0f;
        glm::vec3 emissive = glm::vec3(0.0f);  // albedo-modulated glow (jellyfish)
    };
    struct PropMesh {
        struct PropLOD {
            GLuint ebo = 0;
            int    indexCount = 0;
            int    triCount = 0;
            std::vector<PropSub> subs;
        };
        GLuint vao = 0, vbo = 0;
        PropLOD lods[3];
        float  fitHeight = 1.0f;   // model height after normalization
        float  radius = 10.0f;      // fitted approximate world radius
        bool   allowTriangleLod = false; // true only for foliage-like meshes
    };
    struct PropInstance {
        int   meshId = 0;
        glm::mat4 model = glm::mat4(1.0f);
        glm::vec3 tint = glm::vec3(1.0f);
        bool  alphaCut = false;    // foliage (ferns/palms) uses alpha test
        float lifeDist = 450.0f;    // stochastic pruning radius
        float lodBias = 1.0f;       // lower = switch to cheaper LODs earlier
        float cullRadius = 20.0f;   // approximate fitted AABB radius
    };
    std::vector<PropMesh>     propMeshes;
    std::vector<PropInstance> propInstances;
    int propDrawnLOD0 = 0, propDrawnLOD1 = 0, propDrawnLOD2 = 0;
    int propPrunedDistance = 0;
    int propCulledFrustum = 0;
    int creatureDrawnLOD0 = 0, creatureDrawnLOD1 = 0, creatureDrawnLOD2 = 0;
    // Texture cache so shared texture files load once.
    std::unordered_map<std::string, GLuint> textureCache;
    GLuint loadTextureCached(const std::string& path);
    // Build a PropMesh from a LoadedMesh (uploads geometry + resolves the
    // per-submesh textures from the glTF material paths).
    int  buildPropMesh(LoadedMesh mesh, float targetSize, bool allowTriangleLod = false);
    int  loadPropMesh(const char* gltf, float targetSize, bool allowTriangleLod = false);
    void initProps();
    // Cached uniform locations for the object shader's per-submesh state.
    struct ObjectShaderLocs {
        GLint tint = -1, useTex = -1, albedo = -1;
        GLint useNormal = -1, normal = -1;
        GLint useArm = -1, arm = -1, armIsMR = -1, metalFactor = -1;
        GLint emissive = -1, useCut = -1, useBlend = -1, matAlpha = -1;
    };
    ObjectShaderLocs queryObjectShaderLocs() const;
    void drawPropSubmeshes(const PropMesh& pm, int lodIndex,
                           const glm::vec3& tint,
                           const ObjectShaderLocs& locs,
                           bool instanceAlphaCut);
    void renderProps(const glm::mat4& view, const glm::mat4& projection,
                     const glm::vec3& camPos, const glm::vec3& sunDir,
                     Camera& camera);

    // === Animated sea creatures (jellyfish drift + tuna swim) =======
    // Drawn through the object shader, animated on the CPU (drift / swim
    // along looping paths) with a vertex-shader tail sway for the fish.
    struct Creature {
        int   meshId = 0;
        glm::vec3 center = glm::vec3(0.0f);  // orbit centre (shark: home)
        float radius = 20.0f;                // swim circle radius
        float speed  = 0.3f;                 // angular speed
        float phase  = 0.0f;                 // start angle
        float bob    = 1.0f;                 // vertical bob amplitude
        float scale  = 1.0f;
        bool  isFish = false;                // fish sway tail in shader
        glm::vec3 tint = glm::vec3(1.0f);    // per-creature colour (shark: dark)
        glm::mat4 orient = glm::mat4(1.0f);  // asset-specific local correction
        glm::mat4 model = glm::mat4(1.0f);
        // --- Shark AI state (used only when isShark) -------------
        bool  isShark = false;
        glm::vec3 pos = glm::vec3(0.0f);     // integrated position
        float yaw = 0.0f, pitchCur = 0.0f;   // smoothed facing
        int   state = 0;                     // 0 patrol, 1 hunt
    };
    std::vector<Creature> creatures;
    void initCreatures();
    void updateCreatures(const glm::vec3& playerPos);
    void renderCreatures(const glm::mat4& view, const glm::mat4& projection,
                         const glm::vec3& camPos, const glm::vec3& sunDir,
                         Camera& camera);

    // Vegetation (instanced kelp + LOD + stochastic pruning)
    GLuint vegetationProgram = 0;
    VegetationSystem vegetation;       // kelp on the sandy shelf
    VegetationSystem coral;            // coral on shallower zones
    VegetationSystem palms;            // palms on the islands (above water)
    VegetationSystem grassLand;        // grass tufts on the island tops
    VegetationSystem seagrass;         // swaying seagrass beds (patchy biomes)

    // ----- Caustics (render-to-texture) ----------
    // We render a dense grid of refracted-light points into this
    // texture each frame; terrain.frag samples it as the caustic
    // overlay on the seabed. This is the same algorithm Evan Wallace
    // used in his classic WebGL Water demo.
    GLuint causticsProgram = 0;
    GLuint causticsFBO     = 0;
    GLuint causticsTex     = 0;
    GLuint causticsGridVAO = 0;
    GLuint causticsGridVBO = 0;
    GLuint causticsGridEBO = 0;
    int    causticsIndexCount = 0;
    int    causticsTexSize    = 1024;    // 1024 is plenty with a dense grid; 2048 was wasteful fill
    float  causticsTileSize   = 160.0f;  // world units the caustic tile covers (then GL_REPEAT)
    void   initCaustics();
    void   renderCaustics(float windSpeed, const glm::vec3& sunDir);

    void createWaterGrid(int resolution, float size);
    void createShorelineDataTexture();
    void createSeabed(float size, float depth);
    void createTerrainFromHeightmap();
    void createScreenQuad();
    void createSkyQuad();
};
