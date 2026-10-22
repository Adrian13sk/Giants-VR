// Command-line tool for the original data: list/extract GZP archives and validate every asset
// the runtime understands.
//   gzptool list <archive.gzp>
//   gzptool extract <archive.gzp> <name> <outfile>
//   gzptool validate <BinDir>          (decompresses every entry, parses every GBS / TGA)
//   gzptool model <BinDir> <name.gbs>  (prints model structure, bounds, winding statistics)
#include <cmath>
#include "core/math.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <map>
#include <string>

#include "assets/gbs.h"
#include "assets/gti.h"
#include "assets/flick.h"
#include "assets/mission.h"
#include "assets/object_defs.h"
#include "assets/skin_anim.h"
#include "assets/world_bin.h"
#include <dirent.h>
#include "assets/gzp.h"
#include "assets/image.h"
#include "scene/model_cache.h"

using namespace gvr;

static int cmdList(const char* path) {
    GzpArchive a;
    std::string err;
    if (!a.open(path, &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    for (const auto& e : a.entries())
        std::printf("%-40s %9u %9u comp=%u\n", e.name.c_str(), e.uncompressedSize, e.storedSize, e.compression);
    std::printf("%zu entries\n", a.entries().size());
    return 0;
}

static int cmdExtract(const char* path, const char* name, const char* outPath) {
    GzpArchive a;
    std::string err;
    if (!a.open(path, &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    const GzpEntry* e = a.find(name);
    if (!e) { std::fprintf(stderr, "not found: %s\n", name); return 1; }
    std::vector<uint8_t> d;
    if (!a.extract(*e, d, &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    FILE* f = std::fopen(outPath, "wb");
    if (!f) return 1;
    std::fwrite(d.data(), 1, d.size(), f);
    std::fclose(f);
    std::printf("wrote %zu bytes\n", d.size());
    return 0;
}

// Signed volume of a triangle soup in runtime coordinates after buildGbsParts (positive means
// front faces are counter-clockwise when viewed from outside, i.e. GL convention).
static double signedVolume(const std::vector<BuiltPart>& parts) {
    double v = 0;
    for (const auto& p : parts) {
        const auto& vs = p.mesh.vertices;
        const auto& ix = p.mesh.indices;
        for (size_t i = 0; i + 2 < ix.size(); i += 3) {
            const float* a = vs[ix[i]].pos; const float* b = vs[ix[i + 1]].pos; const float* c = vs[ix[i + 2]].pos;
            v += (a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) +
                  a[2] * (b[0] * c[1] - b[1] * c[0])) / 6.0;
        }
    }
    return v;
}

static int cmdValidate(const char* dir) {
    AssetFS fs;
    int n = fs.mountAllArchives(dir);
    std::printf("mounted %d archives\n", n);
    fs.mountDirectory(dir);
    std::map<std::string, int> ok, bad;
    int volPos = 0, volNeg = 0;
    for (const std::string& name : fs.list()) {
        std::string ext = toLower(name.substr(name.find_last_of('.') + 1));
        std::vector<uint8_t> d;
        if (!fs.read(name, d)) { bad["extract"]++; std::printf("EXTRACT FAIL %s\n", name.c_str()); continue; }
        ok["extract"]++;
        std::string err;
        if (ext == "gbs") {
            GbsModel m;
            if (parseGbs(d.data(), d.size(), m, &err)) {
                ok["gbs"]++;
                double v = signedVolume(buildGbsParts(m));
                (v >= 0 ? volPos : volNeg)++;
            } else { bad["gbs"]++; std::printf("GBS FAIL %s: %s\n", name.c_str(), err.c_str()); }
        } else if (ext == "gti") {
            GtiTerrain t;
            if (parseGti(d.data(), d.size(), t, &err)) {
                ok["gti"]++;
                std::printf("GTI %-24s %ux%u cells, consumed %zu/%zu bytes\n", name.c_str(), t.tilesX, t.tilesY,
                            t.bytesConsumed, d.size());
            } else { bad["gti"]++; std::printf("GTI FAIL %s: %s\n", name.c_str(), err.c_str()); }
        } else if (ext == "skn") {
            SknModel sk;
            if (parseSkn(d.data(), d.size(), sk, &err)) ok["skn"]++;
            else { bad["skn"]++; std::printf("SKN FAIL %s: %s\n", name.c_str(), err.c_str()); }
        } else if (ext == "anm") {
            AnmAnimation an;
            if (parseAnm(d.data(), d.size(), an, &err)) {
                ok["anm"]++;
                if (an.unsupportedTracks) ok["anm-unsupported-tracks"] += int(an.unsupportedTracks);
            } else { bad["anm"]++; std::printf("ANM FAIL %s: %s\n", name.c_str(), err.c_str()); }
        } else if (ext == "tga") {
            Image img;
            if (decodeImage(d.data(), d.size(), img, &err)) ok["tga"]++;
            else { bad["tga"]++; std::printf("TGA FAIL %s: %s\n", name.c_str(), err.c_str()); }
        }
    }
    // World definition files (loose in Bin/).
    if (DIR* dd = opendir(dir)) {
        std::vector<std::string> worlds;
        while (dirent* de = readdir(dd)) {
            std::string n = de->d_name;
            if (n.size() > 6 && toLower(n.substr(0, 2)) == "w_" && toLower(n.substr(n.size() - 4)) == ".bin") worlds.push_back(n);
        }
        closedir(dd);
        std::sort(worlds.begin(), worlds.end());
        for (const auto& n : worlds) {
            std::vector<uint8_t> d;
            if (!fs.read(n, d)) continue;
            WorldDefinition w;
            std::string err;
            if (!parseWorldBin(d.data(), d.size(), w, &err)) { bad["world"]++; std::printf("WORLD FAIL %s: %s\n", n.c_str(), err.c_str()); continue; }
            (w.complete ? ok["world"] : ok["world-partial"])++;
            std::printf("WORLD %-24s terrain=%-18s records=%-4zu objects=%-3zu consumed %zu/%zu stop=%d\n", n.c_str(),
                        w.terrainName.c_str(), w.recordsParsed, w.objects.size(), w.streamConsumed, w.streamSize, w.stopOpcode);
        }
    }
    for (auto& [k, v] : ok) std::printf("ok   %-8s %d\n", k.c_str(), v);
    for (auto& [k, v] : bad) std::printf("FAIL %-8s %d\n", k.c_str(), v);
    std::printf("gbs signed volume (after winding fix): positive %d, negative %d\n", volPos, volNeg);
    int failures = 0;
    for (auto& [k, v] : bad) failures += v;
    return failures ? 1 : 0;
}

static int cmdModel(const char* dir, const char* name) {
    AssetFS fs;
    fs.mountAllArchives(dir);
    std::vector<uint8_t> d;
    if (!fs.read(name, d)) { std::fprintf(stderr, "not found\n"); return 1; }
    GbsModel m;
    std::string err;
    if (!parseGbs(d.data(), d.size(), m, &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    for (const auto& p : m.positions)
        for (int k = 0; k < 3; ++k) { mn[k] = std::fmin(mn[k], p[k]); mx[k] = std::fmax(mx[k], p[k]); }
    std::printf("%s flags=0x%x positions=%zu displayVerts=%zu maxObjs=%zu subObjects=%zu tris=%zu\n", name,
                m.flags, m.positions.size(), m.displayToPosition.size(), m.maxObjs.size(),
                m.subObjects.size(), m.triangleCount());
    std::printf("bounds (game units, Z-up): x[%.2f %.2f] y[%.2f %.2f] z[%.2f %.2f]\n", mn[0], mx[0], mn[1], mx[1], mn[2], mx[2]);
    for (const auto& s : m.subObjects)
    {
        float u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f, z0 = 1e9f, z1 = -1e9f;
        for (uint32_t di : s.indices) {
            if (di < m.uvs.size()) { u0 = std::fmin(u0, m.uvs[di][0]); u1 = std::fmax(u1, m.uvs[di][0]); v0 = std::fmin(v0, m.uvs[di][1]); v1 = std::fmax(v1, m.uvs[di][1]); }
            if (di < m.displayToPosition.size()) { const auto& p = m.positions[m.displayToPosition[di]]; z0 = std::fmin(z0, p[2]); z1 = std::fmax(z1, p[2]); }
        }
        std::printf("  sub %-20s maxobj=%u tris=%zu tex=%s flags=0x%08x diffuse=%06x uv[%.2f..%.2f x %.2f..%.2f] z[%.1f..%.1f]\n", s.name.c_str(),
                    s.maxObjIndex, s.indices.size() / 3, s.texture.c_str(), s.flags, s.diffuse, u0, u1, v0, v1, z0, z1);
    }
    std::printf("signed volume after conversion: %.3f m^3\n", signedVolume(buildGbsParts(m)));
    return 0;
}

// Rest-pose consistency: rebuild GBS positions from SKN clusters posed by the animation's
// default node transforms; bones whose animation pose equals the export pose must match exactly.
// Prints duration and root-motion of animations: per-node position-key travel for nodes that
// move, so walk/run speeds can be derived from the stride (no foot sliding).
static int cmdFlick(const char* dir, int n, char** names) {
    AssetFS fs;
    fs.mountAllArchives(dir);
    fs.mountDirectory(dir);
    for (int i = 0; i < n; ++i) {
        std::vector<uint8_t> d;
        FlickShot f;
        std::string err;
        const std::string file = std::string("flk_") + names[i] + ".bin";
        if (!fs.read(file, d) || !parseFlick(d.data(), d.size(), f, &err)) { std::printf("%s: %s\n", file.c_str(), err.c_str()); continue; }
        std::printf("%s: next '%s' director %d (%.2f s, %zu nodes) camera '%s' stream '%s' subtitles %zu\n", file.c_str(),
                    f.next.c_str(), f.hasDirector, f.duration(), f.director.nodes.size(), f.cameraNode.c_str(), f.stream.c_str(),
                    f.subtitles.size());
        for (const auto& s : f.subtitles) std::printf("   subtitle %s\n", s.c_str());
        if (!f.hasDirector) continue;
        for (int t = 0; t < f.director.tableCount(); ++t) {
            int rp = -1;
            AnmAnimation sub = f.director.subset(t, &rp);
            std::string roots;
            for (const auto& nd : sub.nodes) if (nd.parent < 0) roots += nd.name + " ";
            std::printf("   table %d: %zu nodes, roots [%s] outside parent '%s'\n", t, sub.nodes.size(), roots.c_str(),
                        rp >= 0 ? f.director.nodes[size_t(rp)].name.c_str() : "");
        }
        std::vector<Mat4> w;
        if (std::getenv("FLICK_NODES")) {
            f.director.evaluate(0.0f, w);
            for (size_t k = 0; k < f.director.nodes.size(); ++k) {
                const auto& nd = f.director.nodes[k];
                if (nd.parent >= 0 && f.director.nodes[size_t(nd.parent)].table == nd.table) continue;  // tree roots only
                std::printf("   node %-18s table %d parent %-14s pos (%.0f %.0f %.0f) keys %zu\n", nd.name.c_str(), nd.table,
                            nd.parent >= 0 ? f.director.nodes[size_t(nd.parent)].name.c_str() : "-", w[k].m[12], w[k].m[13],
                            w[k].m[14], nd.posKeys.size());
            }
        }
        if (const char* kn = std::getenv("FLICK_KEYS")) {
            auto it = f.director.byName.find(toLower(kn));
            if (it != f.director.byName.end()) {
                const auto& nd = f.director.nodes[size_t(it->second)];
                std::printf("   %s default pos (%.2f %.2f %.2f) rot (%.3f %.3f %.3f %.3f)\n", nd.name.c_str(), nd.pos.x, nd.pos.y,
                            nd.pos.z, nd.rot.x, nd.rot.y, nd.rot.z, nd.rot.w);
                std::printf("   key types %d %d %d\n", nd.keyType[0], nd.keyType[1], nd.keyType[2]);
                for (size_t j = 0; j < nd.rawKeys.size(); ++j)
                    std::printf("%s%.2f%s", j % 16 == 0 ? "   raw " : "", nd.rawKeys[j], j % 16 == 15 ? "\n" : " ");
                for (const auto& k : nd.posKeys) std::printf("   pos key t=%d (%.2f %.2f %.2f)\n", k.tick, k.value.x, k.value.y, k.value.z);
                for (const auto& k : nd.rotKeys) std::printf("   rot key t=%d (%.3f %.3f %.3f %.3f)\n", k.tick, k.value.x, k.value.y, k.value.z, k.value.w);
            }
        }
        for (float frac : {0.0f, 0.5f, 1.0f}) {
            f.director.evaluate(frac * f.duration() * 0.999f, w);
            auto show = [&](const std::string& node, const char* what) {
                auto it = f.director.byName.find(node);
                if (it == f.director.byName.end()) { std::printf("   t=%.1f %s '%s' not found\n", frac, what, node.c_str()); return; }
                const Mat4& m = w[size_t(it->second)];
                std::printf("   t=%.1f %-7s %-14s game (%.0f %.0f %.0f) -z (%.2f %.2f %.2f)\n", frac, what, node.c_str(), m.m[12],
                            m.m[13], m.m[14], -m.m[8], -m.m[9], -m.m[10]);
            };
            if (!f.cameraNode.empty()) { show(f.cameraNode, "camera"); show(f.cameraNode + ".target", "target"); }
            if (std::getenv("FLICK_NODES"))
                for (size_t k = 0; k < f.director.nodes.size(); ++k) {
                    const auto& nd = f.director.nodes[k];
                    const bool mover = toLower(nd.name).find("mo") != std::string::npos && toLower(nd.name).find("ver") != std::string::npos;
                    if (!mover && nd.parent >= 0 && f.director.nodes[size_t(nd.parent)].table == nd.table) continue;
                    const Mat4& m = w[k];
                    std::printf("   t=%.1f root %-16s pos (%.0f %.0f %.0f) -z (%.2f %.2f %.2f)\n", frac, nd.name.c_str(), m.m[12],
                                m.m[13], m.m[14], -m.m[8], -m.m[9], -m.m[10]);
                }
            for (const auto& a : f.actors)
                if (!a.node.empty()) show(a.node, ("type" + std::to_string(a.type)).c_str());
        }
    }
    return 0;
}

// Mission setup stream with model names, classes, triggers and path ids.
static int cmdMission(const char* dir, const char* name) {
    AssetFS fs;
    fs.mountAllArchives(dir);
    fs.mountDirectory(dir);
    ObjectRegistry reg;
    reg.build(dir);
    std::vector<uint8_t> d;
    MissionData m;
    std::string err;
    const std::string file = std::string("wm_") + name + ".bin";
    if (!fs.read(file, d) || !parseMission(d.data(), d.size(), m, &err)) { std::printf("%s: %s\n", file.c_str(), err.c_str()); return 1; }
    std::printf("%s: %zu objects, race %u, start flick '%s', 0x13 '%s', outro '%s', items", file.c_str(), m.objects.size(), m.setup[0],
                m.startFlick.c_str(), m.flick13.c_str(), m.flick19.c_str());
    for (uint32_t i : m.items) std::printf(" %u", i);
    std::printf("\n");
    std::map<std::string, int> counts;
    for (size_t i = 0; i < m.objects.size(); ++i) {
        const auto& o = m.objects[i];
        const ObjectType* t = reg.type(o.type);
        const std::string model = reg.modelName(o.type);
        const bool interesting = o.flags || o.group >= 0 || o.parent >= 0 || !o.flicks.empty() || (t && t->objectClass >= 0);
        ++counts[model];
        if (!interesting && !std::getenv("MISSION_ALL")) continue;
        std::printf("  #%-3zu type %4u %-22s class %3d hp %4d at (%7.0f %7.0f %5.0f) yaw %4.0f", i, o.type, model.c_str(),
                    t ? t->objectClass : -1, t ? int(t->hitPoints) : 0, o.x, o.y, o.z, o.yaw);
        if (o.flags) std::printf(" trig %u", o.flags);
        if (o.group >= 0) std::printf(" id %d", o.group);
        if (o.parent >= 0) std::printf(" on #%d (mount %u %u)", o.parent, o.mount[0], o.mount[1]);
        for (const auto& f : o.flicks) std::printf(" flk %s", f.c_str());
        std::printf("\n");
    }
    for (const auto& c : counts) std::printf("  %4d x %s\n", c.second, c.first.c_str());
    return 0;
}

static int cmdAnmInfo(const char* dir, int n, char** anims) {
    AssetFS fs;
    fs.mountAllArchives(dir);
    for (int i = 0; i < n; ++i) {
        std::vector<uint8_t> d;
        AnmAnimation an;
        std::string err;
        if (!fs.read(anims[i], d) || !parseAnm(d.data(), d.size(), an, &err)) { std::printf("%s: %s\n", anims[i], err.c_str()); continue; }
        std::printf("%s: %.2f s, %zu nodes, ticks %d..%d tpf %u fps %u\n", anims[i], an.durationSeconds(), an.nodes.size(),
                    an.startTick, an.endTick, an.ticksPerFrame, an.fps);
        if (std::getenv("ANM_NODES")) {
            std::vector<Mat4> w;
            an.evaluate(0.0f, w);
            for (size_t k = 0; k < an.nodes.size(); ++k)
                std::printf("   node %-16s parent %2d rest world (%.2f %.2f %.2f)\n", an.nodes[k].name.c_str(), an.nodes[k].parent,
                            w[k].m[12], w[k].m[13], w[k].m[14]);
        }
        for (size_t k = 0; k < an.nodes.size(); ++k) {
            const auto& nd = an.nodes[k];
            if (nd.posKeys.size() < 2 || (nd.parent >= 0 && an.nodes[size_t(nd.parent)].parent >= 0)) continue;
            const Vec3 a = nd.posKeys.front().value, b = nd.posKeys.back().value;
            float zmin = 1e9f, zmax = -1e9f;
            for (const auto& kk : nd.posKeys) { zmin = std::min(zmin, kk.value.z); zmax = std::max(zmax, kk.value.z); }
            std::printf("   %-14s parent %2d keys %3zu  first(%.2f %.2f %.2f) last(%.2f %.2f %.2f)  z %.2f..%.2f\n", nd.name.c_str(),
                        nd.parent, nd.posKeys.size(), a.x, a.y, a.z, b.x, b.y, b.z, zmin, zmax);
        }
        // Stride: velocity of the lowest foot/toe node (the planted one) in game units/s.
        std::vector<int> feet;
        for (size_t k = 0; k < an.nodes.size(); ++k) {
            const std::string l = toLower(an.nodes[k].name);
            if (l.find("foot") != std::string::npos || l.find("toe") != std::string::npos) feet.push_back(int(k));
        }
        if (feet.empty()) {
            // Creatures without foot/toe names (e.g. the Ripper): use every leaf node.
            std::vector<int> kids(an.nodes.size(), 0);
            for (const auto& nd : an.nodes)
                if (nd.parent >= 0) ++kids[size_t(nd.parent)];
            for (size_t k = 0; k < an.nodes.size(); ++k)
                if (!kids[k] && an.nodes[k].parent >= 0) feet.push_back(int(k));
        }
        if (feet.empty() || an.durationSeconds() <= 0) continue;
        const int steps = 120;
        const float dur = an.durationSeconds(), dt = dur / steps;
        std::vector<Mat4> w0, w1;
        double sx = 0, sy = 0; int cnt = 0;
        std::vector<float> speeds;
        for (int t = 0; t < steps; ++t) {
            an.evaluate(t * dt, w0);
            an.evaluate(t * dt + dt * 0.5f, w1);
            int low = feet[0]; float lz = 1e9f;
            for (int f : feet) { const float z = w0[size_t(f)].m[14]; if (z < lz) { lz = z; low = f; } }
            const float vx = (w1[size_t(low)].m[12] - w0[size_t(low)].m[12]) / (dt * 0.5f);
            const float vy = (w1[size_t(low)].m[13] - w0[size_t(low)].m[13]) / (dt * 0.5f);
            sx += vx; sy += vy; ++cnt;
            speeds.push_back(std::sqrt(vx * vx + vy * vy));
        }
        std::sort(speeds.begin(), speeds.end());
        std::printf("   planted-foot velocity: mean (%.1f, %.1f) units/s, median |v| %.1f units/s, feet:", sx / cnt, sy / cnt,
                    speeds[speeds.size() / 2]);
        for (int f : feet) std::printf(" %s", an.nodes[size_t(f)].name.c_str());
        std::printf("\n");
    }
    return 0;
}

static int cmdSkinCheck(const char* dir, const char* base, const char* anim) {
    AssetFS fs;
    fs.mountAllArchives(dir);
    std::vector<uint8_t> d;
    std::string err;
    GbsModel g; SknModel sk; AnmAnimation an;
    if (!fs.read(std::string(base) + ".gbs", d) || !parseGbs(d.data(), d.size(), g, &err)) { std::printf("gbs: %s\n", err.c_str()); return 1; }
    if (!fs.read(std::string(base) + ".skn", d) || !parseSkn(d.data(), d.size(), sk, &err)) { std::printf("skn: %s\n", err.c_str()); return 1; }
    if (!fs.read(anim, d) || !parseAnm(d.data(), d.size(), an, &err)) { std::printf("anm: %s\n", err.c_str()); return 1; }
    const bool keyed = std::getenv("SKINCHECK_KEYED") != nullptr;
    // Default (static) transforms only: drop keys (unless SKINCHECK_KEYED is set).
    if (!keyed)
        for (auto& n : an.nodes) { n.posKeys.clear(); n.rotKeys.clear(); n.scaleKeys.clear(); }
    std::vector<Mat4> world;
    an.evaluate(0.0f, world);
    const bool dump = std::getenv("SKINCHECK_DUMP") != nullptr;
    if (dump)
        for (size_t i = 0; i < an.nodes.size(); ++i) {
            const auto& n = an.nodes[i];
            std::printf("node %zu %-12s parent %d pos(%.2f %.2f %.2f) rot(%.3f %.3f %.3f %.3f) scale(%.2f %.2f %.2f)%s keys p%zu r%zu s%zu\n",
                        i, n.name.c_str(), n.parent, n.pos.x, n.pos.y, n.pos.z, n.rot.x, n.rot.y, n.rot.z, n.rot.w,
                        n.scale.x, n.scale.y, n.scale.z, n.mirrored ? " MIRRORED" : "", n.posKeys.size(), n.rotKeys.size(),
                        n.scaleKeys.size());
        }
    std::map<std::string, std::array<double, 7>> cent;  // rest xyz, posed xyz, n
    int exact = 0, total = 0;
    std::map<std::string, std::pair<int, int>> perBone;
    for (size_t o = 0; o < sk.objects.size() && o < g.maxObjs.size(); ++o)
        for (const auto& c : sk.objects[o].clusters) {
            auto bn = sk.boneNames.find(c.bone);
            if (bn == sk.boneNames.end()) continue;
            auto ni = an.byName.find(toLower(bn->second));
            if (ni == an.byName.end()) continue;
            for (const auto& v : c.verts) {
                Vec4 w = world[size_t(ni->second)] * Vec4{v.x, v.y, v.z, 1};
                const auto& p = g.positions[size_t(g.maxObjs[o].vstart) + v.index];
                const float e = std::sqrt((w.x - p[0]) * (w.x - p[0]) + (w.y - p[1]) * (w.y - p[1]) + (w.z - p[2]) * (w.z - p[2]));
                ++total;
                if (dump)
                    std::printf("  v%-3u %-8s local(%7.2f %7.2f %7.2f) rest(%7.2f %7.2f %7.2f) posed(%7.2f %7.2f %7.2f)\n",
                                v.index, bn->second.c_str(), v.x, v.y, v.z, p[0], p[1], p[2], w.x, w.y, w.z);
                auto& ce = cent[bn->second];
                ce[0] += p[0]; ce[1] += p[1]; ce[2] += p[2]; ce[3] += w.x; ce[4] += w.y; ce[5] += w.z; ce[6] += 1;
                auto& pb = perBone[bn->second];
                ++pb.second;
                if (e < 1e-3f) { ++exact; ++pb.first; }
            }
        }
    for (auto& [b, c] : perBone) {
        const auto& ce = cent[b];
        const double n = ce[6];
        std::printf("  %-14s %3d/%3d exact  rest(%.1f %.1f %.1f) posed(%.1f %.1f %.1f)\n", b.c_str(), c.first, c.second,
                    ce[0] / n, ce[1] / n, ce[2] / n, ce[3] / n, ce[4] / n, ce[5] / n);
    }
    std::printf("%s + %s: %d/%d vertices reproduced exactly (<1 mm game units)\n", base, anim, exact, total);
    return exact > 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc >= 4 && !std::strcmp(argv[1], "anminfo")) return cmdAnmInfo(argv[2], argc - 3, argv + 3);
    if (argc >= 4 && !std::strcmp(argv[1], "mission")) return cmdMission(argv[2], argv[3]);
    if (argc >= 4 && !std::strcmp(argv[1], "flick")) return cmdFlick(argv[2], argc - 3, argv + 3);
    if (argc >= 5 && !std::strcmp(argv[1], "skincheck")) return cmdSkinCheck(argv[2], argv[3], argv[4]);
    if (argc >= 3 && !std::strcmp(argv[1], "list")) return cmdList(argv[2]);
    if (argc >= 5 && !std::strcmp(argv[1], "extract")) return cmdExtract(argv[2], argv[3], argv[4]);
    if (argc >= 3 && !std::strcmp(argv[1], "validate")) return cmdValidate(argv[2]);
    if (argc >= 4 && !std::strcmp(argv[1], "model")) return cmdModel(argv[2], argv[3]);
    std::fprintf(stderr, "usage: gzptool list|extract|validate|model ...\n");
    return 2;
}
