#include "TreeWindPatcher.h"
#include "TreeWindSettings.h"

#include "Features/Wind/WindMath.h"
#include "Utils/FileSystem.h"
#include "Utils/Format.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace TreeWindPatcher
{
	using WindMath::ClampFiniteOrDefault;

	namespace
	{
		constexpr std::size_t kMaximumPatchFileSize = 1024 * 1024;
		constexpr std::size_t kMaximumRulesPerFile = 4096;
		constexpr std::size_t kMaximumModelPathLength = 260;
		constexpr float kDefaultUpperBendRange = 100.0f;
		constexpr float kDefaultMaximumDisplacementPercent = 3.0f;
		constexpr float kDefaultTrunkGustInfluence = 0.5f;
		constexpr float kDefaultLeafGustInfluence = 0.99f;
		constexpr float kDefaultTransientWindInfluence = 2.01f;
		constexpr float kDefaultLeafTransientWindInfluence = 5.0f;
		constexpr float kDefaultLeafTransientFlutterMaximum = 20.0f;
		constexpr float kDefaultTransientMaximumBendMultiplier = 2.5f;
		constexpr std::string_view kSchemaFileName = "WindSettings.schema.json";
		const RE::BSFixedString kRuleIdName = "OS_TreeWindRule";
		const RE::BSFixedString kTreeBoundsName = "OS_TreeWindBounds";
		constexpr std::size_t kTreeBoundsValueCount = 8;
		constexpr float kMinimumBoundExtent = 1e-3f;

		enum class ResponseParameter : std::size_t
		{
			Bend,
			LeafAmbient,
			UpperBendRange,
			MaximumDisplacementPercent,
			TrunkGustInfluence,
			LeafGustInfluence,
			TransientWindInfluence,
			LeafTransientWindInfluence,
			LeafTransientFlutterMaximum,
			TransientMaximumBendMultiplier,
			Count
		};
		constexpr std::size_t kResponseParameterCount = static_cast<std::size_t>(ResponseParameter::Count);

		struct ModelAabb
		{
			RE::NiPoint3 minimum{
				(std::numeric_limits<float>::max)(),
				(std::numeric_limits<float>::max)(),
				(std::numeric_limits<float>::max)()
			};
			RE::NiPoint3 maximum{
				(std::numeric_limits<float>::lowest)(),
				(std::numeric_limits<float>::lowest)(),
				(std::numeric_limits<float>::lowest)()
			};
			bool valid = false;
		};

		struct LoadedRule
		{
			float bend = 1.0f;
			float leafAmbient = 1.0f;
			float upperBendRange = kDefaultUpperBendRange;
			float maximumDisplacementPercent = kDefaultMaximumDisplacementPercent;
			float trunkGustInfluence = kDefaultTrunkGustInfluence;
			float leafGustInfluence = kDefaultLeafGustInfluence;
			float transientWindInfluence = kDefaultTransientWindInfluence;
			float leafTransientWindInfluence = kDefaultLeafTransientWindInfluence;
			float leafTransientFlutterMaximum = kDefaultLeafTransientFlutterMaximum;
			float transientMaximumBendMultiplier = kDefaultTransientMaximumBendMultiplier;
			std::array<std::string, kResponseParameterCount> sources{};
			std::filesystem::path ownerPath;
		};

		struct RuntimeRule
		{
			std::string mesh;
			std::filesystem::path ownerPath;
			std::atomic<float> bend{ 1.0f };
			std::atomic<float> leafAmbient{ 1.0f };
			std::atomic<float> upperBendRange{ kDefaultUpperBendRange };
			std::atomic<float> maximumDisplacementPercent{ kDefaultMaximumDisplacementPercent };
			std::atomic<float> trunkGustInfluence{ kDefaultTrunkGustInfluence };
			std::atomic<float> leafGustInfluence{ kDefaultLeafGustInfluence };
			std::atomic<float> transientWindInfluence{ kDefaultTransientWindInfluence };
			std::atomic<float> leafTransientWindInfluence{ kDefaultLeafTransientWindInfluence };
			std::atomic<float> leafTransientFlutterMaximum{ kDefaultLeafTransientFlutterMaximum };
			std::atomic<float> transientMaximumBendMultiplier{ kDefaultTransientMaximumBendMultiplier };
			std::atomic<float> persistedBend{ 1.0f };
			std::atomic<float> persistedLeafAmbient{ 1.0f };
			std::atomic<float> persistedUpperBendRange{ kDefaultUpperBendRange };
			std::atomic<float> persistedMaximumDisplacementPercent{ kDefaultMaximumDisplacementPercent };
			std::atomic<float> persistedTrunkGustInfluence{ kDefaultTrunkGustInfluence };
			std::atomic<float> persistedLeafGustInfluence{ kDefaultLeafGustInfluence };
			std::atomic<float> persistedTransientWindInfluence{ kDefaultTransientWindInfluence };
			std::atomic<float> persistedLeafTransientWindInfluence{ kDefaultLeafTransientWindInfluence };
			std::atomic<float> persistedLeafTransientFlutterMaximum{ kDefaultLeafTransientFlutterMaximum };
			std::atomic<float> persistedTransientMaximumBendMultiplier{ kDefaultTransientMaximumBendMultiplier };
		};

		std::vector<std::unique_ptr<RuntimeRule>> runtimeRules;
		std::unordered_map<std::string, std::uint32_t> ruleIds;
		std::mutex saveMutex;
		std::vector<std::string> conflictingFiles;
		std::atomic<bool> universalOverrideEnabled{ false };
		std::atomic<float> universalBend{ 1.0f };
		std::atomic<float> universalLeafAmbient{ 1.0f };
		std::atomic<float> universalUpperBendRange{ kDefaultUpperBendRange };
		std::atomic<float> universalMaximumDisplacementPercent{ kDefaultMaximumDisplacementPercent };
		std::atomic<float> universalTrunkGustInfluence{ kDefaultTrunkGustInfluence };
		std::atomic<float> universalLeafGustInfluence{ kDefaultLeafGustInfluence };
		std::atomic<float> universalTransientWindInfluence{ kDefaultTransientWindInfluence };
		std::atomic<float> universalLeafTransientWindInfluence{ kDefaultLeafTransientWindInfluence };
		std::atomic<float> universalLeafTransientFlutterMaximum{ kDefaultLeafTransientFlutterMaximum };
		std::atomic<float> universalTransientMaximumBendMultiplier{ kDefaultTransientMaximumBendMultiplier };

		bool ValuesDiffer(float a_lhs, float a_rhs)
		{
			return std::abs(a_lhs - a_rhs) > 0.0001f;
		}

		std::string NormalizeModelPath(std::string a_path);

		bool WriteJsonAtomically(const std::filesystem::path& a_path, const nlohmann::json& a_root, std::string& a_error)
		{
			auto temporaryPath = a_path;
			temporaryPath += std::format(".{}.{}.tmp", GetCurrentProcessId(), GetCurrentThreadId());

			const SKSE::stl::scope_exit cleanupTemporaryFile([&temporaryPath]() noexcept {
				std::error_code cleanupError;
				std::filesystem::remove(temporaryPath, cleanupError);
			});

			std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
			if (!output) {
				a_error = std::format("Failed to open temporary JSON for writing: {}", temporaryPath.string());
				return false;
			}
			output << a_root.dump(4) << '\n';
			output.close();
			if (!output) {
				a_error = std::format("Failed while writing temporary JSON: {}", temporaryPath.string());
				return false;
			}

			if (!MoveFileExW(temporaryPath.c_str(), a_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
				const auto windowsError = GetLastError();
				a_error = std::format("Failed to replace source JSON {}: Windows error {}", a_path.string(), windowsError);
				return false;
			}

			return true;
		}

		bool HasUnsavedValues(const RuntimeRule& a_rule)
		{
			return ValuesDiffer(a_rule.bend.load(std::memory_order_relaxed), a_rule.persistedBend.load(std::memory_order_relaxed)) ||
			       ValuesDiffer(a_rule.leafAmbient.load(std::memory_order_relaxed), a_rule.persistedLeafAmbient.load(std::memory_order_relaxed)) ||
			       ValuesDiffer(a_rule.upperBendRange.load(std::memory_order_relaxed), a_rule.persistedUpperBendRange.load(std::memory_order_relaxed)) ||
			       ValuesDiffer(a_rule.maximumDisplacementPercent.load(std::memory_order_relaxed), a_rule.persistedMaximumDisplacementPercent.load(std::memory_order_relaxed)) ||
			       ValuesDiffer(a_rule.trunkGustInfluence.load(std::memory_order_relaxed), a_rule.persistedTrunkGustInfluence.load(std::memory_order_relaxed)) ||
			       ValuesDiffer(a_rule.leafGustInfluence.load(std::memory_order_relaxed), a_rule.persistedLeafGustInfluence.load(std::memory_order_relaxed)) ||
			       ValuesDiffer(a_rule.transientWindInfluence.load(std::memory_order_relaxed), a_rule.persistedTransientWindInfluence.load(std::memory_order_relaxed)) ||
			       ValuesDiffer(a_rule.leafTransientWindInfluence.load(std::memory_order_relaxed),
					   a_rule.persistedLeafTransientWindInfluence.load(std::memory_order_relaxed)) ||
			       ValuesDiffer(a_rule.leafTransientFlutterMaximum.load(std::memory_order_relaxed),
					   a_rule.persistedLeafTransientFlutterMaximum.load(std::memory_order_relaxed)) ||
			       ValuesDiffer(a_rule.transientMaximumBendMultiplier.load(std::memory_order_relaxed),
					   a_rule.persistedTransientMaximumBendMultiplier.load(std::memory_order_relaxed));
		}

		void MarkPersisted(RuntimeRule& a_rule)
		{
			a_rule.persistedBend.store(a_rule.bend.load(std::memory_order_relaxed), std::memory_order_relaxed);
			a_rule.persistedLeafAmbient.store(a_rule.leafAmbient.load(std::memory_order_relaxed), std::memory_order_relaxed);
			a_rule.persistedUpperBendRange.store(a_rule.upperBendRange.load(std::memory_order_relaxed), std::memory_order_relaxed);
			a_rule.persistedMaximumDisplacementPercent.store(
				a_rule.maximumDisplacementPercent.load(std::memory_order_relaxed), std::memory_order_relaxed);
			a_rule.persistedTrunkGustInfluence.store(
				a_rule.trunkGustInfluence.load(std::memory_order_relaxed), std::memory_order_relaxed);
			a_rule.persistedLeafGustInfluence.store(
				a_rule.leafGustInfluence.load(std::memory_order_relaxed), std::memory_order_relaxed);
			a_rule.persistedTransientWindInfluence.store(
				a_rule.transientWindInfluence.load(std::memory_order_relaxed), std::memory_order_relaxed);
			a_rule.persistedLeafTransientWindInfluence.store(
				a_rule.leafTransientWindInfluence.load(std::memory_order_relaxed), std::memory_order_relaxed);
			a_rule.persistedLeafTransientFlutterMaximum.store(
				a_rule.leafTransientFlutterMaximum.load(std::memory_order_relaxed), std::memory_order_relaxed);
			a_rule.persistedTransientMaximumBendMultiplier.store(
				a_rule.transientMaximumBendMultiplier.load(std::memory_order_relaxed), std::memory_order_relaxed);
		}

		std::string NormalizeModelPath(std::string a_path)
		{
			a_path = Util::FixFilePath(a_path);
			while (a_path.starts_with("./"))
				a_path.erase(0, 2);
			if (const auto meshesPosition = a_path.find("meshes/"); meshesPosition != std::string::npos)
				a_path.erase(0, meshesPosition);
			else
				a_path.insert(0, "meshes/");
			return a_path;
		}

		bool IsSafeModelPath(const std::string& a_path)
		{
			return a_path.size() <= kMaximumModelPathLength && a_path.starts_with("meshes/") && a_path.ends_with(".nif") &&
			       a_path.find("../") == std::string::npos && a_path.find(':') == std::string::npos;
		}

		std::optional<float> ReadFloat(
			const nlohmann::json& a_entry,
			std::string_view a_key,
			const TreeWindSettings::Range& a_range,
			const std::filesystem::path& a_filePath,
			std::size_t a_ruleIndex)
		{
			const auto valueIt = a_entry.find(a_key);
			if (valueIt == a_entry.end())
				return std::nullopt;
			if (!valueIt->is_number()) {
				logger::warn("[TreeWindPatcher] Ignoring non-numeric '{}' in {} rule {}", a_key, a_filePath.string(), a_ruleIndex);
				return std::nullopt;
			}

			const double value = valueIt->get<double>();
			if (!std::isfinite(value)) {
				logger::warn("[TreeWindPatcher] Ignoring non-finite '{}' in {} rule {}", a_key, a_filePath.string(), a_ruleIndex);
				return std::nullopt;
			}

			return std::clamp(static_cast<float>(value), a_range.minimum, a_range.maximum);
		}

		void MergeValue(const std::optional<float>& a_value, float& a_destination,
			ResponseParameter a_parameter, LoadedRule& a_rule,
			const std::filesystem::path& a_filePath, std::set<std::string>& a_conflicts)
		{
			if (!a_value)
				return;

			const std::string fileName = a_filePath.filename().string();
			auto& source = a_rule.sources[static_cast<std::size_t>(a_parameter)];
			if (!source.empty() && source != fileName) {
				a_conflicts.insert(source);
				a_conflicts.insert(fileName);
			}
			a_destination = *a_value;
			source = fileName;
		}

		bool LoadPatchFile(const std::filesystem::path& a_filePath,
			std::unordered_map<std::string, LoadedRule>& a_rules, std::set<std::string>& a_conflicts)
		{
			std::error_code error;
			const auto fileSize = std::filesystem::file_size(a_filePath, error);
			if (error || fileSize > kMaximumPatchFileSize) {
				logger::warn("[TreeWindPatcher] Skipping unreadable or oversized patch file: {}", a_filePath.string());
				return false;
			}

			std::ifstream stream(a_filePath, std::ios::binary);
			if (!stream) {
				logger::warn("[TreeWindPatcher] Failed to open patch file: {}", a_filePath.string());
				return false;
			}

			try {
				const auto root = nlohmann::json::parse(stream);
				if (!root.is_object() || root.value("version", 0) != 1 || !root.contains("trees") || !root["trees"].is_array()) {
					logger::warn("[TreeWindPatcher] Invalid version or trees array in {}", a_filePath.string());
					return false;
				}

				const auto& treeEntries = root["trees"];
				if (treeEntries.size() > kMaximumRulesPerFile) {
					logger::warn("[TreeWindPatcher] Too many rules in {}; maximum is {}", a_filePath.string(), kMaximumRulesPerFile);
					return false;
				}

				for (std::size_t ruleIndex = 0; ruleIndex < treeEntries.size(); ++ruleIndex) {
					const auto& entry = treeEntries[ruleIndex];
					if (!entry.is_object() || !entry.contains("mesh") || !entry["mesh"].is_string()) {
						logger::warn("[TreeWindPatcher] Ignoring malformed rule {} in {}", ruleIndex, a_filePath.string());
						continue;
					}

					const auto modelPath = NormalizeModelPath(entry["mesh"].get<std::string>());
					if (!IsSafeModelPath(modelPath)) {
						logger::warn("[TreeWindPatcher] Ignoring unsafe model path in {} rule {}", a_filePath.string(), ruleIndex);
						continue;
					}

					const auto bendSensitivity =
						ReadFloat(entry, "bendSensitivity", TreeWindSettings::kBend, a_filePath, ruleIndex);
					const auto leafAmbientSensitivity =
						ReadFloat(entry, "leafAmbientSensitivity", TreeWindSettings::kLeafAmbient, a_filePath, ruleIndex);
					const auto upperBendRange =
						ReadFloat(entry, "upperBendRange", TreeWindSettings::kUpperBendPercent, a_filePath, ruleIndex);
					const auto maximumDisplacementPercent = ReadFloat(entry, "maximumDisplacementPercent",
						TreeWindSettings::kMaximumDisplacementPercent, a_filePath, ruleIndex);
					const auto trunkGustInfluence =
						ReadFloat(entry, "trunkGustInfluence", TreeWindSettings::kGustInfluence, a_filePath, ruleIndex);
					const auto leafGustInfluence =
						ReadFloat(entry, "leafGustInfluence", TreeWindSettings::kGustInfluence, a_filePath, ruleIndex);
					const auto transientWindInfluence = ReadFloat(entry, "transientWindInfluence",
						TreeWindSettings::kTransientInfluence, a_filePath, ruleIndex);
					const auto leafTransientWindInfluence = ReadFloat(entry, "leafTransientWindInfluence",
						TreeWindSettings::kTransientInfluence, a_filePath, ruleIndex);
					const auto leafTransientFlutterMaximum = ReadFloat(entry, "leafTransientFlutterMaximum",
						TreeWindSettings::kLeafTransientFlutterMaximum, a_filePath, ruleIndex);
					const auto transientMaximumBendMultiplier = ReadFloat(entry, "transientMaximumBendMultiplier",
						TreeWindSettings::kTransientMaximumBendMultiplier, a_filePath, ruleIndex);
					auto& rule = a_rules[modelPath];
					rule.ownerPath = a_filePath;
					MergeValue(bendSensitivity, rule.bend, ResponseParameter::Bend, rule, a_filePath, a_conflicts);
					MergeValue(leafAmbientSensitivity, rule.leafAmbient, ResponseParameter::LeafAmbient, rule, a_filePath, a_conflicts);
					MergeValue(upperBendRange, rule.upperBendRange, ResponseParameter::UpperBendRange, rule, a_filePath, a_conflicts);
					MergeValue(maximumDisplacementPercent, rule.maximumDisplacementPercent,
						ResponseParameter::MaximumDisplacementPercent, rule, a_filePath, a_conflicts);
					MergeValue(trunkGustInfluence, rule.trunkGustInfluence,
						ResponseParameter::TrunkGustInfluence, rule, a_filePath, a_conflicts);
					MergeValue(leafGustInfluence, rule.leafGustInfluence,
						ResponseParameter::LeafGustInfluence, rule, a_filePath, a_conflicts);
					MergeValue(transientWindInfluence, rule.transientWindInfluence,
						ResponseParameter::TransientWindInfluence, rule, a_filePath, a_conflicts);
					MergeValue(leafTransientWindInfluence, rule.leafTransientWindInfluence,
						ResponseParameter::LeafTransientWindInfluence, rule, a_filePath, a_conflicts);
					MergeValue(leafTransientFlutterMaximum, rule.leafTransientFlutterMaximum,
						ResponseParameter::LeafTransientFlutterMaximum, rule, a_filePath, a_conflicts);
					MergeValue(transientMaximumBendMultiplier, rule.transientMaximumBendMultiplier,
						ResponseParameter::TransientMaximumBendMultiplier, rule, a_filePath, a_conflicts);
				}
				return true;
			} catch (const nlohmann::json::exception& exception) {
				logger::error("[TreeWindPatcher] Failed to parse {}: {}", a_filePath.string(), exception.what());
				return false;
			}
		}

		void LoadRules()
		{
			runtimeRules.clear();
			ruleIds.clear();
			conflictingFiles.clear();
			const auto patchDirectory = Util::PathHelpers::GetWindSettingsPath();
			std::error_code error;
			if (!std::filesystem::is_directory(patchDirectory, error))
				return;

			std::vector<std::filesystem::path> patchPaths;
			std::filesystem::directory_iterator iterator(patchDirectory, error);
			for (const std::filesystem::directory_iterator end; !error && iterator != end; iterator.increment(error)) {
				if (!iterator->is_regular_file(error)) {
					error.clear();
					continue;
				}
				const std::string fileName = Util::FixFilePath(iterator->path().filename().string());
				if (!fileName.ends_with(".json") || fileName.ends_with("_backup.json") ||
					fileName == Util::FixFilePath(std::string(kSchemaFileName)))
					continue;
				patchPaths.push_back(iterator->path());
			}
			if (error)
				logger::warn("[TreeWindPatcher] Failed while scanning {}: {}", patchDirectory.string(), error.message());

			std::ranges::sort(patchPaths, [](const auto& a_lhs, const auto& a_rhs) {
				const std::string lhsName = Util::FixFilePath(a_lhs.filename().string());
				const std::string rhsName = Util::FixFilePath(a_rhs.filename().string());
				return lhsName < rhsName;
			});

			std::unordered_map<std::string, LoadedRule> mergedRules;
			std::set<std::string> conflictSet;
			std::size_t loadedFileCount = 0;
			for (const auto& patchPath : patchPaths) {
				if (LoadPatchFile(patchPath, mergedRules, conflictSet))
					++loadedFileCount;
			}
			conflictingFiles.assign(conflictSet.begin(), conflictSet.end());
			if (!conflictingFiles.empty()) {
				std::string fileList;
				for (const auto& file : conflictingFiles) {
					if (!fileList.empty())
						fileList += ", ";
					fileList += file;
				}
				logger::warn(
					"[TreeWindPatcher] duplicate tree responses found, trees may not behave as intended. "
					"Conflicting JSON files: {}",
					fileList);
			}

			std::vector<std::string> modelPaths;
			modelPaths.reserve(mergedRules.size());
			for (const auto& [modelPath, rule] : mergedRules)
				modelPaths.push_back(modelPath);
			std::ranges::sort(modelPaths);

			runtimeRules.reserve(modelPaths.size());
			ruleIds.reserve(modelPaths.size());
			for (const auto& modelPath : modelPaths) {
				const auto& merged = mergedRules.at(modelPath);
				auto rule = std::make_unique<RuntimeRule>();
				rule->mesh = modelPath;
				rule->ownerPath = merged.ownerPath;
				rule->bend.store(merged.bend, std::memory_order_relaxed);
				rule->leafAmbient.store(merged.leafAmbient, std::memory_order_relaxed);
				rule->upperBendRange.store(merged.upperBendRange, std::memory_order_relaxed);
				rule->maximumDisplacementPercent.store(merged.maximumDisplacementPercent, std::memory_order_relaxed);
				rule->trunkGustInfluence.store(merged.trunkGustInfluence, std::memory_order_relaxed);
				rule->leafGustInfluence.store(merged.leafGustInfluence, std::memory_order_relaxed);
				rule->transientWindInfluence.store(merged.transientWindInfluence, std::memory_order_relaxed);
				rule->leafTransientWindInfluence.store(merged.leafTransientWindInfluence, std::memory_order_relaxed);
				rule->leafTransientFlutterMaximum.store(merged.leafTransientFlutterMaximum, std::memory_order_relaxed);
				rule->transientMaximumBendMultiplier.store(merged.transientMaximumBendMultiplier, std::memory_order_relaxed);
				rule->persistedBend.store(merged.bend, std::memory_order_relaxed);
				rule->persistedLeafAmbient.store(merged.leafAmbient, std::memory_order_relaxed);
				rule->persistedUpperBendRange.store(merged.upperBendRange, std::memory_order_relaxed);
				rule->persistedMaximumDisplacementPercent.store(merged.maximumDisplacementPercent, std::memory_order_relaxed);
				rule->persistedTrunkGustInfluence.store(merged.trunkGustInfluence, std::memory_order_relaxed);
				rule->persistedLeafGustInfluence.store(merged.leafGustInfluence, std::memory_order_relaxed);
				rule->persistedTransientWindInfluence.store(merged.transientWindInfluence, std::memory_order_relaxed);
				rule->persistedLeafTransientWindInfluence.store(
					merged.leafTransientWindInfluence, std::memory_order_relaxed);
				rule->persistedLeafTransientFlutterMaximum.store(
					merged.leafTransientFlutterMaximum, std::memory_order_relaxed);
				rule->persistedTransientMaximumBendMultiplier.store(merged.transientMaximumBendMultiplier, std::memory_order_relaxed);
				const auto id = static_cast<std::uint32_t>(runtimeRules.size() + 1);
				ruleIds.emplace(rule->mesh, id);
				runtimeRules.push_back(std::move(rule));
			}

			if (loadedFileCount > 0)
				logger::info("[TreeWindPatcher] Loaded {} model rules from {} JSON files in {}",
					runtimeRules.size(), loadedFileCount, patchDirectory.string());
		}

		void IncludePoint(ModelAabb& a_bounds, const RE::NiPoint3& a_point)
		{
			if (!std::isfinite(a_point.x) || !std::isfinite(a_point.y) || !std::isfinite(a_point.z))
				return;

			a_bounds.minimum.x = std::min(a_bounds.minimum.x, a_point.x);
			a_bounds.minimum.y = std::min(a_bounds.minimum.y, a_point.y);
			a_bounds.minimum.z = std::min(a_bounds.minimum.z, a_point.z);
			a_bounds.maximum.x = std::max(a_bounds.maximum.x, a_point.x);
			a_bounds.maximum.y = std::max(a_bounds.maximum.y, a_point.y);
			a_bounds.maximum.z = std::max(a_bounds.maximum.z, a_point.z);
			a_bounds.valid = true;
		}

		void IncludeTransformedBox(ModelAabb& a_bounds, const RE::NiPoint3& a_center,
			const RE::NiPoint3& a_extents, const RE::NiTransform& a_transform)
		{
			for (int x = -1; x <= 1; x += 2) {
				for (int y = -1; y <= 1; y += 2) {
					for (int z = -1; z <= 1; z += 2) {
						IncludePoint(a_bounds, a_transform * RE::NiPoint3{
																 a_center.x + a_extents.x * static_cast<float>(x),
																 a_center.y + a_extents.y * static_cast<float>(y),
																 a_center.z + a_extents.z * static_cast<float>(z) });
					}
				}
			}
		}

		std::optional<RE::NiTransform> GetTransformToAncestor(
			const RE::NiAVObject* a_object, const RE::NiAVObject* a_ancestor)
		{
			RE::NiTransform transform;
			const auto* current = a_object;
			while (current && current != a_ancestor) {
				transform = current->local * transform;
				current = current->parent;
			}
			return current == a_ancestor ? std::optional{ transform } : std::nullopt;
		}

		const RE::BSBound* FindAuthoredBound(
			const RE::NiAVObject* a_object, const RE::NiAVObject*& a_owner)
		{
			static REL::Relocation<const RE::NiRTTI*> boundRTTI{ RE::BSBound::Ni_RTTI };
			for (const auto* current = a_object; current; current = current->parent) {
				for (std::uint16_t index = 0; index < current->GetExtraDataSize(); ++index) {
					const auto* extraData = current->GetExtraDataAt(index);
					if (extraData && extraData->GetRTTI() == boundRTTI.get()) {
						a_owner = current;
						return static_cast<const RE::BSBound*>(extraData);
					}
				}
			}
			return nullptr;
		}

		bool IncludeGeometryVertices(
			ModelAabb& a_bounds, RE::BSGeometry& a_geometry, const RE::NiTransform& a_geometryToLeaf)
		{
			auto* rendererData = a_geometry.GetGeometryRuntimeData().rendererData;
			auto* triShape = a_geometry.AsTriShape();
			if (!rendererData || !triShape || !rendererData->rawVertexData ||
				!rendererData->vertexDesc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_VERTEX))
				return false;

			const std::uint32_t vertexSize = rendererData->vertexDesc.GetSize();
			const std::uint32_t vertexCount = triShape->GetTrishapeRuntimeData().vertexCount;
			if (vertexSize < sizeof(RE::NiPoint3) || vertexCount == 0)
				return false;

			bool foundVertex = false;
#if defined(_MSC_VER)
			__try
#endif
			{
				for (std::uint32_t index = 0; index < vertexCount; ++index) {
					RE::NiPoint3 position;
					std::memcpy(&position,
						rendererData->rawVertexData + static_cast<std::size_t>(vertexSize) * index,
						sizeof(position));
					IncludePoint(a_bounds, a_geometryToLeaf * position);
					foundVertex = true;
				}
			}
#if defined(_MSC_VER)
			__except (1) {
				return false;
			}
#endif
			return foundVertex;
		}

		ModelAabb CalculateTreeBounds(RE::BSLeafAnimNode& a_leafParent)
		{
			ModelAabb bounds;
			bool usedNonVertexFallback = false;
			RE::BSVisit::TraverseScenegraphObjects(&a_leafParent, [&](RE::NiAVObject* a_object) {
				if (auto* geometry = a_object->AsGeometry()) {
					const auto geometryToLeaf = GetTransformToAncestor(geometry, &a_leafParent);
					const auto& modelBound = geometry->GetModelData().modelBound;
					const bool hasVertexBounds =
						geometryToLeaf && IncludeGeometryVertices(bounds, *geometry, *geometryToLeaf);
					if (!hasVertexBounds)
						usedNonVertexFallback = true;
					if (geometryToLeaf && !hasVertexBounds && std::isfinite(modelBound.radius) && modelBound.radius > 0.0f) {
						const auto center = *geometryToLeaf * modelBound.center;
						const float radius = std::abs(geometryToLeaf->scale) * modelBound.radius;
						IncludeTransformedBox(bounds, center, { radius, radius, radius }, RE::NiTransform{});
					}
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
			if (bounds.valid && !usedNonVertexFallback)
				return bounds;

			const RE::NiAVObject* boundOwner = nullptr;
			if (const auto* authoredBound = FindAuthoredBound(&a_leafParent, boundOwner)) {
				if (const auto leafToOwner = GetTransformToAncestor(&a_leafParent, boundOwner)) {
					ModelAabb authoredBounds;
					IncludeTransformedBox(
						authoredBounds, authoredBound->center, authoredBound->extents, leafToOwner->Invert());
					if (authoredBounds.valid)
						return authoredBounds;
				}
			}
			return bounds;
		}

		void SetTreeBoundsExtraData(RE::BSGeometry& a_geometry, const ModelAabb& a_leafBounds,
			const RE::NiTransform& a_geometryToLeaf)
		{
			ModelAabb geometryBounds;
			const auto leafToGeometry = a_geometryToLeaf.Invert();
			const RE::NiPoint3 center{
				(a_leafBounds.minimum.x + a_leafBounds.maximum.x) * 0.5f,
				(a_leafBounds.minimum.y + a_leafBounds.maximum.y) * 0.5f,
				(a_leafBounds.minimum.z + a_leafBounds.maximum.z) * 0.5f
			};
			const RE::NiPoint3 extents{
				(a_leafBounds.maximum.x - a_leafBounds.minimum.x) * 0.5f,
				(a_leafBounds.maximum.y - a_leafBounds.minimum.y) * 0.5f,
				(a_leafBounds.maximum.z - a_leafBounds.minimum.z) * 0.5f
			};
			IncludeTransformedBox(geometryBounds, center, extents, leafToGeometry);
			if (!geometryBounds.valid)
				return;

			const float height = geometryBounds.maximum.z - geometryBounds.minimum.z;
			if (height <= kMinimumBoundExtent)
				return;
			// Split tree geometry must sample one shared leaf-node axis despite having different local transforms.
			const float probeBaseHeight = std::clamp(0.0f, a_leafBounds.minimum.z, a_leafBounds.maximum.z);
			const RE::NiPoint3 probeBase = leafToGeometry * RE::NiPoint3{ 0.0f, 0.0f, probeBaseHeight };
			const RE::NiPoint3 probeTop = leafToGeometry * RE::NiPoint3{ 0.0f, 0.0f, a_leafBounds.maximum.z };
			if (!std::isfinite(probeBase.x) || !std::isfinite(probeBase.y) || !std::isfinite(probeBase.z) ||
				!std::isfinite(probeTop.x) || !std::isfinite(probeTop.y) || !std::isfinite(probeTop.z))
				return;

			const std::vector values{
				geometryBounds.minimum.z,
				height,
				probeBase.x,
				probeBase.y,
				probeBase.z,
				probeTop.x,
				probeTop.y,
				probeTop.z
			};
			if (auto* existing = a_geometry.GetExtraData(kTreeBoundsName)) {
				static REL::Relocation<const RE::NiRTTI*> floatsExtraDataRTTI{ RE::NiFloatsExtraData::Ni_RTTI };
				if (existing->GetRTTI() == floatsExtraDataRTTI.get()) {
					auto* boundsData = static_cast<RE::NiFloatsExtraData*>(existing);
					if (boundsData->size == kTreeBoundsValueCount && boundsData->value)
						std::ranges::copy(values, boundsData->value);
				} else {
					logger::warn("[TreeWindPatcher] Extra data '{}' exists with an incompatible type", kTreeBoundsName.c_str());
				}
				return;
			}

			if (auto* extraData = RE::NiFloatsExtraData::Create(kTreeBoundsName, values))
				a_geometry.AddExtraData(extraData);
		}

		void SetIntegerExtraData(RE::NiObjectNET& a_object, const RE::BSFixedString& a_name, std::int32_t a_value)
		{
			if (auto* existing = a_object.GetExtraData(a_name)) {
				static REL::Relocation<const RE::NiRTTI*> integerExtraDataRTTI{ RE::NiIntegerExtraData::Ni_RTTI };
				if (existing->GetRTTI() == integerExtraDataRTTI.get())
					static_cast<RE::NiIntegerExtraData*>(existing)->value = a_value;
				else
					logger::warn("[TreeWindPatcher] Extra data '{}' exists with an incompatible type", a_name.c_str());
				return;
			}

			if (auto* extraData = RE::NiIntegerExtraData::Create(a_name, a_value))
				a_object.AddExtraData(extraData);
		}

		void ApplyModelData(const char* a_modelName, RE::NiNode* a_root)
		{
			if (!a_modelName || !a_root)
				return;

			const auto modelPath = NormalizeModelPath(a_modelName);
			const auto ruleIt = ruleIds.find(modelPath);
			const bool hasRule = ruleIt != ruleIds.end();
			if (!hasRule && modelPath.find("/trees/") == std::string::npos)
				return;

			std::size_t patchedParentCount = 0;
			RE::BSVisit::TraverseScenegraphObjects(a_root, [&](RE::NiAVObject* a_object) {
				if (auto* leafParent = netimmerse_cast<RE::BSLeafAnimNode*>(a_object)) {
					if (hasRule)
						SetIntegerExtraData(*leafParent, kRuleIdName, static_cast<std::int32_t>(ruleIt->second));

					const auto bounds = CalculateTreeBounds(*leafParent);
					if (bounds.valid) {
						RE::BSVisit::TraverseScenegraphObjects(leafParent, [&](RE::NiAVObject* a_child) {
							if (auto* geometry = a_child->AsGeometry()) {
								if (const auto geometryToLeaf = GetTransformToAncestor(geometry, leafParent))
									SetTreeBoundsExtraData(*geometry, bounds, *geometryToLeaf);
							}
							return RE::BSVisit::BSVisitControl::kContinue;
						});
					}
					++patchedParentCount;
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});

			if (hasRule && patchedParentCount == 0)
				logger::debug("[TreeWindPatcher] Matched {} but found no BSLeafAnimNode", a_modelName);
			else if (hasRule)
				logger::debug("[TreeWindPatcher] Applied rule {} to {} leaf parents", ruleIt->second, patchedParentCount);
		}

		void ReadTreeBoundsExtraData(const RE::BSGeometry& a_geometry, Sensitivities& a_values)
		{
			static REL::Relocation<const RE::NiRTTI*> floatsExtraDataRTTI{ RE::NiFloatsExtraData::Ni_RTTI };
			const auto* data = a_geometry.GetExtraData(kTreeBoundsName);
			if (!data || data->GetRTTI() != floatsExtraDataRTTI.get())
				return;

			const auto* boundsData = static_cast<const RE::NiFloatsExtraData*>(data);
			if (boundsData->size != kTreeBoundsValueCount || !boundsData->value)
				return;

			const float minimumZ = boundsData->value[0];
			const float height = boundsData->value[1];
			const float3 probeBase{ boundsData->value[2], boundsData->value[3], boundsData->value[4] };
			const float3 probeTop{ boundsData->value[5], boundsData->value[6], boundsData->value[7] };
			if (!std::isfinite(minimumZ) || !std::isfinite(height) || height <= kMinimumBoundExtent ||
				!std::isfinite(probeBase.x) || !std::isfinite(probeBase.y) || !std::isfinite(probeBase.z) ||
				!std::isfinite(probeTop.x) || !std::isfinite(probeTop.y) || !std::isfinite(probeTop.z))
				return;

			a_values.boundMinimumZ = minimumZ;
			a_values.boundHeight = height;
			a_values.probeBase = probeBase;
			a_values.probeTop = probeTop;
			a_values.hasBounds = true;
		}

		struct TESProcessorPostCreate
		{
			static void thunk(
				RE::TESModelDB::TESProcessor* a_this,
				const RE::BSModelDB::DBTraits::ArgsType& a_args,
				const char* a_modelName,
				RE::NiPointer<RE::NiNode>& a_root,
				std::uint32_t& a_typeOut)
			{
				func(a_this, a_args, a_modelName, a_root, a_typeOut);
				ApplyModelData(a_modelName, a_root.get());
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void LoadAndInstall()
	{
		LoadRules();
		stl::write_vfunc<0x1, TESProcessorPostCreate>(RE::VTABLE_TESModelDB____TESProcessor[0]);
		logger::info("[TreeWindPatcher] Installed model creation hook");
	}

	Sensitivities GetSensitivities(const RE::BSGeometry* a_geometry)
	{
		Sensitivities sensitivities;
		if (!a_geometry)
			return sensitivities;
		ReadTreeBoundsExtraData(*a_geometry, sensitivities);
		if (universalOverrideEnabled.load(std::memory_order_acquire)) {
			sensitivities.bend = universalBend.load(std::memory_order_relaxed);
			sensitivities.leafAmbient = universalLeafAmbient.load(std::memory_order_relaxed);
			sensitivities.upperBendRange = universalUpperBendRange.load(std::memory_order_relaxed);
			sensitivities.maximumDisplacementPercent = universalMaximumDisplacementPercent.load(std::memory_order_relaxed);
			sensitivities.trunkGustInfluence = universalTrunkGustInfluence.load(std::memory_order_relaxed);
			sensitivities.leafGustInfluence = universalLeafGustInfluence.load(std::memory_order_relaxed);
			sensitivities.transientWindInfluence = universalTransientWindInfluence.load(std::memory_order_relaxed);
			sensitivities.leafTransientWindInfluence =
				universalLeafTransientWindInfluence.load(std::memory_order_relaxed);
			sensitivities.leafTransientFlutterMaximum =
				universalLeafTransientFlutterMaximum.load(std::memory_order_relaxed);
			sensitivities.transientMaximumBendMultiplier =
				universalTransientMaximumBendMultiplier.load(std::memory_order_relaxed);
			return sensitivities;
		}

		const auto* leafParent = netimmerse_cast<RE::BSLeafAnimNode*>(a_geometry->parent);
		if (!leafParent)
			return sensitivities;

		static REL::Relocation<const RE::NiRTTI*> integerExtraDataRTTI{ RE::NiIntegerExtraData::Ni_RTTI };
		if (const auto* data = leafParent->GetExtraData(kRuleIdName);
			data && data->GetRTTI() == integerExtraDataRTTI.get()) {
			const auto id = static_cast<const RE::NiIntegerExtraData*>(data)->value;
			if (id > 0 && static_cast<std::size_t>(id) <= runtimeRules.size()) {
				const auto& rule = *runtimeRules[static_cast<std::size_t>(id) - 1];
				sensitivities.bend = rule.bend.load(std::memory_order_relaxed);
				sensitivities.leafAmbient = rule.leafAmbient.load(std::memory_order_relaxed);
				sensitivities.upperBendRange = rule.upperBendRange.load(std::memory_order_relaxed);
				sensitivities.maximumDisplacementPercent = rule.maximumDisplacementPercent.load(std::memory_order_relaxed);
				sensitivities.trunkGustInfluence = rule.trunkGustInfluence.load(std::memory_order_relaxed);
				sensitivities.leafGustInfluence = rule.leafGustInfluence.load(std::memory_order_relaxed);
				sensitivities.transientWindInfluence = rule.transientWindInfluence.load(std::memory_order_relaxed);
				sensitivities.leafTransientWindInfluence =
					rule.leafTransientWindInfluence.load(std::memory_order_relaxed);
				sensitivities.leafTransientFlutterMaximum =
					rule.leafTransientFlutterMaximum.load(std::memory_order_relaxed);
				sensitivities.transientMaximumBendMultiplier =
					rule.transientMaximumBendMultiplier.load(std::memory_order_relaxed);
				return sensitivities;
			}
		}

		return sensitivities;
	}

	std::size_t GetRuleCount()
	{
		return runtimeRules.size();
	}

	RuleSnapshot GetRule(std::size_t a_index)
	{
		if (a_index >= runtimeRules.size())
			return {};

		const auto& rule = *runtimeRules[a_index];
		const float bend = rule.bend.load(std::memory_order_relaxed);
		const float leafAmbient = rule.leafAmbient.load(std::memory_order_relaxed);
		const float upperBendRange = rule.upperBendRange.load(std::memory_order_relaxed);
		const float maximumDisplacementPercent = rule.maximumDisplacementPercent.load(std::memory_order_relaxed);
		const float trunkGustInfluence = rule.trunkGustInfluence.load(std::memory_order_relaxed);
		const float leafGustInfluence = rule.leafGustInfluence.load(std::memory_order_relaxed);
		const float transientWindInfluence = rule.transientWindInfluence.load(std::memory_order_relaxed);
		const float leafTransientWindInfluence = rule.leafTransientWindInfluence.load(std::memory_order_relaxed);
		const float leafTransientFlutterMaximum = rule.leafTransientFlutterMaximum.load(std::memory_order_relaxed);
		const float transientMaximumBendMultiplier =
			rule.transientMaximumBendMultiplier.load(std::memory_order_relaxed);
		return {
			static_cast<std::uint32_t>(a_index + 1),
			rule.mesh,
			bend,
			leafAmbient,
			upperBendRange,
			maximumDisplacementPercent,
			trunkGustInfluence,
			leafGustInfluence,
			transientWindInfluence,
			leafTransientWindInfluence,
			leafTransientFlutterMaximum,
			transientMaximumBendMultiplier,
			ValuesDiffer(bend, rule.persistedBend.load(std::memory_order_relaxed)) ||
				ValuesDiffer(leafAmbient, rule.persistedLeafAmbient.load(std::memory_order_relaxed)) ||
				ValuesDiffer(upperBendRange, rule.persistedUpperBendRange.load(std::memory_order_relaxed)) ||
				ValuesDiffer(maximumDisplacementPercent, rule.persistedMaximumDisplacementPercent.load(std::memory_order_relaxed)) ||
				ValuesDiffer(trunkGustInfluence, rule.persistedTrunkGustInfluence.load(std::memory_order_relaxed)) ||
				ValuesDiffer(leafGustInfluence, rule.persistedLeafGustInfluence.load(std::memory_order_relaxed)) ||
				ValuesDiffer(transientWindInfluence, rule.persistedTransientWindInfluence.load(std::memory_order_relaxed)) ||
				ValuesDiffer(leafTransientWindInfluence,
					rule.persistedLeafTransientWindInfluence.load(std::memory_order_relaxed)) ||
				ValuesDiffer(leafTransientFlutterMaximum,
					rule.persistedLeafTransientFlutterMaximum.load(std::memory_order_relaxed)) ||
				ValuesDiffer(transientMaximumBendMultiplier,
					rule.persistedTransientMaximumBendMultiplier.load(std::memory_order_relaxed))
		};
	}

	bool SetRule(std::size_t a_index, float a_bend, float a_leafAmbient,
		float a_upperBendRange, float a_maximumDisplacementPercent,
		float a_trunkGustInfluence, float a_leafGustInfluence,
		float a_transientWindInfluence, float a_leafTransientWindInfluence,
		float a_leafTransientFlutterMaximum,
		float a_transientMaximumBendMultiplier)
	{
		if (a_index >= runtimeRules.size())
			return false;
		a_bend = ClampFiniteOrDefault(a_bend, TreeWindSettings::kBend, 1.0f);
		a_leafAmbient = ClampFiniteOrDefault(a_leafAmbient, TreeWindSettings::kLeafAmbient, 1.0f);
		a_upperBendRange = ClampFiniteOrDefault(
			a_upperBendRange, TreeWindSettings::kUpperBendPercent, kDefaultUpperBendRange);
		a_maximumDisplacementPercent = ClampFiniteOrDefault(a_maximumDisplacementPercent,
			TreeWindSettings::kMaximumDisplacementPercent, kDefaultMaximumDisplacementPercent);
		a_trunkGustInfluence = ClampFiniteOrDefault(
			a_trunkGustInfluence, TreeWindSettings::kGustInfluence, kDefaultTrunkGustInfluence);
		a_leafGustInfluence = ClampFiniteOrDefault(
			a_leafGustInfluence, TreeWindSettings::kGustInfluence, kDefaultLeafGustInfluence);
		a_transientWindInfluence = ClampFiniteOrDefault(a_transientWindInfluence,
			TreeWindSettings::kTransientInfluence, kDefaultTransientWindInfluence);
		a_leafTransientWindInfluence = ClampFiniteOrDefault(a_leafTransientWindInfluence,
			TreeWindSettings::kTransientInfluence, kDefaultLeafTransientWindInfluence);
		a_leafTransientFlutterMaximum = ClampFiniteOrDefault(a_leafTransientFlutterMaximum,
			TreeWindSettings::kLeafTransientFlutterMaximum, kDefaultLeafTransientFlutterMaximum);
		a_transientMaximumBendMultiplier = ClampFiniteOrDefault(a_transientMaximumBendMultiplier,
			TreeWindSettings::kTransientMaximumBendMultiplier, kDefaultTransientMaximumBendMultiplier);
		runtimeRules[a_index]->bend.store(a_bend, std::memory_order_relaxed);
		runtimeRules[a_index]->leafAmbient.store(a_leafAmbient, std::memory_order_relaxed);
		runtimeRules[a_index]->upperBendRange.store(a_upperBendRange, std::memory_order_relaxed);
		runtimeRules[a_index]->maximumDisplacementPercent.store(a_maximumDisplacementPercent, std::memory_order_relaxed);
		runtimeRules[a_index]->trunkGustInfluence.store(a_trunkGustInfluence, std::memory_order_relaxed);
		runtimeRules[a_index]->leafGustInfluence.store(a_leafGustInfluence, std::memory_order_relaxed);
		runtimeRules[a_index]->transientWindInfluence.store(a_transientWindInfluence, std::memory_order_relaxed);
		runtimeRules[a_index]->leafTransientWindInfluence.store(
			a_leafTransientWindInfluence, std::memory_order_relaxed);
		runtimeRules[a_index]->leafTransientFlutterMaximum.store(
			a_leafTransientFlutterMaximum, std::memory_order_relaxed);
		runtimeRules[a_index]->transientMaximumBendMultiplier.store(
			a_transientMaximumBendMultiplier, std::memory_order_relaxed);
		return true;
	}

	bool SetRule(std::string_view a_mesh, float a_bend, float a_leafAmbient,
		float a_upperBendRange, float a_maximumDisplacementPercent,
		float a_trunkGustInfluence, float a_leafGustInfluence,
		float a_transientWindInfluence, float a_leafTransientWindInfluence,
		float a_leafTransientFlutterMaximum,
		float a_transientMaximumBendMultiplier)
	{
		const auto normalized = NormalizeModelPath(std::string(a_mesh));
		const auto ruleIt = ruleIds.find(normalized);
		return ruleIt != ruleIds.end() && SetRule(static_cast<std::size_t>(ruleIt->second) - 1, a_bend, a_leafAmbient,
											  a_upperBendRange, a_maximumDisplacementPercent, a_trunkGustInfluence, a_leafGustInfluence,
											  a_transientWindInfluence, a_leafTransientWindInfluence,
											  a_leafTransientFlutterMaximum,
											  a_transientMaximumBendMultiplier);
	}

	void SetUniversalOverride(bool a_enabled, const Sensitivities& a_values)
	{
		universalBend.store(
			ClampFiniteOrDefault(a_values.bend, TreeWindSettings::kBend, 1.0f), std::memory_order_relaxed);
		universalLeafAmbient.store(
			ClampFiniteOrDefault(a_values.leafAmbient, TreeWindSettings::kLeafAmbient, 1.0f), std::memory_order_relaxed);
		universalUpperBendRange.store(ClampFiniteOrDefault(a_values.upperBendRange,
										  TreeWindSettings::kUpperBendPercent, kDefaultUpperBendRange),
			std::memory_order_relaxed);
		universalMaximumDisplacementPercent.store(ClampFiniteOrDefault(a_values.maximumDisplacementPercent,
													  TreeWindSettings::kMaximumDisplacementPercent, kDefaultMaximumDisplacementPercent),
			std::memory_order_relaxed);
		universalTrunkGustInfluence.store(ClampFiniteOrDefault(a_values.trunkGustInfluence,
											  TreeWindSettings::kGustInfluence, kDefaultTrunkGustInfluence),
			std::memory_order_relaxed);
		universalLeafGustInfluence.store(ClampFiniteOrDefault(a_values.leafGustInfluence,
											 TreeWindSettings::kGustInfluence, kDefaultLeafGustInfluence),
			std::memory_order_relaxed);
		universalTransientWindInfluence.store(ClampFiniteOrDefault(a_values.transientWindInfluence,
												  TreeWindSettings::kTransientInfluence, kDefaultTransientWindInfluence),
			std::memory_order_relaxed);
		universalLeafTransientWindInfluence.store(ClampFiniteOrDefault(a_values.leafTransientWindInfluence,
													  TreeWindSettings::kTransientInfluence, kDefaultLeafTransientWindInfluence),
			std::memory_order_relaxed);
		universalLeafTransientFlutterMaximum.store(ClampFiniteOrDefault(a_values.leafTransientFlutterMaximum,
													   TreeWindSettings::kLeafTransientFlutterMaximum, kDefaultLeafTransientFlutterMaximum),
			std::memory_order_relaxed);
		universalTransientMaximumBendMultiplier.store(ClampFiniteOrDefault(
														  a_values.transientMaximumBendMultiplier, TreeWindSettings::kTransientMaximumBendMultiplier,
														  kDefaultTransientMaximumBendMultiplier),
			std::memory_order_relaxed);
		universalOverrideEnabled.store(a_enabled, std::memory_order_release);
	}

	std::pair<bool, Sensitivities> GetUniversalOverride()
	{
		Sensitivities values;
		values.bend = universalBend.load(std::memory_order_relaxed);
		values.leafAmbient = universalLeafAmbient.load(std::memory_order_relaxed);
		values.upperBendRange = universalUpperBendRange.load(std::memory_order_relaxed);
		values.maximumDisplacementPercent = universalMaximumDisplacementPercent.load(std::memory_order_relaxed);
		values.trunkGustInfluence = universalTrunkGustInfluence.load(std::memory_order_relaxed);
		values.leafGustInfluence = universalLeafGustInfluence.load(std::memory_order_relaxed);
		values.transientWindInfluence = universalTransientWindInfluence.load(std::memory_order_relaxed);
		values.leafTransientWindInfluence = universalLeafTransientWindInfluence.load(std::memory_order_relaxed);
		values.leafTransientFlutterMaximum = universalLeafTransientFlutterMaximum.load(std::memory_order_relaxed);
		values.transientMaximumBendMultiplier =
			universalTransientMaximumBendMultiplier.load(std::memory_order_relaxed);
		return { universalOverrideEnabled.load(std::memory_order_acquire), values };
	}

	void RevertUnsavedChanges()
	{
		for (auto& rule : runtimeRules) {
			rule->bend.store(rule->persistedBend.load(std::memory_order_relaxed), std::memory_order_relaxed);
			rule->leafAmbient.store(rule->persistedLeafAmbient.load(std::memory_order_relaxed), std::memory_order_relaxed);
			rule->upperBendRange.store(rule->persistedUpperBendRange.load(std::memory_order_relaxed), std::memory_order_relaxed);
			rule->maximumDisplacementPercent.store(rule->persistedMaximumDisplacementPercent.load(std::memory_order_relaxed), std::memory_order_relaxed);
			rule->trunkGustInfluence.store(rule->persistedTrunkGustInfluence.load(std::memory_order_relaxed), std::memory_order_relaxed);
			rule->leafGustInfluence.store(rule->persistedLeafGustInfluence.load(std::memory_order_relaxed), std::memory_order_relaxed);
			rule->transientWindInfluence.store(rule->persistedTransientWindInfluence.load(std::memory_order_relaxed), std::memory_order_relaxed);
			rule->leafTransientWindInfluence.store(
				rule->persistedLeafTransientWindInfluence.load(std::memory_order_relaxed), std::memory_order_relaxed);
			rule->leafTransientFlutterMaximum.store(
				rule->persistedLeafTransientFlutterMaximum.load(std::memory_order_relaxed), std::memory_order_relaxed);
			rule->transientMaximumBendMultiplier.store(
				rule->persistedTransientMaximumBendMultiplier.load(std::memory_order_relaxed), std::memory_order_relaxed);
		}
	}

	SaveResult SaveRules()
	{
		std::scoped_lock lock(saveMutex);
		SaveResult result;

		std::map<std::filesystem::path, std::vector<RuntimeRule*>> dirtyRulesByFile;
		for (const auto& rule : runtimeRules) {
			if (HasUnsavedValues(*rule))
				dirtyRulesByFile[rule->ownerPath].push_back(rule.get());
		}
		if (dirtyRulesByFile.empty()) {
			result.success = true;
			return result;
		}

		struct PendingFile
		{
			std::filesystem::path path;
			nlohmann::json root;
			std::vector<std::pair<RuntimeRule*, std::size_t>> rules;
		};
		std::vector<PendingFile> pendingFiles;
		pendingFiles.reserve(dirtyRulesByFile.size());

		for (const auto& [path, rules] : dirtyRulesByFile) {
			if (path.empty()) {
				result.error = "A changed tree rule has no source JSON file";
				return result;
			}
			std::error_code error;
			const auto fileSize = std::filesystem::file_size(path, error);
			if (error || fileSize > kMaximumPatchFileSize) {
				result.error = std::format("Source JSON is unreadable or oversized: {}", path.string());
				return result;
			}

			std::ifstream input(path, std::ios::binary);
			if (!input) {
				result.error = std::format("Failed to open source JSON: {}", path.string());
				return result;
			}

			PendingFile pending{ path };
			try {
				pending.root = nlohmann::json::parse(input);
			} catch (const nlohmann::json::exception& exception) {
				result.error = std::format("Failed to parse {}: {}", path.string(), exception.what());
				return result;
			}
			if (!pending.root.is_object() || pending.root.value("version", 0) != 1 ||
				!pending.root.contains("trees") || !pending.root["trees"].is_array()) {
				result.error = std::format("Invalid version or trees array in {}", path.string());
				return result;
			}

			for (auto* rule : rules) {
				std::optional<std::size_t> targetIndex;
				for (std::size_t index = 0; index < pending.root["trees"].size(); ++index) {
					const auto& entry = pending.root["trees"][index];
					if (!entry.is_object() || !entry.contains("mesh") || !entry["mesh"].is_string())
						continue;
					const auto modelPath = NormalizeModelPath(entry["mesh"].get<std::string>());
					if (IsSafeModelPath(modelPath) && modelPath == rule->mesh)
						targetIndex = index;
				}
				if (!targetIndex) {
					result.error = std::format("Tree {} is no longer present in {}", rule->mesh, path.string());
					return result;
				}
				pending.rules.emplace_back(rule, *targetIndex);
			}
			pendingFiles.push_back(std::move(pending));
		}

		for (auto& pending : pendingFiles) {
			for (const auto& [rule, entryIndex] : pending.rules) {
				auto& entry = pending.root["trees"][entryIndex];
				const auto writeChanged = [&](std::string_view a_name, const auto& a_value, const auto& a_persisted) {
					const float value = a_value.load(std::memory_order_relaxed);
					if (ValuesDiffer(value, a_persisted.load(std::memory_order_relaxed)))
						entry[std::string(a_name)] = value;
				};
				writeChanged("bendSensitivity", rule->bend, rule->persistedBend);
				writeChanged("leafAmbientSensitivity", rule->leafAmbient, rule->persistedLeafAmbient);
				writeChanged("upperBendRange", rule->upperBendRange, rule->persistedUpperBendRange);
				writeChanged("maximumDisplacementPercent", rule->maximumDisplacementPercent,
					rule->persistedMaximumDisplacementPercent);
				writeChanged("trunkGustInfluence", rule->trunkGustInfluence, rule->persistedTrunkGustInfluence);
				writeChanged("leafGustInfluence", rule->leafGustInfluence, rule->persistedLeafGustInfluence);
				writeChanged("transientWindInfluence", rule->transientWindInfluence,
					rule->persistedTransientWindInfluence);
				writeChanged("leafTransientWindInfluence", rule->leafTransientWindInfluence,
					rule->persistedLeafTransientWindInfluence);
				writeChanged("leafTransientFlutterMaximum", rule->leafTransientFlutterMaximum,
					rule->persistedLeafTransientFlutterMaximum);
				writeChanged("transientMaximumBendMultiplier", rule->transientMaximumBendMultiplier,
					rule->persistedTransientMaximumBendMultiplier);
			}

			if (!WriteJsonAtomically(pending.path, pending.root, result.error))
				return result;

			if (!result.path.empty())
				result.path += ", ";
			result.path += pending.path.string();
			for (const auto& [rule, entryIndex] : pending.rules) {
				(void)entryIndex;
				MarkPersisted(*rule);
				++result.savedRuleCount;
			}
		}

		result.success = true;
		logger::info("[TreeWindPatcher] Saved {} changed model rules to {}", result.savedRuleCount, result.path);
		return result;
	}

	std::size_t GetUnsavedRuleCount()
	{
		return static_cast<std::size_t>(std::ranges::count_if(
			runtimeRules, [](const auto& a_rule) { return HasUnsavedValues(*a_rule); }));
	}

	std::vector<std::string> GetConflictingFiles()
	{
		return conflictingFiles;
	}
}
