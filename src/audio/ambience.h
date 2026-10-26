// Rework: procedural ambient loops (our own synthesis - the original has no world ambience
// sounds in its data: its "a_..." level markers name zones, the shore, trees, wind, rain).
// Each loop is seamless (its end cross-faded into its start) and mono.
#pragma once

#include <cstdint>

#include "audio/mixer.h"

namespace gvr {

enum class AmbienceKind { Surf, Wind, Leaves, River, Rumble, Rain, Thunder };  // (Thunder: a one-shot, not a loop)

// Generates `kind` at `rate` Hz (deterministic for a seed).
WavData makeAmbience(AmbienceKind kind, uint32_t rate, uint32_t seed = 1);
const char* ambienceName(AmbienceKind kind);  // the mixer's sound name ("amb_surf", ...)

}  // namespace gvr
