#pragma once
// D3D11-only implementation, included after the platform headers.
#include "HdUnit3DAsset.h"
#include "HdUnit3DShader.h"
#include <memory>
#include <set>
#include <unordered_map>

namespace OpenXcom {
class HdUnit3DGpu
{
    template<class T> static void release(T *&p) { if(p) {p->Release();p=nullptr;} }
    struct Mesh {
        HdUnit3DAsset asset;
        ID3D11Buffer *vb=nullptr,*ib=nullptr;
        bool failed=false;
        unsigned loggedClips=0;
        ~Mesh(){release(vb);release(ib);}
    };
    std::unordered_map<std::string,std::unique_ptr<Mesh>> meshes;
    std::set<std::string> messages;
    ID3D11VertexShader *vs=nullptr;
    ID3D11PixelShader *ps=nullptr;
    ID3D11InputLayout *layout=nullptr;
    ID3D11Buffer *constants=nullptr;
    ID3D11Texture2D *color=nullptr,*depth=nullptr;
    ID3D11RenderTargetView *rtv=nullptr;
    ID3D11ShaderResourceView *srv=nullptr;
    ID3D11DepthStencilView *dsv=nullptr;
    ID3D11DepthStencilState *depthState=nullptr;
    ID3D11RasterizerState *raster=nullptr;
    // Only a shared D3D pipeline initialization failure disables all models.
    bool pipelineFailed=false;
    void check(HRESULT hr,const char *what) {if(FAILED(hr))throw std::runtime_error(what);}
    void init(ID3D11Device *device)
    {
        if(vs)return;
        ID3DBlob *blob=nullptr,*errors=nullptr;
        HRESULT hr=D3DCompile(HdUnit3DShader,std::strlen(HdUnit3DShader),"Aquanaute3D",nullptr,nullptr,"VS","vs_4_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&errors);
        release(errors);check(hr,"unit vertex shader compile");
        hr=device->CreateVertexShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,&vs);
        if(FAILED(hr)){release(blob);check(hr,"unit vertex shader create");}
        D3D11_INPUT_ELEMENT_DESC elements[]={
            {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"COLOR",0,DXGI_FORMAT_R32G32B32_FLOAT,0,24,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"BLENDWEIGHT",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,36,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"BLENDINDICES",0,DXGI_FORMAT_R32G32B32A32_UINT,0,52,D3D11_INPUT_PER_VERTEX_DATA,0}};
        hr=device->CreateInputLayout(elements,5,blob->GetBufferPointer(),blob->GetBufferSize(),&layout);release(blob);check(hr,"unit input layout");
        hr=D3DCompile(HdUnit3DShader,std::strlen(HdUnit3DShader),"Aquanaute3D",nullptr,nullptr,"PS","ps_4_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&errors);
        release(errors);check(hr,"unit pixel shader compile");
        hr=device->CreatePixelShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,&ps);release(blob);check(hr,"unit pixel shader create");
        D3D11_BUFFER_DESC cb={};cb.ByteWidth=4112;cb.Usage=D3D11_USAGE_DYNAMIC;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;cb.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        check(device->CreateBuffer(&cb,nullptr,&constants),"unit constants");
        D3D11_TEXTURE2D_DESC td={};td.Width=512;td.Height=640;td.MipLevels=1;td.ArraySize=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        check(device->CreateTexture2D(&td,nullptr,&color),"unit render texture");
        check(device->CreateRenderTargetView(color,nullptr,&rtv),"unit render view");
        check(device->CreateShaderResourceView(color,nullptr,&srv),"unit shader view");
        td.Format=DXGI_FORMAT_D32_FLOAT;td.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        check(device->CreateTexture2D(&td,nullptr,&depth),"unit depth texture");check(device->CreateDepthStencilView(depth,nullptr,&dsv),"unit depth view");
        D3D11_DEPTH_STENCIL_DESC ds={};ds.DepthEnable=TRUE;ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;ds.DepthFunc=D3D11_COMPARISON_LESS_EQUAL;
        check(device->CreateDepthStencilState(&ds,&depthState),"unit depth state");
        D3D11_RASTERIZER_DESC rs={};rs.FillMode=D3D11_FILL_SOLID;rs.CullMode=D3D11_CULL_NONE;rs.DepthClipEnable=TRUE;
        check(device->CreateRasterizerState(&rs,&raster),"unit raster state");
    }
public:
    ~HdUnit3DGpu(){reset();}
    void reset() {
        meshes.clear();messages.clear();release(vs);release(ps);release(layout);release(constants);release(rtv);release(srv);release(color);release(dsv);release(depth);release(depthState);release(raster);pipelineFailed=false;
    }
    ID3D11ShaderResourceView *render(ID3D11Device *device,ID3D11DeviceContext *ctx,const std::string &key,const HdUnit3DPose &pose)
    {
        if(pipelineFailed)return nullptr;
        Mesh *mesh=nullptr;
        try {
            // Initialize shared GPU state before associating an error with an
            // individual asset. A bad asset must not disable other models.
            init(device);
            auto it=meshes.find(key);
            if(it==meshes.end()) {
                auto ptr=std::make_unique<Mesh>();mesh=ptr.get();meshes.emplace(key,std::move(ptr));
                if(!FileMap::fileExists(key))throw std::runtime_error("model file missing");
                auto in=FileMap::getIStream(key);if(!in || !*in)throw std::runtime_error("model file unreadable");
                mesh->asset=HdUnit3DAsset::read(*in);
                D3D11_BUFFER_DESC bd={};bd.Usage=D3D11_USAGE_IMMUTABLE;bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;bd.ByteWidth=UINT(mesh->asset.vertices.size()*sizeof(HdUnit3DVertex));
                D3D11_SUBRESOURCE_DATA data={};data.pSysMem=mesh->asset.vertices.data();check(device->CreateBuffer(&bd,&data,&mesh->vb),"unit vertex buffer");
                bd.BindFlags=D3D11_BIND_INDEX_BUFFER;bd.ByteWidth=UINT(mesh->asset.indices.size()*4);data.pSysMem=mesh->asset.indices.data();check(device->CreateBuffer(&bd,&data,&mesh->ib),"unit index buffer");
            } else mesh=it->second.get();
            if(mesh->failed)return nullptr;
            alignas(16) float cb[1028]={};
            unsigned clip=mesh->asset.sample(pose,cb);
            // Direction 0 = north (-Y), 2 = east (+X).
            float yaw=float(pose.direction%8)*0.7853981633974483f;
            cb[1024]=std::cos(yaw);cb[1025]=std::sin(yaw);cb[1026]=std::clamp(pose.height,8.f,32.f)/mesh->asset.height;
            D3D11_MAPPED_SUBRESOURCE mapped={};check(ctx->Map(constants,0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"unit pose upload");std::memcpy(mapped.pData,cb,sizeof(cb));ctx->Unmap(constants,0);
            // No readback, CPU raster or per-frame texture/buffer allocation.
            const float clear[4]={0,0,0,0};ctx->ClearRenderTargetView(rtv,clear);ctx->ClearDepthStencilView(dsv,D3D11_CLEAR_DEPTH,1,0);
            ctx->OMSetRenderTargets(1,&rtv,dsv);ctx->OMSetDepthStencilState(depthState,0);ctx->OMSetBlendState(nullptr,nullptr,0xffffffff);
            D3D11_VIEWPORT vp={0,0,512,640,0,1};ctx->RSSetViewports(1,&vp);ctx->RSSetState(raster);
            UINT stride=sizeof(HdUnit3DVertex),offset=0;ctx->IASetVertexBuffers(0,1,&mesh->vb,&stride,&offset);ctx->IASetIndexBuffer(mesh->ib,DXGI_FORMAT_R32_UINT,0);ctx->IASetInputLayout(layout);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(vs,nullptr,0);ctx->VSSetConstantBuffers(0,1,&constants);ctx->PSSetShader(ps,nullptr,0);ctx->DrawIndexed(UINT(mesh->asset.indices.size()),0,0);
            // Unbind before the compositor samples the render target.
            ctx->OMSetRenderTargets(0,nullptr,nullptr);
            if(!(mesh->loggedClips&(1u<<clip))) {
                mesh->loggedClips|=1u<<clip;
                Log(LOG_INFO)<<"[AQUANAUTE3D][ACTIVE] asset="<<key<<" clip="<<clip<<" provider=REAL_HD skinning=GPU weapons=MAP_SPRITE_LAYERS";
            }
            return srv;
        } catch(const std::exception &e) {
            if(mesh)mesh->failed=true;
            if(!mesh)pipelineFailed=true;
            if(messages.insert(key).second) {
                Log(LOG_WARNING)<<"[AQUANAUTE3D][ERROR] asset="<<key<<" reason="<<e.what()<<" scope="<<(mesh?"MODEL_OR_DRAW":"SHARED_PIPELINE")<<" retry=DEVICE_RESET_OR_RESTART";
            }
            return nullptr;
        }
    }
};
}
