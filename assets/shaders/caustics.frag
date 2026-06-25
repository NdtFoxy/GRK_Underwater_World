#version 330 core
// Caustics fragment shader.
//
// Each fragment writes a brightness value computed from how much the
// incoming surface area was *compressed* during refraction. We get
// that compression for free using screen-space derivatives on the
// pre-refraction position passed from the vertex shader.
//
// Output is a single-channel additive accumulation (rendered with
// glBlendFunc(GL_ONE, GL_ONE) into a R16F texture). Where many rays
// converge, brightness piles up — the bright caustic filament you see
// on a pool floor.

in vec3 vRefractedHit;
in vec3 vOldPos;
in float vDownAngle;

out vec4 FragColor;

void main() {
    // Reject anything that escaped above the surface in the vert shader.
    if (vDownAngle <= 0.001) discard;

    // dFdx/dFdy give us the partial derivatives of the *original*
    // (pre-refraction) surface point with respect to screen coords.
    // The cross product magnitude is the area of the source patch
    // that contributes to one screen-space pixel after refraction.
    vec3 oldPosDx = dFdx(vOldPos);
    vec3 oldPosDy = dFdy(vOldPos);
    vec3 newPosDx = dFdx(vRefractedHit);
    vec3 newPosDy = dFdy(vRefractedHit);

    float oldArea = length(cross(oldPosDx, oldPosDy));
    float newArea = length(cross(newPosDx, newPosDy));

    // Intensity = source area / projected area (light per unit floor).
    // Clamp to avoid numerical singularities at caustic focal points
    // (where newArea → 0 and intensity → ∞).
    float intensity = oldArea / max(newArea, 0.0001);
    intensity = clamp(intensity, 0.0, 4.0);

    // Cosine factor — slanted refraction spreads light over more area.
    intensity *= vDownAngle;

    FragColor = vec4(intensity, 0.0, 0.0, 0.0);
}
