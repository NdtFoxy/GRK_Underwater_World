#version 330 core
// Render-to-texture caustics — Evan Wallace's algorithm.
//
// Each vertex of the input grid represents a point on the water
// surface. We compute the wave height + normal at that point, then
// refract a sun ray through the surface and project it onto a flat
// receiver plane (the seabed level). The result becomes the vertex
// position in the caustics texture (XY in [0,1] UV space, written via
// gl_Position).
//
// The fragment shader uses dFdx/dFdy on the original surface position
// to estimate how much each point's incoming light area contracted
// (or expanded) when refracted — that's the physical caustic
// intensity.

layout (location = 0) in vec2 aGrid;   // [0..1] x [0..1] grid sample

uniform float time;
uniform float windSpeed;
uniform vec3  sunDirection;     // pointing from ground to sun
uniform float terrainSize;      // world extent (unused for projection now)
uniform float causticsTileSize; // world units this tile maps onto (GL_REPEAT)
uniform float waterY;           // base water level (usually 0)
uniform float floorY;           // virtual receiver plane Y (e.g. seabed avg)

out vec3 vRefractedHit;         // where the refracted ray landed
out vec3 vOldPos;               // the un-refracted surface point
out float vDownAngle;           // |dot(refracted, -up)| for falloff

// --- Gerstner wave (must match water_tess.tese) ----------------------
vec3 GerstnerWave(vec4 wave, vec3 p, inout vec3 tangent, inout vec3 binormal) {
    float steepness = wave.z * windSpeed;
    float wavelength = wave.w;
    float k = 2.0 * 3.14159 / wavelength;
    float c = sqrt(9.8 / k) * (0.5 + windSpeed * 0.5);
    vec2 d = normalize(wave.xy);
    float f = k * (dot(d, p.xz) - c * time);
    float a = (steepness / k) * windSpeed;

    tangent  += vec3(-d.x*d.x*(steepness*sin(f)),  d.x*(steepness*cos(f)), -d.x*d.y*(steepness*sin(f)));
    binormal += vec3(-d.x*d.y*(steepness*sin(f)),  d.y*(steepness*cos(f)), -d.y*d.y*(steepness*sin(f)));
    return vec3(d.x*(a*cos(f)), a*sin(f), d.y*(a*cos(f)));
}

void main() {
    // The grid maps onto a `causticsTileSize`-unit square. The tile is
    // repeated across the seabed (GL_REPEAT) so each texel covers far
    // fewer world units → no blocky magnification up close.
    float halfSize = causticsTileSize * 0.5;
    vec3 gridPos = vec3(aGrid.x * causticsTileSize - halfSize,
                        waterY,
                        aGrid.y * causticsTileSize - halfSize);

    vec3 tangent  = vec3(1, 0, 0);
    vec3 binormal = vec3(0, 0, 1);
    vec3 p = gridPos;

    // Same 8-wave stack the water surface uses, so the caustic matches
    // the geometry the player sees from above.
    p += GerstnerWave(vec4( 1.0,  0.3, 0.35, 30.0), gridPos, tangent, binormal);
    p += GerstnerWave(vec4( 0.7,  1.0, 0.30, 20.0), gridPos, tangent, binormal);
    p += GerstnerWave(vec4(-0.5,  0.8, 0.25, 15.0), gridPos, tangent, binormal);
    p += GerstnerWave(vec4( 1.0,  0.6, 0.20, 10.0), gridPos, tangent, binormal);
    p += GerstnerWave(vec4(-0.8,  0.4, 0.18,  7.0), gridPos, tangent, binormal);
    p += GerstnerWave(vec4( 0.4, -0.9, 0.12,  4.0), gridPos, tangent, binormal);
    p += GerstnerWave(vec4(-0.3,  0.5, 0.08,  2.5), gridPos, tangent, binormal);
    p += GerstnerWave(vec4( 0.9, -0.2, 0.05,  1.5), gridPos, tangent, binormal);

    vec3 N = normalize(cross(binormal, tangent));

    // Refract incoming sun ray (going DOWN through the water surface).
    // Snell's law: n1*sin(t1) = n2*sin(t2). Air→water IOR ratio = 1/1.33.
    vec3 incoming = -normalize(sunDirection);   // ray coming down
    vec3 refracted = refract(incoming, N, 1.0 / 1.33);

    // If TIR happened (refracted == 0), fall back to straight down so
    // we don't NaN. This rarely fires for sun above the horizon.
    if (length(refracted) < 0.001) refracted = vec3(0.0, -1.0, 0.0);

    // Project the refracted ray from p down to the receiver plane.
    float t = (floorY - p.y) / refracted.y;
    bool invalid = (refracted.y > -0.001 || t < 0.0);
    if (invalid) {
        // Flag the fragment as invalid via vDownAngle; the frag shader
        // will discard. We still need a sensible gl_Position so the
        // surrounding triangles don't get stretched across the FBO.
        // Fall back to an unrefracted straight-down hit.
        refracted = vec3(0.0, -1.0, 0.0);
        t = (floorY - p.y) / refracted.y;
    }
    vec3 hit = p + refracted * t;

    // Convert hit position into [0,1] UV within the tile. The terrain
    // shader samples the same texture with worldXZ/causticsTileSize and
    // GL_REPEAT, so the projection and the lookup stay consistent.
    vec2 hitUV = vec2(hit.x + halfSize, hit.z + halfSize) / causticsTileSize;

    // Output as clip-space: map [0,1] UV → [-1,1] NDC.
    gl_Position = vec4(hitUV * 2.0 - 1.0, 0.0, 1.0);

    vRefractedHit = hit;
    vOldPos       = p;
    vDownAngle    = invalid ? 0.0 : -refracted.y;   // 1.0 straight down
}
