// GameApp: the main menu (title screen) and the loading-screen art. Same class; see game_app.h
// ("Main menu").
#include "game/game_app.h"

#include <cstdlib>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/log.h"
#include "scene/asset_world.h"
#include "scene/model_cache.h"

namespace gvr {

namespace {
enum TitleRow { kTitleContinue, kTitleNewGame, kTitleMission, kTitleOptions, kTitleRowCount };
// Where the original's menu camera rests (flk_intro_island_2, camera01 and its target, game
// units): above the sea off the island, looking up at the logo.
constexpr float kCamGame[3] = {-3231.0f, 4058.0f, 374.0f};
constexpr float kTargetGame[3] = {2759.0f, -2021.0f, 2869.0f};
const char* const kStoryWorld[3] = {"Story1", "Rstory1", "Kstory1"};
const char* const kStoryName[3] = {"MECC STORY (BAZ)", "REAPER STORY (DELPHI)", "KABUTO STORY"};
}  // namespace

TextureId GameApp::loadingArt(Hero h) {
    if (!assetWorld_ || !assetWorld_->models()) return 0;
    // The original loading screens, one per race (xx_giants_logo_3d.gzp).
    const char* name = h == Hero::Delphi ? "reaper_loading" : (h == Hero::Kabuto ? "kabuto_loading" : "mecc_loading");
    return assetWorld_->models()->texture(name);
}

void GameApp::enterTitle() {
    title_ = true;
    titleViewSet_ = false;
    titleT_ = 0;
    titleHold_ = 0;
    titleActHeld_ = true;  // a trigger still held from the app start doesn't select
    playMode_ = false;
    titleRow_ = titleWorld_.empty() ? kTitleNewGame : kTitleContinue;
    const Hero h = titleWorld_.empty() ? Hero::Mecc : heroForWorld(titleWorld_);
    titleCampaign_ = h == Hero::Delphi ? 1 : (h == Hero::Kabuto ? 2 : 0);
    const Vec3 cam = giantsToRuntime(kCamGame[0], kCamGame[1], kCamGame[2]);
    const Vec3 tgt = giantsToRuntime(kTargetGame[0], kTargetGame[1], kTargetGame[2]);
    titleEye_ = cam;
    titleYaw_ = std::atan2(-(tgt.x - cam.x), -(tgt.z - cam.z));
    if (assetWorld_ && assetWorld_->models()) logoModel_ = assetWorld_->models()->get("Giants_logo_3D.gbs");
    // Mission select (ours): every mission of the three stories in campaign order; starts at the
    // checkpoint's mission when there is one.
    if (assetWorld_ && titleMissions_.empty()) titleMissions_ = assetWorld_->campaignMissions();
    titleMissionSel_ = 0;
    for (size_t i = 0; i < titleMissions_.size(); ++i)
        if (toLower(titleMissions_[i].second) == toLower(titleMission_)) titleMissionSel_ = int(i);
    LOGI("Main menu: %zu missions to choose from", titleMissions_.size());
    if (std::getenv("GVR_LIST_MISSIONS"))
        for (size_t i = 0; i < titleMissions_.size(); ++i) LOGI("  mission %zu: %s / %s", i + 1, titleMissions_[i].first.c_str(), titleMissions_[i].second.c_str());
    LOGI("Main menu (%s), continue: %s", logoModel_ ? "logo" : "no logo", titleWorld_.empty() ? "none" : titleWorld_.c_str());
}

void GameApp::startFromTitle(const std::string& world, const std::string& mission, bool restore, const InputFrame& in) {
    title_ = false;
    haveRestore_ = restore;
    firstBriefDone_ = true;  // the loading screen shows the briefing
    if (!restore) {
        if (!savePath_.empty()) std::remove(savePath_.c_str());
        syringes_ = 0;
        flares_ = 0;
        hasLauncher_ = false;
        weaponSel_ = 0;
    }
    setPlayMode(true, in);
    beginLoading(std::string(), in);
    load_.world = world;
    load_.worldMission = mission;
    load_.title = world;
    LOGI("Main menu: %s %s / %s", restore ? "continue" : "new game", world.c_str(), mission.empty() ? "(first)" : mission.c_str());
}

void GameApp::updateTitle(float dt, const InputFrame& in) {
    titleT_ += dt;
    if (in.headValid && !titleViewSet_) {
        rig_.teleportHead(in, titleEye_ - Vec3{0, std::max(1.0f, in.head.position.y), 0}, titleYaw_);
        titleViewSet_ = true;
    }
    if (in.headValid) headWorldRot_ = rig_.toWorld(in.head).orientation;
    if (assetWorld_) assetWorld_->update(dt);
    // The options (from OPTIONS) work as in the game; their NEW GAME starts a story too.
    if (menuOpen_) {
        if (menuEdge_.pressed(in.hand[kLeft].menu)) menuOpen_ = false;
        else updateMenu(dt, in);
        if (load_.phase != LoadPhase::Idle) title_ = false;
        titleActHeld_ = true;
        return;
    }
    Vec2 st = in.hand[0].thumbstick;
    if (std::fabs(in.hand[1].thumbstick.x) + std::fabs(in.hand[1].thumbstick.y) > std::fabs(st.x) + std::fabs(st.y)) st = in.hand[1].thumbstick;
    int dy = st.y > 0.6f ? -1 : (st.y < -0.6f ? 1 : 0), dx = st.x > 0.6f ? 1 : (st.x < -0.6f ? -1 : 0);
    if (dx == 0 && dy == 0) {
        titleRepeat_ = 0;
        titleFirst_ = true;
    } else {
        titleRepeat_ -= dt;
        if (titleRepeat_ > 0) dx = dy = 0;
        else { titleRepeat_ = titleFirst_ ? 0.4f : 0.16f; titleFirst_ = false; }
    }
    const int first = titleWorld_.empty() ? kTitleNewGame : kTitleContinue;
    if (dy != 0) {
        titleRow_ = std::max(first, std::min(int(kTitleRowCount) - 1, titleRow_ + dy));
        haptics_.push_back({kRight, 0.2f, 0.02f});
        sfx2D("ph_selct", 0.4f);
        titleHold_ = 0;
    }
    if (dx != 0 && titleRow_ == kTitleMission && !titleMissions_.empty()) {
        const int n = int(titleMissions_.size());
        titleMissionSel_ = (titleMissionSel_ + dx + n) % n;
        haptics_.push_back({kRight, 0.3f, 0.03f});
        sfx2D("ph_selct", 0.6f, 1.2f);
        titleHold_ = 0;
    }
    if (dx != 0 && titleRow_ == kTitleNewGame) {
        titleCampaign_ = (titleCampaign_ + dx + 3) % 3;
        haptics_.push_back({kRight, 0.3f, 0.03f});
        sfx2D("ph_selct", 0.6f, 1.2f);
        titleHold_ = 0;
    }
    // A or either trigger: select. A new game over an existing checkpoint is held 1.5 s.
    const bool act = in.hand[1].primary || in.hand[0].trigger > 0.6f || in.hand[1].trigger > 0.6f;
    const bool missionRow = titleRow_ == kTitleMission && !titleMissions_.empty();
    if ((titleRow_ == kTitleNewGame || missionRow) && act && !titleWorld_.empty()) {
        titleHold_ += dt;
        if (titleHold_ > 1.5f) {
            haptics_.push_back({kLeft, 1.0f, 0.3f});
            haptics_.push_back({kRight, 1.0f, 0.3f});
            if (missionRow) {
                const auto& wm = titleMissions_[size_t(titleMissionSel_)];
                startFromTitle(wm.first, wm.second, false, in);
            } else {
                startFromTitle(kStoryWorld[titleCampaign_], std::string(), false, in);
            }
        }
    } else {
        titleHold_ = 0;
        if (act && !titleActHeld_) {
            haptics_.push_back({kRight, 0.6f, 0.08f});
            if (titleRow_ == kTitleContinue && !titleWorld_.empty()) startFromTitle(titleWorld_, titleMission_, true, in);
            else if (titleRow_ == kTitleNewGame) startFromTitle(kStoryWorld[titleCampaign_], std::string(), false, in);
            else if (missionRow) startFromTitle(titleMissions_[size_t(titleMissionSel_)].first, titleMissions_[size_t(titleMissionSel_)].second, false, in);
            else if (titleRow_ == kTitleOptions) openMenu(in);
        }
    }
    titleActHeld_ = act;
}

void GameApp::renderTitle() {
    // The GIANTS logo in the sky above the island, toward where the original's camera looks,
    // facing the viewer, gently bobbing; unlit (it stands out against the night sky).
    if (logoModel_) {
        const Vec3 tgt = giantsToRuntime(kTargetGame[0], kTargetGame[1], kTargetGame[2]);
        const Vec3 dir = normalize(tgt - titleEye_);
        Vec3 flat{dir.x, 0, dir.z};
        flat = normalize(flat);
        // 110 m out, raised 13 degrees (the original looks up ~16).
        const float a = 13.0f * kPi / 180.0f, d = 110.0f;
        const Vec3 at = titleEye_ + flat * (d * std::cos(a)) + Vec3{0, d * std::sin(a) + 1.5f * std::sin(titleT_ * 0.6f), 0};
        const float sway = 0.06f * std::sin(titleT_ * 0.35f);
        const Mat4 xf = Mat4::translation(at) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, titleYaw_ + kPi + sway));
        for (const auto& part : logoModel_->parts) {
            DrawParams dp;
            dp.texture = part.texture;
            dp.blend = part.blend;
            dp.unlit = true;
            dp.noFog = true;
            dp.doubleSided = true;
            renderer_.draw(part.mesh, xf, dp);
        }
    }
    if (!text_.ready() || menuOpen_) return;
    // The menu: world-fixed in front of the start view, a little below eye level.
    const Quat r = Quat::fromAxisAngle({0, 1, 0}, titleYaw_);
    const Vec3 base = titleEye_ + r.rotate(Vec3{0, -0.55f, -1.5f});
    const Quat tilt = r * Quat::fromAxisAngle({1, 0, 0}, 0.32f);  // tipped back toward the eyes
    const Mat4 panel = Mat4::translation(base) * Mat4::rotation(tilt);
    const float w = 1.05f, rowH = 0.11f, h = rowH * float(kTitleRowCount) + 0.16f;
    text_.drawPanel(panel, w, h, {0, 0, 0, 0.72f});
    std::string cont = "CONTINUE";
    if (!titleWorld_.empty()) {
        std::string t, b;
        missionText(titleMission_, t, b);
        cont += "  -  " + (t.empty() ? titleMission_ : t);
    }
    const std::string story = kStoryName[titleCampaign_];
    const std::string ng = titleHold_ > 0.05f && titleRow_ == kTitleNewGame ? "HOLD... " + std::to_string(int(std::ceil(1.5f - titleHold_))) : "NEW GAME:  < " + story + " >";
    std::string ms = "MISSION: (none)";
    if (!titleMissions_.empty()) {
        const auto& wm = titleMissions_[size_t(titleMissionSel_)];
        std::string t, b;
        missionText(wm.second, t, b);
        ms = titleHold_ > 0.05f && titleRow_ == kTitleMission ? "HOLD... " + std::to_string(int(std::ceil(1.5f - titleHold_)))
                                                              : "MISSION:  < " + std::to_string(titleMissionSel_ + 1) + ". " + (t.empty() ? wm.second : t) + " >";
    }
    const std::string rows[kTitleRowCount] = {cont, ng, ms, "OPTIONS"};
    float tw, th;
    for (int i = 0; i < kTitleRowCount; ++i) {
        const float y = 0.5f * h - 0.08f - rowH * (0.5f + float(i));
        const bool usable = (i != kTitleContinue || !titleWorld_.empty()) && (i != kTitleMission || !titleMissions_.empty());
        const bool sel = i == titleRow_;
        if (sel) text_.drawPanel(panel * Mat4::translation({0, y, 0.002f}), w - 0.06f, rowH * 0.9f, {0.25f, 0.55f, 0.25f, 0.6f});
        const Vec4 col = !usable ? Vec4{0.45f, 0.45f, 0.45f, 1} : (sel ? Vec4{1, 1, 0.8f, 1} : Vec4{0.85f, 0.85f, 0.85f, 1});
        if (const MeshId m = text_.mesh(rows[i], 0.055f, w - 0.1f, &tw, &th)) text_.draw(m, panel * Mat4::translation({0, y, 0.004f}), col);
    }
    if (const MeshId m = text_.mesh("STICK: CHOOSE    TRIGGER: SELECT", 0.03f, 1.0f, &tw, &th))
        text_.draw(m, panel * Mat4::translation({0, -0.5f * h + 0.04f, 0.004f}), {0.7f, 0.7f, 0.7f, 1});
    // The chosen story's loading art beside the menu.
    const Hero h2 = titleRow_ == kTitleContinue && !titleWorld_.empty() ? heroForWorld(titleWorld_)
                                                                         : (titleCampaign_ == 1 ? Hero::Delphi : (titleCampaign_ == 2 ? Hero::Kabuto : Hero::Mecc));
    if (const TextureId art = loadingArt(h2)) {
        const Mat4 card = Mat4::translation(titleEye_ + r.rotate(Vec3{0.92f, -0.45f, -1.6f})) * Mat4::rotation(r * Quat::fromAxisAngle({0, 1, 0}, -0.35f));
        text_.drawImage(card, 0.64f, 0.48f, art, {1, 1, 1, 1});
    }
}

std::string GameApp::ambientFor(const std::string& world) const {
    // Race prefix from the hero of the world; a1 / a2 alternate by chapter (ours: which ambient
    // the original plays where is decided in its native code, not decoded).
    const std::string w = toLower(world);
    const char pre = w.rfind("kstory", 0) == 0 ? 'k' : (w.rfind("rstory", 0) == 0 ? 'r' : 'm');
    int chapter = 1;
    for (char c : w)
        if (c >= '0' && c <= '9') { chapter = c - '0'; break; }
    if (w == "intro_island") return "ma2";  // the main menu (ours)
    // A world that names its own ambient set (op 108; only Story2: msu) gets it.
    if (assetWorld_ && toLower(assetWorld_->worldName()) == w && !assetWorld_->world().music[0].empty())
        return toLower(assetWorld_->world().music[0]);
    return std::string(1, pre) + (chapter % 2 == 1 ? "a1" : "a2");
}

}  // namespace gvr
