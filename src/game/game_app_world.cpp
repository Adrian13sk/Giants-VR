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
    samCool_.clear();
    destructEvents_.clear();
    collapses_.clear();
    auto& objs = w->worldObjects();
    objectItem_.assign(objs.size(), -1);
    const ObjectRegistry& reg = w->registry();
    size_t counts[4] = {};
    // Types a mission's death trigger names (not group spawners): those come down too (giant walls
    // and gates in Kabuto's story, the Reaper queen Sappho, ships).
    std::vector<uint32_t> deathTypes;
    for (const auto& t : w->triggerObjects()) {
        const ObjectType* tt = reg.type(t.type);
        if (t.kind == 2 && !(tt && tt->objectClass == 97)) deathTypes.push_back(t.type);
    }
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
        // Mission targets that come down: barracks (class 98), prisons and doors (101), and the
        // other buildings a mission's death trigger names (log_launcher_base; the army barracks,
        // whose data hp is 0, get ours).
        else if (cls == 98 || cls == 101) { kind = DestructKind::House; hp = dataHp > 0 ? dataHp : 400.0f; }
        else if (cls == 44) { kind = DestructKind::House; hp = dataHp > 0 ? dataHp : 200.0f; }  // SAM turrets (guard_sam)
        // The rest of what is built (ours: hit points by size) - fences, walls, columns, gates,
        // Smartie towers, domes, wells, the woodshop pile.
        else if (o.name.find("fence") != std::string::npos) { kind = DestructKind::House; hp = 60.0f; }
        else if (o.name.rfind("smw_wall", 0) == 0 || o.name.rfind("smw_entrance", 0) == 0) { kind = DestructKind::House; hp = 220.0f; }
        else if (o.name.rfind("smw_column", 0) == 0 || o.name.rfind("sm_post", 0) == 0) { kind = DestructKind::House; hp = 120.0f; }
        else if (o.name.rfind("sm_tower", 0) == 0 || o.name.rfind("sm_dome", 0) == 0 || o.name.rfind("sm_well", 0) == 0 ||
                 o.name.rfind("sm_woodshop", 0) == 0) {
            kind = DestructKind::House;
            hp = dataHp > 0 ? dataHp : 250.0f;
        }
        else if (o.name == "log_launcher_base" || o.name == "guard_army_barracks" || o.name == "barracks_story2" || o.name == "barracks" ||
                 o.name == "bw_minibarracks" || o.name == "guard_ripperbarracks") {  // (the last three: rework - they spawn, so they can fall)
            kind = DestructKind::House;
            hp = dataHp > 0 ? dataHp : 600.0f;
        } else if (o.name == "ice_wall") {
            // rstory2_jetski_3: "Use ROCKET PICKUPS to break ICE WALLS, or ram them on TURBO" (hp ours).
            kind = DestructKind::House;
            hp = 150.0f;
        } else if (std::find(deathTypes.begin(), deathTypes.end(), o.type) != deathTypes.end()) {
            // Hit points ours where the data has none: walls for a giant's fists, the queen a boss.
            kind = DestructKind::House;
            hp = dataHp > 0 ? dataHp : (o.name.find("queen") != std::string::npos ? 1500.0f : 800.0f);
        } else continue;
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
    // Towers: their collapse rig and ruin model now, not mid-fight.
    towerRigs_.clear();
    for (size_t k = 0; k < destruct_.items.size(); ++k) {
        const Destructible& d = destruct_.items[k];
        if (d.kind != DestructKind::Tower) continue;
        const auto& o = objs[size_t(d.object)];
        auto m = std::make_unique<SkinnedModel>();
        const std::string anim = reg.modelAnim(o.type);
        if (!anim.empty() && m->load(w->fs(), *mc, renderer_, o.name, anim)) towerRigs_.push_back({int(k), std::move(m)});
        mc->get(o.name + "d.gbs", "", false);  // the ruin, cached
    }
    // Damage look: the data's scorched texture variants ("<texture>_d": Smartie canopies, stone)
    // and our char/ember overlays.
    if (!burnTexIds_[0]) {
        std::vector<uint8_t> charTex[3], embers;
        makeBurnTextures(128, 7u, charTex, embers);
        for (int k = 0; k < 3; ++k) burnTexIds_[k] = renderer_.uploadTexture(128, 128, charTex[k].data(), true, true);
        burnTexIds_[3] = renderer_.uploadTexture(128, 128, embers.data(), true, true);
    }
    w->setBurnTextures(burnTexIds_[0], burnTexIds_[1], burnTexIds_[2], burnTexIds_[3]);
    size_t variants = 0;
    for (const auto& d : destruct_.items) {
        auto& o = objs[size_t(d.object)];
        o.damagedTex.assign(o.model->parts.size(), 0);
        for (size_t p = 0; p < o.model->parts.size(); ++p) {
            const std::string& tn = o.model->parts[p].textureName;
            if (!tn.empty() && w->fs().exists(tn + "_d.tga")) { o.damagedTex[p] = mc->texture(tn + "_d"); ++variants; }
        }
        o.burn = o.glow = 0;
        o.damaged = false;
    }
    LOGI("Damage textures: %zu scorched variants from the data", variants);
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
    if (hero_ == Hero::Kabuto && !kabutoMayDamage(item)) return;  // a gate he hasn't grown into yet
    // The Reaper story's battles: "The enemy ARMY BARRACKS can only be destroyed by a TORNADO SPELL".
    if (hero_ == Hero::Delphi && !tornadoHitting_ && assetWorld_ && item >= 0 && size_t(item) < destruct_.items.size()) {
        const int o = destruct_.items[size_t(item)].object;
        if (o >= 0 && size_t(o) < assetWorld_->worldObjects().size() && assetWorld_->worldObjects()[size_t(o)].name == "guard_army_barracks") return;
    }
    if (!popupBlast_ && popupOnlyTarget(item)) return;  // "Only the POP-UP BOMB can destroy the GATES"
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
                if (lastHitT_.size() != destruct_.items.size()) lastHitT_.assign(destruct_.items.size(), -1e9f);
                lastHitT_[size_t(ev.index)] = float(time_);
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
                    dustBurst(d.base + Vec3{0, 1.0f, 0}, d.radius, 10);
                } else {  // tower: its own collapse clip on a skinned copy
                    sfx("ex_metll", top, 1.0f, 0.85f);
                    obj.hidden = true;
                    Collapse c;
                    c.item = ev.index;
                    c.xf = obj.placement;
                    // The collapse rig was loaded with the level (loading it here stalled the frame).
                    for (auto& pre : towerRigs_)
                        if (pre.first == ev.index && pre.second) { c.model = std::move(pre.second); break; }
                    if (!c.model) {
                        auto m = std::make_unique<SkinnedModel>();
                        const std::string anim = assetWorld_->registry().modelAnim(obj.type);
                        if (!anim.empty() && m->load(assetWorld_->fs(), *mc, renderer_, obj.name, anim)) c.model = std::move(m);
                    }
                    if (c.model) c.model->pose(renderer_, 0.0f);
                    collapses_.push_back(std::move(c));
                }
                kabutoObjectDestroyed(obj.name, d.base);
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
                d.smoke = d.stage == 1 ? 0.5f : 0.25f;
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
                if (d.smoke <= 0) { d.smoke = 0.3f; dustBurst(d.base + Vec3{0, 0.5f, 0}, d.radius * 0.8f, 2); }
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

void GameApp::debugDumpNear(float r) const {
    const Vec3 p = player_.position;
    for (size_t i = 0; i < chars_.size(); ++i)
        if (length(chars_[i].pos - p) < r) LOGI("  near: character %zu type %d rescue %d at %.2f m (ambient %d visible %d)", i, chars_[i].type, chars_[i].rescue, length(chars_[i].pos - p), int(chars_[i].ambient), int(chars_[i].visible));
    for (size_t i = 0; i < rescue_.smarties.size(); ++i)
        if (length(rescue_.smarties[i].pos - p) < r) LOGI("  near: rescue Smartie %zu at %.2f m, state %d", i, length(rescue_.smarties[i].pos - p), int(rescue_.smarties[i].state));
    for (size_t i = 0; i < villagers_.villagers.size(); ++i)
        if (length(villagers_.villagers[i].pos - p) < r) LOGI("  near: villager %zu at %.2f m = game (%.0f, %.0f)", i, length(villagers_.villagers[i].pos - p), -villagers_.villagers[i].pos.x / 0.2f, villagers_.villagers[i].pos.z / 0.2f);
    if (assetWorld_)
        for (const auto& o : assetWorld_->worldObjects())
            if (o.model && length(o.centre - p) < r + o.radius) {
                std::string tex;
                for (const auto& pt : o.model->parts) tex += pt.textureName + " ";
                LOGI("  near: object %s (radius %.1f m) at %.1f m: %s", o.name.c_str(), o.radius, length(o.centre - p), tex.c_str());
            }
}

void GameApp::debugReapersAhead(int n, float dist) {
    if (enemyPool_.empty() || !assetWorld_) return;
    const Vec3 f = Quat::fromAxisAngle({0, 1, 0}, player_.yaw).rotate(Vec3{0, 0, -1});
    const Vec3 side{-f.z, 0, f.x};
    for (int k = 0; k < n; ++k) {
        Vec3 p = player_.position + f * dist + side * ((float(k) - 0.5f * float(n - 1)) * 1.0f);
        float h;
        if (assetWorld_->terrain().heightAt(p.x, p.z, h)) p.y = h;
        debugReapers_.push_back(enemies_.spawn(p, player_.yaw + kPi));
        // Test hook: GVR_REAPER_HP=<hp> spawns them with that much health (one-blow kill tests).
        if (const char* hp = std::getenv("GVR_REAPER_HP"))
            if (debugReapers_.back() >= 0) enemies_.enemies[size_t(debugReapers_.back())].health = float(std::atof(hp));
    }
    LOGI("test: %d Reapers %.0f m ahead", n, dist);
}

void GameApp::debugBlastReapers() {
    int n = 0;
    for (int i : debugReapers_)
        if (i >= 0 && size_t(i) < enemies_.enemies.size() && enemies_.enemies[size_t(i)].state != EnemyState::Dead) {
            explode(enemies_.enemies[size_t(i)].pos + Vec3{0, 0.5f, 0}, 4.0f, 120.0f);
            ++n;
        }
    debugReapers_.clear();
    LOGI("test: blasts on %d Reapers", n);
}

// ---------------------------------------------------------------------------------------------
// Effects

void GameApp::initFx(ModelCache* mc) {
    if (batchesReady_) return;
    const char* names[int(FxTex::Count)] = {"b_smoke", "efx_smoke_black", "efx_smoke_mgrey", "b_dust", "efx_fire_trail", "efx_bfire_trail",
                                           "efx_gfire_trail", "b_drip", "EFX_splash"};
    const size_t caps[int(FxTex::Count)] = {192, 256, 256, 160, 256, 96, 48, 160, 96};
    for (int k = 0; k < int(FxTex::Count); ++k) {
        fxTex_[k] = mc->texture(names[k]);
        fxBatch_[k].init(renderer_, caps[k]);
    }
    const size_t bcaps[5] = {64, 64, 64, 160, 40};
    for (int k = 0; k < 5; ++k) bloodBatch_[k].init(renderer_, bcaps[k]);
    holeTex_ = mc->texture("blastmark_2");  // Ripper holes (game_app_ripper.cpp)
    holeBatch_.init(renderer_, 40);
    streakTex_ = mc->texture("b_thrust");  // a soft glow: its middle column across the streak
    streakBatch_.init(renderer_, 512);
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
    p.additive = tex == FxTex::Fire || tex == FxTex::FireBlue || tex == FxTex::FireGreen || tex == FxTex::Drip;
    fx_.emit(p);
}

void GameApp::dustBurst(const Vec3& at, float radius, int count) {
    for (int i = 0; i < count; ++i) {
        const float a = frand() * 6.28f;
        const Vec3 dir{std::cos(a), 0.25f + 0.3f * frand(), std::sin(a)};
        smokePuff(FxTex::Dust, at + Vec3{std::cos(a), 0, std::sin(a)} * (radius * 0.4f * frand()), dir * (1.5f + 2.0f * frand()),
                  std::min(3.0f, 1.0f + 0.4f * radius), std::min(6.5f, 3.0f + 0.6f * radius), 3.5f, 0.6f, 0.15f);
    }
}

// Cutscene effects (flick op 0x27) by the original definition's name. The definitions themselves
// (fx_*.gfx / fxb_*) aren't decoded, so each family gets a stand-in from our own particles: rock
// bursts are dust, shots and rockets are flame and smoke, glows and spells are coloured light,
// splashes are spray, blood is the game's blood. Unknown names ("test3") draw nothing.
void GameApp::cutsceneEffect(const std::string& name, const Vec3& at) {
    if (playDataFx(name, at)) return;  // the game's own effect (fx.bin)
    auto has = [&](const char* k) { return name.find(k) != std::string::npos; };
    if (has("stone_explode")) {
        const float r = has("large") ? 4.0f : has("medium") ? 2.0f : 1.0f;
        dustBurst(at, r, int(5 + 3 * r));
        for (int i = 0; i < 3; ++i) smokePuff(FxTex::Dust, at, Vec3{frand() - 0.5f, 2.0f + frand(), frand() - 0.5f}, 0.4f * r, 1.2f * r, 1.5f, 0.8f, -4.0f);
    } else if (has("splash") || has("spray")) {
        for (int i = 0; i < 10; ++i) {
            const float a = frand() * 6.28f;
            smokePuff(FxTex::Spray, at, Vec3{std::cos(a) * 1.2f, 1.5f + 1.5f * frand(), std::sin(a) * 1.2f}, 0.15f, 0.6f, 0.7f, 0.8f, -6.0f);
        }
    } else if (has("blood") || has("splat")) {
        bleed(at, Vec3{0, 1, 0}, false, 1.0f);
    } else if (has("shotgun") || has("rocket") || has("bazooka") || has("spear") || has("explosion")) {
        const float s = has("explosion") ? 3.0f : 1.0f;
        smokePuff(FxTex::Fire, at, Vec3{0, 0.3f, 0}, 0.35f * s, 0.9f * s, 0.18f * s, 1.0f, 0.0f);
        for (int i = 0; i < 3; ++i)
            smokePuff(FxTex::SmokeGrey, at, Vec3{frand() - 0.5f, 0.4f, frand() - 0.5f} * s, 0.25f * s, 1.3f * s, 1.4f, 0.5f, 0.3f);
    } else if (has("smoke_wall")) {
        for (int i = 0; i < 8; ++i)
            smokePuff(FxTex::Smoke, at + Vec3{(frand() - 0.5f) * 6.0f, 0, (frand() - 0.5f) * 6.0f}, Vec3{0, 0.3f, 0}, 2.0f, 6.0f, 5.0f, 0.7f, 0.1f);
    } else if (has("glow") || has("spell") || has("transformation") || has("portal") || has("cloak")) {
        const FxTex t = has("yan") ? FxTex::FireGreen : FxTex::FireBlue;
        const float s = has("transformation") || has("portal") ? 2.5f : 1.0f;
        for (int i = 0; i < 6; ++i)
            smokePuff(t, at, Vec3{frand() - 0.5f, frand() * 0.6f, frand() - 0.5f} * s, 0.3f * s, 0.8f * s, 1.2f, 0.9f, 0.2f);
    } else {
        static int unknown = 0;
        if (unknown++ < 8) LOGI("Cutscene effect '%s' has no stand-in", name.c_str());
    }
}

void GameApp::updateFx(float dt) {
    fx_.update(dt);
    updateDataFx(dt);
    // Rocket trails: a flame at the nozzle and grey smoke left hanging behind.
    for (const auto& r : rockets_) {
        if (rocketTrail(r)) continue;  // the original trail (fx.bin) rides it
        const Vec3 back = normalize(r.vel) * -1.0f;
        if (r.kind == 1) {  // Delphi's fire bolt: a blazing ball with a blue-white tail
            smokePuff(FxTex::Fire, r.pos, back * 1.0f, 0.75f, 0.2f, 0.12f, 1.0f, 0.0f);
            smokePuff(FxTex::FireBlue, r.pos + back * 0.25f, back * 2.0f, 0.45f, 0.1f, 0.35f, 0.9f, 0.0f);
            continue;
        }
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
    holeBatch_.clear();
    for (const auto& h : holes_) {
        // Where a Ripper burst out of the ground: a dark, dirt-streaked hole (blastmark_2, browned).
        if (length(h.pos - eye) > 90.0f || holeBatch_.full()) continue;
        const float a = std::min(1.0f, (90.0f - h.age) / 10.0f) * 0.95f;
        const Vec3 n = h.normal;
        const Vec3 t1 = normalize(cross(n, std::fabs(n.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0}));
        const Vec3 t2 = cross(n, t1);
        const float ca = std::cos(h.angle), sa = std::sin(h.angle), r = 0.5f * h.size;
        const Vec3 u = (t1 * ca + t2 * sa) * r, w = (t2 * ca - t1 * sa) * r;
        const Vec3 c0 = h.pos + n * 0.04f;
        const Vec3 q[4] = {c0 - u - w, c0 + u - w, c0 + u + w, c0 - u + w};
        const uint8_t col[4] = {110, 80, 50, uint8_t(a * 255.0f)};
        holeBatch_.add(q, col);
    }
    for (const auto& p : fx_.parts) {
        const float dd = length(p.pos - eye);
        if (dd > 200.0f) continue;
        // A puff filling the view costs a full screen of blending per eye (dust and smoke round a
        // collapsing building stacked dozens deep and the frame rate fell): those that would cover
        // most of the view fade out as the eye gets into them, and are skipped inside.
        const float ang = p.size() / std::max(0.1f, dd);
        if (ang > 1.3f) continue;
        const float near = ang > 0.65f ? (1.3f - ang) / 0.65f : 1.0f;
        const float a = p.opacity() * near;
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
    // Blood the same way: piranha bites spray it right at the player's own legs and chest, and drops
    // and sprays at the eye filled the view red - on the headset the jet-ski race and wading after a
    // fight took 19-23 ms of GPU a frame there. Near the eye they fade, and are skipped inside.
    const uint8_t opaque[4] = {255, 255, 255, 255};
    for (const auto& dr : blood_.drops) {
        const float ang = dr.size / std::max(0.05f, length(dr.pos - eye));
        if (ang > 0.25f) continue;  // (opaque: no fading; a drop this big on screen is a red disc)
        bloodBatch_[3].addBillboard(dr.pos, dr.size, right, up, 0.0f, opaque);
    }
    for (const auto& sp : blood_.sprays) {
        const float f = sp.age / sp.life;
        const float size = sp.size * (0.6f + 1.2f * f);
        const float ang = size / std::max(0.05f, length(sp.pos - eye));
        if (ang > 1.0f) continue;
        const float near = ang > 0.5f ? (1.0f - ang) / 0.5f : 1.0f;
        const uint8_t col[4] = {255, 255, 255, uint8_t((1.0f - f) * near * 255.0f)};
        bloodBatch_[4].addBillboard(sp.pos, size, right, up, sp.age * 2.0f, col);
    }
    for (auto& b : fxBatch_) b.upload(renderer_);
    for (auto& b : bloodBatch_) b.upload(renderer_);
    holeBatch_.upload(renderer_);
    buildStreaks(eye);
    jetGlowBatch_.clear();
    if (jetGlow_ > 0.01f) {
        // The jetpack's light on the ground under the player (additive: colour is brightness).
        const float r = 1.2f + 1.6f * jetGlow_;
        const Vec3 c0 = jetGlowAt_ + Vec3{0, 0.06f, 0};
        const Vec3 q[4] = {c0 + Vec3{-r, 0, r}, c0 + Vec3{r, 0, r}, c0 + Vec3{r, 0, -r}, c0 + Vec3{-r, 0, -r}};
        const uint8_t v = uint8_t(std::min(1.0f, jetGlow_) * 255.0f);
        const uint8_t col[4] = {v, v, v, 255};
        jetGlowBatch_.add(q, col);
    }
    jetGlowBatch_.upload(renderer_);
    buildMapBatch();
}

void GameApp::renderFx() {
    if (!batchesReady_) return;
    DrawParams dp;
    dp.blend = BlendMode::AlphaBlend;
    dp.unlit = true;
    dp.depthWrite = false;
    dp.doubleSided = true;
    if (holeTex_ && holeBatch_.count()) {
        dp.texture = holeTex_;
        holeBatch_.draw(renderer_, dp);
    }
    if (jetGlowTex_ && jetGlowBatch_.count()) {
        DrawParams gp = dp;
        gp.texture = jetGlowTex_;
        gp.blend = BlendMode::Additive;
        gp.noFog = true;
        jetGlowBatch_.draw(renderer_, gp);
    }
    const TextureId bt[5] = {bloodTex_[0], bloodTex_[1], bloodTex_[2], dropTex_, sprayTex_ ? sprayTex_ : dropTex_};
    for (int k = 0; k < 5; ++k) {
        dp.texture = bt[k];
        bloodBatch_[k].draw(renderer_, dp);
    }
    for (int k = 0; k < int(FxTex::Count); ++k) {
        const bool fire = k == int(FxTex::Fire) || k == int(FxTex::FireBlue) || k == int(FxTex::FireGreen) || k == int(FxTex::Drip);
        DrawParams fp = dp;
        fp.texture = fxTex_[k];
        if (fire) { fp.blend = BlendMode::Additive; fp.noFog = true; }
        fxBatch_[k].draw(renderer_, fp);
    }
    if (streakTex_ && streakBatch_.count()) {
        DrawParams sp = dp;
        sp.texture = streakTex_;
        sp.blend = BlendMode::Additive;
        sp.noFog = true;
        streakBatch_.draw(renderer_, sp);
    }
}

// The damage look of each destructible: scorched textures once damaged, char spreading with the
// hit points lost, embers glowing while badly damaged and for a while after it fell.
void GameApp::updateBurn() {
    if (!assetWorld_) return;
    auto& objs = assetWorld_->worldObjects();
    const float t = float(time_);
    for (size_t k = 0; k < destruct_.items.size(); ++k) {
        const Destructible& d = destruct_.items[k];
        if (d.object < 0 || size_t(d.object) >= objs.size()) continue;
        auto& o = objs[size_t(d.object)];
        const float b = burnLevel(d);
        const bool flammable = d.kind != DestructKind::Rock;
        o.burn = flammable ? b : 0.5f * b;  // stone only scorches
        o.damaged = d.stage >= 1;
        float g = 0.0f;
        if (flammable && b > 0.45f) {
            const float hot = d.alive() ? (b - 0.45f) / 0.55f : std::max(0.0f, 1.0f - d.t / 12.0f);  // cools after the fall
            const float flicker = 0.6f + 0.25f * std::sin(t * 7.3f + float(k)) + 0.15f * std::sin(t * 17.0f + 2.0f * float(k));
            g = 0.6f * std::min(1.0f, hot) * flicker;
        }
        o.glow = g;
    }
}

// Health bars over damaged buildings: shown once a building has taken damage, nearer than 90 m;
// bright for a few seconds after each hit. Green, yellow, red by what is left.
void GameApp::renderHealthBars() {
    if (!assetWorld_ || !text_.ready() || !playMode_) return;
    const Vec3 eye = renderer_.currentView().eyePosition;
    const float now = float(time_);
    for (size_t k = 0; k < destruct_.items.size(); ++k) {
        const Destructible& d = destruct_.items[k];
        if (!d.alive() || d.kind == DestructKind::Tree || d.kind == DestructKind::Rock || d.hp >= d.maxHp - 0.5f) continue;
        const Vec3 at = d.base + Vec3{0, d.height + 1.2f, 0};
        const float dist = length(at - eye);
        if (dist > 90.0f * heroScale_ || dist < 4.0f * heroScale_) continue;  // (not in your face: Kabuto's swung tree in a hut)
        const float since = k < lastHitT_.size() ? now - lastHitT_[k] : 99.0f;
        const float a = since < 4.0f ? 1.0f : 0.55f;
        const float f = std::max(0.0f, std::min(1.0f, d.hp / std::max(1.0f, d.maxHp)));
        const float w = std::min(std::max(1.6f, std::min(d.radius * 1.2f, 6.0f)) * std::max(1.0f, dist / 40.0f), dist * 0.18f), h = w * 0.09f;  // (at most ~10 degrees across)
        const Mat4 xf = Mat4::translation(at) * Mat4::rotation(headWorldRot_);
        text_.drawPanel(xf, w + h * 0.5f, h * 1.5f, {0.0f, 0.0f, 0.0f, 0.6f * a});
        const Vec4 col = f > 0.6f ? Vec4{0.3f, 0.9f, 0.3f, a} : (f > 0.3f ? Vec4{0.95f, 0.85f, 0.2f, a} : Vec4{0.95f, 0.25f, 0.15f, a});
        text_.drawPanel(xf * Mat4::translation({-0.5f * w * (1.0f - f), 0, 0.01f}), w * f, h, col);
    }
}

}  // namespace gvr
