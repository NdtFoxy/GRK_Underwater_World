from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCENE_H = ROOT / "src" / "scene" / "Scene.h"
SCENE_CPP = ROOT / "src" / "scene" / "Scene.cpp"
OBJECT_VERT = ROOT / "assets" / "shaders" / "object.vert"


def require(path: Path, needle: str) -> None:
    text = path.read_text(encoding="utf-8", errors="ignore")
    if needle not in text:
        raise SystemExit(f"missing {needle!r} in {path.relative_to(ROOT)}")


def main() -> None:
    # Triangle-thinning LOD is safe for foliage-like meshes, but it creates
    # holes/black gaps on solid rocks and cliffs. Solid props must opt out.
    require(SCENE_H, "bool allowTriangleLod")
    require(SCENE_CPP, "triangleStride = allowTriangleLod ? triangleStride : 1")

    # Missing or failed material textures must not fall back to near-black
    # base factors on large props.
    require(SCENE_CPP, "sanitizeMaterialFactor")
    require(SCENE_CPP, "sub.albedo == 0")

    # Creature assets need an explicit gameplay orientation correction.
    require(SCENE_H, "glm::mat4 orient")
    require(SCENE_CPP, "fishGameplayOrient")
    require(OBJECT_VERT, "bodyAxis =")

    print("check_asset_rendering_invariants: OK")


if __name__ == "__main__":
    main()
