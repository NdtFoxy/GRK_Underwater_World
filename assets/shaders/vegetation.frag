#version 330 core

in vec2  vUV;
in vec3  vWorldPos;
in float vSway;       // 0=root, 1=tip
in float vDistFade;

uniform vec3 sunDirection;
uniform vec3 cameraPos;
uniform int  useTexture;
uniform int  landMode;     // 1 = above-water plant (palm): sky-lit, no fog
uniform int  landCutout;   // landMode: 1 = keep the leaf cutout (grass blades)
uniform vec3 colorRoot;    // procedural plant gradient; root.r < 0 = built-ins
uniform vec3 colorTip;
uniform sampler2D albedoTex;

// --- flashlight (camera spotlight) ---------------------------------
uniform int   flashOn;
uniform vec3  flashPos;
uniform vec3  flashDir;
uniform vec3  flashColor;
uniform float flashInnerCos;
uniform float flashOuterCos;
uniform float flashRange;
uniform float flashIntensity;

vec3 applyFlashlight(vec3 fragPos) {
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
    return flashColor * flashIntensity * cone * distFade;
}

out vec4 FragColor;

// Procedural leaf alpha — vertical streaks that shrink toward the tip.
// Used when no asset texture is bound.
float proceduralLeafAlpha(vec2 uv) {
    float streaks = 0.5
        + 0.5 * sin((uv.x * 7.0 + uv.y * 0.4) * 6.2831);
    float taper = smoothstep(0.0, 0.15, 1.0 - uv.y);
    float sideFade = smoothstep(0.0, 0.20, uv.x)
                   * smoothstep(0.0, 0.20, 1.0 - uv.x);
    return streaks * taper * sideFade;
}

void main() {
    vec3 baseCol;
    float alpha;

    if (useTexture == 1) {
        vec4 texSample = texture(albedoTex, vUV);
        baseCol = texSample.rgb;
        // Real kelp/coral textures use alpha for cutout; sample it.
        alpha = texSample.a * vDistFade;
        // Fall back to a luminance-based mask if the texture has no
        // alpha channel (alpha will be 1 everywhere).
        if (alpha > 0.99 * vDistFade) {
            float lum = dot(baseCol, vec3(0.299, 0.587, 0.114));
            alpha = smoothstep(0.05, 0.20, lum) * vDistFade;
        }
    } else {
        alpha = proceduralLeafAlpha(vUV) * vDistFade;
        baseCol = (colorRoot.r >= 0.0)
            ? mix(colorRoot, colorTip, vSway)
            : mix(vec3(0.05, 0.18, 0.06),
                  vec3(0.20, 0.55, 0.18),
                  vSway);
    }
    // Land foliage (procedural palm) is solid geometry — keep all of it.
    // Grass keeps the cutout so the cards read as blades, not quads.
    if (landMode == 1 && useTexture == 0 && landCutout == 0) alpha = 1.0;
    if (alpha < 0.05) discard;

    vec3 L = normalize(sunDirection);
    float sunHeight = L.y;
    float exposure = mix(0.18, 1.0, smoothstep(-0.2, 0.25, sunHeight));

    // Bioluminescent tip glow, subtle so it works for both kelp & coral
    vec3 tipGlow = vec3(0.55, 0.80, 0.30) *
                   smoothstep(0.7, 1.0, vSway) * 0.4;

    // Translucent leaf wrap lighting.
    vec3 toCam = normalize(cameraPos - vWorldPos);
    float wrap = max(0.0, 0.5 + 0.5 * dot(L, vec3(0.0, 1.0, 0.0)));
    vec3 ambient = baseCol * vec3(0.10, 0.20, 0.25) * exposure;

    vec3 color = baseCol * (0.4 + 0.6 * wrap) * exposure
               + ambient
               + tipGlow * exposure;

    // Flashlight catches the plants. Pre-divide by the post-process
    // depth-darkening factor so the torch stays bright deep down.
    float camDepthFL = max(0.0, -cameraPos.y);
    float depthDarkFL = clamp(exp(-camDepthFL * 0.018), 0.02, 1.0);
    color += applyFlashlight(vWorldPos) * baseCol / depthDarkFL;

    if (landMode == 1) {
        // Above-water foliage: self-coloured when untextured. Custom
        // gradient (grass) wins; otherwise the built-in palm look —
        // brown trunk (low UV.y) → green fronds (high UV.y). Bright sky
        // lighting, NO underwater fog or bioluminescent tip glow.
        if (useTexture == 0) {
            if (colorRoot.r >= 0.0) {
                baseCol = mix(colorRoot, colorTip, vSway);
            } else {
                vec3 bark  = vec3(0.40, 0.27, 0.15);
                vec3 frond = vec3(0.18, 0.42, 0.14);
                baseCol = mix(bark, frond, smoothstep(0.4, 0.55, vUV.y));
            }
        }
        float ndl = max(dot(L, vec3(0.0, 1.0, 0.0)), 0.0);
        color = baseCol * (0.45 + 0.75 * ndl) * exposure;
        // Grass tips catch a little sky bounce so the meadow isn't flat.
        color += baseCol * 0.18 * vSway * exposure;
        float dist = length(cameraPos - vWorldPos);
        float haze = 1.0 - exp(-dist * 0.0025);
        color = mix(color, vec3(0.65, 0.75, 0.85) * exposure, clamp(haze, 0.0, 0.6));
        color += applyFlashlight(vWorldPos) * baseCol;
        FragColor = vec4(color, (landCutout == 1) ? alpha : 1.0);
        return;
    }

    // Underwater fog
    float dist = length(cameraPos - vWorldPos);
    float fog = 1.0 - exp(-dist * 0.012);
    vec3 fogColor = vec3(0.05, 0.18, 0.25) * exposure;
    color = mix(color, fogColor, clamp(fog, 0.0, 0.9));

    FragColor = vec4(color, alpha);
}
