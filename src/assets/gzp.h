// GZP archive reader (Giants: Citizen Kabuto data archives, e.g. Bin/xx_*.gzp).
//
// Layout (verified against the GOG 1.497 data, see docs/formats/GZP.md):
//   u32 magic = 0x6608F101, u32 indexOffset
//   at indexOffset: u32 unknown(0), u32 fileCount, then fileCount entries of
//     u32 storedSize, u32 uncompressedSize, u32 dosDate, u32 dataOffset, u8 compression,
//     u8 nameLength, char name[nameLength] (NUL-terminated)
//   at dataOffset: 16-byte local header {u32 storedSize, u32 uncompressedSize, u32 date,
//     u32 compression} followed by (storedSize - 16) payload bytes.
//   compression 1 = LZSS (Okumura variant, 4 KiB window), 2 = stored.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace gvr {

struct GzpEntry {
    std::string name;       // as stored (original case)
    uint32_t storedSize = 0;
    uint32_t uncompressedSize = 0;
    uint32_t dosDate = 0;
    uint32_t dataOffset = 0;
    uint8_t compression = 0;
};

class GzpArchive {
public:
    GzpArchive() = default;
    GzpArchive(GzpArchive&& o) noexcept;
    GzpArchive& operator=(GzpArchive&& o) noexcept;
    GzpArchive(const GzpArchive&) = delete;
    GzpArchive& operator=(const GzpArchive&) = delete;
    ~GzpArchive();
    // Reads only the header and index; entry data is read on demand with pread(), so mounting
    // every archive of the game (~440 MB) costs only the index memory.
    bool open(const std::string& path, std::string* error = nullptr);
    bool openMemory(std::vector<uint8_t> data, const std::string& label, std::string* error = nullptr);

    const std::vector<GzpEntry>& entries() const { return entries_; }
    const GzpEntry* find(const std::string& name) const;  // case-insensitive
    bool extract(const GzpEntry& e, std::vector<uint8_t>& out, std::string* error = nullptr) const;
    const std::string& path() const { return path_; }

private:
    bool parseIndex(const uint8_t* idx, size_t idxSize, uint32_t indexOff, uint64_t fileSize,
                    std::string* error);
    bool readAt(uint64_t off, void* dst, size_t n) const;
    std::string path_;
    std::vector<uint8_t> data_;  // only for archives opened from memory
    int fd_ = -1;
    std::vector<GzpEntry> entries_;
    std::unordered_map<std::string, size_t> byLowerName_;
};

// Decodes the LZSS stream used by GZP compression type 1. Returns false if the input ends
// before `expectedSize` bytes were produced.
bool lzssDecompress(const uint8_t* in, size_t inSize, size_t expectedSize, std::vector<uint8_t>& out);

std::string toLower(std::string s);

// A virtual file system over several GZP archives plus loose directories, searched in mount
// order (later mounts override earlier ones, like the game's patch archives).
class AssetFS {
public:
    bool mountArchive(const std::string& path, std::string* error = nullptr);
    void mountDirectory(const std::string& dir);
    // Mounts every *.gzp in a directory (sorted by name). Returns number mounted.
    int mountAllArchives(const std::string& dir);
    bool read(const std::string& name, std::vector<uint8_t>& out) const;
    bool exists(const std::string& name) const;
    std::vector<std::string> list(const std::string& suffix = "") const;
    size_t archiveCount() const { return archives_.size(); }

private:
    std::vector<GzpArchive> archives_;
    std::vector<std::string> dirs_;
    // Per mounted directory: lower-case file name -> actual name (the data mixes cases, e.g.
    // flk_story1_Hatch_dilemma.bin is referenced as story1_hatch_dilemma).
    std::vector<std::unordered_map<std::string, std::string>> dirIndex_;
    std::string resolve(size_t dir, const std::string& name) const;
};

}  // namespace gvr
