#pragma once
// Aquanaute crash-test format v1. File decoding is independent of D3D11.
#include <array>
#include <vector>
#include <string>
#include <istream>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <stdexcept>

namespace OpenXcom
{
struct HdUnit3DVertex
{
    float position[3], normal[3], color[3], weights[4];
    uint32_t joints[4];
};
static_assert(sizeof(HdUnit3DVertex) == 68, "AQM1 vertex ABI");
struct HdUnit3DClip { uint32_t first, count; float fps; };
struct HdUnit3DPose
{
    bool walking = false, kneeling = false;
    float walkPhase = 0, seconds = 0, transitionSeconds = 999;
    unsigned direction = 0;
    float height = 24; // Logical projected standing height, not mesh metres.
};
struct HdUnit3DAsset
{
    std::vector<HdUnit3DVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<std::array<float, 16>> matrices;
    std::array<HdUnit3DClip,5> clips{};
    uint32_t bones = 0, frames = 0;
    float height = 0;

    static HdUnit3DAsset read(std::istream &in)
    {
        auto readBytes = [&](void *p, size_t n) {
            if (!in.read(static_cast<char*>(p), static_cast<std::streamsize>(n)))
                throw std::runtime_error("truncated AQM1 file");
        };
        char magic[4]; uint32_t h[6]; HdUnit3DAsset a;
        readBytes(magic,4); readBytes(h,sizeof(h)); readBytes(&a.height,4);
        if (std::memcmp(magic,"AQM1",4) || h[0]!=1 || !h[1] || h[1]>100000 ||
            !h[2] || h[2]>600000 || h[2]%3 || !h[3] || h[3]>64 ||
            !h[4] || h[4]>2048 || h[5]!=5 || !std::isfinite(a.height) || a.height<0.1f || a.height>10)
            throw std::runtime_error("invalid or excessive AQM1 header");
        a.bones=h[3]; a.frames=h[4];
        readBytes(a.clips.data(),sizeof(HdUnit3DClip)*5);
        for (auto &c:a.clips)
            if (!c.count || c.first>=a.frames || c.count>a.frames-c.first ||
                !std::isfinite(c.fps) || c.fps<1 || c.fps>120)
                throw std::runtime_error("invalid AQM1 clip range");
        a.vertices.resize(h[1]); a.indices.resize(h[2]); a.matrices.resize(size_t(a.bones)*a.frames);
        readBytes(a.vertices.data(),a.vertices.size()*sizeof(HdUnit3DVertex));
        readBytes(a.indices.data(),a.indices.size()*4);
        readBytes(a.matrices.data(),a.matrices.size()*64);
        for (const auto &v:a.vertices)
        {
            for(float f:v.position) if(!std::isfinite(f)||std::abs(f)>10) throw std::runtime_error("invalid mesh position");
            for(float f:v.normal) if(!std::isfinite(f)||std::abs(f)>1.01f) throw std::runtime_error("invalid mesh normal");
            for(float f:v.color) if(!std::isfinite(f)||f<0||f>1) throw std::runtime_error("invalid mesh colour");
            float total=0;
            for(unsigned j=0;j<4;++j) {
                if(v.joints[j]>=a.bones || !std::isfinite(v.weights[j]) || v.weights[j]<0 || v.weights[j]>1)
                    throw std::runtime_error("invalid mesh skin weights");
                total+=v.weights[j];
            }
            if(std::abs(total-1)>0.001f) throw std::runtime_error("unnormalized mesh weights");
        }
        for(auto i:a.indices) if(i>=a.vertices.size()) throw std::runtime_error("invalid mesh triangle");
        for(const auto &m:a.matrices) {
            for(float f:m) if(!std::isfinite(f)||std::abs(f)>100) throw std::runtime_error("invalid animation matrix");
            if(std::abs(m[12])+std::abs(m[13])+std::abs(m[14])+std::abs(m[15]-1)>0.001f)
                throw std::runtime_error("non-affine animation matrix");
        }
        return a;
    }
    // CPU selects/interpolates only bone matrices; vertices are skinned on the GPU.
    unsigned sample(const HdUnit3DPose &p, float *out) const
    {
        unsigned clip=p.walking?1:(p.kneeling?2:0);
        float t=p.seconds; bool loop=true;
        unsigned transition=p.kneeling?3:4;
        if(!p.walking && p.transitionSeconds>=0 && p.transitionSeconds<(clips[transition].count-1)/clips[transition].fps)
        { clip=transition; t=p.transitionSeconds; loop=false; }
        const auto &c=clips[clip];
        float f=p.walking?std::clamp(p.walkPhase,0.f,1.f)*c.count:std::max(0.f,t)*c.fps;
        f=loop?std::fmod(f,float(c.count)):std::min(f,float(c.count-1));
        unsigned f0=unsigned(f), f1=loop?(f0+1)%c.count:std::min(f0+1,c.count-1);
        float alpha=f-f0;
        for(unsigned b=0;b<bones;++b)
            for(unsigned k=0;k<16;++k)
                out[b*16+k]=matrices[(c.first+f0)*bones+b][k]*(1-alpha)+matrices[(c.first+f1)*bones+b][k]*alpha;
        return clip;
    }
};
}
