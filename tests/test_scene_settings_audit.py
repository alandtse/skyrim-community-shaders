import os
import unittest

from test_scene_settings_policy import MANAGER_PATH, ROOT
import test_scene_settings_runtime as runtime
from test_scene_settings_runtime import braced


class SceneSettingsAuditTests(unittest.TestCase):
    @unittest.skipUnless(os.name == "nt", "Checks Windows file-sharing semantics")
    def test_native_overwrite_removal_releases_file_before_mutation(self):
        manager = MANAGER_PATH.read_text(encoding="utf-8")
        source = r'''
#define NOMINMAX
#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>
using json = nlohmann::json;
namespace logger { template<class... T> void error(T&&...) {} }
constexpr size_t kMaxSceneOverwriteFileSize = 1024 * 1024;
constexpr int kOverwriteJsonIndent = 2;
constexpr const char* kMetadataKey = "_metadata";
constexpr const char* kMetadataEntryTransitionsKey = "entryTransitions";
struct SceneSettingsManager {
    struct SettingIdentity {
        std::string featureShortName;
        std::vector<std::string> settingPath;
        std::string settingKey;
    };
};
HELPERS
void check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
int main() {
    const auto path = std::filesystem::temp_directory_path() /
        std::format("scene-overwrite-{}.json", GetCurrentProcessId());
    const json original = {
        {"_feature", "Fixture"}, {"Keep", 0.3},
        {"Nested", {{"First", 0.1}, {"Second", 0.2}}},
        {"_metadata", {{"entryTransitions", {{"Nested", {{"First", 1.0}, {"Second", 2.0}}}}}}}
    };
    check(WriteJsonAtomically(path, original, 2, "fixture"), "Fixture created");
    const SceneSettingsManager::SettingIdentity first{"Fixture", {"Nested"}, "First"};
    const bool partial = RemoveSettingsFromOverwriteFile(path, std::span{&first, 1});
    json remaining;
    const bool readable = ReadBoundedSceneJson(path, remaining);
    if (!partial || !readable) {
        std::filesystem::remove(path);
        check(false, "Partial removal must release its input handle before atomic replacement");
    }
    check(remaining["Keep"] == 0.3 && remaining["Nested"].size() == 1 &&
        remaining["Nested"]["Second"] == 0.2, "Unselected values survive partial removal");
    check(remaining["_metadata"]["entryTransitions"]["Nested"].size() == 1 &&
        remaining["_metadata"]["entryTransitions"]["Nested"]["Second"] == 2.0,
        "Only selected transition metadata is removed");
    const std::vector<SceneSettingsManager::SettingIdentity> rest{
        {"Fixture", {"Nested"}, "Second"}, {"Fixture", {}, "Keep"}};
    const auto lock = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(lock != INVALID_HANDLE_VALUE, "External read lock acquired");
    const bool lockedRemoval = RemoveSettingsFromOverwriteFile(path, rest);
    CloseHandle(lock);
    json afterFailure;
    const bool preserved = ReadBoundedSceneJson(path, afterFailure) && afterFailure == remaining;
    const bool deleted = RemoveSettingsFromOverwriteFile(path, rest);
    const bool absent = !std::filesystem::exists(path);
    std::filesystem::remove(path);
    check(!lockedRemoval && preserved, "Failed deletion preserves the file for retry");
    check(deleted && absent, "Removing the last settings deletes the file after releasing its input handle");
}
'''
        writer = (ROOT / "src/Utils/JsonFile.cpp").read_text(encoding="utf-8")
        helpers = braced(writer, "bool WriteJsonAtomically(") + "\n" + "\n".join(braced(manager, declaration) for declaration in (
            "bool IsSceneMetadataKey(",
            "bool ReadBoundedSceneJson(", "bool HasSceneOverwriteContent(",
            "bool RemoveObjectValueAtPath(", "static bool RemoveSettingsFromOverwriteFile("))
        runtime.SceneSettingsRuntimeTests().compile_and_run(
            source.replace("HELPERS", helpers),
            imgui_root=ROOT / "build/ALL/vcpkg_installed/x64-windows-static-md-release")

    def test_scene_switch_never_stores_a_draft(self):
        ui = (ROOT / "src/CSEditor/SceneSettingsUI.cpp").read_text(encoding="utf-8")
        manager = MANAGER_PATH.read_text(encoding="utf-8")
        self.assertNotIn("StoreFeatureSceneEdit", braced(ui, "static bool FinishActiveFeatureSceneEdit("))
        self.assertNotIn("StoreFeatureSceneEdit", braced(manager, "bool SceneSettingsManager::BeginFeatureSceneEdit("))
        toolbar = braced(ui, "bool DrawFeaturePageControls(")
        guard = toolbar.index("state.activeContext != GetFeatureSceneContext(state.edit)")
        self.assertLess(guard, toolbar.index("manager->SetWeatherShowTimeOfDay("))
        self.assertLess(guard, toolbar.index("manager->SetLocationShowTimeOfDay("))
        self.assertIn("state.replaceScene.Request();", toolbar)
        self.assertIn("state.edit = *state.pendingTarget;", toolbar)
        self.assertIn("state.saveFailed = !manager->StoreFeatureSceneEdit();", toolbar)

    def test_native_failed_save_keeps_draft_and_saved_entries(self):
        manager = MANAGER_PATH.read_text(encoding="utf-8")
        store = braced(manager, "bool SceneSettingsManager::StoreFeatureSceneEdit(")
        tail = store[store.index("if (!SaveAllUserSettings())"):store.rfind("}")]
        self.assertIn("auto previousEntries = *destinationEntries;", store)
        source = r'''
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
using json = nlohmann::json;
struct Feature {};
struct Manager {
    struct Edit {
        std::string featureShortName = "Fixture";
        std::map<int, int> workingOverrides{{0, 9}};
        bool dirty = true;
        json originalSettings, workingSettings;
    };
    std::optional<Edit> featureSceneEdit{std::in_place};
    std::map<std::string, json> featureApplyDocuments;
    std::map<std::string, int> pendingApplyVerifications;
    std::vector<int> entries{3};
    bool writeSucceeds = false;
    int reapplies = 0;
    bool SaveAllUserSettings() { return writeSucceeds; }
    void ReapplyIfActive() { ++reapplies; }
    bool SnapshotFeatureSceneEdit(Feature&, json& value) { value = entries; return true; }
    bool Store() {
        auto previousEntries = entries;
        auto* destinationEntries = &entries;
        entries = {9};
        Feature instance;
        Feature* feature = &instance;
        STORE_TAIL
    }
};
void check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
int main() {
    Manager manager;
    check(!manager.Store(), "A failed write must return failure");
    check(manager.entries == std::vector<int>{3}, "Failed save restores the prior saved set");
    check(manager.featureSceneEdit->dirty && !manager.featureSceneEdit->workingOverrides.empty(), "Failed save retains unsaved preview");
    check(manager.reapplies == 0, "Failed save must not rebase away the draft");
    manager.writeSucceeds = true;
    check(manager.Store(), "Retry can succeed");
    check(manager.entries == std::vector<int>{9}, "Successful retry stores the edited value");
    check(!manager.featureSceneEdit->dirty && manager.featureSceneEdit->workingOverrides.empty(), "Only success clears the unsaved marker");
    check(manager.reapplies == 1, "Successful save reapplies once");
}
'''.replace("STORE_TAIL", tail)
        runtime.SceneSettingsRuntimeTests().compile_and_run(source, imgui_root=ROOT / "build/ALL/vcpkg_installed/x64-windows-static-md-release")

    def test_native_bulk_delete_batches_writes_and_keeps_failed_files(self):
        manager = MANAGER_PATH.read_text(encoding="utf-8")
        source = r'''
#include <algorithm>
#include <compare>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>
struct SceneSettingsManager {
    enum class SceneContextType { Weather, Location, Interior };
    enum class EntrySource { User, Overwrite };
    struct SceneContextId { SceneContextType type; int weatherId = 1, locationType = 0; std::string locationFormKey = "Fixture"; };
    struct SettingIdentity {
        std::string featureShortName;
        std::vector<std::string> settingPath;
        std::string settingKey;
        auto operator<=>(const SettingIdentity&) const = default;
    };
    struct Entry : SettingIdentity { EntrySource source; std::filesystem::path path; };
    std::vector<Entry> entries;
    int saves = 0, reloads = 0, reapplies = 0, sceneValueRevision = 0, revisions = 0;
    bool IsValidSceneContext(const SceneContextId&) { return true; }
    bool TryEnsureWeatherDataLoaded() { return true; }
    bool TryEnsureLocationDataLoaded() { return true; }
    auto* GetCopyContextEntriesMut(const SceneContextId&) { return &entries; }
    void PrepareWeatherUserSettingsMutation(int, bool) {}
    void PrepareLocationUserSettingsMutation(int, const std::string&, bool) {}
    void BumpEntryPresentationRevision() { ++revisions; }
    void SaveAllUserSettings() { ++saves; }
    void ReapplyIfActive() { ++reapplies; }
    void ReloadOverwriteEntries();
    void RemoveSceneSettings(const SceneContextId&, std::span<const size_t>);
};
std::map<std::filesystem::path, std::vector<SceneSettingsManager::SettingIdentity>> writes;
auto GetWeatherOverwritePath(int, const SceneSettingsManager::Entry& entry) { return entry.path; }
auto GetLocationOverwritePath(int, const std::string&, const SceneSettingsManager::Entry& entry) { return entry.path; }
bool RemoveSettingsFromOverwriteFile(const std::filesystem::path& path, std::span<const SceneSettingsManager::SettingIdentity> pending) {
    writes[path] = {pending.begin(), pending.end()};
    return path != "failed.json";
}
void SceneSettingsManager::ReloadOverwriteEntries() {
    ++reloads;
    std::erase_if(entries, [](const Entry& entry) {
        return entry.source == EntrySource::Overwrite && entry.path != "failed.json" && writes.contains(entry.path);
    });
    ReapplyIfActive();
}
REMOVE
void check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
int main() {
    using Manager = SceneSettingsManager;
    for (auto type : {Manager::SceneContextType::Weather, Manager::SceneContextType::Location}) {
        Manager manager;
        writes.clear();
        for (int index = 0; index < 100; ++index) {
            manager.entries.push_back({{"Fixture", {}, std::to_string(index)}, Manager::EntrySource::User, {}});
            manager.entries.push_back({{"Fixture", {}, std::to_string(index)}, Manager::EntrySource::Overwrite, "one.json"});
        }
        manager.entries.push_back({{"Fixture", {}, "retained"}, Manager::EntrySource::Overwrite, "failed.json"});
        std::vector<size_t> indices;
        for (size_t index = 0; index < manager.entries.size(); ++index) {
            indices.push_back(index);
            indices.push_back(index);
        }
        indices.push_back(9999);
        manager.RemoveSceneSettings({type}, indices);
        check(manager.entries.size() == 1 && manager.entries.front().settingKey == "retained", "Failed file deletion retains its entries");
        check(manager.saves == 1 && manager.reloads == 1 && manager.reapplies == 1, "Batch saves and reloads only once");
        check(writes.size() == 2 && writes["one.json"].size() == 100, "Each backing file receives one deduplicated batch");
    }
}
'''.replace("REMOVE", braced(manager, "void SceneSettingsManager::RemoveSceneSettings("))
        runtime.SceneSettingsRuntimeTests().compile_and_run(source)

    def test_native_location_defaults_use_target_context(self):
        manager = MANAGER_PATH.read_text(encoding="utf-8")
        lower = braced(manager, "std::optional<SceneSettingsManager::ResolvedSettingMap> SceneSettingsManager::BuildLocationLowerLayers(")
        self.assertNotIn("Util::IsInterior()", lower)
        self.assertIn("GetLocationChainInteriorState(locationTargets)", lower)
        source = r'''
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <vector>
namespace RE {
struct TESObjectCELL { bool interior; bool IsInteriorCell() const { return interior; } };
struct Form { TESObjectCELL* cell; template<class T> T* As() { return cell; } };
}
struct SceneSettingsManager {
    enum class LocationTargetType { Worldspace, Cell, LocationType, Location };
    struct LocationTarget { LocationTargetType type; std::string formKey; };
};
RE::TESObjectCELL exterior{false}, interior{true};
RE::Form exteriorForm{&exterior}, interiorForm{&interior};
RE::Form* ResolveLocationTargetForm(const std::string& key) {
    return key == "outside" ? &exteriorForm : key == "inside" ? &interiorForm : nullptr;
}
CLASSIFY
void check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
int main() {
    using Type = SceneSettingsManager::LocationTargetType;
    using Target = SceneSettingsManager::LocationTarget;
    check(GetLocationChainInteriorState(std::vector<Target>{{Type::Worldspace, "Fixture"}}) == false, "Worldspace is exterior without relying on player location");
    check(GetLocationChainInteriorState(std::vector<Target>{{Type::Cell, "outside"}}) == false, "Exterior cell is exterior");
    check(GetLocationChainInteriorState(std::vector<Target>{{Type::Cell, "inside"}}) == true, "Interior cell is interior");
    check(!GetLocationChainInteriorState(std::vector<Target>{{Type::LocationType, "Fixture"}}).has_value(), "General type has no invented interior/exterior context");
    check(!GetLocationChainInteriorState(std::vector<Target>{{Type::Cell, "missing"}}).has_value(), "Missing cell stays unknown");
}
'''.replace("CLASSIFY", braced(manager, "std::optional<bool> GetLocationChainInteriorState("))
        runtime.SceneSettingsRuntimeTests().compile_and_run(source)


if __name__ == "__main__":
    unittest.main()
