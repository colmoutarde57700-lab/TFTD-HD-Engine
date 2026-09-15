#pragma once

#include <SDL.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace OpenXcom
{

/**
 * Native colour semantics for HD Battlescape assets.
 *
 * IndexedLegacy : source really contains TFTD palette indices; keep the exact
 *                 historical index/shade/active-palette path.
 * Environment   : authored RGB/RGBA world art. Preserve its continuous colour
 *                 detail, then apply the mission/depth colour grade learned
 *                 from TFTD's paired Battlescape palettes.
 * Fixed         : explicit opt-out for technical/debug/presentation graphics.
 * Auto          : normal authored world art -> Environment.
 *
 * Storage format is deliberately NOT semantic metadata. In particular, RGBA is
 * a normal Environment asset unless the rule explicitly says fixed/native.
 */
enum class HdColorMode
{
	Auto,
	IndexedLegacy,
	Environment,
	Fixed
};

inline std::string hdLower(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return value;
}

inline HdColorMode parseHdColorMode(const std::string &value)
{
	const std::string v = hdLower(value);
	if (v == "indexedlegacy" || v == "indexed" || v == "legacy" || v == "palette") return HdColorMode::IndexedLegacy;
	if (v == "fixed" || v == "native") return HdColorMode::Fixed;
	// RGBA describes storage/artwork, not an exemption from Battlescape colour.
	if (v == "environment" || v == "env" || v == "dynamic" || v == "rgba") return HdColorMode::Environment;
	return HdColorMode::Auto;
}

inline HdColorMode resolveHdColorMode(const std::string &value, bool paletteIndexed8)
{
	HdColorMode mode = parseHdColorMode(value);
	if (mode == HdColorMode::Auto)
		return HdColorMode::Environment;
	// Exact Legacy semantics require real 8-bit source indices. Authored RGB is
	// never quantised merely because a stale rule says indexedLegacy.
	if (mode == HdColorMode::IndexedLegacy && !paletteIndexed8)
		return HdColorMode::Environment;
	return mode;
}

inline const char *hdColorModeName(HdColorMode mode)
{
	switch (mode)
	{
		case HdColorMode::IndexedLegacy: return "indexedLegacy";
		case HdColorMode::Environment: return "environment";
		case HdColorMode::Fixed: return "fixed";
		default: return "auto";
	}
}

/**
 * P10A-FAM all-terrain profiles.
 *
 * Authored HD art stays true-colour RGB/RGBA.  A terrain dataset only selects
 * which historical TFTD 16-colour palette blocks calibrate its continuous
 * D0 -> active-depth environment LUT.  No HD pixel is quantised back to the
 * Legacy palette.
 *
 * The masks below were measured from the untouched indexed Legacy terrain PNG
 * pack (same palette indices as the original terrain art).  Datasets sharing
 * the same observed block family reuse the same LUT profile.  Unknown/future
 * datasets deliberately fall back to the original P10A global transform.
 */
enum class HdEnvironmentProfile : uint8_t
{
	Global = 0,
	Count = 51
};

static constexpr size_t HdEnvironmentProfileCount = (size_t)HdEnvironmentProfile::Count;

// Profile 0 is the historical P10A all-block fallback.  The remaining 50
// entries are the unique block masks used by the 65 original TFTD terrain
// datasets plus CORAL's F0-only bubble animation.
static constexpr uint16_t HdEnvironmentProfileMasks[HdEnvironmentProfileCount] = {
	0xFFFFu,
	0x0007u, 0x0406u, 0x05DCu, 0x0CFEu, 0x0DFBu, 0x0DFDu, 0x0E94u, 0x10DCu,
	0x15BEu, 0x1D7Eu, 0x1DFEu, 0x2011u, 0x2D63u, 0x2DFFu, 0x350Cu, 0x3894u,
	0x3DDEu, 0x3DFEu, 0x4C6Fu, 0x4CA2u, 0x65FFu, 0x70CCu, 0x71FFu, 0x740Cu,
	0x75FEu, 0x7880u, 0x7DFEu, 0x7DFFu, 0x8000u, 0x8080u, 0x80C0u, 0x859Eu,
	0x8D7Fu, 0x9D7Eu, 0x9D8Eu, 0x9DFEu, 0xBD7Eu, 0xBDFEu, 0xDD8Eu, 0xDDFDu,
	0xF5FFu, 0xFCBEu, 0xFCFCu, 0xFD0Eu, 0xFD32u, 0xFDBEu, 0xFDBFu, 0xFDFCu,
	0xFDFEu, 0xFDFFu
};

inline size_t hdEnvironmentProfileIndex(HdEnvironmentProfile profile)
{
	const size_t i = (size_t)profile;
	return i < HdEnvironmentProfileCount ? i : 0;
}

inline uint16_t hdEnvironmentProfileBlockMask(HdEnvironmentProfile profile)
{
	return HdEnvironmentProfileMasks[hdEnvironmentProfileIndex(profile)];
}

inline HdEnvironmentProfile hdEnvironmentProfileFromBlockMask(uint16_t blockMask)
{
	if (blockMask == 0xFFFFu) return HdEnvironmentProfile::Global;
	for (size_t i = 1; i < HdEnvironmentProfileCount; ++i)
	{
		if (HdEnvironmentProfileMasks[i] == blockMask)
			return (HdEnvironmentProfile)i;
	}
	return HdEnvironmentProfile::Global;
}

struct HdTerrainDatasetProfile
{
	const char *dataset;
	uint16_t blockMask;
};

// Exact non-transparent Legacy palette-block presence per TERRAIN dataset.
static constexpr HdTerrainDatasetProfile HdTerrainDatasetProfiles[] = {
	{"asunk",    0x05DCu}, {"atlantis", 0x350Cu}, {"blanks",   0x2011u},
	{"cargo1",   0xFDFEu}, {"cargo2",   0xFDFCu}, {"cargo3",   0x10DCu},
	{"cargo4",   0xFCFCu}, {"cargo5",   0xFCFCu}, {"coral",    0x7880u},
	{"crypt1",   0x9D8Eu}, {"crypt2",   0xDD8Eu}, {"crypt3",   0xFD0Eu},
	{"crypt4",   0x859Eu}, {"debris",   0xFDBFu}, {"deckc",    0xFDFEu},
	{"entry",    0xFDFFu}, {"grunge1",  0x2DFFu}, {"grunge2",  0x0DFDu},
	{"grunge3",  0x0DFBu}, {"grunge4",  0x7DFFu}, {"grunge5",  0x65FFu},
	{"hammer",   0x740Cu}, {"island1",  0xFDFEu}, {"island2",  0x3DFEu},
	{"island3",  0xBDFEu}, {"leviath",  0x70CCu}, {"linera",   0xFDFEu},
	{"linerb",   0xFDFEu}, {"linerc",   0xFDFFu}, {"linerd",   0xFDFEu},
	{"msunk1",   0xFDFFu}, {"msunk2",   0xFCBEu}, {"mu",       0x15BEu},
	{"organic",  0xFDFFu}, {"organic1", 0xFDFFu}, {"organic2", 0xFDBFu},
	{"organic3", 0xFD32u}, {"pipes",    0x7DFEu}, {"plane",    0xDDFDu},
	{"port01",   0xFDBEu}, {"port02",   0xFDFFu}, {"port1",    0xFDBEu},
	{"psynom",   0x75FEu}, {"pyramid",  0xF5FFu}, {"rocks",    0x3894u},
	{"sand",     0x0406u}, {"sea",      0x80C0u}, {"triton",   0x71FFu},
	{"uext1",    0x2D63u}, {"uext2",    0x3DDEu}, {"uext3",    0x3DDEu},
	{"ufobits",  0x8080u}, {"uint",     0x0007u}, {"uint1",    0x4C6Fu},
	{"uint2",    0x9D7Eu}, {"uint3",    0x0CFEu}, {"urbits",   0x8D7Fu},
	{"volc",     0x0E94u}, {"weeds",    0x4CA2u}, {"xbases01", 0x9DFEu},
	{"xbases02", 0x1D7Eu}, {"xbases03", 0xBD7Eu}, {"xbases04", 0xFDFEu},
	{"xbases05", 0x1DFEu}, {"xbits",    0xFDFFu}
};

inline uint16_t hdTerrainDatasetBlockMask(const std::string &dataset)
{
	for (const auto &entry : HdTerrainDatasetProfiles)
	{
		if (dataset == entry.dataset) return entry.blockMask;
	}
	return 0xFFFFu;
}

inline bool hdAssetPathEndsWith(const std::string &path, const char *suffix)
{
	const size_t n = std::char_traits<char>::length(suffix);
	return path.size() >= n && path.compare(path.size() - n, n, suffix) == 0;
}

inline HdEnvironmentProfile hdEnvironmentProfileForAssetPath(const std::string &assetPath)
{
	const std::string p = hdLower(assetPath);
	const std::string marker = "/terrain/";
	const size_t markerPos = p.find(marker);
	if (markerPos == std::string::npos) return HdEnvironmentProfile::Global;
	const size_t datasetStart = markerPos + marker.size();
	const size_t datasetEnd = p.find('/', datasetStart);
	if (datasetEnd == std::string::npos || datasetEnd <= datasetStart) return HdEnvironmentProfile::Global;
	const std::string dataset = p.substr(datasetStart, datasetEnd - datasetStart);

	// CORAL 026..033 is a deliberately cyan Legacy animation.  Its source uses
	// palette block F0 only, so keep that narrow response instead of mixing it
	// with the reef blocks used by CORAL 000..025.  Accept both 3- and 4-digit
	// frame naming conventions.
	if (dataset == "coral")
	{
		static const char *bubbleFrames[] = {
			"/026.png", "/027.png", "/028.png", "/029.png",
			"/030.png", "/031.png", "/032.png", "/033.png",
			"/0026.png", "/0027.png", "/0028.png", "/0029.png",
			"/0030.png", "/0031.png", "/0032.png", "/0033.png"
		};
		for (const char *frame : bubbleFrames)
		{
			if (hdAssetPathEndsWith(p, frame))
				return hdEnvironmentProfileFromBlockMask(0x8000u);
		}
	}

	return hdEnvironmentProfileFromBlockMask(hdTerrainDatasetBlockMask(dataset));
}

/**
 * Continuous RGB environment transform learned from TFTD's own palettes.
 *
 * The 256-colour palettes are calibration samples, not an output palette.
 * This class NEVER maps authored RGB back to a Legacy palette index.
 *
 * Build process for the active mission/depth palette:
 *   1. Fit a regularised quadratic RGB->RGB model to all paired palette
 *      colours. This captures the global underwater/mission colour behaviour
 *      and gives sensible extrapolation for HD colours absent from 1995 art.
 *   2. Measure the residual error at the palette samples.
 *   3. Add a smooth Gaussian residual field around those samples so colours
 *      near known TFTD tones inherit the local character of the original game
 *      without hard nearest-colour/Voronoi boundaries.
 *   4. Bake the result to one 33^3 RGB LUT. CPU and D3D11 sample the exact same
 *      table, so software fallback and GPU rendering have identical semantics.
 *
 * The transformation is therefore continuous, preserves HD gradients and
 * detail, and naturally becomes stronger as OXCE selects PAL_BATTLESCAPE_1,
 * _2 or _3. Fixed assets are the only explicit opt-out.
 */
class HdEnvironmentTransform
{
public:
	static constexpr int Size = 33;

private:
	std::vector<Uint8> _rgba;
	uint64_t _signature = 0;
	bool _identity = true;

	static constexpr double RidgeLambda = 0.001;
	static constexpr double ResidualSigma = 20.0; // local TFTD calibration radius in RGB code values
	static constexpr double SupportSigma = 48.0;  // confidence falloff outside the historical palette gamut

	static uint64_t paletteSignature(const SDL_Color *reference, const SDL_Color *active, uint16_t blockMask)
	{
		uint64_t h = 1469598103934665603ull;
		h ^= (Uint8)(blockMask & 0xFFu); h *= 1099511628211ull;
		h ^= (Uint8)(blockMask >> 8); h *= 1099511628211ull;
		for (int i = 0; i < 256; ++i)
		{
			const SDL_Color colors[2] = { reference[i], active[i] };
			for (const SDL_Color &c : colors)
			{
				const Uint8 bytes[3] = { c.r, c.g, c.b };
				for (Uint8 b : bytes) { h ^= b; h *= 1099511628211ull; }
			}
		}
		return h;
	}

	static bool solve3(double a[3][3], double b[3], double x[3])
	{
		double aug[3][4] = {
			{a[0][0], a[0][1], a[0][2], b[0]},
			{a[1][0], a[1][1], a[1][2], b[1]},
			{a[2][0], a[2][1], a[2][2], b[2]}
		};
		for (int c = 0; c < 3; ++c)
		{
			int pivot = c;
			for (int r = c + 1; r < 3; ++r)
				if (std::fabs(aug[r][c]) > std::fabs(aug[pivot][c])) pivot = r;
			if (std::fabs(aug[pivot][c]) < 1e-12) return false;
			if (pivot != c) for (int k = c; k < 4; ++k) std::swap(aug[c][k], aug[pivot][k]);
			const double inv = 1.0 / aug[c][c];
			for (int k = c; k < 4; ++k) aug[c][k] *= inv;
			for (int r = 0; r < 3; ++r)
			{
				if (r == c) continue;
				const double f = aug[r][c];
				for (int k = c; k < 4; ++k) aug[r][k] -= f * aug[c][k];
			}
		}
		for (int i = 0; i < 3; ++i) x[i] = aug[i][3];
		return true;
	}

	static double evalTone(const double coeff[3], double v)
	{
		return coeff[0] + coeff[1] * v + coeff[2] * v * v;
	}

public:
	bool build(const SDL_Color *reference, const SDL_Color *active, uint16_t blockMask = 0xFFFFu)
	{
		if (!reference || !active) return false;
		const uint64_t signature = paletteSignature(reference, active, blockMask);
		if (signature == _signature && !_rgba.empty()) return false;
		_signature = signature;
		_identity = true;
		for (int i = 1; i < 256; ++i)
		{
			if ((blockMask & (uint16_t)(1u << (i >> 4))) == 0) continue;
			if (reference[i].r != active[i].r || reference[i].g != active[i].g || reference[i].b != active[i].b)
			{
				_identity = false;
				break;
			}
		}

		_rgba.resize((size_t)Size * Size * Size * 4u);
		if (_identity)
		{
			for (int bz = 0; bz < Size; ++bz)
			for (int gy = 0; gy < Size; ++gy)
			for (int rx = 0; rx < Size; ++rx)
			{
				const size_t o = (((size_t)bz * Size + gy) * Size + rx) * 4u;
				_rgba[o+0] = (Uint8)std::lround(rx * 255.0 / (Size - 1));
				_rgba[o+1] = (Uint8)std::lround(gy * 255.0 / (Size - 1));
				_rgba[o+2] = (Uint8)std::lround(bz * 255.0 / (Size - 1));
				_rgba[o+3] = 255;
			}
			return true;
		}

		// Stable global behaviour for the entire RGB cube. Each channel gets a
		// regularised quadratic tone curve learned from all TFTD palette pairs.
		// Keeping the global model channel-separable prevents unsupported HD hues
		// from acquiring arbitrary cross-channel colours outside the old gamut.
		double coeff[3][3] = {};
		for (int ch = 0; ch < 3; ++ch)
		{
			double ata[3][3] = {};
			double aty[3] = {};
			for (int i = 1; i < 256; ++i)
			{
				if ((blockMask & (uint16_t)(1u << (i >> 4))) == 0) continue;
				const double src = (ch == 0 ? reference[i].r : (ch == 1 ? reference[i].g : reference[i].b)) / 255.0;
				const double dst = (ch == 0 ? active[i].r : (ch == 1 ? active[i].g : active[i].b)) / 255.0;
				const double f[3] = {1.0, src, src * src};
				for (int r = 0; r < 3; ++r)
				{
					for (int c = 0; c < 3; ++c) ata[r][c] += f[r] * f[c];
					aty[r] += f[r] * dst;
				}
			}
			// Identity prior: y=x when the palette offers little support.
			for (int k = 0; k < 3; ++k) ata[k][k] += RidgeLambda;
			aty[1] += RidgeLambda;
			if (!solve3(ata, aty, coeff[ch]))
			{
				coeff[ch][0] = 0.0; coeff[ch][1] = 1.0; coeff[ch][2] = 0.0;
			}
		}

		// Residuals encode the hue-specific character that a channel-only global
		// model cannot express. They are applied only where the historical palette
		// has nearby support; confidence fades smoothly outside that gamut.
		double residual[256][3] = {};
		for (int i = 1; i < 256; ++i)
		{
			if ((blockMask & (uint16_t)(1u << (i >> 4))) == 0) continue;
			const double src[3] = { reference[i].r / 255.0, reference[i].g / 255.0, reference[i].b / 255.0 };
			const double target[3] = { (double)active[i].r, (double)active[i].g, (double)active[i].b };
			for (int ch = 0; ch < 3; ++ch)
				residual[i][ch] = target[ch] - evalTone(coeff[ch], src[ch]) * 255.0;
		}

		const double residualDen = 2.0 * ResidualSigma * ResidualSigma;
		const double supportDen = 2.0 * SupportSigma * SupportSigma;
		for (int bz = 0; bz < Size; ++bz)
		for (int gy = 0; gy < Size; ++gy)
		for (int rx = 0; rx < Size; ++rx)
		{
			const double rgb[3] = {
				rx * 255.0 / (Size - 1),
				gy * 255.0 / (Size - 1),
				bz * 255.0 / (Size - 1)
			};
			double out[3] = {
				evalTone(coeff[0], rgb[0] / 255.0) * 255.0,
				evalTone(coeff[1], rgb[1] / 255.0) * 255.0,
				evalTone(coeff[2], rgb[2] / 255.0) * 255.0
			};

			double minD2 = 1e30;
			double sumW = 0.0;
			double corr[3] = {0.0, 0.0, 0.0};
			for (int i = 1; i < 256; ++i)
			{
				if ((blockMask & (uint16_t)(1u << (i >> 4))) == 0) continue;
				const double dr = rgb[0] - reference[i].r;
				const double dg = rgb[1] - reference[i].g;
				const double db = rgb[2] - reference[i].b;
				const double d2 = dr*dr + dg*dg + db*db;
				minD2 = std::min(minD2, d2);
				const double w = std::exp(-d2 / residualDen);
				if (w < 1e-8) continue;
				sumW += w;
				corr[0] += w * residual[i][0];
				corr[1] += w * residual[i][1];
				corr[2] += w * residual[i][2];
			}
			if (sumW > 1e-8)
			{
				const double confidence = std::exp(-minD2 / supportDen);
				out[0] += confidence * corr[0] / sumW;
				out[1] += confidence * corr[1] / sumW;
				out[2] += confidence * corr[2] / sumW;
			}

			const size_t o = (((size_t)bz * Size + gy) * Size + rx) * 4u;
			_rgba[o+0] = (Uint8)std::max(0.0, std::min(255.0, std::round(out[0])));
			_rgba[o+1] = (Uint8)std::max(0.0, std::min(255.0, std::round(out[1])));
			_rgba[o+2] = (Uint8)std::max(0.0, std::min(255.0, std::round(out[2])));
			_rgba[o+3] = 255;
		}
		return true;
	}

	const std::vector<Uint8> &rgba() const { return _rgba; }
	bool identity() const { return _identity; }
	uint64_t signature() const { return _signature; }

	void apply(Uint8 &r, Uint8 &g, Uint8 &b) const
	{
		if (_identity || _rgba.empty()) return;
		const double fx = (double)r * (Size - 1) / 255.0;
		const double fy = (double)g * (Size - 1) / 255.0;
		const double fz = (double)b * (Size - 1) / 255.0;
		const int x0 = std::max(0, std::min(Size-1, (int)std::floor(fx)));
		const int y0 = std::max(0, std::min(Size-1, (int)std::floor(fy)));
		const int z0 = std::max(0, std::min(Size-1, (int)std::floor(fz)));
		const int x1 = std::min(Size-1, x0+1), y1 = std::min(Size-1, y0+1), z1 = std::min(Size-1, z0+1);
		const double tx = fx-x0, ty = fy-y0, tz = fz-z0;
		double acc[3] = {0,0,0};
		for (int zz = 0; zz < 2; ++zz)
		for (int yy = 0; yy < 2; ++yy)
		for (int xx = 0; xx < 2; ++xx)
		{
			const int x = xx ? x1 : x0, y = yy ? y1 : y0, z = zz ? z1 : z0;
			const double w = (xx ? tx : 1.0-tx) * (yy ? ty : 1.0-ty) * (zz ? tz : 1.0-tz);
			const size_t o = (((size_t)z * Size + y) * Size + x) * 4u;
			acc[0] += w * _rgba[o+0]; acc[1] += w * _rgba[o+1]; acc[2] += w * _rgba[o+2];
		}
		r = (Uint8)std::max(0.0, std::min(255.0, std::round(acc[0])));
		g = (Uint8)std::max(0.0, std::min(255.0, std::round(acc[1])));
		b = (Uint8)std::max(0.0, std::min(255.0, std::round(acc[2])));
	}
};
} // namespace OpenXcom
