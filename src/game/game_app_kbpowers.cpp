// Kabuto's story powers, from his missions' briefings (rules ours - the original's are native code):
// - Gates that need growth first: k1a "once you've grown, destroy the WOODEN GATE", k1b "once you've
//   gone through another growth spurt, break through the STONE GATE", k2a "once you're at full size,
//   use your ADRENALINE ATTACKS to break through the METAL DOOR". The gates are giantwall1 (wood),
//   giantwall2 (stone) and giantwall3 (metal / steel).
// - ADRENALINE: a meter (shown in the jetpack gauge he doesn't have) filled by fighting and eating;
//   full, at full size, squeezing both grips sets him off for 10 s - double-strength blows and
//   quakes, the only thing that breaks a metal door.
// - OFFSPRING (k2d on: "Each EVIL SMARTIE you eat will allow you to create OFFSPRING - use these
//   darling little creatures to destroy enemies ... they'll grow big and strong"): holding Y lays
//   an egg (offspring_egg_l0) that hatches an Offspring (offspring_l0 and its clips, OF_ sounds) which
//   hunts the nearest Reaper, creature, Vimp or evil Smartie, eats it and grows.
// - The MAGNIFYING GLASS (kstory3_3, kstory4_1: "Smash the GIANT CRYSTAL to get a MAGNIFYING GLASS. Use
//   the MAGNIFYING GLASS to burn through the CRYSTAL GATE ... fry your enemies from a distance"):
//   smashing mag_crystal leaves mag_crystal_shard; walking up to it he takes it; held in the turn
//   hand, that hand's trigger focuses a burning beam where it points. The crystal gate (giantwall4)
//   only gives way to the beam.
#include "game/game_app.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "game/destruction.h"
#include "scene/asset_world.h"
#include "scene/model_cache.h"
#include "scene/skinned_model.h"

namespace gvr {

namespace {
constexpr float kAdrenTime = 10.0f;
constexpr int kMaxOffspring = 4;
}  // namespace

float GameApp::gateNeed(int item, bool* needsAdren) const {
    if (needsAdren) *needsAdren = false;
    if (hero_ != Hero::Kabuto || !assetWorld_ || item < 0 || size_t(item) >= destruct_.items.size()) return 0.0f;
    const int o = destruct_.items[size_t(item)].object;
    if (o < 0 || size_t(o) >= assetWorld_->worldObjects().size()) return 0.0f;
    const std::string& n = assetWorld_->worldObjects()[size_t(o)].name;
    if (n.rfind("giantwall", 0) != 0) return 0.0f;
    const int k = std::atoi(n.c_str() + 9);
    if (k == 1) return 0.7f;
    if (k == 2) return 0.85f;
    if (k == 3) { if (needsAdren) *needsAdren = true; return 1.0f; }
    if (k == 4 && kbGlassAvail_) return 2.0f;  // the crystal gate: only the beam (2 = a marker, see kabutoMayDamage)
    return 0.0f;
}

bool GameApp::kabutoMayDamage(int item) {
    bool adren = false;
    const float need = gateNeed(item, &adren);
    if (need <= 0.0f) return true;
    if (need > 1.5f) {  // the crystal gate
        if (kbBeaming_) return true;
        if (kbGateMsgT_ <= 0) {
            kbGateMsgT_ = 3.0f;
            timmyLine_ = kbHasGlass_ ? "BURN IT WITH THE MAGNIFYING GLASS (RIGHT TRIGGER)" : "THE CRYSTAL GATE WON'T BREAK - SMASH THE GIANT CRYSTAL FOR ITS GLASS";
            timmyLineT_ = 3.0f;
        }
        return false;
    }
    const bool ok = kbGrow_ >= need - 1e-3f && (!adren || kbAdrenT_ > 0);
    if (!ok && kbGateMsgT_ <= 0) {
        kbGateMsgT_ = 3.0f;
        timmyLine_ = kbGrow_ < need - 1e-3f ? std::string("TOO SMALL TO BREAK IT - EAT MORE SMARTIES!")
                                            : std::string("ONLY AN ADRENALINE ATTACK BREAKS THIS DOOR (SQUEEZE BOTH GRIPS)");
        timmyLineT_ = 3.0f;
        sfx2D("ka_mdht1", 0.8f, 0.6f);
        LOGI("Kabuto can't break this gate yet (size %.0f%%, needs %.0f%%%s)", kbGrow_ * 100.0f, need * 100.0f, adren ? " + adrenaline" : "");
    }
    return ok;
}

void GameApp::kabutoGain(float adren) {
    if (hero_ != Hero::Kabuto || kbAdrenT_ > 0) return;
    kbAdren_ = std::min(1.0f, kbAdren_ + adren);
}

bool GameApp::offspringMission() const {
    if (!assetWorld_) return false;
    const std::string m = toLower(assetWorld_->missionName());
    return m.rfind("k2d", 0) == 0 || m.rfind("kstory", 0) == 0;
}

void GameApp::initKabutoPowers() {
    kbHasGlass_ = kbShardOn_ = false;
    kbGlassAvail_ = false;
    if (hero_ == Hero::Kabuto && assetWorld_) {
        for (const auto& o : assetWorld_->worldObjects())
            if (o.name == "mag_crystal" || o.name == "giantwall4" || o.name.rfind("k_rock_gen", 0) == 0) {
                kbGlassAvail_ = kbGlassAvail_ || o.name == "mag_crystal";
                LOGI("Kabuto: %s at game (%.0f %.0f)", o.name.c_str(), -o.centre.x / 0.2f, o.centre.z / 0.2f);
            }
        if (kbGlassAvail_ && !glassModel_) {
            glassModel_ = assetWorld_->models()->get("mag_crystal_shard.gbs", "", true);
            if (glassModel_) {
                glassSize_ = std::max(0.1f, length(glassModel_->boundsMax - glassModel_->boundsMin));
                const Vec3 e = glassModel_->boundsMax - glassModel_->boundsMin;
                LOGI("Magnifying glass shard: %.1f x %.1f x %.1f m", e.x, e.y, e.z);
            }
        }
    }
    if (kbGlassAvail_ && std::getenv("GVR_GLASS")) kbHasGlass_ = true;  // test hook: start holding the glass
    kbAdren_ = std::getenv("GVR_KB_ADREN") ? 1.0f : 0.0f;  // (test hook: adrenaline full at the start)
    kbAdrenT_ = -1;
    kbOffspringCharges_ = 0;
    offspring_.clear();
    if (hero_ != Hero::Kabuto || !assetWorld_ || !offspringMission()) return;
    // A pool of Offspring models, loaded now (behind the loading screen) rather than on the fly.
    for (int i = 0; i < kMaxOffspring; ++i) {
        auto m = std::make_unique<SkinnedModel>();
        if (!m->load(assetWorld_->fs(), *assetWorld_->models(), renderer_, "offspring_l0", "Offspring_stand.anm")) break;
        static const char* clips[6] = {"Offspring_stand.anm", "Offspring_RUN.anm", "Offspring_claw.anm", "Offspring_bite.anm", "Offspring_roar.anm",
                                       "Offspring_die.anm"};
        OffspringPal o;
        o.clip[0] = 0;
        for (int k = 1; k < 6; ++k) o.clip[k] = std::max(0, m->addAnimation(assetWorld_->fs(), clips[k]));
        m->pose(renderer_, 0.0f);
        Vec3 mn, mx;
        m->posedBounds(mn, mx);
        o.h0 = std::max(0.5f, mx.y - mn.y);
        o.lift0 = -mn.y;
        o.model = std::move(m);
        o.alive = false;
        offspring_.push_back(std::move(o));
    }
    if (!eggModel_) eggModel_ = assetWorld_->models()->get("offspring_egg_l0.gbs", "", true);
    if (const char* n = std::getenv("GVR_OFFSPRING")) kbOffspringCharges_ = std::atoi(n);  // test hook
    LOGI("Offspring pool: %zu (egg %d)", offspring_.size(), eggModel_ != nullptr);
}

void GameApp::kabutoAteSmartie() {
    if (hero_ == Hero::Kabuto && offspringMission()) {
        ++kbOffspringCharges_;
        LOGI("Kabuto can lay %d Offspring", kbOffspringCharges_);
    }
}

bool GameApp::layOffspring() {
    if (kbOffspringCharges_ <= 0) return false;
    for (auto& o : offspring_) {
        if (o.alive) continue;
        const float H = player_.height;
        const Vec3 fwd{-std::sin(player_.yaw), 0, -std::cos(player_.yaw)};
        o.pos = player_.position + fwd * (0.35f * H);
        if (player_.groundHeight) o.pos.y = player_.groundHeight(o.pos.x, o.pos.z);
        o.yaw = player_.yaw;
        o.scale = 0.28f * H / o.h0;  // hatches at about a quarter of his height
        o.hatch = 0;
        o.life = 0;
        o.state = 0;
        o.clipT = 0;
        o.cool = 0;
        o.target = -1;
        o.alive = true;
        o.dieT = -1;
        o.retarget = 0;
        o.struck = false;
        --kbOffspringCharges_;
        sfx("of_hatch", o.pos, 1.0f);
        LOGI("Kabuto lays an Offspring egg (%d left)", kbOffspringCharges_);
        return true;
    }
    return false;
}

void GameApp::updateKabutoPowers(float dt, const InputFrame& in) {
    kbGateMsgT_ = std::max(0.0f, kbGateMsgT_ - dt);
    if (hero_ != Hero::Kabuto) return;
    // Adrenaline: fills slowly at full size; both grips squeezed when full sets it off.
    if (kbAdrenT_ > 0) {
        kbAdrenT_ -= dt;
        kbAdren_ = std::max(0.0f, kbAdrenT_ / kAdrenTime);
        // Red light about him and steam off his shoulders.
        EnvironmentParams& env = renderer_.environment();
        env.pointLightPos = player_.position + Vec3{0, 0.8f * player_.height, 0};
        env.pointLightColor = Vec3{1.6f, 0.3f, 0.15f} * (0.8f + 0.2f * std::sin(float(time_) * 8.0f));
        env.pointLightRadius = 0.6f * player_.height;
        if (frand() < dt * 12.0f)
            smokePuff(FxTex::Fire, player_.position + Vec3{(frand() - 0.5f) * 0.3f * player_.height, 0.75f * player_.height, (frand() - 0.5f) * 0.3f * player_.height},
                      Vec3{0, 3.0f, 0}, 0.06f * player_.height, 0.02f * player_.height, 0.5f, 0.9f, 1.0f);
        if (kbAdrenT_ <= 0) { kbAdrenT_ = -1; kbAdren_ = 0; LOGI("Kabuto's adrenaline wears off"); }
    } else {
        if (kbGrow_ >= 0.999f) kbAdren_ = std::min(1.0f, kbAdren_ + dt / 60.0f);
        const bool both = in.hand[0].squeeze > 0.8f && in.hand[1].squeeze > 0.8f && !player_.climbing;  // (not two climbing hands)
        if (both && !kbGripsWas_ && kbAdren_ >= 1.0f) {
            if (kbGrow_ >= 0.999f) {
                kbAdrenT_ = kAdrenTime;
                static const char* roars[] = {"ka_roar3", "ka_roar4"};
                sfx2D(roars[int(frand() * 1.99f)], 1.0f, 0.9f);
                for (int h = 0; h < 2; ++h) haptics_.push_back({h, 1.0f, 0.5f});
                timmyLine_ = "ADRENALINE ATTACK!";
                timmyLineT_ = 2.0f;
                LOGI("Kabuto: ADRENALINE");
            } else {
                timmyLine_ = "GROW TO FULL SIZE FIRST";
                timmyLineT_ = 2.0f;
            }
        }
        kbGripsWas_ = both;
    }
    // Offspring.
    auto ground = [&](const Vec3& p) { return player_.groundHeight ? player_.groundHeight(p.x, p.z) : p.y; };
    for (auto& o : offspring_) {
        if (!o.alive) continue;
        o.life += dt;
        o.clipT += dt;
        o.cool = std::max(0.0f, o.cool - dt);
        if (o.hatch < 1.2f) {  // the egg, then out it comes
            o.hatch += dt;
            if (o.hatch >= 1.2f) { sfx("of_roar1", o.pos, 1.0f, 1.3f); dustBurst(o.pos, 0.05f * player_.height, 6); o.clipT = 0; o.state = 4; }
            continue;
        }
        if (o.dieT >= 0) {
            o.dieT += dt;
            if (o.dieT > 4.0f) o.alive = false;
            continue;
        }
        if (o.life > 90.0f) { o.dieT = 0; o.clipT = 0; o.state = 5; sfx("of_deth1", o.pos, 0.8f); continue; }
        const float h = o.h0 * o.scale;  // its height (m)
        // Target: nearest live Reaper, creature, Vimp or evil Smartie within 120 m (re-picked now and then).
        o.retarget -= dt;
        if (o.retarget <= 0) {
            o.retarget = 0.5f;
            o.target = -1;
            float best = 120.0f;
            for (size_t i = 0; i < enemies_.enemies.size(); ++i)
                if (enemies_.enemies[i].state != EnemyState::Dead) { const float d = length(enemies_.enemies[i].pos - o.pos); if (d < best) { best = d; o.target = int(i); } }
            for (size_t i = 0; i < creatures_.creatures.size(); ++i) {
                const Creature& c = creatures_.creatures[i];
                if (c.state == CreatureState::Dead || CreatureDirector::underground(c)) continue;
                const float d = length(c.pos - o.pos);
                if (d < best) { best = d; o.target = 10000 + int(i); }
            }
            for (size_t i = 0; i < vimps_.vimps.size(); ++i)
                if (vimps_.vimps[i].state != VimpState::Dead) { const float d = length(vimps_.vimps[i].pos - o.pos); if (d < best) { best = d; o.target = 20000 + int(i); } }
            for (size_t i = 0; i < villagers_.villagers.size(); ++i) { if (!VillagerDirector::alive(villagers_.villagers[i])) continue; const float d = length(villagers_.villagers[i].pos - o.pos); if (d < best) { best = d; o.target = 30000 + int(i); } }
        }
        Vec3 goal = player_.position;
        bool hunting = false;
        if (o.target >= 30000 && size_t(o.target - 30000) < villagers_.villagers.size()) { goal = villagers_.villagers[size_t(o.target - 30000)].pos; hunting = true; }
        else if (o.target >= 20000 && size_t(o.target - 20000) < vimps_.vimps.size()) { goal = vimps_.vimps[size_t(o.target - 20000)].pos; hunting = true; }
        else if (o.target >= 10000 && size_t(o.target - 10000) < creatures_.creatures.size()) { goal = creatures_.creatures[size_t(o.target - 10000)].pos; hunting = true; }
        else if (o.target >= 0 && size_t(o.target) < enemies_.enemies.size()) { goal = enemies_.enemies[size_t(o.target)].pos; hunting = true; }
        Vec3 to{goal.x - o.pos.x, 0, goal.z - o.pos.z};
        const float d = length(to);
        const float reach = 0.6f * h;
        const float want = std::atan2(-to.x, -to.z);
        if (o.state == 2 || o.state == 3 || o.state == 4) {  // a blow (or the hatch roar) playing out
            const float len = std::max(0.3f, o.model->animDuration(o.clip[o.state]));
            if (o.state != 4 && !o.struck && o.clipT > len * 0.45f) {
                o.struck = true;
                if (hunting && d < reach + 1.0f) {
                    bool killed = false;
                    const Vec3 dir = d > 1e-3f ? to * (1.0f / d) : Vec3{0, 0, -1};
                    if (o.target >= 30000) {
                        auto& vs = villagers_.villagers;
                        const size_t vi = size_t(o.target - 30000);
                        bleed(vs[vi].pos + Vec3{0, 0.5f, 0}, dir, true, 0.7f);
                        blood_.pool(vs[vi].pos, 0.8f);
                        vs.erase(vs.begin() + long(vi));
                        killed = true;
                    } else if (o.target >= 20000) {
                        hitVimp(o.target - 20000, 30.0f, goal + Vec3{0, 2, 0}, dir);
                        killed = vimps_.vimps[size_t(o.target - 20000)].state == VimpState::Dead;
                    } else if (o.target >= 10000) {
                        killed = creatures_.damage(o.target - 10000, 30.0f, dir);
                        bleed(goal + Vec3{0, 0.5f, 0}, dir, killed, 1.0f);
                    } else {
                        const int obj = enemies_.enemies[size_t(o.target)].objective;
                        killed = enemies_.damage(o.target, 40.0f);
                        bleed(goal + Vec3{0, 1.0f, 0}, dir, killed, 1.0f);
                        if (killed && obj >= 0) completeObjective(obj);
                    }
                    sfx(o.state == 2 ? "of_claw1" : "of_claw2", o.pos, 1.0f);
                    if (killed) {
                        LOGI("An Offspring kills target %d (scale %.2f)", o.target, o.scale);
                        // Eats it and grows (up to twice its hatching size).
                        o.scale = std::min(o.scale * 1.12f, 0.56f * player_.height / o.h0);
                        sfx(frand() < 0.5f ? "of_eat1" : "of_grow", o.pos, 1.0f);
                        o.target = -1;
                        o.retarget = 0;
                    }
                }
            }
            if (o.clipT > len) { o.state = 0; o.clipT = 0; }
        } else if (hunting && d < reach) {
            o.yaw += std::max(-3.0f * dt, std::min(3.0f * dt, angleDelta(o.yaw, want)));
            if (o.cool <= 0) { o.state = frand() < 0.6f ? 2 : 3; o.clipT = 0; o.struck = false; o.cool = 0.8f; }
            else o.state = 0;
        } else if (d > (hunting ? reach : 0.5f * player_.height)) {
            // Runs (fast, scaled with its size) toward its prey, or back to its parent.
            o.yaw += std::max(-3.0f * dt, std::min(3.0f * dt, angleDelta(o.yaw, want)));
            const Vec3 f{-std::sin(o.yaw), 0, -std::cos(o.yaw)};
            const float speed = std::min(d, (hunting ? 14.0f : 10.0f) * std::sqrt(o.scale * o.h0 / 4.0f) * dt);
            const Vec3 np = o.pos + f * speed;
            if (ground(np) > player_.seaLevel - 1.5f) o.pos = np;
            if (o.state != 1) { o.state = 1; o.clipT = 0; }
        } else if (o.state != 0) {
            o.state = 0;
            o.clipT = 0;
        }
        o.pos.y = ground(o.pos);
    }
    // Pose the hatched ones (once per frame, not per eye).
    for (auto& o : offspring_) {
        if (!o.alive || o.hatch < 1.2f || length(o.pos - player_.position) > 600.0f) continue;
        const int c = o.dieT >= 0 ? 5 : o.state;
        const float len = std::max(0.1f, o.model->animDuration(o.clip[c]));
        const float t = c >= 2 ? std::min(o.clipT, len - 0.01f) : std::fmod(o.clipT, len);
        o.model->poseBlend(renderer_, o.clip[c], t, o.clip[c], t, 0.0f);
    }
}

namespace {
Quat upTo(const Vec3& d) {  // the rotation taking +Y onto d
    const Vec3 a = cross(Vec3{0, 1, 0}, d);
    const float l = length(a);
    if (l < 1e-4f) return d.y > 0 ? Quat{} : Quat::fromAxisAngle({1, 0, 0}, 3.14159265f);
    return Quat::fromAxisAngle(a * (1.0f / l), std::atan2(l, d.y));
}
}  // namespace

void GameApp::kabutoObjectDestroyed(const std::string& name, const Vec3& at) {
    if (hero_ != Hero::Kabuto || name != "mag_crystal") return;
    kbShardOn_ = true;
    kbShardAt_ = at;
    if (player_.groundHeight) kbShardAt_.y = player_.groundHeight(at.x, at.z);
    LOGI("The giant crystal shatters: a magnifying glass lies at (%.0f %.0f)", at.x, at.z);
}

void GameApp::updateGlass(float dt, const InputFrame& in) {
    kbBeaming_ = false;
    if (hero_ != Hero::Kabuto) return;
    const float H = player_.height;
    if (kbShardOn_ && !kbHasGlass_) {
        const Vec3 d = kbShardAt_ - player_.position;
        if (d.x * d.x + d.z * d.z < (0.35f * H) * (0.35f * H)) {
            kbHasGlass_ = true;
            kbShardOn_ = false;
            timmyLine_ = "MAGNIFYING GLASS: POINT YOUR RIGHT HAND, HOLD THE TRIGGER TO BURN";
            timmyLineT_ = 4.0f;
            sfx2D("ka_grab", 1.0f);
            LOGI("Kabuto takes the magnifying glass");
        }
    }
    if (!kbHasGlass_) return;
    const int gh = rig_.turnHand();
    const ControllerState& c = in.hand[gh];
    // Where the glass is: in the turn hand (first person) or held out in front (third person).
    if (firstPerson_ && c.gripPoseValid) {
        const Pose g = rig_.toWorld(c.gripPose);
        const Pose a = c.aimPoseValid ? rig_.toWorld(c.aimPose) : g;
        kbGlassAt_ = g.position + a.orientation.rotate(Vec3{0, 0.03f * H, -0.15f * H});
        kbGlassDir_ = a.orientation.rotate(Vec3{0, 0, -1});
    } else {
        const Vec3 fwd{-std::sin(player_.yaw), 0, -std::cos(player_.yaw)};
        kbGlassAt_ = player_.position + fwd * (0.3f * H) + Vec3{0, 0.55f * H, 0};
        kbGlassDir_ = normalize(fwd + Vec3{0, -0.35f, 0});
    }
    if (c.trigger < 0.6f || !weapon_.raycast) { kbBeamT_ = 0; return; }
    // The sun through the glass: a burning point where it lands.
    float t;
    Vec3 n;
    int target = -1;
    const float maxD = 3.0f * H;
    const bool hit = weapon_.raycast(kbGlassAt_, kbGlassDir_, maxD, t, n, target);
    const Vec3 spot = kbGlassAt_ + kbGlassDir_ * (hit ? t : maxD);
    static const bool trace = std::getenv("GVR_GLASS") != nullptr;
    if (trace && int(time_) != int(time_ - dt))
        LOGI("glass trace: H %.1f from (%.0f %.0f %.0f) dir (%.2f %.2f %.2f) hit %d t %.1f target %d", H, kbGlassAt_.x, kbGlassAt_.y, kbGlassAt_.z,
             kbGlassDir_.x, kbGlassDir_.y, kbGlassDir_.z, int(hit), t, target);
    // The focused light: a bright cone narrowing from the lens to the burning point.
    for (int k = 0; k < 8; ++k) {
        const float f = (float(k) + frand()) / 8.0f;
        smokePuff(FxTex::Fire, kbGlassAt_ + (spot - kbGlassAt_) * f, Vec3{0, 0.3f, 0}, 0.03f * H * (1.0f - 0.8f * f), 0.0f, 0.06f, 0.9f, 0.0f);
    }
    if (!hit) return;
    smokePuff(FxTex::Fire, spot, Vec3{0, 2.0f, 0}, 0.06f * H, 0.02f * H, 0.3f, 1.0f, 1.0f);
    {
        EnvironmentParams& env = renderer_.environment();
        env.pointLightPos = spot - kbGlassDir_ * (0.05f * H);
        env.pointLightColor = Vec3{1.8f, 1.2f, 0.5f} * (0.85f + 0.15f * frand());
        env.pointLightRadius = 0.5f * H;
    }
    if (frand() < dt * 10.0f) smokePuff(FxTex::SmokeBlack, spot, Vec3{0, 3.0f, 0}, 0.03f * H, 0.12f * H, 2.0f, 0.5f, 1.0f);
    kbBeamT_ += dt;
    if (kbBeamT_ < 0.25f) return;
    kbBeamT_ = 0;
    sfx("ex_woods", spot, 0.5f, 1.6f);
    haptics_.push_back({gh, 0.3f, 0.05f});
    kbBeaming_ = true;
    if (target >= kObjectTarget) damageObject(target - kObjectTarget, 60.0f, true, kbGlassAt_);
    else if (target >= 0 && weapon_.onHit) { hitMul_ = 1.5f; weapon_.onHit(target, spot, kbGlassDir_); hitMul_ = 1.0f; }
    kbBeaming_ = false;
}

void GameApp::renderKabutoPowers(const Vec3& eye) {
    if (glassModel_ && (kbShardOn_ || kbHasGlass_) && hero_ == Hero::Kabuto) {
        // The shard as the data has it (about 3.5 m across) on the ground; in his hand, a lens
        // about an eighth of his height, edge-on to where it points.
        const Vec3 c = (glassModel_->boundsMin + glassModel_->boundsMax) * 0.5f;
        if (kbShardOn_) {
            drawModel(glassModel_, Mat4::translation(kbShardAt_ + Vec3{0, c.y - glassModel_->boundsMin.y, 0}) *
                                       Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, float(time_) * 0.6f)) * Mat4::translation(c * -1.0f), false);
            // A glint so he can find it.
            if (frand() < 0.3f) smokePuff(FxTex::Fire, kbShardAt_ + Vec3{0, glassSize_ * 0.6f, 0}, Vec3{0, 0.5f, 0}, 0.4f, 0.1f, 0.25f, 0.6f, 0.0f);
        } else if (!dying_) {
            const float k = 0.2f * player_.height / glassSize_;
            drawModel(glassModel_, Mat4::translation(kbGlassAt_) * Mat4::rotation(upTo(kbGlassDir_ * -1.0f))  /* its face (+Y) toward him */ * Mat4::scale(Vec3{k, k, k}) *
                                       Mat4::translation(c * -1.0f), false);
        }
    }
    for (auto& o : offspring_) {
        if (!o.alive || length(o.pos - eye) > 600.0f) continue;
        if (o.hatch < 1.2f) {
            if (eggModel_) {
                const float wob = 0.1f * std::sin(o.hatch * 30.0f) * (o.hatch / 1.2f);
                const float s = o.scale * 0.8f;
                drawModel(eggModel_, Mat4::translation(o.pos) * Mat4::rotation(Quat::fromAxisAngle({0, 0, 1}, wob)) * Mat4::scale(Vec3{s, s, s}), false);
            }
            continue;
        }
        const float sink = o.dieT > 2.0f ? (o.dieT - 2.0f) * 0.3f * o.h0 * o.scale : 0.0f;
        o.model->render(renderer_, Mat4::translation(o.pos + Vec3{0, o.lift0 * o.scale - sink, 0}) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, o.yaw)) *
                                       Mat4::scale(Vec3{o.scale, o.scale, o.scale}));
    }
}

}  // namespace gvr
