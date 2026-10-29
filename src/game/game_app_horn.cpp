// (2026-10-29) The Guards' alarm horn and the guard launchers: story3_launcher_5's briefing ("Look out for
// HORN-WIELDING REAPER GUARDS. Once they sound an alert, Guard reinforcements will be on their way. Watch the skies!")
// and its three log_launcher_base objectives. The data has the horn (Rpm_Horn.gbs, RN_HORN.wav) but its rules are in
// the code we don't have; ours: while a launcher stands, the first guard to notice the player (or one that has, once
// the last alarm is 45 s old) blows the horn; 4 s later each standing launcher sends two guards down from the sky on
// hover packs near the player. Three alarms at most; destroying the launchers stops them.
#include "game/game_app.h"

#include <algorithm>
#include <cmath>

#include "core/log.h"
#include "scene/asset_world.h"
#include "scene/terrain.h"

namespace gvr {

void GameApp::initHorn() {
    hornLaunchers_.clear();
    hornCool_ = 0;
    hornWaveT_ = -1;
    hornAlarms_ = 0;
    if (!assetWorld_) return;
    const auto& objs = assetWorld_->worldObjects();
    for (size_t i = 0; i < destruct_.items.size(); ++i) {
        const int o = destruct_.items[i].object;
        if (o >= 0 && size_t(o) < objs.size() && objs[size_t(o)].name == "log_launcher_base") hornLaunchers_.push_back(int(i));
    }
    if (!hornLaunchers_.empty()) LOGI("Guard launchers: %zu (the guards' horn calls reinforcements from them)", hornLaunchers_.size());
}

void GameApp::updateHorn(float dt) {
    if (hornLaunchers_.empty() || dying_) return;
    int standing = 0;
    for (int d : hornLaunchers_) standing += destruct_.items[size_t(d)].alive() ? 1 : 0;
    hornCool_ = std::max(0.0f, hornCool_ - dt);
    if (hornWaveT_ >= 0) {
        hornWaveT_ -= dt;
        if (hornWaveT_ < 0 && standing > 0) {
            int placed = 0;
            for (int d : hornLaunchers_) {
                const Destructible& l = destruct_.items[size_t(d)];
                if (!l.alive()) continue;
                sfx("me_mtrsh", l.base + Vec3{0, 4, 0}, 1.0f, 0.6f);
                smokePuff(FxTex::SmokeBlack, l.base + Vec3{0, 5, 0}, Vec3{0, 6, 0}, 2.0f, 2.5f, 3.0f, 0.6f, 0.5f);
                for (int k = 0; k < 2; ++k) {
                    for (int attempt = 0; attempt < 10; ++attempt) {
                        const float a = frand() * 6.2832f, r = 14.0f + 12.0f * frand();
                        const Vec3 p = player_.position + Vec3{std::cos(a) * r, 0, std::sin(a) * r};
                        float h;
                        if (!assetWorld_->terrain().heightAt(p.x, p.z, h) || h < player_.seaLevel + 0.2f || std::fabs(h - player_.position.y) > 25.0f) continue;
                        const Vec3 to = player_.position - p;
                        const int ei = enemies_.spawn(Vec3{p.x, h, p.z}, std::atan2(-to.x, -to.z));
                        if (ei < 0) break;
                        Enemy& e = enemies_.enemies[size_t(ei)];
                        e.aware = true;
                        e.alert = 0.3f;
                        e.home = e.patrolTo = player_.position;
                        e.dropping = true;
                        e.pos.y = h + 20.0f + 6.0f * frand();
                        ++placed;
                        break;
                    }
                }
            }
            timmyLine_ = "REINFORCEMENTS - WATCH THE SKIES!";
            timmyLineT_ = 3.0f;
            LOGI("Guard launchers: %d guards sent from %d launcher(s)", placed, standing);
        }
    }
    if (standing == 0 || hornCool_ > 0 || hornWaveT_ >= 0 || hornAlarms_ >= 3) return;
    // The nearest guard that has seen the player (within 90 m) sounds the alarm.
    int best = -1;
    float bestD = 90.0f;
    for (size_t i = 0; i < enemies_.enemies.size(); ++i) {
        const Enemy& e = enemies_.enemies[i];
        if (e.state == EnemyState::Dead || !e.aware || e.dropping) continue;
        const float d = length(e.pos - player_.position);
        if (d < bestD) { bestD = d; best = int(i); }
    }
    if (best < 0) return;
    const Enemy& e = enemies_.enemies[size_t(best)];
    sfx("rn_horn", e.pos + Vec3{0, 1.8f, 0}, 1.0f, 1.0f);
    enemies_.alertNear(e.pos, 60.0f);
    hornCool_ = 45.0f;
    hornWaveT_ = 4.0f;
    ++hornAlarms_;
    timmyLine_ = "A GUARD SOUNDS THE ALARM!";
    timmyLineT_ = 2.5f;
    LOGI("Guard horn: Reaper %u sounds the alarm (%.0f m away; %d launcher(s) standing)", e.uid, bestD, standing);
}

}  // namespace gvr
