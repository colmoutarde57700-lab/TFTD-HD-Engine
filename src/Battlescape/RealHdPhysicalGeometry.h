#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace OpenXcom
{
// Native REAL HD geometry is independent of the four legacy MAP slots.
// Coordinates are in tactical voxels (16 x 16 x 24 per historical tile).
struct RealHdPoint3
{
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
};

enum class RealHdSurfaceShape : std::uint8_t
{
	Full, TriangleNE, TriangleNW, TriangleSE, TriangleSW, Custom
};

enum class RealHdBarrierKind : std::uint8_t
{
	SupportSurface, CardinalWall, DiagonalWall, ObjectSurface, Ceiling, TerrainVolume
};

enum RealHdBarrierUse : std::uint8_t
{
	BlockSight = 1u << 0,
	BlockLight = 1u << 1,
	BlockProjectiles = 1u << 2,
	BlockMovement = 1u << 3
};

// A diagonal wall is a vertical quad represented by two triangles, with no
// implied north/west wall. Cave, coral and overhang surfaces use the same
// physical triangles. Texture alpha never decides these collision flags.
struct RealHdBarrierTriangle
{
	RealHdPoint3 a, b, c;
	RealHdBarrierKind kind = RealHdBarrierKind::ObjectSurface;
	std::uint8_t uses = 0;
};

struct RealHdSupportSurface
{
	std::string surfaceId; // e.g. SAND, VOLC, a ship deck or a galley floor.
	RealHdSurfaceShape shape = RealHdSurfaceShape::Full;
	double height = 0.0;
	bool supportsWalking = false;
	// The actual faces may be sloped, perforated, curved or multilayered. The
	// renderer and gameplay must consume these same resolved faces; shape/height
	// are authoring metadata, never a substitute for physical geometry.
	std::vector<RealHdBarrierTriangle> physicalFaces;
};

struct RealHdTilePhysics
{
	int x = 0, y = 0, z = 0;
	// Unit placement is independent of the visible floor and of any BigWall.
	bool forbidsStanding = false;
	std::vector<RealHdSupportSurface> supports;
	std::vector<RealHdBarrierTriangle> barriers;
	// Compact optional volume for imported MCD/LOFT and native perforated solids.
	// Rows are [localZ * 16 + localY], with bit (15 - localX) set for solid.
	std::array<std::uint16_t, 24 * 16> solidVoxelRows{};
	// MCD sight semantics are resolved at import time. A decorative LOFT can
	// occupy space without hiding the terrain behind it.
	std::vector<std::uint16_t> sightVoxelRows;
	bool hasVoxelVolume = false;
	std::uint8_t voxelUses = BlockSight | BlockLight | BlockProjectiles | BlockMovement;
};

// An editor can resolve PLAN and biome into these cells; a legacy importer can
// populate them from MAP/MCD plus explicit, reviewed corrections. Game systems
// consume this resolved geometry rather than interpreting O_OBJECT/BigWall.
class RealHdPhysicalGeometry
{
public:
	void clear();
	void addTile(RealHdTilePhysics tile);
	// Fail closed until every map cell has an explicit native record. Empty air
	// is represented by an empty tile record, never by an absent record.
	bool seal(int width, int height, int levels);
	bool ready() const { return _ready; }
	const RealHdTilePhysics *tile(int x, int y, int z) const;
	bool canStand(int x, int y, int z) const;
	bool rayClear(const RealHdPoint3 &from, const RealHdPoint3 &to,
		std::uint8_t use) const;

private:
	struct CellKey
	{
		int x, y, z;
		bool operator==(const CellKey &other) const
		{
			return x == other.x && y == other.y && z == other.z;
		}
	};
	struct CellKeyHash
	{
		std::size_t operator()(const CellKey &key) const noexcept;
	};
	using BarrierRef = std::pair<std::size_t, std::size_t>;
	void indexTile(std::size_t index);
	void rebuildBarrierIndex();
	bool voxelSolid(int x, int y, int z, std::uint8_t use) const;
	std::vector<RealHdTilePhysics> _tiles;
	std::vector<std::size_t> _denseTileIndex;
	std::unordered_map<CellKey, std::size_t, CellKeyHash> _tileIndex;
	std::unordered_map<CellKey, std::vector<BarrierRef>, CellKeyHash> _barrierIndex;
	int _width = 0, _height = 0, _levels = 0;
	bool _ready = false;
	bool _hasVoxelVolumes = false;
};
}
