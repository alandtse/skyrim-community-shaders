#pragma once

#include "SceneSettingsCatalog.generated.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace Util::Settings
{
	/** @brief Removes an ImGui identity suffix from visible text. */
	std::string StripImGuiId(std::string_view label);
	/** @brief Decodes escaped catalogue path segments. */
	std::vector<std::string> SplitCatalogPath(std::string_view path);
	/** @brief Identifies structural wrappers omitted from setting labels. */
	bool IsStructuralDisplayPart(std::string_view part);
	/** @brief Formats a catalogue display segment. */
	std::string NormalizeDisplayPart(std::string part);
	/** @brief Returns the translated display hierarchy shared by settings selectors. */
	std::vector<std::string> GetCatalogDisplayPath(const SceneSettingsCatalog::SettingMetadata& setting);
	/** @brief Returns the translated setting label shared by settings selectors. */
	std::string GetCatalogLeafDisplayName(const SceneSettingsCatalog::SettingMetadata& setting);

	struct ExportSetting
	{
		std::string path;
		std::string label;
	};

	/** @brief Lists persisted JSON leaves using catalogue labels when available; arrays remain whole. */
	std::vector<ExportSetting> GetExportSettings(std::string_view featureShortName, const nlohmann::json& settings);
}
