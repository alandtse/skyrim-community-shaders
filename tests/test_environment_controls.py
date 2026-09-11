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
namespace Util {
int stopButtons = 0;
bool ErrorTextButton(const char* id, ImVec2 size) { ++stopButtons; return ImGui::Button(id, size); }
}
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
            const int previousStopButtons = Util::stopButtons;
            DrawPreviewButton(playing);
            check(Util::stopButtons == previousStopButtons + int(playing), "Only Stop uses the shared red button");
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

    def test_slider_press_hold_release_and_hidden_ui(self):
        library_root = ROOT / "build/ALL/vcpkg_installed/x64-windows-static-md-release"
        if os.name != "nt" or not (library_root / "lib/imgui.lib").exists():
            self.skipTest("Uses the Windows build's ImGui library")
        editor = (ROOT / "src/CSEditor/EditorWindow.cpp").read_text(encoding="utf-8")
        source = r'''
#include <imgui.h>
#include <imgui_internal.h>
#include <cstdio>
#include <cstdlib>
struct Global { float value = 12.0f; } hour;
struct Calendar { Global* gameHour = &hour; } calendar;
Calendar* GetCalendar() { return &calendar; }
namespace Util {
int timeJumps = 0;
void RequestTimeJumpTransition() { ++timeJumps; }
namespace EnvironmentControls {
bool held = false;
int begins = 0, ends = 0, edits = 0;
void BeginGameHourScrub() { held = true; ++begins; }
void EndGameHourScrub() { held = false; ++ends; }
bool SetGameHour(float value, bool) { hour.value = value; ++edits; return true; }
}
}
struct EditorWindow {
    static constexpr float kGameHourMax = 24.0f;
    static constexpr double kGameHourScrubRefreshIntervalSeconds = 0.1;
    double lastGameHourScrubRefreshTime = 0.0;
    bool gameHourScrubRefreshIssued = false;
    ImGuiID gameHourScrubId = 0;
    bool DrawGameHourSlider(const char* label, const char* format);
    void FinishGameHourSliderFrame(bool widgetsDrawn);
} editor;
DRAW_SLIDER
FINISH_FRAME
void require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
ImVec2 frame(bool visible = true, bool disabled = false) {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(20, 20));
    ImGui::SetNextWindowSize(ImVec2(600, 400));
    ImGui::Begin("Fixture", nullptr, ImGuiWindowFlags_NoSavedSettings);
    ImVec2 center;
    if (visible) {
        ImGui::SetNextItemWidth(240);
        ImGui::BeginDisabled(disabled);
        editor.DrawGameHourSlider("##Hour", "%.2f");
        center = GImGui->LastItemData.Rect.GetCenter();
        ImGui::EndDisabled();
    }
    ImGui::End();
    editor.FinishGameHourSliderFrame(true);
    ImGui::Render();
    return center;
}
int main() {
    using namespace Util::EnvironmentControls;
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1000, 800);
    unsigned char* pixels; int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    auto center = frame();
    io.AddMousePosEvent(center.x, center.y);
    frame();
    io.AddMouseButtonEvent(0, true);
    frame();
    require(held && begins == 1 && edits == 0 && hour.value == 12,
            "pressing the existing value locks weather without moving or changing the value");
    frame();
    require(held && begins == 1 && edits == 0, "stationary hold keeps the same temporary lock");
    io.AddMouseButtonEvent(0, false);
    frame();
    require(!held && ends == 1 && edits == 0, "release without any edit unlocks weather");

    io.AddMouseButtonEvent(0, true);
    frame();
    io.AddMousePosEvent(center.x + 30, center.y);
    frame();
    require(held && hour.value > 12 && edits > 0, "drag edits time while weather remains held");
    const int jumpsBeforeRelease = Util::timeJumps;
    io.AddMouseButtonEvent(0, false);
    frame();
    require(!held && Util::timeJumps == jumpsBeforeRelease + 1,
            "drag release unlocks and performs the final throttled sky refresh");

    io.AddMouseButtonEvent(0, true);
    frame();
    require(held, "press before UI disappears");
    frame(false);
    require(!held && editor.gameHourScrubId == 0, "a removed slider releases its lock while mouse remains down");
    io.AddMouseButtonEvent(0, false);
    frame();
    io.AddMouseButtonEvent(0, true);
    frame();
    require(held, "press before rendering is skipped");
    editor.FinishGameHourSliderFrame(false);
    require(!held && editor.gameHourScrubId == 0, "closing all UI releases the lock even without another ImGui frame");
    io.AddMouseButtonEvent(0, false);
    frame();

    const int previousBegins = begins;
    io.AddMouseButtonEvent(0, true);
    frame(true, true);
    require(!held && begins == previousBegins, "disabled sliders do not start weather locks");
    io.AddMouseButtonEvent(0, false);
    frame();
    ImGui::DestroyContext();
}
'''
        source = source.replace("DRAW_SLIDER", braced(editor, "bool EditorWindow::DrawGameHourSlider("))
        source = source.replace("FINISH_FRAME", braced(editor, "void EditorWindow::FinishGameHourSliderFrame("))
        runtime.SceneSettingsRuntimeTests.compile_and_run(self, source, imgui_root=library_root)

    def test_shared_weather_time_preview_lifecycle(self):
        header = braced((ROOT / "src/Utils/Game.h").read_text(encoding="utf-8"),
                        "namespace Util::EnvironmentControls")
        implementation = braced((ROOT / "src/Utils/Game.cpp").read_text(encoding="utf-8"),
                                "namespace Util::EnvironmentControls")
        editor = (ROOT / "src/CSEditor/EditorWindow.cpp").read_text(encoding="utf-8")
        weather_hooks = "\n".join(braced(editor, declaration) for declaration in (
            "RE::TESWeather* GetActiveLock(", "void ReapplyLock(",
            "void SetWeatherThunk(", "void ForceWeatherThunk("))
        source = r'''
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
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
    TESWeather* lastWeather = nullptr;
    float currentWeatherPct = 1.0f;
    TESWeather* overrideWeather = nullptr;
    TESWeather* defaultWeather = nullptr;
    int forces = 0, sets = 0, releases = 0;
    void ForceWeather(TESWeather* w, bool override) {
        ++forces;
        currentWeather = w;
        currentWeatherPct = 1.0f;
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
namespace WeatherHooks {
WEATHER_HOOKS
}
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

    require(StartPreview(nullptr, 5.0f), "Time-only Play at Dawn");
    require(GetLockedWeather() == &first && scale.value == 0 && hour.value == 5,
            "Time-only Play locks the current weather and hour");
    int timeOnlyForces = sky.forces, timeOnlyReleases = sky.releases;
    require(StartPreview(nullptr, 7.0f), "Time-only switch to Sunrise");
    require(GetLockedWeather() == &first && sky.forces == timeOnlyForces && sky.releases == timeOnlyReleases,
            "Changing only the period neither changes nor releases weather");
    sky.currentWeather = &third;
    require(StartPreview(nullptr, 12.0f), "Time-only switch during engine weather drift");
    require(GetLockedWeather() == &first && sky.currentWeather == &first && hour.value == 12,
            "Period changes retain the original lock instead of capturing weather drift");
    StopPreview();
    require(!GetLockedWeather() && scale.value == 37, "Time-only Stop restores normal controls");
    hour.value = 9;

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

    require(StartPreview(&second, 5.0f), "Play weather at Dawn");
    int retargetReleases = sky.releases;
    const int retargetForces = sky.forces;
    require(StartPreview(&second, 7.0f), "Select Sunrise without Stop");
    require(IsPreviewActive() && GetLockedWeather() == &second && hour.value == 7 && scale.value == 0,
            "Sunrise updates the running weather/TOD preview together");
    require(sky.releases == retargetReleases && sky.forces == retargetForces,
            "Period change does not release or re-force the same weather");
    require(StartPreview(&third, 7.0f), "Select another weather while playing");
    require(IsPreviewActive() && GetLockedWeather() == &third && sky.currentWeather == &third && scale.value == 0,
            "Weather change updates the existing lock");
    require(sky.releases == retargetReleases, "Weather retarget does not briefly release the override");
    require(StartPreview(&third, 0.5f), "Retarget across midnight");
    StopPreview();
    require(!GetLockedWeather() && scale.value == 37 && hour.value == 0.5f,
            "Stop after multiple retargets restores the original controls, not the last preview");

    SetLockedWeather(&first);
    PauseTime();
    require(StartPreview(&second, 5.0f) && StartPreview(&third, 7.0f), "Retarget over earlier editor locks");
    StopPreview();
    require(GetLockedWeather() == &first && IsTimePaused(), "Retarget preserves pre-Play editor locks");
    ResumeTime();
    SetLockedWeather(nullptr);
    require(scale.value == 37, "Retarget preserves the editor's original resume timescale");

    require(StartPreview(&second, 5.0f) && StartPreview(&second, {}), "Switch TOD preview to normal weather");
    require(IsPreviewActive() && GetLockedWeather() == &second && scale.value == 37,
            "Dropping TOD resumes time while retaining the weather lock");
    require(StartPreview(&second, 7.0f) && StartPreview(nullptr, 12.5f), "Switch weather TOD to general TOD");
    require(IsPreviewActive() && GetLockedWeather() == &second && scale.value == 0 && hour.value == 12.5f,
            "General TOD keeps the selected weather frozen");
    StopPreview();
    require(scale.value == 37, "Stop after adding/removing lock components restores normal time");

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
    const int takeoverReleases = sky.releases, takeoverForces = sky.forces;
    BeginGameHourScrub();
    SetGameHour(7.0f);
    require(!IsPreviewActive() && hour.value == 7 && GetLockedWeather() == &second && scale.value == 37,
            "hour slider takes over and retains the currently previewed weather");
    require(sky.releases == takeoverReleases && sky.forces == takeoverForces,
            "slider takeover never briefly restores a different weather");
    EndGameHourScrub();
    require(GetLockedWeather() == &third, "slider release restores the pre-preview editor lock");
    require(StartPreview(nullptr, 3.0f), "time preview before speed edit");
    SetTimeScale(15);
    require(!IsPreviewActive() && scale.value == 15, "speed control takes over");
    PauseTime();
    SetTimeScale(22);
    require(IsTimePaused() && scale.value == 0 && GetSavedTimeScale() == 22, "speed edit respects editor pause");
    ResetTimeScale();
    ResumeTime();
    require(scale.value == kDefaultTimeScale, "reset speed uses shared default");

    SetLockedWeather(&third);
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

    const int sliderJumps = Util::timeJumps;
    BeginGameHourScrub();
    require(GetLockedWeather() == &first, "pressing the slider locks weather before any value change");
    BeginGameHourScrub();
    require(SetGameHour(7.0f, false), "first slider change");
    require(GetLockedWeather() == &first && scale.value == 20 && !IsTimePaused() && !IsPreviewActive(),
            "slider freezes current weather without pausing time or starting toolbar playback");
    sky.currentWeather = &third;
    MaintainLocks();
    require(SetGameHour(8.0f, false) && sky.currentWeather == &first, "scrubbing retains weather despite drift");
    const int sliderForces = sky.forces, sliderReleases = sky.releases;
    require(SetGameHour(9.0f, false) && Util::timeJumps == sliderJumps,
            "scrubbing still defers celestial synchronization to the shared UI throttle");
    require(sky.forces == sliderForces && sky.releases == sliderReleases,
            "steady scrubbing avoids redundant weather forces and releases");
    require(SetGameHour(10.0f) && Util::timeJumps == sliderJumps + 1, "non-deferred hour change synchronizes once");
    PauseTime();
    require(SetGameHour(11.0f, false) && IsTimePaused() && scale.value == 0 && GetSavedTimeScale() == 20,
            "slider retains an existing time pause and resume speed");
    ResumeTime();
    EndGameHourScrub();
    require(!GetLockedWeather(), "releasing the slider releases its weather lock");
    releases = sky.releases;
    EndGameHourScrub();
    require(sky.releases == releases, "ending an interaction twice has no effect");
    require(SetGameHour(9.0f) && !GetLockedWeather(), "a direct time command never starts a weather lock");

    for (float blend : {0.0f, 0.49f, 0.5f, 0.51f, 1.0f}) {
        sky.lastWeather = &first;
        sky.currentWeather = &second;
        sky.currentWeatherPct = blend;
        auto* expected = blend > 0.5f ? &second : &first;
        BeginGameHourScrub();
        require(GetLockedWeather() == expected && sky.currentWeather == expected && sky.currentWeatherPct == 1.0f,
                "press settles on incoming weather only above the halfway point");
        EndGameHourScrub();
        require(!GetLockedWeather(), "release without a value change unlocks transition-selected weather");
    }
    sky.lastWeather = nullptr;
    sky.currentWeatherPct = 0.1f;
    BeginGameHourScrub();
    require(GetLockedWeather() == &second, "missing previous weather falls back to current weather");
    EndGameHourScrub();

    SetLockedWeather(&third);
    BeginGameHourScrub();
    EndGameHourScrub();
    require(GetLockedWeather() == &third, "a prior manual weather lock survives a slider click");
    SetLockedWeather(nullptr);
    require(StartPreview(&second, 7.0f), "toolbar preview before slider click");
    BeginGameHourScrub();
    EndGameHourScrub();
    require(!GetLockedWeather() && !IsPreviewActive(), "slider takeover never leaves a preview weather locked");

    BeginGameHourScrub();
    ChangeWeather(&third, false);
    EndGameHourScrub();
    require(!GetLockedWeather() && sky.currentWeather == &third,
            "explicit weather selection supersedes a slider without retaining its temporary lock");
    BeginGameHourScrub();
    require(StartPreview(&second, 12.0f), "toolbar playback takes over from a slider");
    EndGameHourScrub();
    require(GetLockedWeather() == &second && IsPreviewActive(), "late slider release cannot clear a new preview");
    StopPreview();
    BeginGameHourScrub();
    SetTimeRunningForMenu(true);
    require(!GetLockedWeather(), "loading cancels a held slider's temporary lock");
    BeginGameHourScrub();
    require(!GetLockedWeather(), "loading refuses a new slider weather lock");
    SetTimeRunningForMenu(false);

    sky.currentWeather = &first;
    BeginGameHourScrub();
    WeatherHooks::ForceWeatherThunk(&sky, &third, false);
    WeatherHooks::SetWeatherThunk(&sky, &second, false, false);
    require(sky.currentWeather == &first, "weather call-site guards hold weather during the press");
    EndGameHourScrub();
    WeatherHooks::ForceWeatherThunk(&sky, &third, false);
    require(sky.currentWeather == &third, "force-weather calls work after slider release");
    WeatherHooks::SetWeatherThunk(&sky, &second, false, false);
    require(sky.currentWeather == &second, "set-weather calls work after slider release");

    require(StartPreview(&second, 6.0f), "valid preview before invalid input");
    require(!StartPreview(&third, 24.0f) && !StartPreview(&third, -1.0f) &&
            !StartPreview(&third, std::numeric_limits<float>::quiet_NaN()), "invalid hours refused");
    require(IsPreviewActive() && GetLockedWeather() == &second && hour.value == 6,
            "invalid input does not disturb existing preview");
    require(!SetGameHour(24.0f), "manual invalid hour refused");
    require(IsPreviewActive() && GetLockedWeather() == &second && hour.value == 6,
            "invalid slider input cannot take over preview or change the lock");
    StopPreview();
    require(!IsPreviewActive() && scale.value == 20, "final Stop restores time");

    std::thread menuEvents([] {
        for (int i = 0; i < 2000; ++i) {
            SetTimeRunningForMenu(true);
            SetTimeRunningForMenu(false);
        }
    });
    for (int i = 0; i < 2000; ++i) {
        StartPreview(&second, 6.0f);
        MaintainLocks();
        IsTimePaused();
        GetSavedTimeScale();
        IsPreviewActive();
        StopPreview();
    }
    menuEvents.join();
    SetTimeRunningForMenu(false);
    StopPreview();
    ResumeTime();
    require(!IsPreviewActive() && !GetLockedWeather() && !IsTimePaused() && scale.value == 20,
            "Concurrent menu events and preview frames leave no stale pause or weather lock");
}
'''
        source = source.replace("HEADER", header).replace("IMPLEMENTATION", implementation)
        source = source.replace("WEATHER_HOOKS", weather_hooks)
        runtime.SceneSettingsRuntimeTests.compile_and_run(self, source)

    def test_environment_state_access_is_synchronized(self):
        source = (ROOT / "src/Utils/Game.cpp").read_text(encoding="utf-8")
        header = braced((ROOT / "src/Utils/Game.h").read_text(encoding="utf-8"),
                        "namespace Util::EnvironmentControls")
        environment = braced(source, "namespace Util::EnvironmentControls")
        atomic_accessors = {"SetWeatherLockAvailable", "IsWeatherLockAvailable", "GetLockedWeather"}
        for line in header.splitlines():
            if "(" not in line or not line.strip().endswith(";"):
                continue
            declaration = line.strip().split("(", 1)[0]
            if declaration.split()[-1] in atomic_accessors:
                continue
            with self.subTest(function=declaration):
                body = braced(environment, declaration + "(")
                self.assertIn("std::scoped_lock lock(environmentMutex);", body.split("\n", 3)[2])

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
        self.assertIn('Util::ErrorTextButton("X"', editor)
        self.assertIn("Util::ErrorTextButton(", braced(scene, "static bool DrawPreviewButton("))
        self.assertIn("Util::EnvironmentControls::SetGameHour(gameHour, false)",
                      braced(editor, "bool EditorWindow::DrawGameHourSlider("))
        self.assertIn("DrawGameHourSlider(", braced(editor, "void EditorWindow::DrawTimeControls("))
        self.assertIn('DrawGameHourSlider("##MenuBarSlider"', editor)
        overlay = (ROOT / "src/Menu/OverlayRenderer.cpp").read_text(encoding="utf-8")
        self.assertIn("FinishGameHourSliderFrame(false)", overlay)
        self.assertIn("FinishGameHourSliderFrame(true)", overlay)
        selector = (ROOT / "src/Features/SceneSelector.cpp").read_text(encoding="utf-8")
        self.assertIn("->DrawTimeControls()", braced(selector, "void SceneSelector::DrawTimeControls("))
        self.assertIn("if (!Util::EnvironmentControls::IsPreviewActive() || !SetFeaturePagePreviewPlaying(true))", scene)
        menu = (ROOT / "src/Menu/FeatureListRenderer.cpp").read_text(encoding="utf-8")
        flyout = braced(menu, "if (flyout) {")
        self.assertLess(flyout.index('"menu.features.enable_at_boot"'), flyout.index('"menu.features.add_to_favorites"'))
        self.assertLess(flyout.index('"menu.features.add_to_favorites"'), flyout.index('"feature.scene_manager.name"'))
        self.assertFalse((ROOT / "src/Utils/EnvironmentControls.cpp").exists())
        self.assertFalse((ROOT / "src/Utils/EnvironmentControls.h").exists())


if __name__ == "__main__":
    unittest.main()
