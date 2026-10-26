// GameApp, rework: one-time hints for the hand mechanics nobody would guess (picking up bodies, stones
// and dropped guns, swimming by strokes). Each shows once, the first time it applies, as a line under
// the view, and is remembered in the settings file (hints_shown) so it never shows again.
#include <cmath>

#include "core/log.h"
#include "game/game_app.h"

namespace gvr {

namespace {
enum : int { kHintBody = 1, kHintStone = 2, kHintGun = 4, kHintSwim = 8 };
}

void GameApp::updateHints(float dt) {
    const bool can = playMode_ && firstPerson_ && !dying_ && !cutsceneActive() && !menuOpen_ && hero_ != Hero::Kabuto && !riding_;
    if (!can) { hintQuiet_ = 0; return; }
    hintPlay_ += dt;
    swimFor_ = player_.swimming() ? swimFor_ + dt : 0.0f;
    // Not over another line (a briefing, Timmy, a pickup); a few quiet seconds between hints.
    if (timmyLineT_ > 0) { hintQuiet_ = 0; return; }
    hintQuiet_ += dt;
    if (hintQuiet_ < 3.0f) return;
    int& shown = settings_.hintsShown;
    auto show = [&](int bit, const char* line) {
        shown |= bit;
        timmyLine_ = line;
        timmyLineT_ = 6.0f;
        hintQuiet_ = 0;
        saveSettings();
        LOGI("Hint: %s", line);
    };
    if (!(shown & kHintSwim) && swimFor_ > 2.0f) {
        show(kHintSwim, "SWIM FASTER: SQUEEZE BOTH GRIPS AND PULL YOUR HANDS BACK THROUGH THE WATER");
        return;
    }
    if (!(shown & kHintGun))
        for (const auto& g : gibs_)
            if (g.owner && g.hand < 0 && g.life > 1.0f && length(g.pos - player_.position) < 10.0f) {
                show(kHintGun, "HIS GUN IS ON THE GROUND - GRAB IT (GRIP) BEFORE HE GETS IT BACK");
                return;
            }
    if (!(shown & kHintBody))
        for (const auto& e : enemies_.enemies)
            if (e.state == EnemyState::Dead && !e.gibbed && e.stateTime > 2.0f && length(e.pos - player_.position) < 4.0f) {
                show(kHintBody, "BEND DOWN AND SQUEEZE A GRIP ON A BODY TO PICK IT UP - SWING IT AND LET GO TO THROW");
                return;
            }
    if (!(shown & kHintStone) && hintPlay_ > 45.0f && pebbleRadius_ > 0) {
        const int cx = int(std::floor(player_.position.x / kPebbleCell)), cz = int(std::floor(player_.position.z / kPebbleCell));
        for (int gz = cz - 2; gz <= cz + 2; ++gz)
            for (int gx = cx - 2; gx <= cx + 2; ++gx) {
                PebbleDesc d;
                if (!pebbleAt(gx, gz, pebbleCentre_, pebbleRadius_, d) || pebbleTaken(gx, gz)) continue;
                if (2.0f * std::max(d.scale.x, std::max(d.scale.y, d.scale.z)) > 0.34f || length(d.base - player_.position) > 2.5f) continue;
                show(kHintStone, "STONES CAN BE PICKED UP - BEND DOWN, SQUEEZE A GRIP ON ONE AND THROW IT");
                return;
            }
    }
}

}  // namespace gvr
