// Reaper soldiers' weapons. From the data: the Reaper guards' guns are separate models
// (xx_rpman_weapons: rpman_spear, rpm_bazooka, rpm_snipergun) with Ref_handle / Ref_nozzle nodes,
// held at the soldier's ref_rhand - the cutscenes attach them that way (op 3). The missions place
// no weapons (the original arms its soldiers natively), so ours: most carry the spear gun and fire
// its bolts from Ref_nozzle; one in four carries the bazooka and fires slow rockets that burst.
#include "game/game_app.h"

#include <algorithm>
#include <cmath>

#include "assets/skin_anim.h"
#include "core/log.h"
#include "game/cutscene.h"
#include "scene/asset_world.h"
#include "scene/model_cache.h"
#include "scene/skinned_model.h"

namespace gvr {

namespace {
bool refNodes(const AssetFS& fs, const char* anm, Mat4& handle, Mat4& nozzle) {
    std::vector<uint8_t> b;
    AnmAnimation a;
    if (!fs.read(anm, b) || !parseAnm(b.data(), b.size(), a)) return false;
    std::vector<Mat4> wm;
    a.evaluate(0.0f, wm);
    bool h = false, n = false;
    for (size_t i = 0; i < a.nodes.size() && i < wm.size(); ++i) {
        const std::string nm = toLower(a.nodes[i].name);
        if (nm == "ref_handle") { handle = wm[i]; h = true; }
        if (nm == "ref_nozzle") { nozzle = wm[i]; n = true; }
    }
    return h && n;
}
}  // namespace

void GameApp::initReaperWeapons() {
    if (rpWeaponsReady_ || !assetWorld_) return;
    rpWeaponsReady_ = true;
    ModelCache* mc = assetWorld_->models();
    const char* models[2] = {"rpman_spear", "rpm_bazooka"};
    for (int k = 0; k < 2; ++k) {
        Mat4 h, n;
        RpWeapon& w = rpWeapon_[k];
        w.model = mc->get(std::string(models[k]) + ".gbs", std::string(models[k]) + ".anm", true);
        if (!w.model || !refNodes(assetWorld_->fs(), (std::string(models[k]) + ".anm").c_str(), h, n)) {
            w.model = nullptr;
            LOGW("Reaper weapon %s unavailable", models[k]);
            continue;
        }
        w.handleInv = gameMatrixToRuntime(h).affineInverse();
        w.nozzle = gameMatrixToRuntime(n);
    }
    LOGI("Reaper weapons: spear gun %s, bazooka %s", rpWeapon_[0].model ? "ok" : "missing", rpWeapon_[1].model ? "ok" : "missing");
}

// After the soldiers are posed: each one's weapon in his right hand.
void GameApp::placeReaperWeapons() {
    initReaperWeapons();
    for (auto& e : enemies_.enemies) {
        if (e.weapon < 0) e.weapon = (e.uid % 4u == 3u) ? 1 : 0;
        e.weaponOn = false;
        if (e.slot < 0 || !rpWeapon_[e.weapon].model) continue;
        Mat4 hand;
        if (!enemyPool_[size_t(e.slot)]->nodeWorld("ref_rhand", hand)) continue;
        const Mat4 body = Mat4::translation(e.pos + Vec3{0, slotLift_[size_t(e.slot)], 0}) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, e.yaw));
        e.weaponXf = body * gameMatrixToRuntime(hand) * rpWeapon_[e.weapon].handleInv;
        e.weaponOn = true;
    }
}

bool GameApp::reaperMuzzle(const Enemy& e, Vec3& muzzle) const {
    if (!e.weaponOn || e.weapon < 0) return false;
    const Mat4 m = e.weaponXf * rpWeapon_[e.weapon].nozzle;
    muzzle = Vec3{m.m[12], m.m[13], m.m[14]};
    return true;
}

void GameApp::renderReaperWeapons(bool shadow, const Vec3& eye, float range) {
    for (const auto& e : enemies_.enemies) {
        if (!e.weaponOn || length(e.pos - eye) > range) continue;
        const RenderModel* m = rpWeapon_[e.weapon].model;
        if (shadow) {
            for (const auto& part : m->parts) renderer_.drawShadow(part.mesh, e.weaponXf);
        } else {
            drawModel(m, e.weaponXf, false);
        }
    }
}

}  // namespace gvr
