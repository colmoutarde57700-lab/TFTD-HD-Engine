#include "HdCausticShader.h"
#include "HdCausticSettings.h"
#include "HdGpuBackend.h"

#include "HdRenderSpace.h"
#include "HdPerf.h"
#include "Logger.h"
#include "FileMap.h"
#include "Options.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <unordered_map>
#include <vector>
#include <istream>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <SDL_syswm.h>
#include "HdUnit3DGpu.h"
#endif

namespace OpenXcom
{

struct HdGpuBackend::Impl
{
	bool frameActive = false;
	bool preparedThisFrame = false;
	bool lastUsed = false;
	bool logicalBaseFrame = false;
	bool lastLogicalBase = false;
	unsigned mapDrawCalls = 0;
	unsigned indexedDrawCalls = 0;
	unsigned environmentDrawCalls = 0;
	unsigned presentationDrawCalls = 0;
	unsigned legacyPresentationDrawCalls = 0;
	unsigned lastMapDrawCalls = 0;
	unsigned lastIndexedDrawCalls = 0;
	unsigned lastEnvironmentDrawCalls = 0;
	unsigned lastPresentationDrawCalls = 0;
	unsigned lastLegacyPresentationDrawCalls = 0;
	SDL_Surface *overlaySurface = nullptr;
	std::vector<Uint8> overlayPixels;
	int overlayW = 0;
	int overlayH = 0;
	bool overlayDirty = false;
	int overlayDirtyX0 = 0, overlayDirtyY0 = 0, overlayDirtyX1 = 0, overlayDirtyY1 = 0;

#if defined(_WIN32)
	HdUnit3DGpu unit3D;
	ID3D11Device *device = nullptr;
	ID3D11DeviceContext *context = nullptr;
	IDXGISwapChain *swap = nullptr;
	ID3D11RenderTargetView *rtv = nullptr;
	// Debug text is drawn straight into the D3D11 swap chain by Direct2D.
	// It never passes through an OXCE Text/Surface or an SDL CPU overlay.
	ID2D1Factory *debugD2dFactory = nullptr;
	IDWriteFactory *debugWriteFactory = nullptr;
	ID2D1RenderTarget *debugD2dTarget = nullptr;
	ID2D1SolidColorBrush *debugTextBrush = nullptr;
	ID2D1SolidColorBrush *debugBackdropBrush = nullptr;
	IDWriteTextFormat *debugTextFormat = nullptr;
	IDWriteTextLayout *debugTextLayout = nullptr;
	std::string debugTextUtf8;
	int debugLayoutW = 0, debugLayoutH = 0;
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
	ID3D11PixelShader *nearestBasePs = nullptr;
	ID3D11VertexShader *spriteVs = nullptr;
	ID3D11PixelShader *spritePs = nullptr;
	ID3D11PixelShader *indexedSpritePs = nullptr;
	ID3D11PixelShader *environmentSpritePs = nullptr;
	ID3D11VertexShader *bedrockVs = nullptr;
	ID3D11PixelShader *bedrockPs = nullptr;
	ID3D11InputLayout *bedrockInputLayout = nullptr;
	ID3D11Buffer *bedrockVertexBuffer = nullptr;
	unsigned bedrockVertexCapacity = 0;
	uint64_t uploadedBedrockVertexRevision = 0;
	unsigned uploadedBedrockVertexCount = 0;
	ID3D11VertexShader *worldCoverageVs = nullptr;
	ID3D11InputLayout *worldCoverageInputLayout = nullptr;
	ID3D11Buffer *worldCoverageVertexBuffer = nullptr;
	unsigned worldCoverageVertexCapacity = 0;
	ID3D11VertexShader *visibilityVs = nullptr;
	ID3D11PixelShader *visibilityPs = nullptr;
	ID3D11PixelShader *visionCoveragePs = nullptr;
	ID3D11PixelShader *visionSoftPs = nullptr;
	ID3D11Texture2D *visionMaskTexture = nullptr;
	ID3D11RenderTargetView *visionMaskRtv = nullptr;
	ID3D11ShaderResourceView *visionMaskSrv = nullptr;
	ID3D11Texture2D *bedrockFogTexture = nullptr;
	ID3D11ShaderResourceView *bedrockFogSrv = nullptr;
	unsigned bedrockFogW = 0, bedrockFogH = 0;
	ID3D11Texture2D *visionSceneCopy = nullptr;
	ID3D11ShaderResourceView *visionSceneSrv = nullptr;
	int visionW = 0, visionH = 0;
	ID3D11InputLayout *visibilityInputLayout = nullptr;
	ID3D11VertexShader *cursorVs = nullptr;
	ID3D11PixelShader *cursorPs = nullptr;
	ID3D11InputLayout *cursorInputLayout = nullptr;
	ID3D11PixelShader *visibilityTakeoverPs = nullptr;
	ID3D11Texture2D *visibilityStateTexture = nullptr;
	ID3D11ShaderResourceView *visibilityStateSrv = nullptr;
	int visibilityStateW = 0, visibilityStateH = 0;
	ID3D11PixelShader *localLightPs = nullptr;
	ID3D11PixelShader *smokePs = nullptr;
	ID3D11PixelShader *mapLegacyOverlayPs = nullptr;
	ID3D11PixelShader *presentationRgbaPs = nullptr;
	ID3D11PixelShader *presentationIndexedPs = nullptr;
	ID3D11SamplerState *pointSampler = nullptr;
	ID3D11SamplerState *linearSampler = nullptr;
	ID3D11SamplerState *linearWrapSampler = nullptr;
	ID3D11SamplerState *bedrockAnisoWrapSampler = nullptr;
	// REAL HD MATERIAL PERF V1. Oversized colour/albedo maps use a dedicated
	// balanced sampler. PBR normal/roughness/AO keep the validated x8 sampler.
	ID3D11SamplerState *bedrockLargeColorWrapSampler = nullptr;
	ID3D11BlendState *alphaBlend = nullptr;
	ID3D11BlendState *premultipliedBlend = nullptr;
	ID3D11BlendState *additiveBlend = nullptr;
	ID3D11RasterizerState *rasterNormal = nullptr;
	ID3D11RasterizerState *rasterScissor = nullptr;
	ID3D11Buffer *spriteConstants = nullptr;
	ID3D11Buffer *roofCausticConstants = nullptr;
	ID3D11Buffer *baseConstants = nullptr;

	// GEOSCAPE HD PROOF OF LIFE V1 resources.
	ID3D11VertexShader *geoscapeVs = nullptr;
	ID3D11PixelShader *geoscapePs = nullptr;
	ID3D11InputLayout *geoscapeInputLayout = nullptr;
	ID3D11Buffer *geoscapeVertexBuffer = nullptr;
	ID3D11Buffer *geoscapeIndexBuffer = nullptr;
	ID3D11Buffer *geoscapeConstants = nullptr;
	ID3D11Texture2D *geoscapeDepth = nullptr;
	ID3D11DepthStencilView *geoscapeDsv = nullptr;
	ID3D11DepthStencilState *geoscapeDepthState = nullptr;
	std::string geoscapeMeshKey;
	unsigned geoscapeVertexCount = 0;
	unsigned geoscapeIndexCount = 0;

	// Real HD map logical extent belongs to the compositor lifecycle and is
	// independent of any Legacy order texture.
	int mapLogicalW = 0, mapLogicalH = 0;

	// Legacy raster order survives only as an explicit compatibility bridge.
	ID3D11Texture2D *orderTexture = nullptr;
	ID3D11ShaderResourceView *orderSrv = nullptr;
	int orderW = 0, orderH = 0;
	bool legacyBridgeOrderReady = false;

	// REAL HD WORLD COMPOSITOR V1. Geometric depth is generated only by
	// primitives that possess real world geometry. Compatibility sprites/Legacy
	// bridge pixels never synthesize depth from painter order.
	ID3D11Texture2D *worldDepth = nullptr;
	ID3D11DepthStencilView *worldDsv = nullptr;
	ID3D11DepthStencilView *worldReadOnlyDsv = nullptr;
	ID3D11ShaderResourceView *worldDepthSrv = nullptr;
	ID3D11DepthStencilState *worldDepthWriteState = nullptr;
	ID3D11DepthStencilState *worldDepthReadState = nullptr;
	int worldDepthW = 0, worldDepthH = 0;
	float worldDepthMin = -64.0f;
	float worldDepthMax = 4096.0f;
	ID3D11Texture2D *bedrockCraterTexture = nullptr;
	ID3D11ShaderResourceView *bedrockCraterSrv = nullptr;
	int bedrockCraterW = 0, bedrockCraterH = 0;
	uint64_t bedrockCraterUploadedRevision = ~0ULL;
	ID3D11Texture2D *bedrockWeaponTexture = nullptr;
	ID3D11ShaderResourceView *bedrockWeaponSrv = nullptr;
	int bedrockWeaponW = 0, bedrockWeaponH = 0;
	uint64_t bedrockWeaponUploadedRevision = ~0ULL;
	ID3D11Texture2D *paletteTexture = nullptr;
	ID3D11ShaderResourceView *paletteSrv = nullptr;
	uint64_t uploadedPaletteSignature = 0;
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
		unsigned mipLevels = 1;
	};
	std::unordered_map<std::string, GpuImage> images;

	struct DynamicSurfaceImage
	{
		ID3D11Texture2D *texture = nullptr;
		ID3D11ShaderResourceView *srv = nullptr;
		int width = 0;
		int height = 0;
		bool indexed = false;
	};
	std::unordered_map<std::string, DynamicSurfaceImage> dynamicSurfaces;

	// PERF FOUNDATION V1: true non-blocking D3D11 timestamps.  CPU submission
	// timers cannot tell whether the GPU is actually busy, so keep a small ring
	// of timestamp/disjoint queries and only read frames that the driver has
	// already completed (DONOTFLUSH).  A busy ring drops a sample rather than
	// ever stalling the render thread for diagnostics.
	struct GpuTimingSlot
	{
		ID3D11Query *disjoint = nullptr;
		ID3D11Query *frameStart = nullptr;
		ID3D11Query *frameEnd = nullptr;
		ID3D11Query *mapStart = nullptr;
		ID3D11Query *mapEnd = nullptr;
		bool submitted = false;
		bool mapStarted = false;
		bool mapEnded = false;
		uint64_t serial = 0;
	};
	static constexpr unsigned GpuTimingRingSize = 6;
	std::array<GpuTimingSlot, GpuTimingRingSize> gpuTimingSlots{};
	int gpuTimingActiveSlot = -1;
	unsigned gpuTimingWriteCursor = 0;
	uint64_t gpuTimingSerial = 0;
	uint64_t gpuTimingResolved = 0;
	uint64_t gpuTimingDropped = 0;
	double gpuTimingLastFrameUs = 0.0;
	double gpuTimingLastMapUs = 0.0;
	bool gpuTimingLastValid = false;

	static void releaseQuery(ID3D11Query *&query)
	{
		if (query) { query->Release(); query = nullptr; }
	}

	void releaseGpuTimingQueries()
	{
		for (auto &slot : gpuTimingSlots)
		{
			releaseQuery(slot.disjoint);
			releaseQuery(slot.frameStart);
			releaseQuery(slot.frameEnd);
			releaseQuery(slot.mapStart);
			releaseQuery(slot.mapEnd);
			slot = GpuTimingSlot{};
		}
		gpuTimingActiveSlot = -1;
		gpuTimingWriteCursor = 0;
		gpuTimingSerial = 0;
		gpuTimingResolved = 0;
		gpuTimingDropped = 0;
		gpuTimingLastFrameUs = 0.0;
		gpuTimingLastMapUs = 0.0;
		gpuTimingLastValid = false;
	}

	bool ensureGpuTimingQueries()
	{
		if (!device || !context) return false;
		if (gpuTimingSlots[0].disjoint) return true;
		D3D11_QUERY_DESC qd = {};
		for (auto &slot : gpuTimingSlots)
		{
			qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
			if (FAILED(device->CreateQuery(&qd, &slot.disjoint)) || !slot.disjoint) { releaseGpuTimingQueries(); return false; }
			qd.Query = D3D11_QUERY_TIMESTAMP;
			if (FAILED(device->CreateQuery(&qd, &slot.frameStart)) || !slot.frameStart ||
				FAILED(device->CreateQuery(&qd, &slot.frameEnd)) || !slot.frameEnd ||
				FAILED(device->CreateQuery(&qd, &slot.mapStart)) || !slot.mapStart ||
				FAILED(device->CreateQuery(&qd, &slot.mapEnd)) || !slot.mapEnd)
			{
				releaseGpuTimingQueries();
				return false;
			}
		}
		return true;
	}

	bool tryResolveGpuTimingSlot(GpuTimingSlot &slot)
	{
		if (!slot.submitted || !context) return !slot.submitted;
		D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint = {};
		if (context->GetData(slot.disjoint, &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
			return false;
		UINT64 frameStart = 0, frameEnd = 0, mapStart = 0, mapEnd = 0;
		if (context->GetData(slot.frameStart, &frameStart, sizeof(frameStart), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
			context->GetData(slot.frameEnd, &frameEnd, sizeof(frameEnd), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
			return false;
		bool mapValid = slot.mapStarted && slot.mapEnded;
		if (mapValid &&
			(context->GetData(slot.mapStart, &mapStart, sizeof(mapStart), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
			 context->GetData(slot.mapEnd, &mapEnd, sizeof(mapEnd), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK))
			return false;

		if (!disjoint.Disjoint && disjoint.Frequency && frameEnd >= frameStart)
		{
			gpuTimingLastFrameUs = (double)(frameEnd - frameStart) * 1000000.0 / (double)disjoint.Frequency;
			gpuTimingLastMapUs = (mapValid && mapEnd >= mapStart)
				? (double)(mapEnd - mapStart) * 1000000.0 / (double)disjoint.Frequency : 0.0;
			gpuTimingLastValid = true;
		}
		else
		{
			gpuTimingLastValid = false;
		}
		++gpuTimingResolved;
		slot.submitted = false;
		slot.mapStarted = slot.mapEnded = false;
		return true;
	}

	void resolveReadyGpuTimings()
	{
		for (auto &slot : gpuTimingSlots) tryResolveGpuTimingSlot(slot);
	}

	void beginGpuTimingFrame()
	{
		gpuTimingActiveSlot = -1;
		if (!ensureGpuTimingQueries()) return;
		resolveReadyGpuTimings();
		for (unsigned attempt = 0; attempt < GpuTimingRingSize; ++attempt)
		{
			const unsigned idx = (gpuTimingWriteCursor + attempt) % GpuTimingRingSize;
			GpuTimingSlot &slot = gpuTimingSlots[idx];
			if (slot.submitted) continue;
			slot.mapStarted = slot.mapEnded = false;
			slot.serial = ++gpuTimingSerial;
			context->Begin(slot.disjoint);
			context->End(slot.frameStart);
			gpuTimingActiveSlot = (int)idx;
			gpuTimingWriteCursor = (idx + 1u) % GpuTimingRingSize;
			return;
		}
		++gpuTimingDropped;
	}

	void beginGpuTimingMap()
	{
		if (gpuTimingActiveSlot < 0 || !context) return;
		GpuTimingSlot &slot = gpuTimingSlots[(unsigned)gpuTimingActiveSlot];
		if (slot.mapStarted) return;
		context->End(slot.mapStart);
		slot.mapStarted = true;
	}

	void endGpuTimingMap()
	{
		if (gpuTimingActiveSlot < 0 || !context) return;
		GpuTimingSlot &slot = gpuTimingSlots[(unsigned)gpuTimingActiveSlot];
		if (!slot.mapStarted || slot.mapEnded) return;
		context->End(slot.mapEnd);
		slot.mapEnded = true;
	}

	void endGpuTimingFrame()
	{
		if (gpuTimingActiveSlot < 0 || !context) { resolveReadyGpuTimings(); return; }
		GpuTimingSlot &slot = gpuTimingSlots[(unsigned)gpuTimingActiveSlot];
		if (slot.mapStarted && !slot.mapEnded) endGpuTimingMap();
		context->End(slot.frameEnd);
		context->End(slot.disjoint);
		slot.submitted = true;
		gpuTimingActiveSlot = -1;
		resolveReadyGpuTimings();
	}

	unsigned gpuTimingPendingCount() const
	{
		unsigned count = 0;
		for (const auto &slot : gpuTimingSlots) if (slot.submitted) ++count;
		return count;
	}

	static void releaseTexture(ID3D11Texture2D *&texture, ID3D11ShaderResourceView *&srv)
	{
		if (srv) { srv->Release(); srv = nullptr; }
		if (texture) { texture->Release(); texture = nullptr; }
	}

	void releaseDebugTarget()
	{
		if (debugTextBrush) { debugTextBrush->Release(); debugTextBrush = nullptr; }
		if (debugBackdropBrush) { debugBackdropBrush->Release(); debugBackdropBrush = nullptr; }
		if (debugD2dTarget) { debugD2dTarget->Release(); debugD2dTarget = nullptr; }
	}

	void releaseDebugOverlay()
	{
		if (debugTextLayout) { debugTextLayout->Release(); debugTextLayout = nullptr; }
		if (debugTextFormat) { debugTextFormat->Release(); debugTextFormat = nullptr; }
		releaseDebugTarget();
		if (debugWriteFactory) { debugWriteFactory->Release(); debugWriteFactory = nullptr; }
		if (debugD2dFactory) { debugD2dFactory->Release(); debugD2dFactory = nullptr; }
		debugTextUtf8.clear();
		debugLayoutW = debugLayoutH = 0;
	}

	bool drawDebugOverlay(const std::string &utf8)
	{
		if (!frameActive || !device || !context || !swap || utf8.empty()) return false;
		if (!debugD2dFactory && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
			IID_ID2D1Factory, nullptr, reinterpret_cast<void **>(&debugD2dFactory)))) return false;
		if (!debugWriteFactory && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
			__uuidof(IDWriteFactory), reinterpret_cast<IUnknown **>(&debugWriteFactory)))) return false;
		if (!debugTextFormat && FAILED(debugWriteFactory->CreateTextFormat(L"Consolas", nullptr,
			DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
			16.0f, L"fr-fr", &debugTextFormat))) return false;
		if (!debugD2dTarget)
		{
			IDXGISurface *surface = nullptr;
			if (FAILED(swap->GetBuffer(0, IID_IDXGISurface, reinterpret_cast<void **>(&surface)))) return false;
			D2D1_RENDER_TARGET_PROPERTIES props = {};
			props.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
			props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
			props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
			props.dpiX = props.dpiY = 96.0f;
			HRESULT hr = debugD2dFactory->CreateDxgiSurfaceRenderTarget(surface, &props, &debugD2dTarget);
			surface->Release();
			if (FAILED(hr)) return false;
			const D2D1_COLOR_F yellow = { 0.95f, 0.92f, 0.25f, 1.0f };
			const D2D1_COLOR_F black = { 0.0f, 0.0f, 0.0f, 0.72f };
			if (FAILED(debugD2dTarget->CreateSolidColorBrush(&yellow, nullptr, &debugTextBrush)) ||
				FAILED(debugD2dTarget->CreateSolidColorBrush(&black, nullptr, &debugBackdropBrush)))
			{
				releaseDebugTarget();
				return false;
			}
		}
		if (!debugTextLayout || debugTextUtf8 != utf8 || debugLayoutW != width || debugLayoutH != height)
		{
			if (debugTextLayout) { debugTextLayout->Release(); debugTextLayout = nullptr; }
			const int count = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
			if (count <= 0) return false;
			std::wstring wide((size_t)count, L'\0');
			if (MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), &wide[0], count) != count) return false;
			if (FAILED(debugWriteFactory->CreateTextLayout(wide.data(), (UINT32)wide.size(), debugTextFormat,
				(float)std::max(1, width - 16), (float)std::max(1, height - 12), &debugTextLayout))) return false;
			debugTextUtf8 = utf8;
			debugLayoutW = width;
			debugLayoutH = height;
		}
		DWRITE_TEXT_METRICS metrics = {};
		if (FAILED(debugTextLayout->GetMetrics(&metrics))) return false;
		const D2D1_RECT_F backdrop = { 0.0f, 0.0f, (float)width,
			std::min((float)height, metrics.height + 12.0f) };
		const D2D1_POINT_2F origin = { 8.0f, 4.0f };
		context->OMSetRenderTargets(0, nullptr, nullptr);
		debugD2dTarget->BeginDraw();
		debugD2dTarget->FillRectangle(&backdrop, debugBackdropBrush);
		debugD2dTarget->DrawTextLayout(origin, debugTextLayout, debugTextBrush);
		const HRESULT hr = debugD2dTarget->EndDraw();
		if (hr == D2DERR_RECREATE_TARGET) releaseDebugTarget();
		return SUCCEEDED(hr);
	}

	void shutdownDevice()
	{
		releaseDebugOverlay();
		// Queries reference the current device/context and must be released before them.
		releaseGpuTimingQueries();
		unit3D.reset();
		for (auto &pair : images)
		{
			if (pair.second.srv) pair.second.srv->Release();
			if (pair.second.texture) pair.second.texture->Release();
		}
		images.clear();
		for (auto &pair : dynamicSurfaces)
		{
			if (pair.second.srv) pair.second.srv->Release();
			if (pair.second.texture) pair.second.texture->Release();
		}
		dynamicSurfaces.clear();
		for (size_t i = 0; i < HdEnvironmentProfileCount; ++i)
		{
			if (environmentSrvs[i]) { environmentSrvs[i]->Release(); environmentSrvs[i] = nullptr; }
			if (environmentTextures[i]) { environmentTextures[i]->Release(); environmentTextures[i] = nullptr; }
			uploadedEnvironmentSignatures[i] = 0;
		}
		releaseTexture(paletteTexture, paletteSrv);
		uploadedPaletteSignature = 0;
		releaseTexture(orderTexture, orderSrv);
		if (worldDepthReadState) { worldDepthReadState->Release(); worldDepthReadState = nullptr; }
		if (worldDepthWriteState) { worldDepthWriteState->Release(); worldDepthWriteState = nullptr; }
		if (worldDepthSrv) { worldDepthSrv->Release(); worldDepthSrv = nullptr; }
		if (worldReadOnlyDsv) { worldReadOnlyDsv->Release(); worldReadOnlyDsv = nullptr; }
		if (worldDsv) { worldDsv->Release(); worldDsv = nullptr; }
		if (worldDepth) { worldDepth->Release(); worldDepth = nullptr; }
		worldDepthW = worldDepthH = 0;
		releaseTexture(visibilityStateTexture, visibilityStateSrv);
		visibilityStateW = visibilityStateH = 0;
		releaseTexture(bedrockCraterTexture, bedrockCraterSrv);
		bedrockCraterW = bedrockCraterH = 0;
		bedrockCraterUploadedRevision = ~0ULL;
		releaseTexture(bedrockWeaponTexture, bedrockWeaponSrv);
		bedrockWeaponW = bedrockWeaponH = 0;
		bedrockWeaponUploadedRevision = ~0ULL;
		releaseTexture(overlayUpload, overlaySrv);
		releaseTexture(baseUpload, baseSrv);
		if (spriteConstants) { spriteConstants->Release(); spriteConstants = nullptr; }
		if (roofCausticConstants) { roofCausticConstants->Release(); roofCausticConstants = nullptr; }
		if (baseConstants) { baseConstants->Release(); baseConstants = nullptr; }
		if (geoscapeDepthState) { geoscapeDepthState->Release(); geoscapeDepthState = nullptr; }
		if (geoscapeDsv) { geoscapeDsv->Release(); geoscapeDsv = nullptr; }
		if (geoscapeDepth) { geoscapeDepth->Release(); geoscapeDepth = nullptr; }
		if (geoscapeConstants) { geoscapeConstants->Release(); geoscapeConstants = nullptr; }
		if (geoscapeIndexBuffer) { geoscapeIndexBuffer->Release(); geoscapeIndexBuffer = nullptr; }
		if (geoscapeVertexBuffer) { geoscapeVertexBuffer->Release(); geoscapeVertexBuffer = nullptr; }
		if (geoscapeInputLayout) { geoscapeInputLayout->Release(); geoscapeInputLayout = nullptr; }
		if (geoscapePs) { geoscapePs->Release(); geoscapePs = nullptr; }
		if (geoscapeVs) { geoscapeVs->Release(); geoscapeVs = nullptr; }
		geoscapeMeshKey.clear();
		geoscapeVertexCount = geoscapeIndexCount = 0;
		if (rasterScissor) { rasterScissor->Release(); rasterScissor = nullptr; }
		if (rasterNormal) { rasterNormal->Release(); rasterNormal = nullptr; }
		if (additiveBlend) { additiveBlend->Release(); additiveBlend = nullptr; }
		if (alphaBlend) { alphaBlend->Release(); alphaBlend = nullptr; }
		if (premultipliedBlend) { premultipliedBlend->Release(); premultipliedBlend = nullptr; }
		if (bedrockLargeColorWrapSampler) { bedrockLargeColorWrapSampler->Release(); bedrockLargeColorWrapSampler = nullptr; }
		if (bedrockAnisoWrapSampler) { bedrockAnisoWrapSampler->Release(); bedrockAnisoWrapSampler = nullptr; }
		if (linearWrapSampler) { linearWrapSampler->Release(); linearWrapSampler = nullptr; }
		if (linearSampler) { linearSampler->Release(); linearSampler = nullptr; }
		if (pointSampler) { pointSampler->Release(); pointSampler = nullptr; }
		if (mapLegacyOverlayPs) { mapLegacyOverlayPs->Release(); mapLegacyOverlayPs = nullptr; }
		if (worldCoverageVertexBuffer) { worldCoverageVertexBuffer->Release(); worldCoverageVertexBuffer = nullptr; }
		worldCoverageVertexCapacity = 0;
		if (worldCoverageInputLayout) { worldCoverageInputLayout->Release(); worldCoverageInputLayout = nullptr; }
		if (worldCoverageVs) { worldCoverageVs->Release(); worldCoverageVs = nullptr; }
		if (bedrockVertexBuffer) { bedrockVertexBuffer->Release(); bedrockVertexBuffer = nullptr; }
		bedrockVertexCapacity = 0;
		uploadedBedrockVertexRevision = 0;
		uploadedBedrockVertexCount = 0;
		if (smokePs) { smokePs->Release(); smokePs = nullptr; }
		if (localLightPs) { localLightPs->Release(); localLightPs = nullptr; }
		if (visibilityInputLayout) { visibilityInputLayout->Release(); visibilityInputLayout = nullptr; }
		if (visionMaskRtv) { visionMaskRtv->Release(); visionMaskRtv = nullptr; }
		if (visionMaskSrv) { visionMaskSrv->Release(); visionMaskSrv = nullptr; }
		if (visionMaskTexture) { visionMaskTexture->Release(); visionMaskTexture = nullptr; }
		if (bedrockFogSrv) { bedrockFogSrv->Release(); bedrockFogSrv = nullptr; }
		if (bedrockFogTexture) { bedrockFogTexture->Release(); bedrockFogTexture = nullptr; }
		bedrockFogW = bedrockFogH = 0;
		if (visionSceneSrv) { visionSceneSrv->Release(); visionSceneSrv = nullptr; }
		if (visionSceneCopy) { visionSceneCopy->Release(); visionSceneCopy = nullptr; }
		visionW = visionH = 0;
		if (visionCoveragePs) { visionCoveragePs->Release(); visionCoveragePs = nullptr; }
		if (visionSoftPs) { visionSoftPs->Release(); visionSoftPs = nullptr; }
		if (cursorInputLayout) { cursorInputLayout->Release(); cursorInputLayout = nullptr; }
		if (cursorPs) { cursorPs->Release(); cursorPs = nullptr; }
		if (cursorVs) { cursorVs->Release(); cursorVs = nullptr; }
		if (visibilityTakeoverPs) { visibilityTakeoverPs->Release(); visibilityTakeoverPs = nullptr; }
		if (visibilityPs) { visibilityPs->Release(); visibilityPs = nullptr; }
		if (visibilityVs) { visibilityVs->Release(); visibilityVs = nullptr; }
		if (bedrockInputLayout) { bedrockInputLayout->Release(); bedrockInputLayout = nullptr; }
		if (bedrockPs) { bedrockPs->Release(); bedrockPs = nullptr; }
		if (bedrockVs) { bedrockVs->Release(); bedrockVs = nullptr; }
		if (environmentSpritePs) { environmentSpritePs->Release(); environmentSpritePs = nullptr; }
		if (indexedSpritePs) { indexedSpritePs->Release(); indexedSpritePs = nullptr; }
		if (spritePs) { spritePs->Release(); spritePs = nullptr; }
		if (presentationIndexedPs) { presentationIndexedPs->Release(); presentationIndexedPs = nullptr; }
		if (presentationRgbaPs) { presentationRgbaPs->Release(); presentationRgbaPs = nullptr; }
		if (spriteVs) { spriteVs->Release(); spriteVs = nullptr; }
		if (nearestBasePs) { nearestBasePs->Release(); nearestBasePs = nullptr; }
		if (fullscreenPs) { fullscreenPs->Release(); fullscreenPs = nullptr; }
		if (fullscreenVs) { fullscreenVs->Release(); fullscreenVs = nullptr; }
		if (rtv) { rtv->Release(); rtv = nullptr; }
		if (swap) { swap->Release(); swap = nullptr; }
		if (context) { context->Release(); context = nullptr; }
		if (device) { device->Release(); device = nullptr; }
		hwnd = nullptr;
		width = height = 0;
		baseW = baseH = overlayTexW = overlayTexH = orderW = orderH = 0;
		mapLogicalW = mapLogicalH = 0;
		legacyBridgeOrderReady = false;
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
			"cbuffer BaseCB:register(b0){uint4 dstRect;uint4 srcInfo;}"
			"struct VOut{float4 pos:SV_POSITION;float2 uv:TEXCOORD0;};"
			"VOut VS(uint id:SV_VertexID){float2 uv=float2((id<<1)&2,id&2);"
			"VOut o;o.pos=float4(uv*float2(2,-2)+float2(-1,1),0,1);o.uv=uv;return o;}"
			"Texture2D tex0:register(t0);SamplerState samp0:register(s0);"
			"float4 PS(VOut i):SV_Target{return tex0.Sample(samp0,i.uv);}"
			"float4 PS_NEAREST(VOut i):SV_Target{"
			" int2 p=int2(floor(i.pos.xy)); int2 local=p-int2(dstRect.xy);"
			" uint dw=max(1u,dstRect.z), dh=max(1u,dstRect.w);"
			" if(local.x<0||local.y<0||local.x>=(int)dw||local.y>=(int)dh) discard;"
			" uint sw=max(1u,srcInfo.x), sh=max(1u,srcInfo.y);"
			" uint sx=min(sw-1,(uint)(((uint)local.x*sw)/dw));"
			" uint sy=min(sh-1,(uint)(((uint)local.y*sh)/dh));"
			" return tex0.Load(int3((int)sx,(int)sy,0));}";
		static const char visionSoftShader[] =
			"cbuffer BaseCB:register(b0){uint4 dstRect;uint4 srcInfo;}"
			"struct VOut{float4 pos:SV_POSITION;float2 uv:TEXCOORD0;};"
			"Texture2D<float4> sceneTex:register(t0);Texture2D<float> visionTex:register(t1);"
			"SamplerState linearSamp:register(s0);"
			"float4 PS_VISION_SOFT(VOut i):SV_Target{"
			" int2 p=int2(i.pos.xy);float4 scene=sceneTex.Load(int3(p,0));"
			" float2 size=float2(dstRect.z,dstRect.w);float2 uv=(float2(p)+0.5)/size;"
			" float coverage=visionTex.SampleLevel(linearSamp,uv,0);if(coverage<0.001)return scene;"
			" float width=0.02+0.42*saturate(float(srcInfo.x)/1000.0);"
			" float fade=smoothstep(0.5-width,0.5+width,coverage);"
			" return float4(scene.rgb*fade,scene.a);}";

		// Compatibility sprite shader: nearest source sampling, per-command shade,
		// right-half clipping and semantic clip mask. It deliberately has no access
		// to Legacy raster-order occlusion and does not write geometric world depth.
		static const char spriteShader[] =
			"cbuffer SpriteCB:register(b0){"
			" float4 destPx; float4 metrics0; float4 metrics1; uint4 ids0; int4 ids1; int4 ids2; float4 grade0; float4 grade1; float4 lightTint;};"
			"struct VOut{float4 pos:SV_POSITION;};"
			"VOut VS(uint id:SV_VertexID){float2 c=float2((id==1||id==3)?1:0,(id>=2)?1:0);"
			" float2 p=destPx.xy+c*destPx.zw; VOut o;"
			" o.pos=float4(p.x/metrics0.x*2-1,1-p.y/metrics0.y*2,0,1);return o;}"
			"Texture2D<float4> rgbaTex:register(t0);Texture2D<uint> orderTex:register(t1);"
			"Texture2D<uint> indexTex:register(t2);Texture2D<float4> paletteTex:register(t3);"
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

			" uint ix=min(srcW-1,(uint)(((uint)local.x*srcW)/(uint)dw));"
			" uint iy=min(srcH-1,(uint)(((uint)local.y*srcH)/(uint)dh));"
			" float4 c=rgbaTex.Load(int3(ix,iy,0)); if(c.a<=0&&(flags&4)==0) discard; c.rgb*=saturate(metrics1.z)*lightTint.xyz; c.rgb=roofCaustic(c.rgb,float2(local),float2(dw,dh),saturate(metrics1.z),(uint)ids2.y); return c;}"
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

			" uint ix=min(srcW-1,(uint)(((uint)local.x*srcW)/(uint)dw));"
			" uint iy=min(srcH-1,(uint)(((uint)local.y*srcH)/(uint)dh));"
			" float4 c=rgbaTex.Load(int3(ix,iy,0)); if(c.a<=0) discard;"
			" if(grade1.z>0.5){"
			"  c.rgb*=grade0.xyz; c.rgb*=grade0.w;"
			"  c.rgb=(c.rgb-0.5)*grade1.x+0.5;"
			"  float lum=dot(c.rgb,float3(0.2126,0.7152,0.0722));"
			"  c.rgb=lum.xxx+(c.rgb-lum.xxx)*grade1.y; c.rgb=saturate(c.rgb);}"
			" c.rgb*=saturate(metrics1.z)*lightTint.xyz; c.rgb=roofCaustic(c.rgb,float2(local),float2(dw,dh),saturate(metrics1.z),(uint)ids2.y); return c;}"
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

			" uint ix=min(srcW-1,(uint)(((uint)local.x*srcW)/(uint)dw));"
			" uint iy=min(srcH-1,(uint)(((uint)local.y*srcH)/(uint)dh));"
			" uint srcIndex=indexTex.Load(int3(ix,iy,0)); if(srcIndex==0) discard;"
			" uint baseColor=(uint)max(ids2.w,0); uint finalIndex=srcIndex;"
			" if(baseColor>0){uint tone=(srcIndex&0x0Fu); finalIndex=((((baseColor-1u)&0x0Fu)<<4)|tone);}"
			" float4 c=paletteTex.Load(int3((int)finalIndex,0,0)); c.rgb*=saturate(metrics1.z)*lightTint.xyz; c.rgb=roofCaustic(c.rgb,float2(local),float2(dw,dh),saturate(metrics1.z),(uint)ids2.y); c.a=1.0; return c;}"
			// Universal post-light pass. destPx is the light bounding rectangle;
			// metrics0.zw is physical light centre, metrics1.xy projected ellipse radii,
			// metrics1.z intensity and grade0.xyz RGB colour.
			"float4 PS_LOCAL_LIGHT(VOut i):SV_Target{"
			" float2 q=(i.pos.xy-metrics0.zw)/max(metrics1.xy,float2(1,1));float d=length(q);if(d>=1.0)discard;"
			" float f=saturate(1.0-d);f=f*f*(3.0-2.0*f);"
			" float e=max(0.0,metrics1.z*f);float gain=1.0-exp(-e);return float4(saturate(grade0.xyz)*gain,0);}"
			// HD SMOKE VOLUME V1: one soft pseudo-volume blob per logical smoke cell.
			// Overlap is continuous; OXCE still owns the actual smoke density and LOS.
			"float smokeHash(float2 p){return frac(sin(dot(p,float2(127.1,311.7)))*43758.5453);}"
			"float smokeNoise(float2 p){float2 c=floor(p),f=frac(p);f=f*f*(3-2*f);"
			" return lerp(lerp(smokeHash(c),smokeHash(c+float2(1,0)),f.x),lerp(smokeHash(c+float2(0,1)),smokeHash(c+1),f.x),f.y);}"
			"float4 PS_SMOKE(VOut i):SV_Target{"
			" float2 q=(i.pos.xy-metrics0.zw)/max(metrics1.xy,float2(1,1));float d=length(q);if(d>=1.0)discard;"
			" float radial=saturate(1.0-d);radial=radial*radial*(3.0-2.0*radial);"
			" float2 flow=q*3.5+float2(metrics1.w*0.12,-metrics1.w*0.08);float n=0.30+0.45*smokeNoise(flow)+0.25*smokeNoise(flow*2.03);"
			" if(metrics1.z>1.0){"
            "  float body=1.0-smoothstep(0.12,1.0,d);"
            "  float thickness=body*metrics1.z*grade0.w*(0.55+0.9*n);"
            "  float opacity=1.0-exp(-thickness);"
            "  return float4(grade0.xyz*(0.78+0.22*n),opacity);}"
            " float a=saturate(radial*metrics1.z*grade0.w*max(0.28,n));return float4(grade0.xyz,a);}"
			// Generic presentation shaders.  They intentionally know nothing about
			// Battlescape draw-order/voxels; destPx is already a physical rectangle.
			// metrics1.z carries global opacity. ids2.y bit0 enables an 8-bit colour
			// key and ids2.z stores that key.
			"float4 PS_PRESENT_RGBA(VOut i):SV_Target{"
			" int2 p=int2(floor(i.pos.xy)); int2 local=p-int2(destPx.xy);"
			" int dw=max(1,(int)destPx.z), dh=max(1,(int)destPx.w);"
			" if(local.x<0||local.y<0||local.x>=dw||local.y>=dh) discard;"
			" uint srcW=ids0.y, srcH=ids0.z;"
			" uint ix=min(srcW-1,(uint)(((uint)local.x*srcW)/(uint)dw));"
			" uint iy=min(srcH-1,(uint)(((uint)local.y*srcH)/(uint)dh));"
			" float4 c=rgbaTex.Load(int3(ix,iy,0)); c.a*=saturate(metrics1.z); if(c.a<=0) discard; return c;}"
			"float4 PS_PRESENT_INDEXED(VOut i):SV_Target{"
			" int2 p=int2(floor(i.pos.xy)); int2 local=p-int2(destPx.xy);"
			" int dw=max(1,(int)destPx.z), dh=max(1,(int)destPx.w);"
			" if(local.x<0||local.y<0||local.x>=dw||local.y>=dh) discard;"
			" uint srcW=ids0.y, srcH=ids0.z; uint flags=(uint)ids2.y;"
			" uint ix=min(srcW-1,(uint)(((uint)local.x*srcW)/(uint)dw));"
			" uint iy=min(srcH-1,(uint)(((uint)local.y*srcH)/(uint)dh));"
			" uint srcIndex=indexTex.Load(int3(ix,iy,0));"
			" if((flags&1)!=0 && srcIndex==(uint)max(ids2.z,0)) discard;"
			" float4 c=paletteTex.Load(int3((int)srcIndex,0,0)); c.a=saturate(metrics1.z); if(c.a<=0) discard; return c;}"
			// Legacy compatibility bridge: replay only pixels that OXCE actually
			// rasterized after the primary terrain was logically removed.
			"float4 PS_MAP_LEGACY_OVERLAY(VOut i):SV_Target{"
			" int2 p=int2(floor(i.pos.xy)); int2 local=p-int2(destPx.xy);"
			" int dw=max(1,(int)destPx.z), dh=max(1,(int)destPx.w);"
			" if(local.x<0||local.y<0||local.x>=dw||local.y>=dh) discard;"
			" float lx=((float)p.x-metrics0.z)/max(metrics1.x,0.0001);"
			" float ly=((float)p.y-metrics0.w)/max(metrics1.y,0.0001);"
			" int ox=(int)floor(lx), oy=(int)floor(ly); int ow=(int)ids0.x, oh=(int)ids0.y;"
			" if(ox<0||oy<0||ox>=ow||oy>=oh) discard;"
			" uint ord=orderTex.Load(int3(ox,oy,0));"
			" if(ord==0||ord<(uint)max(ids1.x,0)||ord>(uint)max(ids1.y,0)) discard;"
			" int sx=(int)metrics1.z+ox, sy=(int)metrics1.w+oy;"
			" return rgbaTex.Load(int3(sx,sy,0));}"
			// REAL HD FOV V1 keeps the former V3 hard screen-mask shader compiled only as
			// an emergency diagnostic fallback. The active V1 presentation path does not
			// call it: visibility is owned by each source Tile/TilePart/Z before draw.
			"float4 PS_VIS_TAKEOVER(VOut i):SV_Target{"
			" int2 p=int2(floor(i.pos.xy)); int2 local=p-int2(destPx.xy);"
			" int dw=max(1,(int)destPx.z), dh=max(1,(int)destPx.w);"
			" if(local.x<0||local.y<0||local.x>=dw||local.y>=dh) discard;"
			" float screenX=(float)local.x/max(metrics1.x,0.0001);"
			" float screenY=(float)local.y/max(metrics1.y,0.0001);"
			" int spriteW=max(4,(int)ids0.z), spriteH=max(1,(int)ids0.w);"
			" int viewLevel=ids1.x; int screenXi=(int)floor(screenX); int screenYi=(int)floor(screenY);"
			" screenYi+=(-spriteW/2)+viewLevel*((spriteH+spriteW/4)/2);"
			" int mapYraw=-screenXi+(int)metrics0.z+2*screenYi-2*(int)metrics0.w;"
			" int mapXraw=screenYi-(int)metrics0.w-mapYraw/4-spriteW/4;"
			" int mapX=mapXraw/(spriteW/4); int mapY=mapYraw/spriteW;"
			" int mapW=(int)ids0.x, mapH=(int)ids0.y;"
			" if(mapX<0||mapY<0||mapX>=mapW||mapY>=mapH) return float4(0,0,0,1);"
			" if(indexTex.Load(int3(mapX,mapY,0))!=0) return float4(0,0,0,1);"
			" discard; return float4(0,0,0,0);}";

		static const char visibilityShader[] =
			"cbuffer VisibilityCB:register(b0){float4 metrics0;};"
			"struct VIn{float2 pos:POSITION;float alpha:TEXCOORD0;};"
			"struct VOut{float4 pos:SV_POSITION;float alpha:TEXCOORD0;};"
			"VOut VS_VISIBILITY(VIn i){VOut o;o.pos=float4(i.pos.x/metrics0.x*2-1,1-i.pos.y/metrics0.y*2,0,1);o.alpha=i.alpha;return o;}"
			"float4 PS_VISIBILITY(VOut i):SV_Target{return float4(0,0,0,saturate(i.alpha));}"
			"float4 PS_VISION_COVERAGE(VOut i):SV_Target{return float4(saturate(i.alpha),0,0,1);}";
		static const char cursorShader[] =
			"cbuffer CursorCB:register(b0){float4 metrics0;float4 depthMeta;};"
			"struct CIn{float2 pos:POSITION;float depth:TEXCOORD0;float alpha:TEXCOORD1;};"
			"struct COut{float4 pos:SV_POSITION;float alpha:TEXCOORD0;};"
			"COut VS_CURSOR(CIn i){COut o;float d=saturate((i.depth-depthMeta.x)/max(depthMeta.y-depthMeta.x,0.0001));"
			"o.pos=float4(i.pos.x/metrics0.x*2-1,1-i.pos.y/metrics0.y*2,lerp(0.98,0.02,d),1);o.alpha=i.alpha;return o;}"
			"float4 PS_CURSOR(COut i):SV_Target{float3 c=metrics0.z>1.5?float3(0.72,0.40,0.86):(metrics0.z>0.5?float3(1,0.87,0.08):float3(1,0.10,0.08));"
			"return float4(c,saturate(i.alpha));}";

		static const char geoscapeShader[] =
			"cbuffer GlobeCB:register(b0){float4 trig;float4 camera;};"
			"struct GIn{float3 pos:POSITION;float3 normal:NORMAL;float4 color:COLOR0;};"
			"struct GOut{float4 pos:SV_POSITION;float3 normal:NORMAL;float4 color:COLOR0;};"
			"float3 rotateEarth(float3 p){"
			" float sy=trig.x,cy=trig.y,sp=trig.z,cp=trig.w;"
			" float3 q=float3(cy*p.x+sy*p.z,p.y,-sy*p.x+cy*p.z);"
			" return float3(q.x,cp*q.y-sp*q.z,sp*q.y+cp*q.z);}"
			"GOut VS_GLOBE(GIn i){GOut o;float3 p=rotateEarth(i.pos);"
			" float aspect=max(camera.x,0.01);float viewZ=max(0.02,camera.y-p.z);float focal=max(camera.z,0.01);"
			" float nearZ=0.01;float farZ=max(camera.w,nearZ+0.1);"
			" float zClip=viewZ*farZ/(farZ-nearZ)-nearZ*farZ/(farZ-nearZ);"
			" o.pos=float4(p.x*focal/aspect,p.y*focal,zClip,viewZ);"
			" o.normal=normalize(rotateEarth(i.normal));o.color=i.color;return o;}"
			"float4 PS_GLOBE(GOut i):SV_Target{"
			" float3 L=normalize(float3(-0.35,-0.18,-0.92));"
			" float ndl=saturate(dot(normalize(i.normal),-L));"
			" float light=0.42+0.58*ndl;"
			" float3 c=pow(saturate(i.color.rgb),1.0/2.2)*light;"
			" return float4(saturate(c),1);}";

		// REAL HD WORLD COVERAGE V1. Depth-only semantic geometry. The shader
		// consumes projected world coordinates + x+y+z camera-ray depth and has no
		// Legacy draw-order input or colour/material responsibility.
		static const char worldCoverageShader[] =
			"cbuffer CoverageCB:register(b0){float4 metrics0;float4 metrics1;}"
			"struct CIn{float2 logical:POSITION;float viewDepth:TEXCOORD0;};"
			"struct COut{float4 pos:SV_POSITION;};"
			"COut VS_COVERAGE(CIn i){"
			" float2 p=float2(metrics0.z+i.logical.x*metrics1.x,metrics0.w+i.logical.y*metrics1.y);"
			" float d=saturate((i.viewDepth-metrics1.z)/max(metrics1.w-metrics1.z,0.0001));"
			" float z=lerp(0.98,0.02,d);COut o;o.pos=float4(p.x/metrics0.x*2-1,1-p.y/metrics0.y*2,z,1);return o;}";

		static const char bedrockShader[] =
			"cbuffer BedrockCB:register(b0){"
			" float4 destPx; float4 metrics0; float4 metrics1; uint4 ids0; int4 ids1; int4 ids2;"
			" float4 localMeta; float4 depthMeta; float4 fogMeta; float4 localLightPosRadius[24]; float4 localLightColorIntensity[24]; float4 causticGlobal; float4 causticA[5]; float4 causticB[5];};"
			"struct BIn{float2 logical:POSITION;float2 uv:TEXCOORD0;float2 craterUv:TEXCOORD3;float light:TEXCOORD1;float role:TEXCOORD2;float viewDepth:TEXCOORD4;float3 lightTint:TEXCOORD5;float3 fogCoord:TEXCOORD6;};"
			"struct BOut{float4 pos:SV_POSITION;float2 uv:TEXCOORD0;float2 craterUv:TEXCOORD3;float light:TEXCOORD1;float role:TEXCOORD2;float3 lightTint:TEXCOORD5;float3 fogCoord:TEXCOORD6;};"
			"BOut VS_BEDROCK(BIn i){"
			" float2 p=float2(metrics0.z+i.logical.x*metrics1.x,metrics0.w+i.logical.y*metrics1.y);"
			" float d=saturate((i.viewDepth-depthMeta.x)/max(depthMeta.y-depthMeta.x,0.0001));"
			" float z=lerp(0.98,0.02,d);"
			" BOut o;o.pos=float4(p.x/metrics0.x*2-1,1-p.y/metrics0.y*2,z,1);"
			" o.uv=i.uv;o.craterUv=i.craterUv;o.light=i.light;o.role=i.role;o.lightTint=i.lightTint;o.fogCoord=i.fogCoord;return o;}"
			"Texture2D<float4> rgbaTex:register(t0);"
			"Texture2D<float4> normalTex:register(t2);Texture2D<float4> roughTex:register(t3);"
			"Texture3D<float4> envTex:register(t4);Texture2D<float4> aoTex:register(t5);"
			"Texture2D<float4> verticalTex:register(t6);Texture2D<float4> verticalNormalTex:register(t7);"
			"Texture2D<float4> verticalRoughTex:register(t8);Texture2D<float4> verticalAoTex:register(t9);"
			"Texture2D<float4> craterTex:register(t10);Texture2D<float4> weaponTex:register(t11);Texture2D<float> fogTex:register(t12);"
			"SamplerState envSamp:register(s1);SamplerState bedrockSamp:register(s2);"
			"SamplerState topColorSamp:register(s3);SamplerState verticalColorSamp:register(s4);"
			// Height in tactical voxels; the halo changes material, never excavates it.
			"float impactHeight(float2 uv,float layer){"
			" uv=clamp(uv,float2(metrics1.z*0.5,layer*fogMeta.y/fogMeta.z+metrics1.w*0.5),"
			"  float2(1.0-metrics1.z*0.5,(layer+1.0)*fogMeta.y/fogMeta.z-metrics1.w*0.5));"
			" float4 b=ids0.z!=0?craterTex.SampleLevel(envSamp,saturate(uv),0):float4(0,0,0,0);"
			" float4 w=ids0.w!=0?weaponTex.SampleLevel(envSamp,saturate(uv),0):float4(0,0,0,0);"
			" return clamp(b.g*1.6-b.r*4.0+w.g*0.65-w.r*1.25-w.a*0.65,-5.25,2.25);}"
			// Animated cellular ridges in WORLD coordinates: no camera-relative drift.
            // Current albedo and old PBR maps are different material generations.
            // Estimate only broad relief from THIS albedo; never claim authored
            // height. Blur grain/color specks before using luminance as a proxy.
            "float sandImageHeight(float2 uv,float2 dx,float2 dy){"
            " float3 rgb=rgbaTex.SampleGrad(topColorSamp,uv,dx*4.0,dy*4.0).rgb;"
            " return saturate((dot(rgb,float3(0.2126,0.7152,0.0722))-0.40)*2.4);}"
            "float4 PS_BEDROCK(BOut i):SV_Target{"
			" float2 fogUv=float2(clamp(i.fogCoord.x,0.5,fogMeta.x-0.5)/fogMeta.x,"
			" (i.fogCoord.z*fogMeta.y+clamp(i.fogCoord.y,0.5,fogMeta.y-0.5))/fogMeta.z);"
			" float coverage=fogTex.SampleLevel(envSamp,fogUv,0);"
			" float fogWidth=0.02+0.42*saturate(fogMeta.w);"
			" float fogAlpha=smoothstep(0.5-fogWidth,0.5+fogWidth,coverage);"
			" if(fogAlpha<=0.001)discard;"
			" int2 p=int2(floor(i.pos.xy));"
			" int ox=(int)floor(((float)p.x-metrics0.z)/max(metrics1.x,0.0001));"
			" int oy=(int)floor(((float)p.y-metrics0.w)/max(metrics1.y,0.0001));"

			" bool coveredGrade=i.role>=4.0;"
			" float surfaceRole=coveredGrade?(i.role-4.0):i.role;"
			" bool vertical=surfaceRole>0.5;"
			// Isometric parallax: the material follows the impact height rather than
			// merely darkening the undeformed sand. No change to gameplay elevation.
			" float2 baseImpactUv=i.craterUv;float reliefHeight=0;"
			" if(!vertical&&(ids0.z!=0||ids0.w!=0)&&abs(impactHeight(i.craterUv,i.fogCoord.z))>0.0001){"
			"  float2 uvPerVoxel=float2(1.0/max(fogMeta.x*16.0,1.0),1.0/max(fogMeta.z*16.0,1.0));"
			"  [unroll]for(int step=0;step<4;++step){reliefHeight=impactHeight(i.craterUv,i.fogCoord.z);i.craterUv=baseImpactUv+uvPerVoxel*reliefHeight;}"
			"  i.craterUv=clamp(i.craterUv,float2(metrics1.z*0.5,i.fogCoord.z*fogMeta.y/fogMeta.z+metrics1.w*0.5),float2(1.0-metrics1.z*0.5,(i.fogCoord.z+1.0)*fogMeta.y/fogMeta.z-metrics1.w*0.5));"
			"  i.uv+=reliefHeight/128.0;}"
            // P2M: image-aligned relief replaces the unrelated old AO proxy.
            " float2 sandDx=ddx(i.uv),sandDy=ddy(i.uv);float sandStrength=0;"
            " if(!vertical&&ids1.w>0){"
            "  float2 sandBaseUv=i.uv;"
            "  float footprint=max(length(sandDx),length(sandDy))*128.0;"
            "  sandStrength=((float)ids1.w/1000.0)/(1.0+footprint*footprint*4.0);"
            "  sandStrength*=1.0-0.75*saturate(abs(reliefHeight)/2.0);"
            "  [unroll]for(int microStep=0;microStep<3;++microStep){"
            "   float microHeight=sandStrength*(sandImageHeight(i.uv,sandDx,sandDy)-0.5);"
            "   i.uv=lerp(i.uv,sandBaseUv+microHeight/128.0,0.65);}"
            " }"
            " float4 c=vertical?verticalTex.Sample(verticalColorSamp,i.uv):rgbaTex.Sample(topColorSamp,i.uv);if(c.a<=0) discard;"
			" if(ids2.x!=0){float3 graded=envTex.SampleLevel(envSamp,c.rgb,0).rgb;"
			"  float gradeStrength=coveredGrade?((float)ids1.z/1000.0):(vertical?((float)ids1.y/1000.0):((float)ids1.x/1000.0));"
			"  c.rgb=saturate(c.rgb+(graded-c.rgb)*gradeStrength);}"
			" float4 blast=(ids0.z!=0&&!vertical)?saturate(craterTex.SampleLevel(envSamp,saturate(i.craterUv),0)):float4(0,0,0,0);"
			" float4 weapon=(ids0.w!=0&&!vertical)?saturate(weaponTex.SampleLevel(envSamp,saturate(i.craterUv),0)):float4(0,0,0,0);"
			" float core=blast.r,rim=blast.g,halo=blast.b;"
			" float weaponCore=weapon.r,weaponRimMask=weapon.g,weaponHalo=weapon.b,weaponLegacy=weapon.a;"
			" float2 impactTexel=float2(metrics1.z,metrics1.w);"
			" float blastHeight=rim*0.90-core*1.30;float2 blastSlope=float2(0,0);"
			" if(ids0.z!=0&&!vertical){"
			"  float4 bxP=craterTex.SampleLevel(envSamp,saturate(i.craterUv+float2(impactTexel.x,0)),0);"
			"  float4 bxM=craterTex.SampleLevel(envSamp,saturate(i.craterUv-float2(impactTexel.x,0)),0);"
			"  float4 byP=craterTex.SampleLevel(envSamp,saturate(i.craterUv+float2(0,impactTexel.y)),0);"
			"  float4 byM=craterTex.SampleLevel(envSamp,saturate(i.craterUv-float2(0,impactTexel.y)),0);"
			"  blastSlope=float2((bxM.g-bxP.g)*0.90-(bxM.r-bxP.r)*1.30,(byM.g-byP.g)*0.90-(byM.r-byP.r)*1.30)*2.35;}"
			" float weaponHeight=weaponRimMask*0.90-weaponCore*1.30-weaponHalo*0.18-weaponLegacy*0.60;"
			" float4 weaponXp=weapon,weaponXm=weapon,weaponYp=weapon,weaponYm=weapon,weaponX2p=weapon,weaponX2m=weapon,weaponY2p=weapon,weaponY2m=weapon;"
			" if(ids0.w!=0&&!vertical){"
			"  weaponXp=saturate(weaponTex.SampleLevel(envSamp,saturate(i.craterUv+float2(impactTexel.x,0)),0));"
			"  weaponXm=saturate(weaponTex.SampleLevel(envSamp,saturate(i.craterUv-float2(impactTexel.x,0)),0));"
			"  weaponYp=saturate(weaponTex.SampleLevel(envSamp,saturate(i.craterUv+float2(0,impactTexel.y)),0));"
			"  weaponYm=saturate(weaponTex.SampleLevel(envSamp,saturate(i.craterUv-float2(0,impactTexel.y)),0));"
			"  weaponX2p=saturate(weaponTex.SampleLevel(envSamp,saturate(i.craterUv+float2(impactTexel.x*2.0,0)),0));"
			"  weaponX2m=saturate(weaponTex.SampleLevel(envSamp,saturate(i.craterUv-float2(impactTexel.x*2.0,0)),0));"
			"  weaponY2p=saturate(weaponTex.SampleLevel(envSamp,saturate(i.craterUv+float2(0,impactTexel.y*2.0)),0));"
			"  weaponY2m=saturate(weaponTex.SampleLevel(envSamp,saturate(i.craterUv-float2(0,impactTexel.y*2.0)),0));}"
			" float hXp=weaponXp.g*0.90-weaponXp.r*1.30-weaponXp.b*0.18-weaponXp.a*0.60;"
			" float hXm=weaponXm.g*0.90-weaponXm.r*1.30-weaponXm.b*0.18-weaponXm.a*0.60;"
			" float hYp=weaponYp.g*0.90-weaponYp.r*1.30-weaponYp.b*0.18-weaponYp.a*0.60;"
			" float hYm=weaponYm.g*0.90-weaponYm.r*1.30-weaponYm.b*0.18-weaponYm.a*0.60;"
			" float hX2p=weaponX2p.g*0.90-weaponX2p.r*1.30-weaponX2p.b*0.18-weaponX2p.a*0.60;"
			" float hX2m=weaponX2m.g*0.90-weaponX2m.r*1.30-weaponX2m.b*0.18-weaponX2m.a*0.60;"
			" float hY2p=weaponY2p.g*0.90-weaponY2p.r*1.30-weaponY2p.b*0.18-weaponY2p.a*0.60;"
			" float hY2m=weaponY2m.g*0.90-weaponY2m.r*1.30-weaponY2m.b*0.18-weaponY2m.a*0.60;"
			" float weaponBowl=saturate(max(weaponCore,weaponLegacy));"
			" float weaponRim=saturate(weaponRimMask);"
			" float weaponOuter=saturate(weaponHalo);"
			" float2 weaponSlope=(float2(hXm-hXp,hYm-hYp)*1.65+float2(hX2m-hX2p,hY2m-hY2p)*0.70);"
			" c.rgb*=lerp(1.0,0.58,core);"
			" c.rgb*=lerp(1.0,1.16,rim);"
			" c.rgb*=lerp(1.0,1.045,halo);"
			" c.rgb*=lerp(1.0,0.56,weaponBowl);"
			" c.rgb*=lerp(1.0,1.26,weaponRim);"
			" c.rgb*=lerp(1.0,1.07,weaponOuter);"
			" float realHdLight=saturate(i.light);"
			// Derivative per WORLD voxel, not per atlas pixel. The previous unscaled
			// difference became almost flat on large blast stamps / dense atlases.
			" float2 reliefSlope=float2(0,0);"
			" if(!vertical&&max(max(core,rim),max(weaponBowl,weaponRim))>0.001){"
			"  float2 stepUv=max(impactTexel,float2(0.000001,0.000001));"
			"  float2 worldStep=stepUv*float2(fogMeta.x,fogMeta.z)*16.0;"
			"  reliefSlope=float2(impactHeight(i.craterUv-float2(stepUv.x,0),i.fogCoord.z)-impactHeight(i.craterUv+float2(stepUv.x,0),i.fogCoord.z),"
			"    impactHeight(i.craterUv-float2(0,stepUv.y),i.fogCoord.z)-impactHeight(i.craterUv+float2(0,stepUv.y),i.fogCoord.z))/max(2.0*worldStep,float2(0.001,0.001));}"
			" float reliefWeight=saturate(max(max(core,rim),max(weaponBowl,weaponRim)));"
			" bool usePbr=vertical?(ids2.z!=0):(ids2.y!=0);"
			" if(usePbr||reliefWeight>0.001){"
			"  float3 nRaw=usePbr?(vertical?verticalNormalTex.Sample(bedrockSamp,i.uv):normalTex.Sample(bedrockSamp,i.uv)).xyz*2.0-1.0:float3(0,0,1);"
			"  float rough=usePbr?saturate(vertical?verticalRoughTex.Sample(bedrockSamp,i.uv).r:roughTex.Sample(bedrockSamp,i.uv).r):0.9;"
			"  float ao=usePbr?saturate(vertical?verticalAoTex.Sample(bedrockSamp,i.uv).r:aoTex.Sample(bedrockSamp,i.uv).r):1.0;"
			"  if(!vertical&&ids1.w>0){"
            "   float e=max(0.003, max(length(sandDx),length(sandDy)));"
            "   float hx=sandImageHeight(i.uv-float2(e,0),sandDx,sandDy)-sandImageHeight(i.uv+float2(e,0),sandDx,sandDy);"
            "   float hy=sandImageHeight(i.uv-float2(0,e),sandDx,sandDy)-sandImageHeight(i.uv+float2(0,e),sandDx,sandDy);"
            "   nRaw=normalize(float3(float2(hx,hy)*sandStrength/(256.0*e),1));"
            "   rough=0.92;ao=1.0;}"
            "  if(!vertical){"
			"   nRaw=normalize(float3(nRaw.xy+reliefSlope,max(0.35,nRaw.z)));"
			"   rough=saturate(rough+core*0.14+rim*0.18+halo*0.05+weaponBowl*0.26+weaponRim*0.18+weaponOuter*0.06);"
			"   ao=saturate(ao*(1.0-core*0.28-rim*0.10-weaponBowl*0.54-weaponRim*0.16)+weaponRim*0.12+weaponOuter*0.05);}"
			"  float3 baseN=float3(0,0,1);"
			"  if(surfaceRole>0.5&&surfaceRole<1.5)baseN=normalize(float3(0.78,-0.42,0.46));"
			"  else if(surfaceRole>=1.5)baseN=normalize(float3(-0.42,0.78,0.46));"
			"  float topBlend=vertical?0.38:saturate(0.60+max(weaponBowl,core)*0.34+max(weaponRim,rim)*0.15);"
			"  float3 n=normalize(lerp(baseN,nRaw,topBlend));"
			"  float3 L=normalize(float3(-0.38,-0.28,0.88));float3 V=float3(0,0,1);float3 H=normalize(L+V);"
			"  float ndl=saturate(dot(n,L));float ndh=saturate(dot(n,H));"
			"  float diffuse=0.72+0.28*ndl;"
			"  float specPower=lerp(96.0,8.0,rough);"
			"  float spec=pow(ndh,specPower)*(1.0-rough)*0.24;"
			"  c.rgb*=lerp(0.68,1.0,ao)*diffuse;"
			"  c.rgb+=float3(spec,spec,spec);"
			" }"
			" float depthLuma=max((float)ids2.w,0.0)/1000.0;"
			" c.rgb*=realHdLight*depthLuma*i.lightTint;"
            // Covered surfaces are excluded; multiply existing illumination so
            // the effect cannot reveal unknown ground or illuminate dark rooms.
            " if(!vertical&&!coveredGrade&&depthMeta.z>0.0){"
            "  float pattern=waterCaustic(i.uv*9.35,depthMeta.w,causticGlobal,causticA,causticB);"
            "  float lightGate=smoothstep(0.12,0.65,realHdLight);"
            "  c.rgb*=1.0+float3(0.277725,0.357075,0.3769125)*pattern*depthMeta.z*lightGate;}"
			" c.rgb*=fogAlpha;c.a=1.0;return c;}";

		// The HLSL string needs the compile-time render scale without runtime
		// branches. Build the only generated fragment here.
		const std::string bedrockSource = std::string(HdCausticShaderFunctions) + bedrockShader;
		std::string spriteSource = std::string("cbuffer RoofCausticCB:register(b1){float4 roofGlobal;float4 roofA[5];float4 roofB[5];float4 roofMeta;uint4 roofRows[4];};") + HdCausticShaderFunctions +
            "float3 roofCaustic(float3 color,float2 local,float2 size,float light,uint flags){"
            "[branch]if((flags&8u)==0u||roofMeta.w<=0.0)return color;"
            "float2 q=local/size*float2(32,40)-float2(16,24);"
            "float2 uv=float2(q.y/16.0+q.x/32.0,q.y/16.0-q.x/32.0);"
            "if(any(uv<0.0)||any(uv>=1.0))return color;"
            "uint2 cell=(uint2)floor(uv*16.0);uint row=roofRows[cell.y/4u][cell.y%4u];"
            "if((row&(1u<<cell.x))==0u)return color;"
            "float pattern=waterCaustic((roofMeta.xy+uv)*(9.35/8.0),roofMeta.z,roofGlobal,roofA,roofB);"
            "return color*(1.0+float3(0.277725,0.357075,0.3769125)*pattern*roofMeta.w*smoothstep(0.12,0.65,light));}" + spriteShader;
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
		if (!compileShader(visionSoftShader, "PS_VISION_SOFT", "ps_4_0", &blob)) { shutdownDevice(); return false; }
		hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &visionSoftPs);
		blob->Release(); blob = nullptr;
		if (FAILED(hr) || !visionSoftPs) { shutdownDevice(); return false; }
		// Architecture V1 is additive: failure of the new logical-base shader must
		// never disable the already-validated P5/P6 D3D11 map path.  Keep the old
		// physical-base compositor alive and let beginFrameLogical() decline cleanly.
		if (compileShader(fullscreenShader, "PS_NEAREST", "ps_4_0", &blob))
		{
			hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &nearestBasePs);
			blob->Release(); blob = nullptr;
			if (FAILED(hr)) nearestBasePs = nullptr;
		}
		if (!nearestBasePs)
		{
			Log(LOG_WARNING) << "[RC12 ARCH1 GPU] direct logical-base shader unavailable; keeping validated physical-base D3D11 fallback.";
		}
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
		if (!compileShader(spriteSource.c_str(), "PS_LOCAL_LIGHT", "ps_4_0", &blob)) { shutdownDevice(); return false; }
		hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &localLightPs);
		blob->Release(); blob = nullptr;
		if (FAILED(hr) || !localLightPs) { shutdownDevice(); return false; }
		if (!compileShader(spriteSource.c_str(), "PS_SMOKE", "ps_4_0", &blob)) { shutdownDevice(); return false; }
		hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &smokePs);
		blob->Release(); blob = nullptr;
		if (FAILED(hr) || !smokePs) { shutdownDevice(); return false; }
		if (!compileShader(worldCoverageShader, "VS_COVERAGE", "vs_4_0", &blob)) { shutdownDevice(); return false; }
		hr = device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &worldCoverageVs);
		if (FAILED(hr) || !worldCoverageVs) { blob->Release(); blob = nullptr; shutdownDevice(); return false; }
		D3D11_INPUT_ELEMENT_DESC worldCoverageLayout[] =
		{
			{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, (UINT)offsetof(HdGpuWorldCoverageVertex, logicalX), D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32_FLOAT, 0, (UINT)offsetof(HdGpuWorldCoverageVertex, viewDepth), D3D11_INPUT_PER_VERTEX_DATA, 0 }
		};
		hr = device->CreateInputLayout(worldCoverageLayout, (UINT)(sizeof(worldCoverageLayout)/sizeof(worldCoverageLayout[0])),
			blob->GetBufferPointer(), blob->GetBufferSize(), &worldCoverageInputLayout);
		blob->Release(); blob = nullptr;
		if (FAILED(hr) || !worldCoverageInputLayout) { shutdownDevice(); return false; }

		if (!compileShader(bedrockSource.c_str(), "VS_BEDROCK", "vs_4_0", &blob)) { shutdownDevice(); return false; }
		hr = device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &bedrockVs);
		if (FAILED(hr) || !bedrockVs) { blob->Release(); blob = nullptr; shutdownDevice(); return false; }
		D3D11_INPUT_ELEMENT_DESC bedrockLayout[] =
		{
			{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, (UINT)offsetof(HdGpuBedrockVertex, logicalX), D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, (UINT)offsetof(HdGpuBedrockVertex, worldU), D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 3, DXGI_FORMAT_R32G32_FLOAT, 0, (UINT)offsetof(HdGpuBedrockVertex, craterU), D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 0, (UINT)offsetof(HdGpuBedrockVertex, light), D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 2, DXGI_FORMAT_R32_FLOAT, 0, (UINT)offsetof(HdGpuBedrockVertex, materialRole), D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 4, DXGI_FORMAT_R32_FLOAT, 0, (UINT)offsetof(HdGpuBedrockVertex, viewDepth), D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 5, DXGI_FORMAT_R32G32B32_FLOAT, 0, (UINT)offsetof(HdGpuBedrockVertex, lightTintR), D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 6, DXGI_FORMAT_R32G32B32_FLOAT, 0, (UINT)offsetof(HdGpuBedrockVertex, fogX), D3D11_INPUT_PER_VERTEX_DATA, 0 },
		};
		hr = device->CreateInputLayout(bedrockLayout, (UINT)(sizeof(bedrockLayout)/sizeof(bedrockLayout[0])),
			blob->GetBufferPointer(), blob->GetBufferSize(), &bedrockInputLayout);
		blob->Release(); blob = nullptr;
		if (FAILED(hr) || !bedrockInputLayout) { shutdownDevice(); return false; }
		if (!compileShader(bedrockSource.c_str(), "PS_BEDROCK", "ps_4_0", &blob)) { shutdownDevice(); return false; }
		hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &bedrockPs);
		blob->Release(); blob = nullptr;
		if (FAILED(hr) || !bedrockPs) { shutdownDevice(); return false; }

		// GEOSCAPE HD PROOF OF LIFE V1: POSITION + NORMAL + COLOR_0 mesh path.
		if (!compileShader(geoscapeShader, "VS_GLOBE", "vs_4_0", &blob)) { shutdownDevice(); return false; }
		hr = device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &geoscapeVs);
		if (FAILED(hr) || !geoscapeVs) { blob->Release(); blob = nullptr; shutdownDevice(); return false; }
		D3D11_INPUT_ELEMENT_DESC globeLayout[] =
		{
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 }
		};
		hr = device->CreateInputLayout(globeLayout, (UINT)(sizeof(globeLayout)/sizeof(globeLayout[0])),
			blob->GetBufferPointer(), blob->GetBufferSize(), &geoscapeInputLayout);
		blob->Release(); blob = nullptr;
		if (FAILED(hr) || !geoscapeInputLayout) { shutdownDevice(); return false; }
		if (!compileShader(geoscapeShader, "PS_GLOBE", "ps_4_0", &blob)) { shutdownDevice(); return false; }
		hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &geoscapePs);
		blob->Release(); blob = nullptr;
		if (FAILED(hr) || !geoscapePs) { shutdownDevice(); return false; }

		D3D11_BUFFER_DESC globeCbDesc = {};
		globeCbDesc.ByteWidth = 32;
		globeCbDesc.Usage = D3D11_USAGE_DYNAMIC;
		globeCbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		globeCbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		hr = device->CreateBuffer(&globeCbDesc, nullptr, &geoscapeConstants);
		if (FAILED(hr) || !geoscapeConstants) { shutdownDevice(); return false; }

		D3D11_TEXTURE2D_DESC depthDesc = {};
		depthDesc.Width = (UINT)base->w;
		depthDesc.Height = (UINT)base->h;
		depthDesc.MipLevels = 1;
		depthDesc.ArraySize = 1;
		depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
		depthDesc.SampleDesc.Count = 1;
		depthDesc.Usage = D3D11_USAGE_DEFAULT;
		depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
		hr = device->CreateTexture2D(&depthDesc, nullptr, &geoscapeDepth);
		if (FAILED(hr) || !geoscapeDepth) { shutdownDevice(); return false; }
		hr = device->CreateDepthStencilView(geoscapeDepth, nullptr, &geoscapeDsv);
		if (FAILED(hr) || !geoscapeDsv) { shutdownDevice(); return false; }
		D3D11_DEPTH_STENCIL_DESC depthStateDesc = {};
		depthStateDesc.DepthEnable = TRUE;
		depthStateDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		depthStateDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
		depthStateDesc.StencilEnable = FALSE;
		hr = device->CreateDepthStencilState(&depthStateDesc, &geoscapeDepthState);
		if (FAILED(hr) || !geoscapeDepthState) { shutdownDevice(); return false; }
		Log(LOG_INFO) << "[GEOSCAPE HD PROOF OF LIFE V1][GPU] mesh shader/depth path ready";
		if (!compileShader(visibilityShader, "VS_VISIBILITY", "vs_4_0", &blob)) { shutdownDevice(); return false; }
		hr = device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &visibilityVs);
		if (FAILED(hr) || !visibilityVs) { blob->Release(); blob = nullptr; shutdownDevice(); return false; }
		D3D11_INPUT_ELEMENT_DESC visibilityLayout[] =
		{
			{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, (UINT)offsetof(HdGpuVisibilityVertex, x), D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32_FLOAT, 0, (UINT)offsetof(HdGpuVisibilityVertex, alpha), D3D11_INPUT_PER_VERTEX_DATA, 0 }
		};
		hr = device->CreateInputLayout(visibilityLayout, (UINT)(sizeof(visibilityLayout)/sizeof(visibilityLayout[0])),
			blob->GetBufferPointer(), blob->GetBufferSize(), &visibilityInputLayout);
		blob->Release(); blob = nullptr;
		if (FAILED(hr) || !visibilityInputLayout) { shutdownDevice(); return false; }
		if (!compileShader(visibilityShader, "PS_VISIBILITY", "ps_4_0", &blob)) { shutdownDevice(); return false; }
		hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &visibilityPs);
		blob->Release(); blob = nullptr;
		if (FAILED(hr) || !visibilityPs) { shutdownDevice(); return false; }
		if (!compileShader(visibilityShader, "PS_VISION_COVERAGE", "ps_4_0", &blob)) { shutdownDevice(); return false; }
		hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &visionCoveragePs);
		blob->Release(); blob = nullptr;
		if (FAILED(hr) || !visionCoveragePs) { shutdownDevice(); return false; }
		if (!compileShader(cursorShader, "VS_CURSOR", "vs_4_0", &blob)) { shutdownDevice(); return false; }
		hr = device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &cursorVs);
		if (FAILED(hr) || !cursorVs) { blob->Release(); blob = nullptr; shutdownDevice(); return false; }
		D3D11_INPUT_ELEMENT_DESC cursorLayout[] =
		{
			{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, (UINT)offsetof(HdGpuCursorVertex, x), D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32_FLOAT, 0, (UINT)offsetof(HdGpuCursorVertex, viewDepth), D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 0, (UINT)offsetof(HdGpuCursorVertex, alpha), D3D11_INPUT_PER_VERTEX_DATA, 0 }
		};
		hr = device->CreateInputLayout(cursorLayout, (UINT)(sizeof(cursorLayout)/sizeof(cursorLayout[0])),
			blob->GetBufferPointer(), blob->GetBufferSize(), &cursorInputLayout);
		blob->Release(); blob = nullptr;
		if (FAILED(hr) || !cursorInputLayout) { shutdownDevice(); return false; }
		if (!compileShader(cursorShader, "PS_CURSOR", "ps_4_0", &blob)) { shutdownDevice(); return false; }
		hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &cursorPs);
		blob->Release(); blob = nullptr;
		if (FAILED(hr) || !cursorPs) { shutdownDevice(); return false; }
		if (!compileShader(spriteSource.c_str(), "PS_VIS_TAKEOVER", "ps_4_0", &blob)) { shutdownDevice(); return false; }
		hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &visibilityTakeoverPs);
		blob->Release(); blob = nullptr;
		if (FAILED(hr) || !visibilityTakeoverPs) { shutdownDevice(); return false; }
		Log(LOG_INFO) << "[REAL HD FOV V1][FALLBACK GPU] former V3 hard screen-mask shader compiled; active path=SOURCE_OWNED_MULTI_Z";
		if (!compileShader(spriteSource.c_str(), "PS_MAP_LEGACY_OVERLAY", "ps_4_0", &blob)) { shutdownDevice(); return false; }
		hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &mapLegacyOverlayPs);
		blob->Release(); blob = nullptr;
		if (FAILED(hr) || !mapLegacyOverlayPs) { shutdownDevice(); return false; }
		// Presentation Pipeline V1 is deliberately additive too.  A driver/compiler
		// that rejects these new generic presentation shaders must keep ARCH1 and the
		// validated map renderer alive; affected surfaces simply fall back to the
		// existing CPU physical-overlay path.
		if (compileShader(spriteSource.c_str(), "PS_PRESENT_RGBA", "ps_4_0", &blob))
		{
			hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &presentationRgbaPs);
			blob->Release(); blob = nullptr;
			if (FAILED(hr)) presentationRgbaPs = nullptr;
		}
		if (compileShader(spriteSource.c_str(), "PS_PRESENT_INDEXED", "ps_4_0", &blob))
		{
			hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &presentationIndexedPs);
			blob->Release(); blob = nullptr;
			if (FAILED(hr)) presentationIndexedPs = nullptr;
		}
		if (!presentationRgbaPs || !presentationIndexedPs)
		{
			if (presentationRgbaPs) { presentationRgbaPs->Release(); presentationRgbaPs = nullptr; }
			if (presentationIndexedPs) { presentationIndexedPs->Release(); presentationIndexedPs = nullptr; }
			Log(LOG_WARNING) << "[RC12 PRESENT1 GPU] generic presentation shaders unavailable; keeping ARCH1 + CPU overlay fallback.";
		}

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
		D3D11_SAMPLER_DESC sdWrap = sdLinear;
		sdWrap.AddressU = sdWrap.AddressV = sdWrap.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		hr = device->CreateSamplerState(&sdWrap, &linearWrapSampler);
		if (FAILED(hr) || !linearWrapSampler) { shutdownDevice(); return false; }

		D3D11_SAMPLER_DESC sdBedrock = sdWrap;
		sdBedrock.Filter = D3D11_FILTER_ANISOTROPIC;
		sdBedrock.MaxAnisotropy = 8;
		hr = device->CreateSamplerState(&sdBedrock, &bedrockAnisoWrapSampler);
		if (FAILED(hr) || !bedrockAnisoWrapSampler)
		{
			bedrockAnisoWrapSampler = nullptr;
			Log(LOG_WARNING) << "[BEDROCK MATERIAL SAMPLING V1] anisotropic sampler unavailable; falling back to trilinear wrap.";
		}
		else
		{
			Log(LOG_INFO) << "[BEDROCK MATERIAL SAMPLING V1] sampler=anisotropic maxAnisotropy=8 lodBias=0";
		}

		// REAL HD MATERIAL PERF V1. Large authored albedo maps are preserved at
		// full source resolution and with their complete mip chain, but they no
		// longer force x8 anisotropic colour sampling. The colour-only sampler
		// halves max anisotropy and applies a small positive LOD bias; normal,
		// roughness and AO still use x8/bias0 through bedrockAnisoWrapSampler.
		D3D11_SAMPLER_DESC sdLargeColor = sdBedrock;
		sdLargeColor.MaxAnisotropy = 4;
		sdLargeColor.MipLODBias = 0.50f;
		hr = device->CreateSamplerState(&sdLargeColor, &bedrockLargeColorWrapSampler);
		if (FAILED(hr) || !bedrockLargeColorWrapSampler)
		{
			bedrockLargeColorWrapSampler = nullptr;
			Log(LOG_WARNING) << "[REAL HD MATERIAL PERF V1] large-colour sampler unavailable; using validated x8 sampler";
		}
		else
		{
			Log(LOG_INFO) << "[REAL HD MATERIAL PERF V1] largeColourThreshold=4096 sampler=anisotropic4 lodBias=0.5 pbrSampler=anisotropic8 lodBiasPbr=0";
		}

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
		bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
		hr = device->CreateBlendState(&bd, &premultipliedBlend);
		if (FAILED(hr) || !premultipliedBlend) { shutdownDevice(); return false; }

		D3D11_BLEND_DESC addBd = {};
		addBd.RenderTarget[0].BlendEnable = TRUE;
		// REAL HD local-light ceiling: overlapping lights take the strongest
		// contribution per channel. A fully lit pixel cannot get brighter from
		// another emitter; a coloured source can still change its hue.
		addBd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
		addBd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
		addBd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_MAX;
		addBd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
		addBd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
		addBd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		addBd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
		hr = device->CreateBlendState(&addBd, &additiveBlend);
		if (FAILED(hr) || !additiveBlend) { shutdownDevice(); return false; }
		Log(LOG_INFO) << "[REAL HD LOCAL LIGHT] accumulation=max-per-channel gain=1-exp(-energy)";

		D3D11_RASTERIZER_DESC rd = {};
		rd.FillMode = D3D11_FILL_SOLID;
		rd.CullMode = D3D11_CULL_NONE;
		rd.DepthClipEnable = TRUE;
		hr = device->CreateRasterizerState(&rd, &rasterNormal);
		if (FAILED(hr) || !rasterNormal) { shutdownDevice(); return false; }
		rd.ScissorEnable = TRUE;
		hr = device->CreateRasterizerState(&rd, &rasterScissor);
		if (FAILED(hr) || !rasterScissor) { shutdownDevice(); return false; }

		if (nearestBasePs)
		{
			D3D11_BUFFER_DESC baseCb = {};
			baseCb.ByteWidth = 32; // uint4 destination + uint4 source metadata.
			baseCb.Usage = D3D11_USAGE_DYNAMIC;
			baseCb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			baseCb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			hr = device->CreateBuffer(&baseCb, nullptr, &baseConstants);
			if (FAILED(hr) || !baseConstants)
			{
				if (nearestBasePs) { nearestBasePs->Release(); nearestBasePs = nullptr; }
				Log(LOG_WARNING) << "[RC12 ARCH1 GPU] direct logical-base constants unavailable; keeping validated physical-base D3D11 fallback.";
			}
		}

		D3D11_BUFFER_DESC cb = {};
		cb.ByteWidth = 2048; // 16-byte aligned; includes BEDROCK local-light arrays in HD LOCAL LIGHTS V1.
		cb.Usage = D3D11_USAGE_DYNAMIC;
		cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		hr = device->CreateBuffer(&cb, nullptr, &roofCausticConstants);
        if (FAILED(hr) || !roofCausticConstants) { shutdownDevice(); return false; }
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
		const bool packedBgra =
			surface->format->Rmask == 0x00ff0000u &&
			surface->format->Gmask == 0x0000ff00u &&
			surface->format->Bmask == 0x000000ffu;
		if (packedBgra)
		{
			const size_t bytes = (size_t)surface->w * 4u;
			for (int y = 0; y < surface->h; ++y)
				std::memcpy(dst + (size_t)y * mapped.RowPitch, src + (size_t)y * surface->pitch, bytes);
		}
		else
		{
			// Logical OXCE surfaces normally use BGRA-compatible masks already.
			// Keep the architecture correct for unusual SDL formats instead of
			// silently swapping red/blue when the direct GPU base path is active.
			for (int y = 0; y < surface->h; ++y)
			{
				const Uint32 *srcRow = reinterpret_cast<const Uint32*>(src + (size_t)y * surface->pitch);
				Uint8 *dstRow = dst + (size_t)y * mapped.RowPitch;
				for (int x = 0; x < surface->w; ++x)
				{
					Uint8 r = 0, g = 0, b = 0, a = 255;
					SDL_GetRGBA(srcRow[x], surface->format, &r, &g, &b, &a);
					Uint8 *d = dstRow + (size_t)x * 4u;
					d[0] = b; d[1] = g; d[2] = r; d[3] = surface->format->Amask ? a : 255;
				}
			}
		}
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

	void setViewportRect(int x, int y, int w, int h)
	{
		D3D11_VIEWPORT vp = {};
		vp.TopLeftX = (FLOAT)x; vp.TopLeftY = (FLOAT)y;
		vp.Width = (FLOAT)std::max(1, w); vp.Height = (FLOAT)std::max(1, h);
		vp.MinDepth = 0; vp.MaxDepth = 1;
		context->RSSetViewports(1, &vp);
	}

	bool drawNearestBase(ID3D11ShaderResourceView *srv, int srcW, int srcH, int x, int y, int w, int h)
	{
		if (!srv || !baseConstants || !nearestBasePs || srcW <= 0 || srcH <= 0 || w <= 0 || h <= 0) return false;
		struct alignas(16) BaseConstants
		{
			uint32_t dstRect[4];
			uint32_t srcInfo[4];
		};
		BaseConstants c = {};
		c.dstRect[0] = (uint32_t)std::max(0, x);
		c.dstRect[1] = (uint32_t)std::max(0, y);
		c.dstRect[2] = (uint32_t)std::max(1, w);
		c.dstRect[3] = (uint32_t)std::max(1, h);
		c.srcInfo[0] = (uint32_t)srcW;
		c.srcInfo[1] = (uint32_t)srcH;

		D3D11_MAPPED_SUBRESOURCE mapped = {};
		HRESULT hr = context->Map(baseConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
		if (FAILED(hr)) return false;
		std::memcpy(mapped.pData, &c, sizeof(c));
		context->Unmap(baseConstants, 0);

		setViewportRect(x, y, w, h);
		context->RSSetState(rasterNormal);
		context->OMSetRenderTargets(1, &rtv, nullptr);
		context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
		context->IASetInputLayout(nullptr);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->VSSetShader(fullscreenVs, nullptr, 0);
		context->PSSetShader(nearestBasePs, nullptr, 0);
		context->PSSetConstantBuffers(0, 1, &baseConstants);
		context->PSSetShaderResources(0, 1, &srv);
		context->Draw(3, 0);
		ID3D11ShaderResourceView *nullSrv = nullptr;
		context->PSSetShaderResources(0, 1, &nullSrv);
		setFullViewport();
		return true;
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

	bool ensureBedrockVertexBuffer(unsigned vertexCount)
	{
		if (!vertexCount) return false;
		if (bedrockVertexBuffer && bedrockVertexCapacity >= vertexCount) return true;
		if (bedrockVertexBuffer) { bedrockVertexBuffer->Release(); bedrockVertexBuffer = nullptr; }
		bedrockVertexCapacity = 0;
		uploadedBedrockVertexRevision = 0;
		uploadedBedrockVertexCount = 0;
		unsigned capacity = 1024;
		while (capacity < vertexCount) capacity *= 2;
		D3D11_BUFFER_DESC bd = {};
		bd.ByteWidth = capacity * (UINT)sizeof(HdGpuBedrockVertex);
		bd.Usage = D3D11_USAGE_DYNAMIC;
		bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (FAILED(device->CreateBuffer(&bd, nullptr, &bedrockVertexBuffer)) || !bedrockVertexBuffer) return false;
		bedrockVertexCapacity = capacity;
		return true;
	}

	bool ensureWorldCoverageVertexBuffer(unsigned vertexCount)
	{
		if (!vertexCount) return false;
		if (worldCoverageVertexBuffer && worldCoverageVertexCapacity >= vertexCount) return true;
		if (worldCoverageVertexBuffer) { worldCoverageVertexBuffer->Release(); worldCoverageVertexBuffer = nullptr; }
		worldCoverageVertexCapacity = 0;
		unsigned capacity = 2048;
		while (capacity < vertexCount) capacity *= 2;
		D3D11_BUFFER_DESC bd = {};
		bd.ByteWidth = capacity * (UINT)sizeof(HdGpuWorldCoverageVertex);
		bd.Usage = D3D11_USAGE_DYNAMIC;
		bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (FAILED(device->CreateBuffer(&bd, nullptr, &worldCoverageVertexBuffer)) || !worldCoverageVertexBuffer) return false;
		worldCoverageVertexCapacity = capacity;
		return true;
	}

	bool ensureVisibilityStateTexture(unsigned w, unsigned h)
	{
		if (!w || !h) return false;
		if (visibilityStateTexture && visibilityStateSrv && visibilityStateW == (int)w && visibilityStateH == (int)h) return true;
		releaseTexture(visibilityStateTexture, visibilityStateSrv);
		visibilityStateW = visibilityStateH = 0;
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8_UINT; td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DYNAMIC; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		HRESULT hr = device->CreateTexture2D(&td, nullptr, &visibilityStateTexture);
		if (FAILED(hr) || !visibilityStateTexture) return false;
		D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
		srvd.Format = td.Format; srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; srvd.Texture2D.MipLevels = 1;
		hr = device->CreateShaderResourceView(visibilityStateTexture, &srvd, &visibilityStateSrv);
		if (FAILED(hr) || !visibilityStateSrv) { releaseTexture(visibilityStateTexture, visibilityStateSrv); return false; }
		visibilityStateW = (int)w; visibilityStateH = (int)h;
		return true;
	}

	bool uploadVisibilityState(const unsigned char *data, unsigned w, unsigned h)
	{
		if (!data || !ensureVisibilityStateTexture(w, h)) return false;
		D3D11_MAPPED_SUBRESOURCE mapped = {};
		HRESULT hr = context->Map(visibilityStateTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
		if (FAILED(hr)) return false;
		for (unsigned y = 0; y < h; ++y)
			std::memcpy(static_cast<unsigned char*>(mapped.pData) + (size_t)y * mapped.RowPitch, data + (size_t)y * w, w);
		context->Unmap(visibilityStateTexture, 0);
		return true;
	}

	bool ensureVisionBuffers()
	{
		if (!device || width <= 0 || height <= 0) return false;
		if (visionW == width && visionH == height && visionMaskTexture &&
			visionMaskRtv && visionMaskSrv && visionSceneCopy && visionSceneSrv) return true;
		if (visionMaskRtv) { visionMaskRtv->Release(); visionMaskRtv = nullptr; }
		if (visionMaskSrv) { visionMaskSrv->Release(); visionMaskSrv = nullptr; }
		if (visionMaskTexture) { visionMaskTexture->Release(); visionMaskTexture = nullptr; }
		if (visionSceneSrv) { visionSceneSrv->Release(); visionSceneSrv = nullptr; }
		if (visionSceneCopy) { visionSceneCopy->Release(); visionSceneCopy = nullptr; }
		visionW = visionH = 0;

		D3D11_TEXTURE2D_DESC maskDesc = {};
		maskDesc.Width = (UINT)width; maskDesc.Height = (UINT)height;
		maskDesc.MipLevels = 1; maskDesc.ArraySize = 1;
		maskDesc.Format = DXGI_FORMAT_R8_UNORM; maskDesc.SampleDesc.Count = 1;
		maskDesc.Usage = D3D11_USAGE_DEFAULT;
		maskDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		if (FAILED(device->CreateTexture2D(&maskDesc, nullptr, &visionMaskTexture)) ||
			FAILED(device->CreateRenderTargetView(visionMaskTexture, nullptr, &visionMaskRtv)) ||
			FAILED(device->CreateShaderResourceView(visionMaskTexture, nullptr, &visionMaskSrv))) return false;

		ID3D11Texture2D *back = nullptr;
		if (!swap || FAILED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back) return false;
		D3D11_TEXTURE2D_DESC sceneDesc = {};
		back->GetDesc(&sceneDesc);
		back->Release();
		sceneDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		sceneDesc.CPUAccessFlags = 0;
		sceneDesc.MiscFlags = 0;
		sceneDesc.Usage = D3D11_USAGE_DEFAULT;
		if (FAILED(device->CreateTexture2D(&sceneDesc, nullptr, &visionSceneCopy)) ||
			FAILED(device->CreateShaderResourceView(visionSceneCopy, nullptr, &visionSceneSrv))) return false;
		visionW = width; visionH = height;
		return true;
	}

	bool ensureWorldDepth()
	{
		if (!device || width <= 0 || height <= 0) return false;
		if (worldDepth && worldDsv && worldReadOnlyDsv && worldDepthSrv && worldDepthWriteState && worldDepthReadState
			&& worldDepthW == width && worldDepthH == height) return true;

		if (worldDepthReadState) { worldDepthReadState->Release(); worldDepthReadState = nullptr; }
		if (worldDepthWriteState) { worldDepthWriteState->Release(); worldDepthWriteState = nullptr; }
		if (worldDepthSrv) { worldDepthSrv->Release(); worldDepthSrv = nullptr; }
		if (worldReadOnlyDsv) { worldReadOnlyDsv->Release(); worldReadOnlyDsv = nullptr; }
		if (worldDsv) { worldDsv->Release(); worldDsv = nullptr; }
		if (worldDepth) { worldDepth->Release(); worldDepth = nullptr; }
		worldDepthW = worldDepthH = 0;

		D3D11_TEXTURE2D_DESC td = {};
		td.Width = (UINT)width; td.Height = (UINT)height; td.MipLevels = 1; td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R32_TYPELESS; td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
		HRESULT hr = device->CreateTexture2D(&td, nullptr, &worldDepth);
		if (FAILED(hr) || !worldDepth) return false;
		worldDepthW = width; worldDepthH = height;

		D3D11_DEPTH_STENCIL_VIEW_DESC dsvd = {};
		dsvd.Format = DXGI_FORMAT_D32_FLOAT;
		dsvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
		hr = device->CreateDepthStencilView(worldDepth, &dsvd, &worldDsv);
		if (FAILED(hr) || !worldDsv) return false;
		dsvd.Flags = D3D11_DSV_READ_ONLY_DEPTH;
		hr = device->CreateDepthStencilView(worldDepth, &dsvd, &worldReadOnlyDsv);
		if (FAILED(hr) || !worldReadOnlyDsv) return false;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
		srvd.Format = DXGI_FORMAT_R32_FLOAT;
		srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvd.Texture2D.MipLevels = 1;
		hr = device->CreateShaderResourceView(worldDepth, &srvd, &worldDepthSrv);
		if (FAILED(hr) || !worldDepthSrv) return false;

		D3D11_DEPTH_STENCIL_DESC dsd = {};
		dsd.DepthEnable = TRUE;
		dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		dsd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
		dsd.StencilEnable = FALSE;
		hr = device->CreateDepthStencilState(&dsd, &worldDepthWriteState);
		if (FAILED(hr) || !worldDepthWriteState) return false;
		dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		hr = device->CreateDepthStencilState(&dsd, &worldDepthReadState);
		return SUCCEEDED(hr) && worldDepthReadState;
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

	bool ensureBedrockCraterTexture(unsigned w, unsigned h)
	{
		if (!w || !h) return false;
		if (bedrockCraterTexture && bedrockCraterSrv && bedrockCraterW == (int)w && bedrockCraterH == (int)h) return true;
		releaseTexture(bedrockCraterTexture, bedrockCraterSrv);
		bedrockCraterW = bedrockCraterH = 0;
		bedrockCraterUploadedRevision = ~0ULL;
		releaseTexture(bedrockWeaponTexture, bedrockWeaponSrv);
		bedrockWeaponW = bedrockWeaponH = 0;
		bedrockWeaponUploadedRevision = ~0ULL;
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DYNAMIC; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		HRESULT hr = device->CreateTexture2D(&td, nullptr, &bedrockCraterTexture);
		if (FAILED(hr) || !bedrockCraterTexture) return false;
		D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
		srvd.Format = td.Format; srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; srvd.Texture2D.MipLevels = 1;
		hr = device->CreateShaderResourceView(bedrockCraterTexture, &srvd, &bedrockCraterSrv);
		if (FAILED(hr) || !bedrockCraterSrv)
		{
			releaseTexture(bedrockCraterTexture, bedrockCraterSrv);
			return false;
		}
		bedrockCraterW = (int)w; bedrockCraterH = (int)h;
		return true;
	}

	bool uploadBedrockCrater(const unsigned char *data, unsigned w, unsigned h, uint64_t revision)
	{
		if (!data || !ensureBedrockCraterTexture(w, h)) return false;
		if (bedrockCraterUploadedRevision == revision) return true;
		D3D11_MAPPED_SUBRESOURCE mapped = {};
		HRESULT hr = context->Map(bedrockCraterTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
		if (FAILED(hr)) return false;
		for (unsigned y = 0; y < h; ++y)
			std::memcpy(static_cast<unsigned char*>(mapped.pData) + (size_t)y * mapped.RowPitch, data + (size_t)y * w * 4u, (size_t)w * 4u);
		context->Unmap(bedrockCraterTexture, 0);
		bedrockCraterUploadedRevision = revision;
		return true;
	}

	bool ensureBedrockWeaponTexture(unsigned w, unsigned h)
	{
		if (!w || !h) return false;
		if (bedrockWeaponTexture && bedrockWeaponSrv && bedrockWeaponW == (int)w && bedrockWeaponH == (int)h) return true;
		releaseTexture(bedrockWeaponTexture, bedrockWeaponSrv);
		bedrockWeaponW = bedrockWeaponH = 0;
		bedrockWeaponUploadedRevision = ~0ULL;
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DYNAMIC; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		HRESULT hr = device->CreateTexture2D(&td, nullptr, &bedrockWeaponTexture);
		if (FAILED(hr) || !bedrockWeaponTexture) return false;
		D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
		srvd.Format = td.Format; srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; srvd.Texture2D.MipLevels = 1;
		hr = device->CreateShaderResourceView(bedrockWeaponTexture, &srvd, &bedrockWeaponSrv);
		if (FAILED(hr) || !bedrockWeaponSrv)
		{
			releaseTexture(bedrockWeaponTexture, bedrockWeaponSrv);
			return false;
		}
		bedrockWeaponW = (int)w; bedrockWeaponH = (int)h;
		return true;
	}

	bool uploadBedrockWeapon(const unsigned char *data, unsigned w, unsigned h, uint64_t revision)
	{
		if (!data || !ensureBedrockWeaponTexture(w, h)) return false;
		if (bedrockWeaponUploadedRevision == revision) return true;
		D3D11_MAPPED_SUBRESOURCE mapped = {};
		HRESULT hr = context->Map(bedrockWeaponTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
		if (FAILED(hr)) return false;
		for (unsigned y = 0; y < h; ++y)
			std::memcpy(static_cast<unsigned char*>(mapped.pData) + (size_t)y * mapped.RowPitch, data + (size_t)y * w * 4u, (size_t)w * 4u);
		context->Unmap(bedrockWeaponTexture, 0);
		bedrockWeaponUploadedRevision = revision;
		return true;
	}

	bool ensurePaletteTexture(const SDL_Color *palette)
	{
		if (!palette) return false;
		uint64_t signature = 1469598103934665603ULL;
		for (int i = 0; i < 256; ++i)
		{
			signature ^= palette[i].r; signature *= 1099511628211ULL;
			signature ^= palette[i].g; signature *= 1099511628211ULL;
			signature ^= palette[i].b; signature *= 1099511628211ULL;
		}
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
			uploadedPaletteSignature = 0;
		}
		if (uploadedPaletteSignature == signature) return true;

		D3D11_MAPPED_SUBRESOURCE mapped = {};
		HRESULT hr = context->Map(paletteTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
		if (FAILED(hr)) return false;
		Uint8 *dst = static_cast<Uint8*>(mapped.pData);
		for (int i = 0; i < 256; ++i)
		{
			dst[i * 4 + 0] = palette[i].r;
			dst[i * 4 + 1] = palette[i].g;
			dst[i * 4 + 2] = palette[i].b;
			// Transparency belongs to the sprite's colour-key semantics, not to
			// palette entry zero itself. The Battlescape indexed shader already
			// discards index 0 explicitly; generic UI presentation may legitimately
			// use palette index 0 as an opaque colour when no colour key is set.
			dst[i * 4 + 3] = 255;
		}
		context->Unmap(paletteTexture, 0);
		uploadedPaletteSignature = signature;
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

	bool ensureOverlaySurface(int w, int h)
	{
		if (w <= 0 || h <= 0) return false;
		if (!overlaySurface || overlayW != w || overlayH != h)
		{
			if (overlaySurface) { SDL_FreeSurface(overlaySurface); overlaySurface = nullptr; }
			overlayW = w; overlayH = h;
			overlayPixels.assign((size_t)overlayW * (size_t)overlayH * 4u, 0);
			overlayDirty = false;
			overlayDirtyX0 = overlayDirtyY0 = overlayDirtyX1 = overlayDirtyY1 = 0;
			const Uint32 rmask = 0x00ff0000, gmask = 0x0000ff00, bmask = 0x000000ff, amask = 0xff000000;
			overlaySurface = SDL_CreateRGBSurfaceFrom(overlayPixels.data(), overlayW, overlayH,
				32, overlayW * 4, rmask, gmask, bmask, amask);
			if (!overlaySurface) return false;
			SDL_SetAlpha(overlaySurface, 0, 255);
		}
		// flushCpuOverlay() already clears the staging pixels after upload.
		// Only an aborted frame can leave dirty pixels behind here; clearing
		// the full display buffer unconditionally on every frame is redundant.
		if (overlayDirty)
		{
			for (int y = overlayDirtyY0; y < overlayDirtyY1; ++y)
				std::memset(overlayPixels.data() + ((size_t)y * overlayW + overlayDirtyX0) * 4u,
					0, (size_t)(overlayDirtyX1 - overlayDirtyX0) * 4u);
		}
		overlayDirty = false;
		overlayDirtyX0 = overlayDirtyY0 = overlayDirtyX1 = overlayDirtyY1 = 0;
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
		const unsigned tightPitch = sprite.imageWidth * (sprite.legacyIndexed ? 1u : 4u);
		init.SysMemPitch = sprite.sourcePitch ? sprite.sourcePitch : tightPitch;
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


	enum class MaterialMipKind
	{
		Color,
		Normal,
		Scalar
	};

	static const char *materialMipKindName(MaterialMipKind kind)
	{
		switch (kind)
		{
			case MaterialMipKind::Color: return "color";
			case MaterialMipKind::Normal: return "normal-renormalized";
			case MaterialMipKind::Scalar: return "scalar";
		}
		return "unknown";
	}

	bool ensureMaterialImage(const char *assetKey, const unsigned char *rgba, unsigned width, unsigned height,
		MaterialMipKind kind, ID3D11ShaderResourceView **outSrv, unsigned *outMipLevels = nullptr)
	{
		if (!assetKey || !rgba || !width || !height || !outSrv) return false;

		unsigned mipLevels = 1;
		for (unsigned w = width, h = height; w > 1 || h > 1; )
		{
			w = std::max(1u, w / 2u);
			h = std::max(1u, h / 2u);
			++mipLevels;
		}

		std::string cacheKey(assetKey);
		cacheKey += "#BEDROCK_MIP_";
		cacheKey += materialMipKindName(kind);
		auto it = images.find(cacheKey);
		if (it != images.end() && it->second.width == width && it->second.height == height &&
			!it->second.indexed && it->second.mipLevels == mipLevels)
		{
			*outSrv = it->second.srv;
			if (outMipLevels) *outMipLevels = it->second.mipLevels;
			return *outSrv != nullptr;
		}
		if (it != images.end())
		{
			if (it->second.srv) it->second.srv->Release();
			if (it->second.texture) it->second.texture->Release();
			images.erase(it);
		}

		std::vector<std::vector<unsigned char> > levels(mipLevels);
		levels[0].assign(rgba, rgba + (size_t)width * (size_t)height * 4u);
		unsigned srcW = width, srcH = height;
		for (unsigned level = 1; level < mipLevels; ++level)
		{
			const unsigned dstW = std::max(1u, srcW / 2u);
			const unsigned dstH = std::max(1u, srcH / 2u);
			const std::vector<unsigned char> &src = levels[level - 1];
			std::vector<unsigned char> &dst = levels[level];
			dst.resize((size_t)dstW * (size_t)dstH * 4u);

			for (unsigned y = 0; y < dstH; ++y)
			{
				for (unsigned x = 0; x < dstW; ++x)
				{
					const unsigned sx0 = std::min(srcW - 1u, x * 2u);
					const unsigned sx1 = std::min(srcW - 1u, x * 2u + 1u);
					const unsigned sy0 = std::min(srcH - 1u, y * 2u);
					const unsigned sy1 = std::min(srcH - 1u, y * 2u + 1u);
					const size_t si[4] =
					{
						((size_t)sy0 * srcW + sx0) * 4u,
						((size_t)sy0 * srcW + sx1) * 4u,
						((size_t)sy1 * srcW + sx0) * 4u,
						((size_t)sy1 * srcW + sx1) * 4u
					};
					const size_t di = ((size_t)y * dstW + x) * 4u;

					if (kind == MaterialMipKind::Normal)
					{
						float nx = 0.0f, ny = 0.0f, nz = 0.0f;
						unsigned alpha = 0;
						for (int k = 0; k < 4; ++k)
						{
							nx += (float)src[si[k] + 0] * (2.0f / 255.0f) - 1.0f;
							ny += (float)src[si[k] + 1] * (2.0f / 255.0f) - 1.0f;
							nz += (float)src[si[k] + 2] * (2.0f / 255.0f) - 1.0f;
							alpha += src[si[k] + 3];
						}
						nx *= 0.25f; ny *= 0.25f; nz *= 0.25f;
						const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
						if (len > 1.0e-6f)
						{
							nx /= len; ny /= len; nz /= len;
						}
						else
						{
							nx = 0.0f; ny = 0.0f; nz = 1.0f;
						}
						auto enc = [](float v) -> unsigned char
						{
							v = std::max(-1.0f, std::min(1.0f, v));
							return (unsigned char)std::lround((v * 0.5f + 0.5f) * 255.0f);
						};
						dst[di + 0] = enc(nx);
						dst[di + 1] = enc(ny);
						dst[di + 2] = enc(nz);
						dst[di + 3] = (unsigned char)((alpha + 2u) / 4u);
					}
					else
					{
						for (int c = 0; c < 4; ++c)
						{
							const unsigned sum = (unsigned)src[si[0] + c] + (unsigned)src[si[1] + c] +
								(unsigned)src[si[2] + c] + (unsigned)src[si[3] + c];
							dst[di + c] = (unsigned char)((sum + 2u) / 4u);
						}
					}
				}
			}
			srcW = dstW; srcH = dstH;
		}

		std::vector<D3D11_SUBRESOURCE_DATA> init(mipLevels);
		unsigned mipW = width, mipH = height;
		for (unsigned level = 0; level < mipLevels; ++level)
		{
			init[level] = {};
			init[level].pSysMem = levels[level].data();
			init[level].SysMemPitch = mipW * 4u;
			mipW = std::max(1u, mipW / 2u);
			mipH = std::max(1u, mipH / 2u);
		}

		GpuImage image;
		image.width = width; image.height = height; image.indexed = false; image.mipLevels = mipLevels;
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = width; td.Height = height; td.MipLevels = mipLevels; td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		HRESULT hr = device->CreateTexture2D(&td, init.data(), &image.texture);
		if (FAILED(hr) || !image.texture) return false;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
		srvd.Format = td.Format; srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvd.Texture2D.MostDetailedMip = 0; srvd.Texture2D.MipLevels = mipLevels;
		hr = device->CreateShaderResourceView(image.texture, &srvd, &image.srv);
		if (FAILED(hr) || !image.srv)
		{
			if (image.texture) image.texture->Release();
			return false;
		}

		auto inserted = images.emplace(cacheKey, image);
		*outSrv = inserted.first->second.srv;
		if (outMipLevels) *outMipLevels = mipLevels;
		Log(LOG_INFO) << "[BEDROCK MATERIAL SAMPLING V1] upload kind=" << materialMipKindName(kind)
			<< " size=" << width << "x" << height << " mips=" << mipLevels << " asset=" << assetKey;
		return true;
	}

	bool ensureDynamicSurface(const std::string &cacheKey, SDL_Surface *source,
		bool indexed, ID3D11ShaderResourceView **outSrv)
	{
		if (!source || !outSrv || source->w <= 0 || source->h <= 0) return false;
		if (indexed && source->format->BitsPerPixel != 8) return false;
		if (!indexed && source->format->BitsPerPixel != 32) return false;

		DynamicSurfaceImage &image = dynamicSurfaces[cacheKey];
		const bool recreate = !image.texture || !image.srv || image.width != source->w ||
			image.height != source->h || image.indexed != indexed;
		if (recreate)
		{
			if (image.srv) { image.srv->Release(); image.srv = nullptr; }
			if (image.texture) { image.texture->Release(); image.texture = nullptr; }
			D3D11_TEXTURE2D_DESC td = {};
			td.Width = (UINT)source->w; td.Height = (UINT)source->h; td.MipLevels = 1; td.ArraySize = 1;
			td.Format = indexed ? DXGI_FORMAT_R8_UINT : DXGI_FORMAT_B8G8R8A8_UNORM;
			td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_DYNAMIC; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			HRESULT hr = device->CreateTexture2D(&td, nullptr, &image.texture);
			if (FAILED(hr) || !image.texture) return false;
			D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
			sd.Format = td.Format; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			sd.Texture2D.MipLevels = 1;
			hr = device->CreateShaderResourceView(image.texture, &sd, &image.srv);
			if (FAILED(hr) || !image.srv)
			{
				if (image.texture) { image.texture->Release(); image.texture = nullptr; }
				return false;
			}
			image.width = source->w;
			image.height = source->h;
			image.indexed = indexed;
		}

		if (!indexed)
		{
			const SDL_PixelFormat *fmt = source->format;
			const bool packedBgra = fmt && fmt->BytesPerPixel == 4 &&
				fmt->Rmask == 0x00ff0000u && fmt->Gmask == 0x0000ff00u &&
				fmt->Bmask == 0x000000ffu;
			if (!packedBgra) return false;
		}

		if (SDL_MUSTLOCK(source) && SDL_LockSurface(source) != 0) return false;
		D3D11_MAPPED_SUBRESOURCE mapped = {};
		HRESULT hr = context->Map(image.texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
		if (FAILED(hr))
		{
			if (SDL_MUSTLOCK(source)) SDL_UnlockSurface(source);
			return false;
		}
		const unsigned bytesPerPixel = indexed ? 1u : 4u;
		const size_t rowBytes = (size_t)source->w * bytesPerPixel;
		const Uint8 *src = static_cast<const Uint8*>(source->pixels);
		Uint8 *dst = static_cast<Uint8*>(mapped.pData);
		for (int y = 0; y < source->h; ++y)
			std::memcpy(dst + (size_t)y * mapped.RowPitch, src + (size_t)y * source->pitch, rowBytes);
		context->Unmap(image.texture, 0);
		if (SDL_MUSTLOCK(source)) SDL_UnlockSurface(source);
		*outSrv = image.srv;
		return *outSrv != nullptr;
	}

	bool drawPresentationSrv(ID3D11ShaderResourceView *srv, bool indexed,
		const SDL_Color *palette, int srcW, int srcH,
		int destX, int destY, int destW, int destH, Uint8 opacity,
		bool hasColorKey, Uint8 colorKey, bool hasClip,
		int clipX0, int clipY0, int clipX1, int clipY1)
	{
		if (!srv || !spriteConstants || !presentationRgbaPs || !presentationIndexedPs ||
			srcW <= 0 || srcH <= 0 || destW <= 0 || destH <= 0) return false;
		if (indexed && (!palette || !ensurePaletteTexture(palette))) return false;

		struct alignas(16) SpriteConstants
		{
			float destPx[4];
			float metrics0[4];
			float metrics1[4];
			uint32_t ids0[4];
			int32_t ids1[4];
			int32_t ids2[4];
		};
		SpriteConstants c = {};
		c.destPx[0] = (float)destX; c.destPx[1] = (float)destY;
		c.destPx[2] = (float)destW; c.destPx[3] = (float)destH;
		c.metrics0[0] = (float)width; c.metrics0[1] = (float)height;
		c.metrics1[2] = (float)opacity / 255.0f;
		c.ids0[1] = (uint32_t)srcW; c.ids0[2] = (uint32_t)srcH;
		c.ids2[1] = hasColorKey ? 1 : 0;
		c.ids2[2] = (int32_t)colorKey;

		D3D11_MAPPED_SUBRESOURCE mapped = {};
		HRESULT hr = context->Map(spriteConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
		if (FAILED(hr)) return false;
		std::memcpy(mapped.pData, &c, sizeof(c));
		context->Unmap(spriteConstants, 0);

		D3D11_RECT oldRect = {0, 0, width, height};
		if (hasClip)
		{
			D3D11_RECT rect = {
				std::max(0, clipX0), std::max(0, clipY0),
				std::min(width, clipX1), std::min(height, clipY1)
			};
			if (rect.right <= rect.left || rect.bottom <= rect.top) return true;
			context->RSSetState(rasterScissor);
			context->RSSetScissorRects(1, &rect);
		}
		else
		{
			context->RSSetState(rasterNormal);
		}

		static const float blendFactor[4] = {0,0,0,0};
		setFullViewport();
		context->OMSetRenderTargets(1, &rtv, nullptr);
		context->OMSetBlendState(alphaBlend, blendFactor, 0xffffffffu);
		context->IASetInputLayout(nullptr);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		context->VSSetShader(spriteVs, nullptr, 0);
		context->VSSetConstantBuffers(0, 1, &spriteConstants);
		context->PSSetShader(indexed ? presentationIndexedPs : presentationRgbaPs, nullptr, 0);
		context->PSSetConstantBuffers(0, 1, &spriteConstants);
		ID3D11ShaderResourceView *srvs[4] = { nullptr, nullptr, nullptr, nullptr };
		if (indexed)
		{
			srvs[2] = srv;
			srvs[3] = paletteSrv;
		}
		else
		{
			srvs[0] = srv;
		}
		context->PSSetShaderResources(0, 4, srvs);
		context->Draw(4, 0);
		ID3D11ShaderResourceView *nullSrvs[4] = { nullptr, nullptr, nullptr, nullptr };
		context->PSSetShaderResources(0, 4, nullSrvs);
		context->RSSetState(rasterNormal);
		(void)oldRect;
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

bool HdGpuBackend::preparePresentationFrame(SDL_Surface *physicalTarget, SDL_Surface *logicalBase)
{
	_impl->preparedThisFrame = false;
#if defined(_WIN32)
	if (!physicalTarget || !logicalBase || physicalTarget->format->BitsPerPixel != 32 || logicalBase->format->BitsPerPixel != 32) return false;
	if (!_impl->ensureDevice(physicalTarget)) return false;
	if (!_impl->nearestBasePs || !_impl->baseConstants || !_impl->spriteVs || !_impl->indexedSpritePs) return false;
	// STRICT REAL HD: resource readiness may never depend on uploading the OXCE
	// logical canvas. It remains an input to this preflight only for dimensions.
	if (!_impl->ensureOverlaySurface(physicalTarget->w, physicalTarget->h)) return false;
	_impl->preparedThisFrame = true;
	return true;
#else
	(void)physicalTarget; (void)logicalBase;
	return false;
#endif
}

bool HdGpuBackend::directWorldReady() const
{
	return _impl->preparedThisFrame;
}

bool HdGpuBackend::beginFrameHdOnly(SDL_Surface *physicalTarget)
{
	_impl->preparedThisFrame = false;
	_impl->frameActive = false;
	_impl->lastUsed = false;
	_impl->logicalBaseFrame = false;
	_impl->mapDrawCalls = 0;
	_impl->indexedDrawCalls = 0;
	_impl->environmentDrawCalls = 0;
	_impl->presentationDrawCalls = 0;
	_impl->legacyPresentationDrawCalls = 0;
#if defined(_WIN32)
	if (!physicalTarget || !_impl->ensureDevice(physicalTarget)) return false;
	if (!_impl->ensureOverlaySurface(physicalTarget->w, physicalTarget->h)) return false;
	_impl->beginGpuTimingFrame();
	const float clear[4] = {0, 0, 0, 1};
	_impl->context->OMSetRenderTargets(1, &_impl->rtv, nullptr);
	_impl->context->ClearRenderTargetView(_impl->rtv, clear);
	_impl->setFullViewport();
	_impl->frameActive = true;
	return true;
#else
	(void)physicalTarget;
	return false;
#endif
}

bool HdGpuBackend::beginFrame(SDL_Surface *physicalBase)
{
	_impl->preparedThisFrame = false;
	_impl->frameActive = false;
	_impl->lastUsed = false;
	_impl->logicalBaseFrame = false;
	_impl->mapDrawCalls = 0;
	_impl->indexedDrawCalls = 0;
	_impl->environmentDrawCalls = 0;
	_impl->presentationDrawCalls = 0;
	_impl->legacyPresentationDrawCalls = 0;
#if defined(_WIN32)
	if (!_impl->ensureDevice(physicalBase)) return false;
	const uint64_t uploadStart = hdPerfNowUs();
	if (!_impl->ensureDynamicBgra(_impl->baseUpload, _impl->baseSrv, _impl->baseW, _impl->baseH,
		physicalBase->w, physicalBase->h)) return false;
	if (!_impl->uploadBgra(_impl->baseUpload, physicalBase)) return false;
	getHdPerfStats().current.gpuBaseUploadUs += hdPerfNowUs() - uploadStart;
	if (!_impl->ensureOverlaySurface(physicalBase->w, physicalBase->h)) return false;
	_impl->beginGpuTimingFrame();

	const uint64_t presentStart = hdPerfNowUs();
	const float clear[4] = {0,0,0,1};
	_impl->context->OMSetRenderTargets(1, &_impl->rtv, nullptr);
	_impl->context->ClearRenderTargetView(_impl->rtv, clear);
	_impl->drawFullscreen(_impl->baseSrv, false);
	getHdPerfStats().current.gpuBasePresentUs += hdPerfNowUs() - presentStart;

	_impl->frameActive = true;
	return true;
#else
	(void)physicalBase;
	return false;
#endif
}

bool HdGpuBackend::beginFrameLogical(SDL_Surface *physicalTarget, SDL_Surface *logicalBase, const PresentationContext &presentation)
{
	_impl->preparedThisFrame = false;
	_impl->frameActive = false;
	_impl->lastUsed = false;
	_impl->logicalBaseFrame = true;
	_impl->mapDrawCalls = 0;
	_impl->indexedDrawCalls = 0;
	_impl->environmentDrawCalls = 0;
	_impl->presentationDrawCalls = 0;
	_impl->legacyPresentationDrawCalls = 0;
#if defined(_WIN32)
	if (!physicalTarget || !logicalBase || logicalBase->format->BitsPerPixel != 32) return false;
	if (!_impl->ensureDevice(physicalTarget)) return false;
	if (!_impl->nearestBasePs || !_impl->baseConstants) return false;

	const uint64_t uploadStart = hdPerfNowUs();
	if (!_impl->ensureDynamicBgra(_impl->baseUpload, _impl->baseSrv, _impl->baseW, _impl->baseH,
		logicalBase->w, logicalBase->h)) return false;
	if (!_impl->uploadBgra(_impl->baseUpload, logicalBase)) return false;
	getHdPerfStats().current.gpuBaseUploadUs += hdPerfNowUs() - uploadStart;

	const PresentationRect &r = presentation.contentRect();
	const int x = std::max(0, std::min(physicalTarget->w, r.x));
	const int y = std::max(0, std::min(physicalTarget->h, r.y));
	const int w = std::max(1, std::min(physicalTarget->w - x, r.w));
	const int h = std::max(1, std::min(physicalTarget->h - y, r.h));
	if (!_impl->ensureOverlaySurface(physicalTarget->w, physicalTarget->h)) return false;
	_impl->beginGpuTimingFrame();

	const uint64_t presentStart = hdPerfNowUs();
	const float clear[4] = {0,0,0,1};
	_impl->context->OMSetRenderTargets(1, &_impl->rtv, nullptr);
	_impl->context->ClearRenderTargetView(_impl->rtv, clear);
	if (!_impl->drawNearestBase(_impl->baseSrv, logicalBase->w, logicalBase->h, x, y, w, h))
	{
		_impl->endGpuTimingFrame();
		return false;
	}
	getHdPerfStats().current.gpuBasePresentUs += hdPerfNowUs() - presentStart;
	_impl->frameActive = true;
	return true;
#else
	(void)physicalTarget; (void)logicalBase; (void)presentation;
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
	// Most HD UI rasters and the staging target use the same packed BGRA8
	// layout.  Reading those bytes directly avoids an SDL format conversion for
	// every pixel in the physical HUD, including the large diagnostic overlay.
	const bool packedBgra = bpp == 4 && sf->Rmask == 0x00ff0000u &&
		sf->Gmask == 0x0000ff00u && sf->Bmask == 0x000000ffu &&
		sf->Amask == 0xff000000u;

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
			if (packedBgra)
			{
				sb = sp[0]; sg = sp[1]; sr = sp[2]; sa = sp[3];
			}
			else
			{
				SDL_GetRGBA(raw, sf, &sr, &sg, &sb, &sa);
			}
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
	if (!_impl->overlayDirty)
	{
		_impl->overlayDirtyX0 = x0; _impl->overlayDirtyY0 = y0;
		_impl->overlayDirtyX1 = x1; _impl->overlayDirtyY1 = y1;
	}
	else
	{
		_impl->overlayDirtyX0 = std::min(_impl->overlayDirtyX0, x0);
		_impl->overlayDirtyY0 = std::min(_impl->overlayDirtyY0, y0);
		_impl->overlayDirtyX1 = std::max(_impl->overlayDirtyX1, x1);
		_impl->overlayDirtyY1 = std::max(_impl->overlayDirtyY1, y1);
	}
	_impl->overlayDirty = true;
	return true;
#else
	(void)source; (void)dstX; (void)dstY;
	return false;
#endif
}

bool HdGpuBackend::flushCpuOverlay()
{
#if defined(_WIN32)
	if (!_impl->frameActive || !_impl->overlaySurface) return false;
	if (!_impl->overlayDirty) return true;
	const int x0 = _impl->overlayDirtyX0, y0 = _impl->overlayDirtyY0;
	const int x1 = _impl->overlayDirtyX1, y1 = _impl->overlayDirtyY1;
	const int dirtyW = x1 - x0, dirtyH = y1 - y0;
	bool drawn = false;
	if (dirtyW > 0 && dirtyH > 0 &&
		(size_t)dirtyW * dirtyH * 2u < (size_t)_impl->overlayW * _impl->overlayH)
	{
		// Wrap the dirty pixels with their original row pitch. The dynamic
		// texture copies only dirtyW pixels per row, then draws at the original
		// physical position with the same straight-alpha blend as fullscreen.
		Uint8 *first = _impl->overlayPixels.data() + ((size_t)y0 * _impl->overlayW + x0) * 4u;
		SDL_Surface *crop = SDL_CreateRGBSurfaceFrom(first, dirtyW, dirtyH, 32,
			_impl->overlayW * 4, 0x00ff0000u, 0x0000ff00u, 0x000000ffu, 0xff000000u);
		if (crop)
		{
			ID3D11ShaderResourceView *srv = nullptr;
			if (_impl->ensureDynamicSurface("CPU_OVERLAY_CROP", crop, false, &srv))
				drawn = _impl->drawPresentationSrv(srv, false, nullptr,
					dirtyW, dirtyH, x0, y0, dirtyW, dirtyH, 255, false, 0, false, 0, 0, 0, 0);
			SDL_FreeSurface(crop);
		}
	}
	if (!drawn && (!_impl->ensureDynamicBgra(_impl->overlayUpload, _impl->overlaySrv, _impl->overlayTexW, _impl->overlayTexH,
		_impl->overlaySurface->w, _impl->overlaySurface->h) ||
		!_impl->uploadBgra(_impl->overlayUpload, _impl->overlaySurface)))
	{
		return false;
	}
	if (!drawn) _impl->drawFullscreen(_impl->overlaySrv, true);
	for (int y = y0; y < y1; ++y)
		std::memset(_impl->overlayPixels.data() + ((size_t)y * _impl->overlayW + x0) * 4u,
			0, (size_t)dirtyW * 4u);
	_impl->overlayDirty = false;
	_impl->overlayDirtyX0 = _impl->overlayDirtyY0 = _impl->overlayDirtyX1 = _impl->overlayDirtyY1 = 0;
	return true;
#else
	return false;
#endif
}

bool HdGpuBackend::drawDebugOverlay(const std::string &utf8)
{
#if defined(_WIN32)
	if (!flushCpuOverlay()) return false;
	return _impl->drawDebugOverlay(utf8);
#else
	(void)utf8;
	return false;
#endif
}

bool HdGpuBackend::drawPresentationSprite(const HdGpuPresentationSprite &s)
{
#if defined(_WIN32)
	if (!_impl->frameActive || s.destW <= 0 || s.destH <= 0 || !s.assetKey || !s.imageWidth || !s.imageHeight) return false;
	if (!flushCpuOverlay()) return false;

	HdGpuMapSprite key;
	key.assetKey = s.assetKey;
	key.rgba = s.rgba;
	key.indices = s.indices;
	key.legacyIndexed = s.legacyIndexed;
	key.imageWidth = s.imageWidth;
	key.imageHeight = s.imageHeight;
	ID3D11ShaderResourceView *srv = nullptr;
	if (!_impl->ensureImage(key, &srv)) return false;

	const uint64_t start = hdPerfNowUs();
	if (!_impl->drawPresentationSrv(srv, s.legacyIndexed, s.palette,
		(int)s.imageWidth, (int)s.imageHeight,
		s.destX, s.destY, s.destW, s.destH, s.opacity,
		s.hasColorKey, s.colorKey, s.hasClip,
		s.clipX0, s.clipY0, s.clipX1, s.clipY1))
	{
		return false;
	}
	getHdPerfStats().current.presentationGpuUs += hdPerfNowUs() - start;
	++getHdPerfStats().current.presentationGpuDraws;
	++_impl->presentationDrawCalls;
	return true;
#else
	(void)s;
	return false;
#endif
}

bool HdGpuBackend::drawLegacySurface(SDL_Surface *source, const void *stableKey,
	int destX, int destY, int destW, int destH, Uint8 opacity)
{
	if (Options::hdGraphics) return false; // Native OXCE pixels cannot enter REAL HD.
#if defined(_WIN32)
	if (!_impl->frameActive || !source || destW <= 0 || destH <= 0) return false;
	const bool indexed = source->format && source->format->BitsPerPixel == 8;
	const bool rgba32 = source->format && source->format->BitsPerPixel == 32;
	if (!indexed && !rgba32) return false;
	if (rgba32 && (source->flags & SDL_SRCCOLORKEY)) return false;
	if (indexed && (!source->format->palette || !source->format->palette->colors)) return false;
	if (!flushCpuOverlay()) return false;

	const bool alphaEnabled = (source->flags & SDL_SRCALPHA) != 0;
	const Uint8 sourceAlpha = alphaEnabled ? source->format->alpha : 255;
	const Uint8 effectiveOpacity = (Uint8)(((unsigned)opacity * (unsigned)sourceAlpha + 127u) / 255u);
	const bool hasColorKey = indexed && ((source->flags & SDL_SRCCOLORKEY) != 0);
	const Uint8 colorKey = hasColorKey ? (Uint8)(source->format->colorkey & 0xffu) : 0;

	const uintptr_t keyValue = reinterpret_cast<uintptr_t>(stableKey ? stableKey : source);
	std::string cacheKey = std::string("LEGACY_SURFACE:") + std::to_string((unsigned long long)keyValue) +
		(indexed ? ":IDX" : ":BGRA");
	ID3D11ShaderResourceView *srv = nullptr;
	const uint64_t start = hdPerfNowUs();
	if (!_impl->ensureDynamicSurface(cacheKey, source, indexed, &srv)) return false;
	const SDL_Color *palette = indexed ? source->format->palette->colors : nullptr;
	if (!_impl->drawPresentationSrv(srv, indexed, palette,
		source->w, source->h, destX, destY, destW, destH, effectiveOpacity,
		hasColorKey, colorKey, false, 0, 0, 0, 0))
	{
		return false;
	}
	const uint64_t elapsed = hdPerfNowUs() - start;
	getHdPerfStats().current.presentationGpuUs += elapsed;
	getHdPerfStats().current.presentationLegacyGpuUs += elapsed;
	++getHdPerfStats().current.presentationGpuDraws;
	++getHdPerfStats().current.presentationLegacyGpuDraws;
	++_impl->presentationDrawCalls;
	++_impl->legacyPresentationDrawCalls;
	return true;
#else
	(void)source; (void)stableKey; (void)destX; (void)destY; (void)destW; (void)destH; (void)opacity;
	return false;
#endif
}


bool HdGpuBackend::drawGeoscapeGlobeProof(const char *assetKey, int destX, int destY, int destW, int destH,
	float centerLon, float centerLat, float cameraDistance, float fovYDegrees)
{
#if defined(_WIN32)
	if (!_impl->frameActive || !assetKey || destW <= 0 || destH <= 0 ||
		!_impl->geoscapeVs || !_impl->geoscapePs || !_impl->geoscapeInputLayout ||
		!_impl->geoscapeConstants || !_impl->geoscapeDsv || !_impl->geoscapeDepthState)
	{
		return false;
	}
	if (!flushCpuOverlay()) return false;

	if (_impl->geoscapeMeshKey != assetKey || !_impl->geoscapeVertexBuffer || !_impl->geoscapeIndexBuffer)
	{
		if (_impl->geoscapeVertexBuffer) { _impl->geoscapeVertexBuffer->Release(); _impl->geoscapeVertexBuffer = nullptr; }
		if (_impl->geoscapeIndexBuffer) { _impl->geoscapeIndexBuffer->Release(); _impl->geoscapeIndexBuffer = nullptr; }
		_impl->geoscapeMeshKey.clear();
		_impl->geoscapeVertexCount = _impl->geoscapeIndexCount = 0;

		auto in = FileMap::getIStream(assetKey);
		if (!in)
		{
			Log(LOG_WARNING) << "[GEOSCAPE HD PROOF OF LIFE V1] mesh provider missing: " << assetKey;
			return false;
		}

		char magic[4] = {};
		uint32_t version = 0, vertexCount = 0, indexCount = 0, stride = 0;
		float sourceMeta[4] = {};
		in->read(magic, 4);
		in->read(reinterpret_cast<char*>(&version), sizeof(version));
		in->read(reinterpret_cast<char*>(&vertexCount), sizeof(vertexCount));
		in->read(reinterpret_cast<char*>(&indexCount), sizeof(indexCount));
		in->read(reinterpret_cast<char*>(&stride), sizeof(stride));
		in->read(reinterpret_cast<char*>(sourceMeta), sizeof(sourceMeta));
		if (!*in || std::memcmp(magic, "HDGM", 4) != 0 || version != 1 || stride != 28 ||
			vertexCount == 0 || indexCount == 0)
		{
			Log(LOG_ERROR) << "[GEOSCAPE HD PROOF OF LIFE V1] invalid HDGM mesh header asset=" << assetKey;
			return false;
		}

		std::vector<unsigned char> vertexBytes((size_t)vertexCount * stride);
		std::vector<uint32_t> indices(indexCount);
		in->read(reinterpret_cast<char*>(vertexBytes.data()), (std::streamsize)vertexBytes.size());
		in->read(reinterpret_cast<char*>(indices.data()), (std::streamsize)(indices.size() * sizeof(uint32_t)));
		if (!*in)
		{
			Log(LOG_ERROR) << "[GEOSCAPE HD PROOF OF LIFE V1] truncated mesh asset=" << assetKey;
			return false;
		}

		// GEOSCAPE HD MESH HYGIENE V1
		// The source GLB is split into several authored pieces.  A small subset of
		// transformed primitives arrives with mirrored winding/normals.  With the
		// proof shader this shows up as tiny dark/flickering facets.  The Earth is
		// a closed radial surface, so sanitize the derived runtime mesh without
		// changing the source GLB: normals point away from the origin and triangle
		// winding follows the same outward convention.
		struct GlobeMeshVertex
		{
			float p[3];
			float n[3];
			Uint8 color[4];
		};
		static_assert(sizeof(GlobeMeshVertex) == 28, "HDGM vertex layout changed");
		GlobeMeshVertex *gv = reinterpret_cast<GlobeMeshVertex*>(vertexBytes.data());
		unsigned flippedNormals = 0;
		for (uint32_t i = 0; i < vertexCount; ++i)
		{
			const double radialDot = (double)gv[i].p[0] * gv[i].n[0] +
				(double)gv[i].p[1] * gv[i].n[1] +
				(double)gv[i].p[2] * gv[i].n[2];
			if (radialDot < 0.0)
			{
				gv[i].n[0] = -gv[i].n[0];
				gv[i].n[1] = -gv[i].n[1];
				gv[i].n[2] = -gv[i].n[2];
				++flippedNormals;
			}
		}

		unsigned flippedTriangles = 0;
		bool invalidIndex = false;
		for (uint32_t i = 0; i + 2 < indexCount; i += 3)
		{
			uint32_t ia = indices[i], ib = indices[i + 1], ic = indices[i + 2];
			if (ia >= vertexCount || ib >= vertexCount || ic >= vertexCount)
			{
				invalidIndex = true;
				break;
			}
			const GlobeMeshVertex &a = gv[ia];
			const GlobeMeshVertex &b = gv[ib];
			const GlobeMeshVertex &c = gv[ic];
			const double abx = (double)b.p[0] - a.p[0];
			const double aby = (double)b.p[1] - a.p[1];
			const double abz = (double)b.p[2] - a.p[2];
			const double acx = (double)c.p[0] - a.p[0];
			const double acy = (double)c.p[1] - a.p[1];
			const double acz = (double)c.p[2] - a.p[2];
			const double nx = aby * acz - abz * acy;
			const double ny = abz * acx - abx * acz;
			const double nz = abx * acy - aby * acx;
			const double cx = ((double)a.p[0] + b.p[0] + c.p[0]) / 3.0;
			const double cy = ((double)a.p[1] + b.p[1] + c.p[1]) / 3.0;
			const double cz = ((double)a.p[2] + b.p[2] + c.p[2]) / 3.0;
			if (nx * cx + ny * cy + nz * cz < 0.0)
			{
				std::swap(indices[i + 1], indices[i + 2]);
				++flippedTriangles;
			}
		}
		if (invalidIndex)
		{
			Log(LOG_ERROR) << "[GEOSCAPE HD MESH HYGIENE V1] invalid index in asset=" << assetKey;
			return false;
		}
		Log(LOG_INFO) << "[GEOSCAPE HD MESH HYGIENE V1] asset=" << assetKey
			<< " outwardNormalsFlipped=" << flippedNormals
			<< " outwardTrianglesFlipped=" << flippedTriangles
			<< " sourceGLB=UNCHANGED";

		D3D11_BUFFER_DESC vbDesc = {};
		vbDesc.ByteWidth = (UINT)vertexBytes.size();
		vbDesc.Usage = D3D11_USAGE_IMMUTABLE;
		vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		D3D11_SUBRESOURCE_DATA vbData = {};
		vbData.pSysMem = vertexBytes.data();
		HRESULT hr = _impl->device->CreateBuffer(&vbDesc, &vbData, &_impl->geoscapeVertexBuffer);
		if (FAILED(hr) || !_impl->geoscapeVertexBuffer) return false;

		D3D11_BUFFER_DESC ibDesc = {};
		ibDesc.ByteWidth = (UINT)(indices.size() * sizeof(uint32_t));
		ibDesc.Usage = D3D11_USAGE_IMMUTABLE;
		ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
		D3D11_SUBRESOURCE_DATA ibData = {};
		ibData.pSysMem = indices.data();
		hr = _impl->device->CreateBuffer(&ibDesc, &ibData, &_impl->geoscapeIndexBuffer);
		if (FAILED(hr) || !_impl->geoscapeIndexBuffer)
		{
			_impl->geoscapeVertexBuffer->Release();
			_impl->geoscapeVertexBuffer = nullptr;
			return false;
		}

		_impl->geoscapeMeshKey = assetKey;
		_impl->geoscapeVertexCount = vertexCount;
		_impl->geoscapeIndexCount = indexCount;
		Log(LOG_INFO) << "[GEOSCAPE HD PROOF OF LIFE V1][MESH] asset=" << assetKey
			<< " vertices=" << vertexCount
			<< " triangles=" << (indexCount / 3)
			<< " sourceRadiusMeters=" << sourceMeta[0]
			<< " vertexColor=ON normals=ON uv=NOT_REQUIRED"
			<< " attribution=\"Earth Terrain and Sea Map\" by John Davies, CC BY-SA 4.0";
	}

	struct GlobeConstants
	{
		float trig[4];
		float camera[4];
	} cb = {};
	cb.trig[0] = std::sin(centerLon);
	cb.trig[1] = std::cos(centerLon);
	cb.trig[2] = std::sin(centerLat);
	cb.trig[3] = std::cos(centerLat);
	const float aspect = std::max(0.01f, (float)destW / (float)destH);
	const float fovYRadians = std::max(1.0f, std::min(120.0f, fovYDegrees)) * 3.14159265358979323846f / 180.0f;
	const float focalY = 1.0f / std::tan(fovYRadians * 0.5f);
	cb.camera[0] = aspect;
	cb.camera[1] = std::max(1.01f, cameraDistance);
	cb.camera[2] = focalY;
	cb.camera[3] = std::max(8.0f, cb.camera[1] + 2.0f);

	D3D11_MAPPED_SUBRESOURCE mapped = {};
	if (FAILED(_impl->context->Map(_impl->geoscapeConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
	std::memcpy(mapped.pData, &cb, sizeof(cb));
	_impl->context->Unmap(_impl->geoscapeConstants, 0);

	_impl->context->OMSetRenderTargets(1, &_impl->rtv, _impl->geoscapeDsv);
	_impl->context->ClearDepthStencilView(_impl->geoscapeDsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
	_impl->context->OMSetDepthStencilState(_impl->geoscapeDepthState, 0);
	const float blendFactor[4] = {0,0,0,0};
	_impl->context->OMSetBlendState(nullptr, blendFactor, 0xffffffffu);
	_impl->context->RSSetState(_impl->rasterNormal);

	D3D11_VIEWPORT vp = {};
	vp.TopLeftX = (FLOAT)destX;
	vp.TopLeftY = (FLOAT)destY;
	vp.Width = (FLOAT)destW;
	vp.Height = (FLOAT)destH;
	vp.MinDepth = 0.0f;
	vp.MaxDepth = 1.0f;
	_impl->context->RSSetViewports(1, &vp);

	UINT stride = 28, offset = 0;
	_impl->context->IASetInputLayout(_impl->geoscapeInputLayout);
	_impl->context->IASetVertexBuffers(0, 1, &_impl->geoscapeVertexBuffer, &stride, &offset);
	_impl->context->IASetIndexBuffer(_impl->geoscapeIndexBuffer, DXGI_FORMAT_R32_UINT, 0);
	_impl->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	_impl->context->VSSetShader(_impl->geoscapeVs, nullptr, 0);
	_impl->context->VSSetConstantBuffers(0, 1, &_impl->geoscapeConstants);
	_impl->context->PSSetShader(_impl->geoscapePs, nullptr, 0);
	_impl->context->DrawIndexed(_impl->geoscapeIndexCount, 0, 0);

	// Restore the ordinary 2D compositor target/state for following UI surfaces.
	_impl->context->OMSetRenderTargets(1, &_impl->rtv, nullptr);
	_impl->context->OMSetDepthStencilState(nullptr, 0);
	D3D11_VIEWPORT full = {};
	full.Width = (FLOAT)_impl->width;
	full.Height = (FLOAT)_impl->height;
	full.MinDepth = 0.0f;
	full.MaxDepth = 1.0f;
	_impl->context->RSSetViewports(1, &full);

	++_impl->presentationDrawCalls;
	return true;
#else
	(void)assetKey; (void)destX; (void)destY; (void)destW; (void)destH;
	(void)centerLon; (void)centerLat; (void)scaleX; (void)scaleY;
	return false;
#endif
}

bool HdGpuBackend::frameActive() const { return _impl->frameActive; }

void HdGpuBackend::abortFrame()
{
#if defined(_WIN32)
	_impl->endGpuTimingFrame();
#endif
	_impl->preparedThisFrame = false;
	_impl->frameActive = false;
	_impl->lastUsed = false;
	_impl->logicalBaseFrame = false;
}

bool HdGpuBackend::present()
{
#if defined(_WIN32)
	if (!_impl->frameActive || !_impl->overlaySurface || !_impl->swap) return false;
	if (!flushCpuOverlay())
	{
		abortFrame();
		return false;
	}
	_impl->endGpuTimingFrame();
	HRESULT hr = _impl->swap->Present(0, 0);
	if (FAILED(hr))
	{
		_impl->shutdownDevice();
		abortFrame();
		return false;
	}
	_impl->lastUsed = true;
	_impl->lastLogicalBase = _impl->logicalBaseFrame;
	_impl->lastMapDrawCalls = _impl->mapDrawCalls;
	_impl->lastIndexedDrawCalls = _impl->indexedDrawCalls;
	_impl->lastEnvironmentDrawCalls = _impl->environmentDrawCalls;
	_impl->lastPresentationDrawCalls = _impl->presentationDrawCalls;
	_impl->lastLegacyPresentationDrawCalls = _impl->legacyPresentationDrawCalls;
	_impl->frameActive = false;
	_impl->logicalBaseFrame = false;
	return true;
#else
	return false;
#endif
}

bool HdGpuBackend::beginMap(int mapLogicalWidth, int mapLogicalHeight,
	float worldDepthMin, float worldDepthMax, const SDL_Color *palette, const SDL_Color *neutralPalette,
	int clipX0, int clipY0, int clipX1, int clipY1)
{
#if defined(_WIN32)
	if (!_impl->frameActive || !palette || mapLogicalWidth <= 0 || mapLogicalHeight <= 0) return false;
	if (!_impl->ensureWorldDepth()) return false;
	if (!_impl->ensurePaletteTexture(palette)) return false;
	if (!_impl->ensureEnvironmentTextures(neutralPalette ? neutralPalette : palette, palette)) return false;
	_impl->mapLogicalW = mapLogicalWidth;
	_impl->mapLogicalH = mapLogicalHeight;
	_impl->legacyBridgeOrderReady = false;
	_impl->worldDepthMin = worldDepthMin;
	_impl->worldDepthMax = std::max(worldDepthMin + 1.0f, worldDepthMax);

	// Start a fresh GPU-owned geometric depth frame. No product of the Legacy
	// raster is required to initialize this frame or this depth target.
	_impl->beginGpuTimingMap();
	_impl->context->ClearDepthStencilView(_impl->worldDsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
	_impl->context->OMSetDepthStencilState(_impl->worldDepthWriteState, 0);

	_impl->setFullViewport();
	D3D11_RECT rect = { clipX0, clipY0, clipX1, clipY1 };
	_impl->context->RSSetState(_impl->rasterScissor);
	_impl->context->RSSetScissorRects(1, &rect);
	return true;
#else
	(void)mapLogicalWidth; (void)mapLogicalHeight; (void)worldDepthMin; (void)worldDepthMax;
	(void)palette; (void)neutralPalette; (void)clipX0; (void)clipY0; (void)clipX1; (void)clipY1;
	return false;
#endif
}

bool HdGpuBackend::setMapLegacyBridgeOrder(const unsigned *legacyBridgeOrder, int mapWidth, int mapHeight)
{
	if (Options::hdGraphics) return false; // No native painter-order upload.
#if defined(_WIN32)
	if (!_impl->frameActive || !legacyBridgeOrder || mapWidth <= 0 || mapHeight <= 0) return false;
	if (!_impl->ensureOrderTexture(mapWidth, mapHeight)) return false;
	D3D11_MAPPED_SUBRESOURCE mapped = {};
	HRESULT hr = _impl->context->Map(_impl->orderTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
	if (FAILED(hr)) return false;
	const Uint8 *src = reinterpret_cast<const Uint8*>(legacyBridgeOrder);
	Uint8 *dst = static_cast<Uint8*>(mapped.pData);
	const size_t rowBytes = (size_t)mapWidth * sizeof(unsigned);
	for (int y = 0; y < mapHeight; ++y)
		std::memcpy(dst + (size_t)y * mapped.RowPitch, src + (size_t)y * rowBytes, rowBytes);
	_impl->context->Unmap(_impl->orderTexture, 0);
	_impl->legacyBridgeOrderReady = true;
	return true;
#else
	(void)legacyBridgeOrder; (void)mapWidth; (void)mapHeight;
	return false;
#endif
}

bool HdGpuBackend::drawMapSprite(const HdGpuMapSprite &s)
{
#if defined(_WIN32)
	if (!_impl->frameActive || s.destW <= 0 || s.destH <= 0) return false;
	ID3D11ShaderResourceView *spriteSrv = nullptr;
	if (!_impl->ensureImage(s, &spriteSrv)) return false;
    return drawMapSpriteResource(s,spriteSrv);
#else
    (void)s; return false;
#endif
}

bool HdGpuBackend::drawMapUnit3D(const HdGpuMapSprite &s, const std::string &asset, const HdUnit3DPose &pose)
{
#if defined(_WIN32)
    if (!_impl->frameActive || s.destW<=0 || s.destH<=0) return false;
    D3D11_RECT rect{}; UINT count=1; _impl->context->RSGetScissorRects(&count,&rect);
    auto *srv=_impl->unit3D.render(_impl->device,_impl->context,asset,pose);
    _impl->setFullViewport(); _impl->context->RSSetState(_impl->rasterScissor);
    if(count) _impl->context->RSSetScissorRects(1,&rect);
    if(!srv) return false;
    return drawMapSpriteResource(s,srv);
#else
    (void)s; (void)asset; (void)pose; return false;
#endif
}

bool HdGpuBackend::drawMapSpriteResource(const HdGpuMapSprite &s, void *resource)
{
#if defined(_WIN32)
    auto *spriteSrv=static_cast<ID3D11ShaderResourceView*>(resource);
    if(!spriteSrv) return false;
    const uint64_t perfStarted = hdPerfNowUs();
	struct alignas(16) SpriteConstants
	{
		float destPx[4];
		float metrics0[4]; // outputW, outputH, baseRenderX, baseRenderY
		float metrics1[4]; // renderToPhysicalX/Y, light, unused
		uint32_t ids0[4];  // drawOrder, srcW, srcH, mapW
		int32_t ids1[4];   // mapH, clipBegX, clipBegY, clipEndX
		int32_t ids2[4];   // clipEndY, flags, indexed shade, indexed recolour
		float grade0[4];   // material tint R/G/B, exposure
		float grade1[4];   // contrast, saturation, enabled, reserved
		float lightTint[4]; // physical REAL HD light colour response
	};
	static_assert(sizeof(SpriteConstants) == 144, "SpriteConstants must match HLSL cbuffer");

	SpriteConstants c = {};
	c.destPx[0] = (float)s.destX; c.destPx[1] = (float)s.destY;
	c.destPx[2] = (float)s.destW; c.destPx[3] = (float)s.destH;
	c.metrics0[0] = (float)_impl->width; c.metrics0[1] = (float)_impl->height;
	c.metrics0[2] = (float)s.baseRenderX; c.metrics0[3] = (float)s.baseRenderY;
	c.metrics1[0] = (float)s.renderToPhysicalX; c.metrics1[1] = (float)s.renderToPhysicalY;
	c.metrics1[2] = (float)(16 - std::max(0, std::min(16, s.shade))) / 16.0f;
	c.metrics1[3] = 0.0f;
	c.ids0[0] = s.drawOrder; c.ids0[1] = s.imageWidth; c.ids0[2] = s.imageHeight; c.ids0[3] = (uint32_t)_impl->mapLogicalW;
	c.ids1[0] = _impl->mapLogicalH; c.ids1[1] = s.clipBegX; c.ids1[2] = s.clipBegY; c.ids1[3] = s.clipEndX;
	c.ids2[0] = s.clipEndY; c.ids2[1] = (s.hasClipMask ? 1 : 0) | (s.rightHalfOnly ? 2 : 0) | (s.premultiplied ? 4 : 0);
	c.ids2[2] = std::max(0, std::min(16, s.shade));
	c.ids2[3] = (s.colorMode == HdColorMode::IndexedLegacy) ? std::max(0, std::min(16, s.legacyBaseColor)) : 0;
	c.grade0[0] = s.materialTintR; c.grade0[1] = s.materialTintG; c.grade0[2] = s.materialTintB; c.grade0[3] = s.materialExposure;
	c.grade1[0] = s.materialContrast; c.grade1[1] = s.materialSaturation; c.grade1[2] = s.materialGradeEnabled ? 1.0f : 0.0f; c.grade1[3] = 0.0f;
	c.lightTint[0] = s.lightTintR; c.lightTint[1] = s.lightTintG; c.lightTint[2] = s.lightTintB; c.lightTint[3] = 1.0f;

	D3D11_MAPPED_SUBRESOURCE mapped = {};

    if (s.roofCaustic)
    {
        c.ids2[1] |= 8;
        struct alignas(16) RoofConstants { float global[4]; float a[5][4]; float b[5][4]; float meta[4]; unsigned rows[16]; } roof = {};
        const auto &profile = HdCausticSettings::instance().current();
        roof.global[0]=profile.intensity/1000.0f; roof.global[1]=profile.weakBoost/1000.0f;
        for(int k=0;k<5;++k) {
            const auto &l=profile.layers[k];
            for(int j=0;j<4;++j)roof.a[k][j]=l[j]/1000.0f;
            roof.b[k][0]=l[4]/1000.0f;roof.b[k][1]=l[5]*0.01745329252f;
            roof.b[k][2]=l[6]/1000.0f;roof.b[k][3]=l[7]/1000.0f;
        }
        roof.meta[0]=s.roofWorldX;roof.meta[1]=s.roofWorldY;roof.meta[2]=s.roofTime;
        roof.meta[3]=HdCausticSettings::instance().missionDepth()>0 ? profile.presence/1000.0f : 0.0f;
        std::copy(s.roofSunRows.begin(),s.roofSunRows.end(),roof.rows);
        D3D11_MAPPED_SUBRESOURCE roofMapped={};
        if(FAILED(_impl->context->Map(_impl->roofCausticConstants,0,D3D11_MAP_WRITE_DISCARD,0,&roofMapped)))return false;
        std::memcpy(roofMapped.pData,&roof,sizeof(roof));_impl->context->Unmap(_impl->roofCausticConstants,0);
        _impl->context->PSSetConstantBuffers(1,1,&_impl->roofCausticConstants);
    }
	HRESULT hr = _impl->context->Map(_impl->spriteConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
	if (FAILED(hr)) return false;
	std::memcpy(mapped.pData, &c, sizeof(c));
	_impl->context->Unmap(_impl->spriteConstants, 0);

	static const float blendFactor[4] = {0,0,0,0};
	_impl->context->OMSetRenderTargets(1, &_impl->rtv, nullptr);
	_impl->context->OMSetDepthStencilState(nullptr, 0);
	_impl->context->OMSetBlendState(s.premultiplied ? _impl->premultipliedBlend : _impl->alphaBlend, blendFactor, 0xffffffffu);
	_impl->context->IASetInputLayout(nullptr);
	_impl->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	_impl->context->VSSetShader(_impl->spriteVs, nullptr, 0);
	_impl->context->VSSetConstantBuffers(0, 1, &_impl->spriteConstants);
	ID3D11PixelShader *pixelShader = _impl->spritePs;
	if (s.colorMode == HdColorMode::IndexedLegacy) pixelShader = _impl->indexedSpritePs;
	else if (s.colorMode == HdColorMode::Environment) pixelShader = _impl->environmentSpritePs;
	_impl->context->PSSetShader(pixelShader, nullptr, 0);
	_impl->context->PSSetConstantBuffers(0, 1, &_impl->spriteConstants);
	// Authored HD map sprites no longer sample the Legacy-derived environment LUT.
	// Slot t4 remains null; BEDROCK owns its separate environment-LUT path.
	ID3D11ShaderResourceView *srvs[5] = { nullptr, nullptr, nullptr, _impl->paletteSrv, nullptr };
	if (s.colorMode == HdColorMode::IndexedLegacy) srvs[2] = spriteSrv;
	else srvs[0] = spriteSrv;
	_impl->context->PSSetShaderResources(0, 5, srvs);
	_impl->context->Draw(4, 0);
	ID3D11ShaderResourceView *nullSrvs[5] = { nullptr, nullptr, nullptr, nullptr, nullptr };
	_impl->context->PSSetShaderResources(0, 5, nullSrvs);
	++_impl->mapDrawCalls;
	if (s.colorMode == HdColorMode::IndexedLegacy) ++_impl->indexedDrawCalls;
	if (s.colorMode == HdColorMode::Environment) ++_impl->environmentDrawCalls;
	auto &perf = getHdPerfStats().current;
	perf.mapSpriteSubmitUs += hdPerfNowUs() - perfStarted;
	++perf.mapSpriteDraws;
	return true;
#else
	(void)s; (void)resource;
	return false;
#endif
}

bool HdGpuBackend::drawMapWorldCoverage(const HdGpuWorldCoverage &g)
{
#if defined(_WIN32)
	if (!_impl->frameActive || !g.vertices || !g.vertexCount || g.destW <= 0 || g.destH <= 0 ||
		!_impl->worldCoverageVs || !_impl->worldCoverageInputLayout || !_impl->worldDsv ||
		!_impl->worldDepthWriteState || !_impl->spriteConstants) return false;
	if (!_impl->ensureWorldCoverageVertexBuffer(g.vertexCount)) return false;

	D3D11_MAPPED_SUBRESOURCE vb = {};
	HRESULT hr = _impl->context->Map(_impl->worldCoverageVertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &vb);
	if (FAILED(hr)) return false;
	std::memcpy(vb.pData, g.vertices, (size_t)g.vertexCount * sizeof(HdGpuWorldCoverageVertex));
	_impl->context->Unmap(_impl->worldCoverageVertexBuffer, 0);

	struct alignas(16) CoverageConstants
	{
		float metrics0[4]; // outputW, outputH, mapPhysicalOriginX/Y
		float metrics1[4]; // renderScaleX/Y, depthMin/max
	};
	CoverageConstants c = {};
	c.metrics0[0] = (float)_impl->width; c.metrics0[1] = (float)_impl->height;
	c.metrics0[2] = (float)g.mapPhysicalOriginX; c.metrics0[3] = (float)g.mapPhysicalOriginY;
	c.metrics1[0] = (float)g.renderScaleX; c.metrics1[1] = (float)g.renderScaleY;
	c.metrics1[2] = _impl->worldDepthMin; c.metrics1[3] = _impl->worldDepthMax;
	D3D11_MAPPED_SUBRESOURCE cb = {};
	hr = _impl->context->Map(_impl->spriteConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &cb);
	if (FAILED(hr)) return false;
	std::memcpy(cb.pData, &c, sizeof(c));
	_impl->context->Unmap(_impl->spriteConstants, 0);

	UINT stride = (UINT)sizeof(HdGpuWorldCoverageVertex), offset = 0;
	_impl->context->OMSetRenderTargets(0, nullptr, _impl->worldDsv);
	_impl->context->OMSetDepthStencilState(_impl->worldDepthWriteState, 0);
	_impl->context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
	_impl->context->RSSetState(_impl->rasterScissor);
	_impl->context->IASetInputLayout(_impl->worldCoverageInputLayout);
	_impl->context->IASetVertexBuffers(0, 1, &_impl->worldCoverageVertexBuffer, &stride, &offset);
	_impl->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	_impl->context->VSSetShader(_impl->worldCoverageVs, nullptr, 0);
	_impl->context->VSSetConstantBuffers(0, 1, &_impl->spriteConstants);
	// No pixel shader and no colour render target: this pass can only populate
	// Real HD depth/coverage. It is impossible for it to alter the displayed art.
	_impl->context->PSSetShader(nullptr, nullptr, 0);
	_impl->context->Draw(g.vertexCount, 0);
	++_impl->mapDrawCalls;
	return true;
#else
	(void)g;
	return false;
#endif
}

void HdGpuBackend::clearHdImageCache()
{
#if defined(_WIN32)
	if (!_impl) return;
	for (auto &pair : _impl->images)
	{
		if (pair.second.srv) pair.second.srv->Release();
		if (pair.second.texture) pair.second.texture->Release();
	}
	_impl->images.clear();
#endif
}

bool HdGpuBackend::drawBedrock(const HdGpuBedrock &g)
{
#if defined(_WIN32)
	if (!_impl->frameActive || !g.vertices || !g.vertexCount || !g.assetKey || !g.rgba ||
		!g.imageWidth || !g.imageHeight || !_impl->bedrockVs || !_impl->bedrockPs ||
		!_impl->bedrockInputLayout || !_impl->spriteConstants || !g.fogCoverage ||
		!g.fogWidth || !g.fogHeight || !g.fogLayerHeight) return false;
	const uint64_t perfStarted = hdPerfNowUs();
	if (!_impl->bedrockFogTexture || !_impl->bedrockFogSrv ||
		_impl->bedrockFogW != g.fogWidth || _impl->bedrockFogH != g.fogHeight)
	{
		if (_impl->bedrockFogSrv) { _impl->bedrockFogSrv->Release(); _impl->bedrockFogSrv = nullptr; }
		if (_impl->bedrockFogTexture) { _impl->bedrockFogTexture->Release(); _impl->bedrockFogTexture = nullptr; }
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = g.fogWidth; desc.Height = g.fogHeight;
		desc.MipLevels = 1; desc.ArraySize = 1; desc.Format = DXGI_FORMAT_R8_UNORM;
		desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (FAILED(_impl->device->CreateTexture2D(&desc, nullptr, &_impl->bedrockFogTexture)) ||
			FAILED(_impl->device->CreateShaderResourceView(_impl->bedrockFogTexture, nullptr, &_impl->bedrockFogSrv))) return false;
		_impl->bedrockFogW = g.fogWidth; _impl->bedrockFogH = g.fogHeight;
	}
	{
		D3D11_MAPPED_SUBRESOURCE mapped = {};
		if (FAILED(_impl->context->Map(_impl->bedrockFogTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
		for (unsigned row = 0; row < g.fogHeight; ++row)
			std::memcpy((unsigned char*)mapped.pData + (size_t)row * mapped.RowPitch,
				g.fogCoverage + (size_t)row * g.fogWidth, g.fogWidth);
		_impl->context->Unmap(_impl->bedrockFogTexture, 0);
	}

	ID3D11ShaderResourceView *bedrockSrv = nullptr;
	if (!_impl->ensureMaterialImage(g.assetKey, g.rgba, g.imageWidth, g.imageHeight,
		Impl::MaterialMipKind::Color, &bedrockSrv) || !bedrockSrv) return false;

	ID3D11ShaderResourceView *verticalSrv = bedrockSrv;
	if (g.verticalAssetKey && g.verticalRgba && g.verticalImageWidth && g.verticalImageHeight)
	{
		if (!_impl->ensureMaterialImage(g.verticalAssetKey, g.verticalRgba, g.verticalImageWidth, g.verticalImageHeight,
			Impl::MaterialMipKind::Color, &verticalSrv) || !verticalSrv) return false;
	}

	auto ensureMaterialSrv = [&](const char *assetKey, const unsigned char *rgba, unsigned w, unsigned h,
		Impl::MaterialMipKind kind, ID3D11ShaderResourceView **out) -> bool
	{
		return assetKey && rgba && w && h && out && _impl->ensureMaterialImage(assetKey, rgba, w, h, kind, out) && *out;
	};

	ID3D11ShaderResourceView *normalSrv = nullptr;
	ID3D11ShaderResourceView *roughnessSrv = nullptr;
	ID3D11ShaderResourceView *aoSrv = nullptr;
	const bool pbrReady = g.usePbrMaterial
		&& ensureMaterialSrv(g.normalKey, g.normalRgba, g.normalWidth, g.normalHeight, Impl::MaterialMipKind::Normal, &normalSrv)
		&& ensureMaterialSrv(g.roughnessKey, g.roughnessRgba, g.roughnessWidth, g.roughnessHeight, Impl::MaterialMipKind::Scalar, &roughnessSrv)
		&& ensureMaterialSrv(g.aoKey, g.aoRgba, g.aoWidth, g.aoHeight, Impl::MaterialMipKind::Scalar, &aoSrv);

	ID3D11ShaderResourceView *verticalNormalSrv = nullptr;
	ID3D11ShaderResourceView *verticalRoughnessSrv = nullptr;
	ID3D11ShaderResourceView *verticalAoSrv = nullptr;
	const bool verticalPbrReady = g.useVerticalPbrMaterial
		&& ensureMaterialSrv(g.verticalNormalKey, g.verticalNormalRgba, g.verticalNormalWidth, g.verticalNormalHeight, Impl::MaterialMipKind::Normal, &verticalNormalSrv)
		&& ensureMaterialSrv(g.verticalRoughnessKey, g.verticalRoughnessRgba, g.verticalRoughnessWidth, g.verticalRoughnessHeight, Impl::MaterialMipKind::Scalar, &verticalRoughnessSrv)
		&& ensureMaterialSrv(g.verticalAoKey, g.verticalAoRgba, g.verticalAoWidth, g.verticalAoHeight, Impl::MaterialMipKind::Scalar, &verticalAoSrv);

	const bool craterReady = g.craterField && g.craterFieldWidth && g.craterFieldHeight
		&& _impl->uploadBedrockCrater(g.craterField, g.craterFieldWidth, g.craterFieldHeight, g.craterFieldRevision)
		&& _impl->bedrockCraterSrv;
	const bool weaponReady = g.weaponField && g.weaponFieldWidth && g.weaponFieldHeight
		&& _impl->uploadBedrockWeapon(g.weaponField, g.weaponFieldWidth, g.weaponFieldHeight, g.weaponFieldRevision)
		&& _impl->bedrockWeaponSrv;

	if (!_impl->ensureBedrockVertexBuffer(g.vertexCount)) return false;
	if (!g.vertexRevision || _impl->uploadedBedrockVertexRevision != g.vertexRevision ||
		_impl->uploadedBedrockVertexCount != g.vertexCount)
	{
		D3D11_MAPPED_SUBRESOURCE vb = {};
		HRESULT hr = _impl->context->Map(_impl->bedrockVertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &vb);
		if (FAILED(hr)) return false;
		std::memcpy(vb.pData, g.vertices, (size_t)g.vertexCount * sizeof(HdGpuBedrockVertex));
		_impl->context->Unmap(_impl->bedrockVertexBuffer, 0);
		_impl->uploadedBedrockVertexRevision = g.vertexRevision;
		_impl->uploadedBedrockVertexCount = g.vertexCount;
	}

	struct alignas(16) BedrockConstants
	{
		float destPx[4];
		float metrics0[4]; // outputW/H, physical map origin X/Y
		float metrics1[4]; // logical map -> physical scale X/Y
		uint32_t ids0[4];
		int32_t ids1[4];
		int32_t ids2[4];   // x = apply environment LUT
		float localMeta[4]; // mapW, mapH, mapZ, localLightCount
		float depthMeta[4]; // raw world view-depth min/max in voxel units
		float fogMeta[4]; // atlas width, layer height, atlas height, edge softness
		float localLightPosRadius[24][4];
		float localLightColorIntensity[24][4];
        float causticGlobal[4]; float causticA[5][4]; float causticB[5][4];
	};
	static_assert(sizeof(BedrockConstants) == 1088, "BedrockConstants must match HLSL cbuffer");
	BedrockConstants c = {};
    const auto &profile=HdCausticSettings::instance().current();
    c.causticGlobal[0]=profile.intensity/1000.0f; c.causticGlobal[1]=profile.weakBoost/1000.0f;
    for(int k=0;k<5;++k){const auto &l=profile.layers[k];
        for(int j=0;j<4;++j)c.causticA[k][j]=l[j]/1000.0f;
        c.causticB[k][0]=l[4]/1000.0f;c.causticB[k][1]=l[5]*0.01745329252f;
        c.causticB[k][2]=l[6]/1000.0f;c.causticB[k][3]=l[7]/1000.0f;
    }
	c.destPx[0] = (float)g.destX; c.destPx[1] = (float)g.destY;
	c.destPx[2] = (float)g.destW; c.destPx[3] = (float)g.destH;
	c.metrics0[0] = (float)_impl->width; c.metrics0[1] = (float)_impl->height;
	c.metrics0[2] = (float)g.mapPhysicalOriginX; c.metrics0[3] = (float)g.mapPhysicalOriginY;
	c.metrics1[0] = (float)g.renderScaleX; c.metrics1[1] = (float)g.renderScaleY;
	// Impact texel size for both blast and weapon fields. Weapon triplets now provide the
	// main pseudo-height source when present; both fields use the same world-space resolution.
	const unsigned impactFieldW = weaponReady ? g.weaponFieldWidth : (craterReady ? g.craterFieldWidth : 0u);
	const unsigned impactFieldH = weaponReady ? g.weaponFieldHeight : (craterReady ? g.craterFieldHeight : 0u);
	c.metrics1[2] = impactFieldW ? (1.0f / (float)impactFieldW) : 0.0f;
	c.metrics1[3] = impactFieldH ? (1.0f / (float)impactFieldH) : 0.0f;
	c.ids0[0] = (uint32_t)_impl->mapLogicalW; c.ids0[1] = (uint32_t)_impl->mapLogicalH; c.ids0[2] = craterReady ? 1u : 0u; c.ids0[3] = weaponReady ? 1u : 0u;
	c.ids1[0] = std::max(0, g.environmentGradeTopPermille);
	c.ids1[1] = std::max(0, g.environmentGradeVerticalPermille);
	c.ids1[2] = std::max(0, g.environmentGradeCoveredPermille);
	c.ids1[3] = pbrReady ? std::max(0, std::min(4000, g.sandMicroreliefPermille)) : 0;
	c.ids2[0] = g.applyEnvironment ? 1 : 0;
	c.ids2[1] = pbrReady ? 1 : 0;
	c.ids2[2] = verticalPbrReady ? 1 : 0;
	c.ids2[3] = std::max(0, g.depthLuminancePermille);
	c.localMeta[0] = (float)g.localLightMapW;
	c.localMeta[1] = (float)g.localLightMapH;
	c.localMeta[2] = (float)g.localLightMapZ;
	const unsigned localLightCount = std::min(24u, g.localLightCount);
	c.localMeta[3] = (float)localLightCount;
	c.depthMeta[0] = _impl->worldDepthMin;
	c.depthMeta[1] = _impl->worldDepthMax;
    c.depthMeta[2] = HdCausticSettings::instance().missionDepth()>0 ? profile.presence/1000.0f : 0.0f;
    c.depthMeta[3] = g.causticPhase;
    static bool loggedWaterMaterial = false;
    if (!loggedWaterMaterial && g.sandMicroreliefPermille > 0)
    {
        loggedWaterMaterial = true;
        Log(LOG_INFO) << "[REAL HD P2S][WATER MATERIAL] microrelief=" << c.ids1[3]
            << " pbr=" << pbrReady << " causticStrength=" << g.causticStrength
            << " heightSource=current-albedo-proxy oldSandPbr=overridden causticClock=real-time";
    }
	c.fogMeta[0] = (float)g.fogWidth;
	c.fogMeta[1] = (float)g.fogLayerHeight;
	c.fogMeta[2] = (float)g.fogHeight;
	c.fogMeta[3] = (float)std::max(0, std::min(1000, g.fogSoftnessPermille)) / 1000.0f;
	for (unsigned li = 0; li < localLightCount; ++li)
	{
		const HdGpuLocalLight &light = g.localLights[li];
		c.localLightPosRadius[li][0] = light.x;
		c.localLightPosRadius[li][1] = light.y;
		c.localLightPosRadius[li][2] = light.z;
		c.localLightPosRadius[li][3] = light.radius;
		c.localLightColorIntensity[li][0] = light.r;
		c.localLightColorIntensity[li][1] = light.g;
		c.localLightColorIntensity[li][2] = light.b;
		c.localLightColorIntensity[li][3] = light.intensity;
	}

	D3D11_MAPPED_SUBRESOURCE cb = {};
	HRESULT hr = _impl->context->Map(_impl->spriteConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &cb);
	if (FAILED(hr)) return false;
	std::memcpy(cb.pData, &c, sizeof(c));
	_impl->context->Unmap(_impl->spriteConstants, 0);

	static const float blendFactor[4] = {0,0,0,0};
	UINT stride = (UINT)sizeof(HdGpuBedrockVertex), offset = 0;
	ID3D11DepthStencilView *worldDsv = nullptr;
	ID3D11DepthStencilState *worldDepthState = nullptr;
	if (g.worldDepthMode == HdGpuWorldDepthMode::OpaqueWrite)
	{
		worldDsv = _impl->worldDsv;
		worldDepthState = _impl->worldDepthWriteState;
	}
	else if (g.worldDepthMode == HdGpuWorldDepthMode::TranslucentRead)
	{
		worldDsv = _impl->worldReadOnlyDsv;
		worldDepthState = _impl->worldDepthReadState;
	}
	_impl->context->OMSetRenderTargets(1, &_impl->rtv, worldDsv);
	_impl->context->OMSetDepthStencilState(worldDepthState, 0);
	_impl->context->OMSetBlendState(_impl->alphaBlend, blendFactor, 0xffffffffu);
	_impl->context->IASetInputLayout(_impl->bedrockInputLayout);
	_impl->context->IASetVertexBuffers(0, 1, &_impl->bedrockVertexBuffer, &stride, &offset);
	_impl->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	_impl->context->VSSetShader(_impl->bedrockVs, nullptr, 0);
	_impl->context->VSSetConstantBuffers(0, 1, &_impl->spriteConstants);
	_impl->context->PSSetShader(_impl->bedrockPs, nullptr, 0);
	_impl->context->PSSetConstantBuffers(0, 1, &_impl->spriteConstants);
	const size_t envIndex = hdEnvironmentProfileIndex(g.environmentProfile);
	ID3D11ShaderResourceView *srvs[13] =
	{
		bedrockSrv,
		nullptr,
		pbrReady ? normalSrv : bedrockSrv,
		pbrReady ? roughnessSrv : bedrockSrv,
		_impl->environmentSrvs[envIndex],
		pbrReady ? aoSrv : bedrockSrv,
		verticalSrv ? verticalSrv : bedrockSrv,
		verticalPbrReady ? verticalNormalSrv : (verticalSrv ? verticalSrv : bedrockSrv),
		verticalPbrReady ? verticalRoughnessSrv : (verticalSrv ? verticalSrv : bedrockSrv),
		verticalPbrReady ? verticalAoSrv : (verticalSrv ? verticalSrv : bedrockSrv),
		craterReady ? _impl->bedrockCraterSrv : bedrockSrv,
		weaponReady ? _impl->bedrockWeaponSrv : (craterReady ? _impl->bedrockCraterSrv : bedrockSrv),
		_impl->bedrockFogSrv
	};
	_impl->context->PSSetShaderResources(0, 13, srvs);
	_impl->context->PSSetSamplers(1, 1, &_impl->linearSampler);
	ID3D11SamplerState *bedrockSampler = _impl->bedrockAnisoWrapSampler ? _impl->bedrockAnisoWrapSampler : _impl->linearWrapSampler;
	_impl->context->PSSetSamplers(2, 1, &bedrockSampler);
	const bool largeTopColour = g.imageWidth > 4096u || g.imageHeight > 4096u;
	const bool largeVerticalColour = g.verticalImageWidth > 4096u || g.verticalImageHeight > 4096u;
	ID3D11SamplerState *largeColourSampler = _impl->bedrockLargeColorWrapSampler ? _impl->bedrockLargeColorWrapSampler : bedrockSampler;
	ID3D11SamplerState *colourSamplers[2] =
	{
		largeTopColour ? largeColourSampler : bedrockSampler,
		largeVerticalColour ? largeColourSampler : bedrockSampler
	};
	_impl->context->PSSetSamplers(3, 2, colourSamplers);
	static bool loggedLargeTopColour = false;
	if (largeTopColour && !loggedLargeTopColour)
	{
		Log(LOG_INFO) << "[REAL HD MATERIAL PERF V1][LARGE-ALBEDO] asset=" << g.assetKey
			<< " size=" << g.imageWidth << "x" << g.imageHeight
			<< " sourceResolution=PRESERVED sampler=" << (_impl->bedrockLargeColorWrapSampler ? "ANISO4_BIAS0.5" : "FALLBACK_ANISO8")
			<< " pbr=ANISO8_BIAS0";
		loggedLargeTopColour = true;
	}
	_impl->context->Draw(g.vertexCount, 0);
	ID3D11ShaderResourceView *nullSrvs[13] = {};
	_impl->context->PSSetShaderResources(0, 13, nullSrvs);
	ID3D11Buffer *nullBuffer = nullptr; UINT zero = 0;
	_impl->context->IASetVertexBuffers(0, 1, &nullBuffer, &zero, &zero);
	++_impl->mapDrawCalls;
	if (g.applyEnvironment) ++_impl->environmentDrawCalls;
	auto &perf = getHdPerfStats().current;
	perf.mapBedrockSubmitUs += hdPerfNowUs() - perfStarted;
	++perf.mapBedrockDraws;
	return true;
#else
	(void)g;
	return false;
#endif
}

bool HdGpuBackend::drawMapLocalLights(const HdGpuLocalLight *lights, unsigned lightCount)
{
#if defined(_WIN32)
	const uint64_t perfStarted = hdPerfNowUs();
	if (!_impl->frameActive || !lights || lightCount == 0 || !_impl->spriteVs || !_impl->localLightPs || !_impl->additiveBlend) return true;
	const unsigned count = std::min(24u, lightCount);
	static const float blendFactor[4] = {0,0,0,0};

	struct alignas(16) LightConstants
	{
		float destPx[4];
		float metrics0[4];
		float metrics1[4];
		uint32_t ids0[4];
		int32_t ids1[4];
		int32_t ids2[4];
		float grade0[4];
		float grade1[4];
	};
	static_assert(sizeof(LightConstants) == 128, "LightConstants must match SpriteCB prefix");

	_impl->context->OMSetRenderTargets(1, &_impl->rtv, nullptr);
	_impl->context->OMSetBlendState(_impl->additiveBlend, blendFactor, 0xffffffffu);
	_impl->context->IASetInputLayout(nullptr);
	_impl->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	_impl->context->VSSetShader(_impl->spriteVs, nullptr, 0);
	_impl->context->VSSetConstantBuffers(0, 1, &_impl->spriteConstants);
	_impl->context->PSSetShader(_impl->localLightPs, nullptr, 0);
	_impl->context->PSSetConstantBuffers(0, 1, &_impl->spriteConstants);

	for (unsigned li = 0; li < count; ++li)
	{
		const HdGpuLocalLight &light = lights[li];
		if (light.intensity <= 0.0f || light.screenRadiusX <= 0.0f || light.screenRadiusY <= 0.0f) continue;
		LightConstants c = {};
		const float rx = std::max(1.0f, light.screenRadiusX);
		const float ry = std::max(1.0f, light.screenRadiusY);
		c.destPx[0] = light.screenX - rx; c.destPx[1] = light.screenY - ry;
		c.destPx[2] = rx * 2.0f; c.destPx[3] = ry * 2.0f;
		c.metrics0[0] = (float)_impl->width; c.metrics0[1] = (float)_impl->height;
		c.metrics0[2] = light.screenX; c.metrics0[3] = light.screenY;
		c.metrics1[0] = rx; c.metrics1[1] = ry; c.metrics1[2] = light.intensity;
		c.grade0[0] = light.r; c.grade0[1] = light.g; c.grade0[2] = light.b; c.grade0[3] = 1.0f;

		D3D11_MAPPED_SUBRESOURCE cb = {};
		HRESULT hr = _impl->context->Map(_impl->spriteConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &cb);
		if (FAILED(hr)) return false;
		std::memcpy(cb.pData, &c, sizeof(c));
		_impl->context->Unmap(_impl->spriteConstants, 0);
		_impl->context->Draw(4, 0);
		++_impl->mapDrawCalls;
	}

	// Restore normal alpha blending for the visibility mask / subsequent map work.
	_impl->context->OMSetBlendState(_impl->alphaBlend, blendFactor, 0xffffffffu);
	getHdPerfStats().current.mapLocalLightSubmitUs += hdPerfNowUs() - perfStarted;
	return true;
#else
	(void)lights; (void)lightCount;
	return false;
#endif
}

bool HdGpuBackend::drawMapSmokeVolume(const HdGpuSmokeBlob *blobs, unsigned blobCount)
{
#if defined(_WIN32)
	const uint64_t perfStarted = hdPerfNowUs();
	if (!_impl->frameActive || !blobs || blobCount == 0 || !_impl->spriteVs || !_impl->smokePs || !_impl->alphaBlend) return true;
	const unsigned count = std::min(256u, blobCount);
	static const float blendFactor[4] = {0,0,0,0};
	struct alignas(16) SmokeConstants
	{
		float destPx[4]; float metrics0[4]; float metrics1[4]; uint32_t ids0[4]; int32_t ids1[4]; int32_t ids2[4]; float grade0[4]; float grade1[4];
	};
	static_assert(sizeof(SmokeConstants) == 128, "SmokeConstants must match SpriteCB");
	_impl->context->OMSetRenderTargets(1, &_impl->rtv, nullptr);
	_impl->context->OMSetBlendState(_impl->alphaBlend, blendFactor, 0xffffffffu);
	_impl->context->IASetInputLayout(nullptr);
	_impl->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	_impl->context->VSSetShader(_impl->spriteVs, nullptr, 0);
	_impl->context->VSSetConstantBuffers(0, 1, &_impl->spriteConstants);
	_impl->context->PSSetShader(_impl->smokePs, nullptr, 0);
	_impl->context->PSSetConstantBuffers(0, 1, &_impl->spriteConstants);
	for (unsigned bi=0; bi<count; ++bi)
	{
		const HdGpuSmokeBlob &b=blobs[bi];
		if (b.density<=0.0f || b.opacity<=0.0f || b.screenRadiusX<=0.0f || b.screenRadiusY<=0.0f) continue;
		SmokeConstants c={}; const float rx=std::max(1.0f,b.screenRadiusX), ry=std::max(1.0f,b.screenRadiusY);
		c.destPx[0]=b.screenX-rx; c.destPx[1]=b.screenY-ry; c.destPx[2]=rx*2.0f; c.destPx[3]=ry*2.0f;
		c.metrics0[0]=(float)_impl->width; c.metrics0[1]=(float)_impl->height; c.metrics0[2]=b.screenX; c.metrics0[3]=b.screenY;
		c.metrics1[0]=rx; c.metrics1[1]=ry; c.metrics1[2]=std::max(0.0f,b.density); c.metrics1[3]=b.phase;
		c.grade0[0]=b.r; c.grade0[1]=b.g; c.grade0[2]=b.b; c.grade0[3]=std::max(0.0f,b.opacity);
		D3D11_MAPPED_SUBRESOURCE cb={}; HRESULT hr=_impl->context->Map(_impl->spriteConstants,0,D3D11_MAP_WRITE_DISCARD,0,&cb); if(FAILED(hr)) return false;
		std::memcpy(cb.pData,&c,sizeof(c)); _impl->context->Unmap(_impl->spriteConstants,0); _impl->context->Draw(4,0); ++_impl->mapDrawCalls;
	}
	getHdPerfStats().current.mapSmokeSubmitUs += hdPerfNowUs() - perfStarted;
	return true;
#else
	(void)blobs; (void)blobCount; return false;
#endif
}

bool HdGpuBackend::drawMapVisibilityMask(const HdGpuVisibilityVertex *vertices, unsigned vertexCount)
{
#if defined(_WIN32)
	if (!_impl->frameActive || !vertices || vertexCount == 0 || !_impl->visibilityVs || !_impl->visibilityPs || !_impl->visibilityInputLayout) return true;
	if (!_impl->ensureBedrockVertexBuffer(vertexCount)) return false;

	D3D11_MAPPED_SUBRESOURCE vb = {};
	HRESULT hr = _impl->context->Map(_impl->bedrockVertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &vb);
	if (FAILED(hr)) return false;
	std::memcpy(vb.pData, vertices, (size_t)vertexCount * sizeof(HdGpuVisibilityVertex));
	_impl->context->Unmap(_impl->bedrockVertexBuffer, 0);

	struct alignas(16) VisibilityConstants { float metrics0[4]; };
	VisibilityConstants c = {};
	c.metrics0[0] = (float)_impl->width; c.metrics0[1] = (float)_impl->height;
	D3D11_MAPPED_SUBRESOURCE cb = {};
	hr = _impl->context->Map(_impl->spriteConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &cb);
	if (FAILED(hr)) return false;
	std::memcpy(cb.pData, &c, sizeof(c));
	_impl->context->Unmap(_impl->spriteConstants, 0);

	static const float blendFactor[4] = {0,0,0,0};
	UINT stride = (UINT)sizeof(HdGpuVisibilityVertex), offset = 0;
	_impl->context->OMSetRenderTargets(1, &_impl->rtv, nullptr);
	_impl->context->OMSetBlendState(_impl->alphaBlend, blendFactor, 0xffffffffu);
	_impl->context->IASetInputLayout(_impl->visibilityInputLayout);
	_impl->context->IASetVertexBuffers(0, 1, &_impl->bedrockVertexBuffer, &stride, &offset);
	_impl->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	_impl->context->VSSetShader(_impl->visibilityVs, nullptr, 0);
	_impl->context->VSSetConstantBuffers(0, 1, &_impl->spriteConstants);
	_impl->context->PSSetShader(_impl->visibilityPs, nullptr, 0);
	_impl->context->PSSetConstantBuffers(0, 1, &_impl->spriteConstants);
	_impl->context->Draw(vertexCount, 0);

	ID3D11Buffer *nullBuffer = nullptr; UINT zero = 0;
	_impl->context->IASetVertexBuffers(0, 1, &nullBuffer, &zero, &zero);
	++_impl->mapDrawCalls;
	return true;
#else
	(void)vertices; (void)vertexCount;
	return false;
#endif
}

bool HdGpuBackend::applyMapVisionFeather(const HdGpuVisibilityVertex *visibleSurface,
	unsigned vertexCount, int softnessPermille)
{
#if defined(_WIN32)
	if (vertexCount == 0) return true;
	if (!_impl->frameActive || !visibleSurface || !_impl->visionCoveragePs ||
		!_impl->visionSoftPs || !_impl->visibilityVs || !_impl->visibilityInputLayout ||
		!_impl->fullscreenVs || !_impl->baseConstants || !_impl->spriteConstants ||
		!_impl->ensureVisionBuffers() || !_impl->ensureBedrockVertexBuffer(vertexCount)) return false;

	D3D11_MAPPED_SUBRESOURCE vb = {};
	HRESULT hr = _impl->context->Map(_impl->bedrockVertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &vb);
	if (FAILED(hr)) return false;
	std::memcpy(vb.pData, visibleSurface, (size_t)vertexCount * sizeof(HdGpuVisibilityVertex));
	_impl->context->Unmap(_impl->bedrockVertexBuffer, 0);
	_impl->uploadedBedrockVertexRevision = 0;

	struct alignas(16) VisibilityConstants { float metrics0[4]; } vision = {};
	vision.metrics0[0] = (float)_impl->width;
	vision.metrics0[1] = (float)_impl->height;
	D3D11_MAPPED_SUBRESOURCE cb = {};
	hr = _impl->context->Map(_impl->spriteConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &cb);
	if (FAILED(hr)) return false;
	std::memcpy(cb.pData, &vision, sizeof(vision));
	_impl->context->Unmap(_impl->spriteConstants, 0);

	static const float clear[4] = {0,0,0,0};
	_impl->context->ClearRenderTargetView(_impl->visionMaskRtv, clear);
	_impl->setFullViewport();
	_impl->context->RSSetState(_impl->rasterNormal);
	_impl->context->OMSetRenderTargets(1, &_impl->visionMaskRtv, nullptr);
	_impl->context->OMSetBlendState(_impl->additiveBlend, clear, 0xffffffffu);
	UINT stride = (UINT)sizeof(HdGpuVisibilityVertex), offset = 0;
	_impl->context->IASetInputLayout(_impl->visibilityInputLayout);
	_impl->context->IASetVertexBuffers(0, 1, &_impl->bedrockVertexBuffer, &stride, &offset);
	_impl->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	_impl->context->VSSetShader(_impl->visibilityVs, nullptr, 0);
	_impl->context->VSSetConstantBuffers(0, 1, &_impl->spriteConstants);
	_impl->context->PSSetShader(_impl->visionCoveragePs, nullptr, 0);
	_impl->context->Draw(vertexCount, 0);

	_impl->context->OMSetRenderTargets(0, nullptr, nullptr);
	ID3D11Texture2D *back = nullptr;
	hr = _impl->swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back));
	if (FAILED(hr) || !back) return false;
	_impl->context->CopyResource(_impl->visionSceneCopy, back);
	back->Release();

	struct alignas(16) VisionSoftConstants { uint32_t dstRect[4]; uint32_t srcInfo[4]; } soft = {};
	soft.dstRect[2] = (uint32_t)_impl->width;
	soft.dstRect[3] = (uint32_t)_impl->height;
	soft.srcInfo[0] = (uint32_t)std::max(0, std::min(1000, softnessPermille));
	hr = _impl->context->Map(_impl->baseConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &cb);
	if (FAILED(hr)) return false;
	std::memcpy(cb.pData, &soft, sizeof(soft));
	_impl->context->Unmap(_impl->baseConstants, 0);
	_impl->context->OMSetRenderTargets(1, &_impl->rtv, nullptr);
	_impl->context->IASetInputLayout(nullptr);
	ID3D11Buffer *nullBuffer = nullptr; UINT zero = 0;
	_impl->context->IASetVertexBuffers(0, 1, &nullBuffer, &zero, &zero);
	_impl->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	_impl->context->VSSetShader(_impl->fullscreenVs, nullptr, 0);
	_impl->context->PSSetShader(_impl->visionSoftPs, nullptr, 0);
	_impl->context->PSSetConstantBuffers(0, 1, &_impl->baseConstants);
	_impl->context->PSSetSamplers(0, 1, &_impl->linearSampler);
	ID3D11ShaderResourceView *resources[2] = {_impl->visionSceneSrv, _impl->visionMaskSrv};
	_impl->context->PSSetShaderResources(0, 2, resources);
	_impl->context->Draw(3, 0);
	ID3D11ShaderResourceView *nullSrvs[2] = {nullptr, nullptr};
	_impl->context->PSSetShaderResources(0, 2, nullSrvs);
	_impl->context->OMSetBlendState(nullptr, clear, 0xffffffffu);
	_impl->mapDrawCalls += 2;
	return true;
#else
	(void)visibleSurface; (void)vertexCount; (void)softnessPermille;
	return false;
#endif
}

bool HdGpuBackend::drawMapSurfaceCursor(const HdGpuCursorVertex *vertices,
	unsigned vertexCount, bool yellow, bool violet)
{
#if defined(_WIN32)
	if (!_impl->frameActive || !vertices || vertexCount == 0 || !_impl->cursorVs ||
		!_impl->cursorPs || !_impl->cursorInputLayout) return false;
	if (!_impl->ensureBedrockVertexBuffer(vertexCount)) return false;
	D3D11_MAPPED_SUBRESOURCE vb = {};
	HRESULT hr = _impl->context->Map(_impl->bedrockVertexBuffer, 0,
		D3D11_MAP_WRITE_DISCARD, 0, &vb);
	if (FAILED(hr)) return false;
	std::memcpy(vb.pData, vertices, (size_t)vertexCount * sizeof(HdGpuCursorVertex));
	_impl->context->Unmap(_impl->bedrockVertexBuffer, 0);
	_impl->uploadedBedrockVertexRevision = 0;
	struct alignas(16) CursorConstants { float metrics0[4]; float depthMeta[4]; };
	CursorConstants c = {};
	c.metrics0[0] = (float)_impl->width;
	c.metrics0[1] = (float)_impl->height;
	c.metrics0[2] = violet ? 2.0f : (yellow ? 1.0f : 0.0f);
	c.depthMeta[0] = _impl->worldDepthMin;
	c.depthMeta[1] = _impl->worldDepthMax;
	D3D11_MAPPED_SUBRESOURCE cb = {};
	hr = _impl->context->Map(_impl->spriteConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &cb);
	if (FAILED(hr)) return false;
	std::memcpy(cb.pData, &c, sizeof(c));
	_impl->context->Unmap(_impl->spriteConstants, 0);
	static const float blendFactor[4] = {0,0,0,0};
	UINT stride = (UINT)sizeof(HdGpuCursorVertex), offset = 0;
	_impl->context->OMSetRenderTargets(1, &_impl->rtv, _impl->worldReadOnlyDsv);
	_impl->context->OMSetDepthStencilState(_impl->worldDepthReadState, 0);
	_impl->context->OMSetBlendState(_impl->alphaBlend, blendFactor, 0xffffffffu);
	_impl->context->IASetInputLayout(_impl->cursorInputLayout);
	_impl->context->IASetVertexBuffers(0, 1, &_impl->bedrockVertexBuffer, &stride, &offset);
	_impl->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	_impl->context->VSSetShader(_impl->cursorVs, nullptr, 0);
	_impl->context->VSSetConstantBuffers(0, 1, &_impl->spriteConstants);
	_impl->context->PSSetShader(_impl->cursorPs, nullptr, 0);
	_impl->context->PSSetConstantBuffers(0, 1, &_impl->spriteConstants);
	_impl->context->Draw(vertexCount, 0);
	ID3D11Buffer *nullBuffer = nullptr; UINT zero = 0;
	_impl->context->IASetVertexBuffers(0, 1, &nullBuffer, &zero, &zero);
	_impl->context->OMSetRenderTargets(1, &_impl->rtv, nullptr);
	_impl->context->OMSetDepthStencilState(nullptr, 0);
	++_impl->mapDrawCalls;
	return true;
#else
	(void)vertices; (void)vertexCount; (void)yellow; (void)violet;
	return false;
#endif
}

bool HdGpuBackend::drawMapVisibilityTakeover(const unsigned char *hiddenCells, unsigned mapW, unsigned mapH,
	int destX, int destY, int destW, int destH, double renderScaleX, double renderScaleY,
	int mapOffsetX, int mapOffsetY, int spriteWidth, int spriteHeight, int viewLevel,
	int featherPermille, int roundPermille, int insetPermille, int contrastPermille,
	int aaPermille, int opacityPermille)
{
	(void)featherPermille; (void)roundPermille; (void)insetPermille;
	(void)contrastPermille; (void)aaPermille; (void)opacityPermille;
#if defined(_WIN32)
	if (!_impl->frameActive || !hiddenCells || !mapW || !mapH || destW <= 0 || destH <= 0 ||
		!_impl->spriteVs || !_impl->visibilityTakeoverPs || !_impl->spriteConstants || !_impl->alphaBlend)
		return true;
	if (!_impl->uploadVisibilityState(hiddenCells, mapW, mapH)) return false;
	static bool loggedTakeover = false;
	if (!loggedTakeover)
	{
		unsigned hiddenCount = 0;
		for (size_t i = 0, n = (size_t)mapW * (size_t)mapH; i < n; ++i) hiddenCount += hiddenCells[i] ? 1u : 0u;
		Log(LOG_WARNING) << "[REAL HD FOV V1][FALLBACK-ACTIVE] unexpected former V3 hard mask call map=" << mapW << "x" << mapH
			<< " hidden=" << hiddenCount << " viewport=" << destW << "x" << destH
			<< " insetApplied=0 legacyShade16GpuPresentation=BYPASSED";
		loggedTakeover = true;
	}

	struct alignas(16) TakeoverConstants
	{
		float destPx[4]; float metrics0[4]; float metrics1[4];
		uint32_t ids0[4]; int32_t ids1[4]; int32_t ids2[4];
		float grade0[4]; float grade1[4];
	};
	static_assert(sizeof(TakeoverConstants) == 128, "TakeoverConstants must match SpriteCB");
	TakeoverConstants c = {};
	c.destPx[0] = (float)destX; c.destPx[1] = (float)destY;
	c.destPx[2] = (float)destW; c.destPx[3] = (float)destH;
	c.metrics0[0] = (float)_impl->width; c.metrics0[1] = (float)_impl->height;
	c.metrics0[2] = (float)mapOffsetX; c.metrics0[3] = (float)mapOffsetY;
	c.metrics1[0] = (float)std::max(0.0001, renderScaleX);
	c.metrics1[1] = (float)std::max(0.0001, renderScaleY);
	c.ids0[0] = mapW; c.ids0[1] = mapH;
	c.ids0[2] = (uint32_t)std::max(4, spriteWidth);
	c.ids0[3] = (uint32_t)std::max(1, spriteHeight);
	c.ids1[0] = std::max(0, viewLevel);

	D3D11_MAPPED_SUBRESOURCE cb = {};
	HRESULT hr = _impl->context->Map(_impl->spriteConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &cb);
	if (FAILED(hr)) return false;
	std::memcpy(cb.pData, &c, sizeof(c));
	_impl->context->Unmap(_impl->spriteConstants, 0);

	static const float blendFactor[4] = {0,0,0,0};
	_impl->context->OMSetRenderTargets(1, &_impl->rtv, nullptr);
	_impl->context->OMSetBlendState(_impl->alphaBlend, blendFactor, 0xffffffffu);
	_impl->context->IASetInputLayout(nullptr);
	_impl->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	_impl->context->VSSetShader(_impl->spriteVs, nullptr, 0);
	_impl->context->VSSetConstantBuffers(0, 1, &_impl->spriteConstants);
	_impl->context->PSSetShader(_impl->visibilityTakeoverPs, nullptr, 0);
	_impl->context->PSSetConstantBuffers(0, 1, &_impl->spriteConstants);
	ID3D11ShaderResourceView *srv = _impl->visibilityStateSrv;
	_impl->context->PSSetShaderResources(2, 1, &srv);
	_impl->context->Draw(4, 0);
	ID3D11ShaderResourceView *nullSrv = nullptr;
	_impl->context->PSSetShaderResources(2, 1, &nullSrv);
	++_impl->mapDrawCalls;
	return true;
#else
	(void)hiddenCells; (void)mapW; (void)mapH; (void)destX; (void)destY; (void)destW; (void)destH;
	(void)renderScaleX; (void)renderScaleY; (void)mapOffsetX; (void)mapOffsetY;
	(void)spriteWidth; (void)spriteHeight; (void)viewLevel;
	return false;
#endif
}

bool HdGpuBackend::drawMapLegacyOverlay(const HdGpuMapLegacyOverlay &g)
{
	if (Options::hdGraphics) return false; // No native raster replay.
#if defined(_WIN32)
	if (!_impl->frameActive || !_impl->legacyBridgeOrderReady || !_impl->baseSrv || !_impl->orderSrv || !_impl->mapLegacyOverlayPs ||
		!_impl->spriteConstants || g.destW <= 0 || g.destH <= 0) return false;

	struct alignas(16) OverlayConstants
	{
		float destPx[4];
		float metrics0[4]; // outputW/H, physical map origin X/Y
		float metrics1[4]; // physical scale X/Y, source logical X/Y
		uint32_t ids0[4];  // orderW/H
		int32_t ids1[4];
		int32_t ids2[4];
	};
	static_assert(sizeof(OverlayConstants) == 96, "OverlayConstants must match HLSL cbuffer");
	OverlayConstants c = {};
	c.destPx[0]=(float)g.destX; c.destPx[1]=(float)g.destY; c.destPx[2]=(float)g.destW; c.destPx[3]=(float)g.destH;
	c.metrics0[0]=(float)_impl->width; c.metrics0[1]=(float)_impl->height;
	c.metrics0[2]=(float)g.mapPhysicalOriginX; c.metrics0[3]=(float)g.mapPhysicalOriginY;
	c.metrics1[0]=(float)g.renderScaleX; c.metrics1[1]=(float)g.renderScaleY;
	c.metrics1[2]=(float)g.sourceLogicalX; c.metrics1[3]=(float)g.sourceLogicalY;
	c.ids0[0]=(uint32_t)_impl->orderW; c.ids0[1]=(uint32_t)_impl->orderH;
	c.ids1[0]=(int32_t)g.minDrawOrder; c.ids1[1]=(int32_t)g.maxDrawOrder;

	D3D11_MAPPED_SUBRESOURCE mapped = {};
	HRESULT hr = _impl->context->Map(_impl->spriteConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
	if (FAILED(hr)) return false;
	std::memcpy(mapped.pData, &c, sizeof(c));
	_impl->context->Unmap(_impl->spriteConstants, 0);

	static const float blendFactor[4] = {0,0,0,0};
	_impl->context->OMSetRenderTargets(1, &_impl->rtv, nullptr);
	_impl->context->OMSetDepthStencilState(nullptr, 0);
	_impl->context->OMSetBlendState(_impl->alphaBlend, blendFactor, 0xffffffffu);
	_impl->context->IASetInputLayout(nullptr);
	_impl->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	_impl->context->VSSetShader(_impl->spriteVs, nullptr, 0);
	_impl->context->VSSetConstantBuffers(0, 1, &_impl->spriteConstants);
	_impl->context->PSSetShader(_impl->mapLegacyOverlayPs, nullptr, 0);
	_impl->context->PSSetConstantBuffers(0, 1, &_impl->spriteConstants);
	ID3D11ShaderResourceView *srvs[2] = { _impl->baseSrv, _impl->orderSrv };
	_impl->context->PSSetShaderResources(0, 2, srvs);
	_impl->context->Draw(4, 0);
	ID3D11ShaderResourceView *nullSrvs[2] = { nullptr, nullptr };
	_impl->context->PSSetShaderResources(0, 2, nullSrvs);
	++_impl->mapDrawCalls;
	return true;
#else
	(void)g;
	return false;
#endif
}

void HdGpuBackend::endMap()
{
#if defined(_WIN32)
	_impl->endGpuTimingMap();
	if (_impl->context)
	{
		_impl->context->OMSetDepthStencilState(nullptr, 0);
		_impl->context->RSSetState(_impl->rasterNormal);
	}
#endif
}

bool HdGpuBackend::wasUsedLastFrame() const { return _impl->lastUsed; }
const char *HdGpuBackend::lastModeName() const
{
	if (!_impl->lastUsed) return "OFF";
	if (_impl->lastLogicalBase)
	{
		if (_impl->lastPresentationDrawCalls)
		{
			if (_impl->lastIndexedDrawCalls && _impl->lastEnvironmentDrawCalls) return "D3D11-PRESENT1+MAP+IDX+ENV";
			if (_impl->lastIndexedDrawCalls) return "D3D11-PRESENT1+MAP+IDX";
			if (_impl->lastEnvironmentDrawCalls) return "D3D11-PRESENT1+MAP+ENV";
			return "D3D11-PRESENT1+MAP";
		}
		if (_impl->lastIndexedDrawCalls && _impl->lastEnvironmentDrawCalls) return "D3D11-ARCH1+MAP+IDX+ENV";
		if (_impl->lastIndexedDrawCalls) return "D3D11-ARCH1+MAP+IDX";
		if (_impl->lastEnvironmentDrawCalls) return "D3D11-ARCH1+MAP+ENV";
		return "D3D11-ARCH1+MAP";
	}
	if (_impl->lastIndexedDrawCalls && _impl->lastEnvironmentDrawCalls) return "D3D11-MAP+IDX+ENV";
	if (_impl->lastIndexedDrawCalls) return "D3D11-MAP+IDX";
	if (_impl->lastEnvironmentDrawCalls) return "D3D11-MAP+ENV";
	return "D3D11-MAP";
}
unsigned HdGpuBackend::lastMapDrawCalls() const { return _impl->lastMapDrawCalls; }
unsigned HdGpuBackend::lastIndexedDrawCalls() const { return _impl->lastIndexedDrawCalls; }
unsigned HdGpuBackend::lastEnvironmentDrawCalls() const { return _impl->lastEnvironmentDrawCalls; }
unsigned HdGpuBackend::lastPresentationDrawCalls() const { return _impl->lastPresentationDrawCalls; }
unsigned HdGpuBackend::lastLegacyPresentationDrawCalls() const { return _impl->lastLegacyPresentationDrawCalls; }

double HdGpuBackend::lastTrueGpuFrameUs() const
{
#if defined(_WIN32)
	return _impl->gpuTimingLastValid ? _impl->gpuTimingLastFrameUs : 0.0;
#else
	return 0.0;
#endif
}

double HdGpuBackend::lastTrueGpuMapUs() const
{
#if defined(_WIN32)
	return _impl->gpuTimingLastValid ? _impl->gpuTimingLastMapUs : 0.0;
#else
	return 0.0;
#endif
}

bool HdGpuBackend::hasTrueGpuTiming() const
{
#if defined(_WIN32)
	return _impl->gpuTimingLastValid;
#else
	return false;
#endif
}

std::uint64_t HdGpuBackend::resolvedTrueGpuTimingFrames() const
{
#if defined(_WIN32)
	return _impl->gpuTimingResolved;
#else
	return 0;
#endif
}

std::uint64_t HdGpuBackend::droppedTrueGpuTimingFrames() const
{
#if defined(_WIN32)
	return _impl->gpuTimingDropped;
#else
	return 0;
#endif
}

unsigned HdGpuBackend::pendingTrueGpuTimingFrames() const
{
#if defined(_WIN32)
	return _impl->gpuTimingPendingCount();
#else
	return 0;
#endif
}

unsigned HdGpuBackend::cachedHdImageCount() const
{
#if defined(_WIN32)
	return (unsigned)_impl->images.size();
#else
	return 0;
#endif
}

std::uint64_t HdGpuBackend::estimatedCachedHdImageBytes() const
{
#if defined(_WIN32)
	auto mipBytes = [](unsigned w, unsigned h, unsigned levels, unsigned bytesPerPixel) -> std::uint64_t
	{
		std::uint64_t bytes = 0;
		w = std::max(1u, w); h = std::max(1u, h); levels = std::max(1u, levels);
		for (unsigned level = 0; level < levels; ++level)
		{
			bytes += (std::uint64_t)w * (std::uint64_t)h * bytesPerPixel;
			w = std::max(1u, w / 2u);
			h = std::max(1u, h / 2u);
		}
		return bytes;
	};
	std::uint64_t bytes = 0;
	for (const auto &pair : _impl->images)
	{
		const Impl::GpuImage &image = pair.second;
		bytes += mipBytes(image.width, image.height, image.mipLevels, image.indexed ? 1u : 4u);
	}
	return bytes;
#else
	return 0;
#endif
}

std::uint64_t HdGpuBackend::estimatedTrackedGpuBytes() const
{
#if defined(_WIN32)
	std::uint64_t bytes = estimatedCachedHdImageBytes();
	for (const auto &pair : _impl->dynamicSurfaces)
	{
		const Impl::DynamicSurfaceImage &image = pair.second;
		bytes += (std::uint64_t)std::max(0, image.width) * (std::uint64_t)std::max(0, image.height) * (image.indexed ? 1u : 4u);
	}
	bytes += (std::uint64_t)std::max(0, _impl->baseW) * (std::uint64_t)std::max(0, _impl->baseH) * 4u;
	bytes += (std::uint64_t)std::max(0, _impl->overlayTexW) * (std::uint64_t)std::max(0, _impl->overlayTexH) * 4u;
	bytes += (std::uint64_t)std::max(0, _impl->worldDepthW) * (std::uint64_t)std::max(0, _impl->worldDepthH) * 4u;
	bytes += (std::uint64_t)std::max(0, _impl->orderW) * (std::uint64_t)std::max(0, _impl->orderH) * 4u;
	bytes += (std::uint64_t)std::max(0, _impl->visibilityStateW) * (std::uint64_t)std::max(0, _impl->visibilityStateH);
	bytes += (std::uint64_t)std::max(0, _impl->bedrockCraterW) * (std::uint64_t)std::max(0, _impl->bedrockCraterH) * 4u;
	bytes += (std::uint64_t)std::max(0, _impl->bedrockWeaponW) * (std::uint64_t)std::max(0, _impl->bedrockWeaponH) * 4u;
	bytes += (std::uint64_t)_impl->bedrockVertexCapacity * sizeof(HdGpuBedrockVertex);
	bytes += (std::uint64_t)_impl->worldCoverageVertexCapacity * sizeof(HdGpuWorldCoverageVertex);
	return bytes;
#else
	return 0;
#endif
}

}
