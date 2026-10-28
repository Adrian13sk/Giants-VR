// GameApp: the in-headset options menu (left controller menu button) and applying the settings.
#include "game/game_app.h"

#include <cstdlib>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/log.h"
#include "audio/mixer.h"
#include "scene/asset_world.h"

namespace gvr {

// A thin gold edge round a menu panel (rework): four strips just outside it.
void GameApp::drawPanelEdge(const Mat4& xf, float w, float h) {
    const float b = 0.008f;
    const Vec4 gold{0.85f, 0.62f, 0.22f, 0.85f};
    text_.drawPanel(xf * Mat4::translation({0, 0.5f * h + 0.5f * b, 0.001f}), w + 2 * b, b, gold);
    text_.drawPanel(xf * Mat4::translation({0, -0.5f * h - 0.5f * b, 0.001f}), w + 2 * b, b, gold);
    text_.drawPanel(xf * Mat4::translation({-0.5f * w - 0.5f * b, 0, 0.001f}), b, h, gold);
    text_.drawPanel(xf * Mat4::translation({0.5f * w + 0.5f * b, 0, 0.001f}), b, h, gold);
}

namespace {
enum MenuRow { kRowResume, kRowControls, kRowTurn, kRowMove, kRowVignette, kRowAmbush, kRowLaser, kRowReach, kRowSeated, kRowLook, kRowHand, kRowView, kRowHud, kRowVolume, kRowMusic, kRowAmbience, kRowSubs, kRowCuts, kRowFps, kRowFov, kRowDay, kRowFx, kRowMic, kRowNewGame, kRowCount };
}

void GameApp::applySettings() {
    const UserSettings& s = settings_;
    rig_.comfort.snapTurn = s.turn <= 1;
    rig_.comfort.snapTurnDegrees = s.turn == 1 ? 45.0f : 30.0f;
    rig_.comfort.smoothTurnDegPerSec = s.turn == 3 ? 150.0f : 90.0f;
    rig_.comfort.leftHanded = s.leftHanded;
    if (s.seated) LOGI("Seated play: the view and hands raised %.2f m", seatLift());
    if (!std::getenv("GVR_LOOK")) renderer_.setLook(s.classicLook ? 0 : (quality_ >= 2 ? 2 : 1));
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
    menuControls_ = false;
    menuRow_ = kRowResume;
    menuNewGame_ = 0;
    menuCampaign_ = hero_ == Hero::Delphi ? 1 : (hero_ == Hero::Kabuto ? 2 : 0);
    settings_.thirdPerson = !firstPerson_;
    // World-fixed in front of where the player looks (steady while reading), at eye height.
    const Pose head = rig_.toWorld(in.head);
    menuYaw_ = yawOf(head.orientation);
    menuPos_ = head.position + Quat::fromAxisAngle({0, 1, 0}, menuYaw_).rotate(Vec3{0, -0.1f, -1.3f});
    haptics_.push_back({kLeft, 0.4f, 0.05f});
    // (A trigger still held from choosing OPTIONS on the main menu must not press RESUME at once: it
    // closed the options the frame they opened - found by the ui_title_options test, 2026-10-27.)
    menuActHeld_ = true;
    LOGI("Options menu opened%s", title_ ? " (main menu)" : "");
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
    // The controls card: shown over the options; any press (or B, or the stick) closes it.
    if (menuControls_) {
        const bool act = in.hand[1].primary || in.hand[0].trigger > 0.6f || in.hand[1].trigger > 0.6f;
        if ((act && !menuActHeld_) || in.hand[1].secondary || dx != 0 || dy != 0) menuControls_ = false;
        menuActHeld_ = act;
        return;
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
            case kRowMove: s.teleport = !s.teleport; changed = true; break;
            case kRowVignette: s.vignette = (s.vignette + dx + 3) % 3; changed = true; break;
            case kRowAmbush: s.ambushes = !s.ambushes; changed = true; break;
            case kRowLaser: s.aimLaser = !s.aimLaser; changed = true; break;
            case kRowReach: s.reachAssist = !s.reachAssist; changed = true; break;
            case kRowSeated: s.seated = !s.seated; changed = true; break;
            case kRowLook: s.classicLook = !s.classicLook; changed = true; break;
            case kRowHand: s.leftHanded = !s.leftHanded; changed = true; break;
            case kRowView: s.thirdPerson = !s.thirdPerson; firstPerson_ = !s.thirdPerson; changed = true; break;
            case kRowHud: s.hudSize = std::max(0, std::min(2, s.hudSize + dx)); changed = true; break;
            case kRowVolume: s.volume = std::max(0, std::min(10, s.volume + dx)); changed = true; break;
            case kRowMusic: s.music = std::max(0, std::min(10, s.music + dx)); changed = true; break;
            case kRowAmbience: s.ambience = (s.ambience + dx + 3) % 3; changed = true; break;
            case kRowSubs: s.subtitles = !s.subtitles; changed = true; break;
            case kRowCuts: s.cutsceneScreen = !s.cutsceneScreen; changed = true; break;
            case kRowFps: s.showFps = !s.showFps; changed = true; break;
            case kRowFov: s.foveation = std::max(0, std::min(3, s.foveation + dx)); changed = true; break;
            case kRowDay: s.dayNight = !s.dayNight; changed = true; break;
            case kRowFx: s.originalFx = !s.originalFx; changed = true; break;
            case kRowMic: s.micRoar = !s.micRoar; changed = true; break;
            case kRowNewGame: menuCampaign_ = (menuCampaign_ + dx + 3) % 3; haptics_.push_back({kRight, 0.3f, 0.03f}); break;
            default: break;
        }
        if (changed) {
            static const char* kRowNames[kRowCount] = {"resume", "controls", "turn", "movement", "vignette", "ambushes", "aim laser", "reach assist", "seated", "classic look", "hand", "view", "hud size", "volume", "music", "ambience", "subtitles", "cutscene screen", "fps", "foveation", "day and night", "original fx", "mic roar", "new game"};
            LOGI("Options: %s changed", kRowNames[menuRow_]);
            applySettings();
            saveSettings();
            haptics_.push_back({kRight, 0.3f, 0.03f});
            sfx2D("ph_selct", 0.6f, 1.2f);
        }
    }
    // A / either trigger: activate (Resume; New game must be held 2 s).
    const bool act = in.hand[1].primary || in.hand[0].trigger > 0.6f || in.hand[1].trigger > 0.6f;
    if (menuRow_ == kRowControls && act && !menuActHeld_) {
        menuControls_ = true;
        sfx2D("ph_selct", 0.6f, 1.2f);
        LOGI("Options: controls card shown");
    }
    if (menuRow_ == kRowResume && act && !menuActHeld_) { menuOpen_ = false; LOGI("Options menu closed (resume)"); }
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
    if (menuControls_) { renderControlsCard(base); return; }
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
        {"CONTROLS", hero_ == Hero::Delphi ? "DELPHI" : (hero_ == Hero::Kabuto ? "KABUTO" : "MECCARYN")},
        {"TURNING", turns[s.turn]},
        {"MOVEMENT", s.teleport ? "TELEPORT (STICK UP, LET GO)" : "SMOOTH"},
        {"COMFORT VIGNETTE", s.vignette == 0 ? "OFF" : (s.vignette == 1 ? "LOW" : "HIGH")},
        {"AMBUSHES", s.ambushes ? "ON" : "OFF"},
        {"AIM LASER", s.aimLaser ? "ON" : "OFF"},
        {"GROUND REACH", s.reachAssist ? "EASY (BEND DOWN)" : "NATURAL (CROUCH)"},
        {"PLAY SEATED", s.seated ? "ON (EYES RAISED 45 CM)" : "OFF (STANDING)"},
        {"LIGHTING", s.classicLook ? "CLASSIC" : "REWORK"},
        {"MAIN HAND", s.leftHanded ? "LEFT" : "RIGHT"},
        {"VIEW", firstPerson_ ? "FIRST PERSON" : "THIRD PERSON"},
        {"HUD SIZE", huds[s.hudSize]},
        {"VOLUME", vol},
        {"MUSIC", mus},
        {"AMBIENCE", s.ambience == 0 ? "OFF" : (s.ambience == 1 ? "SOUNDS" : "SOUNDS + WEATHER")},
        {"SUBTITLES", s.subtitles ? "ON" : "OFF"},
        {"CUTSCENES", s.cutsceneScreen ? "CINEMA SCREEN" : "AROUND YOU"},
        {"FRAME RATE", s.showFps ? "SHOWN" : "HIDDEN"},
        {"FOVEATED RENDERING", fovs[s.foveation]},
        {"DAY AND NIGHT", s.dayNight ? "CYCLE" : "ALWAYS DAY"},
        {"EFFECTS", s.originalFx ? "ORIGINAL (TEST)" : "SIMPLE"},
        {"KABUTO MIC ROAR", s.micRoar ? (micAllowed_ ? "ON - MIC READY" : "ON") : "OFF"},
        {"NEW GAME", newGame},
    };
    const float rowH = 0.066f, w = 1.15f, h = rowH * float(kRowCount + 2) + 0.1f;
    text_.drawPanel(base, w, h, {0.015f, 0.03f, 0.055f, 0.9f});
    drawPanelEdge(base, w, h);
    float tw, th;
    if (const MeshId t = text_.mesh("OPTIONS", 0.06f, 1.0f, &tw, &th))
        text_.draw(t, base * Mat4::translation({0, 0.5f * h - 0.07f, 0.004f}), {1.0f, 0.85f, 0.4f, 1.0f});
    for (int i = 0; i < kRowCount; ++i) {
        const float y = 0.5f * h - 0.17f - float(i) * rowH;
        const bool sel = i == menuRow_;
        if (sel) text_.drawPanel(base * Mat4::translation({0, y, 0.002f}), w - 0.06f, rowH * 0.9f, {0.75f, 0.5f, 0.15f, 0.45f});
        const Vec4 col = sel ? Vec4{1, 1, 0.8f, 1} : Vec4{0.85f, 0.85f, 0.85f, 1};
        if (const MeshId m = text_.mesh(rows[i][0], 0.042f, 0.7f, &tw, &th))
            text_.draw(m, base * Mat4::translation({-0.5f * w + 0.06f + 0.5f * tw, y, 0.004f}), col);
        if (!rows[i][1].empty()) {
            const std::string v = sel && menuNewGame_ <= 0.05f ? "< " + rows[i][1] + " >" : rows[i][1];
            if (const MeshId m = text_.mesh(v, 0.042f, 0.7f, &tw, &th))
                text_.draw(m, base * Mat4::translation({0.5f * w - 0.06f - 0.5f * tw, y, 0.004f}), col);
        }
    }
    if (const MeshId m = text_.mesh("STICK: CHOOSE / CHANGE    NEW GAME: HOLD TRIGGER 2 S    MENU: CLOSE", 0.032f, 1.1f, &tw, &th))
        text_.draw(m, base * Mat4::translation({0, -0.5f * h + 0.045f, 0.004f}), {0.7f, 0.7f, 0.7f, 1});
}

// The controls of the hero you play, on one card (Options -> CONTROLS). The rework's hand mechanics are
// easy to miss; this is where they are written down.
void GameApp::renderControlsCard(const Mat4& base) {
    static const char* mecc[][2] = {
        {"LEFT STICK", "RUN (LEFT GRIP HELD: SPRINT)"},
        {"RIGHT STICK", "TURN - PUSH FORWARD: NEXT WEAPON"},
        {"RIGHT TRIGGER", "FIRE"},
        {"A (HOLD)", "JETPACK"},
        {"GUNS", "GRIP AT YOUR RIGHT HIP (HANDGUN) OR OVER YOUR SHOULDER"},
        {"STEADY AIM", "OTHER HAND'S GRIP ON THE BARREL"},
        {"GRENADES", "GRIP AT YOUR LEFT HIP, THROW, LET GO"},
        {"PUNCH", "SWING YOUR FREE HAND INTO HIM"},
        {"PICK UP", "BEND DOWN, GRIP A STONE, BODY, SMARTIE OR GUN"},
        {"REAPER GUN", "PICK ONE UP: THAT HAND'S TRIGGER FIRES IT"},
        {"THROW", "SWING AND LET GO OF THE GRIP"},
        {"SWIM", "HOLD THE GRIPS, PULL YOUR HANDS BACK"},
        {"X / Y", "HEALTH INJECTION / MAP"},
        {"INJECTION", "JAB THE NEEDLE INTO YOUR CHEST OR OTHER ARM"},
        {"WRIST", "LEFT FOREARM ACROSS YOUR FACE: HEALTH, AMMO, GOAL"},
        {"LEFT TRIGGER", "FLARE"},
        {"LEFT STICK DOWN", "MISSION BRIEFING (HOLD)"},
        {"RIGHT STICK CLICK", "FIRST / THIRD PERSON"},
    };
    static const char* delphi[][2] = {
        {"LEFT STICK", "RUN (LEFT GRIP HELD: TURBO)"},
        {"A", "JUMP"},
        {"SWORD", "SWING THE RIGHT HAND - THRUST TO RUN ONE THROUGH"},
        {"PARRY", "MEET A BOLT WITH THE BLADE"},
        {"LEFT TRIGGER TAP", "FIRE BOLT (LET GO IN A THROW TO THROW IT)"},
        {"LEFT TRIGGER HOLD", "FIRE WALL - LONGER: TORNADO"},
        {"SPELL GESTURES", "TRIGGER HELD: SWEEP LOW = WALL, CIRCLE OVERHEAD = TORNADO"},
        {"RIGHT STICK FWD", "SWORD / BOWS"},
        {"BOW", "LEFT HAND HOLDS IT, RIGHT GRIP DRAWS, LET GO"},
        {"FIRE ARROW", "LEFT TRIGGER HELD WHILE DRAWING - OR DIP IT IN FLAMES"},
        {"CLIMB", "GRIP A STEEP FACE, PULL DOWN"},
        {"PICK UP / THROW", "BEND DOWN, GRIP; SWING AND LET GO"},
        {"SWIM", "HOLD THE GRIPS, PULL YOUR HANDS BACK"},
        {"REAPERSKI", "BOTH GRIPS: HANDLEBARS, TWIST RIGHT HAND: GAS"},
        {"WAVE", "HAND OVER YOUR HEAD, SIDE TO SIDE: SMARTIES WAVE"},
        {"WRIST", "LEFT FOREARM ACROSS YOUR FACE: HEALTH, ARROWS, GOAL"},
    };
    static const char* kabuto[][2] = {
        {"LEFT STICK", "WALK (LEFT GRIP HELD: RUN)"},
        {"A", "JUMP - OR JUMP FOR REAL"},
        {"PUNCH", "SWING YOUR FISTS - SLAM DOWN: GROUND POUND"},
        {"GRAB", "REACH DOWN AND GRIP: SMARTIES, REAPERS, TREES, BODIES"},
        {"EAT", "BRING IT TO YOUR MOUTH"},
        {"THROW", "SWING AND LET GO OF THE GRIP"},
        {"X", "GRAB AND EAT THE NEAREST"},
        {"LEFT TRIGGER", "STOMP"},
        {"Y / CHEST BEAT", "ROAR (Y HELD: LAY AN EGG)"},
        {"SHOUT (MIC ROAR ON)", "ROAR"},
        {"BOTH GRIPS", "ADRENALINE (WHEN THE GAUGE IS FULL)"},
        {"CLIMB", "GRIP A CLIFF, PULL DOWN"},
        {"RIGHT TRIGGER", "MAGNIFYING GLASS (WHEN YOU HAVE IT)"},
    };
    const char* (*rows)[2] = mecc;
    int n = int(sizeof(mecc) / sizeof(mecc[0]));
    const char* title = "CONTROLS - MECCARYN";
    if (hero_ == Hero::Delphi) { rows = delphi; n = int(sizeof(delphi) / sizeof(delphi[0])); title = "CONTROLS - DELPHI"; }
    if (hero_ == Hero::Kabuto) { rows = kabuto; n = int(sizeof(kabuto) / sizeof(kabuto[0])); title = "CONTROLS - KABUTO"; }
    const float rowH = 0.058f, w = 1.45f, h = rowH * float(n + 2) + 0.12f;
    text_.drawPanel(base, w, h, {0.015f, 0.03f, 0.055f, 0.94f});
    drawPanelEdge(base, w, h);
    float tw, th;
    if (const MeshId t = text_.mesh(title, 0.055f, 1.3f, &tw, &th))
        text_.draw(t, base * Mat4::translation({0, 0.5f * h - 0.07f, 0.004f}), {1.0f, 0.85f, 0.4f, 1.0f});
    for (int i = 0; i < n; ++i) {
        const float y = 0.5f * h - 0.16f - float(i) * rowH;
        if (const MeshId m = text_.mesh(rows[i][0], 0.036f, 0.42f, &tw, &th))
            text_.draw(m, base * Mat4::translation({-0.5f * w + 0.05f + 0.5f * tw, y, 0.004f}), {1.0f, 0.9f, 0.6f, 1});
        if (const MeshId m = text_.mesh(rows[i][1], 0.036f, 0.9f, &tw, &th))
            text_.draw(m, base * Mat4::translation({-0.5f * w + 0.5f + 0.5f * tw, y, 0.004f}), {0.88f, 0.88f, 0.88f, 1});
    }
    if (const MeshId m = text_.mesh("ANY BUTTON: BACK", 0.03f, 1.0f, &tw, &th))
        text_.draw(m, base * Mat4::translation({0, -0.5f * h + 0.045f, 0.004f}), {0.7f, 0.7f, 0.7f, 1});
}

}  // namespace gvr
