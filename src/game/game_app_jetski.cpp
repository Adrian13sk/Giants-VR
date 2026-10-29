// GameApp: the Reaperski - riding it, the races, rivals, pickups. Same class; see game_app.h
// ("Reaperski") and jetski.h.
#include "game/game_app.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>

#include "audio/mixer.h"
#include "core/log.h"
#include "game/destruction.h"
#include "game/player.h"
#include "scene/asset_world.h"
#include "scene/model_cache.h"
#include "scene/skinned_model.h"

namespace gvr {

namespace {
Mat4 skiXf(const Vec3& pos, float yaw, float bank, float bob) {
    const float heave = 0.06f * std::sin(bob * 2.3f), pitch = 0.03f * std::sin(bob * 1.7f);
    return Mat4::translation(pos + Vec3{0, heave, 0}) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, yaw)) *
           Mat4::rotation(Quat::fromAxisAngle({0, 0, 1}, bank)) * Mat4::rotation(Quat::fromAxisAngle({1, 0, 0}, pitch));
}
}  // namespace

void GameApp::initJetski() {
    riding_ = raceMode_ = skiParked_ = false;
    race_ = RaceDirector{};
    rivalSki_.clear();
    rivalRider_.clear();
    skiPickups_.clear();
    skiMissiles_ = 0;
    skiTurbo_ = 0;
    raceObjective_ = -1;
    raceEndT_ = -1;
    raceWon_ = false;
    raceMsg_.clear();
    raceMsgT_ = 0;
    if (skiLoop_ && audio_) { audio_->stop(skiLoop_); skiLoop_ = 0; skiLoopKind_ = -1; }
    if (!assetWorld_) return;
    AssetWorld* w = assetWorld_.get();
    const MissionData& m = w->mission();
    const ObjectRegistry& reg = w->registry();
    std::vector<Vec3> gates;
    std::map<int, std::vector<Vec3>> lines;
    int rivals = 0;
    Vec3 finish;
    bool haveFinish = false, parked = false;
    Vec3 parkPos;
    float parkYaw = 0;
    for (const auto& o : m.objects) {
        const std::string n = toLower(reg.modelName(o.type));
        Vec3 p = giantsToRuntime(o.x, o.y, o.z);
        if (n == "buoy_spline") { p.y = 0; gates.push_back(p); }
        else if (n == "marker_spline") { p.y = 0; lines[o.group].push_back(p); }
        else if (n == "r3_start_finish") { finish = Vec3{p.x, 0, p.z}; haveFinish = true; }
        else if (n == "rpman_l0" && o.group == 11) ++rivals;
        else if (n == "p_jetski_turbo" || n == "p_jetski_missile" || n == "p_jetski_homing")
            skiPickups_.push_back({Vec3{p.x, 0.8f, p.z}, n == "p_jetski_turbo" ? 0 : (n == "p_jetski_missile" ? 1 : 2)});
        else if (n == "rp_jetski" && !parked) { parked = true; parkPos = Vec3{p.x, 0, p.z}; parkYaw = o.yaw * kPi / 180.0f; }
    }
    if (gates.empty() && !parked) return;
    if (hero_ != Hero::Delphi) return;  // only Delphi rides
    ModelCache* mc = w->models();
    if (!skiModel_) {
        auto sm = std::make_unique<SkinnedModel>();
        if (sm->load(w->fs(), *mc, renderer_, "RP_jetski", "RP_jetski.anm")) { sm->pose(renderer_, 0.0f); skiModel_ = std::move(sm); }
    }
    if (!skiModel_) { LOGW("Reaperski model unavailable"); return; }
    skiPickupModel_[0] = mc->get("P_jetski_turbo.gbs", "", true);
    skiPickupModel_[1] = mc->get("P_jetski_missile.gbs", "", true);
    skiPickupModel_[2] = mc->get("P_jetski_homing.gbs", "", true);
    if (playerModel_ && rideClip_[0] < 0) {
        static const char* rc[3] = {"rp_Jetski_ride.anm", "rp_jetski_L_turn.anm", "rp_jetski_R_turn.anm"};
        for (int k = 0; k < 3; ++k) rideClip_[k] = playerModel_->addAnimation(w->fs(), rc[k]);
        if (rideClip_[0] < 0) rideClip_[0] = 0;
        for (int k = 1; k < 3; ++k) if (rideClip_[k] < 0) rideClip_[k] = rideClip_[0];
    }
    // The parked Reaperski of the data is ours to draw (and ride).
    for (auto& o : w->worldObjects())
        if (o.name == "rp_jetski") o.hidden = true;
    if (!gates.empty()) {
        raceMode_ = true;
        std::vector<std::vector<Vec3>> paths;
        {
            // Driving order of the gates (see orderGates): water distance, land crossings dear.
            auto groundAt0 = [w](float x, float z) { float g = -20.0f; w->terrain().heightAt(x, z, g); return g; };
            auto cost = [&groundAt0](const Vec3& a2, const Vec3& b2) {
                const float l = length(Vec3{b2.x - a2.x, 0, b2.z - a2.z});
                const int n = std::max(1, int(l / 8.0f));
                int land = 0;
                for (int k = 0; k <= n; ++k) {
                    const float t = float(k) / float(n);
                    land += groundAt0(a2.x + (b2.x - a2.x) * t, a2.z + (b2.z - a2.z) * t) > 0.0f;
                }
                return l * (1.0f + 6.0f * float(land) / float(n + 1));
            };
            const auto ordered = orderGates(gates, haveFinish ? finish : gates.front(), cost);
            size_t moved = 0;
            for (size_t k = 0; k < gates.size(); ++k) moved += length(ordered[k] - gates[k]) > 1.0f;
            LOGI("Reaperski course: %zu of %zu gates reordered", moved, gates.size());
            gates = ordered;
        }
        // The rivals' lines. The data's marker_spline points (ids 5 and up) are sparse and not all
        // in driving order, so each rival instead runs gate to gate (its own side of each buoy)
        // along the water, routed round the headlands (ours).
        auto groundAt = [w](float x, float z) { float g = -20.0f; w->terrain().heightAt(x, z, g); return g; };
        int nLines = 0;
        for (auto& l : lines) nLines += l.first >= 5;
        if (rivals == 0) rivals = std::max(1, nLines);
        for (int k = 0; k < rivals; ++k) {
            const float side = (float(k) - 0.5f * float(rivals - 1)) * 5.0f;
            std::vector<Vec3> stops;
            if (haveFinish) stops.push_back(finish);
            for (size_t g = 0; g < gates.size(); ++g) {
                const Vec3 prev = g == 0 ? (haveFinish ? finish : gates[g]) : gates[g - 1];
                const Vec3 next = g + 1 < gates.size() ? gates[g + 1] : (haveFinish ? finish : gates[g]);
                Vec3 dir = next - prev;
                dir.y = 0;
                dir = normalize(dir + Vec3{1e-4f, 0, 0});
                Vec3 p = gates[g] + Vec3{dir.z, 0, -dir.x} * side;
                if (groundAt(p.x, p.z) > -0.4f) p = gates[g];
                stops.push_back(p);
            }
            if (haveFinish) stops.push_back(finish);
            std::vector<Vec3> path{stops.front()};
            for (size_t i = 0; i + 1 < stops.size(); ++i) {
                auto leg = waterRoute(stops[i], stops[i + 1], groundAt, -0.4f);
                if (leg.size() == 2) leg = waterRoute(stops[i], stops[i + 1], groundAt, -0.4f, 2.0f, 300.0f);  // narrow channels
                if (leg.size() == 2) leg = waterRoute(stops[i], stops[i + 1], groundAt, 0.6f, 2.0f, 300.0f);   // through the shallows
                if (leg.size() == 2) leg = waterRoute(stops[i], stops[i + 1], groundAt, 0.6f, 8.0f, 1400.0f);  // the long way round
                if (leg.size() == 2 && k == 0 && std::getenv("GVR_SKI_AUTOPILOT")) {
                    float hmax = -1e9f;
                    for (int j = 0; j <= 100; ++j) {
                        const Vec3 q = stops[i] + (stops[i + 1] - stops[i]) * (float(j) / 100.0f);
                        hmax = std::max(hmax, groundAt(q.x, q.z));
                    }
                    if (hmax > 0.6f)
                        LOGI("route leg %zu: (%.0f,%.0f)->(%.0f,%.0f) no water route, land up to %.1f m", i, -stops[i].x / gGameUnitsToMeters,
                             stops[i].z / gGameUnitsToMeters, -stops[i + 1].x / gGameUnitsToMeters, stops[i + 1].z / gGameUnitsToMeters, hmax);
                }
                path.insert(path.end(), leg.begin() + 1, leg.end());
            }
            paths.push_back(path);
        }
        if (const char* dump = std::getenv("GVR_DUMP_RACEMAP")) {
            // Debug: land (grey), water (blue), gates (red), raw line points (by group colour), as PPM.
            const Vec3 mn = w->terrain().boundsMin(), mx = w->terrain().boundsMax();
            const int W = 600, H = int(600.0f * (mx.z - mn.z) / std::max(1.0f, mx.x - mn.x));
            std::vector<uint8_t> img(size_t(W) * size_t(H) * 3);
            auto px = [&](const Vec3& q, int& x, int& y) {
                x = int((q.x - mn.x) / (mx.x - mn.x) * float(W)); y = int((q.z - mn.z) / (mx.z - mn.z) * float(H));
                return x >= 0 && y >= 0 && x < W && y < H;
            };
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    float g = -20;
                    w->terrain().heightAt(mn.x + (float(x) + 0.5f) / float(W) * (mx.x - mn.x), mn.z + (float(y) + 0.5f) / float(H) * (mx.z - mn.z), g);
                    uint8_t* c = &img[(size_t(y) * size_t(W) + size_t(x)) * 3];
                    if (g > 0) { const uint8_t v = uint8_t(std::min(255.0f, 90.0f + g * 2.0f)); c[0] = c[1] = c[2] = v; }
                    else { c[0] = 20; c[1] = 40; c[2] = 120; }
                }
            auto dot = [&](const Vec3& q, uint8_t r, uint8_t gg, uint8_t b, int rad) {
                int x, y;
                if (!px(q, x, y)) return;
                for (int dy = -rad; dy <= rad; ++dy)
                    for (int dx = -rad; dx <= rad; ++dx) {
                        const int xx = x + dx, yy = y + dy;
                        if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
                        uint8_t* c = &img[(size_t(yy) * size_t(W) + size_t(xx)) * 3];
                        c[0] = r; c[1] = gg; c[2] = b;
                    }
            };
            static const uint8_t cols[7][3] = {{255, 255, 0}, {0, 255, 0}, {255, 0, 255}, {0, 255, 255}, {255, 128, 0}, {255, 255, 255}, {128, 255, 128}};
            int gi = 0;
            for (auto& l : lines) {
                if (l.first < 5) continue;
                for (const Vec3& q : l.second) dot(q, cols[gi % 7][0], cols[gi % 7][1], cols[gi % 7][2], 1);
                ++gi;
            }
            if (!paths.empty())
                for (size_t i = 0; i + 1 < paths[0].size(); ++i)
                    for (int k = 0; k <= 40; ++k) dot(paths[0][i] + (paths[0][i + 1] - paths[0][i]) * (float(k) / 40.0f), 255, 255, 255, 0);
            for (size_t gi2 = 0; gi2 < gates.size(); ++gi2) dot(gates[gi2], uint8_t(255), uint8_t(20 * gi2), 0, 3);
            if (const char* sp = std::getenv("GVR_DUMP_RACEMAP_AT")) {
                float sx = 0, sz = 0;
                if (std::sscanf(sp, "%f,%f", &sx, &sz) == 2) dot(Vec3{sx, 0, sz}, 0, 255, 0, 5);
            }
            if (haveFinish) dot(finish, 255, 255, 255, 4);
            if (FILE* f = std::fopen(dump, "wb")) {
                std::fprintf(f, "P6 %d %d 255\n", W, H);
                std::fwrite(img.data(), 1, img.size(), f);
                std::fclose(f);
            }
        }
        if (std::getenv("GVR_SKI_AUTOPILOT"))
            for (size_t k = 0; k < paths.size(); ++k) {
                float hmax = -1e9f;
                Vec3 at;
                for (size_t i = 0; i + 1 < paths[k].size(); ++i) {
                    const Vec3 a2 = paths[k][i], d = paths[k][i + 1] - a2;
                    const int n = std::max(1, int(length(d) / 2.0f));
                    for (int j = 0; j <= n; ++j) {
                        const Vec3 q = a2 + d * (float(j) / float(n));
                        float g = -20.0f;
                        w->terrain().heightAt(q.x, q.z, g);
                        if (g > hmax) { hmax = g; at = q; }
                    }
                }
                LOGI("racing line %zu: %zu points, highest ground %.2f m at game (%.0f, %.0f), sea %.2f", k, paths[k].size(), hmax,
                     -at.x / gGameUnitsToMeters, at.z / gGameUnitsToMeters, player_.seaLevel);
            }
        race_.setup(gates, paths, rivals, haveFinish ? &finish : nullptr);
        for (size_t k = 0; k < race_.racers.size(); ++k) {
            auto s = std::make_unique<SkinnedModel>();
            auto r = std::make_unique<SkinnedModel>();
            if (!s->load(w->fs(), *mc, renderer_, "RPm_jetski", "RPm_jetski.anm") ||
                !r->load(w->fs(), *mc, renderer_, "rpman_l0", "rpm_jetski_ride.anm")) {
                race_.racers.resize(k);
                break;
            }
            s->pose(renderer_, 0.0f);
            r->pose(renderer_, 0.0f);
            rivalSki_.push_back(std::move(s));
            rivalRider_.push_back(std::move(r));
        }
        raceObjective_ = int(objectives_.size());
        objectives_.push_back({6, finish, finish});
        // Start on the start / finish line (beside the rivals' lines), facing the first gate.
        Vec3 start = haveFinish ? finish : assetWorld_->spawnPoint();
        start.y = 0;
        Vec3 to = gates.front() - start;
        if (haveFinish) {
            const Vec3 d = normalize(Vec3{to.x, 0, to.z} + Vec3{1e-4f, 0, 0});
            start = start + Vec3{d.z, 0, -d.x} * 8.0f;  // a little to the side of the line
            to = gates.front() - start;
        }
        mountJetski(start, std::atan2(-to.x, -to.z));
        LOGI("Reaperski race: %zu gates, %zu rivals, %zu pickups, finish line %s", gates.size(), race_.racers.size(), skiPickups_.size(),
             haveFinish ? "yes" : "no");
    } else {
        skiParked_ = true;
        ski_ = JetskiState{};
        ski_.pos = parkPos;
        ski_.yaw = parkYaw;
        LOGI("A Reaperski is moored at game (%.0f, %.0f)", -parkPos.x / gGameUnitsToMeters, parkPos.z / gGameUnitsToMeters);
    }
}

void GameApp::mountJetski(const Vec3& posIn, float yawIn) {
    // (Copies first: the moored ski mounts with its own ski_.pos / ski_.yaw, which the reset below clears -
    // until 2026-10-28 that put Delphi and the ski at the world's origin, (0, 0), as she got on.)
    const Vec3 pos = posIn;
    const float yaw = yawIn;
    riding_ = true;
    skiParked_ = false;
    ski_ = JetskiState{};
    ski_.pos = Vec3{pos.x, 0.0f, pos.z};  // the sea (surface at 0)
    ski_.yaw = yaw;
    camYaw_ = yaw;
    player_.position = ski_.pos;
    player_.velocity = Vec3{};
    player_.yaw = yaw;
    sfx2D("js_ron", 0.9f);
    LOGI("Delphi rides the Reaperski");
}

void GameApp::updateRiding(float dt, const InputFrame& in) {
    const int mh = rig_.moveHand(), th = rig_.turnHand();
    const bool canGo = !raceMode_ || (race_.started() && raceEndT_ < 0);
    float throttle = canGo ? in.hand[mh].thumbstick.y : 0.0f;
    float steer = canGo ? in.hand[mh].thumbstick.x : 0.0f;
    // Rework: handlebars (first person) - both grips squeezed take hold of the bars: turn them as a
    // handlebar (the right grip pulled back steers right) and twist the throttle hand back (its
    // knuckles toward you) to go; the sticks still work. Turbo is then A.
    const bool bars = firstPerson_ && in.hand[0].gripPoseValid && in.hand[1].gripPoseValid && in.hand[0].squeeze > 0.6f && in.hand[1].squeeze > 0.6f;
    if (bars) {
        const Vec3 L = in.hand[kLeft].gripPose.position, R = in.hand[kRight].gripPose.position;  // (tracking space)
        const float a = std::atan2(R.z - L.z, R.x - L.x);
        const Vec3 tf = in.hand[th].gripPose.orientation.rotate(Vec3{0, 0, -1});
        const float pitch = std::asin(std::max(-1.0f, std::min(1.0f, tf.y)));
        if (!skiBars_) {
            skiBars_ = true;
            skiBarYaw0_ = a;
            skiBarPitch0_ = pitch;
            haptics_.push_back({kLeft, 0.4f, 0.05f});
            haptics_.push_back({kRight, 0.4f, 0.05f});
            LOGI("Handlebars taken");
        }
        if (canGo) {
            const float bs = std::max(-1.0f, std::min(1.0f, angleDelta(skiBarYaw0_, a) / 0.45f));
            const float tw = std::max(-0.5f, std::min(1.0f, (pitch - skiBarPitch0_) / 0.55f));
            if (std::fabs(bs) > std::fabs(steer)) steer = bs;
            if (std::fabs(tw) > std::fabs(throttle)) throttle = tw;
        }
        static const bool trace = std::getenv("GVR_SKI_TRACE") != nullptr;
        static int tk = 0;
        if (trace && ++tk % 36 == 0) LOGI("ski trace: bars steer %.2f throttle %.2f speed %.1f", steer, throttle, ski_.speed);
    } else {
        skiBars_ = false;
    }
    if (std::getenv("GVR_SKI_AUTOPILOT") && canGo && raceMode_ && race_.nextGate <= int(race_.gates.size())) {
        // Test hook: full throttle along the first rival's racing line (25 m ahead of the nearest
        // point), else straight for the next gate.
        Vec3 tgt = race_.nextGate < int(race_.gates.size()) ? race_.gates[size_t(race_.nextGate)] : race_.finishLine;
        if (!race_.racers.empty()) {
            // Nearest point of the line (searching forward from the last one), then 15 m on.
            static float sNear = 0;
            const auto& path = race_.racers[race_.racers.size() / 2].path;  // the middle line: through the gates' centres
            float run = 0, best = 1e30f, bestS = sNear;
            for (size_t i = 0; i + 1 < path.size(); ++i) {
                const Vec3 a2 = path[i], d2 = path[i + 1] - a2;
                const float l = std::max(1e-3f, length(Vec3{d2.x, 0, d2.z}));
                if (run + l >= sNear - 5.0f && run <= sNear + 120.0f) {
                    const float t = std::max(0.0f, std::min(1.0f, ((ski_.pos.x - a2.x) * d2.x + (ski_.pos.z - a2.z) * d2.z) / (l * l)));
                    const float dd = length(Vec3{a2.x + d2.x * t - ski_.pos.x, 0, a2.z + d2.z * t - ski_.pos.z});
                    if (dd < best) { best = dd; bestS = run + t * l; }
                }
                run += l;
            }
            sNear = std::max(sNear, bestS);
            static int tick = 0;
            if (++tick % 600 == 0)
                LOGI("autopilot: s %.0f of %.0f, off the line %.1f m, speed %.1f, finish %.0f m away", sNear, run, best, ski_.speed,
                     length(Vec3{race_.finishLine.x - ski_.pos.x, 0, race_.finishLine.z - ski_.pos.z}));
            float s2 = sNear + 15.0f;
            tgt = path.back();
            for (size_t i = 0; i + 1 < path.size(); ++i) {
                const float l = length(path[i + 1] - path[i]);
                if (s2 <= l) { tgt = path[i] + (path[i + 1] - path[i]) * (s2 / std::max(1e-3f, l)); break; }
                s2 -= l;
            }
        }
        const Vec3 d = tgt - ski_.pos;
        steer = std::max(-1.0f, std::min(1.0f, -angleDelta(ski_.yaw, std::atan2(-d.x, -d.z)) * 2.0f));
        throttle = 1.0f;
        static float backUp = 0, stuckT = 0;
        backUp = std::max(0.0f, backUp - dt);
        // Stuck (under 1 m/s for a second while driving forward): back off 0.8 s, turned. (Was: any
        // moment under 1 m/s - which the way back to forward speed after backing off passes through,
        // so it reversed again and again, 1 km backwards on rstory3_race_3 after a bump.)
        stuckT = ski_.speed < 1.0f && race_.time > 2.0f && backUp <= 0 ? stuckT + dt : 0.0f;
        if (stuckT > 1.0f) { backUp = 1.2f; stuckT = 0; }
        if (backUp > 0.4f) { throttle = -1.0f; steer = -steer; }
    }
    if (std::getenv("GVR_SKI_AUTOPILOT") && canGo && !raceMode_ && assetWorld_) {
        // (2026-10-28) Test hook, the moored-Reaperski missions (rstory2_jetski_2/3): full throttle along a water
        // route to the first unfinished "reach" objective (planned as the rivals' lines are, round the headlands).
        int obj = -1;
        for (size_t i = 0; i < objectives_.size() && obj < 0; ++i)
            if (objectives_[i].kind == 1 && !objectives_[i].done) obj = int(i);
        if (obj >= 0) {
            const Vec3 goal = (objectives_[size_t(obj)].boundsMin + objectives_[size_t(obj)].boundsMax) * 0.5f;
            if (obj != skiRouteObj_) {
                skiRouteObj_ = obj;
                AssetWorld* w = assetWorld_.get();
                auto groundAt = [w](float x, float z) { float g = -20.0f; w->terrain().heightAt(x, z, g); return g; };
                // The moored ski (in the shallows by a pier) and the objective (perhaps ashore) may not be in open
                // water: route between the nearest open water to each, then on in.
                auto openWater = [&](Vec3 p) {
                    if (groundAt(p.x, p.z) <= -0.8f) return p;
                    for (float r = 5.0f; r <= 250.0f; r += 5.0f)
                        for (int k = 0; k < 24; ++k) {
                            const float an = float(k) * 0.2618f;
                            const Vec3 q{p.x + std::cos(an) * r, 0, p.z + std::sin(an) * r};
                            if (groundAt(q.x, q.z) < -0.8f) return q;
                        }
                    return p;
                };
                const Vec3 a = openWater(Vec3{ski_.pos.x, 0, ski_.pos.z});
                const Vec3 b = openWater(Vec3{goal.x, 0, goal.z});
                LOGI("autopilot: ski ground %.1f, goal ground %.1f; open water %.0f m and %.0f m from them", groundAt(ski_.pos.x, ski_.pos.z),
                     groundAt(goal.x, goal.z), length(a - Vec3{ski_.pos.x, 0, ski_.pos.z}), length(b - Vec3{goal.x, 0, goal.z}));
                skiRoute_ = waterRoute(a, b, groundAt, -0.4f);
                if (skiRoute_.size() == 2) skiRoute_ = waterRoute(a, b, groundAt, -0.4f, 2.0f, 300.0f);
                if (skiRoute_.size() == 2) skiRoute_ = waterRoute(a, b, groundAt, 0.6f, 2.0f, 300.0f);
                if (skiRoute_.size() == 2) skiRoute_ = waterRoute(a, b, groundAt, 0.6f, 8.0f, 1400.0f);
                if (skiRoute_.size() == 2) skiRoute_ = waterRoute(a, b, groundAt, 1.2f, 4.0f, 800.0f);  // (over the sandbars it hops)
                skiRouteAt_ = 1;
                skiRoute_.push_back(Vec3{goal.x, 0, goal.z});  // (the last stretch: in to the objective itself)
                LOGI("autopilot: a route of %zu points to objective %d, %.0f m straight", skiRoute_.size(), obj, length(b - a));
                for (const Vec3& q : skiRoute_) LOGI("autopilot:   (%.1f %.1f)", q.x, q.z);
            }
            while (skiRouteAt_ + 1 < skiRoute_.size() && length(Vec3{skiRoute_[skiRouteAt_].x - ski_.pos.x, 0, skiRoute_[skiRouteAt_].z - ski_.pos.z}) < 8.0f)
                ++skiRouteAt_;
            const Vec3 tgt = skiRouteAt_ < skiRoute_.size() ? skiRoute_[skiRouteAt_] : goal;
            const Vec3 d = tgt - ski_.pos;
            steer = std::max(-1.0f, std::min(1.0f, -angleDelta(ski_.yaw, std::atan2(-d.x, -d.z)) * 2.0f));
            throttle = 1.0f;
            static float backUp2 = 0, stuck2 = 0;
            backUp2 = std::max(0.0f, backUp2 - dt);
            stuck2 = ski_.speed < 1.0f && backUp2 <= 0 ? stuck2 + dt : 0.0f;
            if (stuck2 > 1.0f) { backUp2 = 1.2f; stuck2 = 0; }
            if (backUp2 > 0.4f) { throttle = -1.0f; steer = -steer; }
            static int tick = 0;
            if (++tick % 600 == 0)
                LOGI("autopilot: point %zu of %zu, %.0f m to the objective, speed %.1f", skiRouteAt_, skiRoute_.size(),
                     length(Vec3{goal.x - ski_.pos.x, 0, goal.z - ski_.pos.z}), ski_.speed);
        }
    }
    if (std::fabs(throttle) < 0.15f) throttle = 0;
    if (std::fabs(steer) < 0.15f) steer = 0;
    const bool turbo = canGo && (bars ? in.hand[th].primary : in.hand[mh].squeeze > 0.5f) && skiTurbo_ > 0;
    if (turbo) {
        if (skiTurbo_ > 0 && skiTurbo_ - dt <= 0) sfx2D("js_rrvd1", 0.7f);
        skiTurbo_ = std::max(0.0f, skiTurbo_ - dt);
    }
    // Something solid ahead (an ice wall, a pier): rammed through on turbo if it breaks, else a bump.
    if (worldRay_ && std::fabs(ski_.speed) > 0.5f) {
        const Vec3 fwd{-std::sin(ski_.yaw), 0, -std::cos(ski_.yaw)};
        const Vec3 dir = ski_.speed > 0 ? fwd : fwd * -1.0f;
        float t;
        Vec3 n;
        // (Objects only: the terrain is the ski's own business - stepJetski.)
        if (worldRay_(ski_.pos + Vec3{0, 0.6f, 0}, dir, std::fabs(ski_.speed) * dt + 2.2f, t, n) && lastWorldTag_ >= 0) {
            const int item = lastWorldTag_ >= 0 && size_t(lastWorldTag_) < objectItem_.size() ? objectItem_[size_t(lastWorldTag_)] : -1;
            const std::string& hitName = size_t(lastWorldTag_) < assetWorld_->worldObjects().size()
                                             ? assetWorld_->worldObjects()[size_t(lastWorldTag_)].name : std::string();
            if (hitName == "r3_start_finish" || hitName == "buoy_spline") {
                // The start / finish arch and the gate buoys: ridden through, not into (a buoy
                // clipped at speed just bobs aside).
            } else if (turbo && item >= 0 && destruct_.items[size_t(item)].alive()) {
                damageObject(item, 600.0f, true, ski_.pos);
                ski_.speed *= 0.7f;
                haptics_.push_back({kLeft, 1.0f, 0.2f});
                haptics_.push_back({kRight, 1.0f, 0.2f});
            } else {
                if (std::getenv("GVR_SKI_AUTOPILOT")) {
                    static int nlog = 0;
                    if (nlog++ < 5) LOGI("ski hits an object (tag %d, %s) at %.0f %.0f", lastWorldTag_,
                                         lastWorldTag_ >= 0 && size_t(lastWorldTag_) < assetWorld_->worldObjects().size() ? assetWorld_->worldObjects()[size_t(lastWorldTag_)].name.c_str() : "?",
                                         -ski_.pos.x / gGameUnitsToMeters, ski_.pos.z / gGameUnitsToMeters);
                }
                ski_.speed = -0.3f * ski_.speed;
                sfx2D(frand() < 0.5f ? "js_bump1" : "js_bump2", 0.9f);
                haptics_.push_back({kLeft, 0.7f, 0.12f});
                haptics_.push_back({kRight, 0.7f, 0.12f});
            }
        }
    }
    const bool bumped = stepJetski(ski_, skiParams_, throttle, steer, turbo, dt, player_.groundHeight, player_.seaLevel);
    if (bumped) {
        if (std::getenv("GVR_SKI_AUTOPILOT")) {
            const Vec3 f = ski_.pos + Vec3{-std::sin(ski_.yaw), 0, -std::cos(ski_.yaw)} * 1.0f;
            LOGI("ski bump at %.1f %.1f: ground %.2f ahead %.2f", ski_.pos.x, ski_.pos.z, player_.groundHeight(ski_.pos.x, ski_.pos.z),
                 player_.groundHeight(f.x, f.z));
        }
        sfx2D(frand() < 0.5f ? "js_bump1" : "js_bump2", 0.9f);
        haptics_.push_back({kLeft, 0.7f, 0.12f});
        haptics_.push_back({kRight, 0.7f, 0.12f});
    }
    // Spray behind at speed.
    skiSplashT_ -= dt;
    if (std::fabs(ski_.speed) > 6.0f && skiSplashT_ <= 0) {
        skiSplashT_ = 0.12f;
        const Vec3 back = ski_.pos + Vec3{std::sin(ski_.yaw), 0, std::cos(ski_.yaw)} * 2.0f;
        waterFx_.push_back({Vec3{back.x, 0.02f, back.z}, 0.0f, 1, 0.6f + 0.03f * std::fabs(ski_.speed)});
    }
    // The rider: the player is the ski (camera, enemies' aim); Delphi rides it.
    player_.position = ski_.pos;
    player_.velocity = Vec3{-std::sin(ski_.yaw), 0, -std::cos(ski_.yaw)} * ski_.speed;
    player_.yaw = ski_.yaw;
    player_.onGround = true;
    // The view turns with the ski (smoothly).
    camYaw_ += angleDelta(camYaw_, ski_.yaw) * std::min(1.0f, dt * 6.0f);
    placeCamera(dt, false, &in);
    bodyXf_ = skiXf(ski_.pos, ski_.yaw, ski_.bank, ski_.bob);
    if (playerModel_) {
        playerModel_->setHideEnabled(false);
        const int c = steer < -0.3f ? rideClip_[1] : (steer > 0.3f ? rideClip_[2] : rideClip_[0]);
        const float len = std::max(0.1f, playerModel_->animDuration(c));
        playerModel_->poseBlend(renderer_, c, std::fmod(ski_.bob, len), c, std::fmod(ski_.bob, len), 0.0f);
    }
    // Missiles (pickups): the turn hand's trigger fires one ahead.
    skiFireCool_ = std::max(0.0f, skiFireCool_ - dt);
    const bool trig = in.hand[th].trigger > 0.6f;
    if (trig && !skiTrigWas_ && skiMissiles_ > 0 && skiFireCool_ <= 0) {
        --skiMissiles_;
        skiFireCool_ = 0.6f;
        Rocket r;
        const Vec3 fwd{-std::sin(ski_.yaw), 0, -std::cos(ski_.yaw)};
        r.pos = ski_.pos + fwd * 3.0f + Vec3{0, 1.0f, 0};
        r.vel = fwd * 50.0f + Vec3{0, 0.5f, 0};
        r.life = 0;
        rockets_.push_back(r);
        sfx2D("me_msls1", 0.9f);
    }
    skiTrigWas_ = trig;
    // Y: off the ski (not in a race). It stays moored where she leaves it.
    const bool y = in.hand[mh].secondary;
    if (y && !skiYWas_ && !raceMode_ && std::fabs(ski_.speed) < 4.0f) {
        riding_ = false;
        skiParked_ = true;
        ski_.speed = 0;
        const Vec3 side{std::cos(ski_.yaw), 0, -std::sin(ski_.yaw)};
        player_.teleport(ski_.pos + side * 2.2f + Vec3{0, 0.5f, 0}, ski_.yaw);
        sfx2D("js_roff", 0.8f);
        LOGI("Delphi leaves the Reaperski");
    }
    skiYWas_ = y;
}

void GameApp::updateJetskiWorld(float dt, const InputFrame& in) {
    (void)in;
    if (!skiModel_ && !raceMode_) return;
    // Walking into the moored Reaperski: on it. (Test hook GVR_SKI_AUTOPILOT: the autopilot gets on at once.)
    static const bool autopilot = std::getenv("GVR_SKI_AUTOPILOT") != nullptr;
    if (skiParked_ && !riding_ && hero_ == Hero::Delphi && playMode_ && !dying_ &&
        (autopilot || length(Vec3{player_.position.x - ski_.pos.x, 0, player_.position.z - ski_.pos.z}) < 2.6f))
        mountJetski(ski_.pos, ski_.yaw);
    if (skiParked_) { ski_.bob += dt; ski_.bank *= 0.95f; }
    if (skiModel_ && (onSki() || skiParked_)) skiModel_->pose(renderer_, std::fmod(ski_.bob, std::max(0.1f, skiModel_->animDuration())));
    // Engine: idle and running loops.
    if (audio_) {
        const int want = !onSki() ? -1 : (std::fabs(ski_.speed) > 4.0f ? 1 : 0);
        if (want != skiLoopKind_) {
            if (skiLoop_) audio_->stop(skiLoop_);
            skiLoop_ = want < 0 ? 0 : audio_->play2D(want == 1 ? "js_rmve1" : "js_ridl1", want == 1 ? 0.55f : 0.4f, 1.0f, true);
            if (want == 1 && skiLoopKind_ == 0) sfx2D("js_rrvu1", 0.6f);
            skiLoopKind_ = want;
        }
    }
    // Pickups (respawn after a while): turbo, missiles, homing missiles.
    for (auto& p : skiPickups_) {
        p.gone = std::max(0.0f, p.gone - dt);
        if (p.gone > 0 || !onSki()) continue;
        if (length(Vec3{p.pos.x - ski_.pos.x, 0, p.pos.z - ski_.pos.z}) < 4.0f) {
            p.gone = 20.0f;
            if (p.kind == 0) skiTurbo_ = std::min(12.0f, skiTurbo_ + 4.0f);
            else skiMissiles_ = std::min(9, skiMissiles_ + (p.kind == 1 ? 2 : 3));
            sfx2D(p.kind == 0 ? "js_tbact" : "pu_hlth", 0.9f);
            raceMsg_ = p.kind == 0 ? "TURBO! (HOLD GRIP)" : "MISSILES! (TRIGGER)";
            raceMsgT_ = 2.0f;
        }
    }
    raceMsgT_ = std::max(0.0f, raceMsgT_ - dt);
    if (!raceMode_) return;
    // --- The race.
    const bool wasCounting = !race_.started();
    const float before = race_.time;
    race_.update(dt, riding_ ? ski_.pos : player_.position);
    if (wasCounting) {
        const int a = int(std::ceil(-before)), b = int(std::ceil(-race_.time));
        if (b != a && b > 0) { raceMsg_ = std::to_string(b); raceMsgT_ = 0.9f; sfx2D("ph_selct", 0.8f, 0.8f); }
        if (race_.started()) { raceMsg_ = "GO!"; raceMsgT_ = 1.2f; sfx2D("js_start", 1.0f); }
    }
    for (int e : race_.gateEvents) {
        if (e > 0) { sfx2D("js_buoy", 0.8f); haptics_.push_back({kLeft, 0.3f, 0.05f}); LOGI("Gate %d of %zu (%.0f s)", race_.nextGate, race_.gates.size(), race_.time); }
        else {
            sfx2D("js_mbuoy", 0.9f);
            raceMsg_ = "MISSED A GATE! (" + std::to_string(race_.missed) + " OF " + std::to_string(race_.params.missLimit) + ")";
            raceMsgT_ = 2.5f;
        }
    }
    // Rivals hit by missiles: proximity fuse on the player's rockets.
    for (auto& r : rockets_) {
        if (r.life >= 1e9f || r.kind != 0) continue;
        for (auto& rv : race_.racers)
            if (!rv.finished && length(rv.pos + Vec3{0, 1, 0} - r.pos) < 4.0f) {
                explode(r.pos, 4.0f, 60.0f);
                r.life = 1e9f;
                break;
            }
    }
    for (size_t k = 0; k < race_.racers.size() && k < rivalSki_.size(); ++k) {
        const Racer& rv = race_.racers[k];
        if (length(rv.pos - player_.position) > 250.0f) continue;
        rivalSki_[k]->pose(renderer_, std::fmod(race_.time + float(k), std::max(0.1f, rivalSki_[k]->animDuration())));
        rivalRider_[k]->pose(renderer_, std::fmod(race_.time + float(k), std::max(0.1f, rivalRider_[k]->animDuration())));
    }
    // The end: first over the line wins the mission; anything else (or three missed gates) is a
    // lost race, run again.
    if (raceEndT_ < 0 && (race_.playerFinished || race_.lost)) {
        raceEndT_ = 0;
        raceWon_ = race_.playerFinished && race_.playerPlace == 1;
        if (raceWon_) {
            raceMsg_ = "YOU WIN!";
            sfx2D("js_win", 1.0f);
            if (raceObjective_ >= 0) completeObjective(raceObjective_);
        } else {
            static const char* nth[] = {"1ST", "2ND", "3RD", "4TH", "5TH", "6TH", "7TH", "8TH", "9TH"};
            raceMsg_ = race_.lost ? "THREE GATES MISSED - RACE LOST" : std::string("YOU FINISHED ") + nth[std::min(8, race_.playerPlace - 1)] + " - RACE LOST";
            sfx2D("js_lose", 1.0f);
            // The race missions name a second cutscene (op 0x13) beside their outro: "<race>_jetski_R_win",
            // the rival Reaper winning (the outro, "..._D_win", is Delphi's). Played on a lost race before
            // it is run again (V: the four race missions; which loss shows it in the original is native).
            const std::string& lose = assetWorld_->mission().flick13;
            if (!lose.empty()) startCutscene(lose);
        }
        raceMsgT_ = 5.0f;
        LOGI("Race over: %s (place %d, %d gates missed, %.1f s)", raceWon_ ? "won" : "lost", race_.playerPlace, race_.missed, race_.time);
    }
    if (raceEndT_ >= 0) {
        raceEndT_ += dt;
        if (!raceWon_ && raceEndT_ > 5.0f && load_.phase == LoadPhase::Idle && assetWorld_) {
            raceEndT_ = -1e9f;  // once
            beginLoading(assetWorld_->missionName(), lastInput_);  // run it again
        }
    }
}

void GameApp::renderJetski(bool shadow, const Vec3& eye) {
    if (skiModel_ && (onSki() || skiParked_)) {
        const Mat4 xf = skiXf(ski_.pos, ski_.yaw, ski_.bank, ski_.bob);
        if (shadow) skiModel_->renderShadow(renderer_, xf);
        else skiModel_->render(renderer_, xf);
    }
    for (size_t k = 0; k < race_.racers.size() && k < rivalSki_.size(); ++k) {
        const Racer& r = race_.racers[k];
        if (length(r.pos - eye) > 400.0f) continue;
        const Mat4 xf = skiXf(Vec3{r.pos.x, player_.seaLevel, r.pos.z}, r.yaw, r.bank, race_.time + float(k));
        if (shadow) { rivalSki_[k]->renderShadow(renderer_, xf); rivalRider_[k]->renderShadow(renderer_, xf); }
        else { rivalSki_[k]->render(renderer_, xf); rivalRider_[k]->render(renderer_, xf); }
    }
    if (shadow) return;
    for (const auto& p : skiPickups_) {
        if (p.gone > 0 || length(p.pos - eye) > 300.0f || !skiPickupModel_[p.kind]) continue;
        const float spin = float(time_) * 1.5f, hover = 0.25f * std::sin(float(time_) * 2.0f + p.pos.x);
        drawModel(skiPickupModel_[p.kind], Mat4::translation(p.pos + Vec3{0, hover, 0}) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, spin)), false);
    }
}

void GameApp::renderRaceHud() {
    if (!text_.ready() || !subInit_ || !playMode_ || cutsceneActive()) return;
    const Quat r = subRot_;
    const Vec3 head = rig_.toWorld(lastInput_.head).position;
    float w = 0, h = 0;
    if (raceMode_ && raceEndT_ < 0) {
        char line[96];
        std::snprintf(line, sizeof line, "POSITION %d / %zu    GATE %d / %zu    MISSED %d", race_.place(ski_.pos), race_.racers.size() + 1,
                      std::min(race_.nextGate + 1, int(race_.gates.size())), race_.gates.size(), race_.missed);
        if (const MeshId m = text_.mesh(line, 0.035f, 1.6f, &w, &h)) {
            const Mat4 xf = Mat4::translation(head + r.rotate(Vec3{0, 0.30f, -1.3f})) * Mat4::rotation(r * Quat::fromAxisAngle({1, 0, 0}, -0.2f));
            text_.drawPanel(xf, w + 0.06f, h + 0.04f, {0, 0, 0, 0.45f});
            text_.draw(m, xf * Mat4::translation({0, 0, 0.003f}), {1.0f, 0.9f, 0.5f, 1.0f});
        }
    }
    if (onSki() && (skiTurbo_ > 0 || skiMissiles_ > 0)) {
        char line[64];
        std::snprintf(line, sizeof line, "TURBO %.0f S    MISSILES %d", std::ceil(skiTurbo_), skiMissiles_);
        if (const MeshId m = text_.mesh(line, 0.03f, 1.2f, &w, &h)) {
            const Mat4 xf = Mat4::translation(head + r.rotate(Vec3{0, -0.36f, -1.3f})) * Mat4::rotation(r * Quat::fromAxisAngle({1, 0, 0}, 0.25f));
            text_.drawPanel(xf, w + 0.06f, h + 0.04f, {0, 0, 0, 0.45f});
            text_.draw(m, xf * Mat4::translation({0, 0, 0.003f}), {0.7f, 0.95f, 1.0f, 1.0f});
        }
    }
    if (raceMsgT_ > 0 && !raceMsg_.empty())
        if (const MeshId m = text_.mesh(raceMsg_, 0.09f, 1.6f, &w, &h)) {
            const float a = std::min(1.0f, raceMsgT_ / 0.3f);
            const Mat4 xf = Mat4::translation(head + r.rotate(Vec3{0, 0.08f, -1.5f})) * Mat4::rotation(r);
            text_.draw(m, xf, {1.0f, 0.85f, 0.3f, a});
        }
}

}  // namespace gvr
