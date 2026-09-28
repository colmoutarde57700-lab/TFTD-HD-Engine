#pragma once
#include <array>
#include "RealHdPhysicalGeometry.h"
namespace OpenXcom {
inline std::array<unsigned,16> hdSunExposureRows(const RealHdPhysicalGeometry &geometry,int tileX,int tileY,int tileZ,int levels) {
    std::array<unsigned,16> rows{};
    for(int y=0;y<16;++y)for(int x=0;x<16;++x) {
        const RealHdPoint3 from{tileX*16.0+x+0.5,tileY*16.0+y+0.5,tileZ*24.0+2.01};
        const RealHdPoint3 sky{from.x,from.y,levels*24.0-0.01};
        if(geometry.rayClear(from,sky,BlockLight))rows[y]|=1u<<x;
    }
    return rows;
}
}
