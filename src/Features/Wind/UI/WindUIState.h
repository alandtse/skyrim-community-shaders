#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <vector>

enum class WindSettingsPage
{
	WindField,
	WindEffects,
	Trees,
	TreeMeshes,
	Grass
};

/** Transient menu and tree-editor state for the Wind settings UI. */
struct WindUIState
{
	WindSettingsPage activeSettingsPage = WindSettingsPage::WindField;
	std::size_t activeWindEffectIndex = 0;
	std::array<char, 256> treeMeshSearch{};
	std::vector<std::size_t> filteredTreeRuleIndices;
	std::string appliedTreeMeshSearch;
	std::string treeWindSaveStatus;
	std::size_t filteredTreeRuleCount = static_cast<std::size_t>(-1);
	int debugWindEffectSpawnCount = 8;
	bool debugWindEffectWorstCase = false;
	bool treeWindSaveSucceeded = false;
};
