#pragma once

#include "Menu.h"

#include <functional>
#include <string>
#include <variant>
#include <vector>

struct Feature;

/**
 * @brief Renders the two-column feature list and settings panel in the main menu.
 *
 * The left column shows a searchable, alphabetical list of built-in pages and
 * installed features. The right column displays the settings UI for whichever
 * item is currently selected.
 */
class FeatureListRenderer
{
public:
	/** @brief Describes a built-in (non-feature) menu page with a name and draw callback. */
	struct BuiltInMenu
	{
		std::string name;  // translated display text
		// Untranslated identifier used for page navigation.
		std::string canonicalId;
		std::function<void()> func;
	};

	/** @brief Represents a section header in the feature list. */
	struct CategoryHeader
	{
		std::string name;
		int count = 0;
	};

	/** @brief Variant type representing any entry in the menu list. */
	using MenuFuncInfo = std::variant<BuiltInMenu, std::string, CategoryHeader, Feature*>;

	/**
	 * @brief Renders the full two-column feature list and settings panel.
	 *
	 * Builds the menu list from built-in pages and loaded features, handles
	 * pending feature selection requests, then draws the left-column navigation
	 * and right-column settings content.
	 *
	 * @param footerHeight Height reserved for the footer area below the list.
	 * @param sidebar Sidebar visibility, animation progress, and expanded width.
	 * @param selectedMenu Index of the currently selected menu item (updated on selection change).
	 * @param featureSearch Current search filter string (updated by the search input).
	 * @param pendingFeatureSelection Name of a feature to auto-select (cleared after processing).
	 * @param drawGeneralSettings Callback that renders the General settings page content.
	 * @param drawAdvancedSettings Callback that renders the Advanced settings page content.
	 */
	static void RenderFeatureList(
		float footerHeight,
		Menu::SidebarState& sidebar,
		size_t& selectedMenu,
		std::string& featureSearch,
		std::string& pendingFeatureSelection,
		const std::function<void()>& drawGeneralSettings,
		const std::function<void()>& drawAdvancedSettings);

private:
	struct ListMenuVisitor
	{
		size_t listId;
		size_t& selectedMenuRef;

		void operator()(const BuiltInMenu& menu);
		void operator()(const std::string& label);
		void operator()(const CategoryHeader& header);
		void operator()(Feature* feat);
	};

	struct DrawMenuVisitor
	{
		explicit DrawMenuVisitor(std::string& pendingFeatureSelectionRef) :
			pendingFeatureSelection(pendingFeatureSelectionRef) {}

		void operator()(const BuiltInMenu& menu);
		void operator()(const std::string&);
		void operator()(const CategoryHeader&);
		void operator()(Feature* feat);

	private:
		std::string& pendingFeatureSelection;

		// Helper methods for Feature rendering
		struct FeatureActionsLayout
		{
			float x{};
			float y{};
			float size{};
		};

		FeatureActionsLayout RenderFeatureHeader(Feature* feat, bool isDisabled, bool isLoaded,
			bool canEditSceneSettings);
		void RenderFeatureActions(Feature* feat, bool isDisabled, bool isLoaded,
			bool sceneControlled, bool canEditSceneSettings, const FeatureActionsLayout& layout);
		float RenderFeatureMaterial(Feature* feat, bool isDisabled, bool isLoaded,
			bool hasFailedMessage, bool sceneControlled, bool sceneEditing);
		void RenderFeatureSettings(Feature* feat, bool isDisabled, bool isLoaded,
			bool hasFailedMessage, bool sceneControlled, bool sceneEditing);
		void RenderReactiveConstraintWarningDialog();
	};

	static std::vector<MenuFuncInfo> BuildMenuList(
		const std::function<void()>& drawGeneralSettings,
		const std::function<void()>& drawAdvancedSettings);

	static void HandlePendingFeatureSelection(
		std::string& pendingFeatureSelection,
		const std::vector<MenuFuncInfo>& menuList,
		size_t& selectedMenu);

	static void RenderLeftColumn(
		const std::vector<MenuFuncInfo>& menuList,
		size_t& selectedMenu,
		std::string& featureSearch);

	static void RenderRightColumn(
		const std::vector<MenuFuncInfo>& menuList,
		size_t selectedMenu,
		std::string& pendingFeatureSelection);
};
