#include "game/skirmish.h"

#include "core/log.h"
#include "core/prof.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace gvr {

namespace {
constexpr float kKabutoFumble = 60.0f;  // damage that makes Kabuto drop a flag he carries (ours, 2026-10-28)
}


namespace {
const char* kTeamNames[3] = {"Blue", "Green", "Red"};  // GTextEnglish commteamno1..3
float yawTo(const Vec3& d) { return std::atan2(-d.x, -d.z); }  // 0 = facing -Z
Vec3 flat(const Vec3& v) { return Vec3{v.x, 0, v.z}; }
// The bots' weapons (ours): the Meccaryn's gun bolts, the Sea Reaper's arrows; Kabuto stomps.
struct BotGun { float speed, damage, interval, gravity, spread; };
const BotGun kGuns[2] = {{70.0f, 9.0f, 0.45f, 0.0f, 0.035f}, {45.0f, 16.0f, 1.1f, 3.0f, 0.03f}};
}  // namespace

// ---------------------------------------------------------------------------------------------
// SkNav

void SkNav::build(float x0, float z0, float x1, float z1, float cell, const std::function<float(float, float)>& ground, float seaLevel) {
    cell_ = std::max(2.0f, cell);
    x0_ = x0;
    z0_ = z0;
    sea_ = seaLevel;
    w_ = std::max(2, int((x1 - x0) / cell_) + 1);
    h_ = std::max(2, int((z1 - z0) / cell_) + 1);
    hgt_.assign(size_t(w_) * size_t(h_), seaLevel - 30.0f);
    for (int z = 0; z < h_; ++z)
        for (int x = 0; x < w_; ++x) hgt_[size_t(z) * size_t(w_) + size_t(x)] = ground(x0_ + float(x) * cell_, z0_ + float(z) * cell_);
    // The steepest rise along each step out of a cell (a bank between two samples blocks a walker). On
    // the water's surface, not its bottom: a swimmer climbs out onto a beach, not up from the sea floor.
    static const int dx8[8] = {1, -1, 0, 0, 1, 1, -1, -1}, dz8[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    const float surf = seaLevel - 1.0f;
    auto top = [&](float x, float z) { return std::max(ground(x, z), surf); };
    up_.assign(size_t(w_) * size_t(h_) * 8, 0.0f);
    for (int z = 0; z < h_; ++z)
        for (int x = 0; x < w_; ++x)
            for (int k = 0; k < 8; ++k) {
                const float L = cell_ * (k < 4 ? 1.0f : 1.4142f);
                const int n = std::max(3, int(L * 2.0f));  // every half metre
                const float sx = x0_ + float(x) * cell_, sz = z0_ + float(z) * cell_;
                float prev = std::max(hgt_[size_t(z) * size_t(w_) + size_t(x)], surf), worst = 0;
                for (int i = 1; i <= n; ++i) {
                    const float f = float(i) / float(n);
                    const float hh = top(sx + float(dx8[k]) * cell_ * f, sz + float(dz8[k]) * cell_ * f);
                    worst = std::max(worst, (hh - prev) / (L / float(n)));
                    prev = hh;
                }
                up_[(size_t(z) * size_t(w_) + size_t(x)) * 8 + size_t(k)] = worst;
            }
}

void SkNav::block(float bx0, float bz0, float bx1, float bz1) {
    if (w_ <= 0) return;
    if (blocked_.size() != size_t(w_) * size_t(h_)) blocked_.assign(size_t(w_) * size_t(h_), 0);
    const int cx0 = std::max(0, int(std::floor((bx0 - x0_) / cell_))), cx1 = std::min(w_ - 1, int(std::ceil((bx1 - x0_) / cell_)));
    const int cz0 = std::max(0, int(std::floor((bz0 - z0_) / cell_))), cz1 = std::min(h_ - 1, int(std::ceil((bz1 - z0_) / cell_)));
    for (int z = cz0; z <= cz1; ++z)
        for (int x = cx0; x <= cx1; ++x) blocked_[size_t(z) * size_t(w_) + size_t(x)] = 1;
}

void SkNav::blockBox(const Mat4& xf, const Vec3& mn, const Vec3& mx, float margin) {
    if (w_ <= 0) return;
    if (blocked_.size() != size_t(w_) * size_t(h_)) blocked_.assign(size_t(w_) * size_t(h_), 0);
    // The box's horizontal axes in the world (the building stands upright: yaw only).
    const Vec4 c4 = xf * Vec4{0.5f * (mn.x + mx.x), 0.5f * (mn.y + mx.y), 0.5f * (mn.z + mx.z), 1.0f};
    const Vec3 c{c4.x, c4.y, c4.z};
    Vec3 ax{xf.m[0], 0, xf.m[2]}, az{xf.m[8], 0, xf.m[10]};
    const float sx = length(ax), sz = length(az);
    if (sx < 1e-6f || sz < 1e-6f) return;
    ax = ax * (1.0f / sx);
    az = az * (1.0f / sz);
    const float hx = 0.5f * (mx.x - mn.x) * sx + margin, hz = 0.5f * (mx.z - mn.z) * sz + margin;
    const float r = std::sqrt(hx * hx + hz * hz);
    const int cx0 = std::max(0, int(std::floor((c.x - r - x0_) / cell_))), cx1 = std::min(w_ - 1, int(std::ceil((c.x + r - x0_) / cell_)));
    const int cz0 = std::max(0, int(std::floor((c.z - r - z0_) / cell_))), cz1 = std::min(h_ - 1, int(std::ceil((c.z + r - z0_) / cell_)));
    for (int z = cz0; z <= cz1; ++z)
        for (int x = cx0; x <= cx1; ++x) {
            const Vec3 d{x0_ + float(x) * cell_ - c.x, 0, z0_ + float(z) * cell_ - c.z};
            if (std::fabs(dot(d, ax)) <= hx && std::fabs(dot(d, az)) <= hz) blocked_[size_t(z) * size_t(w_) + size_t(x)] = 1;
        }
}

void SkNav::penalize(const Vec3& p, Profile prof) {
    int cx, cz;
    if (!cellOf(p, cx, cz)) return;
    if (pen_.size() != size_t(w_) * size_t(h_) * 4) pen_.assign(size_t(w_) * size_t(h_) * 4, 0);
    uint8_t& v = pen_[(size_t(cz) * size_t(w_) + size_t(cx)) * 4 + size_t(prof)];
    if (v < 255) ++v;
}

bool SkNav::blocked(const Vec3& p) const {
    int cx, cz;
    if (blocked_.empty() || !cellOf(p, cx, cz)) return false;
    return blocked_[size_t(cz) * size_t(w_) + size_t(cx)] != 0;
}

bool SkNav::cellOf(const Vec3& p, int& cx, int& cz) const {
    cx = int(std::lround((p.x - x0_) / cell_));
    cz = int(std::lround((p.z - z0_) / cell_));
    cx = std::max(0, std::min(w_ - 1, cx));
    cz = std::max(0, std::min(h_ - 1, cz));
    return w_ > 0;
}

// Cost of a step between neighbouring cells (< 0: impassable for that profile).
float SkNav::stepCost(int a, int b, int dir, float dist, Profile prof) const {
    const float ha = hgt_[size_t(a)], hb = hgt_[size_t(b)];
    const float dh = std::max(hb, sea_ - 1.0f) - std::max(ha, sea_ - 1.0f), s = dh / dist;  // (surface to surface)
    const float up = up_[size_t(a) * 8 + size_t(dir)];  // the steepest metre of it
    const float depth = sea_ - hb;
    float k = 1.0f + 0.5f * std::max(0.0f, s);
    if (!pen_.empty()) {
        k += 4.0f * float(std::min<uint8_t>(pen_[size_t(b) * 4 + size_t(prof)], 3));  // (3 and more: no way, unless the goal - path())
    }
    if (depth > 0.8f) {  // water: swimming (the Sea Reaper is at home in it; a Meccaryn swims slowly)
        k *= prof == Swimmer ? 1.0f : (prof == Giant ? (depth > 12.0f ? 3.0f : 1.3f) : (depth > 3.0f ? 5.0f : 2.5f));
        return dist * k;
    }
    switch (prof) {
        case Walker: case Swimmer:
            if (up > 0.85f || dh < -12.0f) return -1;  // (the walk blocks above 1.19; a margin for the line walked)
            break;
        case Jetter:
            if (up > 0.85f) { if (dh > 30.0f) return -1; k += 2.0f + 0.1f * dh; }  // jets up it
            break;
        case Giant:
            // His walk blocks on the same slope as anyone's (it's checked over each small step).
            if (up > 1.0f || dh < -40.0f) return -1;
            break;
    }
    return dist * k;
}

bool SkNav::path(const Vec3& from, const Vec3& to, Profile prof, std::vector<Vec3>& out, int maxExpand) const {
    out.clear();
    if (w_ <= 0) return false;
    int sx, sz, gx, gz;
    cellOf(from, sx, sz);
    cellOf(to, gx, gz);
    const int start = sz * w_ + sx, goal = gz * w_ + gx;
    if (start == goal) { out.push_back(to); return true; }
    const size_t n = size_t(w_) * size_t(h_);
    if (g_.size() != n) { g_.assign(n, 0); came_.assign(n, -1); seen_.assign(n, 0); closed_.assign(n, 0); stamp_ = 0; }
    ++stamp_;
    auto gOf = [&](int i) { return seen_[size_t(i)] == stamp_ ? g_[size_t(i)] : 1e30f; };
    auto isClosed = [&](int i) { return closed_[size_t(i)] == stamp_; };
    struct Node { float f; int i; };
    auto worse = [](const Node& a, const Node& b) { return a.f > b.f; };
    std::vector<Node> open;
    auto h = [&](int i) {
        const float dx = float(i % w_ - gx), dz = float(i / w_ - gz);
        return std::sqrt(dx * dx + dz * dz) * cell_ * 1.3f;  // weighted (greedier, far fewer cells searched)
    };
    g_[size_t(start)] = 0;
    seen_[size_t(start)] = stamp_;
    came_[size_t(start)] = -1;
    open.reserve(4096);
    open.push_back({h(start), start});
    int expanded = 0;
    static const int dx8[8] = {1, -1, 0, 0, 1, 1, -1, -1}, dz8[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    bool found = false;
    int nearest = start;
    float nearestH = h(start);
    while (!open.empty() && expanded < maxExpand) {
        std::pop_heap(open.begin(), open.end(), worse);
        const Node cur = open.back();
        open.pop_back();
        if (isClosed(cur.i)) continue;
        closed_[size_t(cur.i)] = stamp_;
        ++expanded;
        if (cur.i == goal) { found = true; break; }
        if (const float hc = h(cur.i); hc < nearestH) { nearestH = hc; nearest = cur.i; }
        const int cx = cur.i % w_, cz = cur.i / w_;
        for (int k = 0; k < 8; ++k) {
            const int nx = cx + dx8[k], nz = cz + dz8[k];
            if (nx < 0 || nz < 0 || nx >= w_ || nz >= h_) continue;
            const int ni = nz * w_ + nx;
            if (isClosed(ni)) continue;
            if (!blocked_.empty()) {
                // A building (its cell may still be the goal: a flag among the base's buildings); and no
                // cutting diagonally past a building's corner.
                if (blocked_[size_t(ni)] && ni != goal) continue;
                if (k >= 4 && (blocked_[size_t(cz * w_ + nx)] || blocked_[size_t(nz * w_ + cx)])) continue;
            }
            if (!pen_.empty() && pen_[size_t(ni) * 4 + size_t(prof)] >= 3 && ni != goal) continue;
            const float c = stepCost(cur.i, ni, k, cell_ * (k < 4 ? 1.0f : 1.4142f), prof);
            if (c < 0) continue;
            const float ng = gOf(cur.i) + c;
            if (ng < gOf(ni)) {
                g_[size_t(ni)] = ng;
                seen_[size_t(ni)] = stamp_;
                came_[size_t(ni)] = cur.i;
                open.push_back({ng + h(ni), ni});
                std::push_heap(open.begin(), open.end(), worse);
            }
        }
    }
    // Not reached within the budget: the way to the cell nearest the goal (progress; the next search
    // goes on from there). Nothing reachable at all: no route.
    const int end = found ? goal : nearest;
    if (end == start) return false;
    std::vector<int> cells;
    for (int i = end; i != -1; i = came_[size_t(i)]) cells.push_back(i);
    std::reverse(cells.begin(), cells.end());
    // Every cell where the route turns (a straight line between thinned points can cut across the
    // very bank the route went round).
    for (size_t k = 1; k + 1 < cells.size(); ++k) {
        const int a = cells[k - 1], i = cells[k], b = cells[k + 1];
        const bool straight = (i - a) == (b - i);
        if (straight && k % 4 != 0) continue;
        out.push_back(Vec3{x0_ + float(i % w_) * cell_, hgt_[size_t(i)], z0_ + float(i / w_) * cell_});
    }
    if (found) out.push_back(to);
    else out.push_back(Vec3{x0_ + float(end % w_) * cell_, hgt_[size_t(end)], z0_ + float(end / w_) * cell_});
    return true;
}

// ---------------------------------------------------------------------------------------------
// Skirmish

float Skirmish::rnd() {
    rng_ = rng_ * 1664525u + 1013904223u;
    return float((rng_ >> 8) & 0xFFFFFF) / float(0x1000000);
}

void Skirmish::setup(const std::vector<SkHero>& heroes, const std::vector<Vec3>& bases, const std::vector<Vec3>& markers) {
    teams.clear();
    flags.clear();
    players.clear();
    shots.clear();
    events.clear();
    winner = -1;
    time = 0;
    for (size_t t = 0; t < heroes.size() && t < bases.size(); ++t) {
        SkTeam tm;
        tm.hero = heroes[t];
        tm.name = kTeamNames[t % 3];
        tm.base = bases[t];
        tm.base.y = ground(tm.base);
        teams.push_back(tm);
        SkFlag f;
        f.used = tm.hero != SkHero::Kabuto;
        f.home = f.pos = tm.base;
        flags.push_back(f);
    }
    // Markers round each base are its spawn points (the map's markers also mark the ground between).
    for (const Vec3& m : markers) {
        int best = -1;
        float bd = 160.0f;
        for (size_t t = 0; t < teams.size(); ++t) {
            const float d = length(flat(m - teams[t].base));
            if (d < bd) { bd = d; best = int(t); }
        }
        if (best >= 0) teams[size_t(best)].spawns.push_back(Vec3{m.x, ground(m), m.z});
    }
    // The bots' walking map over the bases and markers, with room round them.
    if (groundAt && !teams.empty()) {
        float x0 = 1e9f, z0 = 1e9f, x1 = -1e9f, z1 = -1e9f;
        auto grow = [&](const Vec3& p) { x0 = std::min(x0, p.x); z0 = std::min(z0, p.z); x1 = std::max(x1, p.x); z1 = std::max(z1, p.z); };
        for (const auto& t : teams) grow(t.base);
        for (const auto& m : markers) grow(m);
        x0 -= 250.0f; z0 -= 250.0f; x1 += 250.0f; z1 += 250.0f;
        const float cell = std::max(3.0f, std::max(x1 - x0, z1 - z0) / 360.0f);
        nav.build(x0, z0, x1, z1, cell, groundAt, seaLevel);
    }
    for (auto& t : teams)
        if (t.spawns.size() < 2) {
            const float r = t.hero == SkHero::Kabuto ? 35.0f : 12.0f;
            for (int k = 0; k < 6; ++k) {
                const float a = float(k) * 1.0472f;
                Vec3 p = t.base + Vec3{std::cos(a) * r, 0, std::sin(a) * r};
                p.y = ground(p);
                t.spawns.push_back(p);
            }
        }
}

SkNav::Profile Skirmish::profileOf(int team) const {
    const SkHero h = size_t(team) < teams.size() ? teams[size_t(team)].hero : SkHero::Mecc;
    return h == SkHero::Mecc ? SkNav::Jetter : (h == SkHero::Reaper ? SkNav::Swimmer : SkNav::Giant);
}

void Skirmish::prepareRoutes() {
    reach_.assign(teams.size() * flags.size(), 1);
    if (!nav.ready()) return;
    std::vector<Vec3> way;
    const int all = 1 << 30;  // unbounded: once per pair, at the start
    for (size_t t = 0; t < teams.size(); ++t)
        for (size_t f = 0; f < flags.size(); ++f) {
            if (t == f || !flags[f].used) continue;
            const SkNav::Profile prof = profileOf(int(t));
            const bool there = nav.path(teams[t].base, flags[f].home, prof, way, all) && !way.empty() &&
                               length(flat(way.back() - flags[f].home)) < 1.0f;
            const bool back = there && nav.path(flags[f].home, teams[t].base, prof, way, all) && !way.empty() &&
                              length(flat(way.back() - teams[t].base)) < 1.0f;
            reach_[t * flags.size() + f] = there && back;
            if (!(there && back))
                LOGI("Skirmish: no way for %s to the %s flag%s - its bots leave it alone", teams[t].name.c_str(), teams[f].name.c_str(), there ? " and back" : "");
        }
}

int Skirmish::addPlayer(const std::string& name, int team, bool bot, bool local, const PlayerController& proto, float maxHp) {
    SkPlayer p;
    p.id = int(players.size());
    for (const auto& o : players)
        if (o.gone) { p.id = o.id; break; }  // a slot someone left
    p.name = name;
    p.team = team;
    p.bot = bot;
    p.local = local;
    p.body = proto;
    if (groundAt) p.body.groundHeight = groundAt;
    p.body.collision = collision;
    p.maxHp = p.hp = maxHp;
    int mates = 0;
    for (const auto& o : players) mates += o.team == team && o.bot && !o.gone;
    p.role = (mates % 2 == 0) ? SkPlayer::Attack : SkPlayer::Defend;
    p.think = 0.1f * float(p.id);
    if (size_t(p.id) < players.size()) players[size_t(p.id)] = p;
    else players.push_back(p);
    spawn(p.id);
    return p.id;
}

SkPlayer* Skirmish::player(int id) { return id >= 0 && size_t(id) < players.size() ? &players[size_t(id)] : nullptr; }
const SkPlayer* Skirmish::player(int id) const { return id >= 0 && size_t(id) < players.size() ? &players[size_t(id)] : nullptr; }

Vec3 Skirmish::spawnPoint(int id) const {
    const SkPlayer* p = player(id);
    if (!p || size_t(p->team) >= teams.size()) return {};
    const auto& sp = teams[size_t(p->team)].spawns;
    if (sp.empty()) return teams[size_t(p->team)].base;
    // The spawn point farthest from any living enemy (no spawning into a fight).
    size_t best = size_t(id) % sp.size();
    float bestD = -1;
    for (size_t k = 0; k < sp.size(); ++k) {
        float near = 1e9f;
        for (const auto& o : players)
            if (o.alive && enemies(o.id, id)) near = std::min(near, length(o.body.position - sp[k]));
        near += 3.0f * float((k + size_t(id)) % 3);  // spread mates over the points
        if (near > bestD) { bestD = near; best = k; }
    }
    return sp[best];
}

void Skirmish::spawn(int id) {
    SkPlayer* p = player(id);
    if (!p || p->gone) return;
    const Vec3 at = spawnPoint(id);
    // Facing the middle of the map.
    Vec3 mid;
    for (const auto& t : teams) mid = mid + t.base * (1.0f / float(std::max<size_t>(1, teams.size())));
    p->body.teleport(at, yawTo(mid - at));
    p->hp = p->maxHp;
    p->alive = true;
    p->deadT = 0;
    p->carrying = -1;
    p->target = -1;
    p->lastAttacker = -1;
    p->lastHurtAt = -1e9f;
    p->moveTo = at;
    p->lastPos = at;
    p->stuckT = p->detourT = 0;
    SkEvent e;
    e.kind = SkEvent::Respawn;
    e.a = id;
    e.at = at;
    events.push_back(e);
}

bool Skirmish::enemies(int a, int b) const {
    const SkPlayer* pa = player(a);
    const SkPlayer* pb = player(b);
    return pa && pb && pa->team != pb->team;
}

int Skirmish::nearestEnemy(int team, const Vec3& p, float range, bool needSight) const {
    int best = -1;
    float bd = range;
    for (const auto& o : players) {
        if (!o.alive || o.team == team) continue;
        const Vec3 c = o.body.position + Vec3{0, 0.6f * o.body.height, 0};
        const float d = length(c - p);
        if (d >= bd) continue;
        if (needSight && !clear(p, c)) continue;
        bd = d;
        best = o.id;
    }
    return best;
}

float Skirmish::touchRadius(const SkPlayer& p) const { return std::max(2.5f, 0.12f * p.body.height); }

void Skirmish::setLocal(int id, const Vec3& pos, float yaw, float hp, bool alive) {
    SkPlayer* p = player(id);
    if (!p || p->gone) return;  // (a spectator, GVR_SK_SPECTATE)
    p->body.position = pos;
    p->body.yaw = yaw;
    p->hp = hp;
    if (alive && !p->alive) {  // the game respawned it
        p->alive = true;
        p->deadT = 0;
    }
}

void Skirmish::localDied(int id) {
    SkPlayer* p = player(id);
    if (p && p->alive) kill(*p, time - p->lastHurtAt < 10.0f ? p->lastAttacker : -1);
}

void Skirmish::remoteDied(int id, int killer) {
    SkPlayer* p = player(id);
    if (!p || !p->alive) return;
    p->lastAttacker = killer;
    kill(*p, killer);
}

void Skirmish::removePlayer(int id) {
    SkPlayer* p = player(id);
    if (!p) return;
    dropFlag(*p);
    p->alive = false;
    p->remote = false;
    p->bot = false;
    p->local = false;
    p->gone = true;    // (never respawned; nobody's enemy; not drawn)
    p->deadT = 0;
}

SkPlayer& Skirmish::mirrorPlayer(int id) {
    while (int(players.size()) <= id) {
        SkPlayer p;
        p.id = int(players.size());
        p.alive = false;
        p.bot = false;
        players.push_back(p);
    }
    return players[size_t(id)];
}

void Skirmish::damage(int victim, float amount, int attacker, const Vec3& from) {
    SkPlayer* v = player(victim);
    if (!v || !v->alive || winner >= 0) return;
    const SkPlayer* a = player(attacker);
    if (a && a->id != v->id && a->team == v->team && !rules.friendlyFire) return;
    v->lastAttacker = attacker;
    v->lastHurtAt = time;
    if (v->carrying >= 0 && teams[size_t(v->team)].hero == SkHero::Kabuto) {
        v->carryHurt += amount;
        if (v->carryHurt >= kKabutoFumble) {
            dropFlag(*v);
            v->noGrabT = 4.0f;
        }
    }
    if (v->local || v->remote) {  // its own game applies it (a remote player's: sent over the network)
        SkEvent e;
        e.kind = SkEvent::HurtLocal;
        e.a = attacker;
        e.b = victim;
        e.amount = amount;
        e.at = from;
        events.push_back(e);
        return;
    }
    v->hp -= amount;
    // Hurt, a bot turns on whoever did it.
    if (v->bot && a && a->team != v->team) v->target = a->id;
    if (v->hp <= 0) kill(*v, attacker);
}

void Skirmish::kill(SkPlayer& v, int killer) {
    if (!v.alive) return;
    v.alive = false;
    v.hp = 0;
    v.deadT = 0;
    ++v.deaths;
    if (size_t(v.team) < teams.size()) ++teams[size_t(v.team)].deaths;
    SkPlayer* k = player(killer);
    if (k && k->team != v.team) {
        ++k->kills;
        ++teams[size_t(k->team)].kills;
    }
    dropFlag(v);
    SkEvent e;
    e.kind = SkEvent::Kill;
    e.a = k ? killer : -1;
    e.b = v.id;
    e.at = v.body.position;
    events.push_back(e);
}

void Skirmish::dropFlag(SkPlayer& p) {
    if (p.carrying < 0 || size_t(p.carrying) >= flags.size()) { p.carrying = -1; return; }
    SkFlag& f = flags[size_t(p.carrying)];
    f.carrier = -1;
    f.atHome = false;
    f.pos = Vec3{p.body.position.x, ground(p.body.position), p.body.position.z};
    f.droppedT = 0;
    SkEvent e;
    e.kind = SkEvent::FlagDropped;
    e.a = p.id;
    e.team = p.carrying;
    e.at = f.pos;
    events.push_back(e);
    p.carrying = -1;
}

void Skirmish::think(SkPlayer& p) {
    const Vec3 eye = p.body.position + Vec3{0, 0.85f * p.body.height, 0};
    const float sightRange = p.body.height > 5.0f ? 120.0f : 70.0f;
    const int seen = nearestEnemy(p.team, eye, sightRange, true);
    if (seen >= 0) p.target = seen;
    else if (const SkPlayer* t = player(p.target); !t || !t->alive || length(t->body.position - p.body.position) > sightRange) p.target = -1;
    const SkFlag& own = flags[size_t(p.team)];
    // Our flag taken or lying out: the defenders go for it, and attackers only when it's near them (a
    // whole team running home left nobody to attack: no captures at all in 10-minute bot matches).
    const float kNearFlag = 150.0f;
    const SkPlayer* thief = own.used && own.carrier >= 0 ? player(own.carrier) : nullptr;
    const bool chase = thief && (p.role == SkPlayer::Defend || length(flat(thief->body.position - p.body.position)) < kNearFlag);
    const bool fetch = own.used && !own.atHome && own.carrier < 0 &&
                       (p.role == SkPlayer::Defend || length(flat(own.pos - p.body.position)) < kNearFlag);
    // A team-mate carrying a flag home: the attackers go with it.
    const SkPlayer* mate = nullptr;
    for (const auto& o : players)
        if (o.alive && o.team == p.team && o.carrying >= 0 && o.id != p.id) mate = &o;
    if (p.carrying >= 0) {
        p.moveTo = teams[size_t(p.team)].base;                 // home with it
    } else if (chase) {
        p.moveTo = thief->body.position;                       // after the thief
    } else if (fetch) {
        p.moveTo = own.pos;                                    // bring it back
    } else if (p.role == SkPlayer::Attack && mate) {
        p.moveTo = mate->body.position;                        // escort
    } else if (p.role == SkPlayer::Attack) {
        // The nearest enemy flag that isn't already carried (Kabuto's team has none).
        float bd = 1e9f;
        bool any = false;
        for (size_t t = 0; t < flags.size() && p.giveUpT <= 0; ++t) {
            if (int(t) == p.team || !flags[t].used || flags[t].carrier >= 0 || !canReach(p.team, int(t))) continue;
            const float d = length(flags[t].pos - p.body.position);
            if (d < bd) { bd = d; p.moveTo = flags[t].pos; any = true; }
        }
        if (!any) {  // nothing to take: hunt
            if (const SkPlayer* t = player(p.target)) p.moveTo = t->body.position;
            else {
                const int far = nearestEnemy(p.team, eye, 1e9f, false);
                if (const SkPlayer* f = player(far)) p.moveTo = f->body.position;
            }
        }
    } else {
        // Defend: round the base; at whoever comes close.
        const Vec3 base = teams[size_t(p.team)].base;
        const SkPlayer* t = player(p.target);
        if (t && length(flat(t->body.position - base)) < 45.0f) p.moveTo = t->body.position;
        else if (length(flat(p.moveTo - p.body.position)) < 3.0f || length(flat(p.moveTo - base)) > 25.0f) {
            const float a = rnd() * 6.2832f, r = 6.0f + 14.0f * rnd();
            p.moveTo = base + Vec3{std::cos(a) * r, 0, std::sin(a) * r};
        }
    }
    if (rnd() < 0.3f) p.strafe = (rnd() - 0.5f) * 2.0f;
    // The route there (re-planned when the goal moves, every few seconds, or after getting stuck).
    p.routeAge += 0.4f;
    if (nav.ready() && routesThisUpdate == 0 && (p.route.empty() || p.routeAge > 6.0f || length(flat(p.moveTo - p.routeGoal)) > 10.0f)) {
        ++routesThisUpdate;
        const SkNav::Profile prof = profileOf(p.team);
        GVR_PROF("skirmish route");
        if (!nav.path(p.body.position, p.moveTo, prof, p.route)) {
            p.route.clear();
            if (p.carrying < 0) p.giveUpT = 10.0f;  // nowhere to go from here: hunt for a while
        }
        p.routeAt = 0;
        p.routeGoal = p.moveTo;
        p.routeAge = 0;
    }
}

void Skirmish::fire(SkPlayer& p, const Vec3& at) {
    const SkHero h = teams[size_t(p.team)].hero;
    if (h == SkHero::Kabuto) return;
    const BotGun& g = kGuns[int(h)];
    const Vec3 muzzle = p.body.position + Vec3{0, 0.75f * p.body.height, 0} +
                        Quat::fromAxisAngle({0, 1, 0}, p.body.yaw).rotate(Vec3{0.25f, 0, -0.4f});
    Vec3 d = at - muzzle;
    const float dist = length(d);
    d = d * (1.0f / std::max(1e-3f, dist));
    // An arrow arcs: aim up by the drop over its flight.
    if (g.gravity > 0) {
        const float tof = dist / g.speed;
        d = normalize(d + Vec3{0, 0.5f * g.gravity * tof * tof / std::max(1.0f, dist), 0});
    }
    const float err = g.spread * (1.0f + dist / 30.0f);
    d = normalize(d + Vec3{(rnd() - 0.5f) * err, (rnd() - 0.5f) * err * 0.6f, (rnd() - 0.5f) * err});
    SkShot s;
    s.shooter = p.id;
    s.team = p.team;
    s.pos = muzzle;
    s.vel = d * g.speed;
    s.damage = g.damage;
    s.gravity = g.gravity;
    s.hero = h;
    shots.push_back(s);
    p.fireCool = g.interval * (0.8f + 0.5f * rnd());
    p.firing = true;
    SkEvent e;
    e.kind = SkEvent::Fire;
    e.a = p.id;
    e.at = muzzle;
    events.push_back(e);
}

void Skirmish::moveBot(SkPlayer& p, float dt) {
    PlayerInput in;
    const bool giant = teams[size_t(p.team)].hero == SkHero::Kabuto;
    Vec3 goal = p.moveTo;
    if (p.detourT > 0) {
        p.detourT -= dt;
    } else if (p.routeAt < p.route.size()) {
        const float reach = std::max(3.0f, std::max(0.7f * nav.cell(), giant ? 0.12f * p.body.height : 0.0f));
        while (p.routeAt + 1 < p.route.size() && length(flat(p.route[p.routeAt] - p.body.position)) < reach) ++p.routeAt;
        goal = p.route[p.routeAt];
        // The end of a partial route (the search ran out short of the goal): plan on from here.
        // Close by, the last stretch is walked straight (re-planning would end at the same cell).
        if (p.routeAt + 1 == p.route.size() && length(flat(goal - p.body.position)) < reach && length(flat(goal - p.moveTo)) > 5.0f) {
            if (length(flat(p.moveTo - p.body.position)) < 40.0f) {
                p.route.assign(1, p.moveTo);
                p.routeAt = 0;
                p.routeGoal = p.moveTo;
                goal = p.moveTo;
            } else {
                p.route.clear();
            }
        }
    }
    Vec3 d = flat(goal - p.body.position);
    const float dist = length(d);
    const float arrive = giant ? 0.15f * p.body.height : 1.5f;
    in.viewYaw = dist > 1e-3f ? yawTo(d) : p.body.yaw;
    if (dist > arrive) in.move = Vec2{0.35f * p.strafe, 1.0f};
    const SkPlayer* t = player(p.target);
    if (t && t->alive) {
        // Fight while moving: face the target (third-person aiming turns the body without turning the run).
        const Vec3 to = t->body.position - p.body.position;
        in.aiming = true;
        in.aimYaw = yawTo(to);
        p.aim = normalize(to + Vec3{0, 0.5f * (t->body.height - p.body.height), 0});
    } else {
        p.aim = Quat::fromAxisAngle({0, 1, 0}, p.body.yaw).rotate(Vec3{0, 0, -1});
    }
    // Getting no nearer the waypoint for seconds (swimming along a bank it can't climb, sliding along a
    // wall: moving, but not getting anywhere) counts as stuck too.
    bool noProgress = false;
    if (p.detourT > 0 || length(flat(goal - p.progGoal)) > 1.0f) {
        p.progGoal = goal;
        p.bestDist = dist;
        p.progT = 0;
    } else if (dist < p.bestDist - 1.0f) {
        p.bestDist = dist;
        p.progT = 0;
    } else if ((p.progT += dt) > 5.0f && dist > arrive * 2.0f) {
        noProgress = true;
        p.progT = 0;
        p.bestDist = 1e9f;
    }
    // Stuck (a wall, a cliff): step aside, the Meccaryn jets over.
    p.stuckT += dt;
    if (p.stuckT > 1.5f || noProgress) {
        const float moved = length(flat(p.body.position - p.lastPos));
        if (dist > arrive * 2.0f && (moved < 0.8f || noProgress)) {
            // Following a route: the grid was wrong about the way ahead (learnt for everyone of this kind).
            if (p.detourT <= 0 && p.routeAt < p.route.size()) {
                const SkNav::Profile prof = profileOf(p.team);
                const Vec3 ahead = p.body.position + d * (std::min(dist, nav.cell()) / std::max(1e-3f, dist));
                nav.penalize(ahead, prof);
            }
            const Vec3 side = Vec3{-d.z, 0, d.x} * (1.0f / std::max(1e-3f, dist)) * ((rnd() < 0.5f ? -1.0f : 1.0f) * (8.0f + 8.0f * rnd()));
            p.moveTo = p.body.position + side + d * (std::min(dist, 10.0f) / std::max(1e-3f, dist));
            p.detourT = 2.5f;
            p.jetT = teams[size_t(p.team)].hero == SkHero::Mecc ? 0.9f : 0.15f;  // a jump for the others
            p.route.clear();  // and a fresh route afterwards
        }
        p.stuckT = 0;
        p.lastPos = p.body.position;
    }
    // Walking into a bank or a step (barely moving while trying): jump it (the Meccaryn jets).
    // (Wading is slow, not blocked: no jumping in the water.)
    const bool wading = ground(p.body.position) < seaLevel + 0.2f;
    if (dist > arrive && p.body.onGround && !wading && p.body.horizontalSpeed() < 0.15f * p.body.params.runSpeed) {
        p.blockedT += dt;
        if (p.blockedT > 0.4f && p.jetT <= 0) {
            p.jetT = teams[size_t(p.team)].hero == SkHero::Mecc ? 0.7f : 0.15f;
            p.blockedT = -0.6f;  // (give the jump time to land)
        }
    } else {
        p.blockedT = std::min(p.blockedT, 0.0f);
    }
    if (p.jetT > 0) {
        p.jetT -= dt;
        in.jet = true;  // the jetpack (a jump for the others)
    }
    // A goal well above: jet up to it. Far away, a Meccaryn travels in jetpack hops (thrust while the
    // tank is well filled, glide, land and refill).
    // A flag carrier (and anyone whose fight is still well off) keeps travelling at full speed.
    const float toGoal = length(flat(p.moveTo - p.body.position));
    const bool hurry = !t || p.carrying >= 0 || length(flat(t->body.position - p.body.position)) > 30.0f;
    if (teams[size_t(p.team)].hero == SkHero::Mecc) {
        if (goal.y > p.body.position.y + 4.0f && dist < 40.0f) in.jet = true;
        if (toGoal > 60.0f && hurry && p.jetT <= 0 && p.body.onGround && p.body.fuel() > 0.7f) p.jetT = 1.6f;
    }
    in.sprint = toGoal > 30.0f && hurry;
    if (teams[size_t(p.team)].hero == SkHero::Kabuto && p.carrying >= 0) {  // (balance: a flag slows the giant down)
        in.sprint = false;
        in.move = Vec2{in.move.x * 0.8f, in.move.y * 0.8f};
    }
    p.body.update(dt, in);
    static const int dbgId = std::getenv("GVR_SK_DEBUG") ? std::atoi(std::getenv("GVR_SK_DEBUG")) : -1;  // test hook
    if (p.id == dbgId && std::floor(time * 2.0f) != std::floor((time - dt) * 2.0f))
        LOGI("skdbg %.1f pos(%.1f %.1f %.1f) gnd%.1f sea%.1f on%d swim%d spd%.1f goal(%.1f %.1f %.1f) moveTo(%.1f %.1f %.1f) r%zu/%zu det%.1f blk%.2f jet%.2f mv%.1f,%.1f",
             time, p.body.position.x, p.body.position.y, p.body.position.z, ground(p.body.position), p.body.seaLevel, p.body.onGround, p.body.swimming(),
             p.body.horizontalSpeed(), goal.x, goal.y, goal.z, p.moveTo.x, p.moveTo.y, p.moveTo.z, p.routeAt, p.route.size(), p.detourT, p.blockedT, p.jetT, in.move.x, in.move.y);
}

void Skirmish::updateShots(float dt) {
    for (auto& s : shots) {
        s.life += dt;
        const Vec3 from = s.pos;
        s.vel.y -= s.gravity * dt;
        s.pos = s.pos + s.vel * dt;
        if (s.life > 3.0f || (!mirror && (s.pos.y < ground(s.pos) || !clear(from, s.pos)))) { s.life = 1e9f; continue; }
        if (s.visual || mirror) continue;  // drawn only: its hits are reported by whoever fired it
        // A player within reach of the segment.
        const Vec3 seg = s.pos - from;
        const float L = length(seg);
        for (auto& o : players) {
            if (!o.alive || o.id == s.shooter) continue;
            if (o.team == s.team && !rules.friendlyFire) continue;
            const float r = std::max(0.5f, o.body.radius * 1.5f);
            // Nearest point of the body's axis segment to the shot segment (sampled: shots are short).
            const Vec3 a0 = o.body.position + Vec3{0, std::min(0.5f * o.body.height, r), 0};
            const Vec3 a1 = o.body.position + Vec3{0, std::max(0.5f * o.body.height, o.body.height - r), 0};
            bool hit = false;
            for (int k = 0; k <= 4 && !hit; ++k) {
                const Vec3 q = from + seg * (0.25f * float(k));
                const Vec3 ax = a1 - a0;
                const float t = std::max(0.0f, std::min(1.0f, dot(q - a0, ax) / std::max(1e-4f, dot(ax, ax))));
                hit = length(q - (a0 + ax * t)) < r + 0.1f;
            }
            if (!hit || L <= 0) continue;
            damage(o.id, s.damage, s.shooter, from);
            s.life = 1e9f;
            break;
        }
    }
    shots.erase(std::remove_if(shots.begin(), shots.end(), [](const SkShot& s) { return s.life >= 1e8f; }), shots.end());
}

void Skirmish::updateFlags(float dt) {
    for (size_t i = 0; i < flags.size(); ++i) {
        SkFlag& f = flags[i];
        if (!f.used) continue;
        if (f.carrier >= 0) {
            const SkPlayer* c = player(f.carrier);
            if (!c || !c->alive) { f.carrier = -1; continue; }
            f.pos = c->body.position + Vec3{0, 0.6f * c->body.height, 0};
        } else if (!f.atHome) {
            f.droppedT += dt;
            if (f.droppedT > rules.flagReturnSeconds) {
                f.atHome = true;
                f.pos = f.home;
                SkEvent e;
                e.kind = SkEvent::FlagReturned;
                e.team = int(i);
                e.at = f.home;
                events.push_back(e);
            }
        }
    }
    for (auto& p : players) p.noGrabT = std::max(0.0f, p.noGrabT - dt);
    if (time < rules.captureDelay) return;
    for (auto& p : players) {
        if (!p.alive) continue;
        const float r = touchRadius(p);
        for (size_t i = 0; i < flags.size(); ++i) {
            SkFlag& f = flags[i];
            if (!f.used) continue;
            const Vec3 fp{f.pos.x, p.body.position.y, f.pos.z};
            const bool touch = length(fp - p.body.position) < r && std::fabs(f.pos.y - p.body.position.y) < std::max(4.0f, p.body.height);
            if (int(i) == p.team) {
                if (touch && !f.atHome && f.carrier < 0) {  // own flag lying out: back home
                    f.atHome = true;
                    f.pos = f.home;
                    SkEvent e;
                    e.kind = SkEvent::FlagReturned;
                    e.a = p.id;
                    e.team = int(i);
                    e.at = f.home;
                    events.push_back(e);
                }
            } else if (touch && f.carrier < 0 && p.carrying < 0 && p.noGrabT <= 0) {
                f.carrier = p.id;
                p.carryHurt = 0;
                f.atHome = false;
                p.carrying = int(i);
                SkEvent e;
                e.kind = SkEvent::FlagTaken;
                e.a = p.id;
                e.team = int(i);
                e.at = f.pos;
                events.push_back(e);
            }
        }
        // Home with an enemy flag (and our own at home, if we have one): a capture.
        if (p.carrying >= 0) {
            const SkFlag& own = flags[size_t(p.team)];
            const Vec3 base = teams[size_t(p.team)].base;
            const float d = length(flat(base - p.body.position));
            if (d < r * 1.5f && (!own.used || own.atHome)) {
                SkFlag& f = flags[size_t(p.carrying)];
                f.carrier = -1;
                f.atHome = true;
                f.pos = f.home;
                ++p.captures;
                ++teams[size_t(p.team)].captures;
                SkEvent e;
                e.kind = SkEvent::FlagCaptured;
                e.a = p.id;
                e.team = p.carrying;
                e.at = base;
                events.push_back(e);
                p.carrying = -1;
            }
        }
    }
}

void Skirmish::update(float dt) {
    if (dt <= 0) return;
    time += dt;
    routesThisUpdate = 0;
    if (mirror) {  // a joiner: the host decides; only the shots fly on between snapshots
        updateShots(dt);
        return;
    }
    if (winner >= 0) { shots.clear(); return; }  // (no bolts hanging in the air through the result)
    for (auto& p : players) {
        p.firing = false;
        if (!p.alive) {
            p.deadT += dt;
            if (p.bot && p.deadT >= rules.respawnSeconds) spawn(p.id);
            continue;
        }
        if (!p.bot) continue;  // the local and remote players move themselves
        p.fireCool -= dt;
        p.giveUpT -= dt;
        p.think -= dt;
        if (p.think <= 0) {
            p.think = 0.35f + 0.1f * rnd();
            think(p);
        }
        moveBot(p, dt);
        // The sea: a bot that falls in (and can't swim out) drowns after a while.
        const SkPlayer* t = player(p.target);
        if (!t || !t->alive) continue;
        const Vec3 tc = t->body.position + Vec3{0, 0.55f * t->body.height, 0};
        const float dist = length(tc - p.body.position);
        if (teams[size_t(p.team)].hero == SkHero::Kabuto) {
            // Kabuto stomps whatever is at his feet.
            const float reach = 0.22f * p.body.height;
            if (p.fireCool <= 0 && length(flat(t->body.position - p.body.position)) < reach) {
                p.fireCool = 1.6f;
                SkEvent e;
                e.kind = SkEvent::Stomp;
                e.a = p.id;
                e.at = p.body.position + Quat::fromAxisAngle({0, 1, 0}, p.body.yaw).rotate(Vec3{0, 0, -0.1f * p.body.height});
                events.push_back(e);
                for (auto& o : players)
                    if (o.alive && o.team != p.team && length(flat(o.body.position - e.at)) < reach * 1.2f) damage(o.id, 45.0f, p.id, e.at);
            }
        } else if (p.fireCool <= 0 && dist < 80.0f) {
            // Lead a moving target a little.
            const float speed = teams[size_t(p.team)].hero == SkHero::Reaper ? 45.0f : 70.0f;
            fire(p, tc + t->body.velocity * (dist / speed) * 0.7f);
        }
    }
    updateShots(dt);
    updateFlags(dt);
    for (size_t t = 0; t < teams.size(); ++t) {
        const bool won = teams[t].captures >= rules.captureLimit || (rules.killLimit > 0 && teams[t].kills >= rules.killLimit);
        if (won && winner < 0) {
            winner = int(t);
            SkEvent e;
            e.kind = SkEvent::Win;
            e.team = int(t);
            events.push_back(e);
        }
    }
}

}  // namespace gvr
