// ======================================================================
//  КАРТА ЗАЩИТЫ — где какой оцениваемый метод (открывай этот файл и
//  показывай). Подробный "ЗАЩИТА"-баннер есть в начале каждого файла.
// ----------------------------------------------------------------------
//  ОБЯЗАТЕЛЬНЫЕ (30 б.):
//    Normal mapping ....... assets/shaders/object.frag + terrain.frag (TBN)
//    PBR lighting ......... assets/shaders/object.frag (metallic/roughness)
//    Quaternion camera .... src/scene/Camera.h
//    Shadow mapping ....... src/render/ShadowMap.h + computeShadow в шейдерах
//    Parallel Transport ... src/scene/SplinePath.h (морской змей)
//    Underwater cubemap ... src/render/Cubemap.h + skybox-пасс в Scene_Render.cpp
//  ВЫБРАННЫЕ:
//    A07 Instanced+LOD .... src/scene/VegetationSystem.cpp/.h  (30 б.)
//    B07 Heightmap-дно .... src/core/HeightmapLoader.* + Scene_Terrain.cpp (15 б.)
//  ИНТЕРАКЦИИ (см. processInput ниже):
//    F = фонарь,  K = шторм вкл/выкл,  Q = сонар-пинг,  + ImGui-панель.
// ======================================================================
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>
#include "scene/Scene.h"
#include "scene/Camera.h"
#include "player/PlayerController.h"
#include "player/PlayerHUD.h"

#ifdef _WIN32
#include <windows.h>
extern "C" {
    __declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

// ImGui headers
#include "../external/imgui/imgui.h"
#include "../external/imgui/backends/imgui_impl_glfw.h"
#include "../external/imgui/backends/imgui_impl_opengl3.h"

int screenWidth = 1000;
int screenHeight = 1000;
Scene* myScene = nullptr;

// Camera
Camera camera(glm::vec3(0.0f, 12.0f, 35.0f));
float lastX = screenWidth / 2.0f;
float lastY = screenHeight / 2.0f;
bool firstMouse = true;
bool rightMousePressed = false;

// Player (movement, gentle wave bob)
PlayerController player;

// Timing
float deltaTime = 0.0f;
float lastFrame = 0.0f;
float gFpsAvg = 0.0f;   // ground-truth FPS (averaged each second)

// Wave + cloud speeds (independent), controlled by ImGui
float waveSpeed = 1.0f;
float cloudSpeed = 1.0f;
// Overall wave height: 0 = perfectly flat calm, up to big stormy seas.
float waveAmp = 1.0f;

// Storm control: stormForced toggles a manual override; stormLevel is the
// forced intensity (0..1) used while it is on. Off = weather-driven (auto).
bool  stormForced = false;
float stormLevel  = 1.0f;
bool  lightningOn = true;   // sky-flash flicker on/off

// Time of day (0.0 to 24.0)
float timeOfDay = 12.0f;

void framebuffer_size_callback(GLFWwindow* window, int width, int height) {
    screenWidth = width;
    screenHeight = height;
    glViewport(0, 0, width, height);
    if (myScene != nullptr) {
        myScene->OnResize(width, height);
    }
}

void mouse_callback(GLFWwindow* window, double xposIn, double yposIn) {
    if (!rightMousePressed) return;

    float xpos = static_cast<float>(xposIn);
    float ypos = static_cast<float>(yposIn);

    if (firstMouse) {
        lastX = xpos;
        lastY = ypos;
        firstMouse = false;
    }

    float xoffset = xpos - lastX;
    float yoffset = lastY - ypos; // reversed since y-coordinates go from bottom to top

    lastX = xpos;
    lastY = ypos;

    camera.ProcessMouseMovement(xoffset, yoffset);
}

void mouse_button_callback(GLFWwindow* window, int button, int action, int mods) {
    if (button == GLFW_MOUSE_BUTTON_RIGHT) {
        if (action == GLFW_PRESS) {
            rightMousePressed = true;
            firstMouse = true; // reset to avoid jump
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        } else if (action == GLFW_RELEASE) {
            rightMousePressed = false;
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        }
    }
}

void processInput(GLFWwindow *window) {
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
        glfwSetWindowShouldClose(window, true);

    // Flashlight toggle on F (edge-triggered so a hold = one toggle).
    static bool fWasDown = false;
    bool fDown = glfwGetKey(window, GLFW_KEY_F) == GLFW_PRESS;
    if (fDown && !fWasDown && myScene) myScene->flashlight.toggle();
    fWasDown = fDown;

    // Storm toggle on K (edge-triggered: first press forces full storm,
    // second press releases back to weather-driven).
    static bool kWasDown = false;
    bool kDown = glfwGetKey(window, GLFW_KEY_K) == GLFW_PRESS;
    if (kDown && !kWasDown && myScene) {
        stormForced = !stormForced;
        myScene->SetStormOverride(stormForced ? stormLevel : -1.0f);
    }
    kWasDown = kDown;

    // Sonar ping on Q (edge-triggered): emit from the camera position.
    static bool qWasDown = false;
    bool qDown = glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS;
    if (qDown && !qWasDown && myScene) myScene->FireSonar(camera.Position);
    qWasDown = qDown;

    // Gather movement input; PlayerController applies the physics.
    PlayerController::Input in;
    in.forward  = glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS;
    in.backward = glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS;
    in.left     = glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS;
    in.right    = glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS;
    in.up       = glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS;
    in.down     = glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS;
    in.sprint   = (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) ||
                  (glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS);

    // Feed terrain collision height under the camera so the player
    // can't swim through the seabed.
    if (myScene) {
        player.terrainFloor =
            myScene->SampleTerrainHeight(camera.Position.x, camera.Position.z);

        // Underwater storm: the current bodily drags the diver along the
        // wind direction (quadratic in intensity so calm seas barely pull),
        // weakening with depth; the camera picks up a surge roll/heave.
        float storm = myScene->GetStormIntensity();
        float wd = glm::radians(myScene->GetWindDirDeg());
        glm::vec3 curDir(std::cos(wd), 0.0f, std::sin(wd));
        bool underwater = camera.Position.y < player.waterLevel;
        float depthFade = glm::clamp(
            1.0f - (player.waterLevel - camera.Position.y) / 45.0f, 0.25f, 1.0f);
        player.waterCurrent = underwater
            ? curDir * (storm * storm * 2.6f * depthFade)
            : glm::vec3(0.0f);
    }

    player.update(camera, in, deltaTime);
}

int main() {
    if (!glfwInit()) return -1;

    // OpenGL 4.3 core: superset of the lab's 3.3 — all existing #version 330 shaders
    // and 3.3 API keep working unchanged. The bump enables hardware tessellation
    // (GL 4.0+) used by the water surface (water_tess.*).
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    GLFWwindow* window = glfwCreateWindow(screenWidth, screenHeight, "Underworld Engine", NULL, NULL);
    if (!window) {
        glfwTerminate();
        return -1;
    }

    glfwMakeContextCurrent(window);
    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
    glfwSetCursorPosCallback(window, mouse_callback);
    glfwSetMouseButtonCallback(window, mouse_button_callback);

    if (glewInit() != GLEW_OK) return -1;

    // Report the context we actually got + whether compute shaders are available.
    printf("GL version: %s\n", glGetString(GL_VERSION));
    printf("GLSL version: %s\n", glGetString(GL_SHADING_LANGUAGE_VERSION));
    printf("Compute shaders supported: %s\n", GLEW_ARB_compute_shader ? "yes" : "no");
    fflush(stdout);

    // Setup ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    myScene = new Scene(screenWidth, screenHeight);
    myScene->Init();

    // Optional startup override of the 3D render scale (e.g. UW_SCALE=0.7).
    {
        char sbuf[16] = {0}; size_t sl = 0; getenv_s(&sl, sbuf, sizeof(sbuf), "UW_SCALE");
        if (sl > 0) { float v = (float)atof(sbuf); if (v > 0.05f) myScene->SetRenderScale(v); }
    }

    // Set initial camera orientation (quaternion-based).
    camera.SetEuler(-90.0f, -15.0f);

    // Player starts in Player mode; water surface is at Y=0.
    player.waterLevel = 0.0f;
    player.spawnPoint = glm::vec3(0.0f, -6.0f, 0.0f);
    camera.Position = player.spawnPoint;

    // IMPORTANT: query the REAL framebuffer size now. The requested
    // window size (1000x1000) is not necessarily what the OS gives us
    // (DPI scaling, maximized state, etc). Without this the scene
    // renders into a stale 1000-wide viewport inside a wider window —
    // the "black band on the right" bug.
    {
        int fbW = screenWidth, fbH = screenHeight;
        glfwGetFramebufferSize(window, &fbW, &fbH);
        if (fbW > 0 && fbH > 0) {
            screenWidth = fbW; screenHeight = fbH;
            glViewport(0, 0, fbW, fbH);
            myScene->OnResize(fbW, fbH);
        }
    }

    while (!glfwWindowShouldClose(window)) {
        // Keep our cached size in sync with the real framebuffer every
        // frame. Cheap, and bulletproofs against any missed resize
        // event (DPI change, snap, maximize) that would otherwise leave
        // a black band where the viewport doesn't cover the window.
        {
            int fbW = 0, fbH = 0;
            glfwGetFramebufferSize(window, &fbW, &fbH);
            if (fbW > 0 && fbH > 0 &&
                (fbW != screenWidth || fbH != screenHeight)) {
                screenWidth = fbW; screenHeight = fbH;
                glViewport(0, 0, fbW, fbH);
                if (myScene) myScene->OnResize(fbW, fbH);
            }
        }

        float currentFrame = static_cast<float>(glfwGetTime());
        deltaTime = currentFrame - lastFrame;
        lastFrame = currentFrame;

        // Real frame-time meter (no glFinish): ground-truth FPS, averaged
        // each second, shown in the panel and logged to stdout.
        {
            static float fpsAcc = 0.0f; static int fpsN = 0;
            fpsAcc += deltaTime; ++fpsN;
            if (fpsAcc >= 1.0f) {
                gFpsAvg = (float)fpsN / fpsAcc;
                printf("[fps] %.1f  (%.2f ms/frame)\n", gFpsAvg, 1000.0f * fpsAcc / fpsN);
                fflush(stdout);
                fpsAcc = 0.0f; fpsN = 0;
            }
        }

        processInput(window);

        // Start the ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // ===================== LEFT CONTROL PANEL =====================
        // Docked against the left edge, styled translucent-dark (Req 9.1).
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(330, (float)screenHeight), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.09f, 0.12f, 0.92f));
        ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0.10f, 0.28f, 0.40f, 1.0f));
        ImGui::Begin("Ocean Controls", nullptr,
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoCollapse);

        // --- Weather presets (Req 9.2, 5, 10.2) ---
        if (myScene) {
            ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "WEATHER");
            int wcount = myScene->GetWeatherCount();
            int widx = myScene->GetWeatherIndex();
            for (int i = 0; i < wcount; ++i) {
                if (i > 0) ImGui::SameLine();
                bool selected = (i == widx);
                if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.42f, 0.60f, 1.0f));
                if (ImGui::Button(myScene->GetWeatherName(i))) myScene->SelectWeather(i);
                if (selected) ImGui::PopStyleColor();
            }
            ImGui::Separator();
        }

        ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "OCEAN & SKY");
        ImGui::SliderFloat("Wave Height", &waveAmp, 0.0f, 2.5f, "%.2f");
        ImGui::SliderFloat("Wave Speed", &waveSpeed, 0.0f, 5.0f, "%.2f");
        ImGui::SliderFloat("Cloud Speed", &cloudSpeed, 0.0f, 5.0f, "%.2f");
        if (myScene) {
            // Storm: explicit on/off + intensity. Off = weather-driven (auto).
            if (ImGui::Checkbox("Storm", &stormForced))
                myScene->SetStormOverride(stormForced ? stormLevel : -1.0f);
            ImGui::SameLine();
            if (ImGui::Checkbox("Lightning", &lightningOn))
                myScene->SetLightningEnabled(lightningOn);
            ImGui::BeginDisabled(!stormForced);
            if (ImGui::SliderFloat("Storm Intensity", &stormLevel, 0.0f, 1.0f, "%.2f"))
                myScene->SetStormOverride(stormLevel);
            ImGui::EndDisabled();
        }
        ImGui::SliderFloat("Time of Day", &timeOfDay, 0.0f, 24.0f, "%.2f");
        if (myScene) {
            ImGui::ColorEdit3("Water Shallow", myScene->WeatherShallowPtr());
            ImGui::ColorEdit3("Water Deep", myScene->WeatherDeepPtr());
            ImGui::SliderFloat("Cloud Density", myScene->WeatherCloudDensityPtr(), 0.0f, 2.0f, "%.2f");
        }
        ImGui::Separator();

        // --- Points of interest: the sunken wreck + the shark ---
        if (myScene) {
            ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "POINTS OF INTEREST");
            if (myScene->HasWreck()) {
                glm::vec3 w = myScene->GetWreckPos();
                float dw = std::sqrt((w.x - camera.Position.x) * (w.x - camera.Position.x)
                                   + (w.z - camera.Position.z) * (w.z - camera.Position.z));
                ImGui::Text("Wreck (%5.0f, %4.0f, %5.0f)  %4.0f m", w.x, w.y, w.z, dw);
                ImGui::SameLine();
                if (ImGui::SmallButton("Go##wreck")) {
                    camera.Position = w + glm::vec3(0.0f, 10.0f, -14.0f);
                    player.velocity = glm::vec3(0.0f);
                }
            }
            {
                glm::vec3 s = myScene->GetSharkPos();
                float ds = std::sqrt((s.x - camera.Position.x) * (s.x - camera.Position.x)
                                   + (s.z - camera.Position.z) * (s.z - camera.Position.z));
                ImGui::Text("Shark (%5.0f, %4.0f, %5.0f)  %4.0f m", s.x, s.y, s.z, ds);
                ImGui::SameLine();
                if (ImGui::SmallButton("Go##shark")) {
                    camera.Position = s + glm::vec3(12.0f, 4.0f, 0.0f);
                    player.velocity = glm::vec3(0.0f);
                }
            }
            {
                glm::vec3 sp = myScene->GetSerpentPos();
                float dsp = std::sqrt((sp.x - camera.Position.x) * (sp.x - camera.Position.x)
                                    + (sp.z - camera.Position.z) * (sp.z - camera.Position.z));
                ImGui::Text("Serpent (%5.0f, %4.0f, %5.0f)  %4.0f m", sp.x, sp.y, sp.z, dsp);
                ImGui::SameLine();
                if (ImGui::SmallButton("Go##serpent")) {
                    camera.Position = sp + glm::vec3(10.0f, 4.0f, 0.0f);
                    player.velocity = glm::vec3(0.0f);
                }
            }
            ImGui::Separator();
        }

        ImGui::TextDisabled("RMB look | WASD move | Space/Ctrl | Shift boost | F light");
        ImGui::Separator();

        // --- Player / Admin mode toggle ---
        int modeIdx = (player.mode == PlayerController::Mode::Admin) ? 1 : 0;
        const char* modes[] = { "Player (swim)", "Admin (noclip)" };
        if (ImGui::Combo("Mode", &modeIdx, modes, 2)) {
            player.mode = (modeIdx == 1) ? PlayerController::Mode::Admin
                                         : PlayerController::Mode::Player;
        }
        ImGui::Text("FPS: %.0f  (%.1f ms)", gFpsAvg, gFpsAvg > 0.0f ? 1000.0f / gFpsAvg : 0.0f);
        if (myScene) {
            float rs = myScene->GetRenderScale();
            if (ImGui::SliderFloat("Render Scale", &rs, 0.5f, 1.0f, "%.2fx"))
                myScene->SetRenderScale(rs);
        }
        ImGui::SliderFloat("Player Speed", &player.playerMaxSpeed, 4.0f, 40.0f, "%.0f");
        if (ImGui::Button("Respawn")) player.respawn(camera);
        if (myScene) {
            ImGui::Checkbox("Flashlight (F)", &myScene->flashlight.enabled);
        }
        ImGui::Separator();
        ImGui::Text("FPS: %.1f  (frame %.2f ms)", io.Framerate, 1000.0f / io.Framerate);
        if (myScene) {
            const auto& cor = myScene->GetCoral();
            ImGui::Text("Coral total: %d", cor.totalInstances());
            ImGui::Text("  drawn LOD0/1/2: %d / %d / %d",
                        cor.drawnLOD0, cor.drawnLOD1, cor.drawnLOD2);
            int culled2 = cor.totalInstances() - cor.drawnLOD0 - cor.drawnLOD1 - cor.drawnLOD2;
            ImGui::Text("  pruned (stochastic): %d", culled2);
            ImGui::Text("Props LOD0/1/2: %d / %d / %d",
                        myScene->GetPropDrawnLOD0(),
                        myScene->GetPropDrawnLOD1(),
                        myScene->GetPropDrawnLOD2());
            ImGui::Text("Props pruned: %d  frustum: %d",
                        myScene->GetPropPruned(),
                        myScene->GetPropFrustumCulled());
            ImGui::Text("Creatures LOD0/1/2: %d / %d / %d",
                        myScene->GetCreatureDrawnLOD0(),
                        myScene->GetCreatureDrawnLOD1(),
                        myScene->GetCreatureDrawnLOD2());
            ImGui::Text("Terrain chunks: %d / %d drawn",
                        myScene->GetTerrainChunksDrawn(),
                        myScene->GetTerrainChunksTotal());
        }
        ImGui::End();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);

        // Calculate sun direction based on time of day
        // 12.0 is noon (sun straight up if we want, or at an angle)
        // Let's make it a simple circle.
        float timeAngle = (timeOfDay - 6.0f) / 24.0f * glm::two_pi<float>();
        glm::vec3 sunDir = glm::normalize(glm::vec3(cos(timeAngle), sin(timeAngle), -0.8f));

        myScene->Update(window);

        myScene->Render(camera, waveSpeed, cloudSpeed, waveAmp, timeOfDay, sunDir);

        // Minimal HUD: crosshair + admin indicator.
        PlayerHUD::draw(player, screenWidth, screenHeight);

        // Render ImGui
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        // Headless verification screenshot: when the env var UW_SHOT=<path.bmp>
        // is set, save the back buffer at frame ~200 (scene loaded, surface warmed
        // up) as a 24-bit BMP and quit. Used by tooling; no effect otherwise.
        char uwShotPath[512] = { 0 };
        size_t uwShotLen = 0;
        getenv_s(&uwShotLen, uwShotPath, sizeof(uwShotPath), "UW_SHOT");
        if (uwShotLen > 1) {
            static int uwShotFrame = 0;
            if (++uwShotFrame == 200) {
                int sw = 0, sh = 0;
                glfwGetFramebufferSize(window, &sw, &sh);
                std::vector<unsigned char> px((size_t)sw * sh * 3);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, sw, sh, GL_BGR, GL_UNSIGNED_BYTE, px.data());
                unsigned int rowPad = (4 - (sw * 3) % 4) % 4;
                unsigned int dataSize = (unsigned int)((sw * 3 + rowPad) * sh);
                unsigned char hdr[54] = { 'B', 'M' };
                unsigned int fileSize = 54 + dataSize, dataOff = 54, infoSize = 40;
                std::memcpy(hdr + 2,  &fileSize, 4);
                std::memcpy(hdr + 10, &dataOff,  4);
                std::memcpy(hdr + 14, &infoSize, 4);
                std::memcpy(hdr + 18, &sw, 4);
                std::memcpy(hdr + 22, &sh, 4);   // positive = bottom-up, like glReadPixels
                hdr[26] = 1; hdr[28] = 24;
                FILE* f = nullptr;
                if (fopen_s(&f, uwShotPath, "wb") == 0 && f) {
                    std::fwrite(hdr, 1, 54, f);
                    const unsigned char pad[3] = { 0, 0, 0 };
                    for (int y = 0; y < sh; ++y) {
                        std::fwrite(px.data() + (size_t)y * sw * 3, 1, (size_t)sw * 3, f);
                        if (rowPad) std::fwrite(pad, 1, rowPad, f);
                    }
                    std::fclose(f);
                }
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }
        }

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    delete myScene;
    glfwTerminate();
    return 0;
}
