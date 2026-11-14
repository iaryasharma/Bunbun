// Bunbun — which media player the island shows, when several are running (Linux MPRIS).
// Pure, so it is unit-tested; mpris.cpp feeds it the players' states.
//
// Rules: a player picked by hand stays on show until it stops (then the island goes back
// to choosing for itself). Otherwise a playing player beats a paused one, and among equals
// the one that played most recently wins. A stopped player is never shown.
#pragma once

#include <string>
#include <vector>

namespace bb::mediapick {

enum class State { Stopped, Paused, Playing };

struct Candidate {
    std::string id;           // the player's bus name
    State state = State::Stopped;
    long long lastPlaying = 0;  // monotonic time of the last Playing seen
};

// The index of the player to show, or -1 when none. `*pickGone` is set when `picked` named a
// player that is no longer there or has stopped, so the caller can forget the pick.
inline int Choose(const std::vector<Candidate>& c, const std::string& picked, bool* pickGone = nullptr) {
    if (pickGone) *pickGone = false;
    if (!picked.empty()) {
        for (size_t i = 0; i < c.size(); ++i)
            if (c[i].id == picked && c[i].state != State::Stopped) return static_cast<int>(i);
        if (pickGone) *pickGone = true;
    }
    int best = -1;
    for (size_t i = 0; i < c.size(); ++i) {
        if (c[i].state == State::Stopped) continue;
        if (best < 0) { best = static_cast<int>(i); continue; }
        const Candidate& b = c[static_cast<size_t>(best)];
        const bool playing = c[i].state == State::Playing, bestPlaying = b.state == State::Playing;
        if ((playing && !bestPlaying) || (playing == bestPlaying && c[i].lastPlaying > b.lastPlaying)) best = static_cast<int>(i);
    }
    return best;
}

// One click on the source label: the listed source after the one on show, wrapping round.
// "" when there is nothing to pick.
inline std::string NextPick(const std::vector<std::string>& listed, const std::string& shown) {
    if (listed.empty()) return {};
    for (size_t i = 0; i < listed.size(); ++i)
        if (listed[i] == shown) return listed[(i + 1) % listed.size()];
    return listed[0];
}

// Names for the list: the same app twice ("Chrome", "Chrome") becomes "Chrome", "Chrome (2)".
inline std::vector<std::string> UniqueNames(const std::vector<std::string>& names) {
    std::vector<std::string> out;
    for (size_t i = 0; i < names.size(); ++i) {
        int same = 1;
        for (size_t k = 0; k < i; ++k)
            if (names[k] == names[i]) ++same;
        out.push_back(same > 1 ? names[i] + " (" + std::to_string(same) + ")" : names[i]);
    }
    return out;
}

}  // namespace bb::mediapick
