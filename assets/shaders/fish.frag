#version 330 core
// Small schooling fish: silver belly / dark blue back, a sharp moving
// glint (the "school flash" when they turn), underwater fog.
in vec3  WorldPos;
in vec3  WorldNormal;
in float BellyT;

out vec4 FragColor;

uniform vec3 cameraPos;
uniform vec3 sunDirection;
uniform float uAir;   // 1 = air mode (sky-lit, no underwater fog), 0 = fish

void main() {
    vec3 N = normalize(WorldNormal);
    vec3 V = normalize(cameraPos - WorldPos);
    if (dot(N, V) < 0.0) N = -N;          // two-sided (tail fin card)
    vec3 L = normalize(sunDirection);

    float exposure = mix(0.2, 1.0, smoothstep(-0.2, 0.25, L.y));

    // Counter-shaded body. Fish: dark-blue back -> silver belly.
    // Air mode: grey back -> white belly.
    vec3 back  = mix(vec3(0.10, 0.16, 0.24), vec3(0.42, 0.45, 0.50), uAir);
    vec3 belly = mix(vec3(0.72, 0.78, 0.80), vec3(0.95, 0.96, 0.97), uAir);
    vec3 albedo = mix(belly, back, smoothstep(-1.0, 1.0, BellyT) );

    float wrap = clamp((dot(N, L) + 0.5) / 1.5, 0.0, 1.0);
    vec3 color = albedo * (0.25 + 0.85 * wrap) * exposure;

    // School flash: tight specular that sparks as they bank into turns.
    vec3 H = normalize(V + L);
    color += vec3(0.9, 0.95, 1.0) * pow(max(dot(N, H), 0.0), 32.0) * 0.9 * exposure;

    // Distance haze: dense blue-green underwater, faint sky haze in air.
    float dist = length(cameraPos - WorldPos);
    float fog      = 1.0 - exp(-dist * mix(0.014, 0.0025, uAir));
    float fogMax   = mix(0.92, 0.5, uAir);
    vec3  fogColor = mix(vec3(0.05, 0.18, 0.25), vec3(0.62, 0.72, 0.82), uAir) * exposure;
    color = mix(color, fogColor, clamp(fog, 0.0, fogMax));

    FragColor = vec4(color, 1.0);
}
