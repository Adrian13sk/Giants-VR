// Rework: procedural ambient loops (see ambience.h). Plain DSP: white / brown noise, one-pole and
// biquad filters, envelopes periodic in the loop's length, events (drops, bubbles) placed at random;
// every loop ends in a cross-fade into its own start so it repeats without a seam.
#include "audio/ambience.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace gvr {

namespace {
constexpr float kTau = 6.28318530718f;

struct Rng {
    uint32_t s;
    float white() {  // -1..1
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return float(s & 0xffffff) / float(0x7fffff) - 1.0f;
    }
    float uni() { return 0.5f + 0.5f * white(); }
};

struct OnePole {  // low-pass
    float a = 1, y = 0;
    OnePole(float fc, float rate) : a(1.0f - std::exp(-kTau * fc / rate)) {}
    float operator()(float x) { y += a * (x - y); return y; }
};

struct Biquad {  // band-pass (RBJ, constant 0 dB peak)
    float b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    void bandpass(float fc, float q, float rate) {
        const float w = kTau * fc / rate, al = std::sin(w) / (2.0f * q), a0 = 1.0f + al;
        b0 = al / a0; b1 = 0; b2 = -al / a0; a1 = -2.0f * std::cos(w) / a0; a2 = (1.0f - al) / a0;
    }
    float operator()(float x) {
        const float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }
};

// Loops of `seconds`, generated `fade` seconds longer and folded: seamless.
std::vector<float> fold(std::vector<float> g, size_t loop, size_t fade) {
    std::vector<float> out(g.begin(), g.begin() + long(loop));
    for (size_t i = 0; i < fade && loop + i < g.size(); ++i) {
        const float w = std::sin(0.5f * 3.14159265f * float(i) / float(fade));
        out[i] = g[i] * w + g[loop + i] * std::sqrt(std::max(0.0f, 1.0f - w * w));
    }
    return out;
}

void normalize(std::vector<float>& v, float peak) {
    float m = 1e-6f;
    for (float x : v) m = std::max(m, std::fabs(x));
    const float k = peak / m;
    for (float& x : v) x *= k;
}
}  // namespace

const char* ambienceName(AmbienceKind kind) {
    switch (kind) {
        case AmbienceKind::Surf: return "amb_surf";
        case AmbienceKind::Wind: return "amb_wind";
        case AmbienceKind::Leaves: return "amb_leaves";
        case AmbienceKind::River: return "amb_river";
        case AmbienceKind::Rumble: return "amb_rumble";
        case AmbienceKind::Rain: return "amb_rain";
        case AmbienceKind::Thunder: return "amb_thunder";
    }
    return "amb_wind";
}

WavData makeAmbience(AmbienceKind kind, uint32_t rate, uint32_t seed) {
    const float fr = float(rate);
    Rng rng{seed * 2654435761u + 12345u};
    float loopS = 12.0f;
    switch (kind) {
        case AmbienceKind::Surf: loopS = 16.0f; break;
        case AmbienceKind::Wind: loopS = 16.0f; break;
        case AmbienceKind::Leaves: loopS = 12.0f; break;
        case AmbienceKind::River: loopS = 10.0f; break;
        case AmbienceKind::Rumble: loopS = 12.0f; break;
        case AmbienceKind::Rain: loopS = 8.0f; break;
        case AmbienceKind::Thunder: loopS = 7.0f; break;
    }
    const size_t loop = size_t(loopS * fr), fade = kind == AmbienceKind::Thunder ? 0 : size_t(0.75f * fr), n = loop + fade;
    std::vector<float> g(n, 0.0f);
    float brown = 0;
    auto nextBrown = [&]() { brown = 0.996f * brown + 0.06f * rng.white(); return brown; };
    switch (kind) {
        case AmbienceKind::Surf: {
            // Two breakers per loop (8 s apart, the second a little smaller and later): a rising
            // swell, the crash, the foam washing up and drawing back; a low roar of the sea under it.
            OnePole roar(260.0f, fr), boom(110.0f, fr), foamLo(700.0f, fr), foamHi(4200.0f, fr);
            for (size_t i = 0; i < n; ++i) {
                const float t = float(i) / fr;
                const float tl = std::fmod(t, loopS);
                const int wave = tl < 8.0f ? 0 : 1;
                const float tau = tl - (wave ? 8.4f : 0.0f);  // seconds into this breaker
                const float big = wave ? 0.85f : 1.0f;
                float swell = 0, crash = 0, wash = 0;
                if (tau < 1.6f) swell = std::pow(std::max(0.0f, tau / 1.6f), 2.0f);
                else crash = std::exp(-(tau - 1.6f) / 1.9f);
                {
                    const float on = std::min(1.0f, std::max(0.0f, (tau - 1.2f) / 0.6f));
                    wash = on * on * (3.0f - 2.0f * on) * std::exp(-std::max(0.0f, tau - 2.0f) / 1.3f);
                }
                const float b = nextBrown();
                const float w = rng.white();
                const float low = roar(b) * (0.35f + 0.65f * big * std::max(swell * 0.6f, crash));
                const float thump = boom(b) * big * std::exp(-std::fabs(tau - 1.65f) / 0.25f) * 0.7f;
                const float foam = (foamHi(w) - foamLo(w)) * big * wash * 0.9f;
                g[i] = low + thump + foam + 0.04f * foamHi.y;  // (a faint hiss always: the sea further off)
            }
            break;
        }
        case AmbienceKind::Wind: {
            // A low rush rising and falling in gusts (periodic in the loop), a faint whistle on top.
            OnePole rush(330.0f, fr), rush2(900.0f, fr);
            Biquad wh;
            for (size_t i = 0; i < n; ++i) {
                const float t = float(i) / fr;
                const float p = kTau * t / loopS;
                const float gust = std::max(0.08f, 0.55f + 0.25f * std::sin(p + 0.3f) + 0.15f * std::sin(3.0f * p + 1.7f) + 0.08f * std::sin(7.0f * p + 4.1f));
                if (i % 32 == 0) wh.bandpass(620.0f + 160.0f * std::sin(2.0f * p) + 120.0f * gust, 14.0f, fr);
                const float w = rng.white();
                const float b = nextBrown();
                g[i] = rush(b) * gust + rush2(w) * 0.08f * gust * gust + wh(w) * 0.35f * gust * gust;
            }
            break;
        }
        case AmbienceKind::Leaves: {
            // Rustling: bright noise, in breaths of wind, fluttering.
            OnePole lo(1800.0f, fr), hi(7000.0f, fr), flut(22.0f, fr);
            for (size_t i = 0; i < n; ++i) {
                const float t = float(i) / fr;
                const float p = kTau * t / loopS;
                const float gust = std::max(0.08f, 0.45f + 0.3f * std::sin(p) + 0.18f * std::sin(5.0f * p + 1.0f));
                const float w = rng.white();
                const float band = hi(w) - lo(w);
                const float f = std::fabs(flut(rng.white())) * 9.0f;
                g[i] = band * gust * (0.35f + std::min(1.5f, f));
            }
            break;
        }
        case AmbienceKind::River: {
            // A bed of water noise and many small bubbles (rising chirps).
            OnePole bedLo(900.0f, fr), deep(200.0f, fr);
            for (size_t i = 0; i < n; ++i) g[i] = bedLo(rng.white()) * 0.35f + deep(nextBrown()) * 0.4f;
            const int bubbles = int(45.0f * loopS);
            for (int k = 0; k < bubbles; ++k) {
                const float at = rng.uni() * (loopS + 0.75f);
                const float dur = 0.015f + 0.04f * rng.uni();
                const float f0 = 280.0f + 700.0f * rng.uni() * rng.uni();
                const float amp = 0.08f + 0.3f * rng.uni() * rng.uni();
                const size_t s0 = size_t(at * fr), len = size_t(dur * fr);
                float ph = 0;
                for (size_t j = 0; j < len && s0 + j < n; ++j) {
                    const float u = float(j) / float(len);
                    ph += kTau * f0 * (1.0f + 0.6f * u) / fr;
                    g[s0 + j] += std::sin(ph) * std::sin(3.14159265f * u) * std::exp(-2.5f * u) * amp;
                }
            }
            break;
        }
        case AmbienceKind::Rumble: {
            // Deep earth rumble (the volcanoes): slow swells of very low noise.
            OnePole lo(65.0f, fr), lo2(140.0f, fr);
            for (size_t i = 0; i < n; ++i) {
                const float p = kTau * (float(i) / fr) / loopS;
                const float b = nextBrown();
                g[i] = (lo(b) * 1.0f + lo2(b) * 0.3f) * (0.6f + 0.3f * std::sin(p) + 0.1f * std::sin(4.0f * p + 2.0f));
            }
            break;
        }
        case AmbienceKind::Thunder: {
            // A crack, then the roll: deep noise swelling and dying away in a few bursts.
            OnePole crackLo(900.0f, fr), crackHi(5000.0f, fr), roll(140.0f, fr), roll2(60.0f, fr);
            float bursts[5][2];  // time, size
            for (int k = 0; k < 5; ++k) { bursts[k][0] = 0.25f + 3.5f * rng.uni() * rng.uni(); bursts[k][1] = 0.4f + 0.6f * rng.uni(); }
            for (size_t i = 0; i < n; ++i) {
                const float t = float(i) / fr;
                const float w = rng.white(), b = nextBrown();
                const float crack = (crackHi(w) - crackLo(w)) * std::exp(-t / 0.09f) * std::min(1.0f, t / 0.004f) * 1.4f;
                float env = std::exp(-t / 2.2f) * std::min(1.0f, t / 0.12f);
                for (int k = 0; k < 5; ++k) env += bursts[k][1] * std::exp(-std::fabs(t - bursts[k][0]) / (0.35f + 0.2f * float(k)));
                env *= std::max(0.0f, 1.0f - t / loopS);  // (to silence at the end)
                g[i] = crack + (roll(b) * 1.0f + roll2(b) * 1.4f) * env;
            }
            break;
        }
        case AmbienceKind::Rain: {
            // A steady hiss of falling rain and drops ticking on leaves and ground.
            OnePole lo(500.0f, fr), hi(6000.0f, fr), body(1500.0f, fr);
            for (size_t i = 0; i < n; ++i) {
                const float w = rng.white();
                g[i] = (hi(w) - lo(w)) * 0.45f + body(rng.white()) * 0.15f;
            }
            const int drops = int(500.0f * loopS);
            for (int k = 0; k < drops; ++k) {
                const size_t s0 = size_t(rng.uni() * (loopS + 0.75f) * fr);
                const float f = 2200.0f + 3800.0f * rng.uni();
                const float amp = 0.04f + 0.4f * std::pow(rng.uni(), 4.0f);
                const size_t len = size_t((0.001f + 0.003f * rng.uni()) * fr);
                for (size_t j = 0; j < len && s0 + j < n; ++j)
                    g[s0 + j] += std::sin(kTau * f * float(j) / fr) * std::exp(-5.0f * float(j) / float(len)) * amp;
            }
            break;
        }
    }
    // (filters start from rest: skip nothing - the fade region at the end is past their warm-up anyway)
    WavData out;
    out.rate = rate;
    out.channels = 1;
    out.mono = fade ? fold(std::move(g), loop, fade) : std::move(g);
    normalize(out.mono, kind == AmbienceKind::Rumble ? 0.9f : 0.75f);
    return out;
}

}  // namespace gvr
