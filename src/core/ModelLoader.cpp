#include "ModelLoader.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_map>

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/pbrmaterial.h>

// ----------------------------------------------------------------------
// Public entry point — dispatch by extension.
// ----------------------------------------------------------------------
LoadedMesh ModelLoader::Load(const std::string& path) {
    auto pos = path.find_last_of('.');
    std::string ext = (pos == std::string::npos) ? "" : path.substr(pos + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });

    if (ext == "obj") return LoadOBJ(path);
    // Everything else goes through Assimp (FBX, gltf, glb, dae, ...).
    return LoadAssimp(path);
}

// ----------------------------------------------------------------------
// Minimal OBJ reader. Handles `v`, `vt`, `vn`, `f` with the patterns
//   f a/b/c d/e/f g/h/i           (pos/uv/normal)
//   f a//c d//f g//i              (pos//normal)
//   f a/b   d/e   g/h             (pos/uv)
//   f a     d     g               (pos)
// Triangulates polygons as fans.
// ----------------------------------------------------------------------
namespace {
struct OBJKey {
    int v, t, n;
    bool operator==(const OBJKey& o) const { return v == o.v && t == o.t && n == o.n; }
};
struct OBJKeyHash {
    size_t operator()(const OBJKey& k) const {
        return ((size_t)k.v * 73856093u) ^ ((size_t)k.t * 19349663u) ^ ((size_t)k.n * 83492791u);
    }
};

OBJKey parseFaceVert(const std::string& tok) {
    OBJKey k{ 0, 0, 0 };
    int slot = 0;
    int sign = 1, val = 0;
    bool inNumber = false;
    auto flush = [&]() {
        if (!inNumber) val = 0;
        int v = sign * val;
        if (slot == 0) k.v = v;
        else if (slot == 1) k.t = v;
        else if (slot == 2) k.n = v;
        inNumber = false; sign = 1; val = 0;
    };
    for (char c : tok) {
        if (c == '/') { flush(); ++slot; }
        else if (c == '-') { sign = -1; }
        else if (c >= '0' && c <= '9') { val = val * 10 + (c - '0'); inNumber = true; }
    }
    flush();
    return k;
}

// Directory portion of a path (with trailing slash), or "" for none.
std::string dirOf(const std::string& path) {
    auto p = path.find_last_of("/\\");
    return (p == std::string::npos) ? "" : path.substr(0, p + 1);
}
} // namespace

LoadedMesh ModelLoader::LoadOBJ(const std::string& path) {
    LoadedMesh out;
    std::ifstream f(path);
    if (!f.is_open()) {
        std::cerr << "ModelLoader: cannot open " << path << "\n";
        return out;
    }

    std::vector<float> positions;  // xyz
    std::vector<float> uvs;        // uv
    std::vector<float> normals;    // xyz

    std::unordered_map<OBJKey, unsigned int, OBJKeyHash> dedup;

    std::string line;
    float minY =  1e30f, maxY = -1e30f;

    while (std::getline(f, line)) {
        if (line.size() >= 2 && line[0] == 'v' && line[1] == ' ') {
            float x, y, z;
            std::istringstream(line.substr(2)) >> x >> y >> z;
            positions.push_back(x);
            positions.push_back(y);
            positions.push_back(z);
            if (y < minY) minY = y;
            if (y > maxY) maxY = y;
        } else if (line.size() >= 3 && line[0] == 'v' && line[1] == 't') {
            float u, v;
            std::istringstream(line.substr(3)) >> u >> v;
            uvs.push_back(u);
            uvs.push_back(v);
        } else if (line.size() >= 3 && line[0] == 'v' && line[1] == 'n') {
            float x, y, z;
            std::istringstream(line.substr(3)) >> x >> y >> z;
            normals.push_back(x);
            normals.push_back(y);
            normals.push_back(z);
        } else if (line.size() >= 2 && line[0] == 'f' && line[1] == ' ') {
            std::istringstream is(line.substr(2));
            std::vector<unsigned int> faceIdx;
            faceIdx.reserve(4);
            std::string tok;
            while (is >> tok) {
                OBJKey k = parseFaceVert(tok);
                // Resolve negative (relative) indices.
                int posCount = (int)(positions.size() / 3);
                int uvCount  = (int)(uvs.size() / 2);
                int nrmCount = (int)(normals.size() / 3);
                if (k.v < 0) k.v = posCount + k.v + 1;
                if (k.t < 0) k.t = uvCount + k.t + 1;
                if (k.n < 0) k.n = nrmCount + k.n + 1;

                auto it = dedup.find(k);
                unsigned int idx;
                if (it != dedup.end()) {
                    idx = it->second;
                } else {
                    idx = (unsigned int)(out.vertices.size() / 8);
                    // pos
                    if (k.v > 0 && k.v - 1 < posCount) {
                        out.vertices.push_back(positions[(k.v - 1) * 3 + 0]);
                        out.vertices.push_back(positions[(k.v - 1) * 3 + 1]);
                        out.vertices.push_back(positions[(k.v - 1) * 3 + 2]);
                    } else { out.vertices.push_back(0); out.vertices.push_back(0); out.vertices.push_back(0); }
                    // uv
                    if (k.t > 0 && k.t - 1 < uvCount) {
                        out.vertices.push_back(uvs[(k.t - 1) * 2 + 0]);
                        out.vertices.push_back(uvs[(k.t - 1) * 2 + 1]);
                    } else { out.vertices.push_back(0); out.vertices.push_back(0); }
                    // normal
                    if (k.n > 0 && k.n - 1 < nrmCount) {
                        out.vertices.push_back(normals[(k.n - 1) * 3 + 0]);
                        out.vertices.push_back(normals[(k.n - 1) * 3 + 1]);
                        out.vertices.push_back(normals[(k.n - 1) * 3 + 2]);
                    } else { out.vertices.push_back(0); out.vertices.push_back(1); out.vertices.push_back(0); }
                    dedup.emplace(k, idx);
                }
                faceIdx.push_back(idx);
            }
            // Triangulate as fan
            for (size_t i = 1; i + 1 < faceIdx.size(); ++i) {
                out.indices.push_back(faceIdx[0]);
                out.indices.push_back(faceIdx[i]);
                out.indices.push_back(faceIdx[i + 1]);
            }
        }
    }

    out.minY = minY;
    out.maxY = maxY;

    // If the OBJ had no normals, generate flat normals.
    if (normals.empty() && !out.vertices.empty()) {
        // Zero out then accumulate; finalise per-vertex.
        std::vector<float> nrmAcc(out.vertices.size() / 8 * 3, 0.0f);
        for (size_t t = 0; t + 2 < out.indices.size(); t += 3) {
            unsigned int ia = out.indices[t + 0];
            unsigned int ib = out.indices[t + 1];
            unsigned int ic = out.indices[t + 2];
            float ax = out.vertices[ia * 8 + 0], ay = out.vertices[ia * 8 + 1], az = out.vertices[ia * 8 + 2];
            float bx = out.vertices[ib * 8 + 0], by = out.vertices[ib * 8 + 1], bz = out.vertices[ib * 8 + 2];
            float cx = out.vertices[ic * 8 + 0], cy = out.vertices[ic * 8 + 1], cz = out.vertices[ic * 8 + 2];
            float ux = bx - ax, uy = by - ay, uz = bz - az;
            float vx = cx - ax, vy = cy - ay, vz = cz - az;
            float nx = uy * vz - uz * vy;
            float ny = uz * vx - ux * vz;
            float nz = ux * vy - uy * vx;
            nrmAcc[ia * 3 + 0] += nx; nrmAcc[ia * 3 + 1] += ny; nrmAcc[ia * 3 + 2] += nz;
            nrmAcc[ib * 3 + 0] += nx; nrmAcc[ib * 3 + 1] += ny; nrmAcc[ib * 3 + 2] += nz;
            nrmAcc[ic * 3 + 0] += nx; nrmAcc[ic * 3 + 1] += ny; nrmAcc[ic * 3 + 2] += nz;
        }
        for (size_t i = 0; i < nrmAcc.size(); i += 3) {
            float nx = nrmAcc[i + 0], ny = nrmAcc[i + 1], nz = nrmAcc[i + 2];
            float l = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (l > 1e-6f) { nx /= l; ny /= l; nz /= l; }
            else { ny = 1.0f; nx = nz = 0.0f; }
            out.vertices[(i / 3) * 8 + 5] = nx;
            out.vertices[(i / 3) * 8 + 6] = ny;
            out.vertices[(i / 3) * 8 + 7] = nz;
        }
    }

    // One submesh covering the whole buffer (OBJ uses a single texture
    // bound by the caller).
    if (!out.indices.empty()) {
        SubMeshGroup g;
        g.indexOffset = 0;
        g.indexCount  = (unsigned int)out.indices.size();
        out.submeshes.push_back(g);
    }

    std::cout << "OBJ " << path
              << "  v=" << out.vertexCount()
              << "  tris=" << out.triangleCount()
              << "  Y=[" << out.minY << ".." << out.maxY << "]\n";
    return out;
}

// ----------------------------------------------------------------------
// Assimp loader. Walks every node, transforms vertices into the scene's
// root space, merges into one buffer, and records a SubMeshGroup per
// aiMesh so each material slice keeps its own textures.
// ----------------------------------------------------------------------
namespace {

// A pending submesh recorded during the node walk: an index range and
// the source material index (resolved to texture paths afterwards).
struct PendingSlice {
    unsigned int offset = 0;
    unsigned int count  = 0;
    unsigned int matIdx = 0;
};

// Resolve a material's texture of a given type to a path relative to the
// model directory. Skips embedded textures (uri starting with '*').
std::string texPath(const aiMaterial* mat, aiTextureType type,
                    const std::string& modelDir) {
    if (!mat || mat->GetTextureCount(type) == 0) return "";
    aiString p;
    if (mat->GetTexture(type, 0, &p) != AI_SUCCESS) return "";
    std::string s = p.C_Str();
    if (s.empty() || s[0] == '*') return "";   // embedded — not handled
    // Normalize back-slashes and strip any leading "./".
    for (char& c : s) if (c == '\\') c = '/';
    if (s.rfind("./", 0) == 0) s = s.substr(2);
    return modelDir + s;
}

void resolveMaterial(const aiScene* scene, unsigned int matIdx,
                     const std::string& modelDir, SubMeshGroup& g) {
    if (matIdx >= scene->mNumMaterials) return;
    const aiMaterial* mat = scene->mMaterials[matIdx];

    // Base colour: glTF exposes BASE_COLOR (Assimp 5.0+); all versions
    // also expose DIFFUSE, which the glTF2 importer fills for compat.
    g.baseColorPath.clear();
#ifdef AI_MATKEY_BASE_COLOR
    g.baseColorPath = texPath(mat, aiTextureType_BASE_COLOR, modelDir);
#endif
    if (g.baseColorPath.empty())
        g.baseColorPath = texPath(mat, aiTextureType_DIFFUSE, modelDir);

    g.normalPath = texPath(mat, aiTextureType_NORMALS, modelDir);
    if (g.normalPath.empty())
        g.normalPath = texPath(mat, aiTextureType_HEIGHT, modelDir);

    // glTF metallic-roughness arrives under different enums depending on
    // the Assimp version — try the likely ones (PBR enums are 5.0+).
    g.metalRoughPath.clear();
#ifdef AI_MATKEY_BASE_COLOR
    g.metalRoughPath = texPath(mat, aiTextureType_METALNESS, modelDir);
    if (g.metalRoughPath.empty())
        g.metalRoughPath = texPath(mat, aiTextureType_DIFFUSE_ROUGHNESS, modelDir);
#endif
    if (g.metalRoughPath.empty())
        g.metalRoughPath = texPath(mat, aiTextureType_UNKNOWN, modelDir);

    // Base colour factor (tints untextured materials sensibly).
    aiColor4D col;
    bool gotColor = false;
#ifdef AI_MATKEY_BASE_COLOR
    gotColor = (mat->Get(AI_MATKEY_BASE_COLOR, col) == AI_SUCCESS);
#endif
    if (!gotColor)
        gotColor = (mat->Get(AI_MATKEY_COLOR_DIFFUSE, col) == AI_SUCCESS);
    if (gotColor) {
        g.baseColorFactor[0] = col.r;
        g.baseColorFactor[1] = col.g;
        g.baseColorFactor[2] = col.b;
        g.baseColorFactor[3] = col.a;
    }

    // glTF occlusionTexture arrives as LIGHTMAP in Assimp. If a material has
    // NO occlusion map, its metallicRoughness red channel is NOT AO — reading
    // it as AO black-spots the mesh wherever that channel happens to be dark.
    g.hasOcclusion = !texPath(mat, aiTextureType_LIGHTMAP, modelDir).empty();

    // Alpha mode (MASK foliage must alpha-test, BLEND must blend — drawing
    // them opaque shows the transparent texels as black patches).
    aiString am;
    if (mat->Get(AI_MATKEY_GLTF_ALPHAMODE, am) == AI_SUCCESS) {
        std::string s = am.C_Str();
        if (s == "MASK")  g.alphaMode = 1;
        else if (s == "BLEND") g.alphaMode = 2;
    }

    float metal = 1.0f;
    if (mat->Get(AI_MATKEY_GLTF_PBRMETALLICROUGHNESS_METALLIC_FACTOR, metal) == AI_SUCCESS)
        g.metallicFactor = metal;

    // Emissive (jellyfish glow): approximate "emissive texture x factor" as
    // "albedo x factor" in the shader, which holds for the assets we ship.
    aiColor3D em(0.0f, 0.0f, 0.0f);
    if (mat->Get(AI_MATKEY_COLOR_EMISSIVE, em) == AI_SUCCESS) {
        g.emissiveFactor[0] = em.r;
        g.emissiveFactor[1] = em.g;
        g.emissiveFactor[2] = em.b;
    }
}

void appendNode(const aiScene* scene, const aiNode* node,
                const aiMatrix4x4& parentXform,
                LoadedMesh& out, std::vector<PendingSlice>& slices)
{
    aiMatrix4x4 xform = parentXform * node->mTransformation;
    aiMatrix3x3 nXform = aiMatrix3x3(xform);
    // Rough inverse-transpose for normals — uniform scale assumed.
    nXform.Inverse();
    nXform.Transpose();

    for (unsigned i = 0; i < node->mNumMeshes; ++i) {
        const aiMesh* m = scene->mMeshes[node->mMeshes[i]];
        unsigned int baseIdx   = (unsigned int)(out.vertices.size() / 8);
        unsigned int sliceStart = (unsigned int)out.indices.size();
        for (unsigned v = 0; v < m->mNumVertices; ++v) {
            aiVector3D p = xform * m->mVertices[v];
            aiVector3D n = m->HasNormals()
                ? (nXform * m->mNormals[v]).Normalize()
                : aiVector3D(0, 1, 0);
            float u = 0.0f, vv = 0.0f;
            if (m->HasTextureCoords(0)) {
                u  = m->mTextureCoords[0][v].x;
                vv = m->mTextureCoords[0][v].y;
            }
            out.vertices.push_back(p.x);
            out.vertices.push_back(p.y);
            out.vertices.push_back(p.z);
            out.vertices.push_back(u);
            out.vertices.push_back(vv);
            out.vertices.push_back(n.x);
            out.vertices.push_back(n.y);
            out.vertices.push_back(n.z);

            if (p.y < out.minY) out.minY = p.y;
            if (p.y > out.maxY) out.maxY = p.y;
        }
        for (unsigned f = 0; f < m->mNumFaces; ++f) {
            const aiFace& face = m->mFaces[f];
            if (face.mNumIndices < 3) continue;
            for (unsigned k = 1; k + 1 < face.mNumIndices; ++k) {
                out.indices.push_back(baseIdx + face.mIndices[0]);
                out.indices.push_back(baseIdx + face.mIndices[k]);
                out.indices.push_back(baseIdx + face.mIndices[k + 1]);
            }
        }
        unsigned int sliceEnd = (unsigned int)out.indices.size();
        if (sliceEnd > sliceStart) {
            PendingSlice ps;
            ps.offset = sliceStart;
            ps.count  = sliceEnd - sliceStart;
            ps.matIdx = m->mMaterialIndex;
            slices.push_back(ps);
        }
    }
    for (unsigned i = 0; i < node->mNumChildren; ++i)
        appendNode(scene, node->mChildren[i], xform, out, slices);
}
} // namespace

LoadedMesh ModelLoader::LoadAssimp(const std::string& path) {
    LoadedMesh out;
    out.minY =  1e30f;
    out.maxY = -1e30f;

    Assimp::Importer imp;
    // NO aiProcess_FlipUVs: Assimp's importers already emit UVs in the
    // bottom-left (OBJ/OpenGL) convention — its glTF2 importer outputs
    // 1-v relative to the file. Our textures are stb-loaded with a
    // vertical flip (also bottom-left), so the conventions match as-is.
    // Adding FlipUVs flipped V a THIRD time, so atlases sampled their
    // black background (black patches on rocks, smeared fish skins).
    const aiScene* scene = imp.ReadFile(
        path,
        aiProcess_Triangulate
        | aiProcess_GenSmoothNormals
        | aiProcess_JoinIdenticalVertices
        | aiProcess_ImproveCacheLocality);
    if (!scene || !scene->HasMeshes()) {
        std::cerr << "ModelLoader::LoadAssimp failed for " << path
                  << ": " << imp.GetErrorString() << "\n";
        out.minY = out.maxY = 0.0f;
        return out;
    }

    std::string modelDir = dirOf(path);
    std::vector<PendingSlice> slices;
    aiMatrix4x4 identity;
    appendNode(scene, scene->mRootNode, identity, out, slices);

    // Resolve each pending slice's material into a SubMeshGroup.
    for (const auto& ps : slices) {
        SubMeshGroup g;
        g.indexOffset = ps.offset;
        g.indexCount  = ps.count;
        resolveMaterial(scene, ps.matIdx, modelDir, g);
        out.submeshes.push_back(g);
    }

    if (out.vertices.empty()) { out.minY = out.maxY = 0.0f; }
    std::cout << "Assimp " << path
              << "  v=" << out.vertexCount()
              << "  tris=" << out.triangleCount()
              << "  parts=" << out.submeshes.size()
              << "  Y=[" << out.minY << ".." << out.maxY << "]\n";
    return out;
}

// ----------------------------------------------------------------------
// Split a model into one LoadedMesh per aiMesh — used for asset packs
// that bundle many distinct objects (rock collections) in one file.
// Each returned mesh is baked to world space, recentred on XZ with its
// base at y=0, and carries a single submesh with its own textures.
// ----------------------------------------------------------------------
namespace {
void collectMeshNodes(const aiScene* scene, const aiNode* node,
                      const aiMatrix4x4& parentXform,
                      std::vector<std::pair<const aiMesh*, aiMatrix4x4>>& outMeshes) {
    aiMatrix4x4 xform = parentXform * node->mTransformation;
    for (unsigned i = 0; i < node->mNumMeshes; ++i)
        outMeshes.emplace_back(scene->mMeshes[node->mMeshes[i]], xform);
    for (unsigned i = 0; i < node->mNumChildren; ++i)
        collectMeshNodes(scene, node->mChildren[i], xform, outMeshes);
}
} // namespace

std::vector<LoadedMesh> ModelLoader::LoadAssimpParts(const std::string& path) {
    std::vector<LoadedMesh> parts;

    Assimp::Importer imp;
    // NO aiProcess_FlipUVs — see LoadAssimp above.
    const aiScene* scene = imp.ReadFile(
        path,
        aiProcess_Triangulate
        | aiProcess_GenSmoothNormals
        | aiProcess_JoinIdenticalVertices
        | aiProcess_ImproveCacheLocality);
    if (!scene || !scene->HasMeshes()) {
        std::cerr << "ModelLoader::LoadAssimpParts failed for " << path
                  << ": " << imp.GetErrorString() << "\n";
        return parts;
    }

    std::string modelDir = dirOf(path);
    std::vector<std::pair<const aiMesh*, aiMatrix4x4>> meshNodes;
    aiMatrix4x4 identity;
    collectMeshNodes(scene, scene->mRootNode, identity, meshNodes);

    for (auto& mn : meshNodes) {
        const aiMesh* m = mn.first;
        const aiMatrix4x4& xform = mn.second;
        if (m->mNumVertices == 0 || m->mNumFaces == 0) continue;

        aiMatrix3x3 nXform = aiMatrix3x3(xform);
        nXform.Inverse();
        nXform.Transpose();

        LoadedMesh out;
        out.minY = 1e30f; out.maxY = -1e30f;
        for (unsigned v = 0; v < m->mNumVertices; ++v) {
            aiVector3D p = xform * m->mVertices[v];
            aiVector3D n = m->HasNormals()
                ? (nXform * m->mNormals[v]).Normalize()
                : aiVector3D(0, 1, 0);
            float u = 0.0f, vv = 0.0f;
            if (m->HasTextureCoords(0)) {
                u  = m->mTextureCoords[0][v].x;
                vv = m->mTextureCoords[0][v].y;
            }
            out.vertices.push_back(p.x); out.vertices.push_back(p.y); out.vertices.push_back(p.z);
            out.vertices.push_back(u);   out.vertices.push_back(vv);
            out.vertices.push_back(n.x); out.vertices.push_back(n.y); out.vertices.push_back(n.z);
        }
        for (unsigned f = 0; f < m->mNumFaces; ++f) {
            const aiFace& face = m->mFaces[f];
            if (face.mNumIndices < 3) continue;
            for (unsigned k = 1; k + 1 < face.mNumIndices; ++k) {
                out.indices.push_back(face.mIndices[0]);
                out.indices.push_back(face.mIndices[k]);
                out.indices.push_back(face.mIndices[k + 1]);
            }
        }
        // Recentre on XZ, drop base to y=0.
        int vc = out.vertexCount();
        float mnx = 1e30f, mny = 1e30f, mnz = 1e30f, mxx = -1e30f, mxz = -1e30f;
        for (int i = 0; i < vc; ++i) {
            float* p = &out.vertices[i * 8];
            mnx = std::min(mnx, p[0]); mxx = std::max(mxx, p[0]);
            mny = std::min(mny, p[1]);
            mnz = std::min(mnz, p[2]); mxz = std::max(mxz, p[2]);
        }
        float cx = (mnx + mxx) * 0.5f, cz = (mnz + mxz) * 0.5f;
        for (int i = 0; i < vc; ++i) {
            float* p = &out.vertices[i * 8];
            p[0] -= cx; p[1] -= mny; p[2] -= cz;
            if (p[1] < out.minY) out.minY = p[1];
            if (p[1] > out.maxY) out.maxY = p[1];
        }

        SubMeshGroup g;
        g.indexOffset = 0;
        g.indexCount  = (unsigned int)out.indices.size();
        resolveMaterial(scene, m->mMaterialIndex, modelDir, g);
        out.submeshes.push_back(g);
        parts.push_back(std::move(out));
    }

    std::cout << "AssimpParts " << path << "  objects=" << parts.size() << "\n";
    return parts;
}
