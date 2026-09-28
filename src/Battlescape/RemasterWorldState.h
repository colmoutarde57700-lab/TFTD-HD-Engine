#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <tuple>

namespace OpenXcom
{

/**
 * Renderer-neutral snapshot of the tactical world owned by the remaster layer.
 *
 * This type intentionally contains no Tile, MapData, Surface, PCK, framebuffer,
 * painter-order or _drawOrderBuffer references. OXCE may populate it through an
 * adapter during the migration, but REAL HD can consume the copied facts without
 * consulting the Legacy raster.
 */
struct RemasterVec3i
{
	int x = 0;
	int y = 0;
	int z = 0;
};

enum class RemasterTerrainPartKind : std::uint8_t
{
	Floor = 0,
	WestWall = 1,
	NorthWall = 2,
	Object = 3
};

struct RemasterTileState
{
	RemasterVec3i position;
	int terrainLevel = 0;
	int visibleNow = 0;
	std::array<bool, 4> discovered{{false, false, false, false}};
	int smoke = 0;
	int fire = 0;
	int lightAmbient = 0;
	int lightFire = 0;
	int lightItems = 0;
	int lightUnits = 0;
	int shade = 0;
};

struct RemasterTerrainPartState
{
	RemasterVec3i tile;
	RemasterTerrainPartKind part = RemasterTerrainPartKind::Floor;
	std::string datasetName;
	int datasetIndex = -1;
	int mapDataId = -1;
	int mapDataSetId = -1;
	int logicalAnimationFrame = 0;
	std::array<int, 12> loftIds{{-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1}};
	int yOffset = 0;
	int terrainLevel = 0;
	int bigWall = 0;
	bool isDoor = false;
	bool isUfoDoor = false;
	bool ufoDoorOpen = false;
	bool noFloor = false;
};

enum class RemasterUnitAction : std::uint8_t
{
	Unknown, Standing, Walking, Flying, Turning, Aiming, Collapsing,
	Dead, Unconscious, Panicking, Berserk, Ignored
};

enum class RemasterFaction : std::uint8_t
{
	Unknown, Player, Hostile, Neutral
};

struct RemasterUnitState
{
	int id = -1;
	// Semantic identity and pose. Neither a sprite sheet nor its pixels are
	// authoritative for equipment, posture or the future animated 3D model.
	std::string armorType;
	RemasterVec3i position;
	RemasterVec3i lastPosition;
	RemasterVec3i destination;
	int direction = 0;
	RemasterUnitAction action = RemasterUnitAction::Unknown;
	RemasterFaction faction = RemasterFaction::Unknown;
	int walkingPhase = 0;
	int fallingPhase = 0;
	int turretDirection = 0;
	int verticalDirection = 0;
	int footprintSize = 1;
	int height = 0;
	int fire = 0;
	bool kneeled = false;
	bool floating = false;
	bool visible = false;
	bool out = false;
};

struct RemasterItemState
{
	int id = -1;
	std::string type;
	RemasterVec3i tile;
	bool hasTile = false;
	int ownerUnitId = -1;
	// An equipped item remains identifiable without consulting HANDOB or
	// inspecting the unit's already composed image. Empty slot identifies a
	// built-in/special weapon when ownerUnitId is set.
	std::string inventorySlot;
	int inventoryX = 0;
	int inventoryY = 0;
	int ammunition = 0;
	int fuseTimer = -1;
};

struct RemasterWorldState
{
	std::uint64_t revision = 0;
	int mapSizeX = 0;
	int mapSizeY = 0;
	int mapSizeZ = 0;
	int missionDepth = 0;
	int turn = 0;
	int side = 0;
	std::string missionType;
	std::vector<RemasterTileState> tiles;
	std::vector<RemasterTerrainPartState> terrainParts;
	std::vector<RemasterUnitState> units;
	std::vector<RemasterItemState> items;

	void clear();
};

// Camera-independent terrain inventory. The keys remain stable while the view
// moves; a later renderer can upload a dirty chunk without rebuilding the map.
// No Surface, sprite pixels, painter order or screen coordinates enter here.
struct RemasterStaticInstance
{
	std::uint64_t key = 0;
	RemasterVec3i worldTile;
	RemasterTerrainPartKind part = RemasterTerrainPartKind::Floor;
	std::string datasetName;
	int mapDataId = -1;
	int mapDataSetId = -1;
	int frame = 0;
	int terrainLevel = 0;
	int yOffset = 0;
	int bigWall = 0;
};

struct RemasterStaticChunk
{
	int x = 0, y = 0, z = 0;
	std::vector<RemasterStaticInstance> instances;
};

class RemasterStaticScene
{
public:
	static constexpr int ChunkSize = 16;
	std::uint64_t revision = 0;
	std::map<std::tuple<int, int, int>, RemasterStaticChunk> chunks;
	void rebuild(const RemasterWorldState &world);
	void clear();
	std::size_t instanceCount() const;
};

}
