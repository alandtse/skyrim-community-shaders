import os
import unittest

from test_scene_settings_policy import ROOT
import test_scene_settings_runtime as runtime
from test_scene_settings_runtime import braced


@unittest.skipUnless(os.name == "nt", "Uses Windows atomic replacement and file-sharing semantics")
class FeatureOverwritePersistenceTests(unittest.TestCase):
    def test_native_companion_save_cleanup_and_selected_export(self):
        self.compile_persistence_test()

    def compile_persistence_test(self, scenario=None):
        manager = (ROOT / "src/SettingsOverrideManager.cpp").read_text(encoding="utf-8")
        writer = (ROOT / "src/Utils/JsonFile.cpp").read_text(encoding="utf-8")
        filesystem = (ROOT / "src/Utils/FileSystem.cpp").read_text(encoding="utf-8")
        persistence = (ROOT / "src/SettingsOverridePersistence.cpp").read_text(encoding="utf-8")
        keys = (ROOT / "src/Utils/SettingsKeys.cpp").read_text(encoding="utf-8")
        catalogue_header = (ROOT / "src/Utils/SettingsCatalog.h").read_text(encoding="utf-8")
        catalogue = (ROOT / "src/Utils/SettingsCatalog.cpp").read_text(encoding="utf-8")
        scene = (ROOT / "src/SceneSettingsManager.cpp").read_text(encoding="utf-8")
        state = (ROOT / "src/State.cpp").read_text(encoding="utf-8")
        load = braced(state, "void State::LoadFromJson(")
        boot_preferences = load[load.index("disabledFeatures.clear();"):load.index("favoriteFeatures.clear();")]
        self.assertIn("SceneLayerGuard", load)
        self.assertIn("RestoreBaselinesInSerializedSettings(settings)", braced(state, "void State::SaveToJson("))
        self.assertIn("PrepareUserSettings(settings)", braced(state, "void State::Save("))
        self.assertIn("SaveUserEdits(effectiveSettings, saveConfig)", braced(state, "void State::Save("))
        self.assertLess(state.index("CleanupStaleUserOverrides()"), state.index("if (overridesDiscovered > 0)"))
        methods = "\n".join(braced(manager, declaration) for declaration in (
            "std::string ComputeContentHash(",
            "size_t SettingsOverrideManager::DiscoverOverrides(",
            "size_t SettingsOverrideManager::ApplyOverrides(",
            "size_t SettingsOverrideManager::ApplyGlobalOverrides(",
            "void SettingsOverrideManager::RefreshOverrides(",
            "std::unique_ptr<SettingsOverrideManager::OverrideInfo> SettingsOverrideManager::LoadOverrideFile(",
            "std::pair<std::string, std::string> SettingsOverrideManager::ParseOverrideFilename(",
            "bool SettingsOverrideManager::ValidateOverrideFormat(",
            "bool SettingsOverrideManager::ValidateJsonDataTypes(",
            "json SettingsOverrideManager::SanitizeJsonData(",
            "void SettingsOverrideManager::MergeJson(",
            "bool SettingsOverrideManager::LoadUserOverride(",
            "bool SettingsOverrideManager::HasUserOverride(",
            "json SettingsOverrideManager::GetMergedOverrideSettings("))
        source = r'''
#define NOMINMAX
#include <Windows.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <nlohmann/json.hpp>
#include "MANAGER_HEADER"
#include "PATCH_HEADER"
#include "GENERATED_HEADER"
CATALOGUE_HEADER
#define T(key, fallback) fallback
namespace logger {
template<class... T> void error(T&&...) {}
template<class... T> void info(T&&...) {}
template<class... T> void warn(T&&...) {}
}
namespace SKSE::stl {
template<class F> struct scope_exit {
    F f;
    scope_exit(F action) : f(action) {}
    ~scope_exit() { f(); }
};
}
struct Feature {
    bool loaded = true;
    bool alwaysEnabled = false, disabledByDefault = false;
    bool IsAlwaysEnabled() const { return alwaysEnabled; }
    bool IsDisabledByDefault() const { return disabledByDefault; }
    bool UsesMainSettings() const { return true; }
    std::string GetName() { return "Fixture Feature"; }
    std::string GetShortName() { return "Fixture"; }
    static std::vector<Feature*> GetFeatureList() { static Feature f; return {&f}; }
    static Feature* FindFeatureByShortName(const std::string& name) {
        auto* f = GetFeatureList()[0];
        return f->loaded && name == f->GetShortName() ? f : nullptr;
    }
};
struct SceneSettingsManager {
    struct SettingAddress {
        std::string featureShortName;
        std::vector<std::string> settingPath;
        std::string settingKey;
        auto operator<=>(const SettingAddress&) const = default;
    };
    std::map<SettingAddress, json> baselineSettings;
    void RestoreBaselinesInSerializedSettings(json& settings) const;
} sceneManager;
struct State {
    json live;
    std::unordered_map<std::string, bool> disabledFeatures;
    void SaveToJson(json& values) {
        values = live;
        values["Disable at Boot"] = disabledFeatures;
        sceneManager.RestoreBaselinesInSerializedSettings(values);
    }
    void LoadFromJson(json& settings) {
        live = settings;
        BOOT_PREFERENCES
    }
};
namespace globals { State instance; State* state = &instance; }
namespace Util { std::string PrettifyIdentifier(const std::string& value) { return value; }
bool IEquals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
        [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
} }
std::filesystem::path fixtureRoot;
std::filesystem::path SettingsOverrideManager::GetOverridesDirectory() const { return fixtureRoot; }
std::filesystem::path SettingsOverrideManager::GetUserOverridesDirectory() const { return fixtureRoot / "User"; }
void SettingsOverrideManager::ReportOverrideFailure(const std::string&, const std::string&, const std::string&) {}
namespace Util::FileHelpers {
WRITER
RESOLVE
SANITIZE
}
namespace Util::PathHelpers {
WITHIN
}
KEYS
namespace SceneSettingsCatalog {
std::span<const SettingMetadata> GetSettings() {
    static const std::array<SettingMetadata, 2> entries = [] {
        std::array<SettingMetadata, 2> entries{};
        entries[0].featureShortName = "Fixture";
        entries[0].serializedKey = "Strength";
        entries[0].serializedComponent = -1;
        entries[0].settingKey = "Strength";
        entries[0].displayName = "Catalogue Strength";
        entries[0].flags = SettingFlag::Persisted;
        entries[1] = entries[0];
        entries[1].serializedKey = "Vector";
        entries[1].settingKey = "Vector";
        entries[1].serializedComponent = 0;
        entries[1].displayName = "Whole Vector";
        return entries;
    }();
    return entries;
}
}
CATALOGUE
using Util::Settings::SplitCatalogPath;
const SceneSettingsCatalog::SettingMetadata* FindAllowedCatalogSetting(const std::string& feature,
    const std::vector<std::string>&, const std::string& key) {
    for (const auto& setting : SceneSettingsCatalog::GetSettings())
        if (setting.featureShortName == feature && setting.settingKey == key) return &setting;
    return nullptr;
}
SCENE_HELPERS
METHODS
PERSISTENCE
void check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
json read(const std::filesystem::path& path) { json value; std::ifstream input(path); input >> value; return value; }
void write(const std::filesystem::path& path, const json& value) {
    check(Util::FileHelpers::WriteJsonAtomically(path, value, 2, "fixture"), "Fixture write succeeds");
}
int main() {
    fixtureRoot = std::filesystem::temp_directory_path() / std::format("feature-overwrite-test-{}", GetCurrentProcessId());
    check(!std::filesystem::exists(fixtureRoot), "Fresh temporary directory");
    std::filesystem::create_directory(fixtureRoot);
    const SKSE::stl::scope_exit cleanup([] { std::filesystem::remove_all(fixtureRoot); });
    auto* manager = SettingsOverrideManager::GetSingleton();
    const auto global = fixtureRoot / "A_Global.json";
    const auto first = fixtureRoot / "B_Fixture.json";
    const auto second = fixtureRoot / "C_Fixture.json";
    const auto companion = fixtureRoot / "User/Fixture.user.json";
    const auto globalCompanion = fixtureRoot / "User/Global.user.json";
    const auto mainConfig = fixtureRoot / "Config/SettingsUser.json";
    const json base{{"Fixture Feature", {{"Strength", 1.0}, {"Toggle", true}, {"Vector", {1.0, 2.0}}, {"Untouched", 9.0}}},
        {"General", {{"Value", 0.0}}}, {"Disable at Boot", json::object()}};
    write(global, {{"Fixture Feature", {{"Strength", 2.0}}}, {"General", {{"Value", 1.0}}}});
    write(first, {{"Strength", 3.0}, {"Toggle", false}, {"Vector", {3.0, 4.0}}, {"_metadata", {{"description", "Preserve me"}}}});
    write(second, {{"Strength", 4.0}, {"_metadata", {{"description", "Preserve me too"}}}});
    const auto globalBefore = read(global), firstBefore = read(first), secondBefore = read(second);
    write(companion, {{"Strength", 8.0}});
    write(globalCompanion, {{"General", {{"Value", 6.0}}}});
    auto beforeDiscovery = base;
    check(!manager->LoadUserOverride("Fixture", beforeDiscovery["Fixture Feature"]) &&
          !manager->LoadUserOverride("Global", beforeDiscovery), "Companions cannot load before discovery");
    check(beforeDiscovery == base, "Undiscovered companions leave normal settings unchanged");
    manager->CaptureBaseSettings(base);
    check(manager->DiscoverOverrides() == 3, "Discover fixture files");
    check(manager->LoadUserOverride("Fixture", beforeDiscovery["Fixture Feature"]) &&
          manager->LoadUserOverride("Global", beforeDiscovery), "Discovered companions load normally");
    check(beforeDiscovery["Fixture Feature"]["Strength"] == 8.0 && beforeDiscovery["General"]["Value"] == 6.0,
          "Discovered companions apply only installed overwrite-owned keys");
    std::filesystem::remove(companion);
    std::filesystem::remove(globalCompanion);
    const auto load = [&] {
        auto values = std::filesystem::exists(mainConfig) ? read(mainConfig) : base;
        manager->CaptureBaseSettings(values);
        manager->DiscoverOverrides();
        check(manager->CleanupStaleUserOverrides(), "Cleanup on startup succeeds");
        manager->ApplyGlobalOverrides(values);
        manager->LoadUserOverride("Global", values);
        manager->ApplyOverrides("Fixture", values["Fixture Feature"]);
        manager->LoadUserOverride("Fixture", values["Fixture Feature"]);
        globals::state->LoadFromJson(values);
        manager->CaptureAppliedSettings(values);
    };
    auto& live = globals::state->live;
    const auto save = [&](bool fail = false) {
        json current;
        globals::state->SaveToJson(current);
        auto normal = current;
        manager->PrepareUserSettings(normal);
        return manager->SaveUserEdits(current, [&] {
            if (fail) return false;
            write(mainConfig, normal);
            return true;
        });
    };
    load();
    const auto applied = live;
    check(save(), "Untouched save succeeds");
    check(read(mainConfig) == base, "Untouched overwrite values never leak into normal settings");
    check(!std::filesystem::exists(companion) && !std::filesystem::exists(globalCompanion), "Untouched save creates no companion");
    live["Fixture Feature"]["Strength"] = 7.0;
    live["Fixture Feature"]["Toggle"] = true;
    live["Fixture Feature"]["Vector"] = {5.0, 6.0};
    live["Fixture Feature"]["Untouched"] = 11.0;
    live["General"]["Value"] = 2.0;
    check(!save(true), "Failed config write aborts save");
    check(!std::filesystem::exists(companion) && !std::filesystem::exists(globalCompanion), "Rollback removes newly created companions");
    check(read(mainConfig) == base, "Failed save retains original normal settings");
    check(save(), "Save customized values");
    check(read(companion) == json{{"Strength", 7.0}, {"Toggle", true}, {"Vector", {5.0, 6.0}}}, "Only edited feature settings enter the companion");
    check(read(globalCompanion) == json{{"General", {{"Value", 2.0}}}}, "Shadowed global settings do not become phantom user edits");
    check(read(mainConfig) == live, "Intentional edits also become ordinary user settings");
    check(read(global) == globalBefore && read(first) == firstBefore && read(second) == secondBefore, "Normal save leaves every installed file unchanged");
    load();
    check(live["Fixture Feature"]["Strength"] == 7.0, "Companion edits survive reload");
    check(save() && read(mainConfig)["Fixture Feature"]["Strength"] == 7.0, "Repeated save retains previous intentional edits");

    live["Fixture Feature"]["Strength"] = applied["Fixture Feature"]["Strength"];
    live["Fixture Feature"]["Toggle"] = false;
    live["Fixture Feature"]["Vector"] = applied["Fixture Feature"]["Vector"];
    check(save(), "Returning edits to installed values saves");
    check(!std::filesystem::exists(companion), "Companion disappears when no differences remain");
    check(read(mainConfig)["Fixture Feature"]["Strength"] == applied["Fixture Feature"]["Strength"], "Explicit return to installed value is retained in normal settings");

    live["Fixture Feature"]["Strength"] = 0.0;
    check(save() && read(companion)["Strength"] == 0.0, "Restoring defaults remains a user customization");
    const auto companionBefore = read(companion);
    const auto lock = CreateFileW(companion.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    check(lock != INVALID_HANDLE_VALUE, "Lock companion");
    live["Fixture Feature"]["Strength"] = 8.0;
    check(!save(), "Locked companion prevents partial main save");
    CloseHandle(lock);
    check(read(companion) == companionBefore && read(mainConfig)["Fixture Feature"]["Strength"] == 0.0, "Failed save preserves prior companion and main config");
    check(save(), "Retry saves pending edits");
    const auto beforeFailure = read(companion);
    live["Fixture Feature"]["Strength"] = 9.0;
    check(!save(true) && read(companion) == beforeFailure, "Failed main save restores an existing companion");
    live["Fixture Feature"]["Strength"] = 8.0;

    const auto installedLock = CreateFileW(first.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    check(installedLock != INVALID_HANDLE_VALUE && save(), "Read-only installed overwrite does not prevent saving companion edits");
    check(!manager->DeleteFile(first.string()), "Locked installed file cannot be deleted");
    CloseHandle(installedLock);
    check(read(companion) == beforeFailure && manager->GetOverrides().size() == 3, "Failed deletion rolls back companions and inventory");

    check(manager->DeleteFile(first.string()), "Delete one source");
    check(read(companion) == json{{"Strength", 8.0}}, "Shared companion keeps settings still owned by another source");
    check(manager->DeleteFile(second.string()) && !std::filesystem::exists(companion), "Deleting last feature source removes its companion");
    check(manager->DeleteFile(global.string()) && !std::filesystem::exists(globalCompanion), "Deleting last global source removes its companion");
    check(live["Fixture Feature"]["Strength"] == 8.0 && live["General"]["Value"] == 2.0, "Saved personal settings remain after all installed overwrites are removed");

    write(companion, {{"Strength", 12.0}});
    write(globalCompanion, {{"General", {{"Value", 13.0}}}});
    load();
    check(manager->GetOverrides().empty() && !std::filesystem::exists(companion) && !std::filesystem::exists(globalCompanion), "Startup cleans companions even with zero installed files");
    check(live["Fixture Feature"]["Strength"] == 8.0, "Orphan companions cannot replace normal user settings");

    write(first, {{"Strength", 100.0}, {"Toggle", false}});
    load();
    live["Fixture Feature"]["Strength"] = 10.0;
    manager->CaptureAppliedSettings(live);
    check(save() && !std::filesystem::exists(companion), "Generic loader normalization is not a user edit");
    check(read(mainConfig)["Fixture Feature"]["Strength"] == 8.0, "Normalized applied values do not leak into normal config");
    live["Fixture Feature"]["Strength"] = 6.0;
    check(save() && read(companion)["Strength"] == 6.0, "Actual edits after normalization save normally");

    sceneManager.baselineSettings[{"Fixture", {}, "Strength"}] = 6.0;
    sceneManager.baselineSettings[{"Fixture", {}, "Vector"}] = 5.0;
    live["Fixture Feature"]["Strength"] = 99.0;
    live["Fixture Feature"]["Vector"] = {99.0, 6.0};
    live["Fixture Feature"]["Untouched"] = 15.0;
    check(save(), "Saving under an active scene succeeds");
    check(read(mainConfig)["Fixture Feature"]["Strength"] == 6.0 && read(companion)["Strength"] == 6.0, "Scene scalar cannot leak into either user file");
    check(read(mainConfig)["Fixture Feature"]["Vector"] == json{5.0, 6.0}, "Scene array component cannot leak into normal settings");
    check(read(mainConfig)["Fixture Feature"]["Untouched"] == 15.0 && live["Fixture Feature"]["Strength"] == 99.0, "Unowned edits save without disturbing the live scene");

    sceneManager.baselineSettings.clear();
    load();
    auto exportSource = live;
    exportSource["Fixture Feature"]["Nested"] = {{"Keep", 8.0}, {"Omit", false}};
    exportSource["Fixture Feature"]["a/b"] = {{"~key", true}};
    exportSource["Fixture Feature"]["0"] = {{"1", 5}};
    const auto options = Util::Settings::GetExportSettings("Fixture", exportSource["Fixture Feature"]);
    const auto strength = std::ranges::find(options, std::string("/Strength"), &Util::Settings::ExportSetting::path);
    check(strength != options.end() && strength->label == "Catalogue Strength", "Export uses shared generic catalogue");
    check(std::ranges::find(options, std::string("/Vector/0"), &Util::Settings::ExportSetting::path) == options.end(), "Arrays remain whole");
    const std::vector<std::string> selected{"/Strength", "/Vector", "/Nested/Keep", "/a~1b/~0key", "/0/1"};
    check(manager->ExportSettings("Exported", "Fixture", selected, exportSource), "Selected export succeeds");
    const auto exported = fixtureRoot / "Exported_Fixture.json";
    check(read(exported)["Strength"] == 6.0 && !read(exported).contains("Toggle"), "Export contains selected settings only");
    check(read(exported)["a/b"]["~key"] == true && read(exported)["0"].is_object(), "Escaped and numeric object keys are preserved");
    const auto beforeInvalid = read(exported);
    for (const auto& paths : {std::vector<std::string>{}, std::vector<std::string>{"/Missing"},
            std::vector<std::string>{"/Vector/0"}, std::vector<std::string>{"/Strength", "/Nested"}})
        check(!manager->ExportSettings("Exported", "Fixture", paths, exportSource), "Invalid selections are rejected");
    check(read(exported) == beforeInvalid, "Invalid selection makes no writes");
}
'''
        if scenario is not None:
            source = source[:source.index("int main() {")] + scenario
        def without_includes(text):
            return "\n".join(line for line in text.splitlines() if not line.startswith("#include"))

        source = source.replace("MANAGER_HEADER", (ROOT / "src/SettingsOverrideManager.h").as_posix())
        source = source.replace("PATCH_HEADER", (ROOT / "src/Utils/SettingsPatch.h").as_posix())
        source = source.replace("GENERATED_HEADER", (ROOT / "build/ALL/generated/SceneSettingsCatalog.generated.h").as_posix())
        source = source.replace("CATALOGUE_HEADER", without_includes(catalogue_header).replace("#pragma once", ""))
        source = source.replace("CATALOGUE", without_includes(catalogue))
        source = source.replace("WRITER", braced(writer, "bool WriteJsonAtomically("))
        source = source.replace("RESOLVE", braced(writer, "std::filesystem::path ResolveExistingFile("))
        sanitize_start = filesystem.index("std::string SanitizeFileName(")
        sanitize_end = filesystem.index("\n\t\t}", sanitize_start) + len("\n\t\t}")
        source = source.replace("SANITIZE", filesystem[sanitize_start:sanitize_end])
        source = source.replace("WITHIN", braced(filesystem, "bool IsPathLexicallyWithinDirectory("))
        source = source.replace("KEYS", without_includes(keys))
        scene_helpers = "\n".join(braced(scene, declaration) for declaration in (
            "bool IsCompatibleSceneSettingValue(", "bool ParseCatalogArrayIndex("))
        for declaration in ("Json* GetCatalogNodeAtPath(", "Json* GetCatalogSerializedValue("):
            scene_helpers += "\ntemplate<class Json>\n" + braced(scene, declaration)
        scene_helpers += "\n" + braced(scene, "void SceneSettingsManager::RestoreBaselinesInSerializedSettings(")
        source = source.replace("SCENE_HELPERS", scene_helpers).replace("BOOT_PREFERENCES", boot_preferences)
        source = source.replace("METHODS", methods).replace("PERSISTENCE", without_includes(persistence))
        runtime.SceneSettingsRuntimeTests().compile_and_run(
            source, imgui_root=ROOT / "build/ALL/vcpkg_installed/x64-windows-static-md-release")

    def test_native_live_file_changes_preserve_drafts_and_scene_safe_exports(self):
        self.compile_persistence_test(r'''
int main() {
    const auto root = std::filesystem::temp_directory_path() / std::format("feature-overwrite-changes-{}", GetCurrentProcessId());
    check(!std::filesystem::exists(root), "Fresh fixture root");
    std::filesystem::create_directory(root);
    const SKSE::stl::scope_exit cleanup([root] { std::filesystem::remove_all(root); });
    auto* manager = SettingsOverrideManager::GetSingleton();
    const json base{{"Fixture Feature", {{"Strength", 1.0}, {"Toggle", true}, {"Vector", {1.0, 2.0}}, {"Other", 9.0}}},
        {"Disable at Boot", {{"Fixture", false}}}};
    const auto reset = [&](const char* name) {
        fixtureRoot = root / name;
        std::filesystem::create_directory(fixtureRoot);
        manager->CaptureBaseSettings(base);
        manager->CaptureAppliedSettings(base);
        manager->DiscoverOverrides();
        sceneManager.baselineSettings.clear();
        auto values = base;
        globals::state->LoadFromJson(values);
        manager->CaptureAppliedSettings(values);
    };
    const auto reloadLive = [&] {
        auto values = base;
        manager->ApplyGlobalOverrides(values);
        manager->LoadUserOverride("Global", values);
        manager->ApplyOverrides("Fixture", values["Fixture Feature"]);
        manager->LoadUserOverride("Fixture", values["Fixture Feature"]);
        globals::state->LoadFromJson(values);
        manager->CaptureAppliedSettings(values);
    };
    const std::vector<std::string> strength{"/Strength"}, other{"/Other"};
    reset("pending");
    const auto lower = fixtureRoot / "Lower_Global.json";
    const auto upper = fixtureRoot / "Upper_Fixture.json";
    write(lower, {{"Fixture Feature", {{"Strength", 2.0}, {"Toggle", true}, {"Vector", {1.0, 2.0}}}}});
    write(upper, {{"Strength", 4.0}, {"Toggle", true}, {"Vector", {1.0, 2.0}}});
    manager->DiscoverOverrides();
    reloadLive();
    auto& live = globals::state->live;
    live["Fixture Feature"]["Strength"] = 7.0;
    live["Fixture Feature"]["Toggle"] = false;
    live["Fixture Feature"]["Vector"] = {3.0, 4.0};
    live["Fixture Feature"]["Other"] = 11.0;
    const auto pending = live;
    check(manager->DeleteFile(lower.string()), "Delete shadowed lower file");
    check(live == pending, "Deleting a shadowed file retains numeric, boolean, array and unrelated drafts");
    check(read(upper)["Strength"] == 4.0, "Deletion does not save drafts");
    check(manager->DeleteFile(upper.string()) && live == pending, "Deleting last owner retains unsaved edits as ordinary values");
    auto serialized = live;
    manager->RestoreBaselines(serialized);
    check(serialized == pending, "Formerly owned drafts can now save as ordinary user values");

    reset("saved");
    const auto saved = fixtureRoot / "Saved_Fixture.json";
    write(saved, {{"Strength", 4.0}});
    manager->DiscoverOverrides();
    reloadLive();
    live["Fixture Feature"]["Strength"] = 7.0;
    check(manager->SaveUserEdits(live), "Save edited overwrite into companion and user baseline");
    check(read(saved)["Strength"] == 4.0 && manager->HasUserOverride("Fixture"), "Save creates a companion and leaves installed file unchanged");
    check(manager->DeleteFile(saved.string()) && live["Fixture Feature"]["Strength"] == 7.0,
        "Deleting saved overwrite retains edits in normal user settings");

    reset("legacy");
    const auto legacyOwner = fixtureRoot / "Owner_Fixture.json";
    const auto legacyLower = fixtureRoot / "Lower_Fixture.json";
    const auto legacy = fixtureRoot / "User/Fixture.user.json";
    write(legacyOwner, {{"Strength", 4.0}});
    write(legacyLower, {{"Strength", 2.0}});
    write(legacy, {{"Strength", 7.0}, {"Unowned", 8.0}});
    manager->DiscoverOverrides();
    reloadLive();
    check(live["Fixture Feature"]["Strength"] == 7.0, "Legacy customization loads while owned");
    check(manager->DeleteFile(legacyOwner.string()) && live["Fixture Feature"]["Strength"] == 7.0,
        "Legacy customization remains while another corresponding owner exists");
    check(manager->DeleteFile(legacyLower.string()) && live["Fixture Feature"]["Strength"] == 1.0,
        "Orphaned legacy customization stops applying after the last owner is deleted");
    check(!std::filesystem::exists(legacy), "Deletion cleans orphaned companion files");

    reset("external");
    const auto external = fixtureRoot / "External_Fixture.json";
    write(external, {{"Strength", 4.0}});
    manager->DiscoverOverrides();
    reloadLive();
    write(external, {{"Strength", 8.0}});
    check(!manager->SaveUserEdits(live), "External change blocks stale save");
    live["Fixture Feature"]["Other"] = 11.0;
    check(manager->ExportSettings("Other", "Fixture", other, live), "Unrelated pending value can export");
    check(read(fixtureRoot / "Other_Fixture.json")["Other"] == 11.0, "Export includes unsaved ordinary edits");
    check(!manager->SaveUserEdits(live) && read(external)["Strength"] == 8.0,
        "Unrelated export never clears another file's external-change protection");
    check(!manager->ExportSettings("external", "Fixture", strength, live) && read(external)["Strength"] == 8.0,
        "Case-alias export cannot bypass protection for the same file");

    reset("partial");
    const auto partial = fixtureRoot / "Partial_Fixture.json";
    write(partial, {{"Other", 16.0}, {"_metadata", {{"description", "Preserved"}}}});
    live["Fixture Feature"]["Strength"] = 7.0;
    const auto exportSource = live;
    check(manager->ExportSettings("Partial", "Fixture", strength, exportSource), "Export discovers only its own preexisting target");
    check(live["Fixture Feature"]["Other"] == 16.0, "Preserved existing keys are applied, not mistaken for new unsaved edits");
    check(manager->SaveUserEdits(live) && read(partial)["Other"] == 16.0, "Subsequent save preserves unselected file contents");
    check(manager->ExportSettings("partial", "Fixture", strength, live) && manager->GetOverrides().size() == 1,
        "Case-alias export updates the registered file instead of duplicating ownership");

    reset("priority");
    write(fixtureRoot / "Middle_Fixture.json", {{"Strength", 4.0}});
    manager->DiscoverOverrides();
    reloadLive();
    live["Fixture Feature"]["Strength"] = 7.0;
    check(manager->ExportSettings("AFirst", "Fixture", strength, live), "Export a potentially earlier-priority file");
    const auto beforeRediscovery = manager->GetMergedOverrideSettings("Fixture", json::object());
    manager->RefreshOverrides();
    check(manager->GetMergedOverrideSettings("Fixture", json::object()) == beforeRediscovery,
        "Export registration uses the same priority as file discovery");

    reset("scene");
    sceneManager.baselineSettings[{"Fixture", {}, "Strength"}] = 7.0;
    sceneManager.baselineSettings[{"Fixture", {}, "Vector"}] = 3.0;
    live["Fixture Feature"]["Strength"] = 99.0;
    live["Fixture Feature"]["Vector"] = {99.0, 2.0};
    live["Fixture Feature"]["Other"] = 11.0;
    json sceneFree;
    globals::state->SaveToJson(sceneFree);
    check(sceneFree["Fixture Feature"]["Strength"] == 7.0, "Scene serialization restores the unsaved normal-feature baseline");
    const std::vector<std::string> sceneSelection{"/Strength", "/Vector", "/Other"};
    check(manager->ExportSettings("SceneFree", "Fixture", sceneSelection, sceneFree), "Export under a live scene layer");
    const auto exported = read(fixtureRoot / "SceneFree_Fixture.json");
    check(exported["Strength"] == 7.0 && exported["Vector"] == json{3.0, 2.0} && exported["Other"] == 11.0,
        "Export excludes scene scalar/component values and includes pending ordinary values");
    check(live["Fixture Feature"]["Strength"] == 99.0, "Export does not disturb live scene preview");

    reset("boot");
    const auto boot = fixtureRoot / "Boot_Global.json";
    write(boot, {{"Disable at Boot", {{"Fixture", true}}}});
    manager->DiscoverOverrides();
    reloadLive();
    check(globals::state->disabledFeatures.at("Fixture"), "Global overwrite selects disabled-at-boot");
    check(manager->DeleteFile(boot.string()) && !globals::state->disabledFeatures.at("Fixture"),
        "Deleting global overwrite restores selected boot preference");
    globals::state->SaveToJson(serialized);
    check(serialized["Disable at Boot"]["Fixture"] == false, "Next save cannot bake the deleted boot preference into user settings");
    auto* feature = Feature::GetFeatureList()[0];
    feature->disabledByDefault = true;
    auto implicitDefault = base;
    implicitDefault.erase("Disable at Boot");
    globals::state->LoadFromJson(implicitDefault);
    check(globals::state->disabledFeatures.at("Fixture"), "Missing boot preferences use feature defaults instead of stale choices");
    feature->alwaysEnabled = true;
    globals::state->LoadFromJson(implicitDefault);
    check(!globals::state->disabledFeatures.contains("Fixture"), "Always-enabled features cannot be disabled by reload");
}
''')


if __name__ == "__main__":
    unittest.main()
