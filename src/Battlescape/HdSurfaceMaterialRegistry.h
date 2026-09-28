#pragma once

#include <string>
#include <vector>
#include "../Mod/MapData.h"

namespace OpenXcom
{

/**
 * Presentation-only material binding for reusable HD surface families.
 * It never changes MAP/MCD gameplay semantics; it only answers which visual
 * material key a logical dataset/MCD/part may use when a renderer opts in.
 */
struct HdSurfaceMaterialRule
{
	std::string material;
	std::string dataset;
	std::vector<int> mcd;
	int mcdMin = -1;
	int mcdMax = -1;
	unsigned partMask = 0x0Fu;
};

class HdSurfaceMaterialRegistry
{
public:
	/** Lazy-loads Ruleset/TFTD_HD_surface_materials.yml through the VFS. */
	static const std::vector<HdSurfaceMaterialRule> &rules();

	/** Resolve a presentation material key for one stable logical MCD identity. */
	static std::string resolve(const MapData *data, TilePart part);

	/** Canonical V2 material-root convention: RealHD/Datasets/<DATASET>/Materials/<MATERIAL>/. */
	static std::string rootPath(const std::string &dataset, const std::string &material);
	static std::string topBasePath(const std::string &dataset, const std::string &material);
	static std::string topNormalPath(const std::string &dataset, const std::string &material);
	static std::string topRoughnessPath(const std::string &dataset, const std::string &material);
	static std::string topAoPath(const std::string &dataset, const std::string &material);
	static std::string verticalBasePath(const std::string &dataset, const std::string &material);
	static std::string verticalNormalPath(const std::string &dataset, const std::string &material);
	static std::string verticalRoughnessPath(const std::string &dataset, const std::string &material);
	static std::string verticalAoPath(const std::string &dataset, const std::string &material);

	/** Legacy path helpers retained only for compatibility with prepared/old callers. */
	static std::string legacyRootPath(const std::string &material);
};

}
