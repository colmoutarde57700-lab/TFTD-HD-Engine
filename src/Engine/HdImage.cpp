#include "HdImage.h"
#include "FileMap.h"
#include "Logger.h"
#include "../lodepng.h"
#include <algorithm>
#include <cstring>

namespace OpenXcom
{

bool HdImageCache::exists(const std::string &path)
{
	auto it = _exists.find(path);
	if (it != _exists.end()) return it->second;
	const bool found = FileMap::fileExists(path);
	_exists[path] = found;
	return found;
}

HdImage *HdImageCache::get(const std::string &path)
{
	auto &entry = _images[path];
	if (entry.loadAttempted)
	{
		return entry.loaded ? &entry : nullptr;
	}
	entry.loadAttempted = true;
	if (!exists(path)) return nullptr;

	SDL_RWops *rw = FileMap::getRWops(path);
	if (!rw)
	{
		Log(LOG_WARNING) << "HD asset could not be opened from VFS: " << path;
		return nullptr;
	}
	const Sint64 end = SDL_RWseek(rw, 0, RW_SEEK_END);
	if (end <= 0 || SDL_RWseek(rw, 0, RW_SEEK_SET) < 0)
	{
		SDL_RWclose(rw);
		Log(LOG_WARNING) << "HD asset could not be sized: " << path;
		return nullptr;
	}
	const size_t size = (size_t)end;
	std::vector<unsigned char> png(size);
	const size_t readBytes = SDL_RWread(rw, png.data(), 1, size);
	SDL_RWclose(rw);
	if (readBytes != size)
	{
		Log(LOG_WARNING) << "HD asset could not be read: " << path;
		return nullptr;
	}

	unsigned w = 0, h = 0;
	// P8 inspects the PNG without colour conversion first only to preserve its
	// storage representation. Storage is NOT colour semantics: palette PNGs
	// are also decoded to RGBA and default to Environment like any authored art.
	lodepng::State state;
	state.decoder.color_convert = 0;
	std::vector<unsigned char> raw;
	unsigned error = lodepng::decode(raw, w, h, state, png);
	if (error)
	{
		Log(LOG_WARNING) << "HD PNG decode failed (" << error << ") for " << path << ": " << lodepng_error_text(error);
		return nullptr;
	}
	entry.width = w;
	entry.height = h;
	entry.paletteIndexed8 = state.info_png.color.colortype == LCT_PALETTE && state.info_png.color.bitdepth == 8;

	if (entry.paletteIndexed8)
	{
		const size_t pixelCount = (size_t)w * h;
		if (raw.size() < pixelCount)
		{
			Log(LOG_WARNING) << "HD indexed PNG has unexpected raw size for " << path;
			return nullptr;
		}
		entry.indices.assign(raw.begin(), raw.begin() + pixelCount);
		entry.rgba.resize(pixelCount * 4u);
		const LodePNGColorMode &color = state.info_png.color;
		for (size_t i = 0; i < pixelCount; ++i)
		{
			const unsigned idx = entry.indices[i];
			const bool valid = idx < color.palettesize && color.palette;
			entry.rgba[i * 4u + 0] = valid ? color.palette[idx * 4u + 0] : 0;
			entry.rgba[i * 4u + 1] = valid ? color.palette[idx * 4u + 1] : 0;
			entry.rgba[i * 4u + 2] = valid ? color.palette[idx * 4u + 2] : 0;
			entry.rgba[i * 4u + 3] = valid ? color.palette[idx * 4u + 3] : 0;
		}
	}
	else
	{
		// The raw buffer may be RGB, grayscale or another PNG-native mode.
		// Decode once more to the canonical RGBA representation used by the
		// existing software compositor and by true-colour GPU textures.
		entry.rgba.clear();
		error = lodepng::decode(entry.rgba, w, h, png);
		if (error)
		{
			entry.rgba.clear();
			Log(LOG_WARNING) << "HD RGBA decode failed (" << error << ") for " << path << ": " << lodepng_error_text(error);
			return nullptr;
		}
	}

	// TEST8-B: build a conservative non-transparent span for each source row.
	// This costs a few KB per HD image and lets every later software blit skip
	// the large alpha=0 margins typical of isometric HD assets.
	entry.alphaRowMinX.assign(h, w);
	entry.alphaRowMaxX.assign(h, 0);
	entry.hasVisiblePixels = false;
	for (unsigned y = 0; y < h; ++y)
	{
		unsigned minX = w;
		unsigned maxX = 0;
		for (unsigned x = 0; x < w; ++x)
		{
			if (entry.rgba[((size_t)y * w + x) * 4 + 3] != 0)
			{
				minX = std::min(minX, x);
				maxX = std::max(maxX, x + 1);
			}
		}
		entry.alphaRowMinX[y] = minX;
		entry.alphaRowMaxX[y] = maxX;
		if (minX < maxX) entry.hasVisiblePixels = true;
	}

	entry.loaded = true;
	Log(LOG_INFO) << "[HD] loaded " << path << " -> " << w << "x" << h
		<< (entry.paletteIndexed8 ? " PALETTE8+RGBA" : " RGBA");
	return &entry;
}

void HdImageCache::clear()
{
	_images.clear();
	_exists.clear();
}

void HdImageCache::blit(SDL_Surface *destination, const HdImage &image,
	int x, int y, int drawWidth, int drawHeight,
	Uint8 opacity, int shade, const SDL_Rect *clip, bool rightHalfOnly)
{
	if (!destination || destination->format->BitsPerPixel != 32 || !image.loaded || image.width == 0 || image.height == 0) return;
	if (drawWidth <= 0 || drawHeight <= 0 || opacity == 0 || !image.hasVisiblePixels) return;

	const int clipLeft = clip ? clip->x : 0;
	const int clipTop = clip ? clip->y : 0;
	const int clipRight = clip ? clip->x + clip->w : destination->w;
	const int clipBottom = clip ? clip->y + clip->h : destination->h;
	const int startX = rightHalfOnly ? drawWidth / 2 : 0;
	shade = std::max(0, std::min(16, shade));
	const unsigned light = (unsigned)(16 - shade); // 16 = native, 0 = black

	// TEST8-B: compute the expensive horizontal source mapping once per blit,
	// not once for every destination pixel on every row.
	std::vector<unsigned> xSource((size_t)drawWidth);
	for (int xx = 0; xx < drawWidth; ++xx)
	{
		xSource[(size_t)xx] = std::min(image.width - 1,
			(unsigned)((unsigned long long)xx * image.width / (unsigned)drawWidth));
	}

	SDL_PixelFormat *fmt = destination->format;
	const bool fast32 = fmt && fmt->BytesPerPixel == 4 &&
		fmt->Rmask && fmt->Gmask && fmt->Bmask &&
		fmt->Rloss == 0 && fmt->Gloss == 0 && fmt->Bloss == 0 &&
		(!fmt->Amask || fmt->Aloss == 0);

	auto packFast = [fmt](Uint8 r, Uint8 g, Uint8 b, Uint8 a) -> Uint32
	{
		Uint32 out =
			(((Uint32)r << fmt->Rshift) & fmt->Rmask) |
			(((Uint32)g << fmt->Gshift) & fmt->Gmask) |
			(((Uint32)b << fmt->Bshift) & fmt->Bmask);
		if (fmt->Amask) out |= (((Uint32)a << fmt->Ashift) & fmt->Amask);
		return out;
	};

	if (SDL_MUSTLOCK(destination) && SDL_LockSurface(destination) != 0) return;
	for (int yy = 0; yy < drawHeight; ++yy)
	{
		const int dy = y + yy;
		if (dy < clipTop || dy >= clipBottom || dy < 0 || dy >= destination->h) continue;

		const unsigned sy = std::min(image.height - 1,
			(unsigned)((unsigned long long)yy * image.height / (unsigned)drawHeight));

		// Skip empty source rows, then conservatively map the non-transparent
		// [min,max) source span into destination coordinates.
		if (sy >= image.alphaRowMinX.size() || sy >= image.alphaRowMaxX.size()) continue;
		const unsigned sourceMin = image.alphaRowMinX[sy];
		const unsigned sourceMax = image.alphaRowMaxX[sy];
		if (sourceMin >= sourceMax) continue;

		const int spanStart = (int)(((unsigned long long)sourceMin * drawWidth + image.width - 1) / image.width);
		const int spanEnd = (int)(((unsigned long long)sourceMax * drawWidth + image.width - 1) / image.width);
		int xx0 = std::max(startX, spanStart);
		int xx1 = std::min(drawWidth, spanEnd);
		xx0 = std::max(xx0, clipLeft - x);
		xx1 = std::min(xx1, clipRight - x);
		xx0 = std::max(xx0, -x);
		xx1 = std::min(xx1, destination->w - x);
		if (xx0 >= xx1) continue;

		for (int xx = xx0; xx < xx1; ++xx)
		{
			const unsigned sx = xSource[(size_t)xx];
			const size_t si = ((size_t)sy * image.width + sx) * 4;
			const Uint8 srcA0 = image.rgba[si + 3];
			if (!srcA0) continue;
			const Uint8 sa = opacity == 255 ? srcA0 : (Uint8)(((unsigned)srcA0 * opacity + 127) / 255);
			if (!sa) continue;

			Uint8 sr = image.rgba[si + 0];
			Uint8 sg = image.rgba[si + 1];
			Uint8 sb = image.rgba[si + 2];
			if (light != 16)
			{
				sr = (Uint8)(((unsigned)sr * light + 8) / 16);
				sg = (Uint8)(((unsigned)sg * light + 8) / 16);
				sb = (Uint8)(((unsigned)sb * light + 8) / 16);
			}

			const int dx = x + xx;
			Uint8 *pixelAddress = (Uint8*)destination->pixels + dy * destination->pitch + dx * 4;
			Uint32 out;

			if (sa == 255)
			{
				out = fast32 ? packFast(sr, sg, sb, 255) : SDL_MapRGBA(fmt, sr, sg, sb, 255);
			}
			else
			{
				Uint32 dstPixel;
				memcpy(&dstPixel, pixelAddress, sizeof(dstPixel));
				Uint8 dr, dg, db, da;
				if (fast32)
				{
					dr = (Uint8)((dstPixel & fmt->Rmask) >> fmt->Rshift);
					dg = (Uint8)((dstPixel & fmt->Gmask) >> fmt->Gshift);
					db = (Uint8)((dstPixel & fmt->Bmask) >> fmt->Bshift);
					da = fmt->Amask ? (Uint8)((dstPixel & fmt->Amask) >> fmt->Ashift) : 255;
				}
				else
				{
					SDL_GetRGBA(dstPixel, fmt, &dr, &dg, &db, &da);
				}

				const unsigned inv = 255 - sa;
				const Uint8 rr = (Uint8)((sr * sa + dr * inv + 127) / 255);
				const Uint8 rg = (Uint8)((sg * sa + dg * inv + 127) / 255);
				const Uint8 rb = (Uint8)((sb * sa + db * inv + 127) / 255);
				const Uint8 ra = (Uint8)std::min(255u, (unsigned)sa + ((unsigned)da * inv + 127) / 255);
				out = fast32 ? packFast(rr, rg, rb, ra) : SDL_MapRGBA(fmt, rr, rg, rb, ra);
			}
			memcpy(pixelAddress, &out, sizeof(out));
		}
	}
	if (SDL_MUSTLOCK(destination)) SDL_UnlockSurface(destination);
}

}
