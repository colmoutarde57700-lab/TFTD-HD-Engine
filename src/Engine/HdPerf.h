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
	uint64_t sdlFlipUs = 0;

	// RC12 P9A: presentation cadence measured between consecutive rendered frames.
	// This is intentionally independent from gameplay timers and camera timing.
	uint64_t frameIntervalUs = 0;
	uint64_t frameTargetUs = 0;
	uint64_t framePacingErrorUs = 0;

	uint64_t mapLogicalBlitUs = 0;
	uint64_t hudLogicalBlitUs = 0;
	uint64_t mapDrawUs = 0;
	uint64_t mapPhysicalUs = 0;
	uint64_t mapSeedUs = 0;
	uint64_t mapCompositeUs = 0;
	uint64_t mapGpuSubmitUs = 0;
	uint64_t mapCacheBlitUs = 0;
	uint64_t hudPhysicalBlitUs = 0;

	uint64_t mapPixelsTested = 0;
	uint64_t mapPixelsWritten = 0;
	unsigned mapCommands = 0;
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
	double sdlFlipUs = 0.0;
	double frameIntervalUs = 0.0;
	double frameTargetUs = 0.0;
	double framePacingErrorUs = 0.0;
	double mapLogicalBlitUs = 0.0;
	double hudLogicalBlitUs = 0.0;
	double mapDrawUs = 0.0;
	double mapPhysicalUs = 0.0;
	double mapSeedUs = 0.0;
	double mapCompositeUs = 0.0;
	double mapGpuSubmitUs = 0.0;
	double mapCacheBlitUs = 0.0;
	double hudPhysicalBlitUs = 0.0;
	double mapPixelsTested = 0.0;
	double mapPixelsWritten = 0.0;
	double mapCommands = 0.0;
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
		HDPERF_EMA(sdlFlipUs);
		HDPERF_EMA(frameIntervalUs);
		HDPERF_EMA(frameTargetUs);
		HDPERF_EMA(framePacingErrorUs);
		HDPERF_EMA(mapLogicalBlitUs);
		HDPERF_EMA(hudLogicalBlitUs);
		HDPERF_EMA(mapDrawUs);
		HDPERF_EMA(mapPhysicalUs);
		HDPERF_EMA(mapSeedUs);
		HDPERF_EMA(mapCompositeUs);
		HDPERF_EMA(mapGpuSubmitUs);
		HDPERF_EMA(mapCacheBlitUs);
		HDPERF_EMA(hudPhysicalBlitUs);
		HDPERF_EMA(mapPixelsTested);
		HDPERF_EMA(mapPixelsWritten);
		HDPERF_EMA(mapCommands);
		HDPERF_EMA(logicalZoomSurfaces);
		HDPERF_EMA(physicalZoomSurfaces);
#undef HDPERF_EMA
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
