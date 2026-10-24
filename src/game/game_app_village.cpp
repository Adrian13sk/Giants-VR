// GameApp: Smartie villagers (VillagerDirector + the original Smartie model and sm_* clips).
#include "game/game_app.h"

#include <algorithm>
#include <cmath>

#include "core/log.h"
#include "scene/asset_world.h"
#include "scene/model_cache.h"
#include "scene/skinned_model.h"

namespace gvr {

void GameApp::initVillagers(AssetWorld* w, ModelCache* mc) {
    villagers_ = VillagerDirector{};
    villagerModels_.clear();
    villagerLift_.clear();
    if (!w || !w->hasTerrain()) return;
    // Villages: clusters of Smartie houses (smw_house*) within 60 m of each other.
    struct House { Vec3 c; float r; };
    std::vector<House> houses;
    for (const auto& o : w->worldObjects())
        if (o.name.rfind("smw_house", 0) == 0) houses.push_back({o.centre, o.radius});
    if (houses.size() < 3) return;
    std::vector<int> cluster(houses.size(), -1);
    int clusters = 0;
    for (size_t i = 0; i < houses.size(); ++i) {
        if (cluster[i] >= 0) continue;
        cluster[i] = clusters;
        for (bool grew = true; grew;) {
            grew = false;
            for (size_t j = 0; j < houses.size(); ++j)
                if (cluster[j] < 0)
                    for (size_t k = 0; k < houses.size(); ++k)
                        if (cluster[k] == clusters && length(Vec3{houses[j].c.x - houses[k].c.x, 0, houses[j].c.z - houses[k].c.z}) < 60.0f) {
                            cluster[j] = clusters;
                            grew = true;
                            break;
                        }
        }
        ++clusters;
    }
    villagers_.groundHeight = [w](float x, float z) {
        float y;
        return w->terrain().heightAt(x, z, y) ? y : 0.0f;
    };
    villagers_.blocked = [w](const Vec3& a, const Vec3& b) {
        const Vec3 o = a + Vec3{0, 0.5f, 0};
        Vec3 d = (b + Vec3{0, 0.5f, 0}) - o;
        const float l = length(d);
        if (l < 1e-3f) return false;
        float t;
        Vec3 n;
        return w->collision().raycast(o, d * (1.0f / l), l + 0.3f, t, n);
    };
    static const char* clips[int(VillagerClip::Count)] = {"sm_stand.anm",   "sm_hwalk.anm",  "sm_scan.anm",      "sm_talk1.anm",
                                                          "sm_listen1.anm", "sm_laugh1.anm", "sm_stand_sing.anm", "sm_tai_chi.anm",
                                                          "sm_sit.anm",     "sm_celebrate.anm", "sm_wave.anm",   "sm_Run_terror.anm",
                                                          "sm_cower.anm"};
    const int kMax = 16;
    for (int c = 0; c < clusters && int(villagers_.villagers.size()) < kMax; ++c) {
        Vec3 centre;
        int n = 0;
        float extent = 0;
        for (size_t i = 0; i < houses.size(); ++i)
            if (cluster[i] == c) { centre = centre + houses[i].c; ++n; }
        if (n < 3) continue;
        centre = centre * (1.0f / float(n));
        for (size_t i = 0; i < houses.size(); ++i)
            if (cluster[i] == c) extent = std::max(extent, length(Vec3{houses[i].c.x - centre.x, 0, houses[i].c.z - centre.z}));
        const int want = std::min(8, n + 2);
        int made = 0;
        for (int tries = 0; tries < 60 && made < want && int(villagers_.villagers.size()) < kMax; ++tries) {
            const float a = frand() * 6.2832f, d = 3.0f + frand() * (extent + 6.0f);
            Vec3 p = centre + Vec3{std::cos(a) * d, 0, std::sin(a) * d};
            float gy;
            if (!w->terrain().heightAt(p.x, p.z, gy) || gy < 0.4f) continue;  // on land
            p.y = gy;
            bool inside = false;
            for (const auto& h : houses) inside |= length(Vec3{p.x - h.c.x, 0, p.z - h.c.z}) < h.r * 0.85f;
            if (inside) continue;
            auto m = std::make_unique<SkinnedModel>();
            if (!m->load(w->fs(), *mc, renderer_, "sm_L0", clips[0])) return;
            if (villagerModels_.empty())
                for (int k = 1; k < int(VillagerClip::Count); ++k) villagerClip_[k] = std::max(0, m->addAnimation(w->fs(), clips[k]));
            else
                for (int k = 1; k < int(VillagerClip::Count); ++k) m->addAnimation(w->fs(), clips[k]);
            m->pose(renderer_, 0.0f);
            Vec3 mn, mx;
            m->posedBounds(mn, mx);
            const int i = villagers_.add(p, frand() * 6.2832f);
            villagers_.villagers[size_t(i)].home = centre + (p - centre) * 0.6f;
            villagers_.villagers[size_t(i)].home.y = p.y;
            villagers_.params.homeRadius = std::max(12.0f, extent + 4.0f);
            villagers_.villagers[size_t(i)].slot = int(villagerModels_.size());
            villagerModels_.push_back(std::move(m));
            villagerLift_.push_back(-mn.y);
            ++made;
        }
        LOGI("Village %d: %d houses, %d Smarties about (game %.0f, %.0f)", c, n, made, -centre.x / gGameUnitsToMeters,
             centre.z / gGameUnitsToMeters);
    }
}

void GameApp::updateVillagers(float dt) {
    if (villagers_.villagers.empty()) return;
    std::vector<Vec3> threats, scares;
    for (const auto& e : enemies_.enemies)
        if (e.state != EnemyState::Dead) threats.push_back(e.pos);
    for (const auto& c : creatures_.creatures)
        if (c.state != CreatureState::Dead && !CreatureDirector::underground(c)) threats.push_back(c.pos);
    if (hero_ == Hero::Kabuto) threats.push_back(player_.position);  // Smarties are his food
    for (const auto& b : blasts_)
        if (b.age < 0.05f) scares.push_back(b.pos);
    if (weapon_.shotsFired() != villagerShots_) {  // the Meccaryn's gunfire frightens them too
        villagerShots_ = weapon_.shotsFired();
        scares.push_back(player_.position);
    }
    villagers_.update(dt, player_.position, threats, scares);
    for (const auto& v : villagers_.villagers) {
        if (v.slot < 0) continue;
        const float d = length(v.pos - player_.position);
        if (d > 70.0f || (d > 25.0f && ((enemyPoseTick_ + uint32_t(v.slot)) & 1))) continue;  // beyond 25 m: every other frame
        const int clip = villagerClip_[int(v.clip)];
        villagerModels_[size_t(v.slot)]->poseBlend(renderer_, clip, v.clipTime, clip, v.clipTime, 0.0f);
    }
}

void GameApp::renderVillagers(bool shadow, const Vec3& eye, float range) {
    for (const auto& v : villagers_.villagers) {
        if (v.slot < 0 || length(v.pos - eye) > range) continue;
        const Mat4 xf = Mat4::translation(v.pos + Vec3{0, villagerLift_[size_t(v.slot)], 0}) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, v.yaw));
        if (shadow) villagerModels_[size_t(v.slot)]->renderShadow(renderer_, xf);
        else villagerModels_[size_t(v.slot)]->render(renderer_, xf);
    }
}

}  // namespace gvr
