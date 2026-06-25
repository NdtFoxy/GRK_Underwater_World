#version 330 core
in vec3 FragPos;
in vec3 WorldNormal;
in vec2 UV;
in vec4 FragPosLight;

out vec4 FragColor;

uniform vec3  cameraPos;
uniform vec3  sunDirection;
uniform float time;

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

float hash21(vec2 p){ return fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); }

// Large-radius PCF — soft, diffuse underwater shadows.
float shadowPCF(vec4 fragPosLight, vec3 N, vec3 L) {
    if (shadowEnabled == 0) return 1.0;
    vec3 proj = fragPosLight.xyz / fragPosLight.w;
    proj = proj * 0.5 + 0.5;
    if (proj.z > 1.0) return 1.0;
    float bias = max(0.004 * (1.0 - dot(N, L)), 0.001);
    float sh = 0.0;
    vec2 texel = 1.0 / vec2(textureSize(shadowMap, 0));
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y) {
            float d = texture(shadowMap, proj.xy + vec2(x, y) * texel * 2.0).r;
            sh += (proj.z - bias > d) ? 0.0 : 1.0;
        }
    return sh / 9.0;
}

vec3 flashlight(vec3 fragPos, vec3 N){
    if (flashOn == 0) return vec3(0.0);
    vec3 toFrag = fragPos - flashPos;
    float dist = length(toFrag);
    if (dist > flashRange) return vec3(0.0);
    vec3 L = toFrag / max(dist, 1e-4);
    float theta = dot(L, normalize(flashDir));
    float cone = clamp((theta - flashOuterCos)/max(flashInnerCos-flashOuterCos,0.001),0.0,1.0);
    cone *= cone;
    float fade = 1.0 - dist/flashRange; fade *= fade;
    float ndl = max(dot(N, -L), 0.0);
    return flashColor * flashIntensity * cone * fade * (0.25 + 0.75*ndl);
}

void main() {
    vec3 N = normalize(WorldNormal);
    vec3 L = normalize(sunDirection);
    vec3 V = normalize(cameraPos - FragPos);

    // Iridescent sea-serpent skin: blue-green body with a brighter belly
    // and faint scale banding along the body.
    float belly = smoothstep(-0.2, 0.9, dot(N, vec3(0,-1,0)));
    vec3 backCol  = vec3(0.05, 0.20, 0.28);
    vec3 bellyCol = vec3(0.55, 0.78, 0.70);
    vec3 base = mix(backCol, bellyCol, belly);
    // Scale bands around the ring + along the body.
    float bands = 0.85 + 0.15 * sin(UV.y * 40.0) * sin(UV.x * 120.0);
    base *= bands;
    // Iridescent rim from view angle.
    float rim = pow(1.0 - max(dot(N, V), 0.0), 3.0);
    base += vec3(0.1, 0.3, 0.4) * rim;

    float sunHeight = L.y;
    float exposure = mix(0.18, 1.0, smoothstep(-0.2, 0.25, sunHeight));
    float ndl = max(dot(N, L), 0.0);
    float shadow = shadowPCF(FragPosLight, N, L);

    vec3 sunCol = vec3(1.0, 0.97, 0.92) * 1.6 * exposure;
    vec3 ambient = base * vec3(0.18, 0.30, 0.38) * exposure;
    vec3 color = ambient + base * sunCol * ndl * shadow;

    // Specular sheen (wet skin).
    vec3 H = normalize(V + L);
    float spec = pow(max(dot(N, H), 0.0), 48.0) * shadow;
    color += vec3(0.6) * spec * exposure;

    // Flashlight (pre-divided by the post-process depth-darkening so the
    // beam stays bright at depth — same trick as terrain/cave).
    float camDepthFL = max(0.0, -cameraPos.y);
    float depthDarkFL = clamp(exp(-camDepthFL * 0.018), 0.02, 1.0);
    color += flashlight(FragPos, N) * base / depthDarkFL;

    // Underwater fog by distance.
    float dist = length(cameraPos - FragPos);
    float fog = 1.0 - exp(-dist * 0.010);
    vec3 fogCol = vec3(0.02, 0.10, 0.16) * exposure;
    color = mix(color, fogCol, clamp(fog, 0.0, 0.9));

    FragColor = vec4(color, 1.0);
}
