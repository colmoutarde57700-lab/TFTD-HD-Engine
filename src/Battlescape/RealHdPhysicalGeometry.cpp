#include "RealHdPhysicalGeometry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace OpenXcom
{
std::size_t RealHdPhysicalGeometry::CellKeyHash::operator()(const CellKey &key) const noexcept
{
	std::size_t hash = 1469598103934665603ull;
	for (int value : {key.x, key.y, key.z})
	{
		hash ^= static_cast<std::uint32_t>(value);
		hash *= 1099511628211ull;
	}
	return hash;
}

namespace
{
constexpr double EPSILON = 1e-9;
constexpr double TILE_XY = 16.0;
constexpr double TILE_Z = 24.0;

RealHdPoint3 subtract(const RealHdPoint3 &a, const RealHdPoint3 &b)
{
	return {a.x - b.x, a.y - b.y, a.z - b.z};
}

RealHdPoint3 cross(const RealHdPoint3 &a, const RealHdPoint3 &b)
{
	return {a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x};
}

double dot(const RealHdPoint3 &a, const RealHdPoint3 &b)
{
	return a.x*b.x + a.y*b.y + a.z*b.z;
}

bool intersects(const RealHdPoint3 &from, const RealHdPoint3 &direction,
	const RealHdBarrierTriangle &triangle)
{
	const RealHdPoint3 edge1 = subtract(triangle.b, triangle.a);
	const RealHdPoint3 edge2 = subtract(triangle.c, triangle.a);
	const RealHdPoint3 p = cross(direction, edge2);
	const double determinant = dot(edge1, p);
	if (std::abs(determinant) <= EPSILON) return false;
	const double inverse = 1.0 / determinant;
	const RealHdPoint3 t = subtract(from, triangle.a);
	const double u = dot(t, p) * inverse;
	if (u < -EPSILON || u > 1.0 + EPSILON) return false;
	const RealHdPoint3 q = cross(t, edge1);
	const double v = dot(direction, q) * inverse;
	if (v < -EPSILON || u + v > 1.0 + EPSILON) return false;
	const double rayFraction = dot(edge2, q) * inverse;
	return rayFraction > EPSILON && rayFraction < 1.0 - EPSILON;
}

int cell(double coordinate, double cellSize)
{
	return static_cast<int>(std::floor(coordinate / cellSize));
}

struct AxisWalk
{
	int step = 0;
	double next = std::numeric_limits<double>::infinity();
	double delta = std::numeric_limits<double>::infinity();
};

AxisWalk axisWalk(double origin, double direction, int currentCell, double cellSize)
{
	AxisWalk result;
	if (std::abs(direction) <= EPSILON) return result;
	result.step = direction > 0.0 ? 1 : -1;
	const double boundary = (currentCell + (result.step > 0 ? 1 : 0)) * cellSize;
	result.next = (boundary - origin) / direction;
	result.delta = cellSize / std::abs(direction);
	return result;
}
}

void RealHdPhysicalGeometry::clear()
{
	_tiles.clear();
	_denseTileIndex.clear();
	_tileIndex.clear();
	_barrierIndex.clear();
	_width = _height = _levels = 0;
	_ready = false;
	_hasVoxelVolumes = false;
}

void RealHdPhysicalGeometry::indexTile(std::size_t index)
{
	const auto &barriers = _tiles[index].barriers;
	for (std::size_t i = 0; i < barriers.size(); ++i)
	{
		const RealHdBarrierTriangle &b = barriers[i];
		const int minX = cell(std::min({b.a.x, b.b.x, b.c.x}), TILE_XY);
		const int minY = cell(std::min({b.a.y, b.b.y, b.c.y}), TILE_XY);
		const int minZ = cell(std::min({b.a.z, b.b.z, b.c.z}), TILE_Z);
		const int maxX = cell(std::max({b.a.x, b.b.x, b.c.x}), TILE_XY);
		const int maxY = cell(std::max({b.a.y, b.b.y, b.c.y}), TILE_XY);
		const int maxZ = cell(std::max({b.a.z, b.b.z, b.c.z}), TILE_Z);
		for (int z = minZ; z <= maxZ; ++z)
			for (int y = minY; y <= maxY; ++y)
				for (int x = minX; x <= maxX; ++x)
					_barrierIndex[{x, y, z}].emplace_back(index, i);
	}
}

void RealHdPhysicalGeometry::rebuildBarrierIndex()
{
	_barrierIndex.clear();
	for (std::size_t i = 0; i < _tiles.size(); ++i) indexTile(i);
}

void RealHdPhysicalGeometry::addTile(RealHdTilePhysics tile)
{
	_ready = false;
	_denseTileIndex.clear();
	for (const RealHdSupportSurface &support : tile.supports)
		for (RealHdBarrierTriangle face : support.physicalFaces)
		{
			face.kind = RealHdBarrierKind::SupportSurface;
			tile.barriers.push_back(std::move(face));
		}
	const CellKey key{tile.x, tile.y, tile.z};
	const auto found = _tileIndex.find(key);
	if (found != _tileIndex.end())
	{
		_tiles[found->second] = std::move(tile);
		rebuildBarrierIndex();
		_hasVoxelVolumes = false;
		for (const RealHdTilePhysics &candidate : _tiles)
			_hasVoxelVolumes = _hasVoxelVolumes || candidate.hasVoxelVolume;
		return;
	}
	_tileIndex.emplace(key, _tiles.size());
	_tiles.push_back(std::move(tile));
	_hasVoxelVolumes = _hasVoxelVolumes || _tiles.back().hasVoxelVolume;
	indexTile(_tiles.size() - 1);
}

bool RealHdPhysicalGeometry::seal(int width, int height, int levels)
{
	_ready = false;
	if (width <= 0 || height <= 0 || levels <= 0) return false;
	const std::size_t expected = std::size_t(width) * std::size_t(height) * std::size_t(levels);
	if (_tileIndex.size() != expected) return false;
	for (const auto &entry : _tileIndex)
	{
		const CellKey &p = entry.first;
		if (p.x < 0 || p.x >= width || p.y < 0 || p.y >= height || p.z < 0 || p.z >= levels)
			return false;
	}
	_denseTileIndex.assign(expected, std::size_t(-1));
	for (const auto &entry : _tileIndex)
	{
		const CellKey &p = entry.first;
		_denseTileIndex[(std::size_t(p.z) * height + p.y) * width + p.x] = entry.second;
	}
	const auto validPoint = [&](const RealHdPoint3 &p)
	{
		return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)
			&& p.x >= 0.0 && p.x <= width * TILE_XY
			&& p.y >= 0.0 && p.y <= height * TILE_XY
			&& p.z >= 0.0 && p.z <= levels * TILE_Z;
	};
	for (const RealHdTilePhysics &current : _tiles)
		for (const RealHdBarrierTriangle &barrier : current.barriers)
		{
			if (!validPoint(barrier.a) || !validPoint(barrier.b) || !validPoint(barrier.c))
				return false;
			const RealHdPoint3 normal = cross(subtract(barrier.b, barrier.a),
				subtract(barrier.c, barrier.a));
			if (dot(normal, normal) <= EPSILON * EPSILON) return false;
		}
	_width = width;
	_height = height;
	_levels = levels;
	_ready = true;
	return true;
}

const RealHdTilePhysics *RealHdPhysicalGeometry::tile(int x, int y, int z) const
{
	if (_ready && x >= 0 && x < _width && y >= 0 && y < _height
		&& z >= 0 && z < _levels)
		return &_tiles[_denseTileIndex[(std::size_t(z) * _height + y) * _width + x]];
	const auto found = _tileIndex.find({x, y, z});
	return found == _tileIndex.end() ? nullptr : &_tiles[found->second];
}

bool RealHdPhysicalGeometry::canStand(int x, int y, int z) const
{
	if (!_ready) return false;
	const RealHdTilePhysics *current = tile(x, y, z);
	if (!current || current->forbidsStanding) return false;
	for (const RealHdSupportSurface &support : current->supports)
		if (support.supportsWalking) return true;
	return false;
}

bool RealHdPhysicalGeometry::voxelSolid(int x, int y, int z, std::uint8_t use) const
{
	if (x < 0 || y < 0 || z < 0) return true;
	const RealHdTilePhysics *current = tile(x / 16, y / 16, z / 24);
	if (!current || !current->hasVoxelVolume || (current->voxelUses & use) == 0) return false;
	const int localX = x % 16, localY = y % 16, localZ = z % 24;
	const std::size_t rowIndex = std::size_t(localZ) * 16 + localY;
	const bool sight = (use & (BlockSight | BlockLight)) != 0;
	const std::uint16_t row = sight
		? (rowIndex < current->sightVoxelRows.size() ? current->sightVoxelRows[rowIndex] : 0)
		: current->solidVoxelRows[rowIndex];
	return (row & (std::uint16_t(1u) << (15 - localX))) != 0;
}

bool RealHdPhysicalGeometry::rayClear(const RealHdPoint3 &from,
	const RealHdPoint3 &to, std::uint8_t use) const
{
	if (!_ready || !std::isfinite(from.x) || !std::isfinite(from.y) || !std::isfinite(from.z)
		|| !std::isfinite(to.x) || !std::isfinite(to.y) || !std::isfinite(to.z))
		return false;
	const auto inside = [&](const RealHdPoint3 &p)
	{
		return p.x >= 0.0 && p.x < _width * TILE_XY &&
			p.y >= 0.0 && p.y < _height * TILE_XY &&
			p.z >= 0.0 && p.z < _levels * TILE_Z;
	};
	if (!inside(from) || !inside(to)) return false;
	const RealHdPoint3 direction = subtract(to, from);
	int x = cell(from.x, TILE_XY);
	int y = cell(from.y, TILE_XY);
	int z = cell(from.z, TILE_Z);
	const int endX = cell(to.x, TILE_XY);
	const int endY = cell(to.y, TILE_XY);
	const int endZ = cell(to.z, TILE_Z);
	AxisWalk wx = axisWalk(from.x, direction.x, x, TILE_XY);
	AxisWalk wy = axisWalk(from.y, direction.y, y, TILE_XY);
	AxisWalk wz = axisWalk(from.z, direction.z, z, TILE_Z);
	for (;;)
	{
		const auto found = _barrierIndex.find({x, y, z});
		if (found != _barrierIndex.end())
			for (const BarrierRef &ref : found->second)
			{
				const RealHdBarrierTriangle &barrier = _tiles[ref.first].barriers[ref.second];
				if ((barrier.uses & use) != 0 && intersects(from, direction, barrier))
					return false;
			}
		if (x == endX && y == endY && z == endZ) break;
		const double next = std::min({wx.next, wy.next, wz.next});
		if (!std::isfinite(next) || next > 1.0 + EPSILON) break;
		if (wx.next <= next + EPSILON) { x += wx.step; wx.next += wx.delta; }
		if (wy.next <= next + EPSILON) { y += wy.step; wy.next += wy.delta; }
		if (wz.next <= next + EPSILON) { z += wz.step; wz.next += wz.delta; }
	}
	if (_hasVoxelVolumes)
	{
		int vx = cell(from.x, 1.0), vy = cell(from.y, 1.0), vz = cell(from.z, 1.0);
		const int endVx = cell(to.x, 1.0), endVy = cell(to.y, 1.0), endVz = cell(to.z, 1.0);
		AxisWalk ax = axisWalk(from.x, direction.x, vx, 1.0);
		AxisWalk ay = axisWalk(from.y, direction.y, vy, 1.0);
		AxisWalk az = axisWalk(from.z, direction.z, vz, 1.0);
		bool first = true;
		for (;;)
		{
			const bool last = vx == endVx && vy == endVy && vz == endVz;
			if (!first && !last && voxelSolid(vx, vy, vz, use)) return false;
			if (last) break;
			const double next = std::min({ax.next, ay.next, az.next});
			if (!std::isfinite(next) || next >= 1.0 - EPSILON) break;
			const bool crossX = ax.next <= next + EPSILON;
			const bool crossY = ay.next <= next + EPSILON;
			const bool crossZ = az.next <= next + EPSILON;
			// Supercover all voxels touched at a simultaneous edge/corner crossing.
			for (int subset = 1; subset < 8; ++subset)
			{
				if (((subset & 1) && !crossX) || ((subset & 2) && !crossY)
					|| ((subset & 4) && !crossZ)) continue;
				const int nx = vx + ((subset & 1) ? ax.step : 0);
				const int ny = vy + ((subset & 2) ? ay.step : 0);
				const int nz = vz + ((subset & 4) ? az.step : 0);
				if (!(nx == endVx && ny == endVy && nz == endVz)
					&& voxelSolid(nx, ny, nz, use)) return false;
			}
			if (crossX) { vx += ax.step; ax.next += ax.delta; }
			if (crossY) { vy += ay.step; ay.next += ay.delta; }
			if (crossZ) { vz += az.step; az.next += az.delta; }
			first = false;
		}
	}
	return true;
}
}
