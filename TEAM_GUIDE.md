# Project Underworld — командный гайд по коду

Это **внутренний документ для нашей тройки**: что где лежит, как это работает,
что чем рендерится и **как поделить материал на 3 человек** так, чтобы каждый
мог уверенно защитить свою часть + свой обязательный метод.

> Публичный `README.md` — это «лицо» проекта (фичи, сборка, управление).
> Этот файл — «изнанка»: карта кода и план подготовки к защите.

---

## 1. Что это вообще такое

Реал-тайм рендер подводного мира на **OpenGL core** (база 3.3; берётся
4.x-контекст ради аппаратной тесселяции поверхности воды). Никаких игровых
движков — весь рендер, освещение, вода, частицы и игрок написаны руками.

**Графический конвейер за кадр** (порядок проходов — буквально буфера
профайлера в `Scene_Render.cpp`):

```
[1] cpu/update        обновление систем, физика игрока, анимации
[2] shadow + caustics depth-pass от солнца → shadow map; caustics в текстуру
[3] terrain           seabed/террейн из heightmap (PBR + нормал-маппинг)
[4] vegetation        тысячи растений инстансингом + LOD
[5] props + anim      статичные пропсы (LOD) + анимированные существа
[6] water + ssr       поверхность воды + screen-space отражения
[7] post              god rays, туман глубины, тонмаппинг → экран
```

**Поток управления:**

```
main.cpp
  ├─ создаёт окно (GLFW), GL-контекст, ImGui
  ├─ PlayerController  (ввод W/A/S/D, мышь → quaternion-камера)
  ├─ Scene::Update()   обновляет все подсистемы
  └─ Scene::Render()   рисует кадр (7 проходов выше)
        ↓ дёргает per-system render: terrain / vegetation / props / water / ...
        ↓ каждая система биндит свой шейдер из assets/shaders/
```

---

## 2. Карта репозитория

```
project_underworld/
├─ src/
│  ├─ main.cpp                точка входа: окно, главный цикл, ImGui-панель, ввод
│  ├─ core/                   загрузчики данных
│  ├─ render/                 низкоуровневые GL-обёртки
│  ├─ scene/                  СЦЕНА и все системы (основная масса кода)
│  └─ player/                 игрок: камера, плавание, фонарь, HUD
├─ assets/
│  ├─ shaders/                ВСЕ GLSL (каждая система = пара .vert/.frag)
│  │  └─ lib/                 общие GLSL-инклуды (foam_lib, ocean_lib)
│  ├─ textures/               PBR-сеты биомов, heightmap, маски биомов
│  ├─ 3d/ models/             модели (кораллы, келп, существа)
│  ├─ hdri/  audio/           окружение и звуковые лупы
├─ external/                  вендоренные либы (imgui, stb_image, tinyexr)
├─ generate_heightmap.py      Python-генератор мира + визуальный редактор
├─ project_underworld.vcxproj основной билд (Visual Studio, Win32) ← мы собираем им
├─ CMakeLists.txt             кросс-платформенный билд (mac/linux)
├─ README.md                  публичное описание проекта
└─ TEAM_GUIDE.md              ← этот файл
```

---

## 3. Файлы по слоям — что делает и что использует

### `src/core/` — загрузка данных
| Файл | LOC | Что делает | Использует |
|---|---|---|---|
| `HeightmapLoader.{h,cpp}` | 267 | Грузит PNG-heightmap + маски биомов в `HeightmapData`; источник высот для террейна и коллизий. | stb_image |
| `ModelLoader.{h,cpp}` | 526 | Импорт glTF/OBJ/FBX → `LoadedMesh` (вершины, индексы, материалы, текстуры). Авто-фит размера. | Assimp |

### `src/render/` — GL-обёртки
| Файл | Что делает |
|---|---|
| `Framebuffer.{h,cpp}` | FBO для render-to-texture (главный буфер сцены, SSR, тени, каустика). |
| `ShaderLoader.{h,cpp}` | Компиляция/линковка GLSL, кэш юниформов, hot-reload. |
| `Cubemap.h` | Процедурный подводный environment-кубмап (**обяз. метод: Cubemap**). |
| `ShadowMap.h` | 2048² depth-текстура от солнца (**обяз. метод: Shadow mapping + PCF**). |

### `src/scene/` — сцена и системы (ядро проекта)
| Файл | LOC | Что делает |
|---|---|---|
| `Scene.h` | 394 | Объявление класса `Scene` — общий «фасад» всех систем, держит все методы. |
| `Scene.cpp` | 1490 | «Дирижёр»: `Init()` (поднимает всё), `Update()`, создание water-grid/skybox/shadow, существа, каустика, загрузка текстур. |
| `Scene_Render.cpp` | 774 | **Главный per-frame проход** `Render()` (7 пассов) + профайлер `g_prof` (UW_PROF). |
| `Scene_Terrain.cpp` | 664 | **B07 (graded):** heightmap-seabed — построение меша террейна, shoreline-данные, сэмплинг высоты для коллизий. |
| `Scene_Props.cpp` | 553 | **A07-смежное:** статичные пропсы (камни/скалы) с дискретным LOD, фрустум-куллингом, инстанс-размещением. |
| `SceneInternal.h` | 77 | Общие inline-хелперы: `Frustum` (куллинг), `chooseObjectLOD`, `hash01`, `sanitizeMaterialFactor`. |
| `VegetationSystem.{h,cpp}` | 586 | **A07 (graded):** тысячи растений — GPU-инстансинг (1 draw call на LOD), 3 дискретных LOD, стохастический pruning. |
| `Camera.h` | 128 | **Обяз. метод: quaternion-камера** (orientation в `glm::quat`, без gimbal-lock). |
| `SplinePath.h` | 189 | **Обяз. метод: Parallel Transport Frames** — тело морского змея вдоль Catmull-Rom сплайна. |
| `FishSchool.{h,cpp}` | 201 | Косяки рыб (дешёвое орбитальное движение вокруг якоря). |
| `WaveField.h` | 153 | **Gerstner-волны — единый источник:** и рендеримая поверхность воды (через `upload()` в `water_tess.tese`), и физика/плавучесть (буи, бобинг игрока). Одна и та же таблица волн → лодки сидят ровно на нарисованной волне. |
| `WaterGrid.h` | 37 | Сетка-меш водной поверхности (радиальная, follow-камеры). |
| `Weather.h` | 129 | Шторм/погода (интенсивность управляет волнами, дождём, молнией). |
| `FloatingObject.h` | 232 | Плавающие объекты (буи) с плавучестью от `WaveField`. |
| `PalmMesh.h` | 84 | Процедурная пальма на берегу. |

### `src/player/` — игрок
| Файл | Что делает |
|---|---|
| `PlayerController.h` | Физика плавания: ввод → скорость, инерция, коллизия с террейном. |
| `Flashlight.h` | Фонарь (конусный свет, `flashlight.glsl`). |
| `CameraShake.h` | Тряска камеры (взрывы/удары). |
| `PlayerHUD.h` | HUD: прицел + индикатор Admin. |

### `assets/shaders/` — по парам на систему
`terrain.*` `seabed.*` (террейн, PBR+нормалмап) · `water_tess.{vert,tesc,tese}` + `water.frag` (тесселированная Gerstner-поверхность) · `surf.*` `spray.*` (прибой/брызги) · `caustics.*` · `vegetation.*` (инстансинг) · `object.*` (пропсы/существа) · `fish.*` · `spline.*` (PTF-змей) · `depth.*` (shadow) · `skybox.*` `sky.*` `clouds.*` (небо/кубмап) · `screen.*` (пост-обработка) · `lib/*.glsl` (общие инклуды: `foam_lib`, `ocean_lib`).

---

## 4. Где «живут» обязательные и graded методы (шпаргалка для защиты)

| Метод | Тип | Главные файлы |
|---|---|---|
| **A07 — Instancing + дискретный LOD** | graded | `VegetationSystem.cpp`, `vegetation.{vert,frag}` (+ доп. LOD в `Scene_Props.cpp`) |
| **B07 — Heightmap seabed** | graded | `Scene_Terrain.cpp`, `HeightmapLoader.cpp`, `terrain.{vert,frag}`, `seabed.{vert,frag}` |
| Normal mapping | обяз. | `terrain.frag`, `object.frag`, `water.frag` |
| PBR (Cook-Torrance) | обяз. | `terrain.frag` |
| Quaternion-камера | обяз. | `scene/Camera.h` |
| Cubemap | обяз. | `render/Cubemap.h`, `skybox.*`, `sky.frag` |
| Parallel Transport Frames | обяз. | `scene/SplinePath.h`, `spline.*` |
| Shadow mapping + PCF | обяз. | `render/ShadowMap.h`, `depth.*`, PCF в `terrain.frag`/`spline.frag` |

---

## 5. Деление на 3 человек

Принцип: каждый берёт **связный домен**, в котором лежит **минимум один graded
ИЛИ несколько обязательных методов**, чтобы на защите отвечать «по своему куску».
Вода намеренно отдана одному человеку, но она **не graded** (по требованиям курса
с неё спрашивают только визуальную связность, не волновую математику) — поэтому её
вес уравновешивает остальное.

### 👤 Человек 1 — «Террейн, освещение, материалы» (владелец **B07**)
**Учит и защищает:**
- `core/HeightmapLoader.{h,cpp}` — как PNG → высоты/маски.
- `scene/Scene_Terrain.cpp` — построение меша seabed, shoreline, коллизии.
- `render/ShadowMap.h` + `depth.{vert,frag}` — теневой проход.
- Шейдеры: `terrain.*`, `seabed.*`.

**Методы на защиту:** **B07 (heightmap seabed)** + обязательные **Normal mapping**,
**PBR-материалы**, **Shadow mapping + PCF**.
**Почему вместе:** террейн, его материалы и тени — один освещенческий пайплайн;
PBR/нормалмап/тени все сходятся в `terrain.frag`.

### 👤 Человек 2 — «Инстансинг, растительность, существа» (владелец **A07**)
**Учит и защищает:**
- `scene/VegetationSystem.{h,cpp}` — инстансинг + 3 LOD + pruning (эталон A07).
- `scene/Scene_Props.cpp` — пропсы с дискретным LOD и фрустум-куллингом.
- `core/ModelLoader.{h,cpp}` — импорт моделей (Assimp).
- `scene/FishSchool.{h,cpp}` — косяки рыб.
- `scene/SplinePath.h` — морской змей на сплайне.
- `SceneInternal.h` — `Frustum`, `chooseObjectLOD` (общий LOD-механизм).
- Шейдеры: `vegetation.*`, `object.*`, `fish.*`, `spline.*`.

**Методы на защиту:** **A07 (instancing + LOD)** + обязательный **Parallel
Transport Frames** (змей).
**Почему вместе:** всё, что «много геометрии через один draw call + выбор LOD по
расстоянию» — растительность, пропсы, рыбы; PTF-змей тоже геометрия-вдоль-кривой.

### 👤 Человек 3 — «Вода, кадр, камера, игрок, пост» (владелец рендер-цикла)
**Учит и защищает:**
- `scene/Scene_Render.cpp` — **порядок 7 проходов кадра** (ключ ко всей картине).
- `scene/Scene.cpp` — `Init()`/`Update()`: как всё связывается вместе.
- Вода: `WaveField.h` (Gerstner — поверхность + физика), `WaterGrid.h`, `Weather.h`,
  `FloatingObject.h` + шейдеры `water_tess.*`, `water.frag`, `surf.*`, `spray.*`, `caustics.*`, SSR.
- Камера/кубмап: `scene/Camera.h`, `render/Cubemap.h`, `skybox.*`, `sky.*`, `clouds.*`.
- Игрок: весь `player/` + `main.cpp` (главный цикл, ввод, ImGui).
- Пост: `screen.*` (god rays, туман, тонмаппинг).

**Методы на защиту:** обязательные **Quaternion-камера** и **Cubemap**.
**Почему вместе:** этот человек «владеет кадром» — главный цикл, очередность
проходов, вода и пост-обработка как финальная композиция; камера и игрок —
вход в этот цикл. Вода объёмная, но graded-глубины не требует.

### Что знают все трое (общая база)
- Поток `main.cpp → Scene::Update → Scene::Render` (раздел 1).
- Где какой метод (таблица в разделе 4) — чтобы подстраховать друг друга.
- Как собрать и запустить (раздел 6).

### Баланс нагрузки (ориентир по LOC)
| | Человек 1 | Человек 2 | Человек 3 |
|---|---|---|---|
| graded | B07 | A07 | — (владелец кадра) |
| обязательных | 3 | 1 | 2 |
| ядро кода | Terrain+Shadow (~1.0k) | Vegetation+Props+Model+Fish (~1.5k) | Render+Scene core+Water+Player (~2k, но вода неглубоко) |

---

## 6. Сборка и запуск (быстрый старт)

**Windows (наш основной путь):** открыть `project_underworld.vcxproj`, конфигурация
**`Release | Win32`** (см. memory: x64 не настроен, Release ~2.5× быстрее Debug —
для честного FPS запускать из `Release\`).

Из консоли:
```
MSBuild project_underworld.vcxproj -p:Configuration=Release -p:Platform=Win32 -m
.\Release\project_underworld.exe
```

**mac/linux:** через `CMakeLists.txt` (`cmake -S . -B build && cmake --build build -j`).

> При добавлении нового `.cpp` — вписать его И в `project_underworld.vcxproj`
> (`<ClCompile Include=...>`), И в `CMakeLists.txt` (`APP_SOURCES`). Оба списка
> ведутся вручную.

**Профилирование:** переменная окружения `UW_PROF=1` включает per-pass профайлер
в `Scene_Render.cpp` (печатает мс по 7 пассам — видно, что террейн самый дорогой).

---

## 7. Советы к защите
- На вопрос «покажи свой метод» — открывай **конкретный файл из раздела 4**, не
  скролль трёхтысячный монолит (его и поделили на `Scene_*.cpp` ради этого).
- Названия файлов `Scene_Terrain.cpp` / `VegetationSystem.cpp` сами называют B07/A07 —
  это работает на вас.
- Уметь объяснить **порядок проходов** (раздел 1) — частый вопрос «почему вода
  рисуется после террейна?» (ответ: SSR/прозрачность читают уже готовую сцену).
```
