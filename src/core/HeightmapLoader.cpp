#include "HeightmapLoader.h"
#include "../../external/stb_image.h"
#include <iostream>
#include <cmath>
#include <algorithm>
#include <limits>

// ============================================================
// zmapGradient — piecewise-linear path in RGB space encoding
// height. Highest depth (above water) first, deepest last.
// ============================================================
static std::vector<HeightmapLoader::GradientKey> buildGradientTable() {
    std::vector<HeightmapLoader::GradientKey> g;
    g.push_back({   160.0f, 196.0f/255.0f, 196.0f/255.0f, 255.0f/255.0f });
    g.push_back({     1.0f, 160.0f/255.0f, 128.0f/255.0f,  16.0f/255.0f });
    g.push_back({    -1.0f,   0.0f/255.0f, 255.0f/255.0f,   0.0f/255.0f });
    g.push_back({  -100.0f, 128.0f/255.0f,   0.0f/255.0f,   0.0f/255.0f });
    g.push_back({  -200.0f,   0.0f/255.0f,   0.0f/255.0f,   0.0f/255.0f });
    g.push_back({  -300.0f,   0.0f/255.0f,   0.0f/255.0f, 255.0f/255.0f });
    g.push_back({  -400.0f, 128.0f/255.0f,  64.0f/255.0f,   0.0f/255.0f });
    g.push_back({  -500.0f, 255.0f/255.0f, 255.0f/255.0f, 255.0f/255.0f });
    g.push_back({ -3040.0f,   0.0f/255.0f,   0.0f/255.0f,   0.0f/255.0f });
    return g;
}

static inline float colorDist2(float r1, float g1, float b1, float r2, float g2, float b2) {
    float dr = r1 - r2, dg = g1 - g2, db = b1 - b2;
    return dr*dr + dg*dg + db*db;
}

// ------------------------------------------------------------
// Robust decoder: precompute many discrete samples along the
// piecewise gradient and pick the absolute closest one.
// This is more stable than per-segment projection because
// segments cross each other in RGB space (white appears twice,
// black appears twice — the sample-based lookup naturally
// handles ambiguity by picking nearest, while the linear path
// preserves smooth transitions between keys.
// ------------------------------------------------------------
struct GradSample {
    float r, g, b;
    float depth;
};

static std::vector<GradSample> buildGradientSamples(const std::vector<HeightmapLoader::GradientKey>& g, int samplesPerSeg = 64) {
    std::vector<GradSample> s;
    s.reserve((g.size() - 1) * samplesPerSeg + 1);
    for (size_t i = 0; i + 1 < g.size(); ++i) {
        const auto& a = g[i];
        const auto& b = g[i + 1];
        for (int k = 0; k < samplesPerSeg; ++k) {
            float t = (float)k / (float)samplesPerSeg;
            GradSample p;
            p.r = a.r + t * (b.r - a.r);
            p.g = a.g + t * (b.g - a.g);
            p.b = a.b + t * (b.b - a.b);
            p.depth = a.depth + t * (b.depth - a.depth);
            s.push_back(p);
        }
    }
    s.push_back({ g.back().r, g.back().g, g.back().b, g.back().depth });
    return s;
}

// ------------------------------------------------------------
// Box-blur the heights map in place (3x3 kernel), respecting
// NO_TERRAIN sentinels (treated as "no contribution").
// ------------------------------------------------------------
static void boxBlurHeights(std::vector<float>& heights, int w, int h, float NO_TERRAIN, int passes) {
    std::vector<float> tmp(heights.size());
    for (int p = 0; p < passes; ++p) {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                int idx = y * w + x;
                if (heights[idx] <= NO_TERRAIN + 1.0f) {
                    tmp[idx] = heights[idx];
                    continue;
                }
                float sum = 0.0f;
                int cnt = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    int yy = y + dy;
                    if (yy < 0 || yy >= h) continue;
                    for (int dx = -1; dx <= 1; ++dx) {
                        int xx = x + dx;
                        if (xx < 0 || xx >= w) continue;
                        float v = heights[yy * w + xx];
                        if (v <= NO_TERRAIN + 1.0f) continue;
                        sum += v;
                        ++cnt;
                    }
                }
                tmp[idx] = (cnt > 0) ? (sum / (float)cnt) : heights[idx];
            }
        }
        std::swap(heights, tmp);
    }
}

// ------------------------------------------------------------
// 3x3 median filter — kills isolated decoder spikes while
// preserving genuine slopes.
// ------------------------------------------------------------
static void medianFilterHeights(std::vector<float>& heights, int w, int h, float NO_TERRAIN, int passes) {
    std::vector<float> tmp(heights.size());
    for (int p = 0; p < passes; ++p) {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                int idx = y * w + x;
                if (heights[idx] <= NO_TERRAIN + 1.0f) {
                    tmp[idx] = heights[idx];
                    continue;
                }
                float buf[9];
                int cnt = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    int yy = y + dy;
                    if (yy < 0 || yy >= h) continue;
                    for (int dx = -1; dx <= 1; ++dx) {
                        int xx = x + dx;
                        if (xx < 0 || xx >= w) continue;
                        float v = heights[yy * w + xx];
                        if (v <= NO_TERRAIN + 1.0f) continue;
                        buf[cnt++] = v;
                    }
                }
                if (cnt == 0) { tmp[idx] = heights[idx]; continue; }
                std::sort(buf, buf + cnt);
                tmp[idx] = buf[cnt / 2];
            }
        }
        std::swap(heights, tmp);
    }
}

// ------------------------------------------------------------
// Main loader
// ------------------------------------------------------------
HeightmapData HeightmapLoader::Load(
    const std::string& heightmapPath,
    const std::string& castleMaskPath,
    const std::string& lavaMaskPath,
    const std::string& riverMaskPath
) {
    HeightmapData result;
    auto gradient = buildGradientTable();
    auto samples = buildGradientSamples(gradient, 96);

    int w, h, channels;
    stbi_set_flip_vertically_on_load(false);
    unsigned char* data = stbi_load(heightmapPath.c_str(), &w, &h, &channels, 4);
    if (!data) {
        std::cout << "ERROR: Could not load heightmap: " << heightmapPath << std::endl;
        result.width = 0;
        result.height = 0;
        result.minHeight = 0;
        result.maxHeight = 0;
        return result;
    }

    std::cout << "Heightmap loaded: " << w << " x " << h << " (channels=" << channels << ")" << std::endl;

    result.width = w;
    result.height = h;
    result.heights.resize(w * h);

    const float NO_TERRAIN = -99999.0f;
    // Clamp the decoded depth to a useful range. We now allow terrain to
    // rise above the waterline (Y=0) so islands can poke out of the sea
    // with beaches (Req 3.1). ISLAND_MAX caps island height so the rare
    // misclassified bright-blue pixel can't spike into a needle.
    const float ISLAND_MAX =  60.0f;  // up to +60 world units above water
    const float DEPTH_MAX = ISLAND_MAX;
    const float DEPTH_MIN = -300.0f;  // hard floor

    int validPixels = 0;
    int emptyPixels = 0;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int idx = (y * w + x) * 4;
            float r = data[idx + 0] / 255.0f;
            float g = data[idx + 1] / 255.0f;
            float b = data[idx + 2] / 255.0f;
            float a = data[idx + 3] / 255.0f;

            // Background detection: alpha-based first, then pure black fallback
            bool isBackground = (a < 0.5f);
            if (!isBackground && channels <= 3) {
                isBackground = (r < 0.005f && g < 0.005f && b < 0.005f);
            }

            if (isBackground) {
                result.heights[y * w + x] = NO_TERRAIN;
                ++emptyPixels;
                continue;
            }

            // Find closest gradient sample
            float bestD2 = std::numeric_limits<float>::max();
            float bestDepth = 0.0f;
            for (const auto& s : samples) {
                float d2 = colorDist2(r, g, b, s.r, s.g, s.b);
                if (d2 < bestD2) {
                    bestD2 = d2;
                    bestDepth = s.depth;
                }
            }

            // Clamp into useful range
            if (bestDepth > DEPTH_MAX) bestDepth = DEPTH_MAX;
            if (bestDepth < DEPTH_MIN) bestDepth = DEPTH_MIN;

            result.heights[y * w + x] = bestDepth;
            ++validPixels;
        }
    }

    stbi_image_free(data);

    // 1) Median filter kills isolated outliers (the spiky pixels
    //    coming from misclassified colors near gradient boundaries)
    medianFilterHeights(result.heights, w, h, NO_TERRAIN, 3);
    // 2) Then a few box blur passes give us soft, sculptable terrain
    boxBlurHeights(result.heights, w, h, NO_TERRAIN, 4);

    // Compute final min/max
    result.minHeight =  std::numeric_limits<float>::max();
    result.maxHeight = -std::numeric_limits<float>::max();
    for (float v : result.heights) {
        if (v <= NO_TERRAIN + 1.0f) continue;
        if (v < result.minHeight) result.minHeight = v;
        if (v > result.maxHeight) result.maxHeight = v;
    }

    std::cout << "=== Heightmap Decoded ===" << std::endl;
    std::cout << "  Valid pixels: " << validPixels << std::endl;
    std::cout << "  Empty pixels: " << emptyPixels << std::endl;
    std::cout << "  Height range (after clamp+smooth): "
              << result.minHeight << " .. " << result.maxHeight << std::endl;

     // Biome masks: leave empty for now (the rest of the pipeline ignores them)
    result.castleMask.resize(w * h, 0.0f);
    result.lavaMask.resize(w * h, 0.0f);
    result.riverMask.resize(w * h, 0.0f);

    return result;
}

std::vector<float> HeightmapLoader::loadMask(const std::string& path, int expectedW, int expectedH) {
    return std::vector<float>(expectedW * expectedH, 0.0f);
}
