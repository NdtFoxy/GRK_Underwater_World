from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCENE_H = ROOT / "src" / "scene" / "Scene.h"
SCENE_CPP = ROOT / "src" / "scene" / "Scene.cpp"
WATER_FRAG = ROOT / "assets" / "shaders" / "water.frag"
TERRAIN_FRAG = ROOT / "assets" / "shaders" / "terrain.frag"


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="ignore")


def require(path: Path, needle: str) -> None:
    text = read(path)
    if needle not in text:
        raise SystemExit(f"missing {needle!r} in {path.relative_to(ROOT)}")


def main() -> None:
    # Shore foam must be driven by the generated terrain collision field,
    # not by a hand-painted static line in the terrain shader.
    require(SCENE_H, "shoreDataTextureID")
    require(SCENE_H, "createShorelineDataTexture")
    require(SCENE_CPP, "collisionHeights")
    require(SCENE_CPP, "GL_RGBA32F")
    require(SCENE_CPP, "shoreDataTextureID")
    require(SCENE_CPP, "shoreDistance")
    require(SCENE_CPP, "shoreData[dst + 2]")
    require(SCENE_CPP, "shoreDataTex")
    require(SCENE_CPP, "shoreTexelSize")
    require(SCENE_CPP, "shoreTerrainSize")

    # The water shader should add contact/breaker foam on top of crest foam.
    require(WATER_FRAG, "uniform sampler2D shoreDataTex")
    require(WATER_FRAG, "uniform vec2 shoreTexelSize")
    require(WATER_FRAG, "shoreFoam")
    require(WATER_FRAG, "shoreDist")
    require(WATER_FRAG, "shoreDepth")
    require(WATER_FRAG, "shoreLine")
    require(WATER_FRAG, "shoreTangent")
    require(WATER_FRAG, "breaker")
    require(WATER_FRAG, "shoreAlphaBoost")
    require(WATER_FRAG, "openWaterFoam")
    require(WATER_FRAG, "shoreFoam * 0.95")
    require(WATER_FRAG, "specSoft = pow(NdotH, 64.0) * 0.10")
    require(WATER_FRAG, "sunGlint = pow(NdotH, 128.0) * glintMask * 2.0")
    require(WATER_FRAG, "glintMask = pow(smoothstep")
    require(WATER_FRAG, "totalFoam = clamp(openWaterFoam + shoreFoam")
    require(TERRAIN_FRAG, "swashWet * 0.18")
    require(TERRAIN_FRAG, "terrainSurfFoam")

    print("check_shore_foam_invariants: OK")


if __name__ == "__main__":
    main()
