#version 330 core

in vec3 FragPos;
in vec2 TexCoords;
in vec3 WorldNormal;

uniform sampler2D sandDiffuse;
uniform sampler2D sandNormal;
uniform sampler2D sandRoughness;
uniform vec3 sunDirection;
uniform vec3 cameraPos;
uniform float time;
uniform float underwaterFactor;

out vec4 FragColor;

#define PI 3.14159265

void main() {
    // Sample sand textures
    vec3 albedo = texture(sandDiffuse, TexCoords).rgb;
    vec3 normalTex = texture(sandNormal, TexCoords).xyz * 2.0 - 1.0;
    float roughness = texture(sandRoughness, TexCoords).r;
    
    // Build TBN for normal mapping (flat plane, so tangent = X, bitangent = Z)
    vec3 T = vec3(1.0, 0.0, 0.0);
    vec3 B = vec3(0.0, 0.0, 1.0);
    vec3 N = vec3(0.0, 1.0, 0.0);
    vec3 normal = normalize(T * normalTex.x + B * normalTex.y + N * normalTex.z);
    
    // Caustics - animated light patterns on the sand
    vec2 causticsUV1 = FragPos.xz * 0.3 + vec2(time * 0.02, time * 0.015);
    vec2 causticsUV2 = FragPos.xz * 0.25 - vec2(time * 0.018, time * 0.01);
    float caustic1 = texture(sandNormal, causticsUV1).r;
    float caustic2 = texture(sandNormal, causticsUV2).g;
    float caustics = pow(caustic1 * caustic2, 1.5) * 3.0;
    
    // Lighting
    vec3 sunDir = normalize(sunDirection);
    float sunHeight = sunDir.y;
    float currentExposure = mix(0.1, 1.0, smoothstep(-0.2, 0.2, sunHeight));
    
    // Diffuse
    float NdotL = max(dot(normal, sunDir), 0.0);
    vec3 diffuse = albedo * NdotL * currentExposure;
    
    // Ambient (tinted by water absorption - blue/green)
    vec3 waterTint = vec3(0.15, 0.5, 0.6);
    vec3 ambient = albedo * 0.15 * waterTint * currentExposure;
    
    // Add caustics (only when sun is up)
    vec3 causticsColor = vec3(0.3, 0.7, 0.6) * caustics * currentExposure * smoothstep(-0.1, 0.2, sunHeight);
    
    // Specular (wet sand)
    vec3 viewDir = normalize(cameraPos - FragPos);
    vec3 H = normalize(sunDir + viewDir);
    float NdotH = max(dot(normal, H), 0.0);
    float spec = pow(NdotH, mix(16.0, 64.0, 1.0 - roughness)) * (1.0 - roughness) * 0.5;
    vec3 specColor = vec3(1.0, 0.95, 0.85) * spec * currentExposure;
    
    vec3 color = diffuse + ambient + causticsColor * albedo + specColor;
    
    // Water depth fog - sand fades into water color with distance from camera
    float dist = length(cameraPos - FragPos);
    float depthFog = 1.0 - exp(-dist * 0.015);
    vec3 waterFogColor = vec3(0.01, 0.08, 0.15) * currentExposure;
    color = mix(color, waterFogColor, clamp(depthFog, 0.0, 0.95));
    
    FragColor = vec4(color, 1.0);
}
