#include "game/jetski.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <queue>

#include "game/player.h"  // angleDelta

namespace gvr {

bool stepJetski(JetskiState& s, const JetskiParams& p, float throttle, float steer, bool turbo, float dt,
                const std::function<float(float, float)>& ground, float seaLevel) {
    throttle = std::max(-1.0f, std::min(1.0f, throttle));
    steer = std::max(-1.0f, std::min(1.0f, steer));
    const float top = turbo ? p.turboSpeed : p.maxSpeed;
    if (turbo) throttle = 1.0f;
    float want = throttle >= 0 ? throttle * top : throttle * p.reverseSpeed;
    if (throttle == 0.0f) s.speed -= s.speed * std::min(1.0f, p.drag * dt);
    else if (s.speed < want) s.speed = std::min(want, s.speed + (s.speed < 0 ? p.brake : p.accel) * (turbo ? 1.6f : 1.0f) * dt);
    else s.speed = std::max(want, s.speed - p.brake * dt);
    // Turning: needs way on (stalls when stopped), a little tighter at moderate speed.
    const float k = std::min(1.0f, std::fabs(s.speed) / 6.0f);
    // Stick right (+) turns right (yaw decreasing: forward is (-sin yaw, 0, -cos yaw)); leans into it.
    s.yaw -= steer * p.turnRate * k * (s.speed < 0 ? -1.0f : 1.0f) * dt;
    s.bank += (-steer * 0.35f * k - s.bank) * std::min(1.0f, dt * 5.0f);
    s.bob += dt;
    const Vec3 fwd{-std::sin(s.yaw), 0, -std::cos(s.yaw)};
    Vec3 np = s.pos + fwd * (s.speed * dt);
    bool bumped = false;
    auto surface = [&](float x, float z) { return std::max(seaLevel, ground ? ground(x, z) : seaLevel); };
    const float here = s.pos.y, next = surface(np.x, np.z);
    const float run = std::max(1e-3f, std::fabs(s.speed) * dt);
    if (s.air) {
        // Flying off a ramp: gravity until it meets the water or the ground again.
        s.vy -= p.gravity * dt;
        np.y = s.pos.y + s.vy * dt;
        if (np.y <= next) {
            np.y = next;
            s.air = false;
            s.vy = 0;
        }
    } else if (next > seaLevel + 0.3f && ((next - here) / run > p.maxSlope && (std::fabs(s.speed) < 3.0f || next - here > 2.5f || next > seaLevel + 8.0f))) {
        // (Moving, it hops up small steps of the heightfield - sandbar edges, the islet race 3's line crosses (5.7 m) -
        // like a ramp; not up a hillside: over 8 m above the sea a steep face stops it at any speed. 2026-10-28: the
        // test autopilot drove up a hill to 38 m above the sea, a frame's climb at a time.)
        // A face too steep to drive up: knocked back, most of the speed lost.
        s.speed = -0.3f * s.speed;
        np = s.pos;
        bumped = true;
    } else {
        // Up the slope it climbs; where the ground falls away faster than it can follow, it leaves
        // the ground with the climb it had (a ramp jump).
        const float climb = std::max(-20.0f, std::min((next - here) / dt, 0.8f * std::fabs(s.speed)));
        if (next < here - 0.3f && s.vy > 2.0f) {
            s.air = true;
            np.y = here + s.vy * dt;
        } else {
            np.y = next;
            s.vy = climb;
        }
    }
    if (!s.air && ground && ground(np.x, np.z) > seaLevel + 0.2f) s.speed -= s.speed * std::min(1.0f, p.landDrag * dt);  // on land
    s.pos = np;
    return bumped;
}

std::vector<Vec3> waterRoute(const Vec3& a, const Vec3& b, const std::function<float(float, float)>& ground, float maxGround, float cell,
                             float margin) {
    auto wet = [&](float x, float z) { return ground(x, z) < maxGround; };
    auto clear = [&](const Vec3& p, const Vec3& q) {
        const float l = length(Vec3{q.x - p.x, 0, q.z - p.z});
        const int n = std::max(1, int(l / (cell * 0.5f)));
        for (int k = 0; k <= n; ++k) {
            const float t = float(k) / float(n);
            if (!wet(p.x + (q.x - p.x) * t, p.z + (q.z - p.z) * t)) return false;
        }
        return true;
    };
    if (clear(a, b)) return {a, b};
    const float x0 = std::min(a.x, b.x) - margin, z0 = std::min(a.z, b.z) - margin;
    const int W = int((std::fabs(a.x - b.x) + 2 * margin) / cell) + 1, H = int((std::fabs(a.z - b.z) + 2 * margin) / cell) + 1;
    if (W * H > 1500000) return {a, b};
    auto idx = [&](int x, int z) { return z * W + x; };
    auto cx = [&](float x) { return std::max(0, std::min(W - 1, int((x - x0) / cell))); };
    auto cz = [&](float z) { return std::max(0, std::min(H - 1, int((z - z0) / cell))); };
    std::vector<uint8_t> water(size_t(W) * size_t(H));
    for (int z = 0; z < H; ++z)
        for (int x = 0; x < W; ++x) water[size_t(idx(x, z))] = wet(x0 + (float(x) + 0.5f) * cell, z0 + (float(z) + 0.5f) * cell);
    const int sx = cx(a.x), sz = cz(a.z), tx = cx(b.x), tz = cz(b.z);
    water[size_t(idx(sx, sz))] = water[size_t(idx(tx, tz))] = 1;  // the end points themselves
    std::vector<float> dist(size_t(W) * size_t(H), 1e30f);
    std::vector<int> from(size_t(W) * size_t(H), -1);
    using Node = std::pair<float, int>;
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open;
    const int start = idx(sx, sz), goal = idx(tx, tz);
    dist[size_t(start)] = 0;
    open.push({0, start});
    auto h = [&](int i) { const int x = i % W, z = i / W; return std::sqrt(float((x - tx) * (x - tx) + (z - tz) * (z - tz))); };
    while (!open.empty()) {
        const auto [f, i] = open.top();
        open.pop();
        if (i == goal) break;
        if (f - h(i) > dist[size_t(i)] + 1e-3f) continue;
        const int x = i % W, z = i / W;
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) {
                if (!dx && !dz) continue;
                const int nx = x + dx, nz = z + dz;
                if (nx < 0 || nz < 0 || nx >= W || nz >= H) continue;
                const int j = idx(nx, nz);
                if (!water[size_t(j)]) continue;
                const float nd = dist[size_t(i)] + ((dx && dz) ? 1.4142f : 1.0f);
                if (nd < dist[size_t(j)]) { dist[size_t(j)] = nd; from[size_t(j)] = i; open.push({nd + h(j), j}); }
            }
    }
    if (from[size_t(goal)] < 0) return {a, b};
    std::vector<Vec3> cells;
    for (int i = goal; i != start && i >= 0; i = from[size_t(i)])
        cells.push_back(Vec3{x0 + (float(i % W) + 0.5f) * cell, a.y, z0 + (float(i / W) + 0.5f) * cell});
    std::reverse(cells.begin(), cells.end());
    // Keep only the corners: the farthest point still in clear sight, step by step.
    std::vector<Vec3> out{a};
    size_t k = 0;
    while (k < cells.size()) {
        size_t far = k;
        for (size_t j = k; j < cells.size(); ++j)
            if (clear(out.back(), cells[j])) far = j;
        out.push_back(cells[far]);
        k = far + 1;
    }
    out.push_back(b);
    return out;
}

std::vector<Vec3> orderGates(const std::vector<Vec3>& gates, const Vec3& start, const std::function<float(const Vec3&, const Vec3&)>& cost) {
    const size_t n = gates.size();
    if (n < 3) return gates;
    // Node 0 = the start, 1..n = gates.
    std::vector<Vec3> pts{start};
    pts.insert(pts.end(), gates.begin(), gates.end());
    const size_t m = pts.size();
    std::vector<float> c(m * m);
    for (size_t i = 0; i < m; ++i)
        for (size_t j = 0; j < m; ++j) c[i * m + j] = i == j ? 0 : cost(pts[i], pts[j]);
    std::vector<size_t> tour{0};
    std::vector<bool> used(m, false);
    used[0] = true;
    for (size_t k = 1; k < m; ++k) {
        size_t best = 0;
        float bd = 1e30f;
        for (size_t j = 1; j < m; ++j)
            if (!used[j] && c[tour.back() * m + j] < bd) { bd = c[tour.back() * m + j]; best = j; }
        used[best] = true;
        tour.push_back(best);
    }
    // 2-opt on the closed tour (the start stays first).
    for (bool improved = true; improved;) {
        improved = false;
        for (size_t i = 1; i + 1 < m; ++i)
            for (size_t j = i + 1; j < m; ++j) {
                const size_t a = tour[i - 1], b = tour[i], cc = tour[j], d = tour[(j + 1) % m];
                if (c[a * m + cc] + c[b * m + d] + 1e-3f < c[a * m + b] + c[cc * m + d]) {
                    std::reverse(tour.begin() + long(i), tour.begin() + long(j) + 1);
                    improved = true;
                }
            }
    }
    // Direction: the one that agrees with more of the data's consecutive gate pairs.
    std::vector<size_t> pos(m);
    for (size_t i = 0; i < m; ++i) pos[tour[i]] = i;
    int agree = 0;
    for (size_t g = 1; g + 1 < m; ++g) agree += pos[g + 1] > pos[g] ? 1 : -1;
    if (agree < 0) std::reverse(tour.begin() + 1, tour.end());
    std::vector<Vec3> out;
    for (size_t i = 1; i < m; ++i) out.push_back(pts[tour[i]]);
    return out;
}

void RaceDirector::setup(const std::vector<Vec3>& g, const std::vector<std::vector<Vec3>>& lines, int rivals, const Vec3* finish) {
    gates = g;
    hasFinishLine = finish != nullptr;
    if (finish) finishLine = *finish;
    racers.clear();
    time = -params.countdown;
    nextGate = missed = 0;
    playerFinished = lost = false;
    playerPlace = 0;
    for (int k = 0; k < rivals && k < int(lines.size()); ++k) {
        Racer r;
        r.path = lines[size_t(k)];
        if (r.path.size() < 2) continue;
        for (size_t i = 1; i < r.path.size(); ++i) r.length += length(r.path[i] - r.path[i - 1]);
        // Rivals of slightly different pace (ours): the fastest a little under the player's full
        // throttle, as they never miss a corner - a clean line wins it, the turbo pickups comfortably.
        r.base = 15.5f + 1.2f * float(k % 3) + 0.25f * float(k);
        r.pos = along(r.path, 0.0f, &r.yaw);
        racers.push_back(r);
    }
}

Vec3 RaceDirector::along(const std::vector<Vec3>& path, float s, float* yaw) {
    for (size_t i = 1; i < path.size(); ++i) {
        const Vec3 d = path[i] - path[i - 1];
        const float l = length(d);
        if (s <= l || i + 1 == path.size()) {
            const float t = l > 1e-3f ? std::min(1.0f, s / l) : 0.0f;
            if (yaw) *yaw = std::atan2(-d.x, -d.z);
            return path[i - 1] + d * t;
        }
        s -= l;
    }
    return path.empty() ? Vec3{} : path.back();
}

float RaceDirector::playerProgress(const Vec3& player) const {
    if (gates.empty()) return 0;
    if (playerFinished) return 1.0f;
    // Gates passed plus the fraction of the way to the next.
    const Vec3 prev = nextGate == 0 ? (hasFinishLine ? finishLine : gates.front()) : gates[size_t(nextGate - 1)];
    const Vec3 next = nextGate < int(gates.size()) ? gates[size_t(nextGate)] : (hasFinishLine ? finishLine : gates.back());
    const float seg = std::max(1.0f, length(Vec3{next.x - prev.x, 0, next.z - prev.z}));
    const float left = length(Vec3{next.x - player.x, 0, next.z - player.z});
    const float f = std::max(0.0f, std::min(1.0f, 1.0f - left / seg));
    return (float(nextGate) + f) / float(gates.size() + (hasFinishLine ? 1 : 0));
}

int RaceDirector::place(const Vec3& player) const {
    if (playerFinished) return playerPlace;
    const float me = playerProgress(player);
    int ahead = 0;
    for (const auto& r : racers)
        if (r.finished || (r.length > 0 && r.along / r.length > me)) ++ahead;
    return ahead + 1;
}

void RaceDirector::update(float dt, const Vec3& player) {
    gateEvents.clear();
    if (lost || playerFinished) {
        // The rivals finish their laps anyway.
    }
    time += dt;
    if (time < 0) return;
    const float me = playerProgress(player);
    for (auto& r : racers) {
        if (r.finished) continue;
        // Rubber band (ours): a little slower far ahead of the player, quicker far behind.
        const float lead = r.length > 0 ? r.along / r.length - me : 0;
        const float band = lead > 0.08f ? 0.9f : (lead < -0.08f ? 1.08f : 1.0f);
        const float target = r.spin > 0 ? 4.0f : r.base * band;
        r.spin = std::max(0.0f, r.spin - dt);
        r.speed += (target - r.speed) * std::min(1.0f, dt * 0.8f);
        r.along += r.speed * dt;
        float yaw = r.yaw;
        r.pos = along(r.path, r.along, &yaw);
        const float d = angleDelta(r.yaw, yaw);
        r.bank += (std::max(-0.35f, std::min(0.35f, -d * 3.0f)) - r.bank) * std::min(1.0f, dt * 4.0f);
        r.yaw += d * std::min(1.0f, dt * 4.0f);
        if (r.spin > 0) r.yaw += 6.0f * dt;
        if (r.along >= r.length) {
            r.finished = true;
            r.finishTime = time;
        }
    }
    if (playerFinished || lost) return;
    // Gates, in order. Passing a later gate first counts the ones skipped as missed.
    for (int g = nextGate; g < int(gates.size()) && g < nextGate + 3; ++g) {
        const Vec3 c = gates[size_t(g)];
        if (length(Vec3{player.x - c.x, 0, player.z - c.z}) > params.gateRadius) continue;
        for (int m = nextGate; m < g; ++m) { ++missed; gateEvents.push_back(-1); }
        gateEvents.push_back(+1);
        nextGate = g + 1;
        break;
    }
    if (missed >= params.missLimit) { lost = true; return; }
    // Finish: every gate taken (or missed), then across the line.
    if (nextGate >= int(gates.size())) {
        const bool atLine = !hasFinishLine ||
                            length(Vec3{player.x - finishLine.x, 0, player.z - finishLine.z}) < params.finishRadius;
        if (atLine) {
            playerFinished = true;
            int before = 0;
            for (const auto& r : racers) before += r.finished;
            playerPlace = before + 1;
        }
    }
}

}  // namespace gvr
