// ----------------------------------------------------------------------
// Scene — terrain / seabed (graded method B07: heightmap-driven seabed).
// Split out of Scene.cpp: shoreline data texture, flat seabed fallback,
// heightmap terrain mesh build, and the CPU-side collision height field
// sampling used for buoyancy / floor queries. All methods belong to the
// Scene class declared in Scene.h.
// ----------------------------------------------------------------------
#include "Scene.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cmath>
#include <vector>
#include <thread>
#include <iostream>

void Scene::createShorelineDataTexture() {
    if (shoreDataTextureID) {
        glDeleteTextures(1, &shoreDataTextureID);
        shoreDataTextureID = 0;
    }
    shoreDataResolution = 0;

    if (collisionRes <= 1 || collisionHeights.empty() || collisionValid.empty()) {
        std::cout << "Shoreline data texture skipped: no collision field\n";
        return;
    }

    shoreDataResolution = collisionRes;
    int n = shoreDataResolution;
    float cellWorld = terrainSize / (float)(n - 1);
    const float INF = 1.0e6f;
    std::vector<float> shoreDistance((size_t)n * n, INF);
    std::vector<float> landMask((size_t)n * n, 0.0f);

    // Distance to real above-water land, not merely shallow terrain.
    // This keeps surf foam tight to the island outline instead of
    // painting every shallow shelf as white water.
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            int idx = y * n + x;
            bool land = collisionValid[idx] && collisionHeights[idx] > waterLevel + 0.25f;
            if (land) {
                shoreDistance[idx] = 0.0f;
                landMask[idx] = 1.0f;
            }
        }
    }

    auto relax = [&](int x, int y, int nx, int ny, float w) {
        if (nx < 0 || nx >= n || ny < 0 || ny >= n) return;
        int idx = y * n + x;
        int nidx = ny * n + nx;
        shoreDistance[idx] = std::min(shoreDistance[idx], shoreDistance[nidx] + w);
    };

    const float DIAG = 1.41421356f;
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            relax(x, y, x - 1, y,     1.0f);
            relax(x, y, x,     y - 1, 1.0f);
            relax(x, y, x - 1, y - 1, DIAG);
            relax(x, y, x + 1, y - 1, DIAG);
        }
    }
    for (int y = n - 1; y >= 0; --y) {
        for (int x = n - 1; x >= 0; --x) {
            relax(x, y, x + 1, y,     1.0f);
            relax(x, y, x,     y + 1, 1.0f);
            relax(x, y, x + 1, y + 1, DIAG);
            relax(x, y, x - 1, y + 1, DIAG);
        }
    }

    std::vector<float> shoreData((size_t)n * n * 4, 0.0f);
    for (int y = 0; y < shoreDataResolution; ++y) {
        for (int x = 0; x < shoreDataResolution; ++x) {
            int src = y * shoreDataResolution + x;
            int dst = src * 4;
            bool valid = collisionValid[src] != 0;
            shoreData[dst + 0] = valid ? collisionHeights[src] : collisionNoTerrain;
            shoreData[dst + 1] = valid ? 1.0f : 0.0f;
            shoreData[dst + 2] = std::min(shoreDistance[src] * cellWorld, 9999.0f);
            shoreData[dst + 3] = landMask[src];
        }
    }

    glGenTextures(1, &shoreDataTextureID);
    glBindTexture(GL_TEXTURE_2D, shoreDataTextureID);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F,
                 shoreDataResolution, shoreDataResolution, 0,
                 GL_RGBA, GL_FLOAT, shoreData.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    std::cout << "Shoreline data texture: " << shoreDataResolution << "x"
              << shoreDataResolution << " RGBA32F\n";
}

void Scene::createSeabed(float size, float depth) {
    // Simple subdivided plane at given depth
    int res = 20;
    std::vector<float> vertices;
    std::vector<unsigned int> indices;
    for (int i = 0; i <= res; i++) {
        for (int j = 0; j <= res; j++) {
            float x = (float)j / res * size - size / 2.0f;
            float z = (float)i / res * size - size / 2.0f;
            vertices.push_back(x); vertices.push_back(depth); vertices.push_back(z);
            vertices.push_back((float)j / res); vertices.push_back((float)i / res);
        }
    }
    for (int i = 0; i < res; i++) {
        for (int j = 0; j < res; j++) {
            int row1 = i * (res + 1);
            int row2 = (i + 1) * (res + 1);
            indices.push_back(row1 + j); indices.push_back(row2 + j); indices.push_back(row1 + j + 1);
            indices.push_back(row1 + j + 1); indices.push_back(row2 + j); indices.push_back(row2 + j + 1);
        }
    }
    seabedIndicesCount = (int)indices.size();
    glGenVertexArrays(1, &seabedVAO);
    glGenBuffers(1, &seabedVBO);
    glGenBuffers(1, &seabedEBO);
    glBindVertexArray(seabedVAO);
    glBindBuffer(GL_ARRAY_BUFFER, seabedVBO);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), vertices.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, seabedEBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
}

void Scene::createTerrainFromHeightmap() {
    // Load heightmap (biome masks disabled — filled with zeros)
    heightmapData = HeightmapLoader::Load(
        "assets/textures/world/T_World_Heightmap.png",
        "assets/textures/world/M_Castle_Depth_Mask.png",
        "assets/textures/world/M_Lava_Depth_Mask.png",
        "assets/textures/world/M_River_Depth_Mask.png"
    );

    if (heightmapData.width == 0 || heightmapData.height == 0) {
        std::cout << "ERROR: Failed to load heightmap, terrain not created." << std::endl;
        terrainIndicesCount = 0;
        return;
    }

    // Grid matches the heightmap source resolution (~1024) — upsampling to
    // 1600 added 2.4x the triangles WITHOUT any new detail (the source is
    // 1024), and terrain is geometry/draw-call bound. 1024 → ~2.1M tris and
    // ~64 chunks (vs 169) for a big FPS win at no real detail loss. Shader
    // fbm/normal-mapping still supplies the fine relief.
    int srcW = heightmapData.width;
    int srcH = heightmapData.height;
    int gridW = 1024;
    int gridH = 1024;

    std::cout << "Terrain grid: " << gridW << " x " << gridH
              << " (source " << srcW << "x" << srcH << ", bilinear sampled)" << std::endl;

    float halfSize = terrainSize / 2.0f;

    // Vertex layout: position(3) + texcoord(2) + normal(3) + biome(3) = 11 floats
    int stride = 11;
    std::vector<float> vertices(gridW * gridH * stride);

    // Track which grid cells are valid terrain. Use char (not
    // vector<bool>) so parallel threads can safely write distinct rows
    // without bit-packing data races.
    std::vector<char> validGrid(gridW * gridH, 0);

    const float NO_TERRAIN_THRESHOLD = -90000.0f;

    auto sampleHeight = [&](float u, float v) -> std::pair<float, bool> {
        // Catmull-Rom bicubic sample with NO_TERRAIN handling.
        // If any of the 16 neighbours is missing the pixel falls back
        // to bilinear; if the bilinear neighbours are missing it's a hole.
        float fx = u * (srcW - 1);
        float fy = v * (srcH - 1);
        int ix = (int)std::floor(fx);
        int iy = (int)std::floor(fy);
        float tx = fx - ix;
        float ty = fy - iy;

        auto fetch = [&](int x, int y) -> float {
            x = std::max(0, std::min(x, srcW - 1));
            y = std::max(0, std::min(y, srcH - 1));
            return heightmapData.heights[y * srcW + x];
        };

        // Check the 4 immediate neighbours for NO_TERRAIN — if any is
        // missing, the cell is a hole.
        float h00 = fetch(ix, iy);
        float h10 = fetch(ix + 1, iy);
        float h01 = fetch(ix, iy + 1);
        float h11 = fetch(ix + 1, iy + 1);
        if (h00 < NO_TERRAIN_THRESHOLD || h10 < NO_TERRAIN_THRESHOLD ||
            h01 < NO_TERRAIN_THRESHOLD || h11 < NO_TERRAIN_THRESHOLD) {
            return { -999.0f, false };
        }

        // Catmull-Rom in 1D
        auto cr = [](float a, float b, float c, float d, float t) {
            float t2 = t * t;
            float t3 = t2 * t;
            return 0.5f * (
                (2.0f * b) +
                (-a + c) * t +
                (2.0f*a - 5.0f*b + 4.0f*c - d) * t2 +
                (-a + 3.0f*b - 3.0f*c + d) * t3);
        };

        // Sample 16 points; replace any NO_TERRAIN with the closest valid
        // neighbour (h00..h11) so the spline doesn't blow up at the edges.
        float p[4][4];
        for (int j = -1; j <= 2; ++j) {
            for (int i = -1; i <= 2; ++i) {
                float h = fetch(ix + i, iy + j);
                if (h < NO_TERRAIN_THRESHOLD) {
                    // Replace with bilinear average of the 4 valid corners
                    h = (h00 + h10 + h01 + h11) * 0.25f;
                }
                p[j + 1][i + 1] = h;
            }
        }

        float row0 = cr(p[0][0], p[0][1], p[0][2], p[0][3], tx);
        float row1 = cr(p[1][0], p[1][1], p[1][2], p[1][3], tx);
        float row2 = cr(p[2][0], p[2][1], p[2][2], p[2][3], tx);
        float row3 = cr(p[3][0], p[3][1], p[3][2], p[3][3], tx);
        float h = cr(row0, row1, row2, row3, ty);

        return { h, true };
    };

    // First pass: positions, UVs — parallelised across CPU cores.
    // Each thread owns a disjoint band of rows writing into the
    // pre-sized `vertices`/`validGrid` buffers, so there are no data
    // races and no locking needed.
    auto firstPassRows = [&](int gyStart, int gyEnd) {
    for (int gy = gyStart; gy < gyEnd; gy++) {
        for (int gx = 0; gx < gridW; gx++) {
            float u = (float)gx / (gridW - 1);
            float v = (float)gy / (gridH - 1);

            float x = u * terrainSize - halfSize;
            float z = v * terrainSize - halfSize;

            auto [rawH, isValid] = sampleHeight(u, v);
            validGrid[gy * gridW + gx] = isValid;

            float y = isValid ? (rawH * heightScale) : -999.0f;

            // Add procedural detail to break up smooth bilinear slopes —
            // multi-octave noise driven by world position. This creates
            // real geometric roughness, not just shading.
            if (isValid) {
                auto hash21 = [](float px, float py) {
                    float s = std::sin(px * 127.1f + py * 311.7f) * 43758.5453f;
                    return s - std::floor(s);
                };
                auto vnoise2 = [&](float px, float py) {
                    float ix = std::floor(px), iy = std::floor(py);
                    float fx = px - ix,        fy = py - iy;
                    float a = hash21(ix,       iy);
                    float b = hash21(ix + 1.0f, iy);
                    float c = hash21(ix,       iy + 1.0f);
                    float d = hash21(ix + 1.0f, iy + 1.0f);
                    float ux = fx * fx * (3.0f - 2.0f * fx);
                    float uy = fy * fy * (3.0f - 2.0f * fy);
                    return (a * (1 - ux) + b * ux) * (1 - uy)
                         + (c * (1 - ux) + d * ux) * uy;
                };
                float detail = 0.0f;
                float amp = 1.0f;
                float freq = 0.06f;
                for (int o = 0; o < 4; ++o) {
                    detail += amp * (vnoise2(x * freq, z * freq) - 0.5f);
                    amp *= 0.5f;
                    freq *= 2.05f;
                }
                // Stronger detail on deeper/steeper terrain
                float depthFactor = glm::clamp(-y / 30.0f, 0.0f, 1.5f);
                y += detail * 1.6f * (0.5f + depthFactor * 0.7f);
            }

            int base = (gy * gridW + gx) * stride;
            vertices[base + 0] = x;
            vertices[base + 1] = y;
            vertices[base + 2] = z;
            vertices[base + 3] = u;
            vertices[base + 4] = v;
            vertices[base + 5] = 0.0f;
            vertices[base + 6] = 1.0f;
            vertices[base + 7] = 0.0f;
            vertices[base + 8]  = 0.0f; // biomes disabled
            vertices[base + 9]  = 0.0f;
            vertices[base + 10] = 0.0f;
        }
    }
    };
    {
        unsigned hw = std::max(1u, std::thread::hardware_concurrency());
        unsigned nThreads = std::min(hw, 8u);
        std::vector<std::thread> pool;
        int rowsPer = (gridH + nThreads - 1) / nThreads;
        for (unsigned ti = 0; ti < nThreads; ++ti) {
            int s = ti * rowsPer;
            int e = std::min(gridH, s + rowsPer);
            if (s >= e) break;
            pool.emplace_back(firstPassRows, s, e);
        }
        for (auto& th : pool) th.join();
    }

    // Second pass: normals via 3x3 Sobel kernel — 2x smoother than
    // central differences. The Sobel weights (1, 2, 1) along the
    // perpendicular axis act as a built-in low-pass filter, killing
    // the visible faceting on steep slopes without needing more mesh.
    auto sobelRows = [&](int gyStart, int gyEnd) {
    for (int gy = gyStart; gy < gyEnd; gy++) {
        for (int gx = 0; gx < gridW; gx++) {
            if (!validGrid[gy * gridW + gx]) continue;

            // Fetch the 3x3 neighborhood, falling back to the centre
            // height where the neighbour is invalid.
            float yc = vertices[(gy * gridW + gx) * stride + 1];
            auto getY = [&](int cx, int cy) -> float {
                cx = std::max(0, std::min(cx, gridW - 1));
                cy = std::max(0, std::min(cy, gridH - 1));
                if (!validGrid[cy * gridW + cx]) return yc;
                return vertices[(cy * gridW + cx) * stride + 1];
            };

            float h00 = getY(gx - 1, gy - 1);
            float h10 = getY(gx,     gy - 1);
            float h20 = getY(gx + 1, gy - 1);
            float h01 = getY(gx - 1, gy);
            float h21 = getY(gx + 1, gy);
            float h02 = getY(gx - 1, gy + 1);
            float h12 = getY(gx,     gy + 1);
            float h22 = getY(gx + 1, gy + 1);

            // Sobel X: detects east-west slope (∂h/∂x)
            float gxh = (h20 + 2.0f * h21 + h22)
                      - (h00 + 2.0f * h01 + h02);
            // Sobel Z: detects north-south slope (∂h/∂z)
            float gzh = (h02 + 2.0f * h12 + h22)
                      - (h00 + 2.0f * h10 + h20);

            // Normalisation factor: Sobel sums 8 samples each side,
            // so divide by 8 cell widths.
            float cellSize = terrainSize / (float)(gridW - 1);
            glm::vec3 normal = glm::normalize(glm::vec3(
                -gxh / (8.0f * cellSize),
                 1.0f,
                -gzh / (8.0f * cellSize)
            ));

            int base = (gy * gridW + gx) * stride;
            vertices[base + 5] = normal.x;
            vertices[base + 6] = normal.y;
            vertices[base + 7] = normal.z;
        }
    }
    };
    {
        unsigned hw = std::max(1u, std::thread::hardware_concurrency());
        unsigned nThreads = std::min(hw, 8u);
        std::vector<std::thread> pool;
        int rowsPer = (gridH + nThreads - 1) / nThreads;
        for (unsigned ti = 0; ti < nThreads; ++ti) {
            int s = ti * rowsPer;
            int e = std::min(gridH, s + rowsPer);
            if (s >= e) break;
            pool.emplace_back(sobelRows, s, e);
        }
        for (auto& th : pool) th.join();
    }

    // Build index buffer — CHUNKED for frustum culling. Instead of one
    // flat row-major index list, we group triangles into spatial tiles
    // (chunks). Each chunk records its AABB and its contiguous slice of
    // the shared index buffer, so at draw time we submit only the chunks
    // inside the camera frustum. Vertex data is untouched → identical look.
    const int CHUNK = 128;   // grid cells per chunk side (1024/128 = 8 chunks across, 64 total)
    std::vector<unsigned int> indices;
    indices.reserve((gridW - 1) * (gridH - 1) * 6);
    terrainChunks.clear();

    for (int cy0 = 0; cy0 < gridH - 1; cy0 += CHUNK) {
        for (int cx0 = 0; cx0 < gridW - 1; cx0 += CHUNK) {
            int cy1 = std::min(cy0 + CHUNK, gridH - 1);
            int cx1 = std::min(cx0 + CHUNK, gridW - 1);

            size_t startIdx = indices.size();
            float minX = 1e30f, minY = 1e30f, minZ = 1e30f;
            float maxX = -1e30f, maxY = -1e30f, maxZ = -1e30f;

            for (int gy = cy0; gy < cy1; gy++) {
                for (int gx = cx0; gx < cx1; gx++) {
                    unsigned int i00 = gy * gridW + gx;
                    unsigned int i10 = gy * gridW + gx + 1;
                    unsigned int i01 = (gy + 1) * gridW + gx;
                    unsigned int i11 = (gy + 1) * gridW + gx + 1;

                    bool v00 = validGrid[i00];
                    bool v10 = validGrid[i10];
                    bool v01 = validGrid[i01];
                    bool v11 = validGrid[i11];

                    auto accumAABB = [&](unsigned int idx) {
                        float vx = vertices[idx * stride + 0];
                        float vy = vertices[idx * stride + 1];
                        float vz = vertices[idx * stride + 2];
                        minX = std::min(minX, vx); maxX = std::max(maxX, vx);
                        minY = std::min(minY, vy); maxY = std::max(maxY, vy);
                        minZ = std::min(minZ, vz); maxZ = std::max(maxZ, vz);
                    };

                    if (v00 && v10 && v01) {
                        indices.push_back(i00);
                        indices.push_back(i10);
                        indices.push_back(i01);
                        accumAABB(i00); accumAABB(i10); accumAABB(i01);
                    }
                    if (v10 && v11 && v01) {
                        indices.push_back(i10);
                        indices.push_back(i11);
                        indices.push_back(i01);
                        accumAABB(i10); accumAABB(i11); accumAABB(i01);
                    }
                }
            }

            int count = (int)(indices.size() - startIdx);

            // Coarser LODs for this chunk: walk the grid at stride S and
            // emit quads over the sparse lattice (same vertices, fewer
            // triangles). Watertight within the chunk; tiny seams between
            // chunks of different LOD are hidden by the underwater fog.
            auto buildLOD = [&](int S, size_t& outOff, int& outCount) {
                size_t off = indices.size();
                for (int gy = cy0; gy < cy1; gy += S) {
                    int gy2 = std::min(gy + S, cy1);
                    for (int gx = cx0; gx < cx1; gx += S) {
                        int gx2 = std::min(gx + S, cx1);
                        unsigned int i00 = gy  * gridW + gx;
                        unsigned int i10 = gy  * gridW + gx2;
                        unsigned int i01 = gy2 * gridW + gx;
                        unsigned int i11 = gy2 * gridW + gx2;
                        bool v00 = validGrid[i00], v10 = validGrid[i10];
                        bool v01 = validGrid[i01], v11 = validGrid[i11];
                        if (v00 && v10 && v01) {
                            indices.push_back(i00); indices.push_back(i10); indices.push_back(i01);
                        }
                        if (v10 && v11 && v01) {
                            indices.push_back(i10); indices.push_back(i11); indices.push_back(i01);
                        }
                    }
                }
                outOff = off * sizeof(unsigned int);
                outCount = (int)(indices.size() - off);
            };

            if (count > 0) {
                TerrainChunk ch;
                ch.aabbMin = glm::vec3(minX, minY, minZ);
                ch.aabbMax = glm::vec3(maxX, maxY, maxZ);
                ch.indexCount  = count;
                ch.indexOffset = startIdx * sizeof(unsigned int);
                ch.lodOffset[0] = ch.indexOffset; ch.lodCount[0] = count;   // full
                buildLOD(2, ch.lodOffset[1], ch.lodCount[1]);               // 1/4 tris
                buildLOD(4, ch.lodOffset[2], ch.lodCount[2]);               // 1/16 tris
                terrainChunks.push_back(ch);
            }
        }
    }
    terrainIndicesCount = (int)indices.size();

    std::cout << "Terrain mesh: " << terrainIndicesCount / 3 << " triangles, "
              << gridW * gridH << " vertices, "
              << terrainChunks.size() << " chunks" << std::endl;

    // --- Build a downsampled collision height field --------------
    // We don't need the full 1024² grid for collision; a 384² field
    // gives ~4m resolution over the 1600u world, which is plenty to keep
    // the player from sinking through the seabed.
    {
        collisionRes = 384;
        collisionHeights.assign(collisionRes * collisionRes, collisionNoTerrain);
        collisionValid.assign(collisionRes * collisionRes, 0);
        for (int cy = 0; cy < collisionRes; ++cy) {
            for (int cx = 0; cx < collisionRes; ++cx) {
                // Map collision cell -> nearest terrain grid vertex.
                int gx = (int)((float)cx / (collisionRes - 1) * (gridW - 1));
                int gy = (int)((float)cy / (collisionRes - 1) * (gridH - 1));
                int gidx = gy * gridW + gx;
                if (validGrid[gidx]) {
                    collisionHeights[cy * collisionRes + cx] =
                        vertices[gidx * stride + 1];   // world-space Y
                    collisionValid[cy * collisionRes + cx] = 1;
                }
            }
        }
    }

    // Upload to GPU
    glGenVertexArrays(1, &terrainVAO);
    glGenBuffers(1, &terrainVBO);
    glGenBuffers(1, &terrainEBO);
    glBindVertexArray(terrainVAO);

    glBindBuffer(GL_ARRAY_BUFFER, terrainVBO);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), vertices.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, terrainEBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, stride * sizeof(float), (void*)(5 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, stride * sizeof(float), (void*)(8 * sizeof(float)));
    glEnableVertexAttribArray(3);
    glBindVertexArray(0);

    // NOTE: heightmap CPU-side data is freed at the end of Init(),
    // after the terrain mesh has consumed it.
}

// Bilinearly sample the downsampled collision height field at a world
// (x,z) position. Returns a very low sentinel where there is no terrain
// (holes / outside the map) so the caller can treat it as "no floor".
// ----------------------------------------------------------------------
float Scene::SampleTerrainHeight(float worldX, float worldZ) const {
    if (collisionRes <= 0) return collisionNoTerrain;

    float half = terrainSize * 0.5f;
    float u = (worldX + half) / terrainSize;   // 0..1
    float v = (worldZ + half) / terrainSize;
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
        return collisionNoTerrain;

    float fx = u * (collisionRes - 1);
    float fz = v * (collisionRes - 1);
    int x0 = (int)fx, z0 = (int)fz;
    int x1 = std::min(x0 + 1, collisionRes - 1);
    int z1 = std::min(z0 + 1, collisionRes - 1);
    float tx = fx - x0, tz = fz - z0;

    auto cell = [&](int cx, int cz, float& outH) -> bool {
        int idx = cz * collisionRes + cx;
        if (!collisionValid[idx]) return false;
        outH = collisionHeights[idx];
        return true;
    };

    float h00, h10, h01, h11;
    bool v00 = cell(x0, z0, h00);
    bool v10 = cell(x1, z0, h10);
    bool v01 = cell(x0, z1, h01);
    bool v11 = cell(x1, z1, h11);

    // If any corner is missing, fall back to the max of the valid ones
    // (conservative: keeps the player above the highest nearby ground).
    if (!(v00 && v10 && v01 && v11)) {
        float best = collisionNoTerrain;
        if (v00) best = std::max(best, h00);
        if (v10) best = std::max(best, h10);
        if (v01) best = std::max(best, h01);
        if (v11) best = std::max(best, h11);
        return best;
    }

    float h0 = h00 * (1.0f - tx) + h10 * tx;
    float h1 = h01 * (1.0f - tx) + h11 * tx;
    return h0 * (1.0f - tz) + h1 * tz;
}
