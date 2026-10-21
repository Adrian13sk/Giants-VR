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
        std::printf("  sub %-20s maxobj=%u tris=%zu tex=%s flags=0x%08x diffuse=%06x\n", s.name.c_str(),
                    s.maxObjIndex, s.indices.size() / 3, s.texture.c_str(), s.flags, s.diffuse);
    std::printf("signed volume after conversion: %.3f m^3\n", signedVolume(buildGbsParts(m)));
    return 0;
}

// Rest-pose consistency: rebuild GBS positions from SKN clusters posed by the animation's
// default node transforms; bones whose animation pose equals the export pose must match exactly.
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
    if (argc >= 5 && !std::strcmp(argv[1], "skincheck")) return cmdSkinCheck(argv[2], argv[3], argv[4]);
    if (argc >= 3 && !std::strcmp(argv[1], "list")) return cmdList(argv[2]);
    if (argc >= 5 && !std::strcmp(argv[1], "extract")) return cmdExtract(argv[2], argv[3], argv[4]);
    if (argc >= 3 && !std::strcmp(argv[1], "validate")) return cmdValidate(argv[2]);
    if (argc >= 4 && !std::strcmp(argv[1], "model")) return cmdModel(argv[2], argv[3]);
    std::fprintf(stderr, "usage: gzptool list|extract|validate|model ...\n");
    return 2;
}
