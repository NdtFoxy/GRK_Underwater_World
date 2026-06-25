// ----------------------------------------------------------------------
// Scene — main per-frame render pass (the heart of the frame). Split out
// of Scene.cpp: orchestrates shadow + caustics, opaque terrain / vegetation
// / props / creatures, the water + SSR pass and post-processing, in that
// order. Scene::Render belongs to the Scene class declared in Scene.h.
// ----------------------------------------------------------------------
#include "Scene.h"
#include "SceneInternal.h"
#include "../render/GLUniform.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

void Scene::Render(Camera& camera, float waveSpeed, float cloudSpeed, float waveAmp, float timeOfDay, glm::vec3 sunDir) {
    // Apply the current wave animation speed to the shared field so the
    // GPU surface and CPU buoyancy/underwater queries agree this frame.
    waveField.speed = waveSpeed;
    // Single sea-state drives the Gerstner field (buoyancy + near-shore surf).
    // Map intensity 0..1 -> amplitude 0.35..2.0, times the user's waveAmp trim.
    waveField.ampScale = (0.35f + 1.65f * stormIntensity) * waveAmp;

    // Animate jellyfish/fish along their looping paths; the shark AI
    // tracks the diver (previous frame's camera — the lag is invisible).
    updateCreatures(prevCamValid ? prevCamPos : glm::vec3(0.0f, -10.0f, 0.0f));

    // Boids schools: steer/flock and flee the camera. One seabed sample
    // per school (at its centroid) is enough for the comfort band.
    {
        glm::vec3 camP = glm::vec3(0.0f);
        // camera position isn't computed yet at this point in Render —
        // use the previous frame's (1-frame lag is invisible in flocking).
        camP = prevCamValid ? prevCamPos : glm::vec3(0.0f, -10.0f, 0.0f);
        for (auto& s : fishSchools) {
            glm::vec3 c = s.centroid();
            float floorY = SampleTerrainHeight(c.x, c.z);
            if (floorY < -90000.0f) floorY = -130.0f;
            s.update(frameDt, time, camP, floorY, waterLevel, stormIntensity);
        }
    }

    // 0. Off-screen passes (caustics texture + sun shadow map). MUST
    // happen before we bind the main FBO, since they change the
    // viewport / FBO / blend state.
    renderCaustics(waveSpeed, sunDir);
    renderShadowDepth(sunDir, camera.Position);

    mainFBO->Bind();
    // Viewport matches the (scaled) scene render target — the 3D scene is
    // rendered at scaledW x scaledH and upscaled to the window in the post
    // pass. The caustics pass and ImGui both touch the viewport, so set it
    // explicitly here.
    glViewport(0, 0, scaledW(), scaledH());
    glEnable(GL_DEPTH_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Camera from class
    glm::vec3 camPos = camera.Position;
    glm::mat4 model = glm::mat4(1.0f);
    glm::mat4 viewMat = camera.GetViewMatrix();
    glm::mat4 projection = glm::perspective(glm::radians(camera.Zoom), (float)screenWidth / screenHeight, 0.1f, 2000.0f);

    // Sample the unified wave field for the player's underwater test.
    float waveH = waveField.sampleHeight(camPos.x, camPos.z, time);
    // Underwater factor: 0 = above water, 1 = fully underwater
    float underwaterFactor = glm::clamp((waveH - camPos.y) * 2.0f + 0.5f, 0.0f, 1.0f);

    // Remember the camera for next frame: the fish/creature update runs at the
    // top of the frame before camPos is recomputed, so it reads this.
    prevCamPos = camPos;
    prevCamValid = true;

    // Disable face culling so we can see water from below
    glDisable(GL_CULL_FACE);

    // =============================================
    // 0b. UNDERWATER CUBEMAP SKYBOX (true background)
    // =============================================
    // Drawn first, at the far plane, with the camera translation
    // removed so it never moves with the player. samplerCube sampled
    // by view direction — the mandatory cubemap method.
    {
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_FALSE);
        glUseProgram(skyboxProgram);
        // Strip translation from the view matrix.
        glm::mat4 skyView = glm::mat4(glm::mat3(viewMat));
        float skyExposure = glm::mix(0.15f, 1.0f,
            glm::clamp(sunDir.y * 2.5f + 0.5f, 0.0f, 1.0f));
        skyExposure *= weather.get().exposure;   // storm darkens the skybox too
        glm::vec3 skyTint = weather.get().sunTint;
        setMat4(skyboxProgram, "view", skyView);
        setMat4(skyboxProgram, "projection", projection);
        setFloat(skyboxProgram, "exposure", skyExposure);
        setVec3(skyboxProgram, "tint", skyTint);
        envCubemap.Bind(GL_TEXTURE0);
        setInt(skyboxProgram, "skybox", 0);
        glBindVertexArray(cubeVAO);
        glDrawArrays(GL_TRIANGLES, 0, 36);
        glDepthMask(GL_TRUE);
        glDepthFunc(GL_LESS);
    }

    // =============================================
    // 1. SKY (background, no depth write)
    // =============================================
    // Above water the HDRI sky is drawn opaque. As the camera submerges
    // we fade it out (alpha blending) so the underwater CUBEMAP abyss
    // gradient drawn in pass 0b becomes the visible background. This is
    // what makes the cubemap a genuine, visible contributor.
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(skyProgram);

    glm::mat4 invProj = glm::inverse(projection);
    glm::mat4 invView = glm::inverse(viewMat);

    setMat4(skyProgram, "invProjection", invProj);
    setMat4(skyProgram, "invView", invView);
    setFloat(skyProgram, "time", time);
    setFloat(skyProgram, "timeOfDay", timeOfDay);
    setFloat(skyProgram, "windSpeed", cloudSpeed);
    setFloat(skyProgram, "underwaterFactor", underwaterFactor);
    setFloat(skyProgram, "weatherExposure", weather.get().exposure);
    // Weather-driven cloud colour + density/coverage (Req 5.3).
    {
        const WeatherParams& wx = weather.get();
        setVec3(skyProgram, "cloudColor", wx.cloudColor);
        glUniform1f (glGetUniformLocation(skyProgram, "cloudDensity"), wx.cloudDensity);
        glUniform1f (glGetUniformLocation(skyProgram, "cloudCoverage"), wx.cloudCoverage);
    }
    setVec3(skyProgram, "sunDirection", sunDir);
    setVec3(skyProgram, "cameraPos", camPos);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, hdriTextureID);
    setInt(skyProgram, "hdriMap", 0);
    glBindVertexArray(skyVAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);

    // =============================================
    // 2. TERRAIN (heightmap-based world, render before water)
    // =============================================
    if (terrainIndicesCount > 0) {
        glUseProgram(terrainProgram);

        setMat4(terrainProgram, "model", model);
        setMat4(terrainProgram, "view", viewMat);
        setMat4(terrainProgram, "projection", projection);
        setFloat(terrainProgram, "time", time);
        setFloat(terrainProgram, "underwaterFactor", underwaterFactor);
        setVec3(terrainProgram, "cameraPos", camPos);
        setVec3(terrainProgram, "sunDirection", sunDir);
        setFloat(terrainProgram, "weatherExposure", weather.get().exposure);
        // Material atlas (sand / mud / rock / lava) + biome masks, bound to
        // texture units 0..15. Same fixed layout the terrain shader expects.
        struct TexBind { GLuint id; const char* name; };
        const TexBind terrainTextures[] = {
            { sandDiffuseID,   "sandDiffuse"   }, // 0  --- sand ---
            { sandNormalID,    "sandNormal"    }, // 1
            { sandRoughnessID, "sandRoughness" }, // 2
            { mudDiffuseID,    "mudDiffuse"    }, // 3  --- mud (silt / deep) ---
            { mudNormalID,     "mudNormal"     }, // 4
            { mudRoughnessID,  "mudRoughness"  }, // 5
            { rockDiffuseID,   "rockDiffuse"   }, // 6  --- rock (steep slopes) ---
            { rockNormalID,    "rockNormal"    }, // 7
            { rockARMID,       "rockARM"       }, // 8
            { lavaDiffuseID,   "lavaDiffuse"   }, // 9  --- lava biome ---
            { lavaNormalID,    "lavaNormal"    }, // 10
            { lavaRoughnessID, "lavaRoughness" }, // 11
            { lavaEmissiveID,  "lavaEmissive"  }, // 12
            { castleMaskTexID, "castleMask"    }, // 13 --- biome masks ---
            { lavaMaskTexID,   "lavaMask"      }, // 14
            { riverMaskTexID,  "riverMask"     }, // 15
        };
        for (int i = 0; i < (int)(sizeof(terrainTextures) / sizeof(TexBind)); ++i) {
            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, terrainTextures[i].id);
            glUniform1i(glGetUniformLocation(terrainProgram, terrainTextures[i].name), i);
        }

        // Render-to-texture caustics from the water-surface refraction pass
        glActiveTexture(GL_TEXTURE16);
        glBindTexture(GL_TEXTURE_2D, causticsTex);
        setInt(terrainProgram, "causticsTex", 16);
        setFloat(terrainProgram, "terrainSize", terrainSize);
        setFloat(terrainProgram, "causticsTileSize", causticsTileSize);
        // Shadow map (sun depth) + light-space matrix for PCF receiving.
        glActiveTexture(GL_TEXTURE17);
        glBindTexture(GL_TEXTURE_2D, shadowMap.depthTex);
        setInt(terrainProgram, "shadowMap", 17);
        glUniformMatrix4fv(glGetUniformLocation(terrainProgram, "lightSpaceMatrix"),
                           1, GL_FALSE, glm::value_ptr(shadowMap.lightSpace));
        glUniform1i(glGetUniformLocation(terrainProgram, "shadowEnabled"),
                    (sunDir.y > 0.05f) ? 1 : 0);

        // Flashlight uniforms (camera spotlight).
        flashlight.setUniforms(terrainProgram, camPos, camera.Front);

        // ---- frustum-culled chunked draw ----
        // Submit only the terrain chunks whose AABB intersects the view
        // frustum. The mesh and shading are unchanged; we just skip the
        // ~50-70% of triangles that are behind/outside the camera.
        glBindVertexArray(terrainVAO);
        if (!terrainChunks.empty()) {
            Frustum fr; fr.extract(projection * viewMat);
            terrainChunksDrawn = 0;
            for (const auto& ch : terrainChunks) {
                if (!fr.testAABB(ch.aabbMin, ch.aabbMax)) continue;
                // Distance LOD: near chunks full-res, far chunks coarser.
                glm::vec3 cc = (ch.aabbMin + ch.aabbMax) * 0.5f;
                float dist = glm::length(camPos - cc);
                int lod = (dist < 90.0f) ? 0 : (dist < 240.0f) ? 1 : 2;
                if (ch.lodCount[lod] == 0) lod = 0;
                glDrawElements(GL_TRIANGLES, ch.lodCount[lod], GL_UNSIGNED_INT,
                               (const void*)ch.lodOffset[lod]);
                ++terrainChunksDrawn;
            }
        } else {
            glDrawElements(GL_TRIANGLES, terrainIndicesCount, GL_UNSIGNED_INT, 0);
        }
    }

    // ---- Vegetation: instanced coral with LOD + stochastic pruning ----
    if (vegetationProgram) {
        glUseProgram(vegetationProgram);
        flashlight.setUniforms(vegetationProgram, camPos, camera.Front);
        setFloat(vegetationProgram, "uCurrent", stormIntensity);        coral.render(vegetationProgram, camPos, viewMat, projection,
                     sunDir, time);
        // Kelp groves in the deeper band (procedural cards, full sway).
        vegetation.render(vegetationProgram, camPos, viewMat, projection,
                          sunDir, time);
        // Seagrass beds on the shelf (underwater lighting + current sway).
        seagrass.render(vegetationProgram, camPos, viewMat, projection,
                        sunDir, time);
        // Palms on the islands (above-water, land-mode lighting).
        palms.render(vegetationProgram, camPos, viewMat, projection,
                     sunDir, time);
        // Grass meadows on the island tops.
        grassLand.render(vegetationProgram, camPos, viewMat, projection,
                         sunDir, time);
    }

    // ---- Static props (rocks / cliffs / palms / ferns) ----
    renderProps(viewMat, projection, camPos, sunDir, camera);

    // ---- Animated sea creatures (jellyfish + tuna) ----
    renderCreatures(viewMat, projection, camPos, sunDir, camera);

    // ---- Boids fish schools (one instanced draw call per school) ----
    if (fishProgram) {
        for (auto& s : fishSchools)
            s.render(fishProgram, viewMat, projection, camPos, sunDir, time);
    }

    // ---- Sea-serpent: tube swept along a Catmull-Rom spline using
    //      Parallel Transport Frames (mandatory PTF method). Patrols the
    //      reef, receives soft PCF shadows and the flashlight. ----
    if (splineProgram && serpent.indexCount > 0) {
        glUseProgram(splineProgram);

        // Patrol orbit around the world centre. Closer + a bit faster than
        // before so the serpent is actually seen. Crucially, the orbit
        // height now follows the terrain: we sample the seabed along the
        // body and lift the whole orbit so it clears the heightmap instead
        // of ploughing through it — while staying safely underwater.
        const float orbitR     = 55.0f;
        const float orbitSpeed = 0.14f;
        const float baseDepth  = -22.0f;
        float ang = time * orbitSpeed;
        glm::vec3 dir = glm::normalize(glm::vec3(-std::sin(ang), 0.0f, std::cos(ang)));
        glm::vec3 headXZ(std::cos(ang) * orbitR, 0.0f, std::sin(ang) * orbitR);
        float topY = -1.0e9f;
        for (float s = -0.5f; s <= 0.5f; s += 0.25f) {
            glm::vec3 p = headXZ + dir * (s * serpent.bodyLength);
            float th = SampleTerrainHeight(p.x, p.z);
            if (th > -90000.0f) topY = std::max(topY, th);
        }
        float centerY = baseDepth;
        if (topY > -1.0e8f)
            centerY = std::max(baseDepth, topY + 9.0f);   // clear the seabed
        centerY = std::min(centerY, waterLevel - 6.0f);   // stay underwater

        glm::mat4 sModel = serpent.patrolModel(time, glm::vec3(0.0f, centerY, 0.0f),
                                               orbitR, orbitSpeed);
        setMat4(splineProgram, "model", sModel);
        setMat4(splineProgram, "view", viewMat);
        setMat4(splineProgram, "projection", projection);
        setFloat(splineProgram, "time", time);
        setVec3(splineProgram, "cameraPos", camPos);
        setVec3(splineProgram, "sunDirection", sunDir);
        setMat4(splineProgram, "lightSpaceMatrix", shadowMap.lightSpace);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, shadowMap.depthTex);
        setInt(splineProgram, "shadowMap", 0);
        setInt(splineProgram, "shadowEnabled", (sunDir.y > 0.05f) ? 1 : 0);
        flashlight.setUniforms(splineProgram, camPos, camera.Front);
        glDisable(GL_CULL_FACE);
        glBindVertexArray(serpent.VAO);
        glDrawElements(GL_TRIANGLES, serpent.indexCount, GL_UNSIGNED_INT, 0);
        glBindVertexArray(0);
    }

    // Fallback flat seabed (renders below terrain as safety net)
    glUseProgram(seabedProgram);
    setMat4(seabedProgram, "model", model);
    setMat4(seabedProgram, "view", viewMat);
    setMat4(seabedProgram, "projection", projection);
    setFloat(seabedProgram, "time", time);
    setFloat(seabedProgram, "underwaterFactor", underwaterFactor);
    setVec3(seabedProgram, "cameraPos", camPos);
    setVec3(seabedProgram, "sunDirection", sunDir);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sandDiffuseID);
    setInt(seabedProgram, "sandDiffuse", 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, sandNormalID);
    setInt(seabedProgram, "sandNormal", 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, sandRoughnessID);
    setInt(seabedProgram, "sandRoughness", 2);
    glBindVertexArray(seabedVAO);
    glDrawElements(GL_TRIANGLES, seabedIndicesCount, GL_UNSIGNED_INT, 0);

    // ---- SSR snapshot: copy the opaque scene (color+depth) into ssrFBO BEFORE the
    //      water draws, so water.frag can ray-march reflections of the islands /
    //      rocks / seabed. Boats are drawn after water, so they won't reflect. ----
    glBindFramebuffer(GL_READ_FRAMEBUFFER, mainFBO->FBO);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ssrFBO->FBO);
    glBlitFramebuffer(0, 0, scaledW(), scaledH(), 0, 0, scaledW(), scaledH(),
                      GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, mainFBO->FBO);   // resume drawing into mainFBO

    // =============================================
    // 3. WATER (with alpha blending for transparency)
    // =============================================
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(waterProgram);

    // Upload the unified Gerstner field — the water tessellation shader evaluates it
    // per vertex for the rendered surface, and the CPU buoyancy mirror (WaveField::
    // sample) uses the SAME table, so floating objects sit exactly on the surface.
    waveField.upload(waterProgram);

    // Camera-follow the radial water grid CONTINUOUSLY. The old 1.5 m snap
    // made the whole grid pop in discrete steps during fast camera flight
    // (visible jerking); the Gerstner wave field is world-anchored (evaluated
    // by worldXZ in the .tese), and the tessellator uses fractional spacing —
    // so letting the vertices slide smoothly through the field is artifact-free
    // and far smoother.
    glm::vec3 waterCenter(camPos.x, 0.0f, camPos.z);
    glm::mat4 waterModel = glm::translate(glm::mat4(1.0f), waterCenter);
    setFloat(waterProgram, "uGridHalfExtent", waterHalfExtent);
    setMat4(waterProgram, "model", waterModel);
    setMat4(waterProgram, "view", viewMat);
    setMat4(waterProgram, "projection", projection);
    setFloat(waterProgram, "time", time);
    // Foam/normal scroll follows the WAVE speed; clouds in the Snell
    // window follow the independent CLOUD speed.
    setFloat(waterProgram, "windSpeed", waveSpeed);
    setFloat(waterProgram, "cloudSpeed", cloudSpeed);
    setFloat(waterProgram, "waveAmp", waveAmp);
    setFloat(waterProgram, "weatherExposure", weather.get().exposure);
    setFloat(waterProgram, "underwaterFactor", underwaterFactor);
    setFloat(waterProgram, "uStorm", stormIntensity);   // drives whitecaps/foam/color
    setVec3(waterProgram, "cameraPos", camPos);
    setVec3(waterProgram, "sunDirection", sunDir);

    // Weather-driven water colour (Req 5.2).
    const WeatherParams& wx = weather.get();
    // Storm-boosted fog: only amplified when the camera is actually underwater.
    float stormFog = wx.fogDensity * (1.0f + 1.4f * stormIntensity * underwaterFactor);
    setVec3(waterProgram, "weatherShallow", wx.waterShallow);
    setVec3(waterProgram, "weatherDeep", wx.waterDeep);
    glUniform1f (glGetUniformLocation(waterProgram, "weatherFog"),     stormFog);
    // Cloud deck (same values skyProgram gets): the water's reflected sky and the
    // ambient light on the body/foam must match the sky that is actually drawn,
    // otherwise the sea mirrors a clear blue sky under a grey overcast deck.
    setVec3(waterProgram, "cloudColor", wx.cloudColor);
    glUniform1f (glGetUniformLocation(waterProgram, "cloudDensity"),  wx.cloudDensity);
    glUniform1f (glGetUniformLocation(waterProgram, "cloudCoverage"), wx.cloudCoverage);

    // Bind water normal map
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, waterNormalTextureID);
    setInt(waterProgram, "normalMap", 0);
    // Bind HDRI map
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, hdriTextureID);
    setInt(waterProgram, "hdriMap", 1);
    // Terrain-derived shoreline data: height, valid, distance-to-land, land mask.
    // The water shader uses it for dynamic contact foam and shore breakers.
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, shoreDataTextureID);
    setInt(waterProgram, "shoreDataTex", 2);    float shoreTexel = (shoreDataResolution > 0) ? (1.0f / (float)shoreDataResolution) : 1.0f;
    glUniform2f(glGetUniformLocation(waterProgram, "shoreTexelSize"), shoreTexel, shoreTexel);
    setFloat(waterProgram, "shoreTerrainSize", terrainSize);
    setFloat(waterProgram, "shoreFoamWidth", 22.0f);
    setFloat(waterProgram, "shoreFoamIntensity", 1.15f);
    setFloat(waterProgram, "shoreBreakSpeed", 1.05f);
    // SSR inputs: opaque-scene color+depth snapshot + projection for screen marching.
    {
        glm::mat4 viewProj = projection * viewMat;
        setMat4(waterProgram, "uViewProj", viewProj);
        setFloat(waterProgram, "camNear", 0.1f);
        setFloat(waterProgram, "camFar", 2000.0f);
        glUniform2f(glGetUniformLocation(waterProgram, "screenSize"), (float)screenWidth, (float)screenHeight);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, ssrFBO->textureColorbuffer);
        setInt(waterProgram, "ssrColor", 3);
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, ssrFBO->textureDepthbuffer);
        setInt(waterProgram, "ssrDepth", 4);
        glActiveTexture(GL_TEXTURE0);
    }

    // Hardware tessellation: each grid triangle is a 3-vertex patch the GPU subdivides
    // near the camera (real polygons on crests). uTessMax caps the near subdivision;
    // the TCS scales it down with distance (pure distance LOD).
    setFloat(waterProgram, "uTessMax", 8.0f);    glPatchParameteri(GL_PATCH_VERTICES, 3);
    glBindVertexArray(waterVAO);
    glDrawElements(GL_PATCHES, waterIndicesCount, GL_UNSIGNED_INT, 0);


    glDisable(GL_BLEND);

    // =============================================
    // 3. POST-PROCESS (god rays + tonemapping)
    // =============================================
    mainFBO->Unbind();
    glViewport(0, 0, screenWidth, screenHeight);
    glDisable(GL_DEPTH_TEST);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(screenProgram);

    // Compute sun screen position for god rays
    glm::vec4 sunWorldPos = glm::vec4(camPos + sunDir * 1000.0f, 1.0f);
    glm::vec4 sunClip = projection * viewMat * sunWorldPos;
    glm::vec2 sunScreen(0.5f, 0.5f);
    if (sunClip.w > 0.0f) {
        glm::vec3 sunNDC = glm::vec3(sunClip) / sunClip.w;
        sunScreen = glm::vec2(sunNDC.x * 0.5f + 0.5f, sunNDC.y * 0.5f + 0.5f);
    }

    setVec2(screenProgram, "sunScreenPos", sunScreen);
    setFloat(screenProgram, "godRayIntensity", 1.2f);
    setVec3(screenProgram, "sunDirection", sunDir);

    // Weather colour grade for the final image.
    {
        const WeatherParams& wx = weather.get();
        // Shark panic: while the predator circles, the image flushes warm
        // and the vignette pulses at heartbeat rate — pure post-process
        // menace (the shark never deals damage).
        glm::vec3 tintG = wx.gradeTint *
            glm::vec3(1.0f + 0.16f * sharkThreat,
                      1.0f - 0.05f * sharkThreat,
                      1.0f - 0.08f * sharkThreat);
        setVec3(screenProgram, "gradeTint", tintG);
        setFloat(screenProgram, "gradeSat", wx.saturation);
        setFloat(screenProgram, "gradeContrast", wx.contrast);
        // Storm (low exposure) gets a stronger vignette.
        float vig = glm::clamp((1.0f - wx.exposure) * 0.25f, 0.0f, 0.18f);
        if (sharkThreat > 0.01f) {
            float beat = 0.5f + 0.5f * std::sin(time * 9.0f);   // ~86 bpm
            vig += sharkThreat * (0.14f + 0.16f * beat);
        }
        setFloat(screenProgram, "gradeVignette", vig);    }
    // Extra post FX state.
    glUniform1f(glGetUniformLocation(screenProgram, "aboveWaterSun"),
                glm::clamp(sunDir.y * 3.0f, 0.0f, 1.0f));
    // Lens raindrops: strong in storm, and briefly when at the surface.
    {
        float rain = stormIntensity * 0.8f;
        glUniform1f(glGetUniformLocation(screenProgram, "rainAmount"),
                    rain * (1.0f - underwaterFactor));
    }
    // Auto-exposure: by day, lift brightness as the camera dives into the
    // dark so the deep stays readable. At NIGHT, do the opposite — clamp the
    // whole scene down so underwater goes genuinely dark and moody and you
    // lean on the flashlight (the day/night lift was why night looked like
    // day underwater). dayMix: 1 day .. 0 night, from the sun height.
    {
        float camDepth = glm::max(0.0f, waterLevel - camPos.y);
        // Surface sun intensity on the SAME gradual curve the sky/terrain use,
        // so dusk is dusky (not black) and below water matches above water.
        float t = glm::clamp((sunDir.y + 0.25f) / 0.43f, 0.0f, 1.0f);
        float sunI = t * t * (3.0f - 2.0f * t);             // smoothstep(-0.25, 0.18)
        // Depth readability lift only matters when there's surface light to
        // adapt to; at night there's none, so the eye stays in the dark.
        float aeDay = 1.0f + glm::clamp(camDepth * 0.012f, 0.0f, 1.4f);
        float ae = glm::mix(0.30f, aeDay, sunI);            // night → ~0.30 (dark, use the torch)
        setFloat(screenProgram, "autoExposure", ae);    }
    setFloat(screenProgram, "underwaterFactor", underwaterFactor);
    setFloat(screenProgram, "stormMurk", stormIntensity * underwaterFactor); // 0..1
    setFloat(screenProgram, "lightningFlash", lightningFlash);
    // Extra uniforms for the volumetric underwater shafts (world-space
    // reconstruction from the depth buffer).
    glm::mat4 invViewMat = glm::inverse(viewMat);
    glm::mat4 invProjMat = glm::inverse(projection);
    setMat4(screenProgram, "invView", invViewMat);
    setMat4(screenProgram, "invProjection", invProjMat);
    setVec3(screenProgram, "cameraPos", camPos);
    setFloat(screenProgram, "time", time);
    setFloat(screenProgram, "waterLevel", 0.0f);
    // ---- Sonar ping: shell uniforms + colour-coded creature contacts ----
    {
        const float sonarSpeed = 32.0f;    // m/s shell expansion
        const float sonarMaxR  = 170.0f;   // shell range
        const float sonarLife  = sonarMaxR / sonarSpeed + 1.2f;
        float sonarAge = -1.0f;
        if (sonarActive) {
            sonarAge = time - sonarStart;
            if (sonarAge > sonarLife) { sonarActive = false; sonarAge = -1.0f; }
        }
        setFloat(screenProgram, "sonarAge", sonarAge);
        setVec3(screenProgram, "sonarOrigin", sonarOrigin);
        setFloat(screenProgram, "sonarSpeed", sonarSpeed);
        setFloat(screenProgram, "sonarMaxRadius", sonarMaxR);
        std::vector<glm::vec2> cUV;
        std::vector<float>     cDist;
        std::vector<glm::vec3> cCol;
        auto addContact = [&](const glm::vec3& wp, const glm::vec3& col) {
            if (cUV.size() >= 16) return;
            glm::vec4 clip = projection * viewMat * glm::vec4(wp, 1.0f);
            if (clip.w <= 0.001f) return;                       // behind the camera
            glm::vec3 ndc = glm::vec3(clip) / clip.w;
            if (ndc.x < -1.4f || ndc.x > 1.4f || ndc.y < -1.4f || ndc.y > 1.4f) return;
            cUV.push_back(glm::vec2(ndc.x * 0.5f + 0.5f, ndc.y * 0.5f + 0.5f));
            cDist.push_back(glm::length(wp - sonarOrigin));
            cCol.push_back(col);
        };
        if (sonarAge >= 0.0f) {
            // Gather candidates (live world pos + type colour), keep those the
            // shell can reach, then ping the NEAREST ones first so a swarm of
            // distant jellyfish can't crowd out the shark.
            struct Cand { glm::vec3 pos; glm::vec3 col; float d; };
            std::vector<Cand> cand;
            auto consider = [&](const glm::vec3& wp, const glm::vec3& col) {
                float d = glm::length(wp - sonarOrigin);
                if (d <= sonarMaxR + 8.0f) cand.push_back({ wp, col, d });
            };
            for (const auto& c : creatures) {
                glm::vec3 wp  = glm::vec3(c.model[3]);                  // live position
                glm::vec3 col = c.isShark ? glm::vec3(1.00f, 0.16f, 0.12f)  // shark → red
                              : c.isFish  ? glm::vec3(0.22f, 1.00f, 0.45f)  // fish  → green
                                          : glm::vec3(0.25f, 0.55f, 1.00f); // jelly → blue
                consider(wp, col);
            }
            for (const auto& s : fishSchools)
                consider(s.centroid(), glm::vec3(0.22f, 1.00f, 0.45f));      // small fish → green
            std::sort(cand.begin(), cand.end(),
                      [](const Cand& a, const Cand& b) { return a.d < b.d; });
            for (const auto& c : cand) addContact(c.pos, c.col);            // caps at 16 + culls offscreen
        }
        int nC = (int)cUV.size();
        setInt(screenProgram, "sonarContactCount", nC);        if (nC > 0) {
            glUniform2fv(glGetUniformLocation(screenProgram, "sonarContactUV"),    nC, &cUV[0].x);
            glUniform1fv(glGetUniformLocation(screenProgram, "sonarContactDist"),  nC, cDist.data());
            glUniform3fv(glGetUniformLocation(screenProgram, "sonarContactColor"), nC, &cCol[0].x);
        }
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, mainFBO->textureColorbuffer);
    setInt(screenProgram, "screenTexture", 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, mainFBO->textureDepthbuffer);
    setInt(screenProgram, "depthTexture", 1);
    glBindVertexArray(quadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);

}
