import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_scene_settings_policy import GENERATOR, MANAGER_PATH, ROOT, extract_function


def braced(source, declaration):
    start = source.index(declaration)
    opening = source.index("{", start)
    closing = GENERATOR.find_matching_brace(source, opening)
    if closing < 0:
        raise AssertionError(f"Unbalanced declaration: {declaration}")
    return source[start:closing + 1]


class SceneSettingsPersistenceTests(unittest.TestCase):
    def test_exports_and_loaders_share_mode_metadata(self):
        manager = MANAGER_PATH.read_text(encoding="utf-8")
        for name in ("ExportWeatherUserSettingsToOverwrites", "ExportLocationUserSettingsToOverwrites",
                     "ExportEntryLayerToOverwrites"):
            body = extract_function(manager, name)
            self.assertIn("kMetadataTimeOfDayEnabledKey", body)
            self.assertIn("timeOfDayEnabled", body)
        for name in ("LoadLocationUserSettings", "LoadWeatherUserSettings"):
            body = extract_function(manager, name)
            self.assertIn("userTimeOfDayEnabled", body)
            self.assertIn("RefreshTimeOfDayMode", body)
        saving = extract_function(manager, "SaveAllUserSettings")
        self.assertIn("configIt->second.userTimeOfDayEnabled.has_value()", saving)
        self.assertIn('locationEntry["timeOfDayEnabled"] = *config.userTimeOfDayEnabled', saving)
        reload = extract_function(manager, "ReloadOverwriteEntries")
        self.assertEqual(reload.count("overwriteTimeOfDayEnabled.reset()"), 2)
        self.assertNotIn("userTimeOfDayEnabled.reset()", reload)

    def test_discovery_inventories_files_before_duplicate_filtering(self):
        manager = MANAGER_PATH.read_text(encoding="utf-8")
        for name in ("DiscoverOverwritesInDir", "DiscoverLocationOverwritesForTarget",
                     "DiscoverWeatherOverwritesForSpid"):
            body = extract_function(manager, name)
            self.assertLess(body.index("overwriteBackingFiles["), body.index("AddOverwriteEntryIfUnique"))
        self.assertIn("overwriteBackingFiles.clear()", extract_function(manager, "ReloadOverwriteEntries"))
        self.assertIn("DeleteEntryLayer", extract_function(manager, "DeleteAllOverwrites"))

    def test_native_export_modes_and_scoped_deletion(self):
        manager = MANAGER_PATH.read_text(encoding="utf-8")
        header = (ROOT / "src/SceneSettingsManager.h").read_text(encoding="utf-8")
        filesystem = (ROOT / "src/Utils/FileSystem.cpp").read_text(encoding="utf-8")
        containment = braced(filesystem, "bool IsPathLexicallyWithinDirectory(")
        declarations = "\n".join([
            braced(header, "enum class TimeOfDayPeriod") + ";",
            braced(header, "enum class EntrySource") + ";",
            braced(header, "enum class SceneContextType") + ";",
            braced(header, "enum class LocationTargetType") + ";",
            braced(header, "enum class SceneType") + ";",
            braced(header, "struct SettingEntry\n") + ";",
            braced(header, "struct PeriodicSceneConfig") + ";",
            braced(header, "struct SceneContextId\n") + ";",
        ])
        helpers = "\n".join(braced(manager, declaration) for declaration in (
            "bool IsSceneSettingPrimitive(",
            "bool ReadBoundedSceneJson(",
            "bool IsSceneMetadataKey(",
            "json* GetObjectAtPath(json& data, const std::vector<std::string>& path, bool create)",
            "const json* GetObjectAtPath(const json& data, const std::vector<std::string>& path)",
            "bool RemoveObjectValueAtPath(",
            "void CollectOverwriteEntries(",
            "static bool WriteGroupedOverwriteFile(",
            "static bool ParseOverwriteFileEntries(",
            "static bool HasOverwriteEntryForPeriod(",
            "static bool AddOverwriteEntryIfUnique(",
        ))
        deletion = extract_function(manager, "DeleteEntryLayer")
        inventory = braced(deletion, "for (const auto& [context, files] : overwriteBackingFiles)")
        delete_files = braced(deletion, "for (const auto& path : backingFiles)")
        # The first brace is the initializer list rather than the loop body.
        cleanup_start = deletion.index('for (const auto* sectionName : { "worldspaces"')
        cleanup_body = deletion.index(") {", cleanup_start) + 2
        cleanup = deletion[cleanup_start:GENERATOR.find_matching_brace(deletion, cleanup_body) + 1]
        source = r'''
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>
using json = nlohmann::json;
namespace RE { using FormID = unsigned int; }
namespace logger {
template<class... Args> void error(Args&&...) {}
template<class... Args> void warn(Args&&...) {}
}
namespace Util::PathHelpers {
std::filesystem::path root;
std::filesystem::path GetSceneSettingsPath() { return root; }
CONTAINMENT
}
struct Feature {
    static Feature* FindFeatureByShortName(const std::string& name) {
        static Feature instance;
        return name == "Sample" ? &instance : nullptr;
    }
};
struct SceneSettingsManager {
DECLARATIONS
    static constexpr float kMaxLocationTransitionSeconds = 300;
    static bool IsFeatureAllowedForType(SceneType, const std::string&) { return true; }
    static std::string GetFeatureDisplayName(const std::string& name) { return name; }
};
constexpr int kOverwriteJsonIndent = 2;
constexpr int kMaxSceneOverwriteFileSize = 1024 * 1024;
constexpr const char* kFeatureKey = "_feature";
constexpr const char* kMetadataKey = "_metadata";
constexpr const char* kMetadataDescriptionKey = "description";
constexpr const char* kMetadataEntryTransitionsKey = "entryTransitions";
constexpr const char* kMetadataTimeOfDayEnabledKey = "timeOfDayEnabled";
bool ValidateSceneSettingEntry(const char*, SceneSettingsManager::SceneType, const std::string&,
                              const std::vector<std::string>&, const std::string&, const json& value, bool numeric) {
    return numeric ? value.is_number_float() : value.is_primitive();
}
std::string GetSceneSettingDisplayName(const std::string&, const std::vector<std::string>&, const std::string& key) { return key; }
bool IsSameSetting(const SceneSettingsManager::SettingEntry& entry, const std::string& feature,
                   const std::vector<std::string>& path, const std::string& key) {
    return entry.featureShortName == feature && entry.settingPath == path && entry.settingKey == key;
}
bool WriteJsonAtomically(const std::filesystem::path& path, const json& data, int indent, std::string_view) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path);
    output << data.dump(indent);
    return output.good();
}
HELPERS
using Manager = SceneSettingsManager;
using SceneContextType = Manager::SceneContextType;
using Context = Manager::SceneContextId;
using Period = Manager::TimeOfDayPeriod;
using Source = Manager::EntrySource;
using Kind = Manager::LocationTargetType;
using Inventory = std::map<Context, std::set<std::filesystem::path>>;
std::string NormalizeLocationFormKey(std::string_view key) {
    std::string result(key);
    std::ranges::transform(result, result.begin(), [](unsigned char c) { return std::tolower(c); });
    return result;
}
void DeleteFiles(const Inventory& overwriteBackingFiles, std::optional<SceneContextType> scope, const Context* target = nullptr) {
    if (target) scope = target->type;
    std::set<std::filesystem::path> backingFiles;
INVENTORY
DELETE_FILES
}
void ClearUnresolved(json& unresolvedLocationUserSettings) {
CLEANUP
}
void check(bool success, const char* message) {
    if (!success) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
int main(int argc, char** argv) {
    check(argc == 2, "Fixture directory required");
    const auto root = std::filesystem::path(argv[1]);
    Util::PathHelpers::root = root;
    check(Util::PathHelpers::IsPathLexicallyWithinDirectory(root, root / "InteriorOnly/imported.json"),
          "A discovered virtual path stays valid when its directory maps to another physical root");
    check(Util::PathHelpers::IsPathLexicallyWithinDirectory(root / "", root / "Weather/../InteriorOnly/imported.json"),
          "Normalize in-root traversal and trailing separators");
    const auto outside = root.parent_path() / "outside.json";
    std::ofstream(outside) << "{}";
    for (const auto& rejected : { outside, root / "../outside.json", root.parent_path() / "fixtures-other/file.json" }) {
        check(!Util::PathHelpers::IsPathLexicallyWithinDirectory(root, rejected), "Reject paths outside the logical root");
        DeleteFiles({{ Context{}, { rejected } }}, std::nullopt);
    }
    check(std::filesystem::exists(outside), "Deletion must preserve out-of-root files");
    check(!Util::PathHelpers::IsPathLexicallyWithinDirectory(root, {}) &&
          !Util::PathHelpers::IsPathLexicallyWithinDirectory({}, root), "Reject empty paths");
    Manager::SettingEntry entry;
    entry.featureShortName = "Sample";
    entry.settingKey = "amount";
    entry.value = 2.0;
    entry.period = Period::Day;
    const std::vector<const Manager::SettingEntry*> entries{ &entry };
    const auto timed = root / "Weather/first/Day/A.json";
    check(WriteGroupedOverwriteFile(root, timed, "Sample", "Weather", entries,
                                  {{ kMetadataTimeOfDayEnabledKey, true }}), "Export timed metadata");
    std::vector<Manager::SettingEntry> parsed;
    std::optional<bool> mode;
    check(ParseOverwriteFileEntries(timed, Manager::SceneType::TimeOfDay, true, parsed, &mode), "Read exported file");
    check(mode == true && parsed.size() == 1 && parsed.front().value == 2.0, "Round-trip timed mode and value");
    parsed.front().period = Period::Day;
    Manager::PeriodicSceneConfig config;
    config.entries = parsed;
    config.overwriteTimeOfDayEnabled = mode;
    config.RefreshTimeOfDayMode();
    check(config.timeOfDayEnabled, "Clean install activates imported timed default");
    entry.value = 7.0;
    entry.period = Period::Count;
    const auto normal = root / "Weather/first/A.json";
    check(WriteGroupedOverwriteFile(root, normal, "Sample", "Weather", entries,
                                  {{ kMetadataTimeOfDayEnabledKey, false }}), "Export normal metadata");
    parsed.clear();
    check(ParseOverwriteFileEntries(normal, Manager::SceneType::TimeOfDay, true, parsed, &mode), "Read normal export");
    check(mode == false && parsed.front().value == 7.0, "Round-trip Normal mode and value");
    config.entries.push_back(parsed.front());
    config.overwriteTimeOfDayEnabled = mode;
    config.RefreshTimeOfDayMode();
    check(!config.timeOfDayEnabled && config.entries.front().value == 2.0 && config.entries.back().value == 7.0,
          "Independent values survive Normal mode selection");
    config.userTimeOfDayEnabled = false;
    config.overwriteTimeOfDayEnabled.reset();
    config.overwriteTimeOfDayEnabled = true;
    config.RefreshTimeOfDayMode();
    check(!config.timeOfDayEnabled, "Explicit local Normal wins after overwrite reload");
    config.userTimeOfDayEnabled = true;
    config.overwriteTimeOfDayEnabled = false;
    config.RefreshTimeOfDayMode();
    check(config.timeOfDayEnabled, "Explicit local TOD wins over imported Normal");
    config.userTimeOfDayEnabled.reset();
    config.overwriteTimeOfDayEnabled.reset();
    config.RefreshTimeOfDayMode();
    check(!config.timeOfDayEnabled, "Metadata-free mixed sets default Normal");
    config.entries.pop_back();
    config.RefreshTimeOfDayMode();
    check(config.timeOfDayEnabled, "Metadata-free timed-only set activates TOD");
    config.entries.clear();
    config.RefreshTimeOfDayMode();
    check(!config.timeOfDayEnabled, "Empty scene defaults Normal");
    const auto duplicate = root / "Weather/first/Day/B.json";
    std::filesystem::copy_file(timed, duplicate);
    const Context first{ .type = SceneContextType::Weather, .weatherId = 1 };
    Inventory inventory;
    std::vector<Manager::SettingEntry> visible;
    for (const auto& path : { timed, duplicate }) {
        parsed.clear();
        check(ParseOverwriteFileEntries(path, Manager::SceneType::TimeOfDay, true, parsed), "Read duplicate fixture");
        inventory[first].insert(path);
        for (auto& value : parsed) {
            value.period = Period::Day;
            AddOverwriteEntryIfUnique(visible, std::move(value), "weather");
        }
    }
    check(visible.size() == 1 && inventory[first].size() == 2, "Keep both backing files while exposing one setting");
    const auto second = root / "Weather/second/Day/A.json";
    const auto world = root / "Locations/world/A.json";
    const auto cell = root / "Locations/cell/A.json";
    const auto interior = root / "InteriorOnly/A.json";
    const auto globalTime = root / "TimeOfDay/Day/A.json";
    for (const auto& path : { second, world, cell, interior, globalTime }) {
        std::filesystem::create_directories(path.parent_path());
        std::filesystem::copy_file(timed, path);
    }
    inventory[first].insert(normal);
    inventory[{ .type = SceneContextType::Weather, .weatherId = 2 }].insert(second);
    inventory[{ .type = SceneContextType::Location, .locationType = Kind::Worldspace, .locationFormKey = "WORLD" }].insert(world);
    inventory[{ .type = SceneContextType::Location, .locationType = Kind::Cell, .locationFormKey = "CELL" }].insert(cell);
    inventory[{ .type = SceneContextType::Interior }].insert(interior);
    inventory[{ .type = SceneContextType::TimeOfDay, .period = Period::Day }].insert(globalTime);
    DeleteFiles(inventory, std::nullopt, &first);
    check(!std::filesystem::exists(timed) && !std::filesystem::exists(duplicate) && !std::filesystem::exists(normal),
          "Target deletion removes duplicates and both sets");
    check(std::filesystem::exists(second) && std::filesystem::exists(world), "Target deletion preserves other scenes");
    const Context worldTarget{ .type = SceneContextType::Location, .locationType = Kind::Worldspace, .locationFormKey = "world" };
    DeleteFiles(inventory, std::nullopt, &worldTarget);
    check(!std::filesystem::exists(world) && std::filesystem::exists(cell), "Location target comparison retains type and normalizes key");
    DeleteFiles(inventory, SceneContextType::Weather);
    check(!std::filesystem::exists(second) && std::filesystem::exists(cell) && std::filesystem::exists(interior),
          "Weather scope preserves locations and interior");
    DeleteFiles(inventory, std::nullopt);
    check(!std::filesystem::exists(cell) && !std::filesystem::exists(interior) && !std::filesystem::exists(globalTime),
          "Global deletion covers remaining kinds");
    json unresolved;
    for (const auto* section : { "worldspaces", "regions", "locationTypes", "categories", "locations", "cells" })
        unresolved[section]["missing"] = {{ "entries", json::array({1}) }, { "name", "preserved" }};
    unresolved["unknown"]["entries"] = json::array({9});
    ClearUnresolved(unresolved);
    for (const auto* section : { "worldspaces", "regions", "locationTypes", "categories", "locations", "cells" })
        check(!unresolved[section]["missing"].contains("entries") && unresolved[section]["missing"]["name"] == "preserved",
              "Delete unresolved entries without dropping metadata");
    check(unresolved["unknown"]["entries"] == json::array({9}), "Preserve unknown sections");
}
'''
        for token, replacement in (("DECLARATIONS", declarations), ("HELPERS", helpers), ("CONTAINMENT", containment),
                                   ("INVENTORY", inventory), ("DELETE_FILES", delete_files), ("CLEANUP", cleanup)):
            source = source.replace(token, replacement)
        includes = sorted((ROOT / "build/ALL/vcpkg_installed").glob("*/include/nlohmann/json.hpp"))
        if not includes:
            self.skipTest("Configured nlohmann-json dependency unavailable")
        with tempfile.TemporaryDirectory(prefix="scene-persistence-test-") as directory:
            directory = Path(directory)
            cpp = directory / "scene_persistence.cpp"
            executable = directory / ("scene_persistence.exe" if os.name == "nt" else "scene_persistence")
            fixtures = directory / "fixtures"
            fixtures.mkdir()
            mod_directory = directory / "mod-scene-settings"
            mod_directory.mkdir()
            virtual_directory = fixtures / "InteriorOnly"
            if os.name == "nt":
                linked = subprocess.run(["cmd", "/c", "mklink", "/J", str(virtual_directory), str(mod_directory)],
                                        capture_output=True, text=True)
                self.assertEqual(linked.returncode, 0, linked.stdout + linked.stderr)
            else:
                virtual_directory.symlink_to(mod_directory, target_is_directory=True)
            cpp.write_text(source, encoding="utf-8")
            include = includes[0].parents[1]
            compiler = shutil.which("clang++") or shutil.which("g++")
            if compiler:
                command = [compiler, "-std=c++20", "-I", str(include), str(cpp), "-o", str(executable)]
            elif os.name == "nt":
                vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
                if not vswhere.exists():
                    self.skipTest("Native C++ compiler unavailable")
                installation = subprocess.check_output([
                    str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                    "-property", "installationPath"], text=True).strip()
                if not installation:
                    self.skipTest("MSVC C++ compiler unavailable")
                vcvars = Path(installation) / "VC/Auxiliary/Build/vcvars64.bat"
                command = f'call "{vcvars}" >nul && cl /nologo /EHsc /std:c++20 /I"{include}" "{cpp}" /Fe:"{executable}"'
            else:
                self.skipTest("Native C++ compiler unavailable")
            compiled = subprocess.run(command, shell=isinstance(command, str), cwd=directory,
                                      capture_output=True, text=True, timeout=90)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            tested = subprocess.run([str(executable), str(fixtures)], capture_output=True, text=True, timeout=10)
            self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)


if __name__ == "__main__":
    unittest.main()
