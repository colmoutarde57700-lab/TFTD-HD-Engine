#include "HdRenderTrace.h"
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
#include "Screen.h"
#include "HdPerf.h"
#include "HdGpuBackend.h"
#include "HdCanvasRenderer.h"
#include "HdCanvasSignature.h"
#include "../Interface/Cursor.h"
#include "../Interface/Text.h"
#include "../Interface/TextButton.h"
#include "../Interface/FpsCounter.h"
#include "PresentationSpaces.h"
#include <algorithm>
#include <sstream>
#include <cmath>
#include <iomanip>
#include <climits>
#include <cstdio>
#include "../lodepng.h"
#include "Exception.h"
#include "Surface.h"
#include "InteractiveSurface.h"
#include "Logger.h"
#include "Action.h"
#include "Options.h"
#include "CrossPlatform.h"
#include "FileMap.h"
#include "Zoom.h"
#include "Timer.h"
#include <SDL.h>
#include <SDL_rotozoom.h>
#include <algorithm>

namespace OpenXcom
{

const int Screen::ORIGINAL_WIDTH = 320;
const int Screen::ORIGINAL_HEIGHT = 200;

void Screen::presentHdCanvas(const HdCanvas &canvas, double opacity)
{
	if (!Options::hdGraphics || useOpenGL() || !_screen ||
		_screen->format->BitsPerPixel != 32 || _forceLegacy8Bit)
		throw Exception("[HD PRESENTATION ERROR] HD scene requires the 32-bit HD presentation path");
	// Keep the source scene, not a snapshot of the old logical screen, for fades.
	_lastHdCanvas = std::make_unique<HdCanvas>(canvas);
	HdCanvas frame(_lastHdCanvas->bounds().w, _lastHdCanvas->bounds().h);
	frame.composite(*_lastHdCanvas, {}, opacity);
	// Own the entire target, including letterboxing. A previous state's SDL
	// clip must not preserve pixels from its historical presentation.
	SDL_SetClipRect(_screen, nullptr);
	SDL_FillRect(_screen, nullptr, SDL_MapRGBA(_screen->format, 0, 0, 0, 255));
	const HdCanvasTransform transform{double(logicalToPhysicalX(0)), double(logicalToPhysicalY(0)),
		getRenderScaleX(), getRenderScaleY()};
	HdCanvasRenderer::render(frame, _hdCanvasImages, _screen, transform);
	auto &gpu = HdGpuBackend::instance();
	// This upload contains only the just-rendered HD scene at physical resolution.
	// beginFrameLogical and Screen::flip are intentionally not part of this path.
	if (!gpu.beginFrame(_screen) || !gpu.present())
	{
		gpu.abortFrame();
		throw Exception("[HD PRESENTATION ERROR] HD scene presentation failed; caller must use traced fallback");
	}
}

void Screen::fadeHdCanvas(double opacity)
{
	if (_lastHdCanvas) presentHdCanvas(*_lastHdCanvas, opacity);
}

void Screen::clearHdCanvas()
{
	presentHdCanvas(HdCanvas(_baseWidth, _baseHeight));
	_hdCanvasImages.clear();
}

static const int VIDEO_WINDOW_POS_LEN = 40;
static char VIDEO_WINDOW_POS[VIDEO_WINDOW_POS_LEN];

static const char* SDL_VIDEO_CENTERED_UNSET = "SDL_VIDEO_CENTERED=";
static const char* SDL_VIDEO_CENTERED_CENTER = "SDL_VIDEO_CENTERED=center";
static const char* SDL_VIDEO_WINDOW_POS_UNSET = "SDL_VIDEO_WINDOW_POS=";

/**
 * Sets up all the internal display flags depending on
 * the current video settings.
 */
void Screen::makeVideoFlags()
{
	_flags = SDL_HWSURFACE|SDL_DOUBLEBUF|SDL_HWPALETTE;
	if (Options::asyncBlit)
	{
		_flags |= SDL_ASYNCBLIT;
	}
	if (useOpenGL())
	{
		_flags = SDL_OPENGL;
		SDL_GL_SetAttribute( SDL_GL_RED_SIZE, 5 );
		SDL_GL_SetAttribute( SDL_GL_GREEN_SIZE, 5 );
		SDL_GL_SetAttribute( SDL_GL_BLUE_SIZE, 5 );
		SDL_GL_SetAttribute( SDL_GL_DEPTH_SIZE, 16 );
		SDL_GL_SetAttribute( SDL_GL_DOUBLEBUFFER, 1 );
	}
	if (Options::allowResize)
	{
		_flags |= SDL_RESIZABLE;
	}

	// Handle window positioning
	if (!Options::fullscreen && Options::rootWindowedMode)
	{
		snprintf(VIDEO_WINDOW_POS, VIDEO_WINDOW_POS_LEN, "SDL_VIDEO_WINDOW_POS=%d,%d", Options::windowedModePositionX, Options::windowedModePositionY);
		SDL_putenv(VIDEO_WINDOW_POS);
		SDL_putenv((char *)SDL_VIDEO_CENTERED_UNSET);
	}
	else if (Options::borderless)
	{
		SDL_putenv((char *)SDL_VIDEO_WINDOW_POS_UNSET);
		SDL_putenv((char *)SDL_VIDEO_CENTERED_CENTER);
	}
	else
	{
		SDL_putenv((char *)SDL_VIDEO_WINDOW_POS_UNSET);
		SDL_putenv((char *)SDL_VIDEO_CENTERED_UNSET);
	}

	// Handle display mode
	if (Options::fullscreen)
	{
		_flags |= SDL_FULLSCREEN;
	}
	if (Options::borderless)
	{
		_flags |= SDL_NOFRAME;
	}

	// The full-body HD prototype composites a true RGBA sprite after the
	// legacy 8-bit Battlescape map has been blitted, so its internal screen
	// buffer must be true-colour even when no scaler/OpenGL path requested it.
	_bpp = (!_forceLegacy8Bit && (Options::hdGraphics || use32bitScaler() || useOpenGL())) ? 32 : 8;
	_baseWidth = Options::baseXResolution;
	_baseHeight = Options::baseYResolution;
}


/**
 * Initializes a new display screen for the game to render contents to.
 * The screen is set up based on the current options.
 */
Screen::Screen() : _baseWidth(ORIGINAL_WIDTH), _baseHeight(ORIGINAL_HEIGHT), _scaleX(1.0), _scaleY(1.0), _flags(0), _numColors(0), _firstColor(0), _pushPalette(false), _flickerFix(false), _forceLegacy8Bit(false)
{
	_flickerFix = Options::oxceEnablePaletteFlickerFix;

	resetDisplay();
	memset(deferredPalette, 0, 256*sizeof(SDL_Color));
}

/**
 * Deletes the buffer from memory. The display screen itself
 * is automatically freed once SDL shuts down.
 */
Screen::~Screen()
{

}

/**
 * Returns the screen's internal buffer surface. Any
 * contents that need to be shown will be blitted to this.
 * @return Pointer to the buffer surface.
 */
SDL_Surface *Screen::getSurface()
{
	_pushPalette = true;
	return _surface.get();
}

/**
 * Handles screen key shortcuts.
 * @param action Pointer to an action.
 */
void Screen::handle(Action *action)
{
	if (Options::debug)
	{
		if (action->getDetails()->type == SDL_KEYDOWN && action->getDetails()->key.keysym.sym == SDLK_F8 && (SDL_GetModState() & KMOD_ALT) != 0)
		{
			switch(Timer::gameSlowSpeed)
			{
				case 1: Timer::gameSlowSpeed = 5; break;
				case 5: Timer::gameSlowSpeed = 15; break;
				default: Timer::gameSlowSpeed = 1; break;
			}
		}
	}

	if (action->getDetails()->type == SDL_KEYDOWN && action->getDetails()->key.keysym.sym == SDLK_RETURN && (SDL_GetModState() & KMOD_ALT) != 0)
	{
		Options::fullscreen = !Options::fullscreen;
		resetDisplay();
	}
	else if (action->getDetails()->type == SDL_KEYDOWN && action->getDetails()->key.keysym.sym == Options::keyScreenshot)
	{
		std::ostringstream ss;
		int i = 0;
		do
		{
			ss.str("");
			ss << Options::getMasterUserFolder() << "screen" << std::setfill('0') << std::setw(3) << i << ".png";
			i++;
		}
		while (CrossPlatform::fileExists(ss.str()));
		screenshot(ss.str());
		return;
	}
}


/**
 * Renders the buffer's contents onto the screen, applying
 * any necessary filters or conversions in the process.
 * If the scaling factor is bigger than 1, the entire contents
 * of the buffer are resized by that factor (eg. 2 = doubled)
 * before being put on screen.
 */
void Screen::flip(const std::function<void(SDL_Surface*)> &physicalPass)
{
	if (Options::hdGraphics)
	{
		// REAL HD PIXEL FIREWALL: OXCE may update rules and state, but its
		// logical canvas is never uploaded, scaled or presented. Device failure
		// fails closed instead of silently restoring the native renderer.
		HdGpuBackend &gpu = HdGpuBackend::instance();
		if (!gpu.beginFrameHdOnly(_screen))
			throw Exception("REAL HD presentation unavailable: native display is forbidden");
		if (physicalPass)
		{
			SDL_Surface *overlay = gpu.cpuOverlay();
			if (!overlay)
			{
				gpu.abortFrame();
				throw Exception("REAL HD overlay unavailable: native display is forbidden");
			}
			physicalPass(overlay);
		}
		if (!gpu.present())
			throw Exception("REAL HD present failed: native display is forbidden");
		return;
	}
	// perform any requested palette update
	if (_flickerFix && _pushPalette && _numColors && _screen->format->BitsPerPixel == 8)
	{
		if (_screen->format->BitsPerPixel == 8 && SDL_SetColors(_screen, &(deferredPalette[_firstColor]), _firstColor, _numColors) == 0)
		{
			Log(LOG_DEBUG) << "Display palette doesn't match requested palette";
		}
		_numColors = 0;
		_pushPalette = false;
	}

	HdGpuBackend &hdGpu = HdGpuBackend::instance();
	bool hdGpuFrame = false;
	bool hdGpuLogicalBase = false;
	bool softwareBaseReady = false;
	auto buildSoftwareBase = [&]()
	{
		if (softwareBaseReady) return;
		const uint64_t perfScaleStart = hdPerfNowUs();
		if (getWidth() != _baseWidth || getHeight() != _baseHeight || useOpenGL())
		{
			Zoom::flipWithZoom(_surface.get(), _screen, _topBlackBand, _bottomBlackBand, _leftBlackBand, _rightBlackBand, &glOutput);
		}
		else
		{
			SDL_BlitSurface(_surface.get(), 0, _screen, 0);
		}
		getHdPerfStats().current.scaleUs += hdPerfNowUs() - perfScaleStart;
		getHdPerfStats().current.cpuScaleBypassed = false;
		softwareBaseReady = true;
	};

	// Renderer Architecture V1: D3D11 now owns the logical->physical presentation
	// transform on the normal HD path. OXCE still rasterizes its logical canvas,
	// but the expensive full-frame CPU Zoom::flipWithZoom is no longer required
	// before the GPU can render the HD world. This is deliberately behavior-
	// preserving: the same logical canvas, letterbox geometry, input coordinates
	// and physical HUD restore pass remain in force.
	const bool canUseHdGpu = !useOpenGL() && Options::hdGraphics && physicalPass &&
		_screen && _surface && _screen->format->BitsPerPixel == 32 && _surface->format->BitsPerPixel == 32;
	if (canUseHdGpu)
	{
		const uint64_t gpuStart = hdPerfNowUs();
		hdGpuFrame = hdGpu.beginFrameLogical(_screen, _surface.get(), _presentation);
		getHdPerfStats().current.gpuOverlayUs += hdPerfNowUs() - gpuStart;
		if (hdGpuFrame)
		{
			hdGpuLogicalBase = true;
			getHdPerfStats().current.cpuScaleBypassed = true;
		}
	}

	// If the direct GPU presentation path is unavailable, retain the exact stable
	// historical path and optionally seed D3D11 from the already-scaled software
	// framebuffer. This is also the automatic device/shader fallback.
	if (!hdGpuFrame)
	{
		if (Options::hdGraphics) hdTraceRoute("presentation", "frame", "LEGACY_NATIVE",
            canUseHdGpu ? "beginFrameLogical failed" : "HD presentation unavailable or caller has no HD pass", true);
		buildSoftwareBase();
		if (canUseHdGpu)
		{
			const uint64_t gpuStart = hdPerfNowUs();
			hdGpuFrame = hdGpu.beginFrame(_screen);
			getHdPerfStats().current.gpuOverlayUs += hdPerfNowUs() - gpuStart;
		}
	}

	// Physical HD stays after the logical render. Software writes directly to the
	// final framebuffer on fallback. D3D11 uses a transparent native-resolution
	// staging layer for HUD/popups/cursor, composited after the world.
	if (physicalPass && _screen)
	{
		const uint64_t perfPhysicalStart = hdPerfNowUs();
		if (hdGpuFrame)
		{
			SDL_Surface *overlay = hdGpu.cpuOverlay();
			if (overlay)
			{
				physicalPass(overlay);
			}
			else
			{
				hdTraceRoute("presentation", "frame", "LEGACY_NATIVE", "HD overlay unavailable", true);
				hdGpu.abortFrame();
				hdGpuFrame = false;
				if (hdGpuLogicalBase) buildSoftwareBase();
				physicalPass(_screen);
			}
		}
		else if (useOpenGL())
		{
#ifndef __NO_OPENGL
			if (_glPhysicalOverlayW != _screen->w || _glPhysicalOverlayH != _screen->h || !_glPhysicalOverlaySurface)
			{
				_glPhysicalOverlayW = _screen->w;
				_glPhysicalOverlayH = _screen->h;
				_glPhysicalOverlayBuffer = Surface::NewAlignedBuffer(32, _glPhysicalOverlayW, _glPhysicalOverlayH);
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
				const Uint32 rmask = 0xff000000, gmask = 0x00ff0000, bmask = 0x0000ff00, amask = 0x000000ff;
#else
				const Uint32 rmask = 0x000000ff, gmask = 0x0000ff00, bmask = 0x00ff0000, amask = 0xff000000;
#endif
				SDL_Surface *raw = SDL_CreateRGBSurfaceFrom(_glPhysicalOverlayBuffer.get(), _glPhysicalOverlayW, _glPhysicalOverlayH, 32, _glPhysicalOverlayW * 4, rmask, gmask, bmask, amask);
				if (!raw) throw Exception(SDL_GetError());
				_glPhysicalOverlaySurface = Surface::NewSdlSurface(raw);
			}
			memset(_glPhysicalOverlaySurface->pixels, 0, (size_t)_glPhysicalOverlaySurface->pitch * _glPhysicalOverlaySurface->h);
			physicalPass(_glPhysicalOverlaySurface.get());
			const uint64_t gpuStart = hdPerfNowUs();
			glOutput.drawPhysicalOverlay(_glPhysicalOverlaySurface.get(), _screen->w, _screen->h);
			getHdPerfStats().current.gpuOverlayUs = hdPerfNowUs() - gpuStart;
#endif
		}
		else if (_screen->format->BitsPerPixel == 32)
		{
			physicalPass(_screen);
		}
		getHdPerfStats().current.physicalPassUs = hdPerfNowUs() - perfPhysicalStart;
	}

	// perform any requested palette update
	if (!_flickerFix && _pushPalette && _numColors && _screen->format->BitsPerPixel == 8)
	{
		if (_screen->format->BitsPerPixel == 8 && SDL_SetColors(_screen, &(deferredPalette[_firstColor]), _firstColor, _numColors) == 0)
		{
			Log(LOG_DEBUG) << "Display palette doesn't match requested palette";
		}
		_numColors = 0;
		_pushPalette = false;
	}

	const uint64_t perfFlipStart = hdPerfNowUs();
	bool hdGpuPresented = false;
	if (hdGpuFrame)
	{
		const uint64_t gpuStart = hdPerfNowUs();
		hdGpuPresented = hdGpu.present();
		getHdPerfStats().current.gpuOverlayUs += hdPerfNowUs() - gpuStart;
		if (!hdGpuPresented)
		{
			hdTraceRoute("presentation", "frame", "LEGACY_NATIVE", "GPU present failed", true);
			// Device/swap failure: if D3D11 owned the base transform, reconstruct the
			// exact stable software framebuffer now, then re-run physical composition.
			hdGpu.abortFrame();
			if (hdGpuLogicalBase) buildSoftwareBase();
			if (physicalPass && _screen->format->BitsPerPixel == 32) physicalPass(_screen);
		}
	}
	if (!hdGpuPresented)
	{
		if (useOpenGL())
		{
#ifndef __NO_OPENGL
			SDL_GL_SwapBuffers();
#endif
		}
		else if (SDL_Flip(_screen) == -1)
		{
			throw Exception(SDL_GetError());
		}
	}
	getHdPerfStats().current.sdlFlipUs = hdPerfNowUs() - perfFlipStart;
}

double Screen::getRenderScaleX() const
{
	return _presentation.scaleX();
}

double Screen::getRenderScaleY() const
{
	return _presentation.scaleY();
}

int Screen::logicalToPhysicalX(double x) const
{
	return _presentation.logicalToPhysicalX(x);
}

int Screen::logicalToPhysicalY(double y) const
{
	return _presentation.logicalToPhysicalY(y);
}

bool Screen::blitNativeSurfaceAt(Surface *surface, SDL_Surface *destination, int x, int y, int w, int h) const
{
	if (Options::hdGraphics) return false; // REAL HD pixel firewall.
    if (!surface || !destination || w <= 0 || h <= 0) return false;
    const int ds = std::max(1, surface->getDisplayScale());
    const double sx = double(w) / std::max(1, surface->getWidth() * ds);
    const double sy = double(h) / std::max(1, surface->getHeight() * ds);
    // These exact classes expose their complete raster. Do not include compound
    // controls (ComboBox, Slider, TextList...) whose children need native blit().
    if (typeid(*surface) == typeid(Surface) || typeid(*surface) == typeid(InteractiveSurface) ||
        typeid(*surface) == typeid(Text) || typeid(*surface) == typeid(TextButton) ||
        typeid(*surface) == typeid(Cursor) || typeid(*surface) == typeid(FpsCounter))
    {
        auto src = surface->getPresentationSurface();
        if (!src) return false;
        auto &gpu = HdGpuBackend::instance();
        if (gpu.isCpuOverlay(destination) && gpu.drawLegacySurface(src, surface, x, y, w, h, surface->getDisplayAlpha())) return true;
        auto raw = zoomSurface(src, double(w) / src->w, double(h) / src->h, 0);
        if (!raw) return false;
        std::unique_ptr<SDL_Surface, void(*)(SDL_Surface*)> scaled(raw, SDL_FreeSurface);
        if (src->flags & SDL_SRCCOLORKEY) SDL_SetColorKey(raw, SDL_SRCCOLORKEY, src->format->colorkey);
        if (surface->getDisplayAlpha() < 255) SDL_SetAlpha(raw, SDL_SRCALPHA, surface->getDisplayAlpha());
        if (gpu.isCpuOverlay(destination)) return gpu.blitCpuOverlay(raw, x, y);
        SDL_Rect dst = {(Sint16)x, (Sint16)y, 0, 0};
        return SDL_BlitSurface(raw, nullptr, destination, &dst) == 0;
    }
    const int cw = std::max(640, std::max(_baseWidth, surface->getDisplayX() + surface->getWidth() * ds));
    const int ch = std::max(360, std::max(_baseHeight, surface->getDisplayY() + surface->getHeight() * ds));
    auto raw = SDL_CreateRGBSurface(SDL_SWSURFACE, cw, ch, 32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000);
    if (!raw) return false;
    std::unique_ptr<SDL_Surface, void(*)(SDL_Surface*)> logical(raw, SDL_FreeSurface);
    SDL_FillRect(raw, nullptr, 0);
    surface->blit(raw);
    SDL_SetAlpha(raw, SDL_SRCALPHA, 255);
    auto scaled = zoomSurface(raw, sx, sy, 0);
    if (!scaled) return false;
    std::unique_ptr<SDL_Surface, void(*)(SDL_Surface*)> physical(scaled, SDL_FreeSurface);
    SDL_SetAlpha(scaled, SDL_SRCALPHA, 255);
    const int px = int(std::lround(x - surface->getDisplayX() * sx));
    const int py = int(std::lround(y - surface->getDisplayY() * sy));
    auto &gpu = HdGpuBackend::instance();
    if (gpu.isCpuOverlay(destination)) return gpu.blitCpuOverlay(scaled, px, py);
    SDL_Rect dst = {(Sint16)px, (Sint16)py, 0, 0};
    return SDL_BlitSurface(scaled, nullptr, destination, &dst) == 0;
}

bool Screen::tryBlitHdSurfaceAt(Surface *surface, SDL_Surface *destination, int x, int y, int w, int h) const
{
    if (!Options::hdGraphics || !surface || !surface->isDisplayVisible() || !destination || w <= 0 || h <= 0) return false;
    if (!_hdUiWidgetsEnabled)
    {
        if (_hdTraceContext == "Global:FPS-Cursor")
            hdTraceRoute("ui-policy", _hdTraceContext, "LEGACY_NATIVE", "R4 native global controls", true);
        return false; // Planned native UI, no HD attempt/exception per widget.
    }
    // A failed producer is not retried on every frame. Resource-cache reset or
    // a newly created Surface permits a fresh attempt; diagnostics remain deduplicated.
    if (surface->hasHdPresentationFailure(_hdCanvasImages.generation())) return false;
    // Cursor coordinates are not a resource identity: moving it must not grow
    // the route registry or append a new log line for each position.
    const bool cursor = typeid(*surface) == typeid(Cursor);
    const std::string identity = _hdTraceContext + ":" + std::string(typeid(*surface).name()) + ":" + surface->getHdResourceId() +
        (cursor ? "" : "@" + std::to_string(surface->getX()) + "," + std::to_string(surface->getY()));
    auto &cache = _hdUiRasterCache;
    cache.generation(_hdCanvasImages.generation());
    const Uint32 now = SDL_GetTicks();
    if (!cache.reportTick) cache.reportTick = now;
    if (Uint32(now - cache.reportTick) >= 5000)
    {
        Log(LOG_INFO) << "[HD UI PERF P2.1] intervalMs=" << Uint32(now - cache.reportTick)
            << " hits=" << cache.hits << " misses=" << cache.misses << " rasters=" << cache.rasters
            << " rasterPixels=" << cache.rasterPixels << " composeMs=" << cache.composeMs
            << " rasterMs=" << cache.rasterMs << " submitMs=" << cache.submitMs
            << " cacheBytes=" << cache.bytes() << " entries=" << cache.size() << " evictions=" << cache.evictions;
        cache.hits = cache.misses = cache.rasters = cache.rasterPixels = cache.evictions = 0;
        cache.composeMs = cache.rasterMs = cache.submitMs = 0;
        cache.reportTick = now;
    }
    try
    {
        const int scale = std::max(1, surface->getDisplayScale());
        const int logicalW = surface->getWidth() * scale, logicalH = surface->getHeight() * scale;
        if (logicalW <= 0 || logicalH <= 0) return true;
        HdCanvas scene(std::max(640, std::max(_baseWidth, surface->getDisplayX() + logicalW)),
            std::max(360, std::max(_baseHeight, surface->getDisplayY() + logicalH)));
        const Uint32 composeStart = SDL_GetTicks();
        surface->composeHd(scene, _hdCanvasImages);
        cache.composeMs += Uint32(SDL_GetTicks() - composeStart);
        const double sx = double(w) / logicalW, sy = double(h) / logicalH;
        const double originX = x - surface->getDisplayX() * sx;
        const double originY = y - surface->getDisplayY() * sy;
        // Compound widgets (notably dropdown lists) extend beyond their button.
        // Include every top-level HD layer rather than clipping to that button.
        double left = surface->getDisplayX(), top = surface->getDisplayY();
        double right = left + logicalW, bottom = top + logicalH;
        for (const auto &command : scene.commands())
        {
            const HdRect bounds = command.op == HdCanvasOp::Layer
                ? command.transform.rect(command.layer->bounds()) : command.bounds;
            left = std::min(left, bounds.x); top = std::min(top, bounds.y);
            right = std::max(right, bounds.x + bounds.w); bottom = std::max(bottom, bounds.y + bounds.h);
        }
        x = std::max(0, int(std::floor(originX + left * sx)));
        y = std::max(0, int(std::floor(originY + top * sy)));
        w = std::min(destination->w, int(std::ceil(originX + right * sx))) - x;
        h = std::min(destination->h, int(std::ceil(originY + bottom * sy))) - y;
        if (w <= 0 || h <= 0) return true;
        const HdCanvasTransform rasterTransform{originX - x, originY - y, sx, sy};
        HdCanvasSignature key;
        key.text(_hdTraceContext); key.scalar(w); key.scalar(h);
        key.scalar(destination->format->Rmask); key.scalar(destination->format->Gmask);
        key.scalar(destination->format->Bmask); key.scalar(destination->format->Amask);
        key.transform(rasterTransform); key.canvas(scene);
        std::string signature = key.take();
        HdUiRasterCache::SurfacePtr target;
        if (const auto *cached = cache.find(surface, signature))
        {
            if (!cached->error.empty())
            {
                hdTraceRoute("widget", identity, "LEGACY_NATIVE", cached->error, true);
                return false;
            }
            target = cached->surface;
        }
        else
        {
            // Rebuild only when the complete HD command content changes.
            // Hover/pressed state, text, palette, crop, child layers, clipping,
            // scale and image treatment are all part of the exact signature.
            auto raw = SDL_CreateRGBSurface(SDL_SWSURFACE, w, h, 32,
                destination->format->Rmask, destination->format->Gmask,
                destination->format->Bmask, destination->format->Amask ? destination->format->Amask :
                ~(destination->format->Rmask | destination->format->Gmask | destination->format->Bmask));
            if (!raw) throw Exception(SDL_GetError());
            target = HdUiRasterCache::SurfacePtr(raw, SDL_FreeSurface);
            SDL_FillRect(raw, nullptr, SDL_MapRGBA(raw->format, 0, 0, 0, 0));
            const Uint32 rasterStart = SDL_GetTicks();
            ++cache.rasters; cache.rasterPixels += std::uint64_t(w) * h;
            try { HdCanvasRenderer::render(scene, _hdCanvasImages, raw, rasterTransform); }
            catch (const std::exception &error)
            {
                cache.rasterMs += Uint32(SDL_GetTicks() - rasterStart);
                cache.store(surface, std::move(signature), {}, error.what());
                throw;
            }
            cache.rasterMs += Uint32(SDL_GetTicks() - rasterStart);
            SDL_SetAlpha(raw, SDL_SRCALPHA, 255);
            cache.store(surface, std::move(signature), target);
        }
        const Uint32 submitStart = SDL_GetTicks();
        auto raw = target.get();
        auto &gpu = HdGpuBackend::instance();
        if (gpu.isCpuOverlay(destination))
        {
            if (!gpu.blitCpuOverlay(raw, x, y)) throw Exception("Cannot submit HD widget");
        }
        else
        {
            SDL_Rect position = {(Sint16)x, (Sint16)y, 0, 0};
            if (SDL_BlitSurface(raw, nullptr, destination, &position) != 0) throw Exception(SDL_GetError());
        }
        cache.submitMs += Uint32(SDL_GetTicks() - submitStart);
        hdTraceRoute("widget", identity, "HD_PIPELINE", "HD producer completed");
        return true;
    }
    catch (const std::exception &error)
    {
        surface->latchHdPresentationFailure(_hdCanvasImages.generation(), error.what());
        hdTraceRoute("widget", identity, "LEGACY_NATIVE", error.what(), true);
        return false;
    }
}

void Screen::blitSurfacePhysical(Surface *surface, bool cursorCoordinates, SDL_Surface *destination) const
{
	SDL_Surface *target = destination ? destination : _screen;
	if (!surface || !_screen || !target || target->format->BitsPerPixel != 32 || !surface->isDisplayVisible()) return;
	if (surface->getWidth() <= 0 || surface->getHeight() <= 0) return;

	// V1-C2 R2 cursor cutover: cursor POSITION still follows the real physical
	// mouse through the current Screen scale/bands, but cursor SIZE belongs to
	// presentation/UI space and must not shrink/grow with Battlescape world zoom.
	const double positionScaleX = cursorCoordinates ? _scaleX : getRenderScaleX();
	const double positionScaleY = cursorCoordinates ? _scaleY : getRenderScaleY();
	double sizeScaleX = positionScaleX;
	double sizeScaleY = positionScaleY;
	if (cursorCoordinates)
	{
		const PresentationTransform cursorUi = PresentationSpacesContract::uniformUiFit(
			640, 360, _screen->w, _screen->h);
		sizeScaleX = cursorUi.scaleX;
		sizeScaleY = cursorUi.scaleY;

		static double lastPositionScaleX = -1.0;
		static double lastPositionScaleY = -1.0;
		static double lastSizeScaleX = -1.0;
		static double lastSizeScaleY = -1.0;
		if (std::fabs(lastPositionScaleX - positionScaleX) > 0.000001 ||
			std::fabs(lastPositionScaleY - positionScaleY) > 0.000001 ||
			std::fabs(lastSizeScaleX - sizeScaleX) > 0.000001 ||
			std::fabs(lastSizeScaleY - sizeScaleY) > 0.000001)
		{
			Log(LOG_INFO) << "[PRESENTATION-SPACES CURSOR V1]"
				<< " positionScale=" << positionScaleX << "x" << positionScaleY
				<< " sizeScale=" << sizeScaleX << "x" << sizeScaleY
				<< " physical=" << _screen->w << "x" << _screen->h;
			lastPositionScaleX = positionScaleX;
			lastPositionScaleY = positionScaleY;
			lastSizeScaleX = sizeScaleX;
			lastSizeScaleY = sizeScaleY;
		}
	}
	const int bandX = cursorCoordinates ? _cursorLeftBlackBand : _leftBlackBand;
	const int bandY = cursorCoordinates ? _cursorTopBlackBand : _topBlackBand;
	const int presentation = std::max(1, surface->getDisplayScale());
	const int drawW = std::max(1, (int)std::floor((double)surface->getWidth() * presentation * sizeScaleX + 0.5));
	const int drawH = std::max(1, (int)std::floor((double)surface->getHeight() * presentation * sizeScaleY + 0.5));
	const int drawX = bandX + (int)std::floor((double)surface->getDisplayX() * positionScaleX + 0.5);
	const int drawY = bandY + (int)std::floor((double)surface->getDisplayY() * positionScaleY + 0.5);

	if (tryBlitHdSurfaceAt(surface, target, drawX, drawY, drawW, drawH)) return;
	if (Options::hdGraphics)
	{
		// An unmigrated widget is absent, never replaced by OXCE pixels.
		hdTraceRoute("pixel-firewall", _hdTraceContext, "REAL_HD_MISSING",
			"Native widget pixels forbidden in REAL HD", true);
		return;
	}
	if (blitNativeSurfaceAt(surface, target, drawX, drawY, drawW, drawH)) return;
	hdTraceRoute("widget", typeid(*surface).name(), "LEGACY_NATIVE", "Compound fallback allocation failed; parent raster only", true);
	SDL_Surface *src = surface->getPresentationSurface();
	if (!src) return;
	// Presentation Pipeline V1: on the D3D11 path, ordinary OXCE surfaces are
	// themselves valid renderer inputs. Keep their Legacy pixels/palette exactly
	// as authored, but let the GPU perform the logical->physical nearest scaling
	// instead of allocating a temporary zoomSurface on the CPU every frame.
	HdGpuBackend &gpu = HdGpuBackend::instance();
	if (gpu.isCpuOverlay(target) && gpu.drawLegacySurface(src, surface,
		drawX, drawY, drawW, drawH, surface->getDisplayAlpha()))
	{
		return;
	}

	++getHdPerfStats().current.physicalZoomSurfaces;
	SDL_Surface *scaled = zoomSurface(src, (double)drawW / (double)src->w, (double)drawH / (double)src->h, 0);
	if (!scaled) return;
	if (src->flags & SDL_SRCCOLORKEY)
	{
		SDL_SetColorKey(scaled, SDL_SRCCOLORKEY, src->format->colorkey);
	}
	if (surface->getDisplayAlpha() < 255)
	{
		SDL_SetAlpha(scaled, SDL_SRCALPHA, surface->getDisplayAlpha());
	}
	SDL_Rect dst = { (Sint16)drawX, (Sint16)drawY, 0, 0 };
	if (gpu.isCpuOverlay(target))
	{
		gpu.blitCpuOverlay(scaled, drawX, drawY);
	}
	else
	{
		SDL_BlitSurface(scaled, nullptr, target, &dst);
	}
	SDL_FreeSurface(scaled);
}

/**
 * Clears all the contents out of the internal buffer.
 */
void Screen::clear()
{
	Surface::CleanSdlSurface(_surface.get());
	Surface::CleanSdlSurface(_screen);
}

/**
 * Changes the 8bpp palette used to render the screen's contents.
 * @param colors Pointer to the set of colors.
 * @param firstcolor Offset of the first color to replace.
 * @param ncolors Amount of colors to replace.
 * @param immediately Apply palette changes immediately, otherwise wait for next blit.
 */
void Screen::setPalette(const SDL_Color* colors, int firstcolor, int ncolors, bool immediately)
{
	if (_numColors && (_numColors != ncolors) && (_firstColor != firstcolor))
	{
		// an initial palette setup has not been committed to the screen yet
		// just update it with whatever colors are being sent now
		memmove(&(deferredPalette[firstcolor]), colors, sizeof(SDL_Color)*ncolors);
		_numColors = 256; // all the use cases are just a full palette with 16-color follow-ups
		_firstColor = 0;
	}
	else
	{
		memmove(&(deferredPalette[firstcolor]), colors, sizeof(SDL_Color) * ncolors);
		_numColors = ncolors;
		_firstColor = firstcolor;
	}

	SDL_SetColors(_surface.get(), const_cast<SDL_Color *>(colors), firstcolor, ncolors);

	// defer actual update of screen until SDL_Flip()
	if (immediately && _screen->format->BitsPerPixel == 8 && SDL_SetColors(_screen, const_cast<SDL_Color *>(colors), firstcolor, ncolors) == 0)
	{
		Log(LOG_DEBUG) << "Display palette doesn't match requested palette";
	}

	// Sanity check
	/*
	SDL_Color *newcolors = _screen->format->palette->colors;
	for (int i = firstcolor, j = 0; i < firstcolor + ncolors; i++, j++)
	{
		Log(LOG_DEBUG) << (int)newcolors[i].r << " - " << (int)newcolors[i].g << " - " << (int)newcolors[i].b;
		Log(LOG_DEBUG) << (int)colors[j].r << " + " << (int)colors[j].g << " + " << (int)colors[j].b;
		if (newcolors[i].r != colors[j].r ||
			newcolors[i].g != colors[j].g ||
			newcolors[i].b != colors[j].b)
		{
			Log(LOG_ERROR) << "Display palette doesn't match requested palette";
			break;
		}
	}
	*/
}

/**
 * Returns the screen's 8bpp palette.
 * @return Pointer to the palette's colors.
 */
SDL_Color *Screen::getPalette() const
{
	return (SDL_Color*)deferredPalette;
}

/**
 * Returns the width of the screen.
 * @return Width in pixels.
 */
int Screen::getWidth() const
{
	return _screen->w;
}

/**
 * Returns the height of the screen.
 * @return Height in pixels
 */
int Screen::getHeight() const
{
	return _screen->h;
}

void Screen::setForceLegacy8Bit(bool force, bool resetVideo)
{
	if (_forceLegacy8Bit == force) return;
	_forceLegacy8Bit = force;
	resetDisplay(resetVideo);
}

/**
 * Resets the screen surfaces based on the current display options,
 * as they don't automatically take effect.
 * @param resetVideo Reset display surface.
 */
void Screen::resetDisplay(bool resetVideo, bool noShaders)
{
#if defined __linux__ || defined _WIN32 || defined  __CYGWIN__
	Uint32 oldFlags = _flags;
#endif

	int width = Options::displayWidth;
	int height = Options::displayHeight;
	makeVideoFlags();

	if (!_surface || (_surface->format->BitsPerPixel != _bpp ||
		_surface->w != _baseWidth ||
		_surface->h != _baseHeight)) // don't reallocate _surface if not necessary, it's a waste of CPU cycles
	{
		if (_bpp == 32)
		{
			std::tie(_buffer, _surface) = Surface::NewPair32Bit(_baseWidth, _baseHeight);
		}
		else
		{
			std::tie(_buffer, _surface) = Surface::NewPair8Bit(_baseWidth, _baseHeight);
		}

		if (_surface->format->BitsPerPixel == 8)
		{
			SDL_SetColors(_surface.get(), deferredPalette, 0, 255);
		}
	}
	SDL_SetColorKey(_surface.get(), 0, 0); // turn off color key!

	if (resetVideo || _screen->format->BitsPerPixel != _bpp)
	{
		Log(LOG_INFO) << "Attempting to set display to " << width << "x" << height << "x" << _bpp << "...";

#if defined __linux__ || defined _WIN32 || defined  __CYGWIN__
		// Workaround for segfault when switching to opengl
		if ((oldFlags & SDL_OPENGL) != (_flags & SDL_OPENGL))
		{
			Uint8 cursor = 0;
			char *_oldtitle = 0;
			SDL_WM_GetCaption(&_oldtitle, NULL);
			std::string title(_oldtitle);
			SDL_QuitSubSystem(SDL_INIT_VIDEO);
			SDL_InitSubSystem(SDL_INIT_VIDEO);

			// recreate operations done by `Game::Game` constructor
			SDL_ShowCursor(SDL_ENABLE);
			SDL_EnableUNICODE(1);
			SDL_WM_SetCaption(title.c_str(), 0);
			SDL_WM_GrabInput(Options::captureMouse);
			SDL_SetCursor(SDL_CreateCursor(&cursor, &cursor, 1,1,0,0));
		}
#endif
		_screen = SDL_SetVideoMode(width, height, _bpp, _flags);
		if (_screen == 0)
		{
			Log(LOG_ERROR) << SDL_GetError();
			Log(LOG_INFO) << "Attempting to set display to default resolution...";
			_screen = SDL_SetVideoMode(640, 400, _bpp, _flags);
			if (_screen == 0)
			{
				if (_flags & SDL_OPENGL)
				{
					Options::useOpenGL = false;
				}
				throw Exception(SDL_GetError());
			}
		}
		Log(LOG_INFO) << "Display set to " << getWidth() << "x" << getHeight() << "x" << (int)_screen->format->BitsPerPixel << ".";
	}
	else
	{
		clear();
	}

	Options::displayWidth = getWidth();
	Options::displayHeight = getHeight();
	_scaleX = getWidth() / (double)_baseWidth;
	_scaleY = getHeight() / (double)_baseHeight;

	double pixelRatioY = 1.0;
	if (Options::nonSquarePixelRatio && !Options::allowResize)
	{
		pixelRatioY = 1.2;
	}
	bool cursorInBlackBands;
	if (!Options::keepAspectRatio)
	{
		cursorInBlackBands = false;
	}
	else if (Options::fullscreen)
	{
		cursorInBlackBands = Options::cursorInBlackBandsInFullscreen;
	}
	else if (!Options::borderless)
	{
		cursorInBlackBands = Options::cursorInBlackBandsInWindow;
	}
	else
	{
		cursorInBlackBands = Options::cursorInBlackBandsInBorderlessWindow;
	}

	if (_scaleX > _scaleY && Options::keepAspectRatio)
	{
		int targetWidth = (int)floor(_scaleY * (double)_baseWidth);
		_topBlackBand = _bottomBlackBand = 0;
		_leftBlackBand = (getWidth() - targetWidth) / 2;
		if (_leftBlackBand < 0)
		{
			_leftBlackBand = 0;
		}
		_rightBlackBand = getWidth() - targetWidth - _leftBlackBand;
		_cursorTopBlackBand = 0;

		if (cursorInBlackBands)
		{
			_scaleX = _scaleY;
			_cursorLeftBlackBand = _leftBlackBand;
		}
		else
		{
			_cursorLeftBlackBand = 0;
		}
	}
	else if (_scaleY > _scaleX && Options::keepAspectRatio)
	{
		int targetHeight = (int)floor(_scaleX * (double)_baseHeight * pixelRatioY);
		_topBlackBand = (getHeight() - targetHeight) / 2;
		if (_topBlackBand < 0)
		{
			_topBlackBand = 0;
		}
		_bottomBlackBand = getHeight() - targetHeight - _topBlackBand;
		if (_bottomBlackBand < 0)
		{
			_bottomBlackBand = 0;
		}
		_leftBlackBand = _rightBlackBand = 0;
		_cursorLeftBlackBand = 0;

		if (cursorInBlackBands)
		{
			_scaleY = _scaleX;
			_cursorTopBlackBand = _topBlackBand;
		}
		else
		{
			_cursorTopBlackBand = 0;
		}
	}
	else
	{
		_topBlackBand = _bottomBlackBand = _leftBlackBand = _rightBlackBand = _cursorTopBlackBand = _cursorLeftBlackBand = 0;
	}

	// Renderer Architecture V1: one authoritative description of the logical
	// canvas inside the physical display.  Map/GPU/UI presentation code must
	// consume this instead of independently rebuilding scale/band formulae.
	_presentation.configure(_baseWidth, _baseHeight, getWidth(), getHeight(),
		_leftBlackBand, _topBlackBand, _rightBlackBand, _bottomBlackBand);

	const PresentationRect &traceContent = _presentation.contentRect();
	Log(LOG_INFO) << "[PRESENTATION-SPACES TRACE V1][SCREEN] physical=" << getWidth() << "x" << getHeight()
		<< " logical=" << _baseWidth << "x" << _baseHeight
		<< " content=" << traceContent.x << "," << traceContent.y << "," << traceContent.w << "x" << traceContent.h
		<< " pcScale=" << _presentation.scaleX() << "x" << _presentation.scaleY()
		<< " rawScale=" << _scaleX << "x" << _scaleY
		<< " bands(LTRB)=" << _leftBlackBand << "," << _topBlackBand << "," << _rightBlackBand << "," << _bottomBlackBand
		<< " cursorBands(LT)=" << _cursorLeftBlackBand << "," << _cursorTopBlackBand
		<< " optionsBase=" << Options::baseXResolution << "x" << Options::baseYResolution
		<< " world(battle/geo)=" << Options::battlescapeScale << "/" << Options::geoscapeScale
		<< " ui(battle/geo/aqua)=" << Options::getBattleUiScale() << "/" << Options::getGeoUiScale() << "/" << Options::getAquanautUiScale();

	if (useOpenGL())
	{
#ifndef __NO_OPENGL
		OpenGL::checkErrors = Options::checkOpenGLErrors;
		glOutput.init(_baseWidth, _baseHeight);
		glOutput.linear = Options::useOpenGLSmoothing; // setting from shader file will override this, though
		if (!noShaders && FileMap::fileExists(Options::useOpenGLShader))
		{
			if (!glOutput.set_shader(Options::useOpenGLShader.c_str()))
			{
				Options::useOpenGLShader = "";
			}
		}
		glOutput.setVSync(Options::vSyncForOpenGL);
#endif
	}

	if (_screen->format->BitsPerPixel == 8)
	{
		setPalette(getPalette());
	}
}

/**
 * Returns the screen's X scale.
 * @return Scale factor.
 */
double Screen::getXScale() const
{
	return _scaleX;
}

/**
 * Returns the screen's Y scale.
 * @return Scale factor.
 */
double Screen::getYScale() const
{
	return _scaleY;
}

/**
 * Returns the screen's top black forbidden to cursor band's height.
 * @return Height in pixel.
 */
int Screen::getCursorTopBlackBand() const
{
	return _cursorTopBlackBand;
}

/**
 * Returns the screen's left black forbidden to cursor band's width.
 * @return Width in pixel.
 */
int Screen::getCursorLeftBlackBand() const
{
	return _cursorLeftBlackBand;
}

/**
 * Saves a screenshot of the screen's contents.
 * @param filename Filename of the PNG file.
 */
void Screen::screenshot(const std::string &filename)
{
	// The direct D3D11 presentation path deliberately leaves the SDL physical
	// framebuffer out of the hot frame loop. Refresh it only on an explicit
	// screenshot request; this preserves historical screenshot semantics without
	// paying the full-frame CPU scale cost every rendered frame.
	if (!useOpenGL() && Options::hdGraphics && useHdGpuPhysical() && _surface && _screen)
	{
		Zoom::flipWithZoom(_surface.get(), _screen, _topBlackBand, _bottomBlackBand, _leftBlackBand, _rightBlackBand, &glOutput);
	}
	SDL_Surface *screenshot = SDL_AllocSurface(0, getWidth() - getWidth()%4, getHeight(), 24, 0xff, 0xff00, 0xff0000, 0);

	if (useOpenGL())
	{
#ifndef __NO_OPENGL
		GLenum format = GL_RGB;

		for (int y = 0; y < getHeight(); ++y)
		{
			glReadPixels(0, getHeight()-(y+1), getWidth() - getWidth()%4, 1, format, GL_UNSIGNED_BYTE, ((Uint8*)screenshot->pixels) + y*screenshot->pitch);
		}
		glErrorCheck();
#endif
	}
	else
	{
		SDL_BlitSurface(_screen, 0, screenshot, 0);
	}
	std::vector<unsigned char> out;
	if (_screen->format->BitsPerPixel == 8 && Options::oxceRawScreenShots)
	{
		SDL_Color *palette = getPalette();
		lodepng::State state;
		for (size_t i = 0; i < 256; ++i)
		{
			SDL_Color color = palette[i];
			lodepng_palette_add(&state.info_png.color, color.r, color.g, color.b, 255);
			lodepng_palette_add(&state.info_raw, color.r, color.g, color.b, 255);
		}
		state.info_png.color.colortype = LCT_PALETTE; //if you comment this line, and create the above palette in info_raw instead, then you get the same image in a RGBA PNG.
		state.info_png.color.bitdepth = 8;
		state.info_raw.colortype = LCT_PALETTE;
		state.info_raw.bitdepth = 8;
		state.encoder.auto_convert = 0; //we specify ourselves exactly what output PNG color mode we want
		unsigned error = lodepng::encode(out, (const unsigned char *)(_surface->pixels), _surface->w, _surface->h, state);
		if (error)
		{
			Log(LOG_ERROR) << "Saving to PNG failed: " << lodepng_error_text(error);
		}
	}
	else
	{
		unsigned error = lodepng::encode(out, (const unsigned char *)(screenshot->pixels), getWidth() - getWidth()%4, getHeight(), LCT_RGB);
		if (error)
		{
			Log(LOG_ERROR) << "Saving to PNG failed: " << lodepng_error_text(error);
		}
	}

	SDL_FreeSurface(screenshot);

	CrossPlatform::writeFile(filename, out);
}


/**
 * Check whether a 32bpp scaler has been selected.
 * @return if it is enabled with a compatible resolution.
 */
bool Screen::use32bitScaler()
{
	int w = Options::displayWidth;
	int h = Options::displayHeight;
	int baseW = Options::baseXResolution;
	int baseH = Options::baseYResolution;
	int maxScale = 0;

	if (Options::useHQXFilter)
	{
		maxScale = 4;
	}
	else if (Options::useXBRZFilter)
	{
		maxScale = 6;
	}

	for (int i = 2; i <= maxScale; i++)
	{
		if (w == baseW * i && h == baseH * i)
		{
			return true;
		}
	}
	return false;
}

/**
 * Check if OpenGL is enabled.
 * @return if it is enabled.
 */
bool Screen::useOpenGL()
{
#ifdef __NO_OPENGL
	return false;
#else
	return Options::useOpenGL;
#endif
}

bool Screen::useHdGpuPhysical()
{
	return HdGpuBackend::instance().wasUsedLastFrame();
}

const char *Screen::hdGpuModeName()
{
	return HdGpuBackend::instance().lastModeName();
}

unsigned Screen::hdGpuMapDrawCalls()
{
	return HdGpuBackend::instance().lastMapDrawCalls();
}

unsigned Screen::hdGpuIndexedDrawCalls()
{
	return HdGpuBackend::instance().lastIndexedDrawCalls();
}

unsigned Screen::hdGpuEnvironmentDrawCalls()
{
	return HdGpuBackend::instance().lastEnvironmentDrawCalls();
}


/**
 * Gets the Horizontal offset from the mid-point of the screen, in pixels.
 * @return the horizontal offset.
 */
int Screen::getDX() const
{
	return (_baseWidth - ORIGINAL_WIDTH) / 2;
}

/**
 * Gets the Vertical offset from the mid-point of the screen, in pixels.
 * @return the vertical offset.
 */
int Screen::getDY() const
{
	return (_baseHeight - ORIGINAL_HEIGHT) / 2;
}

/**
 * Changes a given scale, and if necessary, switch the current base resolution.
 * @param type the new scale level.
 * @param width reference to which x scale to adjust.
 * @param height reference to which y scale to adjust.
 * @param change should we change the current scale.
 */
void Screen::updateScale(int type, int &width, int &height, bool change)
{
	const int traceInputW = width;
	const int traceInputH = height;
	const int traceBaseW = Options::baseXResolution;
	const int traceBaseH = Options::baseYResolution;
	double pixelRatioY = 1.0;

	if (Options::nonSquarePixelRatio)
	{
		pixelRatioY = 1.2;
	}

	switch (type)
	{
	case SCALE_15X:
		width = Screen::ORIGINAL_WIDTH * 1.5;
		height = Screen::ORIGINAL_HEIGHT * 1.5;
		break;
	case SCALE_2X:
		width = Screen::ORIGINAL_WIDTH * 2;
		height = Screen::ORIGINAL_HEIGHT * 2;
		break;
	case SCALE_SCREEN_DIV_10:
		width = Options::displayWidth / 10.0;
		height = Options::displayHeight / pixelRatioY / 10.0;
		break;
	case SCALE_SCREEN_DIV_8:
		width = Options::displayWidth / 8.0;
		height = Options::displayHeight / pixelRatioY / 8.0;
		break;
	case SCALE_SCREEN_DIV_6:
		width = Options::displayWidth / 6.0;
		height = Options::displayHeight / pixelRatioY / 6.0;
		break;
	case SCALE_SCREEN_DIV_5:
		width = Options::displayWidth / 5.0;
		height = Options::displayHeight / pixelRatioY / 5.0;
		break;
	case SCALE_SCREEN_DIV_4:
		width = Options::displayWidth / 4.0;
		height = Options::displayHeight / pixelRatioY / 4.0;
		break;
	case SCALE_SCREEN_DIV_3:
		width = Options::displayWidth / 3.0;
		height = Options::displayHeight / pixelRatioY / 3.0;
		break;
	case SCALE_SCREEN_DIV_2:
		width = Options::displayWidth / 2.0;
		height = Options::displayHeight / pixelRatioY  / 2.0;
		break;
	case SCALE_SCREEN:
		width = Options::displayWidth;
		height = Options::displayHeight / pixelRatioY;
		break;
	case SCALE_ORIGINAL:
	default:
		width = Screen::ORIGINAL_WIDTH;
		height = Screen::ORIGINAL_HEIGHT;
		break;
	}

	// don't go under minimum resolution... it's bad, mmkay?
	width = std::max(width, Screen::ORIGINAL_WIDTH);
	height = std::max(height, Screen::ORIGINAL_HEIGHT);

	if (change && (Options::baseXResolution != width || Options::baseYResolution != height))
	{
		Options::baseXResolution = width;
		Options::baseYResolution = height;
	}

	Log(LOG_INFO) << "[PRESENTATION-SPACES TRACE V1][UPDATE-SCALE] type=" << type
		<< " change=" << (change ? 1 : 0)
		<< " input=" << traceInputW << "x" << traceInputH
		<< " output=" << width << "x" << height
		<< " baseBefore=" << traceBaseW << "x" << traceBaseH
		<< " baseAfter=" << Options::baseXResolution << "x" << Options::baseYResolution
		<< " display=" << Options::displayWidth << "x" << Options::displayHeight;
}

}
