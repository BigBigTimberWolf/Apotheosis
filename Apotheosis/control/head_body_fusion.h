#pragma once

#include "types.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace control {

// A matched head is a second observation of the body target. Keep the body
// box unchanged: class-specific aim coordinates must remain body-relative.
inline void fuseHeadBody(std::vector<Candidate>& candidates, int headClassId,
                         int bodyClassId) {
    if (headClassId < 0 || bodyClassId < 0 || headClassId == bodyClassId) return;
    std::vector<bool> matched(candidates.size(), false);
    for (size_t headIndex = 0; headIndex < candidates.size(); ++headIndex) {
        const auto& head = candidates[headIndex];
        if (head.classId != headClassId || !head.box.valid()) continue;
        double bestScore = std::numeric_limits<double>::infinity();
        size_t bestBody = candidates.size();
        for (size_t bodyIndex = 0; bodyIndex < candidates.size(); ++bodyIndex) {
            const auto& body = candidates[bodyIndex];
            if (body.classId != bodyClassId || !body.box.valid()) continue;
            const double hw = head.box.w / body.box.w;
            const double hh = head.box.h / body.box.h;
            const double dx = std::abs(head.box.centerX() - body.box.centerX()) / body.box.w;
            const double dy = (head.box.centerY() - body.box.y) / body.box.h;
            if (hw > 0.85 || hh > 0.7 || dx > 0.6 || dy < -0.45 || dy > 0.55)
                continue;
            const double score = dx * dx + (dy + 0.05) * (dy + 0.05);
            if (score < bestScore) { bestScore = score; bestBody = bodyIndex; }
        }
        if (bestBody != candidates.size()) matched[headIndex] = true;
    }
    size_t index = 0;
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
        [&](const Candidate&) { return matched[index++]; }), candidates.end());
}

} // namespace control
