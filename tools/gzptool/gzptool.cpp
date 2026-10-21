// Command-line tool for the original data: list/extract GZP archives and validate every asset
// the runtime understands.
//   gzptool list <archive.gzp>
//   gzptool extract <archive.gzp> <name> <outfile>
//   gzptool validate <BinDir>          (decompresses every entry, parses every GBS / TGA)
//   gzptool model <BinDir> <name.gbs>  (prints model structure, bounds, winding statistics)
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "assets/gbs.h"
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
        } else if (ext == "tga") {
            Image img;
            if (decodeImage(d.data(), d.size(), img, &err)) ok["tga"]++;
            else { bad["tga"]++; std::printf("TGA FAIL %s: %s\n", name.c_str(), err.c_str()); }
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

int main(int argc, char** argv) {
    if (argc >= 3 && !std::strcmp(argv[1], "list")) return cmdList(argv[2]);
    if (argc >= 5 && !std::strcmp(argv[1], "extract")) return cmdExtract(argv[2], argv[3], argv[4]);
    if (argc >= 3 && !std::strcmp(argv[1], "validate")) return cmdValidate(argv[2]);
    if (argc >= 4 && !std::strcmp(argv[1], "model")) return cmdModel(argv[2], argv[3]);
    std::fprintf(stderr, "usage: gzptool list|extract|validate|model ...\n");
    return 2;
}
