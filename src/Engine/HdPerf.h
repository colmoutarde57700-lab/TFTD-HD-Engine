#pragma once

#include <chrono>
#include <cstdint>

namespace OpenXcom
{

struct HdPerfSample
{
	uint64_t renderUs = 0;
	uint64_t clearUs = 0;
	uint64_t statesBlitUs = 0;
	uint64_t scaleUs = 0;
	uint64_t physicalPassUs = 0;
	uint64_t gpuOverlayUs = 0;
	uint64_t gpuBaseUploadUs = 0;
	uint64_t gpuBasePresentUs = 0;
	uint64_t presentationGpuUs = 0;
	uint64_t presentationLegacyGpuUs = 0;
	unsigned presentationGpuDraws = 0;
	unsigned presentationLegacyGpuDraws = 0;
	uint64_t sdlFlipUs = 0;
	bool cpuScaleBypassed = false;

	// RC12 P9A: presentation cadence measured between consecutive rendered frames.
	// This is intentionally independent from gameplay timers and camera timing.
	uint64_t frameIntervalUs = 0;
	uint64_t frameTargetUs = 0;
	uint64_t framePacingErrorUs = 0;
	uint64_t loopThinkUs = 0;
	uint64_t loopWaitUs = 0;
	uint64_t deadlineLatenessUs = 0;

	uint64_t mapLogicalBlitUs = 0;
	uint64_t hudLogicalBlitUs = 0;
	uint64_t mapDrawUs = 0;
	uint64_t mapPhysicalUs = 0;
	uint64_t mapSeedUs = 0;
	uint64_t mapCompositeUs = 0;
	uint64_t mapGpuSubmitUs = 0;
	// REAL HD MATERIAL PERF V1: CPU-side submission cost split by renderer owner.
	// These timers intentionally do not claim to be GPU timestamps; they identify
	// where the render thread blocks while submitting work to D3D11.
	uint64_t mapBedrockSubmitUs = 0;
	uint64_t mapSpriteSubmitUs = 0;
	uint64_t mapLocalLightSubmitUs = 0;
	uint64_t mapSmokeSubmitUs = 0;
	// REAL HD MATERIAL PERF DIAG V2: CPU owner split inside Map::blitHdOverlaysGpu.
	// These are deliberately observational only: no renderer/gameplay behavior changes.
	uint64_t mapBeginMapUs = 0;
	uint64_t mapLocalLightBuildUs = 0;
	uint64_t mapSmokeBuildUs = 0;
	uint64_t mapBedrockOwnerUs = 0;
	uint64_t mapImpactMaskPrepUs = 0;
	uint64_t mapImpactFieldRebuildUs = 0;
	uint64_t mapBedrockGeometryUs = 0;
	uint64_t mapWorldReplayUs = 0;
	uint64_t mapPostFxUs = 0;
	uint64_t mapImpactMaskScanPixels = 0;
	unsigned mapImpactMaskScans = 0;
	unsigned mapImpactBoundsCacheHits = 0;
	uint64_t mapHelmetLightBuildUs = 0;
	unsigned mapHelmetLightRebuilds = 0;
	uint64_t mapHelmetLightCandidateSamples = 0;
	uint64_t mapHelmetLightFieldCells = 0;
	uint64_t mapImageCpuCacheBytes = 0;
	unsigned mapImageCpuCacheCount = 0;
	unsigned mapBedrockDraws = 0;
	unsigned mapSpriteDraws = 0;
	uint64_t mapCacheBlitUs = 0;
	uint64_t hudPhysicalBlitUs = 0;

	uint64_t mapPixelsTested = 0;
	uint64_t mapPixelsWritten = 0;
	unsigned mapCommands = 0;
	unsigned mapHdGpuCommands = 0;
	unsigned mapLegacyGpuCommands = 0;
	unsigned mapCpuLegacyBlits = 0;
	uint64_t mapResolveUs = 0;
	unsigned mapResolveQueries = 0;
	unsigned mapResolveCacheHits = 0;
	unsigned logicalZoomSurfaces = 0;
	unsigned physicalZoomSurfaces = 0;
	bool mapRedraw = false;
	bool mapCacheHit = false;
};

struct HdPerfAverage
{
	double renderUs = 0.0;
	double clearUs = 0.0;
	double statesBlitUs = 0.0;
	double scaleUs = 0.0;
	double physicalPassUs = 0.0;
	double gpuOverlayUs = 0.0;
	double gpuBaseUploadUs = 0.0;
	double gpuBasePresentUs = 0.0;
	double presentationGpuUs = 0.0;
	double presentationLegacyGpuUs = 0.0;
	double presentationGpuDraws = 0.0;
	double presentationLegacyGpuDraws = 0.0;
	double sdlFlipUs = 0.0;
	double cpuScaleBypassedPct = 0.0;
	double frameIntervalUs = 0.0;
	double frameTargetUs = 0.0;
	double framePacingErrorUs = 0.0;
	double loopThinkUs = 0.0;
	double loopWaitUs = 0.0;
	double deadlineLatenessUs = 0.0;
	double mapLogicalBlitUs = 0.0;
	double hudLogicalBlitUs = 0.0;
	double mapDrawUs = 0.0;
	double mapPhysicalUs = 0.0;
	double mapSeedUs = 0.0;
	double mapCompositeUs = 0.0;
	double mapGpuSubmitUs = 0.0;
	double mapBedrockSubmitUs = 0.0;
	double mapSpriteSubmitUs = 0.0;
	double mapLocalLightSubmitUs = 0.0;
	double mapSmokeSubmitUs = 0.0;
	double mapBeginMapUs = 0.0;
	double mapLocalLightBuildUs = 0.0;
	double mapSmokeBuildUs = 0.0;
	double mapBedrockOwnerUs = 0.0;
	double mapImpactMaskPrepUs = 0.0;
	double mapImpactFieldRebuildUs = 0.0;
	double mapBedrockGeometryUs = 0.0;
	double mapWorldReplayUs = 0.0;
	double mapPostFxUs = 0.0;
	double mapImpactMaskScanPixels = 0.0;
	double mapImpactMaskScans = 0.0;
	double mapImpactBoundsCacheHits = 0.0;
	double mapHelmetLightBuildUs = 0.0;
	double mapHelmetLightRebuilds = 0.0;
	double mapHelmetLightCandidateSamples = 0.0;
	double mapHelmetLightFieldCells = 0.0;
	double mapImageCpuCacheBytes = 0.0;
	double mapImageCpuCacheCount = 0.0;
	double mapBedrockDraws = 0.0;
	double mapSpriteDraws = 0.0;
	double mapCacheBlitUs = 0.0;
	double hudPhysicalBlitUs = 0.0;
	double mapPixelsTested = 0.0;
	double mapPixelsWritten = 0.0;
	double mapCommands = 0.0;
	double mapHdGpuCommands = 0.0;
	double mapLegacyGpuCommands = 0.0;
	double mapCpuLegacyBlits = 0.0;
	double mapResolveUs = 0.0;
	double mapResolveQueries = 0.0;
	double mapResolveCacheHits = 0.0;
	double logicalZoomSurfaces = 0.0;
	double physicalZoomSurfaces = 0.0;
	double mapRedrawPct = 0.0;
	double mapCacheHitPct = 0.0;
};

struct HdPerfStats
{
	HdPerfSample current;
	HdPerfSample last;
	HdPerfAverage avg;
	uint64_t completedFrames = 0;

	void beginFrame()
	{
		current = HdPerfSample{};
	}

	void endFrame()
	{
		last = current;
		++completedFrames;
		const double a = completedFrames == 1 ? 1.0 : 0.05; // ~20-frame EMA
#define HDPERF_EMA(field) avg.field += a * ((double)current.field - avg.field)
		HDPERF_EMA(renderUs);
		HDPERF_EMA(clearUs);
		HDPERF_EMA(statesBlitUs);
		HDPERF_EMA(scaleUs);
		HDPERF_EMA(physicalPassUs);
		HDPERF_EMA(gpuOverlayUs);
		HDPERF_EMA(gpuBaseUploadUs);
		HDPERF_EMA(gpuBasePresentUs);
		HDPERF_EMA(presentationGpuUs);
		HDPERF_EMA(presentationLegacyGpuUs);
		HDPERF_EMA(presentationGpuDraws);
		HDPERF_EMA(presentationLegacyGpuDraws);
		HDPERF_EMA(sdlFlipUs);
		HDPERF_EMA(frameIntervalUs);
		HDPERF_EMA(frameTargetUs);
		HDPERF_EMA(framePacingErrorUs);
		HDPERF_EMA(loopThinkUs);
		HDPERF_EMA(loopWaitUs);
		HDPERF_EMA(deadlineLatenessUs);
		HDPERF_EMA(mapLogicalBlitUs);
		HDPERF_EMA(hudLogicalBlitUs);
		HDPERF_EMA(mapDrawUs);
		HDPERF_EMA(mapPhysicalUs);
		HDPERF_EMA(mapSeedUs);
		HDPERF_EMA(mapCompositeUs);
		HDPERF_EMA(mapGpuSubmitUs);
		HDPERF_EMA(mapBedrockSubmitUs);
		HDPERF_EMA(mapSpriteSubmitUs);
		HDPERF_EMA(mapLocalLightSubmitUs);
		HDPERF_EMA(mapSmokeSubmitUs);
		HDPERF_EMA(mapBeginMapUs);
		HDPERF_EMA(mapLocalLightBuildUs);
		HDPERF_EMA(mapSmokeBuildUs);
		HDPERF_EMA(mapBedrockOwnerUs);
		HDPERF_EMA(mapImpactMaskPrepUs);
		HDPERF_EMA(mapImpactFieldRebuildUs);
		HDPERF_EMA(mapBedrockGeometryUs);
		HDPERF_EMA(mapWorldReplayUs);
		HDPERF_EMA(mapPostFxUs);
		HDPERF_EMA(mapImpactMaskScanPixels);
		HDPERF_EMA(mapImpactMaskScans);
		HDPERF_EMA(mapImpactBoundsCacheHits);
		HDPERF_EMA(mapHelmetLightBuildUs);
		HDPERF_EMA(mapHelmetLightRebuilds);
		HDPERF_EMA(mapHelmetLightCandidateSamples);
		HDPERF_EMA(mapHelmetLightFieldCells);
		HDPERF_EMA(mapImageCpuCacheBytes);
		HDPERF_EMA(mapImageCpuCacheCount);
		HDPERF_EMA(mapBedrockDraws);
		HDPERF_EMA(mapSpriteDraws);
		HDPERF_EMA(mapCacheBlitUs);
		HDPERF_EMA(hudPhysicalBlitUs);
		HDPERF_EMA(mapPixelsTested);
		HDPERF_EMA(mapPixelsWritten);
		HDPERF_EMA(mapCommands);
		HDPERF_EMA(mapHdGpuCommands);
		HDPERF_EMA(mapLegacyGpuCommands);
		HDPERF_EMA(mapCpuLegacyBlits);
		HDPERF_EMA(mapResolveUs);
		HDPERF_EMA(mapResolveQueries);
		HDPERF_EMA(mapResolveCacheHits);
		HDPERF_EMA(logicalZoomSurfaces);
		HDPERF_EMA(physicalZoomSurfaces);
#undef HDPERF_EMA
		avg.cpuScaleBypassedPct += a * ((current.cpuScaleBypassed ? 100.0 : 0.0) - avg.cpuScaleBypassedPct);
		avg.mapRedrawPct += a * ((current.mapRedraw ? 100.0 : 0.0) - avg.mapRedrawPct);
		avg.mapCacheHitPct += a * ((current.mapCacheHit ? 100.0 : 0.0) - avg.mapCacheHitPct);
	}
};

inline HdPerfStats &getHdPerfStats()
{
	static HdPerfStats stats;
	return stats;
}

inline uint64_t hdPerfNowUs()
{
	using namespace std::chrono;
	return (uint64_t)duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

}
