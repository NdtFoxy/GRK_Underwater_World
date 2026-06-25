#pragma once
#include "../../external/imgui/imgui.h"
#include "OxygenSystem.h"
#include "PlayerController.h"
#include <cmath>
#include <vector>

// ----------------------------------------------------------------------
// PlayerHUD
//
// Minimalist, "liquid glass" oxygen orb drawn with ImGui's background
// draw list — no textures required. The orb is a frosted glass circle
// with:
//   * a soft outer glow
//   * a translucent glass body with a top specular highlight
//   * an animated liquid level inside (sloshing sine surface) whose
//     height = remaining oxygen
//   * a thin progress ring around the rim
//
// Plus the Subnautica-style fade-to-black death overlay.
// ----------------------------------------------------------------------
namespace PlayerHUD {

inline float clamp01(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }

inline ImU32 lerpCol(ImU32 a, ImU32 b, float t) {
    ImVec4 ca = ImGui::ColorConvertU32ToFloat4(a);
    ImVec4 cb = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(
        ca.x + (cb.x - ca.x) * t,
        ca.y + (cb.y - ca.y) * t,
        ca.z + (cb.z - ca.z) * t,
        ca.w + (cb.w - ca.w) * t));
}

// Filled circle segment clip is awkward in ImGui, so we draw the liquid
// as a polygon: the circle's lower cap up to the wavy surface line.
inline void drawLiquid(ImDrawList* dl, ImVec2 c, float radius,
                       float fillFrac, float t, ImU32 colTop, ImU32 colBot) {
    fillFrac = clamp01(fillFrac);
    if (fillFrac <= 0.001f) return;

    // Surface Y in screen space (top of the liquid). y grows downward.
    float surfaceY = c.y + radius - 2.0f * radius * fillFrac;

    const int N = 48;
    std::vector<ImVec2> poly;
    poly.reserve(N + 8);

    // Top edge: wavy surface across the circle width at this height.
    for (int i = 0; i <= N; ++i) {
        float fx = (float)i / N;                 // 0..1 across diameter
        float x = c.x - radius + fx * 2.0f * radius;
        // Only within the circle: clamp the wave segment to the chord.
        float dx = x - c.x;
        float halfChord = radius * radius - (surfaceY - c.y) * (surfaceY - c.y);
        halfChord = halfChord > 0 ? sqrtf(halfChord) : 0.0f;
        if (dx < -halfChord) x = c.x - halfChord;
        if (dx >  halfChord) x = c.x + halfChord;

        float wave = sinf(fx * 6.2831f * 1.5f + t * 2.0f) * 2.2f
                   + sinf(fx * 6.2831f * 3.0f - t * 1.3f) * 1.1f;
        poly.push_back(ImVec2(x, surfaceY + wave));
    }
    // Lower arc of the circle (left side down, around the bottom, up right).
    for (int i = N; i >= 0; --i) {
        float a = 3.14159f * ((float)i / N);     // 0..PI
        float x = c.x - cosf(a) * radius;        // left → right
        float y = c.y + sinf(a) * radius;        // bottom half
        // Only keep points below the surface.
        if (y >= surfaceY - 4.0f)
            poly.push_back(ImVec2(x, y));
    }

    if (poly.size() >= 3)
        dl->AddConvexPolyFilled(poly.data(), (int)poly.size(), colBot);
}

inline void draw(const OxygenSystem& oxy,
                 const PlayerController& player,
                 int screenW, int screenH) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    float W = (float)screenW, H = (float)screenH;
    float o2 = oxy.oxygenFraction();
    float t  = (float)ImGui::GetTime();

    bool isPlayer = (player.mode == PlayerController::Mode::Player);

    if (isPlayer) {
        // ---- liquid-glass oxygen orb (bottom-right corner) ----
        float radius = 46.0f;
        ImVec2 c(W - radius - 34.0f, H - radius - 34.0f);

        // Colours: cyan when full, warm red when low.
        ImU32 liqHi = IM_COL32(90, 220, 255, 200);
        ImU32 liqLo = IM_COL32(255, 95, 70, 210);
        ImU32 liquid = lerpCol(liqLo, liqHi, clamp01((o2 - 0.15f) / 0.5f));

        // Low-air gentle pulse on the orb (not the whole screen).
        float pulse = (o2 < 0.3f)
            ? (0.5f + 0.5f * sinf(t * 5.0f)) * (1.0f - o2 / 0.3f)
            : 0.0f;

        // 1) Outer soft glow — concentric translucent rings.
        for (int i = 6; i >= 1; --i) {
            float rr = radius + i * 3.0f;
            int alpha = (int)(10 + pulse * 25);
            dl->AddCircleFilled(c, rr, IM_COL32(120, 210, 255, alpha / i), 48);
        }

        // 2) Glass body — dark translucent base.
        dl->AddCircleFilled(c, radius, IM_COL32(12, 26, 36, 150), 64);

        // 3) Animated liquid inside.
        drawLiquid(dl, c, radius - 3.0f, o2, t, liquid, liquid);

        // 4) Pulse-brighten the liquid when low.
        if (pulse > 0.0f) {
            dl->AddCircleFilled(c, (radius - 3.0f) * o2 * 0.5f,
                                IM_COL32(255, 255, 255, (int)(pulse * 40)), 32);
        }

        // 5) Glass rim + progress ring.
        dl->AddCircle(c, radius, IM_COL32(180, 230, 255, 70), 64, 2.0f);
        // Progress arc (starts at bottom, clockwise).
        {
            const int seg = 64;
            int filled = (int)(seg * o2);
            float start = 3.14159265f * 0.5f;
            for (int i = 0; i < filled; ++i) {
                float a0 = start + (float)i       / seg * 6.2831853f;
                float a1 = start + (float)(i + 1) / seg * 6.2831853f;
                dl->AddLine(
                    ImVec2(c.x + cosf(a0) * radius, c.y + sinf(a0) * radius),
                    ImVec2(c.x + cosf(a1) * radius, c.y + sinf(a1) * radius),
                    liquid, 3.0f);
            }
        }

        // 6) Top specular highlight — the "glass" gleam.
        ImVec2 hl(c.x - radius * 0.35f, c.y - radius * 0.40f);
        dl->AddCircleFilled(hl, radius * 0.22f, IM_COL32(255, 255, 255, 45), 24);
        dl->AddCircleFilled(ImVec2(hl.x + 2, hl.y + 2), radius * 0.10f,
                            IM_COL32(255, 255, 255, 70), 16);

        // 7) Centred seconds-of-air text.
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", (int)(oxy.oxygen + 0.5f));
        ImVec2 ts = ImGui::CalcTextSize(buf);
        dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f),
                    IM_COL32(255, 255, 255, 230), buf);

        // "breathe" hint when surfaced and refilling.
        if (player.headAboveWater && o2 < 0.99f) {
            const char* msg = "Breathing...";
            ImVec2 ms = ImGui::CalcTextSize(msg);
            dl->AddText(ImVec2(c.x - ms.x * 0.5f, c.y + radius + 6.0f),
                        IM_COL32(150, 230, 255, 200), msg);
        }
    } else {
        dl->AddText(ImVec2(14.0f, H - 26.0f),
                    IM_COL32(255, 210, 80, 220), "[ ADMIN / NOCLIP ]");
    }

    // ---- subtle crosshair dot ----
    dl->AddCircleFilled(ImVec2(W * 0.5f, H * 0.5f), 2.0f,
                        IM_COL32(255, 255, 255, 80));

    // ---- death fade-to-black ----
    if (player.deathFade > 0.001f) {
        int a = (int)(clamp01(player.deathFade) * 255.0f);
        dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), IM_COL32(0, 0, 0, a));
        if (player.deathFade > 0.6f) {
            int ta = (int)((player.deathFade - 0.6f) / 0.4f * 255.0f);
            const char* msg = "You ran out of oxygen";
            ImVec2 ms = ImGui::CalcTextSize(msg);
            dl->AddText(ImVec2(W * 0.5f - ms.x * 0.5f, H * 0.5f - ms.y * 0.5f),
                        IM_COL32(200, 220, 255, ta), msg);
        }
    }
}

} // namespace PlayerHUD
