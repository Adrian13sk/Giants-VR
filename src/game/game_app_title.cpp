// GameApp: the main menu (title screen) and the loading-screen art. Same class; see game_app.h
// ("Main menu").
#include "game/game_app.h"
#include "audio/mixer.h"
#include "game/cutscene.h"
#include "game/progress.h"
#include "game/save_game.h"

#include <cstdlib>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/log.h"
#include "scene/asset_world.h"
#include "scene/model_cache.h"

namespace gvr {

namespace {
enum TitleRow { kTitleContinue, kTitleNewGame, kTitleMission, kTitlePractice, kTitleSkMode, kTitleSkirmish, kTitleSkTeam, kTitleSkAddr, kTitleOptions, kTitleRowCount };
// This headset's own address on the Wi-Fi (the host's differs only in its last number, almost always).
uint32_t ownLanAddress() {
    for (const auto& a : UdpSocket::localAddresses())
        if ((a.ip >> 24) != 127 && a.ip != 0) return a.ip;
    return 0xC0A80100u;  // (192.168.1.x when there's no network: shown, can't work)
}
const char* const kSkModes[3] = {"WITH COMPUTER PLAYERS", "HOST A WI-FI GAME", "JOIN A WI-FI GAME"};
// The original's multiplayer maps (w_M_*), played against computer players (game_app_skirmish.cpp).
const char* const kSkMaps[12] = {"M_3WAY_L1", "M_3WAY_L2", "M_3WAY_L3", "M_3WAY_Lw1", "M_Mecc_L1", "M_Mecc_L2",
                                 "M_Mecc_L3", "M_Mecc_Lw1", "M_Reaper_L1", "M_Reaper_L2", "M_Reaper_L3", "M_Reaper_Lw1"};
std::string skMapLabel(int i) {
    const std::string m = kSkMaps[i];
    const std::string kind = m.find("3WAY") != std::string::npos ? "3-WAY" : (m.find("Mecc") != std::string::npos ? "MECCARYN" : "REAPER");
    return kind + " " + (m.back() == '1' && m[m.size() - 2] == 'w' ? std::string("W1") : std::string(1, m.back()));
}
int skTeams(int map) { return std::string(kSkMaps[map]).find("3WAY") != std::string::npos ? 3 : 2; }
std::string skTeamLabel(int map, int team) {
    static const char* names[3] = {"BLUE", "GREEN", "RED"};
    const std::string m = kSkMaps[map];
    const char* race = m.find("3WAY") != std::string::npos ? (team == 0 ? "MECCARYN" : (team == 1 ? "SEA REAPER" : "KABUTO"))
                                                           : (m.find("Mecc") != std::string::npos ? "MECCARYN" : "SEA REAPER");
    return std::string(names[team % 3]) + " (" + race + ")";
}
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
    hoverRow_ = -2;
    titleHoldRow_ = -1;
    stickChose_ = true;  // (the highlighted row is the menu's own choice until the pointer moves onto another)
    pointDotOn_ = false;
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
    if (assetWorld_ && titleMissions_.empty()) {
        titleMissions_ = assetWorld_->campaignMissions();
        // (2026-10-28) Unlocks: each hero's story up to its first unfinished mission (and the checkpoint's).
        static const char* all = std::getenv("GVR_ALL_MISSIONS");
        if (!(all && all[0] == '1')) {
            const auto open = unlockedMissions(titleMissions_, missionsDone_, titleMission_);
            LOGI("Main menu: %zu of %zu missions unlocked (%zu finished)", open.size(), titleMissions_.size(), missionsDone_.size());
            titleMissions_ = open;
        }
    }
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
        resetArsenal();
    }
    setPlayMode(true, in);
    beginLoading(std::string(), in);
    load_.world = world;
    load_.worldMission = mission;
    load_.title = world;
    LOGI("Main menu: %s %s / %s", restore ? "continue" : "new game", world.c_str(), mission.empty() ? "(first)" : mission.c_str());
}

// (2026-10-28) PRACTICE: the main menu's island (intro_island: no objectives, so the Reaper test waves
// come) with the hero chosen. The save, the progress file and the best times are left alone.
void GameApp::startPractice(Hero h, const InputFrame& in) {
    title_ = false;
    haveRestore_ = false;
    firstBriefDone_ = true;
    practiceHero_ = h;
    syringes_ = 0;
    flares_ = 0;
    resetArsenal();
    setPlayMode(true, in);
    beginLoading(std::string(), in);
    load_.world = "intro_island";
    load_.practice = true;
    load_.title = "PRACTICE";
    LOGI("Main menu: practice as %s", h == Hero::Delphi ? "Delphi" : (h == Hero::Kabuto ? "Kabuto" : "Baz"));
}

// Everything the hero's story hands out over its missions (as mission select carries it), full.
void GameApp::grantPracticeKit() {
    if (!assetWorld_) return;
    std::vector<uint32_t> items;
    int missions = 0;
    for (const auto& wm : assetWorld_->campaignMissions()) {
        if (heroForWorld(wm.first) != practiceHero_) continue;
        const auto it = missionItems(wm.second);
        items.insert(items.end(), it.begin(), it.end());
        ++missions;
    }
    if (practiceHero_ == Hero::Kabuto) items.clear();  // (his fists; kstory5_5's guns are the Meccaryn's)
    grantMissionItems(items, "practice");
    for (auto& g : guns_)
        if (g.have && g.maxAmmo > 0) g.ammo = g.maxAmmo;
    for (auto& b : bows_)
        if (b.have && b.maxAmmo > 0) b.ammo = b.maxAmmo;
    if (hasLauncher_) { launcherAmmo_ = launcherMax_; launcherHoming_ = launcherHomingMax_; }
    if (hero_ == Hero::Mecc && mineModel_) mines_ = minesMax_;
    syringes_ = kMaxSyringes;
    flares_ = kMaxFlares;
    int bows = 0;
    for (const auto& b : bows_) bows += b.have;
    LOGI("Practice: the kit of %d missions (%d bows)", missions, bows);
}

void GameApp::quitToTitle(const InputFrame& in) {
    menuOpen_ = false;
    if (cutsceneActive()) cutscene_->stop();
    // Sounds that loop with the hero's state (the main menu doesn't update them, review 2026-10-28).
    if (audio_) {
        for (uint32_t* v : {&jetLoop_, &slideLoop_, &swimLoop_, &speedWind_, &mgLoop_, &streamVoice_})
            if (*v) audio_->stop(*v);
        for (uint32_t v : cueVoices_) audio_->stop(v);
    }
    jetLoop_ = slideLoop_ = swimLoop_ = speedWind_ = mgLoop_ = streamVoice_ = 0;
    cueVoices_.clear();
    streamLeft_ = 0;
    wasThrusting_ = wasSliding_ = false;
    // The CONTINUE row: the checkpoint as it is on disk now (this mission's start, or none).
    SaveGame g;
    if (!savePath_.empty() && loadSaveGame(savePath_, g)) {
        pendingRestore_ = g;
        titleWorld_ = g.world;
        titleMission_ = g.mission;
    } else {
        titleWorld_.clear();
        titleMission_.clear();
    }
    haveRestore_ = false;
    // What he carries now is left behind: CONTINUE hands out the checkpoint's kit (and practice's kit
    // must never reach the campaign).
    syringes_ = 0;
    flares_ = 0;
    resetArsenal();
    titleMissions_.clear();  // (rebuilt by enterTitle: missions finished since unlock the next)
    beginLoading(std::string(), in);
    load_.world = "intro_island";
    load_.toTitle = true;
    load_.title = "MAIN MENU";
    LOGI("Options: back to the main menu (%s)", practice_ ? "practice" : (titleWorld_.empty() ? "no checkpoint" : titleWorld_.c_str()));
}

// Mission select: a fresh start at the chosen mission, carrying what the campaign would have
// given by then - every item the missions before it hand out (the shotgun from 1c, the sniper gun
// from story3_bunker_3, the rocket launcher from story3_launcher_5...).
void GameApp::startChosenMission(const InputFrame& in) {
    const auto wm = titleMissions_[size_t(titleMissionSel_)];
    startFromTitle(wm.first, wm.second, false, in);
    std::vector<uint32_t> items;
    for (int i = 0; i < titleMissionSel_ && size_t(i) < titleMissions_.size(); ++i) {
        const auto it = missionItems(titleMissions_[size_t(i)].second);
        items.insert(items.end(), it.begin(), it.end());
    }
    grantMissionItems(items, "mission select");
    LOGI("Mission select: %s / %s (rocket launcher %s)", wm.first.c_str(), wm.second.c_str(), hasLauncher_ ? "carried" : "not yet");
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
        if (!menuOpen_) { hoverRow_ = -2; stickChose_ = true; }  // (back on the main menu: OPTIONS stays the choice, the row under the pointer isn't one)
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
    // (The CONTINUE row is also the save slot's: reachable when the slot is empty, to switch slots.)
    const int first = baseSavePath_.empty() && titleWorld_.empty() ? kTitleNewGame : kTitleContinue;
    // (2026-10-28) Pointing: the row under either controller's ray is chosen; the trigger on its left or right
    // part changes it as the stick would, in the middle selects.
    const bool actRaw = in.hand[1].primary || in.hand[0].trigger > 0.6f || in.hand[1].trigger > 0.6f;
    int side = 0;
    {
        float lx = 0, ly = 0;
        int ph = kRight;
        const float w = 1.05f, rowH = 0.11f, h = rowH * float(kTitleRowCount) + 0.16f;
        pointDotOn_ = titleViewSet_ && panelRay(titlePanel(), w, h, in, lx, ly, ph);  // (not before the view is placed)
        int row = -1;
        if (pointDotOn_) {
            const int vi = int(std::floor((0.5f * h - 0.08f - ly) / rowH));
            for (int i = 0, k = 0; i < kTitleRowCount; ++i) {
                if (i == kTitleSkAddr && skNetWant_ != 2) continue;
                if (k++ == vi) row = i;
            }
            if (row >= 0 && row < first) row = -1;
        }
        if (hoverRow_ != -2 && row >= 0 && row != hoverRow_) {
            titleRow_ = row;
            titleHold_ = 0;
            stickChose_ = false;
            haptics_.push_back({ph, 0.15f, 0.015f});
            sfx2D("ph_selct", 0.3f);
            LOGI("Main menu: pointing at row %d", row);
        }
        static const bool trace = std::getenv("GVR_POINT_TRACE") != nullptr;
        if (trace && int(titleT_ * 4) != int((titleT_ - dt) * 4)) LOGI("point: on %d at (%.2f %.2f) row %d stick %d sel %d", int(pointDotOn_), lx, ly, row, int(stickChose_), titleRow_);
        if (titleViewSet_) hoverRow_ = row;  // (until then the first row seen is still to be adopted)
        pointDotDim_ = row != titleRow_;
        // (review 2026-10-28) A fresh press on a row other than the chosen one only chooses it.
        if (row >= 0 && row != titleRow_ && actRaw && !titleActHeld_ && !stickChose_) {
            titleRow_ = row;
            titleHold_ = 0;
            side = 2;
            sfx2D("ph_selct", 0.3f);
            LOGI("Main menu: pointing at row %d (pressed)", row);
        }
        const bool changes = row != kTitleOptions && (row != kTitleContinue || !baseSavePath_.empty());
        if (side == 0 && row >= 0 && row == titleRow_ && changes && std::fabs(lx) > 0.3f * w) side = lx > 0 ? 1 : -1;
        if ((side == 1 || side == -1) && actRaw && !titleActHeld_ && dx == 0) dx = side;
    }
    if (dy != 0) {
        titleRow_ = std::max(first, std::min(int(kTitleRowCount) - 1, titleRow_ + dy));
        if (titleRow_ == kTitleSkAddr && skNetWant_ != 2) titleRow_ = std::max(first, std::min(int(kTitleRowCount) - 1, titleRow_ + dy));  // (only when joining)
        stickChose_ = true;
        haptics_.push_back({kRight, 0.2f, 0.02f});
        sfx2D("ph_selct", 0.4f);
        titleHold_ = 0;
    }
    if (dx != 0 && titleRow_ == kTitleContinue && !baseSavePath_.empty()) {
        switchSaveSlot((settings_.saveSlot + dx + 3) % 3);
        haptics_.push_back({kRight, 0.3f, 0.03f});
        sfx2D("ph_selct", 0.6f, 1.2f);
        titleHold_ = 0;
    }
    if (dx != 0 && titleRow_ == kTitleMission && !titleMissions_.empty()) {
        const int n = int(titleMissions_.size());
        titleMissionSel_ = (titleMissionSel_ + dx + n) % n;
        haptics_.push_back({kRight, 0.3f, 0.03f});
        sfx2D("ph_selct", 0.6f, 1.2f);
        titleHold_ = 0;
    }
    // Joining: listen for games on the LAN while that's chosen; a welcome loads the host's map.
    if (skNetWant_ == 2) {
        if (skClient_.state == SkNetClient::State::Idle || skClient_.state == SkNetClient::State::Lost) skClient_.search();
        SkNetSelf none;
        none.alive = false;  // (still in the menu / loading: not in the game yet)
        Skirmish scratch;
        skClient_.update(dt, none, scratch);
        skClient_.games.erase(std::remove_if(skClient_.games.begin(), skClient_.games.end(), [](const SkNetGame& g) { return g.age > 4.0f; }),
                              skClient_.games.end());
        titleSkGame_ = std::max(0, std::min(int(skClient_.games.size()) - 1, titleSkGame_));
        // Only a map of ours (the host's word is all there is): else not joined.
        bool knownMap = false;
        for (const char* m : kSkMaps) knownMap = knownMap || toLower(m) == toLower(skClient_.map);
        if (titleJoining_ && skClient_.state == SkNetClient::State::Joined && !knownMap) {
            LOGW("Main menu: the host plays an unknown map - not joining");
            skClient_.leave();
            titleJoining_ = false;
        }
        if (titleJoining_ && skClient_.state == SkNetClient::State::Joined) {
            titleJoining_ = false;
            skWantTeam_ = skClient_.myTeam;
            title_ = false;
            haveRestore_ = false;
            firstBriefDone_ = true;
            setPlayMode(true, in);
            beginLoading(std::string(), in);
            load_.world = skClient_.map;
            load_.worldMission.clear();
            load_.title = "JOINING " + skClient_.map;
            LOGI("Main menu: joined the game on %s", skClient_.map.c_str());
            return;
        }
        if (titleJoining_ && skClient_.state == SkNetClient::State::Lost) titleJoining_ = false;
    } else if (skClient_.state == SkNetClient::State::Searching) {
        skClient_.leave();
    }
    if (dx != 0 && titleRow_ == kTitleSkMode) {
        skNetWant_ = (skNetWant_ + dx + 3) % 3;
        titleJoining_ = false;
        haptics_.push_back({kRight, 0.3f, 0.03f});
        sfx2D("ph_selct", 0.6f, 1.2f);
    }
    if (dx != 0 && titleRow_ == kTitleSkirmish && skNetWant_ == 2 && !skClient_.games.empty()) {
        titleSkGame_ = (titleSkGame_ + dx + int(skClient_.games.size())) % int(skClient_.games.size());
        haptics_.push_back({kRight, 0.3f, 0.03f});
        sfx2D("ph_selct", 0.6f, 1.2f);
    } else if (dx != 0 && (titleRow_ == kTitleSkirmish || titleRow_ == kTitleSkTeam)) {
        if (titleRow_ == kTitleSkirmish) titleSkMap_ = (titleSkMap_ + dx + 12) % 12;
        else titleSkTeam_ = (titleSkTeam_ + dx + skTeams(titleSkMap_)) % skTeams(titleSkMap_);
        titleSkTeam_ = std::min(titleSkTeam_, skTeams(titleSkMap_) - 1);
        haptics_.push_back({kRight, 0.3f, 0.03f});
        sfx2D("ph_selct", 0.6f, 1.2f);
        titleHold_ = 0;
    }
    if (dx != 0 && titleRow_ == kTitleSkAddr && skNetWant_ == 2) {
        // The host's address by hand (the found games can be missing when the router keeps the
        // headsets' broadcasts apart): its last number; the rest is this headset's own network.
        if (titleSkAddr_ <= 0) {
            titleSkAddr_ = int(ownLanAddress() & 0xFF);
            if (!skClient_.games.empty()) titleSkAddr_ = int(skClient_.games.front().host.ip & 0xFF);
        }
        titleSkAddr_ = (titleSkAddr_ - 1 + dx + 254) % 254 + 1;
        haptics_.push_back({kRight, 0.2f, 0.02f});
        sfx2D("ph_selct", 0.5f, 1.3f);
    }
    if (dx != 0 && titleRow_ == kTitlePractice) {
        titlePractice_ = (titlePractice_ + dx + 3) % 3;
        haptics_.push_back({kRight, 0.3f, 0.03f});
        sfx2D("ph_selct", 0.6f, 1.2f);
    }
    if (dx != 0 && titleRow_ == kTitleNewGame) {
        titleCampaign_ = (titleCampaign_ + dx + 3) % 3;
        haptics_.push_back({kRight, 0.3f, 0.03f});
        sfx2D("ph_selct", 0.6f, 1.2f);
        titleHold_ = 0;
    }
    // A or either trigger: select. A new game over an existing checkpoint is held 1.5 s.
    const bool act = side == 0 && actRaw;
    // (review 2026-10-28) A hold counts only when the press began on this row (not a trigger still held from the
    // last screen with the pointer drifting onto NEW GAME).
    if (act && !titleActHeld_) titleHoldRow_ = titleRow_;
    if (!act || titleHoldRow_ != titleRow_) titleHoldRow_ = -1;
    const bool held = act && titleHoldRow_ == titleRow_;
    const bool missionRow = titleRow_ == kTitleMission && !titleMissions_.empty();
    if ((titleRow_ == kTitleNewGame || missionRow) && held && !titleWorld_.empty()) {
        titleHold_ += dt;
        if (titleHold_ > 1.5f) {
            haptics_.push_back({kLeft, 1.0f, 0.3f});
            haptics_.push_back({kRight, 1.0f, 0.3f});
            if (missionRow) {
                startChosenMission(in);
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
            else if (missionRow) startChosenMission(in);
            else if (titleRow_ == kTitlePractice) startPractice(titlePractice_ == 1 ? Hero::Delphi : (titlePractice_ == 2 ? Hero::Kabuto : Hero::Mecc), in);
            else if (titleRow_ == kTitleSkAddr && skNetWant_ == 2 && !titleJoining_) {
                if (titleSkAddr_ <= 0) titleSkAddr_ = int(ownLanAddress() & 0xFF);
                const NetAddr host{(ownLanAddress() & 0xFFFFFF00u) | uint32_t(titleSkAddr_), kSkNetPort};
                titleJoining_ = skClient_.join(host, "QUEST", titleSkTeam_);
                LOGI("Main menu: joining %s by hand", host.str().c_str());
                sfx2D("pu_hlth", 0.6f, 1.2f);
            }
            else if ((titleRow_ == kTitleSkMode || titleRow_ == kTitleSkirmish || titleRow_ == kTitleSkTeam) && skNetWant_ == 2) {
                // Join the chosen game (its welcome, in a moment, starts the load above).
                if (!skClient_.games.empty() && !titleJoining_) {
                    titleJoining_ = skClient_.join(skClient_.games[size_t(titleSkGame_)].host, "QUEST", titleSkTeam_);
                    sfx2D("pu_hlth", 0.6f, 1.2f);
                }
            }
            else if (titleRow_ == kTitleSkMode || titleRow_ == kTitleSkirmish || titleRow_ == kTitleSkTeam) {
                // A skirmish doesn't touch the campaign's checkpoint.
                skWantTeam_ = titleSkTeam_;
                title_ = false;
                haveRestore_ = false;
                firstBriefDone_ = true;
                setPlayMode(true, in);
                beginLoading(std::string(), in);
                load_.world = kSkMaps[titleSkMap_];
                load_.worldMission.clear();
                load_.title = load_.world;
                LOGI("Main menu: multiplayer %s, team %d", kSkMaps[titleSkMap_], titleSkTeam_);
            }
            else if (titleRow_ == kTitleOptions) openMenu(in);
        }
    }
    titleActHeld_ = actRaw;
}

// The menu panel: world-fixed in front of the start view, a little below eye level, tipped back toward the eyes.
Mat4 GameApp::titlePanel() const {
    const Quat r = Quat::fromAxisAngle({0, 1, 0}, titleYaw_);
    const Vec3 base = titleEye_ + r.rotate(Vec3{0, -0.55f, -1.5f});
    return Mat4::translation(base) * Mat4::rotation(r * Quat::fromAxisAngle({1, 0, 0}, 0.32f));
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
    const Mat4 panel = titlePanel();
    const float w = 1.05f, rowH = 0.11f, h = rowH * float(kTitleRowCount) + 0.16f;
    // Rework: a dark glass panel with a thin gold edge.
    text_.drawPanel(panel, w, h, {0.015f, 0.03f, 0.055f, 0.86f});
    drawPanelEdge(panel, w, h);
    std::string cont = "CONTINUE";
    if (!titleWorld_.empty()) {
        std::string t, b;
        missionText(titleMission_, t, b);
        cont += "  -  " + (t.empty() ? titleMission_ : t);
    }
    if (!baseSavePath_.empty()) {
        std::string t, b;
        if (!titleWorld_.empty()) missionText(titleMission_, t, b);
        const std::string slot = std::to_string(settings_.saveSlot + 1);
        cont = (titleRow_ == kTitleContinue ? "SLOT < " + slot + " >  " : "SLOT " + slot + "  ") +
               (titleWorld_.empty() ? std::string("EMPTY") : "CONTINUE: " + (t.empty() ? titleMission_ : t));
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
    const std::string mode = std::string("MULTIPLAYER:  < ") + kSkModes[skNetWant_] + " >";
    std::string sk = "   MAP:  < " + skMapLabel(titleSkMap_) + " >";
    if (skNetWant_ == 2) {
        if (titleJoining_) sk = "   JOINING...";
        else if (skClient_.games.empty()) sk = "   LOOKING FOR GAMES ON THIS WI-FI...";
        else {
            const SkNetGame& g = skClient_.games[size_t(std::min<int>(titleSkGame_, int(skClient_.games.size()) - 1))];
            int mapIdx = 0;
            for (int i = 0; i < 12; ++i)
                if (toLower(kSkMaps[i]) == toLower(g.map)) mapIdx = i;
            sk = "   GAME:  < " + skMapLabel(mapIdx) + " AT " + g.host.str().substr(0, g.host.str().find(':')) + " (" + std::to_string(g.players) + ") >";
        }
    }
    const std::string skt = "   YOUR TEAM:  < " + skTeamLabel(titleSkMap_, titleSkTeam_) + " >";
    std::string addr;
    if (skNetWant_ == 2) {
        const uint32_t own = ownLanAddress();
        const int last = titleSkAddr_ > 0 ? titleSkAddr_ : int(own & 0xFF);
        char a[64];
        std::snprintf(a, sizeof a, "   OR HOST'S ADDRESS:  %u.%u.%u.< %d >", own >> 24, (own >> 16) & 0xFF, (own >> 8) & 0xFF, last);
        addr = a;
    }
    static const char* kPracticeName[3] = {"BAZ (MECCARYN)", "DELPHI (SEA REAPER)", "KABUTO"};
    const std::string pr = std::string("PRACTICE:  < ") + kPracticeName[titlePractice_] + " >";
    const std::string rows[kTitleRowCount] = {cont, ng, ms, pr, mode, sk, skt, addr, "OPTIONS"};
    float tw, th;
    for (int i = 0, vi = 0; i < kTitleRowCount; ++i) {
        if (i == kTitleSkAddr && skNetWant_ != 2) continue;  // (a row only for joining)
        const float y = 0.5f * h - 0.08f - rowH * (0.5f + float(vi++));
        const bool usable = (i != kTitleContinue || !titleWorld_.empty()) && (i != kTitleMission || !titleMissions_.empty());
        const bool sel = i == titleRow_;
        if (sel) text_.drawPanel(panel * Mat4::translation({0, y, 0.002f}), w - 0.06f, rowH * 0.9f, {0.75f, 0.5f, 0.15f, 0.45f});
        const Vec4 col = !usable ? Vec4{0.45f, 0.45f, 0.45f, 1} : (sel ? Vec4{1, 1, 0.8f, 1} : Vec4{0.85f, 0.85f, 0.85f, 1});
        const float size = rows[i].size() > 34 ? std::max(0.038f, 0.055f * 34.0f / float(rows[i].size())) : 0.055f;  // (long rows: smaller, on one line)
        if (const MeshId m = text_.mesh(rows[i], size, w - 0.1f, &tw, &th)) text_.draw(m, panel * Mat4::translation({0, y, 0.004f}), col);
    }
    renderPointDot();
    if (const MeshId m = text_.mesh("POINT OR STICK: CHOOSE    TRIGGER: SELECT (ON A ROW'S ENDS: CHANGE)", 0.03f, 1.0f, &tw, &th))
        text_.draw(m, panel * Mat4::translation({0, -0.5f * h + 0.04f, 0.004f}), {0.7f, 0.7f, 0.7f, 1});
    // (2026-10-28, release readiness) Under the panel: what this is.
    if (const MeshId m = text_.mesh("UNOFFICIAL FAN PROJECT - NOT AFFILIATED WITH OR ENDORSED BY THE GAME'S MAKERS OR OWNERS. "
                                    "PLAYS YOUR OWN COPY OF GIANTS: CITIZEN KABUTO.", 0.024f, 1.0f, &tw, &th))
        text_.draw(m, panel * Mat4::translation({0, -0.5f * h - 0.05f, 0.004f}), {0.6f, 0.6f, 0.6f, 0.9f});
    // The chosen story's loading art beside the menu.
    const Hero h2 = titleRow_ == kTitleContinue && !titleWorld_.empty() ? heroForWorld(titleWorld_)
                    : titleRow_ == kTitlePractice ? (titlePractice_ == 1 ? Hero::Delphi : (titlePractice_ == 2 ? Hero::Kabuto : Hero::Mecc))
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

void GameApp::loadProgress() {
    missionsDone_.clear();
    bestTimes_.clear();
    if (const char* p = std::getenv("GVR_PROGRESS")) {  // test hook: these missions count as finished
        std::string list = p;
        size_t at = 0;
        while (at <= list.size()) {
            size_t end = list.find(',', at);
            if (end == std::string::npos) end = list.size();
            if (end > at) missionsDone_.insert(toLower(list.substr(at, end - at)));
            at = end + 1;
        }
        LOGI("Progress from GVR_PROGRESS: %zu missions finished", missionsDone_.size());
        return;
    }
    if (progressPath_.empty()) return;
    FILE* f = std::fopen(progressPath_.c_str(), "r");
    if (!f) return;
    char line[256];
    while (std::fgets(line, sizeof line, f)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
        if (s.rfind("done=", 0) == 0 && s.size() > 5) missionsDone_.insert(toLower(s.substr(5)));
        if (s.rfind("best=", 0) == 0) {
            const size_t sp = s.find(' ', 5);
            if (sp != std::string::npos) {
                const float t = float(std::atof(s.c_str() + sp + 1));
                const std::string m = toLower(s.substr(5, sp - 5));
                if (t > 0 && (!bestTimes_.count(m) || t < bestTimes_[m])) bestTimes_[m] = t;
            }
        }
    }
    std::fclose(f);
    LOGI("Progress: %zu missions finished", missionsDone_.size());
}

void GameApp::markMissionDone(const std::string& mission) {
    const std::string m = toLower(mission);
    if (practice_ || m.empty() || !missionsDone_.insert(m).second) return;  // (practice counts for nothing)
    LOGI("Progress: %s finished (%zu missions)", m.c_str(), missionsDone_.size());
    if (progressPath_.empty() || std::getenv("GVR_PROGRESS")) return;
    if (FILE* f = std::fopen(progressPath_.c_str(), "a")) {  // (appended: a crash can't lose the earlier ones)
        std::fprintf(f, "done=%s\n", m.c_str());
        std::fclose(f);
    }
}

std::string GameApp::slotPath(int slot) const { return saveSlotPath(baseSavePath_, slot); }

void GameApp::switchSaveSlot(int slot) {
    settings_.saveSlot = std::max(0, std::min(2, slot));
    saveSettings();
    savePath_ = slotPath(settings_.saveSlot);
    SaveGame g;
    if (loadSaveGame(savePath_, g)) {
        pendingRestore_ = g;
        haveRestore_ = true;
        titleWorld_ = g.world;
        titleMission_ = g.mission;
        const Hero h = heroForWorld(titleWorld_);
        titleCampaign_ = h == Hero::Delphi ? 1 : (h == Hero::Kabuto ? 2 : 0);
    } else {
        haveRestore_ = false;
        titleWorld_.clear();
        titleMission_.clear();
    }
    LOGI("Save slot %d: %s", settings_.saveSlot + 1, titleWorld_.empty() ? "empty" : (titleWorld_ + " / " + titleMission_).c_str());
    // The mission list again, with this slot's checkpoint mission (review 2026-10-28).
    titleMissions_.clear();
    if (assetWorld_) {
        titleMissions_ = assetWorld_->campaignMissions();
        static const char* all = std::getenv("GVR_ALL_MISSIONS");
        if (!(all && all[0] == '1')) titleMissions_ = unlockedMissions(titleMissions_, missionsDone_, titleMission_);
    }
    titleMissionSel_ = 0;
    for (size_t i = 0; i < titleMissions_.size(); ++i)
        if (toLower(titleMissions_[i].second) == toLower(titleMission_)) titleMissionSel_ = int(i);
}

}  // namespace gvr
