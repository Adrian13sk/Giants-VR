// Campaign progress helpers (2026-10-28), platform-independent and unit-tested: which missions the main menu's
// mission select offers, and where each save slot's checkpoint file lives.
#pragma once

#include <set>
#include <string>
#include <utility>
#include <vector>

namespace gvr {

// (world, mission) in campaign order -> the ones offered: each hero's story (Meccaryn: other worlds, Delphi:
// Rstory*, Kabuto: Kstory*) up to and including its first mission not in `done` (lower case names), plus the
// checkpoint's mission wherever it is.
std::vector<std::pair<std::string, std::string>> unlockedMissions(const std::vector<std::pair<std::string, std::string>>& all,
                                                                  const std::set<std::string>& done, const std::string& checkpointMission);

// Slot 0 is `base` itself (the original save.txt); slot n is save<n+1>.txt beside it.
std::string saveSlotPath(const std::string& base, int slot);

}  // namespace gvr
