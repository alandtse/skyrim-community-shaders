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
    float evaluate(bool transitionStart = false) const {
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
        source = source.replace("MEMBERSHIP", braced(manager, "bool EntryBelongsToContext("))
        numeric_requirements = "\n".join([
            braced(manager, "bool CopyContextRequiresNumeric(SceneSettingsManager::SceneContextType"),
            braced(manager, "bool CopyContextRequiresNumeric(const SceneSettingsManager::SceneContextId&"),
        ])
        source = source.replace("NUMERIC_REQUIREMENTS", numeric_requirements)
        source = source.replace("TRANSITION_LAYER", braced(header, "struct LocationTransitionLayer"))
        source = source.replace("GETTER", getter).replace("PERIOD_LOOP", period_loop)
        with tempfile.TemporaryDirectory(prefix="scene-settings-test-") as directory:
            directory = Path(directory)
            cpp = directory / "scene_runtime.cpp"
            executable = directory / ("scene_runtime.exe" if os.name == "nt" else "scene_runtime")
            cpp.write_text(source, encoding="utf-8")
            compiler = shutil.which("clang++") or shutil.which("g++")
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
            else:
                self.skipTest("Native C++ compiler unavailable")
            compiled = subprocess.run(command, shell=isinstance(command, str), cwd=directory,
                                      capture_output=True, text=True, timeout=60)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            tested = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
            self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)


if __name__ == "__main__":
    unittest.main()
