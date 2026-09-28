#include "HdRenderTrace.h"
#include "HdImage.h"
#include "HdPngIndices.h"
#include "FileMap.h"
#include "Logger.h"
#include "Exception.h"
#include "../lodepng.h"
#include <algorithm>
#include <cstring>

namespace OpenXcom
{

HdAssetResolution HdImageCache::resolve(const HdAssetKey &key, const std::vector<HdAssetCandidate> &candidates, bool traceMissing)
{
	auto result = hdResolveAsset(key, candidates, [this](const std::string &path)
	{
		if (!exists(path)) return HdAssetAvailability::Absent;
		return get(path) ? HdAssetAvailability::Ready : HdAssetAvailability::Invalid;
	});
	std::string trail;
	for (const auto &attempt : result.attempted) trail += attempt.path + ";";
	for (const auto &invalid : result.invalid)
		hdTraceRoute("asset", key.family + ":" + std::to_string(key.frame),
			result ? hdAssetProviderName(result.asset.provider) : "UNRESOLVED",
			"invalid=" + invalid.path + " attempts=" + trail);
	// Candidate chains such as inventory portraits probe several legal OXCE names.
	// A missing intermediate candidate is not a final HD failure; callers can mute
	// that probe while still logging the provider that eventually succeeds.
	if (result || traceMissing)
		hdTraceRoute("asset", key.family + ":" + std::to_string(key.frame),
			result ? hdAssetProviderName(result.asset.provider) : "UNRESOLVED", "attempts=" + trail);
	return result;
}

HdImage &HdImageCache::require(const std::string &path)
{
	if (HdImage *image = get(path)) return *image;
	const std::string reason = exists(path) ? "INVALID_RESOURCE" : "MISSING_RESOURCE";
	const std::string message = "[HD RESOURCE ERROR][" + reason + "] " + path;
	hdTraceRoute("asset-read", path, "UNRESOLVED", message);
	throw Exception(message);
}

bool HdImageCache::usable(const std::string &path)
{
    if (!exists(path)) return false;
    if (get(path)) return true;
    hdTraceRoute("asset", path, "TRY_NEXT_PROVIDER", "PNG exists but cannot be decoded");
    return false;
}

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
	// Inspect the header before allocating decoded pixels. The old path fully
	// inflated every RGB/RGBA PNG in its native format and then inflated it a
	// second time to RGBA. REAL HD materials can exceed 5000 pixels per side,
	// so the second full decode costs both startup time and substantial peak RAM.
	// Palette files still need the native decode to preserve their indices.
	lodepng::State state;
	unsigned error = lodepng_inspect(&w, &h, &state, png.data(), png.size());
	if (error)
	{
		Log(LOG_WARNING) << "HD PNG header failed (" << error << ") for " << path << ": " << lodepng_error_text(error);
		return nullptr;
	}
	entry.width = w;
	entry.height = h;
	entry.paletteIndexed8 = state.info_png.color.colortype == LCT_PALETTE;

	if (entry.paletteIndexed8)
	{
		state.decoder.color_convert = 0;
		std::vector<unsigned char> raw;
		error = lodepng::decode(raw, w, h, state, png);
		if (error)
		{
			Log(LOG_WARNING) << "HD indexed PNG decode failed (" << error << ") for " << path << ": " << lodepng_error_text(error);
			return nullptr;
		}
		const size_t pixelCount = (size_t)w * h;
		if (!hdUnpackPngIndices(raw, pixelCount, state.info_png.color.bitdepth, entry.indices))
		{
			Log(LOG_WARNING) << "HD indexed PNG has unexpected raw size for " << path;
			return nullptr;
		}
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
		// Decode directly to canonical RGBA once. Storage colour type was read
		// from IHDR, so no native-pixel staging buffer is needed here.
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
	entry.influenceMinX = w;
	entry.influenceMinY = h;
	entry.influenceMaxX = 0;
	entry.influenceMaxY = 0;
	entry.hasInfluencePixels = false;
	for (unsigned y = 0; y < h; ++y)
	{
		unsigned minX = w;
		unsigned maxX = 0;
		for (unsigned x = 0; x < w; ++x)
		{
			const size_t pi = ((size_t)y * w + x) * 4u;
			const unsigned a = entry.rgba[pi + 3];
			if (a != 0)
			{
				minX = std::min(minX, x);
				maxX = std::max(maxX, x + 1);
			}

			// Preserve the exact former ImpactMaskBounds threshold:
			// influence = luma(RGB) * alpha / 255, accepted when >= 4.
			const unsigned r = entry.rgba[pi + 0];
			const unsigned g = entry.rgba[pi + 1];
			const unsigned b = entry.rgba[pi + 2];
			const unsigned luma = (54u * r + 183u * g + 19u * b) >> 8;
			const unsigned influence = (luma * a + 127u) / 255u;
			if (influence >= 4u)
			{
				entry.hasInfluencePixels = true;
				entry.influenceMinX = std::min(entry.influenceMinX, x);
				entry.influenceMinY = std::min(entry.influenceMinY, y);
				entry.influenceMaxX = std::max(entry.influenceMaxX, x);
				entry.influenceMaxY = std::max(entry.influenceMaxY, y);
			}
		}
		entry.alphaRowMinX[y] = minX;
		entry.alphaRowMaxX[y] = maxX;
		if (minX < maxX) entry.hasVisiblePixels = true;
	}
	if (!entry.hasInfluencePixels && w && h)
	{
		// Match the old per-frame fallback for a valid but fully sub-threshold mask.
		entry.influenceMinX = entry.influenceMinY = 0;
		entry.influenceMaxX = w - 1u;
		entry.influenceMaxY = h - 1u;
	}

	entry.loaded = true;
	hdTraceRoute("asset", path, hdProviderForPath(path), "PNG decoded");
	if (_prewarmComplete)
	{
		++_postPrewarmLoads;
		Log(LOG_INFO) << "[HD-PREWARM LATE-LOAD] #" << _postPrewarmLoads << " resource=" << path << " reason=NOT_IN_INITIAL_MANIFEST status=FOUND_DECODED_CACHED";
	}
	const FileMap::FileRecord *provider = FileMap::at(path);
	// Share the bounded trace registry across image-cache instances. Releasing
	// one cache must not reopen an unlimited per-glyph logging path.
	hdTraceRoute("asset-load", path, hdProviderForPath(path),
		std::to_string(w) + "x" + std::to_string(h) +
		(entry.paletteIndexed8 ? " PALETTE8+RGBA" : " RGBA") +
		" pngBitDepth=" + std::to_string(state.info_png.color.bitdepth) +
		" provider=" + (provider ? provider->fullpath : std::string("<unknown>")));
	return &entry;
}


size_t HdImageCache::imageCount() const
{
	size_t count = 0;
	for (const auto &pair : _images) if (pair.second.loaded) ++count;
	return count;
}

std::uint64_t HdImageCache::estimatedCpuBytes() const
{
	std::uint64_t bytes = 0;
	for (const auto &pair : _images)
	{
		const HdImage &image = pair.second;
		if (!image.loaded) continue;
		bytes += sizeof(HdImage);
		bytes += image.rgba.size() * sizeof(unsigned char);
		bytes += image.indices.size() * sizeof(unsigned char);
		bytes += image.alphaRowMinX.size() * sizeof(unsigned);
		bytes += image.alphaRowMaxX.size() * sizeof(unsigned);
		bytes += pair.first.size();
	}
	return bytes;
}

void HdImageCache::clear()
{
	++_generation;
	_images.clear();
	_exists.clear();
	_prewarmComplete = false;
	_postPrewarmLoads = 0;
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
