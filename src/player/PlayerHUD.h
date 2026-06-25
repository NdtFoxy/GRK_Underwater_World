#pragma once
#include "../../external/imgui/imgui.h"
#include "PlayerController.h"

// ----------------------------------------------------------------------
// PlayerHUD
//
// Minimal heads-up display drawn with ImGui's background draw list:
//   * a subtle centre crosshair dot
//   * an "[ ADMIN / NOCLIP ]" tag while in Admin mode
// ----------------------------------------------------------------------
namespace PlayerHUD {

inline void draw(const PlayerController& player, int screenW, int screenH) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    float W = (float)screenW, H = (float)screenH;

    // Admin / noclip indicator.
    if (player.mode != PlayerController::Mode::Player) {
        dl->AddText(ImVec2(14.0f, H - 26.0f),
                    IM_COL32(255, 210, 80, 220), "[ ADMIN / NOCLIP ]");
    }

    // Subtle crosshair dot.
    dl->AddCircleFilled(ImVec2(W * 0.5f, H * 0.5f), 2.0f,
                        IM_COL32(255, 255, 255, 80));
}

} // namespace PlayerHUD
