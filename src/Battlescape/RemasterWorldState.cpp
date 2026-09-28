#include "RemasterWorldState.h"

namespace OpenXcom
{

void RemasterWorldState::clear()
{
	mapSizeX = mapSizeY = mapSizeZ = 0;
	missionDepth = 0;
	turn = 0;
	side = 0;
	missionType.clear();
	tiles.clear();
	terrainParts.clear();
	units.clear();
	items.clear();
}

void RemasterStaticScene::clear()
{
	revision = 0;
	chunks.clear();
}

void RemasterStaticScene::rebuild(const RemasterWorldState &world)
{
	clear();
	for (const RemasterTerrainPartState &part : world.terrainParts)
	{
		const int cx = part.tile.x / ChunkSize;
		const int cy = part.tile.y / ChunkSize;
		const auto chunkKey = std::make_tuple(cx, cy, part.tile.z);
		RemasterStaticChunk &chunk = chunks[chunkKey];
		chunk.x = cx; chunk.y = cy; chunk.z = part.tile.z;
		RemasterStaticInstance instance;
		instance.worldTile = part.tile;
		instance.part = part.part;
		instance.datasetName = part.datasetName;
		instance.mapDataId = part.mapDataId;
		instance.mapDataSetId = part.mapDataSetId;
		instance.frame = part.logicalAnimationFrame;
		instance.terrainLevel = part.terrainLevel;
		instance.yOffset = part.yOffset;
		instance.bigWall = part.bigWall;
		instance.key = (((std::uint64_t)part.tile.z * (std::uint64_t)world.mapSizeY
			+ (std::uint64_t)part.tile.y) * (std::uint64_t)world.mapSizeX
			+ (std::uint64_t)part.tile.x) * 4u + (std::uint64_t)part.part;
		chunk.instances.push_back(std::move(instance));
	}
	revision = world.revision;
}

std::size_t RemasterStaticScene::instanceCount() const
{
	std::size_t total = 0;
	for (const auto &entry : chunks) total += entry.second.instances.size();
	return total;
}

}
