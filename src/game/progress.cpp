#include "game/progress.h"

#include <algorithm>
#include <cctype>

namespace gvr {

namespace {
std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
int storyOf(const std::string& world) {
    const std::string w = lower(world);
    if (w.rfind("rstory", 0) == 0) return 1;
    if (w.rfind("kstory", 0) == 0) return 2;
    return 0;
}
}  // namespace

std::vector<std::pair<std::string, std::string>> unlockedMissions(const std::vector<std::pair<std::string, std::string>>& all,
                                                                  const std::set<std::string>& done, const std::string& checkpointMission) {
    bool blocked[3] = {false, false, false};
    const std::string cp = lower(checkpointMission);
    std::vector<std::pair<std::string, std::string>> open;
    for (const auto& wm : all) {
        const std::string m = lower(wm.second);
        const int k = storyOf(wm.first);
        if (!blocked[k] || (!cp.empty() && m == cp)) open.push_back(wm);
        if (!done.count(m)) blocked[k] = true;
    }
    return open;
}

std::string saveSlotPath(const std::string& base, int slot) {
    if (slot <= 0 || base.empty()) return base;
    const size_t slash = base.find_last_of("/\\");
    return (slash == std::string::npos ? std::string(".") : base.substr(0, slash)) + "/save" + std::to_string(slot + 1) + ".txt";
}

}  // namespace gvr
