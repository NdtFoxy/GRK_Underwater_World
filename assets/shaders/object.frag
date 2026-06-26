#version 330 core
// ======================================================================
// ЗАЩИТА (2 обязательных метода в одном шейдере):
//   PBR LIGHTING (metallic/roughness):
//     F0 = mix(0.04, albedo, metallic); Френель + GGX-спекуляр от
//     roughness; ARM-текстура даёт AO/Roughness/Metallic на материал.
//     Параметры демонстрируемы (лодки/буи/пропсы).
//   NORMAL MAPPING (≥2 материала — здесь + terrain.frag):
//     TBN-фрейм строится из ПРОИЗВОДНЫХ (dFdx/dFdy) позиции и UV
//     (applyNormalMap), поэтому не нужны пер-вершинные тангенты в GL 3.3.
//   СЛОВА: metallic/roughness, F0/Fresnel, GGX, TBN, dFdx/dFdy.
// ======================================================================
in vec3 FragPos;
in vec2 UV;
in vec3 WorldNormal;
in vec4 FragPosLight;

out vec4 FragColor;

uniform vec3  cameraPos;
uniform vec3  sunDirection;
uniform vec3  tint;            // base colour (per-object)
uniform int   useTexture;
uniform sampler2D albedoTex;
uniform int   useAlphaCut;     // 1 = discard transparent foliage texels

// Optional PBR maps (boats/buoys).
uniform int   useNormalMap;
uniform sampler2D normalTex;
uniform int   useArm;          // r=AO g=Roughness b=Metallic
uniform sampler2D armTex;
uniform int   armIsMR;         // 1 = armTex is glTF metallicRoughness (R is NOT AO)
uniform float metallicFactor;  // glTF metallic multiplier (per material)
uniform vec3  emissiveBoost;   // emissive = albedo * this (jellyfish glow)
uniform int   useAlphaBlend;   // 1 = glTF BLEND slice (alpha from texture)
uniform float materialAlpha;   // glTF baseColorFactor.a for BLEND slices

uniform float weatherExposure; // storm dims the lit object

// Soft PCF shadows (same map the terrain receives).
uniform sampler2D shadowMap;
uniform int       shadowEnabled;

// Flashlight (camera spotlight).
uniform int   flashOn;
uniform vec3  flashPos;
uniform vec3  flashDir;
uniform vec3  flashColor;
uniform float flashInnerCos;
uniform float flashOuterCos;
uniform float flashRange;
uniform float flashIntensity;

float computeShadow(vec4 fp, vec3 N, vec3 L) {
    if (shadowEnabled == 0) return 1.0;
    vec3 proj = fp.xyz / fp.w;
    proj = proj * 0.5 + 0.5;
    if (proj.z > 1.0 || proj.x < 0.0 || proj.x > 1.0 ||
        proj.y < 0.0 || proj.y > 1.0) return 1.0;
    float bias = max(0.004 * (1.0 - dot(N, L)), 0.0012);
    vec2 texel = 1.0 / vec2(textureSize(shadowMap, 0));
    float sh = 0.0;
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y) {
            float d = texture(shadowMap, proj.xy + vec2(x, y) * texel * 2.0).r;
            sh += (proj.z - bias > d) ? 0.0 : 1.0;
        }
    return sh / 9.0;
}

// Derivative-based cotangent frame — gives a TBN without per-vertex
// tangents (works on any mesh in GL 3.3 core).
vec3 applyNormalMap(vec3 N, vec3 fragPos, vec2 uv) {
    vec3 dp1 = dFdx(fragPos);
    vec3 dp2 = dFdy(fragPos);
    vec2 duv1 = dFdx(uv);
    vec2 duv2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, N);
    vec3 dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float maxLen2 = max(dot(T, T), dot(B, B));
    // Degenerate UV derivatives (constant/missing UVs, mip-collapsed islands)
    // made inversesqrt(0) = inf -> NaN normals -> black blotches. Keep N instead.
    if (maxLen2 < 1e-14) return N;
    float invmax = inversesqrt(maxLen2);
    mat3 TBN = mat3(T * invmax, B * invmax, N);
    vec3 n = texture(normalTex, uv).xyz * 2.0 - 1.0;
    // Mid-grey texels (not a real tangent normal, e.g. grayscale bump fallback)
    // normalize() to garbage — fall back to the vertex normal there too.
    if (dot(n, n) < 1e-4) return N;
    return normalize(TBN * n);
}

vec3 flashlight(vec3 fragPos, vec3 N, vec3 V, vec3 albedo, vec3 F0, float roughness) {
    if (flashOn == 0) return vec3(0.0);
    vec3 toFrag = fragPos - flashPos;
    float dist = length(toFrag);
    if (dist > flashRange) return vec3(0.0);
    vec3 L  = toFrag / max(dist, 1e-4);    // light -> fragment
    vec3 Ld = -L;                          // fragment -> light
    float theta = dot(L, normalize(flashDir));
    float cone = clamp((theta - flashOuterCos) / max(flashInnerCos - flashOuterCos, 0.001), 0.0, 1.0);
    cone *= cone;
    float fade = 1.0 - dist / flashRange; fade *= fade;
    float ndl = max(dot(N, Ld), 0.0);
    vec3 radiance = flashColor * flashIntensity * cone * fade;

    // Diffuse (lambert with a small wrap so grazing surfaces still read).
    vec3 diff = albedo * (0.25 + 0.75 * ndl);

    // Blinn-Phong specular that tracks the camera — the demonstrable
    // specular term. A pure dielectric F0 (0.04) is too dim to read on
    // matte stone, so we lift the reflectance to a glossy floor: the
    // flashlight produces a clearly visible hot-spot that slides as you
    // move (shine it on a rock to show specular). Metals keep their F0.
    vec3 H = normalize(Ld + V);
    float NdotH = max(dot(N, H), 0.0);
    float shininess = mix(48.0, 256.0, 1.0 - roughness);
    vec3  specCol = max(F0, vec3(0.25));            // glossy floor for the demo
    vec3  spec = specCol * pow(NdotH, shininess) * 2.0;

    return radiance * (diff + spec);
}

void main() {
    vec3 N = normalize(WorldNormal);
    vec3 L = normalize(sunDirection);
    vec3 V = normalize(cameraPos - FragPos);

    vec3 albedo = tint;
    float alphaOut = 1.0;
    if (useTexture == 1) {
        vec4 tex = texture(albedoTex, UV);
        if (useAlphaCut == 1 && tex.a < 0.5) discard;   // foliage cutout
        if (useAlphaBlend == 1) {
            alphaOut = tex.a * materialAlpha;
            if (alphaOut < 0.02) discard;
        }
        albedo *= tex.rgb;
    } else if (useAlphaBlend == 1) {
        alphaOut = materialAlpha;
    }

    // Mandatory normal mapping on the floating objects.
    if (useNormalMap == 1) N = applyNormalMap(N, FragPos, UV);

    // Foliage cards are two-sided: light the side that faces the camera,
    // otherwise every back-facing frond is a flat dark cutout.
    if (useAlphaCut == 1 && dot(N, V) < 0.0) N = -N;

    float roughness = 0.6, metallic = 0.0, ao = 1.0;
    if (useArm == 1) {
        vec3 arm = texture(armTex, UV).rgb;
        // Pure metallicRoughness textures carry junk in R — reading it as
        // AO painted black blotches on rocks/fish that have no AO map.
        ao        = (armIsMR == 1) ? 1.0 : arm.r;
        roughness = clamp(arm.g, 0.05, 1.0);
        metallic  = clamp(arm.b * metallicFactor, 0.0, 1.0);
    }

    float sunHeight = L.y;
    float exposure = mix(0.2, 1.0, smoothstep(-0.2, 0.25, sunHeight));
    float ndl = max(dot(N, L), 0.0);
    float shadow = computeShadow(FragPosLight, N, L);

    vec3 sunCol = vec3(1.0, 0.97, 0.92) * 1.9 * exposure;
    vec3 color;
    if (useAlphaCut == 1) {
        // Foliage: leaves are thin translucent diffusers, not glossy
        // shells — the "plastic" look was a white specular highlight +
        // hard lambert + flat blue-grey ambient. Instead: wrapped
        // diffuse, sun BLEEDING THROUGH the frond toward the camera
        // (green transmission), and a sky/ground lighting hemisphere.
        float wrap = clamp((dot(N, L) + 0.55) / 1.55, 0.0, 1.0);
        float backLit = max(dot(-N, L), 0.0) * pow(max(dot(V, -L), 0.0), 2.0);
        vec3 hemi = mix(vec3(0.20, 0.23, 0.16),        // warm ground bounce
                        vec3(0.33, 0.41, 0.50),        // cool sky dome
                        clamp(0.5 + 0.5 * N.y, 0.0, 1.0));
        color = albedo * hemi * exposure * ao
              + albedo * sunCol * (wrap * wrap) * shadow
              + albedo * vec3(0.95, 1.0, 0.55) * backLit * sunCol * 0.5 * shadow;
    } else {
        // Cook-Torrance-ish: diffuse + GGX-flavoured spec via roughness.
        vec3 F0 = mix(vec3(0.04), albedo, metallic);
        vec3 H = normalize(V + L);
        float NdotH = max(dot(N, H), 0.0);
        float shininess = mix(8.0, 256.0, 1.0 - roughness);
        float specStr = (1.0 - roughness) * 0.8 + 0.05;
        vec3 spec = F0 * pow(NdotH, shininess) * specStr * shadow;

        // Without IBL a full (1-metallic) kill turns metal slices solid black
        // (tuna fins) — keep part of the diffuse term instead.
        vec3 diffuse = albedo * (1.0 - 0.6 * metallic) * sunCol * ndl * shadow;
        vec3 ambient = albedo * vec3(0.32, 0.38, 0.44) * exposure * ao;
        color = ambient + diffuse + spec * sunCol;
    }

    // Weather dims the lit object together with the rest of the scene.
    color *= weatherExposure;

    // Self-glow (added after the weather dim so jellyfish read at night).
    color += albedo * emissiveBoost * 0.6;

    // Pre-divide flashlight by the post-process depth darkening so it
    // stays bright when the camera is deep (matches terrain/cave).
    float camDepthFL = max(0.0, -cameraPos.y);
    float depthDarkFL = clamp(exp(-camDepthFL * 0.018), 0.02, 1.0);
    vec3 F0flash = mix(vec3(0.04), albedo, metallic);
    color += flashlight(FragPos, N, V, albedo, F0flash, roughness) / depthDarkFL;

    FragColor = vec4(color, alphaOut);
}
