#version 430 core
#include "lib/ocean_lib.glsl"
// Tessellation evaluation: runs the analytic Gerstner displacement at EVERY tessellated
// vertex, so crests get real geometry (polygons), not just normal-mapped lighting.
// The wave table is the SAME one the CPU buoyancy samples (src/scene/WaveField.h),
// so floating objects sit exactly on this surface.
layout(triangles, fractional_odd_spacing, cw) in;

in vec3 tcLocalPos[];
in vec2 tcUV[];

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform float time;
uniform float uGridHalfExtent;

#define MAX_WAVES 24
uniform vec4  uWave[MAX_WAVES];
uniform float uSteep[MAX_WAVES];
uniform int   uWaveCount;
uniform float uWaveSpeed;
uniform float uWaveAmpScale;
uniform float uQNorm;

uniform sampler2D shoreDataTex;
uniform float shoreTerrainSize;
uniform vec2 shoreTexelSize;

out vec3 FragPos;
out vec2 TexCoords;
out vec3 WorldNormal;
out float WaveHeight;
out float WaveSteep;
out float WaveShoal;
out float WaveBreak;
out float WaveJ;

#define PI 3.14159265

void main() {
    // Barycentric interpolation of the patch corners -> this tessellated vertex.
    vec3 aPos = gl_TessCoord.x * tcLocalPos[0]
              + gl_TessCoord.y * tcLocalPos[1]
              + gl_TessCoord.z * tcLocalPos[2];
    vec2 aTexCoords = gl_TessCoord.x * tcUV[0]
                    + gl_TessCoord.y * tcUV[1]
                    + gl_TessCoord.z * tcUV[2];

    vec3 p = aPos;
    vec2 worldXZ = (model * vec4(aPos, 1.0)).xz;

    float waveAtten = 1.0;
    float depth = 0.0;
    float validTerrain = 0.0;
    float shoreDist = 1.0e9;
    vec2  shoreUV = worldXZ / max(shoreTerrainSize, 1.0) + vec2(0.5);
    bool  inField = shoreUV.x > 0.001 && shoreUV.x < 0.999 &&
                    shoreUV.y > 0.001 && shoreUV.y < 0.999;
    if (inField) {
        vec4 sd = textureLod(shoreDataTex, shoreUV, 0.0);
        depth        = max(0.0, -sd.r);
        validTerrain = sd.g;
        shoreDist    = sd.b;
        waveAtten = oceanShoalGrowth(depth, validTerrain);
    }

    float halfGrid = uGridHalfExtent;
    float edgeFade = 1.0 - smoothstep(halfGrid * 0.80, halfGrid * 0.98, length(aPos.xz));
    waveAtten *= edgeFade;

    // ========================================================================
    // Open-ocean surface: analytic Gerstner (trochoidal) wave sum.
    // ------------------------------------------------------------------------
    // We add up uWaveCount travelling waves. Component i has direction d,
    // amplitude A, wavelength wl, and wavenumber  k = 2*pi/wl. Deep-water
    // gravity waves obey the dispersion relation  c = sqrt(g/k), so LONGER
    // waves travel FASTER — this is what makes the sum read as a real,
    // dispersive ocean instead of a frozen ripple pattern. The phase of a
    // point at world (x,z) and time t is  f = k*(d . xz) - c*t.
    //
    // A Gerstner wave is not just a vertical sine A*sin(f): it also pulls the
    // vertex HORIZONTALLY back toward the crest by Q*A*cos(f). That sharpens
    // crests and flattens troughs — the trochoidal shape of real wind waves.
    // Q in [0,1] is the per-wave steepness; uQNorm scales every Q down just
    // enough that crests can never overhang and self-intersect (the cap is
    // computed once on the CPU in WaveField::computeQNorm()).
    //
    // The normal is the EXACT analytic derivative of the height field (no
    // finite differences, so it's both precise and cheap). The running sum
    // sinSumG = sum(Q*k*A*sin) measures how close the steepest crest is to
    // folding, so we reuse it directly as the whitecap (foam) signal.
    //
    // This loop is identical to WaveField::sample() on the CPU, so a boat's
    // buoyancy query lands EXACTLY on the surface drawn here (no drift).
    // ========================================================================
    vec3  disp = vec3(0.0);              // accumulated displacement (x,y,z)
    vec3  nrm  = vec3(0.0, 1.0, 0.0);    // accumulated surface normal
    float sinSumG = 0.0;                 // crest-fold metric -> whitecap weight
    for (int i = 0; i < uWaveCount; ++i) {
        vec2  d  = normalize(uWave[i].xy);      // unit travel direction
        float A  = uWave[i].z * uWaveAmpScale;  // amplitude (weather-scaled)
        float wl = max(uWave[i].w, 0.5);        // wavelength, metres
        float k  = 2.0 * PI / wl;               // wavenumber
        float c  = sqrt(9.8 / k);               // phase speed (dispersion: g/k)
        float Q  = uSteep[i] * uQNorm;          // steepness, capped vs. folding
        float f  = k * dot(d, worldXZ) - c * time * uWaveSpeed;   // phase
        float sf = sin(f), cf = cos(f);
        // Trochoid: lift vertically, pinch horizontally toward the crest.
        disp.y += A * sf;
        disp.x += Q * A * d.x * cf;
        disp.z += Q * A * d.y * cf;
        // Analytic normal: minus the horizontal gradient of the height field.
        float WA = k * A;
        nrm.x -= d.x * WA * cf;
        nrm.z -= d.y * WA * cf;
        sinSumG += Q * WA * sf;
    }
    nrm.y = max(1.0 - sinSumG, 0.25);    // floor keeps the normal from inverting on a sharp crest
    vec2  nrmH = nrm.xz / nrm.y;         // horizontal slope (to blend with the shore normal below)
    // Whitecaps appear where the trochoid is near folding (sinSumG high).
    float foamSum = smoothstep(0.50, 1.05, sinSumG) * 1.3;

    // Shoaling + grid-edge fade. (No choppiness gain: Q is already normalized
    // right up to the fold limit, so boosting the pinch would self-intersect.)
    disp *= waveAtten;
    float chopShore = (inField && validTerrain > 0.5) ? smoothstep(0.0, 6.0, depth) : 1.0;
    p.x += disp.x * chopShore;
    p.y += disp.y;
    p.z += disp.z * chopShore;
    float height = disp.y;
    vec3  normal = normalize(mix(vec3(0.0, 1.0, 0.0),
                                 normalize(vec3(nrmH.x, 1.0, nrmH.y)), waveAtten));
    float sinSum = clamp(1.0 - normal.y, 0.0, 1.0);
    float jFoam  = clamp(foamSum, 0.0, 1.0) * waveAtten;

    float breakAmt = 0.0;
    if (inField && validTerrain > 0.5) {
        float band = (1.0 - smoothstep(0.0, 9.0, depth));
        float dl = textureLod(shoreDataTex, shoreUV - vec2(shoreTexelSize.x, 0.0), 0.0).b;
        float dr = textureLod(shoreDataTex, shoreUV + vec2(shoreTexelSize.x, 0.0), 0.0).b;
        float dd = textureLod(shoreDataTex, shoreUV - vec2(0.0, shoreTexelSize.y), 0.0).b;
        float du = textureLod(shoreDataTex, shoreUV + vec2(0.0, shoreTexelSize.y), 0.0).b;
        vec2  grad = vec2(dr - dl, du - dd);
        vec2  shoreDir = (length(grad) > 1e-5) ? normalize(-grad) : vec2(0.0);

        breakAmt = oceanBreak(height, depth) * band;
        p.xz += shoreDir * breakAmt * (0.25 + 0.35 * uWaveAmpScale);

        vec3 sw = oceanShoreWaves(shoreDist, depth, time, uWaveSpeed, 0.5 + 0.6 * uWaveAmpScale);
        p.y  += sw.x;
        p.xz += shoreDir * sw.x * 0.3;
        normal.x += shoreDir.x * sw.z;
        normal.z += shoreDir.y * sw.z;
        breakAmt  = max(breakAmt, sw.y);

        // Tuck the surface back to flat sea level right at the waterline (depth->0)
        // so it slides cleanly under the sand instead of poking through the beach;
        // full wave motion only returns once there's real water depth to carry it.
        float waterline = smoothstep(0.0, 0.9, depth);
        p.y = mix(0.0, p.y, waterline);
        p.x = mix(aPos.x, p.x, waterline);
        p.z = mix(aPos.z, p.z, waterline);
    }

    WaveBreak = breakAmt;
    WaveJ = jFoam;
    WorldNormal = normalize(normal);
    WaveHeight  = height;
    WaveSteep   = clamp(sinSum, 0.0, 1.0);
    WaveShoal   = waveAtten;

    FragPos = vec3(model * vec4(p, 1.0));
    TexCoords = aTexCoords;
    gl_Position = projection * view * vec4(FragPos, 1.0);
}
