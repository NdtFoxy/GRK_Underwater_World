"""
Heightmap generator / editor for Project Underworld.

This tool produces RGB PNG heightmaps that match the project's
"zmapGradient" encoding (see src/core/HeightmapLoader.cpp), plus the
Subnautica-style biome masks (M_Lava / M_Castle / M_River) the engine
reads to light up biomes.

Usage:
    # List built-in procedural presets
    python generate_heightmap.py list

    # Generate a preset to a PNG (asks for coverage interactively)
    python generate_heightmap.py preset world --out world.png
    python generate_heightmap.py preset deep  --coverage full --masks --out deep.png

    # Open the VISUAL editor (paint terrain + biomes, apply to project)
    python generate_heightmap.py editor
    python generate_heightmap.py editor --load assets/textures/world/T_World_Heightmap.png

Dependencies:  numpy, Pillow
    pip install numpy pillow
"""

from __future__ import annotations

import argparse
import os
import sys
from dataclasses import dataclass

import numpy as np
from PIL import Image

# --------------------------------------------------------------------
# zmapGradient — RGB color encoding for depth.
# Mirrors the table in HeightmapLoader.cpp. The C++ decoder samples the
# piecewise-linear path between adjacent keys, so we encode the same.
# --------------------------------------------------------------------
GRADIENT = [
    # (depth,  R,    G,    B)
    ( 160.0, 196, 196, 255),
    (   1.0, 160, 128,  16),
    (  -1.0,   0, 255,   0),
    (-100.0, 128,   0,   0),
    (-200.0,   0,   0,   0),
    (-300.0,   0,   0, 255),
    (-400.0, 128,  64,   0),
    (-500.0, 255, 255, 255),
    (-3040.0,  0,   0,   0),
]
DEPTH_MIN = GRADIENT[-1][0]   # -3040
DEPTH_MAX = GRADIENT[ 0][0]   # +160


def depth_to_rgb(depth: np.ndarray) -> np.ndarray:
    """Convert depths into an RGB uint8 image using the zmapGradient.
    Piecewise linear between adjacent keys; out-of-range clamps."""
    depth = np.clip(depth, DEPTH_MIN, DEPTH_MAX)
    out = np.zeros(depth.shape + (3,), dtype=np.float32)
    for (d_hi, r_hi, g_hi, b_hi), (d_lo, r_lo, g_lo, b_lo) in zip(GRADIENT, GRADIENT[1:]):
        mask = (depth <= d_hi) & (depth >= d_lo)
        if not mask.any():
            continue
        t = (d_hi - depth[mask]) / (d_hi - d_lo)
        out[mask, 0] = r_hi + t * (r_lo - r_hi)
        out[mask, 1] = g_hi + t * (g_lo - g_hi)
        out[mask, 2] = b_hi + t * (b_lo - b_hi)
    return np.clip(out, 0, 255).astype(np.uint8)


def encode_with_alpha(depth: np.ndarray, mask: np.ndarray) -> Image.Image:
    """Encode depth + 'is terrain' mask into an RGBA image.
    mask==False -> alpha=0 (NO_TERRAIN hole in the world)."""
    rgb = depth_to_rgb(depth)
    h, w = depth.shape
    rgba = np.zeros((h, w, 4), dtype=np.uint8)
    rgba[..., :3] = rgb
    rgba[..., 3] = np.where(mask, 255, 0).astype(np.uint8)
    return Image.fromarray(rgba, mode="RGBA")


def encode_biome_mask(intensity: np.ndarray, rgb_active) -> Image.Image:
    """0..1 biome intensity -> RGBA mask the engine understands.
    Active pixels get rgb_active with alpha=intensity*255; inactive
    pixels alpha=0. The terrain shader keys on warm/saturated colour
    plus alpha to turn each biome on."""
    h, w = intensity.shape
    out = np.zeros((h, w, 4), dtype=np.uint8)
    a = np.clip(intensity, 0.0, 1.0)
    out[..., 0] = rgb_active[0]
    out[..., 1] = rgb_active[1]
    out[..., 2] = rgb_active[2]
    out[..., 3] = (a * 255.0).astype(np.uint8)
    return Image.fromarray(out, mode="RGBA")


# --------------------------------------------------------------------
# Noise helpers
# --------------------------------------------------------------------
def _value_noise(shape, freq, seed):
    rng = np.random.default_rng(seed)
    gw = max(2, int(shape[1] * freq) + 1)
    gh = max(2, int(shape[0] * freq) + 1)
    grid = rng.random((gh, gw), dtype=np.float32)
    ys = np.linspace(0, gh - 1, shape[0], dtype=np.float32)
    xs = np.linspace(0, gw - 1, shape[1], dtype=np.float32)
    xv, yv = np.meshgrid(xs, ys)
    x0 = np.floor(xv).astype(np.int32)
    y0 = np.floor(yv).astype(np.int32)
    x1 = np.clip(x0 + 1, 0, gw - 1)
    y1 = np.clip(y0 + 1, 0, gh - 1)
    fx = xv - x0
    fy = yv - y0
    fx = fx * fx * (3 - 2 * fx)
    fy = fy * fy * (3 - 2 * fy)
    a = grid[y0, x0]; b = grid[y0, x1]
    c = grid[y1, x0]; d = grid[y1, x1]
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy


def fbm(shape, base_freq=0.005, octaves=6, persistence=0.5, lacunarity=2.0, seed=0):
    out = np.zeros(shape, dtype=np.float32)
    amp, freq, norm = 1.0, base_freq, 0.0
    for o in range(octaves):
        out += amp * _value_noise(shape, freq, seed + o * 17)
        norm += amp
        amp *= persistence
        freq *= lacunarity
    return out / norm


def radial_falloff(shape, center=(0.5, 0.5), inner=0.2, outer=0.85):
    h, w = shape
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    cy = h * center[1]; cx = w * center[0]
    dx = (xx - cx) / max(w, h)
    dy = (yy - cy) / max(w, h)
    r = np.sqrt(dx * dx + dy * dy) * 2.0
    t = np.clip((outer - r) / (outer - inner + 1e-6), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def ridged_fbm(shape, base_freq=0.004, octaves=6, seed=0):
    """Ridged multifractal — sharp mountain ridges / canyon walls."""
    out = np.zeros(shape, dtype=np.float32)
    amp, freq, norm, prev = 1.0, base_freq, 0.0, 1.0
    for o in range(octaves):
        n = _value_noise(shape, freq, seed + o * 31)
        ridge = 1.0 - np.abs(2.0 * n - 1.0)
        ridge = ridge * ridge
        out += ridge * amp * prev
        prev = np.clip(ridge, 0.0, 1.0)
        norm += amp
        amp *= 0.5
        freq *= 2.0
    return out / norm


def thermal_erosion(depth, iterations=4, talus=2.5, factor=0.4):
    """Slumps material on steep slopes into natural talus."""
    d = depth.copy()
    for _ in range(iterations):
        dn = np.zeros_like(d)
        for ax, sh in [(0, 1), (0, -1), (1, 1), (1, -1)]:
            diff = d - np.roll(d, sh, axis=ax)
            move = np.where(diff > talus, (diff - talus) * factor * 0.25, 0.0)
            dn -= move
            dn += np.roll(move, -sh, axis=ax)
        d += dn
    return d


def smoothstep_np(a, b, x):
    t = np.clip((x - a) / (b - a + 1e-6), 0.0, 1.0)
    return t * t * (3 - 2 * t)


# --------------------------------------------------------------------
# Procedural presets
# --------------------------------------------------------------------
@dataclass
class PresetResult:
    depth: np.ndarray
    mask:  np.ndarray
    lava:   np.ndarray = None
    castle: np.ndarray = None
    river:  np.ndarray = None


def preset_atoll(size: int, seed: int = 42, coverage: str = "full") -> PresetResult:
    shape = (size, size)
    base = fbm(shape, base_freq=0.004, octaves=6, seed=seed)
    detail = fbm(shape, base_freq=0.020, octaves=5, seed=seed + 100) * 0.4
    h, w = shape
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    cy, cx = h / 2, w / 2
    r = np.sqrt(((xx - cx) / (w / 2)) ** 2 + ((yy - cy) / (h / 2)) ** 2)
    profile = np.zeros_like(r)
    profile = np.where(r < 0.20, -10 - 5 * r, profile)
    profile = np.where((r >= 0.20) & (r < 0.32),  5 - 80 * (r - 0.27) ** 2, profile)
    profile = np.where((r >= 0.32) & (r < 0.55), -20 - 350 * (r - 0.32), profile)
    profile = np.where((r >= 0.55) & (r < 0.85), -250 - 250 * (r - 0.55), profile)
    profile = np.where(r >= 0.85, -500, profile)
    profile += (base - 0.5) * 80.0 * np.exp(-((r - 0.32) ** 2) * 20.0)
    profile += (detail - 0.5) * 18.0
    mask = r < 1.0
    return PresetResult(depth=profile.astype(np.float32), mask=mask)


def preset_trench(size: int, seed: int = 7, coverage: str = "full") -> PresetResult:
    shape = (size, size)
    h, w = shape
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    u = yy / h
    cx = w * (0.5 + 0.10 * np.sin(u * 4.0))
    dx = (xx - cx) / (w / 2)
    base = fbm(shape, base_freq=0.0035, octaves=6, seed=seed)
    fine = fbm(shape, base_freq=0.030, octaves=4, seed=seed + 21) * 0.3
    a = np.abs(dx)
    profile = np.where(a < 0.08, -2800 + 200 * (1 - a / 0.08), 0.0)
    profile = np.where((a >= 0.08) & (a < 0.20), -2600 + 2600 * ((a - 0.08) / 0.12), profile)
    profile = np.where((a >= 0.20) & (a < 0.45), -10 + 30 * np.cos((a - 0.20) * 12.0), profile)
    profile = np.where(a >= 0.45, -150 - 250 * ((a - 0.45) / 0.55), profile)
    profile += (base - 0.5) * 90.0
    profile += fine * 25.0
    mask = np.ones(shape, dtype=bool)
    return PresetResult(depth=profile.astype(np.float32), mask=mask)


def preset_archipelago(size: int, seed: int = 99, coverage: str = "full") -> PresetResult:
    shape = (size, size)
    base = fbm(shape, base_freq=0.0035, octaves=7, seed=seed)
    medium = fbm(shape, base_freq=0.012, octaves=5, seed=seed + 50)
    fine = fbm(shape, base_freq=0.045, octaves=4, seed=seed + 200)
    combined = 0.45 * base + 0.40 * medium + 0.15 * fine
    fall = radial_falloff(shape, inner=0.30, outer=0.95)
    h_norm = combined - 0.50
    depth = np.zeros(shape, dtype=np.float32)
    depth = np.where(h_norm > 0.07, 5 + 90 * (h_norm - 0.07), depth)
    depth = np.where((h_norm > -0.05) & (h_norm <= 0.07), -50 + 800 * (h_norm), depth)
    depth = np.where((h_norm > -0.20) & (h_norm <= -0.05), -250 + 1300 * (h_norm + 0.05), depth)
    depth = np.where(h_norm <= -0.20, -300 - 600 * (-h_norm - 0.20), depth)
    depth = depth * (0.5 + 0.5 * fall) - 250.0 * (1.0 - fall)
    mask = np.ones(shape, dtype=bool)
    return PresetResult(depth=depth.astype(np.float32), mask=mask)


def preset_world(size: int, seed: int = 1234, coverage: str = "full") -> PresetResult:
    """Main world: sandy shelf, rocky ridges, canyons, abyss and
    Subnautica-style biome zones. `coverage` controls how much of the
    square is terrain (full / half / quarter) so it doesn't look like a
    circle cut into a box."""
    shape = (size, size)
    h, w = shape
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    cx, cy = w / 2.0, h / 2.0
    nx = (xx - cx) / (w * 0.5)
    ny = (yy - cy) / (h * 0.5)
    r = np.sqrt(nx * nx + ny * ny)

    if coverage == "full":
        inner, outer, edgeDrop = 0.50, 1.60, 110.0
    elif coverage == "half":
        inner, outer, edgeDrop = 0.20, 1.00, 300.0
    else:
        inner, outer, edgeDrop = 0.10, 0.65, 380.0

    base    = fbm(shape, base_freq=0.0030, octaves=7, seed=seed)
    ridges  = ridged_fbm(shape, base_freq=0.0050, octaves=6, seed=seed + 11)
    detail  = fbm(shape, base_freq=0.020, octaves=5, seed=seed + 77)
    rolling = fbm(shape, base_freq=0.010, octaves=5, seed=seed + 55)  # mid hills
    warm    = fbm(shape, base_freq=0.006, octaves=4, seed=seed + 303)

    # The engine's HeightmapLoader now allows heights up to ~+60, so we
    # let the strongest ridges rise ABOVE the waterline (Y=0) to form
    # rocky islands with beaches. Most of the world stays submerged.
    shelf = -8.0 - rolling * 26.0            # gentle shelf -8 .. -34
    # Ridges rise toward — and through — the surface where base is high.
    ridgeMask = np.clip((base - 0.45) * 2.5, 0.0, 1.0)
    shelf += (ridges ** 1.6) * 34.0 * ridgeMask   # underwater mountains
    # Island peaks: where ridges AND base are strong, push above water.
    # Lower thresholds + higher amplitude so islands clearly breach the
    # surface (heightmap up to ~+55 → world +22 after heightScale 0.40).
    islandMask = np.clip((base - 0.55) * 4.0, 0.0, 1.0) * np.clip((ridges - 0.30) * 2.2, 0.0, 1.0)
    shelf += (ridges ** 1.6) * 95.0 * islandMask
    # Deeper basins where base is low.
    shelf -= (1.0 - base) * 50.0

    seabed = -70.0 - (r ** 2.0) * edgeDrop
    interior = radial_falloff(shape, inner=inner, outer=outer)
    depth = shelf * interior + seabed * (1.0 - interior)

    canyon = ridged_fbm(shape, base_freq=0.0035, octaves=4, seed=seed + 200)
    canyonMask = smoothstep_np(0.74, 0.92, canyon) * interior
    depth -= canyonMask * 60.0
    depth += (detail - 0.5) * 10.0

    # Keep a circular patch of open water at the map centre so the boat
    # has somewhere to float (the boat spawns near world origin).
    centerWater = radial_falloff(shape, inner=0.0, outer=0.12)
    depth = np.where(centerWater > 0.01,
                     np.minimum(depth, -6.0 - centerWater * 8.0),
                     depth)

    depth = thermal_erosion(depth, iterations=4, talus=2.5, factor=0.5)
    # Stay within the loader's usable range; islands up to ~+55.
    depth = np.clip(depth, -290.0, 55.0)

    mask = np.ones(shape, dtype=bool)

    # ---- biome masks (FIXED so lava is only in the deep, not everywhere) ----
    # LAVA: deep zones only (depth more negative than ~-150), patchy.
    deepZone = smoothstep_np(-150.0, -250.0, depth)       # 0 shallow → 1 deep
    lava = np.clip(deepZone * smoothstep_np(0.5, 0.78, warm) * 1.3, 0.0, 1.0).astype(np.float32)
    # CASTLE / grassy shelf: the shallow flat reef (depth -6..-40).
    shelfBand = smoothstep_np(-45.0, -10.0, depth)
    castle = np.clip(shelfBand * smoothstep_np(0.5, 0.7, base) * 0.9, 0.0, 1.0).astype(np.float32)
    # RIVER / kelp channels: along the canyons, mid depth.
    river = np.clip(canyonMask * smoothstep_np(-120.0, -25.0, depth), 0.0, 1.0).astype(np.float32)

    # The whole world is solid terrain (no cave holes). The terrain
    # presence mask stays fully valid.
    return PresetResult(depth=depth.astype(np.float32), mask=mask,
                        lava=lava, castle=castle, river=river)


def preset_deep(size: int, seed: int = 4321, coverage: str = "full") -> PresetResult:
    """Abyssal layer: deep lava basin, towering rock pillars. Everything
    is far below the surface — a 'go deeper' level where the engine's
    depth-darkness makes the flashlight essential."""
    shape = (size, size)
    h, w = shape
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    cx, cy = w / 2.0, h / 2.0
    nx = (xx - cx) / (w * 0.5)
    ny = (yy - cy) / (h * 0.5)
    r = np.sqrt(nx * nx + ny * ny)

    outer = {"full": 1.6, "half": 1.0}.get(coverage, 0.65)
    interior = radial_falloff(shape, inner=0.25, outer=outer)

    base   = fbm(shape, base_freq=0.0035, octaves=7, seed=seed)
    ridges = ridged_fbm(shape, base_freq=0.0065, octaves=7, seed=seed + 13)
    detail = fbm(shape, base_freq=0.025, octaves=5, seed=seed + 90)

    floor = -320.0 - (1.0 - base) * 120.0
    pillars = (ridges ** 2.2) * 320.0
    pillarMask = np.clip((ridges - 0.45) * 4.0, 0.0, 1.0)
    depth = floor + pillars * pillarMask
    depth = depth * interior + (-560.0 - r * 120.0) * (1.0 - interior)
    depth += (detail - 0.5) * 20.0
    depth = thermal_erosion(depth, iterations=3, talus=4.0, factor=0.5)
    depth = np.clip(depth, -700.0, -40.0)

    mask = np.ones(shape, dtype=bool)
    warm = fbm(shape, base_freq=0.005, octaves=4, seed=seed + 500)
    centreFall = radial_falloff(shape, inner=0.0, outer=0.55)
    lava = np.clip(centreFall * smoothstep_np(0.4, 0.7, warm) * 1.6, 0.0, 1.0).astype(np.float32)
    river = np.clip(smoothstep_np(0.85, 0.97, ridged_fbm(shape, 0.004, 4, seed + 600))
                    * (1.0 - centreFall), 0.0, 1.0).astype(np.float32)
    castle = np.zeros(shape, dtype=np.float32)

    # Solid terrain (no cave holes).
    return PresetResult(depth=depth.astype(np.float32), mask=mask,
                        lava=lava, castle=castle, river=river)


PRESETS = {
    "world":        ("Main world: shelf, ridges, canyons, abyss + biomes.",            preset_world),
    "deep":         ("Abyssal layer: deep lava basin, towering rock pillars.",         preset_deep),
    "atoll":        ("Circular reef around shallow lagoon, surrounded by deep ocean.", preset_atoll),
    "trench":       ("Deep oceanic trench flanked by underwater ridges.",              preset_trench),
    "archipelago":  ("Multiple islands on a continental shelf.",                       preset_archipelago),
}


def write_all(result: PresetResult, out_path: str, also_masks: bool):
    """Write the heightmap PNG and (optionally) the three biome masks."""
    encode_with_alpha(result.depth, result.mask).save(out_path)
    if also_masks and result.lava is not None:
        out_dir = os.path.dirname(os.path.abspath(out_path))
        for fname, field, col in [
            ("M_Lava_Depth_Mask.png",   result.lava,   (255, 60, 20)),
            ("M_Castle_Depth_Mask.png", result.castle, (120, 200, 120)),
            ("M_River_Depth_Mask.png",  result.river,  (60, 160, 255)),
        ]:
            if field is not None:
                encode_biome_mask(field, col).save(os.path.join(out_dir, fname))


# --------------------------------------------------------------------
# Visual editor (tkinter) — paint terrain depth AND biome masks, load
# presets with coverage, generate default test masks, apply to project.
# --------------------------------------------------------------------
def run_editor(size: int, load_path: str | None):
    import tkinter as tk
    from tkinter import filedialog, messagebox
    from PIL import ImageTk

    # ----- state: depth + terrain mask + 3 biome fields ---------------
    st = {
        "depth":  np.full((size, size), -120.0, dtype=np.float32),
        "tmask":  np.ones((size, size), dtype=bool),     # terrain present
        "lava":   np.zeros((size, size), dtype=np.float32),
        "castle": np.zeros((size, size), dtype=np.float32),
        "river":  np.zeros((size, size), dtype=np.float32),
        "layer":  "depth",     # which thing the brush paints
        "depth_val": -10.0,
        "brush":  max(10, size // 30),
        "hard":   0.5,
        "coverage": "full",
        "last":   None,
    }

    if load_path and os.path.exists(load_path):
        img = Image.open(load_path).convert("RGBA")
        if img.size != (size, size):
            img = img.resize((size, size), Image.BILINEAR)
        # We can't perfectly decode depth back, so keep the loaded RGB
        # as a visual base and let the user repaint. Terrain mask from a.
        arr = np.array(img)
        st["tmask"] = arr[..., 3] > 127

    root = tk.Tk()
    root.title("Underworld World Builder")
    root.configure(bg="#181818")
    DISPLAY = min(820, size)

    canvas = tk.Canvas(root, width=DISPLAY, height=DISPLAY, bg="black",
                       highlightthickness=0)
    canvas.grid(row=0, column=0, rowspan=40, padx=8, pady=8)
    tkimg = [None]; cid = [None]

    # ---- compose a colour preview of the current state ----
    def compose_preview():
        # Base: depth gradient RGB, darkened by absolute depth so the
        # deep abyss reads black (matches the in-engine darkening).
        rgb = depth_to_rgb(st["depth"]).astype(np.float32)
        # Depth darkness preview: deeper => darker.
        dn = np.clip(1.0 + st["depth"] / 220.0, 0.05, 1.0)[..., None]
        rgb *= dn
        # Overlay biome tints.
        rgb[..., 0] += st["lava"]   * 180.0
        rgb[..., 1] += st["castle"] * 120.0
        rgb[..., 2] += st["river"]  * 150.0
        return np.clip(rgb, 0, 255).astype(np.uint8)

    def refresh():
        img = Image.fromarray(compose_preview(), "RGB")
        if img.size != (DISPLAY, DISPLAY):
            img = img.resize((DISPLAY, DISPLAY), Image.NEAREST)
        photo = ImageTk.PhotoImage(img)
        tkimg[0] = photo
        if cid[0] is None:
            cid[0] = canvas.create_image(0, 0, anchor="nw", image=photo)
        else:
            canvas.itemconfig(cid[0], image=photo)

    # ---- brush stamping on the active layer ----
    def stamp(cx, cy, erase):
        r = st["brush"]
        x0 = max(0, cx - r); x1 = min(size, cx + r + 1)
        y0 = max(0, cy - r); y1 = min(size, cy + r + 1)
        if x0 >= x1 or y0 >= y1:
            return
        yy, xx = np.mgrid[y0:y1, x0:x1].astype(np.float32)
        d = np.sqrt((xx - cx) ** 2 + (yy - cy) ** 2)
        core = max(1.0, r * st["hard"])
        t = np.clip((r - d) / max(1.0, r - core), 0.0, 1.0)
        wsel = t * t * (3 - 2 * t)
        wsel[d > r] = 0.0

        layer = st["layer"]
        if layer == "depth":
            tgt = st["depth_val"]
            cur = st["depth"][y0:y1, x0:x1]
            st["depth"][y0:y1, x0:x1] = cur * (1 - wsel) + tgt * wsel
        else:  # a biome field
            fld = st[layer][y0:y1, x0:x1]
            if erase:
                st[layer][y0:y1, x0:x1] = np.clip(fld - wsel, 0.0, 1.0)
            else:
                st[layer][y0:y1, x0:x1] = np.clip(fld + wsel, 0.0, 1.0)

    def to_xy(e):
        return int(e.x * size / DISPLAY), int(e.y * size / DISPLAY)

    def press(e, btn):
        ix, iy = to_xy(e); st["last"] = (ix, iy, btn)
        stamp(ix, iy, btn == "right"); refresh()

    def drag(e):
        if st["last"] is None:
            return
        ix, iy = to_xy(e); lx, ly, btn = st["last"]
        steps = max(1, int(np.hypot(ix - lx, iy - ly)))
        for i in range(steps + 1):
            f = i / steps
            stamp(int(lx + (ix - lx) * f), int(ly + (iy - ly) * f), btn == "right")
        st["last"] = (ix, iy, btn); refresh()

    def release(e):
        st["last"] = None

    canvas.bind("<ButtonPress-1>", lambda e: press(e, "left"))
    canvas.bind("<ButtonPress-3>", lambda e: press(e, "right"))
    canvas.bind("<B1-Motion>", drag)
    canvas.bind("<B3-Motion>", drag)
    canvas.bind("<ButtonRelease-1>", release)
    canvas.bind("<ButtonRelease-3>", release)

    # ================= right panel =================
    PS = {"bg": "#181818", "fg": "#e0e0e0"}
    col = 1
    rowi = [0]
    def nextrow():
        rowi[0] += 1
        return rowi[0]

    tk.Label(root, text="LAYER (what the brush paints)", **PS).grid(row=nextrow(), column=col, sticky="w", padx=6)
    layer_var = tk.StringVar(value="depth")
    layers = [("Terrain depth", "depth"),
              ("Lava biome", "lava"), ("Castle biome", "castle"), ("River biome", "river")]
    lf = tk.Frame(root, bg="#181818"); lf.grid(row=nextrow(), column=col, sticky="we", padx=6)
    for txt, val in layers:
        tk.Radiobutton(lf, text=txt, value=val, variable=layer_var,
                       command=lambda: st.update(layer=layer_var.get()),
                       bg="#181818", fg="#ddd", selectcolor="#2c4870",
                       activebackground="#181818", anchor="w").pack(fill="x")

    tk.Label(root, text="Depth value (for Terrain layer)", **PS).grid(row=nextrow(), column=col, sticky="w", padx=6)
    dv = tk.DoubleVar(value=-10.0)
    tk.Scale(root, from_=160, to=-600, resolution=1, orient="horizontal", length=240,
             variable=dv, bg="#181818", fg="#ddd", troughcolor="#444", highlightthickness=0,
             command=lambda v: st.update(depth_val=float(v))).grid(row=nextrow(), column=col, padx=6)
    qf = tk.Frame(root, bg="#181818"); qf.grid(row=nextrow(), column=col, sticky="we", padx=6)
    for lab, val in [("Shore", 5), ("Sand", -10), ("Mid", -100), ("Deep", -300), ("Abyss", -550)]:
        tk.Button(qf, text=lab, width=6, relief="flat", bg="#333", fg="#ddd",
                  command=lambda v=val: (dv.set(v), st.update(depth_val=float(v)))).pack(side="left", padx=1)

    tk.Label(root, text="Brush size", **PS).grid(row=nextrow(), column=col, sticky="w", padx=6)
    bv = tk.IntVar(value=st["brush"])
    tk.Scale(root, from_=4, to=size // 3, orient="horizontal", length=240, variable=bv,
             bg="#181818", fg="#ddd", troughcolor="#444", highlightthickness=0,
             command=lambda v: st.update(brush=int(v))).grid(row=nextrow(), column=col, padx=6)
    tk.Label(root, text="Hardness", **PS).grid(row=nextrow(), column=col, sticky="w", padx=6)
    hv = tk.DoubleVar(value=st["hard"])
    tk.Scale(root, from_=0, to=1, resolution=0.05, orient="horizontal", length=240, variable=hv,
             bg="#181818", fg="#ddd", troughcolor="#444", highlightthickness=0,
             command=lambda v: st.update(hard=float(v))).grid(row=nextrow(), column=col, padx=6)

    # ---- preset + coverage ----
    tk.Label(root, text="Coverage", **PS).grid(row=nextrow(), column=col, sticky="w", padx=6)
    cov_var = tk.StringVar(value="full")
    cvf = tk.Frame(root, bg="#181818"); cvf.grid(row=nextrow(), column=col, sticky="we", padx=6)
    for txt, val in [("Full", "full"), ("Half", "half"), ("Quarter", "quarter")]:
        tk.Radiobutton(cvf, text=txt, value=val, variable=cov_var,
                       command=lambda: st.update(coverage=cov_var.get()),
                       bg="#181818", fg="#ddd", selectcolor="#2c4870",
                       activebackground="#181818").pack(side="left")

    def load_preset(name):
        res = PRESETS[name][1](size, coverage=st["coverage"])
        st["depth"][...] = res.depth
        st["tmask"][...] = res.mask
        st["lava"][...]   = res.lava   if res.lava   is not None else 0.0
        st["castle"][...] = res.castle if res.castle is not None else 0.0
        st["river"][...]  = res.river  if res.river  is not None else 0.0
        refresh()

    tk.Label(root, text="Load preset", **PS).grid(row=nextrow(), column=col, sticky="w", padx=6)
    pf = tk.Frame(root, bg="#181818"); pf.grid(row=nextrow(), column=col, sticky="we", padx=6)
    for name in PRESETS:
        tk.Button(pf, text=name, relief="flat", bg="#2c4870", fg="#fff", width=9,
                  command=lambda n=name: load_preset(n)).pack(side="left", padx=1, pady=2)

    # ---- default test masks ----
    def default_masks():
        """Quick procedural biome masks for testing without a preset:
        lava in the deep centre, castle on the shelf, river in mid-band."""
        yy, xx = np.mgrid[0:size, 0:size].astype(np.float32)
        r = np.sqrt(((xx - size/2)/(size/2))**2 + ((yy - size/2)/(size/2))**2)
        st["lava"][...]   = np.clip(smoothstep_np(0.0, 0.35, 0.35 - r), 0, 1) * \
                            smoothstep_np(-260, -100, -st["depth"])
        st["castle"][...] = smoothstep_np(-28, -5, st["depth"]) * (1 - smoothstep_np(-5, 10, st["depth"]))
        st["river"][...]  = smoothstep_np(0.45, 0.6, r) * (1 - smoothstep_np(0.6, 0.8, r))
        refresh()

    tk.Button(root, text="Generate default test biomes", relief="flat",
              bg="#5a3d80", fg="#fff", command=default_masks
              ).grid(row=nextrow(), column=col, sticky="we", padx=6, pady=(8, 2))

    # ---- ONE-CLICK: generate the full world AND apply to project ----
    def one_click_test():
        # Generate the full 'world' preset at full coverage, fill all
        # state, and write straight into the project — the fastest path
        # to "see it in the engine".
        res = PRESETS["world"][1](size, coverage="full")
        st["depth"][...]  = res.depth
        st["tmask"][...]  = res.mask
        st["lava"][...]   = res.lava
        st["castle"][...] = res.castle
        st["river"][...]  = res.river
        refresh()
        target = os.path.abspath(os.path.join("assets", "textures", "world",
                                              "T_World_Heightmap.png"))
        if not os.path.exists(os.path.dirname(target)):
            messagebox.showerror("Not found", "Run from the project root.")
            return
        if os.path.exists(target):
            bak = target + ".bak"
            try:
                if os.path.exists(bak):
                    os.remove(bak)
                os.replace(target, bak)
            except OSError:
                pass
        write_all(build_result(), target, also_masks=True)
        messagebox.showinfo("Done",
                            "Full test world generated AND applied!\n"
                            "Heightmap + all biome masks written.\n\n"
                            "Just restart project_underworld.exe.")

    tk.Button(root, text="★ GENERATE FULL TEST WORLD + APPLY ★", relief="flat",
              bg="#e67e22", fg="#fff", font=("Segoe UI", 11, "bold"),
              command=one_click_test
              ).grid(row=nextrow(), column=col, sticky="we", padx=6, pady=(10, 4))

    # ---- file actions ----
    def build_result():
        return PresetResult(depth=st["depth"], mask=st["tmask"],
                            lava=st["lava"], castle=st["castle"], river=st["river"])

    def save_as():
        path = filedialog.asksaveasfilename(defaultextension=".png",
                                            filetypes=[("PNG", "*.png")],
                                            initialfile="custom_world.png")
        if not path:
            return
        write_all(build_result(), path, also_masks=True)
        messagebox.showinfo("Saved", f"Heightmap + biome masks written to:\n{os.path.dirname(path)}")

    def apply_project():
        target = os.path.abspath(os.path.join("assets", "textures", "world",
                                              "T_World_Heightmap.png"))
        if not os.path.exists(os.path.dirname(target)):
            messagebox.showerror("Not found", "Run from the project root.")
            return
        if not messagebox.askyesno("Apply to project",
                f"Overwrite the project heightmap + biome masks?\n{target}\n\n"
                "Restart the engine to see the new world."):
            return
        if os.path.exists(target):
            bak = target + ".bak"
            try:
                if os.path.exists(bak):
                    os.remove(bak)
                os.replace(target, bak)
            except OSError:
                pass
        write_all(build_result(), target, also_masks=True)
        messagebox.showinfo("Applied",
                            "Wrote T_World_Heightmap.png + M_*_Depth_Mask.png\n"
                            "Restart project_underworld.exe.")

    af = tk.Frame(root, bg="#181818"); af.grid(row=nextrow(), column=col, sticky="we", padx=6, pady=8)
    tk.Button(af, text="Save As", relief="flat", bg="#2e7d32", fg="#fff", width=10,
              command=save_as).pack(side="left", padx=2)
    tk.Button(af, text="Reset", relief="flat", bg="#b71c1c", fg="#fff", width=10,
              command=lambda: (st["depth"].fill(-120.0), st["tmask"].fill(True),
                               st["lava"].fill(0), st["castle"].fill(0),
                               st["river"].fill(0), refresh())).pack(side="left", padx=2)

    tk.Button(root, text="APPLY TO PROJECT (heightmap + masks)", relief="flat",
              bg="#0277bd", fg="#fff", font=("Segoe UI", 10, "bold"),
              command=apply_project).grid(row=nextrow(), column=col, sticky="we", padx=6, pady=(0, 6))

    tk.Label(root, justify="left", anchor="w", bg="#181818", fg="#888",
             font=("Consolas", 8),
             text=("Left = paint   Right = erase\n"
                   "Pick a LAYER, then paint. Presets fill everything.\n"
                   "Preview darkens with depth (like the engine).")
             ).grid(row=nextrow(), column=col, sticky="we", padx=6, pady=(4, 8))

    refresh()
    root.mainloop()


# --------------------------------------------------------------------
# CLI entry point
# --------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(
        description="Heightmap generator/editor for Project Underworld",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd")

    sub.add_parser("list", help="Show available procedural presets")

    p = sub.add_parser("preset", help="Generate a heightmap from a preset")
    p.add_argument("name", choices=list(PRESETS.keys()))
    p.add_argument("--size", type=int, default=1024)
    p.add_argument("--out", type=str, default="generated_heightmap.png")
    p.add_argument("--seed", type=int, default=None)
    p.add_argument("--coverage", choices=["full", "half", "quarter"], default=None,
                   help="How much of the square is terrain (world/deep).")
    p.add_argument("--masks", action="store_true",
                   help="Also write M_*_Depth_Mask.png next to --out.")

    pe = sub.add_parser("editor", help="Open the visual world builder")
    pe.add_argument("--size", type=int, default=1024)
    pe.add_argument("--load", type=str, default=None)

    args = parser.parse_args()

    if args.cmd is None:
        pm = "assets/textures/world/T_World_Heightmap.png"
        run_editor(1024, pm if os.path.exists(pm) else None)
        return

    if args.cmd == "list":
        print("Available presets:\n")
        for name, (desc, _) in PRESETS.items():
            print(f"  {name:<14} {desc}")
        return

    if args.cmd == "preset":
        kwargs = {} if args.seed is None else {"seed": args.seed}
        if args.name in ("world", "deep"):
            cov = args.coverage
            if cov is None:
                print("Coverage: 1) full  2) half  3) quarter")
                choice = input("Choose [1/2/3] (default 1): ").strip()
                cov = {"1": "full", "2": "half", "3": "quarter"}.get(choice, "full")
            kwargs["coverage"] = cov

        result = PRESETS[args.name][1](args.size, **kwargs)

        want_masks = args.masks or os.path.basename(args.out) == "T_World_Heightmap.png"
        write_all(result, args.out, also_masks=want_masks)
        print(f"Wrote {args.out}  ({args.size}x{args.size}, "
              f"depth {result.depth.min():.0f}..{result.depth.max():.0f})")
        if want_masks and result.lava is not None:
            print("  + biome masks written")
        return

    if args.cmd == "editor":
        try:
            import tkinter  # noqa: F401
            from PIL import ImageTk  # noqa: F401
        except ImportError as e:
            print(f"Editor needs tkinter + Pillow ImageTk: {e}", file=sys.stderr)
            sys.exit(1)
        run_editor(args.size, args.load)
        return


if __name__ == "__main__":
    main()
