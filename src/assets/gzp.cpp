#include "assets/gzp.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <set>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/log.h"

namespace gvr {

namespace {
constexpr uint32_t kGzpMagic = 0x6608F101u;

uint32_t rd32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz < 0) { std::fclose(f); return false; }
    out.resize(static_cast<size_t>(sz));
    size_t got = sz ? std::fread(out.data(), 1, out.size(), f) : 0;
    std::fclose(f);
    return got == out.size();
}

void setErr(std::string* e, const std::string& msg) {
    if (e) *e = msg;
}
}  // namespace

std::string toLower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

bool lzssDecompress(const uint8_t* in, size_t n, size_t expected, std::vector<uint8_t>& out) {
    out.clear();
    out.reserve(expected);
    uint8_t window[4096];
    std::memset(window, 0x20, sizeof(window));
    unsigned r = 0xFEE;
    unsigned flags = 0;
    size_t i = 0;
    while (out.size() < expected) {
        flags >>= 1;
        if ((flags & 0x100) == 0) {
            if (i >= n) break;
            flags = in[i++] | 0xFF00u;  // high byte counts the 8 remaining flag bits
        }
        if (flags & 1) {
            if (i >= n) break;
            uint8_t c = in[i++];
            out.push_back(c);
            window[r] = c;
            r = (r + 1) & 0xFFF;
        } else {
            if (i + 1 >= n) break;
            unsigned b1 = in[i++], b2 = in[i++];
            unsigned pos = b1 | ((b2 & 0xF0u) << 4);
            unsigned len = (b2 & 0x0Fu) + 3;
            for (unsigned k = 0; k < len && out.size() < expected; ++k) {
                uint8_t c = window[(pos + k) & 0xFFF];
                out.push_back(c);
                window[r] = c;
                r = (r + 1) & 0xFFF;
            }
        }
    }
    return out.size() == expected;
}

GzpArchive::GzpArchive(GzpArchive&& o) noexcept { *this = std::move(o); }

GzpArchive& GzpArchive::operator=(GzpArchive&& o) noexcept {
    if (this != &o) {
        if (fd_ >= 0) ::close(fd_);
        path_ = std::move(o.path_);
        data_ = std::move(o.data_);
        entries_ = std::move(o.entries_);
        byLowerName_ = std::move(o.byLowerName_);
        fd_ = o.fd_;
        o.fd_ = -1;
    }
    return *this;
}

GzpArchive::~GzpArchive() {
    if (fd_ >= 0) ::close(fd_);
}

bool GzpArchive::readAt(uint64_t off, void* dst, size_t n) const {
    if (fd_ < 0) {
        if (off + n > data_.size()) return false;
        std::memcpy(dst, data_.data() + off, n);
        return true;
    }
    uint8_t* p = static_cast<uint8_t*>(dst);
    while (n > 0) {
        ssize_t got = ::pread(fd_, p, n, static_cast<off_t>(off));
        if (got <= 0) return false;
        p += got;
        off += static_cast<uint64_t>(got);
        n -= static_cast<size_t>(got);
    }
    return true;
}

bool GzpArchive::open(const std::string& path, std::string* error) {
    path_ = path;
    entries_.clear();
    byLowerName_.clear();
    data_.clear();
    if (fd_ >= 0) ::close(fd_);
    fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd_ < 0) {
        setErr(error, "cannot open " + path);
        return false;
    }
    struct stat st;
    if (fstat(fd_, &st) != 0) {
        setErr(error, "cannot stat " + path);
        return false;
    }
    const uint64_t fileSize = static_cast<uint64_t>(st.st_size);
    uint8_t hdr[8];
    if (fileSize < 8 || !readAt(0, hdr, 8) || rd32(hdr) != kGzpMagic) {
        setErr(error, path + ": bad GZP magic");
        return false;
    }
    const uint32_t indexOff = rd32(hdr + 4);
    if (static_cast<uint64_t>(indexOff) + 8 > fileSize) {
        setErr(error, path + ": index offset out of range");
        return false;
    }
    std::vector<uint8_t> idx(static_cast<size_t>(fileSize - indexOff));
    if (!readAt(indexOff, idx.data(), idx.size())) {
        setErr(error, path + ": cannot read index");
        return false;
    }
    return parseIndex(idx.data(), idx.size(), indexOff, fileSize, error);
}

bool GzpArchive::openMemory(std::vector<uint8_t> d, const std::string& label, std::string* error) {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    path_ = label;
    data_ = std::move(d);
    entries_.clear();
    byLowerName_.clear();
    if (data_.size() < 8 || rd32(data_.data()) != kGzpMagic) {
        setErr(error, label + ": bad GZP magic");
        return false;
    }
    const uint32_t indexOff = rd32(data_.data() + 4);
    if (static_cast<uint64_t>(indexOff) + 8 > data_.size()) {
        setErr(error, label + ": index offset out of range");
        return false;
    }
    return parseIndex(data_.data() + indexOff, data_.size() - indexOff, indexOff, data_.size(), error);
}

bool GzpArchive::parseIndex(const uint8_t* idx, size_t idxSize, uint32_t indexOff, uint64_t fileSize,
                            std::string* error) {
    (void)indexOff;
    if (idxSize < 8) {
        setErr(error, path_ + ": truncated index header");
        return false;
    }
    const uint32_t count = rd32(idx + 4);
    size_t p = 8;
    entries_.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (p + 18 > idxSize) {
            setErr(error, path_ + ": truncated index");
            return false;
        }
        const uint8_t* e = idx + p;
        GzpEntry ent;
        ent.storedSize = rd32(e);
        ent.uncompressedSize = rd32(e + 4);
        ent.dosDate = rd32(e + 8);
        ent.dataOffset = rd32(e + 12);
        ent.compression = e[16];
        const uint8_t nameLen = e[17];
        if (p + 18 + nameLen > idxSize) {
            setErr(error, path_ + ": truncated name");
            return false;
        }
        const char* nm = reinterpret_cast<const char*>(e + 18);
        ent.name.assign(nm, strnlen(nm, nameLen));
        p += 18 + nameLen;
        if (static_cast<uint64_t>(ent.dataOffset) + ent.storedSize > fileSize || ent.storedSize < 16) {
            LOGW("%s: entry %s has out-of-range data; skipped", path_.c_str(), ent.name.c_str());
            continue;
        }
        byLowerName_[toLower(ent.name)] = entries_.size();
        entries_.push_back(std::move(ent));
    }
    return true;
}

const GzpEntry* GzpArchive::find(const std::string& name) const {
    auto it = byLowerName_.find(toLower(name));
    return it == byLowerName_.end() ? nullptr : &entries_[it->second];
}

bool GzpArchive::extract(const GzpEntry& e, std::vector<uint8_t>& out, std::string* error) const {
    const size_t payloadSize = e.storedSize - 16;
    std::vector<uint8_t> buf(payloadSize);
    if (!readAt(uint64_t(e.dataOffset) + 16, buf.data(), payloadSize)) {
        setErr(error, e.name + ": read failed");
        return false;
    }
    const uint8_t* payload = buf.data();
    if (e.compression == 2) {
        if (payloadSize < e.uncompressedSize) {
            setErr(error, e.name + ": stored payload shorter than declared size");
            return false;
        }
        out.assign(payload, payload + e.uncompressedSize);
        return true;
    }
    if (e.compression == 1) {
        if (!lzssDecompress(payload, payloadSize, e.uncompressedSize, out)) {
            setErr(error, e.name + ": LZSS stream ended early");
            return false;
        }
        return true;
    }
    setErr(error, e.name + ": unknown compression " + std::to_string(e.compression));
    return false;
}

// ---------------------------------------------------------------------------------------------

bool AssetFS::mountArchive(const std::string& path, std::string* error) {
    GzpArchive a;
    if (!a.open(path, error)) return false;
    archives_.push_back(std::move(a));
    return true;
}

void AssetFS::mountDirectory(const std::string& dir) {
    dirs_.push_back(dir);
    std::unordered_map<std::string, std::string> idx;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* de = readdir(d)) idx[toLower(de->d_name)] = de->d_name;
        closedir(d);
    }
    dirIndex_.push_back(std::move(idx));
}

std::string AssetFS::resolve(size_t dir, const std::string& name) const {
    auto it = dirIndex_[dir].find(toLower(name));
    return dirs_[dir] + "/" + (it != dirIndex_[dir].end() ? it->second : name);
}

int AssetFS::mountAllArchives(const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d) return 0;
    std::vector<std::string> names;
    while (dirent* de = readdir(d)) {
        std::string n = de->d_name;
        if (n.size() > 4 && toLower(n.substr(n.size() - 4)) == ".gzp") names.push_back(n);
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    // aanoeng.gzp holds language-neutral versions of signs (the drop-off pads, shop signs): the
    // English ones stay, so it goes last (lowest priority).
    std::stable_partition(names.begin(), names.end(), [](const std::string& n) { return toLower(n) != "aanoeng.gzp"; });
    int mounted = 0;
    for (const auto& n : names) {
        std::string err;
        if (mountArchive(dir + "/" + n, &err)) ++mounted;
        else LOGW("mount %s failed: %s", n.c_str(), err.c_str());
    }
    return mounted;
}

bool AssetFS::read(const std::string& name, std::vector<uint8_t>& out) const {
    for (size_t i = dirs_.size(); i-- > 0;)
        if (readFile(resolve(i, name), out)) return true;
    // Archives in name order, the first holding the file wins: the override packs are named to
    // sort first (aablood.gzp: red blood over the green in xx_efx.gzp; arpfix.gzp).
    for (auto it = archives_.begin(); it != archives_.end(); ++it) {
        if (const GzpEntry* e = it->find(name)) {
            std::string err;
            if (it->extract(*e, out, &err)) return true;
            LOGW("extract %s from %s failed: %s", name.c_str(), it->path().c_str(), err.c_str());
        }
    }
    return false;
}

bool AssetFS::exists(const std::string& name) const {
    for (const auto& a : archives_)
        if (a.find(name)) return true;
    struct stat st;
    for (size_t i = 0; i < dirs_.size(); ++i)
        if (stat(resolve(i, name).c_str(), &st) == 0) return true;
    return false;
}

std::vector<std::string> AssetFS::list(const std::string& suffix) const {
    std::set<std::string> out;
    const std::string ls = toLower(suffix);
    for (const auto& a : archives_)
        for (const auto& e : a.entries()) {
            std::string l = toLower(e.name);
            if (ls.empty() || (l.size() >= ls.size() && l.compare(l.size() - ls.size(), ls.size(), ls) == 0))
                out.insert(e.name);
        }
    return {out.begin(), out.end()};
}

}  // namespace gvr
