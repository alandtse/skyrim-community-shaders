import importlib.util
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).parents[1]
GENERATOR_PATH = ROOT / "cmake" / "generate_scene_settings_catalog.py"
SPEC = importlib.util.spec_from_file_location("scene_catalog_generator", GENERATOR_PATH)
GENERATOR = importlib.util.module_from_spec(SPEC)
assert SPEC.loader
SPEC.loader.exec_module(GENERATOR)


SYNTHETIC_HEADER = r'''
struct SyntheticFeature : Feature
{
    struct WaterSettings
    {
        float amount = 0.0f;
    };

    struct Settings
    {
        float regular = 0.0f;
        WaterSettings water{};
        float guardSentinel = 0.0f;
        float2 unboundedRange{};
        float constrained = 0.0f;
        int iterations = 0;
        float3 standardColor3{};
        float4 standardColor4{};
        float3 hdrColor3{};
        float4 hdrColor4{};
        uint32_t wrappedToggle = 0;
        float wrappedFloat = 0.0f;
        float3 wrappedColor3{};
        float4 wrappedColor4{};
        uint32_t proxyToggle = 0;
        uint32_t proxySteps = 0;
        uint32_t tableMode = 0;
        bool fallbackToggle = false;
        int fallbackInteger = 0;
        float fallbackFloat = 0.0f;
        std::string fallbackText;
    } settings;

    std::string GetShortName() { return "Synthetic"; }
    std::string GetName() { return "Synthetic Feature"; }
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    SyntheticFeature::WaterSettings,
    amount)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    SyntheticFeature::Settings,
    regular,
    water,
    guardSentinel,
    unboundedRange,
    constrained,
    iterations,
    standardColor3,
    standardColor4,
    hdrColor3,
    hdrColor4,
    wrappedToggle,
    wrappedFloat,
    wrappedColor3,
    wrappedColor4,
    proxyToggle,
    proxySteps,
    tableMode,
    fallbackToggle,
    fallbackInteger,
    fallbackFloat,
    fallbackText)
'''


SYNTHETIC_SOURCE = r'''
#define I18N_KEY_PREFIX "feature.synthetic."

constexpr uint32_t kModeFirst = 2;
constexpr uint32_t kModeSecond = 5;
constexpr uint32_t kModeThird = 9;
constexpr uint32_t kModeFourth = 14;
constexpr uint32_t kProxyMaximum = 12;

namespace Synthetic::Wrappers
{
bool Checkbox(const char* label, Feature* feature, const char* settingName, bool* value)
{
    return ImGui::Checkbox(label, value);
}

bool SliderFloat(const char* label, Feature* feature, const char* settingName, float* value,
                 float minimum, float maximum, const char* format)
{
    return ImGui::SliderFloat(
        label, value, minimum, maximum, format);
}

bool ColorEdit3(const char* label, Feature* feature, const char* settingName, float values[3])
{
    return ImGui::ColorEdit3(label, values);
}

bool ColorEdit4(const char* label, Feature* feature, const char* settingName, float values[4])
{
    return ImGui::ColorEdit4(label, values);
}
}

namespace Synthetic::Decoys
{
bool Checkbox(const char* label, Feature* feature, const char* settingName, bool* value)
{
    return ImGui::Checkbox(label, value);
}

bool SliderFloat(const char* label, Feature* feature, const char* settingName, float* value,
                 float minimum, float maximum, const char* format)
{
    return ImGui::SliderFloat(label, value, -100.0f, 100.0f, format);
}

bool ColorEdit3(const char* label, Feature* feature, const char* settingName, float values[3])
{
    return ImGui::ColorEdit3(label, values, ImGuiColorEditFlags_HDR);
}

bool ColorEdit4(const char* label, Feature* feature, const char* settingName, float values[4])
{
    return ImGui::ColorEdit4(label, values, ImGuiColorEditFlags_HDR);
}
}

void DrawGuardedSlider(const char* label, float& value, float minimum, float maximum)
{
    ImGui::SliderFloat(label, &value, minimum, maximum);
}

void SyntheticFeature::DrawSettings()
{
    if (ImGui::BeginTabItem(T(TKEY("advanced"), "Advanced"))) {
        ImGui::SeparatorText(T(TKEY("shape"), "Shape"));
        ImGui::SliderFloat(
            T(TKEY("regular"), "Regular"), &settings.regular, -1.0f, 1.0f);
        ImGui::InputFloat2(
            T(TKEY("unbounded_range"), "Unbounded Range"),
            &settings.unboundedRange.x, "%.3f");

        const ImGuiSliderFlags constrainedFlags = ImGuiSliderFlags_AlwaysClamp;
        ImGui::SliderFloat(
            T(TKEY("constrained"), "Constrained"),
            &settings.constrained, -2.0f, 2.0f, "%.3f", constrainedFlags);
        ImGui::SliderInt(
            T(TKEY("iterations"), "Iterations"), &settings.iterations, 1, 8);
        ImGui::ColorEdit3(
            T(TKEY("standard_color_3"), "Standard Color 3"),
            &settings.standardColor3.x);
        ImGui::ColorEdit4(
            T(TKEY("standard_color_4"), "Standard Color 4"),
            &settings.standardColor4.x);
        ImGui::ColorEdit3(
            T(TKEY("hdr_color_3"), "HDR Color 3"),
            &settings.hdrColor3.x, ImGuiColorEditFlags_HDR);
        ImGui::ColorEdit4(
            T(TKEY("hdr_color_4"), "HDR Color 4"),
            &settings.hdrColor4.x, ImGuiColorEditFlags_HDR);
        Synthetic::Wrappers::Checkbox(
            T(TKEY("wrapped_toggle"), "Wrapped Toggle"), this,
            "wrappedToggle", (bool*)&settings.wrappedToggle);
        Synthetic::Wrappers::SliderFloat(
            T(TKEY("wrapped_float"), "Wrapped Float"), this,
            "wrappedFloat", &settings.wrappedFloat, -4.0f, 9.0f, "%.2f");
        Synthetic::Wrappers::ColorEdit3(
            T(TKEY("wrapped_color_3"), "Wrapped Color 3"), this,
            "wrappedColor3", &settings.wrappedColor3.x);
        Synthetic::Wrappers::ColorEdit4(
            T(TKEY("wrapped_color_4"), "Wrapped Color 4"), this,
            "wrappedColor4", &settings.wrappedColor4.x);
        bool proxyToggle = settings.proxyToggle != 0;
        if (ImGui::Checkbox(
                T(TKEY("proxy_toggle"), "Proxy Toggle"), &proxyToggle)) {
            settings.proxyToggle = proxyToggle ? 1u : 0u;
        }
        int proxySteps = (int)settings.proxySteps;
        if (ImGui::SliderInt(
                T(TKEY("proxy_steps"), "Proxy Steps"), &proxySteps,
                2, (int)kProxyMaximum)) {
            settings.proxySteps = (uint)std::clamp(
                proxySteps, 2, (int)kProxyMaximum);
        }
        struct TableOption
        {
            uint32_t value;
            const char* label;
        };
        const TableOption tableOptions[][2] = {
            {
                { kModeFirst, T(TKEY("mode_first"), "First") },
                { kModeSecond, T(TKEY("mode_second"), "Second") },
            },
            {
                { kModeThird, T(TKEY("mode_third"), "Third") },
                { kModeFourth, T(TKEY("mode_fourth"), "Fourth") },
            },
        };
        int tableMode = static_cast<int>(settings.tableMode);
        if (ImGui::BeginTable("##TableOptions", 2)) {
            for (const auto& row : tableOptions) {
                for (const auto& option : row) {
                    ImGui::TableNextColumn();
                    if (ImGui::RadioButton(
                            option.label, &tableMode,
                            static_cast<int>(option.value))) {
                        settings.tableMode = static_cast<uint32_t>(tableMode);
                    }
                }
            }
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }

    DrawWaterSettings();
}

void SyntheticFeature::DrawWaterSettings()
{
    if (!ImGui::BeginTabItem(T(TKEY("water"), "Water")))
        return;

    auto& water = settings.water;
    DrawGuardedSlider(
        T(TKEY("water_amount"), "Water Amount"), water.amount, 0.0f, 2.0f);
    ImGui::EndTabItem();

    settings.guardSentinel = settings.guardSentinel;
}

void SyntheticFeature::SaveSettings(json& output)
{
    output = settings;
}
'''


def write_synthetic_source(root: Path) -> None:
    source_dir = root / "src"
    source_dir.mkdir(parents=True)
    (source_dir / "SyntheticFeature.h").write_text(
        SYNTHETIC_HEADER, encoding="utf-8")
    (source_dir / "SyntheticFeature.cpp").write_text(
        SYNTHETIC_SOURCE, encoding="utf-8")


class SceneSettingsCatalogGeneratorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp_directory = tempfile.TemporaryDirectory()
        cls.synthetic_root = Path(cls.temp_directory.name)
        write_synthetic_source(cls.synthetic_root)
        cls.entries = GENERATOR.build_entries(cls.synthetic_root)
        cls.entries_by_id = {
            (entry["path"], entry["key"]): entry
            for entry in cls.entries
        }

    @classmethod
    def tearDownClass(cls):
        cls.temp_directory.cleanup()

    def test_numeric_widgets_preserve_source_and_input_policy(self):
        regular = self.entries_by_id[("", "regular")]
        constrained = self.entries_by_id[("", "constrained")]
        integer = self.entries_by_id[("", "iterations")]

        self.assertEqual(regular["sourceWidget"], "SliderFloat")
        self.assertEqual((regular["minimum"], regular["maximum"]), (-1.0, 1.0))
        self.assertFalse(regular["clampNumericInput"])

        self.assertEqual(constrained["sourceWidget"], "SliderFloat")
        self.assertEqual(
            (constrained["minimum"], constrained["maximum"]), (-2.0, 2.0))
        self.assertTrue(constrained["clampNumericInput"])

        self.assertEqual(integer["sourceWidget"], "SliderInt")
        self.assertEqual(integer["type"], "Integer")
        self.assertEqual((integer["minimum"], integer["maximum"]), (1.0, 8.0))

    def test_unbounded_aggregate_uses_default_component_labels_without_bounds(self):
        entries = [
            self.entries_by_id[("unboundedRange", component)]
            for component in ("x", "y")
        ]

        self.assertEqual(
            [entry["componentDisplayName"] for entry in entries],
            ["", ""])
        self.assertTrue(all(entry["componentDisplayNameKey"] == "" for entry in entries))
        self.assertTrue(all(entry["sourceWidget"] == "InputFloat2" for entry in entries))
        self.assertTrue(all(not entry["hasNumericBounds"] for entry in entries))

    def test_early_return_tab_and_nested_draw_helper_are_discovered(self):
        guarded = self.entries_by_id[("water", "amount")]
        sentinel = self.entries_by_id[("", "guardSentinel")]

        self.assertEqual(guarded["selectorPath"], "Water")
        self.assertEqual(guarded["displayName"], "Water Amount")
        self.assertEqual(guarded["sourceWidget"], "SliderFloat")
        self.assertEqual((guarded["minimum"], guarded["maximum"]), (0.0, 2.0))
        self.assertEqual(sentinel["selectorPath"], "")

    def test_component_discovery_accepts_unique_and_shared_factories(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            header = root / "FactoryFeature.h"
            source = root / "FactoryFeature.cpp"
            header.write_text(r'''
struct UniqueComponent
{
    std::string GetType() { return "Unique"; }
    std::string GetDisplayName() { return "Unique Component"; }
};

struct SharedComponent
{
    std::string GetType() { return "Shared"; }
    std::string GetDisplayName() { return "Shared Component"; }
};

struct FactoryFeature : Feature
{
    std::string GetShortName() { return "Factory"; }
    std::string GetName() { return "Factory Feature"; }
};
''', encoding="utf-8")
            source.write_text(r'''
void FactoryFeature::Setup()
{
    pipeline[0] = std::make_unique<UniqueComponent>();
    pipeline[1] = std::make_shared<SharedComponent>();
}
''', encoding="utf-8")

            paths = [header, source]
            features = GENERATOR.collect_features([header])
            components = GENERATOR.collect_settings_components(features, paths)

            self.assertEqual(
                {component[0] for component in components["FactoryFeature"]},
                {"UniqueComponent", "SharedComponent"})

    def test_standard_colors_are_bounded_and_hdr_colors_are_unbounded(self):
        for field, component_count in (("standardColor3", 3), ("standardColor4", 4)):
            entries = [
                self.entries_by_id[(field, component)]
                for component in ("x", "y", "z", "w")[:component_count]
            ]
            self.assertTrue(all(entry["sourceWidget"] == f"ColorEdit{component_count}"
                                for entry in entries))
            self.assertTrue(all(entry["hasNumericBounds"] for entry in entries))
            self.assertTrue(all((entry["minimum"], entry["maximum"]) == (0.0, 1.0)
                                for entry in entries))
            self.assertTrue(all(entry["clampNumericInput"] for entry in entries))
            self.assertTrue(all(not entry["hdrColor"] for entry in entries))

        for field, component_count in (("hdrColor3", 3), ("hdrColor4", 4)):
            entries = [
                self.entries_by_id[(field, component)]
                for component in ("x", "y", "z", "w")[:component_count]
            ]
            self.assertTrue(all(entry["sourceWidget"] == f"ColorEdit{component_count}"
                                for entry in entries))
            self.assertTrue(all(not entry["hasNumericBounds"] for entry in entries))
            self.assertTrue(all(not entry["clampNumericInput"] for entry in entries))
            self.assertTrue(all(entry["hdrColor"] for entry in entries))

    def test_namespaced_wrappers_preserve_direct_control_metadata(self):
        toggle = self.entries_by_id[("", "wrappedToggle")]
        self.assertEqual(toggle["type"], "Integer")
        self.assertEqual(toggle["editorSemantic"], "Toggle")
        self.assertEqual(toggle["sourceWidget"], "Checkbox")
        self.assertIn("BooleanControl", toggle["flags"])

        slider = self.entries_by_id[("", "wrappedFloat")]
        self.assertEqual(slider["editorSemantic"], "Numeric")
        self.assertEqual(slider["sourceWidget"], "SliderFloat")
        self.assertEqual((slider["minimum"], slider["maximum"]), (-4.0, 9.0))
        self.assertFalse(slider["clampNumericInput"])

        for field, component_count in (("wrappedColor3", 3), ("wrappedColor4", 4)):
            entries = [
                self.entries_by_id[(field, component)]
                for component in ("x", "y", "z", "w")[:component_count]
            ]
            self.assertTrue(all(entry["sourceWidget"] == f"ColorEdit{component_count}"
                                for entry in entries))
            self.assertTrue(all(entry["aggregateSemantic"] == "Color"
                                for entry in entries))
            self.assertTrue(all(entry["aggregatePresentation"] == "ColorPicker"
                                for entry in entries))
            self.assertTrue(all((entry["minimum"], entry["maximum"]) == (0.0, 1.0)
                                for entry in entries))
            self.assertTrue(all(entry["clampNumericInput"] for entry in entries))
            self.assertTrue(all(not entry["hdrColor"] for entry in entries))

    def test_local_proxies_preserve_control_semantics_and_bounds(self):
        toggle = self.entries_by_id[("", "proxyToggle")]
        self.assertEqual(toggle["type"], "Integer")
        self.assertEqual(toggle["editorSemantic"], "Toggle")
        self.assertEqual(toggle["sourceWidget"], "Checkbox")
        self.assertIn("BooleanControl", toggle["flags"])

        slider = self.entries_by_id[("", "proxySteps")]
        self.assertEqual(slider["type"], "Integer")
        self.assertEqual(slider["editorSemantic"], "Numeric")
        self.assertEqual(slider["sourceWidget"], "SliderInt")
        self.assertEqual((slider["minimum"], slider["maximum"]), (2.0, 12.0))

    def test_record_array_radio_buttons_emit_choices(self):
        mode = self.entries_by_id[("", "tableMode")]
        self.assertEqual(mode["editorSemantic"], "Choice")
        self.assertEqual(mode["sourceWidget"], "RadioButton")
        self.assertEqual(
            mode["choices"],
            ((2, "First", "feature.synthetic.mode_first"),
             (5, "Second", "feature.synthetic.mode_second"),
             (9, "Third", "feature.synthetic.mode_third"),
             (14, "Fourth", "feature.synthetic.mode_fourth")))

    def test_tabs_select_and_headings_only_label(self):
        entry = self.entries_by_id[("", "regular")]
        self.assertEqual(entry["selectorPath"], "Advanced")
        self.assertEqual(entry["displayPath"], "Shape")
        self.assertNotIn("Shape", entry["selectorPath"])

    def test_unbound_persisted_primitives_use_generic_fallback(self):
        expected_types = {
            "fallbackToggle": "Boolean",
            "fallbackInteger": "Integer",
            "fallbackFloat": "Float",
            "fallbackText": "String",
        }
        for key, expected_type in expected_types.items():
            entry = self.entries_by_id[("", key)]
            self.assertEqual(entry["type"], expected_type)
            self.assertEqual(entry["editorSemantic"], "Generic")
            self.assertEqual(entry["sourceWidget"], "")
            self.assertIn("SceneControllable", entry["flags"])

    def test_generated_schema_contains_runtime_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            GENERATOR.write_catalog(self.entries, output)
            header = (output / "SceneSettingsCatalog.generated.h").read_text(
                encoding="utf-8")
            source = (output / "SceneSettingsCatalog.generated.cpp").read_text(
                encoding="utf-8")

        self.assertIn("std::string_view sourceWidget;", header)
        self.assertIn("bool clampNumericInput;", header)
        self.assertIn("bool hdrColor;", header)
        self.assertIn('"SliderInt"', source)
        self.assertIn('"ColorEdit4"', source)

    def test_scalar_bounds_use_the_correct_imgui_arguments(self):
        constants = {}
        slider = GENERATOR.get_control_numeric_metadata(
            "SliderScalarN",
            ["Label", "Type", "Data", "3", "-4", "9", "Format", "Flags"],
            constants)
        drag = GENERATOR.get_control_numeric_metadata(
            "DragScalarN",
            ["Label", "Type", "Data", "3", "Speed", "-7", "11", "Format", "Flags"],
            constants)
        self.assertEqual(slider, (-4.0, 9.0, 1.0))
        self.assertEqual(drag, (-7.0, 11.0, 1.0))

    def test_unknown_control_with_known_storage_has_generic_editor(self):
        binding = GENERATOR.ControlBinding(
            "SyntheticFeature", ("flags",), GENERATOR.LocalizedText("Flags"),
            GENERATOR.LocalizedText(), "CheckboxFlags",
            source_widget="CheckboxFlags")
        self.assertEqual(
            GENERATOR.resolve_editor_semantic(binding, "Integer"), "Generic")
        self.assertEqual(
            GENERATOR.resolve_editor_semantic(None, "Integer"), "Generic")
        self.assertEqual(
            GENERATOR.resolve_editor_semantic(binding, "Integer", True), "None")

    def test_validation_rejects_inconsistent_input_metadata(self):
        entry = dict(self.entries_by_id[("", "regular")])
        entry["clampNumericInput"] = True
        entry["hasNumericBounds"] = False
        with self.assertRaisesRegex(ValueError, "clamped numeric input without bounds"):
            GENERATOR.validate_entries([entry], 1)

        entry = dict(self.entries_by_id[("hdrColor3", "x")])
        entry["hdrColor"] = True
        entry["hasNumericBounds"] = True
        with self.assertRaisesRegex(ValueError, "invalid HDR color metadata"):
            GENERATOR.validate_entries([entry], 1)

    def test_catalog_paths_are_reversible(self):
        self.assertEqual(
            GENERATOR.join_catalog_path(("Group/Heading", "Value~Name")),
            "Group~1Heading/Value~0Name")

    def test_repository_catalog_validates_without_a_test_inventory(self):
        entries = GENERATOR.build_entries(ROOT)
        self.assertTrue(entries)
        GENERATOR.validate_entries(entries, 1)


if __name__ == "__main__":
    unittest.main()
