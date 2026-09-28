#pragma once
namespace OpenXcom {
static const char HdUnit3DShader[] = R"HLSL(
cbuffer Unit : register(b0) {
 row_major float4x4 bones[64];
 float4 metrics; // yaw cosine, yaw sine, logical height/metre, unused
};
struct Input { float3 p:POSITION; float3 n:NORMAL; float3 c:COLOR; float4 w:BLENDWEIGHT; uint4 j:BLENDINDICES; };
struct Output { float4 p:SV_POSITION; float3 n:NORMAL; float3 c:COLOR; };
Output VS(Input i) {
 float4 p=0; float3 n=0;
 [unroll] for(uint k=0;k<4;++k) { p+=mul(bones[i.j[k]],float4(i.p,1))*i.w[k]; n+=mul((float3x3)bones[i.j[k]],i.n)*i.w[k]; }
 float c=metrics.x,s=metrics.y;
 p.xy=float2(c*p.x-s*p.y,s*p.x+c*p.y);
 n.xy=float2(c*n.x-s*n.y,s*n.x+c*n.y);
 // Native isometric projection: x-y, (x+y)/2-z. Feet anchor (16,32).
 float2 screen=float2(16+(p.x-p.y)*metrics.z,32+((p.x+p.y)*0.5-p.z)*metrics.z);
 Output o;o.p=float4(screen.x/16-1,1-screen.y/20,saturate(0.5-(p.x+p.y+p.z)*0.08),1);
 o.n=n;o.c=i.c;return o;
}
float4 PS(Output i):SV_TARGET {
 float light=0.48+0.52*saturate(dot(normalize(i.n),normalize(float3(0.4,0.6,1))));
 return float4(pow(saturate(i.c*light),1.0/2.2),1);
}
)HLSL";
}
