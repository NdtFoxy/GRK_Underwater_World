#version 330 core
out vec4 FragColor;
in vec2 TexCoords;

uniform sampler2D screenTexture;
uniform sampler2D depthTexture;
uniform vec2  sunScreenPos;
uniform float godRayIntensity;
uniform vec3  sunDirection;
uniform float underwaterFactor;

// For volumetric underwater shafts (world-space reconstruction).
uniform mat4  invView;
uniform mat4  invProjection;
uniform vec3  cameraPos;
uniform float time;
uniform float waterLevel;

// Weather colour grade (Req 5 + final post).
uniform vec3  gradeTint;     // colour multiply (cool grey in storm)
uniform float gradeSat;      // saturation (storm desaturates)
uniform float gradeContrast; // contrast
uniform float gradeVignette; // extra vignette strength (storm > calm)

// --- extra post FX ---
uniform float aboveWaterSun; // 1 when sun is above horizon
uniform float autoExposure;  // brightness comp for eye adaptation (depth)
uniform float stormMurk;     // stormIntensity * underwaterFactor: 0..1
uniform float lightningFlash; // 0..1 current flash brightness (decays fast)

// --- Sonar ping (Q) ---
uniform float sonarAge;            // seconds since launch; < 0 = inactive
uniform vec3  sonarOrigin;         // world-space ping centre
uniform float sonarSpeed;          // shell expansion speed (m/s)
uniform float sonarMaxRadius;      // shell stops here
uniform int   sonarContactCount;   // number of creature contacts (<=16)
uniform vec2  sonarContactUV[16];  // screen UV of each contact
uniform float sonarContactDist[16];// distance from ping origin
uniform vec3  sonarContactColor[16];// type colour (red shark / green fish / amber serpent)

#define ABOVE_SAMPLES 40
#define UW_STEPS      28

// ACES Filmic Tonemapping
vec3 ACESFilm(vec3 x) {
    float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x*(a*x+b))/(x*(c*x+d)+e), 0.0, 1.0);
}

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// 3D-ish value noise for the underwater shaft pattern.
float hash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}
float vnoise3(vec3 p) {
    vec3 i = floor(p), f = fract(p);
    f = f*f*(3.0-2.0*f);
    float n000=hash13(i), n100=hash13(i+vec3(1,0,0));
    float n010=hash13(i+vec3(0,1,0)), n110=hash13(i+vec3(1,1,0));
    float n001=hash13(i+vec3(0,0,1)), n101=hash13(i+vec3(1,0,1));
    float n011=hash13(i+vec3(0,1,1)), n111=hash13(i+vec3(1,1,1));
    return mix(mix(mix(n000,n100,f.x),mix(n010,n110,f.x),f.y),
               mix(mix(n001,n101,f.x),mix(n011,n111,f.x),f.y),f.z);
}

// Reconstruct world position from the depth buffer.
vec3 worldFromDepth(vec2 uv, float depth) {
    vec4 clip = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 v = invProjection * clip;
    v /= v.w;
    vec4 w = invView * v;
    return w.xyz;
}

// ---- Bloom: cheap multi-tap bright-pass blur of the scene texture ----
// Samples only the bright part (knee) in a small disk → soft glow.
vec3 bloomSample(vec2 uv) {
    vec2 px = 1.0 / vec2(textureSize(screenTexture, 0));
    vec3 sum = vec3(0.0);
    float wsum = 0.0;
    // 12-tap rotating disk at two radii.
    for (int i = 0; i < 12; ++i) {
        float a = float(i) / 12.0 * 6.2831853;
        float r = (i < 6) ? 3.0 : 7.0;
        vec2 o = vec2(cos(a), sin(a)) * r;
        vec3 s = texture(screenTexture, uv + o * px).rgb;
        float lum = dot(s, vec3(0.2126, 0.7152, 0.0722));
        float bright = max(lum - 1.1, 0.0);          // knee
        sum += s * bright;
        wsum += 1.0;
    }
    return sum / wsum;
}

// ---- Lens flare + ghosts along the screen->sun line ----
vec3 lensFlare(vec2 uv, vec2 sunUV) {
    vec3 col = vec3(0.0);
    vec2 dir = (vec2(0.5) - sunUV);   // toward centre (ghosts mirror across)
    // A few small ghost sprites at fractions along the line.
    float ghosts[4] = float[4](0.35, 0.55, 0.75, 1.15);
    float sizes[4]  = float[4](0.10, 0.06, 0.04, 0.13);
    for (int i = 0; i < 4; ++i) {
        vec2 gp = sunUV + dir * ghosts[i];
        float d = length(uv - gp);
        float g = smoothstep(sizes[i], 0.0, d);
        // Tint each ghost slightly (chromatic look).
        vec3 t = vec3(1.0 - 0.2 * float(i), 0.8, 0.6 + 0.15 * float(i));
        col += g * t * 0.12;
    }
    // Halo ring around the sun — small and only very close to the sun.
    float ds = length(uv - sunUV);
    col += smoothstep(0.06, 0.0, ds) * vec3(1.0, 0.85, 0.6) * 0.4;
    return col;
}

// (Lens raindrops removed — caused screen flicker in storm and a pinching
//  "point" artifact even without storm.)

void main() {
    vec2 texel = 1.0 / vec2(textureSize(screenTexture, 0));

    // --- underwater wobble: distort the whole screen UV under water ---
    vec2 uvBase = TexCoords;
    if (underwaterFactor > 0.0) {
        uvBase.x += sin(TexCoords.y * 42.0 + time * 1.6) * 0.0028 * underwaterFactor;
        uvBase.y += cos(TexCoords.x * 38.0 + time * 1.3) * 0.0028 * underwaterFactor;
    }

    // --- chromatic aberration: split RGB radially from the centre ---
    vec2 caDir = TexCoords - 0.5;
    float caAmt = (0.0012 + underwaterFactor * 0.0022);
    vec3 hdrColor;
    hdrColor.r = texture(screenTexture, uvBase + caDir * caAmt).r;
    hdrColor.g = texture(screenTexture, uvBase).g;
    hdrColor.b = texture(screenTexture, uvBase - caDir * caAmt).b;
    float depthVal = texture(depthTexture, uvBase).r;

    // --- Depth of Field: blur only the FAR field (sky/horizon), never
    // the close water/boat. Disabled underwater (wobble handles that). ---
    {
        float zN = 0.1, zF = 2000.0;
        float ld = (2.0*zN*zF) / (zF + zN - (2.0*depthVal - 1.0) * (zF - zN));
        float dofAmt = smoothstep(400.0, 1200.0, ld) * (1.0 - underwaterFactor);
        if (dofAmt > 0.01) {
            vec2 px = (1.0 / vec2(textureSize(screenTexture, 0))) * (1.0 + 2.0 * dofAmt);
            vec3 b = hdrColor;
            b += texture(screenTexture, uvBase + vec2( px.x,  px.y)).rgb;
            b += texture(screenTexture, uvBase + vec2(-px.x,  px.y)).rgb;
            b += texture(screenTexture, uvBase + vec2( px.x, -px.y)).rgb;
            b += texture(screenTexture, uvBase + vec2(-px.x, -px.y)).rgb;
            b /= 5.0;
            hdrColor = mix(hdrColor, b, clamp(dofAmt, 0.0, 0.6));
        }
    }

    // Sanitize input.
    if (any(isnan(hdrColor)) || any(isinf(hdrColor))) hdrColor = vec3(0.0);
    hdrColor = clamp(hdrColor, 0.0, 64.0);

    // Linearize depth.
    float zNear = 0.1, zFar = 2000.0;
    float linearDepth = (2.0*zNear*zFar) /
        (zFar + zNear - (2.0*depthVal - 1.0) * (zFar - zNear));

    float sunUp = smoothstep(-0.05, 0.15, sunDirection.y);

    // =================================================================
    // UNDERWATER VOLUMETRIC GOD RAYS (accumulate now, ADD after fog)
    // Proper light shafts descending from the surface. We march the
    // view ray in world space and accumulate light penetrating the
    // water, slanted along the sun direction and animated over time.
    // The result is added AFTER the fog pass below, because the shafts
    // ARE scattered light in the water volume — fog must not erase them.
    // =================================================================
    vec3 uwShafts = vec3(0.0);
    if (underwaterFactor > 0.0 && sunUp > 0.0) {
        vec3 wpos = worldFromDepth(TexCoords, depthVal);
        vec3 rd   = wpos - cameraPos;
        float marchLen = min(length(rd), 140.0);
        rd = normalize(rd);

        float jitter = hash12(TexCoords * 1000.0 + fract(time));
        float stepLen = marchLen / float(UW_STEPS);

        // Light travels DOWN along -sunDirection. For each marched point
        // we project it back UP to the water surface along that ray and
        // sample a 2D light pattern there. Points lit by the same
        // surface spot share the same beam → coherent straight shafts
        // parallel to the sun, instead of 3D blobs.
        vec3 lightDir = normalize(-sunDirection);          // downward
        float invLy = 1.0 / max(-lightDir.y, 0.08);        // guard near-horizontal

        float accum = 0.0;
        for (int i = 0; i < UW_STEPS; ++i) {
            float t = (float(i) + jitter) * stepLen;
            vec3 P = cameraPos + rd * t;

            float depthBelow = waterLevel - P.y;           // >0 underwater
            if (depthBelow < 0.5) continue;                // skip right at surface

            // March back up to the surface along the light ray.
            float s = depthBelow * invLy;
            vec2 entry = P.xz - lightDir.xz * s;

            // Surface light pattern: two layered moving stripe sets
            // broken by noise → narrow, organic shafts (not uniform).
            vec2 uvA = entry * 0.05 + vec2(time * 0.012, time * 0.008);
            vec2 uvB = entry * 0.11 - vec2(time * 0.010, time * 0.015);
            float s1 = 0.5 + 0.5 * sin(uvA.x * 6.2831 + sin(uvA.y * 3.0) * 1.4);
            float s2 = 0.5 + 0.5 * sin(uvB.x * 6.2831 + sin(uvB.y * 5.0) * 1.1);
            float n  = vnoise3(vec3(entry * 0.07, time * 0.04));
            // Multiply stripe sets so only their overlaps form bright
            // beams → fewer, sharper shafts.
            float beam = s1 * s2;
            // Higher threshold -> FEWER, sharper shafts (distinct sun beams,
            // not an overwhelming uniform curtain).
            beam = smoothstep(0.60, 0.95, beam * 0.7 + n * 0.35);

            // Beer-Lambert depth falloff, plus a gentle fade-in just
            // under the surface so beams emerge rather than pop.
            float penetration = exp(-depthBelow * 0.06);
            float emerge = smoothstep(0.5, 6.0, depthBelow);
            accum += beam * penetration * emerge;
        }
        accum = (accum / float(UW_STEPS)) * 2.0;

        // Soften toward grazing view (less shaft when looking along them).
        vec3 shaftColor = vec3(0.5, 0.92, 0.85);
        uwShafts = shaftColor * accum * godRayIntensity * sunUp * underwaterFactor * 0.18;
    }

    // =================================================================
    // ABOVE-WATER GOD RAYS (screen-space radial light shafts)
    // Key fix: only SKY pixels (at the far plane) act as light sources.
    // This stops the sun's bright mirror reflection on the water from
    // streaking into a hard vertical column.
    // =================================================================
    if (underwaterFactor < 0.5) {
        vec2 sunUV = sunScreenPos;
        float sunOnScreen = 1.0 - smoothstep(0.0, 1.4, length(sunUV - vec2(0.5)));

        if (sunOnScreen > 0.01 && sunUp > 0.0) {
            vec2 rayDir = sunUV - TexCoords;
            float rayLen = length(rayDir);
            vec2 stepUV = rayDir / float(ABOVE_SAMPLES);
            float jitter = hash12(TexCoords * 1000.0 + vec2(0.5)) * 0.5;
            vec2 coord = TexCoords + stepUV * jitter;

            float decay = 1.0;
            vec3 rays = vec3(0.0);
            for (int i = 0; i < ABOVE_SAMPLES; ++i) {
                coord += stepUV;
                vec2 sUV = clamp(coord, vec2(0.001), vec2(0.999));
                // Only sky contributes (depth at/near the far plane).
                float sDepth = texture(depthTexture, sUV).r;
                float skyMask = step(0.9995, sDepth);
                vec3 s = texture(screenTexture, sUV).rgb;
                float lum = dot(s, vec3(0.2126, 0.7152, 0.0722));
                float bright = smoothstep(1.5, 5.0, lum);
                // Clamp per-sample so one ultra-bright texel can't form
                // a solid line.
                rays += min(s, vec3(8.0)) * bright * skyMask * decay * 0.012;
                decay *= 0.975;
            }
            float distFade = 1.0 - smoothstep(0.0, 0.9, rayLen);
            hdrColor += rays * godRayIntensity * sunOnScreen * distFade * sunUp;
        }
    }

    // =================================================================
    // UNDERWATER FOG / TINT
    // =================================================================
    if (underwaterFactor > 0.0) {
        // Storm churns up the water: visibility drops sharply (stormMurk
        // is stormIntensity x underwaterFactor, fed from Scene).
        float fogDensity = 0.03 + 0.05 * stormMurk;
        float fogFactor = clamp(1.0 - exp(-linearDepth * fogDensity), 0.0, 1.0);
        // Richer, less-flat palette: near water reads teal-green, distance falls
        // off into a deep blue — a visible depth gradient instead of flat navy.
        vec3 deepWaterColor    = vec3(0.00, 0.07, 0.17);   // far / deep: deep blue
        vec3 surfaceWaterColor = vec3(0.05, 0.30, 0.40);   // near: saturated teal
        vec3 fogColor = mix(surfaceWaterColor, deepWaterColor, min(linearDepth*0.012, 1.0));
        // --- Option B: water's scattered light = surface sun intensity, on the
        // SAME gradual curve as the sky (so sunset under water is dusky, not
        // black), warm near the surface at low sun, cold/dark in the deep. ---
        float sunI   = smoothstep(-0.25, 0.18, sunDirection.y);   // 0 night .. 1 day (~0.5 at sunset)
        float lowSun = smoothstep(0.30, 0.0, sunDirection.y);     // 1 near horizon, 0 high noon
        // Warm sunset light only reaches the shallows; depth absorbs the warm
        // wavelengths first, so the deep stays cold blue.
        float nearSurf = 1.0 - min(linearDepth * 0.02, 1.0);
        vec3  sunTint  = mix(vec3(1.0), vec3(1.5, 0.85, 0.5), lowSun * nearSurf);
        fogColor *= mix(0.06, 1.0, sunI) * sunTint;               // night water → near black
        hdrColor = mix(hdrColor, fogColor, fogFactor * underwaterFactor);
        hdrColor *= mix(vec3(1.0), vec3(0.4, 0.8, 1.0), underwaterFactor);

        // Light shafts are scattered light WITHIN the water volume, so
        // they sit on top of the fog (additive) — this is why they were
        // invisible before: the fog mix above used to erase them.
        hdrColor += uwShafts;

        // ---- depth darkness ----
        // The deeper the CAMERA descends, the less sunlight reaches it.
        // Past ~140m it's pitch black (Subnautica-style "you need a
        // light down here"). Flashlight is added before tonemap in the
        // terrain/cave shaders, so it still pierces this darkness.
        float camDepth = max(0.0, waterLevel - cameraPos.y);
        float lightLevel = exp(-camDepth * 0.018);   // 1 at surface → ~0 at depth
        lightLevel = clamp(lightLevel, 0.02, 1.0);
        hdrColor *= lightLevel;
        // Tint the residual toward cold abyssal blue as it darkens.
        hdrColor = mix(hdrColor, hdrColor * vec3(0.4, 0.6, 0.9), 1.0 - lightLevel);
    }

    // =================================================================
    // (Lens flare / big bloom removed — caused a white blob on screen.)
    // Keep only a faint bloom on very bright water sparkles.
    // =================================================================
    if (underwaterFactor < 0.5) {
        vec3 bloom = bloomSample(uvBase);
        hdrColor += bloom * 0.25;
    }

    // Auto-exposure / eye adaptation: lift the image as the camera dives
    // into darkness so the deep stays readable, then tonemap clamps it.
    hdrColor *= autoExposure;

    // Lightning: bright white flash above water; dim, brief, blue-shifted underwater.
    if (lightningFlash > 0.001) {
        vec3 flashCol = mix(vec3(1.4, 1.45, 1.5),   // above water: bright cool white
                            vec3(0.15, 0.30, 0.55),  // underwater: dim blue
                            clamp(underwaterFactor, 0.0, 1.0));
        hdrColor += flashCol * lightningFlash;
    }

    // Guard before tonemap.
    if (any(isnan(hdrColor)) || any(isinf(hdrColor))) hdrColor = vec3(0.0);
    hdrColor = clamp(hdrColor, 0.0, 64.0);

    // Tonemap + gamma.
    vec3 mapped = ACESFilm(max(hdrColor, vec3(0.0)) * 0.7);
    if (any(isnan(mapped)) || any(isinf(mapped))) mapped = vec3(0.0);
    mapped = pow(mapped, vec3(1.0/2.2));

    // ---- weather colour grade (final look) ----
    // Saturation around luminance, then contrast around mid-grey, then
    // a colour-tint multiply. Storm desaturates + cools; tropical pops.
    float luma = dot(mapped, vec3(0.2126, 0.7152, 0.0722));
    mapped = mix(vec3(luma), mapped, gradeSat);
    mapped = (mapped - 0.5) * gradeContrast + 0.5;
    mapped *= gradeTint;
    mapped = clamp(mapped, 0.0, 1.0);

    // Storm underwater murk: desaturate + darken toward a cold blue-grey.
    // Only active when BOTH stormy (stormMurk > 0) AND underwater (factor baked in).
    if (stormMurk > 0.001) {
        float murkLuma = dot(mapped, vec3(0.299, 0.587, 0.114));
        vec3  murkCol  = vec3(murkLuma) * vec3(0.55, 0.62, 0.72);
        mapped = mix(mapped, murkCol, clamp(stormMurk, 0.0, 1.0) * 0.6);
    }

    // Vignette (weather strengthens it for a moodier storm).
    vec2 uv = TexCoords * 2.0 - 1.0;
    mapped *= 1.0 - dot(uv, uv) * (0.12 + gradeVignette);

    // =================================================================
    // SONAR PING (Q): an expanding spherical shell from sonarOrigin.
    // Over the depth buffer it draws a bright travelling contour that
    // reveals terrain/cave/wreck silhouettes in the dark; for each known
    // creature it blooms a colour-coded blip as the wavefront reaches it,
    // so the shark (red) and fish (green) light up even in pitch black.
    // =================================================================
    if (sonarAge >= 0.0) {
        vec2  res    = vec2(textureSize(screenTexture, 0));
        float aspect = res.x / res.y;
        float ringR  = sonarAge * sonarSpeed;
        float lifeFade = clamp(1.0 - ringR / sonarMaxRadius, 0.0, 1.0);
        float ageFade  = smoothstep(0.0, 0.05, sonarAge) * lifeFade;

        // 1) Geometry shell (skip sky pixels at the far plane).
        if (depthVal < 0.9995) {
            vec3  wp   = worldFromDepth(uvBase, depthVal);
            float d    = distance(wp, sonarOrigin);
            float band = exp(-pow((d - ringR) / 4.0, 2.0));   // glowing shell
            float within = step(d, sonarMaxRadius);
            mapped += vec3(0.25, 0.95, 1.0) * band * within * ageFade * 1.2;
        }

        // 2) Creature blips — colour-coded, visible through darkness.
        for (int i = 0; i < sonarContactCount; ++i) {
            float dr     = ringR - sonarContactDist[i];       // wave has reached it
            float pass   = smoothstep(0.0, 3.0, dr);
            float linger = exp(-max(dr, 0.0) / 40.0);         // fades after the pass
            vec2  dd     = (TexCoords - sonarContactUV[i]); dd.x *= aspect;
            float g      = exp(-dot(dd, dd) / 0.0030);        // soft screen-space blob
            mapped += sonarContactColor[i] * g * pass * linger * ageFade * 1.5;
        }
        mapped = clamp(mapped, 0.0, 1.0);
    }

    // Film grain + ordered dithering — hides banding in sky/fog and adds
    // a subtle cinematic texture.
    float grain = hash12(TexCoords * vec2(textureSize(screenTexture, 0)) + fract(time) * 100.0);
    mapped += (grain - 0.5) * 0.025;
    mapped = clamp(mapped, 0.0, 1.0);

    FragColor = vec4(mapped, 1.0);
}
