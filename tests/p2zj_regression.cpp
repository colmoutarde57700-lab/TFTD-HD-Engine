#include "HdStoreyBands.h"
#include "HdCausticReceiver.h"
#include <iostream>
#include <stdexcept>
using namespace OpenXcom;
int tests=0;
void check(bool b,const char *msg){++tests;if(!b)throw std::runtime_error(msg);}
RealHdPhysicalGeometry makeScene(int mode){
 RealHdPhysicalGeometry g;
 for(int z=0;z<3;++z){RealHdTilePhysics t;t.x=0;t.y=0;t.z=z;
  if(z==2 && mode!=0){
   if(mode==3){t.hasVoxelVolume=true;t.voxelUses=BlockLight;for(int y=0;y<16;++y)t.solidVoxelRows[y]=0xff00;t.sightVoxelRows.assign(t.solidVoxelRows.begin(),t.solidVoxelRows.end());}
   else {unsigned char use=mode==2?BlockSight:BlockLight;
    t.barriers.push_back({{0,0,48},{16,0,48},{16,16,48},RealHdBarrierKind::Ceiling,use});
    t.barriers.push_back({{0,0,48},{16,16,48},{0,16,48},RealHdBarrierKind::Ceiling,use});}
  }
  g.addTile(t);
 }
 check(g.seal(1,1,3),"scene seal");return g;
}
int count(std::array<unsigned,16> rows){int n=0;for(auto r:rows)for(int x=0;x<16;++x)n+=(r>>x)&1;return n;}
int main(){
 for(int last=0;last<6;++last)for(int z=0;z<=last;++z)for(int origin=-100;origin<900;origin+=17)for(int scale:{1,2,4}) {
  for(int y=0;y<600;++y){int hits=0;for(int l=0;l<=last;++l){auto b=hdStoreyBand(l,last,z,origin,600,scale);hits+=y>=b.top&&y<b.bottom;}check(hits==1,"gap or double composition");}
 }
 // Actual stair geometry: base projection Y=100, foot at 116 (T=-16).
 // Upper floor plane is Y=108. Torso belongs to upper level, legs below.
 auto lower=hdStoreyBand(0,1,0,132,600,1),upper=hdStoreyBand(1,1,0,132,600,1);
 check(upper.top<=100 && 100<upper.bottom,"head not promoted");check(lower.top<=116&&116<lower.bottom,"legs promoted above lower walls");
 // A late 3D command rendered first must not suppress unrelated earlier commands.
 std::vector<bool> replaced(12,false);hdMarkUnitReplacement(replaced,8,3);check(!replaced[1]&&!replaced[7]&&replaced[8]&&replaced[10]&&!replaced[11],"3D groups cross-suppressed");hdMarkUnitReplacement(replaced,1,2);check(replaced[1]&&replaced[2]&&!replaced[3],"3D earlier group");
 auto open=makeScene(0),covered=makeScene(1),transmitting=makeScene(2),half=makeScene(3);
 check(count(hdSunExposureRows(open,0,0,0,3))==256,"open floor dark");
 check(count(hdSunExposureRows(covered,0,0,0,3))==0,"nonadjacent roof missed");
 check(count(hdSunExposureRows(covered,0,0,1,3))==0,"interior exposed");
 check(count(hdSunExposureRows(covered,0,0,2,3))==256,"roof self blocks");
 check(count(hdSunExposureRows(transmitting,0,0,0,3))==256,"sight/light channels confused");
 check(count(hdSunExposureRows(half,0,0,0,3))==128,"partial cover not respected");
 std::cout<<tests<<" assertions passed\n";
}
