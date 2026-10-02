#pragma once
#include <mutex>
#include <cstdint>
namespace macros {
// Runtime-only overrides. Zero/default values leave the original controller unchanged.
struct ControlDirective {
    uint64_t revision=0,commandSerial=0;
    int command=0; // 1 lock nearest point, 2 unlock, 3 select nearest point, 4 clear
    double x=0,y=0;
    int classId=-1,priority=0; // 0 configured, 1 nearest, 2 farthest, 3 confidence, 4 largest
    double partX=-1,partY=-1,predictionMs=0,smoothing=1,speed=0;
};
inline std::mutex directiveMutex;
inline ControlDirective directive;
inline ControlDirective readDirective(){std::lock_guard<std::mutex> l(directiveMutex);return directive;}
template<class F> inline void changeDirective(F fn){std::lock_guard<std::mutex> l(directiveMutex);fn(directive);++directive.revision;}
inline void clearDirective(){changeDirective([](ControlDirective& d){auto revision=d.revision;d={};d.revision=revision;d.command=2;d.commandSerial=revision+1;});}
}
