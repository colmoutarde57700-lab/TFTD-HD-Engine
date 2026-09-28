#pragma once

#include <algorithm>
#include <cmath>

namespace OpenXcom
{

/**
 * Physical presentation contract for the HD renderer.
 *
 * OXCE still owns a logical canvas.  This class is the single description of
 * how that canvas maps to the actual display.  It deliberately contains no
 * gameplay, camera or UI policy; later renderer work can add independent world
 * and UI viewports without scattering coordinate formulae across Screen, Map
 * and widgets.
 */
struct PresentationRect
{
	int x = 0;
	int y = 0;
	int w = 0;
	int h = 0;
};

class PresentationContext
{
private:
	int _logicalW = 1;
	int _logicalH = 1;
	int _physicalW = 1;
	int _physicalH = 1;
	PresentationRect _content;
	double _scaleX = 1.0;
	double _scaleY = 1.0;

public:
	void configure(int logicalW, int logicalH, int physicalW, int physicalH,
		int leftBand, int topBand, int rightBand, int bottomBand)
	{
		_logicalW = std::max(1, logicalW);
		_logicalH = std::max(1, logicalH);
		_physicalW = std::max(1, physicalW);
		_physicalH = std::max(1, physicalH);
		_content.x = std::max(0, leftBand);
		_content.y = std::max(0, topBand);
		_content.w = std::max(1, _physicalW - std::max(0, leftBand) - std::max(0, rightBand));
		_content.h = std::max(1, _physicalH - std::max(0, topBand) - std::max(0, bottomBand));
		_scaleX = (double)_content.w / (double)_logicalW;
		_scaleY = (double)_content.h / (double)_logicalH;
	}

	int logicalWidth() const { return _logicalW; }
	int logicalHeight() const { return _logicalH; }
	int physicalWidth() const { return _physicalW; }
	int physicalHeight() const { return _physicalH; }
	const PresentationRect &contentRect() const { return _content; }
	double scaleX() const { return _scaleX; }
	double scaleY() const { return _scaleY; }

	int logicalToPhysicalX(double x) const
	{
		return _content.x + (int)std::floor(x * _scaleX + 0.5);
	}
	int logicalToPhysicalY(double y) const
	{
		return _content.y + (int)std::floor(y * _scaleY + 0.5);
	}
	double physicalToLogicalX(double x) const
	{
		return (x - (double)_content.x) / std::max(0.000001, _scaleX);
	}
	double physicalToLogicalY(double y) const
	{
		return (y - (double)_content.y) / std::max(0.000001, _scaleY);
	}
	bool identity() const
	{
		return _content.x == 0 && _content.y == 0 &&
		_content.w == _logicalW && _content.h == _logicalH &&
		_physicalW == _logicalW && _physicalH == _logicalH;
	}
};

}
