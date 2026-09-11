import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_scene_settings_policy import GENERATOR, MANAGER_PATH, ROOT


def braced(source, declaration):
    start = source.index(declaration)
    opening = source.index("{", start)
    closing = GENERATOR.find_matching_brace(source, opening)
    if closing < 0:
        raise AssertionError(f"Unbalanced declaration: {declaration}")
    return source[start:closing + 1]


class SceneSettingsRuntimeTests(unittest.TestCase):
    def test_native_toolbar_loading_and_overwrite_locks(self):
        manager = MANAGER_PATH.read_text(encoding="utf-8")
        source = r'''
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>
namespace globals {
struct State { bool isMainMenuOpen = false, isLoadingMenuOpen = false; };
State* state = nullptr;
namespace game {
struct Player { bool cell = true; void* GetParentCell() { return cell ? this : nullptr; } };
Player* player = nullptr;
}
}
std::string NormalizeLocationFormKey(const std::string& key) { return key; }
struct SceneSettingsManager {
    enum class SceneContextType { Interior, Location };
    struct Context { SceneContextType type = SceneContextType::Interior; int locationType = 0; std::string locationFormKey; };
    struct Edit { Context context; };
    struct Target { int type; std::string formKey; };
    std::optional<Edit> featureSceneEdit{std::in_place};
    std::vector<Target> targets;
    bool overwrites = false, paused = false;
    bool IsSceneReady() const;
    bool IsFeatureSceneEditPreviewActive() const;
    bool HasFeatureSceneEditOverwrites() const { return IsFeatureSceneEditPreviewActive() && overwrites; }
    bool AreFeatureSceneEditOverwritesPaused() const { return paused; }
    bool AreFeatureSceneEditActionsLocked() const;
    const auto& GetCurrentLocationTargets() const { return targets; }
};
READY
PREVIEW
LOCKED
void check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
int main() {
    globals::State state;
    globals::game::Player player;
    SceneSettingsManager manager;
    for (int unavailable = 0; unavailable < 6; ++unavailable)
        for (bool overwritten : {false, true})
            for (bool paused : {false, true}) {
                globals::state = unavailable == 1 ? nullptr : &state;
                globals::game::player = unavailable == 2 ? nullptr : &player;
                player.cell = unavailable != 3;
                state.isMainMenuOpen = unavailable == 4;
                state.isLoadingMenuOpen = unavailable == 5;
                manager.overwrites = overwritten;
                manager.paused = paused;
                check(manager.IsSceneReady() == (unavailable == 0), "Only a loaded game scene enables the toolbar");
                check(manager.IsFeatureSceneEditPreviewActive() == (unavailable == 0), "Loading cannot capture baseline values into a retained draft");
                check(manager.AreFeatureSceneEditActionsLocked() == (unavailable != 0 || (overwritten && !paused)), "Toolbar actions require a ready scene and paused overwrites");
                check(manager.featureSceneEdit.has_value(), "Loading retains the draft");
            }
    state.isLoadingMenuOpen = false;
    manager.featureSceneEdit->context = {SceneSettingsManager::SceneContextType::Location, 1, "Fixture"};
    check(!manager.IsFeatureSceneEditPreviewActive(), "Location preview stays inactive outside its target");
    manager.targets.push_back({1, "Fixture"});
    check(manager.IsFeatureSceneEditPreviewActive(), "Location preview resumes inside its target");
}
'''
        for token, declaration in {
            "READY": "bool SceneSettingsManager::IsSceneReady(",
            "PREVIEW": "bool SceneSettingsManager::IsFeatureSceneEditPreviewActive(",
            "LOCKED": "bool SceneSettingsManager::AreFeatureSceneEditActionsLocked(",
        }.items():
            source = source.replace(token, braced(manager, declaration))
        self.compile_and_run(source)

    def test_native_scene_control_outline(self):
        library_root = ROOT / "build/ALL/vcpkg_installed/x64-windows-static-md-release"
        if os.name != "nt" or not (library_root / "lib/imgui.lib").exists():
            self.skipTest("Uses the Windows build's ImGui library")
        hooks = (ROOT / "src/SceneSettingsUIHooks.cpp").read_text(encoding="utf-8")
        theme = json.loads((ROOT / "package/SKSE/Plugins/CommunityShaders/Themes/Default.json").read_text(encoding="utf-8"))["Theme"]
        source = r'''
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>
namespace SceneSettingsCatalog { struct SettingMetadata {}; }
namespace Util { float scale = 1; float GetUIScale() { return scale; } }
using Setting = SceneSettingsCatalog::SettingMetadata;
bool g_featureSceneEditing = true;
unsigned int g_controlDetourDepth = 0;
std::unordered_set<const Setting*> g_cachedAlteredFeatureSceneEditSettings;
const Setting* matched = nullptr;
bool blocked = false;
const Setting* FindControlSetting(const char*, const void*) { return matched; }
bool ShouldBlockSetting(const Setting&) { return blocked; }
void ClearControlledItem() {}
void FinishControlledItem() {}
bool TrackFeatureSettingMutation(bool changed) { return changed; }
SHOULD_OUTLINE
OUTLINE_GUARD;
DETOUR_SCOPE;
DRAW_CONTROL
void check(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
int main() {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1000, 800);
    unsigned char* pixels; int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    auto& style = ImGui::GetStyle();
    const ImVec4 themeColors[] = {THEME_COLORS};
    std::copy_n(themeColors, std::min(std::size(themeColors), std::size(style.Colors)), style.Colors);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(FRAME_COLOR);
    style.Colors[ImGuiCol_SliderGrab] = style.Colors[ImGuiCol_FrameBg];
    style.Colors[ImGuiCol_SliderGrabActive] = style.Colors[ImGuiCol_FrameBg];
    check(ImGui::GetColorU32(ImGuiCol_HeaderActive) != ImGui::GetColorU32(ImGuiCol_FrameBg), "Theme highlight contrasts with the control background");
    style.FrameBorderSize = 0;
    Setting setting;
    matched = &setting;
    float value = 5;
    for (float scale : {1.0f, 1.5f, 2.0f})
        for (bool altered : {false, true})
            for (bool unavailable : {false, true}) {
                Util::scale = scale;
                blocked = unavailable;
                g_cachedAlteredFeatureSceneEditSettings.clear();
                if (altered) g_cachedAlteredFeatureSceneEditSettings.insert(&setting);
                ImGui::NewFrame();
                ImGui::SetNextWindowSize(ImVec2(600, 400));
                ImGui::Begin("Fixture", nullptr, ImGuiWindowFlags_NoSavedSettings);
                ImGui::SetNextItemWidth(300);
                const ImVec4 previousBorder = style.Colors[ImGuiCol_Border];
                auto* list = ImGui::GetWindowDrawList();
                const int firstVertex = list->VtxBuffer.Size;
                DrawControl("Control", &value, [&] {
                    check(style.FrameBorderSize == (altered ? scale : 0), "Outline thickness follows DPI without changing widget layout");
                    if (altered) check(ImGui::GetColorU32(ImGuiCol_Border) == ImGui::GetColorU32(ImGuiCol_HeaderActive), "Outline uses the theme selection highlight, not the frame background");
                    return ImGui::SliderFloat("Control", &value, 0, 10);
                });
                check(style.FrameBorderSize == 0 && style.Colors[ImGuiCol_Border].x == previousBorder.x, "Outline restores shared style");
                check(((GImGui->LastItemData.ItemFlags & ImGuiItemFlags_Disabled) != 0) == unavailable, "Outline does not change editing availability");
                if (altered && !unavailable) {
                    int accentVertices = 0;
                    for (int index = firstVertex; index < list->VtxBuffer.Size; ++index)
                        accentVertices += list->VtxBuffer[index].col == ImGui::GetColorU32(ImGuiCol_HeaderActive);
                    check(accentVertices > 0, "Altered control draws a visible theme-accent border");
                }
                check(value == 5, "Outline never modifies a setting");
                ImGui::End();
                ImGui::Render();
            }
    g_featureSceneEditing = false;
    check(!ShouldOutlineSetting(setting), "Outline is limited to toolbar editing");
    ImGui::DestroyContext();
}
'''
        source = source.replace("THEME_COLORS", ", ".join(
            "ImVec4(" + ", ".join(f"{float(value)}f" for value in color) + ")"
            for color in theme["FullPalette"]))
        source = source.replace("FRAME_COLOR", ", ".join(f"{float(value)}f" for value in theme["Palette"]["FrameBorder"]))
        source = source.replace("SHOULD_OUTLINE", braced(hooks, "bool ShouldOutlineSetting("))
        source = source.replace("OUTLINE_GUARD;", braced(hooks, "struct SettingOutlineGuard") + ";")
        source = source.replace("DETOUR_SCOPE;", braced(hooks, "struct ControlDetourScope") + ";")
        source = source.replace("DRAW_CONTROL", "template<class Draw>\n" + braced(hooks, "bool DrawControl("))
        self.compile_and_run(source, imgui_root=library_root)

    def test_native_readonly_value_editor_frames(self):
        library_root = ROOT / "build/ALL/vcpkg_installed/x64-windows-static-md-release"
        if os.name != "nt" or not (library_root / "lib/imgui.lib").exists():
            self.skipTest("Uses the Windows build's ImGui library")
        ui = (ROOT / "src/CSEditor/SceneSettingsUI.cpp").read_text(encoding="utf-8")
        utility = (ROOT / "src/Utils/UI.cpp").read_text(encoding="utf-8")
        utility_header = (ROOT / "src/Utils/UI.h").read_text(encoding="utf-8")
        source = r'''
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
using json = nlohmann::json;
const char* T(const char*, const char* text) { return text; }
constexpr float kSceneFloatDragSpeed = 0.01f, kSceneIntDragSpeed = 1.0f;
struct SettingEntry { json value; bool choice = false; };
struct SceneSettingsManager {
    enum class SettingType { Boolean, Float, Integer, String };
    static size_t GetSettingChoiceCount(const SettingEntry& entry) { return entry.choice ? 1 : 0; }
    static bool IsBooleanControlSetting(const SettingEntry& entry) { return entry.value.is_boolean(); }
    static bool IsInvertedDisplaySetting(const SettingEntry&) { return false; }
    static bool IsNumericInputClamped(const SettingEntry&) { return true; }
    static double GetNumericDisplayScale(const SettingEntry&) { return 1; }
    static bool GetNumericDisplayValue(const SettingEntry&, double value, double& result) { result = value; return true; }
    static bool GetNumericStoredValue(const SettingEntry&, double value, double& result) { result = value; return true; }
    static bool GetNumericBounds(const SettingEntry&, double& minimum, double& maximum) { minimum = 0; maximum = 10; return true; }
    static bool GetSettingChoice(const SettingEntry&, size_t, std::int64_t& value, std::string& label) { value = 1; label = "One"; return true; }
    static SettingType DetectSettingType(const json& value) {
        return value.is_boolean() ? SettingType::Boolean : value.is_number_float() ? SettingType::Float :
               value.is_number_integer() ? SettingType::Integer : SettingType::String;
    }
};
namespace Util {
GUARD;
GUARD_BEGIN
GUARD_END
bool BeginSearchableCombo(const char* id, const char* preview, ImGuiComboFlags flags, const void*) { return ImGui::BeginCombo(id, preview, flags); }
bool SearchableComboMatches(const std::string&) { return true; }
void EndSearchableCombo() { ImGui::EndCombo(); }
}
EDITOR
void check(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
int main() {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(800, 600);
    unsigned char* pixels; int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    ImGui::GetStyle().Colors[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.34f, 0.56f, 1.0f);
    const SettingEntry entries[] = {{2.5}, {2}, {true}, {false}, {"text"}, {1, true}};
    for (bool outerDisabled : {false, true})
        for (bool sliders : {false, true})
            for (const auto& entry : entries) {
                ImGui::NewFrame();
                ImGui::SetNextWindowPos(ImVec2(10, 10));
                ImGui::SetNextWindowSize(ImVec2(600, 400));
                ImGui::Begin("Fixture", nullptr, ImGuiWindowFlags_NoSavedSettings);
                ImGui::BeginDisabled(outerDisabled);
                const float originalAlpha = ImGui::GetStyle().Alpha;
                ImGui::BeginDisabled();
                const ImU32 expected = ImGui::GetColorU32(ImGuiCol_FrameBg);
                ImGui::EndDisabled();
                auto* list = ImGui::GetWindowDrawList();
                const int firstVertex = list->VtxBuffer.Size;
                int updates = 0;
                DrawValueEditorCore(entry, 300, sliders, true, [&](const json&) { ++updates; }, [&] { ++updates; });
                check((GImGui->LastItemData.ItemFlags & ImGuiItemFlags_Disabled) != 0, "Overwrite control stays disabled");
                check(updates == 0 && ImGui::GetStyle().Alpha == originalAlpha, "Readonly editor preserves values and style");
                float left = 1e6f, right = -1e6f;
                for (int index = firstVertex; index < list->VtxBuffer.Size; ++index)
                    if (list->VtxBuffer[index].col == expected) {
                        left = std::min(left, list->VtxBuffer[index].pos.x);
                        right = std::max(right, list->VtxBuffer[index].pos.x);
                    }
                check(right > left, "Readonly control retains visible frame background");
                if (!entry.value.is_boolean()) check(right - left > 250, "Numeric, choice, and text fields retain full-width frames");
                ImGui::EndDisabled();
                ImGui::End();
                ImGui::Render();
            }
    ImGui::DestroyContext();
}
'''
        source = source.replace("GUARD;", braced(utility_header, "class DisableGuard") + ";")
        source = source.replace("GUARD_BEGIN", braced(utility, "DisableGuard::DisableGuard("))
        source = source.replace("GUARD_END", braced(utility, "DisableGuard::~DisableGuard("))
        source = source.replace("EDITOR", braced(ui, "static void DrawValueEditorCore("))
        self.compile_and_run(source, imgui_root=library_root)

    def test_native_paused_feature_preview_and_save(self):
        manager = MANAGER_PATH.read_text(encoding="utf-8")
        header = (ROOT / "src/SceneSettingsManager.h").read_text(encoding="utf-8")
        source = r'''
#include <algorithm>
#include <array>
#include <compare>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>
namespace RE { using FormID = unsigned int; }
using json = float;
namespace Util { bool interior = false; bool IsInterior() { return interior; } }
std::string NormalizeLocationFormKey(std::string_view key) { return std::string(key); }
std::vector<std::string> SplitCatalogPath(std::string_view path) { return path.empty() ? std::vector<std::string>{} : std::vector{std::string(path)}; }
struct SceneSettingsManager {
SCENE_TYPE;
CONTEXT_TYPE;
PERIOD;
LOCATION_TYPE;
CONTEXT;
SOURCE;
ADDRESS;
static constexpr int kPeriodCount = static_cast<int>(TimeOfDayPeriod::Count);
struct SettingEntry {
    std::string featureShortName;
    std::vector<std::string> settingPath;
    std::string settingKey;
    std::string displayName;
    json value = 0;
    json originalValue = 0;
    bool paused = false;
    EntrySource source = EntrySource::User;
    TimeOfDayPeriod period = TimeOfDayPeriod::Count;
};
PERIODIC_CONFIG;
struct Target { LocationTargetType type; std::string formKey; };
using ResolvedSettingMap = std::map<SettingAddress, json>;
struct Edit {
    std::string featureShortName = "Fixture";
    SceneContextId context;
    std::vector<SettingAddress> editableAddresses;
    ResolvedSettingMap workingOverrides;
    bool overwritesPaused = false;
    std::set<SettingAddress> overwriteAddresses, alteredAddresses;
};
std::optional<Edit> featureSceneEdit{std::in_place};
int featureSceneEditRevision = 0;
ResolvedSettingMap baselineSettings;
std::map<SceneType, std::vector<SettingEntry>> entries;
std::map<RE::FormID, PeriodicSceneConfig> weatherSceneConfigs;
std::map<std::string, PeriodicSceneConfig> locationSceneConfigs;
std::vector<Target> currentTargets;
const auto& GetCurrentLocationTargets() const { return currentTargets; }
const auto& GetEntries(SceneType type) const { return entries.at(type); }
static bool IsValidSceneContext(const SceneContextId&) { return true; }
static std::string GetLocationConfigKey(LocationTargetType type, std::string_view key) {
    return std::to_string(static_cast<int>(type)) + std::string(key);
}
bool IsFeatureSceneEditPreviewActive() const { return featureSceneEdit.has_value(); }
bool CanCaptureFeatureSceneEdit(std::string_view feature) const { return featureSceneEdit && featureSceneEdit->featureShortName == feature; }
bool IsEntryActive(const SettingEntry& entry) const { return !entry.paused; }
static bool AppliedValuesEqual(float actual, float expected) { return actual == expected; }
void EnsureBaselines(const std::vector<SettingAddress>&) {}
static TimeOfDayPeriod GetCurrentPeriod() { return TimeOfDayPeriod::Day; }
static bool IsSettingAllowedForType(SceneType, const std::string&, const std::vector<std::string>&, const std::string& key) { return key != "blocked"; }
const std::vector<SettingEntry>* GetCopyContextEntries(const SceneContextId&) const;
void ApplyFeatureSceneEditPreview(ResolvedSettingMap&);
bool HasFeatureSceneEditOverwrites(const SceneContextId* = nullptr, bool = true, bool = false) const;
bool IsFeatureSceneEditSettingOverwritten(std::string_view, std::string_view, std::string_view) const;
void ResolveExteriorSettings(ResolvedSettingMap& result, const std::array<float, kPeriodCount>&,
                            const ResolvedSettingMap* location, const std::vector<SettingAddress>&,
                            const void*, const void*, bool, bool, std::set<SettingAddress>*, std::set<SettingAddress>*) {
    for (const auto& [address, value] : *location) result[address] = value;
}
};
MEMBERSHIP
CONTEXT_ENTRIES
PREVIEW
PREVIEW_OVERWRITES
OVERWRITE_LOCK
using Manager = SceneSettingsManager;
using Entry = Manager::SettingEntry;
using Period = Manager::TimeOfDayPeriod;
using Source = Manager::EntrySource;
bool IsSameSetting(const Entry& entry, const std::string& feature, const std::vector<std::string>& path, const std::string& key) {
    return entry.featureShortName == feature && entry.settingPath == path && entry.settingKey == key;
}
std::string GetSceneSettingDisplayName(const std::string&, const std::vector<std::string>&, const std::string& key) { return key; }
void save(std::vector<Entry>& entries, Period period, float value) {
    using EntrySource = Source;
    struct PendingEdit { Manager::SettingAddress address; json value; Period period; json originalValue; };
    std::vector<PendingEdit> pending{{{"Fixture", {}, "Value"}, value, period, 1}};
    auto* destinationEntries = &entries;
STORE_VALUES
}
void check(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
int main() {
    for (auto type : {Manager::SceneContextType::Interior, Manager::SceneContextType::TimeOfDay,
                     Manager::SceneContextType::Weather, Manager::SceneContextType::Location}) {
        for (bool periodic : {false, true}) {
            if (type == Manager::SceneContextType::Interior && periodic) continue;
            if (type == Manager::SceneContextType::TimeOfDay && !periodic) continue;
            Manager manager;
            manager.entries[Manager::SceneType::InteriorOnly] = {};
            manager.entries[Manager::SceneType::TimeOfDay] = {};
            auto& edit = *manager.featureSceneEdit;
            edit.context = {.type = type, .period = periodic ? Period::Day : Period::Count,
                            .weatherId = 100, .locationFormKey = "selected"};
            const Manager::SettingAddress address{"Fixture", {}, "Value"};
            edit.editableAddresses = {address};
            manager.baselineSettings[address] = 1;
            auto& location = manager.locationSceneConfigs[Manager::GetLocationConfigKey(edit.context.locationType, "selected")];
            location.timeOfDayEnabled = periodic;
            manager.currentTargets = {{edit.context.locationType, "selected"}};
            auto& weather = manager.weatherSceneConfigs[100];
            weather.timeOfDayEnabled = periodic;
            auto& entries = type == Manager::SceneContextType::Weather ? weather.entries :
                           type == Manager::SceneContextType::Location ? location.entries :
                           manager.entries[type == Manager::SceneContextType::Interior ? Manager::SceneType::InteriorOnly : Manager::SceneType::TimeOfDay];
            Entry entry{.featureShortName = "Fixture", .settingKey = "Value", .value = 5,
                        .paused = true, .period = edit.context.period};
            entries = {entry};
            auto preview = [&]() {
                Manager::ResolvedSettingMap resolved;
                manager.ApplyFeatureSceneEditPreview(resolved);
                return resolved.at(address);
            };
            check(preview() == 1 && entries.front().paused, "Paused User value falls back to default");
            check(edit.alteredAddresses.empty(), "Paused default does not highlight control");
            check(!manager.IsFeatureSceneEditSettingOverwritten("Fixture", "", "Value"), "Paused User value never locks the control");
            entries.front().paused = false;
            entries.front().value = 1;
            check(preview() == 1 && edit.alteredAddresses.contains(address), "Active User scene setting outlines even when equal to the default");
            const int userRevision = manager.featureSceneEditRevision;
            entries.front().source = Source::Overwrite;
            check(preview() == 1 && edit.alteredAddresses.contains(address), "Active overwrite outlines even when equal to the default");
            check(manager.IsFeatureSceneEditSettingOverwritten("Fixture", "", "Value"), "Active overwrite locks default-valued control");
            check(manager.featureSceneEditRevision > userRevision, "Lock ownership change refreshes UI even when value and outline do not change");
            check(!manager.IsFeatureSceneEditSettingOverwritten("Unrelated", "", "Value"), "Overwrite lock stays scoped to edited feature");
            check(!manager.IsFeatureSceneEditSettingOverwritten("Fixture", "", "Other"), "Overwrite lock stays scoped to overwritten setting");
            entries = {entry};
            edit.workingOverrides[address] = 7;
            check(preview() == 7, "Draft replaces paused User preview");
            save(entries, edit.context.period, 7);
            check(entries.size() == 1 && entries.front().paused && entries.front().value == 7, "Saving preserves existing User pause");
            edit.workingOverrides.clear();
            entry.source = Source::Overwrite;
            entries = {entry};
            check(preview() == 1, "Paused overwrite falls back to default");
            edit.workingOverrides[address] = 8;
            check(preview() == 8 && entries.front().paused, "Draft can preview over paused overwrite");
            save(entries, edit.context.period, 8);
            check(entries.size() == 2 && entries[1].source == Source::User && entries[1].paused, "New User edit of paused overwrite remains paused");
            check(entries.front().value == 5 && entries.front().paused, "Saving does not modify overwrite");
            edit.workingOverrides.clear();
            check(preview() == 1, "Saving a paused User value does not apply it");
            entries.front().paused = false;
            edit.workingOverrides[address] = 9;
            check(preview() == 5, "Active overwrite remains authoritative over draft");
            check(edit.overwriteAddresses.contains(address) && edit.alteredAddresses.contains(address), "Active overwrite records masking and changed control");
            check(manager.IsFeatureSceneEditSettingOverwritten("Fixture", "", "Value"), "Active overwrite is read-only");
            check(manager.HasFeatureSceneEditOverwrites(&edit.context), "Selected context reports effective overwrites");
            auto otherContext = edit.context;
            otherContext.period = periodic ? Period::Night : Period::Day;
            check(!manager.HasFeatureSceneEditOverwrites(&otherContext), "Other periods do not inherit selected preview's overwrite marker");
            check(manager.HasFeatureSceneEditOverwrites(&otherContext, false), "Target marker includes the selected overwritten period");
            edit.overwritesPaused = true;
            check(preview() == 9, "Temporary overwrite bypass previews draft");
            check(!manager.IsFeatureSceneEditSettingOverwritten("Fixture", "", "Value"), "Temporary overwrite pause unlocks the control");
            check(!entries.front().paused && entries[1].paused, "Temporary bypass does not change entry pause flags");
            edit.workingOverrides.clear();
            check(preview() == 1 && edit.alteredAddresses.empty(), "Bypassing overwrite with paused User shows default without outline");
            check(edit.overwriteAddresses.contains(address), "Resume button can remain available during bypass");
            entries[1].paused = false;
            check(preview() == 8, "Temporary bypass exposes active User settings");
            entries[1].value = 1;
            check(preview() == 1 && edit.alteredAddresses.contains(address), "Default-valued User scene setting remains outlined while overwrites are bypassed");
            edit.overwritesPaused = false;
            check(preview() == 5, "Resume restores overwrite priority");
            check(manager.IsFeatureSceneEditSettingOverwritten("Fixture", "", "Value"), "Resuming overwrites relocks the control");
            entries = {};
            edit.workingOverrides.clear();
            entry.period = periodic ? Period::Night : Period::Day;
            entries.push_back(entry);
            check(preview() == 1, "Other period's paused overwrite does not leak into preview");
            entries = {};
            save(entries, edit.context.period, 3);
            check(entries.size() == 1 && !entries.front().paused, "Unrelated new setting stays enabled by default");
        }
    }
}
'''
        replacements = {
            "SCENE_TYPE": braced(header, "enum class SceneType"),
            "CONTEXT_TYPE": braced(header, "enum class SceneContextType"),
            "PERIOD": braced(header, "enum class TimeOfDayPeriod"),
            "LOCATION_TYPE": braced(header, "enum class LocationTargetType"),
            "CONTEXT": braced(header, "struct SceneContextId\n"),
            "SOURCE": braced(header, "enum class EntrySource"),
            "ADDRESS": braced(header, "struct SettingAddress\n"),
            "PERIODIC_CONFIG": braced(header, "struct PeriodicSceneConfig"),
            "MEMBERSHIP": braced(manager, "bool EntryBelongsToContext("),
            "CONTEXT_ENTRIES": braced(manager, "const std::vector<SceneSettingsManager::SettingEntry>* SceneSettingsManager::GetCopyContextEntries("),
            "PREVIEW": braced(manager, "void SceneSettingsManager::ApplyFeatureSceneEditPreview("),
            "PREVIEW_OVERWRITES": braced(manager, "bool SceneSettingsManager::HasFeatureSceneEditOverwrites("),
            "OVERWRITE_LOCK": braced(manager, "bool SceneSettingsManager::IsFeatureSceneEditSettingOverwritten("),
            "STORE_VALUES": braced(braced(manager, "bool SceneSettingsManager::StoreFeatureSceneEdit("), "for (auto& edit : pending)"),
        }
        for token, replacement in replacements.items():
            source = source.replace("\n" + token + ";\n", "\n" + replacement + ";\n")
            source = source.replace("\n" + token + "\n", "\n" + replacement + "\n")
        self.compile_and_run(source)

    def test_native_shared_dialog_resize(self):
        if os.name != "nt":
            self.skipTest("Uses the Windows build's ImGui library")
        library_root = ROOT / "build/ALL/vcpkg_installed/x64-windows-static-md-release"
        if not (library_root / "lib/imgui.lib").exists():
            self.skipTest("Build ImGui first")
        utility = (ROOT / "src/Utils/UI.cpp").read_text(encoding="utf-8")
        theme = (ROOT / "src/Menu/ThemeManager.h").read_text(encoding="utf-8")
        source = r'''
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
namespace SKSE::stl {
template<class F> struct scope_exit { F fn; scope_exit(F f) : fn(f) {} ~scope_exit() { fn(); } };
}
namespace ThemeManager::Constants {
CONSTANTS
}
float GetUIScale() { return 1.0f; }
CLOSE_BUTTON
ANIMATION_STATE;
ANIMATION
PREPARE
POPUP_WINDOW
DIALOG
struct NativeTitleBarButtonHighlightGuard {};
void DrawRoundedTitleBarButtonHighlights(ImGuiWindow*, bool, bool) {}
WINDOW
void check(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
int callbackCalls = 0;
void constrain(ImGuiSizeCallbackData* data) {
    check(data->UserData == &callbackCalls, "Caller constraint userdata is preserved");
    ++callbackCalls;
}
ImVec2 draw(float height, float width, bool modal, bool reopen = false, bool close = false, bool nested = false) {
    auto& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1000, 800);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(10, 10));
    ImGui::SetNextWindowSize(ImVec2(900, 700));
    ImGui::Begin("Fixture", nullptr, ImGuiWindowFlags_NoSavedSettings);
    const char* id = modal ? "Modal" : "Popup";
    if (reopen) ImGui::OpenPopup(id);
    ImGui::SetNextWindowPos(ImVec2(500, 400), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, 650), constrain, &callbackCalls);
    ImVec2 size;
    bool open = true;
    if (BeginDialogPopup(id, "Fixture heading", &open, modal, ImGuiWindowFlags_AlwaysAutoResize)) {
        auto* window = ImGui::GetCurrentWindow();
        size = window->Size;
        if (!window->Hidden)
            check(std::abs(window->Pos.x + size.x * 0.5f - 500) <= 1 &&
                  std::abs(window->Pos.y + size.y * 0.5f - 400) <= 1, "Animated dialog stays centered");
        ImGui::Dummy(ImVec2(50, height));
        if (nested) {
            ImGui::OpenPopup("Nested");
            if (BeginDialogPopup("Nested", "Nested", nullptr, false, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Dummy(ImVec2(40, 40));
                ImGui::EndPopup();
            }
        }
        if (close) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::End();
    ImGui::Render();
    return size;
}
ImVec2 drawWindow(float height, bool automatic = true) {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(100, 100));
    ImGui::SetNextWindowSizeConstraints(ImVec2(300, 0), ImVec2(300, 650));
    if (!automatic) ImGui::SetNextWindowSize(ImVec2(300, 200));
    BeginWithRoundedClose("Add settings fixture", nullptr, ImGuiWindowFlags_NoSavedSettings |
        (automatic ? ImGuiWindowFlags_AlwaysAutoResize : ImGuiWindowFlags_None));
    const auto size = ImGui::GetWindowSize();
    ImGui::Dummy(ImVec2(50, height));
    ImGui::End();
    ImGui::Render();
    return size;
}
int main() {
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* pixels; int width, height;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    for (const float dt : {1.0f / 30, 1.0f / 60, 1.0f / 144}) {
        ImGui::GetIO().DeltaTime = dt;
        for (bool modal : {false, true}) {
            draw(70, 300, modal, true);
            ImVec2 small;
            for (int frame = 0; frame < 6; ++frame) small = draw(70, 300, modal);
            check(small.x == 300 && small.y > 70 && small.y < 150, "Initial dialog sizes to content without animating from stale dimensions");
            draw(270, 500, modal);
            const auto growing = draw(270, 500, modal);
            check(growing.y > small.y && growing.y < small.y + 200, "Content growth animates instead of snapping");
            float previous = growing.y;
            ImVec2 large;
            for (int frame = 0; frame < 80; ++frame) {
                large = draw(270, 500, modal);
                check(large.y >= previous, "Growth is monotonic");
                previous = large.y;
            }
            check(large.x == 500 && std::abs(large.y - small.y - 200) <= 1, "Animation converges to auto-fit size and width constraint");
            draw(70, 300, modal);
            const auto shrinking = draw(70, 300, modal);
            check(shrinking.y < large.y && shrinking.y > small.y, "Content shrink animates instead of snapping");
            for (int frame = 0; frame < 80; ++frame) draw(70, 300, modal);
            draw(70, 300, modal, false, false, true);
            draw(70, 300, modal, false, true);
            draw(70, 300, modal, false, true);
            draw(170, 350, modal, true);
            ImVec2 reopened;
            for (int frame = 0; frame < 4; ++frame) reopened = draw(170, 350, modal);
            check(reopened.x == 350 && std::abs(reopened.y - small.y - 100) <= 1, "Reopen does not animate from the last session");
            for (int frame = 0; frame < 80; ++frame) reopened = draw(1000, 350, modal);
            check(reopened.y <= 650, "Maximum dialog height remains respected");
            draw(70, 300, modal, false, true);
        }
    }
    check(callbackCalls != 0, "Existing size callbacks are still invoked");
    ImGui::GetIO().DeltaTime = 1.0f / 60;
    ImVec2 windowSize;
    for (int frame = 0; frame < 6; ++frame) windowSize = drawWindow(70);
    drawWindow(270);
    const auto growing = drawWindow(270);
    check(growing.y > windowSize.y && growing.y < windowSize.y + 200, "Add Settings window shares dialog animation");
    for (int frame = 0; frame < 80; ++frame) windowSize = drawWindow(270);
    check(drawWindow(270, false).y == 200, "Manually sized windows are not animated");
    ImGui::DestroyContext();
}
'''
        constants = "\n".join(line.strip() for line in theme.splitlines()
                              if "constexpr float POPUP_BUTTON_WIDTH =" in line or
                              "constexpr float DIALOG_RESIZE_RESPONSE =" in line)
        for token, replacement in {
                "CONSTANTS": constants,
                "CLOSE_BUTTON": braced(utility, "bool CloseButton("),
                "ANIMATION_STATE": braced(utility, "struct DialogSizeAnimation"),
                "ANIMATION": braced(utility, "static void AnimateDialogSize("),
                "PREPARE": braced(utility, "static void PrepareDialogSizeAnimation("),
                "POPUP_WINDOW": braced(utility, "static ImGuiWindow* GetOpenDialogWindow("),
                "WINDOW": braced(utility, "bool BeginWithRoundedClose("),
                "DIALOG": braced(utility, "static bool BeginDialogPopup(")}.items():
            source = source.replace("\n" + token + "\n", "\n" + replacement + "\n")
            source = source.replace("\n" + token + ";\n", "\n" + replacement + ";\n")
        self.compile_and_run(source, library_root)

    def test_native_feature_toolbar_status(self):
        manager = MANAGER_PATH.read_text(encoding="utf-8")
        header = (ROOT / "src/SceneSettingsManager.h").read_text(encoding="utf-8")
        ui = (ROOT / "src/CSEditor/SceneSettingsUI.cpp").read_text(encoding="utf-8")
        source = r'''
#include <algorithm>
#include <compare>
#include <cmath>
#include <format>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>
namespace RE {
using FormID = unsigned int;
struct Weather { FormID id; FormID GetFormID() const { return id; } };
struct Sky { Weather* currentWeather = nullptr; Weather* lastWeather = nullptr; float currentWeatherPct = 1.0f; };
}
namespace globals::game { RE::Sky* sky = nullptr; }
struct Value {
    bool numeric = true;
    double number = 1.0;
    template<class T> T get() const { return static_cast<T>(number); }
};
bool IsNumericValue(const Value& value) { return value.numeric; }
namespace Util { bool interior = false; bool IsInterior() { return interior; } }
struct SceneSettingsManager {
SCENE_TYPE;
CONTEXT_TYPE;
PERIOD;
LOCATION_TYPE;
CONTEXT;
SUMMARY;
static constexpr int kPeriodCount = static_cast<int>(TimeOfDayPeriod::Count);
struct SettingEntry {
    std::string featureShortName;
    TimeOfDayPeriod period = TimeOfDayPeriod::Count;
    bool paused = false;
    enum class Source { User, Overwrite } source = Source::User;
    std::vector<std::string> settingPath;
    std::string settingKey;
    Value value;
};
using EntrySource = SettingEntry::Source;
PERIODIC_CONFIG;
struct Config : PeriodicSceneConfig { LocationTargetType type = LocationTargetType::Location; std::string formKey; };
std::map<SceneType, std::vector<SettingEntry>> entries;
std::map<RE::FormID, Config> weatherSceneConfigs;
std::map<std::string, Config> locationSceneConfigs;
std::string pausedFeature;
std::set<std::string> appliedFeatureNames;
mutable RE::FormID cachedPreviousWeatherId = 0;
std::vector<Config> currentTargets;
const auto& GetCurrentLocationTargets() const { return currentTargets; }
RE::FormID GetEffectivePreviousWeatherId(const RE::Sky*, float) const;
bool HasActiveSettingsForFeature(const std::string& name) const { return appliedFeatureNames.contains(name); }
static bool IsSettingAllowedForType(SceneType, const std::string&, const std::vector<std::string>&, const std::string& key) { return key != "blocked"; }
std::uint64_t entryPresentationRevision = 0;
std::uint64_t GetEntryPresentationRevision() const { return entryPresentationRevision; }
void changed() { ++entryPresentationRevision; }
static SceneSettingsManager* GetSingleton() { static SceneSettingsManager manager; return &manager; }
static TimeOfDayPeriod GetCurrentPeriod() { return TimeOfDayPeriod::Day; }
static bool IsValidSceneContext(const SceneContextId&) { return true; }
static std::string GetLocationConfigKey(LocationTargetType type, std::string_view key) {
    return std::to_string(static_cast<int>(type)) + std::string(key);
}
const std::vector<SettingEntry>& GetEntries(SceneType type) const { return entries.at(type); }
std::vector<SettingEntry>& GetEntriesMut(SceneType type) { return entries.at(type); }
bool TryEnsureWeatherDataLoaded() { return true; }
bool TryEnsureLocationDataLoaded() { return true; }
int saves = 0, reapplies = 0, mutations = 0;
std::uint64_t sceneValueRevision = 0;
bool locationOverridesDirty = false;
void BumpEntryPresentationRevision() { changed(); }
void MarkEntryListUserSettingsModified(SceneType) { ++mutations; }
void PrepareWeatherUserSettingsMutation(RE::FormID, bool) { ++mutations; }
void PrepareLocationUserSettingsMutation(LocationTargetType, std::string_view, bool) { ++mutations; }
void SaveAllUserSettings() { ++saves; }
void ReapplyIfActive() { ++reapplies; }
std::vector<SettingEntry>* GetCopyContextEntriesMut(const SceneContextId& context);
void SetFeatureSceneSettingsPaused(std::string_view, const SceneContextId&, bool);
bool IsFeaturePaused(const std::string& name) const { return name == pausedFeature; }
bool previewPaused = false;
bool IsFeatureSceneEditing(std::string_view name) const { return previewPaused && name == "FixtureFeature"; }
bool AreFeatureSceneEditOverwritesPaused() const { return previewPaused; }
bool HasFeatureSceneEditOverwrites(const SceneContextId*, bool, bool) const { return false; }
bool IsEntryActive(const SettingEntry& entry) const;
const std::vector<SettingEntry>* GetCopyContextEntries(const SceneContextId& context) const;
const PeriodicSceneConfig* GetPeriodicSceneConfig(const SceneContextId& context) const;
EntryLayerSummary GetFeatureSceneSummary(std::string_view feature, const SceneContextId& context, bool matchPeriod = true) const;
EntryLayerSummary GetFeatureSceneSummary(std::string_view feature, SceneContextType type) const;
bool HasCurrentSceneSettingsForFeature(const std::string& feature) const;
};
MEMBERSHIP
ACTIVE
CONTEXT_ENTRIES
MUTABLE_CONTEXT_ENTRIES
PAUSE_SETTINGS
FEATURE_SUMMARY
PERIODIC_CONFIG_LOOKUP
TYPE_SUMMARY
CURRENT_SETTINGS
PREVIOUS_WEATHER
using Period = SceneSettingsManager::TimeOfDayPeriod;
PICKER_MARKER;
PICKER_PRESENCE
PICKER_LABEL
bool HasFeatureSceneSettings(std::string_view feature, const SceneSettingsManager::SceneContextId& context, bool matchPeriod = true, bool wholeType = false) {
    return GetFeatureSceneMarker(feature, context, matchPeriod, wholeType) != SceneSettingMarker::None;
}
void check(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
int main() {
    using Manager = SceneSettingsManager;
    using Type = Manager::SceneContextType;
    using Entry = Manager::SettingEntry;
    check(GetScenePickerLabel("Example", SceneSettingMarker::Settings) == "Example *", "Saved settings append an asterisk");
    check(GetScenePickerLabel("Example", SceneSettingMarker::None) == "Example", "Unconfigured text is unchanged");
    check(GetScenePickerLabel("Example", SceneSettingMarker::Overwrite) == "Example **", "Active overwrites append two asterisks");
    auto& manager = *Manager::GetSingleton();
    manager.entries[Manager::SceneType::InteriorOnly] = {};
    manager.entries[Manager::SceneType::TimeOfDay] = {};
    for (const auto type : {Type::Interior, Type::TimeOfDay, Type::Weather, Type::Location}) {
        Manager::SceneContextId context{.type = type, .weatherId = 100, .locationFormKey = "fixture"};
        const bool periodic = type == Type::Weather || type == Type::Location;
        context.period = type == Type::TimeOfDay ? Period::Day : Period::Count;
        auto& config = type == Type::Weather ? manager.weatherSceneConfigs[100] :
            manager.locationSceneConfigs[Manager::GetLocationConfigKey(context.locationType, context.locationFormKey)];
        config.type = context.locationType;
        config.formKey = context.locationFormKey;
        auto& entries = periodic ? config.entries : manager.entries[type == Type::Interior ?
            Manager::SceneType::InteriorOnly : Manager::SceneType::TimeOfDay];
        entries = {{"OtherFeature", context.period, true}};
        manager.changed();
        check(!HasFeatureSceneSettings("FixtureFeature", context), "Other feature does not mark this feature's saved settings");
        entries.push_back({"FixtureFeature", context.period});
        manager.changed();
        check(HasFeatureSceneSettings("FixtureFeature", context), "Saved set is marked");
        check(HasFeatureSceneSettings("FixtureFeature", {.type = type}, false, true), "Root scene type includes saved entries in any target");
        entries.back().source = Entry::Source::Overwrite;
        manager.changed();
        check(GetFeatureSceneMarker("FixtureFeature", context) == SceneSettingMarker::Overwrite, "Active overwrite marks double asterisk");
        manager.previewPaused = true;
        check(GetFeatureSceneMarker("FixtureFeature", context) == SceneSettingMarker::Settings, "Temporary bypass removes double asterisk without altering saved flags");
        manager.previewPaused = false;
        entries.back().paused = true;
        manager.changed();
        check(GetFeatureSceneMarker("FixtureFeature", context) == SceneSettingMarker::Settings, "Paused overwrite uses single asterisk");
        check(HasFeatureSceneSettings("FixtureFeature", context), "Paused overwrites remain marked");
        manager.pausedFeature = "FixtureFeature";
        check(HasFeatureSceneSettings("FixtureFeature", context), "Feature pause does not alter presence");
        manager.pausedFeature.clear();
        if (periodic) {
            config.timeOfDayEnabled = true;
            manager.changed();
            check(HasFeatureSceneSettings("FixtureFeature", context), "Saved Normal entries stay marked when TOD is enabled");
            context.period = Period::Day;
            check(!HasFeatureSceneSettings("FixtureFeature", context), "Empty period is not marked");
            check(HasFeatureSceneSettings("FixtureFeature", context, false), "Target includes both saved modes");
            entries.push_back({"FixtureFeature", Period::Day});
            manager.changed();
            check(HasFeatureSceneSettings("FixtureFeature", context), "Saved period is marked");
            config.timeOfDayEnabled = false;
            manager.changed();
            check(HasFeatureSceneSettings("FixtureFeature", context), "Saved TOD entries stay marked when Normal is enabled");
        }
        entries.clear();
        entries = {{"FixtureFeature", context.period, false},
                   {"FixtureFeature", context.period, true, Entry::Source::Overwrite},
                   {"OtherFeature", context.period, false}};
        if (type != Type::Interior)
            entries.push_back({"FixtureFeature", Period::Night, false});
        manager.changed();
        const int savedBefore = manager.saves;
        auto summary = manager.GetFeatureSceneSummary("FixtureFeature", context);
        check(summary.count == 2 && summary.paused == 1, "Mixed state includes both ownership layers");
        manager.SetFeatureSceneSettingsPaused("FixtureFeature", context, true);
        check(manager.GetFeatureSceneSummary("FixtureFeature", context).AllPaused(), "Pause affects both selected layers");
        check(!entries[2].paused, "Pause excludes other features");
        if (type != Type::Interior) check(!entries[3].paused, "Pause excludes other periods");
        check(manager.saves == savedBefore + 1, "Batch user pause persists once");
        manager.SetFeatureSceneSettingsPaused("FixtureFeature", context, true);
        check(manager.saves == savedBefore + 1, "Unchanged pause does not save again");
        manager.pausedFeature = "FixtureFeature";
        manager.SetFeatureSceneSettingsPaused("FixtureFeature", context, false);
        check(manager.GetFeatureSceneSummary("FixtureFeature", context).paused == 0, "Resume clears both selected layer flags");
        check(manager.pausedFeature == "FixtureFeature", "Selected scene resume leaves broader feature pause alone");
        manager.pausedFeature.clear();
        entries = {{"FixtureFeature", context.period, false, Entry::Source::Overwrite}};
        const int overwriteSaves = manager.saves;
        manager.SetFeatureSceneSettingsPaused("FixtureFeature", context, true);
        check(entries.front().paused && manager.saves == overwriteSaves, "Overwrite-only pause stays session-only");
        check(manager.locationOverridesDirty && manager.sceneValueRevision > 0, "Pause invalidates resolver caches");
        entries.clear();
        manager.changed();
        check(!HasFeatureSceneSettings("FixtureFeature", {.type = type}, false, true), "Deleting final entry clears marker");
    }
    RE::Weather current{100}, previous{200}, unrelated{300};
    RE::Sky sky{&current, &previous, 1.0f};
    globals::game::sky = &sky;
    auto visible = [&] { return manager.HasCurrentSceneSettingsForFeature("FixtureFeature"); };
    manager.weatherSceneConfigs[300].entries = {{"FixtureFeature"}};
    check(!visible(), "Saved unrelated weather alone never shows toggle");
    manager.appliedFeatureNames.insert("FixtureFeature");
    check(visible(), "Actually applied settings show toggle");
    manager.appliedFeatureNames.clear();
    manager.pausedFeature = "FixtureFeature";
    check(!visible(), "Paused feature has no resume toggle for unrelated weather");
    auto& weather = manager.weatherSceneConfigs[100];
    weather.entries = {{"FixtureFeature"}};
    weather.timeOfDayEnabled = false;
    check(visible(), "Current Normal weather can resume");
    weather.timeOfDayEnabled = true;
    check(!visible(), "Inactive Normal mode cannot resume");
    weather.entries.push_back({"FixtureFeature", Period::Day});
    check(visible(), "Enabled weather TOD can resume");
    weather.entries.back().paused = true;
    check(!visible(), "Feature resume cannot resume individually paused entries");
    weather.entries.clear();
    manager.weatherSceneConfigs[200].entries = {{"FixtureFeature"}};
    check(!visible(), "Finished previous weather does not show toggle");
    sky.currentWeatherPct = 0.5f;
    check(visible(), "Fading previous weather can resume");
    Util::interior = true;
    check(!visible(), "Weather is inactive in interiors");
    auto& interior = manager.entries[Manager::SceneType::InteriorOnly];
    interior = {{"FixtureFeature"}};
    interior.front().value.numeric = false;
    check(visible(), "Interior non-float scene setting can resume");
    Util::interior = false;
    sky.currentWeatherPct = 1.0f;
    check(!visible(), "Interior settings cannot resume outdoors");
    auto& tod = manager.entries[Manager::SceneType::TimeOfDay];
    tod = {{"FixtureFeature", Period::Day}};
    check(visible(), "Global TOD can resume outdoors");
    tod.front().value.numeric = false;
    check(!visible(), "Invalid non-float TOD does not show toggle");
    tod.front().value.numeric = true;
    tod.front().value.number = std::numeric_limits<double>::infinity();
    check(!visible(), "Non-finite values do not show toggle");
    tod.clear();
    auto& location = manager.locationSceneConfigs[Manager::GetLocationConfigKey(Manager::LocationTargetType::Location, "fixture")];
    location.entries = {{"FixtureFeature"}};
    location.timeOfDayEnabled = false;
    check(!visible(), "Unrelated location does not show toggle");
    manager.currentTargets.push_back(location);
    check(visible(), "Current location can resume");
    location.timeOfDayEnabled = true;
    check(!visible(), "Inactive Normal location set cannot resume");
    location.entries.push_back({"FixtureFeature", Period::Day});
    check(visible(), "Current location TOD can resume");
    location.entries.back().settingKey = "blocked";
    check(!visible(), "Policy-disallowed entries cannot show toggle");
    manager.currentTargets.clear();
    check(!visible(), "Leaving location removes resume toggle");
}

'''
        replacements = {
            "SCENE_TYPE": braced(header, "enum class SceneType"),
            "CONTEXT_TYPE": braced(header, "enum class SceneContextType"),
            "PERIOD": braced(header, "enum class TimeOfDayPeriod"),
            "LOCATION_TYPE": braced(header, "enum class LocationTargetType"),
            "CONTEXT": braced(header, "struct SceneContextId\n"),
            "SUMMARY": braced(header, "struct EntryLayerSummary"),
            "PERIODIC_CONFIG": braced(header, "struct PeriodicSceneConfig"),
            "MEMBERSHIP": braced(manager, "bool EntryBelongsToContext("),
            "ACTIVE": braced(manager, "bool SceneSettingsManager::IsEntryActive("),
            "CONTEXT_ENTRIES": braced(manager, "const std::vector<SceneSettingsManager::SettingEntry>* SceneSettingsManager::GetCopyContextEntries("),
            "MUTABLE_CONTEXT_ENTRIES": braced(manager, "std::vector<SceneSettingsManager::SettingEntry>* SceneSettingsManager::GetCopyContextEntriesMut("),
            "PAUSE_SETTINGS": braced(manager, "void SceneSettingsManager::SetFeatureSceneSettingsPaused("),
            "FEATURE_SUMMARY": braced(manager, "SceneSettingsManager::EntryLayerSummary SceneSettingsManager::GetFeatureSceneSummary("),
            "PERIODIC_CONFIG_LOOKUP": braced(manager, "const SceneSettingsManager::PeriodicSceneConfig* SceneSettingsManager::GetPeriodicSceneConfig("),
            "TYPE_SUMMARY": braced(manager, "SceneSettingsManager::EntryLayerSummary SceneSettingsManager::GetFeatureSceneSummary(\n\tstd::string_view featureShortName, SceneContextType type)"),
            "CURRENT_SETTINGS": braced(manager, "bool SceneSettingsManager::HasCurrentSceneSettingsForFeature("),
            "PREVIOUS_WEATHER": braced(manager, "RE::FormID SceneSettingsManager::GetEffectivePreviousWeatherId("),
            "PICKER_MARKER": braced(ui, "enum class SceneSettingMarker"),
            "PICKER_PRESENCE": braced(ui, "static SceneSettingMarker GetFeatureSceneMarker("),
            "PICKER_LABEL": braced(ui, "static std::string GetScenePickerLabel("),
        }
        for token, replacement in replacements.items():
            source = source.replace("\n" + token + ";\n", "\n" + replacement + ";\n")
            source = source.replace("\n" + token + "\n", "\n" + replacement + "\n")
        self.compile_and_run(source)

    def test_native_feature_draft_switch_confirmation(self):
        ui = (ROOT / "src/CSEditor/SceneSettingsUI.cpp").read_text(encoding="utf-8")
        source = r'''
#include <cstdio>
#include <cstdlib>
#include <format>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
const char* T(const char*, const char* text) { return text; }
struct I18n {
    static I18n* GetSingleton() { static I18n instance; return &instance; }
    std::string Format(const char*, const std::unordered_map<std::string, std::string>& args, const char* text) {
        std::string result = text;
        for (const auto& [key, value] : args) {
            auto placeholder = "{" + key + "}";
            if (auto pos = result.find(placeholder); pos != std::string::npos) result.replace(pos, placeholder.size(), value);
        }
        return result;
    }
};
struct Feature {
    std::string name;
    bool supported = true;
    std::string GetShortName() const { return name; }
    std::string GetDisplayName() const { return name; }
};
namespace Util {
struct ConfirmationPopup {
    std::string title, message, confirmLabel, cancelLabel;
    enum class Answer { None, Confirm, Cancel } answer = Answer::None;
    bool open = false;
    static inline int draws = 0;
    void Request() { open = true; answer = Answer::None; }
    bool IsOpen() const { return open; }
    bool Draw() {
        ++draws;
        if (answer == Answer::None) return false;
        open = false;
        return answer == Answer::Confirm;
    }
};
}
struct SceneSettingsManager {
    std::string owner;
    bool pending = false, overwritesPaused = false;
    int value = 1, stored = 1, begins = 0, ends = 0, saves = 0;
    static SceneSettingsManager* GetSingleton() { static SceneSettingsManager instance; return &instance; }
    static std::string GetFeatureDisplayName(const std::string& feature) { return feature; }
    bool HasPendingFeatureSceneEdits() const { return pending; }
    void EndFeatureSceneEdit(bool save) {
        if (owner.empty()) return;
        ++ends;
        if (save) { ++saves; stored = value; }
        owner.clear(); pending = false; overwritesPaused = false; value = stored;
    }
    bool BeginFeatureSceneEdit(Feature* feature, int) { ++begins; owner = feature->name; return true; }
};
struct FeaturePageEditorState {
    std::string featureShortName;
    bool toolbarOpen = false;
    std::string pendingFeatureShortName;
    Util::ConfirmationPopup replaceEditor;
    std::vector<int> supportedTypes;
    int edit = 0;
    std::optional<int> activeContext;
};
FeaturePageEditorState s_featurePageEditor;
bool CanEditFeaturePage(Feature* feature) { return feature && feature->supported; }
std::vector<int> GetFeatureSceneContextTypes(const std::string&) { return {1}; }
void InitializeFeatureSceneTarget(Feature*, int& edit) { edit = 1; }
std::optional<int> GetFeatureSceneContext(int edit) { return edit; }
void InitializeFeatureCopyDestination(FeaturePageEditorState&) {}
bool environmentPlaying = false;
bool SetFeaturePagePreviewPlaying(bool playing) { environmentPlaying = playing; return true; }
START_EDITOR
REQUEST_EDITOR
DRAW_CONFIRMATION
IS_EDITING
HIDE_EDITOR
void check(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
int main() {
    Feature first{"First"}, second{"Second"}, third{"Third"}, unsupported{"Unsupported", false};
    auto& state = s_featurePageEditor;
    auto* manager = SceneSettingsManager::GetSingleton();
    check(!BeginFeaturePageEditing(nullptr), "Null feature cannot replace the draft");
    check(BeginFeaturePageEditing(&first) && IsFeaturePageEditing(&first), "Open first toolbar");
    environmentPlaying = true;
    manager->pending = true; manager->value = 9; manager->overwritesPaused = true;
    DrawFeaturePageEditConfirmation(&second);
    check(IsFeaturePageEditing(&first) && !IsFeaturePageEditing(&second), "Visiting another feature leaves the toolbar on its owner");
    check(manager->value == 9 && manager->pending && manager->overwritesPaused, "Navigation preserves draft and overwrite bypass");
    const int begins = manager->begins;
    HideFeaturePageEditing();
    check(!IsFeaturePageEditing(&first) && manager->pending && manager->value == 9, "Hiding the toolbar retains the draft preview");
    check(environmentPlaying, "Hiding the toolbar keeps weather/time preview running");
    check(BeginFeaturePageEditing(&first) && manager->begins == begins, "Reopening same feature resumes without restarting its context");
    check(!BeginFeaturePageEditing(&unsupported) && manager->owner == first.name, "Unsupported target leaves current draft intact");
    check(!BeginFeaturePageEditing(&second) && state.replaceEditor.IsOpen(), "Unsaved changes require confirmation before replacing the editor");
    DrawFeaturePageEditConfirmation(&second);
    check(state.replaceEditor.message.find(first.name) != std::string::npos && state.replaceEditor.message.find(second.name) != std::string::npos, "Warning identifies the draft owner and destination");
    check(manager->owner == first.name && manager->value == 9 && manager->ends == 0, "Pending dialog neither discards nor saves");
    state.replaceEditor.answer = Util::ConfirmationPopup::Answer::Cancel;
    DrawFeaturePageEditConfirmation(&second);
    check(state.pendingFeatureShortName.empty() && manager->pending && manager->value == 9, "Cancel retains unsaved changes");
    check(manager->overwritesPaused && manager->saves == 0, "Cancel preserves overwrite bypass without saving");
    check(environmentPlaying, "Canceled replacement preserves weather/time preview");
    BeginFeaturePageEditing(&second);
    state.replaceEditor.answer = Util::ConfirmationPopup::Answer::Confirm;
    DrawFeaturePageEditConfirmation(&second);
    check(IsFeaturePageEditing(&second) && !manager->pending && manager->value == 1, "Confirmation discards old draft and opens target");
    check(!environmentPlaying, "Confirmed replacement releases weather/time preview");
    check(!manager->overwritesPaused && manager->stored == 1 && manager->saves == 0, "Confirmed replacement resumes overwrites without persisting the discarded draft");
    const int draws = Util::ConfirmationPopup::draws;
    check(BeginFeaturePageEditing(&third), "Clean draft switches immediately");
    DrawFeaturePageEditConfirmation(&third);
    check(IsFeaturePageEditing(&third) && !state.replaceEditor.IsOpen() && Util::ConfirmationPopup::draws == draws, "No unsaved changes means no popup");
    manager->pending = true; manager->value = 4;
    BeginFeaturePageEditing(&first);
    manager->pending = false;
    DrawFeaturePageEditConfirmation(&first);
    check(IsFeaturePageEditing(&first) && Util::ConfirmationPopup::draws == draws, "A draft cleared before drawing does not show a stale confirmation");
    check(manager->saves == 0, "Only an explicit Save action may persist a draft");
}
'''
        for token, declaration in {
            "START_EDITOR": "static bool StartFeaturePageEditing(",
            "REQUEST_EDITOR": "bool BeginFeaturePageEditing(",
            "DRAW_CONFIRMATION": "static void DrawFeaturePageEditConfirmation(",
            "IS_EDITING": "bool IsFeaturePageEditing(",
            "HIDE_EDITOR": "void HideFeaturePageEditing(",
        }.items():
            source = source.replace(token, braced(ui, declaration))
        self.compile_and_run(source)

    def test_native_menu_close_retains_feature_draft(self):
        manager = MANAGER_PATH.read_text(encoding="utf-8")
        source = r'''
#include <atomic>
#include <compare>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>
namespace logger { void warn(const char*, const std::string&) {} }
namespace globals {
struct Menu { bool IsEnabled = true; };
struct State { int frameCount = 0; bool isMainMenuOpen = false; bool isLoadingMenuOpen = false; };
Menu* menu = nullptr;
State* state = nullptr;
}
struct SceneSettingsManager {
    struct Edit { std::string featureShortName; bool overwritesPaused = true; int value = 9; };
    struct Address {
        std::string featureShortName;
        std::vector<std::string> path;
        std::string key;
        auto operator<=>(const Address&) const = default;
    };
    std::optional<Edit> featureSceneEdit;
    std::set<std::string> pendingApplyVerifications, featureApplyDocuments, appliedFeatureNames;
    std::map<Address, int> appliedSettings;
    bool resolverDirty = false;
    int lastUpdateFrame = -1, stores = 0;
    int savedValue = 2, liveValue = 2;
    bool overwritePresent = false;
    std::atomic_bool queuedLoadingTransition = false;
    bool StoreFeatureSceneEdit() { ++stores; savedValue = liveValue; return true; }
    void ResolveAndApply(bool = false) {
        liveValue = featureSceneEdit ? featureSceneEdit->value : overwritePresent ? 6 : savedValue;
    }
    void VerifyPendingApplies(bool = false) {}
    void FlushDeferredSceneChanges() {}
    void OnLoadingTransition() { ResolveAndApply(true); }
    void EndFeatureSceneEdit(bool storeChanges);
    void Update();
    void draft() {
        featureSceneEdit = Edit{"FixtureFeature"};
        pendingApplyVerifications.insert("FixtureFeature");
        featureApplyDocuments.insert("FixtureFeature");
        appliedFeatureNames.insert("FixtureFeature");
        appliedSettings[{"FixtureFeature", {}, "Value"}] = 9;
        appliedSettings[{"OtherFeature", {}, "Value"}] = 5;
        liveValue = 9;
    }
};
END_EDIT
UPDATE
void check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
int main() {
    globals::Menu menu;
    globals::State state;
    globals::menu = &menu;
    globals::state = &state;
    for (int closeKind = 0; closeKind < 4; ++closeKind) {
        SceneSettingsManager manager;
        menu.IsEnabled = true;
        state.isMainMenuOpen = state.isLoadingMenuOpen = false;
        manager.draft();
        ++state.frameCount;
        manager.Update();
        check(manager.featureSceneEdit.has_value() && manager.stores == 0, "Open menu retains unsaved draft");
        menu.IsEnabled = closeKind != 0;
        state.isMainMenuOpen = closeKind == 1;
        state.isLoadingMenuOpen = closeKind == 2;
        globals::menu = closeKind == 3 ? nullptr : &menu;
        ++state.frameCount;
        manager.Update();
        check(manager.featureSceneEdit.has_value() && manager.stores == 0, "Menu and loading lifecycle retain draft without storing");
        check(manager.featureSceneEdit->value == 9 && manager.savedValue == 2, "Draft remains separate from saved settings");
        check(manager.featureSceneEdit->overwritesPaused, "Temporary overwrite bypass stays with the retained preview");
        if (closeKind == 0) check(manager.liveValue == 9, "Closing the OS menu keeps the draft preview applied");
        globals::menu = &menu;
        menu.IsEnabled = true;
        state.isMainMenuOpen = state.isLoadingMenuOpen = false;
        ++state.frameCount;
        manager.Update();
        check(manager.stores == 0 && manager.liveValue == 9, "Reopening restores the retained draft, without saving");
    }
    SceneSettingsManager manager;
    manager.draft();
    manager.StoreFeatureSceneEdit();
    manager.featureSceneEdit->value = 12;
    manager.liveValue = 12;
    menu.IsEnabled = false;
    ++state.frameCount;
    manager.Update();
    check(manager.stores == 1 && manager.savedValue == 9 && manager.liveValue == 12, "Closing retains edits after the last explicit save without saving them");
    for (bool store : {false, true}) {
        SceneSettingsManager bypassed;
        bypassed.overwritePresent = true;
        bypassed.draft();
        check(bypassed.featureSceneEdit->overwritesPaused, "Draft starts with temporary bypass for fixture");
        bypassed.EndFeatureSceneEdit(store);
        check(!bypassed.featureSceneEdit && bypassed.liveValue == 6 && bypassed.overwritePresent, "Ending the draft explicitly restores overwrites");
    }
}
'''.replace("END_EDIT", braced(manager, "void SceneSettingsManager::EndFeatureSceneEdit("))
        source = source.replace("UPDATE", braced(manager, "void SceneSettingsManager::Update("))
        self.compile_and_run(source)

    def test_native_direct_location_membership(self):
        manager = MANAGER_PATH.read_text(encoding="utf-8")
        header = (ROOT / "src/SceneSettingsManager.h").read_text(encoding="utf-8")
        builder = braced(manager, "std::vector<SceneSettingsManager::LocationTarget> BuildLocationTargetChain(")
        self.assertNotIn("parentLoc", builder)
        current = braced(manager, "const std::vector<SceneSettingsManager::LocationTarget>& SceneSettingsManager::GetCurrentLocationTargets(")
        self.assertIn("BuildLocationTargetChain(location, cell)", current)
        runtime = braced(manager, "SceneSettingsManager::ResolvedSettingMap& SceneSettingsManager::BuildResolvedSettings(")
        self.assertIn("const auto& locationTargets = GetCurrentLocationTargets();", runtime)
        self.assertIn("cachedLocationUserSettings, locationTargets", runtime)
        self.assertIn("cachedLocationOverwriteSettings, locationTargets", runtime)
        resolving = braced(manager, "std::vector<SceneSettingsManager::LocationTarget> ResolveLocationTargetChain(")
        self.assertIn("BuildLocationTargetChain(form->As<RE::BGSLocation>(), nullptr)", resolving)
        self.assertIn("BuildLocationTargetChain(cell->GetLocation(), cell)", resolving)
        copying = braced(manager, "SceneSettingsManager::CopyResult SceneSettingsManager::CopySettings(")
        self.assertIn("destinationLocationTargets = ResolveLocationTargetChain(", copying)
        preview = braced(manager, "void SceneSettingsManager::ApplyFeatureSceneEditPreview(")
        self.assertIn("GetCurrentLocationTargets()", preview)
        lower = braced(manager, "std::optional<SceneSettingsManager::ResolvedSettingMap> SceneSettingsManager::BuildLocationLowerLayers(")
        self.assertIn("ResolveLocationTargetChain(type, formKey)", lower)
        discovery = braced(manager, "const std::vector<SceneSettingsManager::LocationTarget>& SceneSettingsManager::GetLocationManagementTargets(")
        self.assertIn("GetFormArray<RE::BGSLocation>()", discovery)
        source = r'''
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace RE {
using FormID = unsigned int;
struct Form { FormID id = 0; FormID GetFormID() const { return id; } };
struct TESWorldSpace : Form {};
struct TESRegion : Form {};
struct BGSKeyword : Form { bool isLocationType = true; };
struct TESObjectCELL : Form {
    bool exterior = true;
    TESWorldSpace* worldSpace = nullptr;
    std::vector<TESRegion*> regions;
    bool IsExteriorCell() const { return exterior; }
    TESObjectCELL& GetRuntimeData() { return *this; }
    std::vector<TESRegion*>* GetRegionList(bool) { return &regions; }
};
struct Marker {
    TESObjectCELL* cell = nullptr;
    TESObjectCELL* GetParentCell() const { return cell; }
};
struct MarkerHandle {
    Marker* marker = nullptr;
    explicit operator bool() const { return marker != nullptr; }
    Marker* get() const { return marker; }
};
struct BGSLocation : Form {
    BGSLocation* parentLoc = nullptr;
    std::vector<BGSKeyword*> keywords;
    MarkerHandle worldLocMarker;
    const auto& GetKeywords() const { return keywords; }
};
struct Player : Marker {};
struct Sky { TESRegion* region = nullptr; };
}
namespace globals::game {
RE::Player* player = nullptr;
RE::Sky* sky = nullptr;
}
namespace Util {
template<class Form> std::string GetFormFileKey(const Form* form) { return std::to_string(form->GetFormID()); }
std::string GetFormEditorID(const RE::TESObjectCELL* cell) { return GetFormFileKey(cell); }
std::string GetFormDisplayName(RE::FormID id) { return std::to_string(id); }
}
template<class Form> std::string GetLocationTargetDisplayName(const Form* form) { return Util::GetFormFileKey(form); }
std::string NormalizeLocationFormKey(std::string_view key) { return std::string(key); }
bool IsLocationTypeKeyword(const RE::BGSKeyword* keyword) { return keyword && keyword->isLocationType; }
std::string GetLocationTypeDisplayName(const RE::BGSKeyword* keyword) { return Util::GetFormFileKey(keyword); }
std::vector<std::string> GetLocationTypeLabels(const RE::BGSLocation* location) {
    std::vector<std::string> labels;
    for (auto* keyword : location->GetKeywords())
        if (IsLocationTypeKeyword(keyword)) labels.push_back(GetLocationTypeDisplayName(keyword));
    return labels;
}
struct SceneSettingsManager {
TARGET_TYPE
LOCATION_TARGET_DECLARATION
};
WORLDSPACE_TARGET
BUILD_CHAIN
using Kind = SceneSettingsManager::LocationTargetType;
void check(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
bool contains(const std::vector<SceneSettingsManager::LocationTarget>& targets, Kind kind, RE::FormID id) {
    return std::ranges::any_of(targets, [&](const auto& target) { return target.type == kind && target.formId == id; });
}
int main() {
    RE::TESWorldSpace world{{1000}};
    RE::TESRegion region{{2000}};
    RE::BGSKeyword directType{{10}}, parentOnlyType{{11}}, sharedType{{12}}, otherKeyword{{13}, false};
    RE::BGSLocation parent{{3001}}, direct{{3000}};
    parent.keywords = {&parentOnlyType, &sharedType};
    direct.parentLoc = &parent;
    direct.keywords = {&directType, &sharedType, &directType, &otherKeyword, nullptr};
    RE::TESObjectCELL cell{{4000}, true, &world, {&region}};
    RE::Marker marker{&cell};
    direct.worldLocMarker = {&marker};
    parent.worldLocMarker = {&marker};
    RE::Player player{{&cell}};
    RE::Sky sky{&region};
    globals::game::player = &player;
    globals::game::sky = &sky;
    const auto targets = BuildLocationTargetChain(&direct, &cell);
    check(targets.size() == 6, "Only worldspace, direct types, region, direct location and cell are included");
    check(contains(targets, Kind::Location, direct.id), "Direct location remains current");
    check(!contains(targets, Kind::Location, parent.id), "Parent-only location is not current or matched");
    check(contains(targets, Kind::LocationType, directType.id) && contains(targets, Kind::LocationType, sharedType.id),
          "Direct keywords remain active even when also present on a parent");
    check(!contains(targets, Kind::LocationType, parentOnlyType.id), "Parent-only location type is not inherited");
    check(!contains(targets, Kind::LocationType, otherKeyword.id), "Unrelated keywords remain excluded");
    check(targets[0].type == Kind::Worldspace && targets[1].type == Kind::LocationType &&
          targets[2].type == Kind::LocationType && targets[3].type == Kind::Region &&
          targets[4].type == Kind::Location && targets[5].type == Kind::Cell, "Existing precedence order is unchanged");
    const auto offsite = BuildLocationTargetChain(&direct, nullptr);
    check(offsite.size() == targets.size() && !contains(offsite, Kind::Location, parent.id),
          "Offsite direct-location copy context uses the same membership rules");
    const auto parentContext = BuildLocationTargetChain(&parent, nullptr);
    check(contains(parentContext, Kind::Location, parent.id) && contains(parentContext, Kind::LocationType, parentOnlyType.id),
          "Parent records remain valid standalone management targets when selected directly");
    RE::TESObjectCELL wilderness{{4001}, true, &world, {&region}};
    player.cell = &wilderness;
    const auto outside = BuildLocationTargetChain(nullptr, &wilderness);
    check(outside.size() == 3 && contains(outside, Kind::Worldspace, world.id) &&
          contains(outside, Kind::Region, region.id) && contains(outside, Kind::Cell, wilderness.id),
          "Worldspace and region remain across unnamed wilderness cell boundaries");
    cell.exterior = false;
    const auto interior = BuildLocationTargetChain(&direct, &cell);
    check(!contains(interior, Kind::Worldspace, world.id) && !contains(interior, Kind::Region, region.id) &&
          contains(interior, Kind::Location, direct.id), "Interior direct location is preserved without exterior membership");
}
'''
        source = source.replace("TARGET_TYPE", braced(header, "enum class LocationTargetType") + ";")
        source = source.replace("LOCATION_TARGET_DECLARATION", braced(header, "struct LocationTarget\n") + ";")
        source = source.replace("WORLDSPACE_TARGET", braced(manager, "SceneSettingsManager::LocationTarget MakeWorldspaceTarget("))
        source = source.replace("BUILD_CHAIN", builder)
        self.compile_and_run(source)

    def test_native_edit_capture_and_float_application(self):
        manager = MANAGER_PATH.read_text(encoding="utf-8")
        capture = braced(manager, "bool SceneSettingsManager::CaptureFeatureSceneEditChanges(")
        capture = capture[capture.index("featureSceneEdit->workingSettings = sanitized;"):capture.rfind("}")]
        refresh = braced(manager, "void SceneSettingsManager::RefreshFeatureSceneEditOverrides(")
        refresh = refresh[refresh.index("auto previousOverrides ="):refresh.rfind("}")]
        storing = braced(manager, "bool SceneSettingsManager::StoreFeatureSceneEdit(")
        storing = storing[storing.index("if (IsFeatureSceneEditPreviewActive()"):storing.index("const auto context =")]
        endpoints = braced(manager, "void SceneSettingsManager::RefreshLocationTransitionEndpoints(")
        endpoint_update = braced(endpoints, "if (std::isfinite(numeric)")
        resolving = braced(manager, "void SceneSettingsManager::ResolveAndApply(")
        start = resolving.index("resolved[address] = finished ?")
        resolved_endpoint = resolving[start:resolving.index(";", start) + 1]
        advancing = braced(manager, "bool SceneSettingsManager::AdvanceLocationTransitions(")
        start = advancing.index("batch.updates[index].value =")
        advanced_endpoint = advancing[start:advancing.index(";", start) + 1]
        source = r'''
#include <algorithm>
#include <cmath>
#include <compare>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

struct json {
    std::variant<double, std::int64_t, bool> value;
    json() : value(0.0) {}
    json(float number) : value(static_cast<double>(number)) {}
    json(double number) : value(number) {}
    json(std::int64_t number) : value(number) {}
    json(bool boolean) : value(boolean) {}
    bool is_number_float() const { return std::holds_alternative<double>(value); }
    template <class T> T get() const {
        return std::visit([](auto number) { return static_cast<T>(number); }, value);
    }
    bool operator==(const json&) const = default;
};
struct SceneSettingsManager {
    static bool ResolvedValuesEqual(const json&, const json&);
    static bool AppliedValuesEqual(const json&, const json&);
};
RESOLVED_EQUALITY
APPLIED_EQUALITY

void check(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
struct SettingAddress {
    std::string featureShortName;
    std::vector<std::string> settingPath;
    std::string settingKey;
    auto operator<=>(const SettingAddress&) const = default;
};
using Document = std::map<std::string, json>;
using Values = std::map<SettingAddress, json>;
const std::string* FindAllowedCatalogSetting(const std::string&, const std::vector<std::string>&,
                                            const std::string& key) { return &key; }
template <class D> auto* GetCatalogSerializedValue(D& document, const std::string& key) {
    auto entry = document.find(key);
    return entry != document.end() ? &entry->second : nullptr;
}
bool IsSceneSettingPrimitive(const json&) { return true; }

struct EditCapture {
    struct EditState {
        std::string featureShortName = "Synthetic";
        Document originalSettings{{"Value", 5.0f}, {"Other", 1.0f}};
        Document workingSettings = originalSettings;
        Values workingOverrides;
        bool dirty = false;
    };
    std::optional<EditState> featureSceneEdit{std::in_place};
    std::map<std::string, Document> featureApplyDocuments{{"Synthetic", featureSceneEdit->originalSettings}};
    Values appliedSettings, savedSettings;
    std::set<std::string> appliedFeatureNames;
    std::map<std::string, int> pendingApplyVerifications;
    bool resolverDirty = false;
    bool groupedControls = false;
    static SettingAddress address(const std::string& key) { return {"Synthetic", {}, key}; }
    static bool AppliedValuesEqual(const json& actual, const json& expected) {
        return SceneSettingsManager::AppliedValuesEqual(actual, expected);
    }
    void EnsureBaselines(const std::vector<SettingAddress>&) {}
    void RefreshFeatureSceneEditOverrides(Document liveSettings) {
        struct Group { bool changed = false; std::vector<std::pair<SettingAddress, json>> values; };
        std::map<std::string, Group> groups;
        for (const auto& [key, value] : featureSceneEdit->workingSettings) {
            const auto* original = &featureSceneEdit->originalSettings.at(key);
            const auto* working = &value;
            auto& group = groups[groupedControls ? "Compound" : key];
GROUP_CHANGED
            group.values.emplace_back(address(key), value);
        }
REFRESH_OVERRIDES
    }
    bool Capture(Document sanitized) {
        auto document = featureApplyDocuments.find(featureSceneEdit->featureShortName);
CAPTURE_EDITS
    }
    void Reapply(float overwrite) {
        featureApplyDocuments.at("Synthetic")["Value"] = overwrite;
        appliedSettings[address("Value")] = overwrite;
    }
    bool IsFeatureSceneEditPreviewActive() const { return true; }
    bool CaptureFeatureSceneEditChanges(EditCapture*) { return Capture(featureApplyDocuments.at("Synthetic")); }
    bool Save() {
        auto* feature = this;
STORE_CAPTURE
        savedSettings = featureSceneEdit->workingOverrides;
        return true;
    }
};

bool UpdateEndpoint(float& endpoint, float numeric) {
    bool locationTransitionBatchesDirty = false;
ENDPOINT_UPDATE
    return locationTransitionBatchesDirty;
}
json FinishTransition(bool fastPath) {
    struct Transition { float startValue = 1e8f; float targetValue = 0.004f; } transition;
    const float smooth = 1.0f;
    if (fastPath) {
        struct Update { json value; };
        struct Batch { std::vector<Update> updates{1}; } batch;
        const std::size_t index = 0;
        const float linear = 1.0f;
ADVANCED_ENDPOINT
        return batch.updates[0].value;
    }
    const bool finished = true;
    const int address = 0;
    std::map<int, json> resolved;
RESOLVED_ENDPOINT
    return resolved.at(0);
}

int main() {
    using Manager = SceneSettingsManager;
    check(!Manager::ResolvedValuesEqual(0.005f, 0.004f), "Small explicit float edit must apply");
    check(!Manager::ResolvedValuesEqual(1e-8f, 2e-8f), "Tiny float changes must apply");
    check(!Manager::ResolvedValuesEqual(1e8f, std::nextafter(1e8f, INFINITY)), "Adjacent float changes must apply at any scale");
    check(!Manager::ResolvedValuesEqual(0.1, std::nextafter(0.1, 1.0)), "Double-valued edits retain their precision");
    check(Manager::ResolvedValuesEqual(0.005f, 0.005f), "Unchanged requested value avoids redundant apply");
    check(Manager::AppliedValuesEqual(json(0.004f), json(0.004)), "Float JSON readback may round to float precision");
    check(!Manager::AppliedValuesEqual(json(0.005f), json(0.004)), "Readback cannot accept a different small float");
    check(!Manager::AppliedValuesEqual(json(0.0f), json(1e-8f)), "Readback cannot lose a tiny target");
    check(!Manager::ResolvedValuesEqual(json(std::int64_t{9007199254740992LL}), json(std::int64_t{9007199254740993LL})), "Discrete integers retain exact equality");
    check(!Manager::AppliedValuesEqual(false, true), "Discrete booleans retain exact equality");
    float endpoint = 0.005f;
    check(UpdateEndpoint(endpoint, 0.004f) && endpoint == 0.004f, "Live transition endpoint follows a small change");
    check(!UpdateEndpoint(endpoint, 0.004f), "Stable endpoint avoids rebuilding transition batches");
    check(!UpdateEndpoint(endpoint, INFINITY), "Invalid endpoint remains rejected");
    check(FinishTransition(false) == json(0.004f), "Resolver completion applies exact endpoint without cancellation");
    check(FinishTransition(true) == json(0.004f), "Fast transition completion applies exact endpoint without cancellation");

    EditCapture edit;
    check(edit.Capture({{"Value", 2.0f}, {"Other", 1.0f}}), "Capture user edit under overwrite");
    edit.Reapply(5.0f);
    check(edit.Save(), "Save pending user edit after overwrite reapplication");
    check(edit.savedSettings.at(EditCapture::address("Value")) == json(2.0f), "Save retains pending user2 instead of rendered overwrite5");
    check(edit.featureApplyDocuments.at("Synthetic").at("Value") == json(5.0f), "Overwrite remains the presented value");
    check(edit.appliedSettings.at(EditCapture::address("Value")) == json(5.0f), "Applied tracking describes live overwrite rather than pending user value");
    check(edit.Capture({{"Value", 5.0f}, {"Other", 3.0f}}), "Capture another control while first edit is masked");
    check(edit.featureSceneEdit->workingOverrides.at(EditCapture::address("Value")) == json(2.0f), "Unrelated edit preserves masked pending value");
    check(edit.featureSceneEdit->workingOverrides.at(EditCapture::address("Other")) == json(3.0f), "Unrelated edit is captured normally");
    check(edit.Capture({{"Value", 4.0f}, {"Other", 3.0f}}), "Edit masked control again");
    check(edit.featureSceneEdit->workingOverrides.at(EditCapture::address("Value")) == json(4.0f), "New user input replaces older pending edit");
    check(edit.Capture({{"Value", 5.0f}, {"Other", 3.0f}}), "Revert changed control to original value");
    check(!edit.featureSceneEdit->workingOverrides.contains(EditCapture::address("Value")), "Explicit reversion clears pending value");
    EditCapture rounded;
    check(rounded.Capture({{"Value", 2.0f}, {"Other", 1.0f}}), "Capture before decimal overwrite is applied");
    rounded.featureApplyDocuments.at("Synthetic")["Value"] = 0.004;
    check(rounded.Capture({{"Value", 0.004f}, {"Other", 1.0f}}), "Capture rounded float readback of overwrite");
    check(rounded.featureSceneEdit->workingOverrides.at(EditCapture::address("Value")) == json(2.0f), "Float readback rounding cannot replace pending user edit");
    EditCapture reverted;
    reverted.featureSceneEdit->originalSettings["Value"] = 0.2;
    reverted.featureApplyDocuments.at("Synthetic")["Value"] = 0.2;
    check(reverted.Capture({{"Value", 0.5f}, {"Other", 1.0f}}) && reverted.featureSceneEdit->dirty, "Changing a default-valued control creates a pending edit");
    reverted.Reapply(0.5f);
    reverted.resolverDirty = false;
    check(reverted.Capture({{"Value", 0.2f}, {"Other", 1.0f}}), "Restore the original default through float controls");
    check(!reverted.featureSceneEdit->dirty && reverted.featureSceneEdit->workingOverrides.empty(), "Restoring float default clears both Save marker and pending override");
    check(reverted.resolverDirty, "Restoring default invalidates the draft preview");
    check(reverted.Save() && reverted.savedSettings.empty(), "Saving after restoring default cannot create scene entries");
    check(reverted.Capture({{"Value", 0.2f}, {"Other", 3.0f}}), "Edit a second control after restoring default");
    check(!reverted.featureSceneEdit->workingOverrides.contains(EditCapture::address("Value")), "Unrelated edits cannot resurrect a reverted float default");
    check(reverted.featureSceneEdit->workingOverrides.contains(EditCapture::address("Other")), "Other pending settings stay intact");
    EditCapture grouped;
    grouped.groupedControls = true;
    grouped.featureSceneEdit->originalSettings = {{"Value", 0.2}, {"Other", 0.4}};
    grouped.featureApplyDocuments.at("Synthetic") = grouped.featureSceneEdit->originalSettings;
    grouped.Capture({{"Value", 0.5f}, {"Other", 0.4f}});
    check(grouped.featureSceneEdit->workingOverrides.size() == 2, "A changed compound control keeps its components together");
    grouped.Reapply(0.5f);
    grouped.Capture({{"Value", 0.2f}, {"Other", 0.4f}});
    check(!grouped.featureSceneEdit->dirty && grouped.featureSceneEdit->workingOverrides.empty(), "Restoring all compound components clears the whole pending edit");
    for (const auto& [original, changed] : std::vector<std::pair<json, json>>{{false, true}, {std::int64_t{2}, std::int64_t{3}}}) {
        EditCapture discrete;
        discrete.featureSceneEdit->originalSettings["Value"] = original;
        discrete.featureApplyDocuments.at("Synthetic")["Value"] = original;
        discrete.Capture({{"Value", changed}, {"Other", 1.0f}});
        check(discrete.featureSceneEdit->dirty, "Discrete edit creates pending state");
        discrete.Capture({{"Value", original}, {"Other", 1.0f}});
        check(!discrete.featureSceneEdit->dirty && discrete.featureSceneEdit->workingOverrides.empty(), "Restoring a discrete default clears pending state");
    }
}
'''
        source = source.replace("RESOLVED_EQUALITY", braced(manager, "bool SceneSettingsManager::ResolvedValuesEqual("))
        source = source.replace("APPLIED_EQUALITY", braced(manager, "bool SceneSettingsManager::AppliedValuesEqual("))
        source = source.replace("GROUP_CHANGED", next(line.strip() for line in manager.splitlines() if "group.changed |=" in line))
        source = source.replace("REFRESH_OVERRIDES", refresh).replace("CAPTURE_EDITS", capture)
        source = source.replace("STORE_CAPTURE", storing).replace("ENDPOINT_UPDATE", endpoint_update)
        source = source.replace("ADVANCED_ENDPOINT", advanced_endpoint).replace("RESOLVED_ENDPOINT", resolved_endpoint)
        self.compile_and_run(source)

    def test_native_saved_sets_and_composed_transitions(self):
        manager = MANAGER_PATH.read_text(encoding="utf-8")
        header = (ROOT / "src/SceneSettingsManager.h").read_text(encoding="utf-8")
        resolver = braced(manager, "void SceneSettingsManager::ResolveExteriorSettings(")
        start = resolver.index("float result = 0.0f;")
        end = resolver.index("if (std::isfinite(result))", start)
        period_loop = resolver[start:end]
        getter = braced(resolver, "const auto getPeriodValue =") + ";"
        declarations = "\n".join([
            braced(header, "enum class TimeOfDayPeriod") + ";",
            "static constexpr int kPeriodCount = static_cast<int>(TimeOfDayPeriod::Count);",
            "struct SettingEntry { TimeOfDayPeriod period; float value; };",
            braced(header, "struct PeriodicSceneConfig") + ";",
            braced(header, "enum class SceneContextType") + ";",
            "struct SceneContextId { SceneContextType type; TimeOfDayPeriod period; bool allPeriods; };",
            braced(header, "enum class LocationTargetType") + ";",
            braced(header, "struct LocationTarget\n") + ";",
        ])
        ui = (ROOT / "src/CSEditor/SceneSettingsUI.cpp").read_text(encoding="utf-8")
        selection = braced(ui, "static bool GetWeatherSelectionTimeOfDay(")
        selection = selection[selection.index("return "):selection.rfind("}")]
        copying = braced(manager, "SceneSettingsManager::CopyResult SceneSettingsManager::CopySettings(")
        start = copying.index("if (destinationConfig && destinationEntries->empty())")
        initial_copy_mode = copying[start:copying.index(";", start) + 1]
        chain = braced(manager, "std::vector<SceneSettingsManager::LocationTarget> BuildLocationTargetChain(")
        start = chain.index("std::vector<SceneSettingsManager::LocationTarget> targets;")
        worldspace_chain = chain[start:chain.index("std::vector<RE::BGSKeyword*> locationTypes;", start)]
        source = r'''
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <vector>
#include <cstdlib>
#include <string>

namespace RE {
using FormID = unsigned int;
struct TESWorldSpace {
    FormID id;
    FormID GetFormID() const { return id; }
};
struct TESObjectCELL {
    bool exterior;
    TESWorldSpace* worldSpace;
    bool IsExteriorCell() const { return exterior; }
    const TESObjectCELL& GetRuntimeData() const { return *this; }
};
}
namespace Util {
std::string GetFormFileKey(RE::TESWorldSpace* worldspace) { return std::to_string(worldspace->id); }
}
std::string GetLocationTargetDisplayName(RE::TESWorldSpace* worldspace) { return Util::GetFormFileKey(worldspace); }

struct SceneSettingsManager {
DECLARATIONS
};
WORLDSPACE_TARGET
std::vector<SceneSettingsManager::LocationTarget> WorldspaceTargets(RE::TESObjectCELL* cell) {
WORLDSPACE_CHAIN
    return targets;
}
MEMBERSHIP
NUMERIC_REQUIREMENTS
using Period = SceneSettingsManager::TimeOfDayPeriod;
void InitializeCopiedMode(SceneSettingsManager::PeriodicSceneConfig* destinationConfig,
                         const SceneSettingsManager::SceneContextId& destination) {
    using TimeOfDayPeriod = SceneSettingsManager::TimeOfDayPeriod;
    const auto* destinationEntries = destinationConfig ? &destinationConfig->entries : nullptr;
INITIAL_COPY_MODE
}
bool PreferTimeOfDay(const SceneSettingsManager::PeriodicSceneConfig& config) {
WEATHER_SELECTION
}
constexpr int kPeriodCount = SceneSettingsManager::kPeriodCount;
TRANSITION_LAYER;
using SettingAddress = int;
using PeriodSettingMap = std::map<SettingAddress, std::array<std::optional<float>, kPeriodCount>>;

void check(bool condition, const char* name) {
    if (!condition) { std::fprintf(stderr, "%s\n", name); std::exit(1); }
}
void near(float actual, float expected, const char* name) {
    if (std::abs(actual - expected) > 0.0001f) {
        std::fprintf(stderr, "%s: got %g, expected %g\n", name, actual, expected);
        std::exit(1);
    }
}
struct Evaluation {
    float baseline = 10.0f;
    float weatherLerp = 0.25f;
    std::array<float, kPeriodCount> factors{0.25f, 0.75f};
    PeriodSettingMap userTimeOfDayValues, overwriteTimeOfDayValues;
    PeriodSettingMap previousUser, currentUser, previousOverwrite, currentOverwrite, userLocation, overwriteLocation;
    struct Transition { std::array<LocationTransitionLayer, kPeriodCount> startLayers; };
    std::map<SettingAddress, Transition> activeLocationTransitions;
    float evaluate(bool transitionStart = false, bool applyOverwrites = true, std::set<SettingAddress>* appliedSceneSettings = nullptr) const {
        const SettingAddress address = 0;
        const auto* previousUserWeather = &previousUser;
        const auto* currentUserWeather = &currentUser;
        const auto* previousOverwriteWeather = &previousOverwrite;
        const auto* currentOverwriteWeather = &currentOverwrite;
        const auto* userLocationPeriods = transitionStart ? nullptr : &userLocation;
        const auto* overwriteLocationPeriods = transitionStart ? nullptr : &overwriteLocation;
        const std::optional<float> flatLocation;
        const auto transition = transitionStart ? activeLocationTransitions.find(address) : activeLocationTransitions.end();
GETTER
PERIOD_LOOP
APPLIED_SCENE_SETTINGS
        return result;
    }
};

int main() {
    RE::TESWorldSpace firstWorld{1}, secondWorld{2};
    RE::TESObjectCELL townCell{true, &firstWorld}, wildernessCell{true, &firstWorld};
    auto town = WorldspaceTargets(&townCell);
    auto wilderness = WorldspaceTargets(&wildernessCell);
    check(town.size() == 1 && wilderness.size() == 1 && town[0].formKey == wilderness[0].formKey,
          "Worldspace stays current across cells without a location record");
    check(town[0].type == SceneSettingsManager::LocationTargetType::Worldspace,
          "Worldspace is distinct from a region, location or cell");
    wildernessCell.worldSpace = &secondWorld;
    check(WorldspaceTargets(&wildernessCell)[0].formId == secondWorld.id,
          "Worldspace follows the actual exterior cell");
    wildernessCell.exterior = false;
    check(WorldspaceTargets(&wildernessCell).empty(), "Interiors do not inherit exterior worldspace membership");
    townCell.worldSpace = nullptr;
    check(WorldspaceTargets(&townCell).empty() && WorldspaceTargets(nullptr).empty(), "Missing worldspace is handled safely");
    SceneSettingsManager::PeriodicSceneConfig config;
    check(!PreferTimeOfDay(config), "Unconfigured weather keeps Normal selected");
    config.timeOfDayEnabled = true;
    check(PreferTimeOfDay(config), "Empty weather preserves selected timed mode");
    config.timeOfDayEnabled = false;
    config.entries = {{Period::Dawn, 25.0f}};
    check(PreferTimeOfDay(config), "Timed-only weather automatically selects Time of Day");
    check(!config.timeOfDayEnabled && config.entries.size() == 1, "Selection query does not modify saved sets");
    config.entries.push_back({Period::Count, 100.0f});
    check(!PreferTimeOfDay(config), "Mixed weather preserves Normal selection");
    config.timeOfDayEnabled = true;
    check(PreferTimeOfDay(config), "Mixed weather preserves Time of Day selection");
    config.timeOfDayEnabled = false;
    config.entries = {{Period::Count, 100.0f}};
    check(!PreferTimeOfDay(config), "Normal-only weather stays Normal");
    config.entries = {{static_cast<Period>(-1), 100.0f}};
    check(!PreferTimeOfDay(config), "Invalid periods do not select Time of Day");
    config.entries = {{Period::Count, 100.0f}, {Period::Dawn, 25.0f}};
    check(config.IsPeriodActive(Period::Count) && !config.IsPeriodActive(Period::Dawn), "Normal mode isolation");
    config.timeOfDayEnabled = true;
    check(!config.IsPeriodActive(Period::Count) && config.IsPeriodActive(Period::Dawn), "Time of Day mode isolation");
    check(!config.IsPeriodActive(static_cast<Period>(-1)), "Invalid period is inactive");
    config.timeOfDayEnabled = false;
    check(config.entries.size() == 2 && config.entries[0].value == 100.0f && config.entries[1].value == 25.0f, "Switch preserves both saved sets");
    using Type = SceneSettingsManager::SceneContextType;
    InitializeCopiedMode(nullptr, {Type::Interior, Period::Count, false});
    for (auto type : {Type::Weather, Type::Location}) {
        SceneSettingsManager::PeriodicSceneConfig copied;
        InitializeCopiedMode(&copied, {type, Period::Day, false});
        check(copied.timeOfDayEnabled, "First timed copy activates an empty scene");
        copied.entries.push_back({Period::Day, 25.0f});
        InitializeCopiedMode(&copied, {type, Period::Count, false});
        check(copied.timeOfDayEnabled, "Copying Normal does not disable a configured timed scene");
        copied.entries.clear();
        InitializeCopiedMode(&copied, {type, Period::Count, false});
        check(!copied.timeOfDayEnabled, "First Normal copy activates an empty scene's Normal set");
        copied.entries.push_back({Period::Count, 100.0f});
        InitializeCopiedMode(&copied, {type, Period::Count, true});
        check(!copied.timeOfDayEnabled, "Copying All preserves a configured Normal scene");
        copied.entries.clear();
        InitializeCopiedMode(&copied, {type, Period::Count, true});
        check(copied.timeOfDayEnabled, "First All-periods copy activates Time of Day");
        check(EntryBelongsToContext(config.entries[0], {type, Period::Count, false}), "Normal copy membership");
        check(!EntryBelongsToContext(config.entries[1], {type, Period::Count, false}), "Normal excludes periods");
        check(!EntryBelongsToContext(config.entries[0], {type, Period::Count, true}), "All periods excludes Normal");
        check(EntryBelongsToContext(config.entries[1], {type, Period::Count, true}), "All periods includes saved period");
        check(!EntryBelongsToContext(config.entries[1], {type, Period::Night, false}), "Period copy isolation");
    }
    check(!CopyContextRequiresNumeric({Type::Interior, Period::Count, false}), "Interior allows discrete settings");
    check(!CopyContextRequiresNumeric({Type::Location, Period::Count, false}), "Normal locations allow discrete settings");
    check(CopyContextRequiresNumeric({Type::Location, Period::Dawn, false}), "Location periods require floats");
    check(CopyContextRequiresNumeric({Type::Location, Period::Count, true}), "Location All requires floats");
    check(CopyContextRequiresNumeric({Type::Weather, Period::Count, false}), "Normal weather requires floats for weather blending");
    check(CopyContextRequiresNumeric({Type::TimeOfDay, Period::Dawn, false}), "Global periods require floats");

    Evaluation e;
    std::set<SettingAddress> controlled;
    near(e.evaluate(false, true, &controlled), 10.0f, "Unconfigured default preview");
    check(controlled.empty(), "Default alone is not a scene setting");
    e.userTimeOfDayValues[0][0] = 10.0f;
    near(e.evaluate(false, true, &controlled), 10.0f, "Default-valued time of day scene setting");
    check(controlled.contains(0), "Default-valued lower Time of Day is outlined in location preview");
    e.userTimeOfDayValues.clear();
    controlled.clear();
    e.currentUser[0][0] = 10.0f;
    near(e.evaluate(false, true, &controlled), 10.0f, "Default-valued weather scene setting");
    check(controlled.contains(0), "Default-valued lower weather is outlined in location preview");
    controlled.clear();
    e.weatherLerp = 0;
    e.evaluate(false, true, &controlled);
    check(controlled.empty(), "Zero-weight weather does not outline the control");
    e.currentUser.clear();
    e.weatherLerp = 0.25f;
    e.overwriteTimeOfDayValues[0][0] = 10.0f;
    e.evaluate(false, true, &controlled);
    check(controlled.contains(0), "Default-valued lower overwrite is outlined");
    controlled.clear();
    e.evaluate(false, false, &controlled);
    check(controlled.empty(), "Bypassed lower overwrite cannot outline a default fallback");
    e.overwriteTimeOfDayValues.clear();
    near(e.evaluate(), 10.0f, "Empty periods fall back to baseline");
    e.userTimeOfDayValues[0][0] = 20.0f;
    e.userTimeOfDayValues[0][1] = 40.0f;
    near(e.evaluate(), 35.0f, "Time of Day interpolation");
    e.previousUser[0][0] = 60.0f;
    e.currentUser[0][0] = 100.0f;
    e.currentUser[0][1] = 80.0f;
    near(e.evaluate(), 55.0f, "Weather and weather periods blend over global periods");
    e.userLocation[0][0] = 200.0f;
    near(e.evaluate(), 87.5f, "Sparse location period falls through to weather");
    e.overwriteTimeOfDayValues[0][0] = 300.0f;
    near(e.evaluate(), 112.5f, "Global overwrite wins over user location");
    e.previousOverwrite[0][0] = 500.0f;
    near(e.evaluate(), 150.0f, "Weather overwrite uses overwrite fallback");
    e.overwriteLocation[0][0] = 700.0f;
    near(e.evaluate(), 212.5f, "Location overwrite has final priority");
    near(e.evaluate(false, false), 87.5f, "Temporary preview bypass removes all overwrite layers and preserves User layer fallbacks");

    for (float weather : {0.0f, 0.25f, 1.0f}) {
        for (float time : {0.0f, 0.5f, 1.0f}) {
            for (float cell : {0.0f, 0.4f, 1.0f}) {
                e.weatherLerp = weather;
                e.factors = {time, 1.0f - time};
                const float priorDawn = 500.0f + (300.0f - 500.0f) * weather;
                const float sunrise = 40.0f + (80.0f - 40.0f) * weather;
                auto& start = e.activeLocationTransitions[0].startLayers;
                start[0] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
                const float from = e.evaluate(true);
                const float to = e.evaluate();
                near(from + (to - from) * cell,
                    time * (priorDawn + (700.0f - priorDawn) * cell) + (1.0f - time) * sunrise,
                    "Simultaneous time, weather, cell and overwrite transition");
                start[0] = {1.0f - cell, 0.0f, 0.0f, cell, 700.0f * cell};
                near(e.evaluate(true), from + (to - from) * cell, "Interrupted transition retains current weighted source");
            }
        }
    }
    e = Evaluation{};
    e.userTimeOfDayValues[0][0] = 20.0f;
    e.activeLocationTransitions[0].startLayers[0] = {0.4f, 0.6f, 60.0f, 0.0f, 0.0f};
    near(e.evaluate(true), 24.5f, "Interrupted user-location fade keeps lower layers live");
    e.overwriteTimeOfDayValues[0][0] = 200.0f;
    near(e.evaluate(true), 57.5f, "Overwrite masks all user-location weights");
}
'''
        source = source.replace("WEATHER_SELECTION", selection)
        source = source.replace("INITIAL_COPY_MODE", initial_copy_mode)
        source = source.replace("WORLDSPACE_TARGET", braced(manager, "SceneSettingsManager::LocationTarget MakeWorldspaceTarget("))
        source = source.replace("WORLDSPACE_CHAIN", worldspace_chain)
        source = source.replace("DECLARATIONS", declarations)
        start = resolver.index("if (appliedSceneSettings && (hasUserSetting")
        source = source.replace("APPLIED_SCENE_SETTINGS", resolver[start:resolver.index(";", start) + 1])
        source = source.replace("MEMBERSHIP", braced(manager, "bool EntryBelongsToContext("))
        numeric_requirements = "\n".join([
            braced(manager, "bool CopyContextRequiresNumeric(SceneSettingsManager::SceneContextType"),
            braced(manager, "bool CopyContextRequiresNumeric(const SceneSettingsManager::SceneContextId&"),
        ])
        source = source.replace("NUMERIC_REQUIREMENTS", numeric_requirements)
        source = source.replace("TRANSITION_LAYER", braced(header, "struct LocationTransitionLayer"))
        source = source.replace("GETTER", getter).replace("PERIOD_LOOP", period_loop)
        self.compile_and_run(source)

    def compile_and_run(self, source, imgui_root=None):
        with tempfile.TemporaryDirectory(prefix="scene-settings-test-") as directory:
            directory = Path(directory)
            cpp = directory / "scene_runtime.cpp"
            executable = directory / ("scene_runtime.exe" if os.name == "nt" else "scene_runtime")
            cpp.write_text(source, encoding="utf-8")
            compiler = None if imgui_root else shutil.which("clang++") or shutil.which("g++")
            if compiler:
                command = [compiler, "-std=c++20", str(cpp), "-o", str(executable)]
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
                command = f'call "{vcvars}" >nul && cl /nologo /EHsc /std:c++20 "{cpp}" /Fe:"{executable}"'
                if imgui_root:
                    command += f' /MD /I"{imgui_root / "include"}" "{imgui_root / "lib/imgui.lib"}" user32.lib gdi32.lib imm32.lib'
            else:
                self.skipTest("Native C++ compiler unavailable")
            compiled = subprocess.run(command, shell=isinstance(command, str), cwd=directory,
                                      capture_output=True, text=True, timeout=60)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            tested = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
            self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)


if __name__ == "__main__":
    unittest.main()
