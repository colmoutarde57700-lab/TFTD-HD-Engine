#pragma once
#include <algorithm>
#include <vector>
#include <cstddef>
namespace OpenXcom {
struct HdStoreyBand { int top, bottom; };
inline HdStoreyBand hdStoreyBand(int level,int last,int unitZ,int originY,int height,int scale) {
    return {level==last?0:std::clamp(originY-(level+1-unitZ)*24*scale,0,height),
            level==0?height:std::clamp(originY-(level-unitZ)*24*scale,0,height)};
}
inline void hdMarkUnitReplacement(std::vector<bool> &commands,std::size_t begin,std::size_t count) {
    const std::size_t end=begin+std::min(count,commands.size()-begin);
    for(std::size_t i=begin;i<end;++i)commands[i]=true;
}
}
