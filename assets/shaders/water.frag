#version 330 core
#include "lib/foam_lib.glsl"

in vec3 FragPos;
in vec2 TexCoords;
in vec3 WorldNormal;
in float WaveHeight;
in float WaveSteep;   // crest sharpness 0..1 from the vertex shader
in float WaveShoal;   // 0 near shallow shore, 1 in open ocean (wave shoaling)
in float WaveBreak;   // depth-limited breaking weight near shore
in float WaveJ;       // Gerstner fold (whitecap) weight on open-ocean crests

uniform vec3 cameraPos;
uniform float time;
uniform sampler2D normalMap;
uniform sampler2D hdriMap;

uniform vec3 sunDirection;
uniform float windSpeed;
uniform float cloudSpeed;
uniform float waveAmp;        // overall wave height (0 = glassy calm)
uniform float underwaterFactor;
uniform float weatherExposure;
uniform float uStorm;         // 0 calm .. 1 full storm — drives foam coverage + sea colour depth

// Weather-driven water colour (Req 5.2). When provided (non-zero) these
// override the hardcoded turquoise palette so stormy seas read grey.
uniform vec3  weatherShallow;
uniform vec3  weatherDeep;
uniform float weatherFog;

// Weather cloud deck (the same uniforms sky.frag receives). The reflected sky and
// the ambient light on the water body/foam are built from these so the sea always
// mirrors the sky that is actually drawn — clear blue, white cumulus or grey storm.
uniform vec3  cloudColor;
uniform float cloudDensity;
uniform float cloudCoverage;

// Terrain-derived shoreline data generated from Scene collision heights.
// R = terrain height, G = valid terrain mask, B = distance to land, A = land mask.
uniform sampler2D shoreDataTex;
uniform vec2 shoreTexelSize;
uniform float shoreTerrainSize;
uniform float shoreFoamWidth;
uniform float shoreFoamIntensity;
uniform float shoreBreakSpeed;

// SSR — screen-space reflection of the opaque scene (islands/rocks/seabed),
// snapshotted into ssrColor/ssrDepth before the water draws.
uniform sampler2D ssrColor;
uniform sampler2D ssrDepth;
uniform mat4  uViewProj;
uniform float camNear;
uniform float camFar;
uniform vec2  screenSize;

out vec4 FragColor;

#define PI 3.14159265

// Sea of Thieves style colors - bright turquoise
const vec3 WATER_DEEP    = vec3(0.01, 0.05, 0.15);
const vec3 WATER_MID     = vec3(0.02, 0.15, 0.30);
const vec3 WATER_SHALLOW = vec3(0.05, 0.35, 0.40);
const vec3 WATER_BRIGHT  = vec3(0.10, 0.55, 0.50); // Bright turquoise for wave peaks

// ===== NOISE =====
float hash21(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx)*0.1031);
    p3 += dot(p3, p3.yzx+33.33);
    return fract((p3.x+p3.y)*p3.z);
}
float noise2D(vec2 p) {
    vec2 i=floor(p); vec2 f=fract(p);
    f=f*f*(3.0-2.0*f);
    float a=hash21(i),b=hash21(i+vec2(1,0)),c=hash21(i+vec2(0,1)),d=hash21(i+vec2(1,1));
    return mix(mix(a,b,f.x),mix(c,d,f.x),f.y);
}
float fbm2D(vec2 p) {
    float v=0.0, a=0.5;
    for(int i=0;i<4;i++){v+=a*noise2D(p);p*=2.0;a*=0.5;}
    return v;
}

float boundsMask(vec2 uv) {
    vec2 lo = step(vec2(0.0), uv);
    vec2 hi = step(uv, vec2(1.0));
    return lo.x * lo.y * hi.x * hi.y;
}

// Linearize a [0,1] hardware depth value to view-space distance (perspective).
float ssrLinDepth(float d) {
    float z = d * 2.0 - 1.0;
    return (2.0 * camNear * camFar) / (camFar + camNear - z * (camFar - camNear));
}

// (SSR reflection removed — water reflects only the sky. ssrColor/ssrDepth are
//  still used below for screen-space REFRACTION, i.e. seeing the seabed through
//  the water; ssrLinDepth above serves that.)

float saturate(float v) {
    return clamp(v, 0.0, 1.0);
}

// ===== Cook-Torrance GGX specular (ported from GodotOceanWaves, MIT (c) 2024 Ethan
// Truong; GGX + Smith masking-shadowing). Used for the soft, broad sun/moon highlight
// — the sharp mirror sun comes from the Fresnel-weighted sky reflection instead. =====
float ggxDistribution(float NdotH, float a) {
    float a2 = a * a;
    float d  = 1.0 + (a2 - 1.0) * NdotH * NdotH;
    return a2 / (PI * d * d);
}
float smithLambda(float cosT, float a) {
    float t  = cosT / (a * sqrt(max(1.0 - cosT * cosT, 1e-5)));
    float t2 = t * t;
    return t < 1.6 ? (1.0 - 1.259 * t + 0.396 * t2) / (3.535 * t + 2.181 * t2) : 0.0;
}
float ggxSpecular(vec3 N, vec3 L, vec3 V, float rough) {
    vec3  H = normalize(L + V);
    float NdotH = max(dot(N, H), 0.0);
    float NdotL = max(dot(N, L), 2e-5);
    float NdotV = max(dot(N, V), 2e-5);
    float a = rough * rough;
    float D = ggxDistribution(NdotH, a);
    float G = 1.0 / (1.0 + smithLambda(NdotV, a) + smithLambda(NdotL, a));
    return D * G / (4.0 * NdotV + 0.1);
}

float computeShoreFoam(vec3 surfPos, vec3 surfNormal, float windSpeedNormal,
                       out float shoreShallow, out float shoreEdge) {
    shoreShallow = 0.0;
    shoreEdge = 0.0;

    vec2 uv = surfPos.xz / max(shoreTerrainSize, 1.0) + vec2(0.5);
    float inBounds = boundsMask(uv);

    vec4 c = texture(shoreDataTex, uv);
    float terrainY = c.r;
    float valid = c.g * inBounds;

    float shoreDist = c.b;
    float width = max(shoreFoamWidth, 0.5);

    // Perf guard: the bubbly detail field is ~30 worley taps. Skip it where foam
    // is impossible (no land sampled, or far offshore).
    if (valid < 0.001 || shoreDist > width * 1.6) {
        return 0.0;
    }

    vec2 tx = vec2(shoreTexelSize.x, 0.0);
    vec2 ty = vec2(0.0, shoreTexelSize.y);
    vec4 l = texture(shoreDataTex, uv - tx);
    vec4 r = texture(shoreDataTex, uv + tx);
    vec4 d = texture(shoreDataTex, uv - ty);
    vec4 u = texture(shoreDataTex, uv + ty);
    float validNbr = min(min(l.g, r.g), min(d.g, u.g));
    valid *= smoothstep(0.15, 0.75, validNbr);

    float shoreDepth = max(0.0, -terrainY);

    // Shore tangent (along the contour) from the gradient of the distance field.
    float cellWorld = max(shoreTerrainSize * max(shoreTexelSize.x, shoreTexelSize.y), 0.001);
    vec2 distanceGradient = vec2(r.b - l.b, u.b - d.b) / (cellWorld * 2.0);
    float gradLen = length(distanceGradient);
    vec2 shoreDir = (gradLen > 0.0004)
        ? normalize(-distanceGradient)
        : normalize(vec2(0.74, 0.43));
    vec2 shoreTangent = vec2(-shoreDir.y, shoreDir.x);
    float along = dot(surfPos.xz, shoreTangent);

    // ---- swash: the contact line rushes up the slope and slides back ----
    float swashPhase = time * (0.06 + shoreBreakSpeed * 0.05)
                     + foamFbm(vec2(along * 0.02, 0.0)) * 0.35;
    vec2 swash = foamSwash(swashPhase, max(width * 0.14, 2.5));
    float wash = shoreDist - swash.x;
    float recede = swash.y;

    // ---- foam coverage `amount`: where foam wants to be (near the waterline) --
    // shoreLine = thin crisp contact band hugging rocks and the urez.
    float shoreLine = smoothstep(0.9, 0.0, abs(wash + 0.1));
    float lip = smoothstep(0.8, 0.0, abs(wash - 0.3)) * 0.8;
    float warp = foamFbm(vec2(along * 0.04, time * 0.05)) * 1.8;
    float windGain = clamp(0.55 + windSpeedNormal * 0.30, 0.40, 1.50);
    float breaker = foamBreakerBands(wash * 0.85, warp, time, shoreBreakSpeed, windGain);
    float nearShore = 1.0 - smoothstep(1.0, width * 0.55, shoreDist);
    breaker *= nearShore * smoothstep(0.0, 1.0, shoreDepth);

    // Keep the shoreline foam a THIN contact line: the wide breaker band read
    // as a thick white collar, so it only contributes a faint amount now.
    float amount = clamp(shoreLine * 0.95 + lip + breaker * 0.15, 0.0, 1.0);
    amount *= smoothstep(3.5, 0.3, abs(wash));   // tight confine to the urez

    // ---- dissolve the coverage with fine bubbly detail -> lacy foam ----
    float detail = foamBubbles(surfPos.xz, 5.0, time * 0.25);
    float foam = foamDissolve(amount, detail, recede);

    // ---- gates ----
    float slopeGate = mix(0.7, 1.0, smoothstep(0.05, 0.35, gradLen));
    float waterSide = 1.0 - c.a * 0.75;
    float shoreFoam = foam * slopeGate * valid * waterSide * shoreFoamIntensity;

    // ---- outputs that soften the water near the shore ----
    // shoreShallow lightens + de-reflects very shallow water (kills the pastel
    // pink edge). shoreEdge fades alpha at the very contact so the hard water
    // mesh boundary dissolves into wet sand instead of a sharp colour step.
    shoreShallow = (1.0 - smoothstep(0.0, 3.5, shoreDepth)) * valid;
    shoreEdge = (1.0 - smoothstep(0.05, 1.2, shoreDepth)) * valid;

    return saturate(shoreFoam);
}

// ===== HDRI SAMPLING =====
vec2 dirToUV(vec3 dir) {
    float phi = atan(dir.z, dir.x);
    float theta = asin(clamp(dir.y, -1.0, 1.0));
    return vec2(phi / (2.0*PI) + 0.5, theta / PI + 0.5);
}

// ===== volumetric clouds (compact, mirrors sky.frag/clouds.frag) =====
float hash13c(vec3 p){ p=fract(p*0.1031); p+=dot(p,p.zyx+31.32); return fract((p.x+p.y)*p.z); }
float vnoise3w(vec3 p){
    vec3 i=floor(p); vec3 f=fract(p); f=f*f*(3.0-2.0*f);
    float n000=hash13c(i),n100=hash13c(i+vec3(1,0,0)),n010=hash13c(i+vec3(0,1,0)),n110=hash13c(i+vec3(1,1,0));
    float n001=hash13c(i+vec3(0,0,1)),n101=hash13c(i+vec3(1,0,1)),n011=hash13c(i+vec3(0,1,1)),n111=hash13c(i+vec3(1,1,1));
    return mix(mix(mix(n000,n100,f.x),mix(n010,n110,f.x),f.y),
               mix(mix(n001,n101,f.x),mix(n011,n111,f.x),f.y),f.z);
}
float fbm3w(vec3 p){ float v=0.0,a=0.5; for(int i=0;i<4;i++){v+=a*vnoise3w(p);p=p*2.03+vec3(1.7,9.2,3.3);a*=0.5;} return v; }

vec4 marchCloudsW(vec3 ro, vec3 rd, vec3 sunDir) {
    const float B = 120.0, T = 300.0;
    if (rd.y <= 0.03) return vec4(0.0);
    float tN = max((B - ro.y)/rd.y, 0.0);
    float tF = (T - ro.y)/rd.y;
    if (tF <= tN) return vec4(0.0);

    const int STEPS = 20;
    float ss = (tF - tN)/float(STEPS);
    float dither = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
    vec3 p = ro + rd * (tN + ss * dither);
    vec3 stp = rd*ss;

    float wind = time * (0.01 + cloudSpeed * 0.02);
    vec2 windOff = vec2(wind, wind * 0.6);

    float trans = 1.0; vec3 scat = vec3(0.0);
    vec3 sunCol = vec3(1.0,0.95,0.85), amb = vec3(0.4,0.5,0.65);
    for (int i=0;i<STEPS;i++){
        vec3 q = p*0.0035 + vec3(windOff.x, 0.0, windOff.y);
        float h = (p.y - B)/(T - B);
        float hf = smoothstep(0.0,0.3,h)*smoothstep(1.0,0.55,h);
        float n = fbm3w(q);
        float d = smoothstep(0.45, 0.75, n) * hf;
        if (d > 0.001){
            float lt = 1.0; vec3 lp = p;
            for(int j=0;j<2;j++){ lp += sunDir*20.0; float hh=(lp.y-B)/(T-B);
                float hff=smoothstep(0.0,0.3,hh)*smoothstep(1.0,0.55,hh);
                float ln=fbm3w(lp*0.0035+vec3(windOff.x,0.0,windOff.y));
                lt *= exp(-smoothstep(0.45,0.75,ln)*hff*14.0); }
            vec3 lit = amb + sunCol*lt;
            float a = 1.0 - exp(-d*ss*1.4);
            scat += trans*a*lit; trans *= 1.0-a;
            if(trans<0.02) break;
        }
        p += stp;
    }
    return vec4(scat, 1.0 - trans);
}

// Stable spherical-UV star field — same construction as sky.frag.
// Reflected naturally through sampleSky() so wave normals diffuse
// the stars into elongated streaks instead of producing flicker.
float hashSky(vec2 p) {
    p = fract(p * vec2(0.1031, 0.1030));
    p += dot(p, p.yx + 33.33);
    return fract((p.x + p.y) * p.x);
}
vec3 starField(vec3 rd, float t) {
    vec2 uv = dirToUV(rd);
    vec2 grid = uv * vec2(700.0, 350.0);
    vec2 cell = floor(grid);
    vec2 f = fract(grid);
    float r1 = hashSky(cell);
    if (r1 < 0.985) return vec3(0.0);
    vec2 starPos = vec2(hashSky(cell + 1.7), hashSky(cell + 5.3));
    float dist = length(f - starPos);
    float radius = 0.05 + hashSky(cell + 9.1) * 0.06;
    float core = exp(-dist * dist / (radius * radius));
    float bright = 0.6 + hashSky(cell + 13.7) * 1.4;
    float tw = 0.85 + 0.15 * sin(t * 0.6 + r1 * 100.0);
    float colorMix = hashSky(cell + 17.3);
    vec3 col = mix(vec3(0.85, 0.92, 1.0), vec3(1.0, 0.93, 0.82), colorMix);
    return col * core * bright * tw;
}

// Fraction of the sky dome hidden by the weather's cloud deck (0 clear .. 1 solid
// overcast). Mirrors how coverage/density fill the sky in sky.frag's cloud march.
float overcastAmount() {
    return clamp(cloudCoverage * (0.45 + 0.75 * cloudDensity), 0.0, 1.0);
}

vec3 sampleSky(vec3 dir) {
    vec3 sunDir = normalize(sunDirection);
    float sunHeight = sunDir.y;

    float dayFactor = smoothstep(-0.1, 0.2, sunHeight);
    float sunsetFactor = smoothstep(-0.2, 0.1, sunHeight) - smoothstep(0.1, 0.4, sunHeight);
    float nightFactor = 1.0 - smoothstep(-0.2, 0.0, sunHeight);

    vec3 currentTint = vec3(1.0)*dayFactor + vec3(1.2,0.6,0.3)*sunsetFactor + vec3(0.05,0.1,0.3)*nightFactor;
    float currentExposure = mix(0.1, 1.0, smoothstep(-0.2, 0.2, sunHeight));

    vec3 sky = min(texture(hdriMap, dirToUV(dir)).rgb, vec3(1.2)) * currentTint * currentExposure;

    // Weather cloud deck mirrored into the reflection: blend toward a lit cloud
    // dome so the water reflects the SAME overcast sky sky.frag draws. Without
    // this the sea mirrors a clear blue HDRI under a grey storm deck — the flat
    // "painted plastic" look.
    float cover = overcastAmount();
    if (cover > 0.001) {
        float domeLum = mix(1.05, 0.55, clamp(cloudDensity * 0.55, 0.0, 1.0)); // thick deck = darker
        float zenithShade = mix(1.10, 0.85, clamp(dir.y, 0.0, 1.0));           // brighter near horizon
        float sunPatch = pow(max(dot(dir, sunDir), 0.0), 6.0)
                       * dayFactor * (1.0 - 0.5 * min(cloudDensity, 1.0));     // bright smudge through thin cloud
        vec3 dome = cloudColor * currentTint * currentExposure
                  * (domeLum * zenithShade + 0.45 * sunPatch);
        sky = mix(sky, dome, cover * smoothstep(0.0, 0.22, dir.y));            // deck fades at the horizon
    }

    if (sunHeight > -0.1) {
        float sunDot = dot(dir, sunDir);
        float sunDisc = smoothstep(0.9995, 0.99985, sunDot);
        vec3 sunColor = vec3(1.0,0.95,0.85) * 15.0 * sunDisc;
        sunColor += vec3(1.0,0.85,0.5) * pow(max(sunDot, 0.0), 128.0) * 2.0;
        sunColor *= mix(vec3(1.0, 0.5, 0.1), vec3(1.0), smoothstep(-0.1, 0.2, sunHeight));
        sunColor *= smoothstep(-0.1, 0.0, sunHeight);
        sky += sunColor * (1.0 - cover * 0.92);   // the deck hides the mirror sun
    }

    if (nightFactor > 0.0) {
        vec3 moonDir = normalize(-sunDirection);
        float moonDot = dot(dir, moonDir);
        float moonDisc = smoothstep(0.9995, 0.99985, moonDot);
        vec3 moonCol = vec3(0.8, 0.9, 1.0) * 5.0 * moonDisc;
        moonCol += vec3(0.5, 0.6, 1.0) * pow(max(moonDot, 0.0), 128.0) * 0.5;
        sky += moonCol * nightFactor * (1.0 - cover * 0.85);

        // Stars baked into the sky we sample for reflections.
        float starVisibility = nightFactor * smoothstep(0.0, 0.15, dir.y) * (1.0 - cover);
        sky += starField(dir, time) * starVisibility;
    }

    return sky;
}

void main() {
    vec3 viewDir = normalize(cameraPos - FragPos);
    float viewDist = length(cameraPos - FragPos);
    // Per-pixel procedural detail (normal-map octaves, glint noise, bubble worley)
    // is sub-pixel beyond ~150 m and CANNOT be mipmapped, so it aliased into the
    // navy/pink fresnel speckle toward the horizon. Fade it out with distance and
    // let the smooth vertex (Gerstner) normal carry the far field.
    float detailFade = exp(-viewDist * 0.008);
    vec3 sunDir = normalize(sunDirection);
    float sunHeight = sunDir.y;
    float currentExposure = mix(0.1, 1.0, smoothstep(-0.2, 0.2, sunHeight));

    // Time-of-day tints (shared by the body / foam / specular sections below).
    float dayFactor = smoothstep(-0.1, 0.2, sunHeight);
    float sunsetFactor = smoothstep(-0.2, 0.1, sunHeight) - smoothstep(0.1, 0.4, sunHeight);
    float nightFactor = 1.0 - smoothstep(-0.2, 0.0, sunHeight);
    vec3 currentTint = vec3(1.0)*dayFactor + vec3(1.2,0.6,0.3)*sunsetFactor + vec3(0.05,0.1,0.3)*nightFactor;

    // Weather light: how much of the dome the cloud deck covers, how much direct
    // sun survives it, and the colour of the diffuse sky-dome light. Everything
    // below (body, foam, specular) is lit with THESE, so the water sits in the
    // same weather as the sky instead of being self-coloured plastic.
    float skyCover = overcastAmount();
    float sunVis   = 1.0 - skyCover * 0.92;
    vec3  skyAmbient = mix(vec3(0.55, 0.75, 0.95), cloudColor * 0.95, skyCover) * currentTint;

    // ===== NORMALS =====
    float windSpeedNormal = max(windSpeed, 0.5);
    // P1: fine surface detail — 5 octaves of the normal map, each at ~2x scale,
    // scrolling along the wind at its own speed + a counter-drifting twin, so the
    // ripple field is dense and multi-scale and never looks like a tiling texture.
    // This procedural micro-detail layers on TOP of the analytic Gerstner normal
    // (which carries the large wave shape) to add the fine high-frequency sparkle.
    vec2 wdir  = normalize(vec2(1.0, 0.35));     // wind direction (matches WaveField)
    vec2 wperp = vec2(-wdir.y, wdir.x);
    // Low-frequency domain warp: bends the sample grid so the normal map never reads
    // as a regular repeating tile (kills the visible periodic ripple pattern).
    vec2 warp = (vec2(fbm2D(FragPos.xz * 0.015 + time * 0.010),
                      fbm2D(FragPos.xz * 0.015 - time * 0.013)) - 0.5) * 6.0;
    vec3 nrm = vec3(0.0, 0.0, 1.0);
    float sc = 0.05, amp = 0.55, sp = 0.018;
    for (int i = 0; i < 3; ++i) {
        // Rotate each octave (~72°) so successive ripple layers don't align into a tile.
        float a   = float(i) * 1.2566;
        vec2  pos = vec2(dot(FragPos.xz, vec2(cos(a),  sin(a))),
                         dot(FragPos.xz, vec2(-sin(a), cos(a)))) + warp;
        vec2 drift = wdir * (time * sp * windSpeedNormal) + wperp * (sin(time*0.05 + float(i)) * 0.02);
        vec2 uvo = pos * sc + drift + vec2(float(i) * 7.3, float(i) * 3.1);
        vec3 no  = texture(normalMap, uvo).xyz * 2.0 - 1.0;
        nrm.xy += no.xy * amp;
        sc *= 2.0; amp *= 0.58; sp *= 1.7;       // 6th octave = fine capillary ripples
    }
    vec3 micro = normalize(nrm);
    // Choppiness: sharpen the slope a touch so crests read crisp, not only smooth swell.
    // (0.88 keeps more high-frequency slope than 0.82 -> less smooth/plastic.)
    micro.xy = sign(micro.xy) * pow(abs(micro.xy), vec2(0.88));
    // Keep fine ripples alive even in light wind; fade only toward glassy calm.
    float detailGain = clamp(0.4 + 0.7 * windSpeed, 0.0, 1.6) * clamp(waveAmp, 0.0, 1.6);
    micro.xy *= 0.32 * detailGain * detailFade;   // detailFade: no sub-pixel chop at distance

    // Rain stipple: when it's raining ABOVE water, pelt the surface with fine, fast,
    // isotropic ripples so the water reads as dimpled by raindrops (uStorm = sea state).
    float rainMask = clamp(uStorm, 0.0, 1.0) * (1.0 - clamp(underwaterFactor, 0.0, 1.0));
    if (rainMask > 0.01) {
        vec2  rp  = FragPos.xz * 0.9;
        float rA  = noise2D(rp + time * 1.7);
        float rB  = noise2D(rp * 1.9 - time * 2.1);
        // 0.5 (was 0.7): strong stipple scattered the normals so much it matted
        // out ALL gloss/glints — rainy water read as dull plastic, not wet glass.
        micro.xy += (vec2(rA, rB) - 0.5) * 0.5 * rainMask * detailFade;
    }

    // (the view-side test moved below surfNormal — see isUnderwaterView there)

    // ===== PER-PIXEL SURFACE NORMAL =====
    // Analytic Gerstner: use the per-vertex Gerstner normal and lean on the
    // procedural normal-map octaves (micro) for the fine sparkle.
    vec3 baseN = normalize(vec3(WorldNormal.x, max(WorldNormal.y, 0.2), WorldNormal.z));
    micro.xy *= 1.5;
    vec3 surfNormal = normalize(vec3(
        baseN.x + micro.x,
        abs(baseN.y),
        baseN.z + micro.y
    ));
    // Which side of the surface is the camera on? GEOMETRIC test (surface normal
    // vs view direction) instead of gl_FrontFacing: with the tessellated mesh the
    // winding-based test can flip, sending every above-water pixel down the
    // UNDERWATER branch (dim TIR colours, specular x0.1, foam x0.1) — i.e. the
    // flat dark 'plastic' sea. surfNormal always points up, so the dot sign is
    // unambiguous regardless of triangle orientation.
    bool isUnderwaterView = dot(surfNormal, viewDir) < 0.0 || underwaterFactor > 0.5;
    vec3 finalNormal = isUnderwaterView ? -surfNormal : surfNormal;

    // ===== FRESNEL =====
    float NdotV = max(dot(finalNormal, viewDir), 0.0);
    float fresnel;
    if (isUnderwaterView) {
        float criticalCos = sqrt(1.0 - (1.0/1.33)*(1.0/1.33));
        float t = clamp(NdotV / criticalCos, 0.0, 1.0);
        fresnel = 0.02 + 0.98 * pow(1.0 - t, 3.0);
    } else {
        // Glaze floor 0.16: even looking straight DOWN a thin sheen of sky coats
        // the surface (wet gloss), ramping to a full mirror at grazing. Without
        // the floor the top-down view is pure body colour = matte plastic.
        fresnel = 0.16 + 0.84 * pow(1.0 - NdotV, 4.0);
    }
    fresnel = clamp(fresnel, 0.0, 1.0);

    // ===== WATER BODY COLOR (weather-driven) =====
    // Blend deep→shallow by wave height. The palette comes from the
    // active weather preset so tropical reads turquoise and stormy grey.
    float heightFactor = smoothstep(-2.0, 3.0, WaveHeight);
    vec3 wDeep    = weatherDeep;
    vec3 wShallow = weatherShallow;
    vec3 wBright  = mix(wShallow, vec3(1.0), 0.15);   // crest highlight
    vec3 waterBody = mix(wDeep, wShallow, clamp(heightFactor, 0.0, 1.0));
    waterBody = mix(waterBody, wBright, pow(heightFactor, 2.0) * 0.4);

    // Beer-Lambert absorption: the longer the light path through water (deeper
    // seabed), the more the warm wavelengths are absorbed -> deeper = darker blue.
    {
        vec2 dUV = FragPos.xz / max(shoreTerrainSize, 1.0) + vec2(0.5);
        if (boundsMask(dUV) > 0.5) {
            vec4 sdc = texture(shoreDataTex, dUV);
            float seaDepth = max(0.0, -sdc.r) * sdc.g;     // metres of water column
            float absorb = 1.0 - exp(-seaDepth * 0.12);    // 0 shallow .. 1 deep
            waterBody = mix(wShallow, waterBody, clamp(absorb, 0.0, 1.0));
        }
    }

    // Open-ocean deepening: distant water saturates toward the deep colour, so the
    // sea reads as a real ocean (turquoise near the islands -> deep blue out to the
    // horizon). Kept partial — a full mix froze the whole far field to one flat hue.
    {
        float distXZ = length(cameraPos.xz - FragPos.xz);
        float farDeep = smoothstep(80.0, 700.0, distXZ);
        waterBody = mix(waterBody, wDeep, farDeep * 0.70);
    }

    // Light response: the body colour is an ALBEDO, not paint. Shade it by the
    // sky-dome ambient (grey deck in storm, blue-white in clear) plus a soft
    // wrap-lambert from the sun, so wave faces toward the light glow and back
    // faces sink. This per-normal response is what makes the surface read as a
    // lit material instead of a uniformly tinted plastic sheet.
    {
        float wrapNL = clamp((dot(surfNormal, sunDir) + 0.35) / 1.35, 0.0, 1.0);
        vec3 bodyLight = skyAmbient * 0.62
                       + vec3(1.0, 0.97, 0.90) * (wrapNL * 0.62 * sunVis + 0.12);
        waterBody *= bodyLight * 1.18;
    }

    // ===== SUBSURFACE SCATTERING (Atlas/GodotOceanWaves model) =====
    // Glow when the sun is BEHIND a thin crest and we look toward it: combine the
    // view-toward-sun term with a back-light term (strong where the normal faces away
    // from the sun) and the wave height, after the 'Atlas' GDC water BSDF.
    float sssView = pow(max(dot(viewDir, -sunDir), 0.0), 4.0);
    float sssBack = pow(max(0.5 - 0.5 * dot(sunDir, finalNormal), 0.0), 3.0);
    float waveThin = smoothstep(-0.5, 3.0, WaveHeight);
    // Crest glow is DIRECT sunlight through thin water — the cloud deck kills it.
    float sssIntensity = smoothstep(-0.1, 0.1, sunHeight) * (0.30 + 0.70 * sunVis);
    if (isUnderwaterView) sssIntensity *= 0.3;
    // Bright turquoise SSS glow (greener bias per the Atlas SSS modifier).
    vec3 sssColor = vec3(0.05, 0.7, 0.5) * sssView * sssBack * waveThin * sssIntensity * 3.0;
    // Additional ambient SSS that doesn't depend on sun direction (makes water bright from all angles)
    vec3 ambientSSS = vec3(0.008, 0.05, 0.045) * waveThin * sssIntensity;
    waterBody += sssColor + ambientSSS;

    // Apply exposure
    waterBody *= currentExposure;

    // ===== REFLECTION & REFRACTION =====
    vec3 reflColor, transmitted;

    if (isUnderwaterView) {
        // Underwater TIR reflection: reflects the underwater environment
        // Use a blue-green color that simulates reflected underwater ambient light
        vec3 uwReflDir = reflect(-viewDir, finalNormal);
        // Sample procedural underwater color based on reflection direction
        float reflDepthFactor = smoothstep(-1.0, 0.0, uwReflDir.y); // How much reflects upward vs downward
        vec3 uwReflDeep = vec3(0.005, 0.03, 0.08) * currentExposure; // Looking toward depths
        vec3 uwReflShallow = vec3(0.02, 0.12, 0.20) * currentExposure; // Looking toward shallows
        reflColor = mix(uwReflDeep, uwReflShallow, reflDepthFactor);
        // Add subtle ripple variation to TIR reflection
        float ripple = noise2D(FragPos.xz * 2.0 + time * 0.1) * 0.3;
        reflColor += vec3(0.005, 0.02, 0.03) * ripple * currentExposure;
        
        // Snell's window: see sky through surface
        vec3 refrDir = refract(-viewDir, finalNormal, 1.33 / 1.0);
        if (length(refrDir) < 0.01) {
            // Total internal reflection zone
            transmitted = reflColor;
            fresnel = 1.0;
        } else {
            // Inside Snell's window: see sky
            vec3 skyColor = sampleSky(refrDir) * 1.5;

            // Volumetric clouds — ONLY here (one march per water pixel,
            // and only the small Snell-window region) so the diver sees
            // the same drifting clouds as above the surface without the
            // cost of marching for every reflection/fog sample.
            float dayFac = smoothstep(-0.1, 0.2, sunHeight);
            if (dayFac > 0.01 && refrDir.y > 0.03) {
                vec3 sdir = normalize(sunDirection);
                vec4 cl = marchCloudsW(cameraPos, refrDir, sdir);
                float horizFade = smoothstep(0.03, 0.25, refrDir.y);
                skyColor = mix(skyColor, cl.rgb * 1.5,
                               clamp(cl.a * dayFac * horizFade, 0.0, 1.0));
            }

            // Smooth edge glow at the boundary of Snell's window
            float edgeFactor = 1.0 - smoothstep(0.0, 0.3, length(refrDir));
            vec3 edgeGlow = vec3(0.05, 0.20, 0.25) * edgeFactor * currentExposure;
            transmitted = skyColor + edgeGlow;
        }
    } else {
        // Above water: reflection is sky, with screen-space reflection of the scene
        // (islands/rocks/seabed) blended on top where the reflected ray hits geometry.
        // Far water reflects with a FLATTENED normal: per-pixel chop noise averages
        // out over distance, leaving the smooth coherent sheen a real sea shows at
        // grazing angles instead of salt-and-pepper reflection sparkle.
        float reflFlat = smoothstep(60.0, 700.0, length(cameraPos.xz - FragPos.xz));
        vec3 reflN = normalize(mix(finalNormal, vec3(0.0, 1.0, 0.0), reflFlat * 0.55));
        vec3 reflDir = reflect(-viewDir, reflN);
        reflColor = sampleSky(reflDir) * 1.15;     // small lift so the glaze reads glossy
        // A real sea does NOT mirror the whole sky out to the horizon —
        // grazing fresnel ramps to 1 and was painting the far field as a
        // bright flat mirror. Darken the DIFFUSE sky reflection with
        // distance; the bright sun path stays (sunGlint/GGX specular are
        // added separately below), so only strong light rays survive far out.
        reflColor *= mix(1.0, 0.40, reflFlat);
        // SSR reflection of the opaque scene (islands/rocks/seabed) DISABLED:
        // from above it produced phantom/ghost reflections of trees & rocks that
        // looked wrong. The water now reflects only the sky. (Screen-space
        // REFRACTION below — the see-through seabed — is kept.)

        // ---- REAL transparency: screen-space refraction of the scene behind the
        // surface (the seabed/rocks already drawn into ssrColor), distorted by the
        // surface normal, then absorbed by the water-column thickness (Beer-Lambert).
        // This is what makes the water read as a clear refracting medium instead of a
        // flat opaque blue 'plastic' sheet.
        vec2 suv    = gl_FragCoord.xy / screenSize;
        float surfZ = ssrLinDepth(gl_FragCoord.z);
        vec2 refrUV = clamp(suv + finalNormal.xz * 0.045, vec2(0.001), vec2(0.999));
        float sceneZ = ssrLinDepth(texture(ssrDepth, refrUV).r);
        // Reject the distortion if it samples something IN FRONT of the surface.
        if (sceneZ < surfZ) {
            refrUV = suv;
            sceneZ = ssrLinDepth(texture(ssrDepth, suv).r);
        }
        float waterDepth = max(sceneZ - surfZ, 0.0);         // metres of water column
        vec3  sceneBehind = texture(ssrColor, refrUV).rgb;   // the seabed seen through the water
        float absorb = clamp(1.0 - exp(-waterDepth * 0.05), 0.0, 1.0);
        // Shallow -> seabed; mid -> turquoise body; DEEP -> dark navy. Real deep water
        // absorbs almost all light, so from above it is near-black blue, NOT a bright
        // flat blue — that bright flat body colour was the 'plastic' look.
        vec3 deepCol = vec3(0.012, 0.045, 0.075) * currentExposure;
        vec3 bodyCol = mix(waterBody, deepCol, absorb * absorb);
        transmitted = mix(sceneBehind, bodyCol, absorb);
    }

    // ===== FOAM (much more, Sea of Thieves style) =====
    float foamNoise1 = fbm2D(FragPos.xz * 0.5 + vec2(time*0.03 * windSpeedNormal));
    float foamNoise2 = fbm2D(FragPos.xz * 1.5 - vec2(time*0.02 * windSpeedNormal, time*0.01));
    float foamDetail = noise2D(FragPos.xz * 4.0 + time * 0.5 * windSpeedNormal);
    
    // Base foam on wave crests
    float foamThreshold = mix(1.8, 0.2, clamp((windSpeed - 0.5) / 3.0, 0.0, 1.0));
    float foamMask = smoothstep(foamThreshold, foamThreshold + 1.0, WaveHeight);
    foamMask *= (foamNoise1 * 0.5 + 0.5);
    foamMask *= (0.6 + 0.4 * foamDetail);
    
    // Extra foam on steep wave slopes
    float slopeFoam = 1.0 - abs(dot(surfNormal, vec3(0, 1, 0)));
    slopeFoam = smoothstep(0.3, 0.8, slopeFoam) * 0.25;
    slopeFoam *= foamNoise2;
    
    // Trailing foam / turbulence
    float trailFoam = fbm2D(FragPos.xz * 0.8 + vec2(time*0.01, -time*0.015));
    trailFoam = smoothstep(0.55, 0.75, trailFoam) * smoothstep(foamThreshold - 0.5, foamThreshold + 0.5, WaveHeight) * 0.18;
    
    // Crest spray: where the wave is sharply pinched (steep crest) and
    // the wind is strong, add bright foam/spray. WaveSteep comes from
    // the vertex shader and peaks exactly on breaking crests.
    float spray = smoothstep(0.55, 0.95, WaveSteep) * clamp((windSpeed - 0.8) / 2.0 + uStorm * 0.9, 0.0, 1.4);
    spray *= (0.6 + 0.4 * foamDetail);

    float shoreShallow, shoreEdge;
    float shoreFoam = computeShoreFoam(FragPos, surfNormal, windSpeedNormal, shoreShallow, shoreEdge);
    // Shallow shoreline water reads as clean turquoise, not a pink sky mirror.
    if (!isUnderwaterView) fresnel *= (1.0 - shoreShallow * 0.6);
    // Lacy bubble texture (advected down the wave slope) — computed ONCE here, used
    // both to ERODE the Jacobian foam mask and to texture the foam interior below.
    vec2  foamAdv = surfNormal.xz * 1.5;
    float foamTex = foamBubbles(FragPos.xz + foamAdv, 6.0, time * 0.30);
    // Far away the worley bubbles are sub-pixel noise — settle on their mean so
    // distant foam comes only from the smooth Gerstner crest-fold field.
    foamTex = mix(0.45, foamTex, detailFade);

    // Physically-placed whitecaps: foam where the Gerstner crest folds (WaveJ)
    // and where wind is up. This rides the actual crest geometry, not a height
    // threshold, so it reads as real breaking water.
    float whitecap = smoothstep(0.35, 0.90, WaveJ) * clamp(0.2 + windSpeed * 0.7, 0.0, 1.8);
    whitecap *= (0.6 + 0.4 * foamDetail);
    // Erode the crest whitecap with the lacy bubble field so patch edges dissolve into
    // clumps of bubbles instead of reading as a solid white fill.
    whitecap *= (0.55 + 0.75 * foamTex);
    whitecap = clamp(whitecap * clamp(0.7 + windSpeed * 0.25 + uStorm * 0.45, 0.0, 1.8), 0.0, 1.6);

    // P3: ambient drifting surface foam — faint large-scale streaks over the whole
    // ocean so open water never reads as flat (matches the reference open-ocean look).
    float ambientFoam = smoothstep(0.60, 0.95,
        fbm2D(FragPos.xz * 0.04 + vec2(time * 0.012, -time * 0.009) * windSpeedNormal));
    ambientFoam *= clamp(0.05 + windSpeed * 0.05, 0.0, 0.16);

    // Open-water foam = ONLY real breaking crests (Gerstner crest-fold -> whitecap) plus
    // a little crest spray. The broad slope/ambient/threshold foam terms are dropped:
    // with the detailed per-pixel normals they fired everywhere and painted the whole
    // sea in grainy foam. Clean water now reads clean, like the reference.
    float openWaterFoam = clamp(whitecap * 0.55 + spray * 0.10, 0.0, 0.7);
    openWaterFoam *= WaveShoal;                 // open-ocean foam fades into shore
    // Depth-limited breakers foam up AT the island (added after the shoal fade).
    float breakerFoam = WaveBreak * (0.55 + 0.45 * foamDetail);
    openWaterFoam = clamp(openWaterFoam + breakerFoam * 0.6, 0.0, 0.85);
    float totalFoam = clamp(openWaterFoam + shoreFoam * 0.95, 0.0, 1.0);

    // Final foam opacity, re-textured by the bubble field so the interior of a foam
    // patch is bubbly, not a flat white fill; high threshold keeps clean water clean.
    float openWaterFoamSharp = smoothstep(0.20, 0.80, openWaterFoam * (0.8 + 0.5 * foamTex));

    float visibleShoreFoam = shoreFoam;
    if (isUnderwaterView) {
        totalFoam *= 0.1;       // Barely visible from below
        visibleShoreFoam *= 0.18;
    }

    // Foam coverage at this pixel — used as a "roughness" mask: CLEAN water is a near
    // mirror (sharp specular), FOAM is rough/diffuse (no mirror, just bright white).
    float foamCover = clamp(max(openWaterFoamSharp,
                                smoothstep(0.0, 0.85, visibleShoreFoam)), 0.0, 1.0);

    // ===== SPECULAR (Cook-Torrance GGX + sparkle glints) =====
    // Roughness rises with foam: clean water gives a soft, broad GGX sun highlight;
    // foam scatters and broadens it further. The SHARP mirror sun comes separately
    // from the Fresnel-weighted sky reflection (reflColor), which already has the disc.
    float waterRough = mix(0.26, 0.95, foamCover);   // tighter highlight = more 'pop'
    float sunSpecGGX = ggxSpecular(finalNormal, sunDir, viewDir, waterRough);

    // Direct-sun highlight only survives where the cloud deck lets the sun through.
    float specIntensity = smoothstep(-0.1, 0.0, sunHeight) * sunVis;
    vec3 sunSpecCol = vec3(1.0, 0.9, 0.75) * specIntensity;

    // Sparkle glints — high-frequency noise-based micro-specular (kept on top of GGX).
    // A LIVING GLITTER FIELD across the sun side of every ripple: this is the
    // strongest "real wet water" cue when looking down at the surface, where
    // fresnel is low and the sky glaze alone can't carry the material.
    // Damped at night so they don't compete with star reflections.
    vec3  H = normalize(sunDir + viewDir);
    float NdotH = max(dot(finalNormal, H), 0.0);
    float glintNoise = noise2D(FragPos.xz * 8.0 + time * 0.35 * windSpeedNormal);
    float glintNoise2 = noise2D(FragPos.xz * 15.0 - time * 0.5);
    float glintMask = pow(smoothstep(0.58, 0.90, glintNoise) *
                          smoothstep(0.56, 0.88, glintNoise2), 1.8);
    // detailFade: the 8/m + 15/m glint noise is pure per-pixel randomness at
    // distance — without the fade it sparkled the whole far field.
    float sunGlint = pow(NdotH, 90.0) * glintMask * 2.4 * dayFactor * sunVis * detailFade;

    // Moon specular (GGX too, slightly tighter).
    vec3 moonDir = normalize(-sunDirection);
    float moonSpecGGX = ggxSpecular(finalNormal, moonDir, viewDir, waterRough * 0.85);
    vec3 moonSpecCol = vec3(0.7, 0.85, 1.0) * (moonSpecGGX * 1.2 + sunGlint * 0.4)
                     * nightFactor * (1.0 - skyCover * 0.85);
    
    // Star reflections come naturally from sampleSky() now — no
    // separate per-pixel sparkle pass.
    
    if (isUnderwaterView) {
        sunSpecCol *= 0.1;
        moonSpecCol *= 0.1;
    }

    // ===== COMBINE =====
    vec3 color = mix(transmitted, reflColor, fresnel);
    // Luminous turquoise bias in shallow shoreline water so the edge reads
    // clean and bright instead of washed-out pastel.
    if (!isUnderwaterView && shoreShallow > 0.001) {
        vec3 shallowTint = mix(wShallow, vec3(0.32, 0.92, 0.90), 0.45) * currentExposure;
        color = mix(color, shallowTint, shoreShallow * 0.45);
    }
    // Mirror specular lives on CLEAN water only — foam is rough and scatters light
    // diffusely, so it gets no sharp glint (just its bright albedo below).
    float specClean = 1.0 - 0.9 * foamCover;
    color += (sunSpecGGX * 2.8 + sunGlint) * sunSpecCol * specClean;
    color += moonSpecCol * specClean;
    
    // Foam is ~white diffuse bubbles lit by the SAME light as everything else:
    // the sky dome (grey deck in storm, bright white in clear weather) plus the
    // direct sun where the deck lets it through. Self-luminous pure-white foam
    // ("white paint blobs") was a big part of the plastic look.
    float foamNL  = clamp((dot(finalNormal, sunDir) + 0.4) / 1.4, 0.0, 1.0);
    vec3  foamLit = (skyAmbient + vec3(0.10, 0.13, 0.22) * nightFactor) * 0.85
                  + vec3(1.0, 0.96, 0.88) * (foamNL * 0.80 * sunVis + 0.10);
    foamLit *= max(currentExposure, 0.45);   // night floor so foam stays readable
    vec3 foamColor = vec3(0.94, 0.97, 1.0) * foamLit;
    // Thin foam reads slightly translucent and cool over clear water; thick
    // foam goes bright opaque white.
    vec3 thinFoam  = vec3(0.88, 0.94, 0.97);
    vec3 thickFoam = vec3(1.0, 1.0, 1.0);
    float foamThick = smoothstep(0.18, 0.70, visibleShoreFoam);
    vec3 shoreFoamColor = mix(thinFoam, thickFoam, foamThick) * foamLit;
    color = mix(color, foamColor, openWaterFoamSharp * 0.65);
    color = mix(color, shoreFoamColor, smoothstep(0.0, 0.85, visibleShoreFoam) * 0.90);

    // Reveal the wave FORM: lighten crests and sun/sky-facing wave faces, darken
    // troughs, so the 3D swell shape reads as relief instead of a flat sheet. This
    // is what makes medium-scale waves look solid (done before fog, after foam).
    if (!isUnderwaterView) {
        float crest = smoothstep(-1.8, 2.0, WaveHeight);
        vec3  keyDir = normalize(sunDir * 0.5 + vec3(0.0, 1.0, 0.0));
        float faceLight = smoothstep(-0.15, 0.55, dot(finalNormal, keyDir));
        float form = mix(0.80, 1.16, clamp(crest * 0.55 + faceLight * 0.45, 0.0, 1.0));
        // Shade only the water BODY: scaling the mirrored sky / foam by wave height
        // made reflections look painted onto the surface and crushed the grazing
        // sheen — exactly the plastic read.
        color *= mix(1.0, form, (1.0 - fresnel) * (1.0 - foamCover * 0.8));
        // Beer-Lambert: deep troughs between waves sink toward near-black navy (long
        // light path through dense water), but never under foam (foam stays bright).
        float trough = (1.0 - smoothstep(-3.0, -0.3, WaveHeight))
                     * (1.0 - foamCover) * (1.0 - fresnel);
        vec3  troughDeep = vec3(0.02, 0.07, 0.13) * currentExposure;
        color = mix(color, troughDeep, trough * 0.26);
    }

    // Atmospheric horizon: water dissolves into the actual SKY colour toward the
    // horizon (and faster at grazing view angles), so the sea blends seamlessly
    // into the sky instead of ending in a hard dark band.
    if (!isUnderwaterView) {
        float dist = length(cameraPos - FragPos);
        float fog = 1.0 - exp(-dist * 0.0045);
        float graze = 1.0 - clamp(abs(viewDir.y) * 3.5, 0.0, 1.0);  // horizon pixels haze more
        fog = clamp(fog + graze * 0.55, 0.0, 1.0);
        vec3 fogDir = normalize(vec3(FragPos.x - cameraPos.x, 0.05, FragPos.z - cameraPos.z));
        vec3 fogColor = sampleSky(fogDir);                 // == sky -> seamless horizon
        color = mix(color, fogColor, fog);
    }

    // Alpha: transparency is now done IN-SHADER via screen-space refraction (the
    // seabed is composited into `transmitted`), so the surface itself is opaque —
    // otherwise the seabed would show twice (refraction + alpha blend). Keep only a
    // soft fade at the very waterline so the mesh edge dissolves into wet sand.
    float alpha = 1.0;
    if (!isUnderwaterView) {
        alpha *= mix(1.0, 0.30, shoreEdge);
    }

    FragColor = vec4(color * weatherExposure, alpha);
}
