#include "crosshair/am_centroid.h"
#include <cstdio>
struct Band {int h_low,h_high,s_min,s_max,v_min,v_max;};
int main() {
    auto hsv=crosshair::amHsv(0,5,255);
    if(hsv.h!=0 || hsv.s!=255 || hsv.v!=255)return 1;
    hsv=crosshair::amHsv(100,100,200);
    if(hsv.s!=127)return 2;
    const Band red{170,10,80,255,80,255};
    if(!crosshair::amHsvMatches(crosshair::amHsv(0,0,255),red))return 3;
    if(crosshair::amHsvMatches(crosshair::amHsv(0,255,0),red))return 4;
    if(crosshair::amCentroidCoordinate(281,2)!=140)return 5;
    if(crosshair::amCentroidCoordinate(140,1)!=140)return 6;
    std::puts("AM scalar HSV and centroid rules passed");
}
