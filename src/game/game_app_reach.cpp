// Test hooks for mission completability:
// - GVR_REACH_CHECK=1: can the hero walk from where he starts to each "reach" objective? A flood fill
//   over the terrain on a 2 m grid: a step is blocked uphill steeper than the walking limit
//   (PlayerController maxSlope, 50 degrees; a giant judges it over his stride, slopeWindow), by water
//   deeper than Kabuto can wade (the others swim), and by scenery a body would be pushed out of.
//   Logs, per objective, reachable or not, and the nearest the walk gets.
// - GVR_REACH_WALK=1 as well: then walk there in play, along the found path, with the real player
//   controller as the judge: where it won't go on (no progress for 2 s), that spot is marked blocked
//   and the path planned again from where he stands. Logs "reach walk: arrived" or gives up.
#include "game/game_app.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <deque>

#include "core/log.h"
#include "scene/asset_world.h"
#include "scene/collision.h"
#include "scene/terrain.h"

namespace gvr {

namespace {
constexpr float kCell = 2.0f;

// Flood fill from the player; fills `seen` (1 walkable, 2 blocked) and `parent`; grid origin `lo`, size W x H.
void reachFill(const AssetWorld& world, const PlayerController& pc, bool giant, const std::vector<uint8_t>& extraBlocked, Vec3& lo, int& W,
               int& H, std::vector<uint8_t>& seen, std::vector<int>& parent, std::vector<float>& hgt) {
    const Terrain& terr = world.terrain();
    lo = terr.boundsMin();
    const Vec3 hi = terr.boundsMax();
    W = int((hi.x - lo.x) / kCell) + 1;
    H = int((hi.z - lo.z) / kCell) + 1;
    if (W <= 2 || H <= 2 || size_t(W) * size_t(H) > 8000000u) { W = H = 0; return; }
    if (hgt.size() != size_t(W) * H) {
        hgt.assign(size_t(W) * H, -20.0f);
        for (int z = 0; z < H; ++z)
            for (int x = 0; x < W; ++x) {
                float y;
                // (Off the terrain is open sea, its floor at -20 m as for the player: swimmable.)
                hgt[size_t(z) * W + x] = terr.heightAt(lo.x + x * kCell, lo.z + z * kCell, y) ? y : -20.0f;
            }
    }
    const float sea = pc.seaLevel;
    (void)giant;  // (everyone swims: Kabuto wades, then swims slowly)
    const float wade = 1e9f;
    // A jumper (Kabuto, Delphi) gets up a ledge lower than most of his jump; the Meccaryn's jetpack
    // gets him up anything (the walker presses jump where it's stuck).
    const bool pure = std::getenv("GVR_REACH_PURE") != nullptr;  // (slope only: no jumps, no stride window)
    const float jumpUp = pure ? 1e9f : pc.jetpackAvailable ? 1e9f : (pc.params.jumper ? 0.75f * pc.params.jumpSpeed * pc.params.jumpSpeed / (2.0f * pc.params.gravity) : 0.0f);
    const float maxSlope = std::getenv("GVR_REACH_SLOPE") ? float(std::atof(std::getenv("GVR_REACH_SLOPE"))) : pc.params.maxSlope;
    seen.assign(size_t(W) * H, 0);
    parent.assign(size_t(W) * H, -1);
    for (size_t i = 0; i < extraBlocked.size() && i < seen.size(); ++i)
        if (extraBlocked[i]) seen[i] = 2;
    int sx = int(std::lround((pc.position.x - lo.x) / kCell)), sz = int(std::lround((pc.position.z - lo.z) / kCell));
    auto valid = [&](int x, int z) { return x >= 0 && z >= 0 && x < W && z < H && !std::isnan(hgt[size_t(z) * W + x]); };
    if (!valid(sx, sz)) {  // (swimming off the terrain's edge: start from the nearest ground cell)
        int bx = -1, bz = -1;
        for (int r = 1; r < 40 && bx < 0; ++r)
            for (int dz = -r; dz <= r && bx < 0; ++dz)
                for (int dx = -r; dx <= r; ++dx)
                    if (valid(sx + dx, sz + dz)) { bx = sx + dx; bz = sz + dz; break; }
        if (bx < 0) { W = H = 0; return; }
        sx = bx;
        sz = bz;
    }
    const int s = sz * W + sx;
    seen[size_t(s)] = 1;
    std::deque<int> q;
    q.push_back(s);
    while (!q.empty()) {
        const int c = q.front();
        q.pop_front();
        const int cx = c % W, cz = c / W;
        // Where he stands: on the bed, or afloat with his feet at floatDepth below the surface.
        auto stand = [&](float h) { return std::max(h, sea - pc.floatDepth); };
        const float hc = stand(hgt[size_t(c)]);
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) {
                if (!dx && !dz) continue;
                const int x = cx + dx, z = cz + dz;
                if (x < 0 || z < 0 || x >= W || z >= H) continue;
                const int k = z * W + x;
                if (seen[size_t(k)]) continue;
                const float hk0 = hgt[size_t(k)];
                if (std::isnan(hk0) || hk0 < sea - wade) continue;
                const float hk = stand(hk0);
                const float run = kCell * ((dx && dz) ? 1.4142f : 1.0f);
                const bool steepUp = hk - hc > run * maxSlope;
                if (pure && steepUp) continue;
                if (steepUp) {
                    // Up a steep bit (a jump or a giant's stride): only onto ground he can stand on
                    // (not steeper than the limit there - he would slide back down).
                    auto hAt = [&](int xx, int zz) { return valid(xx, zz) ? hgt[size_t(zz) * W + xx] : hk; };
                    auto gentler = [](float a, float b) { return std::fabs(a) < std::fabs(b) ? a : b; };
                    const float gx = gentler((hAt(x + 1, z) - hk) / kCell, (hk - hAt(x - 1, z)) / kCell);
                    const float gz = gentler((hAt(x, z + 1) - hk) / kCell, (hk - hAt(x, z - 1)) / kCell);
                    if (std::sqrt(gx * gx + gz * gz) > maxSlope) continue;
                }
                if (steepUp && hk - hc > jumpUp) {
                    // A giant judges the slope over his stride, as the controller does each frame.
                    const float w = pc.params.slopeWindow;
                    if (w <= 0 || hk - hc > pc.params.maxStepUp) continue;
                    const float ux = float(dx) * kCell / run, uz = float(dz) * kCell / run;
                    auto windowOk = [&](float px, float pz, float py) {
                        float ya;
                        return terr.heightAt(px + ux * w, pz + uz * w, ya) && ya - py <= w * maxSlope;
                    };
                    if (!windowOk(lo.x + cx * kCell, lo.z + cz * kCell, hc) || !windowOk(lo.x + x * kCell, lo.z + z * kCell, hk)) continue;
                }
                {   // Scenery: a body standing here is pushed out of a rock or a building -> blocked.
                    Vec3 f{lo.x + x * kCell, hk, lo.z + z * kCell};
                    const Vec3 f0 = f;
                    if (world.collision().pushOut(f, pc.radius * 1.05f, pc.height * 0.8f, pc.stepUp) &&
                        std::sqrt((f.x - f0.x) * (f.x - f0.x) + (f.z - f0.z) * (f.z - f0.z)) > 0.25f * pc.radius) {
                        seen[size_t(k)] = 2;
                        continue;
                    }
                }
                seen[size_t(k)] = 1;
                parent[size_t(k)] = c;
                q.push_back(k);
            }
    }
}
}  // namespace

bool GameApp::planReach(int objective, bool log) {
    if (!assetWorld_ || objective < 0 || size_t(objective) >= objectives_.size()) return false;
    Vec3 lo;
    int W = 0, H = 0;
    std::vector<uint8_t> seen;
    std::vector<int> parent;
    reachFill(*assetWorld_, player_, hero_ == Hero::Kabuto, reachBlocked_, lo, W, H, seen, parent, reachHgt_);
    if (!W) { LOGW("reach check: start outside the terrain"); return false; }
    reachW_ = W;
    reachLo_ = lo;
    if (reachBlocked_.size() != size_t(W) * H) reachBlocked_.assign(size_t(W) * H, 0);
    const Objective& o = objectives_[size_t(objective)];
    const Vec3 c = (o.boundsMin + o.boundsMax) * 0.5f;
    int goal = -1;
    float best = 1e9f;
    for (int z = 0; z < H && goal < 0; ++z)
        for (int x = 0; x < W; ++x) {
            if (seen[size_t(z) * W + x] != 1) continue;
            const Vec3 p{lo.x + x * kCell, 0, lo.z + z * kCell};
            best = std::min(best, std::sqrt((p.x - c.x) * (p.x - c.x) + (p.z - c.z) * (p.z - c.z)));
            if (p.x >= o.boundsMin.x && p.x <= o.boundsMax.x && p.z >= o.boundsMin.z && p.z <= o.boundsMax.z) { goal = z * W + x; break; }
        }
    if (const char* dump = std::getenv("GVR_REACH_DUMP")) {
        // Debug: heights (grey), walkable (green tint), blocked (red), the player (blue) and the objective (yellow) as a PPM.
        if (FILE* f = std::fopen(dump, "wb")) {
            std::fprintf(f, "P6 %d %d 255\n", W, H);
            float mn = 1e9f, mx = -1e9f;
            for (float v : reachHgt_) if (!std::isnan(v)) { mn = std::min(mn, v); mx = std::max(mx, v); }
            const int px = int(std::lround((player_.position.x - lo.x) / kCell)), pz = int(std::lround((player_.position.z - lo.z) / kCell));
            for (int z = 0; z < H; ++z)
                for (int x = 0; x < W; ++x) {
                    const float v = reachHgt_[size_t(z) * W + x];
                    int g = std::isnan(v) ? 0 : int(40 + 200 * (v - mn) / std::max(1.0f, mx - mn));
                    unsigned char c[3] = {(unsigned char)g, (unsigned char)g, (unsigned char)g};
                    const uint8_t sv = seen[size_t(z) * W + x];
                    if (sv == 1) { c[0] = (unsigned char)(g * 0.6f); c[2] = (unsigned char)(g * 0.6f); }
                    if (sv == 2 || (size_t(z) * W + x < reachBlocked_.size() && reachBlocked_[size_t(z) * W + x])) { c[0] = 255; c[1] = 0; c[2] = 0; }
                    const Vec3 wp{lo.x + x * kCell, 0, lo.z + z * kCell};
                    if (wp.x >= o.boundsMin.x && wp.x <= o.boundsMax.x && wp.z >= o.boundsMin.z && wp.z <= o.boundsMax.z) { c[0] = 255; c[1] = 255; c[2] = 0; }
                    if (std::abs(x - px) <= 3 && std::abs(z - pz) <= 3) { c[0] = 0; c[1] = 0; c[2] = 255; }
                    std::fwrite(c, 1, 3, f);
                }
            std::fclose(f);
            LOGI("reach check: map written to %s (%dx%d, heights %.0f..%.0f m)", dump, W, H, mn, mx);
        }
    }
    if (std::getenv("GVR_REACH_PROFILE")) {
        // Heights in rings round the objective's centre (every 15 degrees, out to 120 m): where's the way up?
        for (int a = 0; a < 24; ++a) {
            const float ang = float(a) * 0.2618f;
            std::string line;
            for (int r = 0; r <= 120; r += 8) {
                float y = -99;
                assetWorld_->terrain().heightAt(c.x + std::cos(ang) * float(r), c.z + std::sin(ang) * float(r), y);
                char b[16];
                std::snprintf(b, sizeof b, " %4.0f", y);
                line += b;
            }
            LOGI("reach profile %3d deg:%s", a * 15, line.c_str());
        }
    }
    if (log)
        LOGI("reach check: objective %d at game (%.0f, %.0f): %s (nearest walk %.0f m from its centre)", objective, -c.x / gGameUnitsToMeters,
             c.z / gGameUnitsToMeters, goal >= 0 ? "REACHABLE" : "NOT reachable on foot", goal >= 0 ? 0.0f : best);
    reachPath_.clear();
    if (goal < 0) return false;
    std::vector<Vec3> rev;
    for (int k = goal; k >= 0; k = parent[size_t(k)]) rev.push_back(Vec3{lo.x + (k % W) * kCell, 0, lo.z + (k / W) * kCell});
    for (size_t k = rev.size(); k-- > 0;) reachPath_.push_back(rev[k]);
    if (log && std::getenv("GVR_REACH_STEEP")) {  // the path's steep climbs (game coordinates)
        for (size_t k = 1; k < reachPath_.size(); ++k) {
            float a, b;
            if (!assetWorld_->terrain().heightAt(reachPath_[k - 1].x, reachPath_[k - 1].z, a) || !assetWorld_->terrain().heightAt(reachPath_[k].x, reachPath_[k].z, b)) continue;
            const float run = length(reachPath_[k] - reachPath_[k - 1]);
            if (b - a > run * 1.19f)
                LOGI("reach steep: game (%.0f %.0f) -> (%.0f %.0f): %.1f m up over %.1f m (%.0f deg)", -reachPath_[k - 1].x / gGameUnitsToMeters,
                     reachPath_[k - 1].z / gGameUnitsToMeters, -reachPath_[k].x / gGameUnitsToMeters, reachPath_[k].z / gGameUnitsToMeters, b - a, run,
                     std::atan2(b - a, run) * 57.3f);
        }
    }
    return true;
}

void GameApp::checkReachObjectives() {
    for (size_t i = 0; i < objectives_.size(); ++i) {
        if (objectives_[i].kind != 1) continue;
        const bool ok = planReach(int(i), true);
        if (ok && std::getenv("GVR_REACH_WALK") && reachTarget_ < 0) {
            reachTarget_ = int(i);
            reachStuckT_ = 0;
            reachReplans_ = 0;
        }
    }
    reachPath_.clear();
    if (reachTarget_ >= 0 && planReach(reachTarget_, false))
        LOGI("reach walk: objective %d, %zu waypoints", reachTarget_, reachPath_.size());
}

// Steers the player along reachPath_ (test hook): stick forward, facing the next waypoint.
bool GameApp::reachWalkInput(Vec2& move, float& viewYaw, bool& jump, float dt) {
    if (reachPath_.empty() || reachTarget_ < 0) return false;
    if (objectives_[size_t(reachTarget_)].done) {
        LOGI("reach walk: arrived (objective done)");
        reachPath_.clear();
        reachTarget_ = -1;
        return false;
    }
    const Vec3 p{player_.position.x, 0, player_.position.z};
    const float near = std::max(3.0f, 1.5f * player_.radius);
    size_t dropped = 0;
    while (reachPath_.size() > 1 && length(reachPath_.front() - p) < near) {
        reachPath_.erase(reachPath_.begin());
        ++dropped;
    }
    reachStuckT_ = dropped ? 0.0f : reachStuckT_ + dt;
    jump = reachStuckT_ > 0.6f && std::fmod(reachStuckT_, 0.8f) < 0.25f;  // stuck: try jumping (or the jetpack) up it
    if (reachStuckT_ > 3.0f) {
        // The controller won't go on here: block the spot ahead (and round it) and plan again.
        const Vec3 w = reachPath_.front();
        const int wx = int(std::lround((w.x - reachLo_.x) / kCell)), wz = int(std::lround((w.z - reachLo_.z) / kCell));
        const int r = std::max(1, int(player_.radius / kCell));
        for (int dz = -r; dz <= r; ++dz)
            for (int dx = -r; dx <= r; ++dx) {
                const int x = wx + dx, z = wz + dz;
                if (x >= 0 && z >= 0 && x < reachW_ && size_t(z) * reachW_ + x < reachBlocked_.size()) reachBlocked_[size_t(z) * reachW_ + x] = 1;
            }
        reachStuckT_ = 0;
        bool planned = ++reachReplans_ <= 80 && planReach(reachTarget_, false);
        if (!planned && reachReplans_ <= 80 && reachResets_ < 4) {
            // Boxed in by our own marks: forget them and try again from here.
            ++reachResets_;
            std::fill(reachBlocked_.begin(), reachBlocked_.end(), uint8_t(0));
            planned = planReach(reachTarget_, false);
            LOGI("reach walk: cleared the blocked marks (%d) - %s", reachResets_, planned ? "a way again" : "still none");
        }
        if (!planned) {
            LOGI("reach walk: gave up after %d replans at (%.0f %.0f %.0f)", reachReplans_, player_.position.x, player_.position.y, player_.position.z);
            reachPath_.clear();
            reachTarget_ = -1;
            return false;
        }
        LOGI("reach walk: stuck at (%.0f %.0f %.0f)%s%s%s, waypoint (%.0f %.0f), replanned (%d): %zu waypoints", player_.position.x, player_.position.y,
             player_.position.z, player_.onGround ? " ground" : " air", player_.swimming() ? " swimming" : "", player_.sliding() ? " sliding" : "", w.x, w.z,
             reachReplans_, reachPath_.size());
    }
    if (++reachLog_ % 600 == 0)
        LOGI("reach walk: %zu waypoints left, at (%.0f %.0f %.0f)", reachPath_.size(), player_.position.x, player_.position.y, player_.position.z);
    const Vec3 aim = reachPath_[std::min<size_t>(reachPath_.size() - 1, 1)] - p;
    viewYaw = std::atan2(-aim.x, -aim.z);
    move = Vec2{0, 1};
    return true;
}

}  // namespace gvr
