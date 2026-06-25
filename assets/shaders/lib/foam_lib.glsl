// foam_lib.glsl — shared dynamic shore-foam helpers for water.frag and
// terrain.frag. GLSL 330 core. No main(), no in/out, no uniforms, no #version.
// All symbols are prefixed `foam` so they never collide with the hash/noise
// helpers already in water.frag (hash21/noise2D/fbm2D) or terrain.frag (fbm).
//
// Foam model: a "dissolve". A coverage field A (0..1) says WHERE foam wants to
// be (contact line, breakers, swash tongues); a fine bubbly DETAIL field D
// (foamDetail) is thresholded against (1 - A). High A -> solid foam; low A ->
// only the densest bubbles survive -> lacy, frayed edges. This reads as real
// sea foam, never as cartoon honeycomb (we never draw worley cell *walls*).
#ifndef FOAM_LIB_GLSL
#define FOAM_LIB_GLSL

float foamHash21(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

vec2 foamHash22(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.xx + p3.yz) * p3.zy);
}

float foamValueNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = foamHash21(i);
    float b = foamHash21(i + vec2(1.0, 0.0));
    float c = foamHash21(i + vec2(0.0, 1.0));
    float d = foamHash21(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

float foamFbm(vec2 p) {
    float v = 0.0;
    float a = 0.5;
    for (int i = 0; i < 5; ++i) {
        v += a * foamValueNoise(p);
        p = p * 2.02 + vec2(11.3, 7.7);
        a *= 0.5;
    }
    return v;
}

// Worley (cellular) noise — nearest feature-point distance F1 only. Small F1 =
// near a bubble centre. We use 1-F1 as bright bubble clumps; we never use the
// cell-wall (F2-F1) term, which is what produced the cartoon honeycomb.
float foamWorleyF1(vec2 p) {
    vec2 ip = floor(p);
    vec2 fp = fract(p);
    float f1 = 1.0e9;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 g = vec2(float(x), float(y));
            vec2 o = foamHash22(ip + g);
            vec2 r = g + o - fp;
            f1 = min(f1, dot(r, r));
        }
    }
    return sqrt(f1);
}

// Fine bubbly DETAIL field in 0..1 (the dissolve mask). Three worley octaves of
// packed bubbles plus an fbm grain. `scale` ~2-3 gives ~0.4 m primary cells in
// world space; keep it FINE (do NOT use ~0.6 — that is the honeycomb scale).
float foamBubbles(vec2 p, float scale, float t) {
    float w1 = foamWorleyF1(p * scale        + vec2( t * 0.7, -t * 0.5));
    float w2 = foamWorleyF1(p * scale * 2.1  + vec2(-t * 0.9,  t * 0.6));
    float w3 = foamWorleyF1(p * scale * 4.3  + vec2( t * 1.3, -t * 0.4));
    float f = (1.0 - smoothstep(0.0, 1.0, w1)) * 0.50
            + (1.0 - smoothstep(0.0, 1.0, w2)) * 0.32
            + (1.0 - smoothstep(0.0, 1.0, w3)) * 0.18;
    f *= 0.7 + 0.5 * foamFbm(p * scale * 1.3);
    return clamp(f, 0.0, 1.0);
}

// Apply the dissolve: coverage `amount` (0..1) eroded by detail `d` (0..1).
// `dissolve` (0..1) raises the floor so foam breaks into lace as a wave recedes.
// Returns foam opacity 0..1 with a little internal bubble shading.
float foamDissolve(float amount, float d, float dissolve) {
    float thr = clamp(1.0 - amount + dissolve * 0.18, 0.0, 1.0);
    float foam = smoothstep(thr - 0.16, thr + 0.10, d);
    return clamp(foam * (0.6 + 0.4 * d), 0.0, 1.0);
}

// Swash cycle. `phase` wraps every 1.0. Returns vec2(reach, recede):
//   reach  — how far (scaled by `amplitude`) the wash extends up the slope.
//            Rushes up fast, slides back slow.
//   recede — 0 on the way in, ramps to 1 on the slow pull-back. Drives dissolve.
vec2 foamSwash(float phase, float amplitude) {
    float p = fract(phase);
    float up   = smoothstep(0.00, 0.30, p);
    float down = smoothstep(0.30, 1.00, p);
    float reach = (up - down) * amplitude;
    return vec2(reach, down);
}

// Stacked breaker lines advancing toward shore. `coord` is the cross-shore
// coordinate (shore distance, swash already applied); `warp` bends the lines
// along the contour; `t` is time; `speed` scales advance; `gain` scales output.
float foamBreakerBands(float coord, float warp, float t, float speed, float gain) {
    float foam = 0.0;
    for (int i = 0; i < 3; ++i) {
        float fi = float(i);
        float phase = coord * (1.0 + 0.16 * fi)
                    - t * (0.9 + speed * 0.8) * (1.0 + 0.15 * fi)
                    + warp - fi * 0.8;
        float s = sin(phase) * 0.5 + 0.5;
        float band = pow(smoothstep(0.55, 0.98, s), 1.3 + 0.5 * fi);
        foam = max(foam, band * (1.0 - fi * 0.22));
    }
    return clamp(foam * gain, 0.0, 1.0);
}

#endif // FOAM_LIB_GLSL
