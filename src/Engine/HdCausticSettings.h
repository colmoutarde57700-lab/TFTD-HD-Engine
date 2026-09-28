#pragma once
#include "Options.h"
#include <array>
#include <map>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>

namespace OpenXcom {
// Presentation-only, exact mission depth/shade keys; never camera Z.
struct HdCausticProfile {
 int intensity=1000, weakBoost=1000, presence=-1;
 // contribution, speed, deformation, thickness, scale, angle degrees, drift, light variation
 std::array<std::array<int,8>,5> layers{{
  {{400,1000,1000,1000,1000,0,1000,1000}},
  {{300,1000,1000,1000,1000,0,1000,1000}},
  {{180,1000,1000,1000,1000,0,1000,1000}},
  {{120,1000,1000,1000,1000,0,1000,1000}},
  {{120,800,1200,2000,450,25,1000,1200}} }};
};
class HdCausticSettings {
 std::map<std::pair<int,int>,HdCausticProfile> profiles, savedProfiles;
 int depth=0, shade=0;
 bool loaded=false;
 std::string status="Reference P2ZG (non sauvegardee)";
 std::filesystem::path path() const {return std::filesystem::path(Options::getUserFolder())/"real_hd_caustics_profiles.cfg";}
 void load() {
  if(loaded)return; loaded=true;
  std::ifstream in(path()); if(!in)return;
  std::string line; std::getline(in,line);
  const int layerCount=line=="REAL_HD_CAUSTICS_V1"?4:5;
  if(line!="REAL_HD_CAUSTICS_V1" && line!="REAL_HD_CAUSTICS_V2") {status="Format de profils inconnu";return;}
  while(std::getline(in,line)) {
   std::istringstream row(line); int d,s; HdCausticProfile p;
   if(!(row>>d>>s>>p.intensity>>p.weakBoost>>p.presence))continue;
   bool valid=d>=0 && d<=100 && s>=0 && s<=15;
   for(int i=0;i<layerCount;++i)for(int &v:p.layers[i])if(!(row>>v))valid=false;
   if(!valid)continue;
   p.presence=std::clamp(p.presence,0,2000);p.intensity=std::clamp(p.intensity,0,3000);p.weakBoost=std::clamp(p.weakBoost,0,3000);
   for(auto &l:p.layers)for(int j=0;j<8;++j)l[j]=std::clamp(l[j],j==5?-180:(j==4?250:0),j==5?180:3000);
   profiles[{d,s}]=p;savedProfiles[{d,s}]=p;
  }
 }
public:
 static HdCausticSettings &instance(){static HdCausticSettings v;return v;}
 void context(int d,int s){load();if(d!=depth || s!=shade){depth=d;shade=s;status=profiles.count({d,s})?"Profil charge":"Reference P2ZG";}}
 int missionDepth()const{return depth;} int missionShade()const{return shade;}
 HdCausticProfile &current(){load();auto &p=profiles[{depth,shade}];
  if(p.presence<0)p.presence=(int)((depth==1?1000.0f:(depth==2?650.0f:0.0f))*std::clamp((8.0f-shade)/8.0f,0.0f,1.0f));return p;}
 const std::string &message()const{return status;}
 void changed(){status="Modifie - S pour sauvegarder";}
 void reset(){current()=HdCausticProfile();current();changed();}
 bool save(){
  load();current();auto target=path();auto temp=target;temp+=".tmp";
  try {
   auto toSave=savedProfiles;toSave[{depth,shade}]=current();
   std::ofstream out(temp,std::ios::trunc);out<<"REAL_HD_CAUSTICS_V2\n";
   for(const auto &entry:toSave){out<<entry.first.first<<' '<<entry.first.second<<' '<<entry.second.intensity<<' '<<entry.second.weakBoost<<' '<<entry.second.presence;
    for(const auto &l:entry.second.layers)for(int v:l)out<<' '<<v;out<<'\n';}
   out.flush();if(!out){status="Erreur de sauvegarde";return false;}out.close();
   if(std::filesystem::exists(target)){auto backup=target;backup+=".bak";std::filesystem::copy_file(target,backup,std::filesystem::copy_options::overwrite_existing);}
   std::filesystem::copy_file(temp,target,std::filesystem::copy_options::overwrite_existing);
   std::filesystem::remove(temp);savedProfiles=toSave;status="Profil sauvegarde";return true;
  }catch(...){status="Erreur de sauvegarde - reglages conserves en memoire";return false;}
 }
};
}
