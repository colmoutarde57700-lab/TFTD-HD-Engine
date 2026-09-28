#pragma once

#include "PresentationContext.h"
#include <algorithm>
#include <cmath>

namespace OpenXcom
{

/**
 * Explicit presentation spaces for the TFTD-HD renderer refactor.
 *
 * CONTRACT_V1 is intentionally shadow-only: it describes candidate transforms
 * but does not own rendering or input yet. The historical OXCE paths remain
 * authoritative until parity has been demonstrated.
 */
enum class PresentationSpaceKind
{
	World,
	UI,
	Physical
};

struct PresentationTransform
{
	int logicalW = 1;
	int logicalH = 1;
	PresentationRect physicalContent;
	double scaleX = 1.0;
	double scaleY = 1.0;

	int logicalToPhysicalX(double x) const
	{
		return physicalContent.x + (int)std::floor(x * scaleX + 0.5);
	}
	int logicalToPhysicalY(double y) const
	{
		return physicalContent.y + (int)std::floor(y * scaleY + 0.5);
	}
	double physicalToLogicalX(double x) const
	{
		return (x - (double)physicalContent.x) / std::max(0.000001, scaleX);
	}
	double physicalToLogicalY(double y) const
	{
		return (y - (double)physicalContent.y) / std::max(0.000001, scaleY);
	}
	PresentationRect logicalToPhysical(const PresentationRect &r) const
	{
		PresentationRect out;
		out.x = logicalToPhysicalX(r.x);
		out.y = logicalToPhysicalY(r.y);
		const int x2 = logicalToPhysicalX(r.x + r.w);
		const int y2 = logicalToPhysicalY(r.y + r.h);
		out.w = x2 - out.x;
		out.h = y2 - out.y;
		return out;
	}
	bool uniform(double epsilon = 0.000001) const
	{
		return std::fabs(scaleX - scaleY) <= epsilon;
	}
};

struct PresentationSpacesContract
{
	PresentationTransform world;
	PresentationTransform ui;
	PresentationRect physical;

	static PresentationTransform worldFrom(const PresentationContext &pc)
	{
		PresentationTransform out;
		out.logicalW = pc.logicalWidth();
		out.logicalH = pc.logicalHeight();
		out.physicalContent = pc.contentRect();
		out.scaleX = pc.scaleX();
		out.scaleY = pc.scaleY();
		return out;
	}

	/**
	 * Candidate UI transform independent from the current world canvas.
	 * The UI design canvas is fitted uniformly into the physical display.
	 * This is the key shadow contract we need before migrating any HUD pixels.
	 */
	static PresentationTransform uniformUiFit(int uiLogicalW, int uiLogicalH, int physicalW, int physicalH)
	{
		PresentationTransform out;
		out.logicalW = std::max(1, uiLogicalW);
		out.logicalH = std::max(1, uiLogicalH);
		const int pw = std::max(1, physicalW);
		const int ph = std::max(1, physicalH);
		const double scale = std::min((double)pw / (double)out.logicalW,
			(double)ph / (double)out.logicalH);
		const int contentW = std::max(1, (int)std::floor(out.logicalW * scale + 0.5));
		const int contentH = std::max(1, (int)std::floor(out.logicalH * scale + 0.5));
		out.physicalContent.x = (pw - contentW) / 2;
		out.physicalContent.y = (ph - contentH) / 2;
		out.physicalContent.w = contentW;
		out.physicalContent.h = contentH;
		out.scaleX = scale;
		out.scaleY = scale;
		return out;
	}

	static PresentationSpacesContract shadow(const PresentationContext &pc, int uiLogicalW, int uiLogicalH)
	{
		PresentationSpacesContract out;
		out.world = worldFrom(pc);
		out.physical = {0, 0, pc.physicalWidth(), pc.physicalHeight()};
		out.ui = uniformUiFit(uiLogicalW, uiLogicalH, pc.physicalWidth(), pc.physicalHeight());
		return out;
	}
};

}
