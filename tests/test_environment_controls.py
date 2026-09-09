import os
import unittest

from test_scene_settings_policy import ROOT
import test_scene_settings_runtime as runtime
from test_scene_settings_runtime import braced


class EnvironmentControlTests(unittest.TestCase):
    def test_play_stop_button_size_icon_and_disabled_state(self):
        library_root = ROOT / "build/ALL/vcpkg_installed/x64-windows-static-md-release"
        if os.name != "nt" or not (library_root / "lib/imgui.lib").exists():
            self.skipTest("Uses the Windows build's ImGui library")
        ui = (ROOT / "src/CSEditor/SceneSettingsUI.cpp").read_text(encoding="utf-8")
        source = r'''
#include <imgui.h>
#include <imgui_internal.h>
#include <initializer_list>
#include <cmath>
#include <cstdio>
#include <cstdlib>
DRAW_BUTTON
void check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
int main() {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1000, 800);
    unsigned char* pixels; int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    for (float scale : {1.0f, 1.5f, 2.0f}) {
        ImVec2 playSize;
        ImGuiID playId = 0;
        for (bool playing : {false, true}) for (bool disabled : {false, true}) {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(20, 20));
            ImGui::SetNextWindowSize(ImVec2(600, 400));
            ImGui::Begin("Fixture", nullptr, ImGuiWindowFlags_NoSavedSettings);
            ImGui::SetWindowFontScale(scale);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4 * scale, 3 * scale));
            ImGui::BeginDisabled(disabled);
            DrawPreviewButton(playing);
            auto rect = GImGui->LastItemData.Rect;
            check(rect.GetWidth() == rect.GetHeight(), "Play and Stop are square");
            if (!playing) { playSize = rect.GetSize(); playId = GImGui->LastItemData.ID; }
            else {
                check(rect.GetWidth() == playSize.x && rect.GetHeight() == playSize.y,
                      "Play/Stop do not shift toolbar layout at any DPI");
                check(GImGui->LastItemData.ID == playId, "Play/Stop preserve widget identity");
                auto* list = ImGui::GetWindowDrawList();
                check(list->VtxBuffer.Size >= 4, "Stop square emits vertices");
                ImRect icon;
                icon.Min = list->VtxBuffer[list->VtxBuffer.Size - 4].pos;
                icon.Max = list->VtxBuffer[list->VtxBuffer.Size - 2].pos;
                check(icon.GetWidth() == ImGui::GetFontSize() * 0.5f && icon.GetWidth() == icon.GetHeight(),
                      "Stop icon scales with the font");
                check(icon.GetCenter().x == rect.GetCenter().x && icon.GetCenter().y == rect.GetCenter().y,
                      "Stop square is centered");
                check(list->VtxBuffer.back().col == ImGui::GetColorU32(ImGuiCol_Text),
                      "Stop icon honors theme and disabled alpha");
            }
            check(((GImGui->LastItemData.ItemFlags & ImGuiItemFlags_Disabled) != 0) == disabled,
                  "Play/Stop respect loading/disabled guard");
            ImGui::EndDisabled();
            ImGui::PopStyleVar();
            ImGui::End();
            ImGui::Render();
        }
    }
    ImGui::DestroyContext();
}
'''
        source = source.replace("DRAW_BUTTON", braced(ui, "static bool DrawPreviewButton("))
        runtime.SceneSettingsRuntimeTests.compile_and_run(self, source, imgui_root=library_root)

    def test_shared_weather_time_preview_lifecycle(self):
        header = braced((ROOT / "src/Utils/Game.h").read_text(encoding="utf-8"),
                        "namespace Util::EnvironmentControls")
        implementation = braced((ROOT / "src/Utils/Game.cpp").read_text(encoding="utf-8"),
                                "namespace Util::EnvironmentControls")
        source = r'''
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
namespace RE {
struct TESWeather {};
struct Sky {
    enum class Flags { kReleaseWeatherOverride };
    struct FlagSet {
        bool release = false;
        bool any(Flags) const { return release; }
        void reset(Flags) { release = false; }
    } flags;
    TESWeather* currentWeather = nullptr;
    TESWeather* overrideWeather = nullptr;
    TESWeather* defaultWeather = nullptr;
    int forces = 0, sets = 0, releases = 0;
    void ForceWeather(TESWeather* w, bool override) {
        ++forces;
        currentWeather = w;
        if (override) overrideWeather = w;
    }
    void SetWeather(TESWeather* w, bool override, bool) {
        ++sets;
        currentWeather = w;
        if (override) overrideWeather = w;
    }
    void ReleaseWeatherOverride() { ++releases; flags.release = true; }
    void ResetWeather() { currentWeather = defaultWeather; }
};
struct Global { float value = 0; };
struct Calendar { Global* gameHour; Global* timeScale; };
}
namespace globals::game {
RE::Sky* sky = nullptr;
RE::Calendar* calendar = nullptr;
}
namespace Util {
int timeJumps = 0;
void RequestTimeJumpTransition() { ++timeJumps; }
}
HEADER
IMPLEMENTATION
void require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
int main() {
    using namespace Util::EnvironmentControls;
    RE::TESWeather first, second, third;
    require(!StartPreview(&first, {}), "weather preview needs sky");
    require(!StartPreview(nullptr, 12.0f), "time preview needs calendar");
    require(!StartPreview(nullptr, {}), "empty preview refused");
    require(!IsWeatherLockAvailable(), "hook readiness begins false");
    SetWeatherLockAvailable();
    require(IsWeatherLockAvailable(), "shared hook readiness");
    RE::Sky sky;
    RE::Global hour{9}, scale{37};
    RE::Calendar calendar{&hour, &scale};
    globals::game::sky = &sky;
    globals::game::calendar = &calendar;
    sky.currentWeather = sky.defaultWeather = &first;

    require(StartPreview(&second, {}), "weather Play");
    require(IsPreviewActive() && GetLockedWeather() == &second && sky.currentWeather == &second,
            "Play claims weather override");
    require(scale.value == 37 && hour.value == 9, "weather alone does not stop time");
    const int firstForces = sky.forces;
    MaintainLocks();
    require(sky.forces == firstForces, "unchanged frame does not force weather again");
    sky.currentWeather = &third;
    sky.flags.release = true;
    MaintainLocks();
    require(sky.currentWeather == &second && !sky.flags.release, "weather survives cell/weather release");
    StopPreview();
    require(!IsPreviewActive() && !GetLockedWeather() && sky.flags.release, "Stop releases weather");

    SetLockedWeather(&first);
    PauseTime();
    require(IsTimePaused() && scale.value == 0 && GetSavedTimeScale() == 37, "editor time pause");
    require(StartPreview(&second, 12.0f), "combined weather and TOD Play");
    require(scale.value == 0 && hour.value == 12 && sky.currentWeather == &second, "combined preview applied");
    const int jumps = Util::timeJumps;
    MaintainLocks();
    require(Util::timeJumps == jumps, "steady clock avoids repeated sky synchronization");
    hour.value = 15;
    scale.value = 20;
    MaintainLocks();
    require(hour.value == 12 && scale.value == 0, "preview holds hour and timescale");
    StopPreview();
    require(GetLockedWeather() == &first && sky.currentWeather == &first && IsTimePaused(),
            "Stop preserves pre-existing editor locks");
    require(hour.value == 12, "Stop resumes from preview hour, not previous hour");
    ResumeTime();
    require(scale.value == 37, "original timescale restored");
    SetLockedWeather(nullptr);

    require(StartPreview(nullptr, 0.0f), "midnight is a valid time lock");
    SetTimeRunningForMenu(true);
    require(scale.value == 37 && !IsTimePaused(), "loading/wait restores running time");
    hour.value = 6;
    MaintainLocks();
    require(hour.value == 6 && scale.value == 37, "preview cannot fight loading/wait time");
    SetTimeRunningForMenu(true);
    require(!StartPreview(&third, 10.0f) && IsPreviewActive(), "busy menu refuses replacement preview");
    SetTimeRunningForMenu(false);
    require(IsTimePaused() && hour.value == 0, "menu close resumes selected time lock");
    SetTimeRunningForMenu(true);
    StopPreview();
    SetTimeRunningForMenu(false);
    require(!IsTimePaused() && scale.value == 37, "Stop during loading does not leave a pending pause");

    PauseTime();
    require(StartPreview(nullptr, 18.0f), "preview while previously paused");
    SetTimeRunningForMenu(true);
    StopPreview();
    require(scale.value > 0, "Stop preserves engine progress during loading");
    SetTimeRunningForMenu(false);
    require(IsTimePaused() && scale.value == 0, "pre-existing pause restored after loading");
    ResumeTime();
    require(scale.value == 37, "pre-existing pause still has its saved speed");

    SetTimeRunningForMenu(true);
    PauseTime();
    require(scale.value > 0, "explicit Pause cannot freeze a loading menu");
    ResumeTime();
    SetTimeRunningForMenu(false);
    require(scale.value == 37, "Resume cancels deferred pause");
    scale.value = 0;
    SetTimeRunningForMenu(true);
    SetTimeRunningForMenu(false);
    require(scale.value > 0 && !IsTimePaused(), "foreign zero-timescale cannot leave loading stuck");
    scale.value = 37;

    require(StartPreview(&second, 14.0f), "preview for explicit-control takeover");
    ChangeWeather(&third, true);
    require(!IsPreviewActive() && GetLockedWeather() == &third && scale.value == 37,
            "weather picker retargets shared lock and ends combined preview");
    require(StartPreview(&second, 11.0f), "replacement over editor weather lock");
    SetGameHour(7.0f);
    require(!IsPreviewActive() && hour.value == 7 && GetLockedWeather() == &third && scale.value == 37,
            "hour slider takes over and preserves earlier editor weather lock");
    require(StartPreview(nullptr, 3.0f), "time preview before speed edit");
    SetTimeScale(15);
    require(!IsPreviewActive() && scale.value == 15, "speed control takes over");
    PauseTime();
    SetTimeScale(22);
    require(IsTimePaused() && scale.value == 0 && GetSavedTimeScale() == 22, "speed edit respects editor pause");
    ResetTimeScale();
    ResumeTime();
    require(scale.value == kDefaultTimeScale, "reset speed uses shared default");

    sky.currentWeather = &third;
    int releases = sky.releases;
    RefreshWeather(&third);
    require(sky.releases == releases && GetLockedWeather() == &third, "weather edits preserve lock");
    SetLockedWeather(nullptr);
    releases = sky.releases;
    RefreshWeather(&third);
    require(sky.releases == releases + 1, "unlocked weather refresh releases its temporary override");
    int forces = sky.forces;
    RefreshWeather(&second);
    require(sky.forces == forces, "editing inactive weather does not change scene");
    ChangeWeather(&second, false);
    require(sky.sets > 0 && !GetLockedWeather(), "unlocked gradual selection remains gradual");
    require(StartPreview(&third, 4.0f), "preview before reset weather");
    ResetWeather();
    require(!IsPreviewActive() && !GetLockedWeather() && sky.currentWeather == &first && scale.value == 20,
            "reset weather releases preview instead of being reversed next frame");

    require(StartPreview(&second, 6.0f), "valid preview before invalid input");
    require(!StartPreview(&third, 24.0f) && !StartPreview(&third, -1.0f) &&
            !StartPreview(&third, std::numeric_limits<float>::quiet_NaN()), "invalid hours refused");
    require(IsPreviewActive() && GetLockedWeather() == &second && hour.value == 6,
            "invalid input does not disturb existing preview");
    require(!SetGameHour(24.0f), "manual invalid hour refused");
    StopPreview();
    require(!IsPreviewActive() && scale.value == 20, "final Stop restores time");
}
'''
        source = source.replace("HEADER", header).replace("IMPLEMENTATION", implementation)
        runtime.SceneSettingsRuntimeTests.compile_and_run(self, source)

    def test_all_ui_environment_writes_use_shared_controls(self):
        editor = (ROOT / "src/CSEditor/EditorWindow.cpp").read_text(encoding="utf-8")
        scene = (ROOT / "src/CSEditor/SceneSettingsUI.cpp").read_text(encoding="utf-8")
        for function in ("PauseTime", "ResumeTime", "ResetTimeScale", "SetTimeRunningForMenu"):
            self.assertIn(f"Util::EnvironmentControls::{function}(", braced(editor, f"void EditorWindow::{function}("))
        for path in ("src/CSEditor/SceneSettingsUI.cpp", "src/CSEditor/Widget.cpp",
                     "src/Features/SceneSelector.cpp", "src/Menu/AdvancedSettingsRenderer.cpp"):
            source = (ROOT / path).read_text(encoding="utf-8")
            self.assertNotIn("->ForceWeather(", source)
            self.assertNotIn("->SetWeather(", source)
            self.assertNotIn("gameHour->value =", source)
            self.assertNotIn("timeScale->value =", source)
        self.assertNotIn("StopPreview", braced(scene, "void HideFeaturePageEditing("))
        self.assertIn("SetFeaturePagePreviewPlaying(false)", braced(scene, "static bool StartFeaturePageEditing("))
        self.assertIn("DrawPreviewButton(playing)", scene)
        self.assertFalse((ROOT / "src/Utils/EnvironmentControls.cpp").exists())
        self.assertFalse((ROOT / "src/Utils/EnvironmentControls.h").exists())


if __name__ == "__main__":
    unittest.main()
