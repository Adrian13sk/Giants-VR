// Adaptive music from the original data (platform-independent, unit-tested).
//
// Bin/MusicSet.bin (decoded 2026-10-21; layout verified against the shipped file):
//   u32 nSongs, u32 nPairs, u32 nSetEntries, u32 songOffset[nSongs]
//   pairs[nPairs]       {char from[16], char to[16]}   "from this segment, enter at `to`"
//   setEntries[n]       {char set[16], char song[16]}  songs available for a mood set
//   song @ offset:      char name[16], char segment[16]... , u8 end (0/1 loop, 2 + char[16] chain)
// Segments are Music/<prefix>/<Segment>.raw: headerless 16-bit stereo PCM at 22050 Hz (the
// sound DLL opens every stream at 22050 Hz, 16-bit). Songs play their segments in order (the
// segment joins are sample-continuous). A mood change waits for the current segment to end
// and jumps to the entry segment the transition table gives (a bridge "trn_*" segment opens a
// variant song of the target set); without an entry it starts the target set's song.
// WHICH mood plays when is decided by native game code that is not decoded; GameApp maps
// gameplay states to sets (ambient, battle, victory, death) itself.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace gvr {

class MusicSet {
public:
    struct Song {
        std::vector<std::string> segments;
        int endMode = 1;          // 0/1: loop, 2: chain to `next`
        std::string next;
    };
    bool parse(const uint8_t* d, size_t n, std::string* error = nullptr);
    const Song* song(const std::string& name) const;
    // Songs a mood set can play (the set's own song first if it exists).
    std::vector<std::string> variants(const std::string& set) const;
    // Mood set a song belongs to.
    std::string setOfSong(const std::string& song) const;
    // Song containing a segment and its index; false if none.
    bool locate(const std::string& segment, std::string& song, size_t& index) const;
    // Entry segment when leaving `fromSegment` for `targetSet`, or empty.
    std::string transition(const std::string& fromSegment, const std::string& targetSet) const;
    size_t songCount() const { return songs_.size(); }
    size_t transitionCount() const { return transitions_.size(); }

private:
    std::map<std::string, Song> songs_;
    std::map<std::string, std::vector<std::string>> sets_;
    std::map<std::string, std::string> songSet_;
    std::map<std::string, std::pair<std::string, size_t>> segmentHome_;
    std::map<std::pair<std::string, std::string>, std::string> transitions_;
};

// Chooses the segment sequence. The caller feeds it to the audio output: next() is asked for
// the segment to queue after the current one.
class MusicDirector {
public:
    explicit MusicDirector(const MusicSet& set) : set_(set) {}
    // Requests a mood set (takes effect at the next segment boundary). Empty = silence.
    void request(const std::string& moodSet) { wanted_ = moodSet; }
    const std::string& current() const { return currentSet_; }
    // The segment to play after the one now queued last; empty = nothing (silence/unknown).
    std::string next();
    uint32_t seed = 12345u;

private:
    void startSet(const std::string& set);
    const MusicSet& set_;
    std::string wanted_, currentSet_, song_;
    size_t index_ = 0;
    bool playing_ = false;
    uint32_t rnd() { seed = seed * 1664525u + 1013904223u; return seed >> 8; }
};

// Headerless 16-bit stereo 22050 Hz PCM -> interleaved float stereo.
std::vector<float> decodeMusicRaw(const uint8_t* d, size_t n);

}  // namespace gvr
