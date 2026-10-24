// fx.bin (loose in Bin/; fx_world_dust.bin is a byte-identical copy): the game's effect library.
//   header: u32 version=1, u32 size of what follows, u32 count
//   directory: count x {NUL-terminated name, u32 offset} - offsets from file offset 12
//   each effect: a byte-coded stream of ops {u8 op, fixed arguments}; it ends with op 0x02.
// The argument layout of all 91 ops was derived from the data: with it every one of the 679 effects
// parses to exactly its end (the next effect's start), which a wrong length would break. Arguments are
// u32, f32, a byte or a NUL-terminated name. Meanings are read from the values (FORMATS.md) - e.g.
// 0x03 {type} starts a particle emitter of a GameObj type (efx_flash, efx_fire_trail...), 0x06 opens
// its parameters and 0x07 closes it; 0x24 {projectile type, trail type, ?}; 0x2B {sound, sound}.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace gvr {

struct FxOp {
    uint8_t op = 0;
    std::vector<uint32_t> u;  // the u32 arguments, in order
    std::vector<float> f;     // the f32 arguments, in order
    std::string s;            // the name argument (if any)
    uint8_t b = 0;            // the byte argument (ops 0x76, 0x78)
};

struct FxDef {
    std::string name;
    std::vector<FxOp> ops;
};

struct FxLibrary {
    std::vector<FxDef> effects;
    std::unordered_map<std::string, size_t> byName;  // lower-case name -> index
    const FxDef* find(const std::string& name) const;
};

// The argument spec of an op ("u", "fff", "usu"...), nullptr if the op is unknown.
const char* fxOpSpec(uint8_t op);
bool parseFxLibrary(const uint8_t* data, size_t size, FxLibrary& out, std::string* err = nullptr);

}  // namespace gvr
