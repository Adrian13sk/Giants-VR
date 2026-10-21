#include "audio/music.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace gvr {

namespace {
uint32_t rd32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
std::string name16(const uint8_t* p) {
    size_t k = 0;
    while (k < 16 && p[k]) ++k;
    std::string s(reinterpret_cast<const char*>(p), k);
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
}  // namespace

bool MusicSet::parse(const uint8_t* d, size_t n, std::string* error) {
    auto fail = [&](const char* m) { if (error) *error = m; return false; };
    if (n < 12) return fail("too short");
    const uint32_t nSongs = rd32(d), nPairs = rd32(d + 4), nSets = rd32(d + 8);
    if (nSongs > 100000 || nPairs > 1000000 || nSets > 100000) return fail("bad counts");
    const size_t pairsAt = 12 + 4 * size_t(nSongs), setsAt = pairsAt + 32 * size_t(nPairs),
                 songsAt = setsAt + 32 * size_t(nSets);
    if (songsAt > n) return fail("tables overrun the file");
    std::vector<uint32_t> offs(nSongs);
    for (uint32_t i = 0; i < nSongs; ++i) offs[i] = rd32(d + 12 + 4 * i);
    std::vector<uint32_t> sorted = offs;  // two songs may share an offset (shipped file: one pair)
    std::sort(sorted.begin(), sorted.end());
    for (uint32_t i = 0; i < nSongs; ++i) {
        const size_t o = offs[i];
        auto nx = std::upper_bound(sorted.begin(), sorted.end(), offs[i]);
        const size_t e = nx != sorted.end() ? *nx : n;
        if (o < songsAt || o + 16 > n || e > n || e < o + 16) return fail("bad song offset");
        Song s;
        const std::string name = name16(d + o);
        size_t q = o + 16;
        while (q + 16 <= e && d[q] > 2) {
            s.segments.push_back(name16(d + q));
            q += 16;
        }
        if (q < e) {
            s.endMode = d[q];
            if (s.endMode == 2 && q + 17 <= e) s.next = name16(d + q + 1);
        }
        for (size_t k = 0; k < s.segments.size(); ++k) segmentHome_.emplace(s.segments[k], std::make_pair(name, k));
        songs_[name] = std::move(s);
    }
    for (uint32_t i = 0; i < nSets; ++i) {
        const std::string set = name16(d + setsAt + 32 * i), song = name16(d + setsAt + 32 * i + 16);
        sets_[set].push_back(song);
        songSet_.emplace(song, set);
    }
    for (uint32_t i = 0; i < nPairs; ++i) {
        const std::string from = name16(d + pairsAt + 32 * i), to = name16(d + pairsAt + 32 * i + 16);
        std::string song;
        size_t idx;
        if (!locate(to, song, idx)) continue;
        transitions_.emplace(std::make_pair(from, setOfSong(song)), to);
    }
    return true;
}

const MusicSet::Song* MusicSet::song(const std::string& name) const {
    auto it = songs_.find(name);
    return it == songs_.end() ? nullptr : &it->second;
}

std::vector<std::string> MusicSet::variants(const std::string& set) const {
    std::vector<std::string> v;
    if (songs_.count(set)) v.push_back(set);
    auto it = sets_.find(set);
    if (it != sets_.end())
        for (const auto& s : it->second)
            if (s != set && songs_.count(s)) v.push_back(s);
    return v;
}

std::string MusicSet::setOfSong(const std::string& song) const {
    auto it = songSet_.find(song);
    if (it != songSet_.end()) return it->second;
    // A set's own song ("ma1", "msb") or its loop ("msbloop" belongs to "msb").
    std::string s = song;
    if (s.size() > 4 && s.compare(s.size() - 4, 4, "loop") == 0) s.resize(s.size() - 4);
    while (!s.empty() && std::isdigit(static_cast<unsigned char>(s.back())) && s.size() > 3) s.pop_back();
    return s;
}

bool MusicSet::locate(const std::string& segment, std::string& song, size_t& index) const {
    auto it = segmentHome_.find(segment);
    if (it == segmentHome_.end()) return false;
    song = it->second.first;
    index = it->second.second;
    return true;
}

std::string MusicSet::transition(const std::string& fromSegment, const std::string& targetSet) const {
    auto it = transitions_.find(std::make_pair(fromSegment, targetSet));
    return it == transitions_.end() ? std::string() : it->second;
}

void MusicDirector::startSet(const std::string& set) {
    const auto v = set_.variants(set);
    currentSet_ = set;
    if (v.empty()) { playing_ = false; song_.clear(); return; }
    // The set's own song opens the mood; later loops may pick variants.
    song_ = v.front();
    index_ = 0;
    playing_ = true;
}

std::string MusicDirector::next() {
    if (wanted_ != currentSet_) {
        if (wanted_.empty()) { currentSet_.clear(); playing_ = false; return {}; }
        // Enter the new mood where the transition table says, else at its start.
        std::string entry;
        if (playing_) {
            const MusicSet::Song* s = set_.song(song_);
            if (s && index_ < s->segments.size()) entry = set_.transition(s->segments[index_], wanted_);
        }
        std::string song;
        size_t idx = 0;
        if (!entry.empty() && set_.locate(entry, song, idx)) {
            currentSet_ = wanted_;
            song_ = song;
            index_ = idx;
            playing_ = true;
        } else {
            startSet(wanted_);
        }
        if (!playing_) return {};
        return set_.song(song_)->segments[index_];
    }
    if (!playing_) return {};
    const MusicSet::Song* s = set_.song(song_);
    if (!s || s->segments.empty()) { playing_ = false; return {}; }
    if (++index_ >= s->segments.size()) {
        if (s->endMode == 2 && set_.song(s->next)) {
            song_ = s->next;
        } else {
            // Song over: play it again or another variant of the mood.
            const auto v = set_.variants(currentSet_);
            if (v.size() > 1 && s->endMode == 1) song_ = v[rnd() % v.size()];
        }
        index_ = 0;
        s = set_.song(song_);
        if (!s || s->segments.empty()) { playing_ = false; return {}; }
    }
    return s->segments[index_];
}

std::vector<float> decodeMusicRaw(const uint8_t* d, size_t n) {
    std::vector<float> out(n / 2);
    for (size_t i = 0; i + 1 < n; i += 2) {
        int16_t v;
        std::memcpy(&v, d + i, 2);
        out[i / 2] = float(v) / 32768.0f;
    }
    if (out.size() & 1) out.pop_back();
    return out;
}

}  // namespace gvr
