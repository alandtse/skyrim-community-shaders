import importlib.util
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).parents[1]
POLICY_PATH = ROOT / "src" / "SceneSettingsPolicy.h"
MANAGER_PATH = ROOT / "src" / "SceneSettingsManager.cpp"
GENERATOR_PATH = ROOT / "cmake" / "generate_scene_settings_catalog.py"

SPEC = importlib.util.spec_from_file_location("scene_catalog_generator", GENERATOR_PATH)
GENERATOR = importlib.util.module_from_spec(SPEC)
assert SPEC.loader
SPEC.loader.exec_module(GENERATOR)


def extract_initializer(source: str, name: str) -> str:
    declaration = re.search(rf"\b{re.escape(name)}\s*=", source)
    if not declaration:
        raise AssertionError(f"Could not find {name}")
    start = source.find("{", declaration.end())
    end = GENERATOR.find_matching_brace(source, start)
    if start < 0 or end < 0:
        raise AssertionError(f"Could not parse {name}")
    return source[start + 1:end]


def extract_function(source: str, name: str) -> str:
    declaration = re.search(
        rf"SceneSettingsManager::{re.escape(name)}\s*\(", source)
    if not declaration:
        raise AssertionError(f"Could not find {name}")
    start = source.find("{", declaration.end())
    end = GENERATOR.find_matching_brace(source, start)
    if start < 0 or end < 0:
        raise AssertionError(f"Could not parse {name}")
    return source[start + 1:end]


def extract_braced_rows(source: str) -> list[str]:
    rows = []
    position = 0
    while position < len(source):
        start = source.find("{", position)
        if start < 0:
            break
        end = GENERATOR.find_matching_brace(source, start)
        if end < 0:
            raise AssertionError("Unbalanced policy initializer")
        rows.append(source[start + 1:end])
        position = end + 1
    return rows


def extract_paths(source: str, name: str) -> list[tuple[str, ...]]:
    return [
        tuple(re.findall(r'"([^"]+)"', row))
        for row in extract_braced_rows(extract_initializer(source, name))
    ]


def normalize_address_token(token: str) -> str:
    return "".join(GENERATOR.prettify(token).split()).casefold()


def decode_catalog_path(path: str) -> list[str]:
    return [
        part.replace("~1", "/").replace("~0", "~")
        for part in path.split("/")
        if part
    ]


def catalog_address(entry: dict[str, object]) -> tuple[str, ...]:
    path = [
        part for part in decode_catalog_path(str(entry["path"]))
        if part.casefold() != "settings"
    ]
    return (str(entry["feature"]), *path, str(entry["key"]))


def normalize_path(path: tuple[str, ...]) -> tuple[str, ...]:
    return tuple(normalize_address_token(token) for token in path)


def is_prefix(prefix: tuple[str, ...], address: tuple[str, ...]) -> bool:
    return len(prefix) <= len(address) and address[:len(prefix)] == prefix


class SceneSettingsPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.policy = POLICY_PATH.read_text(encoding="utf-8")
        cls.manager = MANAGER_PATH.read_text(encoding="utf-8")
        cls.entries = GENERATOR.build_entries(ROOT)
        cls.addresses = [normalize_path(catalog_address(entry)) for entry in cls.entries]
        cls.blacklist = extract_paths(cls.policy, "kSettingBlacklist")
        cls.location_paths = extract_paths(
            cls.policy, "kLocationFeatureWhitelist")
        cls.location_features = {path[0] for path in cls.location_paths}
        cls.time_paths = extract_paths(
            cls.policy, "kTimeOfDayFeatureWhitelist")
        cls.time_features = {path[0] for path in cls.time_paths}

    def test_policy_collections_are_nonempty_and_unique(self):
        self.assertTrue(self.blacklist)
        self.assertTrue(self.location_paths)
        self.assertTrue(self.time_paths)
        self.assertTrue(self.location_features)
        self.assertTrue(self.time_features)
        self.assertEqual(len(self.blacklist), len(set(self.blacklist)))
        self.assertEqual(len(self.location_paths), len(set(self.location_paths)))
        self.assertEqual(len(self.time_paths), len(set(self.time_paths)))
        self.assertTrue(all(self.blacklist))
        self.assertTrue(all(self.location_paths))
        self.assertTrue(all(self.time_paths))

    def test_every_policy_feature_is_discovered(self):
        discovered = {entry["feature"] for entry in self.entries}
        policy_features = {
            *self.location_features,
            *self.time_features,
        }
        self.assertTrue(policy_features <= discovered)

    def test_every_blacklist_prefix_matches_catalogued_settings(self):
        for path in self.blacklist:
            prefix = normalize_path(path)
            with self.subTest(path=path):
                self.assertTrue(any(is_prefix(prefix, address)
                                    for address in self.addresses))

    def test_location_whitelist_prefixes_resolve_and_restrict(self):
        for feature in self.location_features:
            normalized_feature = normalize_address_token(feature)
            feature_addresses = [
                address for address in self.addresses
                if address[0] == normalized_feature
            ]
            self.assertTrue(feature_addresses)

            prefixes = [
                normalize_path(path) for path in self.location_paths
                if path[0] == feature
            ]
            for prefix in prefixes:
                with self.subTest(feature=feature, prefix=prefix):
                    self.assertTrue(any(is_prefix(prefix, address)
                                        for address in feature_addresses))
            if all(len(prefix) > 1 for prefix in prefixes):
                self.assertTrue(any(
                    not any(is_prefix(prefix, address) for prefix in prefixes)
                    for address in feature_addresses
                ))

    def test_time_of_day_whitelist_prefixes_resolve(self):
        for path in self.time_paths:
            prefix = normalize_path(path)
            with self.subTest(path=path):
                self.assertTrue(any(is_prefix(prefix, address)
                                    for address in self.addresses))

    def test_literal_debug_sections_are_blacklisted(self):
        blacklist = [normalize_path(path) for path in self.blacklist]
        debug_entries = [
            entry for entry in self.entries
            if any(normalize_address_token(part) == "debug"
                   for part in decode_catalog_path(str(entry["displayPath"])))
        ]
        self.assertTrue(debug_entries)
        for entry in debug_entries:
            address = normalize_path(catalog_address(entry))
            with self.subTest(address=address):
                self.assertTrue(any(is_prefix(prefix, address)
                                    for prefix in blacklist))

    def test_manager_consumes_every_policy_collection(self):
        for name in (
                "kSettingBlacklist",
                "kLocationFeatureWhitelist",
                "kTimeOfDayFeatureWhitelist"):
            self.assertIn(f"SceneSettingsPolicy::{name}", self.manager)

    def test_feature_context_delete_is_scoped_and_transactional(self):
        body = extract_function(self.manager, "DeleteFeatureSceneSettings")
        self.assertIn("entry.source == EntrySource::User", body)
        self.assertIn("entry.featureShortName == featureShortName", body)
        self.assertIn("EntryBelongsToContext(entry, context)", body)
        self.assertIn("CommitSceneSettingChanges();", body)
        self.assertIn("PrepareWeatherUserSettingsMutation", body)
        self.assertIn("PrepareLocationUserSettingsMutation", body)

    def test_feature_edit_preview_preserves_overwrite_precedence(self):
        body = extract_function(self.manager, "ApplyFeatureSceneEditPreview")
        self.assertGreaterEqual(body.count("EntrySource::Overwrite"), 4)
        self.assertIn("ResolveExteriorSettings(", body)
        self.assertIn(
            "overlayLocationEntries(EntrySource::Overwrite, preview)", body)
        self.assertNotIn(
            "for (const auto& [address, value] : edit.workingOverrides)\n"
            "\t\tresolved[address] = value;",
            body,
        )

    def test_menu_and_page_navigation_preserve_feature_drafts(self):
        body = extract_function(self.manager, "Update")
        self.assertNotIn("EndFeatureSceneEdit", body)
        self.assertNotIn("StoreFeatureSceneEdit", body)
        renderer = (ROOT / "src/Menu/FeatureListRenderer.cpp").read_text(encoding="utf-8")
        self.assertNotIn("EndFeaturePageEditing", renderer)
        self.assertNotIn("EndFeatureSceneEdit", renderer)

    def test_overwrite_lock_is_shared_by_controls_and_capture(self):
        hooks = (ROOT / "src/SceneSettingsUIHooks.cpp").read_text(encoding="utf-8")
        start = hooks.index("void RefreshBlockedFeatureSceneEditSettings(")
        end = GENERATOR.find_matching_brace(hooks, hooks.index("{", start))
        self.assertIn("!manager->IsFeatureSceneEditSettingOverwritten(",
                      hooks[start:end])
        self.assertIn("!IsFeatureSceneEditSettingOverwritten(",
                      extract_function(self.manager, "CaptureFeatureSceneEditChanges"))

    def test_toolbar_save_requires_pending_edits(self):
        ui = (ROOT / "src/CSEditor/SceneSettingsUI.cpp").read_text(encoding="utf-8")
        self.assertIn("ImGui::BeginDisabled(!state.activeContext || !manager->HasPendingFeatureSceneEdits());", ui)

    def test_toolbar_actions_respect_overwrites_and_loading(self):
        ui = (ROOT / "src/CSEditor/SceneSettingsUI.cpp").read_text(encoding="utf-8")
        toolbar = ui[ui.index("bool DrawFeaturePageControls("):ui.index("// Core add-setting dialog:")]
        for column in (4, 5, 6):
            block = toolbar.split(f"ImGui::TableSetColumnIndex({column});", 1)[1].split("ImGui::EndDisabled();", 1)[0]
            self.assertIn("manager->AreFeatureSceneEditActionsLocked()", block)
        self.assertIn("Util::DisableGuard(!manager->IsSceneReady())", toolbar)
        self.assertIn("state.deleteSettings.Draw() && requestedContext && !manager->AreFeatureSceneEditActionsLocked()", toolbar)
        self.assertIn("state.compatibleCount == 0 || (selectSettings && manager->AreFeatureSceneEditActionsLocked())", ui)
        renderer = (ROOT / "src/Menu/FeatureListRenderer.cpp").read_text(encoding="utf-8")
        self.assertIn("const bool sceneEditing = sceneManager->IsFeatureSceneEditing(featureName);", renderer)

    def test_copy_choices_and_execution_share_compatibility_policy(self):
        choices = extract_function(self.manager, "GetCopySourceSettings")
        execution = extract_function(self.manager, "BuildCopyCandidates")
        self.assertIn("IsCopyEntryCompatible(entry, *destinationType)", choices)
        self.assertIn("incompatibleGroups.insert(group)", choices)
        self.assertIn("logicalSettings.erase(group)", choices)
        self.assertIn("IsCopyEntryCompatible(*entry, destination.type)", execution)

    def test_copy_initializes_empty_scenes_and_preserves_configured_modes(self):
        body = extract_function(self.manager, "CopySettings")
        self.assertNotIn("SetSceneTimeOfDayEnabled", body)
        mode = body.index("if (destinationConfig && destinationEntries->empty())")
        self.assertGreater(mode, body.index("if (pending.empty())"))
        self.assertLess(mode, body.index("for (auto& copy : pending)"))
        self.assertIn("destinationConfig->timeOfDayEnabled = destination.allPeriods || destination.period != TimeOfDayPeriod::Count", body)
        candidates = extract_function(self.manager, "BuildCopyCandidates")
        self.assertIn("sourceValue.destinationPeriod", candidates)
        self.assertNotIn("IsWholePeriodContext", self.manager)

    def test_mode_change_does_not_convert_saved_entries(self):
        body = extract_function(self.manager, "SetSceneTimeOfDayEnabled")
        self.assertIn("config->timeOfDayEnabled = enabled", body)
        self.assertIn("SaveAllUserSettings()", body)
        self.assertNotIn("entries", body)
        for name in ("BuildWeatherValueGroups", "ResolveLocationSettings"):
            self.assertIn("IsPeriodActive(entry.period)",
                          extract_function(self.manager, name))
        loading = extract_function(self.manager, "LoadWeatherUserSettings")
        self.assertIn('"timeOfDayEnabled"', loading)

    def test_bulk_copy_can_commit_once_after_all_destinations(self):
        body = extract_function(self.manager, "CopySettings")
        self.assertIn("if (!deferCommit)\n\t\tCommitSceneSettingChanges();", body)
        ui = (ROOT / "src/CSEditor/SceneSettingsUI.cpp").read_text(encoding="utf-8")
        self.assertIn("state.policy, selected, true).Changed()", ui)
        self.assertIn("if (copied)\n\t\t\t\tmanager->CommitSceneSettingChanges();", ui)

    def test_bulk_operations_filter_every_scene_kind(self):
        for name in ("GetEntryLayerSummary", "SetEntryLayerPaused",
                     "DeleteEntryLayer", "ExportEntryLayerToOverwrites"):
            body = extract_function(self.manager, name)
            with self.subTest(operation=name):
                for kind in ("Interior", "TimeOfDay", "Weather", "Location"):
                    self.assertIn(f"SceneContextType::{kind}", body)
                self.assertIn("scope", body)

    def test_feature_copy_does_not_implicitly_save(self):
        ui = (ROOT / "src/CSEditor/SceneSettingsUI.cpp").read_text(encoding="utf-8")
        begin = ui.index('ImGui::TableSetColumnIndex(4);',
                         ui.index("bool DrawFeaturePageControls("))
        end = ui.index('ImGui::TableSetColumnIndex(7);', begin)
        buttons = ui[begin:end]
        self.assertNotIn("StoreFeatureSceneEdit", buttons)
        self.assertIn("!state.hasSavedSettings", buttons)
        self.assertIn("HasPendingFeatureSceneEdits()", buttons)

    def test_toolbar_pause_and_readonly_editor_contracts(self):
        ui = (ROOT / "src/CSEditor/SceneSettingsUI.cpp").read_text(encoding="utf-8")
        toolbar = ui[ui.index("bool DrawFeaturePageControls("):ui.index("static void DrawValueEditorCore(")]
        self.assertNotIn('"##SceneManagerTitle"', toolbar)
        self.assertLess(toolbar.index('"##SceneManagerCopy"'), toolbar.index('"##SceneManagerPause"'))
        self.assertLess(toolbar.index('"##SceneManagerPause"'), toolbar.index('"##SceneManagerDelete"'))
        self.assertIn("state.savedSummary.Empty() || manager->HasPendingFeatureSceneEdits()", toolbar)
        self.assertIn("GetFeatureSceneSummary(featureShortName, *requestedContext)", toolbar)
        self.assertIn("GetScenePickerTextColor(candidate == current, paused)", ui)
        editor = ui[ui.index("static void DrawValueEditorCore("):ui.index("static void DrawValueEditorCore(") + 16000]
        editor = editor[:editor.index("\n\tstatic ", 1)]
        self.assertIn("Util::DisableGuard(readOnly)", editor)
        self.assertNotIn("ImGuiCol_FrameBg", editor)
        store = extract_function(self.manager, "StoreFeatureSceneEdit")
        self.assertNotIn("existing->paused = false", store)

    def test_target_management_filters_weather_and_location(self):
        for name in ("GetEntryLayerSummary", "SetEntryLayerPaused",
                     "DeleteEntryLayer", "ExportEntryLayerToOverwrites"):
            body = extract_function(self.manager, name)
            with self.subTest(operation=name):
                self.assertIn("scope = target->type", body)
                self.assertIn("target->weatherId", body)
                self.assertIn("target->locationType", body)
                self.assertIn("target->locationFormKey", body)
        deletion = extract_function(self.manager, "DeleteEntryLayer")
        self.assertIn("DeleteAllWeatherUserSettings(target->weatherId)", deletion)
        self.assertIn("DeleteAllLocationUserSettings(target->locationType, target->locationFormKey)", deletion)

    def test_copy_popup_supports_all_periods_and_location_selection(self):
        ui = (ROOT / "src/CSEditor/SceneSettingsUI.cpp").read_text(encoding="utf-8")
        self.assertIn('DrawCopyPeriodPicker("##CopyDestinationScenePeriod", state.target.period, true)', ui)
        self.assertIn('DrawCopyPeriodPicker("##CopyDestinationPeriod", state.target.period, true)', ui)
        self.assertIn("timeOfDay && state.target.period == Period::Count", ui)
        self.assertIn("context.period = static_cast<Period>(index)", ui)
        self.assertIn("for (const auto& [type, formKey] : state.locations)", ui)
        self.assertIn("state.locations.insert(identity)", ui)
        self.assertIn("state.locations.erase(identity)", ui)

    def test_copy_popup_uses_feature_bounds_and_close_button(self):
        ui = (ROOT / "src/CSEditor/SceneSettingsUI.cpp").read_text(encoding="utf-8")
        self.assertIn("s_featurePageCenter = ImGui::GetCurrentWindow()->Rect().GetCenter()", ui)
        self.assertIn("SetNextWindowPos(s_featurePageCenter, ImGuiCond_Always", ui)
        self.assertIn('Util::Popup(id, T("feature.scene_manager.copy.to", "Copy to"))', ui)
        utilities = (ROOT / "src/Utils/UI.cpp").read_text(encoding="utf-8")
        self.assertIn('CloseButton("##ClosePopup", size)', utilities)
        self.assertIn("isOpen = BeginDialogPopup(name, name, p_open, true, flags)", utilities)

    def test_location_pickers_share_weather_status_presentation(self):
        ui = (ROOT / "src/CSEditor/SceneSettingsUI.cpp").read_text(encoding="utf-8")
        for name in ("RefreshFeatureLocationPickerCache", "RefreshLocationTargetPickerCache"):
            start = ui.index(f"static void {name}(")
            body = ui[start:ui.index("\n\tstatic ", start + 1)]
            with self.subTest(picker=name):
                self.assertIn("GetScenePickerPresentation(GetLocationTargetLabel(target), entries,", body)
                self.assertIn(".paused = presentation.paused", body)
                self.assertIn("cache.revision == revision", body)
        self.assertIn("DrawScenePickerText(", ui)
        self.assertIn("GetScenePickerTextColor(IsCurrentLocation(target), entry.paused", ui)
        self.assertNotIn("GetScenePickerColor(", ui)

    def test_management_buttons_overlay_full_scene_rows(self):
        ui = (ROOT / "src/CSEditor/SceneSettingsUI.cpp").read_text(encoding="utf-8")
        start = ui.index("static bool DrawSceneTargetSelectable(")
        body = ui[start:ui.index("\n\tstatic ", start + 1)]
        self.assertIn("ImGuiSelectableFlags_AllowOverlap", body)
        self.assertIn("right - buttonWidth - ImGui::GetStyle().ItemInnerSpacing.x", body)
        self.assertLess(body.index("ImGui::Selectable("), body.index("ImGui::SmallButton("))
        self.assertIn("DrawTargetManagementPopup(*target, label, manage)", body)
        self.assertIn("ImGuiSelectableFlags_NoAutoClosePopups", body)
        self.assertIn("pressed && !manageHovered", body)

    def test_interior_source_is_visible_and_bulk_management_is_not_centered(self):
        ui = (ROOT / "src/CSEditor/SceneSettingsUI.cpp").read_text(encoding="utf-8")
        start = ui.index("void DrawCopyPanel()")
        body = ui[start:ui.index("struct FeaturePageEditorState", start)]
        self.assertIn('ImGui::TextUnformatted(T("feature.scene_manager.tab.interior", "Interior"))', body)
        start = ui.index("void DrawGlobalActions(")
        body = ui[start:ui.index("static AddSettingState", start)]
        self.assertNotIn("SetNextWindowPos", body)

    def test_float_table_controls_match_scene_mode(self):
        ui = (ROOT / "src/CSEditor/SceneSettingsUI.cpp").read_text(encoding="utf-8")
        self.assertIn("const bool changed = useFullWidthSliders && hasBounds ?", ui)
        self.assertIn("const bool unifiedChanged = showExpandedAggregateControls && hasBounds[0] ?", ui)
        self.assertIn("changed |= showExpandedAggregateControls && hasBounds[component] ?", ui)
        for widget in ('"##val", &val', '"##all", &unifiedValue', '"##val", &values[component]'):
            with self.subTest(widget=widget):
                self.assertIn(f"ImGui::SliderFloat({widget}", ui)
                self.assertIn(f"ImGui::DragFloat({widget}", ui)
        for widget in ('"##val"', '"##all"', '"##locationTransitionSeconds"'):
            self.assertIn(f"ImGui::DragFloat({widget}", ui)
        self.assertIn("DrawValueEditorCore(entry, inputWidth, type == SceneType::InteriorOnly, readOnly", ui)
        self.assertIn("DrawAggregateValueEditorCore(entries, indices, inputWidth, type == SceneType::InteriorOnly, readOnly", ui)
        start = ui.index("static void DrawLocationValueEditor(")
        location = ui[start:ui.index("static void DrawLocationTransitionEditor(", start)]
        self.assertIn("DrawValueEditorCore(entry, inputWidth, entry.period == Period::Count, readOnly", location)
        self.assertIn("const bool useFullWidthSliders = entry.period == Period::Count", location)
        self.assertIn("DrawAggregateValueEditorCore(entries, indices, inputWidth, useFullWidthSliders, readOnly", location)
        self.assertIn("DrawValueEditorCore(entry, inputWidth, useFullWidthSliders, readOnly", location)
        start = ui.index("void DrawWeatherValueEditor(")
        weather = ui[start:ui.index("void DrawPopups(", start)]
        self.assertIn("DrawValueEditorCore(entry, inputWidth, false, readOnly", weather)
        self.assertIn("DrawAggregateValueEditorCore(entries, indices, inputWidth, false, readOnly", weather)

    def test_transition_column_is_drawn_after_both_value_layouts(self):
        ui = (ROOT / "src/CSEditor/SceneSettingsUI.cpp").read_text(encoding="utf-8")
        start = ui.index("// Single-column: collapsed view")
        opening = ui.rfind("{", 0, start)
        end = GENERATOR.find_matching_brace(ui, opening)
        auxiliary = ui.index("if (hasAuxiliaryColumn && ImGui::TableSetColumnIndex(auxiliaryColumn))", start)
        self.assertGreater(auxiliary, end)
        panel = ui[ui.index("void DrawLocationPanel("):]
        self.assertGreater(panel.index("const auto& entries = manager->GetLocationConfig"),
                           panel.index("DrawLocationAddDialog(selectedTarget"))
        self.assertGreater(panel.index("const bool hasTransitionEntries"),
                           panel.index("DrawLocationAddDialog(selectedTarget"))

    def test_location_durations_have_no_mutable_global_fallback(self):
        header = (ROOT / "src/SceneSettingsManager.h").read_text(encoding="utf-8")
        self.assertNotIn("GetLocationTransitionSeconds", header)
        self.assertNotIn("SetLocationTransitionSeconds", header)
        self.assertNotIn("locationTransitionModified", self.manager)
        self.assertIn('locationObj.erase("transitionSeconds")', self.manager)
        resolver = extract_function(self.manager, "ResolveLocationSettings")
        self.assertIn("entry.transitionSeconds.value_or(kDefaultLocationTransitionSeconds)", resolver)
        ui = (ROOT / "src/CSEditor/SceneSettingsUI.cpp").read_text(encoding="utf-8")
        self.assertNotIn("locationTransitionOverride", ui)

    def test_scene_popups_use_shared_utilities_and_target_name(self):
        ui = (ROOT / "src/CSEditor/SceneSettingsUI.cpp").read_text(encoding="utf-8")
        self.assertNotIn("ImGuiStyleVar_PopupRounding", ui)
        self.assertNotIn("DrawScenePopupHeading", ui)
        self.assertIn('Util::Popup("##ManageSceneTarget", state.targetLabel.c_str())', ui)
        self.assertIn('Util::Popup("##ManageSceneSettings", label)', ui)
        utilities = (ROOT / "src/Utils/UI.cpp").read_text(encoding="utf-8")
        self.assertIn("isOpen(BeginDialogPopup(id, title, nullptr, false, flags))", utilities)
        self.assertIn("isOpen = BeginDialogPopup(name, name, p_open, true, flags)", utilities)

    def test_weather_mode_is_chosen_on_selection_not_copy_execution(self):
        ui = (ROOT / "src/CSEditor/SceneSettingsUI.cpp").read_text(encoding="utf-8")
        self.assertIn("state.sourceTimeOfDay = GetWeatherSelectionTimeOfDay(state.sourceWeatherId)", ui)
        self.assertIn("manager->SetWeatherShowTimeOfDay(s_selectedWeatherId, GetWeatherSelectionTimeOfDay(s_selectedWeatherId))", ui)
        self.assertIn("const bool weatherChanged = selected || target != previousTarget", ui)
        copy = extract_function(self.manager, "CopySettings")
        self.assertNotIn("SetWeatherShowTimeOfDay", copy)
        self.assertNotIn("GetWeatherSelectionTimeOfDay", copy)

    def test_weather_tab_defaults_only_to_current_weather(self):
        ui = (ROOT / "src/CSEditor/SceneSettingsUI.cpp").read_text(encoding="utf-8")
        start = ui.index("static bool DrawCopyWeatherPicker(")
        picker = ui[start:ui.index("static std::vector<RE::TESWeather*> GetCopySourceWeatherTargets(", start)]
        self.assertIn("bool selectDefault = true", picker)
        opening = picker.index("{", picker.index("if (selectDefault)"))
        end = GENERATOR.find_matching_brace(picker, opening)
        self.assertIn("selectedWeather = weatherTargets.begin()", picker[opening:end])
        self.assertIn("sky->currentWeather", picker[opening:end])
        self.assertNotIn("selectedWeather = weatherTargets.begin()", picker[end:])
        self.assertIn('"Select a weather..."', picker)
        start = ui.index("void DrawWeatherPanel()")
        panel = ui[start:ui.index("static void DrawWeatherPopups(", start)]
        self.assertIn("weatherTargets, s_selectedWeatherId, true, false)", panel)
        self.assertIn("s_selectedWeatherId = 0", panel)
        self.assertIn("s_selectedWeatherId = sky->currentWeather->GetFormID()", panel)
        self.assertIn("s_selectedWeatherId != previousWeatherId", panel)
        self.assertNotIn("weatherTargets.begin()", panel)
        self.assertNotIn("configured", panel)
        self.assertIn('"No weather is selected."', panel)

    def test_copy_destinations_prioritize_current_targets_without_duplicates(self):
        ui = (ROOT / "src/CSEditor/SceneSettingsUI.cpp").read_text(encoding="utf-8")
        start = ui.index("static void DrawCopyWeatherDestinations(")
        weather = ui[start:ui.index("static void DrawCopyLocationDestinations(", start)]
        self.assertIn("globals::game::sky->currentWeather == weather", weather)
        self.assertIn("{ &current, &configured, &available }", weather)
        self.assertLess(weather.index('drawGroup(T("feature.scene_manager.weather.group.current"'),
                        weather.index('drawGroup(T("feature.scene_manager.weather.group.configured"'))
        start = ui.index("static void DrawCopyLocationDestinations(")
        location = ui[start:ui.index("static std::vector<SceneSettingsManager::SceneContextId> GetCopyDestinations(", start)]
        self.assertIn("currentTargets.rbegin()", location)
        self.assertIn("{ &cache.currentIndices, &cache.configuredIndices, &cache.locationTypeIndices, &cache.unconfiguredIndices }", location)
        self.assertIn("groups[group] != &cache.currentIndices && cache.currentMembership[index]", location)
        start = ui.index("static bool IsInactiveCopySet(")
        inactive = ui[start:ui.index("static void DrawCopyWeatherDestinations(", start)]
        self.assertIn("!entries.empty() && manager->IsSceneTimeOfDayEnabled(target) != timeOfDay", inactive)

    def test_expanded_columns_share_interior_width_but_weather_drags_stay_compact(self):
        ui = (ROOT / "src/CSEditor/SceneSettingsUI.cpp").read_text(encoding="utf-8")
        start = ui.index("static SourceTableLayout GetSourceTableLayout(")
        layout = ui[start:ui.index("struct CachedSourceTable", start)]
        self.assertIn("const float regularMinimumWidth = !multiColumn && showExpandedAggregateControls ?", layout)
        self.assertIn("GetExpandedValueColumnWidth()", layout)
        self.assertIn("layout.checkboxOnlyValueColumn ?", layout)
        self.assertNotIn("SCENE_SECTION_HEADER_TARGET_COLS", layout)
        self.assertIn("RefreshSourcePanelCache(s_interiorTableCache, entries, 1, true)", ui)
        self.assertIn("entries, showTimeOfDay ? kPeriodCount : 1, !showTimeOfDay", ui)
        self.assertIn("config.entries, showTod ? kPeriodCount : 1, false, true", ui)

    def test_scene_labels_use_color_and_only_the_enabled_mode_suffix(self):
        ui = (ROOT / "src/CSEditor/SceneSettingsUI.cpp").read_text(encoding="utf-8")
        self.assertIn('T("feature.scene_manager.location.target_label", "{0} ({1})")', ui)
        self.assertIn("targetTypeName = target.locationTypes.back();", ui)
        self.assertNotIn('targetTypeName += ", "', ui)
        self.assertNotIn("MarkCurrentScene", ui)
        self.assertNotIn("MarkInactiveCopySet", ui)
        self.assertNotIn("remaining.ends_with", ui)
        start = ui.index("static ScenePickerPresentation GetScenePickerPresentation(")
        presentation = ui[start:ui.index("static void DrawScenePickerText(", start)]
        self.assertIn("if (timeOfDay)", presentation)
        self.assertIn('T("feature.scene_manager.mode.time_of_day_short", "TOD")', presentation)
        self.assertNotIn("Normal", presentation)
        self.assertNotIn("Paused", presentation)
        start = ui.index("static ImVec4 GetScenePickerTextColor(")
        colors = ui[start:ui.index("static bool IsCurrentLocation(", start)]
        self.assertLess(colors.index("if (paused)"), colors.index("if (current)"))
        self.assertIn("Util::Colors::GetWarning()", colors)
        self.assertIn("Util::Colors::GetSuccess()", colors)
        self.assertIn("target.period, true, GetCopySourceTypeLabel(target.type)", ui)
        self.assertIn("ImGui::RadioButton(GetCopySourceTypeLabel(type), !timeOfDay)", ui)

    def test_copy_selection_sections_use_distinct_ids_and_framed_setting_header(self):
        ui = (ROOT / "src/CSEditor/SceneSettingsUI.cpp").read_text(encoding="utf-8")
        for function, scope in (
                ("static void DrawCopyDestinationPopup(", "CopySettingSelection"),
                ("static void DrawCopyWeatherDestinations(", "CopyWeatherDestinations"),
                ("static void DrawCopyLocationDestinations(", "CopyLocationDestinations")):
            start = ui.index(function, ui.index("static bool IsInactiveCopySet("))
            body = ui[start:ui.index("\n\tstatic ", start + 1)]
            self.assertLess(body.index(f'ImGui::PushID("{scope}")'),
                            body.index('T("feature.scene_manager.action.select_all"'))
            self.assertIn("scope_exit restoreId", body)
        self.assertIn('ImGui::CollapsingHeader(T("feature.scene_manager.column.setting", "Setting"))', ui)
        self.assertNotIn('ImGui::TreeNodeEx(T("feature.scene_manager.column.setting", "Setting")', ui)
        self.assertIn('std::format("{}###SceneTarget", label)', ui)

    def test_toolbar_marker_uses_normal_text_and_scene_status(self):
        ui = (ROOT / "src/CSEditor/SceneSettingsUI.cpp").read_text(encoding="utf-8")
        utility = (ROOT / "src/Utils/UI.cpp").read_text(encoding="utf-8")
        self.assertIn('std::format("{} *", label)', ui)
        self.assertIn('SceneSettingMarker::Overwrite ? std::format("{} **", label)', ui)
        self.assertNotIn("DrawTextWithWeight", ui + utility)
        self.assertNotIn("BeginComboWithStyledPreview", ui + utility)
        self.assertNotIn("GetFeatureScenePickerColor", ui)
        self.assertIn("GetScenePickerTextColor(current, GetWeatherPickerPresentation(weather).paused)", ui)
        self.assertIn("GetScenePickerTextColor(IsCurrentLocation(candidate), entry.paused)", ui)
        self.assertIn("GetScenePickerLabel(preview, hasPeriodSettings(period))", ui)
        self.assertIn("GetScenePickerLabel(preview, hasTypeSettings(selectedType))", ui)
        self.assertEqual(ui.count("if (Util::BeginSearchableCombo(id, previewLabel.c_str()"), 2)
        self.assertIn('std::format("##SceneType{}",', ui)
        self.assertIn('ImGui::Selectable("##SceneTarget"', ui)

    def test_copy_targets_keep_configured_membership_when_current(self):
        ui = (ROOT / "src/CSEditor/SceneSettingsUI.cpp").read_text(encoding="utf-8")
        start = ui.index("static void DrawCopyWeatherDestinations(")
        weather = ui[start:ui.index("static void DrawCopyLocationDestinations(", start)]
        self.assertLess(weather.index("configured.push_back(weather)"), weather.index("current.push_back(weather)"))
        self.assertIn("!manager->GetWeatherConfig(id).entries.empty()", weather)
        start = ui.index("static void DrawCopyLocationDestinations(")
        location = ui[start:ui.index("static std::vector<SceneSettingsManager::SceneContextId> GetCopyDestinations(", start)]
        self.assertIn("cache.entries[found->second].configured", location[:location.index("cache.currentIndices.push_back")])
        self.assertIn("LocationTargetType::LocationType", location[:location.index("cache.currentIndices.push_back")])

    def test_copy_settings_list_fits_content_and_manage_buttons_align_right(self):
        ui = (ROOT / "src/CSEditor/SceneSettingsUI.cpp").read_text(encoding="utf-8")
        self.assertIn("std::clamp(copy.sourceSettings.size(), size_t{ 1 }, kCopySettingVisibleRows)", ui)
        self.assertIn('ImGui::BeginChild("##FeatureCopySettings", ImVec2(0.0f, listHeight)', ui)
        self.assertIn("right - buttonWidth - ImGui::GetStyle().ItemInnerSpacing.x", ui)
        self.assertIn("selected || manageHovered", ui)
        renderer = (ROOT / "src/Menu/FeatureListRenderer.cpp").read_text(encoding="utf-8")
        self.assertIn("if (sceneMgr->HasCurrentSceneSettingsForFeature(featureShortName))", renderer)
        self.assertNotIn("HasAnySceneEntriesForFeature", renderer)
        start = ui.index("static void DrawTargetManagementPopup(", ui.index("static void DrawSceneManagementActions("))
        popup = ui[start:ui.index("void DrawGlobalActions(", start)]
        self.assertIn("ImGui::SetNextWindowSize(ImVec2(0.0f, 0.0f), ImGuiCond_Appearing)", popup)
        self.assertNotIn("SetNextWindowSizeConstraints", popup)

    def test_copy_defaults_to_non_timed_sets(self):
        ui = (ROOT / "src/CSEditor/SceneSettingsUI.cpp").read_text(encoding="utf-8")
        start = ui.index("struct CopyDestinationState")
        state = ui[start:ui.index("static void DrawCopyDestinationPopup(", start)]
        self.assertIn("bool timeOfDay = false", state)
        self.assertNotIn("state.destination.timeOfDay =", ui)
        self.assertIn("if (source.allPeriods)", ui)

    def test_location_types_keep_their_own_section_when_current(self):
        ui = (ROOT / "src/CSEditor/SceneSettingsUI.cpp").read_text(encoding="utf-8")
        for start_name, end_name in (
                ("static void RefreshFeatureLocationPickerCache(", "static bool DrawFeatureLocationPicker("),
                ("static void RefreshLocationTargetPickerCache(", "static const LocationTarget* ResolveSelectedLocationTarget(")):
            start = ui.index(start_name)
            cache = ui[start:ui.index(end_name, start)]
            self.assertIn("LocationTargetType::LocationType ? cache.locationTypeIndices", cache)
        for start_name, end_name in (
                ("static bool DrawFeatureLocationPicker(", "static bool DrawFeatureSceneTypePicker("),
                ("void DrawLocationPanel(", "struct WeatherPanelState")):
            start = ui.index(start_name)
            picker = ui[start:ui.index(end_name, start)]
            self.assertIn("current->type ==", picker)
            self.assertIn("LocationTargetType::LocationType)", picker)
            self.assertIn("currentTargets.rbegin()", picker)
            configured = picker.index('T("feature.scene_manager.location.group.configured"')
            types = picker.index('T("feature.scene_manager.location.group.types"')
            available = picker.index('T("feature.scene_manager.location.group.available"')
            self.assertLess(configured, types)
            self.assertLess(types, available)

    def test_location_types_collapse_without_hiding_search_matches(self):
        ui = (ROOT / "src/CSEditor/SceneSettingsUI.cpp").read_text(encoding="utf-8")
        start = ui.index("static bool DrawLocationPickerGroupHeader(")
        header = ui[start:ui.index("static bool DrawFeatureLocationPicker(", start)]
        self.assertIn("if (locationTypes && !filtering)", header)
        self.assertIn('ImGui::SeparatorText("");', header)
        self.assertIn("return ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_NoTreePushOnOpen);", header)
        self.assertNotIn("CollapsingHeader", header)
        self.assertIn("ImGui::SeparatorText(label);", header)
        self.assertNotIn("DefaultOpen", header)
        for start_name, end_name, type_group, filtering in (
                ("static bool DrawFeatureLocationPicker(", "static bool DrawFeatureSceneTypePicker(",
                 "&candidates == &cache.locationTypeIndices", "!Util::GetSearchableComboFilter().empty()"),
                ("void DrawLocationPanel(", "struct WeatherPanelState",
                 "&candidates == &targetPicker.locationTypeIndices", "!Util::GetSearchableComboFilter().empty()"),
                ("static void DrawCopyLocationDestinations(", "static std::vector<SceneSettingsManager::SceneContextId> GetCopyDestinations(",
                 "groups[group] == &cache.locationTypeIndices", "!filter.empty()")):
            start = ui.index(start_name)
            picker = ui[start:ui.index(end_name, start)]
            self.assertIn("if (!DrawLocationPickerGroupHeader(", picker)
            self.assertIn(type_group, picker)
            self.assertIn(filtering, picker)

    def test_location_type_order_prioritizes_configured_then_current(self):
        ui = (ROOT / "src/CSEditor/SceneSettingsUI.cpp").read_text(encoding="utf-8")
        start = ui.index("static void OrderLocationTypePickerEntries(")
        ordering = ui[start:ui.index("static bool DrawFeatureLocationPicker(", start)]
        self.assertIn("std::ranges::sort(cache.locationTypeIndices);", ordering)
        self.assertIn("cache.entries[index].configured", ordering)
        self.assertIn("std::ranges::stable_partition(unconfigured", ordering)
        self.assertIn("IsCurrentLocation(targets[cache.entries[index].targetIndex])", ordering)
        self.assertEqual(ui.count("OrderLocationTypePickerEntries(cache, targets);"), 2)
        self.assertIn("OrderLocationTypePickerEntries(targetPicker, targets);", ui)

    def test_location_metadata_precedes_the_time_of_day_mode(self):
        ui = (ROOT / "src/CSEditor/SceneSettingsUI.cpp").read_text(encoding="utf-8")
        start = ui.index("void DrawLocationPanel(")
        panel = ui[start:ui.index("struct WeatherPanelState", start)]
        self.assertLess(panel.index('T("feature.scene_manager.location.spid_key"'),
                        panel.index('ImGui::Checkbox(T("feature.scene_manager.tab.time_of_day"'))
        self.assertLess(panel.index('T("feature.scene_manager.location.coc_code"'),
                        panel.index('ImGui::Checkbox(T("feature.scene_manager.tab.time_of_day"'))

    def test_weather_picker_clips_rows_and_keeps_management_popup_owner(self):
        ui = (ROOT / "src/CSEditor/SceneSettingsUI.cpp").read_text(encoding="utf-8")
        start = ui.index("static bool DrawCopyWeatherPicker(")
        picker = ui[start:ui.index("static std::vector<RE::TESWeather*> GetCopySourceWeatherTargets(", start)]
        self.assertIn("std::vector<size_t> visibleIndices", picker)
        self.assertIn("!managing && filtering && !Util::SearchableComboMatches", picker)
        self.assertIn('ImHashStr("##ManageSceneTarget"', picker)
        self.assertIn("clipper.IncludeItemByIndex(selectedVisibleIndex);", picker)
        self.assertIn("clipper.IncludeItemByIndex(managedVisibleIndex);", picker)
        self.assertIn("candidates[visibleIndices[visibleIndex]]", picker)
        self.assertLess(picker.index("while (clipper.Step())"),
                        picker.index("DrawSceneTargetSelectable("))

    def test_worldspaces_have_detection_copy_and_persistence_support(self):
        start = self.manager.index("BuildLocationTargetChain(")
        chain = self.manager[start:self.manager.index("RE::TESForm* ResolveLocationTargetForm(", start)]
        world = chain.index("targets.push_back(MakeWorldspaceTarget(worldspace))")
        self.assertLess(world, chain.index("for (auto* locationType : locationTypes)"))
        self.assertIn("cell->GetRuntimeData().worldSpace", chain)
        current = extract_function(self.manager, "GetCurrentLocationTargets")
        self.assertIn("player->GetWorldspace()", current)
        self.assertIn("cachedTargetWorldspaceId == worldspaceId", current)
        management = extract_function(self.manager, "GetLocationManagementTargets")
        self.assertIn("GetFormArray<RE::TESWorldSpace>()", management)
        loading = extract_function(self.manager, "LoadLocationUserSettings")
        self.assertIn('loadSection("worldspaces", LocationTargetType::Worldspace, "Worldspace")', loading)
        self.assertIn("RE::FormType::WorldSpace", loading)
        overwrite = extract_function(self.manager, "DiscoverLocationOverwritesForTarget")
        self.assertIn('targetType == "Worldspace"', overwrite)
        self.assertIn("RE::FormType::WorldSpace", overwrite)
        self.assertIn('return "worldspaces"', extract_function(self.manager, "GetLocationSectionName"))
        self.assertIn('return "Worldspace"', extract_function(self.manager, "GetLocationTargetTypeName"))

    def test_normal_location_table_keeps_non_float_controls(self):
        ui = (ROOT / "src/CSEditor/SceneSettingsUI.cpp").read_text(encoding="utf-8")
        self.assertIn("showTimeOfDay ? kPeriodCount : 1, !showTimeOfDay, showTimeOfDay,", ui)
        start = ui.index("static void DrawLocationAddDialog(")
        end = ui.index("static void DrawLocationValueEditor(", start)
        add = ui[start:end]
        self.assertIn("if (showTimeOfDay)", add)
        self.assertIn("!std::ranges::all_of(setting.members", add)
        for control in ("ImGui::Checkbox", "Util::BeginSearchableCombo", "ImGui::DragScalar", "ImGui::InputText"):
            self.assertIn(control, ui)
        self.assertIn("layout.checkboxOnlyValueColumn = !multiColumn && IsCheckboxOnlyGroup(group, entries)", ui)
        self.assertIn("bool inlineActions = HasInlineActionColumn(numValueColumns)", ui)
        location = extract_function(self.manager, "ResolveLocationSettings")
        self.assertIn("!config->second.IsPeriodActive(entry.period)", location)
        self.assertIn("if (!IsNumericValue(entry.value))", location)
        self.assertIn("resolved[address] = entry.value", location)

    def test_named_feature_policy_does_not_leak_into_scene_manager_code(self):
        named_features = {
            *(path[0] for path in self.blacklist),
            *self.location_features,
            *self.time_features,
        }
        implementation_paths = [
            path for path in (ROOT / "src").glob("SceneSettings*.h")
            if path != POLICY_PATH
        ] + list((ROOT / "src").glob("SceneSettings*.cpp")) + [
            ROOT / "src" / "CSEditor" / "SceneSettingsUI.h",
            ROOT / "src" / "CSEditor" / "SceneSettingsUI.cpp",
            GENERATOR_PATH,
        ]
        for path in implementation_paths:
            source = path.read_text(encoding="utf-8")
            for feature in named_features:
                with self.subTest(path=path, feature=feature):
                    self.assertNotIn(f'"{feature}"', source)


if __name__ == "__main__":
    unittest.main()
