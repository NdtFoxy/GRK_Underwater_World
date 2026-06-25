<!--
  ════════════════════════════════════════════════════════════════════
  WSKAZÓWKA: zrzuty ekranu wrzucaj do  docs/img/  (NIE do assets/,
  bo assets/ jest w DVC i .gitignore — obrazy z assets/ nie trafią na GitHub).
  Podmień banner.png oraz pliki 01..06 na własne screeny z gry.
  ════════════════════════════════════════════════════════════════════
-->

<div align="center" style="margin: 0; padding: 0;">
  <a href="#" style="display: block; margin: 0;">
    <img src="docs/img/banner.png" alt="Project Underworld" width="2560" height="auto" style="display: block; margin: 0;">
  </a>

  <h3 align="center" style="margin: 0;">🌊 Project Underworld — Renderer Świata Podwodnego</h3>
  <p>
    <kbd>OPENGL 3.3 CORE</kbd> ✛ <kbd>C++20</kbd> ✛ <kbd>GLSL / TESSELACJA</kbd> ✛ <kbd>BEZ SILNIKA GRY</kbd>
  </p>
</div>

**Opis projektu:** Renderowany w czasie rzeczywistym świat podwodny w stylu *Subnautiki* — napisany **w całości ręcznie** w OpenGL 3.3 core, bez użycia silnika gry. Ocean z falami **Gerstnera**, dno generowane z **heightmapy** z biomami PBR, kaustyki (render-to-texture), roślinność przez **instancing + LOD**, wolumetryczne promienie światła (god rays), cykl **dnia i nocy**, sztormy oraz system gracza (pływanie, latarka, kolizje z terenem). Cały rendering, oświetlenie, woda, cząsteczki i fizyka gracza są zaimplementowane od zera.

### 👥 Skład grupy

- **Mykyta Kyslytsia**
- **Aliaksandra Mantulenka**
- **Artem Isianov**

**Wybrane metody:** **A07** — Instanced rendering with LOD · **B07** — Heightmap-based seabed mesh

### Built With

- ![C++](https://img.shields.io/badge/C%2B%2B_20-00599C?style=for-the-badge&logo=cplusplus&logoColor=white)
- ![OpenGL](https://img.shields.io/badge/OpenGL_3.3-5586A4?style=for-the-badge&logo=opengl&logoColor=white)
- ![GLSL](https://img.shields.io/badge/GLSL_Shaders-FF6F00?style=for-the-badge&logo=opengl&logoColor=white)
- ![Dear ImGui](https://img.shields.io/badge/Dear_ImGui-1E1E1E?style=for-the-badge)
- ![CMake](https://img.shields.io/badge/CMake-064F8C?style=for-the-badge&logo=cmake&logoColor=white)
- ![Visual Studio](https://img.shields.io/badge/Visual_Studio-5C2D91?style=for-the-badge&logo=visualstudio&logoColor=white)
- ![Python](https://img.shields.io/badge/Python-3776AB?style=for-the-badge&logo=python&logoColor=white)
- ![Git](https://img.shields.io/badge/Git-F05032?style=for-the-badge&logo=git&logoColor=white)
- ![DVC](https://img.shields.io/badge/DVC-945DD6?style=for-the-badge&logo=dvc&logoColor=white)

**Stos technologiczny:**
- **Rdzeń:** C++20, OpenGL 3.3 core.
- **Biblioteki:** GLFW (okno/wejście), GLEW (ładowanie GL), GLM (matematyka), Assimp (modele), Dear ImGui (panel), stb_image + tinyexr (tekstury/HDRI).
- **Narzędzia świata:** Python (generator heightmapy + wizualny edytor).
- **Zasoby (2 GB):** Backblaze B2 (S3) zarządzane przez **DVC**. Kontrola wersji: Git.

---

## 🎓 Metody — wymagane + ocena­ne (A07 + B07)

Kombinacja ocenianych technik naszej grupy:

> 🟩 **A07 — Instancing + dyskretny LOD** (system roślinności)
> 🟦 **B07 — Dno z heightmapy** (system terenu)

Wszystkie **6 metod obowiązkowych** jest zaimplementowanych:

| # | Metoda | Typ | Gdzie w kodzie (do obrony) |
|---|--------|-----|----------------------------|
| 🟩 | **A07 — Instancing + LOD** | **oceniana** | `VegetationSystem.cpp`, `vegetation.{vert,frag}` (+ LOD propsów w `Scene_Props.cpp`) |
| 🟦 | **B07 — Heightmap seabed** | **oceniana** | `Scene_Terrain.cpp`, `HeightmapLoader.cpp`, `terrain.{vert,frag}`, `seabed.*` |
| 1 | Normal mapping | obowiązkowa | `terrain.frag`, `object.frag`, `water.frag` |
| 2 | Materiały PBR (Cook-Torrance) | obowiązkowa | `terrain.frag` |
| 3 | Kamera kwaternionowa | obowiązkowa | `src/scene/Camera.h` |
| 4 | Cubemap (środowisko) | obowiązkowa | `src/render/Cubemap.h`, `skybox.*`, `sky.frag` |
| 5 | Parallel Transport Frames (wąż morski) | obowiązkowa | `src/scene/SplinePath.h`, `spline.*` |
| 6 | Shadow mapping + PCF | obowiązkowa | `src/render/ShadowMap.h`, `depth.*`, PCF w `terrain.frag` |

---

<div align="center">
  <h1>STRUKTURA PROJEKTU</h1>
</div>

<hr>

<details open>
  <summary><kbd>🌊 ROOT</kbd> <b><code>/project_underworld</code></b> ── <i>Renderer Świata Podwodnego</i></summary>

  <blockquote>
    <details open>
      <summary>🟧 <kbd>📁 src</kbd> ── <i>Kod silnika C++ 🧠</i></summary>
      <blockquote>
        🌐 <code>main.cpp</code> ── Okno (GLFW), główna pętla, panel ImGui, obsługa wejścia
        <details open>
          <summary>🗺️ <kbd>📁 scene</kbd> ── <i>Scena i wszystkie systemy (rdzeń)</i></summary>
          <blockquote>
            🎬 <code>Scene_Render.cpp</code> ── Główna pętla klatki: <b>7 przebiegów</b> renderowania<br>
            🟦 <code>Scene_Terrain.cpp</code> ── <b>B07:</b> dno z heightmapy, shoreline, kolizje<br>
            🟩 <code>VegetationSystem.cpp</code> ── <b>A07:</b> tysiące roślin (instancing + 3× LOD + pruning)<br>
            🪨 <code>Scene_Props.cpp</code> ── Statyczne propsy z LOD i frustum cullingiem<br>
            🐟 <code>FishSchool.cpp</code> ── Ławice ryb (uciekają przed nurkiem)<br>
            🌊 <code>WaveField.h</code> ── Fale Gerstnera (jedno źródło: powierzchnia + fizyka)<br>
            📷 <code>Camera.h</code> ── Kamera kwaternionowa (bez gimbal-locka)<br>
            🐍 <code>SplinePath.h</code> ── Wąż morski wzdłuż splajnu (PTF)<br>
            ⛈️ <code>Weather.h</code> ── Pogoda/sztorm (steruje falami, deszczem, błyskawicą)
          </blockquote>
        </details>
        <details>
          <summary>⚙️ <kbd>📁 render</kbd> ── <i>Niskopoziomowe opakowania GL</i></summary>
          <blockquote>
            🖼️ <code>Framebuffer.cpp</code> ── FBO / render-to-texture<br>
            🔧 <code>ShaderLoader.cpp</code> ── Kompilacja GLSL + hot-reload<br>
            🌌 <code>Cubemap.h</code> ── Proceduralny cubemap środowiska<br>
            🌓 <code>ShadowMap.h</code> ── Mapa cieni 2048² + PCF
          </blockquote>
        </details>
        <details>
          <summary>🤿 <kbd>📁 player</kbd> ── <i>Gracz</i></summary>
          <blockquote>
            🎮 <code>PlayerController.h</code> ── Pływanie, inercja, kolizja z terenem<br>
            🔦 <code>Flashlight.h</code> ── Latarka · 📟 <code>PlayerHUD.h</code> ── HUD (celownik)
          </blockquote>
        </details>
        🔸 <kbd>📁 core</kbd> ── Loadery danych: <code>HeightmapLoader</code> (stb_image), <code>ModelLoader</code> (Assimp)
      </blockquote>
    </details>
    <details>
      <summary>🟧 <kbd>📁 assets</kbd> ── <i>Zasoby (2 GB, przez DVC) 📦</i></summary>
      <blockquote>
        🎨 <code>shaders/</code> ── Wszystkie GLSL (każdy system = para .vert/.frag) + <code>lib/</code><br>
        🧱 <code>textures/</code> ── Zestawy PBR biomów, heightmapa, maski biomów<br>
        🐚 <code>3d/ models/</code> ── Modele (korale, kelp, stworzenia)<br>
        🌅 <code>hdri/ audio/</code> ── Środowisko i pętle dźwiękowe
      </blockquote>
    </details>
    <details>
      <summary>📜 <kbd>BUILD &amp; DOCS</kbd></summary>
      <blockquote>
        🔷 <b><kbd>project_underworld.vcxproj</kbd></b> ── <i>Główny build (Visual Studio, Win32)</i><br>
        🧩 <code>CMakeLists.txt</code> ── Build wieloplatformowy (mac/linux)<br>
        🐍 <code>generate_heightmap.py</code> ── Generator świata + wizualny edytor<br>
        📦 <code>assets.dvc</code> ── Wskaźnik DVC na ciężkie zasoby<br>
        📖 <code>README.md</code> ── Ten plik · 🧭 <code>TEAM_GUIDE.md</code> ── Wewnętrzna mapa kodu
      </blockquote>
    </details>
  </blockquote>
</details>
<hr>

# 🚀 Uruchomienie krok po kroku

Potrzebne: **Git**, **Visual Studio 2019/2022** (z C++ desktop workload), **Python 3** oraz **DVC** (do zasobów).

## 1. Sklonuj repozytorium

```sh
git clone https://github.com/NdtFoxy/GRK_Underwater_World.git
cd GRK_Underwater_World
```

---

## 🗄️ 2. Zasoby (DVC + Backblaze B2)

Ciężkie zasoby (tekstury, modele, HDRI — łącznie **2 GB**) są poza Gitem, zarządzane przez **DVC** na zdalnym dysku Backblaze B2.

> [!CAUTION]
> **Wymagany klucz dostępu.** Repozytorium używa prywatnego remote'a Backblaze B2 (S3). Żeby pobrać zasoby, potrzebujesz klucza aplikacyjnego.

### Pobranie zasobów:
1. **Poproś o klucz:** jeśli jesteś w zespole, napisz do prowadzącego repo (Mykyta) — `<email-lidera>` — po `keyID` i `applicationKey` Backblaze.
2. **Skonfiguruj lokalnie** (klucz zapisuje się tylko lokalnie, **nie** trafia do Gita):
   ```sh
   pip install "dvc[s3]"
   dvc remote modify --local b2 access_key_id <keyID>
   dvc remote modify --local b2 secret_access_key <applicationKey>
   ```
3. **Pobierz zasoby:**
   ```sh
   dvc pull
   ```

---

## 🛠️ 3. Budowanie (Windows — Visual Studio)

> [!IMPORTANT]
> Buduj w konfiguracji **`Release | Win32`**. Konfiguracja x64 nie jest skonfigurowana, a `Release` jest ~2,5× szybszy od `Debug` — dla uczciwego FPS uruchamiaj z folderu `Release\`.

**Wariant A — GUI:** otwórz `project_underworld.vcxproj`, wybierz **`Release | Win32`**, naciśnij **Build** (Ctrl+Shift+B).

**Wariant B — z konsoli (Developer PowerShell):**
```powershell
MSBuild project_underworld.vcxproj -p:Configuration=Release -p:Platform=Win32 -m
```

## ▶️ 4. Uruchomienie

```powershell
.\Release\project_underworld.exe
```

> 💡 **macOS / Linux:** projekt ma też `CMakeLists.txt` — `cmake -S . -B build && cmake --build build -j`, a następnie `./build/project_underworld`.

---

# 🎮 Sterowanie

| Klawisz / mysz | Akcja |
|----------------|-------|
| **W A S D** | pływanie |
| **Spacja / Ctrl** | wynurzanie / zanurzanie |
| **Shift** | przyspieszenie |
| **Prawy przycisk myszy + ruch** | rozglądanie się (kamera kwaternionowa) |
| **F** | latarka |
| **K** | pełny sztorm (deszcz, błyskawice, wielkie fale) |
| **Q** | ping sonaru (odkrywa teren w ciemności, koloruje rekina/ryby) |
| **Esc** | wyjście |

**Panel ImGui (na żywo):** presety pogody · **Wave Height** (0–2.5) · **Wave Speed** (0–5) · **Cloud Speed** · **Storm Intensity** · **Time of Day** (0–24) · **Cloud Density** · skok do **Points of Interest** (wrak / rekin) · tryb **Player/Admin** (liczniki LOD/instancingu) · **Render Scale** · **Player Speed** · **Respawn**.

---

# 🎬 Scenariusz prezentacji (3–5 min)

Gotowy plan demo — wykonuj punkty po kolei, wszystko klika się z panelu ImGui i klawiatury.

| ⏱️ | Co pokazać | Jak |
|----|-----------|-----|
| **0:00** | **Intro** — real-time świat podwodny w czystym OpenGL, bez silnika | uruchom `Release\project_underworld.exe` |
| **0:30** | **Kamera kwaternionowa + pływanie** — zanurz się, pokaż cubemap/abyss | `WASD`, `RMB`+mysz, `Spacja/Ctrl`, `Shift` |
| **1:00** | 🟦 **B07 — dno z heightmapy:** ukształtowanie terenu, biomy **PBR** (lawa/skała/piasek), **normal mapping**, **cienie + PCF** | przeleć nad dnem |
| **1:30** | 🟩 **A07 — instancing + LOD:** tysiące roślin z 1 draw-callem; tryb **Admin** pokazuje liczniki `LOD0/1/2` i `pruned` | panel → **Mode: Admin** |
| **2:00** | **Woda (fale Gerstnera)** — od płaskiej tafli do sztormowej; łódki/boje siedzą dokładnie na fali | suwak **Wave Height** 0→2.5, potem **Wave Speed** |
| **2:30** | **🌅 Cykl dnia i nocy** — słońce przesuwa się, zmienia się światło, cienie, kolor wody i god rays | suwak **Time of Day** 0→24 |
| **3:00** | **⛈️ Sztorm i pogoda** — deszcz, błyskawice, wzburzone morze | klawisz **K** lub preset pogody |
| **3:20** | **🐟 Ryby reagują** — podpłyń do ławicy, ryby **uciekają od nurka** (promień 8 m, im bliżej, tym silniej) | podpłyń blisko ławicy |
| **3:40** | **🐍 PTF — wąż morski** wzdłuż splajnu + **🔦 latarka** | obserwuj węża, `F` |
| **4:00** | **🛰️ Sonar** — fala w ciemności odkrywa teren i koloruje rekina/ryby | klawisz **Q** |
| **4:30** | **Points of Interest** — szybki skok do wraku / rekina jako finał | panel → **Go** przy obiekcie |

---

# ✨ Interaktywność (najważniejsze)

- **🌊 Fale na żywo** — `Wave Height` i `Wave Speed` zmieniają ocean od idealnie spokojnej tafli po sztorm. To **jedno źródło Gerstnera** napędza i renderowaną powierzchnię, i fizykę pływalności — boje i łódki zawsze siedzą na widocznej fali.
- **🌅 Dzień ↔ noc** — `Time of Day` (0–24) przelicza kierunek słońca: zmieniają się cienie, barwa wody, god rays i nastrój sceny.
- **🐟 Reagujące ryby** — ławice trzymają komfortową głębokość i **uciekają przed nurkiem** w promieniu 8 m (odpychanie 1/r — najsilniejsze z bliska).
- **⛈️ Pogoda** — presety i `K`: sztorm steruje falami, deszczem i błyskawicami jednocześnie.
- **🛰️ Sonar (Q)** — rozchodząca się powłoka odsłania teren w ciemności i koloruje stworzenia (rekin/ryby).
- **🛠️ Tryb Admin** — podgląd liczników instancingu/LOD/pruningu i frustum cullingu (dowód działania A07).

---

# 🖼️ Galeria

<!-- Wrzuć własne screeny do docs/img/ i podmień ścieżki poniżej -->
<div align="center">
  <img src="docs/img/01_terrain.png"  width="32%" alt="Dno z heightmapy (B07)">
  <img src="docs/img/02_vegetation.png" width="32%" alt="Roślinność instancing + LOD (A07)">
  <img src="docs/img/03_water.png"     width="32%" alt="Fale Gerstnera">
  <img src="docs/img/04_daynight.png"  width="32%" alt="Cykl dnia i nocy">
  <img src="docs/img/05_storm.png"     width="32%" alt="Sztorm">
  <img src="docs/img/06_sonar.png"     width="32%" alt="Sonar">
</div>

---

## 🧰 Tworzenie świata (opcjonalnie)

```sh
python generate_heightmap.py editor      # wizualny edytor + jeden klik „apply”
python generate_heightmap.py list        # lista presetów proceduralnych
```
Edytor zapisuje `assets/textures/world/T_World_Heightmap.png` oraz maski biomów, które wczytuje silnik.

---

## 🙏 Credits

- Creepvine Asset Pack — *gavinpgamer1* (CC-BY)
- Modele korali i zestawy tekstur PBR — odpowiedni autorzy
- Dear ImGui, stb_image, tinyexr — vendored w `external/`
