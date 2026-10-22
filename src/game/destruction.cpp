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

}  // namespace gvr
