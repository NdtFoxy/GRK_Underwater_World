#pragma once
#include <string>
#include <vector>

// One material group inside a loaded model. glTF/FBX models routinely
// carry several materials (rock packs, palms, fish), each with its own
// textures. We keep the geometry in ONE shared vertex/index buffer and
// describe each material as a contiguous slice of the index buffer plus
// the resolved (model-dir + uri) texture paths. The renderer binds the
// right textures per slice — this is what fixes the "black / white
// camouflage" look on multi-material meshes.
struct SubMeshGroup {
    unsigned int indexOffset = 0;   // element offset into LoadedMesh.indices
    unsigned int indexCount  = 0;   // number of indices in this slice
    std::string  baseColorPath;     // resolved path, or empty
    std::string  normalPath;        // resolved path, or empty
    std::string  metalRoughPath;    // resolved path, or empty (glTF MR / ARM)
    float        baseColorFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    int          alphaMode = 0;     // glTF: 0 OPAQUE, 1 MASK (cutout), 2 BLEND
    bool         hasOcclusion = false; // occlusionTexture present (MR red = real AO)
    float        metallicFactor = 1.0f;   // glTF metallic multiplier
    float        emissiveFactor[3] = { 0.0f, 0.0f, 0.0f };
};

// Single-buffer mesh. Both OBJ and FBX/glTF (via Assimp) are supported.
// All submeshes are merged into one buffer so a model can be drawn with
// one VAO; `submeshes` records the per-material index ranges + textures.
//
// Vertex layout (8 floats per vertex):
//   pos.xyz  (3)
//   uv.xy    (2)
//   normal.xyz (3)
struct LoadedMesh {
    std::vector<float>        vertices;   // 8 floats per vertex
    std::vector<unsigned int> indices;
    std::vector<SubMeshGroup> submeshes;  // per-material slices + textures
    float minY = 0.0f;
    float maxY = 0.0f;

    bool valid() const { return !vertices.empty() && !indices.empty(); }
    int  vertexCount() const { return (int)(vertices.size() / 8); }
    int  triangleCount() const { return (int)(indices.size() / 3); }
};

class ModelLoader {
public:
    // Auto-detects format by file extension. Returns an empty mesh on
    // failure (and prints to std::cerr).
    static LoadedMesh Load(const std::string& path);

    // Format-specific entry points (exposed for tests / direct use).
    static LoadedMesh LoadOBJ(const std::string& path);
    static LoadedMesh LoadAssimp(const std::string& path);   // FBX, glTF, ...

    // Split a model into one LoadedMesh per Assimp mesh (baked to world
    // space, each with its own material textures). Use this for asset
    // packs that bundle MANY separate objects in a single file (rock
    // collections) so each object can be scattered individually instead
    // of being merged into one unusable blob.
    static std::vector<LoadedMesh> LoadAssimpParts(const std::string& path);
};
