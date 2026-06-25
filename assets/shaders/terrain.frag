#version 330 core
#include "lib/foam_lib.glsl"
// ======================================================================
// ЗАЩИТА: здесь ВТОРОЙ материал с NORMAL MAPPING (требование «≥2 матер.»):
//   песок/ил/скалы имеют свои normal-карты; TBN-фрейм поднимает из них
//   нормали => детальный рельеф поверхности без лишней геометрии.
//   Также этот шейдер ПРИНИМАЕТ тени (shadow map + PCF) и рисует
//   heightmap-дно (B07). СЛОВА: TBN, tangent space, два материала.
// ======================================================================

in vec3 FragPos;
in vec2 TexCoords;     // 0..1 over the whole heightmap (used for masks)
in vec3 WorldNormal;
in vec3 BiomeWeights;  // unused (vertex attr stays for layout compat)
in float Depth;        // world Y after heightScale
in vec4 FragPosLight;  // fragment position in the sun's light space

// --- biome textures ----------------------------------------
uniform sampler2D sandDiffuse;
uniform sampler2D sandNormal;
uniform sampler2D sandRoughness;

uniform sampler2D mudDiffuse;
uniform sampler2D mudNormal;
uniform sampler2D mudRoughness;

uniform sampler2D rockDiffuse;
uniform sampler2D rockNormal;
uniform sampler2D rockARM;       // R=AO, G=Roughness, B=Metallic

uniform sampler2D lavaDiffuse;
uniform sampler2D lavaNormal;
uniform sampler2D lavaRoughness;
uniform sampler2D lavaEmissive;

// --- biome masks (RGBA: alpha=0 means biome inactive) ------
uniform sampler2D castleMask;
uniform sampler2D lavaMask;
uniform sampler2D riverMask;

// --- render-to-texture caustics --------------------------------
// Single-channel R16F texture filled this frame by caustics.vert/frag.
// Sampled in world-XZ UV space, tiled across the seabed.
uniform sampler2D causticsTex;
uniform float terrainSize;
uniform float causticsTileSize;

uniform vec3 sunDirection;
uniform vec3 cameraPos;
uniform float time;
uniform float underwaterFactor;
uniform float weatherExposure;   // storm darkens the lit terrain too

// --- shadow mapping (sun depth + large-radius PCF) -----------------
uniform sampler2D shadowMap;
uniform int       shadowEnabled;

// --- flashlight (camera spotlight, toggled with F) -----------------
uniform int   flashOn;
uniform vec3  flashPos;
uniform vec3  flashDir;
uniform vec3  flashColor;
uniform float flashInnerCos;
uniform float flashOuterCos;
uniform float flashRange;
uniform float flashIntensity;

vec3 applyFlashlight(vec3 fragPos, vec3 N) {
    if (flashOn == 0) return vec3(0.0);
    vec3 toFrag = fragPos - flashPos;
    float dist = length(toFrag);
    if (dist > flashRange) return vec3(0.0);
    vec3 L = toFrag / max(dist, 0.0001);
    float theta = dot(L, normalize(flashDir));
    float cone = clamp((theta - flashOuterCos) /
                       max(flashInnerCos - flashOuterCos, 0.001), 0.0, 1.0);
    cone = cone * cone;
    float distFade = 1.0 - (dist / flashRange);
    distFade = distFade * distFade;
    float ndl = max(dot(N, -L), 0.0);
    return flashColor * flashIntensity * cone * distFade * (0.25 + 0.75 * ndl);
}

out vec4 FragColor;

// ---------- helpers ----------------------------------------
float hash21(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}
float vnoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    float a = hash21(i);
    float b = hash21(i + vec2(1.0, 0.0));
    float c = hash21(i + vec2(0.0, 1.0));
    float d = hash21(i + vec2(1.0, 1.0));
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// Fractal Brownian Motion — adds rocky detail to normals
float fbm(vec2 p) {
    // 3 octaves (was 5): the terrain shader calls fbm ~7x per pixel plus
    // fbmGrad (4x), so trimming octaves is a cheap ALU win; the missing
    // high-frequency wobble is invisible under normal mapping + fog.
    float v = 0.0;
    float a = 0.5;
    for (int i = 0; i < 3; ++i) {
        v += a * vnoise(p);
        p *= 2.07;
        a *= 0.5;
    }
    return v;
}

// Compute analytical gradient of fbm for normal perturbation
vec2 fbmGrad(vec2 p) {
    float eps = 0.5;
    float fx = fbm(p + vec2(eps, 0.0)) - fbm(p - vec2(eps, 0.0));
    float fz = fbm(p + vec2(0.0, eps)) - fbm(p - vec2(0.0, eps));
    return vec2(fx, fz) / (2.0 * eps);
}

// Realistic Voronoi caustics — the bright "spider web" pattern is the
// boundary between Voronoi cells (second-nearest minus nearest distance).
// Animated cell points produce the shimmering, refracting look you see
// on a swimming-pool floor.
vec2 hash22(vec2 p) {
    p = vec2(dot(p, vec2(127.1, 311.7)),
             dot(p, vec2(269.5, 183.3)));
    return fract(sin(p) * 43758.5453123);
}

float voronoiCaustic(vec2 uv, float t) {
    // Domain warp adds large-scale curvature so the cells aren't a regular grid
    vec2 warp = vec2(sin(uv.y * 0.8 + t * 0.4),
                     cos(uv.x * 0.8 + t * 0.5));
    uv += warp * 0.6;

    vec2 i = floor(uv);
    vec2 f = fract(uv);

    float minD  = 8.0;
    float minD2 = 8.0;

    // Sample 3x3 neighbourhood
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 g = vec2(float(x), float(y));
            vec2 o = hash22(i + g);
            // Animate each cell point in a small circle so the pattern shifts
            o = 0.5 + 0.5 * sin(t * 1.3 + 6.2831 * o);
            float d = length(g + o - f);
            if (d < minD) { minD2 = minD; minD = d; }
            else if (d < minD2) { minD2 = d; }
        }
    }
    // Edge intensity: thin where cells are far apart, bright on the seam
    float edge = minD2 - minD;
    float c = 1.0 - smoothstep(0.0, 0.08, edge);
    // Square it for sharper filaments, then add a soft halo
    return pow(c, 6.0) + 0.10 * pow(c, 1.5);
}

vec3 sampleCaustics(vec2 worldXZ, float t) {
    // Two layers at slightly different scales/timings to break the
    // Voronoi tile pattern and give the caustic a richer look.
    float a = voronoiCaustic(worldXZ * 0.30,        t);
    float b = voronoiCaustic(worldXZ * 0.45 + 13.7, t * 0.7 + 5.0);
    // Wavelength split — red focuses slightly differently from blue,
    // mimicking dispersion of sunlight through water.
    float r = a;
    float g = mix(a, b, 0.5);
    float bl = b;
    return vec3(r, g, bl) * 1.2;
}

// Sample a tileable albedo at two scales to hide the tiling pattern
vec3 sampleTwoScales(sampler2D tex, vec2 worldXZ, float a, float b) {
    vec3 sa = texture(tex, worldXZ * a).rgb;
    vec3 sb = texture(tex, worldXZ * b).rgb;
    return mix(sa, sb, 0.4);
}
float sampleTwoScalesR(sampler2D tex, vec2 worldXZ, float a, float b) {
    return mix(texture(tex, worldXZ * a).r,
               texture(tex, worldXZ * b).r, 0.4);
}
vec3 sampleTwoScalesNormal(sampler2D tex, vec2 worldXZ, float a, float b) {
    vec3 n1 = texture(tex, worldXZ * a).xyz * 2.0 - 1.0;
    vec3 n2 = texture(tex, worldXZ * b).xyz * 2.0 - 1.0;
    vec3 n = normalize(mix(n1, n2, 0.5));
    n.xy *= 0.85;  // stronger micro-relief
    return normalize(n);
}

// ---------- stochastic (non-tiling) sampling ------------------------
// Inigo Quilez's "texture repetition" trick. Instead of sampling the
// texture at a single UV (which shows an obvious repeating grid), we
// blend two copies offset by a per-region random amount. The blend
// boundary is itself dithered by the texture contrast, so the result
// has NO visible tiling — the #1 fix for "видно шов все время".
//   2 samples only → cheap enough for full-screen terrain.
vec3 noTile(sampler2D tex, vec2 uv) {
    // Low-frequency index that varies across the world.
    float k = vnoise(uv * 0.25);
    float l = k * 8.0;
    float f = fract(l);
    float ia = floor(l);
    float ib = ia + 1.0;
    // Random per-cell UV offsets (sin gives deterministic pseudo-random).
    vec2 offa = sin(vec2(3.0, 7.0) * (ia + 1.0));
    vec2 offb = sin(vec2(3.0, 7.0) * (ib + 1.0));
    vec3 cola = texture(tex, uv + offa).rgb;
    vec3 colb = texture(tex, uv + offb).rgb;
    // Blend, biased by per-sample contrast so the seam is invisible.
    float blend = smoothstep(0.2, 0.8, f - 0.1 * (cola.x + cola.y + cola.z
                                                 - colb.x - colb.y - colb.z));
    return mix(cola, colb, blend);
}
float noTileR(sampler2D tex, vec2 uv) {
    float k = vnoise(uv * 0.25);
    float l = k * 8.0;
    float f = fract(l);
    float ia = floor(l), ib = ia + 1.0;
    vec2 offa = sin(vec2(3.0, 7.0) * (ia + 1.0));
    vec2 offb = sin(vec2(3.0, 7.0) * (ib + 1.0));
    float ca = texture(tex, uv + offa).r;
    float cb = texture(tex, uv + offb).r;
    return mix(ca, cb, smoothstep(0.2, 0.8, f));
}
vec3 noTileNormal(sampler2D tex, vec2 uv) {
    float k = vnoise(uv * 0.25);
    float l = k * 8.0;
    float f = fract(l);
    float ia = floor(l), ib = ia + 1.0;
    vec2 offa = sin(vec2(3.0, 7.0) * (ia + 1.0));
    vec2 offb = sin(vec2(3.0, 7.0) * (ib + 1.0));
    vec3 na = texture(tex, uv + offa).xyz * 2.0 - 1.0;
    vec3 nb = texture(tex, uv + offb).xyz * 2.0 - 1.0;
    vec3 n = normalize(mix(na, nb, smoothstep(0.2, 0.8, f)));
    n.xy *= 0.85;
    return normalize(n);
}

// ---------- triplanar mapping ---------------------------------------
// On steep cliffs a flat XZ projection stretches the texture into long
// streaks. Triplanar samples the texture three times (XY, YZ, XZ) and
// blends by the dominant axis of the surface normal. This removes the
// streaking that a flat XZ projection causes on the rocky terrain biome.
vec3 triplanarAlbedo(sampler2D tex, vec3 worldPos, vec3 N, float scale) {
    vec3 blend = abs(N);
    blend = pow(blend, vec3(4.0));
    blend /= dot(blend, vec3(1.0)) + 1e-5;
    vec3 sx = texture(tex, worldPos.zy * scale).rgb;
    vec3 sy = texture(tex, worldPos.xz * scale).rgb;
    vec3 sz = texture(tex, worldPos.xy * scale).rgb;
    return sx * blend.x + sy * blend.y + sz * blend.z;
}
float triplanarR(sampler2D tex, vec3 worldPos, vec3 N, float scale, int channel) {
    vec3 blend = abs(N);
    blend = pow(blend, vec3(4.0));
    blend /= dot(blend, vec3(1.0)) + 1e-5;
    vec4 sx = texture(tex, worldPos.zy * scale);
    vec4 sy = texture(tex, worldPos.xz * scale);
    vec4 sz = texture(tex, worldPos.xy * scale);
    float vx = sx[channel], vy = sy[channel], vz = sz[channel];
    return vx * blend.x + vy * blend.y + vz * blend.z;
}
// "Whiteout" triplanar normal: blend tangent-space normals after
// projecting them around the appropriate axis.
vec3 triplanarNormal(sampler2D tex, vec3 worldPos, vec3 N, float scale) {
    vec3 blend = abs(N);
    blend = pow(blend, vec3(4.0));
    blend /= dot(blend, vec3(1.0)) + 1e-5;
    vec3 nx = texture(tex, worldPos.zy * scale).xyz * 2.0 - 1.0;
    vec3 ny = texture(tex, worldPos.xz * scale).xyz * 2.0 - 1.0;
    vec3 nz = texture(tex, worldPos.xy * scale).xyz * 2.0 - 1.0;
    nx = vec3(0.0,    nx.y,  nx.x) * blend.x;
    ny = vec3(ny.x,  0.0,    ny.y) * blend.y;
    nz = vec3(nz.x,  nz.y,  0.0)  * blend.z;
    return normalize(N + nx + ny + nz);
}

// ---------- height-based material blend -----------------------------
// Linear mixing of two materials looks muddy. Real ground transitions
// have the rougher material "poke through" — pebbles showing through
// sand etc. We bias the blend by each material's height/contrast so the
// transition is sharp and natural. `h1`,`h2` are per-material heights
// (use the albedo luminance as a cheap height proxy).
float heightBlend(float t, float h1, float h2) {
    float depth = 0.2;   // transition softness
    float ma = max(h1 + (1.0 - t), 0.0001);
    float mb = max(h2 + t,         0.0001);
    // Sharpen around the dominant one.
    float b = smoothstep(0.0, depth, (mb - ma) + depth * 0.5);
    return clamp(b, 0.0, 1.0);
}

// ---------- Cook-Torrance PBR BRDF ----------------------------------
// Real physically-based lighting. Replaces the old cheap diffuse+spec
// so the PBR texture sets (albedo/normal/roughness/AO) actually read as
// proper materials: GGX microfacet specular, Smith geometry term and
// Fresnel-Schlick. This is the single biggest "make it look AAA" win.
#define PI_C 3.14159265359

float DistributionGGX(vec3 N, vec3 H, float rough) {
    float a  = rough * rough;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float d = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    return a2 / max(PI_C * d * d, 1e-5);
}
float GeometrySchlickGGX(float NdotV, float rough) {
    float r = rough + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}
float GeometrySmith(vec3 N, vec3 V, vec3 L, float rough) {
    float ggxV = GeometrySchlickGGX(max(dot(N, V), 0.0), rough);
    float ggxL = GeometrySchlickGGX(max(dot(N, L), 0.0), rough);
    return ggxV * ggxL;
}
vec3 FresnelSchlick(float cosT, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosT, 0.0, 1.0), 5.0);
}

// Evaluate one directional/point light with the Cook-Torrance model.
vec3 pbrLight(vec3 N, vec3 V, vec3 L, vec3 radiance,
              vec3 albedo, float rough, float metallic) {
    vec3 H = normalize(V + L);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    float NDF = DistributionGGX(N, H, rough);
    float G   = GeometrySmith(N, V, L, rough);
    vec3  F   = FresnelSchlick(max(dot(H, V), 0.0), F0);

    vec3 num = NDF * G * F;
    float den = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 1e-4;
    vec3 specular = num / den;

    vec3 kd = (vec3(1.0) - F) * (1.0 - metallic);
    float NdotL = max(dot(N, L), 0.0);
    return (kd * albedo / PI_C + specular) * radiance * NdotL;
}

// ---------- shadow mapping with large-radius PCF --------------------
// Underwater light is heavily scattered, so hard shadow edges look
// wrong. We sample a 3x3 kernel (9 taps) with an extra spread multiplier
// for very soft, diffuse shadows from kelp and rocks. Returns 1.0 fully
// lit, 0.0 fully shadowed.
float computeShadow(vec4 fragPosLight, vec3 N, vec3 L) {
    if (shadowEnabled == 0) return 1.0;
    vec3 proj = fragPosLight.xyz / fragPosLight.w;
    proj = proj * 0.5 + 0.5;
    // Outside the light frustum → fully lit.
    if (proj.z > 1.0 || proj.x < 0.0 || proj.x > 1.0 ||
        proj.y < 0.0 || proj.y > 1.0) return 1.0;

    float bias = max(0.004 * (1.0 - dot(N, L)), 0.0012);
    vec2 texel = 1.0 / vec2(textureSize(shadowMap, 0));
    float spread = 2.6;   // widen the kernel for soft underwater shadows

    // 3x3 PCF (9 taps) with a wide spread → soft shadows at a fraction
    // of the cost of a 5x5 kernel.
    float sh = 0.0;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            float d = texture(shadowMap, proj.xy + vec2(x, y) * texel * spread).r;
            sh += (proj.z - bias > d) ? 0.0 : 1.0;
        }
    }
    return sh / 9.0;
}

// --------------------------------------------------------------
void main() {
    vec3 N = normalize(WorldNormal);
    vec3 L = normalize(sunDirection);
    float sunHeight = L.y;
    float exposure = mix(0.18, 1.0, smoothstep(-0.2, 0.25, sunHeight));

    vec2 uv = TexCoords;
    vec2 worldXZ = FragPos.xz;

    // ===== blending weights =====================================
    // 1) Slope (rocks on steep faces) — lower threshold so cliffs read as rock
    float slope = 1.0 - clamp(N.y, 0.0, 1.0); // 0 flat, 1 vertical
    float rockMask = smoothstep(0.18, 0.55, slope);
    // Break the rock edge with multi-octave noise so it's irregular.
    float rockNoise = fbm(worldXZ * 0.03);
    rockMask = clamp(rockMask + (rockNoise - 0.5) * 0.5, 0.0, 1.0);

    // 2) Depth (sand→mud transition under water). The raw smoothstep is
    //    a dead-straight horizontal line; we wobble the threshold with
    //    noise so the boundary looks organic, not like a contour line.
    float depthWobble = (fbm(worldXZ * 0.018) - 0.5) * 14.0;
    float dShallow = smoothstep(-25.0, -3.0, Depth + depthWobble);
    float rockWeight = rockMask;

    // 3) Lava biome — only where the mask is *warm* (red dominant).
    //    The mask PNG has a lot of green (active biome region) and red
    //    cores (lava itself). Keep only the red cores so lava pools are
    //    isolated, not a giant central blob.
    vec4 lavaSamp = texture(lavaMask, uv);
    float warmth = lavaSamp.r - max(lavaSamp.g, lavaSamp.b);   // >0 only for red
    float lavaBiome = clamp(warmth, 0.0, 1.0) * lavaSamp.a;
    lavaBiome = smoothstep(0.10, 0.45, lavaBiome) * (1.0 - rockMask * 0.4);

    // ===== sample each material (non-tiling) ====================
    // sand & mud use stochastic noTile sampling at two world scales —
    // this removes the obvious repeating grid. Rock uses triplanar so
    // cliffs don't streak.
    // NOTE: single-scale sampling. noTile() already hides the repeat, so
    // the previous two-scale double-sampling was redundant — dropping it
    // halves the per-pixel texture fetches (the terrain shader is the
    // frame's biggest fragment cost) with no visible tiling change.
    vec3 sandCol = noTile(sandDiffuse, worldXZ * 0.035);
    vec3 mudCol  = noTile(mudDiffuse,  worldXZ * 0.032);
    vec3 rockCol = triplanarAlbedo(rockDiffuse, FragPos, N, 0.040);
    vec3 lavaCol = sampleTwoScales (lavaDiffuse,  worldXZ, 0.020, 0.090);

    float sandRgh = noTileR(sandRoughness, worldXZ * 0.035);
    float mudRgh  = noTileR(mudRoughness,  worldXZ * 0.032);
    float rockRgh = triplanarR(rockARM, FragPos, N, 0.040, 1);
    float rockAO  = triplanarR(rockARM, FragPos, N, 0.040, 0);
    float lavaRgh = sampleTwoScalesR(lavaRoughness, worldXZ, 0.020, 0.090);

    vec3 sandN = noTileNormal(sandNormal, worldXZ * 0.035);
    vec3 mudN  = noTileNormal(mudNormal,  worldXZ * 0.032);
    vec3 rockN = normalize(triplanarNormal(rockNormal, FragPos, N, 0.040));
    vec3 lavaN = sampleTwoScalesNormal(lavaNormal, worldXZ, 0.020, 0.090);

    // ===== height-aware blend (sand vs mud) =====================
    // Use albedo luminance as a height proxy so the rougher material
    // pokes through at the transition instead of a muddy linear fade.
    float sandH = dot(sandCol, vec3(0.333));
    float mudH  = dot(mudCol,  vec3(0.333));
    float sm = heightBlend(dShallow, mudH, sandH);   // 0=mud, 1=sand
    vec3  groundCol = mix(mudCol, sandCol, sm);
    float groundRgh = mix(mudRgh, sandRgh, sm);
    vec3  groundN   = normalize(mix(mudN, sandN, sm));

    // ===== blend ground (sand+mud) with rock ===================
    // groundCol already combines sand & mud via height-blend. Now mix
    // in rock by the slope-driven rockWeight, again height-aware so
    // rock juts through rather than fading in muddily.
    float rockH = dot(rockCol, vec3(0.333));
    float gH    = dot(groundCol, vec3(0.333));
    float rb = heightBlend(rockWeight, gH, rockH);
    vec3  albedo    = mix(groundCol, rockCol, rb);
    float roughness = mix(groundRgh, rockRgh, rb);
    vec3  normalTS_soft = groundN;   // tangent-space part (sand/mud)

    // Apply lava biome on top (but rock above lava remains rocky)
    albedo    = mix(albedo,    lavaCol, lavaBiome * 0.85);
    roughness = mix(roughness, lavaRgh, lavaBiome * 0.85);
    normalTS_soft = normalize(mix(normalTS_soft, lavaN, lavaBiome * 0.7));

    // ===== gentle biome tints (castle / river) ==================
    vec4 castleSamp = texture(castleMask, uv);
    float castleWhite = min(castleSamp.r, min(castleSamp.g, castleSamp.b));
    float castleBiome = castleSamp.a * (1.0 - castleWhite);
    castleBiome = smoothstep(0.05, 0.5, castleBiome);
    albedo = mix(albedo, albedo * vec3(0.65, 0.65, 0.70) + vec3(0.04, 0.04, 0.05),
                 castleBiome * 0.55);

    vec4 riverSamp = texture(riverMask, uv);
    float riverWhite = min(riverSamp.r, min(riverSamp.g, riverSamp.b));
    float riverBiome = riverSamp.a * (1.0 - riverWhite);
    riverBiome = smoothstep(0.05, 0.5, riverBiome);
    albedo = mix(albedo, albedo * vec3(0.55, 0.75, 0.65), riverBiome * 0.4);

    // ===== ambient occlusion (rocks only) =======================
    float aoBlend = mix(1.0, mix(1.0, rockAO, 0.85), rb);
    albedo *= aoBlend;

    // ===== macro biome tinting by depth zone ====================
    // Gives readable visual zones (the "биомы непонятно" fix) without
    // hard lines: a large-scale tint that shifts with depth + a slow
    // noise so it's not uniform.
    float zoneNoise = fbm(worldXZ * 0.006);
    float shelfZone = smoothstep(-30.0, -8.0, Depth);   // 1 shallow shelf
    float deepZone  = 1.0 - smoothstep(-70.0, -35.0, Depth); // 1 in the deep
    // Warm sandy tint on the shelf, cold blue-green silt in the deep.
    vec3 shelfTint = vec3(1.08, 1.04, 0.92);
    vec3 deepTint  = vec3(0.80, 0.92, 1.00);
    albedo *= mix(vec3(1.0), shelfTint, shelfZone * (0.5 + 0.5 * zoneNoise));
    albedo *= mix(vec3(1.0), deepTint,  deepZone  * (0.5 + 0.5 * zoneNoise));

    // ===== beach / grass / above-water island shading (Req 3.2, 3.3) ==
    // Depth is the world-space Y of this fragment (waterline at Y=0).
    // Below 0: wet sand. 0..+3: dry beach sand. Above +3 on gentle
    // ground: realistic grass that gets greener/denser toward the
    // island interior (higher + flatter). Cliffs stay rocky.
    float aboveWater = smoothstep(0.0, 1.5, Depth);
    float shoreGlow = 0.0;   // bioluminescent wash glow (set in the beach block)
    // The whole beach / swash / foam / grass block only affects fragments
    // near or above the waterline. Skipping it for the deep seabed (the
    // bulk of an underwater view) avoids a pile of fbm/foam noise ALU per
    // pixel with zero visible change down there — the biggest terrain win.
    if (Depth > -2.5) {
        // Dry beach band: 0..+7 pale warm sand on flat-ish ground.
        float beach = smoothstep(-0.5, 1.5, Depth) * (1.0 - smoothstep(5.0, 9.0, Depth));
        beach *= (1.0 - rockWeight);
        vec3 drySand = sandCol * vec3(1.15, 1.07, 0.92);
        albedo = mix(albedo, drySand, clamp(beach, 0.0, 1.0));

        // (Shore swash / run-up wave on the sand removed — plain dry/wet sand,
        //  no moving wet sheet or oscillating waterline on the beach.)

        // ----- realistic grass on the island interior --------------
        // Grass starts higher now (above the bigger beach) on gentle ground.
        float grassBand   = smoothstep(6.0, 11.0, Depth);
        float flatness     = smoothstep(0.45, 0.15, slope);
        float grassNoise   = fbm(worldXZ * 0.05);
        float patch        = smoothstep(0.35, 0.65, grassNoise);
        float grass = grassBand * flatness * (0.5 + 0.5 * patch);
        vec3 grassLo = vec3(0.32, 0.40, 0.16);
        vec3 grassHi = vec3(0.16, 0.34, 0.12);
        vec3 grassCol = mix(grassLo, grassHi, smoothstep(9.0, 20.0, Depth));
        grassCol *= 0.8 + 0.4 * fbm(worldXZ * 0.4);
        albedo = mix(albedo, grassCol, clamp(grass, 0.0, 1.0));
        roughness = mix(roughness, 0.95, clamp(grass, 0.0, 1.0));

        // (Shore foam line removed — shoreGlow stays 0, no cyan night wash.)
    }

    // ===== final shading normal ================================
    // Build the TBN frame from the geometric normal N to lift the
    // tangent-space (sand/mud/lava) bump map to world space.
    vec3 T = normalize(cross(N, vec3(0.0, 0.0, 1.0)) + vec3(0.001, 0.0, 0.0));
    vec3 B = normalize(cross(N, T));
    vec3 nMapTS_world = normalize(T * normalTS_soft.x + B * normalTS_soft.y + N * normalTS_soft.z);

    // Blend the soft (TS-derived) world-space normal with the rock's
    // already-world-space triplanar normal by the height-aware rock
    // factor (rb). This is the correct way to mix two world-space normals.
    vec3 nMap = normalize(mix(nMapTS_world, rockN, rb));

    // Geometric detail injection: large-scale rocky bumps via fbm.
    // This makes smooth low-poly slopes look chiseled.
    vec2 g = fbmGrad(worldXZ * 0.20);
    float detailAmp = mix(0.4, 1.4, rockWeight);  // strongest on rocks
    nMap = normalize(nMap - vec3(g.x, 0.0, g.y) * detailAmp);

    // Soften normal map on rocks so geometry edges stay readable
    vec3 shadingN = normalize(mix(nMap, N, rockWeight * 0.15));

    roughness = clamp(roughness, 0.25, 0.95);

    // ===== caustics (underwater only, fade with depth) ==========
    vec3 causticsColor = vec3(0.0);
    if (Depth < 0.0) {
        // Sample the refraction-pass caustic texture, tiled in world XZ.
        vec2 cUV = worldXZ / causticsTileSize;

        // 5-tap cross blur (was 9-tap): smooths the caustic grid into soft
        // light at ~half the texture fetches; the difference is imperceptible.
        float texel = 1.0 / 1024.0;
        float c = texture(causticsTex, cUV).r * 0.34;
        c += texture(causticsTex, cUV + vec2( texel,  0.0)).r * 0.165;
        c += texture(causticsTex, cUV + vec2(-texel,  0.0)).r * 0.165;
        c += texture(causticsTex, cUV + vec2( 0.0,  texel)).r * 0.165;
        c += texture(causticsTex, cUV + vec2( 0.0, -texel)).r * 0.165;

        // Single-channel intensity → no chromatic rainbow. We tint it
        // a slightly warm white so it reads as sunlight, not a prism.
        vec3 cau = vec3(c);

        // Contrast curve: bright filaments pop, dim glow stays subtle.
        cau = pow(cau, vec3(1.5)) * 1.2;

        // Fade with depth (water absorbs more of the refracted light)
        float depthFade = smoothstep(-50.0, -3.0, Depth) * (1.0 - smoothstep(-2.0, 0.0, Depth));
        // Caustics only on flat-ish surfaces (sun reaches them)
        float surfaceFlat = clamp(N.y, 0.0, 1.0);
        surfaceFlat = pow(surfaceFlat, 2.0);
        // Sun must be above horizon
        float sunGate = smoothstep(-0.05, 0.30, sunHeight);
        // Warm sunlight tint instead of an RGB split.
        causticsColor = cau * vec3(0.85, 0.97, 0.90)
                      * depthFade * surfaceFlat * sunGate * exposure * 1.5;
    }

    // ===== PBR lighting (Cook-Torrance) =========================
    vec3 V = normalize(cameraPos - FragPos);
    float metallic = 0.0;   // seabed materials are dielectric

    // Sun radiance: warm, attenuated underwater by depth.
    float sunGateL = smoothstep(-0.1, 0.2, sunHeight);
    vec3  sunCol = mix(vec3(1.0, 0.5, 0.2), vec3(1.0, 0.97, 0.92), sunGateL);
    float waterAtten = exp(-max(0.0, -Depth) * 0.012); // light dies with depth
    vec3  sunRadiance = sunCol * (2.6 * exposure) * waterAtten;

    vec3 lit = pbrLight(shadingN, V, L, sunRadiance, albedo, roughness, metallic);

    // Soft PCF shadow attenuates ONLY the direct sun term (ambient and
    // caustics stay so shadowed areas keep the scattered underwater glow).
    float shadow = computeShadow(FragPosLight, shadingN, L);
    lit *= mix(0.35, 1.0, shadow);   // never fully black — water scatters light

    // Image-based-ish ambient (depth-aware hemisphere term).
    vec3 ambSurface = vec3(0.45, 0.65, 0.80);
    vec3 ambDeep    = vec3(0.04, 0.07, 0.12);
    vec3 ambTint = mix(ambDeep, ambSurface, dShallow);
    // Fresnel adds a subtle rim from ambient on grazing angles.
    vec3 F0 = vec3(0.04);
    vec3 Famb = FresnelSchlick(max(dot(shadingN, V), 0.0), F0);
    vec3 ambient = albedo * ambTint * 0.6 * exposure * (1.0 - Famb * 0.5);

    vec3 color = lit + ambient + causticsColor * albedo;

    // Bioluminescent shore glow on the wet-sand wash line at night (matches
    // the glowing breaking wave in surf.frag — the "glowing beach" look).
    float nightShore = smoothstep(0.06, -0.14, sunHeight);
    color += vec3(0.10, 0.92, 1.0) * shoreGlow * nightShore * 0.8;

    // ===== flashlight (camera spotlight) ========================
    // The post-process pass (screen.frag) multiplies the whole frame by
    // a depth-darkening factor exp(-camDepth*0.018). That would dim the
    // torch too. We pre-divide the flashlight term by the SAME factor so
    // the multiply cancels out and the beam keeps full brightness at any
    // depth — "the flashlight pierces the abyss".
    float camDepthFL = max(0.0, -cameraPos.y);
    float depthDarkFL = clamp(exp(-camDepthFL * 0.018), 0.02, 1.0);
    color += applyFlashlight(FragPos, shadingN) * albedo / depthDarkFL;

    // ===== lava emission ========================================
    if (lavaBiome > 0.001) {
        vec3 emCol = sampleTwoScales(lavaEmissive, worldXZ, 0.020, 0.090);
        // Pulse over time
        float pulse = 0.7 + 0.3 * sin(time * 1.7 + worldXZ.x * 0.05 + worldXZ.y * 0.07);
        vec3 emission = emCol * vec3(2.4, 0.9, 0.25) * pulse * lavaBiome;
        color += emission;
    }

    // ===== weather darkening (storm dims the lit scene) =========
    // Applied to the surface lighting before fog so islands/seabed go
    // appropriately moody in a storm. Lava emission already added above
    // keeps its glow because we scale gently.
    color *= mix(1.0, weatherExposure, 1.0 - underwaterFactor * 0.5);

    // ===== underwater fog (distance + depth) ====================
    float dist = length(cameraPos - FragPos);
    float depthDensity = mix(0.004, 0.020, 1.0 - dShallow);
    float fogAmount = 1.0 - exp(-dist * depthDensity);

    vec3 shallowFog = vec3(0.10, 0.32, 0.40) * exposure;
    vec3 deepFog    = vec3(0.005, 0.020, 0.045) * exposure;
    vec3 fogColor = mix(deepFog, shallowFog, dShallow);

    // Underwater fog only applies below the surface; above-water island
    // ground keeps its bright daylight colour.
    float aboveW = smoothstep(0.0, 1.5, Depth);   // 1 on dry land
    color = mix(color, fogColor, clamp(fogAmount, 0.0, 0.95) * (1.0 - aboveW));

    FragColor = vec4(color, 1.0);
}
