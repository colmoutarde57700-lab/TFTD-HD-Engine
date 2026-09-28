	#pragma once
/*
 * Copyright 2010-2016 OpenXcom Developers.
 *
 * This file is part of OpenXcom.
 *
 * OpenXcom is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * OpenXcom is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with OpenXcom.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <SDL.h>
#include <string>
#include <functional>
#include "OpenGL.h"
#include "Surface.h"
#include "PresentationContext.h"
#include "HdCanvas.h"
#include "HdImage.h"
#include "HdUiRasterCache.h"

namespace OpenXcom
{

class Surface;
class Action;

/**
 * A display screen, handles rendering onto the game window.
 * In SDL a Screen is treated like a Surface, so this is just
 * a specialized version of a Surface with functionality more
 * relevant for display screens. Contains a Surface buffer
 * where all the contents are kept, so any filters or conversions
 * can be applied before rendering the screen.
 */
class Screen
{
private:
	SDL_Surface *_screen;
	int _bpp;
	int _baseWidth, _baseHeight;
	double _scaleX, _scaleY;
	int _topBlackBand, _bottomBlackBand, _leftBlackBand, _rightBlackBand, _cursorTopBlackBand, _cursorLeftBlackBand;
	Uint32 _flags;
	SDL_Color deferredPalette[256];
	int _numColors, _firstColor;
	bool _pushPalette;
	bool _flickerFix;
	bool _forceLegacy8Bit;
	OpenGL glOutput;
	Surface::UniqueBufferPtr _buffer;
	Surface::UniqueSurfacePtr _surface;
	PresentationContext _presentation;
	// A complete own-renderer scene, never a copy of the Legacy logical buffer.
	std::unique_ptr<HdCanvas> _lastHdCanvas;
	mutable HdImageCache _hdCanvasImages;
	mutable HdUiRasterCache _hdUiRasterCache;
	std::string _hdTraceContext;
	bool _hdUiWidgetsEnabled = false;
	// TEST8-D: transparent native-resolution staging layer uploaded to OpenGL.
	Surface::UniqueBufferPtr _glPhysicalOverlayBuffer;
	Surface::UniqueSurfacePtr _glPhysicalOverlaySurface;
	int _glPhysicalOverlayW = 0, _glPhysicalOverlayH = 0;
	/// Sets the _flags and _bpp variables based on game options; needed in more than one place now
	void makeVideoFlags();
public:
	static const int ORIGINAL_WIDTH;
	static const int ORIGINAL_HEIGHT;

	/// Creates a new display screen.
	Screen();
	/// Cleans up the display screen.
	~Screen();
	/// Get horizontal offset.
	int getDX() const;
	/// Get vertical offset.
	int getDY() const;
	/// Gets the internal buffer.
	SDL_Surface *getSurface();
	/// Handles keyboard events.
	void handle(Action *action);
	/// Renders the screen onto the game window.
	void flip(const std::function<void(SDL_Surface*)> &physicalPass = std::function<void(SDL_Surface*)>());
	/// Presents an HD scene without uploading the historical logical canvas.
	void presentHdCanvas(const HdCanvas &canvas, double opacity = 1.0);
	/// Re-presents the last HD scene for an HD fade; no palette/Legacy redraw.
	void fadeHdCanvas(double opacity);
	bool hasHdCanvas() const { return bool(_lastHdCanvas); }
	void discardHdCanvas() { _lastHdCanvas.reset(); }
	/// Clears the physical output through the HD presentation path.
	void clearHdCanvas();
	HdImageCache &getHdCanvasImages() { return _hdCanvasImages; }
	void setHdTraceContext(const std::string &context, bool widgetsEnabled = false) { _hdTraceContext = context; _hdUiWidgetsEnabled = widgetsEnabled; }

	/// Gets the physical display surface (software renderer only).
	SDL_Surface *getDisplaySurface() const { return _screen; }
	/// Scale used by the actual logical->display image, excluding letterbox bands.
	double getRenderScaleX() const;
	double getRenderScaleY() const;
	/// Maps logical coordinates to the displayed image in physical pixels.
	int logicalToPhysicalX(double x) const;
	int logicalToPhysicalY(double y) const;
	/// Central logical-to-physical presentation description used by the HD backend.
	const PresentationContext &getPresentationContext() const { return _presentation; }
	/// Re-blits one logical Surface directly to the final 32-bit framebuffer.
	bool blitNativeSurfaceAt(Surface *surface, SDL_Surface *destination, int x, int y, int w, int h) const;
	bool tryBlitHdSurfaceAt(Surface *surface, SDL_Surface *destination, int x, int y, int w, int h) const;
	void blitSurfacePhysical(Surface *surface, bool cursorCoordinates = false, SDL_Surface *destination = nullptr) const;
	/// Clears the screen.
	void clear();
	/// Sets the screen's 8bpp palette.
	void setPalette(const SDL_Color *colors, int firstcolor = 0, int ncolors = 256, bool immediately = false);
	/// Gets the screen's 8bpp palette.
	SDL_Color *getPalette() const;
	/// Gets the screen's width.
	int getWidth() const;
	/// Gets the screen's height.
	int getHeight() const;
	/// Resets the screen display.
	void resetDisplay(bool resetVideo = true, bool noShaders = false);
	/// Temporarily forces the original 8-bit display path (used by legacy FLI/FLC playback).
	void setForceLegacy8Bit(bool force, bool resetVideo = true);
	bool getForceLegacy8Bit() const { return _forceLegacy8Bit; }
	/// Gets the screen's X scale.
	double getXScale() const;
	/// Gets the screen's Y scale.
	double getYScale() const;
	/// Gets the screen's top black forbidden to cursor band's height.
	int getCursorTopBlackBand() const;
	/// Gets the screen's left black forbidden to cursor band's width.
	int getCursorLeftBlackBand() const;
	/// Takes a screenshot.
	void screenshot(const std::string &filename);
	/// Checks whether a 32bit scaler is requested and works for the selected resolution
	static bool use32bitScaler();
	/// Checks whether OpenGL output is requested
	static bool useOpenGL();
	/// RC12 P5: true when the previous frame was presented by the D3D11 physical compositor.
	static bool useHdGpuPhysical();
	static const char *hdGpuModeName();
	static unsigned hdGpuMapDrawCalls();
	static unsigned hdGpuIndexedDrawCalls();
	static unsigned hdGpuEnvironmentDrawCalls();
	/// update the game scale as required.
	static void updateScale(int type, int &width, int &height, bool change);
};

}
