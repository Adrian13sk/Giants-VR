#include "game/destruction.h"

#include <algorithm>
#include <cmath>

namespace gvr {

int DestructionDirector::add(int object, DestructKind kind, const Vec3& base, float height, float radius, float hitPoints) {
    Destructible d;
    d.object = object;
    d.kind = kind;
    d.base = base;
    d.height = height;
    d.radius = radius;
    d.hp = d.maxHp = std::max(1.0f, hitPoints);
    items.push_back(d);
    return int(items.size()) - 1;
}

bool DestructionDirector::damage(int i, float amount, bool blast, const Vec3& from, std::vector<DestructEvent>& events) {
    if (i < 0 || size_t(i) >= items.size()) return false;
    Destructible& d = items[size_t(i)];
    if (!d.alive()) return false;
    const int k = int(d.kind);
    d.hp -= amount * (blast ? blastFactor[k] : bulletFactor[k]);
    events.push_back({DestructEvent::Hit, i, d.base});
    // Falls away from where the damage came from.
    Vec3 away{d.base.x - from.x, 0, d.base.z - from.z};
    if (length(away) > 1e-3f) d.fallDir = normalize(away);
    const float f = d.hp / d.maxHp;
    const int stage = d.hp <= 0 ? 3 : (f <= 0.33f ? 2 : (f <= 0.66f ? 1 : 0));
    if (stage > d.stage) {
        d.stage = stage;
        events.push_back({stage == 3 ? DestructEvent::Collapse : DestructEvent::Stage, i, d.base});
        if (stage == 3) {
            d.t = 0;
            return true;
        }
    }
    return false;
}

float DestructionDirector::fallAngle(float t, float duration) {
    const float x = std::max(0.0f, std::min(1.0f, t / std::max(1e-3f, duration)));
    return 1.5708f * x * x;  // constant angular acceleration
}

void DestructionDirector::update(float dt, std::vector<DestructEvent>& events) {
    for (size_t i = 0; i < items.size(); ++i) {
        Destructible& d = items[i];
        if (d.stage != 3) continue;
        const float before = d.t;
        d.t += dt;
        const float dur = collapseTime[int(d.kind)];
        // Trees hit the ground at the end of the fall; the others are simply down.
        if (before < dur && d.t >= dur) {
            if (d.kind == DestructKind::Tree) events.push_back({DestructEvent::Fallen, int(i), d.base + d.fallDir * d.height});
            d.stage = 4;
            events.push_back({DestructEvent::Down, int(i), d.base});
        }
    }
}

// Periodic value noise (period `cells` across the tile), smoothly interpolated.
static float tileNoise(float x, float y, int cells, uint32_t seed) {
    auto h = [&](int i, int j) {
        i = ((i % cells) + cells) % cells;
        j = ((j % cells) + cells) % cells;
        uint32_t v = uint32_t(i) * 374761393u + uint32_t(j) * 668265263u + seed * 2246822519u;
        v = (v ^ (v >> 13)) * 1274126177u;
        return float((v ^ (v >> 16)) & 0xffffu) / 65535.0f;
    };
    const float fx = x * float(cells), fy = y * float(cells);
    const int ix = int(std::floor(fx)), iy = int(std::floor(fy));
    float tx = fx - float(ix), ty = fy - float(iy);
    tx = tx * tx * (3 - 2 * tx);
    ty = ty * ty * (3 - 2 * ty);
    const float a = h(ix, iy), b = h(ix + 1, iy), c = h(ix, iy + 1), d = h(ix + 1, iy + 1);
    return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
}

void makeBurnTextures(int size, uint32_t seed, std::vector<uint8_t> charTex[3], std::vector<uint8_t>& embers) {
    const float thresholds[3] = {0.66f, 0.52f, 0.36f};  // noise above this is charred: coverage grows
    for (int k = 0; k < 3; ++k) charTex[k].assign(size_t(size) * size_t(size) * 4, 0);
    embers.assign(size_t(size) * size_t(size) * 4, 0);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const float u = float(x) / float(size), v = float(y) / float(size);
            const float n = 0.55f * tileNoise(u, v, 4, seed) + 0.3f * tileNoise(u, v, 8, seed + 1) + 0.15f * tileNoise(u, v, 32, seed + 2);
            const float grain = tileNoise(u, v, 64, seed + 3);
            const size_t o = (size_t(y) * size_t(size) + size_t(x)) * 4;
            for (int k = 0; k < 3; ++k) {
                const float a = std::max(0.0f, std::min(1.0f, (n - thresholds[k]) / 0.06f + 0.5f));
                const float dark = 0.55f + 0.45f * grain;  // soot grain
                charTex[k][o + 0] = uint8_t(24.0f * dark);
                charTex[k][o + 1] = uint8_t(17.0f * dark);
                charTex[k][o + 2] = uint8_t(12.0f * dark);
                charTex[k][o + 3] = uint8_t(std::min(1.0f, a * (0.85f + 0.15f * grain)) * 255.0f);
            }
            // Embers: a thin hot band along the heavy char's edge, and specks inside it.
            const float band = std::max(0.0f, 1.0f - std::fabs(n - thresholds[1]) / 0.022f);
            const float speck = n > thresholds[1] && grain > 0.82f ? (grain - 0.82f) / 0.18f : 0.0f;
            const float e = std::min(1.0f, band + speck);
            embers[o + 0] = uint8_t(255.0f * e);
            embers[o + 1] = uint8_t(105.0f * e * e);
            embers[o + 2] = uint8_t(20.0f * e * e * e);
            embers[o + 3] = 255;
        }
}

float burnLevel(const Destructible& d) {
    if (!d.alive()) return 1.0f;
    return d.maxHp > 0 ? std::max(0.0f, std::min(1.0f, 1.0f - d.hp / d.maxHp)) : 0.0f;
}

}  // namespace gvr
