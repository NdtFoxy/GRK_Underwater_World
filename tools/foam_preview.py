"""Offscreen shore-foam previewer (photoreal target).

GPU-less iteration aid: mirrors the GLSL foam math (assets/shaders/lib/foam_lib.glsl)
in numpy so the shoreline look can be tuned and *seen* without running the engine.
Renders a top-down "water meets beach" scene to PNG. Tune here, then port the same
algorithm/params back to the shaders (water.frag = water side, terrain.frag = sand).
"""
import numpy as np
from PIL import Image

# ----------------------------------------------------------------------------
# GLSL-style helpers (vectorized over HxW)
# ----------------------------------------------------------------------------
def fract(x): return x - np.floor(x)
def clamp(x, a, b): return np.clip(x, a, b)
def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)
def mix(a, b, t): return a * (1.0 - t) + b * t

def hash21(px, py):
    p3x = fract(px * 0.1031); p3y = fract(py * 0.1031); p3z = fract(px * 0.1031)
    d = p3x * (p3y + 33.33) + p3y * (p3z + 33.33) + p3z * (p3x + 33.33)
    p3x += d; p3y += d; p3z += d
    return fract((p3x + p3y) * p3z)

def value_noise(px, py):
    ix, iy = np.floor(px), np.floor(py)
    fx, fy = px - ix, py - iy
    ux = fx * fx * (3 - 2 * fx); uy = fy * fy * (3 - 2 * fy)
    a = hash21(ix, iy); b = hash21(ix + 1, iy)
    c = hash21(ix, iy + 1); d = hash21(ix + 1, iy + 1)
    return mix(mix(a, b, ux), mix(c, d, ux), uy)

def fbm(px, py, oct=5):
    v = np.zeros_like(px); amp = 0.5; x, y = px.copy(), py.copy()
    for _ in range(oct):
        v += amp * value_noise(x, y); x = x * 2.02 + 11.3; y = y * 2.02 + 7.7; amp *= 0.5
    return v

def hash22(px, py):
    p3x = fract(px * 0.1031); p3y = fract(py * 0.1030); p3z = fract(px * 0.0973)
    d = p3x * (p3y + 33.33) + p3y * (p3z + 33.33) + p3z * (p3x + 33.33)
    p3x += d; p3y += d; p3z += d
    return fract((p3x + p3y) * p3z), fract((p3x + p3z) * p3y)

def worley(px, py):
    ipx, ipy = np.floor(px), np.floor(py)
    fpx, fpy = px - ipx, py - ipy
    f1 = np.full_like(px, 9.0)
    for gy in (-1, 0, 1):
        for gx in (-1, 0, 1):
            ox, oy = hash22(ipx + gx, ipy + gy)
            rx = gx + ox - fpx; ry = gy + oy - fpy
            f1 = np.minimum(f1, rx * rx + ry * ry)
    return np.sqrt(f1)

def foam_bubbles(wx, wz, t, scale):
    """Fine bubbly detail in [0,1] — the dissolve mask. FINE cells = real foam."""
    w1 = worley(wx * scale + t * 0.7,        wz * scale - t * 0.5)
    w2 = worley(wx * scale * 2.1 - t * 0.9,  wz * scale * 2.1 + t * 0.6)
    w3 = worley(wx * scale * 4.3 + t * 1.3,  wz * scale * 4.3 - t * 0.4)
    f = (1 - smoothstep(0, 1, w1)) * 0.5 + (1 - smoothstep(0, 1, w2)) * 0.32 + (1 - smoothstep(0, 1, w3)) * 0.18
    f = f * (0.7 + 0.5 * fbm(wx * scale * 1.3, wz * scale * 1.3, 3))
    return clamp(f, 0, 1)

def dissolve(amount, d, rec):
    thr = clamp(1.0 - amount + rec * 0.18, 0.0, 1.0)
    return clamp(smoothstep(thr - 0.16, thr + 0.10, d) * (0.6 + 0.4 * d), 0, 1)

# ----------------------------------------------------------------------------
def render(width=1400, height=900, swash_t=0.0, fname="foam_preview.png"):
    WX0, WX1, WZ0, WZ1 = 0.0, 44.0, 0.0, 28.0
    xs = np.linspace(WX0, WX1, width); zs = np.linspace(WZ0, WZ1, height)
    wx, wz = np.meshgrid(xs, zs)
    t = 7.0
    shore_x = 18.0 + 4.5 * np.sin(wz * 0.22) + 2.4 * fbm(wz * 0.18, np.zeros_like(wz) + 3.0, 4)
    dist = wx - shore_x                 # >0 water, <0 land
    depth = np.maximum(dist * 0.16, 0.0)

    # --- swash: the wash front rushes UP the beach (onto land, dist<0) then
    #     slides back toward the sea. `front` is the dist of the wash edge. ---
    SWASH = 6.0
    p = swash_t % 1.0
    rush = smoothstep(0.0, 0.28, p); back = smoothstep(0.28, 1.0, p)
    front = -SWASH * rush + (SWASH + 0.6) * back     # neg = up the beach, pos = receded
    recede = back
    wash = dist - front                              # >0 seaward of front, <0 up-beach of it

    # ---------------- water colour (luminous tropical) ----------------------
    sand_dry = np.array([0.85, 0.80, 0.68])
    sand_lit = np.array([0.62, 0.86, 0.80])
    shallow  = np.array([0.16, 0.80, 0.82])
    middeep  = np.array([0.03, 0.55, 0.66])
    deep     = np.array([0.01, 0.30, 0.50])
    water = mix(sand_lit, shallow, smoothstep(0.0, 2.2, depth)[..., None])
    water = mix(water, middeep, smoothstep(1.5, 7.0, depth)[..., None])
    water = mix(water, deep, smoothstep(6.0, 16.0, depth)[..., None])
    caust = smoothstep(0.62, 0.96, fbm(wx * 0.55 + t * 0.1, wz * 0.55, 4)) * smoothstep(11, 1.5, depth)
    water += (0.12 * caust)[..., None] * np.array([0.7, 1.0, 0.95])
    water += (0.13 * smoothstep(0.74, 0.97, fbm(wx * 0.12 + wz * 0.5, wz * 0.1 - wx * 0.04, 4)))[..., None]

    # ---------------- sand, damp sand, swash sheet, foam --------------------
    bump = fbm(wx * 0.7, wz * 1.4, 4)
    sand = sand_dry * (0.92 + 0.10 * bump)[..., None]
    onland = smoothstep(0.5, -0.3, dist)                    # 1 on land, 0 in water

    # Damp sand: band just above the wash front the water recently left, drying
    # a few metres up the beach.
    above_front = smoothstep(front - 0.2, front + 0.4, dist) * onland
    wet = clamp(above_front * (1.0 - smoothstep(front, front + 4.0, dist)), 0, 1) ** 0.85
    wet_sand = sand * mix(1.0, 0.46, wet)[..., None]
    wet_sand += (0.10 * wet * smoothstep(0.3, 0.9, bump))[..., None] * np.array([0.5, 0.68, 0.82])
    sand = mix(sand, wet_sand, wet[..., None])

    # Swash SHEET: a thin film of clear water now covering the sand from the
    # waterline up to the wash front — sand shows through, glossy & sky-tinted,
    # thinning toward the front. This is the wave "running onto the beach".
    covered = smoothstep(front - 0.3, front + 0.5, dist)    # 1 seaward of the front
    sheet = covered * onland * (1.0 - recede * 0.45)
    sheet_thk = clamp(sheet * smoothstep(front - 1.0, 0.8, dist), 0, 1)
    sheet_col = np.array([0.30, 0.78, 0.80])
    sand = mix(sand, mix(sand, sheet_col, 0.55), sheet_thk[..., None])
    sand += (0.16 * sheet_thk * smoothstep(0.45, 0.92, bump))[..., None] * np.array([0.6, 0.8, 0.9])

    see = smoothstep(0.0, 2.0, depth)                       # softens water->sand edge
    body = mix(sand, water, see[..., None])
    img = np.where((dist > 0.0)[..., None], body, sand)

    # ---------------- foam: THIN lacy line riding the wash front ------------
    along = wz
    D = foam_bubbles(wx, wz, t, scale=5.5)
    edge = smoothstep(0.6, 0.0, np.abs(wash))              # thin crisp leading edge
    fizz = smoothstep(2.0, 0.0, wash) * smoothstep(-0.8, 0.3, wash)  # lacy fizz behind front
    fingers = smoothstep(0.55, 0.92, fbm(along * 0.9, dist * 0.35 - t * 0.2, 4))
    A_edge = clamp(edge, 0, 1)
    A_lace = clamp(fizz * 0.7 + edge * fingers * 0.6, 0, 1)
    foam = np.maximum(dissolve(A_edge, D, recede * 0.5), dissolve(A_lace, D, recede) * 0.9)
    foam *= smoothstep(5.0, 0.5, np.abs(wash))             # confine near the front
    foam_op = foam * mix(0.5, 0.9, smoothstep(0.2, 0.7, foam))
    thinF = np.array([0.86, 0.93, 0.96]); thickF = np.array([0.99, 1.0, 1.0])
    foam_col = mix(thinF, thickF, smoothstep(0.3, 0.8, foam)[..., None])
    img = mix(img, foam_col, foam_op[..., None])

    img = clamp(img, 0, 1) ** (1.0 / 1.08)
    Image.fromarray((clamp(img, 0, 1) * 255).astype(np.uint8)).save(fname)
    print("wrote", fname)

if __name__ == "__main__":
    render(swash_t=0.22, fname="tools/foam_preview_a.png")   # wash rushed up the beach
    render(swash_t=0.62, fname="tools/foam_preview_b.png")   # wash receding
