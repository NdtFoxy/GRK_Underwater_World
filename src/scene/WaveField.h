#pragma once
#include <GL/glew.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cmath>
#include <vector>

// ----------------------------------------------------------------------
// WaveField  (Req 2.4, 6, 7)
//
// Single source of truth for the Gerstner ocean. The SAME component
// table and math are used by:
//   * the GPU water tessellation shader (via upload()) — rendered surface
//                                                         (water_tess.tese)
//   * the CPU sample()                                 — buoyancy + player
//
// Sharing one definition guarantees floating objects sit exactly on the
// rendered wave surface (Property 1) and lets us decouple wave-speed
// (animation) from amplitude (weather).
//
// Self-intersection guard (Property 2): a per-frame normalization factor
// `qNorm` scales every wave's horizontal pinch so that
//   sum_i(steepness_i * amplitude_i * ampScale * k_i) <= Q_LIMIT < 1
// for ANY weather amplitude — the Gerstner surface can never fold.
// ----------------------------------------------------------------------
struct GerstnerWave {
    glm::vec2 dir;        // horizontal direction (will be normalized)
    float     amplitude;  // base vertical amplitude (world units)
    float     wavelength; // crest-to-crest distance (world units)
    float     steepness;  // horizontal pinch Q (0..1)
};

class WaveField {
public:
    static const int COUNT = 24;         // richer spectrum; JONSWAP-sampled, wide spread
    GerstnerWave waves[COUNT];
    float speed    = 1.0f;               // Wave_Speed (animation)   Req 6
    float ampScale = 1.0f;               // weather amplitude scale  Req 7.2

    static constexpr float GRAV    = 9.8f;
    static constexpr float Q_LIMIT = 0.85f;   // rounded, no fold (0.93 spiked in storms)

    void initDefault() {
        // Build a 16-component ocean from a JONSWAP-shaped energy spectrum around
        // a constant wind direction. Long swells carry most of the energy near the
        // peak wavelength; shorter, steeper chop fans out to wider angles so the
        // surface never reads as a repeating grid. Wind direction is a constant
        // for now (later: a UI control). The per-frame qNorm guard still prevents
        // self-intersection at any amplitude (Property 2).
        const glm::vec2 windDir = glm::normalize(glm::vec2(1.0f, 0.35f));
        const float wlLong  = 72.0f;   // longest swell (m)
        const float wlShort = 2.1f;    // shortest chop  (m)
        const float peakWL  = 44.0f;   // spectral peak  (m)
        const float energy  = 2.3f;    // overall amplitude (storm preset multiplies by 1.8x,
                                       // so keep the base modest or waves spike into ice peaks)

        // Deterministic pseudo-random in [0,1) — no <random> dependency.
        auto rnd = [](int i) -> float {
            float s = std::sin(float(i) * 12.9898f + 4.1414f) * 43758.5453f;
            return s - std::floor(s);
        };

        for (int i = 0; i < COUNT; ++i) {
            float u  = float(i) / float(COUNT - 1);                 // 0..1
            // Geometric wavelength spread: long swell -> short chop.
            float wl = wlLong * std::pow(wlShort / wlLong, u);
            // JONSWAP-ish band: Pierson-Moskowitz envelope, peaked at peakWL.
            float r  = peakWL / wl;
            float pm = std::pow(r, 3.0f) * std::exp(-1.25f * r * r);
            float amp = std::max(energy * pm, 0.02f);
            // WIDE directional spread for EVERY component (incl. the big swells) so
            // waves never line up into parallel ridges — a real multi-directional
            // sea. A broad random fan around the wind + extra jitter; longer swells
            // lean a bit closer to the wind, short chop fans out fully.
            float fan = glm::radians(24.0f + 34.0f * u);            // ±24..58 deg (varied, not chaotic)
            float a = (rnd(i * 5 + 1) - 0.5f) * 2.0f * fan
                    + (rnd(i * 13 + 7) - 0.5f) * glm::radians(16.0f);
            glm::vec2 d(windDir.x * std::cos(a) - windDir.y * std::sin(a),
                        windDir.x * std::sin(a) + windDir.y * std::cos(a));
            // Steepness rises with wavenumber so short waves are sharper.
            float steep = glm::clamp(0.32f + 0.55f * u, 0.0f, 0.92f);
            waves[i] = { glm::normalize(d), amp, wl, steep };
        }
    }

    // Normalization factor that keeps the surface from self-intersecting
    // at the current amplitude scale. Identical on CPU and GPU.
    float computeQNorm() const {
        float total = 0.0f;
        for (int i = 0; i < COUNT; ++i) {
            float k = 6.28318530718f / waves[i].wavelength;
            total += waves[i].steepness * waves[i].amplitude * ampScale * k;
        }
        if (total <= Q_LIMIT || total <= 1e-6f) return 1.0f;
        return Q_LIMIT / total;
    }

    // CPU evaluation — MUST mirror water_tess.tese exactly. `atten` is the same
    // depth-based shoaling factor the vertex shader applies (1 = full open-ocean
    // swell, 0 = flattened shallow water near shore) so floating objects ride
    // the same surface the GPU renders.
    void sample(float x, float z, float t, float& outY, glm::vec3& outNormal,
                float atten = 1.0f) const {
        float qNorm = computeQNorm();
        glm::vec3 normal(0.0f, 1.0f, 0.0f);
        float sinSum = 0.0f;
        float y = 0.0f;
        for (int i = 0; i < COUNT; ++i) {
            const GerstnerWave& w = waves[i];
            float k = 6.28318530718f / w.wavelength;
            float c = std::sqrt(GRAV / k);
            float A = w.amplitude * ampScale * atten;
            float Q = w.steepness * qNorm;
            float f = k * (w.dir.x * x + w.dir.y * z) - c * t * speed;
            float cf = std::cos(f);
            float sf = std::sin(f);
            y += A * sf;
            float WA = k * A;
            normal.x -= w.dir.x * WA * cf;
            normal.z -= w.dir.y * WA * cf;
            sinSum   += Q * WA * sf;
        }
        normal.y = 1.0f - sinSum;
        outY = y;
        outNormal = glm::normalize(normal);
    }

    // Convenience: just the height (used where the normal isn't needed).
    float sampleHeight(float x, float z, float t, float atten = 1.0f) const {
        float y; glm::vec3 n; sample(x, z, t, y, n, atten); return y;
    }

    // Upload the component table + scalars to a linked program. Call
    // after glUseProgram(program).
    void upload(GLuint program) const {
        float qNorm = computeQNorm();
        // Pack each wave into a vec4 (dir.xy, amplitude, wavelength) +
        // a parallel float for steepness.
        glm::vec4 packed[COUNT];
        float steep[COUNT];
        for (int i = 0; i < COUNT; ++i) {
            packed[i] = glm::vec4(waves[i].dir.x, waves[i].dir.y,
                                  waves[i].amplitude, waves[i].wavelength);
            steep[i] = waves[i].steepness;
        }
        glUniform4fv(glGetUniformLocation(program, "uWave"), COUNT, glm::value_ptr(packed[0]));
        glUniform1fv(glGetUniformLocation(program, "uSteep"), COUNT, steep);
        glUniform1i (glGetUniformLocation(program, "uWaveCount"), COUNT);
        glUniform1f (glGetUniformLocation(program, "uWaveSpeed"), speed);
        glUniform1f (glGetUniformLocation(program, "uWaveAmpScale"), ampScale);
        glUniform1f (glGetUniformLocation(program, "uQNorm"), qNorm);
    }
};
