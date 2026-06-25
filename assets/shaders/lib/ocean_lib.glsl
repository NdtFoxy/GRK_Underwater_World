// ocean_lib.glsl — near-shore ocean dynamics shared by the water surface
// (water_tess.tese) and the shore surf strip (surf.vert).
// No main(), no in/out, no uniforms, no #version — this is a pure include of
// helper functions. Every symbol is prefixed `ocean` so it can never collide
// with a local in the including shader. These helpers act ONLY inside the
// near-shore band; the open-ocean Gerstner sum (which the CPU buoyancy
// mirrors, so boats match the surface) is computed in the .tese itself.
#ifndef OCEAN_LIB_GLSL
#define OCEAN_LIB_GLSL

// Green's-law shoaling. As the seabed shallows, swell energy compresses so the
// wave height GROWS (~depth^-1/4) through the surf band, then is killed to zero
// over land. `depth` = metres of water column (>=0); `mask` = valid-terrain (sd.g).
// Open ocean (mask 0) keeps full amplitude (returns 1).
float oceanShoalGrowth(float depth, float mask) {
    float deepFactor = smoothstep(0.25, 7.0, depth);          // 0 at shore, 1 deep
    float growth = 1.0 + 0.35 * exp(-pow((depth - 2.2) / 1.4, 2.0)); // gentle surf-zone bump
    return mix(1.0, deepFactor * growth, mask);
}

// Depth-limited breaking (McCowan criterion ~H/d = 0.78). Returns 0 (unbroken)
// .. 1 (full whitewater) as the local wave height approaches the breaking height.
float oceanBreak(float waveHeight, float depth) {
    float hb = 0.78 * max(depth, 0.05);
    return clamp((abs(waveHeight) - 0.55 * hb) / (0.50 * hb), 0.0, 1.0);
}

// Swash run-up near the waterline. `phase` should be the same time basis the
// fragment foam uses so geometry and foam advance together. Returns vec2:
//   x = surge — vertical lift (m) that pushes the sheet up the beach then drains
//               (fast advance, slow recede), matching foam_lib::foamSwash timing.
//   y = recede — 0 advancing, ->1 draining.
vec2 oceanSwash(float phase, float amplitude) {
    float p = fract(phase);
    float up   = smoothstep(0.00, 0.30, p);
    float down = smoothstep(0.30, 1.00, p);
    return vec2((up - down) * amplitude, down);
}

// Shore-aligned rolling swell (wave refraction). Crests are parallel to the
// shore (iso-distance lines of `shoreDist`) and travel TOWARD land as time
// advances, so you see lines of surf rolling in and breaking. `shoreDist` =
// metres to land (sd.b); `depth` = water column (m). Returns vec3:
//   x = heave — vertical displacement (m), enveloped to 0 at the waterline AND
//               offshore, so it never lifts geometry over dry sand (no slabs).
//   y = crest — 0..1 foam key, peaking on the rolling crest / break line.
//   z = slope — d(heave)/d(shoreDist), used to tilt the normal so the wave is lit.
vec3 oceanShoreWaves(float shoreDist, float depth, float t, float speed, float amp) {
    // Zero at the urez (depth -> 0) and in deep water; peak in the surf zone.
    float env = smoothstep(0.1, 1.1, depth) * (1.0 - smoothstep(6.0, 13.0, depth));
    if (env <= 0.0) return vec3(0.0);
    float heave = 0.0, slope = 0.0, crest = 0.0;
    for (int i = 0; i < 3; ++i) {
        float fi = float(i);
        float wl = 16.0 - 4.0 * fi;                 // 16, 12, 8 m (mesh-friendly)
        float k  = 6.2831853 / wl;
        float omega = (1.7 + 0.5 * fi) * (0.6 + speed * 0.6);  // faster roll-in
        float ph = k * shoreDist + omega * t;       // const phase -> shoreDist falls with t
        float s = sin(ph), cph = cos(ph);
        float w = 1.0 - fi * 0.22;
        heave += s * w;
        slope += k * cph * w;
        crest  = max(crest, smoothstep(0.55, 0.98, s) * w);  // sharp leading face
    }
    float a = amp * env / 2.0;                       // normalize the 3-band sum
    return vec3(heave * a, crest * env, slope * a);
}

#endif // OCEAN_LIB_GLSL
