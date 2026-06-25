#pragma once
// ======================================================================
// ЗАЩИТА (метод B07: Heightmap-based seabed mesh)  — 15 баллов
//   СУТЬ:   читаю картинку карты высот (stb_image), декодирую цвет
//           пикселя -> высоту, и генерирую меш дна: вершины (x, h, z),
//           НОРМАЛИ (по разнице высот соседей), UV-координаты.
//   ГДЕ:    рельефное дно — холмы, впадины, склоны (а не плоскость).
//   СЛОВА:  heightmap -> mesh, decode height, нормали по соседям, UV.
//   (Само построение меша/чанков — в Scene_Terrain.cpp.)
// ======================================================================
#include <vector>
#include <string>
#include <glm/glm.hpp>

struct HeightmapData {
    std::vector<float> heights;      // decoded heights per pixel
    std::vector<float> castleMask;   // 0..1 castle biome intensity
    std::vector<float> lavaMask;     // 0..1 lava biome intensity
    std::vector<float> riverMask;    // 0..1 river biome intensity
    int width, height;
    float minHeight, maxHeight;
};

class HeightmapLoader {
public:
    // zmapGradient key: depth -> RGB color
    struct GradientKey {
        float depth;
        float r, g, b; // normalized 0..1
    };

    // Load heightmap and all depth masks, decode zmapGradient colors to heights
    static HeightmapData Load(
        const std::string& heightmapPath,
        const std::string& castleMaskPath,
        const std::string& lavaMaskPath,
        const std::string& riverMaskPath
    );

private:
    // Load a depth mask image and produce a 0..1 mask
    // Non-white pixels = mask active, white = no mask
    static std::vector<float> loadMask(const std::string& path, int expectedW, int expectedH);
};
