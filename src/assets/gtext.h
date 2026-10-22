// Game text database (Bin/GText<Language>.bin): mission names, objective messages, menu
// strings and every cutscene subtitle line. Layout (verified on GTextEnglish.bin, 2003 entries):
//   u32 n, u32 keyBytes, u32 valueBytes, {u32 keyOffset, u32 valueOffset}[n], keys, values
// (offsets relative to the start of the key / value blocks; NUL-terminated strings).
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace gvr {

class GameText {
public:
    bool parse(const uint8_t* d, size_t n, std::string* error = nullptr);
    // Text for a key (case-insensitive), or empty.
    std::string get(const std::string& key) const;
    size_t size() const { return text_.size(); }

private:
    std::unordered_map<std::string, std::string> text_;
};

}  // namespace gvr
