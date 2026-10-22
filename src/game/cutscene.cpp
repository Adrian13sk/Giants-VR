#include "game/cutscene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "core/log.h"

namespace gvr {

Mat4 gameMatrixToRuntime(const Mat4& m) {
    const float s = gGameUnitsToMeters, is = 1.0f / s;
    Mat4 c = Mat4::identity(), ci = Mat4::identity();
    // runtime = s * (-x, z, y): columns are the images of the game axes.
    c.m[0] = -s; c.m[1] = 0; c.m[2] = 0;
    c.m[4] = 0; c.m[5] = 0; c.m[6] = s;
    c.m[8] = 0; c.m[9] = s; c.m[10] = 0;
    ci.m[0] = -is; ci.m[1] = 0; ci.m[2] = 0;
    ci.m[4] = 0; ci.m[5] = 0; ci.m[6] = is;
    ci.m[8] = 0; ci.m[9] = is; ci.m[10] = 0;
    return c * m * ci;
}

bool CutscenePlayer::start(const std::string& firstShot) {
    shotsPlayed_ = 0;
    streamStarted_.clear();
    active_ = true;
    active_ = loadShot(firstShot);
    return active_;
}

bool CutscenePlayer::loadShot(const std::string& name) {
    std::vector<uint8_t> bytes;
    std::string err;
    FlickShot s;
    if (!fs_.read("flk_" + name + ".bin", bytes) || !parseFlick(bytes.data(), bytes.size(), s, &err) || !s.hasDirector) {
        LOGW("Cutscene shot %s unavailable %s", name.c_str(), err.c_str());
        return false;
    }
    shot_ = std::move(s);
    shotName_ = name;
    t_ = 0;
    duration_ = std::max(0.5f, shot_.duration());
    actors_.clear();
    const AnmAnimation& dir = shot_.director;
    auto lower = [](std::string v) { return toLower(v); };
    // --- Which director node table performs which actor ---
    // Movers carry the actor's name in varying spellings ("Timmy_mover", "lander_Moover",
    // "mover_Baz", "Flack_mover" for actor "flak"); some are anonymous ("mover", "Moover").
    auto moverOwner = [&](const std::string& mover) {
        std::string l = lower(mover);
        for (const char* suf : {"_mover", "_moover"}) {
            const size_t k = std::string(suf).size();
            if (l.size() > k && l.compare(l.size() - k, k, suf) == 0) return l.substr(0, l.size() - k);
        }
        for (const char* pre : {"mover_", "moover_"}) {
            const size_t k = std::string(pre).size();
            if (l.size() > k && l.compare(0, k, pre) == 0) return l.substr(k);
        }
        return std::string();
    };
    auto similar = [](const std::string& x, const std::string& y) {
        if (x == y) return true;
        if (x.size() < 4 || y.size() < 4) return false;
        // Levenshtein distance <= 1.
        const std::string& a = x.size() <= y.size() ? x : y;
        const std::string& b = x.size() <= y.size() ? y : x;
        if (b.size() - a.size() > 1) return false;
        size_t i = 0, j = 0, edits = 0;
        while (i < a.size() && j < b.size()) {
            if (a[i] == b[j]) { ++i; ++j; continue; }
            if (++edits > 1) return false;
            if (a.size() == b.size()) ++i;
            ++j;
        }
        return edits + (b.size() - j) + (a.size() - i) <= 1;
    };
    struct Table { AnmAnimation anim; int mover = -1; std::string owner; bool camera = false; int root = -1; };
    std::vector<Table> tables(size_t(dir.tableCount()));
    for (int t = 0; t < dir.tableCount(); ++t) {
        Table& tb = tables[size_t(t)];
        tb.anim = dir.subset(t, &tb.mover);
        if (tb.mover >= 0) tb.owner = moverOwner(dir.nodes[size_t(tb.mover)].name);
        for (size_t n = 0; n < dir.nodes.size(); ++n) {
            if (dir.nodes[n].table != t) continue;
            if (lower(dir.nodes[n].name).rfind("camera", 0) == 0) tb.camera = true;
            if (tb.root < 0 && (dir.nodes[n].parent < 0 || dir.nodes[size_t(dir.nodes[n].parent)].table != t)) tb.root = int(n);
        }
    }
    std::vector<bool> claimed(tables.size(), false);
    // Loads an actor's model and finds its tree; false if it can't be shown in this shot.
    auto place = [&](const FlickActor& a, const std::string& model, std::string idleAnim, bool quiet) {
        Actor act;
        act.info = a;
        // Objects with no animation slot can still be skinned with their own <model>.anm (the
        // lander's parts: canopy, door, chairs, stick).
        if (idleAnim.empty() && fs_.exists(model + ".anm")) idleAnim = model + ".anm";
        std::unique_ptr<SkinnedModel> sm;
        if (fs_.exists(model + ".skn") && !idleAnim.empty()) {
            sm = std::make_unique<SkinnedModel>();
            if (!sm->load(fs_, models_, r_, model, idleAnim)) sm.reset();
        }
        const size_t needBones = sm ? std::max<size_t>(3, sm->boneCount() / 2) : 0;
        auto fits = [&](int t) { return !sm || sm->boneMatches(tables[size_t(t)].anim) >= needBones; };
        const std::string me = lower(a.node);
        int table = -1;
        // 1. A mover named after the actor.
        for (int t = 0; t < int(tables.size()) && table < 0; ++t)
            if (!claimed[size_t(t)] && !tables[size_t(t)].owner.empty() && similar(tables[size_t(t)].owner, me) && fits(t)) table = t;
        // 2. A node named after the actor (static props hang on it; skinned actors use its table).
        auto byName = dir.byName.find(me);
        if (table < 0 && byName != dir.byName.end()) {
            const int t = dir.nodes[size_t(byName->second)].table;
            if (!sm) { table = t; act.node = byName->second; }
            else if (!claimed[size_t(t)] && fits(t)) table = t;
        }
        // 3. Skinned: the unclaimed tree that best matches the skeleton (anonymous movers, e.g.
        //    "Moover" carrying the lander in story1_start_10).
        if (table < 0 && sm) {
            size_t best = needBones;
            for (int t = 0; t < int(tables.size()); ++t) {
                if (claimed[size_t(t)] || tables[size_t(t)].camera) continue;
                const size_t m = sm->boneMatches(tables[size_t(t)].anim);
                if (m >= best) { best = m + 1; table = t; }
            }
        }
        // 4. Static: an unclaimed, mover-less table without a camera.
        if (table < 0 && !sm)
            for (int t = 0; t < int(tables.size()) && table < 0; ++t)
                if (!claimed[size_t(t)] && !tables[size_t(t)].camera && tables[size_t(t)].mover < 0) { table = t; act.node = tables[size_t(t)].root; }
        if (table < 0) {
            if (!quiet) LOGI("Cutscene %s: actor %s not staged in this shot", name.c_str(), a.node.c_str());
            return false;
        }
        act.mover = tables[size_t(table)].mover;
        if (sm) {
            act.clip = sm->addAnimation(AnmAnimation(tables[size_t(table)].anim), shotName_ + ":" + a.node);
            act.skinned = std::move(sm);
        } else {
            act.model = models_.get(model + ".gbs", idleAnim);
            if (!act.model) return false;
        }
        claimed[size_t(table)] = true;
        act.table = table;
        LOGI("Cutscene %s: actor %s -> %s (%s, table %d)", name.c_str(), a.node.c_str(), model.c_str(),
             act.skinned ? "skinned" : "static", table);
        actors_.push_back(std::move(act));
        return true;
    };
    for (const auto& a : shot_.actors) {
        if (a.type < 0 || a.node.empty()) continue;
        const std::string model = reg_.modelName(uint32_t(a.type));
        if (model.empty()) { LOGW("Cutscene %s: actor %s (type %d) has no model", name.c_str(), a.node.c_str(), a.type); continue; }
        place(a, model, reg_.modelAnim(uint32_t(a.type)), false);
    }
    // Performances with no typed actor record: the Mecc crew (Baz, Gordon, ...) on their own
    // movers. Any remaining tree that fits the Meccaryn skeleton is shown as a Meccaryn.
    for (int t = 0; t < int(tables.size()); ++t) {
        if (claimed[size_t(t)] || tables[size_t(t)].camera || tables[size_t(t)].anim.nodes.size() < 30) continue;
        FlickActor a;
        a.node = tables[size_t(t)].owner.empty() ? "meccaryn" : tables[size_t(t)].owner;
        a.type = -2;
        place(a, "mc_l0", "mc_stand.anm", true);
    }
    // Viewpoint: the camera at the start of the shot, facing its target (or view axis).
    dir.evaluate(0.0f, world_);
    eye_ = {0, 0, 0};
    yaw_ = 0;
    auto cam = dir.byName.find(lower(shot_.cameraNode.empty() ? "camera01" : shot_.cameraNode));
    camNode_ = cam != dir.byName.end() ? cam->second : -1;
    hasEyeOverride_ = false;
    if (cam != dir.byName.end()) {
        const Mat4& m = world_[size_t(cam->second)];
        eye_ = giantsToRuntime(m.m[12], m.m[13], m.m[14]);
        // Target cameras (Max look-at controller; their rotation track is type 2, which carries
        // no keys) face their ".target" node; free cameras look down their local -Z.
        Vec3 fwd = giantsToRuntime(-m.m[8], -m.m[9], -m.m[10]);
        auto tgt = dir.byName.find(cam->first + ".target");
        if (tgt != dir.byName.end()) {
            const Mat4& tm = world_[size_t(tgt->second)];
            fwd = giantsToRuntime(tm.m[12], tm.m[13], tm.m[14]) - eye_;
        }
        if (fwd.x * fwd.x + fwd.z * fwd.z > 1e-6f) yaw_ = std::atan2(-fwd.x, -fwd.z);
    }
    // Test hook (headless inspection): GVR_CUT_EYE="gx gy gz yawdeg" overrides the viewpoint.
    if (const char* e = std::getenv("GVR_CUT_EYE")) {
        float gx = 0, gy = 0, gz = 0, yd = 0;
        if (std::sscanf(e, "%f %f %f %f", &gx, &gy, &gz, &yd) == 4) {
            eye_ = giantsToRuntime(gx, gy, gz);
            yaw_ = yd * 3.14159265f / 180.0f;
            hasEyeOverride_ = true;
        }
    }
    newShot_ = true;
    if (!shot_.stream.empty() && shot_.stream != streamStarted_) {
        pendingStream_ = shot_.stream;
        streamStarted_ = shot_.stream;
    }
    ++shotsPlayed_;
    LOGI("Cutscene shot %s: %.1f s, %zu actors, camera (%.1f %.1f %.1f)", name.c_str(), duration_, actors_.size(), eye_.x,
         eye_.y, eye_.z);
    update(0.0f);
    for (const auto& a : actors_)
        LOGI("  actor %s at (%.1f %.1f %.1f)", a.info.node.c_str(), a.xf.m[12], a.xf.m[13], a.xf.m[14]);
    return true;
}

void CutscenePlayer::update(float dt) {
    if (!active_) return;
    t_ += dt;
    if (t_ >= duration_) {
        const std::string next = shot_.next;
        if (next.empty() || !loadShot(next)) {
            active_ = false;
            actors_.clear();
            return;
        }
    }
    const float t = std::min(t_, duration_ * 0.999f);
    shot_.director.evaluate(t, world_);
    eyeNow_ = eye_;
    if (camNode_ >= 0 && !hasEyeOverride_) {
        const Mat4& m = world_[size_t(camNode_)];
        eyeNow_ = giantsToRuntime(m.m[12], m.m[13], m.m[14]);
    }
    for (auto& a : actors_) {
        Mat4 base = Mat4::identity();
        if (a.mover >= 0) base = world_[size_t(a.mover)];
        if (a.skinned) {
            a.skinned->poseBlend(r_, a.clip, t, a.clip, t, 0.0f);
            a.xf = gameMatrixToRuntime(base);
        } else {
            a.xf = gameMatrixToRuntime(a.node >= 0 ? world_[size_t(a.node)] : base);
        }
    }
}

void CutscenePlayer::render() {
    if (!active_) return;
    for (const auto& a : actors_) {
        if (a.skinned) {
            a.skinned->render(r_, a.xf);
        } else if (a.model) {
            for (const auto& part : a.model->parts) {
                DrawParams dp;
                dp.texture = part.texture;
                dp.blend = part.blend;
                dp.unlit = !part.lit;
                r_.draw(part.mesh, a.xf, dp);
            }
        }
    }
}

bool CutscenePlayer::takeNewShot(Vec3& eye, float& yaw) {
    if (!newShot_) return false;
    newShot_ = false;
    eye = eye_;
    yaw = yaw_;
    return true;
}

std::string CutscenePlayer::takeStream() {
    std::string s;
    s.swap(pendingStream_);
    return s;
}

float CutscenePlayer::blackout() const {
    if (!active_) return 0.0f;
    const float f = 0.25f;
    const float in = 1.0f - std::min(1.0f, t_ / f);
    const float out = shot_.next.empty() ? 0.0f : std::max(0.0f, 1.0f - (duration_ - t_) / f);
    return std::max(in, out);
}

std::vector<int> CutscenePlayer::actorTypes() const {
    std::vector<int> v;
    for (const auto& a : actors_) v.push_back(a.info.type);
    return v;
}

}  // namespace gvr
