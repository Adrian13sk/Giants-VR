// Destructible scenery (renderer-independent, unit-tested): trees, rocks, Smartie houses and
// the Reapers' guard towers take damage, show it in stages (smoke, then heavy smoke and fire)
// and collapse: trees topple away from the damage, rocks shatter, houses cave in, towers play
// their own collapse clip (guard_tower1.anm: upright, shaking, bending over) and are replaced by
// their ruin (Guard_tower1D). Hit points of houses and towers are the original type values (op
// 0x31: 330, 350, 500); trees and rocks have none in the data, theirs are ours, as are the
// resistances to bullets and blasts and the timings.
#pragma once

#include <vector>

#include "core/math.h"

namespace gvr {

enum class DestructKind { Tree, Rock, House, Tower };

struct Destructible {
    int object = -1;          // the world object it is (AssetWorld index)
    DestructKind kind = DestructKind::Tree;
    Vec3 base;                // ground point under it (runtime)
    float height = 5, radius = 1;  // m
    float hp = 100, maxHp = 100;
    int stage = 0;            // 0 intact, 1 damaged, 2 badly damaged, 3 collapsing, 4 down
    float t = 0;              // seconds since the collapse began
    Vec3 fallDir{1, 0, 0};    // horizontal: away from the damage
    float smoke = 0;          // effect timer (game layer)
    bool alive() const { return stage < 3; }
};

struct DestructEvent {
    enum Kind { Hit, Stage, Collapse, Fallen, Down } kind;
    int index;
    Vec3 pos;
};

class DestructionDirector {
public:
    std::vector<Destructible> items;
    // Collapse durations by kind (tree, rock, house, tower): the tower's is its clip's 3.3 s.
    float collapseTime[4] = {1.7f, 0.05f, 3.0f, 3.3f};
    float bulletFactor[4] = {0.35f, 0.15f, 0.3f, 0.3f};  // share of a bullet's damage taken
    float blastFactor[4] = {1.0f, 0.9f, 1.0f, 1.0f};

    int add(int object, DestructKind kind, const Vec3& base, float height, float radius, float hitPoints);
    // Damage from `from` (shooter or blast centre). Returns true if this started the collapse.
    bool damage(int i, float amount, bool blast, const Vec3& from, std::vector<DestructEvent>& events);
    void update(float dt, std::vector<DestructEvent>& events);
    // Toppling angle (0 .. pi/2) of a falling tree or tower after t of `duration` seconds: slow
    // at first, accelerating like a falling trunk.
    static float fallAngle(float t, float duration);
};

}  // namespace gvr
