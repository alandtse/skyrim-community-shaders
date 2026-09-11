#include "SceneSettingsManager.h"

#include "Feature.h"
#include "Globals.h"
#include "Menu.h"
#include "SceneSettingsCatalog.generated.h"
#include "SceneSettingsPolicy.h"
#include "State.h"
#include "Utils/FileSystem.h"
#include "Utils/Format.h"
#include "Utils/Game.h"
#include "Utils/SettingsCatalog.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <numeric>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>

namespace
{
	using SceneSettingControlType = SceneSettingsManager::SettingControlType;
	using ManagerAggregatePresentation = SceneSettingsManager::AggregatePresentation;
	using ManagerUnifiedEditMode = SceneSettingsManager::UnifiedEditMode;
	using ManagerSettingDescriptor = SceneSettingsManager::SettingDescriptor;

	void CombineHash(size_t& signature, size_t value)
	{
		signature ^= value + 0x9E3779B9u + (signature << 6) + (signature >> 2);
	}

	void HashSceneSettingValue(size_t& signature, const json& value)
	{
		CombineHash(signature, static_cast<size_t>(value.type()));
		if (value.is_boolean())
			CombineHash(signature, std::hash<bool>{}(value.get<bool>()));
		else if (value.is_number_unsigned())
			CombineHash(signature, std::hash<std::uint64_t>{}(value.get<std::uint64_t>()));
		else if (value.is_number_integer())
			CombineHash(signature, std::hash<std::int64_t>{}(value.get<std::int64_t>()));
		else if (value.is_number_float())
			CombineHash(signature, std::hash<double>{}(value.get<double>()));
		else if (value.is_string())
			CombineHash(signature, std::hash<std::string_view>{}(value.get_ref<const std::string&>()));
	}

	SceneSettingsManager* sceneSettingsManagerSingleton = nullptr;
}

SceneSettingsManager::SceneSettingsManager()
{
	assert(!sceneSettingsManagerSingleton);
	sceneSettingsManagerSingleton = this;
}

SceneSettingsManager::~SceneSettingsManager()
{
	if (sceneSettingsManagerSingleton == this)
		sceneSettingsManagerSingleton = nullptr;
}

SceneSettingsManager* SceneSettingsManager::GetSingleton()
{
	return sceneSettingsManagerSingleton;
}

namespace
{
	constexpr auto kOverwriteJsonIndent = 2;
	constexpr auto kMaxSceneOverwriteFileSize = 1024 * 1024;
	constexpr const char* kFeatureKey = "_feature";
	constexpr const char* kMetadataKey = "_metadata";
	constexpr const char* kMetadataDescriptionKey = "description";
	constexpr const char* kMetadataEntryTransitionsKey = "entryTransitions";
	constexpr const char* kMetadataTimeOfDayEnabledKey = "timeOfDayEnabled";
	constexpr std::string_view kSceneSettingDisplaySeparator = " / ";
	bool IsLocationTypeKeyword(const RE::BGSKeyword* keyword);

	bool IsSceneSettingPrimitive(const json& value)
	{
		return value.is_boolean() || value.is_number_integer() || value.is_number_float() || value.is_string();
	}

	bool IsEntryListSceneType(SceneSettingsManager::SceneType type)
	{
		return type == SceneSettingsManager::SceneType::InteriorOnly ||
		       type == SceneSettingsManager::SceneType::TimeOfDay;
	}

	bool IsValidLocationTargetType(SceneSettingsManager::LocationTargetType type)
	{
		switch (type) {
		case SceneSettingsManager::LocationTargetType::Region:
		case SceneSettingsManager::LocationTargetType::LocationType:
		case SceneSettingsManager::LocationTargetType::Location:
		case SceneSettingsManager::LocationTargetType::Cell:
		case SceneSettingsManager::LocationTargetType::Worldspace:
			return true;
		default:
			return false;
		}
	}

	using Util::FileHelpers::WriteJsonAtomically;

	using Util::Settings::StripImGuiId;
	std::vector<std::filesystem::path> GetSortedDirectoryPaths(
		const std::filesystem::path& directory, bool directories, std::string_view context)
	{
		std::vector<std::filesystem::path> paths;
		std::error_code ec;
		std::filesystem::directory_iterator iterator(
			directory, std::filesystem::directory_options::skip_permission_denied, ec);
		if (ec) {
			logger::error("[SceneSettings] Failed to enumerate {} '{}': {}", context, directory.string(), ec.message());
			return paths;
		}

		const std::filesystem::directory_iterator end;
		while (iterator != end) {
			const auto& entry = *iterator;
			std::error_code statusError;
			const bool matches = directories ? entry.is_directory(statusError) : entry.is_regular_file(statusError);
			if (statusError) {
				logger::warn("[SceneSettings] Could not inspect '{}': {}", entry.path().string(), statusError.message());
			} else if (matches) {
				paths.push_back(entry.path());
			}

			iterator.increment(ec);
			if (ec) {
				logger::error("[SceneSettings] Failed while enumerating {} '{}': {}", context, directory.string(), ec.message());
				break;
			}
		}

		std::sort(paths.begin(), paths.end(), [](const auto& lhs, const auto& rhs) {
			return lhs.generic_string() < rhs.generic_string();
		});
		return paths;
	}

	std::vector<std::filesystem::path> GetSortedJsonFiles(
		const std::filesystem::path& directory, std::string_view context)
	{
		auto paths = GetSortedDirectoryPaths(directory, false, context);
		std::erase_if(paths, [](const auto& path) { return path.extension() != ".json"; });
		return paths;
	}

	std::string NormalizeLocationFormKey(std::string_view formKey)
	{
		const auto components = Util::ParseSpid(std::string(formKey));
		if (components.localFormId == 0)
			return std::string(formKey);
		if (components.pluginName.empty())
			return std::format("0x{:X}", components.localFormId);

		auto pluginName = components.pluginName;
		std::transform(pluginName.begin(), pluginName.end(), pluginName.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return std::format("0x{:X}~{}", components.localFormId, pluginName);
	}

	std::string CanonicalizeResolvedLocationFormKey(std::string_view formKey)
	{
		const auto components = Util::ParseSpid(std::string(formKey));
		if (components.pluginName.empty() || components.localFormId == 0)
			return std::string(formKey);
		if (!RE::TESDataHandler::GetSingleton())
			return std::string(formKey);
		const auto formId = Util::SpidToFormId(std::string(formKey));
		return formId != 0 ? Util::FormIdToSpid(formId) : std::string(formKey);
	}

	bool ReadOptionalStringField(const json& object, std::string_view field, std::string& value,
		std::string_view context)
	{
		auto it = object.find(std::string(field));
		if (it == object.end())
			return true;
		if (!it->is_string()) {
			logger::warn("[SceneSettings] {} field '{}' must be a string", context, field);
			return false;
		}
		value = it->get<std::string>();
		return true;
	}

	bool IsSceneMetadataKey(std::string_view key)
	{
		return !key.empty() && key.front() == '_';
	}

	bool ReadBoundedSceneJson(const std::filesystem::path& path, json& data)
	{
		std::error_code ec;
		const auto fileSize = std::filesystem::file_size(path, ec);
		if (ec || fileSize > kMaxSceneOverwriteFileSize)
			return false;

		std::ifstream file(path);
		if (!file.is_open())
			return false;
		data = json::parse(file, nullptr, false);
		return data.is_object();
	}

	// TOD/weather can only interpolate float settings, not integer toggles or enum values.
	bool IsNumericValue(const json& value)
	{
		return value.is_number_float();
	}

	bool IsSceneSettingPathWrapper(std::string_view token)
	{
		return token == "settings";
	}

	std::string NormalizeSceneSettingAddressToken(std::string_view token)
	{
		auto normalized = token.find(' ') == std::string_view::npos ?
		                      Util::PrettifyIdentifier(token) :
		                      std::string(token);
		std::erase_if(normalized, [](unsigned char c) { return std::isspace(c); });
		std::transform(normalized.begin(), normalized.end(), normalized.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return normalized;
	}

	bool SceneSettingAddressTokensEqual(std::string_view lhs, std::string_view rhs)
	{
		return NormalizeSceneSettingAddressToken(lhs) == NormalizeSceneSettingAddressToken(rhs);
	}

	bool IsSceneSettingPolicyPrefix(
		const std::vector<std::string>& address, const SceneSettingsPolicy::SettingPolicyPath& prefix)
	{
		if (prefix.size() > address.size())
			return false;

		for (size_t index = 0; index < prefix.size(); ++index)
			if (!SceneSettingAddressTokensEqual(address[index], prefix[index]))
				return false;
		return true;
	}

	bool MatchesSceneSettingPolicy(const std::vector<std::string>& address,
		const std::vector<SceneSettingsPolicy::SettingPolicyPath>& paths)
	{
		return std::any_of(paths.begin(), paths.end(),
			[&](const auto& prefix) { return IsSceneSettingPolicyPrefix(address, prefix); });
	}

	std::vector<std::string> GetSceneSettingAddress(const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey)
	{
		std::vector<std::string> address{ featureShortName };
		address.reserve(settingPath.size() + 2);
		for (const auto& segment : settingPath)
			if (!IsSceneSettingPathWrapper(segment))
				address.push_back(segment);
		address.push_back(settingKey);
		return address;
	}

	bool IsBlacklistedSceneSetting(const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey)
	{
		auto address = GetSceneSettingAddress(featureShortName, settingPath, settingKey);
		return MatchesSceneSettingPolicy(address, SceneSettingsPolicy::kSettingBlacklist);
	}

	bool HasSceneOverwriteContent(const json& data)
	{
		if (!data.is_object())
			return false;

		for (const auto& [key, _] : data.items())
			if (!IsSceneMetadataKey(key))
				return true;
		return false;
	}

	bool IsCompatibleSceneSettingValue(const json& featureValue, const json& value)
	{
		if (featureValue.type() == value.type())
			return true;
		if (featureValue.is_number() && value.is_number())
			return true;
		return false;
	}

	std::string JoinDisplayParts(const std::vector<std::string>& parts, std::string_view leaf)
	{
		std::string displayName;
		for (const auto& part : parts) {
			if (!displayName.empty())
				displayName += kSceneSettingDisplaySeparator;
			displayName += part;
		}
		if (!leaf.empty()) {
			if (!displayName.empty())
				displayName += kSceneSettingDisplaySeparator;
			displayName += leaf;
		}
		return displayName;
	}

	using Util::Settings::SplitCatalogPath;
	std::string ToCatalogPath(const std::vector<std::string>& path)
	{
		std::string result;
		for (const auto& part : path) {
			if (!result.empty())
				result += '/';
			for (const char ch : part) {
				if (ch == '~')
					result += "~0";
				else if (ch == '/')
					result += "~1";
				else
					result += ch;
			}
		}
		return result;
	}

	using Util::Settings::GetCatalogDisplayPath;
	using Util::Settings::IsStructuralDisplayPart;
	using Util::Settings::NormalizeDisplayPart;
	std::vector<std::string> GetCatalogSelectorPath(const SceneSettingsCatalog::SettingMetadata& setting)
	{
		auto parts = SplitCatalogPath(setting.selectorPath);
		auto keys = SplitCatalogPath(setting.selectorPathKeys);
		for (size_t i = 0; i < parts.size(); ++i) {
			if (i < keys.size() && keys[i] != "-")
				parts[i] = T(keys[i], parts[i].c_str());
			parts[i] = StripImGuiId(parts[i]);
		}
		return parts;
	}

	bool EqualDisplayText(std::string_view lhs, std::string_view rhs)
	{
		return std::ranges::equal(lhs, rhs, [](const char a, const char b) {
			return std::tolower(static_cast<unsigned char>(a)) ==
			       std::tolower(static_cast<unsigned char>(b));
		});
	}

	std::vector<std::string> GetCatalogContextPath(const SceneSettingsCatalog::SettingMetadata& setting)
	{
		auto parts = GetCatalogDisplayPath(setting);
		auto selectorDefaults = GetCatalogSelectorPath(setting);
		auto rawParts = SplitCatalogPath(setting.displayPath.empty() ? setting.settingPath : setting.displayPath);
		const auto rawKeys = SplitCatalogPath(setting.displayPathKeys);
		const auto settingParts = SplitCatalogPath(setting.settingPath);
		const bool hasSelector = !selectorDefaults.empty();
		size_t rawOffset = 0;
		for (auto& part : selectorDefaults)
			part = NormalizeDisplayPart(std::move(part));
		while (!parts.empty() && !selectorDefaults.empty() &&
			   EqualDisplayText(parts.front(), selectorDefaults.front())) {
			parts.erase(parts.begin());
			selectorDefaults.erase(selectorDefaults.begin());
			++rawOffset;
		}
		if (hasSelector) {
			while (rawOffset < rawParts.size() && IsStructuralDisplayPart(rawParts[rawOffset]))
				++rawOffset;
			if (!parts.empty() && rawOffset < rawParts.size() && rawOffset < settingParts.size()) {
				const bool translated = rawOffset < rawKeys.size() && rawKeys[rawOffset] != "-";
				auto rawPart = NormalizeDisplayPart(rawParts[rawOffset]);
				auto settingPart = NormalizeDisplayPart(settingParts[rawOffset]);
				if (!translated && EqualDisplayText(parts.front(), rawPart) &&
					EqualDisplayText(rawPart, settingPart))
					parts.erase(parts.begin());
			}
		}
		return parts;
	}

	using Util::Settings::GetCatalogLeafDisplayName;
	double GetCatalogNumericDisplayScale(const SceneSettingsCatalog::SettingMetadata& setting)
	{
		return std::isfinite(setting.displayScale) && setting.displayScale > 0.0 ?
		           setting.displayScale :
		           1.0;
	}

	bool ConvertCatalogNumericStoredToDisplay(const SceneSettingsCatalog::SettingMetadata& setting,
		double storedValue, double& displayValue)
	{
		if (!std::isfinite(storedValue))
			return false;
		if (setting.editorSemantic == SceneSettingsCatalog::EditorSemantic::Generic) {
			displayValue = storedValue;
			return true;
		}
		if (setting.editorSemantic != SceneSettingsCatalog::EditorSemantic::Numeric)
			return false;

		double transformedValue = storedValue;
		switch (setting.numericTransform) {
		case SceneSettingsCatalog::NumericTransform::Identity:
			break;
		case SceneSettingsCatalog::NumericTransform::Log2:
			if (storedValue <= 0.0)
				return false;
			transformedValue = std::log2(storedValue);
			break;
		default:
			return false;
		}

		displayValue = transformedValue * GetCatalogNumericDisplayScale(setting);
		return std::isfinite(displayValue);
	}

	bool ConvertCatalogNumericDisplayToStored(const SceneSettingsCatalog::SettingMetadata& setting,
		double displayValue, double& storedValue)
	{
		if (!std::isfinite(displayValue))
			return false;
		if (setting.editorSemantic == SceneSettingsCatalog::EditorSemantic::Generic) {
			storedValue = displayValue;
			return true;
		}
		if (setting.editorSemantic != SceneSettingsCatalog::EditorSemantic::Numeric)
			return false;

		const double transformedValue = displayValue / GetCatalogNumericDisplayScale(setting);
		if (!std::isfinite(transformedValue))
			return false;
		switch (setting.numericTransform) {
		case SceneSettingsCatalog::NumericTransform::Identity:
			storedValue = transformedValue;
			break;
		case SceneSettingsCatalog::NumericTransform::Log2:
			storedValue = std::exp2(transformedValue);
			break;
		default:
			return false;
		}
		return std::isfinite(storedValue) &&
		       (setting.numericTransform != SceneSettingsCatalog::NumericTransform::Log2 || storedValue > 0.0);
	}

	const SceneSettingsCatalog::SettingMetadata* FindStoredAllComponent(
		const SceneSettingsCatalog::SettingMetadata& setting)
	{
		const auto settings = SceneSettingsCatalog::GetSettings();
		static const auto storedAllComponents = [] {
			using AggregateKey = std::tuple<std::string_view, std::string_view, std::string_view,
				SceneSettingsCatalog::AggregateSemantic, std::int8_t, std::uint8_t>;
			const auto makeKey = [](const auto& candidate) {
				return AggregateKey{ candidate.featureShortName, candidate.serializedPath,
					candidate.serializedKey, candidate.aggregateSemantic,
					candidate.aggregateStart, candidate.aggregateCount };
			};
			std::map<AggregateKey, const SceneSettingsCatalog::SettingMetadata*> storedAll;
			for (const auto& candidate : SceneSettingsCatalog::GetSettings())
				if (candidate.aggregateAll)
					storedAll.try_emplace(makeKey(candidate), &candidate);
			std::vector<const SceneSettingsCatalog::SettingMetadata*> components(
				SceneSettingsCatalog::GetSettings().size(), nullptr);
			for (size_t index = 0; index < SceneSettingsCatalog::GetSettings().size(); ++index) {
				const auto& source = SceneSettingsCatalog::GetSettings()[index];
				if (auto component = storedAll.find(makeKey(source)); component != storedAll.end())
					components[index] = component->second;
			}
			return components;
		}();
		const auto index = static_cast<size_t>(&setting - settings.data());
		assert(index < storedAllComponents.size());
		return index < storedAllComponents.size() ? storedAllComponents[index] : nullptr;
	}

	SceneSettingControlType GetCatalogControlType(const SceneSettingsCatalog::SettingMetadata& setting)
	{
		using enum SceneSettingsCatalog::AggregateSemantic;
		switch (setting.aggregateSemantic) {
		case Color:
			return FindStoredAllComponent(setting) ?
			           SceneSettingControlType::Numeric :
			           SceneSettingControlType::Color;
		case Numeric:
			return SceneSettingControlType::Numeric;
		default:
			return SceneSettingControlType::Scalar;
		}
	}

	std::string GetSettingComponentName(SceneSettingControlType type, std::int8_t componentIndex)
	{
		if (componentIndex < 0 || componentIndex > 3)
			return {};
		if (type == SceneSettingControlType::Color) {
			switch (componentIndex) {
			case 0:
				return T("feature.scene_manager.channel.red", "R");
			case 1:
				return T("feature.scene_manager.channel.green", "G");
			case 2:
				return T("feature.scene_manager.channel.blue", "B");
			default:
				return T("feature.scene_manager.channel.alpha", "A");
			}
		}
		switch (componentIndex) {
		case 0:
			return T("feature.scene_manager.channel.x", "X");
		case 1:
			return T("feature.scene_manager.channel.y", "Y");
		case 2:
			return T("feature.scene_manager.channel.z", "Z");
		default:
			return T("feature.scene_manager.channel.w", "W");
		}
	}

	std::string GetCatalogComponentDisplayName(
		const SceneSettingsCatalog::SettingMetadata& setting, SceneSettingControlType controlType)
	{
		auto displayName = StripImGuiId(setting.componentDisplayName);
		if (!setting.componentDisplayNameKey.empty())
			displayName = StripImGuiId(T(setting.componentDisplayNameKey, displayName.c_str()));
		if (!displayName.empty())
			return displayName;
		if (setting.aggregateAll)
			return T("feature.scene_manager.channel.all", "All");

		auto componentIndex = static_cast<std::int8_t>(setting.aggregateCount > 1 ?
														   setting.serializedComponent - setting.aggregateStart :
														   setting.serializedComponent);
		const auto* storedAll = FindStoredAllComponent(setting);
		if (storedAll && storedAll->serializedComponent < setting.serializedComponent)
			--componentIndex;
		const auto componentType = setting.aggregateSemantic == SceneSettingsCatalog::AggregateSemantic::Color ?
		                               SceneSettingControlType::Color :
		                               controlType;
		return GetSettingComponentName(componentType, componentIndex);
	}

	std::int8_t GetCatalogColorChannelIndex(const SceneSettingsCatalog::SettingMetadata& setting)
	{
		if (setting.aggregateSemantic != SceneSettingsCatalog::AggregateSemantic::Color || setting.aggregateAll)
			return -1;
		auto componentIndex = static_cast<std::int8_t>(setting.aggregateCount > 1 ?
														   setting.serializedComponent - setting.aggregateStart :
														   setting.serializedComponent);
		const auto* storedAll = FindStoredAllComponent(setting);
		if (storedAll && storedAll->serializedComponent < setting.serializedComponent)
			--componentIndex;
		return componentIndex >= 0 && componentIndex < 3 ? componentIndex : -1;
	}

	SceneSettingsManager::SettingControlInfo MakeSettingControlInfo(
		const SceneSettingsCatalog::SettingMetadata& setting)
	{
		SceneSettingsManager::SettingControlInfo info;
		info.controlType = GetCatalogControlType(setting);
		info.settingPath = info.controlType == SceneSettingControlType::Scalar ?
		                       SplitCatalogPath(setting.settingPath) :
		                       SplitCatalogPath(setting.serializedPath);
		info.settingKey = std::string(info.controlType == SceneSettingControlType::Scalar ?
										  setting.settingKey :
										  setting.serializedKey);
		info.displayName = GetCatalogLeafDisplayName(setting);
		info.componentDisplayName = GetCatalogComponentDisplayName(setting, info.controlType);
		info.displayPath = GetCatalogContextPath(setting);
		info.tableDisplayPath = GetCatalogDisplayPath(setting);
		info.componentIndex = setting.serializedComponent;
		info.colorChannelIndex = GetCatalogColorChannelIndex(setting);
		info.aggregateAll = setting.aggregateAll;
		if (info.controlType != SceneSettingControlType::Scalar) {
			info.componentStart = setting.aggregateStart;
			info.componentCount = setting.aggregateCount;
			info.aggregatePresentation =
				info.controlType == SceneSettingControlType::Color &&
						setting.aggregatePresentation == SceneSettingsCatalog::AggregatePresentation::ColorPicker ?
					ManagerAggregatePresentation::ColorPicker :
					ManagerAggregatePresentation::Components;
			switch (setting.unifiedEditMode) {
			case SceneSettingsCatalog::UnifiedEditMode::Always:
				info.unifiedEditMode = ManagerUnifiedEditMode::Always;
				break;
			case SceneSettingsCatalog::UnifiedEditMode::Shift:
				info.unifiedEditMode = ManagerUnifiedEditMode::Shift;
				break;
			default:
				info.unifiedEditMode = ManagerUnifiedEditMode::None;
				break;
			}
		}
		return info;
	}

	bool IsCatalogValueCompatible(const SceneSettingsCatalog::SettingMetadata& setting, const json& value)
	{
		using enum SceneSettingsCatalog::ValueType;
		switch (setting.valueType) {
		case Boolean:
			return value.is_boolean();
		case Integer:
			return value.is_number_integer();
		case Float:
			return value.is_number_float() || value.is_number_integer();
		case String:
			return value.is_string();
		default:
			return false;
		}
	}

	bool IsSameSetting(const SceneSettingsManager::SettingEntry& entry, const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey)
	{
		return entry.featureShortName == featureShortName &&
		       entry.settingPath == settingPath &&
		       entry.settingKey == settingKey;
	}

	std::string GetSettingLogName(const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey)
	{
		return JoinDisplayParts(settingPath, std::format("{}.{}", featureShortName, settingKey));
	}

	json* GetObjectAtPath(json& data, const std::vector<std::string>& path, bool create)
	{
		json* node = &data;
		for (const auto& segment : path) {
			if (!node->is_object()) {
				return nullptr;
			}

			auto it = node->find(segment);
			if (it == node->end()) {
				if (!create)
					return nullptr;
				it = node->emplace(segment, json::object()).first;
			}
			node = &*it;
		}
		return node->is_object() ? node : nullptr;
	}

	bool RemoveObjectValueAtPath(json& data, const std::vector<std::string>& path,
		size_t pathIndex, const std::string& settingKey)
	{
		if (!data.is_object())
			return false;
		if (pathIndex == path.size())
			return data.erase(settingKey) == 1;

		auto childIt = data.find(path[pathIndex]);
		if (childIt == data.end() || !childIt->is_object() ||
			!RemoveObjectValueAtPath(*childIt, path, pathIndex + 1, settingKey))
			return false;
		if (childIt->empty())
			data.erase(childIt);
		return true;
	}

	const json* GetObjectAtPath(const json& data, const std::vector<std::string>& path)
	{
		const json* node = &data;
		for (const auto& segment : path) {
			if (!node->is_object())
				return nullptr;
			auto it = node->find(segment);
			if (it == node->end())
				return nullptr;
			node = &*it;
		}
		return node->is_object() ? node : nullptr;
	}

	json* GetObjectAtPath(json& data, const std::vector<std::string>& path)
	{
		return const_cast<json*>(GetObjectAtPath(std::as_const(data), path));
	}

	bool ParseCatalogArrayIndex(std::string_view value, size_t& index)
	{
		const auto result = std::from_chars(value.data(), value.data() + value.size(), index);
		return result.ec == std::errc{} && result.ptr == value.data() + value.size();
	}

	template <class Json>
	Json* GetCatalogNodeAtPath(Json& data, const std::vector<std::string>& path)
	{
		auto* node = &data;
		for (const auto& segment : path) {
			if (node->is_object()) {
				auto it = node->find(segment);
				if (it == node->end())
					return nullptr;
				node = &*it;
				continue;
			}

			size_t index = 0;
			if (!node->is_array() || !ParseCatalogArrayIndex(segment, index) || index >= node->size())
				return nullptr;
			node = &(*node)[index];
		}
		return node;
	}

	template <class Json>
	Json* GetCatalogSerializedValue(Json& data, const SceneSettingsCatalog::SettingMetadata& setting)
	{
		auto* parent = GetCatalogNodeAtPath(data, SplitCatalogPath(setting.serializedPath));
		if (!parent)
			return nullptr;

		Json* value = nullptr;
		if (parent->is_object()) {
			auto valueIt = parent->find(setting.serializedKey);
			if (valueIt == parent->end())
				return nullptr;
			value = &*valueIt;
		} else {
			size_t index = 0;
			if (!parent->is_array() || !ParseCatalogArrayIndex(setting.serializedKey, index) ||
				index >= parent->size())
				return nullptr;
			value = &(*parent)[index];
		}

		if (setting.serializedComponent < 0)
			return value;
		const auto component = static_cast<size_t>(setting.serializedComponent);
		if (!value->is_array() || component >= value->size())
			return nullptr;
		return &(*value)[component];
	}

	void CollectOverwriteEntries(const json& data, const std::vector<std::string>& settingPath,
		const std::function<void(const std::vector<std::string>&, const std::string&, const json&)>& callback)
	{
		if (!data.is_object())
			return;

		for (const auto& [key, value] : data.items()) {
			if (IsSceneMetadataKey(key))
				continue;
			if (IsSceneSettingPrimitive(value)) {
				callback(settingPath, key, value);

				continue;
			}
			if (!value.is_object())
				continue;

			auto childPath = settingPath;
			childPath.push_back(key);
			CollectOverwriteEntries(value, childPath, callback);
		}
	}

	bool PolicyContainsFeature(const std::vector<SceneSettingsPolicy::SettingPolicyPath>& paths,
		std::string_view featureShortName)
	{
		return std::any_of(paths.begin(), paths.end(), [&](const auto& prefix) {
			return !prefix.empty() && SceneSettingAddressTokensEqual(prefix.front(), featureShortName);
		});
	}

	bool IsInteriorOnlyFeatureAllowed(std::string_view featureShortName)
	{
		return PolicyContainsFeature(SceneSettingsPolicy::kLocationFeatureWhitelist, featureShortName);
	}

	bool IsTimeOfDayFeatureAllowed(std::string_view featureShortName)
	{
		return PolicyContainsFeature(SceneSettingsPolicy::kTimeOfDayFeatureWhitelist, featureShortName);
	}

	bool IsSettingAllowedBySceneTypePolicy(SceneSettingsManager::SceneType type,
		const std::string& featureShortName, const std::vector<std::string>& settingPath,
		const std::string& settingKey)
	{
		const auto address = GetSceneSettingAddress(featureShortName, settingPath, settingKey);
		switch (type) {
		case SceneSettingsManager::SceneType::InteriorOnly:
			return MatchesSceneSettingPolicy(address, SceneSettingsPolicy::kLocationFeatureWhitelist);
		case SceneSettingsManager::SceneType::TimeOfDay:
			return MatchesSceneSettingPolicy(address, SceneSettingsPolicy::kTimeOfDayFeatureWhitelist);
		case SceneSettingsManager::SceneType::Location:
			return MatchesSceneSettingPolicy(address, SceneSettingsPolicy::kLocationFeatureWhitelist) ||
			       MatchesSceneSettingPolicy(address, SceneSettingsPolicy::kTimeOfDayFeatureWhitelist);
		default:
			return false;
		}
	}

	bool ComputeCatalogSettingAllowedByPolicy(const SceneSettingsCatalog::SettingMetadata& setting)
	{
		return SceneSettingsCatalog::IsSceneControllable(setting) &&
		       !IsBlacklistedSceneSetting(
				   std::string(setting.featureShortName), SplitCatalogPath(setting.settingPath),
				   std::string(setting.settingKey));
	}

	bool IsCatalogSettingAllowedByPolicy(const SceneSettingsCatalog::SettingMetadata& setting)
	{
		const auto settings = SceneSettingsCatalog::GetSettings();
		static const auto allowedSettings = [] {
			std::vector<uint8_t> allowed;
			allowed.reserve(SceneSettingsCatalog::GetSettings().size());
			for (const auto& candidate : SceneSettingsCatalog::GetSettings())
				allowed.push_back(ComputeCatalogSettingAllowedByPolicy(candidate) ? 1 : 0);
			return allowed;
		}();
		const auto index = static_cast<size_t>(&setting - settings.data());
		assert(index < allowedSettings.size());
		return index < allowedSettings.size() && allowedSettings[index] != 0;
	}

	bool IsCatalogSettingAllowedForSceneType(SceneSettingsManager::SceneType type,
		const SceneSettingsCatalog::SettingMetadata& setting)
	{
		constexpr size_t sceneTypeCount = 3;
		const auto typeIndex = static_cast<size_t>(type);
		if (typeIndex >= sceneTypeCount)
			return false;

		const auto settings = SceneSettingsCatalog::GetSettings();
		static const auto allowedSettings = [] {
			std::array<std::vector<uint8_t>, sceneTypeCount> allowedByType;
			for (size_t index = 0; index < sceneTypeCount; ++index) {
				const auto sceneType = static_cast<SceneSettingsManager::SceneType>(index);
				auto& allowed = allowedByType[index];
				allowed.reserve(SceneSettingsCatalog::GetSettings().size());
				for (const auto& candidate : SceneSettingsCatalog::GetSettings()) {
					const bool transitionable = sceneType != SceneSettingsManager::SceneType::TimeOfDay ||
					                            SceneSettingsCatalog::HasFlag(
													candidate.flags, SceneSettingsCatalog::SettingFlag::Transitionable);
					allowed.push_back(IsCatalogSettingAllowedByPolicy(candidate) && transitionable &&
											  IsSettingAllowedBySceneTypePolicy(sceneType,
												  std::string(candidate.featureShortName), SplitCatalogPath(candidate.settingPath),
												  std::string(candidate.settingKey)) ?
										  1 :
										  0);
				}
			}
			return allowedByType;
		}();
		const auto settingIndex = static_cast<size_t>(&setting - settings.data());
		assert(settingIndex < allowedSettings[typeIndex].size());
		return settingIndex < allowedSettings[typeIndex].size() &&
		       allowedSettings[typeIndex][settingIndex] != 0;
	}

	std::span<const SceneSettingsCatalog::SettingMetadata> GetCatalogFeatureSettings(
		std::string_view featureShortName)
	{
		const auto settings = SceneSettingsCatalog::GetSettings();
		static const auto featureRanges = [] {
			std::map<std::string_view, std::pair<size_t, size_t>> ranges;
			const auto allSettings = SceneSettingsCatalog::GetSettings();
			for (size_t index = 0; index < allSettings.size();) {
				const auto name = allSettings[index].featureShortName;
				size_t end = index + 1;
				while (end < allSettings.size() && allSettings[end].featureShortName == name)
					++end;
				ranges.emplace(name, std::pair{ index, end });
				index = end;
			}
			return ranges;
		}();
		auto rangeIt = featureRanges.find(featureShortName);
		if (rangeIt == featureRanges.end())
			return {};
		const auto [begin, end] = rangeIt->second;
		return settings.subspan(begin, end - begin);
	}

	const SceneSettingsCatalog::SettingMetadata* FindAllowedCatalogSetting(
		std::string_view featureShortName, const std::vector<std::string>& settingPath,
		std::string_view settingKey, bool requireTransitionable = false)
	{
		auto* setting = SceneSettingsCatalog::FindSetting(
			featureShortName, ToCatalogPath(settingPath), settingKey);
		if (!setting || !IsCatalogSettingAllowedByPolicy(*setting))
			return nullptr;
		if (requireTransitionable &&
			!SceneSettingsCatalog::HasFlag(setting->flags, SceneSettingsCatalog::SettingFlag::Transitionable))
			return nullptr;
		return setting;
	}

	bool CatalogHasSceneSettings(
		std::string_view featureShortName, SceneSettingsManager::SceneType type)
	{
		for (const auto& setting : GetCatalogFeatureSettings(featureShortName)) {
			if (IsCatalogSettingAllowedForSceneType(type, setting))
				return true;
		}
		return false;
	}

	std::vector<std::string> GetLoadedCatalogFeatureNames(SceneSettingsManager::SceneType type)
	{
		auto names = Feature::GetLoadedFeatureNames();
		std::erase_if(names, [&](const auto& name) { return !CatalogHasSceneSettings(name, type); });
		return names;
	}
}

size_t SceneSettingsManager::GetCatalogUpdateSignature(std::string_view featureShortName,
	std::span<const CatalogSceneSettingUpdate> updates)
{
	size_t signature = std::hash<std::string_view>{}(featureShortName);
	for (const auto& update : updates) {
		for (const auto& segment : update.settingPath)
			CombineHash(signature, std::hash<std::string_view>{}(segment));
		CombineHash(signature, std::hash<std::string_view>{}(update.key));
		HashSceneSettingValue(signature, update.value);
	}
	return signature;
}

bool SceneSettingsManager::ApplyCatalogSceneSettings(
	Feature& feature, const std::vector<CatalogSceneSettingUpdate>& updates)
{
	if (updates.empty())
		return true;

	const auto featureShortName = feature.GetShortName();
	auto documentIt = featureApplyDocuments.find(featureShortName);
	if (documentIt == featureApplyDocuments.end()) {
		json settingsDocument;
		try {
			feature.SaveSettings(settingsDocument);
		} catch (const std::exception& e) {
			logger::warn("[SceneSettings] Failed to snapshot settings for {}: {}", featureShortName, e.what());
			return false;
		} catch (...) {
			logger::warn("[SceneSettings] Failed to snapshot settings for {}", featureShortName);
			return false;
		}
		if (!settingsDocument.is_object())
			return false;
		documentIt = featureApplyDocuments.emplace(featureShortName, std::move(settingsDocument)).first;
	}

	auto& settingsDocument = documentIt->second;
	std::vector<const SceneSettingsCatalog::SettingMetadata*> catalogSettings;
	std::vector<json> originalValues;
	catalogSettings.reserve(updates.size());
	originalValues.reserve(updates.size());
	try {
		for (const auto& update : updates) {
			auto* setting = FindAllowedCatalogSetting(
				featureShortName, update.settingPath, update.key);
			auto* currentValue = setting ? GetCatalogSerializedValue(settingsDocument, *setting) : nullptr;
			if (!currentValue || !IsSceneSettingPrimitive(*currentValue) ||
				!IsSceneSettingPrimitive(update.value) ||
				!IsCompatibleSceneSettingValue(*currentValue, update.value))
				return false;
			catalogSettings.push_back(setting);
			originalValues.push_back(*currentValue);
		}
		for (size_t index = 0; index < updates.size(); ++index)
			*GetCatalogSerializedValue(settingsDocument, *catalogSettings[index]) = updates[index].value;
	} catch (const std::exception& e) {
		featureApplyDocuments.erase(featureShortName);
		logger::warn("[SceneSettings] Failed to prepare settings for {}: {}", featureShortName, e.what());
		return false;
	} catch (...) {
		featureApplyDocuments.erase(featureShortName);
		logger::warn("[SceneSettings] Failed to prepare settings for {}", featureShortName);
		return false;
	}

	try {
		feature.LoadSettings(settingsDocument);
		return true;
	} catch (const std::exception& e) {
		logger::warn("[SceneSettings] Failed to apply settings for {}: {}", featureShortName, e.what());
	} catch (...) {
		logger::warn("[SceneSettings] Failed to apply settings for {}", featureShortName);
	}

	try {
		for (size_t index = updates.size(); index-- > 0;)
			*GetCatalogSerializedValue(settingsDocument, *catalogSettings[index]) = std::move(originalValues[index]);
		feature.LoadSettings(settingsDocument);
	} catch (...) {
		featureApplyDocuments.erase(featureShortName);
		logger::error("[SceneSettings] Failed to restore {} after an apply error", featureShortName);
	}
	return false;
}

void SceneSettingsManager::ScheduleApplyVerification(std::string_view featureShortName,
	const std::vector<CatalogSceneSettingUpdate>& updates, size_t signature, bool transition,
	std::span<const SettingAddress> restorationAddresses)
{
	const auto featureName = std::string(featureShortName);
	auto deadline = std::chrono::steady_clock::now() + kApplyVerificationMaxDeferral;
	auto pendingUpdates = updates;
	auto pendingRestorationAddresses = std::vector<SettingAddress>(
		restorationAddresses.begin(), restorationAddresses.end());
	if (auto pending = pendingApplyVerifications.find(featureName);
		pending != pendingApplyVerifications.end()) {
		deadline = pending->second.deadline;
		for (const auto& address : pending->second.restorationAddresses) {
			const bool superseded = std::ranges::any_of(updates, [&](const auto& update) {
				return update.settingPath == address.settingPath && update.key == address.settingKey;
			});
			if (superseded)
				continue;
			auto expected = std::ranges::find_if(pending->second.updates, [&](const auto& update) {
				return update.settingPath == address.settingPath && update.key == address.settingKey;
			});
			if (expected != pending->second.updates.end()) {
				pendingUpdates.push_back(*expected);
				pendingRestorationAddresses.push_back(address);
			}
		}
	}
	pendingApplyVerifications[featureName] = {
		.appliedFrame = lastUpdateFrame,
		.deadline = deadline,
		.updates = std::move(pendingUpdates),
		.restorationAddresses = std::move(pendingRestorationAddresses),
		.signature = signature,
		.transition = transition,
	};
}

void SceneSettingsManager::VerifyPendingApplies(bool overdueOnly)
{
	const auto now = std::chrono::steady_clock::now();
	for (auto verificationIt = pendingApplyVerifications.begin();
		verificationIt != pendingApplyVerifications.end();) {
		auto& [featureShortName, verification] = *verificationIt;
		if (verification.appliedFrame == lastUpdateFrame ||
			(overdueOnly && now < verification.deadline)) {
			++verificationIt;
			continue;
		}

		bool verified = false;
		json actualSettings;
		auto* feature = Feature::FindFeatureByShortName(featureShortName);
		try {
			if (feature)
				feature->SaveSettings(actualSettings);
			if (feature && actualSettings.is_object()) {
				verified = std::all_of(verification.updates.begin(), verification.updates.end(),
					[&](const auto& update) {
						auto* setting = FindAllowedCatalogSetting(
							featureShortName, update.settingPath, update.key);
						const auto* actual = setting ? GetCatalogSerializedValue(actualSettings, *setting) : nullptr;
						return actual && AppliedValuesEqual(*actual, update.value);
					});
			}
		} catch (...) {
			verified = false;
		}

		if (verified) {
			for (const auto& address : verification.restorationAddresses) {
				appliedSettings.erase(address);
				baselineSettings.erase(address);
			}
			if (std::none_of(appliedSettings.begin(), appliedSettings.end(), [&](const auto& item) {
					return item.first.featureShortName == featureShortName;
				}))
				appliedFeatureNames.erase(featureShortName);
		} else {
			logger::warn("[SceneSettings] {} did not retain settings after reporting a successful apply",
				featureShortName);
			featureApplyDocuments.erase(featureShortName);
			for (const auto& update : verification.updates) {
				SettingAddress address{ featureShortName, update.settingPath, update.key };
				if (std::ranges::find(verification.restorationAddresses, address) ==
					verification.restorationAddresses.end())
					appliedSettings.erase(address);
			}
			if (std::none_of(appliedSettings.begin(), appliedSettings.end(), [&](const auto& item) {
					return item.first.featureShortName == featureShortName;
				}))
				appliedFeatureNames.erase(featureShortName);
			auto& failureMap = verification.transition ? transitionApplyFailures : applyFailures;
			auto& failure = failureMap[featureShortName];
			failure.signature = verification.signature;
			failure.retryAfter = std::chrono::steady_clock::now() + kApplyRetryDelay;
			failure.warningLogged = true;
			resolverDirty = true;
		}
		verificationIt = pendingApplyVerifications.erase(verificationIt);
	}
}

static std::filesystem::path GetSceneOverwritePath(SceneSettingsManager::SceneType type, const SceneSettingsManager::SettingEntry& entry);
static bool RemoveSettingFromOverwriteFile(const std::filesystem::path& path,
	const std::vector<std::string>& settingPath, const std::string& settingKey);

static bool HasOverwriteEntryForPeriod(const std::vector<SceneSettingsManager::SettingEntry>& entries,
	const SceneSettingsManager::SettingEntry& candidate)
{
	return std::any_of(entries.begin(), entries.end(), [&](const auto& entry) {
		return entry.source == SceneSettingsManager::EntrySource::Overwrite &&
		       entry.period == candidate.period &&
		       IsSameSetting(entry, candidate.featureShortName, candidate.settingPath, candidate.settingKey);
	});
}

static bool AddOverwriteEntryIfUnique(std::vector<SceneSettingsManager::SettingEntry>& entries,
	SceneSettingsManager::SettingEntry&& entry, std::string_view context)
{
	// Files are scanned lexicographically. The first overwrite for an address and period wins.
	if (HasOverwriteEntryForPeriod(entries, entry)) {
		logger::warn("[SceneSettings] Duplicate {} overwrite for {}.{} ({}) skipped",
			context, entry.featureShortName, entry.settingKey, entry.sourceFilename);
		return false;
	}

	entries.push_back(std::move(entry));
	return true;
}

// --- Path Resolution ---

std::string SceneSettingsManager::GetSceneTypeName(SceneType type)
{
	switch (type) {
	case SceneType::InteriorOnly:
		return "InteriorOnly";
	case SceneType::TimeOfDay:
		return "TimeOfDay";
	case SceneType::Location:
		return "Location";
	default:
		return "Unknown";
	}
}

std::filesystem::path SceneSettingsManager::GetUserSettingsFilePath()
{
	return Util::PathHelpers::GetSceneSettingsPath() / "SceneManager.json";
}

std::filesystem::path SceneSettingsManager::GetOverwritesPath(SceneType type)
{
	return Util::PathHelpers::GetSceneSettingsPath() / GetSceneTypeName(type);
}

std::filesystem::path SceneSettingsManager::GetWeatherOverwritesDir()
{
	return Util::PathHelpers::GetSceneSettingsPath() / "Weather";
}

std::filesystem::path SceneSettingsManager::GetLocationOverwritesDir(LocationTargetType type)
{
	(void)type;
	return Util::PathHelpers::GetSceneSettingsPath() / "Locations";
}

// --- Time of Day Period Helpers ---

const char* SceneSettingsManager::GetPeriodName(TimeOfDayPeriod period)
{
	int idx = static_cast<int>(period);
	return (idx >= 0 && idx < kPeriodCount) ? kPeriodNames[idx] : "Unknown";
}

SceneSettingsManager::TimeOfDayPeriod SceneSettingsManager::GetPeriodFromName(const std::string& name)
{
	for (int i = 0; i < kPeriodCount; ++i) {
		if (name == GetPeriodName(static_cast<TimeOfDayPeriod>(i)))
			return static_cast<TimeOfDayPeriod>(i);
	}
	return TimeOfDayPeriod::Count;
}

float SceneSettingsManager::GetCurrentGameHour()
{
	// Prefer calendar (ground truth), which the Weather Editor slider writes to.
	// sky->currentGameHour may lag when timeScale is 0 (time paused).
	auto calendar = globals::game::calendar ? globals::game::calendar : RE::Calendar::GetSingleton();
	float hour = 12.0f;
	if (calendar && calendar->gameHour)
		hour = calendar->gameHour->value;
	else if (auto sky = globals::game::sky)
		hour = sky->currentGameHour;
	if (!std::isfinite(hour))
		hour = 12.0f;

	// Normalize into [0, 24) so midnight is 0 and never 24.
	hour = std::clamp(hour, 0.0f, 24.0f);
	if (hour >= 24.0f)
		hour = 0.0f;
	return hour;
}

void SceneSettingsManager::GetTimeOfDayFactors(float outFactors[kPeriodCount])
{
	for (int i = 0; i < kPeriodCount; ++i)
		outFactors[i] = 0.0f;

	float hour = GetCurrentGameHour();

	// Normalize to [0, 24) - Night wraps, so also check hour + 24 for pre-dawn hours
	for (int i = 0; i < kPeriodCount; ++i) {
		float start = kPeriodHours[i][0];
		float end = kPeriodHours[i][1];
		float h = (end > 24.0f && hour < start) ? hour + 24.0f : hour;

		if (h >= start && h < end) {
			// Inside this period - check if we're in the blend-out zone near the end.
			float distFromEnd = end - h;

			if (distFromEnd < kTransitionHours) {
				// Blending out to next period
				float t = distFromEnd / kTransitionHours;
				outFactors[i] = t;
				outFactors[(i + 1) % kPeriodCount] = 1.0f - t;
			} else {
				outFactors[i] = 1.0f;
			}
			return;
		}
	}

	// Fallback: noon = Day
	outFactors[static_cast<int>(TimeOfDayPeriod::Day)] = 1.0f;
}

SceneSettingsManager::TimeOfDayPeriod SceneSettingsManager::GetCurrentPeriod()
{
	float hour = GetCurrentGameHour();
	for (int i = 0; i < kPeriodCount; ++i) {
		float start = kPeriodHours[i][0];
		float end = kPeriodHours[i][1];
		float h = (end > 24.0f && hour < start) ? hour + 24.0f : hour;
		if (h >= start && h < end)
			return static_cast<TimeOfDayPeriod>(i);
	}
	return TimeOfDayPeriod::Day;
}

// --- Feature Metadata ---

bool SceneSettingsManager::IsFeatureAllowedForType(SceneType type, const std::string& featureShortName)
{
	if (!Feature::FindFeatureByShortName(featureShortName))
		return false;

	switch (type) {
	case SceneType::InteriorOnly:
		return IsInteriorOnlyFeatureAllowed(featureShortName) &&
		       CatalogHasSceneSettings(featureShortName, type);
	case SceneType::TimeOfDay:
		return IsTimeOfDayFeatureAllowed(featureShortName) &&
		       CatalogHasSceneSettings(featureShortName, type);
	case SceneType::Location:
		return (IsInteriorOnlyFeatureAllowed(featureShortName) ||
				   IsTimeOfDayFeatureAllowed(featureShortName)) &&
		       CatalogHasSceneSettings(featureShortName, type);
	default:
		return false;
	}
}

bool SceneSettingsManager::IsSettingAllowedForType(SceneType type,
	const std::string& featureShortName, const std::vector<std::string>& settingPath,
	const std::string& settingKey)
{
	auto* setting = FindAllowedCatalogSetting(featureShortName, settingPath, settingKey);
	return Feature::FindFeatureByShortName(featureShortName) && setting &&
	       IsCatalogSettingAllowedForSceneType(type, *setting);
}

bool SceneSettingsManager::IsSceneSettingAllowed(
	std::string_view featureShortName, std::string_view settingPath, std::string_view settingKey)
{
	auto* setting = SceneSettingsCatalog::FindSetting(featureShortName, settingPath, settingKey);
	return setting && IsCatalogSettingAllowedByPolicy(*setting);
}

std::vector<std::string> SceneSettingsManager::GetInteriorRelevantFeatureNames()
{
	return GetLoadedCatalogFeatureNames(SceneType::InteriorOnly);
}

std::vector<std::string> SceneSettingsManager::GetExteriorRelevantFeatureNames()
{
	return GetLoadedCatalogFeatureNames(SceneType::TimeOfDay);
}

std::vector<std::string> SceneSettingsManager::GetLocationRelevantFeatureNames()
{
	return GetLoadedCatalogFeatureNames(SceneType::Location);
}

std::string SceneSettingsManager::GetFeatureDisplayName(const std::string& featureShortName)
{
	auto* feature = Feature::FindFeatureByShortName(featureShortName);
	return feature ? feature->GetDisplayName() : featureShortName;
}

namespace
{
	std::string GetDescriptorLabel(const SceneSettingsManager::SettingControlInfo& info,
		std::string_view component = {})
	{
		std::string leaf = info.displayName;
		if (!component.empty())
			leaf += std::format(" ({})", component);
		if (info.displayPath.empty())
			return leaf;
		return std::format("{}: {}", JoinDisplayParts(info.displayPath, {}), leaf);
	}

	ManagerSettingDescriptor MakeScalarDescriptor(
		const SceneSettingsCatalog::SettingMetadata& setting, const json& value)
	{
		auto info = MakeSettingControlInfo(setting);
		const auto physicalPath = SplitCatalogPath(setting.settingPath);
		const auto physicalKey = std::string(setting.settingKey);
		const auto component = info.controlType == SceneSettingControlType::Scalar ?
		                           std::string() :
		                           info.componentDisplayName;
		return {
			.settingPath = physicalPath,
			.key = physicalKey,
			.displayName = GetDescriptorLabel(info, component),
			.displayPath = GetCatalogSelectorPath(setting),
			.value = value,
			.controlType = SceneSettingControlType::Scalar,
			.aggregatePresentation = ManagerAggregatePresentation::Components,
			.unifiedEditMode = ManagerUnifiedEditMode::None,
			.members = { { physicalPath, physicalKey, info.componentDisplayName, value,
				setting.serializedComponent, info.aggregateAll } },
		};
	}

	using DescriptorGroupKey = std::tuple<std::string, std::string, std::int8_t, std::uint8_t, SceneSettingControlType>;

	std::vector<ManagerSettingDescriptor> CollectFeatureSceneSettings(
		const std::string& featureShortName, SceneSettingsManager::SceneType type,
		const json& featureSettings)
	{
		if (!featureSettings.is_object())
			return {};

		const bool transitionableOnly = type == SceneSettingsManager::SceneType::TimeOfDay;
		std::vector<ManagerSettingDescriptor> descriptors;
		std::map<DescriptorGroupKey, ManagerSettingDescriptor> groups;
		for (const auto& setting : GetCatalogFeatureSettings(featureShortName)) {
			if (!IsCatalogSettingAllowedForSceneType(type, setting))
				continue;
			auto settingPath = SplitCatalogPath(setting.settingPath);

			if (setting.settingKey.empty())
				continue;

			const auto* value = GetCatalogSerializedValue(featureSettings, setting);
			if (!value || !IsSceneSettingPrimitive(*value) ||
				!IsCatalogValueCompatible(setting, *value) ||
				(transitionableOnly && !IsNumericValue(*value)))
				continue;

			auto info = MakeSettingControlInfo(setting);
			if (info.controlType == SceneSettingControlType::Scalar || info.componentCount < 2) {
				descriptors.push_back(MakeScalarDescriptor(setting, *value));
				continue;
			}

			DescriptorGroupKey key{
				std::string(setting.serializedPath), std::string(setting.serializedKey),
				info.componentStart, info.componentCount, info.controlType
			};
			auto [groupIt, inserted] = groups.try_emplace(key);
			auto& descriptor = groupIt->second;
			if (inserted) {
				descriptor.settingPath = settingPath;
				descriptor.key = std::string(setting.settingKey);
				descriptor.displayName = GetDescriptorLabel(info);
				descriptor.displayPath = GetCatalogSelectorPath(setting);
				descriptor.value = *value;
				descriptor.controlType = info.controlType;
				descriptor.aggregatePresentation = info.aggregatePresentation;
				descriptor.unifiedEditMode = info.unifiedEditMode;
			} else {
				if (descriptor.aggregatePresentation != info.aggregatePresentation)
					descriptor.aggregatePresentation = ManagerAggregatePresentation::Components;
				if (descriptor.unifiedEditMode != info.unifiedEditMode)
					descriptor.unifiedEditMode = ManagerUnifiedEditMode::None;
			}
			descriptor.members.push_back({ std::move(settingPath), std::string(setting.settingKey), info.componentDisplayName,
				*value, setting.serializedComponent, info.aggregateAll });
		}

		for (auto& [key, descriptor] : groups) {
			const auto expectedCount = std::get<3>(key);
			const auto expectedStart = std::get<2>(key);
			std::sort(descriptor.members.begin(), descriptor.members.end(), [](const auto& lhs, const auto& rhs) {
				return lhs.componentIndex < rhs.componentIndex;
			});
			bool complete = descriptor.members.size() == expectedCount;
			for (size_t index = 0; complete && index < descriptor.members.size(); ++index)
				complete = descriptor.members[index].componentIndex == expectedStart + index;
			if (complete) {
				descriptors.push_back(std::move(descriptor));
				continue;
			}
			for (const auto& member : descriptor.members) {
				auto* setting = FindAllowedCatalogSetting(
					featureShortName, member.settingPath, member.key, transitionableOnly);
				if (setting)
					descriptors.push_back(MakeScalarDescriptor(*setting, member.value));
			}
		}

		std::sort(descriptors.begin(), descriptors.end(), [](const auto& lhs, const auto& rhs) {
			return std::tie(lhs.displayPath, lhs.displayName, lhs.settingPath, lhs.key) <
			       std::tie(rhs.displayPath, rhs.displayName, rhs.settingPath, rhs.key);
		});
		return descriptors;
	}
}

std::vector<SceneSettingsManager::SettingDescriptor> SceneSettingsManager::GetFeatureSceneSettings(
	SceneType type, const std::string& featureShortName)
{
	auto* manager = GetSingleton();
	return manager ? manager->GetCachedFeatureSceneSettings(type, featureShortName) :
	                 std::vector<SettingDescriptor>{};
}

std::vector<SceneSettingsManager::SettingDescriptor> SceneSettingsManager::GetTransitionableSceneSettings(const std::string& featureShortName)
{
	auto* manager = GetSingleton();
	return manager ? manager->GetCachedFeatureSceneSettings(SceneType::TimeOfDay, featureShortName) :
	                 std::vector<SettingDescriptor>{};
}

const std::vector<SceneSettingsManager::SettingDescriptor>& SceneSettingsManager::GetCachedFeatureSceneSettings(
	SceneType type, const std::string& featureShortName)
{
	static const std::vector<SettingDescriptor> empty;
	if (type != SceneType::InteriorOnly && type != SceneType::TimeOfDay && type != SceneType::Location)
		return empty;
	auto cacheIt = featurePresentationCache.find(featureShortName);
	if (cacheIt == featurePresentationCache.end()) {
		const auto* snapshot = GetFeatureBaseSnapshot(featureShortName);
		if (!snapshot)
			return empty;
		CachedFeaturePresentation presentation;
		presentation.interiorSettings = CollectFeatureSceneSettings(
			featureShortName, SceneType::InteriorOnly, *snapshot);
		presentation.timeOfDaySettings = CollectFeatureSceneSettings(
			featureShortName, SceneType::TimeOfDay, *snapshot);
		presentation.locationSettings = CollectFeatureSceneSettings(
			featureShortName, SceneType::Location, *snapshot);
		cacheIt = featurePresentationCache.emplace(featureShortName, std::move(presentation)).first;
	}
	switch (type) {
	case SceneType::InteriorOnly:
		return cacheIt->second.interiorSettings;
	case SceneType::TimeOfDay:
		return cacheIt->second.timeOfDaySettings;
	case SceneType::Location:
		return cacheIt->second.locationSettings;
	default:
		return empty;
	}
}

void SceneSettingsManager::InvalidateFeatureSnapshot(std::string_view featureShortName)
{
	cachedLocationOverridesValid = false;
	locationOverridesDirty = true;
	if (featureShortName.empty()) {
		featureBaseSnapshots.clear();
		featurePresentationCache.clear();
		featureApplyDocuments.clear();
		pendingApplyVerifications.clear();
		std::erase_if(baselineSettings,
			[&](const auto& item) { return !appliedSettings.contains(item.first); });
		return;
	}
	const auto featureName = std::string(featureShortName);
	featureBaseSnapshots.erase(featureName);
	featurePresentationCache.erase(featureName);
	featureApplyDocuments.erase(featureName);
	pendingApplyVerifications.erase(featureName);
	std::erase_if(baselineSettings, [&](const auto& item) {
		return item.first.featureShortName == featureName && !appliedSettings.contains(item.first);
	});
}

bool SceneSettingsManager::GetSettingControlInfo(const SettingEntry& entry, SettingControlInfo& info)
{
	auto* setting = FindAllowedCatalogSetting(
		entry.featureShortName, entry.settingPath, entry.settingKey);
	if (!setting)
		return false;
	info = MakeSettingControlInfo(*setting);
	return true;
}

std::string SceneSettingsManager::GetSettingDisplayName(const std::string& settingKey)
{
	return StripImGuiId(Util::PrettifyIdentifier(settingKey));
}

static std::string GetSceneSettingDisplayName(const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey)
{
	auto* setting = FindAllowedCatalogSetting(featureShortName, settingPath, settingKey);
	if (setting) {
		auto info = MakeSettingControlInfo(*setting);
		auto displayName = info.displayName;
		if (info.controlType != SceneSettingControlType::Scalar && !info.componentDisplayName.empty())
			displayName += std::format(" ({})", info.componentDisplayName);
		return JoinDisplayParts(info.displayPath, displayName);
	}
	return SceneSettingsManager::GetSettingDisplayName(settingKey);
}

json SceneSettingsManager::GetFeatureSettingValue(const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey)
{
	auto* setting = FindAllowedCatalogSetting(featureShortName, settingPath, settingKey);
	if (!setting)
		return {};
	auto* manager = GetSingleton();
	if (!manager)
		return {};
	const auto* snapshot = manager->GetFeatureBaseSnapshot(featureShortName);
	if (!snapshot)
		return {};
	const auto* value = GetCatalogSerializedValue(*snapshot, *setting);
	return value && IsSceneSettingPrimitive(*value) ? *value : json{};
}

SceneSettingsManager::SettingType SceneSettingsManager::DetectSettingType(const json& value)
{
	if (value.is_boolean())
		return SettingType::Boolean;
	if (value.is_number_integer())
		return SettingType::Integer;
	if (value.is_number_float())
		return SettingType::Float;
	if (value.is_string())
		return SettingType::String;
	return SettingType::Unknown;
}

bool SceneSettingsManager::IsBooleanControlSetting(const SettingEntry& entry)
{
	auto* setting = FindAllowedCatalogSetting(
		entry.featureShortName, entry.settingPath, entry.settingKey);
	return setting && SceneSettingsCatalog::HasFlag(
						  setting->flags, SceneSettingsCatalog::SettingFlag::BooleanControl);
}

bool SceneSettingsManager::IsInvertedDisplaySetting(const SettingEntry& entry)
{
	auto* setting = FindAllowedCatalogSetting(
		entry.featureShortName, entry.settingPath, entry.settingKey);
	return setting && setting->invertedDisplay;
}

bool SceneSettingsManager::GetNumericBounds(const SettingEntry& entry, double& minimum, double& maximum)
{
	auto* setting = FindAllowedCatalogSetting(
		entry.featureShortName, entry.settingPath, entry.settingKey);
	if (!setting || setting->editorSemantic != SceneSettingsCatalog::EditorSemantic::Numeric ||
		!setting->hasNumericBounds || !std::isfinite(setting->minimumValue) ||
		!std::isfinite(setting->maximumValue) || setting->minimumValue > setting->maximumValue)
		return false;
	minimum = setting->minimumValue;
	maximum = setting->maximumValue;
	return true;
}

double SceneSettingsManager::GetNumericDisplayScale(const SettingEntry& entry)
{
	auto* setting = FindAllowedCatalogSetting(
		entry.featureShortName, entry.settingPath, entry.settingKey);
	if (!setting || setting->editorSemantic != SceneSettingsCatalog::EditorSemantic::Numeric)
		return 1.0;
	return GetCatalogNumericDisplayScale(*setting);
}

bool SceneSettingsManager::GetNumericDisplayValue(
	const SettingEntry& entry, double storedValue, double& displayValue)
{
	auto* setting = FindAllowedCatalogSetting(
		entry.featureShortName, entry.settingPath, entry.settingKey);
	return setting && ConvertCatalogNumericStoredToDisplay(*setting, storedValue, displayValue);
}

bool SceneSettingsManager::GetNumericStoredValue(
	const SettingEntry& entry, double displayValue, double& storedValue)
{
	auto* setting = FindAllowedCatalogSetting(
		entry.featureShortName, entry.settingPath, entry.settingKey);
	return setting && ConvertCatalogNumericDisplayToStored(*setting, displayValue, storedValue);
}

bool SceneSettingsManager::IsNumericInputClamped(const SettingEntry& entry)
{
	auto* setting = FindAllowedCatalogSetting(
		entry.featureShortName, entry.settingPath, entry.settingKey);
	return setting && setting->clampNumericInput;
}

bool SceneSettingsManager::IsHDRColorSetting(const SettingEntry& entry)
{
	auto* setting = FindAllowedCatalogSetting(
		entry.featureShortName, entry.settingPath, entry.settingKey);
	return setting && setting->hdrColor;
}

size_t SceneSettingsManager::GetSettingChoiceCount(const SettingEntry& entry)
{
	auto* setting = FindAllowedCatalogSetting(
		entry.featureShortName, entry.settingPath, entry.settingKey);
	return setting && setting->editorSemantic == SceneSettingsCatalog::EditorSemantic::Choice ?
	           setting->choiceCount :
	           0;
}

bool SceneSettingsManager::GetSettingChoice(
	const SettingEntry& entry, size_t index, std::int64_t& value, std::string& displayName)
{
	auto* setting = FindAllowedCatalogSetting(
		entry.featureShortName, entry.settingPath, entry.settingKey);
	if (!setting || setting->editorSemantic != SceneSettingsCatalog::EditorSemantic::Choice ||
		index >= setting->choiceCount)
		return false;
	const auto& choice = setting->choices[index];
	value = choice.value;
	displayName = StripImGuiId(choice.displayName);
	if (!choice.displayNameKey.empty())
		displayName = StripImGuiId(T(choice.displayNameKey, displayName.c_str()));
	return true;
}

static bool GetFeatureSettingValueForValidation(const std::string& featureShortName,
	const SceneSettingsCatalog::SettingMetadata& setting, json& featureValue)
{
	featureValue = SceneSettingsManager::GetFeatureSettingValue(
		featureShortName, SplitCatalogPath(setting.settingPath), std::string(setting.settingKey));
	return IsSceneSettingPrimitive(featureValue);
}

static bool IsSceneSettingValueAllowed(const json& featureValue,
	const SceneSettingsCatalog::SettingMetadata& setting, const json& value, bool requireNumeric)
{
	if (!IsCatalogValueCompatible(setting, featureValue) || !IsCatalogValueCompatible(setting, value))
		return false;

	if (value.is_number() && !std::isfinite(value.get<double>()))
		return false;
	if (setting.editorSemantic == SceneSettingsCatalog::EditorSemantic::Numeric) {
		double featureDisplayValue = 0.0;
		double candidateDisplayValue = 0.0;
		if (!featureValue.is_number() || !value.is_number() ||
			!ConvertCatalogNumericStoredToDisplay(setting, featureValue.get<double>(), featureDisplayValue) ||
			!ConvertCatalogNumericStoredToDisplay(setting, value.get<double>(), candidateDisplayValue))
			return false;
		if (setting.clampNumericInput && setting.hasNumericBounds &&
			(candidateDisplayValue < setting.minimumValue || candidateDisplayValue > setting.maximumValue))
			return false;
	}

	if (SceneSettingsCatalog::HasFlag(setting.flags, SceneSettingsCatalog::SettingFlag::BooleanControl)) {
		if (setting.valueType == SceneSettingsCatalog::ValueType::Integer &&
			(!value.is_number_integer() || (value.get<std::int64_t>() != 0 && value.get<std::int64_t>() != 1)))
			return false;
		if (setting.valueType == SceneSettingsCatalog::ValueType::Boolean && !value.is_boolean())
			return false;
	}

	if (setting.choiceCount > 0) {
		if (!value.is_number_integer())
			return false;
		const auto choiceValue = value.get<std::int64_t>();
		if (std::none_of(setting.choices, setting.choices + setting.choiceCount,
				[&](const auto& choice) { return choice.value == choiceValue; }))
			return false;
	}

	if (requireNumeric && (!SceneSettingsCatalog::HasFlag(setting.flags, SceneSettingsCatalog::SettingFlag::Transitionable) ||
							  !IsNumericValue(featureValue) || !IsNumericValue(value) || !std::isfinite(value.get<float>())))
		return false;
	if (!requireNumeric && !IsSceneSettingPrimitive(value))
		return false;

	return IsCompatibleSceneSettingValue(featureValue, value);
}

static bool ValidateSceneSettingEntry(std::string_view context, SceneSettingsManager::SceneType type,
	const std::string& featureShortName, const std::vector<std::string>& settingPath,
	const std::string& settingKey, const json& value, bool requireNumeric)
{
	if (!IsSettingAllowedBySceneTypePolicy(type, featureShortName, settingPath, settingKey)) {
		logger::warn("[SceneSettings] {} entry {} is not whitelisted for this scene type",
			context, GetSettingLogName(featureShortName, settingPath, settingKey));
		return false;
	}
	if (IsBlacklistedSceneSetting(featureShortName, settingPath, settingKey)) {
		logger::warn("[SceneSettings] {} entry {} is blacklisted",
			context, GetSettingLogName(featureShortName, settingPath, settingKey));
		return false;
	}

	auto* setting = FindAllowedCatalogSetting(featureShortName, settingPath, settingKey, requireNumeric);
	if (!setting) {
		logger::warn("[SceneSettings] {} entry {} is not permitted by the compiled scene settings catalog",
			context, GetSettingLogName(featureShortName, settingPath, settingKey));
		return false;
	}

	auto* feature = Feature::FindFeatureByShortName(featureShortName);
	if (!feature) {
		logger::warn("[SceneSettings] {} entry {} - feature '{}' not found/loaded",
			context, GetSettingLogName(featureShortName, settingPath, settingKey), featureShortName);
		return false;
	}

	json featureValue;
	if (!GetFeatureSettingValueForValidation(featureShortName, *setting, featureValue) ||
		!IsSceneSettingValueAllowed(featureValue, *setting, value, requireNumeric)) {
		logger::warn("[SceneSettings] {} entry {} is not a supported scene-manager setting",
			context, GetSettingLogName(featureShortName, settingPath, settingKey));
		return false;
	}
	return true;
}

static bool ApplyEntryValueUpdates(std::string_view context, SceneSettingsManager::SceneType type,
	std::vector<SceneSettingsManager::SettingEntry>& entries,
	std::span<const SceneSettingsManager::EntryValueUpdate> updates,
	bool requireNumeric, bool& userEntriesChanged)
{
	if (updates.empty())
		return false;

	std::set<size_t> updatedIndices;
	for (const auto& update : updates) {
		if (update.index >= entries.size() || !updatedIndices.insert(update.index).second)
			return false;
		const auto& entry = entries[update.index];
		if (!IsSettingAllowedBySceneTypePolicy(
				type, entry.featureShortName, entry.settingPath, entry.settingKey))
			return false;
		auto* setting = FindAllowedCatalogSetting(
			entry.featureShortName, entry.settingPath, entry.settingKey, requireNumeric);
		if (!setting || !Feature::FindFeatureByShortName(entry.featureShortName) ||
			!IsSceneSettingValueAllowed(entry.value, *setting, update.value, requireNumeric)) {
			logger::warn("[SceneSettings] {} update {} is not a supported value",
				context, GetSettingLogName(entry.featureShortName, entry.settingPath, entry.settingKey));
			return false;
		}
	}

	userEntriesChanged = false;
	for (const auto& update : updates) {
		auto& entry = entries[update.index];
		entry.value = update.value;
		userEntriesChanged |= entry.source == SceneSettingsManager::EntrySource::User;
	}
	return true;
}

// --- Generic Entry Management ---

std::vector<SceneSettingsManager::SettingEntry>& SceneSettingsManager::GetEntriesMut(SceneType type)
{
	assert(IsEntryListSceneType(type));
	return entries[type];
}

void SceneSettingsManager::BumpEntryPresentationRevision()
{
	++entryPresentationRevision;
	activeEntryCacheDirty = true;
}

const std::vector<SceneSettingsManager::SettingEntry>& SceneSettingsManager::GetEntries(SceneType type) const
{
	static const std::vector<SettingEntry> empty;
	if (!IsEntryListSceneType(type))
		return empty;
	auto it = entries.find(type);
	return (it != entries.end()) ? it->second : empty;
}

void SceneSettingsManager::MarkEntryListUserSettingsModified(SceneType type)
{
	assert(IsEntryListSceneType(type));
	if (type == SceneType::InteriorOnly)
		interiorUserSettingsModified = true;
	else
		timeOfDayUserSettingsModified = true;
}

bool SceneSettingsManager::IsEntryActive(const SettingEntry& entry) const
{
	return !entry.paused && !IsFeaturePaused(entry.featureShortName);
}

bool SceneSettingsManager::HasEntryFromSource(SceneType type, const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey, EntrySource source) const
{
	for (const auto& entry : GetEntries(type)) {
		if (entry.source == source && IsSameSetting(entry, featureShortName, settingPath, settingKey))
			return true;
	}
	return false;
}

bool SceneSettingsManager::HasEntryForPeriod(const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey,
	TimeOfDayPeriod period, EntrySource source) const
{
	for (const auto& entry : GetEntries(SceneType::TimeOfDay)) {
		if (entry.source == source && entry.period == period &&
			IsSameSetting(entry, featureShortName, settingPath, settingKey))
			return true;
	}
	return false;
}

bool SceneSettingsManager::HasDuplicateEntry(SceneType type, const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey, EntrySource source, TimeOfDayPeriod period) const
{
	if (!IsEntryListSceneType(type))
		return false;
	if (type == SceneType::TimeOfDay)
		return HasEntryForPeriod(featureShortName, settingPath, settingKey, period, source);
	return HasEntryFromSource(type, featureShortName, settingPath, settingKey, source);
}

bool SceneSettingsManager::AddSetting(SceneType type, const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey, const json& value,
	TimeOfDayPeriod period, bool deferCommit)
{
	if (!IsEntryListSceneType(type) ||
		!IsSettingAllowedForType(type, featureShortName, settingPath, settingKey))
		return false;

	const bool requireNumeric = type == SceneType::TimeOfDay;
	if (requireNumeric) {
		// Reject invalid period values (Count is the sentinel, not a real period)
		if (period == TimeOfDayPeriod::Count || static_cast<int>(period) < 0 || static_cast<int>(period) >= kPeriodCount) {
			logger::warn("[SceneSettings] Rejecting TOD setting with invalid period: {}", GetSettingLogName(featureShortName, settingPath, settingKey));
			return false;
		}
	}
	if (!ValidateSceneSettingEntry(
			GetSceneTypeName(type), type, featureShortName, settingPath, settingKey, value, requireNumeric))
		return false;

	if (HasDuplicateEntry(type, featureShortName, settingPath, settingKey, EntrySource::User, period))
		return false;

	auto& vec = GetEntriesMut(type);

	SettingEntry entry;
	entry.featureShortName = featureShortName;
	entry.settingPath = settingPath;
	entry.settingKey = settingKey;
	entry.displayName = GetSceneSettingDisplayName(featureShortName, settingPath, settingKey);
	entry.value = value;
	entry.originalValue = entry.value;
	entry.source = EntrySource::User;
	entry.period = period;
	vec.push_back(std::move(entry));
	if (type == SceneType::TimeOfDay)
		++sceneValueRevision;
	BumpEntryPresentationRevision();
	MarkEntryListUserSettingsModified(type);
	if (deferCommit)
		MarkDeferredSceneChanges();
	else
		CommitSceneSettingChanges();
	return true;
}

void SceneSettingsManager::RemoveSetting(SceneType type, size_t index)
{
	if (!IsEntryListSceneType(type))
		return;
	auto& vec = GetEntriesMut(type);
	if (index >= vec.size())
		return;

	const auto entry = vec[index];
	if (entry.source == EntrySource::Overwrite && !entry.sourceFilename.empty() &&
		!RemoveSettingFromOverwriteFile(GetSceneOverwritePath(type, entry), entry.settingPath, entry.settingKey))
		return;

	logger::info("[SceneSettings] Removed {} entry: {} (source={})", GetSceneTypeName(type),
		GetSettingLogName(entry.featureShortName, entry.settingPath, entry.settingKey),
		entry.source == EntrySource::Overwrite ? "overwrite" : "user");

	vec.erase(vec.begin() + static_cast<ptrdiff_t>(index));
	if (entry.source == EntrySource::Overwrite) {
		const auto directory = type == SceneType::TimeOfDay && entry.period != TimeOfDayPeriod::Count ?
		                           GetOverwritesPath(type) / GetPeriodName(entry.period) :
		                           GetOverwritesPath(type);
		DiscoverOverwritesInDir(type, directory, entry.period);
	}
	if (type == SceneType::TimeOfDay)
		++sceneValueRevision;
	BumpEntryPresentationRevision();
	if (entry.source == EntrySource::User) {
		MarkEntryListUserSettingsModified(type);
		SaveAllUserSettings();
	}
	ReapplyIfActive();
}

void SceneSettingsManager::TogglePauseEntry(SceneType type, size_t index)
{
	if (!IsEntryListSceneType(type))
		return;
	auto& vec = GetEntriesMut(type);
	if (index < vec.size()) {
		vec[index].paused = !vec[index].paused;
		if (type == SceneType::TimeOfDay)
			++sceneValueRevision;
		BumpEntryPresentationRevision();
		if (vec[index].source == EntrySource::User) {
			MarkEntryListUserSettingsModified(type);
			SaveAllUserSettings();
		}
		ReapplyIfActive();
	}
}

void SceneSettingsManager::RevertEntryToDefault(SceneType type, size_t index)
{
	if (!IsEntryListSceneType(type))
		return;
	auto& vec = GetEntriesMut(type);
	if (index >= vec.size())
		return;
	auto& entry = vec[index];
	if (entry.originalValue.is_null() ||
		!ValidateSceneSettingEntry(GetSceneTypeName(type), type, entry.featureShortName,
			entry.settingPath, entry.settingKey, entry.originalValue, type == SceneType::TimeOfDay))
		return;

	entry.value = entry.originalValue;
	if (type == SceneType::TimeOfDay)
		++sceneValueRevision;
	if (entry.source == EntrySource::User) {
		MarkEntryListUserSettingsModified(type);
		SaveAllUserSettings();
	}
	ReapplyIfActive(false);
}

void SceneSettingsManager::SetAllOverwritesPaused(SceneType type, bool paused)
{
	if (!IsEntryListSceneType(type))
		return;
	bool changed = false;
	for (auto& entry : GetEntriesMut(type)) {
		if (entry.source == EntrySource::Overwrite && entry.paused != paused) {
			entry.paused = paused;
			changed = true;
		}
	}
	if (changed && type == SceneType::TimeOfDay)
		++sceneValueRevision;
	if (changed)
		BumpEntryPresentationRevision();
	ReapplyIfActive();
}

bool SceneSettingsManager::AreAllOverwritesPaused(SceneType type) const
{
	if (!IsEntryListSceneType(type))
		return false;
	bool found = false;
	for (const auto& entry : GetEntries(type)) {
		if (entry.source != EntrySource::Overwrite)
			continue;
		found = true;
		if (!entry.paused)
			return false;
	}
	return found;
}

void SceneSettingsManager::DeleteAllOverwrites(SceneType type)
{
	if (!IsEntryListSceneType(type))
		return;
	DeleteEntryLayer(EntrySource::Overwrite,
		type == SceneType::InteriorOnly ? SceneContextType::Interior : SceneContextType::TimeOfDay);
}

void SceneSettingsManager::SetAllUserPaused(SceneType type, bool paused)
{
	if (!IsEntryListSceneType(type))
		return;
	bool changed = false;
	for (auto& entry : GetEntriesMut(type)) {
		if (entry.source == EntrySource::User && entry.paused != paused) {
			entry.paused = paused;
			changed = true;
		}
	}
	if (changed && type == SceneType::TimeOfDay)
		++sceneValueRevision;
	if (changed)
		BumpEntryPresentationRevision();
	MarkEntryListUserSettingsModified(type);
	SaveAllUserSettings();
	ReapplyIfActive();
}

bool SceneSettingsManager::AreAllUserPaused(SceneType type) const
{
	if (!IsEntryListSceneType(type))
		return false;
	bool found = false;
	for (const auto& entry : GetEntries(type)) {
		if (entry.source != EntrySource::User)
			continue;
		found = true;
		if (!entry.paused)
			return false;
	}
	return found;
}

void SceneSettingsManager::DeleteAllUserSettings(SceneType type)
{
	if (!IsEntryListSceneType(type))
		return;
	auto& vec = GetEntriesMut(type);
	const auto removed = std::erase_if(vec, [](const SettingEntry& e) {
		return e.source == EntrySource::User;
	});
	if (removed != 0 && type == SceneType::TimeOfDay)
		++sceneValueRevision;
	if (removed != 0)
		BumpEntryPresentationRevision();
	unresolvedUserEntries[type].clear();

	MarkEntryListUserSettingsModified(type);
	SaveAllUserSettings();
	ReapplyIfActive();
}

static std::string GetSceneOverwriteTypeDescription(SceneSettingsManager::SceneType type, SceneSettingsManager::TimeOfDayPeriod period)
{
	if (type == SceneSettingsManager::SceneType::InteriorOnly)
		return "Interior Only";
	if (period != SceneSettingsManager::TimeOfDayPeriod::Count)
		return std::format("Time of Day - {}", SceneSettingsManager::GetPeriodName(period));
	return "Time of Day";
}

static std::string GetWeatherOverwriteTypeDescription(SceneSettingsManager::TimeOfDayPeriod period)
{
	if (period != SceneSettingsManager::TimeOfDayPeriod::Count)
		return std::format("Weather - {}", SceneSettingsManager::GetPeriodName(period));
	return "Weather";
}

static std::filesystem::path GetSceneOverwritePath(SceneSettingsManager::SceneType type, const SceneSettingsManager::SettingEntry& entry)
{
	if (!entry.sourcePath.empty())
		return entry.sourcePath;

	auto basePath = SceneSettingsManager::GetOverwritesPath(type);
	if (type == SceneSettingsManager::SceneType::TimeOfDay && entry.period != SceneSettingsManager::TimeOfDayPeriod::Count)
		return basePath / SceneSettingsManager::GetPeriodName(entry.period) / entry.sourceFilename;
	return basePath / entry.sourceFilename;
}

static std::filesystem::path GetWeatherOverwritePath(RE::FormID weatherId, const SceneSettingsManager::SettingEntry& entry)
{
	if (!entry.sourcePath.empty())
		return entry.sourcePath;

	auto basePath = SceneSettingsManager::GetWeatherOverwritesDir() / Util::FormIdToSpid(weatherId);
	if (entry.period != SceneSettingsManager::TimeOfDayPeriod::Count)
		return basePath / SceneSettingsManager::GetPeriodName(entry.period) / entry.sourceFilename;
	return basePath / entry.sourceFilename;
}

static std::filesystem::path GetLocationOverwritePath(SceneSettingsManager::LocationTargetType type,
	std::string_view formKey, const SceneSettingsManager::SettingEntry& entry)
{
	if (!entry.sourcePath.empty())
		return entry.sourcePath;
	auto directory = SceneSettingsManager::GetLocationOverwritesDir(type) / formKey;
	if (entry.period != SceneSettingsManager::TimeOfDayPeriod::Count)
		directory /= SceneSettingsManager::GetPeriodName(entry.period);
	return directory / entry.sourceFilename;
}

static bool WriteGroupedOverwriteFile(const std::filesystem::path& allowedRoot,
	const std::filesystem::path& path, const std::string& featureShortName,
	const std::string& overwriteType, const std::vector<const SceneSettingsManager::SettingEntry*>& entries,
	const json& extraMetadata = json::object())
{
	if (path.lexically_normal() == allowedRoot.lexically_normal() ||
		!Util::PathHelpers::IsPathLexicallyWithinDirectory(allowedRoot, path)) {
		logger::error("[SceneSettings] Refusing to write overwrite outside '{}': {}",
			allowedRoot.string(), path.string());
		return false;
	}
	std::error_code ec;
	const auto pathExists = std::filesystem::exists(path, ec);
	if (ec) {
		logger::error("[SceneSettings] WriteGroupedOverwriteFile: could not inspect '{}': {}", path.string(), ec.message());
		return false;
	}

	json data = json::object();
	if (pathExists && !ReadBoundedSceneJson(path, data)) {
		logger::error("[SceneSettings] Refusing to replace invalid overwrite file '{}'", path.string());
		return false;
	}

	if (auto featureIt = data.find(kFeatureKey); featureIt != data.end() &&
												 (!featureIt->is_string() || featureIt->get<std::string>() != featureShortName)) {
		logger::error("[SceneSettings] Refusing to relabel overwrite file '{}' from another feature", path.string());
		return false;
	}
	data[kFeatureKey] = featureShortName;
	auto& metadata = data[kMetadataKey];
	if (!metadata.is_null() && !metadata.is_object()) {
		logger::error("[SceneSettings] Refusing to replace invalid metadata in overwrite file '{}'", path.string());
		return false;
	}
	if (metadata.is_null())
		metadata = json::object();
	metadata[kMetadataDescriptionKey] = std::format("{} scene settings overwrite ({})",
		SceneSettingsManager::GetFeatureDisplayName(featureShortName), overwriteType);
	if (extraMetadata.is_object())
		for (const auto& [key, value] : extraMetadata.items())
			metadata[key] = value;
	auto& entryTransitions = metadata[kMetadataEntryTransitionsKey];
	if (!entryTransitions.is_null() && !entryTransitions.is_object()) {
		logger::error("[SceneSettings] Refusing to replace invalid entry transition metadata in overwrite file '{}'",
			path.string());
		return false;
	}
	if (entryTransitions.is_null())
		entryTransitions = json::object();
	for (const auto* entry : entries) {
		auto* node = GetObjectAtPath(data, entry->settingPath, true);
		if (!node) {
			logger::error("[SceneSettings] Refusing to replace a non-object path in overwrite file '{}'",
				path.string());
			return false;
		}
		(*node)[entry->settingKey] = entry->value;
		if (entry->transitionSeconds) {
			auto* transitionNode = GetObjectAtPath(entryTransitions, entry->settingPath, true);
			if (!transitionNode) {
				logger::error("[SceneSettings] Refusing to replace a non-object transition path in overwrite file '{}'",
					path.string());
				return false;
			}
			(*transitionNode)[entry->settingKey] = *entry->transitionSeconds;
		} else {
			RemoveObjectValueAtPath(entryTransitions, entry->settingPath, 0, entry->settingKey);
		}
	}
	if (entryTransitions.empty())
		metadata.erase(kMetadataEntryTransitionsKey);

	return WriteJsonAtomically(path, data, kOverwriteJsonIndent, "overwrite file");
}

static bool RemoveSettingsFromOverwriteFile(const std::filesystem::path& path,
	std::span<const SceneSettingsManager::SettingIdentity> settings)
{
	if (path.empty())
		return true;

	std::error_code ec;
	if (!std::filesystem::exists(path, ec))
		return !ec;

	json data;
	if (!ReadBoundedSceneJson(path, data)) {
		logger::error("[SceneSettings] Could not read overwrite file '{}' for editing", path.string());
		return false;
	}

	for (const auto& setting : settings) {
		if (!RemoveObjectValueAtPath(data, setting.settingPath, 0, setting.settingKey)) {
			logger::error("[SceneSettings] Overwrite setting '{}' was not found in '{}'",
				setting.settingKey, path.string());
			return false;
		}
		if (auto metadataIt = data.find(kMetadataKey);
			metadataIt != data.end() && metadataIt->is_object()) {
			if (auto transitionsIt = metadataIt->find(kMetadataEntryTransitionsKey);
				transitionsIt != metadataIt->end() && transitionsIt->is_object()) {
				RemoveObjectValueAtPath(*transitionsIt, setting.settingPath, 0, setting.settingKey);
				if (transitionsIt->empty())
					metadataIt->erase(transitionsIt);
			}
		}
	}
	if (!HasSceneOverwriteContent(data)) {
		auto removed = std::filesystem::remove(path, ec);
		if (removed || !ec)
			return true;
		logger::error("[SceneSettings] Failed to delete overwrite file '{}': {}", path.string(), ec.message());
		return false;
	}

	return WriteJsonAtomically(path, data, kOverwriteJsonIndent, "overwrite file");
}

static bool RemoveSettingFromOverwriteFile(const std::filesystem::path& path,
	const std::vector<std::string>& settingPath, const std::string& settingKey)
{
	const SceneSettingsManager::SettingIdentity setting{ {}, settingPath, settingKey };
	return RemoveSettingsFromOverwriteFile(path, std::span{ &setting, 1 });
}

SceneSettingsManager::OverwriteExportResult SceneSettingsManager::ExportUserSettingsToOverwrites(
	SceneType type, const std::vector<size_t>& indices, const std::string& modName)
{
	OverwriteExportResult result;
	if (!IsEntryListSceneType(type))
		return result;
	auto& vec = GetEntriesMut(type);
	auto basePath = GetOverwritesPath(type);
	auto safeModName = Util::FileHelpers::SanitizeFileName(modName);
	if (safeModName.empty())
		return result;

	std::map<std::pair<std::filesystem::path, std::string>, std::vector<const SettingEntry*>> groupedEntries;
	for (auto idx : indices) {
		if (idx >= vec.size() || vec[idx].source != EntrySource::User)
			continue;
		auto& e = vec[idx];
		auto dir = (type == SceneType::TimeOfDay && e.period != TimeOfDayPeriod::Count) ? basePath / GetPeriodName(e.period) : basePath;
		groupedEntries[{ dir, e.featureShortName }].push_back(&e);
	}

	for (const auto& [group, grouped] : groupedEntries) {
		const auto& [dir, featureShortName] = group;
		auto typeDescription = GetSceneOverwriteTypeDescription(type, grouped.front()->period);
		if (WriteGroupedOverwriteFile(basePath,
				dir / std::format("{}_{}.json", safeModName, featureShortName),
				featureShortName, typeDescription, grouped))
			++result.writtenFiles;
		else
			++result.failedFiles;
	}
	if (result.writtenFiles != 0)
		ReloadOverwriteEntries();
	return result;
}

SceneSettingsManager::OverwriteExportResult SceneSettingsManager::ExportWeatherUserSettingsToOverwrites(
	RE::FormID weatherId, const std::vector<size_t>& indices, const std::string& modName)
{
	OverwriteExportResult result;
	if (!TryEnsureWeatherDataLoaded())
		return result;

	auto& config = GetWeatherConfigMut(weatherId);
	auto& vec = config.entries;
	auto baseDir = GetWeatherOverwritesDir() / Util::FormIdToSpid(weatherId);
	auto safeModName = Util::FileHelpers::SanitizeFileName(modName);
	if (safeModName.empty())
		return result;

	std::map<std::pair<std::filesystem::path, std::string>, std::vector<const SettingEntry*>> groupedEntries;
	for (auto idx : indices) {
		if (idx >= vec.size() || vec[idx].source != EntrySource::User)
			continue;
		auto& e = vec[idx];
		auto dir = (e.period != TimeOfDayPeriod::Count) ? baseDir / GetPeriodName(e.period) : baseDir;
		groupedEntries[{ dir, e.featureShortName }].push_back(&e);
	}

	for (const auto& [group, grouped] : groupedEntries) {
		const auto& [dir, featureShortName] = group;
		auto typeDescription = GetWeatherOverwriteTypeDescription(grouped.front()->period);
		if (WriteGroupedOverwriteFile(GetWeatherOverwritesDir(),
				dir / std::format("{}_{}.json", safeModName, featureShortName),
				featureShortName, typeDescription, grouped, { { kMetadataTimeOfDayEnabledKey, config.timeOfDayEnabled } }))
			++result.writtenFiles;
		else
			++result.failedFiles;
	}
	if (result.writtenFiles != 0)
		ReloadOverwriteEntries();
	return result;
}

SceneSettingsManager::EntryLayerSummary SceneSettingsManager::GetEntryLayerSummary(EntrySource source, std::optional<SceneContextType> scope, const SceneContextId* target)
{
	if (target)
		scope = target->type;
	TryEnsureWeatherDataLoaded();
	TryEnsureLocationDataLoaded();
	EntryLayerSummary summary;
	const auto collect = [&](const std::vector<SettingEntry>& sourceEntries) {
		for (const auto& entry : sourceEntries) {
			if (entry.source != source)
				continue;
			++summary.count;
			if (entry.paused)
				++summary.paused;
		}
	};
	if (!scope || *scope == SceneContextType::Interior)
		collect(GetEntries(SceneType::InteriorOnly));
	if (!scope || *scope == SceneContextType::TimeOfDay)
		collect(GetEntries(SceneType::TimeOfDay));
	for (const auto& [weatherId, config] : weatherSceneConfigs)
		if ((!scope || *scope == SceneContextType::Weather) && (!target || target->weatherId == weatherId))
			collect(config.entries);
	for (const auto& [_, config] : locationSceneConfigs)
		if ((!scope || *scope == SceneContextType::Location) &&
			(!target || (target->locationType == config.type && NormalizeLocationFormKey(target->locationFormKey) == NormalizeLocationFormKey(config.formKey))))
			collect(config.entries);
	return summary;
}

void SceneSettingsManager::SetEntryLayerPaused(EntrySource source, bool paused, std::optional<SceneContextType> scope, const SceneContextId* target)
{
	if (target)
		scope = target->type;
	TryEnsureWeatherDataLoaded();
	TryEnsureLocationDataLoaded();
	bool changed = false;
	bool numericChanged = false;
	bool locationChanged = false;
	bool interiorUserChanged = false;
	bool timeOfDayUserChanged = false;
	bool weatherUserChanged = false;
	bool locationUserChanged = false;
	const auto apply = [&](std::vector<SettingEntry>& sourceEntries, bool numeric,
						   bool location, bool& userChanged) {
		for (auto& entry : sourceEntries) {
			if (entry.source != source || entry.paused == paused)
				continue;
			entry.paused = paused;
			changed = true;
			numericChanged |= numeric;
			locationChanged |= location;
			userChanged |= source == EntrySource::User;
		}
	};
	if (!scope || *scope == SceneContextType::Interior)
		apply(GetEntriesMut(SceneType::InteriorOnly), false, false, interiorUserChanged);
	if (!scope || *scope == SceneContextType::TimeOfDay)
		apply(GetEntriesMut(SceneType::TimeOfDay), true, false, timeOfDayUserChanged);
	for (auto& [weatherId, config] : weatherSceneConfigs)
		if ((!scope || *scope == SceneContextType::Weather) && (!target || target->weatherId == weatherId))
			apply(config.entries, true, false, weatherUserChanged);
	for (auto& [_, config] : locationSceneConfigs)
		if ((!scope || *scope == SceneContextType::Location) &&
			(!target || (target->locationType == config.type && NormalizeLocationFormKey(target->locationFormKey) == NormalizeLocationFormKey(config.formKey))))
			apply(config.entries, false, true, locationUserChanged);
	if (!changed)
		return;

	if (numericChanged)
		++sceneValueRevision;
	if (locationChanged)
		locationOverridesDirty = true;
	BumpEntryPresentationRevision();
	if (source == EntrySource::User) {
		interiorUserSettingsModified |= interiorUserChanged;
		timeOfDayUserSettingsModified |= timeOfDayUserChanged;
		weatherUserSettingsModified |= weatherUserChanged;
		locationUserSettingsModified |= locationUserChanged;
		SaveAllUserSettings();
	}
	ReapplyIfActive();
}

void SceneSettingsManager::DeleteEntryLayer(EntrySource source, std::optional<SceneContextType> scope, const SceneContextId* target)
{
	if (target)
		scope = target->type;
	TryEnsureWeatherDataLoaded();
	TryEnsureLocationDataLoaded();
	if (target && source == EntrySource::User) {
		if (target->type == SceneContextType::Weather) {
			DeleteAllWeatherUserSettings(target->weatherId);
			return;
		}
		if (target->type == SceneContextType::Location) {
			DeleteAllLocationUserSettings(target->locationType, target->locationFormKey);
			return;
		}
	}

	if (source == EntrySource::Overwrite) {
		std::set<std::filesystem::path> backingFiles;
		for (const auto& [context, files] : overwriteBackingFiles) {
			if (scope && context.type != *scope)
				continue;
			if (target && ((context.type == SceneContextType::Weather && context.weatherId != target->weatherId) ||
							  (context.type == SceneContextType::Location && (context.locationType != target->locationType ||
																				 NormalizeLocationFormKey(context.locationFormKey) != NormalizeLocationFormKey(target->locationFormKey)))))
				continue;
			backingFiles.insert(files.begin(), files.end());
		}
		const auto collect = [&](const std::vector<SettingEntry>& sourceEntries,
								 const auto& pathForEntry) {
			for (const auto& entry : sourceEntries)
				if (entry.source == EntrySource::Overwrite) {
					auto path = pathForEntry(entry);
					if (!path.empty())
						backingFiles.insert(std::move(path));
				}
		};
		if (!scope || *scope == SceneContextType::Interior) {
			collect(GetEntries(SceneType::InteriorOnly), [&](const SettingEntry& entry) {
				return GetSceneOverwritePath(SceneType::InteriorOnly, entry);
			});
		}
		if (!scope || *scope == SceneContextType::TimeOfDay) {
			collect(GetEntries(SceneType::TimeOfDay), [&](const SettingEntry& entry) {
				return GetSceneOverwritePath(SceneType::TimeOfDay, entry);
			});
		}
		for (const auto& [weatherId, config] : weatherSceneConfigs)
			if ((!scope || *scope == SceneContextType::Weather) && (!target || target->weatherId == weatherId))
				collect(config.entries, [&](const SettingEntry& entry) {
					return GetWeatherOverwritePath(weatherId, entry);
				});
		for (const auto& [_, config] : locationSceneConfigs)
			if ((!scope || *scope == SceneContextType::Location) &&
				(!target || (target->locationType == config.type && NormalizeLocationFormKey(target->locationFormKey) == NormalizeLocationFormKey(config.formKey))))
				collect(config.entries, [&](const SettingEntry& entry) {
					return GetLocationOverwritePath(config.type, config.formKey, entry);
				});
		for (const auto& path : backingFiles) {
			if (!Util::PathHelpers::IsPathLexicallyWithinDirectory(Util::PathHelpers::GetSceneSettingsPath(), path)) {
				logger::error("[SceneSettings] Refusing to delete overwrite outside the scene settings directory: {}", path.string());
				continue;
			}
			std::error_code ec;
			const auto removed = std::filesystem::remove(path, ec);
			if (!removed && ec)
				logger::error("[SceneSettings] Failed to delete overwrite file '{}': {}",
					path.string(), ec.message());
		}
		ReloadOverwriteEntries();
		return;
	}

	bool numericChanged = false;
	bool presentationChanged = false;
	const auto eraseUser = [&](std::vector<SettingEntry>& sourceEntries, bool numeric) {
		const auto removed = std::erase_if(sourceEntries, [](const SettingEntry& entry) {
			return entry.source == EntrySource::User;
		});
		presentationChanged |= removed != 0;
		numericChanged |= numeric && removed != 0;
	};
	if (!scope || *scope == SceneContextType::Interior)
		eraseUser(GetEntriesMut(SceneType::InteriorOnly), false);
	if (!scope || *scope == SceneContextType::TimeOfDay)
		eraseUser(GetEntriesMut(SceneType::TimeOfDay), true);
	for (auto& [weatherId, config] : weatherSceneConfigs)
		if ((!scope || *scope == SceneContextType::Weather) && (!target || target->weatherId == weatherId))
			eraseUser(config.entries, true);
	for (auto& [_, config] : locationSceneConfigs)
		if ((!scope || *scope == SceneContextType::Location) &&
			(!target || (target->locationType == config.type && NormalizeLocationFormKey(target->locationFormKey) == NormalizeLocationFormKey(config.formKey))))
			eraseUser(config.entries, false);
	for (auto& [type, rawEntries] : unresolvedUserEntries)
		if (!scope || (*scope == SceneContextType::Interior && type == SceneType::InteriorOnly) ||
			(*scope == SceneContextType::TimeOfDay && type == SceneType::TimeOfDay))
			rawEntries.clear();
	if ((!scope || *scope == SceneContextType::Weather) && unresolvedWeatherUserSettings.is_object())
		for (auto& [_, rawWeather] : unresolvedWeatherUserSettings.items())
			if (rawWeather.is_object())
				rawWeather.erase("entries");
	if ((!scope || *scope == SceneContextType::Location) && unresolvedLocationUserSettings.is_object())
		for (const auto* sectionName : { "worldspaces", "regions", "locationTypes", "categories", "locations", "cells" }) {
			auto sectionIt = unresolvedLocationUserSettings.find(sectionName);
			if (sectionIt == unresolvedLocationUserSettings.end() || !sectionIt->is_object())
				continue;
			for (auto& [_, rawConfig] : sectionIt->items())
				if (rawConfig.is_object())
					rawConfig.erase("entries");
		}

	interiorUserSettingsModified |= !scope || *scope == SceneContextType::Interior;
	timeOfDayUserSettingsModified |= !scope || *scope == SceneContextType::TimeOfDay;
	weatherUserSettingsModified |= !scope || *scope == SceneContextType::Weather;
	locationUserSettingsModified |= !scope || *scope == SceneContextType::Location;
	locationOverridesDirty = true;
	if (numericChanged)
		++sceneValueRevision;
	if (presentationChanged)
		BumpEntryPresentationRevision();
	SaveAllUserSettings();
	ReapplyIfActive();
}

SceneSettingsManager::OverwriteExportResult SceneSettingsManager::ExportEntryLayerToOverwrites(
	const std::string& modName, EntrySource source, std::optional<SceneContextType> scope, const SceneContextId* target)
{
	OverwriteExportResult result;
	const auto safeModName = Util::FileHelpers::SanitizeFileName(modName);
	if (safeModName.empty())
		return result;
	if (target)
		scope = target->type;
	TryEnsureWeatherDataLoaded();
	TryEnsureLocationDataLoaded();
	const auto writeGroup = [&](const std::filesystem::path& allowedRoot, const std::filesystem::path& path,
								const std::string& featureShortName, const std::string& description,
								const std::vector<const SettingEntry*>& grouped, const json& metadata = json::object()) {
		if (WriteGroupedOverwriteFile(allowedRoot, path, featureShortName, description, grouped, metadata))
			++result.writtenFiles;
		else
			++result.failedFiles;
	};

	for (const auto type : { SceneType::InteriorOnly, SceneType::TimeOfDay }) {
		if (scope && !(*scope == SceneContextType::Interior && type == SceneType::InteriorOnly) &&
			!(*scope == SceneContextType::TimeOfDay && type == SceneType::TimeOfDay))
			continue;
		std::map<std::pair<std::filesystem::path, std::string>,
			std::vector<const SettingEntry*>>
			groups;
		for (const auto& entry : GetEntries(type)) {
			if (entry.source != source)
				continue;
			auto directory = GetOverwritesPath(type);
			if (type == SceneType::TimeOfDay && entry.period != TimeOfDayPeriod::Count)
				directory /= GetPeriodName(entry.period);
			groups[{ std::move(directory), entry.featureShortName }].push_back(&entry);
		}
		for (const auto& [group, grouped] : groups) {
			const auto& [directory, featureShortName] = group;
			writeGroup(GetOverwritesPath(type),
				directory / std::format("{}_{}.json", safeModName, featureShortName),
				featureShortName, GetSceneOverwriteTypeDescription(type, grouped.front()->period), grouped);
		}
	}
	for (const auto& [weatherId, config] : weatherSceneConfigs) {
		if ((scope && *scope != SceneContextType::Weather) || (target && target->weatherId != weatherId))
			continue;
		std::map<std::pair<std::filesystem::path, std::string>,
			std::vector<const SettingEntry*>>
			groups;
		for (const auto& entry : config.entries) {
			if (entry.source != source)
				continue;
			auto directory = GetWeatherOverwritesDir() / Util::FormIdToSpid(weatherId);
			if (entry.period != TimeOfDayPeriod::Count)
				directory /= GetPeriodName(entry.period);
			groups[{ std::move(directory), entry.featureShortName }].push_back(&entry);
		}
		for (const auto& [group, grouped] : groups) {
			const auto& [directory, featureShortName] = group;
			writeGroup(GetWeatherOverwritesDir(),
				directory / std::format("{}_{}.json", safeModName, featureShortName),
				featureShortName, GetWeatherOverwriteTypeDescription(grouped.front()->period), grouped,
				{ { kMetadataTimeOfDayEnabledKey, config.timeOfDayEnabled } });
		}
	}
	for (const auto& [_, config] : locationSceneConfigs) {
		if ((scope && *scope != SceneContextType::Location) ||
			(target && (target->locationType != config.type || NormalizeLocationFormKey(target->locationFormKey) != NormalizeLocationFormKey(config.formKey))))
			continue;
		std::map<std::pair<TimeOfDayPeriod, std::string>, std::vector<const SettingEntry*>> groups;
		for (const auto& entry : config.entries)
			if (entry.source == source)
				groups[{ entry.period, entry.featureShortName }].push_back(&entry);
		const auto description = GetLocationTargetTypeName(config.type);
		const json metadata = {
			{ "targetType", description },
			{ "targetName", config.name },
			{ "coc", config.cocCode },
			{ kMetadataTimeOfDayEnabledKey, config.timeOfDayEnabled },
		};
		for (const auto& [identity, grouped] : groups) {
			const auto& [period, featureShortName] = identity;
			auto directory = GetLocationOverwritesDir(config.type) / config.formKey;
			if (period != TimeOfDayPeriod::Count)
				directory /= GetPeriodName(period);
			writeGroup(GetLocationOverwritesDir(config.type),
				directory / std::format("{}_{}.json", safeModName, featureShortName),
				featureShortName, description, grouped, metadata);
		}
	}
	if (result.writtenFiles != 0)
		ReloadOverwriteEntries();
	return result;
}

void SceneSettingsManager::UpdateEntryValue(SceneType type, size_t index, const json& newValue, bool deferSave)
{
	const EntryValueUpdate update{ index, newValue };
	UpdateEntryValues(type, std::span{ &update, 1 }, deferSave);
}

void SceneSettingsManager::UpdateEntryValues(
	SceneType type, std::span<const EntryValueUpdate> updates, bool deferSave)
{
	if (!IsEntryListSceneType(type))
		return;
	auto& vec = GetEntriesMut(type);
	const bool requireNumeric = type == SceneType::TimeOfDay;
	bool userEntriesChanged = false;
	if (!ApplyEntryValueUpdates(
			GetSceneTypeName(type), type, vec, updates, requireNumeric, userEntriesChanged))
		return;
	if (type == SceneType::TimeOfDay)
		++sceneValueRevision;

	if (userEntriesChanged) {
		MarkEntryListUserSettingsModified(type);
		if (deferSave)
			MarkDeferredSceneChanges();
		else
			SaveAllUserSettings();
	}
	ReapplyIfActive(false);
}

void SceneSettingsManager::CommitSceneSettingChanges()
{
	SaveAllUserSettings();
	ReapplyIfActive();
}

void SceneSettingsManager::MarkDeferredSceneChanges()
{
	deferredSceneChangesPending = true;
	deferredSceneChangesDeadline = std::chrono::steady_clock::now() + kDeferredSaveDelay;
}

void SceneSettingsManager::FlushDeferredSceneChanges()
{
	if (!deferredSceneChangesPending || std::chrono::steady_clock::now() < deferredSceneChangesDeadline)
		return;

	SaveAllUserSettings();
	ReapplyIfActive();
}

// --- Event Handler ---

RE::BSEventNotifyControl SceneSettingsManager::MenuOpenCloseEventHandler::ProcessEvent(
	const RE::MenuOpenCloseEvent* a_event,
	RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
{
	if (a_event && a_event->menuName == RE::LoadingMenu::MENU_NAME && !a_event->opening) {
		GetSingleton()->queuedLoadingTransition.store(true, std::memory_order_relaxed);
	}

	return RE::BSEventNotifyControl::kContinue;
}

// --- Scene Application ---

void SceneSettingsManager::Update()
{
	if (globals::state) {
		const auto frame = globals::state->frameCount;
		if (lastUpdateFrame == frame)
			return;
		lastUpdateFrame = frame;
	}
	VerifyPendingApplies(true);
	FlushDeferredSceneChanges();

	if (queuedLoadingTransition.exchange(false, std::memory_order_relaxed))
		OnLoadingTransition();
	else
		ResolveAndApply();
	VerifyPendingApplies();
}

bool SceneSettingsManager::IsSceneReady() const
{
	return globals::state && !globals::state->isMainMenuOpen && !globals::state->isLoadingMenuOpen &&
	       globals::game::player && globals::game::player->GetParentCell();
}

void SceneSettingsManager::OnLoadingTransition()
{
	suppressLocationTransitionUntilContextResolved = true;
	cachedPreviousWeatherId = 0;
	locationTargetsCached = false;
	resolverDirty = true;
	ResolveAndApply(true, false);
}

void SceneSettingsManager::ReapplyIfActive(bool activeSetMayHaveChanged)
{
	if (activeSetMayHaveChanged)
		activeEntryCacheDirty = true;
	resolverDirty = true;
	if (!resolverSuspended)
		ResolveAndApply(true);
}

bool SceneSettingsManager::HasActiveSettingsForFeature(const std::string& featureShortName) const
{
	return appliedFeatureNames.contains(featureShortName);
}

bool SceneSettingsManager::HasCurrentSceneSettingsForFeature(const std::string& featureShortName) const
{
	if (HasActiveSettingsForFeature(featureShortName))
		return true;
	if (!IsFeaturePaused(featureShortName))
		return false;
	const auto hasResumableEntry = [&](const auto& sourceEntries, SceneType type, const PeriodicSceneConfig* config = nullptr) {
		return std::ranges::any_of(sourceEntries, [&](const auto& entry) {
			if (entry.featureShortName != featureShortName || entry.paused ||
				(config && !config->IsPeriodActive(entry.period)) ||
				!IsSettingAllowedForType(type, entry.featureShortName, entry.settingPath, entry.settingKey))
				return false;
			if (type == SceneType::TimeOfDay && (!IsNumericValue(entry.value) ||
													(!config && (entry.period < TimeOfDayPeriod::Dawn || entry.period >= TimeOfDayPeriod::Count))))
				return false;
			return !IsNumericValue(entry.value) || std::isfinite(entry.value.template get<double>());
		});
	};
	if (Util::IsInterior()) {
		if (hasResumableEntry(GetEntries(SceneType::InteriorOnly), SceneType::InteriorOnly))
			return true;
	} else {
		if (hasResumableEntry(GetEntries(SceneType::TimeOfDay), SceneType::TimeOfDay))
			return true;
		if (const auto* sky = globals::game::sky) {
			const auto weatherLerp = std::isfinite(sky->currentWeatherPct) ? std::clamp(sky->currentWeatherPct, 0.0f, 1.0f) : 0.0f;
			for (const auto weatherId : { sky->currentWeather ? sky->currentWeather->GetFormID() : 0, GetEffectivePreviousWeatherId(sky, weatherLerp) }) {
				const auto config = weatherSceneConfigs.find(weatherId);
				if (config != weatherSceneConfigs.end() && hasResumableEntry(config->second.entries, SceneType::TimeOfDay, &config->second))
					return true;
			}
		}
	}
	for (const auto& target : GetCurrentLocationTargets()) {
		const auto config = locationSceneConfigs.find(GetLocationConfigKey(target.type, target.formKey));
		if (config != locationSceneConfigs.end() && hasResumableEntry(config->second.entries, SceneType::Location, &config->second))
			return true;
	}
	return false;
}

bool SceneSettingsManager::IsActiveSceneSetting(std::string_view featureShortName,
	std::string_view settingPath, std::string_view settingKey) const
{
	return IsActiveSceneSetting(std::string(featureShortName), SplitCatalogPath(settingPath), std::string(settingKey));
}

bool SceneSettingsManager::IsActiveSceneSetting(const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey) const
{
	return appliedSettings.contains({ featureShortName, settingPath, settingKey });
}

void SceneSettingsManager::CaptureExternalFeatureChanges(Feature* feature)
{
	if (!feature)
		return;
	if (appliedSettings.empty()) {
		InvalidateFeatureSnapshot(feature->GetShortName());
		return;
	}

	json featureSettings;
	try {
		feature->SaveSettings(featureSettings);
	} catch (const std::exception& e) {
		logger::warn("[SceneSettings] Could not inspect external changes for {}: {}",
			feature->GetShortName(), e.what());
		return;
	} catch (...) {
		logger::warn("[SceneSettings] Could not inspect external changes for {}",
			feature->GetShortName());
		return;
	}
	if (!featureSettings.is_object())
		return;

	std::vector<std::pair<SettingAddress, json>> changedSettings;
	const auto featureShortName = feature->GetShortName();
	InvalidateFeatureSnapshot(featureShortName);
	for (auto appliedIt = appliedSettings.lower_bound({ featureShortName, {}, {} });
		appliedIt != appliedSettings.end() && appliedIt->first.featureShortName == featureShortName;
		++appliedIt) {
		const auto& [address, appliedValue] = *appliedIt;
		auto* setting = FindAllowedCatalogSetting(
			address.featureShortName, address.settingPath, address.settingKey);
		if (!setting)
			continue;
		const auto* value = GetCatalogSerializedValue(featureSettings, *setting);
		if (!value || !IsSceneSettingPrimitive(*value) ||
			!IsCompatibleSceneSettingValue(appliedValue, *value) || AppliedValuesEqual(*value, appliedValue))
			continue;
		changedSettings.emplace_back(address, *value);
	}

	if (changedSettings.empty())
		return;
	for (const auto& [address, value] : changedSettings) {
		baselineSettings[address] = value;
		appliedSettings[address] = value;
	}
	resolverDirty = true;
	if (!resolverSuspended)
		ResolveAndApply(true);
}

void SceneSettingsManager::RestoreBaselinesInSerializedSettings(json& settings) const
{
	for (const auto& [address, baseline] : baselineSettings) {
		auto* feature = Feature::FindFeatureByShortName(address.featureShortName);
		if (!feature)
			continue;
		auto featureIt = settings.find(feature->GetName());
		if (featureIt == settings.end() || !featureIt->is_object())
			continue;
		auto* setting = FindAllowedCatalogSetting(
			address.featureShortName, address.settingPath, address.settingKey);
		auto* value = setting ? GetCatalogSerializedValue(*featureIt, *setting) : nullptr;
		if (value && IsCompatibleSceneSettingValue(*value, baseline))
			*value = baseline;
	}
}

SceneSettingsManager::SceneLayerGuard::SceneLayerGuard(SceneSettingsManager& manager) :
	manager(manager)
{
	manager.SuspendSceneLayer();
}

SceneSettingsManager::SceneLayerGuard::~SceneLayerGuard()
{
	manager.ResumeSceneLayer();
}

bool SceneSettingsManager::IsFeaturePaused(const std::string& featureShortName) const
{
	auto it = featurePauseStates.find(featureShortName);
	return it != featurePauseStates.end() && it->second;
}

void SceneSettingsManager::SetFeaturePaused(const std::string& featureShortName, bool paused)
{
	featurePauseStates[featureShortName] = paused;
	++sceneValueRevision;
	BumpEntryPresentationRevision();
	locationOverridesDirty = true;
	ReapplyIfActive();
}

void SceneSettingsManager::SuspendSceneLayer()
{
	if (++sceneLayerSuspendDepth > 1)
		return;

	resolverSuspended = true;
	RestoreAppliedSettings();
}

void SceneSettingsManager::ResumeSceneLayer()
{
	if (sceneLayerSuspendDepth <= 0) {
		logger::warn("[SceneSettings] ResumeSceneLayer called without a matching suspend");
		sceneLayerSuspendDepth = 0;
		return;
	}
	if (--sceneLayerSuspendDepth > 0)
		return;

	InvalidateFeatureSnapshot();
	resolverSuspended = false;
	resolverDirty = true;
	ResolveAndApply(true);
}

void SceneSettingsManager::ResolveAndApply(bool force, bool allowLocationTransitions)
{
	if (resolverSuspended || sceneLayerSuspendDepth > 0)
		return;
	if (!locationDataLoaded)
		TryEnsureLocationDataLoaded();
	if (!weatherDataLoaded)
		TryEnsureWeatherDataLoaded();

	if (globals::state && (globals::state->isMainMenuOpen || globals::state->isLoadingMenuOpen)) {
		RestoreAppliedSettings();
		resolverDirty = true;
		return;
	}

	auto* player = globals::game::player;
	auto* cell = player ? player->GetParentCell() : nullptr;
	if (!player || !cell) {
		RestoreAppliedSettings();
		resolverDirty = true;
		return;
	}

	const bool interior = Util::IsInterior();
	const auto hour = GetCurrentGameHour();
	auto* location = player->GetCurrentLocation();
	if (!location)
		location = cell->GetLocation();
	const auto locationId = location ? location->GetFormID() : 0;
	const auto cellId = cell->GetFormID();
	const auto* worldspace = player->GetWorldspace();
	const auto worldspaceId = worldspace ? worldspace->GetFormID() : 0;
	RE::FormID regionId = 0;
	for (const auto& target : GetCurrentLocationTargets())
		if (target.type == LocationTargetType::Region) {
			regionId = target.formId;
			break;
		}
	const bool cellChanged = cellId != lastResolvedCellId;
	const bool locationContextChanged = locationId != lastResolvedLocationId || cellChanged ||
	                                    regionId != lastResolvedRegionId || worldspaceId != lastResolvedWorldspaceId;
	const bool walkedBetweenWorldspaceCells = allowLocationTransitions &&
	                                          !suppressLocationTransitionUntilContextResolved && cellChanged &&
	                                          lastResolvedCellId != 0 && !interior && !lastResolvedInterior &&
	                                          worldspaceId != 0 && worldspaceId == lastResolvedWorldspaceId;

	RE::FormID currentWeatherId = 0;
	RE::FormID previousWeatherId = 0;
	float weatherLerp = 0.0f;
	if (!interior) {
		TryEnsureWeatherDataLoaded();
		if (auto* sky = globals::game::sky) {
			currentWeatherId = sky->currentWeather ? sky->currentWeather->GetFormID() : 0;
			weatherLerp = std::isfinite(sky->currentWeatherPct) ? std::clamp(sky->currentWeatherPct, 0.0f, 1.0f) : 0.0f;
			previousWeatherId = GetEffectivePreviousWeatherId(sky, weatherLerp);
		}
	}
	const auto commitContext = [&] {
		lastResolvedInterior = interior;
		lastResolvedLocationId = locationId;
		lastResolvedCellId = cellId;
		lastResolvedRegionId = regionId;
		lastResolvedWorldspaceId = worldspaceId;
		lastResolvedHour = hour;
		lastResolvedCurrentWeatherId = currentWeatherId;
		lastResolvedPreviousWeatherId = previousWeatherId;
		lastResolvedWeatherLerp = weatherLerp;
		suppressLocationTransitionUntilContextResolved = false;
	};

	if (!HasActiveSceneEntriesCached() && !featureSceneEdit) {
		applyFailures.clear();
		if (!appliedSettings.empty() || !baselineSettings.empty())
			RestoreAppliedSettings();
		else
			ClearLocationTransitions();
		resolverDirty = !appliedSettings.empty() || !baselineSettings.empty();
		commitContext();
		return;
	}

	const bool contextChanged = interior != lastResolvedInterior ||
	                            locationContextChanged ||
	                            currentWeatherId != lastResolvedCurrentWeatherId ||
	                            previousWeatherId != lastResolvedPreviousWeatherId ||
	                            std::abs(weatherLerp - lastResolvedWeatherLerp) >= kBlendEpsilon ||
	                            ((!interior || !cachedLocationUserPeriods.empty() || !cachedLocationOverwritePeriods.empty()) && (lastResolvedHour < 0.0f ||
																																	 std::abs(hour - lastResolvedHour) >= kHourUpdateThreshold));
	const auto now = std::chrono::steady_clock::now();
	const bool applyRetryDue = std::any_of(applyFailures.begin(), applyFailures.end(),
		[&](const auto& item) { return now >= item.second.retryAfter; });
	const auto transitionTime = GetPauseAwareTime();
	if (!force && !resolverDirty && !contextChanged && !applyRetryDue) {
		if (!activeLocationTransitions.empty())
			AdvanceLocationTransitions(transitionTime);
		return;
	}

	if (!allowLocationTransitions || suppressLocationTransitionUntilContextResolved)
		ClearLocationTransitions();
	resolverDirty = false;
	const bool reconcileLocationTransitions = locationContextChanged || locationOverridesDirty;
	auto& resolved = BuildResolvedSettings(reconcileLocationTransitions);
	if (reconcileLocationTransitions) {
		StartLocationTransitions(resolved, transitionTime, walkedBetweenWorldspaceCells);
		locationOverridesDirty = false;
	}
	if (IsFeatureSceneEditPreviewActive()) {
		const auto erased = std::erase_if(activeLocationTransitions, [&](const auto& item) {
			return std::ranges::binary_search(
				featureSceneEdit->editableAddresses, item.first);
		});
		if (erased != 0)
			locationTransitionBatchesDirty = true;
	}
	RefreshLocationTransitionEndpoints(resolved);
	if (!activeLocationTransitions.empty()) {
		for (const auto& [address, transition] : activeLocationTransitions) {
			const bool finished = transition.duration <= 0.0f ||
			                      transitionTime - transition.startTime >= transition.duration;
			if (finished && transition.restoreAtEnd) {
				resolved.erase(address);
				continue;
			}
			const auto linear = transition.duration > 0.0f ?
			                        std::clamp((transitionTime - transition.startTime) /
												   transition.duration,
										0.0f, 1.0f) :
			                        1.0f;
			const auto smooth = linear * linear * (3.0f - 2.0f * linear);
			resolved[address] = finished ? transition.targetValue :
			                               transition.startValue + (transition.targetValue - transition.startValue) * smooth;
		}
	}
	ApplyResolvedSettings(resolved, force);
	for (auto transitionIt = activeLocationTransitions.begin();
		transitionIt != activeLocationTransitions.end();) {
		const auto& [address, transition] = *transitionIt;
		const bool finished = transition.duration <= 0.0f ||
		                      transitionTime - transition.startTime >= transition.duration;
		if (!finished) {
			++transitionIt;
			continue;
		}
		bool applied = false;
		if (transition.restoreAtEnd) {
			if (auto verificationIt = pendingApplyVerifications.find(address.featureShortName);
				verificationIt != pendingApplyVerifications.end())
				applied = std::ranges::find(
							  verificationIt->second.restorationAddresses, address) !=
				          verificationIt->second.restorationAddresses.end();
		} else if (auto appliedIt = appliedSettings.find(address);
			appliedIt != appliedSettings.end() && IsNumericValue(appliedIt->second)) {
			applied = appliedIt->second.get<float>() == transition.targetValue;
		}
		if (!applied) {
			++transitionIt;
			continue;
		}
		transitionIt = activeLocationTransitions.erase(transitionIt);
		locationTransitionBatchesDirty = true;
	}

	commitContext();
}

float SceneSettingsManager::GetPauseAwareTime() const
{
	return globals::state ? globals::state->timer : 0.0f;
}

void SceneSettingsManager::StartLocationTransitions(
	const ResolvedSettingMap& resolved, float now, bool animateChanges)
{
	std::map<SettingAddress, LocationTransitionLayers> nextLayers;
	for (const auto& [address, values] : cachedLocationUserPeriods)
		for (int p = 0; p < kPeriodCount; ++p)
			if (values[p])
				nextLayers[address][p] = { 0.0f, 1.0f, *values[p], 0.0f, 0.0f };
	for (const auto& [address, values] : cachedLocationOverwritePeriods)
		for (int p = 0; p < kPeriodCount; ++p)
			if (values[p])
				nextLayers[address][p] = { 0.0f, 0.0f, 0.0f, 1.0f, *values[p] };
	if (!animateChanges && !activeLocationTransitions.empty()) {
		activeLocationTransitions.clear();
		locationTransitionBatches.clear();
		transitionApplyFailures.clear();
		locationTransitionBatchesDirty = false;
	}

	std::set<SettingAddress> changedAddresses;
	for (const auto& [address, _] : lastLocationLayers)
		changedAddresses.insert(address);
	for (const auto& [address, _] : nextLayers)
		changedAddresses.insert(address);
	for (const auto& [address, _] : activeLocationTransitions)
		changedAddresses.insert(address);

	for (const auto& address : changedAddresses) {
		const auto previous = lastLocationLayers.find(address);
		const auto next = nextLayers.find(address);
		const LocationTransitionLayers emptyLayers{};
		const auto& previousLayers = previous != lastLocationLayers.end() ? previous->second : emptyLayers;
		const auto& targetLayers = next != nextLayers.end() ? next->second : emptyLayers;
		const auto previousDuration = lastLocationTransitionDurations.find(address);
		const auto nextDuration = pendingLocationTransitionDurations.find(address);
		const float duration = nextDuration != pendingLocationTransitionDurations.end()  ? nextDuration->second :
		                       previousDuration != lastLocationTransitionDurations.end() ? previousDuration->second :
		                                                                                   kDefaultLocationTransitionSeconds;
		const bool durationChanged = previousDuration != lastLocationTransitionDurations.end() &&
		                             nextDuration != pendingLocationTransitionDurations.end() &&
		                             std::abs(previousDuration->second - nextDuration->second) >= kBlendEpsilon;
		if (!animateChanges || (previousLayers == targetLayers &&
								   !(durationChanged && activeLocationTransitions.contains(address))))
			continue;
		if (duration <= 0.0f) {
			if (activeLocationTransitions.erase(address) != 0)
				locationTransitionBatchesDirty = true;
			continue;
		}
		const auto baseline = baselineSettings.find(address);
		if (baseline == baselineSettings.end() || !IsNumericValue(baseline->second))
			continue;
		const auto resolvedValue = resolved.find(address);
		const auto& targetValue = resolvedValue != resolved.end() ? resolvedValue->second : baseline->second;
		if (!IsNumericValue(targetValue))
			continue;
		float startValue = baseline->second.get<float>();
		if (const auto applied = appliedSettings.find(address); applied != appliedSettings.end() && IsNumericValue(applied->second))
			startValue = applied->second.get<float>();
		auto startLayers = previousLayers;
		if (const auto active = activeLocationTransitions.find(address); active != activeLocationTransitions.end()) {
			const auto& transition = active->second;
			const float linear = transition.duration > 0.0f ?
			                         std::clamp((now - transition.startTime) / transition.duration, 0.0f, 1.0f) :
			                         1.0f;
			const float progress = linear * linear * (3.0f - 2.0f * linear);
			const auto blend = [progress](float from, float to) { return from + (to - from) * progress; };
			for (int p = 0; p < kPeriodCount; ++p) {
				const auto& from = transition.startLayers[p];
				const auto& to = transition.targetLayers[p];
				startLayers[p] = {
					blend(from.noLocationWeight, to.noLocationWeight),
					blend(from.userWeight, to.userWeight),
					blend(from.userWeightedValue, to.userWeightedValue),
					blend(from.overwriteWeight, to.overwriteWeight),
					blend(from.overwriteWeightedValue, to.overwriteWeightedValue)
				};
			}
		}
		activeLocationTransitions.insert_or_assign(address, LocationTransition{
																.startValue = startValue,
																.targetValue = targetValue.get<float>(),
																.startTime = now,
																.duration = duration,
																.restoreAtEnd = next == nextLayers.end() && resolvedValue == resolved.end(),
																.exitsLocationLayer = next == nextLayers.end(),
																.startLayers = std::move(startLayers),
																.targetLayers = targetLayers });
		locationTransitionBatchesDirty = true;
	}
	lastLocationLayers = std::move(nextLayers);
	lastLocationTransitionDurations = pendingLocationTransitionDurations;
}

void SceneSettingsManager::RefreshLocationTransitionEndpoints(const ResolvedSettingMap& resolved)
{
	if (activeLocationTransitions.empty())
		return;
	std::vector<SettingAddress> addresses;
	addresses.reserve(activeLocationTransitions.size());
	for (const auto& [address, _] : activeLocationTransitions)
		addresses.push_back(address);
	std::array<float, kPeriodCount> factors{};
	GetTimeOfDayFactors(factors.data());
	ResolvedSettingMap liveStartValues;
	ResolveExteriorSettings(liveStartValues, factors, nullptr, addresses, nullptr, nullptr, true);
	for (auto& [address, transition] : activeLocationTransitions) {
		const auto updateEndpoint = [&](float& endpoint, const ResolvedSettingMap& values) {
			const auto value = values.find(address);
			const auto baseline = baselineSettings.find(address);
			const json* next = value != values.end() ? &value->second : baseline != baselineSettings.end() ? &baseline->second :
			                                                                                                 nullptr;
			if (!next || !IsNumericValue(*next))
				return;
			const float numeric = next->get<float>();
			if (std::isfinite(numeric) && numeric != endpoint) {
				endpoint = numeric;
				locationTransitionBatchesDirty = true;
			}
		};
		updateEndpoint(transition.startValue, liveStartValues);
		updateEndpoint(transition.targetValue, resolved);
		if (transition.exitsLocationLayer) {
			const bool restoreAtEnd = !resolved.contains(address);
			if (transition.restoreAtEnd != restoreAtEnd) {
				transition.restoreAtEnd = restoreAtEnd;
				locationTransitionBatchesDirty = true;
			}
		}
	}
}

bool SceneSettingsManager::AdvanceLocationTransitions(float now)
{
	if (activeLocationTransitions.empty())
		return false;
	const auto retryNow = std::chrono::steady_clock::now();
	const bool retryDue = std::any_of(transitionApplyFailures.begin(), transitionApplyFailures.end(),
		[&](const auto& item) { return retryNow >= item.second.retryAfter; });
	if (!locationTransitionBatchesDirty && lastLocationTransitionApplyTime &&
		std::abs(now - *lastLocationTransitionApplyTime) < kBlendEpsilon && !retryDue)
		return false;
	if (locationTransitionBatchesDirty)
		RebuildLocationTransitionBatches();

	bool appliedAny = false;
	for (auto& [featureShortName, batch] : locationTransitionBatches) {
		for (size_t index = 0; index < batch.transitions.size(); ++index) {
			auto& transition = *batch.transitions[index];
			const auto linear = transition.duration > 0.0f ?
			                        std::clamp((now - transition.startTime) / transition.duration, 0.0f, 1.0f) :
			                        1.0f;
			const auto smooth = linear * linear * (3.0f - 2.0f * linear);
			batch.updates[index].value = linear >= 1.0f ? transition.targetValue :
			                                              transition.startValue + (transition.targetValue - transition.startValue) * smooth;
		}

		auto transitionFailureIt = transitionApplyFailures.find(featureShortName);
		if (transitionFailureIt != transitionApplyFailures.end() &&
			transitionFailureIt->second.signature != batch.signature) {
			transitionApplyFailures.erase(transitionFailureIt);
			transitionFailureIt = transitionApplyFailures.end();
		}
		if (transitionFailureIt != transitionApplyFailures.end() &&
			retryNow < transitionFailureIt->second.retryAfter)
			continue;

		auto* feature = Feature::FindFeatureByShortName(featureShortName);
		if (!feature) {
			auto& failure = transitionApplyFailures[featureShortName];
			failure.signature = batch.signature;
			if (!failure.warningLogged) {
				logger::warn("[SceneSettings] Cannot apply location transition, feature {} is not loaded",
					featureShortName);
				failure.warningLogged = true;
			}
			failure.retryAfter = retryNow + kApplyRetryDelay;
			continue;
		}
		if (!ApplyCatalogSceneSettings(*feature, batch.updates)) {
			auto& failure = transitionApplyFailures[featureShortName];
			failure.signature = batch.signature;
			if (!failure.warningLogged) {
				logger::warn("[SceneSettings] Failed to apply location transition for {}", featureShortName);
				failure.warningLogged = true;
			}
			failure.retryAfter = retryNow + kApplyRetryDelay;
			continue;
		}
		transitionApplyFailures.erase(featureShortName);
		std::vector<SettingAddress> restorationAddresses;
		for (size_t index = 0; index < batch.transitions.size(); ++index) {
			const auto& address = batch.addresses[index];
			const auto& transition = *batch.transitions[index];
			const bool finished = transition.duration <= 0.0f ||
			                      now - transition.startTime >= transition.duration;
			if (finished && transition.restoreAtEnd)
				restorationAddresses.push_back(address);
			appliedSettings[address] = batch.updates[index].value;
			if (finished) {
				activeLocationTransitions.erase(address);
				locationTransitionBatchesDirty = true;
			}
		}
		ScheduleApplyVerification(
			featureShortName, batch.updates, batch.signature, true, restorationAddresses);
		appliedFeatureNames.insert(featureShortName);
		appliedAny = true;
	}
	if (activeLocationTransitions.empty()) {
		locationTransitionBatches.clear();
		transitionApplyFailures.clear();
		locationTransitionBatchesDirty = false;
	}
	if (appliedAny)
		lastLocationTransitionApplyTime = now;
	return appliedAny;
}

void SceneSettingsManager::RebuildLocationTransitionBatches()
{
	locationTransitionBatches.clear();
	for (auto& [address, transition] : activeLocationTransitions) {
		auto& batch = locationTransitionBatches[address.featureShortName];
		if (batch.addresses.empty())
			batch.signature = std::hash<std::string_view>{}(address.featureShortName);
		batch.addresses.push_back(address);
		batch.transitions.push_back(&transition);
		batch.updates.push_back({ address.settingPath, address.settingKey, transition.startValue });
		for (const auto& segment : address.settingPath)
			CombineHash(batch.signature, std::hash<std::string_view>{}(segment));
		CombineHash(batch.signature, std::hash<std::string_view>{}(address.settingKey));
		HashSceneSettingValue(batch.signature, transition.startValue);
		HashSceneSettingValue(batch.signature, transition.targetValue);
		HashSceneSettingValue(batch.signature, transition.duration);
		CombineHash(batch.signature, static_cast<size_t>(transition.restoreAtEnd));
		CombineHash(batch.signature, static_cast<size_t>(transition.exitsLocationLayer));
	}
	std::erase_if(transitionApplyFailures,
		[&](const auto& item) { return !locationTransitionBatches.contains(item.first); });
	locationTransitionBatchesDirty = false;
}

void SceneSettingsManager::ClearLocationTransitions()
{
	activeLocationTransitions.clear();
	locationTransitionBatches.clear();
	transitionApplyFailures.clear();
	lastLocationTransitionApplyTime.reset();
	lastLocationLayers.clear();
	lastLocationTransitionDurations.clear();
	pendingLocationTransitionDurations.clear();
	cachedLocationUserSettings.clear();
	cachedLocationOverwriteSettings.clear();
	cachedLocationUserPeriods.clear();
	cachedLocationOverwritePeriods.clear();
	cachedLocationOverridesValid = false;
	locationTransitionBatchesDirty = false;
	locationOverridesDirty = true;
}

bool SceneSettingsManager::HasActiveSceneEntriesCached()
{
	if (!activeEntryCacheDirty)
		return hasActiveSceneEntries;

	const auto hasActiveEntry = [&](const auto& sourceEntries, SceneType type, const PeriodicSceneConfig* config = nullptr) {
		const bool floatsOnly = type == SceneType::TimeOfDay;
		return std::any_of(sourceEntries.begin(), sourceEntries.end(), [&](const auto& entry) {
			return (!config || config->IsPeriodActive(entry.period)) && IsEntryActive(entry) &&
			       IsSettingAllowedForType(
					   type, entry.featureShortName, entry.settingPath, entry.settingKey) &&
			       (!floatsOnly || IsNumericValue(entry.value));
		});
	};

	hasActiveSceneEntries = false;
	for (const auto& [type, sourceEntries] : entries) {
		if (hasActiveEntry(sourceEntries, type)) {
			hasActiveSceneEntries = true;
			break;
		}
	}
	if (!hasActiveSceneEntries)
		for (const auto& [_, config] : weatherSceneConfigs)
			if (hasActiveEntry(config.entries, SceneType::TimeOfDay, &config)) {
				hasActiveSceneEntries = true;
				break;
			}
	if (!hasActiveSceneEntries)
		for (const auto& [_, config] : locationSceneConfigs)
			if (hasActiveEntry(config.entries, SceneType::Location, &config)) {
				hasActiveSceneEntries = true;
				break;
			}

	activeEntryCacheDirty = false;
	return hasActiveSceneEntries;
}

SceneSettingsManager::ResolvedSettingMap& SceneSettingsManager::BuildResolvedSettings(
	bool collectLocationTransitionDurations)
{
	auto& resolved = resolvedSettingsScratch;
	resolved.clear();
	const bool interior = Util::IsInterior();
	std::vector<SettingAddress> requiredBaselines;
	const PeriodSettingMap* userTimeOfDayValues = nullptr;
	const PeriodSettingMap* overwriteTimeOfDayValues = nullptr;
	const auto collectBaselines = [&](const std::vector<SettingEntry>& sourceEntries, SceneType type, const PeriodicSceneConfig* config = nullptr) {
		const bool floatsOnly = type == SceneType::TimeOfDay;
		for (const auto& entry : sourceEntries) {
			if ((config && !config->IsPeriodActive(entry.period)) || !IsEntryActive(entry) || (floatsOnly && !IsNumericValue(entry.value)) ||
				!IsSettingAllowedForType(
					type, entry.featureShortName, entry.settingPath, entry.settingKey))
				continue;
			SettingAddress address{ entry.featureShortName, entry.settingPath, entry.settingKey };
			if (!baselineSettings.contains(address))
				requiredBaselines.push_back(std::move(address));
		}
	};
	const auto collectGroupedBaselines = [&](const PeriodSettingMap& values) {
		for (const auto& [address, _] : values)
			if (!baselineSettings.contains(address))
				requiredBaselines.push_back(address);
	};

	if (interior) {
		collectBaselines(GetEntries(SceneType::InteriorOnly), SceneType::InteriorOnly);
	} else {
		userTimeOfDayValues = &BuildTimeOfDayValueGroups(EntrySource::User);
		overwriteTimeOfDayValues = &BuildTimeOfDayValueGroups(EntrySource::Overwrite);
		collectGroupedBaselines(*userTimeOfDayValues);
		collectGroupedBaselines(*overwriteTimeOfDayValues);
		if (auto* sky = globals::game::sky) {
			const auto weatherLerp = std::isfinite(sky->currentWeatherPct) ?
			                             std::clamp(sky->currentWeatherPct, 0.0f, 1.0f) :
			                             0.0f;
			const auto previousWeatherId = GetEffectivePreviousWeatherId(sky, weatherLerp);
			for (auto weatherId : { sky->currentWeather ? sky->currentWeather->GetFormID() : 0,
					 previousWeatherId })
				collectGroupedBaselines(BuildWeatherValueGroups(weatherId));
		}
	}

	const auto& locationTargets = GetCurrentLocationTargets();
	const bool rebuildLocationOverrides =
		collectLocationTransitionDurations || !cachedLocationOverridesValid;
	if (rebuildLocationOverrides) {
		for (const auto& target : locationTargets) {
			auto it = locationSceneConfigs.find(GetLocationConfigKey(target.type, target.formKey));
			if (it != locationSceneConfigs.end())
				collectBaselines(it->second.entries, SceneType::Location, &it->second);
		}
	}
	std::sort(requiredBaselines.begin(), requiredBaselines.end());
	requiredBaselines.erase(std::unique(requiredBaselines.begin(), requiredBaselines.end()), requiredBaselines.end());
	EnsureBaselines(requiredBaselines);

	if (rebuildLocationOverrides) {
		pendingLocationTransitionDurations.clear();
		cachedLocationUserSettings.clear();
		cachedLocationOverwriteSettings.clear();
		ResolveLocationSettings(
			cachedLocationUserSettings, locationTargets, EntrySource::User, true, &cachedLocationUserPeriods);
		ResolveLocationSettings(
			cachedLocationOverwriteSettings, locationTargets, EntrySource::Overwrite, true, &cachedLocationOverwritePeriods);
		cachedLocationOverridesValid = true;
	}

	std::array<float, kPeriodCount> factors{};
	GetTimeOfDayFactors(factors.data());
	if (interior) {
		ResolveInteriorSettings(resolved, EntrySource::User);
		for (const auto& [address, value] : cachedLocationUserSettings)
			if (!IsNumericValue(value))
				resolved[address] = value;
		ResolvedSettingMap interiorOverwrites;
		ResolveInteriorSettings(interiorOverwrites, EntrySource::Overwrite);
		std::set<SettingAddress> addresses;
		for (const auto& [address, _] : cachedLocationUserPeriods)
			addresses.insert(address);
		for (const auto& [address, _] : cachedLocationOverwritePeriods)
			addresses.insert(address);
		for (const auto& address : addresses) {
			const auto baseline = baselineSettings.find(address);
			if (baseline == baselineSettings.end() || !IsNumericValue(baseline->second))
				continue;
			const auto lower = resolved.find(address);
			const float lowerValue = lower != resolved.end() ? lower->second.get<float>() : baseline->second.get<float>();
			const auto user = cachedLocationUserPeriods.find(address);
			const auto overwrite = cachedLocationOverwritePeriods.find(address);
			const auto interiorOverride = interiorOverwrites.find(address);
			float value = 0.0f;
			for (int index = 0; index < kPeriodCount; ++index) {
				float periodValue = user != cachedLocationUserPeriods.end() ? user->second[index].value_or(lowerValue) : lowerValue;
				if (interiorOverride != interiorOverwrites.end())
					periodValue = interiorOverride->second.get<float>();
				if (overwrite != cachedLocationOverwritePeriods.end())
					periodValue = overwrite->second[index].value_or(periodValue);
				value += factors[index] * periodValue;
			}
			resolved[address] = value;
		}
		for (const auto& [address, value] : interiorOverwrites)
			if (!addresses.contains(address))
				resolved[address] = value;
	} else {
		ResolveExteriorSettings(resolved, factors, &cachedLocationUserSettings, {},
			&cachedLocationUserPeriods, &cachedLocationOverwritePeriods);
	}
	for (const auto& [address, value] : cachedLocationOverwriteSettings)
		if (!IsNumericValue(value))
			resolved[address] = value;
	ApplyFeatureSceneEditPreview(resolved);
	return resolved;
}

void SceneSettingsManager::ApplyResolvedSettings(const ResolvedSettingMap& resolved, bool forceRetry)
{
	struct PendingUpdate
	{
		const SettingAddress* address = nullptr;
		const json* value = nullptr;
		bool restore = false;
	};

	std::map<std::string, std::vector<PendingUpdate>> pendingByFeature;
	for (const auto& [address, baseline] : baselineSettings) {
		if (resolved.contains(address))
			continue;
		pendingByFeature[address.featureShortName].push_back({ &address, &baseline, true });
	}

	for (const auto& [address, value] : resolved) {
		auto appliedIt = appliedSettings.find(address);
		if (appliedIt != appliedSettings.end() && ResolvedValuesEqual(appliedIt->second, value))
			continue;
		pendingByFeature[address.featureShortName].push_back({ &address, &value, false });
	}
	std::erase_if(applyFailures, [&](const auto& item) { return !pendingByFeature.contains(item.first); });

	for (const auto& [featureShortName, pending] : pendingByFeature) {
		std::vector<CatalogSceneSettingUpdate> updates;
		updates.reserve(pending.size());
		for (const auto& update : pending)
			updates.push_back({ update.address->settingPath, update.address->settingKey, *update.value });
		std::optional<size_t> signature;
		const auto getSignature = [&]() {
			if (!signature) {
				signature = GetCatalogUpdateSignature(featureShortName, updates);
				for (const auto& update : pending)
					CombineHash(*signature, static_cast<size_t>(update.restore));
			}
			return *signature;
		};
		auto failureIt = applyFailures.find(featureShortName);
		if (failureIt != applyFailures.end()) {
			if (failureIt->second.signature != getSignature()) {
				applyFailures.erase(failureIt);
				failureIt = applyFailures.end();
			}
		}
		const auto now = std::chrono::steady_clock::now();
		if (!forceRetry && failureIt != applyFailures.end() && now < failureIt->second.retryAfter)
			continue;

		auto* feature = Feature::FindFeatureByShortName(featureShortName);
		if (!feature) {
			auto& failure = applyFailures[featureShortName];
			failure.signature = getSignature();
			if (!failure.warningLogged) {
				logger::warn("[SceneSettings] Cannot apply resolved settings, feature {} is not loaded", featureShortName);
				failure.warningLogged = true;
			}
			failure.retryAfter = now + kApplyRetryDelay;
			continue;
		}

		if (!ApplyCatalogSceneSettings(*feature, updates)) {
			auto& failure = applyFailures[featureShortName];
			failure.signature = getSignature();
			if (!failure.warningLogged) {
				logger::warn("[SceneSettings] Failed to apply resolved settings for {}", featureShortName);
				failure.warningLogged = true;
			}
			failure.retryAfter = now + kApplyRetryDelay;
			continue;
		}
		applyFailures.erase(featureShortName);
		std::vector<SettingAddress> restorationAddresses;
		for (const auto& update : pending)
			if (update.restore)
				restorationAddresses.push_back(*update.address);
		ScheduleApplyVerification(
			featureShortName, updates, getSignature(), false, restorationAddresses);
		restoreFailureWarnings.erase(featureShortName);

		for (const auto& update : pending)
			appliedSettings[*update.address] = *update.value;
		appliedFeatureNames.insert(featureShortName);
		if (featureSceneEdit && featureSceneEdit->featureShortName == featureShortName)
			RebaseFeatureSceneEditPreview();
	}
}

void SceneSettingsManager::RestoreAppliedSettings()
{
	ClearLocationTransitions();
	struct PendingRestore
	{
		SettingAddress address;
		CatalogSceneSettingUpdate update;
	};

	std::map<std::string, std::vector<PendingRestore>> updatesByFeature;
	for (const auto& [address, baseline] : baselineSettings)
		updatesByFeature[address.featureShortName].push_back(
			{ address, { address.settingPath, address.settingKey, baseline } });

	for (const auto& [featureShortName, pending] : updatesByFeature) {
		const auto now = std::chrono::steady_clock::now();
		if (auto retryIt = restoreRetryAfter.find(featureShortName);
			retryIt != restoreRetryAfter.end() && now < retryIt->second)
			continue;
		auto* feature = Feature::FindFeatureByShortName(featureShortName);
		if (!feature) {
			if (restoreFailureWarnings.insert(featureShortName).second)
				logger::warn("[SceneSettings] Cannot restore {}, feature is not loaded", featureShortName);
			restoreRetryAfter[featureShortName] = now + kApplyRetryDelay;
			continue;
		}

		std::vector<CatalogSceneSettingUpdate> updates;
		updates.reserve(pending.size());
		for (const auto& item : pending)
			updates.push_back(item.update);
		pendingApplyVerifications.erase(featureShortName);
		if (!ApplyCatalogSceneSettings(*feature, updates)) {
			if (restoreFailureWarnings.insert(featureShortName).second)
				logger::warn("[SceneSettings] Failed to restore base settings for {}", featureShortName);
			restoreRetryAfter[featureShortName] = now + kApplyRetryDelay;
			continue;
		}
		bool verified = false;
		json actualSettings;
		try {
			feature->SaveSettings(actualSettings);
			if (actualSettings.is_object()) {
				verified = std::all_of(updates.begin(), updates.end(), [&](const auto& update) {
					auto* setting = FindAllowedCatalogSetting(
						featureShortName, update.settingPath, update.key);
					const auto* actual = setting ? GetCatalogSerializedValue(actualSettings, *setting) : nullptr;
					return actual && AppliedValuesEqual(*actual, update.value);
				});
			}
		} catch (...) {
			verified = false;
		}
		if (!verified) {
			if (restoreFailureWarnings.insert(featureShortName).second)
				logger::warn("[SceneSettings] {} did not retain restored base settings", featureShortName);
			featureApplyDocuments.erase(featureShortName);
			restoreRetryAfter[featureShortName] = now + kApplyRetryDelay;
			continue;
		}
		restoreFailureWarnings.erase(featureShortName);
		restoreRetryAfter.erase(featureShortName);

		for (const auto& item : pending) {
			appliedSettings.erase(item.address);
			baselineSettings.erase(item.address);
		}
		appliedFeatureNames.erase(featureShortName);
	}

	if (baselineSettings.empty()) {
		appliedSettings.clear();
		baselineSettings.clear();
		appliedFeatureNames.clear();
		restoreFailureWarnings.clear();
		restoreRetryAfter.clear();
	} else {
		resolverDirty = true;
	}
}

void SceneSettingsManager::ResolveInteriorSettings(
	ResolvedSettingMap& resolved, EntrySource source) const
{
	OverlayEntries(resolved, GetEntries(SceneType::InteriorOnly), SceneType::InteriorOnly, source);
}

const SceneSettingsManager::PeriodSettingMap& SceneSettingsManager::BuildTimeOfDayValueGroups(
	std::optional<EntrySource> source) const
{
	const auto cacheIndex = GetPeriodValueCacheIndex(source);
	auto& cached = timeOfDayValueGroups[cacheIndex];
	if (cached.revision == sceneValueRevision)
		return cached.values;
	auto& values = cached.values;
	values.clear();
	for (auto entrySource : { EntrySource::User, EntrySource::Overwrite }) {
		if (source && entrySource != *source)
			continue;
		for (const auto& entry : GetEntries(SceneType::TimeOfDay)) {
			const auto periodIndex = static_cast<int>(entry.period);
			if (entry.source != entrySource || !IsEntryActive(entry) || !IsNumericValue(entry.value) ||
				periodIndex < 0 || periodIndex >= kPeriodCount ||
				!IsSettingAllowedForType(SceneType::TimeOfDay,
					entry.featureShortName, entry.settingPath, entry.settingKey))
				continue;
			const auto value = entry.value.get<float>();
			if (std::isfinite(value))
				values[{ entry.featureShortName, entry.settingPath, entry.settingKey }][periodIndex] = value;
		}
	}
	cached.revision = sceneValueRevision;
	return values;
}

const SceneSettingsManager::PeriodSettingMap& SceneSettingsManager::BuildWeatherValueGroups(
	RE::FormID weatherId, std::optional<EntrySource> source) const
{
	const auto cacheIndex = GetPeriodValueCacheIndex(source);
	auto& cached = weatherValueGroups[weatherId][cacheIndex];
	if (cached.revision == sceneValueRevision)
		return cached.values;
	auto& values = cached.values;
	values.clear();
	auto configIt = weatherSceneConfigs.find(weatherId);
	if (configIt == weatherSceneConfigs.end()) {
		cached.revision = sceneValueRevision;
		return values;
	}
	for (auto entrySource : { EntrySource::User, EntrySource::Overwrite }) {
		if (source && entrySource != *source)
			continue;
		for (const auto& entry : configIt->second.entries) {
			const auto periodIndex = static_cast<int>(entry.period);
			if (entry.source != entrySource || !IsEntryActive(entry) || !IsNumericValue(entry.value) ||
				!configIt->second.IsPeriodActive(entry.period) ||
				!IsSettingAllowedForType(SceneType::TimeOfDay,
					entry.featureShortName, entry.settingPath, entry.settingKey))
				continue;
			const auto value = entry.value.get<float>();
			if (std::isfinite(value)) {
				auto& periods = values[{ entry.featureShortName, entry.settingPath, entry.settingKey }];
				if (entry.period == TimeOfDayPeriod::Count)
					periods.fill(value);
				else
					periods[periodIndex] = value;
			}
		}
	}
	cached.revision = sceneValueRevision;
	return values;
}

void SceneSettingsManager::ResolveExteriorSettings(ResolvedSettingMap& resolved,
	const std::array<float, kPeriodCount>& factors,
	const ResolvedSettingMap* userLocationValues,
	std::span<const SettingAddress> addressFilter,
	const PeriodSettingMap* userLocationPeriods, const PeriodSettingMap* overwriteLocationPeriods, bool transitionStart,
	bool applyOverwrites, std::set<SettingAddress>* appliedOverwrites,
	std::set<SettingAddress>* appliedSceneSettings) const
{
	const auto& userTimeOfDayValues = BuildTimeOfDayValueGroups(EntrySource::User);
	const auto& overwriteTimeOfDayValues = BuildTimeOfDayValueGroups(EntrySource::Overwrite);
	const PeriodSettingMap* currentUserWeather = nullptr;
	const PeriodSettingMap* previousUserWeather = nullptr;
	const PeriodSettingMap* currentOverwriteWeather = nullptr;
	const PeriodSettingMap* previousOverwriteWeather = nullptr;
	float weatherLerp = 0.0f;
	if (auto* sky = globals::game::sky; sky && sky->currentWeather) {
		weatherLerp = std::isfinite(sky->currentWeatherPct) ?
		                  std::clamp(sky->currentWeatherPct, 0.0f, 1.0f) :
		                  0.0f;
		const auto currentWeatherId = sky->currentWeather->GetFormID();
		const auto previousWeatherId = GetEffectivePreviousWeatherId(sky, weatherLerp);
		currentUserWeather = &BuildWeatherValueGroups(currentWeatherId, EntrySource::User);
		previousUserWeather = &BuildWeatherValueGroups(previousWeatherId, EntrySource::User);
		currentOverwriteWeather = &BuildWeatherValueGroups(currentWeatherId, EntrySource::Overwrite);
		previousOverwriteWeather = &BuildWeatherValueGroups(previousWeatherId, EntrySource::Overwrite);
	}

	std::vector<const SettingAddress*> addresses;
	if (!addressFilter.empty()) {
		addresses.reserve(addressFilter.size());
		for (const auto& address : addressFilter)
			addresses.push_back(&address);
	} else {
		const auto addressCount = userTimeOfDayValues.size() + overwriteTimeOfDayValues.size() +
		                          (currentUserWeather ? currentUserWeather->size() : 0) +
		                          (previousUserWeather ? previousUserWeather->size() : 0) +
		                          (currentOverwriteWeather ? currentOverwriteWeather->size() : 0) +
		                          (previousOverwriteWeather ? previousOverwriteWeather->size() : 0) +
		                          (userLocationValues ? userLocationValues->size() : 0);
		addresses.reserve(addressCount);
		const auto collectAddresses = [&](const PeriodSettingMap* values) {
			if (!values)
				return;
			for (const auto& [address, _] : *values)
				addresses.push_back(&address);
		};
		collectAddresses(&userTimeOfDayValues);
		collectAddresses(&overwriteTimeOfDayValues);
		collectAddresses(currentUserWeather);
		collectAddresses(previousUserWeather);
		collectAddresses(currentOverwriteWeather);
		collectAddresses(previousOverwriteWeather);
		collectAddresses(userLocationPeriods);
		collectAddresses(overwriteLocationPeriods);
		if (userLocationValues)
			for (const auto& [address, _] : *userLocationValues)
				addresses.push_back(&address);
		std::ranges::sort(addresses, [](const auto* lhs, const auto* rhs) { return *lhs < *rhs; });
		addresses.erase(std::unique(addresses.begin(), addresses.end(),
							[](const auto* lhs, const auto* rhs) { return *lhs == *rhs; }),
			addresses.end());
	}

	const auto getPeriodValue = [](const PeriodSettingMap* values, const SettingAddress& address,
									int periodIndex) -> std::optional<float> {
		if (!values)
			return std::nullopt;
		auto addressIt = values->find(address);
		if (addressIt == values->end())
			return std::nullopt;
		return addressIt->second[periodIndex];
	};

	for (const auto* addressPointer : addresses) {
		const auto& address = *addressPointer;
		auto baselineIt = baselineSettings.find(address);
		if (baselineIt == baselineSettings.end())
			continue;
		const json* locationValue = nullptr;
		if (userLocationValues) {
			auto locationIt = userLocationValues->find(address);
			if (locationIt != userLocationValues->end())
				locationValue = &locationIt->second;
		}
		if (locationValue && !IsNumericValue(*locationValue)) {
			resolved[address] = *locationValue;
			if (appliedSceneSettings)
				appliedSceneSettings->insert(address);
			continue;
		}
		if (!IsNumericValue(baselineIt->second))
			continue;
		const auto baseline = baselineIt->second.get<float>();
		const auto flatLocation = locationValue ? std::optional<float>(locationValue->get<float>()) : std::nullopt;
		const auto transition = transitionStart ? activeLocationTransitions.find(address) : activeLocationTransitions.end();
		float result = 0.0f;
		bool hasOverwrite = false;
		bool hasUserSetting = false;
		for (int periodIndex = 0; periodIndex < kPeriodCount; ++periodIndex) {
			if (factors[periodIndex] == 0.0f)
				continue;
			const auto* startLayer = transition != activeLocationTransitions.end() ? &transition->second.startLayers[periodIndex] : nullptr;
			const float nonOverwriteWeight = startLayer ? startLayer->noLocationWeight + startLayer->userWeight : 1.0f;
			const auto location = userLocationPeriods ? getPeriodValue(userLocationPeriods, address, periodIndex) : flatLocation;
			const auto timeOfDayValue = getPeriodValue(&userTimeOfDayValues, address, periodIndex);
			const auto previousWeatherValue = getPeriodValue(previousUserWeather, address, periodIndex);
			const auto currentWeatherValue = getPeriodValue(currentUserWeather, address, periodIndex);
			const auto timeOfDay = timeOfDayValue.value_or(baseline);
			float previous = location.value_or(previousWeatherValue.value_or(timeOfDay));
			float current = location.value_or(currentWeatherValue.value_or(timeOfDay));
			if (appliedSceneSettings)
				hasUserSetting |= location.has_value() || timeOfDayValue.has_value() ||
				                  (previousWeatherValue.has_value() && weatherLerp < 1.0f) ||
				                  (currentWeatherValue.has_value() && weatherLerp > 0.0f);
			if (startLayer) {
				previous = previous * startLayer->noLocationWeight + startLayer->userWeightedValue;
				current = current * startLayer->noLocationWeight + startLayer->userWeightedValue;
			}
			if (const auto overwrite = getPeriodValue(&overwriteTimeOfDayValues, address, periodIndex)) {
				hasOverwrite |= nonOverwriteWeight > 0.0f;
				if (applyOverwrites) {
					previous = *overwrite * nonOverwriteWeight;
					current = *overwrite * nonOverwriteWeight;
				}
			}
			if (const auto overwrite = getPeriodValue(previousOverwriteWeather, address, periodIndex)) {
				hasOverwrite |= weatherLerp < 1.0f && nonOverwriteWeight > 0.0f;
				if (applyOverwrites)
					previous = *overwrite * nonOverwriteWeight;
			}
			if (const auto overwrite = getPeriodValue(currentOverwriteWeather, address, periodIndex)) {
				hasOverwrite |= weatherLerp > 0.0f && nonOverwriteWeight > 0.0f;
				if (applyOverwrites)
					current = *overwrite * nonOverwriteWeight;
			}
			if (const auto overwrite = getPeriodValue(overwriteLocationPeriods, address, periodIndex)) {
				hasOverwrite = true;
				if (applyOverwrites) {
					previous = *overwrite;
					current = *overwrite;
				}
			}
			const float overwriteLocation = startLayer && applyOverwrites ? startLayer->overwriteWeightedValue : 0.0f;
			result += factors[periodIndex] * (previous + (current - previous) * weatherLerp + overwriteLocation);
		}
		if (std::isfinite(result))
			resolved[address] = result;
		if (appliedOverwrites && hasOverwrite)
			appliedOverwrites->insert(address);
		if (appliedSceneSettings && (hasUserSetting || (applyOverwrites && hasOverwrite)))
			appliedSceneSettings->insert(address);
	}
}

void SceneSettingsManager::ResolveTimeOfDaySettings(ResolvedSettingMap& resolved,
	const PeriodSettingMap& values, const std::array<float, kPeriodCount>& factors) const
{
	for (const auto& [address, periodValues] : values) {
		auto baselineIt = baselineSettings.find(address);
		if (baselineIt == baselineSettings.end() || !IsNumericValue(baselineIt->second))
			continue;
		const auto baseline = baselineIt->second.get<float>();
		float result = 0.0f;
		for (int periodIndex = 0; periodIndex < kPeriodCount; ++periodIndex)
			result += factors[periodIndex] * periodValues[periodIndex].value_or(baseline);
		resolved[address] = result;
	}
}

void SceneSettingsManager::ResolveWeatherSettings(ResolvedSettingMap& resolved,
	const PeriodSettingMap& timeOfDayValues, const std::array<float, kPeriodCount>& factors,
	std::optional<EntrySource> valueSource) const
{
	auto* sky = globals::game::sky;
	if (!sky || !sky->currentWeather)
		return;
	const auto weatherLerp = std::isfinite(sky->currentWeatherPct) ?
	                             std::clamp(sky->currentWeatherPct, 0.0f, 1.0f) :
	                             0.0f;
	const auto previousWeatherId = GetEffectivePreviousWeatherId(sky, weatherLerp);
	const auto currentWeatherId = sky->currentWeather->GetFormID();
	const auto& currentValues = BuildWeatherValueGroups(currentWeatherId, valueSource);
	const auto& previousValues = BuildWeatherValueGroups(previousWeatherId, valueSource);
	ResolveWeatherValueGroups(
		resolved, timeOfDayValues, factors, currentValues, previousValues, weatherLerp);
}

void SceneSettingsManager::ResolveWeatherValueGroups(ResolvedSettingMap& resolved,
	const PeriodSettingMap& timeOfDayValues, const std::array<float, kPeriodCount>& factors,
	const PeriodSettingMap& currentValues, const PeriodSettingMap& previousValues,
	float weatherLerp) const
{
	const auto resolveWeather = [&](const SettingAddress& address,
									const PeriodSettingMap& weatherValues) -> std::optional<float> {
		auto weatherIt = weatherValues.find(address);
		if (weatherIt == weatherValues.end())
			return std::nullopt;
		auto baselineIt = baselineSettings.find(address);
		if (baselineIt == baselineSettings.end() || !IsNumericValue(baselineIt->second))
			return std::nullopt;
		const auto baseline = baselineIt->second.get<float>();
		auto timeOfDayIt = timeOfDayValues.find(address);
		float result = 0.0f;
		for (int periodIndex = 0; periodIndex < kPeriodCount; ++periodIndex) {
			const auto lower = timeOfDayIt != timeOfDayValues.end() ?
			                       timeOfDayIt->second[periodIndex].value_or(baseline) :
			                       baseline;
			result += factors[periodIndex] * weatherIt->second[periodIndex].value_or(lower);
		}
		return result;
	};

	auto currentIt = currentValues.begin();
	auto previousIt = previousValues.begin();
	while (currentIt != currentValues.end() || previousIt != previousValues.end()) {
		const SettingAddress* address = nullptr;
		if (previousIt == previousValues.end() ||
			(currentIt != currentValues.end() && currentIt->first < previousIt->first)) {
			address = &currentIt->first;
			++currentIt;
		} else if (currentIt == currentValues.end() || previousIt->first < currentIt->first) {
			address = &previousIt->first;
			++previousIt;
		} else {
			address = &currentIt->first;
			++currentIt;
			++previousIt;
		}
		auto baselineIt = baselineSettings.find(*address);
		if (baselineIt == baselineSettings.end() || !IsNumericValue(baselineIt->second))
			continue;
		float lowerValue = baselineIt->second.get<float>();
		if (auto resolvedIt = resolved.find(*address);
			resolvedIt != resolved.end() && IsNumericValue(resolvedIt->second))
			lowerValue = resolvedIt->second.get<float>();
		const auto currentValue = resolveWeather(*address, currentValues);
		const auto previousValue = resolveWeather(*address, previousValues);
		if (!currentValue && !previousValue)
			continue;
		const auto from = previousValue.value_or(lowerValue);
		const auto to = currentValue.value_or(lowerValue);
		resolved[*address] = from + (to - from) * weatherLerp;
	}
}

void SceneSettingsManager::ResolveLocationSettings(
	ResolvedSettingMap& resolved, std::span<const LocationTarget> locationTargets,
	EntrySource source, bool collectTransitionDurations, PeriodSettingMap* periodValues,
	TimeOfDayPeriod period)
{
	PeriodSettingMap localPeriods;
	auto& values = periodValues ? *periodValues : localPeriods;
	values.clear();
	for (const auto& target : locationTargets) {
		auto config = locationSceneConfigs.find(GetLocationConfigKey(target.type, target.formKey));
		if (config == locationSceneConfigs.end())
			continue;
		for (const auto& entry : config->second.entries) {
			if (entry.source != source || !IsEntryActive(entry) ||
				!config->second.IsPeriodActive(entry.period) ||
				(period != TimeOfDayPeriod::Count && entry.period != TimeOfDayPeriod::Count && entry.period != period) ||
				!IsSettingAllowedForType(SceneType::Location, entry.featureShortName, entry.settingPath, entry.settingKey))
				continue;
			SettingAddress address{ entry.featureShortName, entry.settingPath, entry.settingKey };
			if (!baselineSettings.contains(address))
				continue;
			if (!IsNumericValue(entry.value)) {
				resolved[address] = entry.value;
				continue;
			}
			const auto index = static_cast<int>(entry.period);
			if (index >= 0 && index < kPeriodCount)
				values[address][index] = entry.value.get<float>();
			else if (entry.period == TimeOfDayPeriod::Count)
				values[address].fill(entry.value.get<float>());
			if (collectTransitionDurations)
				pendingLocationTransitionDurations[address] = entry.transitionSeconds.value_or(kDefaultLocationTransitionSeconds);
		}
	}
	std::array<float, kPeriodCount> factors{};
	if (period == TimeOfDayPeriod::Count)
		GetTimeOfDayFactors(factors.data());
	else
		factors[static_cast<size_t>(period)] = 1.0f;
	ResolveTimeOfDaySettings(resolved, values, factors);
}

void SceneSettingsManager::OverlayEntries(ResolvedSettingMap& resolved, const std::vector<SettingEntry>& sourceEntries,
	SceneType type, std::optional<EntrySource> source,
	std::map<SettingAddress, float>* transitionDurations) const
{
	for (const auto& entry : sourceEntries) {
		if (!IsEntryActive(entry) || (source && entry.source != *source) ||
			!IsSettingAllowedForType(type, entry.featureShortName, entry.settingPath, entry.settingKey))
			continue;
		SettingAddress address{ entry.featureShortName, entry.settingPath, entry.settingKey };
		if (!baselineSettings.contains(address))
			continue;
		resolved[address] = entry.value;
		if (transitionDurations && IsNumericValue(entry.value)) {
			const auto duration = entry.transitionSeconds.value_or(kDefaultLocationTransitionSeconds);
			(*transitionDurations)[address] = std::clamp(
				std::isfinite(duration) ? duration : kDefaultLocationTransitionSeconds,
				0.0f, kMaxLocationTransitionSeconds);
		}
	}
}

const json* SceneSettingsManager::GetFeatureBaseSnapshot(const std::string& featureShortName)
{
	if (auto snapshotIt = featureBaseSnapshots.find(featureShortName);
		snapshotIt != featureBaseSnapshots.end())
		return &snapshotIt->second;

	auto* feature = Feature::FindFeatureByShortName(featureShortName);
	if (!feature)
		return nullptr;

	json snapshot;
	try {
		feature->SaveSettings(snapshot);
	} catch (const std::exception& e) {
		logger::warn("[SceneSettings] Could not snapshot {}: {}", featureShortName, e.what());
		return nullptr;
	} catch (...) {
		logger::warn("[SceneSettings] Could not snapshot {}", featureShortName);
		return nullptr;
	}
	if (!snapshot.is_object())
		return nullptr;

	// SaveSettings contains the live scene layer. Replace only applied addresses in memory.
	for (auto appliedIt = appliedSettings.lower_bound({ featureShortName, {}, {} });
		appliedIt != appliedSettings.end() && appliedIt->first.featureShortName == featureShortName;
		++appliedIt) {
		const auto& address = appliedIt->first;
		auto baselineIt = baselineSettings.find(address);
		auto* setting = FindAllowedCatalogSetting(
			address.featureShortName, address.settingPath, address.settingKey);
		auto* value = setting ? GetCatalogSerializedValue(snapshot, *setting) : nullptr;
		if (baselineIt != baselineSettings.end() && value &&
			IsCompatibleSceneSettingValue(*value, baselineIt->second))
			*value = baselineIt->second;
	}

	auto [snapshotIt, _] = featureBaseSnapshots.emplace(featureShortName, std::move(snapshot));
	return &snapshotIt->second;
}

void SceneSettingsManager::EnsureBaselines(std::span<const SettingAddress> addresses)
{
	std::map<std::string, std::vector<const SettingAddress*>> missingByFeature;
	for (const auto& address : addresses)
		if (!baselineSettings.contains(address))
			missingByFeature[address.featureShortName].push_back(&address);

	for (const auto& [featureShortName, missing] : missingByFeature) {
		const auto* snapshot = GetFeatureBaseSnapshot(featureShortName);
		if (!snapshot)
			continue;
		for (const auto* address : missing) {
			auto* setting = FindAllowedCatalogSetting(
				address->featureShortName, address->settingPath, address->settingKey);
			const auto* value = setting ? GetCatalogSerializedValue(*snapshot, *setting) : nullptr;
			if (value && IsSceneSettingPrimitive(*value))
				baselineSettings.try_emplace(*address, *value);
		}
	}
}

json SceneSettingsManager::GetBaselineValue(const SettingAddress& address)
{
	if (!FindAllowedCatalogSetting(address.featureShortName, address.settingPath, address.settingKey))
		return {};
	if (auto it = baselineSettings.find(address); it != baselineSettings.end())
		return it->second;
	EnsureBaselines(std::span{ &address, 1 });
	if (auto it = baselineSettings.find(address); it != baselineSettings.end())
		return it->second;
	return {};
}

bool SceneSettingsManager::ResolvedValuesEqual(const json& lhs, const json& rhs)
{
	return lhs == rhs;
}

bool SceneSettingsManager::AppliedValuesEqual(const json& actual, const json& expected)
{
	return ResolvedValuesEqual(actual, expected) ||
	       (actual.is_number_float() && expected.is_number_float() &&
			   actual.get<double>() == static_cast<double>(expected.get<float>()));
}

// --- Unified Persistence ---

static json EntryToJson(const SceneSettingsManager::SettingEntry& entry)
{
	json item = entry.serializedTemplate.is_object() ? entry.serializedTemplate : json::object();
	item["feature"] = entry.featureShortName;
	if (!entry.settingPath.empty())
		item["path"] = entry.settingPath;
	else
		item.erase("path");
	item["setting"] = entry.settingKey;
	item["value"] = entry.value;
	item["originalValue"] = entry.originalValue;
	item["paused"] = entry.paused;
	if (entry.transitionSeconds)
		item["transitionSeconds"] = *entry.transitionSeconds;
	else
		item.erase("transitionSeconds");
	if (entry.period != SceneSettingsManager::TimeOfDayPeriod::Count)
		item["period"] = SceneSettingsManager::GetPeriodName(entry.period);
	else
		item.erase("period");
	return item;
}

static json UserEntriesToArray(const std::vector<SceneSettingsManager::SettingEntry>& entries, bool transitionOnly = false)
{
	json arr = json::array();
	for (const auto& entry : entries)
		if (entry.source == SceneSettingsManager::EntrySource::User &&
			(!transitionOnly || IsNumericValue(entry.value)))
			arr.push_back(EntryToJson(entry));
	return arr;
}

static void AppendRawEntries(json& arr, const std::vector<json>& rawEntries)
{
	if (!arr.is_array())
		arr = json::array();
	for (const auto& raw : rawEntries)
		arr.push_back(raw);
}

static bool ShouldSerializeUserSection(const json& data, std::string_view key, bool expectObject, bool modified)
{
	auto it = data.find(std::string(key));
	return modified || it == data.end() || (expectObject ? it->is_object() : it->is_array());
}

bool SceneSettingsManager::SaveAllUserSettings()
{
	if (!userSettingsDocumentLoaded)
		LoadAllUserSettings();
	const bool weatherLoaded = TryEnsureWeatherDataLoaded();
	const bool locationLoaded = TryEnsureLocationDataLoaded();
	if (!userSettingsDocumentWritable || !preservedUserSettingsRoot.is_object()) {
		if (!userSettingsWriteBlockedWarning) {
			logger::error("[SceneSettings] Refusing to overwrite SceneManager.json because its existing document is invalid");
			userSettingsWriteBlockedWarning = true;
		}
		deferredSceneChangesPending = true;
		deferredSceneChangesDeadline = std::chrono::steady_clock::now() + kDeferredSaveRetryDelay;
		return false;
	}

	auto path = GetUserSettingsFilePath();
	json data = preservedUserSettingsRoot;
	if (ShouldSerializeUserSection(data, "interiorOnly", false, interiorUserSettingsModified)) {
		data["interiorOnly"] = UserEntriesToArray(GetEntries(SceneType::InteriorOnly));
		AppendRawEntries(data["interiorOnly"], unresolvedUserEntries[SceneType::InteriorOnly]);
	}
	if (ShouldSerializeUserSection(data, "timeOfDay", false, timeOfDayUserSettingsModified)) {
		data["timeOfDay"] = UserEntriesToArray(GetEntries(SceneType::TimeOfDay), true);
		AppendRawEntries(data["timeOfDay"], unresolvedUserEntries[SceneType::TimeOfDay]);
	}

	// Weather entries (keyed by SPID)
	if (weatherLoaded && ShouldSerializeUserSection(data, "weather", true, weatherUserSettingsModified)) {
		json weatherObj = unresolvedWeatherUserSettings.is_object() ?
		                      unresolvedWeatherUserSettings :
		                      json::object();
		std::set<RE::FormID> weatherIds;
		for (const auto& [weatherId, _] : weatherSceneConfigs)
			weatherIds.insert(weatherId);

		for (auto weatherId : weatherIds) {
			if (weatherId == 0)
				continue;
			const auto spid = Util::FormIdToSpid(weatherId);
			auto configIt = weatherSceneConfigs.find(weatherId);
			auto userEntries = configIt != weatherSceneConfigs.end() ?
			                       UserEntriesToArray(configIt->second.entries, true) :
			                       json::array();
			const bool hasShowPreference = configIt != weatherSceneConfigs.end() && configIt->second.userTimeOfDayEnabled.has_value();

			auto rawIt = weatherObj.find(spid);
			const bool hasRaw = rawIt != weatherObj.end();
			if (userEntries.empty() && !hasShowPreference && !hasRaw)
				continue;
			if (hasRaw && !rawIt->is_object()) {
				if (userEntries.empty() && !hasShowPreference)
					continue;
				*rawIt = json::object();
			}

			json weatherEntry = hasRaw ? *rawIt : json::object();
			if (!userEntries.empty()) {
				if (auto entriesIt = weatherEntry.find("entries");
					entriesIt != weatherEntry.end() && entriesIt->is_array())
					for (const auto& rawEntry : *entriesIt)
						userEntries.push_back(rawEntry);
				weatherEntry["entries"] = std::move(userEntries);
			}
			if (hasShowPreference)
				weatherEntry["timeOfDayEnabled"] = configIt->second.timeOfDayEnabled;
			weatherObj[spid] = std::move(weatherEntry);
		}
		data["weather"] = std::move(weatherObj);
	}

	if (locationLoaded && ShouldSerializeUserSection(data, "location", true, locationUserSettingsModified)) {
		json locationObj = unresolvedLocationUserSettings.is_object() ?
		                       unresolvedLocationUserSettings :
		                       json::object();
		locationObj.erase("transitionSeconds");
		for (const auto& [_, config] : locationSceneConfigs) {
			auto userEntries = UserEntriesToArray(config.entries);

			const auto* sectionName = GetLocationSectionName(config.type);
			auto& section = locationObj[sectionName];
			if (!section.is_object())
				section = json::object();
			auto& rawConfig = section[config.formKey];
			json locationEntry = rawConfig.is_object() ? rawConfig : json::object();
			if (auto entriesIt = locationEntry.find("entries");
				entriesIt != locationEntry.end() && entriesIt->is_array())
				for (const auto& rawEntry : *entriesIt)
					userEntries.push_back(rawEntry);
			locationEntry["type"] = GetLocationTargetTypeName(config.type);
			locationEntry["name"] = config.name;
			locationEntry["coc"] = config.cocCode;
			if (config.userTimeOfDayEnabled)
				locationEntry["timeOfDayEnabled"] = *config.userTimeOfDayEnabled;
			else
				locationEntry.erase("timeOfDayEnabled");
			locationEntry["entries"] = std::move(userEntries);
			rawConfig = std::move(locationEntry);
		}
		data["location"] = std::move(locationObj);
	}

	const bool saved = WriteJsonAtomically(path, data, kOverwriteJsonIndent, "SceneManager.json");
	if (saved) {
		preservedUserSettingsRoot = data;
		interiorUserSettingsModified = false;
		timeOfDayUserSettingsModified = false;
		weatherUserSettingsModified = false;
		locationUserSettingsModified = false;
		userSettingsWriteBlockedWarning = false;
		logger::info("[SceneSettings] Saved SceneManager.json");
	}

	deferredSceneChangesPending = !saved;
	if (!saved)
		deferredSceneChangesDeadline = std::chrono::steady_clock::now() + kDeferredSaveRetryDelay;
	return saved;
}

static bool LoadEntryFromJson(const nlohmann::json& item, SceneSettingsManager::SettingEntry& entry,
	bool requirePeriod, const char* typeName,
	std::optional<SceneSettingsManager::SceneType> allowedSceneType = std::nullopt,
	bool requireNumericValue = false)
{
	using SSM = SceneSettingsManager;

	if (!item.contains("feature") || !item.contains("setting") || !item.contains("value")) {
		logger::warn("[SceneSettings] {} entry missing feature/setting/value fields", typeName);
		return false;
	}
	if (!item["feature"].is_string() || !item["setting"].is_string()) {
		logger::warn("[SceneSettings] {} entry feature/setting not strings", typeName);
		return false;
	}

	entry.featureShortName = item["feature"].get<std::string>();
	entry.settingPath.clear();
	if (auto it = item.find("path"); it != item.end()) {
		if (!it->is_array()) {
			logger::warn("[SceneSettings] {} entry path is not an array", typeName);
			return false;
		}
		for (const auto& part : *it) {
			if (!part.is_string()) {
				logger::warn("[SceneSettings] {} entry path contains a non-string component", typeName);
				return false;
			}
			entry.settingPath.push_back(part.get<std::string>());
		}
	}
	entry.settingKey = item["setting"].get<std::string>();
	entry.value = item["value"];
	entry.originalValue = item.value("originalValue", entry.value);
	entry.serializedTemplate = item.is_object() ? item : json::object();
	if (auto pausedIt = item.find("paused"); pausedIt != item.end() && !pausedIt->is_boolean()) {
		logger::warn("[SceneSettings] {} entry paused field is not boolean", typeName);
		return false;
	}
	entry.paused = item.value("paused", false);
	entry.source = SSM::EntrySource::User;

	auto sceneType = allowedSceneType.value_or(requirePeriod ? SSM::SceneType::TimeOfDay : SSM::SceneType::InteriorOnly);
	if (auto transitionIt = item.find("transitionSeconds"); transitionIt != item.end()) {
		if (sceneType != SSM::SceneType::Location || !transitionIt->is_number()) {
			logger::warn("[SceneSettings] {} entry transitionSeconds is not valid for this scene type", typeName);
			return false;
		}
		const auto seconds = transitionIt->get<float>();
		if (!std::isfinite(seconds) || seconds < 0.0f || seconds > SSM::kMaxLocationTransitionSeconds) {
			logger::warn("[SceneSettings] {} entry transitionSeconds is outside 0..{}",
				typeName, SSM::kMaxLocationTransitionSeconds);
			return false;
		}
		entry.transitionSeconds = seconds;
	}
	if (!SSM::IsFeatureAllowedForType(sceneType, entry.featureShortName)) {
		logger::warn("[SceneSettings] {} entry feature '{}' is not allowed for this scene type", typeName, entry.featureShortName);
		return false;
	}

	if (requirePeriod || item.contains("period")) {
		if (!item.contains("period") || !item["period"].is_string()) {
			logger::warn("[SceneSettings] {} entry {}.{} missing period - skipping", typeName, entry.featureShortName, entry.settingKey);
			return false;
		}
		entry.period = SSM::GetPeriodFromName(item["period"].get<std::string>());
		if (entry.period == SSM::TimeOfDayPeriod::Count) {
			logger::warn("[SceneSettings] {} entry {}.{} has invalid period '{}' - skipping", typeName, entry.featureShortName, entry.settingKey, item["period"].get<std::string>());
			return false;
		}
		if (!IsNumericValue(entry.value) || !IsNumericValue(entry.originalValue)) {
			logger::warn("[SceneSettings] {} entry {} is not a float setting - skipping",
				typeName, GetSettingLogName(entry.featureShortName, entry.settingPath, entry.settingKey));
			return false;
		}
		if (!std::isfinite(entry.value.get<float>())) {
			logger::warn("[SceneSettings] {} entry {} has non-finite value - skipping",
				typeName, GetSettingLogName(entry.featureShortName, entry.settingPath, entry.settingKey));
			return false;
		}
	}
	if (requireNumericValue && (!IsNumericValue(entry.value) || !IsNumericValue(entry.originalValue) ||
								   !std::isfinite(entry.value.get<float>()))) {
		logger::warn("[SceneSettings] {} entry {} is not a finite float setting - skipping",
			typeName, GetSettingLogName(entry.featureShortName, entry.settingPath, entry.settingKey));
		return false;
	}

	const bool requireNumeric = entry.period != SSM::TimeOfDayPeriod::Count || requirePeriod || requireNumericValue;
	if (!ValidateSceneSettingEntry(typeName, sceneType, entry.featureShortName, entry.settingPath, entry.settingKey,
			entry.value, requireNumeric) ||
		!ValidateSceneSettingEntry(typeName, sceneType, entry.featureShortName, entry.settingPath, entry.settingKey,
			entry.originalValue, requireNumeric))
		return false;
	if (entry.transitionSeconds &&
		(!IsNumericValue(entry.value) || !FindAllowedCatalogSetting(
											 entry.featureShortName, entry.settingPath, entry.settingKey, true))) {
		logger::warn("[SceneSettings] {} entry {} has a transition on a discrete setting; preserving it without loading",
			typeName, GetSettingLogName(entry.featureShortName, entry.settingPath, entry.settingKey));
		return false;
	}

	entry.displayName = GetSceneSettingDisplayName(entry.featureShortName, entry.settingPath, entry.settingKey);
	return true;
}

void SceneSettingsManager::LoadAllUserSettings()
{
	auto path = GetUserSettingsFilePath();
	logger::info("[SceneSettings] Loading user settings from: {}", path.string());
	for (auto type : { SceneType::InteriorOnly, SceneType::TimeOfDay })
		std::erase_if(entries[type], [](const SettingEntry& entry) { return entry.source == EntrySource::User; });
	unresolvedUserEntries[SceneType::InteriorOnly].clear();
	unresolvedUserEntries[SceneType::TimeOfDay].clear();
	++sceneValueRevision;
	BumpEntryPresentationRevision();
	interiorUserSettingsModified = false;
	timeOfDayUserSettingsModified = false;
	std::error_code ec;
	if (!std::filesystem::exists(path, ec)) {
		userSettingsDocumentLoaded = true;
		userSettingsDocumentWritable = !ec;
		preservedUserSettingsRoot = json::object();
		if (ec)
			logger::error("[SceneSettings] Could not inspect SceneManager.json: {}", ec.message());
		else
			logger::info("[SceneSettings] SceneManager.json not found at {}", path.string());
		return;
	}

	try {
		std::ifstream file(path);
		if (!file.is_open()) {
			userSettingsDocumentLoaded = true;
			userSettingsDocumentWritable = false;
			logger::error("[SceneSettings] Could not open SceneManager.json for reading");
			return;
		}

		json data = json::parse(file, nullptr, false);
		userSettingsDocumentLoaded = true;
		preservedUserSettingsRoot = data;
		if (!data.is_object()) {
			userSettingsDocumentWritable = false;
			logger::error("[SceneSettings] SceneManager.json must contain a valid JSON object; automatic saves are blocked");
			return;
		}
		userSettingsDocumentWritable = true;
		// Interior Only
		if (data.contains("interiorOnly") && data["interiorOnly"].is_array()) {
			auto& vec = GetEntriesMut(SceneType::InteriorOnly);
			int loaded = 0;
			for (const auto& item : data["interiorOnly"]) {
				SettingEntry entry;
				if (!LoadEntryFromJson(item, entry, false, "InteriorOnly")) {
					unresolvedUserEntries[SceneType::InteriorOnly].push_back(item);
					continue;
				}
				if (HasDuplicateEntry(SceneType::InteriorOnly, entry.featureShortName, entry.settingPath,
						entry.settingKey, EntrySource::User, entry.period)) {
					unresolvedUserEntries[SceneType::InteriorOnly].push_back(item);
					continue;
				}
				vec.push_back(std::move(entry));
				loaded++;
			}
			if (loaded > 0)
				logger::info("[SceneSettings] Loaded {} InteriorOnly user settings", loaded);
		} else if (data.contains("interiorOnly"))
			logger::warn("[SceneSettings] Preserving non-array interiorOnly section");

		// Time of Day
		if (data.contains("timeOfDay") && data["timeOfDay"].is_array()) {
			auto& vec = GetEntriesMut(SceneType::TimeOfDay);
			int loaded = 0;
			for (const auto& item : data["timeOfDay"]) {
				SettingEntry entry;
				if (!LoadEntryFromJson(item, entry, true, "TimeOfDay")) {
					unresolvedUserEntries[SceneType::TimeOfDay].push_back(item);
					continue;
				}
				if (HasDuplicateEntry(SceneType::TimeOfDay, entry.featureShortName, entry.settingPath,
						entry.settingKey, EntrySource::User, entry.period)) {
					unresolvedUserEntries[SceneType::TimeOfDay].push_back(item);
					continue;
				}
				vec.push_back(std::move(entry));
				loaded++;
			}
			if (loaded > 0)
				logger::info("[SceneSettings] Loaded {} TimeOfDay user settings", loaded);
		} else if (data.contains("timeOfDay"))
			logger::warn("[SceneSettings] Preserving non-array timeOfDay section");

		// Weather and location are loaded lazily once game data is available.

		logger::info("[SceneSettings] Loaded SceneManager.json (non-weather)");
	} catch (const std::exception& e) {
		userSettingsDocumentLoaded = true;
		userSettingsDocumentWritable = false;
		logger::error("[SceneSettings] Failed to load SceneManager.json: {}", e.what());
	}
}

void SceneSettingsManager::LoadLocationUserSettings(const json& data)
{
	const SKSE::stl::scope_exit refreshModes([&] {
		for (auto& [_, config] : locationSceneConfigs)
			config.RefreshTimeOfDayMode();
	});
	for (auto& [_, config] : locationSceneConfigs) {
		std::erase_if(config.entries, [](const SettingEntry& entry) { return entry.source == EntrySource::User; });
		config.userTimeOfDayEnabled.reset();
	}
	unresolvedLocationUserSettings = json::object();
	locationUserSettingsModified = false;
	auto locationIt = data.find("location");
	if (locationIt == data.end())
		return;
	if (!locationIt->is_object()) {
		logger::warn("[SceneSettings] Preserving non-object location section");
		return;
	}
	unresolvedLocationUserSettings = *locationIt;
	unresolvedLocationUserSettings.erase("transitionSeconds");
	std::set<std::string> canonicalLocationTypeKeys;
	const auto loadSection = [&](const char* sectionName, LocationTargetType type,
								 std::string_view persistedTypeName, bool legacySection = false) {
		auto sectionIt = locationIt->find(sectionName);
		if (sectionIt == locationIt->end() || !sectionIt->is_object())
			return;
		json preservedSection = json::object();

		for (const auto& [formKey, rawConfig] : sectionIt->items()) {
			if (formKey.empty()) {
				preservedSection[formKey] = rawConfig;
				continue;
			}
			const auto canonicalFormKey = CanonicalizeResolvedLocationFormKey(formKey);
			if (!rawConfig.is_object()) {
				preservedSection[formKey] = rawConfig;
				continue;
			}
			const auto formId = Util::SpidToFormId(canonicalFormKey);
			auto* form = formId != 0 ? RE::TESForm::LookupByID(formId) : nullptr;
			const bool targetMatches = form && [&] {
				switch (type) {
				case LocationTargetType::Region:
					return form->GetFormType() == RE::FormType::Region;
				case LocationTargetType::Worldspace:
					return form->GetFormType() == RE::FormType::WorldSpace;
				case LocationTargetType::LocationType:
					return form->GetFormType() == RE::FormType::Keyword &&
					       IsLocationTypeKeyword(form->As<RE::BGSKeyword>());
				case LocationTargetType::Location:
					return form->GetFormType() == RE::FormType::Location;
				case LocationTargetType::Cell:
					return form->GetFormType() == RE::FormType::Cell;
				default:
					return false;
				}
			}();
			if (!targetMatches) {
				preservedSection[formKey] = rawConfig;
				logger::warn("[SceneSettings] Location config '{}' does not resolve to its declared target type; preserving it",
					formKey);
				continue;
			}
			const auto configContext = std::format("Location config '{}'", formKey);
			std::string name;
			std::string persistedType;
			std::string cocCode;
			persistedType = persistedTypeName;
			if (!ReadOptionalStringField(rawConfig, "name", name, configContext) ||
				!ReadOptionalStringField(rawConfig, "type", persistedType, configContext) ||
				!ReadOptionalStringField(rawConfig, "coc", cocCode, configContext)) {
				preservedSection[formKey] = rawConfig;
				continue;
			}
			if (persistedType != persistedTypeName) {
				preservedSection[formKey] = rawConfig;
				continue;
			}
			const auto configKey = GetLocationConfigKey(type, canonicalFormKey);
			const bool canonicalMetadataExists = legacySection &&
			                                     canonicalLocationTypeKeys.contains(configKey);
			auto& config = GetLocationConfigMut(
				type, canonicalFormKey, canonicalMetadataExists ? std::string{} : name);
			if (auto show = rawConfig.find("timeOfDayEnabled"); show != rawConfig.end() && show->is_boolean())
				config.userTimeOfDayEnabled = show->get<bool>();
			if (!cocCode.empty() && !canonicalMetadataExists)
				config.cocCode = cocCode;
			if (!legacySection && type == LocationTargetType::LocationType)
				canonicalLocationTypeKeys.insert(configKey);
			auto entriesIt = rawConfig.find("entries");
			if (entriesIt == rawConfig.end()) {
				preservedSection[formKey] = rawConfig;
				continue;
			}
			if (!entriesIt->is_array()) {
				preservedSection[formKey] = rawConfig;
				continue;
			}
			auto preservedConfig = rawConfig;
			preservedConfig["entries"] = json::array();
			bool hasValidEntry = false;

			for (const auto& item : *entriesIt) {
				SettingEntry entry;
				if (!LoadEntryFromJson(item, entry, false, "Location", SceneType::Location)) {
					preservedConfig["entries"].push_back(item);
					continue;
				}
				hasValidEntry = true;
				if (HasLocationEntry(type, canonicalFormKey, entry.featureShortName, entry.settingPath,
						entry.settingKey, EntrySource::User, entry.period)) {
					if (!legacySection)
						preservedConfig["entries"].push_back(item);
					continue;
				}
				config.entries.push_back(std::move(entry));
			}
			if (legacySection && hasValidEntry) {
				if (preservedConfig["entries"].empty()) {
					preservedConfig.erase("entries");
					preservedConfig.erase("type");
					preservedConfig.erase("name");
					preservedConfig.erase("coc");
				}
			}
			if (hasValidEntry && formKey != canonicalFormKey) {
				preservedConfig.erase("type");
				preservedConfig.erase("name");
				preservedConfig.erase("coc");
			}
			if (!preservedConfig.empty())
				preservedSection[formKey] = std::move(preservedConfig);
		}
		if (legacySection && preservedSection.empty())
			unresolvedLocationUserSettings.erase(sectionName);
		else
			unresolvedLocationUserSettings[sectionName] = std::move(preservedSection);
	};

	loadSection("worldspaces", LocationTargetType::Worldspace, "Worldspace");
	loadSection("regions", LocationTargetType::Region, "Region");
	loadSection("locationTypes", LocationTargetType::LocationType, "LocationType");
	loadSection("categories", LocationTargetType::LocationType, "Category", true);
	loadSection("locations", LocationTargetType::Location, "Location");
	loadSection("cells", LocationTargetType::Cell, "Cell");
}

void SceneSettingsManager::LoadWeatherUserSettings()
{
	const SKSE::stl::scope_exit refreshModes([&] {
		for (auto& [_, config] : weatherSceneConfigs)
			config.RefreshTimeOfDayMode();
	});
	for (auto& [_, config] : weatherSceneConfigs)
		std::erase_if(config.entries, [](const SettingEntry& entry) { return entry.source == EntrySource::User; });
	for (auto& [_, config] : weatherSceneConfigs)
		config.userTimeOfDayEnabled.reset();
	unresolvedWeatherUserSettings = json::object();
	weatherUserSettingsModified = false;
	if (!userSettingsDocumentLoaded || !userSettingsDocumentWritable || !preservedUserSettingsRoot.is_object())
		return;

	try {
		auto weatherIt = preservedUserSettingsRoot.find("weather");
		if (weatherIt == preservedUserSettingsRoot.end())
			return;
		if (!weatherIt->is_object()) {
			logger::warn("[SceneSettings] Preserving non-object weather section");
			return;
		}

		logger::info("[SceneSettings] Weather section found with {} entries", weatherIt->size());
		for (const auto& [spidKey, weatherData] : weatherIt->items()) {
			logger::info("[SceneSettings] Processing weather SPID '{}'", spidKey);
			RE::FormID weatherId = Util::SpidToFormId(spidKey);
			if (weatherId == 0) {
				unresolvedWeatherUserSettings[spidKey] = weatherData;
				logger::warn("[SceneSettings] Weather SPID '{}' could not be resolved - skipping", spidKey);
				continue;
			}
			auto* weather = RE::TESForm::LookupByID<RE::TESWeather>(weatherId);
			if (!weather) {
				unresolvedWeatherUserSettings[spidKey] = weatherData;
				logger::warn("[SceneSettings] Weather SPID '{}' does not resolve to a weather - preserving", spidKey);
				continue;
			}
			logger::info("[SceneSettings] Resolved SPID '{}' to FormID 0x{:X}", spidKey, weatherId);
			if (!weatherData.is_object()) {
				unresolvedWeatherUserSettings[spidKey] = weatherData;
				logger::warn("[SceneSettings] Weather config '{}' is not an object - preserving", spidKey);
				continue;
			}
			auto preservedWeather = weatherData;

			if (auto showIt = weatherData.find("timeOfDayEnabled"); showIt != weatherData.end()) {
				if (!showIt->is_boolean()) {
					logger::warn("[SceneSettings] Weather config '{}' timeOfDayEnabled is not boolean - preserving", spidKey);
				} else {
					GetWeatherConfigMut(weatherId).userTimeOfDayEnabled = showIt->get<bool>();
					preservedWeather.erase("timeOfDayEnabled");
				}
			}

			auto entriesIt = weatherData.find("entries");
			if (entriesIt == weatherData.end()) {
				unresolvedWeatherUserSettings[spidKey] = std::move(preservedWeather);
				continue;
			}
			if (!entriesIt->is_array()) {
				unresolvedWeatherUserSettings[spidKey] = preservedWeather;
				logger::warn("[SceneSettings] Weather config '{}' entries is not an array - preserving", spidKey);
				continue;
			}
			preservedWeather["entries"] = json::array();

			auto& config = GetWeatherConfigMut(weatherId);
			int loaded = 0;
			for (const auto& item : *entriesIt) {
				SettingEntry entry;
				if (!LoadEntryFromJson(item, entry, false, "Weather", SceneType::TimeOfDay, true)) {
					preservedWeather["entries"].push_back(item);
					continue;
				}
				if (HasWeatherEntryForPeriod(weatherId, entry.featureShortName, entry.settingPath,
						entry.settingKey, entry.period, EntrySource::User)) {
					preservedWeather["entries"].push_back(item);
					continue;
				}
				config.entries.push_back(std::move(entry));
				loaded++;
			}
			if (loaded > 0)
				logger::info("[SceneSettings] Loaded {} weather entries for {}", loaded, spidKey);
			unresolvedWeatherUserSettings[spidKey] = std::move(preservedWeather);
		}

		logger::info("[SceneSettings] Loaded weather user settings");
	} catch (const std::exception& e) {
		logger::error("[SceneSettings] Failed to load weather user settings: {}", e.what());
	}
}

void SceneSettingsManager::DiscoverOverwrites(SceneType type)
{
	if (!IsEntryListSceneType(type))
		return;
	const auto previousEntryCount = GetEntries(type).size();
	// TimeOfDay has period subfolders; delegate to a shared loader
	if (type == SceneType::TimeOfDay) {
		auto basePath = GetOverwritesPath(type);
		for (int i = 0; i < kPeriodCount; ++i) {
			auto period = static_cast<TimeOfDayPeriod>(i);
			auto periodPath = basePath / GetPeriodName(period);
			DiscoverOverwritesInDir(type, periodPath, period);
		}
	} else {
		DiscoverOverwritesInDir(type, GetOverwritesPath(type));
	}

	if (GetEntries(type).size() != previousEntryCount && type == SceneType::TimeOfDay)
		++sceneValueRevision;
	if (GetEntries(type).size() != previousEntryCount)
		BumpEntryPresentationRevision();
}

static bool ParseOverwriteFileEntries(const std::filesystem::path& filePath,
	SceneSettingsManager::SceneType allowedType, bool requireNumeric,
	std::vector<SceneSettingsManager::SettingEntry>& outEntries, std::optional<bool>* timeOfDayEnabled = nullptr)
{
	using SSM = SceneSettingsManager;

	json data;
	if (!ReadBoundedSceneJson(filePath, data))
		return false;
	if (timeOfDayEnabled) {
		timeOfDayEnabled->reset();
		if (auto metadata = data.find(kMetadataKey); metadata != data.end() && metadata->is_object()) {
			if (auto mode = metadata->find(kMetadataTimeOfDayEnabledKey); mode != metadata->end()) {
				if (mode->is_boolean())
					*timeOfDayEnabled = mode->get<bool>();
				else
					logger::warn("[SceneSettings] Overwrite '{}' timeOfDayEnabled metadata must be boolean", filePath.string());
			}
		}
	}

	std::string featureShortName = data.value(kFeatureKey, "");
	if (featureShortName.empty()) {
		auto stem = filePath.stem().string();
		auto lastUnderscore = stem.rfind('_');
		if (lastUnderscore != std::string::npos)
			featureShortName = stem.substr(lastUnderscore + 1);
	}

	auto* featurePtr = Feature::FindFeatureByShortName(featureShortName);
	if (!featurePtr || !SSM::IsFeatureAllowedForType(allowedType, featureShortName))
		return false;
	const json* entryTransitions = nullptr;
	if (allowedType == SSM::SceneType::Location) {
		if (auto metadataIt = data.find(kMetadataKey);
			metadataIt != data.end() && metadataIt->is_object()) {
			if (auto transitionsIt = metadataIt->find(kMetadataEntryTransitionsKey);
				transitionsIt != metadataIt->end()) {
				if (transitionsIt->is_object())
					entryTransitions = &*transitionsIt;
				else
					logger::warn("[SceneSettings] Location overwrite '{}' has invalid entry transition metadata",
						filePath.string());
			}
		}
	}

	bool foundAny = false;
	CollectOverwriteEntries(data, {}, [&](const auto& settingPath, const auto& key, const auto& value) {
		if (!ValidateSceneSettingEntry(
				"Overwrite", allowedType, featureShortName, settingPath, key, value, requireNumeric))
			return;

		SSM::SettingEntry entry;
		entry.featureShortName = featureShortName;
		entry.settingPath = settingPath;
		entry.settingKey = key;
		entry.displayName = GetSceneSettingDisplayName(featureShortName, settingPath, key);
		entry.value = value;
		entry.originalValue = entry.value;
		entry.source = SSM::EntrySource::Overwrite;
		entry.sourceFilename = filePath.filename().string();
		if (entryTransitions) {
			if (const auto* transitionNode = GetObjectAtPath(*entryTransitions, settingPath)) {
				if (auto transitionIt = transitionNode->find(key); transitionIt != transitionNode->end()) {
					if (transitionIt->is_number()) {
						const auto seconds = transitionIt->get<float>();
						if (std::isfinite(seconds) && seconds >= 0.0f &&
							seconds <= SSM::kMaxLocationTransitionSeconds)
							entry.transitionSeconds = seconds;
						else
							logger::warn("[SceneSettings] Location overwrite '{}' transition for '{}.{}' is outside 0..{}",
								filePath.string(), featureShortName, key, SSM::kMaxLocationTransitionSeconds);
					} else {
						logger::warn("[SceneSettings] Location overwrite '{}' transition for '{}.{}' must be numeric",
							filePath.string(), featureShortName, key);
					}
				}
			}
		}

		entry.sourcePath = filePath;
		outEntries.push_back(std::move(entry));
		foundAny = true;
	});
	return foundAny;
}

void SceneSettingsManager::DiscoverOverwritesInDir(SceneType type, const std::filesystem::path& dir, TimeOfDayPeriod period)
{
	auto typeName = GetSceneTypeName(type);

	std::error_code ec;
	if (!std::filesystem::exists(dir, ec))
		return;

	logger::info("[SceneSettings] Discovering {} overwrites in: {}", typeName, dir.string());

	bool requireNumeric = (type == SceneType::TimeOfDay);
	auto& vec = GetEntriesMut(type);
	int filesFound = 0, overwritesLoaded = 0;
	for (const auto& filePath : GetSortedJsonFiles(dir, std::format("{} overwrite files", typeName))) {
		filesFound++;
		try {
			std::vector<SettingEntry> parsedEntries;
			if (!ParseOverwriteFileEntries(filePath, type, requireNumeric, parsedEntries))
				continue;
			overwriteBackingFiles[{ .type = type == SceneType::InteriorOnly ? SceneContextType::Interior : SceneContextType::TimeOfDay,
									  .period = period }]
				.insert(filePath);
			for (auto& entry : parsedEntries) {
				entry.period = period;
				if (AddOverwriteEntryIfUnique(vec, std::move(entry), typeName))
					overwritesLoaded++;
			}
		} catch (const std::exception& e) {
			logger::error("[SceneSettings] Failed to load {} overwrite '{}': {}", typeName, filePath.filename().string(), e.what());
		}
	}

	if (filesFound > 0)
		logger::info("[SceneSettings] {} overwrite scan: {} files, {} loaded", typeName, filesFound, overwritesLoaded);
}

void SceneSettingsManager::LoadAll()
{
	if (!dataLoaded) {
		dataLoaded = true;
		DiscoverOverwrites(SceneType::InteriorOnly);
		DiscoverOverwrites(SceneType::TimeOfDay);
		LoadAllUserSettings();
		BumpEntryPresentationRevision();
		activeEntryCacheDirty = true;
		resolverDirty = true;
	}
	TryEnsureLocationDataLoaded();
}

bool SceneSettingsManager::ReloadSceneSettings()
{
	if (HasPendingFeatureSceneEdits() || !IsSceneReady())
		return false;
	EndFeatureSceneEdit(false);
	deferredSceneChangesPending = false;
	LoadAllUserSettings();
	if (weatherDataLoaded)
		LoadWeatherUserSettings();
	if (locationDataLoaded)
		LoadLocationUserSettings(preservedUserSettingsRoot);
	ReloadOverwriteEntries();
	return userSettingsDocumentWritable;
}

void SceneSettingsManager::OnDataLoaded()
{
	gameDataReady = true;
	if (dataLoaded)
		TryEnsureLocationDataLoaded();
}

bool SceneSettingsManager::TryEnsureLocationDataLoaded()
{
	if (locationDataLoaded)
		return true;
	if (!gameDataReady || !RE::TESDataHandler::GetSingleton())
		return false;
	if (!userSettingsDocumentLoaded)
		LoadAllUserSettings();

	try {
		DiscoverLocationOverwrites();
		if (userSettingsDocumentLoaded && userSettingsDocumentWritable && preservedUserSettingsRoot.is_object())
			LoadLocationUserSettings(preservedUserSettingsRoot);
		locationDataLoaded = true;
		locationTargetsCached = false;
		locationManagementTargetsCached = false;
		BumpEntryPresentationRevision();
		activeEntryCacheDirty = true;
		resolverDirty = true;
		return true;
	} catch (const std::exception& e) {
		logger::error("[SceneSettings] Failed to load location settings: {}", e.what());
		return false;
	}
}

bool SceneSettingsManager::TryEnsureWeatherDataLoaded()
{
	if (weatherDataLoaded)
		return true;
	if (!globals::game::sky || !RE::TESDataHandler::GetSingleton())
		return false;
	if (!userSettingsDocumentLoaded)
		LoadAllUserSettings();

	weatherDataLoaded = true;
	LoadWeatherData();
	++sceneValueRevision;
	BumpEntryPresentationRevision();
	activeEntryCacheDirty = true;
	resolverDirty = true;
	return true;
}

void SceneSettingsManager::LoadWeatherData()
{
	DiscoverWeatherOverwrites();
	LoadWeatherUserSettings();
}

RE::FormID SceneSettingsManager::GetEffectivePreviousWeatherId(const RE::Sky* sky, float weatherLerp) const
{
	if (!sky)
		return 0;
	if (weatherLerp >= 1.0f) {
		if (sky->currentWeather)
			cachedPreviousWeatherId = sky->currentWeather->GetFormID();
		return 0;
	}
	if (sky->lastWeather)
		cachedPreviousWeatherId = sky->lastWeather->GetFormID();
	return cachedPreviousWeatherId;
}

// --- Per-Weather Scene Settings ---

const SceneSettingsManager::WeatherSceneConfig SceneSettingsManager::kEmptyWeatherConfig{};

const SceneSettingsManager::WeatherSceneConfig& SceneSettingsManager::GetWeatherConfig(RE::FormID weatherId)
{
	if (!TryEnsureWeatherDataLoaded())
		return kEmptyWeatherConfig;

	auto it = weatherSceneConfigs.find(weatherId);
	return (it != weatherSceneConfigs.end()) ? it->second : kEmptyWeatherConfig;
}

SceneSettingsManager::WeatherSceneConfig& SceneSettingsManager::GetWeatherConfigMut(RE::FormID weatherId)
{
	return weatherSceneConfigs[weatherId];
}

bool SceneSettingsManager::HasWeatherConfig(RE::FormID weatherId)
{
	if (!TryEnsureWeatherDataLoaded())
		return false;

	auto it = weatherSceneConfigs.find(weatherId);
	return it != weatherSceneConfigs.end() && std::any_of(it->second.entries.begin(), it->second.entries.end(),
												  [](const auto& entry) { return IsNumericValue(entry.value); });
}

void SceneSettingsManager::PrepareWeatherUserSettingsMutation(RE::FormID weatherId, bool replaceMalformedEntries)
{
	auto& config = GetWeatherConfigMut(weatherId);
	config.userTimeOfDayEnabled = config.timeOfDayEnabled;
	weatherUserSettingsModified = true;
	if (!unresolvedWeatherUserSettings.is_object())
		unresolvedWeatherUserSettings = json::object();
	const auto canonicalSpid = Util::FormIdToSpid(weatherId);
	const auto normalizedSpid = NormalizeLocationFormKey(canonicalSpid);
	if (replaceMalformedEntries) {
		for (auto& [rawSpid, rawWeather] : unresolvedWeatherUserSettings.items()) {
			if (!rawWeather.is_object() || NormalizeLocationFormKey(rawSpid) != normalizedSpid)
				continue;
			auto entriesIt = rawWeather.find("entries");
			if (entriesIt != rawWeather.end() && !entriesIt->is_array())
				*entriesIt = json::array();
		}
	}

	auto& rawWeather = unresolvedWeatherUserSettings[canonicalSpid];
	if (!rawWeather.is_object())
		rawWeather = json::object();
	if (replaceMalformedEntries) {
		auto entriesIt = rawWeather.find("entries");
		if (entriesIt != rawWeather.end() && !entriesIt->is_array())
			*entriesIt = json::array();
	}
}

std::optional<float> SceneSettingsManager::ResolveWeatherLowerValue(RE::FormID weatherId,
	const SettingAddress& address, TimeOfDayPeriod period, EntrySource selectedSource)
{
	const auto periodIndex = static_cast<int>(period);
	if (periodIndex < 0 || periodIndex > kPeriodCount)
		return std::nullopt;
	auto baseline = GetBaselineValue(address);
	if (!IsNumericValue(baseline))
		return std::nullopt;
	const auto baselineValue = baseline.get<float>();
	if (!std::isfinite(baselineValue))
		return std::nullopt;

	float lowerValue = baselineValue;
	const auto& timeOfDayValues = BuildTimeOfDayValueGroups(
		selectedSource == EntrySource::User ? std::optional{ EntrySource::User } : std::nullopt);
	if (auto valueIt = timeOfDayValues.find(address); valueIt != timeOfDayValues.end())
		if (period == TimeOfDayPeriod::Count) {
			std::array<float, kPeriodCount> factors{};
			GetTimeOfDayFactors(factors.data());
			lowerValue = 0.0f;
			for (int p = 0; p < kPeriodCount; ++p)
				lowerValue += factors[p] * valueIt->second[p].value_or(baselineValue);
		} else {
			lowerValue = valueIt->second[periodIndex].value_or(baselineValue);
		}
	if (selectedSource != EntrySource::Overwrite)
		return lowerValue;

	auto configIt = weatherSceneConfigs.find(weatherId);
	if (configIt == weatherSceneConfigs.end())
		return lowerValue;
	for (const auto& entry : configIt->second.entries) {
		if (entry.source != EntrySource::User || entry.period != period || !IsEntryActive(entry) ||
			!IsNumericValue(entry.value) ||
			!IsSameSetting(entry, address.featureShortName, address.settingPath, address.settingKey))
			continue;
		const auto value = entry.value.get<float>();
		if (std::isfinite(value))
			lowerValue = value;
	}
	return lowerValue;
}

bool SceneSettingsManager::AddWeatherSetting(RE::FormID weatherId, const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey, TimeOfDayPeriod period,
	bool deferSave)
{
	if (!TryEnsureWeatherDataLoaded())
		return false;
	if (!IsSettingAllowedForType(
			SceneType::TimeOfDay, featureShortName, settingPath, settingKey))
		return false;

	if (static_cast<int>(period) < 0 || static_cast<int>(period) > kPeriodCount)
		return false;
	if (HasWeatherEntryForPeriod(weatherId, featureShortName, settingPath, settingKey, period, EntrySource::User))
		return false;
	SettingAddress address{ featureShortName, settingPath, settingKey };
	auto lowerValue = ResolveWeatherLowerValue(weatherId, address, period, EntrySource::User);
	if (!lowerValue || !ValidateSceneSettingEntry(
						   "Weather", SceneType::TimeOfDay, featureShortName, settingPath, settingKey, *lowerValue, true))
		return false;

	auto& config = GetWeatherConfigMut(weatherId);

	SettingEntry entry;
	entry.featureShortName = featureShortName;
	entry.settingPath = settingPath;
	entry.settingKey = settingKey;
	entry.displayName = GetSceneSettingDisplayName(featureShortName, settingPath, settingKey);
	entry.value = *lowerValue;
	entry.originalValue = *lowerValue;
	entry.source = EntrySource::User;
	entry.period = period;
	config.entries.push_back(std::move(entry));
	++sceneValueRevision;
	BumpEntryPresentationRevision();
	PrepareWeatherUserSettingsMutation(weatherId, true);
	if (deferSave) {
		MarkDeferredSceneChanges();
	} else {
		CommitSceneSettingChanges();
	}
	return true;
}

void SceneSettingsManager::RemoveWeatherSetting(RE::FormID weatherId, size_t index)
{
	RemoveSceneSettings({ .type = SceneContextType::Weather, .weatherId = weatherId }, std::span{ &index, 1 });
}

void SceneSettingsManager::RemoveSceneSettings(const SceneContextId& context, std::span<const size_t> indices)
{
	if ((context.type != SceneContextType::Weather && context.type != SceneContextType::Location) ||
		!IsValidSceneContext(context))
		return;
	if (context.type == SceneContextType::Weather ? !TryEnsureWeatherDataLoaded() : !TryEnsureLocationDataLoaded())
		return;
	auto* contextEntries = GetCopyContextEntriesMut(context);
	if (!contextEntries)
		return;
	std::set<size_t, std::greater<>> userIndices;
	std::map<std::filesystem::path, std::set<SettingIdentity>> overwriteSettings;
	for (const auto index : indices) {
		if (index >= contextEntries->size())
			continue;
		const auto& entry = (*contextEntries)[index];
		if (entry.source == EntrySource::User)
			userIndices.insert(index);
		else {
			const auto path = context.type == SceneContextType::Weather ? GetWeatherOverwritePath(context.weatherId, entry) :
			                                                              GetLocationOverwritePath(context.locationType, context.locationFormKey, entry);
			if (!path.empty())
				overwriteSettings[path].insert({ entry.featureShortName, entry.settingPath, entry.settingKey });
		}
	}
	bool overwritesChanged = false;
	for (const auto& [path, settings] : overwriteSettings) {
		const std::vector pending(settings.begin(), settings.end());
		overwritesChanged |= RemoveSettingsFromOverwriteFile(path, pending);
	}
	for (const auto index : userIndices)
		contextEntries->erase(contextEntries->begin() + static_cast<ptrdiff_t>(index));
	if (!userIndices.empty()) {
		if (context.type == SceneContextType::Weather) {
			PrepareWeatherUserSettingsMutation(context.weatherId, false);
			++sceneValueRevision;
		} else
			PrepareLocationUserSettingsMutation(context.locationType, context.locationFormKey, false);
		BumpEntryPresentationRevision();
		SaveAllUserSettings();
	}
	if (overwritesChanged)
		ReloadOverwriteEntries();
	else if (!userIndices.empty())
		ReapplyIfActive();
}

void SceneSettingsManager::DeleteAllWeatherUserSettings(RE::FormID weatherId)
{
	if (!TryEnsureWeatherDataLoaded())
		return;
	auto configIt = weatherSceneConfigs.find(weatherId);
	if (configIt != weatherSceneConfigs.end()) {
		const auto removed = std::erase_if(configIt->second.entries,
			[](const SettingEntry& entry) { return entry.source == EntrySource::User; });
		if (removed != 0)
			++sceneValueRevision;
		if (removed != 0)
			BumpEntryPresentationRevision();
	}
	PrepareWeatherUserSettingsMutation(weatherId, false);
	const auto normalizedSpid = NormalizeLocationFormKey(Util::FormIdToSpid(weatherId));
	for (auto& [rawSpid, rawWeather] : unresolvedWeatherUserSettings.items())
		if (rawWeather.is_object() && NormalizeLocationFormKey(rawSpid) == normalizedSpid)
			rawWeather.erase("entries");
	SaveAllUserSettings();
	ReapplyIfActive();
}

void SceneSettingsManager::TogglePauseWeatherEntry(RE::FormID weatherId, size_t index)
{
	if (!TryEnsureWeatherDataLoaded())
		return;

	auto it = weatherSceneConfigs.find(weatherId);
	if (it == weatherSceneConfigs.end() || index >= it->second.entries.size())
		return;
	it->second.entries[index].paused = !it->second.entries[index].paused;
	++sceneValueRevision;
	BumpEntryPresentationRevision();
	if (it->second.entries[index].source == EntrySource::User) {
		PrepareWeatherUserSettingsMutation(weatherId, false);
		SaveAllUserSettings();
	}
	ReapplyIfActive();
}

void SceneSettingsManager::SetWeatherEntriesPaused(
	RE::FormID weatherId, std::span<const size_t> indices, bool paused)
{
	if (!TryEnsureWeatherDataLoaded())
		return;

	auto configIt = weatherSceneConfigs.find(weatherId);
	if (configIt == weatherSceneConfigs.end())
		return;
	bool changed = false;
	bool userEntriesChanged = false;
	for (const auto index : indices) {
		if (index >= configIt->second.entries.size())
			continue;
		auto& entry = configIt->second.entries[index];
		if (entry.paused == paused)
			continue;
		entry.paused = paused;
		changed = true;
		userEntriesChanged |= entry.source == EntrySource::User;
	}
	if (!changed)
		return;

	++sceneValueRevision;
	BumpEntryPresentationRevision();
	if (userEntriesChanged) {
		PrepareWeatherUserSettingsMutation(weatherId, false);
		SaveAllUserSettings();
	}
	ReapplyIfActive();
}

void SceneSettingsManager::UpdateWeatherEntryValue(RE::FormID weatherId, size_t index, const json& newValue, bool deferSave)
{
	const EntryValueUpdate update{ index, newValue };
	UpdateWeatherEntryValues(weatherId, std::span{ &update, 1 }, deferSave);
}

void SceneSettingsManager::UpdateWeatherEntryValues(
	RE::FormID weatherId, std::span<const EntryValueUpdate> updates, bool deferSave)
{
	if (!TryEnsureWeatherDataLoaded())
		return;

	auto it = weatherSceneConfigs.find(weatherId);
	if (it == weatherSceneConfigs.end())
		return;
	bool userEntriesChanged = false;
	if (!ApplyEntryValueUpdates(
			"Weather", SceneType::TimeOfDay, it->second.entries, updates, true, userEntriesChanged))
		return;
	++sceneValueRevision;
	if (userEntriesChanged) {
		PrepareWeatherUserSettingsMutation(weatherId, false);
		if (deferSave)
			MarkDeferredSceneChanges();
		else
			SaveAllUserSettings();
	}
	ReapplyIfActive(false);
}

void SceneSettingsManager::RevertWeatherEntryToDefault(RE::FormID weatherId, size_t index)
{
	if (!TryEnsureWeatherDataLoaded())
		return;

	auto it = weatherSceneConfigs.find(weatherId);
	if (it == weatherSceneConfigs.end() || index >= it->second.entries.size())
		return;
	auto& entry = it->second.entries[index];
	SettingAddress address{ entry.featureShortName, entry.settingPath, entry.settingKey };
	auto lowerValue = ResolveWeatherLowerValue(weatherId, address, entry.period, entry.source);
	if (!lowerValue || !ValidateSceneSettingEntry(
						   "Weather", SceneType::TimeOfDay, entry.featureShortName,
						   entry.settingPath, entry.settingKey, *lowerValue, true))
		return;
	entry.value = *lowerValue;
	entry.originalValue = *lowerValue;
	++sceneValueRevision;
	if (entry.source == EntrySource::User) {
		PrepareWeatherUserSettingsMutation(weatherId, false);
		SaveAllUserSettings();
	}
	ReapplyIfActive(false);
}

bool SceneSettingsManager::HasWeatherEntryForPeriod(RE::FormID weatherId, const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey, TimeOfDayPeriod period, std::optional<EntrySource> source)
{
	if (!TryEnsureWeatherDataLoaded())
		return false;

	auto it = weatherSceneConfigs.find(weatherId);
	if (it == weatherSceneConfigs.end())
		return false;
	for (const auto& e : it->second.entries)
		if (IsSameSetting(e, featureShortName, settingPath, settingKey) && e.period == period &&
			(!source || e.source == *source))
			return true;
	return false;
}

// --- Per-Weather Persistence ---

const SceneSettingsManager::PeriodicSceneConfig* SceneSettingsManager::GetPeriodicSceneConfig(const SceneContextId& context) const
{
	if (context.type == SceneContextType::Weather) {
		const auto config = weatherSceneConfigs.find(context.weatherId);
		return config != weatherSceneConfigs.end() ? &config->second : nullptr;
	}
	if (context.type == SceneContextType::Location) {
		const auto config = locationSceneConfigs.find(GetLocationConfigKey(context.locationType, context.locationFormKey));
		return config != locationSceneConfigs.end() ? &config->second : nullptr;
	}
	return nullptr;
}

bool SceneSettingsManager::IsSceneTimeOfDayEnabled(const SceneContextId& context) const
{
	const auto* config = GetPeriodicSceneConfig(context);
	return config && config->timeOfDayEnabled;
}

void SceneSettingsManager::SetSceneTimeOfDayEnabled(const SceneContextId& context, bool enabled)
{
	if (!IsValidSceneContext(context))
		return;
	PeriodicSceneConfig* config = nullptr;
	if (context.type == SceneContextType::Weather && TryEnsureWeatherDataLoaded()) {
		config = &GetWeatherConfigMut(context.weatherId);
		if (config->userTimeOfDayEnabled == enabled)
			return;
		PrepareWeatherUserSettingsMutation(context.weatherId, false);
	} else if (context.type == SceneContextType::Location && TryEnsureLocationDataLoaded()) {
		config = &GetLocationConfigMut(context.locationType, context.locationFormKey);
		if (config->userTimeOfDayEnabled == enabled)
			return;
		PrepareLocationUserSettingsMutation(context.locationType, context.locationFormKey, false);
	}
	if (!config)
		return;
	config->timeOfDayEnabled = enabled;
	config->userTimeOfDayEnabled = enabled;
	++sceneValueRevision;
	BumpEntryPresentationRevision();
	SaveAllUserSettings();
	ReapplyIfActive();
}

bool SceneSettingsManager::IsWeatherShowTimeOfDay(RE::FormID weatherId)
{
	return TryEnsureWeatherDataLoaded() && IsSceneTimeOfDayEnabled({ .type = SceneContextType::Weather, .weatherId = weatherId });
}

void SceneSettingsManager::SetWeatherShowTimeOfDay(RE::FormID weatherId, bool show)
{
	SetSceneTimeOfDayEnabled({ .type = SceneContextType::Weather, .weatherId = weatherId }, show);
}

namespace
{
	using CopyGroupKey = std::tuple<std::string, std::vector<std::string>, std::string,
		std::int8_t, std::uint8_t, SceneSettingsManager::SettingControlType>;
	using EffectiveCopyEntries = std::map<SceneSettingsManager::SettingIdentity,
		const SceneSettingsManager::SettingEntry*>;
	using PeriodCopyIdentity = std::pair<SceneSettingsManager::TimeOfDayPeriod,
		SceneSettingsManager::SettingIdentity>;
	using PeriodCopyEntries = std::map<PeriodCopyIdentity,
		const SceneSettingsManager::SettingEntry*>;

	bool IsAllPeriodsContext(const SceneSettingsManager::SceneContextId& context)
	{
		return context.allPeriods &&
		       (context.type == SceneSettingsManager::SceneContextType::TimeOfDay ||
				   context.type == SceneSettingsManager::SceneContextType::Weather ||
				   context.type == SceneSettingsManager::SceneContextType::Location);
	}

	void AddEffectiveCopyEntry(EffectiveCopyEntries& entries,
		const SceneSettingsManager::SettingEntry& entry, bool preferLatestPeriod)
	{
		SceneSettingsManager::SettingIdentity identity{
			entry.featureShortName, entry.settingPath, entry.settingKey
		};
		auto existing = entries.find(identity);
		if (!preferLatestPeriod || existing == entries.end() ||
			existing->second->source != entry.source ||
			static_cast<int>(existing->second->period) < static_cast<int>(entry.period))
			entries[std::move(identity)] = &entry;
	}

	void AddPeriodCopyEntry(PeriodCopyEntries& entries,
		const SceneSettingsManager::SettingEntry& entry)
	{
		SceneSettingsManager::SettingIdentity identity{
			entry.featureShortName, entry.settingPath, entry.settingKey
		};
		entries.try_emplace({ entry.period, std::move(identity) }, &entry);
	}

	CopyGroupKey GetCopyGroupKey(const SceneSettingsManager::SettingIdentity& identity)
	{
		SceneSettingsManager::SettingEntry entry{
			.featureShortName = identity.featureShortName,
			.settingPath = identity.settingPath,
			.settingKey = identity.settingKey,
		};
		SceneSettingsManager::SettingControlInfo info;
		const bool aggregate = SceneSettingsManager::GetSettingControlInfo(entry, info) &&
		                       info.controlType != SceneSettingsManager::SettingControlType::Scalar;
		return { identity.featureShortName,
			aggregate ? info.settingPath : identity.settingPath,
			aggregate ? info.settingKey : identity.settingKey,
			aggregate ? info.componentStart : -1,
			aggregate ? info.componentCount : 0,
			aggregate ? info.controlType : SceneSettingsManager::SettingControlType::Scalar };
	}

	std::string GetCopySettingDisplayName(const SceneSettingsManager::SettingIdentity& identity)
	{
		SceneSettingsManager::SettingEntry entry{
			.featureShortName = identity.featureShortName,
			.settingPath = identity.settingPath,
			.settingKey = identity.settingKey,
		};
		SceneSettingsManager::SettingControlInfo info;
		std::string settingLabel;
		if (SceneSettingsManager::GetSettingControlInfo(entry, info))
			settingLabel = JoinDisplayParts(info.tableDisplayPath, info.displayName);
		else
			settingLabel = GetSceneSettingDisplayName(
				identity.featureShortName, identity.settingPath, identity.settingKey);
		return JoinDisplayParts(
			{ SceneSettingsManager::GetFeatureDisplayName(identity.featureShortName) }, settingLabel);
	}

	bool IsValidCopyConflictPolicy(SceneSettingsManager::CopyConflictPolicy policy)
	{
		return policy == SceneSettingsManager::CopyConflictPolicy::SkipExisting ||
		       policy == SceneSettingsManager::CopyConflictPolicy::OverwriteExisting ||
		       policy == SceneSettingsManager::CopyConflictPolicy::Cancel;
	}

	const char* GetCopyPeriodName(SceneSettingsManager::TimeOfDayPeriod period)
	{
		switch (period) {
		case SceneSettingsManager::TimeOfDayPeriod::Dawn:
			return T("feature.scene_manager.period.dawn", "Dawn");
		case SceneSettingsManager::TimeOfDayPeriod::Sunrise:
			return T("feature.scene_manager.period.sunrise", "Sunrise");
		case SceneSettingsManager::TimeOfDayPeriod::Day:
			return T("feature.scene_manager.period.day", "Day");
		case SceneSettingsManager::TimeOfDayPeriod::Sunset:
			return T("feature.scene_manager.period.sunset", "Sunset");
		case SceneSettingsManager::TimeOfDayPeriod::Dusk:
			return T("feature.scene_manager.period.dusk", "Dusk");
		case SceneSettingsManager::TimeOfDayPeriod::Night:
			return T("feature.scene_manager.period.night", "Night");
		default:
			return "";
		}
	}

	const char* GetCopyLocationTypeName(SceneSettingsManager::LocationTargetType type)
	{
		switch (type) {
		case SceneSettingsManager::LocationTargetType::Worldspace:
			return T("feature.scene_manager.location.target_worldspace", "Worldspace");
		case SceneSettingsManager::LocationTargetType::Region:
			return T("feature.scene_manager.location.target_region", "Region");
		case SceneSettingsManager::LocationTargetType::LocationType:
			return T("feature.scene_manager.location.target_location_type", "Location Type");
		case SceneSettingsManager::LocationTargetType::Location:
			return T("feature.scene_manager.location.target_location", "Location");
		case SceneSettingsManager::LocationTargetType::Cell:
			return T("feature.scene_manager.location.target_cell", "Cell");
		default:
			return "";
		}
	}

	bool EntryBelongsToContext(const SceneSettingsManager::SettingEntry& entry,
		const SceneSettingsManager::SceneContextId& context)
	{
		switch (context.type) {
		case SceneSettingsManager::SceneContextType::Interior:
			return entry.period == SceneSettingsManager::TimeOfDayPeriod::Count;
		case SceneSettingsManager::SceneContextType::TimeOfDay:
			if (context.allPeriods) {
				const auto periodIndex = static_cast<int>(entry.period);
				return periodIndex >= 0 && periodIndex < SceneSettingsManager::kPeriodCount;
			}
			return entry.period == context.period;
		case SceneSettingsManager::SceneContextType::Weather:
		case SceneSettingsManager::SceneContextType::Location:
			return context.allPeriods ? entry.period >= SceneSettingsManager::TimeOfDayPeriod::Dawn &&
			                                entry.period < SceneSettingsManager::TimeOfDayPeriod::Count :
			                            entry.period == context.period;
		default:
			return false;
		}
	}

	bool CopyContextRequiresNumeric(SceneSettingsManager::SceneContextType type)
	{
		return type == SceneSettingsManager::SceneContextType::TimeOfDay ||
		       type == SceneSettingsManager::SceneContextType::Weather;
	}

	bool CopyContextRequiresNumeric(const SceneSettingsManager::SceneContextId& context)
	{
		return CopyContextRequiresNumeric(context.type) ||
		       (context.type == SceneSettingsManager::SceneContextType::Location &&
				   (context.allPeriods || context.period != SceneSettingsManager::TimeOfDayPeriod::Count));
	}

	SceneSettingsManager::SceneType GetCopyContextSceneType(
		SceneSettingsManager::SceneContextType type)
	{
		switch (type) {
		case SceneSettingsManager::SceneContextType::Interior:
			return SceneSettingsManager::SceneType::InteriorOnly;
		case SceneSettingsManager::SceneContextType::TimeOfDay:
		case SceneSettingsManager::SceneContextType::Weather:
			return SceneSettingsManager::SceneType::TimeOfDay;
		case SceneSettingsManager::SceneContextType::Location:
			return SceneSettingsManager::SceneType::Location;
		default:
			assert(false);
			return SceneSettingsManager::SceneType::InteriorOnly;
		}
	}

	template <class Form>
	std::string GetLocationTargetDisplayName(const Form* form)
	{
		if (const char* fullName = form->GetFullName(); fullName && fullName[0] != '\0')
			return std::string(fullName);
		return Util::GetFormDisplayName(form->GetFormID());
	}

	constexpr std::string_view kLocationTypePrefix = "LocType";

	bool IsLocationTypeKeyword(const RE::BGSKeyword* keyword)
	{
		return keyword && Util::GetFormEditorID(keyword).starts_with(kLocationTypePrefix);
	}

	std::string GetLocationTypeDisplayName(const RE::BGSKeyword* keyword)
	{
		if (!keyword)
			return {};
		const auto editorId = Util::GetFormEditorID(keyword);
		auto name = Util::PrettifyIdentifier(editorId.substr(kLocationTypePrefix.size()));
		return name.empty() ? Util::GetFormDisplayName(keyword->GetFormID()) : std::move(name);
	}

	std::vector<std::string> GetLocationTypeLabels(const RE::BGSLocation* location)
	{
		std::vector<std::string> labels;
		if (!location)
			return labels;

		std::set<RE::FormID> seenKeywords;
		for (auto* keyword : location->GetKeywords()) {
			if (!keyword || !seenKeywords.insert(keyword->GetFormID()).second)
				continue;
			if (!IsLocationTypeKeyword(keyword))
				continue;
			auto label = GetLocationTypeDisplayName(keyword);
			if (!label.empty() && std::ranges::find(labels, label) == labels.end())
				labels.push_back(std::move(label));
		}
		std::ranges::sort(labels);
		return labels;
	}

	SceneSettingsManager::LocationTarget MakeWorldspaceTarget(RE::TESWorldSpace* worldspace)
	{
		return {
			.type = SceneSettingsManager::LocationTargetType::Worldspace,
			.formKey = Util::GetFormFileKey(worldspace),
			.name = GetLocationTargetDisplayName(worldspace),
			.formId = worldspace->GetFormID(),
		};
	}

	std::vector<SceneSettingsManager::LocationTarget> BuildLocationTargetChain(
		RE::BGSLocation* location, RE::TESObjectCELL* cell)
	{
		if (!cell && location && location->worldLocMarker) {
			if (auto marker = location->worldLocMarker.get())
				cell = marker->GetParentCell();
		}
		const auto cocCode = cell ? Util::GetFormEditorID(cell) : std::string{};

		std::vector<SceneSettingsManager::LocationTarget> targets;
		if (cell && cell->IsExteriorCell()) {
			if (auto* worldspace = cell->GetRuntimeData().worldSpace)
				targets.push_back(MakeWorldspaceTarget(worldspace));
		}
		std::vector<RE::BGSKeyword*> locationTypes;
		std::set<RE::FormID> seenLocationTypes;
		if (location) {
			for (auto* keyword : location->GetKeywords()) {
				if (IsLocationTypeKeyword(keyword) &&
					seenLocationTypes.insert(keyword->GetFormID()).second)
					locationTypes.push_back(keyword);
			}
		}
		std::ranges::sort(locationTypes, [](const auto* lhs, const auto* rhs) {
			return NormalizeLocationFormKey(Util::GetFormFileKey(lhs)) <
			       NormalizeLocationFormKey(Util::GetFormFileKey(rhs));
		});
		for (auto* locationType : locationTypes) {
			targets.push_back({
				.type = SceneSettingsManager::LocationTargetType::LocationType,
				.formKey = Util::GetFormFileKey(locationType),
				.name = GetLocationTypeDisplayName(locationType),
				.formId = locationType->GetFormID(),
			});
		}

		RE::TESRegion* region = nullptr;
		if (cell && cell->IsExteriorCell()) {
			if (auto* player = globals::game::player;
				player && player->GetParentCell() == cell && globals::game::sky)
				region = globals::game::sky->region;
			if (!region) {
				if (auto* regions = cell->GetRegionList(false)) {
					for (auto* candidate : *regions) {
						if (!candidate)
							continue;
						region = candidate;
						break;
					}
				}
			}
		}
		if (region) {
			targets.push_back({
				.type = SceneSettingsManager::LocationTargetType::Region,
				.formKey = Util::GetFormFileKey(region),
				.name = Util::GetFormDisplayName(region->GetFormID()),
				.cocCode = cocCode,
				.formId = region->GetFormID(),
			});
		}

		if (location) {
			targets.push_back({
				.type = SceneSettingsManager::LocationTargetType::Location,
				.formKey = Util::GetFormFileKey(location),
				.name = GetLocationTargetDisplayName(location),
				.cocCode = cocCode,
				.locationTypes = GetLocationTypeLabels(location),
				.formId = location->GetFormID(),
			});
		}
		if (cell) {
			targets.push_back({
				.type = SceneSettingsManager::LocationTargetType::Cell,
				.formKey = Util::GetFormFileKey(cell),
				.name = GetLocationTargetDisplayName(cell),
				.cocCode = cocCode,
				.formId = cell->GetFormID(),
			});
		}
		return targets;
	}

	RE::TESForm* ResolveLocationTargetForm(std::string_view formKey)
	{
		const auto parsed = Util::ParseSpid(std::string(formKey));
		if (parsed.localFormId == 0)
			return nullptr;
		const auto formId = parsed.pluginName.empty() ? parsed.localFormId :
		                                                Util::SpidToFormId(std::string(formKey));
		return formId != 0 ? RE::TESForm::LookupByID(formId) : nullptr;
	}

	std::optional<bool> GetLocationChainInteriorState(std::span<const SceneSettingsManager::LocationTarget> targets)
	{
		for (const auto& target : targets) {
			if (target.type == SceneSettingsManager::LocationTargetType::Worldspace)
				return false;
			if (target.type == SceneSettingsManager::LocationTargetType::Cell)
				if (auto* form = ResolveLocationTargetForm(target.formKey))
					if (auto* cell = form->As<RE::TESObjectCELL>())
						return cell->IsInteriorCell();
		}
		return std::nullopt;
	}

	std::vector<SceneSettingsManager::LocationTarget> ResolveLocationTargetChain(
		SceneSettingsManager::LocationTargetType type, std::string_view formKey)
	{
		if (type != SceneSettingsManager::LocationTargetType::LocationType) {
			if (auto* manager = SceneSettingsManager::GetSingleton()) {
				const auto& currentTargets = manager->GetCurrentLocationTargets();
				const auto normalizedKey = NormalizeLocationFormKey(formKey);
				if (std::any_of(currentTargets.begin(), currentTargets.end(), [&](const auto& target) {
						return target.type == type && NormalizeLocationFormKey(target.formKey) == normalizedKey;
					}))
					return currentTargets;
			}
		}
		auto* form = ResolveLocationTargetForm(formKey);
		if (!form)
			return {};
		switch (type) {
		case SceneSettingsManager::LocationTargetType::Worldspace:
			{
				auto* worldspace = form->As<RE::TESWorldSpace>();
				return worldspace ? std::vector{ MakeWorldspaceTarget(worldspace) } :
				                    std::vector<SceneSettingsManager::LocationTarget>{};
			}
		case SceneSettingsManager::LocationTargetType::Region:
			{
				auto* region = form->As<RE::TESRegion>();
				if (!region)
					return {};
				std::vector<SceneSettingsManager::LocationTarget> targets;
				if (region->worldSpace)
					targets.push_back(MakeWorldspaceTarget(region->worldSpace));
				targets.push_back({
					.type = SceneSettingsManager::LocationTargetType::Region,
					.formKey = Util::GetFormFileKey(region),
					.name = Util::GetFormDisplayName(region->GetFormID()),
					.formId = region->GetFormID(),
				});
				return targets;
			}
		case SceneSettingsManager::LocationTargetType::LocationType:
			{
				auto* keyword = form->As<RE::BGSKeyword>();
				return IsLocationTypeKeyword(keyword) ?
				           std::vector<SceneSettingsManager::LocationTarget>{ {
							   .type = SceneSettingsManager::LocationTargetType::LocationType,
							   .formKey = Util::GetFormFileKey(keyword),
							   .name = GetLocationTypeDisplayName(keyword),
							   .formId = keyword->GetFormID(),
						   } } :
				           std::vector<SceneSettingsManager::LocationTarget>{};
			}
		case SceneSettingsManager::LocationTargetType::Location:
			return BuildLocationTargetChain(form->As<RE::BGSLocation>(), nullptr);
		case SceneSettingsManager::LocationTargetType::Cell:
			{
				auto* cell = form->As<RE::TESObjectCELL>();
				return cell ? BuildLocationTargetChain(cell->GetLocation(), cell) :
				              std::vector<SceneSettingsManager::LocationTarget>{};
			}
		default:
			return {};
		}
	}

}

bool SceneSettingsManager::IsValidSceneContext(const SceneContextId& context)
{
	const auto periodIndex = static_cast<int>(context.period);
	switch (context.type) {
	case SceneContextType::Interior:
		return !context.allPeriods && context.period == TimeOfDayPeriod::Count && context.weatherId == 0 &&
		       context.locationFormKey.empty() &&
		       context.locationType == LocationTargetType::Location;
	case SceneContextType::TimeOfDay:
		return ((context.allPeriods && context.period == TimeOfDayPeriod::Count) ||
				   (!context.allPeriods && periodIndex >= 0 && periodIndex < kPeriodCount)) &&
		       context.weatherId == 0 &&
		       context.locationFormKey.empty() &&
		       context.locationType == LocationTargetType::Location;
	case SceneContextType::Weather:
		return context.weatherId != 0 &&
		       ((context.allPeriods && context.period == TimeOfDayPeriod::Count) ||
				   (!context.allPeriods && periodIndex >= 0 && periodIndex <= kPeriodCount)) &&
		       context.locationFormKey.empty() &&
		       context.locationType == LocationTargetType::Location;
	case SceneContextType::Location:
		return ((!context.allPeriods && periodIndex >= 0 && periodIndex <= kPeriodCount) ||
				   (context.allPeriods && context.period == TimeOfDayPeriod::Count)) &&
		       context.weatherId == 0 &&
		       IsValidLocationTargetType(context.locationType) &&
		       !context.locationFormKey.empty();
	default:
		return false;
	}
}

bool SceneSettingsManager::IsFeatureSceneEditContextValid(
	std::string_view featureShortName, const SceneContextId& context)
{
	if (!IsValidSceneContext(context) || context.allPeriods)
		return false;
	if (!IsFeatureAllowedForType(GetCopyContextSceneType(context.type), std::string(featureShortName)))
		return false;
	if (context.type == SceneContextType::Weather) {
		const bool showTimeOfDay = IsWeatherShowTimeOfDay(context.weatherId);
		if (showTimeOfDay != (context.period != TimeOfDayPeriod::Count))
			return false;
	}
	if (context.type != SceneContextType::Location)
		return true;
	if (IsLocationShowTimeOfDay(context.locationType, context.locationFormKey) !=
		(context.period != TimeOfDayPeriod::Count))
		return false;

	const auto normalizedFormKey = NormalizeLocationFormKey(context.locationFormKey);
	return std::ranges::any_of(GetCurrentLocationTargets(), [&](const auto& target) {
		return target.type == context.locationType &&
		       NormalizeLocationFormKey(target.formKey) == normalizedFormKey;
	});
}

bool SceneSettingsManager::SnapshotFeatureSceneEdit(Feature& feature, json& snapshot) const
{
	try {
		feature.SaveSettings(snapshot);
	} catch (const std::exception& e) {
		logger::warn("[SceneSettings] Could not snapshot feature-page edits for {}: {}",
			feature.GetShortName(), e.what());
		return false;
	} catch (...) {
		logger::warn("[SceneSettings] Could not snapshot feature-page edits for {}",
			feature.GetShortName());
		return false;
	}
	return snapshot.is_object();
}

bool SceneSettingsManager::BeginFeatureSceneEdit(Feature* feature, const SceneContextId& context)
{
	if (!feature || !IsFeatureSceneEditContextValid(feature->GetShortName(), context))
		return false;
	if (featureSceneEdit && featureSceneEdit->featureShortName != feature->GetShortName() &&
		HasPendingFeatureSceneEdits())
		return false;
	if (featureSceneEdit && featureSceneEdit->featureShortName == feature->GetShortName() &&
		featureSceneEdit->context == context)
		return true;
	if (featureSceneEdit) {
		if (HasPendingFeatureSceneEdits())
			return false;
		EndFeatureSceneEdit(false);
	}

	const auto featureShortName = feature->GetShortName();
	if (!GetFeatureBaseSnapshot(featureShortName))
		return false;
	const auto sceneType = GetCopyContextSceneType(context.type);
	std::vector<SettingAddress> editableAddresses;
	for (const auto& setting : GetCatalogFeatureSettings(featureShortName)) {
		auto settingPath = SplitCatalogPath(setting.settingPath);
		if (!IsSettingAllowedForType(
				sceneType, featureShortName, settingPath, std::string(setting.settingKey)))
			continue;
		if (CopyContextRequiresNumeric(context) && !FindAllowedCatalogSetting(featureShortName, settingPath, std::string(setting.settingKey), true))
			continue;
		editableAddresses.push_back({ featureShortName, std::move(settingPath), std::string(setting.settingKey) });
	}
	std::ranges::sort(editableAddresses);
	const auto uniqueEnd = std::ranges::unique(editableAddresses).begin();
	editableAddresses.erase(uniqueEnd, editableAddresses.end());
	EnsureBaselines(editableAddresses);

	featureSceneEdit.emplace();
	++featureSceneEditRevision;
	featureSceneEdit->featureShortName = featureShortName;
	featureSceneEdit->context = context;
	featureSceneEdit->editableAddresses = std::move(editableAddresses);
	pendingApplyVerifications.erase(featureSceneEdit->featureShortName);
	featureApplyDocuments.erase(featureSceneEdit->featureShortName);
	resolverDirty = true;
	ResolveAndApply(true);

	json snapshot;
	if (!SnapshotFeatureSceneEdit(*feature, snapshot)) {
		featureSceneEdit.reset();
		resolverDirty = true;
		ResolveAndApply(true);
		return false;
	}
	featureSceneEdit->originalSettings = snapshot;
	featureSceneEdit->workingSettings = std::move(snapshot);
	featureApplyDocuments[featureSceneEdit->featureShortName] = featureSceneEdit->workingSettings;
	return true;
}

bool SceneSettingsManager::IsFeatureSceneEditing(std::string_view featureShortName) const
{
	return featureSceneEdit && featureSceneEdit->featureShortName == featureShortName;
}

bool SceneSettingsManager::CanCaptureFeatureSceneEdit(std::string_view featureShortName) const
{
	return IsFeatureSceneEditing(featureShortName) && IsFeatureSceneEditPreviewActive();
}

bool SceneSettingsManager::IsFeatureSceneEditSetting(std::string_view featureShortName,
	std::string_view settingPath, std::string_view settingKey) const
{
	return featureSceneEdit && featureSceneEdit->featureShortName == featureShortName &&
	       std::ranges::binary_search(featureSceneEdit->editableAddresses,
			   SettingAddress{ std::string(featureShortName), SplitCatalogPath(settingPath),
				   std::string(settingKey) });
}

bool SceneSettingsManager::HasPendingFeatureSceneEdits() const
{
	return featureSceneEdit && featureSceneEdit->dirty;
}

void SceneSettingsManager::SetFeatureSceneEditOverwritesPaused(bool paused)
{
	if (!featureSceneEdit || featureSceneEdit->overwritesPaused == paused)
		return;
	featureSceneEdit->overwritesPaused = paused;
	++featureSceneEditRevision;
	ReapplyIfActive(false);
}

bool SceneSettingsManager::AreFeatureSceneEditOverwritesPaused() const
{
	return featureSceneEdit && featureSceneEdit->overwritesPaused;
}

bool SceneSettingsManager::AreFeatureSceneEditActionsLocked() const
{
	return !IsSceneReady() || (HasFeatureSceneEditOverwrites() && !AreFeatureSceneEditOverwritesPaused());
}

bool SceneSettingsManager::HasFeatureSceneEditOverwrites(const SceneContextId* context, bool matchPeriod, bool wholeType) const
{
	if (!IsFeatureSceneEditPreviewActive() || featureSceneEdit->overwriteAddresses.empty())
		return false;
	if (!context)
		return true;
	auto selected = featureSceneEdit->context;
	if (wholeType)
		return selected.type == context->type;
	if (!matchPeriod || context->allPeriods) {
		selected.period = context->period;
		selected.allPeriods = context->allPeriods;
	}
	return selected == *context;
}

bool SceneSettingsManager::IsFeatureSceneEditSettingOverwritten(std::string_view featureShortName,
	std::string_view settingPath, std::string_view settingKey) const
{
	return CanCaptureFeatureSceneEdit(featureShortName) && !featureSceneEdit->overwritesPaused &&
	       featureSceneEdit->overwriteAddresses.contains(
			   { std::string(featureShortName), SplitCatalogPath(settingPath), std::string(settingKey) });
}

bool SceneSettingsManager::IsFeatureSceneEditSettingAltered(std::string_view featureShortName,
	std::string_view settingPath, std::string_view settingKey) const
{
	return CanCaptureFeatureSceneEdit(featureShortName) && featureSceneEdit->alteredAddresses.contains(
															   { std::string(featureShortName), SplitCatalogPath(settingPath), std::string(settingKey) });
}

void SceneSettingsManager::RefreshFeatureSceneEditOverrides(json liveSettings)
{
	if (!featureSceneEdit)
		return;

	struct EditGroup
	{
		bool changed = false;
		std::vector<std::pair<SettingAddress, json>> values;
	};
	std::map<CopyGroupKey, EditGroup> groups;
	const auto sceneType = GetCopyContextSceneType(featureSceneEdit->context.type);
	for (const auto& setting : GetCatalogFeatureSettings(featureSceneEdit->featureShortName)) {
		if (!IsFeatureSceneEditSetting(featureSceneEdit->featureShortName, setting.settingPath, setting.settingKey))
			continue;
		auto settingPath = SplitCatalogPath(setting.settingPath);
		if (!IsSettingAllowedForType(
				sceneType, featureSceneEdit->featureShortName, settingPath, std::string(setting.settingKey)))
			continue;
		const auto* original = GetCatalogSerializedValue(featureSceneEdit->originalSettings, setting);
		const auto* working = GetCatalogSerializedValue(featureSceneEdit->workingSettings, setting);
		if (!original || !working || !IsSceneSettingPrimitive(*original) ||
			!IsSceneSettingPrimitive(*working) || !IsCompatibleSceneSettingValue(*original, *working))
			continue;

		SettingIdentity identity{
			featureSceneEdit->featureShortName, std::move(settingPath), std::string(setting.settingKey)
		};
		auto& group = groups[GetCopyGroupKey(identity)];
		group.changed |= !AppliedValuesEqual(*working, *original);
		group.values.emplace_back(SettingAddress{
									  identity.featureShortName, identity.settingPath, identity.settingKey },
			*working);
	}

	auto previousOverrides = std::move(featureSceneEdit->workingOverrides);
	featureSceneEdit->workingOverrides.clear();
	for (auto& [_, group] : groups)
		if (group.changed)
			for (auto& [address, value] : group.values)
				featureSceneEdit->workingOverrides.insert_or_assign(std::move(address), std::move(value));
	featureSceneEdit->dirty = !featureSceneEdit->workingOverrides.empty();
	if (previousOverrides != featureSceneEdit->workingOverrides)
		resolverDirty = true;

	std::vector<SettingAddress> changedAddresses;
	changedAddresses.reserve(previousOverrides.size() + featureSceneEdit->workingOverrides.size());
	for (const auto& [address, _] : previousOverrides)
		changedAddresses.push_back(address);
	for (const auto& [address, _] : featureSceneEdit->workingOverrides)
		changedAddresses.push_back(address);
	std::sort(changedAddresses.begin(), changedAddresses.end());
	changedAddresses.erase(std::unique(changedAddresses.begin(), changedAddresses.end()), changedAddresses.end());
	EnsureBaselines(changedAddresses);
	for (const auto& address : changedAddresses) {
		auto* setting = FindAllowedCatalogSetting(
			address.featureShortName, address.settingPath, address.settingKey);
		const auto* value = setting ?
		                        GetCatalogSerializedValue(liveSettings, *setting) :
		                        nullptr;
		if (value && IsSceneSettingPrimitive(*value))
			appliedSettings[address] = *value;
	}
	if (!changedAddresses.empty())
		appliedFeatureNames.insert(featureSceneEdit->featureShortName);
	pendingApplyVerifications.erase(featureSceneEdit->featureShortName);
	featureApplyDocuments[featureSceneEdit->featureShortName] = std::move(liveSettings);
}

void SceneSettingsManager::RebaseFeatureSceneEditPreview()
{
	if (!featureSceneEdit)
		return;
	const auto document = featureApplyDocuments.find(featureSceneEdit->featureShortName);
	if (document == featureApplyDocuments.end())
		return;

	const auto sceneType = GetCopyContextSceneType(featureSceneEdit->context.type);
	for (const auto& setting : GetCatalogFeatureSettings(featureSceneEdit->featureShortName)) {
		if (!IsFeatureSceneEditSetting(featureSceneEdit->featureShortName, setting.settingPath, setting.settingKey))
			continue;
		auto settingPath = SplitCatalogPath(setting.settingPath);
		if (!IsSettingAllowedForType(sceneType, featureSceneEdit->featureShortName,
				settingPath, std::string(setting.settingKey)))
			continue;
		const SettingAddress address{
			featureSceneEdit->featureShortName, std::move(settingPath), std::string(setting.settingKey)
		};
		if (featureSceneEdit->workingOverrides.contains(address))
			continue;
		const auto* presented = GetCatalogSerializedValue(document->second, setting);
		auto* original = GetCatalogSerializedValue(featureSceneEdit->originalSettings, setting);
		auto* working = GetCatalogSerializedValue(featureSceneEdit->workingSettings, setting);
		if (!presented || !original || !working || !IsSceneSettingPrimitive(*presented) ||
			!IsCompatibleSceneSettingValue(*original, *presented) ||
			!IsCompatibleSceneSettingValue(*working, *presented))
			continue;
		*original = *presented;
		*working = *presented;
	}
}

bool SceneSettingsManager::CaptureFeatureSceneEditChanges(Feature* feature)
{
	if (!featureSceneEdit || !feature ||
		!CanCaptureFeatureSceneEdit(feature->GetShortName()))
		return false;
	json snapshot;
	if (!SnapshotFeatureSceneEdit(*feature, snapshot))
		return false;

	auto document = featureApplyDocuments.find(featureSceneEdit->featureShortName);
	auto sanitized = document != featureApplyDocuments.end() ?
	                     document->second :
	                     featureSceneEdit->workingSettings;
	const auto sceneType = GetCopyContextSceneType(featureSceneEdit->context.type);
	for (const auto& setting : GetCatalogFeatureSettings(featureSceneEdit->featureShortName)) {
		if (!IsFeatureSceneEditSetting(featureSceneEdit->featureShortName, setting.settingPath, setting.settingKey))
			continue;
		auto settingPath = SplitCatalogPath(setting.settingPath);
		if (!IsSettingAllowedForType(sceneType, featureSceneEdit->featureShortName,
				settingPath, std::string(setting.settingKey)))
			continue;
		const auto* source = GetCatalogSerializedValue(snapshot, setting);
		auto* destination = GetCatalogSerializedValue(sanitized, setting);
		if (!IsFeatureSceneEditSettingOverwritten(featureSceneEdit->featureShortName, setting.settingPath, setting.settingKey) &&
			source && destination && IsSceneSettingPrimitive(*source) &&
			IsCompatibleSceneSettingValue(*destination, *source))
			*destination = *source;
	}
	if (sanitized != snapshot) {
		try {
			feature->LoadSettings(sanitized);
		} catch (const std::exception& e) {
			logger::warn("[SceneSettings] Could not restore non-scene settings for {}: {}",
				feature->GetShortName(), e.what());
			return false;
		} catch (...) {
			logger::warn("[SceneSettings] Could not restore non-scene settings for {}",
				feature->GetShortName());
			return false;
		}
	}
	featureSceneEdit->workingSettings = sanitized;
	if (document != featureApplyDocuments.end()) {
		for (const auto& [address, pendingValue] : featureSceneEdit->workingOverrides) {
			auto* setting = FindAllowedCatalogSetting(
				address.featureShortName, address.settingPath, address.settingKey);
			if (!setting)
				continue;
			const auto* presented = GetCatalogSerializedValue(document->second, *setting);
			const auto* current = GetCatalogSerializedValue(sanitized, *setting);
			auto* working = GetCatalogSerializedValue(featureSceneEdit->workingSettings, *setting);
			if (presented && current && working && AppliedValuesEqual(*current, *presented))
				*working = pendingValue;
		}
	}
	RefreshFeatureSceneEditOverrides(std::move(sanitized));
	return true;
}

bool SceneSettingsManager::StoreFeatureSceneEdit()
{
	if (!featureSceneEdit)
		return false;
	auto* feature = Feature::FindFeatureByShortName(featureSceneEdit->featureShortName);
	if (!feature)
		return false;
	if (IsFeatureSceneEditPreviewActive() && !CaptureFeatureSceneEditChanges(feature))
		return false;
	if (!featureSceneEdit->dirty)
		return true;

	const auto context = featureSceneEdit->context;
	const auto sceneType = GetCopyContextSceneType(context.type);
	const bool requireNumeric = CopyContextRequiresNumeric(context);
	const std::vector destinationPeriods{ context.period };

	struct PendingEdit
	{
		SettingAddress address;
		json value;
		TimeOfDayPeriod period = TimeOfDayPeriod::Count;
		json originalValue;
	};
	std::vector<PendingEdit> pending;
	for (const auto& [address, value] : featureSceneEdit->workingOverrides) {
		const auto valuePeriods = IsNumericValue(value) ? destinationPeriods : std::vector{ TimeOfDayPeriod::Count };
		for (const auto period : valuePeriods) {
			if (!ValidateSceneSettingEntry(GetSceneTypeName(sceneType), sceneType,
					address.featureShortName, address.settingPath, address.settingKey, value, requireNumeric))
				return false;

			json originalValue;
			if (context.type == SceneContextType::Weather) {
				auto lower = ResolveWeatherLowerValue(
					context.weatherId, address, period, EntrySource::User);
				if (lower)
					originalValue = *lower;
			} else if (context.type == SceneContextType::Location) {
				if (auto* setting = FindAllowedCatalogSetting(
						address.featureShortName, address.settingPath, address.settingKey)) {
					if (const auto* original = GetCatalogSerializedValue(
							featureSceneEdit->originalSettings, *setting))
						originalValue = *original;
				}
			} else {
				originalValue = GetBaselineValue(address);
			}
			if (!IsSceneSettingPrimitive(originalValue))
				return false;
			pending.push_back({ address, value, period, std::move(originalValue) });
		}
	}
	if (pending.empty())
		return true;

	std::vector<SettingEntry>* destinationEntries = nullptr;
	std::optional<LocationTarget> locationTarget;
	if (context.type == SceneContextType::Interior) {
		destinationEntries = &GetEntriesMut(SceneType::InteriorOnly);
	} else if (context.type == SceneContextType::TimeOfDay) {
		destinationEntries = &GetEntriesMut(SceneType::TimeOfDay);
	} else if (context.type == SceneContextType::Weather) {
		destinationEntries = &GetWeatherConfigMut(context.weatherId).entries;
	} else {
		const auto normalizedFormKey = NormalizeLocationFormKey(context.locationFormKey);
		const auto& managementTargets = GetLocationManagementTargets();
		auto target = std::ranges::find_if(managementTargets, [&](const auto& candidate) {
			return candidate.type == context.locationType &&
			       NormalizeLocationFormKey(candidate.formKey) == normalizedFormKey;
		});
		if (target == managementTargets.end())
			return false;
		locationTarget = *target;
		auto& config = GetLocationConfigMut(
			context.locationType, context.locationFormKey, locationTarget->name);
		if (!locationTarget->cocCode.empty())
			config.cocCode = locationTarget->cocCode;
		destinationEntries = &config.entries;
	}

	auto previousEntries = *destinationEntries;
	for (auto& edit : pending) {
		auto existing = std::ranges::find_if(*destinationEntries, [&](const auto& entry) {
			return entry.source == EntrySource::User && entry.period == edit.period &&
			       IsSameSetting(entry, edit.address.featureShortName,
					   edit.address.settingPath, edit.address.settingKey);
		});
		if (existing != destinationEntries->end()) {
			existing->value = edit.value;
			continue;
		}
		const auto matchingEntry = [&](const auto& entry) {
			return entry.period == edit.period && IsSameSetting(entry, edit.address.featureShortName, edit.address.settingPath, edit.address.settingKey);
		};
		const bool wasPaused = std::ranges::any_of(*destinationEntries, matchingEntry) &&
		                       std::ranges::all_of(*destinationEntries, [&](const auto& entry) { return !matchingEntry(entry) || entry.paused; });
		destinationEntries->push_back({
			.featureShortName = edit.address.featureShortName,
			.settingPath = edit.address.settingPath,
			.settingKey = edit.address.settingKey,
			.displayName = GetSceneSettingDisplayName(edit.address.featureShortName,
				edit.address.settingPath, edit.address.settingKey),
			.value = edit.value,
			.originalValue = std::move(edit.originalValue),
			.paused = wasPaused,
			.source = EntrySource::User,
			.period = edit.period,
		});
	}

	if (requireNumeric)
		++sceneValueRevision;
	BumpEntryPresentationRevision();
	switch (context.type) {
	case SceneContextType::Interior:
		MarkEntryListUserSettingsModified(SceneType::InteriorOnly);
		break;
	case SceneContextType::TimeOfDay:
		MarkEntryListUserSettingsModified(SceneType::TimeOfDay);
		break;
	case SceneContextType::Weather:
		PrepareWeatherUserSettingsMutation(context.weatherId, true);
		break;
	case SceneContextType::Location:
		PrepareLocationUserSettingsMutation(
			context.locationType, context.locationFormKey, true);
		break;
	}

	if (!SaveAllUserSettings()) {
		*destinationEntries = std::move(previousEntries);
		return false;
	}
	featureSceneEdit->workingOverrides.clear();
	featureSceneEdit->dirty = false;
	ReapplyIfActive();
	json snapshot;
	if (SnapshotFeatureSceneEdit(*feature, snapshot)) {
		featureSceneEdit->originalSettings = snapshot;
		featureSceneEdit->workingSettings = std::move(snapshot);
		featureApplyDocuments[featureSceneEdit->featureShortName] = featureSceneEdit->workingSettings;
	}
	pendingApplyVerifications.erase(featureSceneEdit->featureShortName);
	return true;
}

void SceneSettingsManager::EndFeatureSceneEdit(bool storeChanges)
{
	if (!featureSceneEdit)
		return;
	if (storeChanges && !StoreFeatureSceneEdit()) {
		logger::warn("[SceneSettings] Could not store pending feature-page scene edits for {}",
			featureSceneEdit->featureShortName);
		return;
	}
	const auto featureShortName = featureSceneEdit->featureShortName;
	featureSceneEdit.reset();
	pendingApplyVerifications.erase(featureShortName);
	featureApplyDocuments.erase(featureShortName);
	for (auto applied = appliedSettings.lower_bound({ featureShortName, {}, {} });
		applied != appliedSettings.end() && applied->first.featureShortName == featureShortName;)
		applied = appliedSettings.erase(applied);
	appliedFeatureNames.erase(featureShortName);
	resolverDirty = true;
	ResolveAndApply(true);
}

const std::vector<SceneSettingsManager::SettingEntry>* SceneSettingsManager::GetCopyContextEntries(
	const SceneContextId& context) const
{
	if (!IsValidSceneContext(context))
		return nullptr;
	switch (context.type) {
	case SceneContextType::Interior:
		return &GetEntries(SceneType::InteriorOnly);
	case SceneContextType::TimeOfDay:
		return &GetEntries(SceneType::TimeOfDay);
	case SceneContextType::Weather:
		if (auto configIt = weatherSceneConfigs.find(context.weatherId);
			configIt != weatherSceneConfigs.end())
			return &configIt->second.entries;
		return nullptr;
	case SceneContextType::Location:
		if (auto configIt = locationSceneConfigs.find(
				GetLocationConfigKey(context.locationType, context.locationFormKey));
			configIt != locationSceneConfigs.end())
			return &configIt->second.entries;
		return nullptr;
	default:
		return nullptr;
	}
}

SceneSettingsManager::EntryLayerSummary SceneSettingsManager::GetFeatureSceneSummary(
	std::string_view featureShortName, const SceneContextId& context, bool matchPeriod) const
{
	EntryLayerSummary summary;
	const auto* config = GetPeriodicSceneConfig(context);
	if (const auto* contextEntries = GetCopyContextEntries(context))
		for (const auto& entry : *contextEntries) {
			if ((!featureShortName.empty() && entry.featureShortName != featureShortName) || (matchPeriod && !EntryBelongsToContext(entry, context)))
				continue;
			++summary.count;
			if (entry.paused)
				++summary.paused;
			if (entry.source == EntrySource::Overwrite && IsEntryActive(entry) && (!config || config->IsPeriodActive(entry.period)))
				++summary.activeOverwrites;
		}
	return summary;
}

SceneSettingsManager::EntryLayerSummary SceneSettingsManager::GetFeatureSceneSummary(
	std::string_view featureShortName, SceneContextType type) const
{
	EntryLayerSummary summary;
	const auto collect = [&](const SceneContextId& context) {
		const auto target = GetFeatureSceneSummary(featureShortName, context, false);
		summary.count += target.count;
		summary.paused += target.paused;
		summary.activeOverwrites += target.activeOverwrites;
	};
	switch (type) {
	case SceneContextType::Interior:
	case SceneContextType::TimeOfDay:
		collect({ .type = type, .allPeriods = type == SceneContextType::TimeOfDay });
		break;
	case SceneContextType::Weather:
		for (const auto& [weatherId, _] : weatherSceneConfigs)
			collect({ .type = type, .weatherId = weatherId });
		break;
	case SceneContextType::Location:
		for (const auto& [_, config] : locationSceneConfigs)
			collect({ .type = type, .locationType = config.type, .locationFormKey = config.formKey });
		break;
	}
	return summary;
}

bool SceneSettingsManager::IsCopyEntryCompatible(const SettingEntry& entry,
	SceneContextType destinationType) const
{
	const bool requireNumeric = CopyContextRequiresNumeric(destinationType);
	const auto* setting = FindAllowedCatalogSetting(
		entry.featureShortName, entry.settingPath, entry.settingKey, requireNumeric);
	return setting &&
	       IsSettingAllowedForType(GetCopyContextSceneType(destinationType),
			   entry.featureShortName, entry.settingPath, entry.settingKey) &&
	       IsSceneSettingValueAllowed(entry.value, *setting, entry.value, requireNumeric);
}

std::vector<SceneSettingsManager::CopyCandidate> SceneSettingsManager::BuildCopyCandidates(
	const SceneContextId& source, const SceneContextId& destination,
	EntrySource sourceLayer, std::span<const SettingIdentity> selectedSettings) const
{
	std::vector<CopyCandidate> candidates;
	if (!IsValidSceneContext(source) || !IsValidSceneContext(destination) || selectedSettings.empty())
		return candidates;
	if (source.allPeriods != destination.allPeriods)
		return candidates;
	if (source.allPeriods && (!IsAllPeriodsContext(source) || !IsAllPeriodsContext(destination)))
		return candidates;
	if (destination.type == SceneContextType::Location &&
		ResolveLocationTargetChain(destination.locationType, destination.locationFormKey).empty())
		return candidates;
	const auto sameContext = [&] {
		if (source.type != destination.type)
			return false;
		switch (source.type) {
		case SceneContextType::Interior:
			return true;
		case SceneContextType::TimeOfDay:
			return source.period == destination.period && source.allPeriods == destination.allPeriods;
		case SceneContextType::Weather:
			return source.weatherId == destination.weatherId && source.period == destination.period &&
			       source.allPeriods == destination.allPeriods;
		case SceneContextType::Location:
			return source.period == destination.period && source.allPeriods == destination.allPeriods &&
			       source.locationType == destination.locationType &&
			       NormalizeLocationFormKey(source.locationFormKey) ==
			           NormalizeLocationFormKey(destination.locationFormKey);
		default:
			return false;
		}
	}();
	if (sameContext && sourceLayer == EntrySource::User)
		return candidates;
	const auto* sourceEntries = GetCopyContextEntries(source);
	if (!sourceEntries)
		return candidates;
	const auto* destinationEntries = GetCopyContextEntries(destination);
	static const std::vector<SettingEntry> empty;
	if (!destinationEntries)
		destinationEntries = &empty;

	struct SourceCopyValue
	{
		SettingIdentity identity;
		const SettingEntry* entry = nullptr;
		TimeOfDayPeriod sourcePeriod = TimeOfDayPeriod::Count;
		TimeOfDayPeriod destinationPeriod = TimeOfDayPeriod::Count;
	};
	std::vector<SourceCopyValue> sourceValues;
	if (source.allPeriods) {
		PeriodCopyEntries periodEntries;
		for (const auto& entry : *sourceEntries)
			if (entry.source == sourceLayer && EntryBelongsToContext(entry, source))
				AddPeriodCopyEntry(periodEntries, entry);
		sourceValues.reserve(periodEntries.size());
		for (const auto& [periodIdentity, entry] : periodEntries)
			sourceValues.push_back({ periodIdentity.second, entry, periodIdentity.first, periodIdentity.first });
	} else {
		EffectiveCopyEntries effectiveEntries;
		for (const auto& entry : *sourceEntries)
			if (entry.source == sourceLayer && EntryBelongsToContext(entry, source))
				AddEffectiveCopyEntry(effectiveEntries, entry, false);
		sourceValues.reserve(effectiveEntries.size());
		const auto destinationPeriod = destination.period;
		for (const auto& [identity, entry] : effectiveEntries) {
			const auto targetPeriod = destination.type == SceneContextType::Location && IsNumericValue(entry->value) ?
			                              destination.period :
			                              destinationPeriod;
			sourceValues.push_back({ identity, entry, entry->period, targetPeriod });
		}
	}

	using PeriodSettingIdentity = std::pair<SettingIdentity, TimeOfDayPeriod>;
	std::set<PeriodSettingIdentity> destinationUserSettings;
	std::set<PeriodSettingIdentity> destinationOverwriteSettings;
	for (const auto& entry : *destinationEntries)
		if (EntryBelongsToContext(entry, destination)) {
			SettingIdentity identity{ entry.featureShortName, entry.settingPath, entry.settingKey };
			if (entry.source == EntrySource::User)
				destinationUserSettings.insert({ identity, entry.period });
			else if (entry.source == EntrySource::Overwrite && IsEntryActive(entry))
				destinationOverwriteSettings.insert({ std::move(identity), entry.period });
		}

	std::set<CopyGroupKey> selectedGroups;
	for (const auto& setting : selectedSettings)
		selectedGroups.insert(GetCopyGroupKey(setting));

	for (const auto& sourceValue : sourceValues) {
		const auto& identity = sourceValue.identity;
		const auto* entry = sourceValue.entry;
		if (!selectedGroups.contains(GetCopyGroupKey(identity)))
			continue;

		const std::vector destinationPeriods{ sourceValue.destinationPeriod };

		const bool compatible = IsCopyEntryCompatible(*entry, destination.type) &&
		                        (!CopyContextRequiresNumeric(destination) || IsNumericValue(entry->value));
		const bool conflicts = compatible && std::any_of(destinationPeriods.begin(), destinationPeriods.end(),
												 [&](TimeOfDayPeriod period) { return destinationUserSettings.contains({ identity, period }); });
		const bool maskedByOverwrite = compatible && std::any_of(destinationPeriods.begin(), destinationPeriods.end(),
														 [&](TimeOfDayPeriod period) { return destinationOverwriteSettings.contains({ identity, period }); });
		candidates.push_back({
			.setting = identity,
			.displayName = entry->displayName.empty() ?
		                       GetSceneSettingDisplayName(identity.featureShortName, identity.settingPath, identity.settingKey) :
		                       entry->displayName,
			.value = entry->value,
			.compatible = compatible,
			.conflicts = conflicts,
			.maskedByOverwrite = maskedByOverwrite,
			.sourcePeriod = sourceValue.sourcePeriod,
			.destinationPeriod = sourceValue.destinationPeriod,
		});
	}
	std::map<CopyGroupKey, std::vector<size_t>> candidateGroups;
	for (size_t index = 0; index < candidates.size(); ++index)
		candidateGroups[GetCopyGroupKey(candidates[index].setting)].push_back(index);
	for (const auto& [_, indices] : candidateGroups) {
		const bool groupCompatible = std::all_of(indices.begin(), indices.end(),
			[&](size_t index) { return candidates[index].compatible; });
		if (groupCompatible)
			continue;
		for (const auto index : indices) {
			candidates[index].compatible = false;
			candidates[index].conflicts = false;
			candidates[index].maskedByOverwrite = false;
		}
	}
	std::sort(candidates.begin(), candidates.end(), [](const auto& lhs, const auto& rhs) {
		return std::tie(lhs.displayName, lhs.setting) < std::tie(rhs.displayName, rhs.setting);
	});
	return candidates;
}

std::vector<SceneSettingsManager::CopyCandidate> SceneSettingsManager::GetCopyCandidates(
	const SceneContextId& source, const SceneContextId& destination,
	EntrySource sourceLayer, std::span<const SettingIdentity> settings) const
{
	auto candidates = BuildCopyCandidates(source, destination, sourceLayer, settings);
	std::map<CopyGroupKey, std::vector<CopyCandidate>> groups;
	for (auto& candidate : candidates)
		groups[GetCopyGroupKey(candidate.setting)].push_back(std::move(candidate));

	std::vector<CopyCandidate> choices;
	choices.reserve(groups.size());
	for (auto& [_, group] : groups) {
		assert(!group.empty());
		const bool compatible = std::all_of(group.begin(), group.end(),
			[](const auto& candidate) { return candidate.compatible; });
		const bool conflicts = compatible && std::any_of(group.begin(), group.end(),
												 [](const auto& candidate) { return candidate.conflicts; });
		const bool maskedByOverwrite = compatible && std::any_of(group.begin(), group.end(),
														 [](const auto& candidate) { return candidate.maskedByOverwrite; });
		auto choice = std::move(group.front());
		choice.compatible = compatible;
		choice.conflicts = conflicts;
		choice.maskedByOverwrite = maskedByOverwrite;

		choice.displayName = GetCopySettingDisplayName(choice.setting);
		choices.push_back(std::move(choice));
	}
	std::sort(choices.begin(), choices.end(), [](const auto& lhs, const auto& rhs) {
		return std::tie(lhs.displayName, lhs.setting) < std::tie(rhs.displayName, rhs.setting);
	});
	return choices;
}

std::vector<SceneSettingsManager::CopySourceSetting> SceneSettingsManager::GetCopySourceSettings(
	const SceneContextId& source, EntrySource sourceLayer,
	std::optional<SceneContextType> destinationType) const
{
	std::vector<CopySourceSetting> settings;
	const auto* sourceEntries = GetCopyContextEntries(source);
	if (!sourceEntries)
		return settings;

	std::map<CopyGroupKey, SettingIdentity> logicalSettings;
	std::set<CopyGroupKey> incompatibleGroups;
	std::set<CopyGroupKey> nonNumericGroups;
	for (const auto& entry : *sourceEntries) {
		if (entry.source != sourceLayer || !EntryBelongsToContext(entry, source))
			continue;
		SettingIdentity identity{ entry.featureShortName, entry.settingPath, entry.settingKey };
		const auto group = GetCopyGroupKey(identity);
		if (destinationType && !IsCopyEntryCompatible(entry, *destinationType))
			incompatibleGroups.insert(group);
		logicalSettings.try_emplace(group, std::move(identity));
		if (!IsNumericValue(entry.value))
			nonNumericGroups.insert(group);
	}
	for (const auto& group : incompatibleGroups)
		logicalSettings.erase(group);
	settings.reserve(logicalSettings.size());
	for (auto& [group, identity] : logicalSettings)
		settings.push_back({ identity, GetCopySettingDisplayName(identity), !nonNumericGroups.contains(group) });
	std::sort(settings.begin(), settings.end(), [](const auto& lhs, const auto& rhs) {
		return std::tie(lhs.displayName, lhs.setting) < std::tie(rhs.displayName, rhs.setting);
	});
	return settings;
}

std::vector<SceneSettingsManager::CopySource> SceneSettingsManager::GetCopySources(EntrySource sourceLayer) const
{
	std::vector<CopySource> sources;
	const auto addSource = [&](const SceneContextId& context, const std::vector<SettingEntry>& entries, std::string name) {
		std::set<CopyGroupKey> groups;
		for (const auto& entry : entries)
			if (entry.source == sourceLayer && EntryBelongsToContext(entry, context))
				groups.insert(GetCopyGroupKey({ entry.featureShortName, entry.settingPath, entry.settingKey }));
		if (!groups.empty())
			sources.push_back({ context, std::move(name), groups.size() });
	};
	const auto addPeriods = [&](SceneContextId context, const std::vector<SettingEntry>& entries, const std::string& prefix) {
		context.allPeriods = true;
		addSource(context, entries, prefix + T("feature.scene_manager.channel.all", "All"));
		context.allPeriods = false;
		for (int p = 0; p < kPeriodCount; ++p) {
			context.period = static_cast<TimeOfDayPeriod>(p);
			addSource(context, entries, prefix + GetCopyPeriodName(context.period));
		}
	};
	addSource({ .type = SceneContextType::Interior }, GetEntries(SceneType::InteriorOnly),
		T("feature.scene_manager.tab.interior", "Interior"));
	addPeriods({ .type = SceneContextType::TimeOfDay }, GetEntries(SceneType::TimeOfDay), "");
	const auto addScene = [&](SceneContextId context, const PeriodicSceneConfig& config, const std::string& name) {
		const auto normal = std::format("{} / {}", name, T("feature.scene_manager.mode.normal", "Normal"));
		addSource(context, config.entries, normal + (config.timeOfDayEnabled ? std::format(" ({})", T("feature.scene_manager.mode.inactive", "Inactive")) : ""));
		const auto periods = std::format("{} / {}{} / ", name, T("feature.scene_manager.tab.time_of_day", "Time of Day"),
			!config.timeOfDayEnabled ? std::format(" ({})", T("feature.scene_manager.mode.inactive", "Inactive")) : "");
		addPeriods(context, config.entries, periods);
	};
	for (const auto& [weatherId, config] : weatherSceneConfigs)
		addScene({ .type = SceneContextType::Weather, .weatherId = weatherId }, config, Util::GetFormDisplayName(weatherId));
	for (const auto& [_, config] : locationSceneConfigs)
		addScene({ .type = SceneContextType::Location, .locationType = config.type, .locationFormKey = config.formKey },
			config, std::format("{} / {}", GetCopyLocationTypeName(config.type), config.name.empty() ? config.formKey : config.name));
	std::sort(sources.begin(), sources.end(), [](const auto& lhs, const auto& rhs) {
		return std::tie(lhs.context.type, lhs.displayName, lhs.context) < std::tie(rhs.context.type, rhs.displayName, rhs.context);
	});
	return sources;
}

SceneSettingsManager::CopyResult SceneSettingsManager::CopySettings(const SceneContextId& source,
	const SceneContextId& destination, EntrySource sourceLayer, CopyConflictPolicy conflictPolicy,
	std::span<const SettingIdentity> settings, bool deferCommit)
{
	CopyResult result;
	if (!IsValidSceneContext(source) || !IsValidSceneContext(destination) ||
		!IsValidCopyConflictPolicy(conflictPolicy) || settings.empty())
		return result;
	if ((source.type == SceneContextType::Weather || destination.type == SceneContextType::Weather) &&
		!TryEnsureWeatherDataLoaded())
		return result;
	if ((source.type == SceneContextType::Location || destination.type == SceneContextType::Location) &&
		!TryEnsureLocationDataLoaded())
		return result;

	auto candidates = BuildCopyCandidates(source, destination, sourceLayer, settings);
	if (candidates.empty())
		return result;
	std::map<CopyGroupKey, std::vector<CopyCandidate>> groups;
	for (const auto& candidate : candidates)
		groups[GetCopyGroupKey(candidate.setting)].push_back(candidate);

	for (const auto& [_, group] : groups) {
		if (std::any_of(group.begin(), group.end(), [](const auto& candidate) { return !candidate.compatible; })) {
			result.incompatible += group.size();
			continue;
		}
		result.hadConflicts |= std::any_of(group.begin(), group.end(),
			[](const auto& candidate) { return candidate.conflicts; });
	}
	if (conflictPolicy == CopyConflictPolicy::Cancel && result.hadConflicts) {
		result.cancelled = true;
		return result;
	}

	std::vector<LocationTarget> destinationLocationTargets;
	std::optional<LocationTarget> destinationLocationTarget;
	if (destination.type == SceneContextType::Location) {
		destinationLocationTargets = ResolveLocationTargetChain(
			destination.locationType, destination.locationFormKey);
		const auto destinationKey = GetLocationConfigKey(
			destination.locationType, destination.locationFormKey);
		auto targetIt = std::find_if(destinationLocationTargets.begin(), destinationLocationTargets.end(),
			[&](const auto& target) {
				return GetLocationConfigKey(target.type, target.formKey) == destinationKey;
			});
		if (targetIt == destinationLocationTargets.end())
			return result;
		destinationLocationTarget = *targetIt;
	}

	std::vector<SettingEntry> emptyDestinationEntries;
	std::vector<SettingEntry>* destinationEntries = nullptr;
	PeriodicSceneConfig* destinationConfig = nullptr;
	bool destinationNeedsMaterialization = false;

	switch (destination.type) {
	case SceneContextType::Interior:
		destinationEntries = &GetEntriesMut(SceneType::InteriorOnly);
		break;
	case SceneContextType::TimeOfDay:
		destinationEntries = &GetEntriesMut(SceneType::TimeOfDay);
		break;
	case SceneContextType::Weather:
		{
			auto configIt = weatherSceneConfigs.find(destination.weatherId);
			destinationConfig = configIt != weatherSceneConfigs.end() ? &configIt->second : nullptr;
			destinationEntries = configIt != weatherSceneConfigs.end() ?
			                         &configIt->second.entries :
			                         &emptyDestinationEntries;
			destinationNeedsMaterialization = configIt == weatherSceneConfigs.end();
			break;
		}
	case SceneContextType::Location:
		{
			auto configIt = locationSceneConfigs.find(GetLocationConfigKey(
				destination.locationType, destination.locationFormKey));
			destinationConfig = configIt != locationSceneConfigs.end() ? &configIt->second : nullptr;
			destinationEntries = configIt != locationSceneConfigs.end() ?
			                         &configIt->second.entries :
			                         &emptyDestinationEntries;
			destinationNeedsMaterialization = configIt == locationSceneConfigs.end();
			break;
		}
	}
	if (!destinationEntries)
		return result;

	using DestinationSettingKey = std::pair<SettingIdentity, TimeOfDayPeriod>;
	std::map<DestinationSettingKey, size_t> destinationUserIndices;
	for (size_t index = 0; index < destinationEntries->size(); ++index) {
		const auto& entry = (*destinationEntries)[index];
		if (entry.source == EntrySource::User && EntryBelongsToContext(entry, destination)) {
			SettingIdentity identity{ entry.featureShortName, entry.settingPath, entry.settingKey };
			destinationUserIndices[{ std::move(identity), entry.period }] = index;
		}
	}
	std::vector<SettingAddress> candidateAddresses;
	for (const auto& [_, group] : groups) {
		if (std::any_of(group.begin(), group.end(), [](const auto& candidate) { return !candidate.compatible; }) ||
			(conflictPolicy == CopyConflictPolicy::SkipExisting &&
				std::any_of(group.begin(), group.end(), [](const auto& candidate) { return candidate.conflicts; })))
			continue;
		for (const auto& candidate : group)
			candidateAddresses.push_back({ candidate.setting.featureShortName,
				candidate.setting.settingPath, candidate.setting.settingKey });
	}
	std::sort(candidateAddresses.begin(), candidateAddresses.end());
	candidateAddresses.erase(std::unique(candidateAddresses.begin(), candidateAddresses.end()),
		candidateAddresses.end());
	EnsureBaselines(candidateAddresses);

	std::map<TimeOfDayPeriod, ResolvedSettingMap> lowerLayersByPeriod;

	const auto* sourceEntries = GetCopyContextEntries(source);
	std::map<DestinationSettingKey, std::optional<float>> sourceTransitions;
	if (sourceEntries) {
		if (source.allPeriods) {
			for (const auto& sourceEntry : *sourceEntries) {
				if (sourceEntry.source != sourceLayer || !EntryBelongsToContext(sourceEntry, source))
					continue;
				SettingIdentity identity{
					sourceEntry.featureShortName, sourceEntry.settingPath, sourceEntry.settingKey
				};
				sourceTransitions[{ std::move(identity), sourceEntry.period }] = sourceEntry.transitionSeconds;
			}
		} else {
			EffectiveCopyEntries effectiveSourceEntries;
			for (const auto& sourceEntry : *sourceEntries)
				if (sourceEntry.source == sourceLayer && EntryBelongsToContext(sourceEntry, source))
					AddEffectiveCopyEntry(effectiveSourceEntries, sourceEntry, false);
			for (const auto& [identity, entry] : effectiveSourceEntries)
				sourceTransitions[{ identity, entry->period }] = entry->transitionSeconds;
		}
	}
	struct PendingCopy
	{
		CopyCandidate candidate;
		std::optional<size_t> destinationIndex;
		json originalValue;
		std::optional<float> transitionSeconds;
		TimeOfDayPeriod destinationPeriod = TimeOfDayPeriod::Count;
	};
	std::vector<PendingCopy> pending;
	for (const auto& [_, group] : groups) {
		if (std::any_of(group.begin(), group.end(), [](const auto& candidate) { return !candidate.compatible; }))
			continue;
		const bool hasConflict = std::any_of(group.begin(), group.end(),
			[](const auto& candidate) { return candidate.conflicts; });
		if (hasConflict && conflictPolicy == CopyConflictPolicy::SkipExisting) {
			result.skipped += group.size();
			continue;
		}

		std::vector<PendingCopy> groupPending;
		bool groupValid = true;
		for (const auto& candidate : group) {
			const std::vector candidateDestinationPeriods{ candidate.destinationPeriod };
			for (const auto destinationPeriod : candidateDestinationPeriods) {
				std::optional<size_t> destinationIndex;
				if (auto indexIt = destinationUserIndices.find({ candidate.setting, destinationPeriod });
					indexIt != destinationUserIndices.end())
					destinationIndex = indexIt->second;

				SettingAddress address{ candidate.setting.featureShortName,
					candidate.setting.settingPath, candidate.setting.settingKey };
				json originalValue;
				if (destinationIndex) {
					originalValue = (*destinationEntries)[*destinationIndex].originalValue;
				} else if (destination.type == SceneContextType::Weather) {
					if (const auto lower = ResolveWeatherLowerValue(destination.weatherId, address, destinationPeriod, EntrySource::User))
						originalValue = *lower;
				} else if (destination.type == SceneContextType::Location) {
					auto lower = lowerLayersByPeriod.find(destinationPeriod);
					if (lower == lowerLayersByPeriod.end()) {
						auto values = BuildLocationLowerLayers(destination.locationType, destination.locationFormKey, EntrySource::User, destinationPeriod);
						if (!values) {
							groupValid = false;
							break;
						}
						lower = lowerLayersByPeriod.emplace(destinationPeriod, std::move(*values)).first;
					}
					if (const auto value = lower->second.find(address); value != lower->second.end())
						originalValue = value->second;
					else if (const auto baseline = baselineSettings.find(address); baseline != baselineSettings.end())
						originalValue = baseline->second;
				} else if (auto baselineIt = baselineSettings.find(address); baselineIt != baselineSettings.end()) {
					originalValue = baselineIt->second;
				}
				if (!destinationIndex && !IsSceneSettingPrimitive(originalValue)) {
					groupValid = false;
					break;
				}

				std::optional<float> transitionSeconds;
				if (destination.type == SceneContextType::Location) {
					if (destinationIndex)
						transitionSeconds = (*destinationEntries)[*destinationIndex].transitionSeconds;
					else if (const auto sourceTransition = sourceTransitions.find({ candidate.setting, candidate.sourcePeriod }); sourceTransition != sourceTransitions.end())
						transitionSeconds = sourceTransition->second;
				}
				groupPending.push_back({ candidate, destinationIndex, std::move(originalValue), transitionSeconds, destinationPeriod });
			}
			if (!groupValid)
				break;
		}
		if (!groupValid) {
			result.incompatible += group.size();
			continue;
		}
		pending.insert(pending.end(), std::make_move_iterator(groupPending.begin()),
			std::make_move_iterator(groupPending.end()));
	}

	if (pending.empty())
		return result;
	if (destinationNeedsMaterialization) {
		if (destination.type == SceneContextType::Weather) {
			destinationConfig = &GetWeatherConfigMut(destination.weatherId);
			destinationEntries = &destinationConfig->entries;
		} else if (destination.type == SceneContextType::Location) {
			auto& config = GetLocationConfigMut(destination.locationType,
				destination.locationFormKey, destinationLocationTarget->name);
			config.cocCode = destinationLocationTarget->cocCode;
			destinationConfig = &config;
			destinationEntries = &config.entries;
		}
	}
	if (destinationConfig && destinationEntries->empty())
		destinationConfig->timeOfDayEnabled = destination.allPeriods || destination.period != TimeOfDayPeriod::Count;

	for (auto& copy : pending) {
		if (copy.destinationIndex) {
			auto& destinationEntry = (*destinationEntries)[*copy.destinationIndex];
			destinationEntry.value = copy.candidate.value;
			destinationEntry.paused = false;
			if (destination.type == SceneContextType::Location)
				destinationEntry.transitionSeconds = copy.transitionSeconds;
			++result.overwritten;
			continue;
		}
		destinationEntries->push_back({
			.featureShortName = copy.candidate.setting.featureShortName,
			.settingPath = copy.candidate.setting.settingPath,
			.settingKey = copy.candidate.setting.settingKey,
			.displayName = copy.candidate.displayName,
			.value = copy.candidate.value,
			.originalValue = std::move(copy.originalValue),
			.paused = false,
			.source = EntrySource::User,
			.period = copy.destinationPeriod,
			.transitionSeconds = copy.transitionSeconds,
		});
		++result.copied;
	}
	if (!result.Changed())
		return result;

	if (CopyContextRequiresNumeric(destination.type))
		++sceneValueRevision;
	BumpEntryPresentationRevision();
	switch (destination.type) {
	case SceneContextType::Interior:
		MarkEntryListUserSettingsModified(SceneType::InteriorOnly);
		break;
	case SceneContextType::TimeOfDay:
		MarkEntryListUserSettingsModified(SceneType::TimeOfDay);
		break;
	case SceneContextType::Weather:
		PrepareWeatherUserSettingsMutation(destination.weatherId, true);
		break;
	case SceneContextType::Location:
		PrepareLocationUserSettingsMutation(
			destination.locationType, destination.locationFormKey, true);
		break;
	}
	if (!deferCommit)
		CommitSceneSettingChanges();
	return result;
}

std::vector<SceneSettingsManager::SettingEntry>* SceneSettingsManager::GetCopyContextEntriesMut(const SceneContextId& context)
{
	if (!IsValidSceneContext(context))
		return nullptr;
	if (context.type == SceneContextType::Weather && !TryEnsureWeatherDataLoaded())
		return nullptr;
	if (context.type == SceneContextType::Location && !TryEnsureLocationDataLoaded())
		return nullptr;

	std::vector<SettingEntry>* contextEntries = nullptr;
	switch (context.type) {
	case SceneContextType::Interior:
		contextEntries = &GetEntriesMut(SceneType::InteriorOnly);
		break;
	case SceneContextType::TimeOfDay:
		contextEntries = &GetEntriesMut(SceneType::TimeOfDay);
		break;
	case SceneContextType::Weather:
		if (auto config = weatherSceneConfigs.find(context.weatherId);
			config != weatherSceneConfigs.end())
			contextEntries = &config->second.entries;
		break;
	case SceneContextType::Location:
		if (auto config = locationSceneConfigs.find(
				GetLocationConfigKey(context.locationType, context.locationFormKey));
			config != locationSceneConfigs.end())
			contextEntries = &config->second.entries;
		break;
	}
	return contextEntries;
}

void SceneSettingsManager::SetFeatureSceneSettingsPaused(
	std::string_view featureShortName, const SceneContextId& context, bool paused)
{
	if (featureShortName.empty())
		return;
	auto* contextEntries = GetCopyContextEntriesMut(context);
	if (!contextEntries)
		return;
	bool changed = false;
	bool userEntriesChanged = false;
	for (auto& entry : *contextEntries) {
		if (entry.featureShortName != featureShortName || !EntryBelongsToContext(entry, context) || entry.paused == paused)
			continue;
		entry.paused = paused;
		changed = true;
		userEntriesChanged |= entry.source == EntrySource::User;
	}
	if (!changed)
		return;
	++sceneValueRevision;
	locationOverridesDirty = true;
	BumpEntryPresentationRevision();
	if (userEntriesChanged) {
		switch (context.type) {
		case SceneContextType::Interior:
			MarkEntryListUserSettingsModified(SceneType::InteriorOnly);
			break;
		case SceneContextType::TimeOfDay:
			MarkEntryListUserSettingsModified(SceneType::TimeOfDay);
			break;
		case SceneContextType::Weather:
			PrepareWeatherUserSettingsMutation(context.weatherId, false);
			break;
		case SceneContextType::Location:
			PrepareLocationUserSettingsMutation(context.locationType, context.locationFormKey, false);
			break;
		}
		SaveAllUserSettings();
	}
	ReapplyIfActive();
}

bool SceneSettingsManager::DeleteFeatureSceneSettings(
	std::string_view featureShortName, const SceneContextId& context)
{
	if (featureShortName.empty())
		return false;
	auto* contextEntries = GetCopyContextEntriesMut(context);
	if (!contextEntries)
		return false;

	const auto removed = std::erase_if(*contextEntries, [&](const SettingEntry& entry) {
		return entry.source == EntrySource::User &&
		       entry.featureShortName == featureShortName && EntryBelongsToContext(entry, context);
	});
	if (removed == 0)
		return false;

	if (CopyContextRequiresNumeric(context.type))
		++sceneValueRevision;
	BumpEntryPresentationRevision();
	switch (context.type) {
	case SceneContextType::Interior:
		MarkEntryListUserSettingsModified(SceneType::InteriorOnly);
		break;
	case SceneContextType::TimeOfDay:
		MarkEntryListUserSettingsModified(SceneType::TimeOfDay);
		break;
	case SceneContextType::Weather:
		PrepareWeatherUserSettingsMutation(context.weatherId, false);
		break;
	case SceneContextType::Location:
		PrepareLocationUserSettingsMutation(
			context.locationType, context.locationFormKey, false);
		break;
	}

	if (featureSceneEdit && featureSceneEdit->featureShortName == featureShortName &&
		featureSceneEdit->context == context) {
		featureSceneEdit->workingOverrides.clear();
		featureSceneEdit->dirty = false;
	}
	CommitSceneSettingChanges();
	return true;
}

// --- Per-Location Scene Settings ---

const SceneSettingsManager::LocationSceneConfig SceneSettingsManager::kEmptyLocationConfig{};

std::string SceneSettingsManager::GetLocationConfigKey(LocationTargetType type, std::string_view formKey)
{
	return std::format("{}:{}", GetLocationTargetTypeName(type), NormalizeLocationFormKey(formKey));
}

const char* SceneSettingsManager::GetLocationSectionName(LocationTargetType type)
{
	switch (type) {
	case LocationTargetType::Worldspace:
		return "worldspaces";
	case LocationTargetType::Region:
		return "regions";
	case LocationTargetType::LocationType:
		return "locationTypes";
	case LocationTargetType::Location:
		return "locations";
	case LocationTargetType::Cell:
		return "cells";
	default:
		return "invalid";
	}
}

const char* SceneSettingsManager::GetLocationTargetTypeName(LocationTargetType type)
{
	switch (type) {
	case LocationTargetType::Worldspace:
		return "Worldspace";
	case LocationTargetType::Region:
		return "Region";
	case LocationTargetType::LocationType:
		return "LocationType";
	case LocationTargetType::Location:
		return "Location";
	case LocationTargetType::Cell:
		return "Cell";
	default:
		return "Invalid";
	}
}

const std::vector<SceneSettingsManager::LocationTarget>& SceneSettingsManager::GetCurrentLocationTargets() const
{
	auto* player = globals::game::player;
	auto* cell = player ? player->GetParentCell() : nullptr;
	if (!player || !cell) {
		cachedTargetLocationId = 0;
		cachedTargetCellId = 0;
		cachedTargetRegionId = 0;
		cachedTargetWorldspaceId = 0;
		locationTargetsCached = false;
		cachedLocationTargets.clear();
		return cachedLocationTargets;
	}

	auto* location = player->GetCurrentLocation();
	if (!location)
		location = cell->GetLocation();
	const auto locationId = location ? location->GetFormID() : 0;
	const auto cellId = cell->GetFormID();
	const auto* worldspace = player->GetWorldspace();
	const auto worldspaceId = worldspace ? worldspace->GetFormID() : 0;
	const auto regionId = cell->IsExteriorCell() && globals::game::sky && globals::game::sky->region ?
	                          globals::game::sky->region->GetFormID() :
	                          0;
	if (locationTargetsCached && cachedTargetLocationId == locationId &&
		cachedTargetCellId == cellId && cachedTargetRegionId == regionId && cachedTargetWorldspaceId == worldspaceId)
		return cachedLocationTargets;

	cachedTargetLocationId = locationId;
	cachedTargetCellId = cellId;
	cachedTargetRegionId = regionId;
	cachedTargetWorldspaceId = worldspaceId;
	locationTargetsCached = true;
	cachedLocationTargets = BuildLocationTargetChain(location, cell);
	if (locationManagementTargetsCached) {
		bool addedTarget = false;
		for (const auto& target : cachedLocationTargets) {
			if (std::ranges::none_of(cachedLocationManagementTargets, [&](const auto& candidate) {
					return candidate.type == target.type && candidate.formKey == target.formKey;
				})) {
				cachedLocationManagementTargets.push_back(target);
				addedTarget = true;
			}
		}
		if (addedTarget)
			std::ranges::sort(cachedLocationManagementTargets, [](const auto& lhs, const auto& rhs) {
				return std::tie(lhs.type, lhs.name, lhs.formKey) <
				       std::tie(rhs.type, rhs.name, rhs.formKey);
			});
	}
	return cachedLocationTargets;
}

const std::vector<SceneSettingsManager::LocationTarget>& SceneSettingsManager::GetLocationManagementTargets() const
{
	if (locationManagementTargetsCached)
		return cachedLocationManagementTargets;

	cachedLocationManagementTargets.clear();
	std::map<std::string, LocationTarget> targets;
	const auto addTarget = [&](LocationTarget target) {
		if (target.formKey.empty())
			return;
		targets[GetLocationConfigKey(target.type, target.formKey)] = std::move(target);
	};
	const auto addForm = [&](LocationTargetType type, RE::TESForm* form) {
		if (!form || form->GetFormID() == 0)
			return;
		std::string name;
		std::string cocCode;
		std::vector<std::string> locationTypes;
		if (type == LocationTargetType::Worldspace) {
			auto* worldspace = form->As<RE::TESWorldSpace>();
			if (!worldspace)
				return;
			addTarget(MakeWorldspaceTarget(worldspace));
			return;
		} else if (type == LocationTargetType::Region) {
			name = Util::GetFormDisplayName(form->GetFormID());
		} else if (type == LocationTargetType::LocationType) {
			auto* keyword = form->As<RE::BGSKeyword>();
			if (!IsLocationTypeKeyword(keyword))
				return;
			name = GetLocationTypeDisplayName(keyword);
		} else if (type == LocationTargetType::Location) {
			auto* location = form->As<RE::BGSLocation>();
			if (!location)
				return;
			name = GetLocationTargetDisplayName(location);
			locationTypes = GetLocationTypeLabels(location);
		} else {
			auto* cell = form->As<RE::TESObjectCELL>();
			if (!cell)
				return;
			cocCode = Util::GetFormEditorID(cell);
			const char* fullName = cell->GetFullName();
			if (cocCode.empty() && (!fullName || fullName[0] == '\0'))
				return;
			name = GetLocationTargetDisplayName(cell);
		}
		addTarget({
			.type = type,
			.formKey = Util::GetFormFileKey(form),
			.name = std::move(name),
			.cocCode = std::move(cocCode),
			.locationTypes = std::move(locationTypes),
			.formId = form->GetFormID(),
		});
	};

	if (auto* dataHandler = RE::TESDataHandler::GetSingleton()) {
		for (auto* worldspace : dataHandler->GetFormArray<RE::TESWorldSpace>())
			addForm(LocationTargetType::Worldspace, worldspace);
		for (auto* region : dataHandler->GetFormArray<RE::TESRegion>())
			if (region && region->worldSpace)
				addForm(LocationTargetType::Region, region);
		for (auto* keyword : dataHandler->GetFormArray<RE::BGSKeyword>())
			addForm(LocationTargetType::LocationType, keyword);
		for (auto* location : dataHandler->GetFormArray<RE::BGSLocation>())
			addForm(LocationTargetType::Location, location);
		for (auto* cell : dataHandler->GetFormArray<RE::TESObjectCELL>())
			addForm(LocationTargetType::Cell, cell);
	}
	for (const auto& target : GetCurrentLocationTargets())
		addTarget(target);

	for (const auto& [_, config] : locationSceneConfigs) {
		auto key = GetLocationConfigKey(config.type, config.formKey);
		if (auto targetIt = targets.find(key); targetIt != targets.end()) {
			if (!config.name.empty())
				targetIt->second.name = config.name;
			if (!config.cocCode.empty())
				targetIt->second.cocCode = config.cocCode;
			continue;
		}
		RE::FormID formId = 0;
		if (auto* form = ResolveLocationTargetForm(config.formKey))
			formId = form->GetFormID();
		addTarget({
			.type = config.type,
			.formKey = config.formKey,
			.name = config.name.empty() ? config.formKey : config.name,
			.cocCode = config.cocCode,
			.formId = formId,
		});
	}

	cachedLocationManagementTargets.reserve(targets.size());
	for (auto& [_, target] : targets)
		cachedLocationManagementTargets.push_back(std::move(target));
	std::ranges::sort(cachedLocationManagementTargets, [](const auto& lhs, const auto& rhs) {
		return std::tie(lhs.type, lhs.name, lhs.formKey) < std::tie(rhs.type, rhs.name, rhs.formKey);
	});
	locationManagementTargetsCached = true;
	return cachedLocationManagementTargets;
}

SceneSettingsManager::LocationSceneConfig& SceneSettingsManager::GetLocationConfigMut(
	LocationTargetType type, const std::string& formKey, const std::string& name)
{
	locationManagementTargetsCached = false;
	const auto canonicalFormKey = CanonicalizeResolvedLocationFormKey(formKey);
	auto& config = locationSceneConfigs[GetLocationConfigKey(type, canonicalFormKey)];
	config.type = type;
	config.formKey = canonicalFormKey;
	if (!name.empty())
		config.name = name;
	return config;
}

const SceneSettingsManager::LocationSceneConfig& SceneSettingsManager::GetLocationConfig(
	LocationTargetType type, std::string_view formKey) const
{
	auto it = locationSceneConfigs.find(GetLocationConfigKey(type, formKey));
	return it != locationSceneConfigs.end() ? it->second : kEmptyLocationConfig;
}

bool SceneSettingsManager::HasLocationConfig(LocationTargetType type, std::string_view formKey) const
{
	const auto& config = GetLocationConfig(type, formKey);
	return std::any_of(config.entries.begin(), config.entries.end(), [&](const auto& entry) {
		return IsEntryActive(entry);
	});
}

bool SceneSettingsManager::IsLocationShowTimeOfDay(LocationTargetType type, std::string_view formKey) const
{
	return IsSceneTimeOfDayEnabled({ .type = SceneContextType::Location, .locationType = type, .locationFormKey = std::string(formKey) });
}

void SceneSettingsManager::SetLocationShowTimeOfDay(LocationTargetType type, const std::string& formKey, bool show)
{
	SetSceneTimeOfDayEnabled({ .type = SceneContextType::Location, .locationType = type, .locationFormKey = formKey }, show);
}

std::optional<float> SceneSettingsManager::GetLocationEntryTransitionSeconds(
	LocationTargetType type, std::string_view formKey, size_t index) const
{
	const auto& config = GetLocationConfig(type, formKey);
	return index < config.entries.size() ? config.entries[index].transitionSeconds : std::nullopt;
}

void SceneSettingsManager::SetLocationEntryTransitionSeconds(LocationTargetType type,
	const std::string& formKey, std::span<const size_t> indices, std::optional<float> seconds,
	bool deferSave)
{
	if (indices.empty() || (seconds && (!std::isfinite(*seconds) || *seconds < 0.0f ||
										   *seconds > kMaxLocationTransitionSeconds)))
		return;
	auto configIt = locationSceneConfigs.find(GetLocationConfigKey(type, formKey));
	if (configIt == locationSceneConfigs.end())
		return;
	auto& config = configIt->second;
	std::set<size_t> expandedIndices;
	for (const auto index : indices) {
		if (index >= config.entries.size())
			return;
		expandedIndices.insert(index);
		const auto& selectedEntry = config.entries[index];
		const auto selectedGroup = GetCopyGroupKey({ selectedEntry.featureShortName,
			selectedEntry.settingPath, selectedEntry.settingKey });
		if (std::get<5>(selectedGroup) == SettingControlType::Scalar)
			continue;
		for (size_t candidateIndex = 0; candidateIndex < config.entries.size(); ++candidateIndex) {
			const auto& candidate = config.entries[candidateIndex];
			if (candidate.source == selectedEntry.source && candidate.period == selectedEntry.period &&
				GetCopyGroupKey({ candidate.featureShortName,
					candidate.settingPath, candidate.settingKey }) == selectedGroup)
				expandedIndices.insert(candidateIndex);
		}
	}
	for (const auto index : expandedIndices) {
		const auto& entry = config.entries[index];
		if (entry.source != EntrySource::User || !IsNumericValue(entry.value) ||
			!FindAllowedCatalogSetting(entry.featureShortName, entry.settingPath, entry.settingKey, true))
			return;
	}

	bool changed = false;
	for (const auto index : expandedIndices) {
		auto& entry = config.entries[index];
		if (entry.transitionSeconds != seconds) {
			entry.transitionSeconds = seconds;
			changed = true;
		}
	}
	if (!changed)
		return;
	PrepareLocationUserSettingsMutation(type, formKey, false);
	if (deferSave)
		MarkDeferredSceneChanges();
	else {
		SaveAllUserSettings();
		ReapplyIfActive(false);
	}
}

void SceneSettingsManager::ApplyFeatureSceneEditPreview(ResolvedSettingMap& resolved)
{
	if (!IsFeatureSceneEditPreviewActive())
		return;

	auto& edit = *featureSceneEdit;
	std::set<SettingAddress> overwriteAddresses;
	std::set<SettingAddress> alteredAddresses;
	const auto isEditable = [&](const SettingAddress& address) {
		return std::ranges::binary_search(edit.editableAddresses, address);
	};
	const auto selectedPeriod = (edit.context.type == SceneContextType::Weather || edit.context.type == SceneContextType::Location) &&
	                                    edit.context.period == TimeOfDayPeriod::Count ?
	                                GetCurrentPeriod() :
	                                edit.context.period;
	const auto anyEntry = [](const SettingEntry&) { return true; };
	const auto selectedPeriodEntry = [&](const SettingEntry& entry) {
		return entry.period == selectedPeriod;
	};
	EnsureBaselines(edit.editableAddresses);

	std::erase_if(resolved, [&](const auto& item) {
		return isEditable(item.first);
	});
	ResolvedSettingMap preview;
	for (const auto& address : edit.editableAddresses)
		if (auto baseline = baselineSettings.find(address); baseline != baselineSettings.end())
			preview[address] = baseline->second;
	const auto overlayEntries = [&](const std::vector<SettingEntry>& sourceEntries,
									SceneType type, EntrySource source, const auto& belongs,
									ResolvedSettingMap& destination) {
		for (const auto& entry : sourceEntries) {
			if (entry.source != source || !IsEntryActive(entry) ||
				entry.featureShortName != edit.featureShortName || !belongs(entry) ||
				!IsSettingAllowedForType(
					type, entry.featureShortName, entry.settingPath, entry.settingKey))
				continue;
			SettingAddress address{ entry.featureShortName, entry.settingPath, entry.settingKey };
			if (!isEditable(address) || !baselineSettings.contains(address))
				continue;
			if (source == EntrySource::Overwrite) {
				overwriteAddresses.insert(address);
				if (edit.overwritesPaused)
					continue;
			}
			alteredAddresses.insert(address);
			destination[address] = entry.value;
		}
	};
	const auto applyWorkingOverrides = [&](ResolvedSettingMap& destination) {
		for (const auto& [address, value] : edit.workingOverrides)
			if (isEditable(address)) {
				alteredAddresses.insert(address);
				destination[address] = value;
			}
	};
	const auto overlayLocationEntries = [&](EntrySource source, ResolvedSettingMap& destination) {
		for (const auto& target : GetCurrentLocationTargets()) {
			if (auto configIt = locationSceneConfigs.find(
					GetLocationConfigKey(target.type, target.formKey));
				configIt != locationSceneConfigs.end()) {
				const auto belongs = [&](const SettingEntry& entry) {
					return configIt->second.IsPeriodActive(entry.period) &&
					       (entry.period == TimeOfDayPeriod::Count || entry.period == selectedPeriod);
				};
				overlayEntries(configIt->second.entries, SceneType::Location, source, belongs, destination);
			}
			if (target.type == edit.context.locationType &&
				NormalizeLocationFormKey(target.formKey) ==
					NormalizeLocationFormKey(edit.context.locationFormKey))
				break;
		}
	};

	switch (edit.context.type) {
	case SceneContextType::Interior:
		overlayEntries(GetEntries(SceneType::InteriorOnly), SceneType::InteriorOnly,
			EntrySource::User, anyEntry, preview);
		applyWorkingOverrides(preview);
		overlayEntries(GetEntries(SceneType::InteriorOnly), SceneType::InteriorOnly,
			EntrySource::Overwrite, anyEntry, preview);
		break;
	case SceneContextType::TimeOfDay:
		overlayEntries(GetEntries(SceneType::TimeOfDay), SceneType::TimeOfDay,
			EntrySource::User, selectedPeriodEntry, preview);
		applyWorkingOverrides(preview);
		overlayEntries(GetEntries(SceneType::TimeOfDay), SceneType::TimeOfDay,
			EntrySource::Overwrite, selectedPeriodEntry, preview);
		break;
	case SceneContextType::Weather:
		overlayEntries(GetEntries(SceneType::TimeOfDay), SceneType::TimeOfDay,
			EntrySource::User, selectedPeriodEntry, preview);
		if (auto configIt = weatherSceneConfigs.find(edit.context.weatherId);
			configIt != weatherSceneConfigs.end())
			overlayEntries(configIt->second.entries, SceneType::TimeOfDay, EntrySource::User, [&](const auto& entry) { return entry.period == edit.context.period; }, preview);
		applyWorkingOverrides(preview);
		overlayEntries(GetEntries(SceneType::TimeOfDay), SceneType::TimeOfDay,
			EntrySource::Overwrite, selectedPeriodEntry, preview);
		if (auto configIt = weatherSceneConfigs.find(edit.context.weatherId);
			configIt != weatherSceneConfigs.end())
			overlayEntries(configIt->second.entries, SceneType::TimeOfDay, EntrySource::Overwrite, [&](const auto& entry) { return entry.period == edit.context.period; }, preview);
		break;
	case SceneContextType::Location:
		{
			ResolvedSettingMap userLocationValues;
			overlayLocationEntries(EntrySource::User, userLocationValues);
			applyWorkingOverrides(userLocationValues);
			if (Util::IsInterior()) {
				overlayEntries(GetEntries(SceneType::InteriorOnly), SceneType::InteriorOnly,
					EntrySource::User, anyEntry, preview);
				for (const auto& [address, value] : userLocationValues)
					preview[address] = value;
				overlayEntries(GetEntries(SceneType::InteriorOnly), SceneType::InteriorOnly,
					EntrySource::Overwrite, anyEntry, preview);
			} else {
				std::array<float, kPeriodCount> factors{};
				factors[static_cast<size_t>(selectedPeriod)] = 1.0f;
				ResolveExteriorSettings(
					preview, factors, &userLocationValues, edit.editableAddresses, nullptr, nullptr, false,
					!edit.overwritesPaused, &overwriteAddresses, &alteredAddresses);
			}
			overlayLocationEntries(EntrySource::Overwrite, preview);
			break;
		}
	}

	for (const auto& [address, value] : preview)
		if (isEditable(address))
			resolved[address] = value;
	if (edit.alteredAddresses != alteredAddresses || edit.overwriteAddresses != overwriteAddresses) {
		edit.alteredAddresses = std::move(alteredAddresses);
		edit.overwriteAddresses = std::move(overwriteAddresses);
		++featureSceneEditRevision;
	}
}

bool SceneSettingsManager::IsFeatureSceneEditPreviewActive() const
{
	if (!featureSceneEdit || !IsSceneReady())
		return false;
	if (featureSceneEdit->context.type != SceneContextType::Location)
		return true;

	const auto normalizedFormKey = NormalizeLocationFormKey(
		featureSceneEdit->context.locationFormKey);
	return std::ranges::any_of(GetCurrentLocationTargets(), [&](const auto& target) {
		return target.type == featureSceneEdit->context.locationType &&
		       NormalizeLocationFormKey(target.formKey) == normalizedFormKey;
	});
}

std::optional<json> SceneSettingsManager::ResolveLocationLowerValue(LocationTargetType type,
	std::string_view formKey, const SettingAddress& address, EntrySource selectedSource, TimeOfDayPeriod period)
{
	auto baseline = GetBaselineValue(address);
	if (!IsSceneSettingPrimitive(baseline))
		return std::nullopt;
	auto lowerLayers = BuildLocationLowerLayers(type, formKey, selectedSource, period);
	if (!lowerLayers)
		return std::nullopt;
	if (auto valueIt = lowerLayers->find(address); valueIt != lowerLayers->end() &&
												   IsSceneSettingPrimitive(valueIt->second))
		return valueIt->second;
	return baseline;
}

std::optional<SceneSettingsManager::ResolvedSettingMap> SceneSettingsManager::BuildLocationLowerLayers(
	LocationTargetType type, std::string_view formKey, std::optional<EntrySource> selectedSource, TimeOfDayPeriod period)
{
	if (period == TimeOfDayPeriod::Count) {
		std::array<float, kPeriodCount> factors{};
		GetTimeOfDayFactors(factors.data());
		ResolvedSettingMap blended;
		std::map<SettingAddress, float> numericValues;
		for (int index = 0; index < kPeriodCount; ++index) {
			if (factors[index] == 0.0f)
				continue;
			auto layers = BuildLocationLowerLayers(type, formKey, selectedSource, static_cast<TimeOfDayPeriod>(index));
			if (!layers)
				return std::nullopt;
			for (const auto& [address, value] : *layers) {
				if (!IsNumericValue(value)) {
					blended[address] = value;
					continue;
				}
				const auto baseline = GetBaselineValue(address);
				if (!IsNumericValue(baseline))
					continue;
				auto [it, _] = numericValues.try_emplace(address, baseline.get<float>());
				it->second += factors[index] * (value.get<float>() - baseline.get<float>());
			}
		}
		for (const auto& [address, value] : numericValues)
			blended[address] = value;
		return blended;
	}
	const auto selectedTargetKey = GetLocationConfigKey(type, formKey);
	const auto locationTargets = ResolveLocationTargetChain(type, formKey);
	const auto interior = GetLocationChainInteriorState(locationTargets);
	const auto selectedTarget = std::ranges::find_if(locationTargets, [&](const auto& target) {
		return GetLocationConfigKey(target.type, target.formKey) == selectedTargetKey;
	});
	if (selectedTarget == locationTargets.end())
		return std::nullopt;
	const auto selectedTargetIndex = static_cast<size_t>(
		std::distance(locationTargets.begin(), selectedTarget));
	const auto userTargetCount = selectedTargetIndex +
	                             (selectedSource == EntrySource::Overwrite ? 1 : 0);
	ResolvedSettingMap userLocationLayers;
	ResolveLocationSettings(userLocationLayers, std::span(locationTargets).first(userTargetCount),
		EntrySource::User, false, nullptr, period);

	ResolvedSettingMap lowerLayers;
	std::array<float, kPeriodCount> factors{};
	if (interior == true) {
		ResolveInteriorSettings(lowerLayers, EntrySource::User);
	} else if (interior == false) {
		if (period == TimeOfDayPeriod::Count)
			GetTimeOfDayFactors(factors.data());
		else
			factors[static_cast<size_t>(period)] = 1.0f;
		if (selectedSource != EntrySource::User) {
			ResolveExteriorSettings(lowerLayers, factors, &userLocationLayers);
		} else {
			const auto& userTimeOfDayValues = BuildTimeOfDayValueGroups(EntrySource::User);
			ResolveTimeOfDaySettings(lowerLayers, userTimeOfDayValues, factors);
			ResolveWeatherSettings(
				lowerLayers, userTimeOfDayValues, factors, EntrySource::User);
		}
	}
	if (selectedSource == EntrySource::User) {
		for (const auto& [address, value] : userLocationLayers)
			lowerLayers[address] = value;
		return lowerLayers;
	}

	if (interior != false) {
		for (const auto& [address, value] : userLocationLayers)
			lowerLayers[address] = value;
		if (interior == true)
			ResolveInteriorSettings(lowerLayers, EntrySource::Overwrite);
	}
	ResolveLocationSettings(lowerLayers, std::span(locationTargets).first(selectedTargetIndex),
		EntrySource::Overwrite, false, nullptr, period);
	return lowerLayers;
}

void SceneSettingsManager::PrepareLocationUserSettingsMutation(LocationTargetType type,
	std::string_view formKey, bool replaceMalformedEntries)
{
	auto& config = GetLocationConfigMut(type, std::string(formKey));
	config.userTimeOfDayEnabled = config.timeOfDayEnabled;
	locationOverridesDirty = true;
	locationUserSettingsModified = true;
	if (!unresolvedLocationUserSettings.is_object())
		unresolvedLocationUserSettings = json::object();
	const auto* sectionName = GetLocationSectionName(type);
	auto& section = unresolvedLocationUserSettings[sectionName];
	if (!section.is_object())
		section = json::object();
	const auto canonicalFormKey = CanonicalizeResolvedLocationFormKey(formKey);
	const auto targetKey = GetLocationConfigKey(type, canonicalFormKey);
	if (replaceMalformedEntries) {
		for (auto& [rawFormKey, rawConfig] : section.items()) {
			if (!rawConfig.is_object() ||
				GetLocationConfigKey(type, CanonicalizeResolvedLocationFormKey(rawFormKey)) != targetKey)
				continue;
			auto entriesIt = rawConfig.find("entries");
			if (entriesIt != rawConfig.end() && !entriesIt->is_array())
				*entriesIt = json::array();
			if (rawFormKey != canonicalFormKey) {
				rawConfig.erase("type");
				rawConfig.erase("name");
				rawConfig.erase("coc");
			}
		}
	}

	auto& rawConfig = section[canonicalFormKey];
	if (!rawConfig.is_object())
		rawConfig = json::object();
	if (replaceMalformedEntries) {
		auto entriesIt = rawConfig.find("entries");
		if (entriesIt != rawConfig.end() && !entriesIt->is_array())
			*entriesIt = json::array();
	}
}

bool SceneSettingsManager::AddLocationSetting(LocationTargetType type, const std::string& formKey,
	const std::string& name, const std::string& cocCode, const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey, bool deferSave, TimeOfDayPeriod period)
{
	if (!TryEnsureLocationDataLoaded() || formKey.empty() ||
		!IsSettingAllowedForType(SceneType::Location, featureShortName, settingPath, settingKey) ||
		static_cast<int>(period) < 0 || static_cast<int>(period) > kPeriodCount)
		return false;
	SettingAddress address{ featureShortName, settingPath, settingKey };
	auto lowerValue = ResolveLocationLowerValue(type, formKey, address, EntrySource::User, period);
	if (!lowerValue || !ValidateSceneSettingEntry(
						   "Location", SceneType::Location, featureShortName, settingPath, settingKey, *lowerValue, false))
		return false;

	if ((!IsNumericValue(*lowerValue) && period != TimeOfDayPeriod::Count) ||
		HasLocationEntry(type, formKey, featureShortName, settingPath, settingKey, EntrySource::User, period))
		return false;

	auto& config = GetLocationConfigMut(type, formKey, name);
	if (!cocCode.empty())
		config.cocCode = cocCode;
	config.entries.push_back({
		.featureShortName = featureShortName,
		.settingPath = settingPath,
		.settingKey = settingKey,
		.displayName = GetSceneSettingDisplayName(featureShortName, settingPath, settingKey),
		.value = *lowerValue,
		.originalValue = *lowerValue,
		.source = EntrySource::User,
		.period = period,
	});
	BumpEntryPresentationRevision();
	PrepareLocationUserSettingsMutation(type, formKey, true);
	if (deferSave)
		MarkDeferredSceneChanges();
	else
		CommitSceneSettingChanges();
	return true;
}

void SceneSettingsManager::RemoveLocationSetting(LocationTargetType type, const std::string& formKey, size_t index)
{
	RemoveSceneSettings({ .type = SceneContextType::Location, .locationType = type, .locationFormKey = formKey }, std::span{ &index, 1 });
}

void SceneSettingsManager::DeleteAllLocationUserSettings(LocationTargetType type, const std::string& formKey)
{
	if (!TryEnsureLocationDataLoaded())
		return;
	auto configIt = locationSceneConfigs.find(GetLocationConfigKey(type, formKey));
	if (configIt != locationSceneConfigs.end()) {
		const auto removed = std::erase_if(configIt->second.entries,
			[](const SettingEntry& entry) { return entry.source == EntrySource::User; });
		if (removed != 0)
			BumpEntryPresentationRevision();
	}
	PrepareLocationUserSettingsMutation(type, formKey, false);
	const auto* sectionName = GetLocationSectionName(type);
	auto sectionIt = unresolvedLocationUserSettings.find(sectionName);
	if (sectionIt != unresolvedLocationUserSettings.end() && sectionIt->is_object()) {
		const auto targetKey = GetLocationConfigKey(type, formKey);
		for (auto& [rawFormKey, rawConfig] : sectionIt->items())
			if (rawConfig.is_object() &&
				GetLocationConfigKey(type, CanonicalizeResolvedLocationFormKey(rawFormKey)) == targetKey)
				rawConfig.erase("entries");
	}
	SaveAllUserSettings();
	ReapplyIfActive();
}

void SceneSettingsManager::TogglePauseLocationEntry(LocationTargetType type, const std::string& formKey, size_t index)
{
	auto it = locationSceneConfigs.find(GetLocationConfigKey(type, formKey));
	if (it == locationSceneConfigs.end() || index >= it->second.entries.size())
		return;
	it->second.entries[index].paused = !it->second.entries[index].paused;
	locationOverridesDirty = true;
	BumpEntryPresentationRevision();
	if (it->second.entries[index].source == EntrySource::User) {
		PrepareLocationUserSettingsMutation(type, formKey, false);
		SaveAllUserSettings();
	}
	ReapplyIfActive();
}

void SceneSettingsManager::SetLocationEntriesPaused(LocationTargetType type, const std::string& formKey,
	std::span<const size_t> indices, bool paused)
{
	auto configIt = locationSceneConfigs.find(GetLocationConfigKey(type, formKey));
	if (configIt == locationSceneConfigs.end())
		return;
	bool changed = false;
	bool userEntriesChanged = false;
	for (const auto index : indices) {
		if (index >= configIt->second.entries.size())
			continue;
		auto& entry = configIt->second.entries[index];
		if (entry.paused == paused)
			continue;
		entry.paused = paused;
		changed = true;
		userEntriesChanged |= entry.source == EntrySource::User;
	}
	if (!changed)
		return;

	locationOverridesDirty = true;
	BumpEntryPresentationRevision();
	if (userEntriesChanged) {
		PrepareLocationUserSettingsMutation(type, formKey, false);
		SaveAllUserSettings();
	}
	ReapplyIfActive();
}

void SceneSettingsManager::UpdateLocationEntryValue(LocationTargetType type, const std::string& formKey,
	size_t index, const json& newValue, bool deferSave)
{
	const EntryValueUpdate update{ index, newValue };
	UpdateLocationEntryValues(type, formKey, std::span{ &update, 1 }, deferSave);
}

void SceneSettingsManager::UpdateLocationEntryValues(LocationTargetType type, const std::string& formKey,
	std::span<const EntryValueUpdate> updates, bool deferSave)
{
	auto it = locationSceneConfigs.find(GetLocationConfigKey(type, formKey));
	if (it == locationSceneConfigs.end())
		return;
	bool userEntriesChanged = false;
	if (!ApplyEntryValueUpdates(
			"Location", SceneType::Location, it->second.entries, updates, false, userEntriesChanged))
		return;
	locationOverridesDirty = true;
	if (userEntriesChanged) {
		PrepareLocationUserSettingsMutation(type, formKey, false);
		if (deferSave)
			MarkDeferredSceneChanges();
		else
			SaveAllUserSettings();
	}
	ReapplyIfActive(false);
}

void SceneSettingsManager::RevertLocationEntryToDefault(LocationTargetType type, const std::string& formKey, size_t index)
{
	auto it = locationSceneConfigs.find(GetLocationConfigKey(type, formKey));
	if (it == locationSceneConfigs.end() || index >= it->second.entries.size())
		return;
	auto& entry = it->second.entries[index];
	SettingAddress address{ entry.featureShortName, entry.settingPath, entry.settingKey };
	auto lowerValue = ResolveLocationLowerValue(type, formKey, address, entry.source, entry.period);
	if (!lowerValue || !ValidateSceneSettingEntry(
						   "Location", SceneType::Location, entry.featureShortName,
						   entry.settingPath, entry.settingKey, *lowerValue, false))
		return;
	entry.value = *lowerValue;
	entry.originalValue = *lowerValue;
	locationOverridesDirty = true;
	if (entry.source == EntrySource::User) {
		PrepareLocationUserSettingsMutation(type, formKey, false);
		SaveAllUserSettings();
	}
	ReapplyIfActive(false);
}

bool SceneSettingsManager::HasLocationEntry(LocationTargetType type, std::string_view formKey,
	const std::string& featureShortName, const std::vector<std::string>& settingPath,
	const std::string& settingKey, std::optional<EntrySource> source, TimeOfDayPeriod period) const
{
	const auto& config = GetLocationConfig(type, formKey);
	return std::any_of(config.entries.begin(), config.entries.end(), [&](const auto& entry) {
		return (!source || entry.source == *source) && entry.period == period &&
		       IsSameSetting(entry, featureShortName, settingPath, settingKey);
	});
}

SceneSettingsManager::OverwriteExportResult SceneSettingsManager::ExportLocationUserSettingsToOverwrites(LocationTargetType type,
	const std::string& formKey, const std::vector<size_t>& indices, const std::string& modName)
{
	OverwriteExportResult result;
	auto configIt = locationSceneConfigs.find(GetLocationConfigKey(type, formKey));
	if (configIt == locationSceneConfigs.end())
		return result;
	auto safeModName = Util::FileHelpers::SanitizeFileName(modName);
	if (safeModName.empty())
		return result;

	std::map<std::pair<TimeOfDayPeriod, std::string>, std::vector<const SettingEntry*>> groupedEntries;
	for (auto index : indices) {
		if (index >= configIt->second.entries.size())
			continue;
		const auto& entry = configIt->second.entries[index];
		if (entry.source == EntrySource::User)
			groupedEntries[{ entry.period, entry.featureShortName }].push_back(&entry);
	}

	const auto targetDescription = GetLocationTargetTypeName(type);
	const json metadata = {
		{ "targetType", targetDescription },
		{ "targetName", configIt->second.name },
		{ "coc", configIt->second.cocCode },
		{ kMetadataTimeOfDayEnabledKey, configIt->second.timeOfDayEnabled },
	};
	for (const auto& [identity, grouped] : groupedEntries) {
		const auto& [period, featureShortName] = identity;
		auto directory = GetLocationOverwritesDir(type) / configIt->second.formKey;
		if (period != TimeOfDayPeriod::Count)
			directory /= GetPeriodName(period);
		if (WriteGroupedOverwriteFile(GetLocationOverwritesDir(type),
				directory / std::format("{}_{}.json", safeModName, featureShortName),
				featureShortName, targetDescription, grouped, metadata))
			++result.writtenFiles;
		else
			++result.failedFiles;
	}
	if (result.writtenFiles != 0)
		ReloadOverwriteEntries();
	return result;
}

void SceneSettingsManager::DiscoverLocationOverwrites()
{
	const auto root = GetLocationOverwritesDir(LocationTargetType::Location);
	std::error_code ec;
	if (!std::filesystem::exists(root, ec))
		return;
	for (const auto& directory : GetSortedDirectoryPaths(root, true, "location overwrite directories"))
		DiscoverLocationOverwritesForTarget(LocationTargetType::Location, directory);
	for (auto& [_, config] : locationSceneConfigs)
		config.RefreshTimeOfDayMode();
}

void SceneSettingsManager::DiscoverLocationOverwritesForTarget(LocationTargetType type,
	const std::filesystem::path& targetDir)
{
	(void)type;
	const auto formKey = targetDir.filename().string();
	if (formKey.empty())
		return;

	std::optional<LocationTargetType> resolvedType;
	std::string resolvedName;
	std::string resolvedCocCode;
	std::string canonicalFormKey = formKey;
	if (const auto formId = Util::SpidToFormId(formKey); formId != 0) {
		canonicalFormKey = Util::FormIdToSpid(formId);
		if (auto* form = RE::TESForm::LookupByID(formId)) {
			if (form->GetFormType() == RE::FormType::WorldSpace) {
				resolvedType = LocationTargetType::Worldspace;
			} else if (form->GetFormType() == RE::FormType::Region) {
				resolvedType = LocationTargetType::Region;
			} else if (form->GetFormType() == RE::FormType::Keyword) {
				auto* keyword = form->As<RE::BGSKeyword>();
				if (!IsLocationTypeKeyword(keyword)) {
					logger::warn("[SceneSettings] Location overwrite target '{}' is not a LocType keyword", formKey);
					return;
				}
				resolvedType = LocationTargetType::LocationType;
			} else if (form->GetFormType() == RE::FormType::Location)
				resolvedType = LocationTargetType::Location;
			else if (form->GetFormType() == RE::FormType::Cell)
				resolvedType = LocationTargetType::Cell;
			else {
				logger::warn("[SceneSettings] Location overwrite target '{}' is not a worldspace, region, location type, location, or cell", formKey);
				return;
			}
			resolvedName = *resolvedType == LocationTargetType::LocationType ?
			                   GetLocationTypeDisplayName(form->As<RE::BGSKeyword>()) :
			                   Util::GetFormDisplayName(formId);
			if (*resolvedType == LocationTargetType::Cell)
				resolvedCocCode = Util::GetFormEditorID(form);
		}
	}

	std::vector<std::pair<std::filesystem::path, TimeOfDayPeriod>> files;
	std::error_code ec;
	for (int index = 0; index < kPeriodCount; ++index) {
		const auto period = static_cast<TimeOfDayPeriod>(index);
		if (!std::filesystem::is_directory(targetDir / GetPeriodName(period), ec))
			continue;
		for (const auto& file : GetSortedJsonFiles(targetDir / GetPeriodName(period), "location period overwrite files"))
			files.emplace_back(file, period);
	}
	for (const auto& file : GetSortedJsonFiles(targetDir, "location overwrite files"))
		files.emplace_back(file, TimeOfDayPeriod::Count);
	for (const auto& [filePath, period] : files) {
		try {
			json data;
			if (!ReadBoundedSceneJson(filePath, data)) {
				logger::warn("[SceneSettings] Location overwrite '{}' is invalid or exceeds {} bytes",
					filePath.string(), kMaxSceneOverwriteFileSize);
				continue;
			}

			std::optional<LocationTargetType> metadataType;
			std::string metadataName;
			std::string metadataCocCode;
			if (auto metadataIt = data.find(kMetadataKey); metadataIt != data.end()) {
				if (!metadataIt->is_object()) {
					logger::warn("[SceneSettings] Location overwrite '{}' metadata must be an object",
						filePath.string());
					continue;
				}
				const auto metadataContext = std::format("Location overwrite '{}' metadata", filePath.string());
				std::string targetType;
				if (!ReadOptionalStringField(*metadataIt, "targetType", targetType, metadataContext) ||
					!ReadOptionalStringField(*metadataIt, "targetName", metadataName, metadataContext) ||
					!ReadOptionalStringField(*metadataIt, "coc", metadataCocCode, metadataContext))
					continue;
				if (targetType == "Worldspace")
					metadataType = LocationTargetType::Worldspace;
				else if (targetType == "Region")
					metadataType = LocationTargetType::Region;
				else if (targetType == "LocationType" || targetType == "Category")
					metadataType = LocationTargetType::LocationType;
				else if (targetType == "Location")
					metadataType = LocationTargetType::Location;
				else if (targetType == "Cell")
					metadataType = LocationTargetType::Cell;
				else if (!targetType.empty()) {
					logger::warn("[SceneSettings] {} has invalid targetType '{}'", metadataContext, targetType);
					continue;
				}
			}
			if (resolvedType && metadataType && *resolvedType != *metadataType) {
				logger::warn("[SceneSettings] Location overwrite '{}' targetType does not match resolved form '{}'",
					filePath.string(), formKey);
				continue;
			}
			const auto targetType = resolvedType ? resolvedType : metadataType;
			if (!targetType) {
				logger::warn("[SceneSettings] Location overwrite '{}' has no resolvable target type",
					filePath.string());
				continue;
			}

			auto& config = GetLocationConfigMut(*targetType, canonicalFormKey,
				!metadataName.empty() ? metadataName : resolvedName);
			if (!metadataCocCode.empty())
				config.cocCode = metadataCocCode;
			else if (!resolvedCocCode.empty())
				config.cocCode = resolvedCocCode;

			std::vector<SettingEntry> parsedEntries;
			std::optional<bool> timeOfDayEnabled;
			if (!ParseOverwriteFileEntries(filePath, SceneType::Location, period != TimeOfDayPeriod::Count, parsedEntries, &timeOfDayEnabled))
				continue;
			overwriteBackingFiles[{ .type = SceneContextType::Location, .locationType = *targetType, .locationFormKey = canonicalFormKey }].insert(filePath);
			if (!config.overwriteTimeOfDayEnabled && timeOfDayEnabled)
				config.overwriteTimeOfDayEnabled = timeOfDayEnabled;
			for (auto& entry : parsedEntries) {
				entry.period = period;
				AddOverwriteEntryIfUnique(config.entries, std::move(entry), "location");
			}
		} catch (const std::exception& e) {
			logger::error("[SceneSettings] Failed to load location overwrite '{}': {}",
				filePath.filename().string(), e.what());
		}
	}
}

void SceneSettingsManager::DiscoverWeatherOverwrites()
{
	const auto previousEntryCount = std::accumulate(weatherSceneConfigs.begin(), weatherSceneConfigs.end(), size_t{ 0 },
		[](size_t total, const auto& config) { return total + config.second.entries.size(); });
	auto baseDir = GetWeatherOverwritesDir();
	std::error_code ec;
	if (!std::filesystem::exists(baseDir, ec))
		return;

	logger::info("[SceneSettings] Discovering weather overwrites in: {}", baseDir.string());

	for (const auto& weatherDirectory : GetSortedDirectoryPaths(baseDir, true, "weather overwrite directories")) {
		auto folderName = weatherDirectory.filename().string();
		RE::FormID weatherId = Util::SpidToFormId(folderName);
		if (weatherId == 0) {
			logger::warn("[SceneSettings] Weather overwrite folder '{}' could not be resolved - skipping", folderName);
			continue;
		}

		DiscoverWeatherOverwritesForSpid(weatherId, weatherDirectory);
	}
	for (auto& [_, config] : weatherSceneConfigs)
		config.RefreshTimeOfDayMode();

	const auto entryCount = std::accumulate(weatherSceneConfigs.begin(), weatherSceneConfigs.end(), size_t{ 0 },
		[](size_t total, const auto& config) { return total + config.second.entries.size(); });
	if (entryCount != previousEntryCount)
		++sceneValueRevision;
	if (entryCount != previousEntryCount)
		BumpEntryPresentationRevision();
}

void SceneSettingsManager::DiscoverWeatherOverwritesForSpid(RE::FormID weatherId, const std::filesystem::path& weatherDir)
{
	auto& config = GetWeatherConfigMut(weatherId);
	// Scan period subfolders (TOD entries)
	for (int i = 0; i < kPeriodCount; ++i) {
		auto period = static_cast<TimeOfDayPeriod>(i);
		auto periodDir = weatherDir / GetPeriodName(period);
		std::error_code ec;
		if (!std::filesystem::exists(periodDir, ec))
			continue;

		for (const auto& filePath : GetSortedJsonFiles(periodDir, "weather period overwrite files")) {
			try {
				std::vector<SettingEntry> parsedEntries;
				std::optional<bool> timeOfDayEnabled;
				if (!ParseOverwriteFileEntries(filePath, SceneType::TimeOfDay, true, parsedEntries, &timeOfDayEnabled))
					continue;
				overwriteBackingFiles[{ .type = SceneContextType::Weather, .weatherId = weatherId }].insert(filePath);
				if (!config.overwriteTimeOfDayEnabled && timeOfDayEnabled)
					config.overwriteTimeOfDayEnabled = timeOfDayEnabled;
				for (auto& entry : parsedEntries) {
					entry.period = period;
					AddOverwriteEntryIfUnique(config.entries, std::move(entry), "weather");
				}
			} catch (const std::exception& e) {
				logger::error("[SceneSettings] Failed to load weather overwrite '{}': {}", filePath.filename().string(), e.what());
			}
		}
	}

	{
		for (const auto& filePath : GetSortedJsonFiles(weatherDir, "flat weather overwrite files")) {
			try {
				std::vector<SettingEntry> parsedEntries;
				std::optional<bool> timeOfDayEnabled;
				if (!ParseOverwriteFileEntries(filePath, SceneType::TimeOfDay, true, parsedEntries, &timeOfDayEnabled))
					continue;
				overwriteBackingFiles[{ .type = SceneContextType::Weather, .weatherId = weatherId }].insert(filePath);
				if (!config.overwriteTimeOfDayEnabled && timeOfDayEnabled)
					config.overwriteTimeOfDayEnabled = timeOfDayEnabled;
				for (auto& entry : parsedEntries)
					AddOverwriteEntryIfUnique(config.entries, std::move(entry), "weather");
			} catch (const std::exception& e) {
				logger::error("[SceneSettings] Failed to load weather overwrite '{}': {}", filePath.filename().string(), e.what());
			}
		}
	}
}

void SceneSettingsManager::ReloadOverwriteEntries()
{
	overwriteBackingFiles.clear();
	using PauseKey = std::tuple<std::filesystem::path, std::string,
		std::vector<std::string>, std::string, TimeOfDayPeriod>;
	std::set<PauseKey> pausedEntries;
	const auto rememberPaused = [&](const SettingEntry& entry, const std::filesystem::path& path) {
		if (entry.source == EntrySource::Overwrite && entry.paused)
			pausedEntries.emplace(path.lexically_normal(), entry.featureShortName,
				entry.settingPath, entry.settingKey, entry.period);
	};
	const auto restorePaused = [&](SettingEntry& entry, const std::filesystem::path& path) {
		if (entry.source == EntrySource::Overwrite && pausedEntries.contains({ path.lexically_normal(), entry.featureShortName, entry.settingPath,
														  entry.settingKey, entry.period }))
			entry.paused = true;
	};

	for (const auto type : { SceneType::InteriorOnly, SceneType::TimeOfDay }) {
		auto& sceneEntries = GetEntriesMut(type);
		for (const auto& entry : sceneEntries)
			rememberPaused(entry, GetSceneOverwritePath(type, entry));
		std::erase_if(sceneEntries, [](const SettingEntry& entry) {
			return entry.source == EntrySource::Overwrite;
		});
	}
	if (weatherDataLoaded) {
		for (auto& [weatherId, config] : weatherSceneConfigs) {
			config.overwriteTimeOfDayEnabled.reset();
			for (const auto& entry : config.entries)
				rememberPaused(entry, GetWeatherOverwritePath(weatherId, entry));
			std::erase_if(config.entries, [](const SettingEntry& entry) {
				return entry.source == EntrySource::Overwrite;
			});
		}
	}
	if (locationDataLoaded) {
		for (auto& [_, config] : locationSceneConfigs) {
			config.overwriteTimeOfDayEnabled.reset();
			for (const auto& entry : config.entries)
				rememberPaused(entry, GetLocationOverwritePath(config.type, config.formKey, entry));
			std::erase_if(config.entries, [](const SettingEntry& entry) {
				return entry.source == EntrySource::Overwrite;
			});
		}
	}

	DiscoverOverwrites(SceneType::InteriorOnly);
	DiscoverOverwrites(SceneType::TimeOfDay);
	if (weatherDataLoaded)
		DiscoverWeatherOverwrites();
	if (locationDataLoaded)
		DiscoverLocationOverwrites();
	for (auto& [_, config] : weatherSceneConfigs)
		config.RefreshTimeOfDayMode();
	for (auto& [_, config] : locationSceneConfigs)
		config.RefreshTimeOfDayMode();
	locationManagementTargetsCached = false;

	for (const auto type : { SceneType::InteriorOnly, SceneType::TimeOfDay })
		for (auto& entry : GetEntriesMut(type))
			restorePaused(entry, GetSceneOverwritePath(type, entry));
	if (weatherDataLoaded)
		for (auto& [weatherId, config] : weatherSceneConfigs)
			for (auto& entry : config.entries)
				restorePaused(entry, GetWeatherOverwritePath(weatherId, entry));
	if (locationDataLoaded)
		for (auto& [_, config] : locationSceneConfigs)
			for (auto& entry : config.entries)
				restorePaused(entry, GetLocationOverwritePath(config.type, config.formKey, entry));

	++sceneValueRevision;
	locationOverridesDirty = true;
	BumpEntryPresentationRevision();
	ReapplyIfActive();
}
