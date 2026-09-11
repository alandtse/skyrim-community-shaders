#include "SceneManagerUI.h"

#include "CSEditor/SceneSettingsUI.h"
#include "I18n/I18n.h"

namespace SceneManagerUI
{
	bool CanEditFeaturePage(Feature* feature)
	{
		return SceneSettingsUI::CanEditFeaturePage(feature);
	}

	bool BeginFeaturePageEditing(Feature* feature)
	{
		return SceneSettingsUI::BeginFeaturePageEditing(feature);
	}

	bool IsFeaturePageEditing(Feature* feature)
	{
		return SceneSettingsUI::IsFeaturePageEditing(feature);
	}

	bool DrawFeaturePageControls(Feature* feature, bool enabled)
	{
		return SceneSettingsUI::DrawFeaturePageControls(feature, enabled);
	}

	void HideFeaturePageEditing()
	{
		SceneSettingsUI::HideFeaturePageEditing();
	}

	void Draw()
	{
		SceneSettingsUI::DrawGlobalActions();
		if (!ImGui::BeginTabBar("##SceneManagerTabs"))
			return;

		if (ImGui::BeginTabItem(T("feature.scene_manager.tab.interior", "Interior"))) {
			SceneSettingsUI::DrawInteriorPanel();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem(T("feature.scene_manager.tab.time_of_day", "Time of Day"))) {
			SceneSettingsUI::DrawTimeOfDayPanel();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem(T("feature.scene_manager.tab.locations", "Locations"))) {
			SceneSettingsUI::DrawLocationPanel();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem(T("feature.scene_manager.tab.weather", "Weather"))) {
			SceneSettingsUI::DrawWeatherPanel();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem(T("feature.scene_manager.copy.title", "Copy Scene Settings"))) {
			SceneSettingsUI::DrawCopyPanel();
			ImGui::EndTabItem();
		}

		ImGui::EndTabBar();
	}
}
