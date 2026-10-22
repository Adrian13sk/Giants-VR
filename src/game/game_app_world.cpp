// GameApp: destructible scenery and the particle effects (smoke, dust, fire, trails, blood
// batches). Split from game_app.cpp to keep that file manageable; same class.
#include "game/game_app.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "scene/asset_world.h"
#include "scene/model_cache.h"
#include "scene/skinned_model.h"

namespace gvr {

// ---------------------------------------------------------------------------------------------
// Destruction

void GameApp::initDestruction(AssetWorld* w, ModelCache* mc) {
    destruct_ = DestructionDirector{};
    destructEvents_.clear();
    collapses_.clear();
    auto& objs = w->worldObjects();
    objectItem_.assign(objs.size(), -1);
    const ObjectRegistry& reg = w->registry();
    size_t counts[4] = {};
    for (size_t i = 0; i < objs.size(); ++i) {
        const auto& o = objs[i];
        const ObjectType* t = reg.type(o.type);
        const int cls = t ? t->objectClass : -1;
        const float dataHp = t ? t->hitPoints : 0.0f;
        DestructKind kind;
        float hp;
        if (cls == 60) { kind = DestructKind::Tree; hp = 60.0f; }
        else if (o.name.rfind("rock_", 0) == 0 && o.radius < 12.0f) { kind = DestructKind::Rock; hp = 160.0f; }
        else if (o.name.find("guard_tower") != std::string::npos) { kind = DestructKind::Tower; hp = dataHp > 0 ? dataHp : 350.0f; }
        else if (cls == 61 && dataHp > 0) { kind = DestructKind::House; hp = dataHp; }
        else continue;
        Vec3 base{o.placement.m[12], o.placement.m[13], o.placement.m[14]};
        float gy;
        if (w->terrain().heightAt(base.x, base.z, gy)) base.y = gy;
        const float height = std::max(1.0f, (o.model->boundsMax.y - o.model->boundsMin.y) * o.scale);
        objectItem_[i] = destruct_.add(int(i), kind, base, height, o.radius, hp);
        if (kind == DestructKind::Tower || kind == DestructKind::House)
            LOGI("  destructible %s at game (%.0f %.0f), %.0f hp, %.1f m tall", o.name.c_str(), -base.x / gGameUnitsToMeters,
                 base.z / gGameUnitsToMeters, hp, height);
        ++counts[int(kind)];
    }
    downTime_.assign(destruct_.items.size(), 0.0f);
    if (debrisStone_.empty()) {
        for (const char* n : {"EFX_Stone_debri_1.gbs", "EFX_debri1.gbs", "EFX_debri2.gbs", "EFX_debri_3.gbs"})
            if (const RenderModel* m = mc->get(n, "", true)) debrisStone_.push_back(m);
        for (const char* n : {"EFX_wood_debri_1.gbs", "EFX_wood_debri_2.gbs", "EFX_debri2.gbs"})
            if (const RenderModel* m = mc->get(n, "", true)) debrisWood_.push_back(m);
        for (int k = 1; k <= 5; ++k)
            if (const RenderModel* m = mc->get("EFX_guard_tower_debri_" + std::to_string(k) + ".gbs", "", true)) debrisTower_[0].push_back(m);
        for (int k = 1; k <= 3; ++k)
            if (const RenderModel* m = mc->get("EFX_tower_2_debri_" + std::to_string(k) + ".gbs", "", true)) debrisTower_[1].push_back(m);
    }
    // Sentries belong to the tower they stand on.
    for (auto& e : enemies_.enemies) {
        if (!e.posted) continue;
        float best = 8.0f;
        for (size_t k = 0; k < destruct_.items.size(); ++k) {
            const Destructible& d = destruct_.items[k];
            if (d.kind != DestructKind::Tower) continue;
            const float h = std::hypot(e.pos.x - d.base.x, e.pos.z - d.base.z);
            if (h < best) { best = h; e.postObject = int(k); }
        }
    }
    LOGI("Destructible: %zu trees, %zu rocks, %zu houses, %zu towers", counts[0], counts[1], counts[2], counts[3]);
}

bool GameApp::treeRay(const Vec3& o, const Vec3& d, float maxD, float& t, int& item) const {
    bool hit = false;
    float best = maxD;
    for (size_t k = 0; k < destruct_.items.size(); ++k) {
        const Destructible& it = destruct_.items[k];
        if (it.kind != DestructKind::Tree || !it.alive()) continue;
        if (std::hypot(it.base.x - o.x, it.base.z - o.z) > maxD + 2.0f) continue;
        float th;
        if (rayCapsule(o, d, best, it.base + Vec3{0, 0.2f, 0}, it.base + Vec3{0, it.height * 0.65f, 0}, 0.35f, th) && th < best) {
            best = th;
            item = int(k);
            hit = true;
        }
    }
    if (hit) t = best;
    return hit;
}

void GameApp::throwDebris(const std::vector<const RenderModel*>& pieces, const Vec3& at, int count, float speed, float scale) {
    if (pieces.empty()) return;
    for (int i = 0; i < count; ++i) {
        Gib g;
        g.model = pieces[size_t(i) % pieces.size()];
        g.pos = at + Vec3{(frand() - 0.5f) * 1.5f, frand() * 1.0f, (frand() - 0.5f) * 1.5f} * scale;
        g.vel = Vec3{(frand() - 0.5f) * 2.0f, 0.6f + frand(), (frand() - 0.5f) * 2.0f} * speed;
        g.axis = normalize(Vec3{frand() - 0.5f, frand() - 0.5f, frand() - 0.5f} + Vec3{0.01f, 0, 0});
        g.angle = frand() * 6.28f;
        g.spin = 2.0f + 6.0f * frand();
        g.scale = scale * (0.6f + 0.6f * frand());
        g.life = -4.0f;  // debris lies a while longer than gore
        gibs_.push_back(g);
    }
}

void GameApp::damageObject(int item, float amount, bool blast, const Vec3& from) {
    destruct_.damage(item, amount, blast, from, destructEvents_);
}

void GameApp::updateDestruction(float dt) {
    if (!assetWorld_) return;
    auto& objs = assetWorld_->worldObjects();
    destruct_.update(dt, destructEvents_);
    ModelCache* mc = assetWorld_->models();
    for (const auto& ev : destructEvents_) {
        if (ev.index < 0 || size_t(ev.index) >= destruct_.items.size()) continue;
        Destructible& d = destruct_.items[size_t(ev.index)];
        auto& obj = objs[size_t(d.object)];
        const Vec3 top = d.base + Vec3{0, d.height * 0.85f, 0};
        switch (ev.kind) {
            case DestructEvent::Hit:
            case DestructEvent::Stage:
                break;
            case DestructEvent::Collapse:
                assetWorld_->collisionMutable().setEnabled(obj.collision, false);
                if (d.kind == DestructKind::Tree) {
                    sfx("ex_treel", d.base + Vec3{0, 2, 0}, 1.0f, 0.9f + 0.2f * frand());
                    throwDebris(debrisWood_, d.base + Vec3{0, 0.8f, 0}, 3, 3.0f, 0.5f);
                } else if (d.kind == DestructKind::Rock) {
                    obj.hidden = true;
                    sfx("ex_stonl", obj.centre, 1.0f, 0.9f + 0.2f * frand());
                    throwDebris(debrisStone_, obj.centre, 10, 6.0f, std::min(2.0f, 0.4f * d.radius));
                    dustBurst(obj.centre, d.radius, 12);
                } else if (d.kind == DestructKind::House) {
                    sfx("ex_woodl", obj.centre, 1.0f, 0.9f + 0.2f * frand());
                    sfx("ex_stonl", obj.centre, 0.9f, 0.8f);
                    throwDebris(debrisStone_, top, 8, 7.0f, 1.0f);
                    throwDebris(debrisWood_, top, 6, 7.0f, 0.8f);
                    dustBurst(d.base + Vec3{0, 1.0f, 0}, d.radius, 18);
                } else {  // tower: its own collapse clip on a skinned copy
                    sfx("ex_metll", top, 1.0f, 0.85f);
                    obj.hidden = true;
                    Collapse c;
                    c.item = ev.index;
                    c.xf = obj.placement;
                    auto m = std::make_unique<SkinnedModel>();
                    const std::string anim = assetWorld_->registry().modelAnim(obj.type);
                    if (!anim.empty() && m->load(assetWorld_->fs(), *mc, renderer_, obj.name, anim)) {
                        m->pose(renderer_, 0.0f);
                        c.model = std::move(m);
                    }
                    collapses_.push_back(std::move(c));
                }
                LOGI("Destroyed: %s (%s)", obj.name.c_str(),
                     d.kind == DestructKind::Tree ? "tree" : d.kind == DestructKind::Rock ? "rock" : d.kind == DestructKind::House ? "house" : "tower");
                break;
            case DestructEvent::Fallen:  // a tree hits the ground
                sfx("ex_trees", ev.pos, 1.0f, 0.9f);
                dustBurst(d.base + d.fallDir * (d.height * 0.6f), 1.5f, 8);
                break;
            case DestructEvent::Down:
                if (d.kind == DestructKind::Tower) {
                    // The ruin (Guard_tower1D) stands where the tower was.
                    for (size_t k = 0; k < collapses_.size(); ++k)
                        if (collapses_[k].item == ev.index) { collapses_.erase(collapses_.begin() + long(k)); break; }
                    if (const RenderModel* ruin = mc->get(obj.name + "d.gbs", "", false)) {
                        obj.model = ruin;
                        obj.transform = obj.placement;
                        obj.hidden = false;
                        std::vector<Vec3> tris;
                        for (const Vec3& v : ruin->collisionTris) {
                            const Vec4 p = obj.placement * Vec4{v.x, v.y, v.z, 1.0f};
                            tris.push_back({p.x, p.y, p.z});
                        }
                        obj.collision = assetWorld_->collisionMutable().addObject(tris, d.object);
                    }
                    dustBurst(d.base + Vec3{0, 2.0f, 0}, d.radius, 20);
                    sfx("ex_stonl", d.base, 1.0f, 0.8f);
                }
                break;
        }
    }
    destructEvents_.clear();
    for (size_t k = 0; k < destruct_.items.size(); ++k) {
        Destructible& d = destruct_.items[k];
        auto& obj = objs[size_t(d.object)];
        const Vec3 top = d.base + Vec3{0, d.height * 0.85f, 0};
        // Damaged: smoke from the top (heavier, with fire, when badly damaged).
        if ((d.stage == 1 || d.stage == 2) && d.kind != DestructKind::Tree && d.kind != DestructKind::Rock &&
            length(d.base - player_.position) < 150.0f) {
            d.smoke -= dt;
            if (d.smoke <= 0) {
                d.smoke = d.stage == 1 ? 0.35f : 0.15f;
                const Vec3 at = top + Vec3{(frand() - 0.5f) * d.radius * 0.4f, 0, (frand() - 0.5f) * d.radius * 0.4f};
                smokePuff(FxTex::SmokeBlack, at, Vec3{0, 1.2f, 0}, 1.0f, 4.5f, 5.0f, 0.55f, 0.3f);
                if (d.stage == 2) smokePuff(FxTex::Fire, at, Vec3{0, 1.5f, 0}, 1.2f, 0.4f, 0.6f, 0.9f, 0.0f);
            }
        }
        if (d.stage == 3) {
            const float dur = destruct_.collapseTime[int(d.kind)];
            const float k01 = std::min(1.0f, d.t / dur);
            if (d.kind == DestructKind::Tree) {
                const Vec3 axis = normalize(cross(Vec3{0, 1, 0}, d.fallDir) + Vec3{1e-4f, 0, 0});
                obj.transform = Mat4::translation(d.base) * Mat4::rotation(Quat::fromAxisAngle(axis, DestructionDirector::fallAngle(d.t, dur))) *
                                Mat4::translation(d.base * -1.0f) * obj.placement;
            } else if (d.kind == DestructKind::House) {
                // Caves in: sinks, squashes and leans, raising dust.
                const float e = k01 * k01;
                const Vec3 axis = normalize(cross(Vec3{0, 1, 0}, d.fallDir) + Vec3{1e-4f, 0, 0});
                obj.transform = Mat4::translation(d.base + Vec3{0, -0.2f * d.height * e, 0}) * Mat4::rotation(Quat::fromAxisAngle(axis, 0.18f * e)) *
                                Mat4::scale({1.0f, 1.0f - 0.7f * e, 1.0f}) * Mat4::translation(d.base * -1.0f) * obj.placement;
                d.smoke -= dt;
                if (d.smoke <= 0) { d.smoke = 0.12f; dustBurst(d.base + Vec3{0, 0.5f, 0}, d.radius * 0.8f, 2); }
            }
        }
        if (d.stage == 4) {
            downTime_[k] += dt;
            if (d.kind == DestructKind::Tree && downTime_[k] > 25.0f) {
                // A fallen trunk lies a while, then sinks away.
                const float s = (downTime_[k] - 25.0f) * 0.6f;
                const Vec3 axis = normalize(cross(Vec3{0, 1, 0}, d.fallDir) + Vec3{1e-4f, 0, 0});
                obj.transform = Mat4::translation(d.base + Vec3{0, -s, 0}) * Mat4::rotation(Quat::fromAxisAngle(axis, 1.5708f)) *
                                Mat4::translation(d.base * -1.0f) * obj.placement;
                if (s > 2.0f) obj.hidden = true;
            }
        }
    }
    // Towers: the collapse clip; the sentry is thrown off as it bends, debris flies.
    for (auto& c : collapses_) {
        Destructible& d = destruct_.items[size_t(c.item)];
        if (c.model) c.model->pose(renderer_, std::min(d.t, c.model->animDuration() * 0.999f));
        if (d.t > 0.9f && !c.debris) {
            c.debris = true;
            const Vec3 top = d.base + Vec3{0, d.height * 0.8f, 0};
            const int which = objs[size_t(d.object)].name.find("tower2") != std::string::npos ? 1 : 0;
            throwDebris(debrisTower_[which].empty() ? debrisStone_ : debrisTower_[which], top, 8, 6.0f, 1.0f);
            dustBurst(top, 3.0f, 10);
            for (size_t i = 0; i < enemies_.enemies.size(); ++i) {
                Enemy& e = enemies_.enemies[i];
                if (e.postObject != c.item) continue;
                if (e.state == EnemyState::Dead) {
                    e.posted = false;  // the body drops with it
                } else {
                    enemies_.knockOff(int(i), d.fallDir * 3.0f + Vec3{0, 2.5f, 0});
                    static const char* screams[] = {"rpman_die2", "rpman_die4"};
                    sfx(screams[int(frand() * 1.99f)], e.pos, 1.0f);
                    LOGI("The sentry falls from his tower");
                }
                e.postObject = -1;
            }
        }
    }
}

void GameApp::debugBlast(float gx, float gy, float height) {
    Vec3 at = giantsToRuntime(gx, gy, 0.0f);
    float g;
    if (assetWorld_ && assetWorld_->terrain().heightAt(at.x, at.z, g)) at.y = g;
    at.y += height;
    explode(at, 4.0f, 120.0f);
}

// ---------------------------------------------------------------------------------------------
// Effects

void GameApp::initFx(ModelCache* mc) {
    if (batchesReady_) return;
    const char* names[int(FxTex::Count)] = {"b_smoke", "efx_smoke_black", "efx_smoke_mgrey", "b_dust", "efx_fire_trail", "efx_bfire_trail",
                                           "efx_gfire_trail"};
    const size_t caps[int(FxTex::Count)] = {192, 256, 256, 160, 256, 96, 48};
    for (int k = 0; k < int(FxTex::Count); ++k) {
        fxTex_[k] = mc->texture(names[k]);
        fxBatch_[k].init(renderer_, caps[k]);
    }
    const size_t bcaps[5] = {64, 64, 64, 160, 40};
    for (int k = 0; k < 5; ++k) bloodBatch_[k].init(renderer_, bcaps[k]);
    batchesReady_ = true;
}

void GameApp::smokePuff(FxTex tex, const Vec3& at, const Vec3& vel, float size0, float size1, float life, float alpha, float rise) {
    Particle p;
    p.pos = at;
    p.vel = vel;
    p.size0 = size0;
    p.size1 = size1;
    p.life = life * (0.8f + 0.4f * frand());
    p.alpha = alpha;
    p.rise = rise;
    p.drag = 0.6f;
    p.angle = frand() * 6.28f;
    p.spin = (frand() - 0.5f) * 1.2f;
    p.tex = tex;
    p.additive = tex == FxTex::Fire || tex == FxTex::FireBlue || tex == FxTex::FireGreen;
    fx_.emit(p);
}

void GameApp::dustBurst(const Vec3& at, float radius, int count) {
    for (int i = 0; i < count; ++i) {
        const float a = frand() * 6.28f;
        const Vec3 dir{std::cos(a), 0.25f + 0.3f * frand(), std::sin(a)};
        smokePuff(FxTex::Dust, at + Vec3{std::cos(a), 0, std::sin(a)} * (radius * 0.4f * frand()), dir * (1.5f + 2.0f * frand()),
                  1.0f + 0.4f * radius, 3.0f + 1.0f * radius, 3.5f, 0.6f, 0.15f);
    }
}

void GameApp::updateFx(float dt) {
    fx_.update(dt);
    // Rocket trails: a flame at the nozzle and grey smoke left hanging behind.
    for (const auto& r : rockets_) {
        const Vec3 back = normalize(r.vel) * -1.0f;
        smokePuff(FxTex::Fire, r.pos + back * 0.3f, back * 3.0f, 0.55f, 0.15f, 0.18f, 1.0f, 0.0f);
        smokePuff(FxTex::SmokeGrey, r.pos + back * 0.5f, back * 0.5f + Vec3{0, 0.2f, 0}, 0.3f, 1.4f, 1.2f, 0.5f, 0.35f);
    }
    // Ripper spit: orange (medium) or blue (large) flame trails.
    for (const auto& sp : creatures_.spits)
        smokePuff(sp.size == CreatureSize::Large ? FxTex::FireBlue : FxTex::Fire, sp.pos, Vec3{0, 0.3f, 0},
                  sp.size == CreatureSize::Large ? 0.7f : 0.5f, 0.12f, 0.35f, 0.9f, 0.0f);
    // Badly hurt: smoke pours from the Meccaryn's armour.
    if (playMode_ && !playerGibbed_ && (playerHealth_ < 35.0f || dying_)) {
        smokeTimer_ -= dt;
        if (smokeTimer_ <= 0) {
            smokeTimer_ = dying_ ? 0.06f : 0.1f + 0.15f * std::max(0.0f, playerHealth_ / 35.0f);
            const Quat r = Quat::fromAxisAngle({0, 1, 0}, player_.yaw);
            const Vec3 at = player_.position + Vec3{0, dying_ ? 0.4f : 1.3f, 0} + r.rotate(Vec3{(frand() - 0.5f) * 0.4f, 0, 0.25f});
            smokePuff(frand() < 0.4f ? FxTex::SmokeBlack : FxTex::SmokeGrey, at, Vec3{0, 0.9f, 0} + r.rotate(Vec3{0, 0, 0.3f}), 0.25f, 1.4f,
                      1.8f, 0.55f, 0.35f);
        }
    }
}

void GameApp::buildBatches() {
    if (!batchesReady_) return;
    const Vec3 right = headWorldRot_.rotate(Vec3{1, 0, 0}), up = headWorldRot_.rotate(Vec3{0, 1, 0});
    const Vec3 eye = rig_.toWorld(lastInput_.head).position;
    for (auto& b : fxBatch_) b.clear();
    for (auto& b : bloodBatch_) b.clear();
    for (const auto& p : fx_.parts) {
        if (length(p.pos - eye) > 200.0f) continue;
        const float a = p.opacity();
        if (a <= 0.01f) continue;
        uint8_t c[4] = {255, 255, 255, uint8_t(std::min(1.0f, a) * 255.0f)};
        if (p.additive) { const uint8_t g = c[3]; c[0] = c[1] = c[2] = g; c[3] = 255; }  // additive: fade by colour
        fxBatch_[int(p.tex)].addBillboard(p.pos, p.size(), right, up, p.angle, c);
    }
    for (const auto& d : blood_.decals) {
        if (length(d.pos - eye) > 70.0f) continue;
        const float a = d.alpha() * (d.water ? 0.55f : 0.95f);
        if (a <= 0.01f) continue;
        const Vec3 n = d.normal;
        Vec3 t1 = normalize(cross(n, std::fabs(n.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0}));
        Vec3 t2 = cross(n, t1);
        const float ca = std::cos(d.angle), sa = std::sin(d.angle), h = 0.5f * d.scale();
        const Vec3 u = (t1 * ca + t2 * sa) * h, v = (t2 * ca - t1 * sa) * h;
        const Vec3 c0 = d.pos + n * 0.03f;
        const Vec3 q[4] = {c0 - u - v, c0 + u - v, c0 + u + v, c0 - u + v};
        const uint8_t col[4] = {255, 255, 255, uint8_t(a * 255.0f)};
        bloodBatch_[std::max(0, std::min(2, d.texture))].add(q, col);
    }
    const uint8_t opaque[4] = {255, 255, 255, 255};
    for (const auto& dr : blood_.drops) bloodBatch_[3].addBillboard(dr.pos, dr.size, right, up, 0.0f, opaque);
    for (const auto& sp : blood_.sprays) {
        const float f = sp.age / sp.life;
        const uint8_t col[4] = {255, 255, 255, uint8_t((1.0f - f) * 255.0f)};
        bloodBatch_[4].addBillboard(sp.pos, sp.size * (0.6f + 1.2f * f), right, up, sp.age * 2.0f, col);
    }
    for (auto& b : fxBatch_) b.upload(renderer_);
    for (auto& b : bloodBatch_) b.upload(renderer_);
}

void GameApp::renderFx() {
    if (!batchesReady_) return;
    DrawParams dp;
    dp.blend = BlendMode::AlphaBlend;
    dp.unlit = true;
    dp.depthWrite = false;
    dp.doubleSided = true;
    const TextureId bt[5] = {bloodTex_[0], bloodTex_[1], bloodTex_[2], dropTex_, sprayTex_ ? sprayTex_ : dropTex_};
    for (int k = 0; k < 5; ++k) {
        dp.texture = bt[k];
        bloodBatch_[k].draw(renderer_, dp);
    }
    for (int k = 0; k < int(FxTex::Count); ++k) {
        const bool fire = k == int(FxTex::Fire) || k == int(FxTex::FireBlue) || k == int(FxTex::FireGreen);
        DrawParams fp = dp;
        fp.texture = fxTex_[k];
        if (fire) { fp.blend = BlendMode::Additive; fp.noFog = true; }
        fxBatch_[k].draw(renderer_, fp);
    }
}

}  // namespace gvr
