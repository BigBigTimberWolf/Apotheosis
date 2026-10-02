#pragma once
#include <cstdint>

#ifdef __CUDACC__
#define CROSS_CLUSTER_HD __host__ __device__
#else
#define CROSS_CLUSTER_HD
#endif

namespace crosshair {
struct CentroidComponent {
    int count = 0, sumX = 0, sumY = 0;
    int left = 2147483647, top = 2147483647, right = -1, bottom = -1;
};

// Work in ROI coordinates. Keep whole components: clipping a background blob
// to a small window would turn it into an apparently valid small crosshair.
CROSS_CLUSTER_HD inline bool centroidComponentValid(const CentroidComponent& c, int w, int h) {
    if (!c.count || c.left <= 0 || c.top <= 0 || c.right >= w-1 || c.bottom >= h-1)
        return false;
    const int bw = c.right-c.left+1, bh = c.bottom-c.top+1;
    if (bw > 48 || bh > 48) return false;
    // A large solid patch is background, but a sparse cross/ring is allowed.
    return c.count <= 64 || c.count * 3 < bw * bh * 2;
}

CROSS_CLUSTER_HD inline unsigned long long centroidComponentKey(
    const CentroidComponent& c, int index, int referenceX, int referenceY) {
    const int dx = c.sumX/c.count-referenceX, dy = c.sumY/c.count-referenceY;
    const unsigned int distance = static_cast<unsigned int>(dx*dx+dy*dy);
    return (static_cast<unsigned long long>(0xffffffffu-distance) << 32)
        | (0xffffffffu-static_cast<unsigned int>(index));
}

// Reunite separated crosshair arms, without pooling distant colour matches
// or letting a much larger neighbouring blob dominate a small reticle.
CROSS_CLUSTER_HD inline bool centroidSameCluster(
    const CentroidComponent& c, const CentroidComponent& seed, int w, int h) {
    if (!centroidComponentValid(c,w,h)) return false;
    const int cx = seed.sumX/seed.count, cy = seed.sumY/seed.count;
    const int areaLimit = seed.count*4 > 4 ? seed.count*4 : 4;
    const int cw=c.right-c.left+1,ch=c.bottom-c.top+1;
    const int sw=seed.right-seed.left+1,sh=seed.bottom-seed.top+1;
    const bool arm = c.count<4 || cw>=ch*2 || ch>=cw*2;
    const bool seedArm = seed.count<4 || sw>=sh*2 || sh>=sw*2;
    // Two separate compact dots are competing candidates, not crosshair arms.
    if(!arm && !seedArm) return false;
    return c.count <= areaLimit && c.left >= cx-24 && c.right <= cx+24
        && c.top >= cy-24 && c.bottom <= cy+24;
}
}
#undef CROSS_CLUSTER_HD
