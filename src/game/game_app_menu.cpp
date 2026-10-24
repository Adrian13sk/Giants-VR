// GameApp: the in-headset options menu (left controller menu button) and applying the settings.
#include "game/game_app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/log.h"
#include "audio/mixer.h"
#include "scene/asset_world.h"

namespace gvr {

namespace {
enum MenuRow { kRowResume, kRowTurn, kRowHand, kRowView, kRowHud, kRowVolume, kRowMusic, kRowSubs, kRowFps, kRowFov, kRowDay, kRowNewGame, kRowCount };
}

void GameApp::applySettings() {
    const UserSettings& s = settings_;
    rig_.comfort.snapTurn = s.turn <= 1;
    rig_.comfort.snapTurnDegrees = s.turn == 1 ? 45.0f : 30.0f;
    rig_.comfort.smoothTurnDegPerSec = s.turn == 3 ? 150.0f : 90.0f;
    rig_.comfort.leftHanded = s.leftHanded;
    if (audio_) {
        audio_->setMasterVolume(0.85f * float(s.volume) / 9.0f);
        audio_->setMusicVolume(0.8f * float(s.music) / 8.0f);
    }
}

void GameApp::saveSettings() {
    if (!settingsPath_.empty() && !writeSettings(settingsPath_, settings_)) LOGW("Settings could not be written to %s", settingsPath_.c_str());
}

void GameApp::openMenu(const InputFrame& in) {
    menuOpen_ = true;
    menuRow_ = kRowResume;
    menuNewGame_ = 0;
    menuCampaign_ = hero_ == Hero::Delphi ? 1 : (hero_ == Hero::Kabuto ? 2 : 0);
    settings_.thirdPerson = !firstPerson_;
    // World-fixed in front of where the player looks (steady while reading), at eye height.
    const Pose head = rig_.toWorld(in.head);
    menuYaw_ = yawOf(head.orientation);
    menuPos_ = head.position + Quat::fromAxisAngle({0, 1, 0}, menuYaw_).rotate(Vec3{0, -0.1f, -1.3f});
    haptics_.push_back({kLeft, 0.4f, 0.05f});
}

void GameApp::updateMenu(float dt, const InputFrame& in) {
    // Either stick: up/down picks a row, left/right changes it (repeats while held).
    Vec2 st = in.hand[0].thumbstick;
    if (std::fabs(in.hand[1].thumbstick.x) + std::fabs(in.hand[1].thumbstick.y) > std::fabs(st.x) + std::fabs(st.y)) st = in.hand[1].thumbstick;
    int dy = st.y > 0.6f ? -1 : (st.y < -0.6f ? 1 : 0), dx = st.x > 0.6f ? 1 : (st.x < -0.6f ? -1 : 0);
    if (dx == 0 && dy == 0) {
        menuRepeat_ = 0;
        menuFirst_ = true;
    } else {
        menuRepeat_ -= dt;
        if (menuRepeat_ > 0) dx = dy = 0;
        else { menuRepeat_ = menuFirst_ ? 0.4f : 0.16f; menuFirst_ = false; }
    }
    if (dy != 0) {
        menuRow_ = (menuRow_ + dy + kRowCount) % kRowCount;
        haptics_.push_back({kRight, 0.2f, 0.02f});
        sfx2D("ph_selct", 0.4f);
    }
    bool changed = false;
    UserSettings& s = settings_;
    if (dx != 0) {
        switch (menuRow_) {
            case kRowTurn: s.turn = (s.turn + dx + 4) % 4; changed = true; break;
            case kRowHand: s.leftHanded = !s.leftHanded; changed = true; break;
            case kRowView: s.thirdPerson = !s.thirdPerson; firstPerson_ = !s.thirdPerson; changed = true; break;
            case kRowHud: s.hudSize = std::max(0, std::min(2, s.hudSize + dx)); changed = true; break;
            case kRowVolume: s.volume = std::max(0, std::min(10, s.volume + dx)); changed = true; break;
            case kRowMusic: s.music = std::max(0, std::min(10, s.music + dx)); changed = true; break;
            case kRowSubs: s.subtitles = !s.subtitles; changed = true; break;
            case kRowFps: s.showFps = !s.showFps; changed = true; break;
            case kRowFov: s.foveation = std::max(0, std::min(3, s.foveation + dx)); changed = true; break;
            case kRowDay: s.dayNight = !s.dayNight; changed = true; break;
            case kRowNewGame: menuCampaign_ = (menuCampaign_ + dx + 3) % 3; haptics_.push_back({kRight, 0.3f, 0.03f}); break;
            default: break;
        }
        if (changed) {
            applySettings();
            saveSettings();
            haptics_.push_back({kRight, 0.3f, 0.03f});
            sfx2D("ph_selct", 0.6f, 1.2f);
        }
    }
    // A / either trigger: activate (Resume; New game must be held 2 s).
    const bool act = in.hand[1].primary || in.hand[0].trigger > 0.6f || in.hand[1].trigger > 0.6f;
    if (menuRow_ == kRowResume && act && !menuActHeld_) menuOpen_ = false;
    if (menuRow_ == kRowNewGame && act) {
        menuNewGame_ += dt;
        if (menuNewGame_ > 2.0f) {
            menuOpen_ = false;
            menuNewGame_ = 0;
            haptics_.push_back({kLeft, 1.0f, 0.4f});
            haptics_.push_back({kRight, 1.0f, 0.4f});
            const std::string world = menuCampaign_ == 1 ? "Rstory1" : (menuCampaign_ == 2 ? "Kstory1" : "Story1");
            LOGI("New game (menu): %s", world.c_str());
            if (!savePath_.empty()) std::remove(savePath_.c_str());
            syringes_ = 0;
            flares_ = 0;
            resetArsenal();
            if (!playMode_) setPlayMode(true, in);
            if (toLower(worldName_) == toLower(world) && assetWorld_ && !assetWorld_->missions().empty()) {
                beginLoading(assetWorld_->missions().front(), in);
            } else {
                beginLoading(std::string(), in);
                load_.world = world;
                load_.title = world;
            }
        }
    } else {
        menuNewGame_ = 0;
    }
    menuActHeld_ = act;
}

void GameApp::renderMenu() {
    if (!text_.ready()) return;
    const Quat r = Quat::fromAxisAngle({0, 1, 0}, menuYaw_);
    const Mat4 base = Mat4::translation(menuPos_) * Mat4::rotation(r);
    const UserSettings& s = settings_;
    static const char* turns[] = {"SNAP 30", "SNAP 45", "SMOOTH, SLOW", "SMOOTH, FAST"};
    static const char* huds[] = {"SMALL", "NORMAL", "LARGE"};
    static const char* fovs[] = {"OFF (SHARPEST)", "LOW", "MEDIUM", "HIGH (FASTEST)"};
    char vol[32], mus[32];
    std::snprintf(vol, sizeof vol, "%d%%", s.volume * 10);
    std::snprintf(mus, sizeof mus, "%d%%", s.music * 10);
    const std::string story = menuCampaign_ == 1 ? "REAPER STORY (DELPHI)" : (menuCampaign_ == 2 ? "KABUTO STORY" : "MECC STORY (BAZ)");
    const std::string newGame = menuNewGame_ > 0.05f ? "HOLD... " + std::to_string(int(std::ceil(2.0f - menuNewGame_))) : story;
    const std::string rows[kRowCount][2] = {
        {"RESUME", ""},
        {"TURNING", turns[s.turn]},
        {"MAIN HAND", s.leftHanded ? "LEFT" : "RIGHT"},
        {"VIEW", firstPerson_ ? "FIRST PERSON" : "THIRD PERSON"},
        {"HUD SIZE", huds[s.hudSize]},
        {"VOLUME", vol},
        {"MUSIC", mus},
        {"SUBTITLES", s.subtitles ? "ON" : "OFF"},
        {"FRAME RATE", s.showFps ? "SHOWN" : "HIDDEN"},
        {"FOVEATED RENDERING", fovs[s.foveation]},
        {"DAY AND NIGHT", s.dayNight ? "CYCLE" : "ALWAYS DAY"},
        {"NEW GAME", newGame},
    };
    const float rowH = 0.085f, w = 1.15f, h = rowH * float(kRowCount + 2) + 0.1f;
    text_.drawPanel(base, w, h, {0, 0, 0, 0.86f});
    float tw, th;
    if (const MeshId t = text_.mesh("OPTIONS", 0.06f, 1.0f, &tw, &th))
        text_.draw(t, base * Mat4::translation({0, 0.5f * h - 0.07f, 0.004f}), {1.0f, 0.85f, 0.4f, 1.0f});
    for (int i = 0; i < kRowCount; ++i) {
        const float y = 0.5f * h - 0.17f - float(i) * rowH;
        const bool sel = i == menuRow_;
        if (sel) text_.drawPanel(base * Mat4::translation({0, y, 0.002f}), w - 0.06f, rowH * 0.9f, {0.25f, 0.55f, 0.25f, 0.6f});
        const Vec4 col = sel ? Vec4{1, 1, 0.8f, 1} : Vec4{0.85f, 0.85f, 0.85f, 1};
        if (const MeshId m = text_.mesh(rows[i][0], 0.05f, 0.7f, &tw, &th))
            text_.draw(m, base * Mat4::translation({-0.5f * w + 0.06f + 0.5f * tw, y, 0.004f}), col);
        if (!rows[i][1].empty()) {
            const std::string v = sel && menuNewGame_ <= 0.05f ? "< " + rows[i][1] + " >" : rows[i][1];
            if (const MeshId m = text_.mesh(v, 0.05f, 0.7f, &tw, &th))
                text_.draw(m, base * Mat4::translation({0.5f * w - 0.06f - 0.5f * tw, y, 0.004f}), col);
        }
    }
    if (const MeshId m = text_.mesh("STICK: CHOOSE / CHANGE    NEW GAME: HOLD TRIGGER 2 S    MENU: CLOSE", 0.032f, 1.1f, &tw, &th))
        text_.draw(m, base * Mat4::translation({0, -0.5f * h + 0.045f, 0.004f}), {0.7f, 0.7f, 0.7f, 1});
}

}  // namespace gvr
