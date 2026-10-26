#include "game/cutscene.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "core/log.h"

namespace gvr {

namespace {
bool isEffectModel(std::string n) {
    for (auto& c : n) c = char(std::tolower(static_cast<unsigned char>(c)));
    const size_t slash = n.find_last_of('/');
    if (slash != std::string::npos) n = n.substr(slash + 1);
    return n.rfind("efx_", 0) == 0;
}
}  // namespace

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
    shot_ = FlickShot{};  // no fade state from a previous cutscene
    t_ = 0;
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
    // The fade state carries from shot to shot when the scripts drive it.
    startVis_ = (active_ && !shot_.fades.empty()) ? scriptVisibility(t_) : 1.0f;
    // An opening shot whose script fades it in snaps in from black; one whose first fade goes to black
    // (rstory3_channel_islands_1: one fade, out at its end) starts in view - it was black for 10 s.
    if (shotsPlayed_ == 0 && !s.fades.empty()) startVis_ = s.fades.front().level > 0.5f ? 0.0f : 1.0f;
    // A shot that starts black (the last one faded out) but whose script only fades out again
    // (story3_lift_2 after story3_lift_1) would stay black throughout: it fades in over 0.25 s.
    if (startVis_ < 0.99f && !s.fades.empty() && s.fades.front().level < 0.5f && s.fades.front().time > 0.3f)
        s.fades.insert(s.fades.begin(), {0.0f, 1.0f, 0.25f});
    static const bool fadeTrace = std::getenv("GVR_CUT_TRACE") != nullptr;
    if (fadeTrace)
        for (const auto& f : s.fades) LOGI("cut trace: %s fade at %.2f s to %.2f over %.2f s", name.c_str(), f.time, f.level, f.seconds);
    shot_ = std::move(s);
    shotName_ = name;
    t_ = 0;
    cueNext_ = 0;
    fxNext_ = 0;
    duration_ = std::max(0.5f, shot_.duration());
    // A last shot fading out holds until its fade is through (story1_start_12's 2 s fade starts
    // 1.3 s before the director ends).
    if (shot_.next.empty())
        for (const auto& f : shot_.fades)
            if (f.level < 0.5f) duration_ = std::max(duration_, f.time + f.seconds);
    // Rework: a shot whose script has faded it to black with nothing left to show or say - no later
    // fade back, subtitle, voice or effect - ends 2 s after (Kstory5_End_18 held 130 s of black after
    // THE END).
    if (!shot_.fades.empty()) {
        const auto& lf = shot_.fades.back();
        if (lf.level < 0.02f) {
            const float black = lf.time + lf.seconds;
            float last = 0;
            for (float t : shot_.subtitleTimes) last = std::max(last, t);
            for (const auto& c : shot_.cues) last = std::max(last, c.time);
            for (const auto& e : shot_.effects) last = std::max(last, e.time);
            const float end = std::max(black, last) + 2.0f;
            if (end + 1.0f < duration_) {
                LOGI("Cutscene %s: black from %.1f s with nothing after it - ends at %.1f s instead of %.1f", name.c_str(), black, end, duration_);
                duration_ = end;
            }
        }
    }
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
    // Trees whose mover names a character (a typed actor of this shot, the Mecc crew, Delphi, Yan,
    // the Smarties...): only that character performs them (rework: a typed actor whose model didn't fit
    // its own tree took the best-fitting one - another character's).
    std::vector<bool> named(tables.size(), false);
    static const char* kCrew[] = {"baz", "tel", "reg", "gordon", "bennet", "meccaryn", "delphi", "yan", "tim", "wife",
                                  "borj", "smartie", "weeman", "toby", "kabuto", "offspring", "sappho", "lander"};
    for (int t = 0; t < int(tables.size()); ++t) {
        const std::string& o = tables[size_t(t)].owner;
        if (o.empty()) continue;
        for (const char* c : kCrew)
            if (o.compare(0, std::strlen(c), c) == 0) named[size_t(t)] = true;
        for (const auto& a : shot_.actors)
            if (a.type >= 0 && !a.node.empty() && similar(o, lower(a.node))) named[size_t(t)] = true;
    }
    // Loads an actor's model and finds its tree; false if it can't be shown in this shot.
    auto place = [&](const FlickActor& a, const std::string& model, std::string idleAnim, bool quiet, int only = -1) {
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
        // At least half the skeleton (small rigs such as the 5-bone hatch need all but a few).
        const size_t needBones = sm ? std::max<size_t>(std::min<size_t>(3, sm->boneCount()), sm->boneCount() / 2) : 0;
        auto fits = [&](int t) { return !sm || sm->boneMatches(tables[size_t(t)].anim) >= needBones; };
        const std::string me = lower(a.node);
        int table = -1;
        // Pinned to one tree (the untyped performances below): that tree or nothing.
        if (only >= 0) {
            if (claimed[size_t(only)] || !fits(only) || (!sm && true)) {
                if (!quiet) LOGI("Cutscene %s: %s does not fit tree %d", name.c_str(), model.c_str(), only);
                return false;
            }
            table = only;
        }
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
        //    Trees whose mover names another character come last: only if no other tree fits
        //    (kstory5_kabuto_fight_2's second Kabuto performs on a "Delphi_mover").
        for (int pass = 0; pass < 2 && table < 0 && sm; ++pass) {
            size_t bestM = 0, bestExtra = ~size_t(0);
            for (int t = 0; t < int(tables.size()); ++t) {
                if (claimed[size_t(t)] || tables[size_t(t)].camera) continue;
                bool other = named[size_t(t)] && !similar(tables[size_t(t)].owner, me);
                for (const char* c : kCrew)  // (the same character's other tree is no one else's)
                    if (tables[size_t(t)].owner.compare(0, std::strlen(c), c) == 0 && me.compare(0, std::strlen(c), c) == 0) other = false;
                if (other != (pass == 1)) continue;
                const size_t m = sm->boneMatches(tables[size_t(t)].anim);
                // Most bones matched; on a tie, the tree nearest the skeleton's size (story3_mrhorn_2's
                // 9-bone drawbridge took a Sonak's 74-node tree, leaving that Sonak out).
                const size_t extra = tables[size_t(t)].anim.nodes.size() > sm->boneCount() ? tables[size_t(t)].anim.nodes.size() - sm->boneCount()
                                                                                           : sm->boneCount() - tables[size_t(t)].anim.nodes.size();
                if (m > bestM || (m == bestM && m >= needBones && extra < bestExtra)) {
                    if (m < needBones) continue;
                    bestM = m;
                    bestExtra = extra;
                    table = t;
                }
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
        if (a.type < 0 || a.node.empty() || a.parentActor >= 0) continue;
        const std::string model = reg_.modelName(uint32_t(a.type));
        if (model.empty()) { LOGW("Cutscene %s: actor %s (type %d) has no model", name.c_str(), a.node.c_str(), a.type); continue; }
        place(a, model, reg_.modelAnim(uint32_t(a.type)), false);
    }
    // Performances with no typed actor record, on their own movers: the Mecc crew (Baz, Gordon, ...),
    // and also Delphi, Yan, Timmy, the Smarties, Borjoyzee and the wife (rework: these were all shown
    // as Meccaryns - or, not fitting a Meccaryn, took a Meccaryn's tree from him - so the ending's
    // last shot showed two Meccaryns where wife, Smartie, Timmy, Delphi and Borjoyzee stand). Each
    // tree gets the model its mover's name says, if its skeleton fits that tree; else a Meccaryn's.
    static const std::pair<const char*, const char*> kByName[] = {
        {"delphi", "rp_l0"}, {"yan", "smyan_l0"}, {"timmy", "smtimmy_l0"}, {"tim", "smtimmy_l0"}, {"wife", "smf_l0"},
        {"borj", "smborjoyzee_l0"}, {"smartie", "sm_l0"}, {"weeman", "sm_l0"}, {"toby", "toby"}, {"kabuto", "kb_l0"},
        {"offspring", "offspring_l0"}};
    for (int t = 0; t < int(tables.size()); ++t) {
        if (claimed[size_t(t)] || tables[size_t(t)].camera || tables[size_t(t)].anim.nodes.size() < 8) continue;
        FlickActor a;
        a.node = tables[size_t(t)].owner.empty() ? "meccaryn" : tables[size_t(t)].owner;
        a.type = -2;
        bool done = false;
        for (const auto& kv : kByName) {
            if (a.node.compare(0, std::strlen(kv.first), kv.first) != 0) continue;
            std::string anim = reg_.animForModel(kv.second);
            if (anim.empty() && fs_.exists(std::string(kv.second) + ".anm")) anim = std::string(kv.second) + ".anm";
            done = !anim.empty() && place(a, kv.second, anim, true, t);
            break;
        }
        // A Meccaryn only for the crew or no name - never for a tree named after someone else (the
        // lander's own tree in story4_T_square was drawn as a Meccaryn).
        static const char* kNotMecc[] = {"lander", "kabuto", "delphi", "yan", "tim", "wife", "borj", "smartie", "weeman",
                                         "toby", "offspring", "sappho", "reaper", "rp", "sonak", "vimp", "rip"};
        bool notMecc = false;
        for (const char* c : kNotMecc)
            if (a.node.compare(0, std::strlen(c), c) == 0) notMecc = true;
        if (!done && !notMecc && tables[size_t(t)].anim.nodes.size() >= 30) place(a, "mc_l0", "mc_stand.anm", true, t);
    }
    // Free actors placed in the world (op 0x20): e.g. the wax-Smartie rig (type 1259) at the
    // rock pillars in story1_save_sm_2a.
    for (const auto& a : shot_.actors) {
        if (!a.placed || a.parentActor >= 0 || a.type < 0) continue;
        const std::string model = reg_.modelName(uint32_t(a.type));
        if (model.empty()) continue;
        Actor act;
        act.info = a;
        std::string anim = reg_.modelAnim(uint32_t(a.type));
        if (anim.empty() && fs_.exists(model + ".anm")) anim = model + ".anm";
        if (fs_.exists(model + ".skn") && !anim.empty()) {
            auto sm = std::make_unique<SkinnedModel>();
            if (sm->load(fs_, models_, r_, model, anim)) { act.clip = 0; act.skinned = std::move(sm); }
        }
        if (!act.skinned) act.model = models_.get(model + ".gbs", anim);
        if (!act.skinned && !act.model) continue;
        act.fixed = true;
        act.xf = Mat4::translation(giantsToRuntime(a.px, a.py, a.pz)) *
                 Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, a.pyaw * 3.14159265f / 180.0f));
        LOGI("Cutscene %s: placed %s (type %d) at (%.0f %.0f %.0f)", name.c_str(), model.c_str(), a.type, a.px, a.py, a.pz);
        actors_.push_back(std::move(act));
    }
    // Props carried by actors (op 3): the jetpack in Timmy's hand, Baz's gun, Timmy riding on
    // Baz's back, the lander's engine flames. Placed on the carrier's bone each frame.
    for (const auto& a : shot_.actors) {
        if (a.parentActor < 0 || a.type < 0) continue;
        int carrier = -1;
        for (size_t k = 0; k < actors_.size(); ++k)
            if (actors_[k].info.index == a.parentActor && actors_[k].skinned) carrier = int(k);
        if (carrier < 0) continue;
        const std::string model = reg_.modelName(uint32_t(a.type));
        if (model.empty()) continue;
        Actor act;
        act.info = a;
        act.parent = carrier;
        act.parentNode = lower(a.parentNode);
        std::string anim = reg_.modelAnim(uint32_t(a.type));
        // The 4th field names either the prop's own clip (".anm") or its node aligned to the bone.
        if (!a.anim.empty() && fs_.exists(a.anim + ".anm")) anim = a.anim + ".anm";
        else act.align = lower(a.anim);
        if (anim.empty() && fs_.exists(model + ".anm")) anim = model + ".anm";
        if (fs_.exists(model + ".skn") && !anim.empty()) {
            auto sm = std::make_unique<SkinnedModel>();
            if (sm->load(fs_, models_, r_, model, anim)) { act.clip = 0; act.skinned = std::move(sm); }
        }
        if (!act.skinned) act.model = models_.get(model + ".gbs", anim, true);
        if (!act.skinned && !act.model) continue;
        if (!act.skinned && !anim.empty()) {
            std::vector<uint8_t> b;
            AnmAnimation ra;
            if (fs_.read(anim, b) && parseAnm(b.data(), b.size(), ra)) {
                std::vector<Mat4> wm;
                ra.evaluate(0.0f, wm);
                for (size_t k = 0; k < ra.nodes.size() && k < wm.size(); ++k) {
                    const std::string nm = lower(ra.nodes[k].name);
                    if (nm.compare(0, 4, "ref_") == 0) act.refs.push_back({nm, wm[k]});
                }
            }
        }
        LOGI("Cutscene %s: prop %s (type %d) on %s.%s", name.c_str(), model.c_str(), a.type,
             actors_[size_t(carrier)].info.node.c_str(), a.parentNode.c_str());
        actors_.push_back(std::move(act));
    }
    // Viewpoint: the camera at the start of the shot, facing its target (or view axis).
    dir.evaluate(0.0f, world_);
    eye_ = {0, 0, 0};
    yaw_ = 0;
    auto cam = dir.byName.find(lower(shot_.cameraNode.empty() ? "camera01" : shot_.cameraNode));
    camNode_ = cam != dir.byName.end() ? cam->second : -1;
    tgtNode_ = -1;
    hasEyeOverride_ = false;
    if (cam != dir.byName.end()) {
        const Mat4& m = world_[size_t(cam->second)];
        eye_ = giantsToRuntime(m.m[12], m.m[13], m.m[14]);
        // Target cameras (Max look-at controller; their rotation track is type 2, which carries
        // no keys) face their ".target" node; free cameras look down their local -Z.
        Vec3 fwd = giantsToRuntime(-m.m[8], -m.m[9], -m.m[10]);
        auto tgt = dir.byName.find(cam->first + ".target");
        tgtNode_ = tgt != dir.byName.end() ? tgt->second : -1;
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
    groundActors();
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
    static const bool trace = std::getenv("GVR_CUT_TRACE") != nullptr;  // test hook
    if (trace && int(t_ * 4) != int((t_ - dt) * 4)) LOGI("cut trace: %s t %.2f / %.2f blackout %.2f (%zu fades)", shotName_.c_str(), t_, duration_, blackout(), shot_.fades.size());
    if (t_ >= duration_) {
        const std::string next = shot_.next;
        if (next.empty() || !loadShot(next)) {
            active_ = false;
            actors_.clear();
            return;
        }
    }
    const float t = std::min(t_, duration_ * 0.999f);
    while (cueNext_ < shot_.cues.size() && shot_.cues[cueNext_].time <= t_) dueCues_.push_back(shot_.cues[cueNext_++].stream);
    shot_.director.evaluate(t, world_);
    eyeNow_ = eye_;
    if (camNode_ >= 0 && !hasEyeOverride_) {
        const Mat4& m = world_[size_t(camNode_)];
        eyeNow_ = giantsToRuntime(m.m[12], m.m[13], m.m[14]);
    }
    // The camera's own axes: a target camera looks at its target (level up, no roll); a free one down
    // its local -Z with its local +Y up.
    camFwd_ = Vec3{-std::sin(yaw_), 0, -std::cos(yaw_)};
    camUp_ = Vec3{0, 1, 0};
    if (camNode_ >= 0 && !hasEyeOverride_) {
        const Mat4& m = world_[size_t(camNode_)];
        if (tgtNode_ >= 0) {
            const Mat4& tm = world_[size_t(tgtNode_)];
            const Vec3 d = giantsToRuntime(tm.m[12], tm.m[13], tm.m[14]) - eyeNow_;
            if (length(d) > 1e-4f) camFwd_ = normalize(d);
        } else {
            const Vec3 f = giantsToRuntime(-m.m[8], -m.m[9], -m.m[10]), u = giantsToRuntime(m.m[4], m.m[5], m.m[6]);
            if (length(f) > 1e-6f) camFwd_ = normalize(f);
            if (length(u) > 1e-6f && std::fabs(dot(normalize(u), camFwd_)) < 0.99f) camUp_ = normalize(u);
        }
    }
    if (tgtNode_ >= 0 && !hasEyeOverride_) {
        const Mat4& m = world_[size_t(tgtNode_)];
        targetNow_ = giantsToRuntime(m.m[12], m.m[13], m.m[14]);
    } else {
        Vec3 c{0, 0, 0};
        int n = 0;
        for (const auto& a : actors_)
            if (a.parent < 0) { c = c + Vec3{a.xf.m[12], a.xf.m[13] + 1.0f, a.xf.m[14]}; ++n; }
        targetNow_ = n ? c * (1.0f / float(n)) : eyeNow_ + Vec3{-std::sin(yaw_), 0, -std::cos(yaw_)} * 8.0f;
    }
    for (auto& a : actors_) {
        if (a.parent >= 0) continue;  // props: second pass, once their carriers are posed
        if (a.fixed) {
            if (a.skinned) a.skinned->pose(r_, t);
            continue;
        }
        Mat4 base = Mat4::identity();
        if (a.mover >= 0) base = world_[size_t(a.mover)];
        if (a.skinned) {
            a.skinned->poseBlend(r_, a.clip, t, a.clip, t, 0.0f);
            a.xf = gameMatrixToRuntime(base);
            // Feet on our ground: our terrain surface differs slightly from the one the shots
            // were staged on, which sank standing actors' feet into hillsides. Only small offsets
            // are corrected (actors deliberately below ground - in water, in the hatch - keep theirs).
            if (a.grounded) {
                a.xf = Mat4::translation({0, a.groundOffset, 0}) * a.xf;
            } else if (groundHeight && a.info.type != 4) {
                Vec3 mn, mx;
                a.skinned->posedBounds(mn, mx);
                const Vec4 c = a.xf * Vec4{0.5f * (mn.x + mx.x), mn.y, 0.5f * (mn.z + mx.z), 1.0f};
                const float g = groundHeight(c.x, c.y, c.z);
                const float sink = g - c.y;
                if (sink > 0.02f && sink < 0.6f) a.xf = Mat4::translation({0, sink, 0}) * a.xf;
                // Test hook: GVR_CUT_GROUND logs each actor's gap above our ground twice a second.
                static const bool trace = std::getenv("GVR_CUT_GROUND") != nullptr;
                if (trace && std::floor((t_ - dt) * 2.0f) != std::floor(t_ * 2.0f))
                    LOGI("cut-ground %s t=%.1f %s gap %.2f m (ground %.2f)", shotName_.c_str(), t_, a.info.node.c_str(), -sink, g);
            }
        } else {
            a.xf = gameMatrixToRuntime(a.node >= 0 ? world_[size_t(a.node)] : base);
        }
    }
    updateProps(t);
    while (fxNext_ < shot_.effects.size() && shot_.effects[fxNext_].time <= t_) {
        const auto& e = shot_.effects[fxNext_++];
        for (const auto& a : actors_) {
            if (a.info.index != e.actor) continue;
            if (!a.visible) break;
            Mat4 bone;
            Vec3 at{a.xf.m[12], a.xf.m[13], a.xf.m[14]};
            if (!e.node.empty() && (a.skinned ? a.skinned->nodeWorld(e.node, bone) : a.refNode(toLower(e.node), bone))) {
                const Mat4 w = a.xf * gameMatrixToRuntime(bone);
                at = Vec3{w.m[12], w.m[13], w.m[14]};
            }
            if (dueFx_.size() < 32) dueFx_.push_back({e.name, at, e.mode});
            break;
        }
    }
}

void CutscenePlayer::groundActors() {
    if (!groundHeight) return;
    const AnmAnimation& dir = shot_.director;
    for (auto& a : actors_) {
        a.grounded = false;
        a.groundOffset = 0;
        if (!a.info.groundFollow || !a.skinned || a.fixed || a.parent >= 0) continue;
        // Sample the shot (3 per second): the feet's lowest gap above the ground under the footprint.
        float minGap = 1e9f;
        const int samples = std::max(2, int(duration_ * 3.0f) + 1);
        for (int k = 0; k < samples; ++k) {
            const float t = std::min(duration_ * 0.999f, duration_ * float(k) / float(samples - 1));
            dir.evaluate(t, world_);
            const Mat4 xf = gameMatrixToRuntime(a.mover >= 0 ? world_[size_t(a.mover)] : Mat4::identity());
            Vec3 mn, mx;
            a.skinned->boundsAt(a.clip, t, mn, mx);
            const Vec4 c = xf * Vec4{0.5f * (mn.x + mx.x), mn.y, 0.5f * (mn.z + mx.z), 1.0f};
            // Footprint corners (game units, actor frame) -> runtime offsets around the feet.
            float g = groundHeight(c.x, c.y, c.z);
            for (int q = 0; q < 4; ++q) {
                const float fx = (q & 1) ? a.info.footMax[0] : a.info.footMin[0];
                const float fy = (q & 2) ? a.info.footMax[1] : a.info.footMin[1];
                const Vec4 o = xf * Vec4{-fx * gGameUnitsToMeters * 0.5f, 0.0f, fy * gGameUnitsToMeters * 0.5f, 0.0f};
                g = std::max(g, groundHeight(c.x + o.x, c.y, c.z + o.z));
            }
            if (g < -1e8f) continue;  // off the terrain
            minGap = std::min(minGap, c.y - g);
        }
        // Only plausible staging offsets: an actor up a tree or deep in a hole is left alone.
        if (minGap < 1e8f && minGap > -0.8f && minGap < 1.2f && std::fabs(minGap) > 0.03f) {
            a.grounded = true;
            a.groundOffset = -minGap;
            LOGI("Cutscene %s: %s grounded (%+.2f m)", shotName_.c_str(), a.info.node.c_str(), a.groundOffset);
        }
    }
}

void CutscenePlayer::updateProps(float t) {
    for (auto& a : actors_) {
        if (a.parent < 0) continue;
        const Actor& c = actors_[size_t(a.parent)];
        Mat4 bone;
        a.visible = c.skinned && c.skinned->nodeWorld(a.parentNode, bone);
        if (!a.visible) continue;
        Mat4 local = gameMatrixToRuntime(bone);
        if (a.skinned) {
            const float d = a.skinned->animDuration(0);
            a.skinned->pose(r_, d > 0 ? std::fmod(t, d) : 0.0f);
            Mat4 al;
            if (!a.align.empty() && a.skinned->nodeWorld(a.align, al)) local = local * gameMatrixToRuntime(al).affineInverse();
        }
        Mat4 al;
        if (!a.skinned && !a.align.empty() && a.refNode(a.align, al)) local = local * gameMatrixToRuntime(al).affineInverse();
        a.xf = c.xf * local;
    }
}

void CutscenePlayer::render(bool nearCull) {
    if (!active_) return;
    for (const auto& a : actors_) {
        if (!a.visible || !shot_.actorShownAt(a.info.index, t_ + 1e-3f)) continue;
        if (a.skinned) {
            // An actor right at the shot camera (closer than 1 m) was outside the original's narrow
            // view but fills a headset's (story1_jetpack_6's hatch lid): leave it out.
            Vec3 mn, mx;
            a.skinned->posedBounds(mn, mx);
            Vec3 wmn{1e9f, 1e9f, 1e9f}, wmx{-1e9f, -1e9f, -1e9f};
            for (int k = 0; k < 8; ++k) {
                const Vec4 c = a.xf * Vec4{k & 1 ? mx.x : mn.x, k & 2 ? mx.y : mn.y, k & 4 ? mx.z : mn.z, 1.0f};
                wmn = {std::min(wmn.x, c.x), std::min(wmn.y, c.y), std::min(wmn.z, c.z)};
                wmx = {std::max(wmx.x, c.x), std::max(wmx.y, c.y), std::max(wmx.z, c.z)};
            }
            const Vec3 e = r_.currentView().eyePosition;
            const Vec3 q{std::max(wmn.x, std::min(e.x, wmx.x)), std::max(wmn.y, std::min(e.y, wmx.y)), std::max(wmn.z, std::min(e.z, wmx.z))};
            if (nearCull && length(q - e) < 1.0f) {
                if (!a.nearLogged) { a.nearLogged = true; LOGI("Cutscene %s: %s at the camera, not drawn", shotName_.c_str(), a.info.node.c_str()); }
                continue;
            }
            // Effect models glow in their own light (efx_portal's glow bar over its black void, in
            // rstory5_kabuto_stone_11).
            if (isEffectModel(a.skinned->name())) a.skinned->renderGlow(r_, a.xf, 1.0f);
            else a.skinned->render(r_, a.xf);
        } else if (a.model) {
            for (const auto& part : a.model->parts) {
                DrawParams dp;
                dp.texture = part.texture;
                dp.blend = part.blend;
                if (part.glass) { dp.blend = BlendMode::AlphaBlend; dp.tint.w *= 0.3f; }
                dp.unlit = !part.lit;
                if (isEffectModel(a.model->name)) dp.unlit = true;  // (an effect's own light; its parts keep their blending)
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

std::vector<std::string> CutscenePlayer::subtitles() const {
    std::vector<std::string> v;
    int cur = -1;
    for (size_t i = 0; i < shot_.subtitles.size(); ++i)
        if (i < shot_.subtitleTimes.size() && shot_.subtitleTimes[i] <= t_ + 0.05f) cur = int(i);
    if (cur >= 0) v.push_back(shot_.subtitles[size_t(cur)]);
    return v;
}

std::vector<CutscenePlayer::FxEvent> CutscenePlayer::takeEffects() {
    std::vector<FxEvent> v;
    v.swap(dueFx_);
    return v;
}

std::vector<std::string> CutscenePlayer::takeCues() {
    std::vector<std::string> v;
    v.swap(dueCues_);
    return v;
}

std::string CutscenePlayer::takeStream() {
    std::string s;
    s.swap(pendingStream_);
    return s;
}

float CutscenePlayer::scriptVisibility(float t) const {
    float v = startVis_;
    const auto& fs = shot_.fades;
    for (size_t i = 0; i < fs.size(); ++i) {
        if (t < fs[i].time) break;
        const float end = i + 1 < fs.size() ? std::min(t, fs[i + 1].time) : t;
        const float k = fs[i].seconds <= 0 ? 1.0f : std::min(1.0f, (end - fs[i].time) / fs[i].seconds);
        v += (fs[i].level - v) * k;
    }
    return v;
}

float CutscenePlayer::blackout() const {
    if (!active_) return 0.0f;
    // The shot's own fades (op 0x1F) when it has any; otherwise a short dip between shots.
    if (!shot_.fades.empty()) return 1.0f - scriptVisibility(t_);
    if (startVis_ < 0.999f) return std::max(0.0f, 1.0f - (startVis_ + (1.0f - startVis_) * std::min(1.0f, t_ / 0.25f)));
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
