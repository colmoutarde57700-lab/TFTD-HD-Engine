#include "RealHdVisibilitySolver.h"

#include <algorithm>
#include <cmath>

#include "BedrockRender.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/Tile.h"
#include "../Mod/MapData.h"
#include "TileEngine.h"

namespace OpenXcom
{
namespace
{

struct Vec3
{
	double x = 0.0, y = 0.0, z = 0.0;
};

Vec3 operator-(const Vec3 &a, const Vec3 &b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
Vec3 cross(const Vec3 &a, const Vec3 &b)
{
	return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x};
}
double dot(const Vec3 &a, const Vec3 &b) { return a.x*b.x + a.y*b.y + a.z*b.z; }

bool segmentTriangle(const Vec3 &origin, const Vec3 &end, const Vec3 &a, const Vec3 &b, const Vec3 &c, double &outT)
{
	const Vec3 dir = end - origin;
	const Vec3 e1 = b - a;
	const Vec3 e2 = c - a;
	const Vec3 p = cross(dir, e2);
	const double det = dot(e1, p);
	if (std::abs(det) < 1e-9) return false;
	const double invDet = 1.0 / det;
	const Vec3 s = origin - a;
	const double u = dot(s, p) * invDet;
	if (u < -1e-7 || u > 1.0 + 1e-7) return false;
	const Vec3 q = cross(s, e1);
	const double v = dot(dir, q) * invDet;
	if (v < -1e-7 || u + v > 1.0 + 1e-7) return false;
	const double t = dot(e2, q) * invDet;
	if (t <= 0.0005 || t >= 0.995) return false; // ignore eye/target endpoint contact
	outT = t;
	return true;
}

Vec3 bedrockCorner(int tileX, int tileY, int tileZ, int corner, const BedrockCellGeometry &g)
{
	const double baseZ = (double)tileZ * Position::TileZ;
	switch (corner)
	{
		case 0: return {(double)tileX * Position::TileXY,       (double)tileY * Position::TileXY,       baseZ - g.top};
		case 1: return {(double)(tileX+1) * Position::TileXY,   (double)tileY * Position::TileXY,       baseZ - g.right};
		case 2: return {(double)(tileX+1) * Position::TileXY,   (double)(tileY+1) * Position::TileXY,   baseZ - g.bottom};
		default:return {(double)tileX * Position::TileXY,       (double)(tileY+1) * Position::TileXY,   baseZ - g.left};
	}
}

bool candidatePoint(const SavedBattleGame *save, const Tile *tile, TilePart part, Vec3 &out, bool &bedrock)
{
	if (!save || !tile || part != O_FLOOR) return false;
	const Position p = tile->getPosition();
	const BedrockMaterial material = BedrockRenderPolicy::resolve(save);
	if (material != BedrockMaterial::None && BedrockRenderPolicy::hasSurface(material, tile))
	{
		const BedrockCellGeometry g = BedrockRenderPolicy::cellGeometry(material, tile);
		const double z = (double)p.z * Position::TileZ - ((double)g.top + g.right + g.bottom + g.left) * 0.25;
		out = {(double)p.x * Position::TileXY + 8.0, (double)p.y * Position::TileXY + 8.0, z + 0.75};
		bedrock = true;
		return true;
	}

	MapData *floor = tile->getMapData(O_FLOOR);
	if (!floor || tile->hasNoFloor(const_cast<SavedBattleGame*>(save))) return false;
	const double z = (double)p.z * Position::TileZ - floor->getTerrainLevel();
	out = {(double)p.x * Position::TileXY + 8.0, (double)p.y * Position::TileXY + 8.0, z + 0.75};
	bedrock = false;
	return true;
}

bool hitTri(const Vec3 &origin, const Vec3 &target, const Vec3 &a, const Vec3 &b, const Vec3 &c, int &tested)
{
	++tested;
	double t = 0.0;
	return segmentTriangle(origin, target, a, b, c, t);
}

bool hitQuad(const Vec3 &origin, const Vec3 &target, const Vec3 &a, const Vec3 &b, const Vec3 &c, const Vec3 &d, int &tested)
{
	return hitTri(origin, target, a, b, c, tested) || hitTri(origin, target, b, d, c, tested);
}

}

RealHdVisibilityAuditResult RealHdVisibilitySolver::auditSurfaceLos(const SavedBattleGame *save, const BattleUnit *observer, const Tile *targetTile, TilePart part)
{
	RealHdVisibilityAuditResult result;
	if (!save || !observer || !targetTile || part != O_FLOOR || !save->getTileEngine()) return result;

	Vec3 target;
	if (!candidatePoint(save, targetTile, part, target, result.targetBedrock)) return result;
	result.supported = true;

	const Position eyeVoxel = save->getTileEngine()->getSightOriginVoxel(const_cast<BattleUnit*>(observer));
	const Vec3 eye{(double)eyeVoxel.x, (double)eyeVoxel.y, (double)eyeVoxel.z};

	// V1A models top surfaces.  An observer below a top surface cannot see its
	// presentation side; roof/underside semantics are deferred to multi-Z V1E.
	if (eye.z <= target.z + 0.05)
	{
		result.visible = false;
		return result;
	}

	const Position targetPos = targetTile->getPosition();
	const BedrockMaterial material = BedrockRenderPolicy::resolve(save);
	const int mapW = save->getMapSizeX();
	const int mapH = save->getMapSizeY();
	const int mapZ = save->getMapSizeZ();
	const int minX = std::max(0, (int)std::floor(std::min(eye.x, target.x) / Position::TileXY) - 1);
	const int maxX = std::min(mapW - 1, (int)std::floor(std::max(eye.x, target.x) / Position::TileXY) + 1);
	const int minY = std::max(0, (int)std::floor(std::min(eye.y, target.y) / Position::TileXY) - 1);
	const int maxY = std::min(mapH - 1, (int)std::floor(std::max(eye.y, target.y) / Position::TileXY) + 1);

	for (int z = 0; z < mapZ; ++z)
	{
		for (int y = minY; y <= maxY; ++y)
		{
			for (int x = minX; x <= maxX; ++x)
			{
				const Tile *tile = save->getTile(Position(x, y, z));
				if (!tile) continue;
				if (x == targetPos.x && y == targetPos.y && z == targetPos.z) continue;

				if (material != BedrockMaterial::None && BedrockRenderPolicy::hasSurface(material, tile))
				{
					const BedrockCellGeometry g = BedrockRenderPolicy::cellGeometry(material, tile);
					const Vec3 a = bedrockCorner(x, y, z, 0, g);
					const Vec3 b = bedrockCorner(x, y, z, 1, g);
					const Vec3 c = bedrockCorner(x, y, z, 2, g);
					const Vec3 d = bedrockCorner(x, y, z, 3, g);
					if (hitQuad(eye, target, a, b, d, c, result.testedTriangles))
					{
						result.visible = false; result.hit = true; result.hitTile = tile->getPosition(); return result;
					}

					// Model the same east/south discontinuity skins used by the BEDROCK mesh.
					if (x + 1 < mapW)
					{
						const Tile *east = save->getTile(Position(x + 1, y, z));
						if (east && BedrockRenderPolicy::hasSurface(material, east))
						{
							const BedrockCellGeometry n = BedrockRenderPolicy::cellGeometry(material, east);
							if (g.right != n.top || g.bottom != n.left)
							{
								const Vec3 e0={(double)(x+1)*16.0,(double)y*16.0,(double)z*24.0-g.right};
								const Vec3 e1={(double)(x+1)*16.0,(double)(y+1)*16.0,(double)z*24.0-g.bottom};
								const Vec3 e2={(double)(x+1)*16.0,(double)y*16.0,(double)z*24.0-n.top};
								const Vec3 e3={(double)(x+1)*16.0,(double)(y+1)*16.0,(double)z*24.0-n.left};
								if (hitQuad(eye,target,e0,e1,e2,e3,result.testedTriangles)) { result.visible=false; result.hit=true; result.hitTile=tile->getPosition(); return result; }
							}
						}
					}
					if (y + 1 < mapH)
					{
						const Tile *south = save->getTile(Position(x, y + 1, z));
						if (south && BedrockRenderPolicy::hasSurface(material, south))
						{
							const BedrockCellGeometry n = BedrockRenderPolicy::cellGeometry(material, south);
							if (g.left != n.top || g.bottom != n.right)
							{
								const Vec3 e0={(double)x*16.0,(double)(y+1)*16.0,(double)z*24.0-g.left};
								const Vec3 e1={(double)(x+1)*16.0,(double)(y+1)*16.0,(double)z*24.0-g.bottom};
								const Vec3 e2={(double)x*16.0,(double)(y+1)*16.0,(double)z*24.0-n.top};
								const Vec3 e3={(double)(x+1)*16.0,(double)(y+1)*16.0,(double)z*24.0-n.right};
								if (hitQuad(eye,target,e0,e1,e2,e3,result.testedTriangles)) { result.visible=false; result.hit=true; result.hitTile=tile->getPosition(); return result; }
							}
						}
					}
					continue;
				}

				MapData *floor = tile->getMapData(O_FLOOR);
				if (!floor || tile->hasNoFloor(const_cast<SavedBattleGame*>(save))) continue;
				const double floorZ = (double)z * Position::TileZ - floor->getTerrainLevel();
				const Vec3 a={(double)x*16.0,(double)y*16.0,floorZ};
				const Vec3 b={(double)(x+1)*16.0,(double)y*16.0,floorZ};
				const Vec3 c={(double)x*16.0,(double)(y+1)*16.0,floorZ};
				const Vec3 d={(double)(x+1)*16.0,(double)(y+1)*16.0,floorZ};
				if (hitQuad(eye, target, a, b, c, d, result.testedTriangles))
				{
					result.visible = false; result.hit = true; result.hitTile = tile->getPosition(); return result;
				}
			}
		}
	}

	result.visible = true;
	return result;
}

}
