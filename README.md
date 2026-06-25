# Project Underworld

A real-time underwater world renderer in OpenGL 3.3 core. Subnautica-style
ocean with Gerstner-wave water, heightmap-driven seabed with PBR biomes,
render-to-texture caustics, instanced vegetation with LOD,
volumetric god rays, an oxygen/flashlight player system, and a Python tool
for authoring the world.

## Selected technique combination

- **A07 — Instanced rendering + discrete LOD** (vegetation system)
- **B07 — Heightmap-based seabed** (terrain system)

## Mandatory methods

All six required methods are implemented:

1. **Normal mapping** — tangent-space normal maps on terrain, props
   (triplanar), and water (`terrain.frag`, `object.frag`, `water.frag`).
2. **PBR materials** — Cook-Torrance BRDF (GGX distribution, Smith
   geometry, Fresnel-Schlick) with albedo/normal/roughness/AO texture
   sets per biome (`terrain.frag`).
3. **Quaternion camera** — orientation stored as a `glm::quat`, mouse-look
   composed from world-yaw and local-pitch rotations, gimbal-lock free
   (`src/scene/Camera.h`).
4. **Cubemap** — procedural underwater environment cubemap: pure-black
   abyss looking down, light greenish-blue surface glow looking up. Drawn
   as the underwater background and revealed as the camera submerges
   (`src/render/Cubemap.h`, `skybox.{vert,frag}`, `sky.frag`).
5. **Parallel Transport Frames** — a sea-serpent body is a tube swept
   along a Catmull-Rom spline using rotation-minimizing (parallel
   transport) frames, so the cross-section never twists. It patrols the
   reef with a travelling-wave swim animation
   (`src/scene/SplinePath.h`, `spline.{vert,frag}`).
6. **Shadow mapping + PCF** — depth pass from the sun into a 2048² depth
   texture; receivers sample it with a wide-radius 5×5 PCF kernel for the
   very soft shadows that suit scattered underwater light. The flashlight
   and sun both feed the lit surfaces
   (`src/render/ShadowMap.h`, `depth.{vert,frag}`, PCF in `terrain.frag`
   and `spline.frag`).

## Other features

- Gerstner-wave ocean surface with crest foam and wind control
- Render-to-texture caustics (refraction through the surface grid)
- Volumetric god rays / light shafts (above- and under-water)
- Volumetric clouds
- Depth-darkening abyss and underwater fog
- Subnautica-style player: oxygen, wave-bob, terrain collision, flashlight
- Python world builder: presets + visual paint editor (`generate_heightmap.py`)

## Stack

- C++20, OpenGL 3.3 core
- GLFW, GLEW, GLM, Assimp
- Dear ImGui (vendored in `external/imgui`)
- stb_image, tinyexr (vendored in `external/`)

## Build

### Windows (Visual Studio — original setup)

Open `project_underworld.vcxproj`. Place pre-built libs in
`dependencies/{freeglut,glew-2.0.0,glfw-3.3.8.bin.WIN32,glm,assimp}`
matching the include / library paths in the `.vcxproj`.

### macOS (CLion)

1. Install dependencies via Homebrew:
   ```sh
   brew install cmake glew glfw glm assimp
   ```
2. Open the project folder in CLion (it detects `CMakeLists.txt`).
3. Build and run target `project_underworld`. Assets are copied to the
   build folder by a post-build step.

### Linux

```sh
sudo apt install build-essential cmake libglew-dev libglfw3-dev libglm-dev libassimp-dev
cmake -S . -B build
cmake --build build -j
./build/project_underworld
```

## Controls

- **W A S D** — swim
- **Space / Ctrl** — ascend / descend
- **Shift** — speed boost
- **Right Mouse + drag** — look around (quaternion camera)
- **F** — toggle flashlight
- **K** — toggle full storm (rain, lightning, big seas); Storm slider in the panel sets intensity (auto = weather-driven)
- **Q** — sonar ping (expanding shell reveals terrain in the dark; colour-codes shark/fish)
- **ImGui panel** — wind speed, time of day, Player/Admin mode, respawn, storm intensity
- **Esc** — quit

## World authoring

```sh
python generate_heightmap.py editor      # visual paint editor + one-click apply
python generate_heightmap.py list        # list procedural presets
python generate_heightmap.py preset world --masks --out world.png
```

The editor writes `assets/textures/world/T_World_Heightmap.png` plus the
`M_{Lava,Castle,River}_Depth_Mask.png` biome masks the engine reads.

## Layout

```
assets/
  shaders/   GLSL (terrain, water, caustics, vegetation, skybox,
             depth/shadow, spline/PTF, screen post, sky/clouds)
  textures/  PBR biome sets, heightmap, biome masks
  3d/        coral / kelp models
external/    vendored single-header libs and ImGui
src/
  core/      heightmap + model loaders
  render/    framebuffer, shader loader, cubemap, shadow map
  scene/     camera (quaternion), scene, vegetation, spline (PTF)
  player/    oxygen, camera shake, controller, HUD, flashlight
  main.cpp
generate_heightmap.py   world generator / visual editor
```

## Credits

- Creepvine Asset Pack by gavinpgamer1 (CC-BY)
- Coral model and PBR texture sets from their respective authors
