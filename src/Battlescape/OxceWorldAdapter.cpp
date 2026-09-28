#include "OxceWorldAdapter.h"

#include "RemasterWorldState.h"
#include "Position.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/Tile.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/BattleItem.h"
#include "../Mod/MapData.h"
#include "../Mod/MapDataSet.h"
#include "../Mod/RuleItem.h"
#include "../Mod/Armor.h"
#include "../Mod/RuleInventory.h"

namespace OpenXcom
{

namespace
{

RemasterVec3i copyPos(const Position &p)
{
	RemasterVec3i out;
	out.x = p.x;
	out.y = p.y;
	out.z = p.z;
	return out;
}

RemasterTerrainPartKind partKind(TilePart part)
{
	switch (part)
	{
	case O_WESTWALL: return RemasterTerrainPartKind::WestWall;
	case O_NORTHWALL: return RemasterTerrainPartKind::NorthWall;
	case O_OBJECT: return RemasterTerrainPartKind::Object;
	case O_FLOOR:
	default: return RemasterTerrainPartKind::Floor;
	}
}

RemasterUnitAction copyAction(UnitStatus status)
{
	switch (status)
	{
	case STATUS_STANDING: return RemasterUnitAction::Standing;
	case STATUS_WALKING: return RemasterUnitAction::Walking;
	case STATUS_FLYING: return RemasterUnitAction::Flying;
	case STATUS_TURNING: return RemasterUnitAction::Turning;
	case STATUS_AIMING: return RemasterUnitAction::Aiming;
	case STATUS_COLLAPSING: return RemasterUnitAction::Collapsing;
	case STATUS_DEAD: return RemasterUnitAction::Dead;
	case STATUS_UNCONSCIOUS: return RemasterUnitAction::Unconscious;
	case STATUS_PANICKING: return RemasterUnitAction::Panicking;
	case STATUS_BERSERK: return RemasterUnitAction::Berserk;
	case STATUS_IGNORE_ME: return RemasterUnitAction::Ignored;
	}
	return RemasterUnitAction::Unknown;
}

RemasterFaction copyFaction(UnitFaction faction)
{
	switch (faction)
	{
	case FACTION_PLAYER: return RemasterFaction::Player;
	case FACTION_HOSTILE: return RemasterFaction::Hostile;
	case FACTION_NEUTRAL: return RemasterFaction::Neutral;
	case FACTION_NONE:
	case FACTION_MAX: return RemasterFaction::Unknown;
	}
	return RemasterFaction::Unknown;
}

}

void OxceWorldAdapter::capture(SavedBattleGame *save, std::uint64_t revision, RemasterWorldState &out)
{
	out.clear();
	out.revision = revision;
	if (!save) return;

	out.mapSizeX = save->getMapSizeX();
	out.mapSizeY = save->getMapSizeY();
	out.mapSizeZ = save->getMapSizeZ();
	out.missionDepth = save->getDepth();
	out.turn = save->getTurn();
	out.side = (int)save->getSide();
	out.missionType = save->getMissionType();

	const size_t tileCapacity = (size_t)out.mapSizeX * (size_t)out.mapSizeY * (size_t)out.mapSizeZ;
	out.tiles.reserve(tileCapacity);
	out.terrainParts.reserve(tileCapacity * 2u);

	for (int z = 0; z < out.mapSizeZ; ++z)
	{
		for (int y = 0; y < out.mapSizeY; ++y)
		{
			for (int x = 0; x < out.mapSizeX; ++x)
			{
				const Tile *tile = save->getTile(Position(x, y, z));
				if (!tile) continue;

				RemasterTileState tileState;
				tileState.position = copyPos(tile->getPosition());
				tileState.terrainLevel = tile->getTerrainLevel();
				tileState.visibleNow = tile->getVisible();
				for (int part = O_FLOOR; part < O_MAX; ++part)
					tileState.discovered[(size_t)part] = tile->isDiscovered((TilePart)part);
				tileState.smoke = tile->getSmoke();
				tileState.fire = tile->getFire();
				tileState.lightAmbient = tile->getLight(LL_AMBIENT);
				tileState.lightFire = tile->getLight(LL_FIRE);
				tileState.lightItems = tile->getLight(LL_ITEMS);
				tileState.lightUnits = tile->getLight(LL_UNITS);
				tileState.shade = tile->getShade();
				out.tiles.push_back(tileState);

				for (int partValue = O_FLOOR; partValue < O_MAX; ++partValue)
				{
					const TilePart part = (TilePart)partValue;
					MapData *data = tile->getMapData(part);
					if (!data) continue;

					RemasterTerrainPartState partState;
					partState.tile = tileState.position;
					partState.part = partKind(part);
					partState.datasetIndex = data->getDatasetIndex();
					if (data->getDataset()) partState.datasetName = data->getDataset()->getName();
					tile->getMapData(&partState.mapDataId, &partState.mapDataSetId, part);
					partState.logicalAnimationFrame = tile->getCurrentFrame(part);
					for (int layer = 0; layer < 12; ++layer) partState.loftIds[(size_t)layer] = data->getLoftID(layer);
					partState.yOffset = tile->getYOffset(part);
					partState.terrainLevel = data->getTerrainLevel();
					partState.bigWall = data->getBigWall();
					partState.isDoor = tile->isDoor(part);
					partState.isUfoDoor = tile->isUfoDoor(part);
					partState.ufoDoorOpen = tile->isUfoDoorOpen(part);
					partState.noFloor = data->isNoFloor();
					out.terrainParts.push_back(partState);
				}
			}
		}
	}

	if (const std::vector<BattleUnit*> *units = save->getUnits())
	{
		out.units.reserve(units->size());
		for (const BattleUnit *unit : *units)
		{
			if (!unit) continue;
			RemasterUnitState state;
			state.id = unit->getId();
			if (const Armor *armor = unit->getArmor())
			{
				state.armorType = armor->getType();
				state.footprintSize = armor->getSize();
			}
			state.position = copyPos(unit->getPosition());
			state.lastPosition = copyPos(unit->getLastPosition());
			state.destination = copyPos(unit->getDestination());
			state.direction = unit->getDirection();
			state.action = copyAction(unit->getStatus());
			state.faction = copyFaction(unit->getFaction());
			state.walkingPhase = unit->getWalkingPhase();
			state.fallingPhase = unit->getFallingPhase();
			state.turretDirection = unit->getTurretDirection();
			state.verticalDirection = unit->getVerticalDirection();
			state.height = unit->getHeight();
			state.fire = unit->getFire();
			state.kneeled = unit->isKneeled();
			state.floating = unit->isFloating();
			state.visible = unit->getVisible();
			state.out = unit->isOut();
			out.units.push_back(state);
		}
	}

	if (const std::vector<BattleItem*> *items = save->getItems())
	{
		out.items.reserve(items->size());
		for (const BattleItem *item : *items)
		{
			if (!item) continue;
			RemasterItemState state;
			state.id = item->getId();
			if (item->getRules()) state.type = item->getRules()->getType();
			if (const Tile *tile = item->getTile())
			{
				state.tile = copyPos(tile->getPosition());
				state.hasTile = true;
			}
			if (const BattleUnit *owner = item->getOwner()) state.ownerUnitId = owner->getId();
			if (const RuleInventory *slot = item->getSlot()) state.inventorySlot = slot->getId();
			state.inventoryX = item->getSlotX();
			state.inventoryY = item->getSlotY();
			state.ammunition = item->getAmmoQuantity();
			state.fuseTimer = item->getFuseTimer();
			out.items.push_back(state);
		}
	}
}

}
