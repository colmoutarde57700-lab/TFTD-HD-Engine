#pragma once
#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include "../Engine/Yaml.h"

namespace OpenXcom
{

/**
 * Generic true-colour visual animation/state definition used by the HD layer.
 * It deliberately lives beside gameplay rules and does not alter gameplay data.
 */
struct HdVisualTransition
{
	std::string state;
	int weight = 100;

	void load(const YAML::YamlNodeReader &reader)
	{
		reader.tryRead("state", state);
		reader.tryRead("weight", weight);
		weight = std::max(0, weight);
	}
};

struct HdVisualState
{
	std::string name;
	std::vector<std::string> frames;
	// Real-time animation in milliseconds, driven by the independent HD presentation clock.
	int frameMs = 100;
	// Optional turn-based animation; when >0 it overrides frameMs.
	int frameTurns = 0;
	// Number of complete clip cycles before choosing a transition.
	int minRepeats = 1;
	int maxRepeats = 1;
	// Optional state duration independent of frame count. 0 = use repeat count.
	int minDurationMs = 0;
	int maxDurationMs = 0;
	int minDurationTurns = 0;
	int maxDurationTurns = 0;
	bool holdLastFrame = false;
	std::vector<HdVisualTransition> next;

	void load(const YAML::YamlNodeReader &reader)
	{
		reader.tryRead("name", name);
		reader.tryRead("frames", frames);
		reader.tryRead("frameMs", frameMs);
		reader.tryRead("frameTurns", frameTurns);
		reader.tryRead("minRepeats", minRepeats);
		reader.tryRead("maxRepeats", maxRepeats);
		reader.tryRead("minDurationMs", minDurationMs);
		reader.tryRead("maxDurationMs", maxDurationMs);
		reader.tryRead("minDurationTurns", minDurationTurns);
		reader.tryRead("maxDurationTurns", maxDurationTurns);
		reader.tryRead("holdLastFrame", holdLastFrame);
		frameMs = std::max(1, frameMs);
		frameTurns = std::max(0, frameTurns);
		minRepeats = std::max(1, minRepeats);
		maxRepeats = std::max(minRepeats, maxRepeats);
		minDurationMs = std::max(0, minDurationMs);
		maxDurationMs = std::max(minDurationMs, maxDurationMs);
		minDurationTurns = std::max(0, minDurationTurns);
		maxDurationTurns = std::max(minDurationTurns, maxDurationTurns);
		for (const auto &n : reader["next"].children())
		{
			HdVisualTransition t;
			t.load(n);
			if (!t.state.empty() && t.weight > 0) next.push_back(t);
		}
	}
};

struct HdVisualLayer
{
	std::string id = "base";
	std::string root;
	std::string initialState = "idle";
	int nativeScale = 8; // explicit scale for rule-driven assets; use 16 for new 512x640 art
	// RC12 P8 colour policy:
	// auto            = environment, regardless of PNG storage format
	// indexedLegacy   = explicit historical TFTD palette/index semantics
	// environment/rgba = authored RGB/RGBA world art + modern semantic HD material grade
	// fixed/native     = explicit opt-out; authored colour is final apart from ordinary local shade
	//
	// IMPORTANT: a PNG using an internal 8-bit palette is NOT automatically
	// Legacy. File encoding and renderer colour semantics are independent.
	std::string colorMode = "auto";
	// Modern true-colour material profile used by the HD renderer. Optional.
	// Examples: terrain, organic, rock, wreck, craft_exterior, craft_interior, unit, effect.
	std::string materialProfile;
	int offsetX = 0;
	int offsetY = 0;
	bool randomPhase = false;
	int speedVariancePct = 0;
	std::map<std::string, HdVisualState> states;

	void load(const YAML::YamlNodeReader &reader)
	{
		reader.tryRead("id", id);
		reader.tryRead("root", root);
		reader.tryRead("initialState", initialState);
		reader.tryRead("nativeScale", nativeScale);
		reader.tryRead("colorMode", colorMode);
		reader.tryRead("materialProfile", materialProfile);
		reader.tryRead("offsetX", offsetX);
		reader.tryRead("offsetY", offsetY);
		reader.tryRead("randomPhase", randomPhase);
		reader.tryRead("speedVariancePct", speedVariancePct);
		nativeScale = std::max(1, nativeScale);
		speedVariancePct = std::max(0, std::min(90, speedVariancePct));
		for (const auto &sreader : reader["states"].children())
		{
			HdVisualState s;
			s.load(sreader);
			if (!s.name.empty()) states[s.name] = s;
		}
		// Convenient shorthand for a simple clip with no explicit states.
		if (states.empty() && reader["frames"])
		{
			HdVisualState s;
			s.name = initialState;
			reader.tryRead("frames", s.frames);
			reader.tryRead("frameMs", s.frameMs);
			reader.tryRead("frameTurns", s.frameTurns);
			s.frameMs = std::max(1, s.frameMs);
			s.frameTurns = std::max(0, s.frameTurns);
			states[s.name] = s;
		}
	}
};

struct HdVisualRule
{
	std::string id;
	std::string target = "terrain"; // terrain, surfaceSet, ui (future-compatible)
	std::string dataset;
	std::string part; // FLOOR, WESTWALL, NORTHWALL, OBJECT
	int mcd = -1;
	int sprite = -1;
	std::string setName;
	int setFrame = -1;
	int depthMin = -1;
	int depthMax = -1;
	bool replaceLegacy = true;
	std::vector<HdVisualLayer> layers;

	void load(const YAML::YamlNodeReader &reader)
	{
		reader.tryRead("id", id);
		reader.tryRead("target", target);
		reader.tryRead("dataset", dataset);
		reader.tryRead("part", part);
		reader.tryRead("mcd", mcd);
		reader.tryRead("sprite", sprite);
		reader.tryRead("set", setName);
		reader.tryRead("frame", setFrame);
		reader.tryRead("depthMin", depthMin);
		reader.tryRead("depthMax", depthMax);
		reader.tryRead("replaceLegacy", replaceLegacy);
		for (const auto &lreader : reader["layers"].children())
		{
			HdVisualLayer layer;
			layer.load(lreader);
			if (!layer.states.empty()) layers.push_back(layer);
		}
		// Shorthand: the rule itself can be one layer.
		if (layers.empty() && (reader["states"] || reader["frames"]))
		{
			HdVisualLayer layer;
			layer.load(reader);
			if (!layer.states.empty()) layers.push_back(layer);
		}
	}

	bool depthMatches(int depth) const
	{
		if (depthMin >= 0 && depth < depthMin) return false;
		if (depthMax >= 0 && depth > depthMax) return false;
		return true;
	}
};

}
