#include "RealHdWorldCoverage.h"

#include <algorithm>
#include <array>
#include <cstddef>

#include "Camera.h"
#include "../Savegame/Tile.h"
#include "../Mod/MapData.h"

namespace OpenXcom
{

namespace
{

struct CoverageVoxelRows
{
	std::array<std::array<Uint16, 16>, 24> rows{};

	bool occupied(int x, int y, int z) const
	{
		if (x < 0 || x >= 16 || y < 0 || y >= 16 || z < 0 || z >= 24) return false;
		const Uint16 bit = (Uint16)(1u << (15 - x));
		return (rows[(size_t)z][(size_t)y] & bit) != 0;
	}
};

HdGpuWorldCoverageVertex projectCorner(const Camera *camera, int worldX, int worldY, int worldZ)
{
	Position screen;
	camera->convertVoxelToScreen(Position(worldX, worldY, worldZ), &screen);
	HdGpuWorldCoverageVertex out;
	out.logicalX = (float)screen.x;
	out.logicalY = (float)screen.y;
	// Camera::convertVoxelToScreen projects (x-y, (x+y)/2-z). x+y+z is the
	// missing camera-ray coordinate and is therefore a real geometric depth.
	out.viewDepth = (float)(worldX + worldY + worldZ);
	return out;
}

void emitQuad(const Camera *camera, std::vector<HdGpuWorldCoverageVertex> &out,
	int ax, int ay, int az, int bx, int by, int bz,
	int cx, int cy, int cz, int dx, int dy, int dz)
{
	const HdGpuWorldCoverageVertex a = projectCorner(camera, ax, ay, az);
	const HdGpuWorldCoverageVertex b = projectCorner(camera, bx, by, bz);
	const HdGpuWorldCoverageVertex c = projectCorner(camera, cx, cy, cz);
	const HdGpuWorldCoverageVertex d = projectCorner(camera, dx, dy, dz);
	out.push_back(a); out.push_back(b); out.push_back(c);
	out.push_back(c); out.push_back(b); out.push_back(d);
}

/** Greedy rectangle extraction from a small binary grid. */
template<size_t H, size_t W, typename Emit>
unsigned emitGreedyRects(std::array<std::array<bool, W>, H> mask, Emit emit)
{
	unsigned quads = 0;
	for (size_t y = 0; y < H; ++y)
	{
		for (size_t x = 0; x < W; ++x)
		{
			if (!mask[y][x]) continue;
			size_t w = 1;
			while (x + w < W && mask[y][x + w]) ++w;
			size_t h = 1;
			for (;;)
			{
				if (y + h >= H) break;
				bool full = true;
				for (size_t xx = 0; xx < w; ++xx)
				{
					if (!mask[y + h][x + xx]) { full = false; break; }
				}
				if (!full) break;
				++h;
			}
			for (size_t yy = 0; yy < h; ++yy)
				for (size_t xx = 0; xx < w; ++xx)
					mask[y + yy][x + xx] = false;
			emit((int)x, (int)y, (int)(x + w), (int)(y + h));
			++quads;
		}
	}
	return quads;
}

}

unsigned RealHdWorldCoverage::appendTerrainPart(const Camera *camera, const Tile *tile, TilePart part,
	const std::vector<Uint16> &voxelData, std::vector<HdGpuWorldCoverageVertex> &vertices)
{
	if (!camera || !tile || part < O_FLOOR || part >= O_MAX) return 0;
	MapData *data = tile->getMapData(part);
	if (!data) return 0;
	if ((part == O_WESTWALL || part == O_NORTHWALL) && tile->isUfoDoorOpen(part)) return 0;

	CoverageVoxelRows shape;
	for (int z = 0; z < 24; ++z)
	{
		const int loft = data->getLoftID(z / 2);
		if (loft < 0) continue;
		const size_t base = (size_t)loft * 16u;
		if (base + 15u >= voxelData.size()) continue;
		for (int y = 0; y < 16; ++y)
			shape.rows[(size_t)z][(size_t)y] = voxelData[base + (size_t)y];
	}

	const Position tp = tile->getPosition();
	const int baseX = tp.x * Position::TileXY;
	const int baseY = tp.y * Position::TileXY;
	const int baseZ = tp.z * Position::TileZ;
	unsigned quads = 0;

	// Camera-facing +Z faces. A 16x16 voxel patch on one Z plane is merged into
	// maximal rectangles before projection; depth remains exact because x+y+z is
	// linear across each planar quad.
	for (int z = 0; z < 24; ++z)
	{
		std::array<std::array<bool, 16>, 16> mask{}; // [y][x]
		for (int y = 0; y < 16; ++y)
			for (int x = 0; x < 16; ++x)
				mask[(size_t)y][(size_t)x] = shape.occupied(x, y, z) && !shape.occupied(x, y, z + 1);
		quads += emitGreedyRects<16,16>(mask, [&](int x0, int y0, int x1, int y1)
		{
			const int wz = baseZ + z + 1;
			emitQuad(camera, vertices,
				baseX + x0, baseY + y0, wz,
				baseX + x1, baseY + y0, wz,
				baseX + x0, baseY + y1, wz,
				baseX + x1, baseY + y1, wz);
		});
	}

	// Camera-facing +X faces. Grid axes are local Y and Z.
	for (int x = 0; x < 16; ++x)
	{
		std::array<std::array<bool, 16>, 24> mask{}; // [z][y]
		for (int z = 0; z < 24; ++z)
			for (int y = 0; y < 16; ++y)
				mask[(size_t)z][(size_t)y] = shape.occupied(x, y, z) && !shape.occupied(x + 1, y, z);
		quads += emitGreedyRects<24,16>(mask, [&](int y0, int z0, int y1, int z1)
		{
			const int wx = baseX + x + 1;
			emitQuad(camera, vertices,
				wx, baseY + y0, baseZ + z0,
				wx, baseY + y1, baseZ + z0,
				wx, baseY + y0, baseZ + z1,
				wx, baseY + y1, baseZ + z1);
		});
	}

	// Camera-facing +Y faces. Grid axes are local X and Z.
	for (int y = 0; y < 16; ++y)
	{
		std::array<std::array<bool, 16>, 24> mask{}; // [z][x]
		for (int z = 0; z < 24; ++z)
			for (int x = 0; x < 16; ++x)
				mask[(size_t)z][(size_t)x] = shape.occupied(x, y, z) && !shape.occupied(x, y + 1, z);
		quads += emitGreedyRects<24,16>(mask, [&](int x0, int z0, int x1, int z1)
		{
			const int wy = baseY + y + 1;
			emitQuad(camera, vertices,
				baseX + x0, wy, baseZ + z0,
				baseX + x1, wy, baseZ + z0,
				baseX + x0, wy, baseZ + z1,
				baseX + x1, wy, baseZ + z1);
		});
	}

	return quads;
}

}
