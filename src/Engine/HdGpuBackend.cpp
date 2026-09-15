#include "HdGpuBackend.h"

#include "HdRenderSpace.h"
#include "Logger.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <SDL_syswm.h>
#endif

namespace OpenXcom
{

struct HdGpuBackend::Impl
{
	bool frameActive = false;
	bool lastUsed = false;
	unsigned mapDrawCalls = 0;
	unsigned indexedDrawCalls = 0;
	unsigned environmentDrawCalls = 0;
	unsigned lastMapDrawCalls = 0;
	unsigned lastIndexedDrawCalls = 0;
	unsigned lastEnvironmentDrawCalls = 0;
	SDL_Surface *overlaySurface = nullptr;
	std::vector<Uint8> overlayPixels;
	int overlayW = 0;
	int overlayH = 0;

#if defined(_WIN32)
	ID3D11Device *device = nullptr;
	ID3D11DeviceContext *context = nullptr;
	IDXGISwapChain *swap = nullptr;
	ID3D11RenderTargetView *rtv = nullptr;
	HWND hwnd = nullptr;
	int width = 0;
	int height = 0;

	ID3D11Texture2D *baseUpload = nullptr;
	ID3D11ShaderResourceView *baseSrv = nullptr;
	int baseW = 0, baseH = 0;
	ID3D11Texture2D *overlayUpload = nullptr;
	ID3D11ShaderResourceView *overlaySrv = nullptr;
	int overlayTexW = 0, overlayTexH = 0;

	ID3D11VertexShader *fullscreenVs = nullptr;
	ID3D11PixelShader *fullscreenPs = nullptr;
	ID3D11VertexShader *spriteVs = nullptr;
	ID3D11PixelShader *spritePs = nullptr;
	ID3D11PixelShader *indexedSpritePs = nullptr;
	ID3D11PixelShader *environmentSpritePs = nullptr;
	ID3D11SamplerState *pointSampler = nullptr;
	ID3D11SamplerState *linearSampler = nullptr;
	ID3D11BlendState *alphaBlend = nullptr;
	ID3D11RasterizerState *rasterNormal = nullptr;
	ID3D11RasterizerState *rasterScissor = nullptr;
	ID3D11Buffer *spriteConstants = nullptr;

	ID3D11Texture2D *orderTexture = nullptr;
	ID3D11ShaderResourceView *orderSrv = nullptr;
	int orderW = 0, orderH = 0;
	ID3D11Texture2D *paletteTexture = nullptr;
	ID3D11ShaderResourceView *paletteSrv = nullptr;
	std::array<ID3D11Texture3D*, HdEnvironmentProfileCount> environmentTextures{};
	std::array<ID3D11ShaderResourceView*, HdEnvironmentProfileCount> environmentSrvs{};
	std::array<HdEnvironmentTransform, HdEnvironmentProfileCount> environmentLuts;
	std::array<uint64_t, HdEnvironmentProfileCount> uploadedEnvironmentSignatures{};

	struct GpuImage
	{
		ID3D11Texture2D *texture = nullptr;
		ID3D11ShaderResourceView *srv = nullptr;
		unsigned width = 0;
		unsigned height = 0;
		bool indexed = false;
	};
	std::unordered_map<std::string, GpuImage> images;

	static void releaseTexture(ID3D11Texture2D *&texture, ID3D11ShaderResourceView *&srv)
	{
		if (srv) { srv->Release(); srv = nullptr; }
		if (texture) { texture->Release(); texture = nullptr; }
	}

	void shutdownDevice()
	{
		for (auto &pair : images)
		{
			if (pair.second.srv) pair.second.srv->Release();
			if (pair.second.texture) pair.second.texture->Release();
		}
		images.clear();
		for (size_t i = 0; i < HdEnvironmentProfileCount; ++i)
		{
			if (environmentSrvs[i]) { environmentSrvs[i]->Release(); environmentSrvs[i] = nullptr; }
			if (environmentTextures[i]) { environmentTextures[i]->Release(); environmentTextures[i] = nullptr; }
			uploadedEnvironmentSignatures[i] = 0;
		}
		releaseTexture(paletteTexture, paletteSrv);
		releaseTexture(orderTexture, orderSrv);
		releaseTexture(overlayUpload, overlaySrv);
		releaseTexture(baseUpload, baseSrv);
		if (spriteConstants) { spriteConstants->Release(); spriteConstants = nullptr; }
		if (rasterScissor) { rasterScissor->Release(); rasterScissor = nullptr; }
		if (rasterNormal) { rasterNormal->Release(); rasterNormal = nullptr; }
		if (alphaBlend) { alphaBlend->Release(); alphaBlend = nullptr; }
		if (linearSampler) { linearSampler->Release(); linearSampler = nullptr; }
		if (pointSampler) { pointSampler->Release(); pointSampler = nullptr; }
		if (environmentSpritePs) { environmentSpritePs->Release(); environmentSpritePs = nullptr; }
		if (indexedSpritePs) { indexedSpritePs->Release(); indexedSpritePs = nullptr; }
		if (spritePs) { spritePs->Release(); spritePs = nullptr; }
		if (spriteVs) { spriteVs->Release(); spriteVs = nullptr; }
		if (fullscreenPs) { fullscreenPs->Release(); fullscreenPs = nullptr; }
		if (fullscreenVs) { fullscreenVs->Release(); fullscreenVs = nullptr; }
		if (rtv) { rtv->Release(); rtv = nullptr; }
		if (swap) { swap->Release(); swap = nullptr; }
		if (context) { context->Release(); context = nullptr; }
		if (device) { device->Release(); device = nullptr; }
		hwnd = nullptr;
		width = height = 0;
		baseW = baseH = overlayTexW = overlayTexH = orderW = orderH = 0;
	}

	bool compileShader(const char *source, const char *entry, const char *target, ID3DBlob **blob)
	{
		ID3DBlob *errors = nullptr;
		HRESULT hr = D3DCompile(source, std::strlen(source), "RC12_P8_D3D11", nullptr, nullptr,
			entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob, &errors);
		if (errors)
		{
			if (FAILED(hr))
			{
				Log(LOG_ERROR) << "[RC12 P10 GPU] shader compile: " << (const char*)errors->GetBufferPointer();
			}
			errors->Release();
		}
		return SUCCEEDED(hr) && *blob;
	}

	bool ensureDevice(SDL_Surface *base)
	{
		if (!base || base->format->BitsPerPixel != 32 || base->w <= 0 || base->h <= 0) return false;

		SDL_SysWMinfo info;
		SDL_VERSION(&info.version);
		if (!SDL_GetWMInfo(&info) || !info.window) return false;
		HWND newHwnd = info.window;
		if (device && swap && rtv && hwnd == newHwnd && width == base->w && height == base->h) return true;

		shutdownDevice();

		DXGI_SWAP_CHAIN_DESC sd = {};
		sd.BufferDesc.Width = (UINT)base->w;
		sd.BufferDesc.Height = (UINT)base->h;
		sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		sd.SampleDesc.Count = 1;
		sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.BufferCount = 2;
		sd.OutputWindow = newHwnd;
		sd.Windowed = TRUE; // SDL remains authoritative for window/fullscreen policy.
		sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

		D3D_FEATURE_LEVEL levels[] = {
			D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
			D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
		};
		D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_10_0;
		HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
			D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, (UINT)(sizeof(levels)/sizeof(levels[0])),
			D3D11_SDK_VERSION, &sd, &swap, &device, &obtained, &context);
		if (FAILED(hr))
		{
			D3D_FEATURE_LEVEL fallback[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
			hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
				D3D11_CREATE_DEVICE_BGRA_SUPPORT, fallback, (UINT)(sizeof(fallback)/sizeof(fallback[0])),
				D3D11_SDK_VERSION, &sd, &swap, &device, &obtained, &context);
		}
		if (FAILED(hr) || !device || !context || !swap)
		{
			shutdownDevice();
			return false;
		}

		ID3D11Texture2D *back = nullptr;
		hr = swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back));
		if (FAILED(hr) || !back) { shutdownDevice(); return false; }
		hr = device->CreateRenderTargetView(back, nullptr, &rtv);
		back->Release();
		if (FAILED(hr) || !rtv) { shutdownDevice(); return false; }

		static const char fullscreenShader[] =
			"struct VOut{float4 pos:SV_POSITION;float2 uv:TEXCOORD0;};"
			"VOut VS(uint id:SV_VertexID){float2 uv=float2((id<<1)&2,id&2);"
			"VOut o;o.pos=float4(uv*float2(2,-2)+float2(-1,1),0,1);o.uv=uv;return o;}"
			"Texture2D tex0:register(t0);SamplerState samp0:register(s0);"
			"float4 PS(VOut i):SV_Target{return tex0.Sample(samp0,i.uv);}";

		// Sprite shader reproduces P4's CPU compositor semantics: nearest source
		// sampling, per-command shade, right-half clipping, semantic clip mask and
		// the OXCE per-logical-pixel draw-order occlusion test.
		static const char spriteShader[] =
			"cbuffer SpriteCB:register(b0){"
			" float4 destPx; float4 metrics0; float4 metrics1; uint4 ids0; int4 ids1; int4 ids2;};"
			"struct VOut{float4 pos:SV_POSITION;};"
			"VOut VS(uint id:SV_VertexID){float2 c=float2((id==1||id==3)?1:0,(id>=2)?1:0);"
			" float2 p=destPx.xy+c*destPx.zw; VOut o;"
			" o.pos=float4(p.x/metrics0.x*2-1,1-p.y/metrics0.y*2,0,1);return o;}"
			"Texture2D<float4> rgbaTex:register(t0);Texture2D<uint> orderTex:register(t1);"
			"Texture2D<uint> indexTex:register(t2);Texture2D<float4> paletteTex:register(t3);"
			"Texture3D<float4> envTex:register(t4);SamplerState envSamp:register(s1);"
			"float4 PS_RGBA(VOut i):SV_Target{"
			" int2 p=int2(floor(i.pos.xy)); int2 local=p-int2(destPx.xy);"
			" int dw=max(1,(int)destPx.z), dh=max(1,(int)destPx.w);"
			" if(local.x<0||local.y<0||local.x>=dw||local.y>=dh) discard;"
			" uint drawOrder=ids0.x, srcW=ids0.y, srcH=ids0.z, mapW=ids0.w; int mapH=ids1.x;"
			" int4 clipMask=int4(ids1.y,ids1.z,ids1.w,ids2.x); uint flags=(uint)ids2.y;"
			" if((flags&2)!=0 && local.x<dw/2) discard;"
			" float rx=metrics0.z+(float)local.x/max(metrics1.x,0.0001);"
			" float ry=metrics0.w+(float)local.y/max(metrics1.y,0.0001);"
			" int mx=(int)floor(rx/__HD_SCALE__.0); int my=(int)floor(ry/__HD_SCALE__.0);"
			" if(mx<0||my<0||mx>=(int)mapW||my>=mapH) discard;"
			" if((flags&1)!=0 && (mx<clipMask.x||my<clipMask.y||mx>=clipMask.z||my>=clipMask.w)) discard;"
			" if(orderTex.Load(int3(mx,my,0))>drawOrder) discard;"
			" uint ix=min(srcW-1,(uint)(((uint)local.x*srcW)/(uint)dw));"
			" uint iy=min(srcH-1,(uint)(((uint)local.y*srcH)/(uint)dh));"
			" float4 c=rgbaTex.Load(int3(ix,iy,0)); if(c.a<=0) discard; c.rgb*=pow(metrics1.z,2.2); return c;}"
			"float4 PS_ENV(VOut i):SV_Target{"
			" int2 p=int2(floor(i.pos.xy)); int2 local=p-int2(destPx.xy);"
			" int dw=max(1,(int)destPx.z), dh=max(1,(int)destPx.w);"
			" if(local.x<0||local.y<0||local.x>=dw||local.y>=dh) discard;"
			" uint drawOrder=ids0.x, srcW=ids0.y, srcH=ids0.z, mapW=ids0.w; int mapH=ids1.x;"
			" int4 clipMask=int4(ids1.y,ids1.z,ids1.w,ids2.x); uint flags=(uint)ids2.y;"
			" if((flags&2)!=0 && local.x<dw/2) discard;"
			" float rx=metrics0.z+(float)local.x/max(metrics1.x,0.0001);"
			" float ry=metrics0.w+(float)local.y/max(metrics1.y,0.0001);"
			" int mx=(int)floor(rx/__HD_SCALE__.0); int my=(int)floor(ry/__HD_SCALE__.0);"
			" if(mx<0||my<0||mx>=(int)mapW||my>=mapH) discard;"
			" if((flags&1)!=0 && (mx<clipMask.x||my<clipMask.y||mx>=clipMask.z||my>=clipMask.w)) discard;"
			" if(orderTex.Load(int3(mx,my,0))>drawOrder) discard;"
			" uint ix=min(srcW-1,(uint)(((uint)local.x*srcW)/(uint)dw));"
			" uint iy=min(srcH-1,(uint)(((uint)local.y*srcH)/(uint)dh));"
			" float4 c=rgbaTex.Load(int3(ix,iy,0)); if(c.a<=0) discard;"
			" c.rgb=envTex.SampleLevel(envSamp,c.rgb,0).rgb; c.rgb*=pow(metrics1.z,2.2); return c;}"
			"float4 PS_INDEXED(VOut i):SV_Target{"
			" int2 p=int2(floor(i.pos.xy)); int2 local=p-int2(destPx.xy);"
			" int dw=max(1,(int)destPx.z), dh=max(1,(int)destPx.w);"
			" if(local.x<0||local.y<0||local.x>=dw||local.y>=dh) discard;"
			" uint drawOrder=ids0.x, srcW=ids0.y, srcH=ids0.z, mapW=ids0.w; int mapH=ids1.x;"
			" int4 clipMask=int4(ids1.y,ids1.z,ids1.w,ids2.x); uint flags=(uint)ids2.y;"
			" if((flags&2)!=0 && local.x<dw/2) discard;"
			" float rx=metrics0.z+(float)local.x/max(metrics1.x,0.0001);"
			" float ry=metrics0.w+(float)local.y/max(metrics1.y,0.0001);"
			" int mx=(int)floor(rx/__HD_SCALE__.0); int my=(int)floor(ry/__HD_SCALE__.0);"
			" if(mx<0||my<0||mx>=(int)mapW||my>=mapH) discard;"
			" if((flags&1)!=0 && (mx<clipMask.x||my<clipMask.y||mx>=clipMask.z||my>=clipMask.w)) discard;"
			" if(orderTex.Load(int3(mx,my,0))>drawOrder) discard;"
			" uint ix=min(srcW-1,(uint)(((uint)local.x*srcW)/(uint)dw));"
			" uint iy=min(srcH-1,(uint)(((uint)local.y*srcH)/(uint)dh));"
			" uint srcIndex=indexTex.Load(int3(ix,iy,0)); if(srcIndex==0) discard;"
			" uint shade=(uint)max(ids2.z,0); uint shaded=(srcIndex+shade)&255u;"
			" uint finalIndex=(((shaded^srcIndex)&0xF0u)!=0)?15u:shaded;"
			" float4 c=paletteTex.Load(int3((int)finalIndex,0,0)); c.a=1.0; return c;}";

		// The HLSL string needs the compile-time render scale without runtime
		// branches. Build the only generated fragment here.
		std::string spriteSource(spriteShader);
		const std::string marker = "__HD_SCALE__";
		size_t markerPos;
		while ((markerPos = spriteSource.find(marker)) != std::string::npos)
			spriteSource.replace(markerPos, marker.size(), std::to_string(HdRenderSpace::Scale));

		ID3DBlob *blob = nullptr;
		if (!compileShader(fullscreenShader, "VS", "vs_4_0", &blob)) { shutdownDevice(); return false; }
		hr = device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &fullscreenVs);
		blob->Release(); blob = nullptr;
		if (FAILED(hr) || !fullscreenVs) { shutdownDevice(); return false; }
		if (!compileShader(fullscreenShader, "PS", "ps_4_0", &blob)) { shutdownDevice(); return false; }
		hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &fullscreenPs);
		blob->Release(); blob = nullptr;
		if (FAILED(hr) || !fullscreenPs) { shutdownDevice(); return false; }
		if (!compileShader(spriteSource.c_str(), "VS", "vs_4_0", &blob)) { shutdownDevice(); return false; }
		hr = device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &spriteVs);
		blob->Release(); blob = nullptr;
		if (FAILED(hr) || !spriteVs) { shutdownDevice(); return false; }
		if (!compileShader(spriteSource.c_str(), "PS_RGBA", "ps_4_0", &blob)) { shutdownDevice(); return false; }
		hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &spritePs);
		blob->Release(); blob = nullptr;
		if (FAILED(hr) || !spritePs) { shutdownDevice(); return false; }
		if (!compileShader(spriteSource.c_str(), "PS_ENV", "ps_4_0", &blob)) { shutdownDevice(); return false; }
		hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &environmentSpritePs);
		blob->Release(); blob = nullptr;
		if (FAILED(hr) || !environmentSpritePs) { shutdownDevice(); return false; }
		if (!compileShader(spriteSource.c_str(), "PS_INDEXED", "ps_4_0", &blob)) { shutdownDevice(); return false; }
		hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &indexedSpritePs);
		blob->Release(); blob = nullptr;
		if (FAILED(hr) || !indexedSpritePs) { shutdownDevice(); return false; }

		D3D11_SAMPLER_DESC sdPoint = {};
		sdPoint.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		sdPoint.AddressU = sdPoint.AddressV = sdPoint.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sdPoint.MaxLOD = D3D11_FLOAT32_MAX;
		hr = device->CreateSamplerState(&sdPoint, &pointSampler);
		if (FAILED(hr) || !pointSampler) { shutdownDevice(); return false; }
		D3D11_SAMPLER_DESC sdLinear = sdPoint;
		sdLinear.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		hr = device->CreateSamplerState(&sdLinear, &linearSampler);
		if (FAILED(hr) || !linearSampler) { shutdownDevice(); return false; }

		D3D11_BLEND_DESC bd = {};
		bd.RenderTarget[0].BlendEnable = TRUE;
		bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
		bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
		bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
		bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		hr = device->CreateBlendState(&bd, &alphaBlend);
		if (FAILED(hr) || !alphaBlend) { shutdownDevice(); return false; }

		D3D11_RASTERIZER_DESC rd = {};
		rd.FillMode = D3D11_FILL_SOLID;
		rd.CullMode = D3D11_CULL_NONE;
		rd.DepthClipEnable = TRUE;
		hr = device->CreateRasterizerState(&rd, &rasterNormal);
		if (FAILED(hr) || !rasterNormal) { shutdownDevice(); return false; }
		rd.ScissorEnable = TRUE;
		hr = device->CreateRasterizerState(&rd, &rasterScissor);
		if (FAILED(hr) || !rasterScissor) { shutdownDevice(); return false; }

		D3D11_BUFFER_DESC cb = {};
		cb.ByteWidth = 96; // 16-byte aligned; matches SpriteConstants below.
		cb.Usage = D3D11_USAGE_DYNAMIC;
		cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		hr = device->CreateBuffer(&cb, nullptr, &spriteConstants);
		if (FAILED(hr) || !spriteConstants) { shutdownDevice(); return false; }

		hwnd = newHwnd;
		width = base->w;
		height = base->h;
		Log(LOG_INFO) << "[RC12 P10 GPU] D3D11 indexed/native-environment/fixed compositor ready " << width << "x" << height
			<< " RLOG=x" << HdRenderSpace::Scale << " feature=0x" << std::hex << (unsigned)obtained << std::dec;
		return true;
	}

	bool ensureDynamicBgra(ID3D11Texture2D *&tex, ID3D11ShaderResourceView *&srv,
		int &tw, int &th, int w, int h)
	{
		if (tex && srv && tw == w && th == h) return true;
		releaseTexture(tex, srv);
		tw = th = 0;
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1;
		td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DYNAMIC; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		HRESULT hr = device->CreateTexture2D(&td, nullptr, &tex);
		if (FAILED(hr) || !tex) return false;
		hr = device->CreateShaderResourceView(tex, nullptr, &srv);
		if (FAILED(hr) || !srv) { releaseTexture(tex, srv); return false; }
		tw = w; th = h;
		return true;
	}

	bool uploadBgra(ID3D11Texture2D *texture, SDL_Surface *surface)
	{
		if (!texture || !surface || surface->format->BytesPerPixel != 4) return false;
		D3D11_MAPPED_SUBRESOURCE mapped = {};
		HRESULT hr = context->Map(texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
		if (FAILED(hr)) return false;
		const Uint8 *src = static_cast<const Uint8*>(surface->pixels);
		Uint8 *dst = static_cast<Uint8*>(mapped.pData);
		const size_t bytes = (size_t)surface->w * 4u;
		for (int y = 0; y < surface->h; ++y)
			std::memcpy(dst + (size_t)y * mapped.RowPitch, src + (size_t)y * surface->pitch, bytes);
		context->Unmap(texture, 0);
		return true;
	}

	void setFullViewport()
	{
		D3D11_VIEWPORT vp = {};
		vp.TopLeftX = 0; vp.TopLeftY = 0; vp.Width = (FLOAT)width; vp.Height = (FLOAT)height;
		vp.MinDepth = 0; vp.MaxDepth = 1;
		context->RSSetViewports(1, &vp);
	}

	void drawFullscreen(ID3D11ShaderResourceView *srv, bool alpha)
	{
		static const float blendFactor[4] = {0,0,0,0};
		setFullViewport();
		context->RSSetState(rasterNormal);
		context->OMSetRenderTargets(1, &rtv, nullptr);
		context->OMSetBlendState(alpha ? alphaBlend : nullptr, blendFactor, 0xffffffffu);
		context->IASetInputLayout(nullptr);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->VSSetShader(fullscreenVs, nullptr, 0);
		context->PSSetShader(fullscreenPs, nullptr, 0);
		context->PSSetSamplers(0, 1, &pointSampler);
		context->PSSetShaderResources(0, 1, &srv);
		context->Draw(3, 0);
		ID3D11ShaderResourceView *nullSrv = nullptr;
		context->PSSetShaderResources(0, 1, &nullSrv);
	}

	bool ensureOrderTexture(int w, int h)
	{
		if (orderTexture && orderSrv && orderW == w && orderH == h) return true;
		releaseTexture(orderTexture, orderSrv);
		orderW = orderH = 0;
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R32_UINT; td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DYNAMIC; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		HRESULT hr = device->CreateTexture2D(&td, nullptr, &orderTexture);
		if (FAILED(hr) || !orderTexture) return false;
		D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
		srvd.Format = td.Format; srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvd.Texture2D.MipLevels = 1;
		hr = device->CreateShaderResourceView(orderTexture, &srvd, &orderSrv);
		if (FAILED(hr) || !orderSrv) { releaseTexture(orderTexture, orderSrv); return false; }
		orderW = w; orderH = h;
		return true;
	}

	bool ensurePaletteTexture(const SDL_Color *palette)
	{
		if (!palette) return false;
		if (!paletteTexture || !paletteSrv)
		{
			releaseTexture(paletteTexture, paletteSrv);
			D3D11_TEXTURE2D_DESC td = {};
			td.Width = 256; td.Height = 1; td.MipLevels = 1; td.ArraySize = 1;
			td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_DYNAMIC; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			HRESULT hr = device->CreateTexture2D(&td, nullptr, &paletteTexture);
			if (FAILED(hr) || !paletteTexture) return false;
			hr = device->CreateShaderResourceView(paletteTexture, nullptr, &paletteSrv);
			if (FAILED(hr) || !paletteSrv) { releaseTexture(paletteTexture, paletteSrv); return false; }
		}

		D3D11_MAPPED_SUBRESOURCE mapped = {};
		HRESULT hr = context->Map(paletteTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
		if (FAILED(hr)) return false;
		Uint8 *dst = static_cast<Uint8*>(mapped.pData);
		for (int i = 0; i < 256; ++i)
		{
			dst[i * 4 + 0] = palette[i].r;
			dst[i * 4 + 1] = palette[i].g;
			dst[i * 4 + 2] = palette[i].b;
			dst[i * 4 + 3] = i == 0 ? 0 : 255;
		}
		context->Unmap(paletteTexture, 0);
		return true;
	}

	bool ensureEnvironmentTextures(const SDL_Color *neutralPalette, const SDL_Color *activePalette)
	{
		if (!neutralPalette || !activePalette) return false;
		for (size_t i = 0; i < HdEnvironmentProfileCount; ++i)
		{
			const HdEnvironmentProfile profile = (HdEnvironmentProfile)i;
			HdEnvironmentTransform &lut = environmentLuts[i];
			lut.build(neutralPalette, activePalette, hdEnvironmentProfileBlockMask(profile));
			if (environmentSrvs[i] && environmentTextures[i] && uploadedEnvironmentSignatures[i] == lut.signature())
				continue;

			if (environmentSrvs[i]) { environmentSrvs[i]->Release(); environmentSrvs[i] = nullptr; }
			if (environmentTextures[i]) { environmentTextures[i]->Release(); environmentTextures[i] = nullptr; }
			const std::vector<Uint8> &pixels = lut.rgba();
			if (pixels.empty()) return false;

			D3D11_TEXTURE3D_DESC td = {};
			td.Width = HdEnvironmentTransform::Size; td.Height = HdEnvironmentTransform::Size; td.Depth = HdEnvironmentTransform::Size;
			td.MipLevels = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			D3D11_SUBRESOURCE_DATA init = {};
			init.pSysMem = pixels.data();
			init.SysMemPitch = HdEnvironmentTransform::Size * 4u;
			init.SysMemSlicePitch = HdEnvironmentTransform::Size * HdEnvironmentTransform::Size * 4u;
			HRESULT hr = device->CreateTexture3D(&td, &init, &environmentTextures[i]);
			if (FAILED(hr) || !environmentTextures[i]) return false;
			D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
			sd.Format = td.Format; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
			sd.Texture3D.MipLevels = 1;
			hr = device->CreateShaderResourceView(environmentTextures[i], &sd, &environmentSrvs[i]);
			if (FAILED(hr) || !environmentSrvs[i])
			{
				if (environmentSrvs[i]) { environmentSrvs[i]->Release(); environmentSrvs[i] = nullptr; }
				if (environmentTextures[i]) { environmentTextures[i]->Release(); environmentTextures[i] = nullptr; }
				return false;
			}
			uploadedEnvironmentSignatures[i] = lut.signature();
		}
		return true;
	}

	bool ensureImage(const HdGpuMapSprite &sprite, ID3D11ShaderResourceView **outSrv)
	{
		if (!sprite.assetKey || !sprite.imageWidth || !sprite.imageHeight) return false;
		if (sprite.legacyIndexed && !sprite.indices) return false;
		if (!sprite.legacyIndexed && !sprite.rgba) return false;

		std::string cacheKey(sprite.assetKey);
		cacheKey += sprite.legacyIndexed ? "#IDX" : "#RGBA";
		auto it = images.find(cacheKey);
		if (it != images.end() && it->second.width == sprite.imageWidth &&
			it->second.height == sprite.imageHeight && it->second.indexed == sprite.legacyIndexed)
		{
			*outSrv = it->second.srv;
			return *outSrv != nullptr;
		}
		if (it != images.end())
		{
			if (it->second.srv) it->second.srv->Release();
			if (it->second.texture) it->second.texture->Release();
			images.erase(it);
		}

		GpuImage image;
		image.width = sprite.imageWidth; image.height = sprite.imageHeight;
		image.indexed = sprite.legacyIndexed;
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = sprite.imageWidth; td.Height = sprite.imageHeight; td.MipLevels = 1; td.ArraySize = 1;
		td.Format = sprite.legacyIndexed ? DXGI_FORMAT_R8_UINT : DXGI_FORMAT_R8G8B8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		D3D11_SUBRESOURCE_DATA init = {};
		init.pSysMem = sprite.legacyIndexed ? static_cast<const void*>(sprite.indices) : static_cast<const void*>(sprite.rgba);
		init.SysMemPitch = sprite.imageWidth * (sprite.legacyIndexed ? 1u : 4u);
		HRESULT hr = device->CreateTexture2D(&td, &init, &image.texture);
		if (FAILED(hr) || !image.texture) return false;
		D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
		srvd.Format = td.Format; srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvd.Texture2D.MipLevels = 1;
		hr = device->CreateShaderResourceView(image.texture, &srvd, &image.srv);
		if (FAILED(hr) || !image.srv)
		{
			if (image.texture) image.texture->Release();
			return false;
		}
		auto inserted = images.emplace(cacheKey, image);
		*outSrv = inserted.first->second.srv;
		return true;
	}

#endif
};

HdGpuBackend &HdGpuBackend::instance()
{
	static HdGpuBackend backend;
	return backend;
}

HdGpuBackend::HdGpuBackend() : _impl(new Impl) {}

HdGpuBackend::~HdGpuBackend()
{
#if defined(_WIN32)
	_impl->shutdownDevice();
#endif
	if (_impl->overlaySurface) SDL_FreeSurface(_impl->overlaySurface);
	delete _impl;
}

bool HdGpuBackend::beginFrame(SDL_Surface *physicalBase)
{
	_impl->frameActive = false;
	_impl->lastUsed = false;
	_impl->mapDrawCalls = 0;
	_impl->indexedDrawCalls = 0;
	_impl->environmentDrawCalls = 0;
#if defined(_WIN32)
	if (!_impl->ensureDevice(physicalBase)) return false;
	if (!_impl->ensureDynamicBgra(_impl->baseUpload, _impl->baseSrv, _impl->baseW, _impl->baseH,
		physicalBase->w, physicalBase->h)) return false;
	if (!_impl->uploadBgra(_impl->baseUpload, physicalBase)) return false;

	const float clear[4] = {0,0,0,1};
	_impl->context->OMSetRenderTargets(1, &_impl->rtv, nullptr);
	_impl->context->ClearRenderTargetView(_impl->rtv, clear);
	_impl->drawFullscreen(_impl->baseSrv, false);

	if (!_impl->overlaySurface || _impl->overlayW != physicalBase->w || _impl->overlayH != physicalBase->h)
	{
		if (_impl->overlaySurface) { SDL_FreeSurface(_impl->overlaySurface); _impl->overlaySurface = nullptr; }
		_impl->overlayW = physicalBase->w; _impl->overlayH = physicalBase->h;
		_impl->overlayPixels.assign((size_t)_impl->overlayW * (size_t)_impl->overlayH * 4u, 0);
		const Uint32 rmask = 0x00ff0000, gmask = 0x0000ff00, bmask = 0x000000ff, amask = 0xff000000;
		_impl->overlaySurface = SDL_CreateRGBSurfaceFrom(_impl->overlayPixels.data(), _impl->overlayW, _impl->overlayH,
			32, _impl->overlayW * 4, rmask, gmask, bmask, amask);
		if (!_impl->overlaySurface) return false;
		SDL_SetAlpha(_impl->overlaySurface, 0, 255);
	}
	std::memset(_impl->overlayPixels.data(), 0, _impl->overlayPixels.size());
	_impl->frameActive = true;
	return true;
#else
	(void)physicalBase;
	return false;
#endif
}

SDL_Surface *HdGpuBackend::cpuOverlay()
{
	return _impl->frameActive ? _impl->overlaySurface : nullptr;
}

bool HdGpuBackend::isCpuOverlay(const SDL_Surface *surface) const
{
	return _impl->frameActive && surface && surface == _impl->overlaySurface;
}

bool HdGpuBackend::blitCpuOverlay(SDL_Surface *source, int dstX, int dstY)
{
#if defined(_WIN32)
	if (!_impl->frameActive || !source || !_impl->overlaySurface) return false;
	SDL_Surface *destination = _impl->overlaySurface;
	if (destination->format->BytesPerPixel != 4) return false;

	const int x0 = std::max(0, dstX);
	const int y0 = std::max(0, dstY);
	const int x1 = std::min(destination->w, dstX + source->w);
	const int y1 = std::min(destination->h, dstY + source->h);
	if (x1 <= x0 || y1 <= y0) return true;

	if (SDL_MUSTLOCK(source) && SDL_LockSurface(source) != 0) return false;
	if (SDL_MUSTLOCK(destination) && SDL_LockSurface(destination) != 0)
	{
		if (SDL_MUSTLOCK(source)) SDL_UnlockSurface(source);
		return false;
	}

	const SDL_PixelFormat *sf = source->format;
	const int bpp = sf->BytesPerPixel;
	const bool hasColorKey = (source->flags & SDL_SRCCOLORKEY) != 0;
	const Uint32 colorKey = sf->colorkey;
	const bool alphaEnabled = (source->flags & SDL_SRCALPHA) != 0;
	const Uint8 surfaceAlpha = alphaEnabled ? sf->alpha : 255;

	auto readRaw = [&](const Uint8 *p) -> Uint32
	{
		switch (bpp)
		{
			case 1: return *p;
			case 2: { Uint16 v; std::memcpy(&v, p, 2); return v; }
			case 3:
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
				return ((Uint32)p[0] << 16) | ((Uint32)p[1] << 8) | p[2];
#else
				return p[0] | ((Uint32)p[1] << 8) | ((Uint32)p[2] << 16);
#endif
			case 4: { Uint32 v; std::memcpy(&v, p, 4); return v; }
			default: return 0;
		}
	};

	for (int dy = y0; dy < y1; ++dy)
	{
		const int sy = dy - dstY;
		const Uint8 *srcRow = static_cast<const Uint8*>(source->pixels) + (size_t)sy * source->pitch;
		Uint8 *dstRow = static_cast<Uint8*>(destination->pixels) + (size_t)dy * destination->pitch;
		for (int dx = x0; dx < x1; ++dx)
		{
			const int sx = dx - dstX;
			const Uint8 *sp = srcRow + (size_t)sx * bpp;
			const Uint32 raw = readRaw(sp);
			if (hasColorKey && raw == colorKey) continue;

			Uint8 sr = 0, sg = 0, sb = 0, sa = 255;
			SDL_GetRGBA(raw, sf, &sr, &sg, &sb, &sa);
			if (!sf->Amask) sa = 255;
			if (alphaEnabled)
				sa = (Uint8)(((unsigned)sa * (unsigned)surfaceAlpha + 127u) / 255u);
			else
				sa = 255;
			if (!sa) continue;

			// The staging surface is deliberately BGRA8 to match DXGI_FORMAT_B8G8R8A8_UNORM.
			Uint8 *dp = dstRow + (size_t)dx * 4u;
			const Uint8 db = dp[0], dg = dp[1], dr = dp[2], da = dp[3];
			if (sa == 255)
			{
				dp[0] = sb; dp[1] = sg; dp[2] = sr; dp[3] = 255;
				continue;
			}
			const unsigned inv = 255u - sa;
			const unsigned dstA = ((unsigned)da * inv + 127u) / 255u;
			const unsigned outA = (unsigned)sa + dstA;
			if (!outA) continue;
			dp[2] = (Uint8)(((unsigned)sr * sa + (unsigned)dr * dstA + outA / 2u) / outA);
			dp[1] = (Uint8)(((unsigned)sg * sa + (unsigned)dg * dstA + outA / 2u) / outA);
			dp[0] = (Uint8)(((unsigned)sb * sa + (unsigned)db * dstA + outA / 2u) / outA);
			dp[3] = (Uint8)std::min(255u, outA);
		}
	}

	if (SDL_MUSTLOCK(destination)) SDL_UnlockSurface(destination);
	if (SDL_MUSTLOCK(source)) SDL_UnlockSurface(source);
	return true;
#else
	(void)source; (void)dstX; (void)dstY;
	return false;
#endif
}

bool HdGpuBackend::frameActive() const { return _impl->frameActive; }

void HdGpuBackend::abortFrame()
{
	_impl->frameActive = false;
	_impl->lastUsed = false;
}

bool HdGpuBackend::present()
{
#if defined(_WIN32)
	if (!_impl->frameActive || !_impl->overlaySurface || !_impl->swap) return false;
	if (!_impl->ensureDynamicBgra(_impl->overlayUpload, _impl->overlaySrv, _impl->overlayTexW, _impl->overlayTexH,
		_impl->overlaySurface->w, _impl->overlaySurface->h) ||
		!_impl->uploadBgra(_impl->overlayUpload, _impl->overlaySurface))
	{
		abortFrame();
		return false;
	}
	_impl->drawFullscreen(_impl->overlaySrv, true);
	HRESULT hr = _impl->swap->Present(0, 0);
	if (FAILED(hr))
	{
		_impl->shutdownDevice();
		abortFrame();
		return false;
	}
	_impl->lastUsed = true;
	_impl->lastMapDrawCalls = _impl->mapDrawCalls;
	_impl->lastIndexedDrawCalls = _impl->indexedDrawCalls;
	_impl->lastEnvironmentDrawCalls = _impl->environmentDrawCalls;
	_impl->frameActive = false;
	return true;
#else
	return false;
#endif
}

bool HdGpuBackend::beginMap(const unsigned *drawOrder, int mapWidth, int mapHeight, const SDL_Color *palette,
	const SDL_Color *neutralPalette, int clipX0, int clipY0, int clipX1, int clipY1)
{
#if defined(_WIN32)
	if (!_impl->frameActive || !drawOrder || !palette || mapWidth <= 0 || mapHeight <= 0) return false;
	if (!_impl->ensureOrderTexture(mapWidth, mapHeight)) return false;
	if (!_impl->ensurePaletteTexture(palette)) return false;
	if (!_impl->ensureEnvironmentTextures(neutralPalette ? neutralPalette : palette, palette)) return false;
	D3D11_MAPPED_SUBRESOURCE mapped = {};
	HRESULT hr = _impl->context->Map(_impl->orderTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
	if (FAILED(hr)) return false;
	const Uint8 *src = reinterpret_cast<const Uint8*>(drawOrder);
	Uint8 *dst = static_cast<Uint8*>(mapped.pData);
	const size_t rowBytes = (size_t)mapWidth * sizeof(unsigned);
	for (int y = 0; y < mapHeight; ++y)
		std::memcpy(dst + (size_t)y * mapped.RowPitch, src + (size_t)y * rowBytes, rowBytes);
	_impl->context->Unmap(_impl->orderTexture, 0);

	_impl->setFullViewport();
	D3D11_RECT rect = { clipX0, clipY0, clipX1, clipY1 };
	_impl->context->RSSetState(_impl->rasterScissor);
	_impl->context->RSSetScissorRects(1, &rect);
	return true;
#else
	(void)drawOrder; (void)mapWidth; (void)mapHeight; (void)palette; (void)neutralPalette; (void)clipX0; (void)clipY0; (void)clipX1; (void)clipY1;
	return false;
#endif
}

bool HdGpuBackend::drawMapSprite(const HdGpuMapSprite &s)
{
#if defined(_WIN32)
	if (!_impl->frameActive || s.destW <= 0 || s.destH <= 0) return false;
	ID3D11ShaderResourceView *spriteSrv = nullptr;
	if (!_impl->ensureImage(s, &spriteSrv)) return false;

	struct alignas(16) SpriteConstants
	{
		float destPx[4];
		float metrics0[4]; // outputW, outputH, baseRenderX, baseRenderY
		float metrics1[4]; // renderToPhysicalX/Y, light, unused
		uint32_t ids0[4];  // drawOrder, srcW, srcH, mapW
		int32_t ids1[4];   // mapH, clipBegX, clipBegY, clipEndX
		int32_t ids2[4];   // clipEndY, flags, unused, unused
	};
	static_assert(sizeof(SpriteConstants) == 96, "SpriteConstants must match HLSL cbuffer");

	SpriteConstants c = {};
	c.destPx[0] = (float)s.destX; c.destPx[1] = (float)s.destY;
	c.destPx[2] = (float)s.destW; c.destPx[3] = (float)s.destH;
	c.metrics0[0] = (float)_impl->width; c.metrics0[1] = (float)_impl->height;
	c.metrics0[2] = (float)s.baseRenderX; c.metrics0[3] = (float)s.baseRenderY;
	c.metrics1[0] = (float)s.renderToPhysicalX; c.metrics1[1] = (float)s.renderToPhysicalY;
	c.metrics1[2] = (float)(16 - std::max(0, std::min(16, s.shade))) / 16.0f;
	c.ids0[0] = s.drawOrder; c.ids0[1] = s.imageWidth; c.ids0[2] = s.imageHeight; c.ids0[3] = (uint32_t)_impl->orderW;
	c.ids1[0] = _impl->orderH; c.ids1[1] = s.clipBegX; c.ids1[2] = s.clipBegY; c.ids1[3] = s.clipEndX;
	c.ids2[0] = s.clipEndY; c.ids2[1] = (s.hasClipMask ? 1 : 0) | (s.rightHalfOnly ? 2 : 0);
	c.ids2[2] = std::max(0, std::min(16, s.shade));

	D3D11_MAPPED_SUBRESOURCE mapped = {};
	HRESULT hr = _impl->context->Map(_impl->spriteConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
	if (FAILED(hr)) return false;
	std::memcpy(mapped.pData, &c, sizeof(c));
	_impl->context->Unmap(_impl->spriteConstants, 0);

	static const float blendFactor[4] = {0,0,0,0};
	_impl->context->OMSetRenderTargets(1, &_impl->rtv, nullptr);
	_impl->context->OMSetBlendState(_impl->alphaBlend, blendFactor, 0xffffffffu);
	_impl->context->IASetInputLayout(nullptr);
	_impl->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	_impl->context->VSSetShader(_impl->spriteVs, nullptr, 0);
	_impl->context->VSSetConstantBuffers(0, 1, &_impl->spriteConstants);
	ID3D11PixelShader *pixelShader = _impl->spritePs;
	if (s.colorMode == HdColorMode::IndexedLegacy) pixelShader = _impl->indexedSpritePs;
	else if (s.colorMode == HdColorMode::Environment) pixelShader = _impl->environmentSpritePs;
	_impl->context->PSSetShader(pixelShader, nullptr, 0);
	_impl->context->PSSetConstantBuffers(0, 1, &_impl->spriteConstants);
	const size_t envIndex = hdEnvironmentProfileIndex(s.environmentProfile);
	ID3D11ShaderResourceView *envSrv = _impl->environmentSrvs[envIndex];
	ID3D11ShaderResourceView *srvs[5] = { nullptr, _impl->orderSrv, nullptr, _impl->paletteSrv, envSrv };
	if (s.colorMode == HdColorMode::IndexedLegacy) srvs[2] = spriteSrv;
	else srvs[0] = spriteSrv;
	_impl->context->PSSetShaderResources(0, 5, srvs);
	if (s.colorMode == HdColorMode::Environment) _impl->context->PSSetSamplers(1, 1, &_impl->linearSampler);
	_impl->context->Draw(4, 0);
	ID3D11ShaderResourceView *nullSrvs[5] = { nullptr, nullptr, nullptr, nullptr, nullptr };
	_impl->context->PSSetShaderResources(0, 5, nullSrvs);
	++_impl->mapDrawCalls;
	if (s.colorMode == HdColorMode::IndexedLegacy) ++_impl->indexedDrawCalls;
	if (s.colorMode == HdColorMode::Environment) ++_impl->environmentDrawCalls;
	return true;
#else
	(void)s;
	return false;
#endif
}

void HdGpuBackend::endMap()
{
#if defined(_WIN32)
	if (_impl->context) _impl->context->RSSetState(_impl->rasterNormal);
#endif
}

bool HdGpuBackend::wasUsedLastFrame() const { return _impl->lastUsed; }
const char *HdGpuBackend::lastModeName() const
{
	if (!_impl->lastUsed) return "OFF";
	if (_impl->lastIndexedDrawCalls && _impl->lastEnvironmentDrawCalls) return "D3D11-MAP+IDX+ENV";
	if (_impl->lastIndexedDrawCalls) return "D3D11-MAP+IDX";
	if (_impl->lastEnvironmentDrawCalls) return "D3D11-MAP+ENV";
	return "D3D11-MAP";
}
unsigned HdGpuBackend::lastMapDrawCalls() const { return _impl->lastMapDrawCalls; }
unsigned HdGpuBackend::lastIndexedDrawCalls() const { return _impl->lastIndexedDrawCalls; }
unsigned HdGpuBackend::lastEnvironmentDrawCalls() const { return _impl->lastEnvironmentDrawCalls; }

}
