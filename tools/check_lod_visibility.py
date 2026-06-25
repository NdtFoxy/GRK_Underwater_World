from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCENE_H = ROOT / "src" / "scene" / "Scene.h"
SCENE_CPP = ROOT / "src" / "scene" / "Scene.cpp"
MAIN_CPP = ROOT / "src" / "main.cpp"


def require(path: Path, needle: str) -> None:
    text = path.read_text(encoding="utf-8", errors="ignore")
    if needle not in text:
        raise SystemExit(f"missing {needle!r} in {path.relative_to(ROOT)}")


def main() -> None:
    # Prop meshes must carry three index-buffer LODs, not just one full mesh.
    require(SCENE_H, "PropLOD lods[3]")
    require(SCENE_H, "float lifeDist")
    require(SCENE_H, "propDrawnLOD0")

    # Rendering must choose object LODs by distance and prune far instances.
    require(SCENE_CPP, "chooseObjectLOD")
    require(SCENE_CPP, "stochastic pruning for far props")
    require(SCENE_CPP, "drawPropSubmeshes")

    # The HUD must expose the stats so the LOD/pruning behavior is visible.
    require(MAIN_CPP, "Props LOD0/1/2")
    require(MAIN_CPP, "Props pruned")

    print("check_lod_visibility: OK")


if __name__ == "__main__":
    main()
