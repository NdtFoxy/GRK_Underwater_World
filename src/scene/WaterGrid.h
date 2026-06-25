#pragma once
#include <vector>

// Camera-centered radial ocean grid (GL 3.3 LOD substitute for tessellation):
// a square grid whose cells grow geometrically with distance from the center,
// so it is dense near the camera and coarse toward the horizon. Generated once
// in local space centered on origin; the caller translates it under the camera.
// Vertex layout matches the old flat grid: 3 floats position (y=0) + 2 floats uv.
struct WaterGridMesh {
    std::vector<float>        vertices;  // x, 0, z, u, v
    std::vector<unsigned int> indices;
    float halfExtent = 0.0f;            // local half-size (metres) — for edge-fade + recenter
};

inline WaterGridMesh buildRadialWaterGrid(int sidePts = 193, float innerCell = 1.5f,
                                          float growth = 1.045f) {
    WaterGridMesh m;
    int half = sidePts / 2;
    std::vector<float> coord(sidePts);
    coord[half] = 0.0f;
    float cell = innerCell, pos = 0.0f;
    for (int i = half + 1; i < sidePts; ++i) { pos += cell; coord[i] = pos; cell *= growth; }
    cell = innerCell; pos = 0.0f;
    for (int i = half - 1; i >= 0; --i) { pos -= cell; coord[i] = pos; cell *= growth; }
    m.halfExtent = coord[sidePts - 1];
    for (int i = 0; i < sidePts; ++i)
        for (int j = 0; j < sidePts; ++j) {
            float x = coord[j], z = coord[i];
            m.vertices.push_back(x); m.vertices.push_back(0.0f); m.vertices.push_back(z);
            m.vertices.push_back(x); m.vertices.push_back(z);   // uv = local xz (unused; waves sample world xz)
        }
    for (int i = 0; i < sidePts - 1; ++i)
        for (int j = 0; j < sidePts - 1; ++j) {
            unsigned int r1 = i * sidePts, r2 = (i + 1) * sidePts;
            m.indices.push_back(r1 + j);     m.indices.push_back(r1 + j + 1); m.indices.push_back(r2 + j);
            m.indices.push_back(r1 + j + 1); m.indices.push_back(r2 + j + 1); m.indices.push_back(r2 + j);
        }
    return m;
}
