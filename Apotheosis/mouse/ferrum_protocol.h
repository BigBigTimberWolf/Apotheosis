#pragma once
#include <string>
#include <vector>

namespace mouse_driver::ferrum_protocol {
// Software API: one report for a chord, without firmware-randomized press()
// durations. https://docs.xferrum.dev/software_api/km_api/keyboard/keys/multi.html
inline std::vector<int> chordKeys(int hid, int modifiers) {
    if (hid <= 0 || hid >= 256 || modifiers < 0 || modifiers > 255) return {};
    std::vector<int> keys;
    for (int i=0;i<8;++i) if ((modifiers & (1<<i)) && hid!=224+i) keys.push_back(224+i);
    keys.push_back(hid);
    return keys;
}
inline std::string keyCommand(const std::vector<int>& keys, bool down) {
    if (keys.empty()) return {};
    for (int key:keys) if (key<=0 || key>=256) return {};
    std::string result=keys.size()==1 ? (down?"km.down(":"km.up(")
                                      : (down?"km.multidown(":"km.multiup(");
    for (size_t i=0;i<keys.size();++i) {
        if (i) result+=',';
        result+=std::to_string(keys[i]);
    }
    return result+')';
}
}
