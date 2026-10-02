#include "mouse/ferrum_protocol.h"
#include <cstdio>
using namespace mouse_driver::ferrum_protocol;
int main() {
    int failures=0;
    auto check=[&](bool ok,const char* name) { if(!ok) { ++failures; std::printf("FAIL: %s\n",name); } };
    check(keyCommand({4,5,6},true)=="km.multidown(4,5,6)","official simultaneous ABC example");
    check(keyCommand({30,31,32},false)=="km.multiup(30,31,32)","official simultaneous release example");
    const auto chord=chordKeys(4,3);
    check(keyCommand(chord,true)=="km.multidown(224,225,4)","control shift A uses one report");
    check(keyCommand(chord,false)=="km.multiup(224,225,4)","same chord released in one report");
    check(chordKeys(224,1)==std::vector<int>{224},"modifier used as main key is not duplicated");
    check(keyCommand(chordKeys(4,0),true)=="km.down(4)","single key keeps existing API");
    check(chordKeys(0,1).empty() && chordKeys(256,1).empty() && chordKeys(4,256).empty(),
          "invalid chord cannot partially press modifiers");
    check(keyCommand({},false).empty() && keyCommand({4,-1},true).empty(),"invalid commands are not sent");
    return failures?1:0;
}
