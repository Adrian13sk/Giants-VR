// The base's Smartie workforce (5c / r4a: "Gather SMARTIES"). From the data: each base mission has
// three Smartie hatches (sm_hatch_l0, class 38) out in the level, and the Smartie clips include
// sm_hatch_exit, sm_hammer_use, sm_saw_use, sm_dig, sm_run_with_materials and sm_spell_builds
// (the Reaper Smarties build by magic); GText "Required: %d", "%d/%d Working" and "Awaiting More
// Smarties" say buildings need a number of workers.
//
// Ours: three Smarties wait under each hatch and come out when the hero gets within 7 m; they
// run back to the base (faster while out of sight) and join the workforce, which starts with
// Borjoyzee's / Yan's three. Every building under construction takes two workers; with fewer
// free, it waits. Workers at a site hammer, saw and dig (Reaper Smarties cast the build spell);
// the rest sit by the base's middle.
#include "game/game_app.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "scene/asset_world.h"
#include "scene/model_cache.h"
#include "scene/skinned_model.h"

namespace gvr {

namespace {
enum CrewClip { kStand = 0, kWalk, kHammer, kSaw, kDig, kSpell, kHatch, kSit, kCelebrate, kCrewClips };
const char* kCrewAnims[kCrewClips] = {"sm_stand.anm",   "sm_run_with_materials.anm", "sm_hammer_use.anm", "sm_saw_use.anm", "sm_dig.anm",
                                      "sm_spell_builds.anm", "sm_hatch_exit.anm",     "sm_sit.anm",        "sm_celebrate.anm"};
constexpr int kStartCrew = 3;
constexpr int kPerHatch = 3;
constexpr int kPerSite = 2;
}  // namespace

bool GameApp::addCrewSmartie(const Vec3& at, bool fromHatch) {
    if (!assetWorld_) return false;
    Base::Worker w;
    w.model = std::make_unique<SkinnedModel>();
    if (!w.model->load(assetWorld_->fs(), *assetWorld_->models(), renderer_, "sm_L0", kCrewAnims[0])) return false;
    for (int k = 1; k < kCrewClips; ++k) {
        const int i = w.model->addAnimation(assetWorld_->fs(), kCrewAnims[k]);
        if (base_.crewClip[k] < 0) base_.crewClip[k] = i;
    }
    w.model->pose(renderer_, 0.0f);
    Vec3 mn, mx;
    w.model->posedBounds(mn, mx);
    w.lift = -mn.y;
    w.pos = at;
    w.clip = fromHatch ? kHatch : kSit;
    w.joined = !fromHatch;
    w.yaw = frand() * 6.28f;
    base_.crew.push_back(std::move(w));
    return true;
}

void GameApp::initCrew() {
    base_.crew.clear();
    base_.hatches.clear();
    base_.crewClip[0] = 0;
    for (int k = 1; k < 16; ++k) base_.crewClip[k] = -1;
    const MissionData& md = assetWorld_->mission();
    const ObjectRegistry& reg = assetWorld_->registry();
    for (const auto& o : md.objects)
        if (toLower(reg.modelName(o.type)) == "sm_hatch_l0") base_.hatches.push_back({giantsToRuntime(o.x, o.y, o.z), false});
    int start = kStartCrew;
    if (const char* f = std::getenv("GVR_BASE_WORKERS")) start = std::max(0, std::atoi(f));
    for (int i = 0; i < start; ++i) {
        const float a = frand() * 6.28f;
        Vec3 p = base_.centre + Vec3{std::cos(a) * 4.0f, 0, std::sin(a) * 4.0f};
        float gy;
        if (assetWorld_->terrain().heightAt(p.x, p.z, gy)) p.y = gy;
        addCrewSmartie(p, false);
    }
    LOGI("Base crew: %zu Smarties, %zu hatches with %d each", base_.crew.size(), base_.hatches.size(), kPerHatch);
}

int GameApp::crewCount() const {
    int n = 0;
    for (const auto& w : base_.crew) n += w.joined;
    return n;
}

// How many buildings can go up at once: two workers each.
int GameApp::crewSites() const { return crewCount() / kPerSite; }

void GameApp::updateCrew(float dt) {
    // Hatches: the hero comes close, out they climb.
    for (auto& h : base_.hatches) {
        if (h.open) continue;
        const Vec3 d = player_.position - h.pos;
        if (d.x * d.x + d.z * d.z > 7.0f * 7.0f || std::fabs(d.y) > 6.0f) continue;
        h.open = true;
        int n = 0;
        for (int i = 0; i < kPerHatch; ++i) {
            Vec3 p = h.pos + Vec3{(frand() - 0.5f) * 1.5f, 0, (frand() - 0.5f) * 1.5f};
            float gy;
            if (assetWorld_->terrain().heightAt(p.x, p.z, gy)) p.y = gy;
            n += addCrewSmartie(p, true);
        }
        timmyLine_ = std::to_string(n) + " SMARTIES COME OUT - THEY'RE OFF TO YOUR BASE TO WORK!";
        timmyLineT_ = 4.0f;
        sfx("ph_selct", h.pos, 1.0f, 1.4f);
        LOGI("Hatch opened at (%.1f %.1f %.1f): %d Smarties", h.pos.x, h.pos.y, h.pos.z, n);
    }
    // Sites that need hands: the buildings going up, oldest order first.
    std::vector<int> sites;
    for (size_t i = 0; i < base_.bldgs.size(); ++i)
        if (base_.bldgs[i].state == 1) sites.push_back(int(i));
    const int active = std::min(int(sites.size()), crewSites());
    // Assign: kPerSite workers to each active site, in order; the rest rest by the middle.
    int slot = 0;
    for (auto& w : base_.crew) {
        if (!w.joined) continue;
        const int si = slot / kPerSite;
        w.site = si < active ? sites[size_t(si)] : -1;
        ++slot;
    }
    const float t = float(time_);
    for (size_t wi = 0; wi < base_.crew.size(); ++wi) {
        auto& w = base_.crew[wi];
        w.clipT += dt;
        // The hatch exit plays once, then off to the base.
        if (w.clip == kHatch) {
            const float d = w.model->animDuration(std::max(0, base_.crewClip[kHatch]));
            if (w.clipT < d) continue;
            w.clip = kWalk;
            w.clipT = 0;
        }
        Vec3 goal;
        if (!w.joined) goal = base_.centre;
        else if (w.site >= 0) {
            const Mat4& x = base_.bldgs[size_t(w.site)].xf;
            const float a = float(wi) * 2.4f;  // spread round the site
            goal = Vec3{x.m[12] + std::cos(a) * 5.0f, x.m[13], x.m[14] + std::sin(a) * 5.0f};
        } else {
            const float a = float(wi) * 0.9f;
            goal = base_.centre + Vec3{std::cos(a) * 4.0f, 0, std::sin(a) * 4.0f};
        }
        const Vec3 g{goal.x - w.pos.x, 0, goal.z - w.pos.z};
        const float gd = std::sqrt(g.x * g.x + g.z * g.z);
        if (gd > 1.0f) {
            const float far = length(w.pos - player_.position) > 60.0f ? 4.0f : 1.0f;  // unseen: hurry
            const float step = std::min(gd, 2.6f * far * dt);
            w.pos = w.pos + g * (step / gd);
            float gy;
            if (assetWorld_->terrain().heightAt(w.pos.x, w.pos.z, gy)) w.pos.y = gy;
            w.yaw = std::atan2(-g.x, -g.z);
            if (w.clip != kWalk) { w.clip = kWalk; w.clipT = 0; }
            continue;
        }
        if (!w.joined) {
            w.joined = true;
            LOGI("Base crew: a Smartie joined (%d)", crewCount());
            timmyLine_ = "SMARTIES: " + std::to_string(crewCount()) + " AT WORK";
            timmyLineT_ = 2.5f;
        }
        int want;
        if (w.site >= 0) {
            const Mat4& x = base_.bldgs[size_t(w.site)].xf;
            w.yaw = std::atan2(-(x.m[12] - w.pos.x), -(x.m[14] - w.pos.z));  // face the building
            static const int jobs[3] = {kHammer, kSaw, kDig};
            want = base_.reaper ? kSpell : jobs[(wi + size_t(t / 6.0f)) % 3];
        } else {
            want = base_.complete ? (int(wi + size_t(t / 8.0f)) % 4 == 0 ? kCelebrate : kSit) : kSit;
        }
        if (w.clip != want) { w.clip = want; w.clipT = 0; }
    }
}

void GameApp::renderCrew(const Vec3& eye, bool shadow) {
    for (auto& w : base_.crew) {
        if (length(w.pos - eye) > (shadow ? 60.0f : 120.0f)) continue;
        const int clip = std::max(0, base_.crewClip[w.clip]);
        const float d = w.model->animDuration(clip);
        if (!shadow) w.model->poseBlend(renderer_, clip, d > 0 ? std::fmod(w.clipT, d) : 0.0f, clip, 0.0f, 0.0f);
        const Mat4 xf = Mat4::translation(w.pos + Vec3{0, w.lift, 0}) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, w.yaw));
        if (shadow) w.model->renderShadow(renderer_, xf);
        else w.model->render(renderer_, xf);
    }
}

}  // namespace gvr
