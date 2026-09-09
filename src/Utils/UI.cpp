#include "UI.h"

#include "../I18n/I18n.h"
#include "D3D.h"
#include "Feature.h"
#include "FileSystem.h"
#include "Menu.h"
#include "Menu/Fonts.h"
#include "Menu/IconLoader.h"
#include "Menu/ThemeManager.h"
#include "PerfUtils.h"
#include "ShaderCache.h"

#ifndef DIRECTINPUT_VERSION
#	define DIRECTINPUT_VERSION 0x0800
#endif
#include <DirectXTex.h>
#include <d3d11.h>
#include <dinput.h>
#include <dxgi.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <wrl/client.h>

#include "../Feature.h"
#include "../Features/VR.h"
#include "../Globals.h"
#include "../Menu.h"
#include "FileSystem.h"
#include "VRUtils.h"

#define STB_IMAGE_IMPLEMENTATION
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <functional>
#include <iomanip>
#include <iterator>
#include <limits>
#include <mutex>
#include <numbers>
#include <sstream>
#include <stb_image.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Util
{
	void DrawSelectionButtons(std::span<uint8_t> selected, const char* selectAll, const char* selectNone)
	{
		if (ImGui::SmallButton(selectAll))
			std::ranges::fill(selected, uint8_t{ 1 });
		ImGui::SameLine();
		if (ImGui::SmallButton(selectNone))
			std::ranges::fill(selected, uint8_t{ 0 });
	}

	static ImVec2 g_screenScaleRatio = { 1.0f, 1.0f };
	static ImVec2 g_displaySize = { 0.0f, 0.0f };

	static int g_lastWindowWidth = 0;
	static int g_lastWindowHeight = 0;
	static thread_local const void* g_activeControlStorageAddress = nullptr;

	class ActiveControlStorageGuard
	{
	public:
		explicit ActiveControlStorageGuard(const void* address) :
			previousAddress(g_activeControlStorageAddress)
		{
			g_activeControlStorageAddress = address;
		}

		~ActiveControlStorageGuard()
		{
			g_activeControlStorageAddress = previousAddress;
		}

	private:
		const void* previousAddress;
	};

	void RefreshScreenScale(HWND hwnd, float bufferWidth, float bufferHeight)
	{
		RECT rect{};
		if (!GetClientRect(hwnd, &rect) || rect.right <= 0 || rect.bottom <= 0)
			return;

		if (rect.right == g_lastWindowWidth && rect.bottom == g_lastWindowHeight)
			return;

		g_displaySize.x = bufferWidth;
		g_displaySize.y = bufferHeight;

		g_screenScaleRatio.x = bufferWidth / static_cast<float>(rect.right);
		g_screenScaleRatio.y = bufferHeight / static_cast<float>(rect.bottom);

		g_lastWindowWidth = rect.right;
		g_lastWindowHeight = rect.bottom;
	}

	void UpdateImGuiInput(HWND hwnd, float bufferWidth, float bufferHeight)
	{
		RefreshScreenScale(hwnd, bufferWidth, bufferHeight);

		auto& io = ImGui::GetIO();
		io.DisplaySize = g_displaySize;

		POINT cursorPos{};
		if (GetCursorPos(&cursorPos) &&
			ScreenToClient(hwnd, &cursorPos)) {
			io.AddMousePosEvent(
				static_cast<float>(cursorPos.x) * g_screenScaleRatio.x,
				static_cast<float>(cursorPos.y) * g_screenScaleRatio.y);
		}
	}

	HoverTooltipWrapper::HoverTooltipWrapper() :
		previousFont(nullptr)
	{
		hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled);
		if (hovered) {
			ImGui::BeginTooltip();
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
			// Apply Subtext font for consistent tooltip styling
			if (auto* menu = globals::menu) {
				if (auto* subtextFont = menu->GetFont(Menu::FontRole::Subtext)) {
					previousFont = ImGui::GetFont();
					ImGui::PushFont(subtextFont, subtextFont->LegacySize);
				}
			}
		}
	}

	HoverTooltipWrapper::~HoverTooltipWrapper()
	{
		if (hovered) {
			if (previousFont) {
				ImGui::PopFont();
			}
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}
	}

	bool CloseButton(const char* id, float size)
	{
		const bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
		const auto minimum = ImGui::GetItemRectMin();
		const auto maximum = ImGui::GetItemRectMax();
		const bool held = ImGui::IsItemActive() || pressed;
		const bool hovered = ImGui::IsItemHovered();
		const auto color = ImGui::GetColorU32(
			held ? ImGuiCol_ButtonActive : hovered ? ImGuiCol_ButtonHovered :
													 ImGuiCol_Button);
		ImGui::RenderFrame(minimum, maximum, color, true, ImGui::GetStyle().FrameRounding);

		const ImVec2 center(
			(minimum.x + maximum.x) * 0.5f,
			(minimum.y + maximum.y) * 0.5f);
		const float halfExtent = std::max(3.0f, size * 0.2f);
		const float stroke = std::max(2.0f, size * 0.1f);
		auto* drawList = ImGui::GetWindowDrawList();
		const auto glyphColor = ImGui::GetColorU32(ImGuiCol_Text);
		drawList->AddLine(
			ImVec2(center.x - halfExtent, center.y - halfExtent),
			ImVec2(center.x + halfExtent, center.y + halfExtent), glyphColor, stroke);
		drawList->AddLine(
			ImVec2(center.x + halfExtent, center.y - halfExtent),
			ImVec2(center.x - halfExtent, center.y + halfExtent), glyphColor, stroke);
		return pressed;
	}

	struct DialogSizeAnimation
	{
		ImVec2 size;
		ImGuiSizeCallback callback = nullptr;
		void* callbackData = nullptr;
	};

	static void AnimateDialogSize(ImGuiSizeCallbackData* data)
	{
		auto& animation = *static_cast<DialogSizeAnimation*>(data->UserData);
		if (animation.callback) {
			data->UserData = animation.callbackData;
			animation.callback(data);
			data->UserData = &animation;
		}
		const float blend = 1.0f - std::exp(-ThemeManager::Constants::DIALOG_RESIZE_RESPONSE * ImGui::GetIO().DeltaTime);
		for (int axis = 0; axis < 2; ++axis) {
			const float difference = data->DesiredSize[axis] - animation.size[axis];
			const float step = std::min(std::abs(difference), std::ceil(std::abs(difference) * blend));
			data->DesiredSize[axis] = animation.size[axis] + std::copysign(step, difference);
		}
	}

	static void PrepareDialogSizeAnimation(ImGuiWindow* window, ImGuiWindowFlags flags, DialogSizeAnimation& animation)
	{
		const auto& context = *GImGui;
		if (!(flags & ImGuiWindowFlags_AlwaysAutoResize) || !window || window->Hidden ||
			window->LastFrameActive != context.FrameCount - 1 || window->AutoFitFramesX > 0 || window->AutoFitFramesY > 0)
			return;
		animation.size = window->SizeFull;
		const bool constrained = context.NextWindowData.HasFlags & ImGuiNextWindowDataFlags_HasSizeConstraint;
		const auto bounds = constrained ? context.NextWindowData.SizeConstraintRect : ImRect(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
		if (constrained) {
			animation.callback = context.NextWindowData.SizeCallback;
			animation.callbackData = context.NextWindowData.SizeCallbackUserData;
		}
		ImGui::SetNextWindowSizeConstraints(bounds.Min, bounds.Max, AnimateDialogSize, &animation);
	}

	static ImGuiWindow* GetOpenDialogWindow(const char* id)
	{
		const auto& context = *GImGui;
		if (context.OpenPopupStack.Size > context.BeginPopupStack.Size) {
			const auto& popup = context.OpenPopupStack[context.BeginPopupStack.Size];
			if (popup.PopupId == ImGui::GetID(id) && popup.OpenFrameCount < context.FrameCount - 1)
				return popup.Window;
		}
		return nullptr;
	}

	static bool BeginDialogPopup(const char* id, const char* title, bool* open, bool modal, ImGuiWindowFlags flags)
	{
		if (!(GImGui->NextWindowData.HasFlags & ImGuiNextWindowDataFlags_HasSize))
			ImGui::SetNextWindowSize(ImVec2(ThemeManager::Constants::POPUP_BUTTON_WIDTH * GetUIScale() * 2.0f +
												ImGui::GetStyle().WindowPadding.x * 2.0f + ImGui::GetStyle().ItemSpacing.x,
										 0.0f),
				ImGuiCond_Appearing);
		DialogSizeAnimation animation;
		PrepareDialogSizeAnimation(GetOpenDialogWindow(id), flags, animation);
		const bool visible = [&] {
			ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, ImGui::GetStyle().WindowRounding);
			ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_PopupBg));
			const SKSE::stl::scope_exit restoreStyle([]() noexcept { ImGui::PopStyleColor(); ImGui::PopStyleVar(); });
			flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings;
			return modal ? ImGui::BeginPopupModal(id, open, flags) : ImGui::BeginPopup(id, flags);
		}();
		if (!visible)
			return false;
		const bool closable = !modal || open;
		const float size = ImGui::GetFrameHeight();
		const auto start = ImGui::GetCursorScreenPos();
		const float right = start.x + ImGui::GetContentRegionAvail().x;
		const float textRight = closable ? right - size - ImGui::GetStyle().ItemSpacing.x : right;
		ImGui::PushClipRect(start, ImVec2(std::max(start.x, textRight), start.y + size), true);
		ImGui::AlignTextToFramePadding();
		const std::string_view label(title);
		const auto visibleLabel = label.substr(0, label.find("##"));
		ImGui::TextUnformatted(visibleLabel.data(), visibleLabel.data() + visibleLabel.size());
		ImGui::PopClipRect();
		if (closable) {
			ImGui::SameLine();
			ImGui::SetCursorScreenPos(ImVec2(right - size, start.y));
			if (CloseButton("##ClosePopup", size)) {
				if (open)
					*open = false;
				ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
				return false;
			}
		}
		return true;
	}

	Popup::Popup(const char* id, const char* title, ImGuiWindowFlags flags) :
		isOpen(BeginDialogPopup(id, title, nullptr, false, flags))
	{}

	Popup::~Popup()
	{
		if (isOpen)
			ImGui::EndPopup();
	}

	CenteredPopupModal::CenteredPopupModal(const char* name, bool* p_open, ImGuiWindowFlags flags, ImVec2 pos, ImVec2 pivot)
	{
		if (pos.x == -FLT_MAX && pos.y == -FLT_MAX)
			pos = ImGui::GetMainViewport()->GetCenter();
		ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
		isOpen = BeginDialogPopup(name, name, p_open, true, flags);
	}

	CenteredPopupModal::~CenteredPopupModal()
	{
		if (isOpen)
			ImGui::EndPopup();
	}

	DisableGuard::DisableGuard(bool disable) :
		disable(disable)
	{
		if (disable)
			ImGui::BeginDisabled();
	}
	DisableGuard::~DisableGuard()
	{
		if (disable)
			ImGui::EndDisabled();
	}

	void TextUnformattedDisabled(const char* a_text, const char* a_textEnd)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ImGui::TextUnformatted(a_text, a_textEnd);
		ImGui::PopStyleColor();
	}

	bool TableRowSelectable(const char* label, bool selected, ImGuiSelectableFlags flags)
	{
		const ImVec4 kTransparent(0.0f, 0.0f, 0.0f, 0.0f);
		ImGui::PushStyleColor(ImGuiCol_Header, kTransparent);
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, kTransparent);
		ImGui::PushStyleColor(ImGuiCol_HeaderActive, kTransparent);

		bool pressed = ImGui::Selectable(label, selected, flags, ImVec2(0, ImGui::GetFrameHeight()));
		bool hovered = ImGui::IsItemHovered();
		bool active = ImGui::IsItemActive();
		ImGui::PopStyleColor(3);

		if (active || hovered) {
			const ImGuiCol highlightCol = active ? ImGuiCol_HeaderActive : ImGuiCol_HeaderHovered;
			const ImU32 rowColor = ImGui::GetColorU32(highlightCol);
			ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, rowColor);
			ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, rowColor);
		} else if (selected) {
			const ImU32 rowColor = ImGui::GetColorU32(ImGuiCol_Header);
			ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, rowColor);
			ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, rowColor);
		}

		return pressed;
	}

	void SetTooltipPositionNearMouse(float estimatedHeight, float estimatedWidth)
	{
		const ImVec2 mousePos = ImGui::GetMousePos();
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		constexpr float kTooltipOffsetX = 16.0f;
		constexpr float kTooltipOffsetY = 12.0f;

		const float viewportLeft = viewport->WorkPos.x;
		const float viewportRight = viewport->WorkPos.x + viewport->WorkSize.x;
		const float viewportTop = viewport->WorkPos.y;
		const float viewportBottom = viewport->WorkPos.y + viewport->WorkSize.y;

		// Vertical: flip above cursor when it would overflow the bottom.
		const bool placeAboveCursor = (mousePos.y + kTooltipOffsetY + estimatedHeight) > viewportBottom;
		float posY;
		float pivotY;
		if (placeAboveCursor) {
			const float tentativeTopY = mousePos.y - kTooltipOffsetY - estimatedHeight;
			posY = (tentativeTopY < viewportTop) ? (viewportTop + estimatedHeight) : (mousePos.y - kTooltipOffsetY);
			pivotY = 1.0f;
		} else {
			posY = mousePos.y + kTooltipOffsetY;
			pivotY = 0.0f;
		}

		// Horizontal: clamp so the tooltip stays within viewport bounds.
		float posX = mousePos.x + kTooltipOffsetX;
		if (estimatedWidth > 0.0f) {
			const float maxX = viewportRight - estimatedWidth;
			posX = ImMax(viewportLeft, ImMin(posX, maxX));
		}

		ImGui::SetNextWindowPos(ImVec2(posX, posY), ImGuiCond_Always, ImVec2(0.0f, pivotY));
	}

	void AddTooltip(const char* a_desc, ImGuiHoveredFlags a_flags)
	{
		if (ImGui::IsItemHovered(a_flags)) {
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 8, 8 });
			const ImVec2 pad = ImGui::GetStyle().WindowPadding;
			const float wrapWidth = ImGui::GetFontSize() * 50.0f;
			const ImVec2 wrappedTextSize = ImGui::CalcTextSize(a_desc, nullptr, false, wrapWidth);
			const float estimatedTooltipHeight = wrappedTextSize.y + pad.y * 2.0f;
			const float estimatedTooltipWidth = wrappedTextSize.x + pad.x * 2.0f;
			SetTooltipPositionNearMouse(estimatedTooltipHeight, estimatedTooltipWidth);

			if (ImGui::BeginTooltip()) {
				ImGui::PushTextWrapPos(wrapWidth);
				ImGui::TextUnformatted(a_desc);
				ImGui::PopTextWrapPos();
				ImGui::EndTooltip();
			}
			ImGui::PopStyleVar();
		}
	}

	void HelpMarker(const char* a_desc)
	{
		ImGui::AlignTextToFramePadding();
		TextUnformattedDisabled("(?)");
		AddTooltip(a_desc, ImGuiHoveredFlags_DelayShort);
	}

	// Static state for clear shader cache confirmation popup
	static bool showClearCacheConfirmation = false;
	static bool dontAskAgainCheckbox = false;
	static ShaderCacheClearScope pendingClearScope = ShaderCacheClearScope::Full;

	ShaderCacheClearScope ResolveShaderCacheClearScope()
	{
		auto* menu = globals::menu;
		const bool smartDefault = menu && menu->GetSettings().SmartClearShaderCacheDefault;
		return ResolveShaderCacheClearScope(smartDefault, ImGui::GetIO().KeyShift);
	}

	const char* GetClearShaderCacheTooltip()
	{
		if (ResolveShaderCacheClearScope() == ShaderCacheClearScope::ActiveOnly) {
			return T("ui.clear_shader_cache_tooltip_smart",
				"Clears only the shaders drawing the current scene. They recompile in the "
				"background as the scene redraws; everything else stays cached.");
		}
		return T("menu.clear_shader_cache_tooltip",
			"Clears the shader cache and disk cache (if enabled). "
			"The Shader Cache is the collection of compiled shaders which replace the vanilla shaders at runtime. "
			"The Disk Cache is a collection of compiled shaders on disk. "
			"Clearing will mean that shaders are recompiled only when the game re-encounters them.");
	}

	// Helper function to perform the actual cache clearing
	static void PerformClearShaderCache(ShaderCacheClearScope a_scope)
	{
		auto* shaderCache = globals::shaderCache;
		if (!shaderCache)
			return;

		if (a_scope == ShaderCacheClearScope::ActiveOnly) {
			shaderCache->BeginActiveShaderCapture();
			return;
		}

		shaderCache->Clear();
		if (shaderCache->IsDiskCache()) {
			shaderCache->DeleteDiskCache();
		}
	}

	void RequestClearShaderCacheConfirmation(ShaderCacheClearScope a_scope)
	{
		auto* menu = globals::menu;
		if (!menu)
			return;

		if (globals::shaderCache &&
			(globals::shaderCache->IsCapturingActiveShaders() || globals::shaderCache->IsAwaitingMenuCloseCapture())) {
			return;  // ignore re-trigger while a capture is already in progress
		}

		// If user has opted to skip confirmation, clear immediately
		if (menu->GetSettings().SkipClearCacheConfirmation) {
			PerformClearShaderCache(a_scope);
			return;
		}

		// Show confirmation popup
		pendingClearScope = a_scope;
		showClearCacheConfirmation = true;
		dontAskAgainCheckbox = false;
	}

	void DrawClearShaderCacheConfirmation()
	{
		if (!showClearCacheConfirmation)
			return;

		const bool isSmart = pendingClearScope == ShaderCacheClearScope::ActiveOnly;
		const char* title = isSmart ? T("ui.smart_clear_shader_cache", "Clear Active Shaders?") : T("ui.clear_shader_cache", "Clear Shader Cache?");

		ImGui::OpenPopup(title);

		if (auto popup = CenteredPopupModal(title, &showClearCacheConfirmation)) {
			ImGui::Text("%s", T("ui.clear_cache_confirm", "Are you sure you want to clear the shader cache?"));
			ImGui::Spacing();
			ImGui::Spacing();
			if (isSmart) {
				ImGui::TextWrapped(
					"%s", T("ui.smart_clear_cache_desc",
							  "Watches the next second of rendering and clears only the shaders drawing this scene, "
							  "twice - once now, and once after this menu closes to catch what it was covering. "
							  "Everything else stays cached.\n\n"
							  "The cleared shaders recompile in the background as the scene redraws, so expect a "
							  "brief stutter and some surfaces reverting to their vanilla appearance until it "
							  "finishes. That is expected - this clears far less than a full wipe, it does not "
							  "make the recompile invisible.\n\n"
							  "Keep the broken area visible while it captures. If the problem persists, use a full clear."));
			} else {
				ImGui::TextWrapped(
					"%s", T("ui.clear_cache_desc",
							  "This will clear all compiled shaders from memory and disk cache (if enabled). "
							  "Shaders will be recompiled when the game next encounters them."));
			}
			ImGui::Spacing();
			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();

			ImGui::Checkbox(T("ui.dont_ask_again", "Don't ask me again"), &dontAskAgainCheckbox);

			ImGui::Spacing();

			// Center buttons
			constexpr float buttonWidth = ThemeManager::Constants::POPUP_BUTTON_WIDTH;
			const float spacing = ImGui::GetStyle().ItemSpacing.x;
			const float totalWidth = buttonWidth * 2 + spacing;
			const float windowWidth = ImGui::GetWindowWidth();
			const float offset = (windowWidth - totalWidth) * 0.5f;
			if (offset > 0)
				ImGui::SetCursorPosX(offset);

			if (ImGui::Button(T("ui.clear_cache", "Clear Cache"), ImVec2(buttonWidth, 0))) {
				// Save preference if checkbox is checked
				if (dontAskAgainCheckbox) {
					if (auto* menu = globals::menu) {
						menu->GetSettings().SkipClearCacheConfirmation = true;
					}
				}

				PerformClearShaderCache(pendingClearScope);
				showClearCacheConfirmation = false;
				ImGui::CloseCurrentPopup();
			}

			ImGui::SameLine();

			if (ImGui::Button(T("ui.cancel", "Cancel"), ImVec2(buttonWidth, 0))) {
				showClearCacheConfirmation = false;
				ImGui::CloseCurrentPopup();
			}
		}
	}

	// --- Reusable ConfirmationPopup ---

	void ConfirmationPopup::Request()
	{
		if (dontAskAgainPersist && *dontAskAgainPersist) {
			confirmed = true;
			return;
		}
		show = true;
		confirmed = false;
		dontAskCheckbox = false;
	}

	bool ConfirmationPopup::Draw()
	{
		if (confirmed) {
			confirmed = false;
			return true;
		}
		if (!show)
			return false;

		ImGui::OpenPopup(title.c_str());

		bool result = false;
		if (auto popup = CenteredPopupModal(title.c_str(), &show)) {
			ImGui::TextWrapped("%s", message.c_str());
			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();

			if (showDontAskAgain)
				ImGui::Checkbox(T("ui.dont_ask_again", "Don't ask me again"), &dontAskCheckbox);

			const auto buttonWidthForLabel = [](const std::string& label) {
				return ImGui::CalcTextSize(label.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
			};
			const float buttonWidth = std::max({
				ThemeManager::Constants::POPUP_BUTTON_WIDTH * GetUIScale(),
				buttonWidthForLabel(confirmLabel),
				buttonWidthForLabel(cancelLabel),
			});
			const float spacing = ImGui::GetStyle().ItemSpacing.x;
			const float totalWidth = buttonWidth * 2 + spacing;
			const float offset = (ImGui::GetWindowWidth() - totalWidth) * 0.5f;
			if (offset > 0)
				ImGui::SetCursorPosX(offset);

			if (ImGui::Button(confirmLabel.c_str(), ImVec2(buttonWidth, 0))) {
				if (showDontAskAgain && dontAskCheckbox && dontAskAgainPersist)
					*dontAskAgainPersist = true;
				result = true;
				show = false;
				ImGui::CloseCurrentPopup();
			}

			ImGui::SameLine();

			if (ImGui::Button(cancelLabel.c_str(), ImVec2(buttonWidth, 0))) {
				show = false;
				ImGui::CloseCurrentPopup();
			}
		}
		return result;
	}

	bool PercentageSlider(const char* label, float* data, float lb, float ub, const char* format)
	{
		float percentageData = (*data) * 1e2f;
		bool retval = ImGui::SliderFloat(label, &percentageData, lb, ub, format);
		(*data) = percentageData * 1e-2f;
		return retval;
	}

	bool InvertedCheckbox(const char* label, bool* storedValue)
	{
		if (!storedValue)
			return false;

		ActiveControlStorageGuard storageGuard(storedValue);
		bool displayValue = !*storedValue;
		const bool changed = ImGui::Checkbox(label, &displayValue);
		if (changed)
			*storedValue = !displayValue;
		return changed;
	}

	bool RadioButton(const char* label, unsigned int* storedValue, unsigned int buttonValue)
	{
		if (!storedValue)
			return false;

		ActiveControlStorageGuard storageGuard(storedValue);
		int displayValue = static_cast<int>(*storedValue);
		const bool changed = ImGui::RadioButton(label, &displayValue, static_cast<int>(buttonValue));
		if (changed)
			*storedValue = static_cast<unsigned int>(displayValue);
		return changed;
	}

	const void* GetActiveControlStorageAddress()
	{
		return g_activeControlStorageAddress;
	}

	ImVec2 GetNativeViewportSizeScaled(float scale)
	{
		const auto Size = ImGui::GetMainViewport()->Size;
		return { Size.x * scale, Size.y * scale };
	}

	bool InitializeMenuIcons(Menu* menu)
	{
		return IconLoader::InitializeMenuIcons(menu);
	}

	// Text rendering helpers
	ImVec2 DrawSharpText(const char* text, bool alignToPixelGrid, float scale)
	{
		ImVec2 startPos = ImGui::GetCursorPos();

		if (alignToPixelGrid) {
			// Get current position
			ImVec2 pos = ImGui::GetCursorPos();

			// Align to pixel grid for sharper rendering
			pos.x = std::round(pos.x);
			pos.y = std::round(pos.y);

			// Set aligned position
			ImGui::SetCursorPos(pos);
		}
		// Apply scale if needed
		if (scale != 1.0f) {
			ImGui::SetWindowFontScale(scale);
		}

		// Use Text instead of TextUnformatted for better rendering
		ImGui::Text("%s", text);
		// Restore original scale if needed
		if (scale != 1.0f)
			ImGui::SetWindowFontScale(1.0f);

		// Calculate and return the rendered size
		ImVec2 endPos = ImGui::GetCursorPos();
		return ImVec2(endPos.x - startPos.x, endPos.y - startPos.y);
	}

	ImVec2 DrawAlignedTextWithLogo(ID3D11ShaderResourceView* logoTexture, const ImVec2& logoSize, const char* text, float textScale, ImU32 logoTint)
	{
		// Save current cursor position
		ImVec2 startPos = ImGui::GetCursorPos();

		// Calculate scaled text height
		float fontHeight = ImGui::GetFontSize() * textScale;
		float logoHeight = logoSize.y;

		// Calculate vertical offset to center align logo with text
		float verticalOffset = (fontHeight - logoHeight) * 0.5f;

		// Position cursor for logo with vertical alignment
		ImGui::SetCursorPos(ImVec2(startPos.x, startPos.y + verticalOffset));

		// Render logo using draw list with tint color support
		ImVec2 logoPos = ImGui::GetCursorScreenPos();
		ImVec2 logoMin = logoPos;
		ImVec2 logoMax = ImVec2(logoPos.x + logoSize.x, logoPos.y + logoSize.y);
		ImGui::GetWindowDrawList()->AddImage(logoTexture, logoMin, logoMax, ImVec2(0, 0), ImVec2(1, 1), logoTint);

		// Advance cursor past logo
		ImGui::Dummy(logoSize);
		ImGui::SameLine();

		// Add consistent spacing between logo and text
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8.0f);

		// Reset cursor for text with proper vertical alignment
		ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX(), startPos.y));
		// Use windowed font scale for sharper text
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
		ImGui::SetWindowFontScale(textScale);

		// Render text aligned to pixel grid for sharpness
		ImGui::Text("%s", text);
		// Restore style
		ImGui::SetWindowFontScale(1.0f);
		ImGui::PopStyleVar();

		// Calculate and return the total rendered size
		ImVec2 endPos = ImGui::GetCursorPos();
		return ImVec2(endPos.x - startPos.x, endPos.y - startPos.y);
	}

	float GetCenterOffsetForContent(float contentWidth)
	{
		// Get full window width for true centering
		float fullWindowWidth = ImGui::GetWindowWidth();
		float windowPaddingX = ImGui::GetStyle().WindowPadding.x;
		float availableFullWidth = fullWindowWidth - (windowPaddingX * 2.0f);

		// Calculate center position
		float centerOffset = (availableFullWidth - contentWidth) * 0.5f;

		// Adjust for current cursor position
		float currentX = ImGui::GetCursorPosX();
		float targetX = windowPaddingX + centerOffset;
		float offset = targetX - currentX;

		return offset > 0.0f ? offset : 0.0f;
	}

	// StyledButtonWrapper implementation
	StyledButtonWrapper::StyledButtonWrapper(const ImVec4& normalColor, const ImVec4& hoveredColor, const ImVec4& activeColor) :
		m_pushedStyles(0)
	{
		ImGui::PushStyleColor(ImGuiCol_Button, normalColor);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hoveredColor);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, activeColor);
		m_pushedStyles = 3;
	}

	StyledButtonWrapper::~StyledButtonWrapper()
	{
		if (m_pushedStyles > 0) {
			ImGui::PopStyleColor(m_pushedStyles);
		}
	}

	namespace ButtonHelpers
	{
		constexpr ImVec4 kUnlockButtonColor = ImVec4(0.2f, 0.8f, 0.2f, 1.0f);
		constexpr ImVec4 kUnlockButtonHoverColor = ImVec4(0.3f, 0.9f, 0.3f, 1.0f);
		constexpr ImVec4 kLockButtonColor = ImVec4(0.8f, 0.2f, 0.2f, 1.0f);
		constexpr ImVec4 kLockButtonHoverColor = ImVec4(0.9f, 0.3f, 0.3f, 1.0f);

		ImVec4 AdjustButtonColor(const ImVec4& color, float amount)
		{
			const float maxChannel = std::max({ color.x, color.y, color.z });
			const float minChannel = ThemeManager::Constants::BUTTON_MIN_COLOR_CHANNEL;
			const float maxColorChannel = ThemeManager::Constants::BUTTON_MAX_COLOR_CHANNEL;
			const float adjustment = maxChannel <= (maxColorChannel - amount) ? amount : -amount;
			return ImVec4(
				std::clamp(color.x + adjustment, minChannel, maxColorChannel),
				std::clamp(color.y + adjustment, minChannel, maxColorChannel),
				std::clamp(color.z + adjustment, minChannel, maxColorChannel),
				color.w);
		}

		ImVec4 WithAlpha(const ImVec4& color, float alpha)
		{
			return ImVec4(color.x, color.y, color.z, alpha);
		}

		template <typename StyleFn, typename ButtonFn>
		bool InvokeStyledButton(StyleFn styleProvider, ButtonFn buttonCall)
		{
			auto _style = styleProvider();
			return buttonCall();
		}
	}

	StyledButtonWrapper StatusButtonStyle(const ImVec4& color)
	{
		auto hover = ButtonHelpers::AdjustButtonColor(color, ThemeManager::Constants::BUTTON_HOVER_BRIGHTEN);
		auto active = ButtonHelpers::AdjustButtonColor(color, ThemeManager::Constants::BUTTON_ACTIVE_BRIGHTEN);
		return StyledButtonWrapper(color, hover, active);
	}

	StyledButtonWrapper DestructiveButtonStyle()
	{
		return StatusButtonStyle(Menu::GetSingleton()->GetTheme().StatusPalette.Error);
	}

	bool ErrorButton(const char* label, const ImVec2& size)
	{
		return ButtonHelpers::InvokeStyledButton(DestructiveButtonStyle, [&] { return ImGui::Button(label, size); });
	}

	bool ErrorButtonWithFlash(const char* label, const ImVec2& size, int flashDurationMs)
	{
		return ButtonHelpers::InvokeStyledButton(DestructiveButtonStyle, [&] { return ButtonWithFlash(label, size, flashDurationMs); });
	}

	StyledButtonWrapper StatusTextButtonStyle(const ImVec4& color)
	{
		return StyledButtonWrapper(color,
			ButtonHelpers::WithAlpha(color, ThemeManager::Constants::BUTTON_STATUS_TEXT_HOVER_ALPHA),
			ButtonHelpers::WithAlpha(color, ThemeManager::Constants::BUTTON_STATUS_TEXT_ACTIVE_ALPHA));
	}

	StyledButtonWrapper SuccessButtonStyle()
	{
		return StatusTextButtonStyle(Menu::GetSingleton()->GetTheme().StatusPalette.SuccessColor);
	}

	StyledButtonWrapper WarningButtonStyle()
	{
		return StatusTextButtonStyle(Menu::GetSingleton()->GetTheme().StatusPalette.Warning);
	}

	bool SuccessButton(const char* label, const ImVec2& size)
	{
		return ButtonHelpers::InvokeStyledButton(SuccessButtonStyle, [&] { return ImGui::Button(label, size); });
	}

	bool WarningButton(const char* label, const ImVec2& size)
	{
		return ButtonHelpers::InvokeStyledButton(WarningButtonStyle, [&] { return ImGui::Button(label, size); });
	}

	StyledButtonWrapper UnlockButtonStyle()
	{
		return StyledButtonWrapper(ButtonHelpers::kUnlockButtonColor, ButtonHelpers::kUnlockButtonHoverColor, ButtonHelpers::kUnlockButtonHoverColor);
	}

	bool UnlockButton(const char* label, const ImVec2& size)
	{
		return ButtonHelpers::InvokeStyledButton(UnlockButtonStyle, [&] { return ImGui::Button(label, size); });
	}

	StyledButtonWrapper LockButtonStyle()
	{
		return StyledButtonWrapper(ButtonHelpers::kLockButtonColor, ButtonHelpers::kLockButtonHoverColor, ButtonHelpers::kLockButtonHoverColor);
	}

	bool LockButton(const char* label, const ImVec2& size)
	{
		return ButtonHelpers::InvokeStyledButton(LockButtonStyle, [&] { return ImGui::Button(label, size); });
	}

	bool ErrorTextButton(const char* label, const ImVec2& size)
	{
		return ButtonHelpers::InvokeStyledButton(
			[] { return StatusTextButtonStyle(Menu::GetSingleton()->GetTheme().StatusPalette.Error); },
			[&] { return ImGui::Button(label, size); });
	}

	StyledButtonWrapper TransparentIconButtonStyle()
	{
		constexpr float kHoverAlpha = 0.25f;
		auto hoverColor = Menu::GetSingleton()->GetTheme().Palette.Text;
		hoverColor.w = kHoverAlpha;
		return StyledButtonWrapper(ImVec4(0, 0, 0, 0), hoverColor, hoverColor);
	}

	ImVec4 GetIconTint()
	{
		const auto& theme = Menu::GetSingleton()->GetTheme();
		return theme.UseMonochromeIcons ? theme.Palette.Text : ImVec4(1, 1, 1, 1);
	}

	static float GetPillRounding(const ImVec2& min, const ImVec2& max)
	{
		IM_ASSERT(max.x >= min.x && max.y >= min.y);
		return ImMin(max.x - min.x, max.y - min.y) * 0.5f;
	}

	static float GetThemedButtonHighlightRounding(const ImVec2& min, const ImVec2& max)
	{
		const float frameRounding = ImGui::GetStyle().FrameRounding;
		IM_ASSERT(frameRounding >= 0.0f);
		return ImMin(ImMax(frameRounding, 0.0f), GetPillRounding(min, max));
	}

	bool DrawRoundedButtonHighlight(const ImVec2& min, const ImVec2& max, bool hovered, bool active, ImDrawList* drawList)
	{
		return DrawRoundedButtonHighlight(min, max, hovered, active, GetThemedButtonHighlightRounding(min, max), drawList);
	}

	bool DrawRoundedButtonHighlight(const ImRect& rect, bool hovered, bool active, ImDrawList* drawList)
	{
		return DrawRoundedButtonHighlight(rect.Min, rect.Max, hovered, active, drawList);
	}

	bool DrawRoundedButtonHighlight(const ImVec2& min, const ImVec2& max, bool hovered, bool active, float rounding, ImDrawList* drawList)
	{
		if (!hovered && !active)
			return false;

		IM_ASSERT(max.x >= min.x && max.y >= min.y);
		IM_ASSERT(rounding >= 0.0f);
		if (!drawList)
			drawList = ImGui::GetWindowDrawList();

		drawList->AddRectFilled(min, max, ImGui::GetColorU32(active ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered), rounding);
		return true;
	}

	bool DrawCurrentItemRoundedButtonHighlight(ImDrawList* drawList)
	{
		return DrawRoundedButtonHighlight(ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()), ImGui::IsItemHovered(), ImGui::IsItemActive(), drawList);
	}

	void DrawDisclosureChevron(ImDrawList* drawList, const ImVec2& min, const ImVec2& max, float progress)
	{
		IM_ASSERT(drawList != nullptr);
		IM_ASSERT(max.x >= min.x && max.y >= min.y);

		constexpr float radiusRatio = 0.30f;
		constexpr float horizontalRatio = 0.866f;
		constexpr float verticalRatio = 0.75f;
		const ImVec2 center((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
		const float radius = std::min(max.x - min.x, max.y - min.y) * radiusRatio;
		const float angle = std::numbers::pi_v<float> * std::clamp(progress, 0.0f, 1.0f);
		const float cosine = std::cos(angle);
		const float sine = std::sin(angle);

		std::array<ImVec2, 3> points = {
			ImVec2(0.0f, verticalRatio * radius),
			ImVec2(-horizontalRatio * radius, -verticalRatio * radius),
			ImVec2(horizontalRatio * radius, -verticalRatio * radius)
		};
		for (auto& point : points) {
			const ImVec2 rotated(
				point.x * cosine - point.y * sine,
				point.x * sine + point.y * cosine);
			point = ImVec2(center.x + rotated.x, center.y + rotated.y);
		}

		drawList->AddTriangleFilled(points[0], points[1], points[2], ImGui::GetColorU32(ImGuiCol_Text));
	}

	void DrawStarIcon(ImDrawList* drawList, const ImVec2& min, const ImVec2& max, ImU32 color)
	{
		constexpr int pointCount = 5;
		constexpr int vertexCount = pointCount * 2;
		constexpr float innerRadiusRatio = 0.381966f;
		const ImVec2 center((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
		const float outerRadius = std::min(max.x - min.x, max.y - min.y) * 0.5f;
		std::array<ImVec2, vertexCount> points;
		for (int i = 0; i < vertexCount; ++i) {
			const float angle = std::numbers::pi_v<float> * (static_cast<float>(i) / pointCount - 0.5f);
			const float radius = outerRadius * (i % 2 == 0 ? 1.0f : innerRadiusRatio);
			points[i] = ImVec2(center.x + std::cos(angle) * radius, center.y + std::sin(angle) * radius);
		}
		drawList->AddConcavePolyFilled(points.data(), vertexCount, color);
	}

	bool FlyoutMenuItem(const char* label, std::optional<bool> selected, bool enabled, float checkmarkLeftOffset, IconDrawCallback selectedIcon)
	{
		IM_ASSERT(checkmarkLeftOffset >= 0.0f);

		auto* window = ImGui::GetCurrentWindow();
		if (window->SkipItems)
			return false;

		auto* drawList = ImGui::GetWindowDrawList();
		IM_ASSERT(drawList->_Splitter._Count == 1);
		drawList->ChannelsSplit(2);
		const SKSE::stl::scope_exit mergeChannels([drawList]() noexcept { drawList->ChannelsMerge(); });
		drawList->ChannelsSetCurrent(1);

		bool pressed;
		{
			constexpr ImVec4 transparent(0.0f, 0.0f, 0.0f, 0.0f);
			ImGui::PushStyleColor(ImGuiCol_Header, transparent);
			ImGui::PushStyleColor(ImGuiCol_HeaderHovered, transparent);
			ImGui::PushStyleColor(ImGuiCol_HeaderActive, transparent);
			ImGui::PushStyleColor(ImGuiCol_NavCursor, transparent);
			const SKSE::stl::scope_exit restoreColors([]() noexcept { ImGui::PopStyleColor(4); });

			if (!selected.has_value()) {
				constexpr ImGuiSelectableFlags flags = ImGuiSelectableFlags_SelectOnRelease | ImGuiSelectableFlags_NoHoldingActiveID |
				                                       ImGuiSelectableFlags_SetNavIdOnHover | ImGuiSelectableFlags_SpanAvailWidth;
				pressed = ImGui::Selectable(label, false, flags | (enabled ? ImGuiSelectableFlags_None : ImGuiSelectableFlags_Disabled));
			} else {
				auto& menuColumns = window->DC.MenuColumns;
				const ImU16 originalMarkOffset = menuColumns.OffsetMark;
				ImU16 baseMarkOffset = originalMarkOffset;
				if (baseMarkOffset == 0) {
					const float fallbackMarkOffset = std::trunc(ImGui::CalcTextSize(label, nullptr, true).x) + menuColumns.Spacing;
					baseMarkOffset = static_cast<ImU16>(std::clamp(
						fallbackMarkOffset, 0.0f, static_cast<float>(std::numeric_limits<ImU16>::max())));
				}
				const auto markShift = static_cast<ImU16>(std::clamp(
					std::lround(checkmarkLeftOffset), 0L, static_cast<long>(baseMarkOffset)));
				menuColumns.OffsetMark = static_cast<ImU16>(baseMarkOffset - (*selected ? markShift : 0));
				const SKSE::stl::scope_exit restoreMarkOffset(
					[&menuColumns, originalMarkOffset]() noexcept { menuColumns.OffsetMark = originalMarkOffset; });

				const ImVec2 textPos(window->DC.CursorPos.x, window->DC.CursorPos.y + window->DC.CurrLineTextBaseOffset);
				const float availableWidth = ImGui::GetContentRegionAvail().x;
				pressed = ImGui::MenuItem(label, nullptr, *selected && !selectedIcon, enabled);
				if (*selected && selectedIcon) {
					constexpr float checkmarkOffsetXRatio = 0.40f;
					constexpr float checkmarkOffsetYRatio = 0.134f * 0.5f;
					constexpr float checkmarkSizeRatio = 0.866f;
					const float fontSize = ImGui::GetFontSize();
					const float iconSize = fontSize * checkmarkSizeRatio;
					const float minimumWidth = static_cast<float>(std::max(menuColumns.TotalWidth, menuColumns.NextTotalWidth));
					const float stretchWidth = std::max(0.0f, availableWidth - minimumWidth);
					const ImVec2 iconMin(textPos.x + menuColumns.OffsetMark + stretchWidth + fontSize * checkmarkOffsetXRatio,
						textPos.y + fontSize * checkmarkOffsetYRatio);
					selectedIcon(drawList, iconMin, ImVec2(iconMin.x + iconSize, iconMin.y + iconSize),
						ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled));
				}
			}
		}

		const ImRect itemRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
		const bool hovered = ImGui::IsItemHovered();
		const bool active = ImGui::IsItemActive();
		const bool focused = ImGui::IsItemFocused();

		drawList->ChannelsSetCurrent(0);
		if (hovered || active || focused) {
			drawList->AddRectFilled(
				itemRect.Min,
				itemRect.Max,
				ImGui::GetColorU32(active ? ImGuiCol_HeaderActive : ImGuiCol_HeaderHovered),
				GetThemedButtonHighlightRounding(itemRect.Min, itemRect.Max));
		}

		return pressed;
	}

	// Shared constants for title-bar button overlays
	static constexpr float kTitleBarButtonPadding = 2.0f;
	static constexpr float kCloseCrossDiagonalScale = 0.5f / std::numbers::sqrt2_v<float>;
	static constexpr float kCloseCrossInset = 1.0f;
	static constexpr ImVec4 kTransparentButtonChrome(0, 0, 0, 0);

	static ImRect TitleBarButtonRect(const ImVec2& origin, float fontSize)
	{
		const float full = fontSize + kTitleBarButtonPadding * 2.0f;
		return ImRect(origin, ImVec2(origin.x + full, origin.y + full));
	}

	static ImVec2 RightTitleBarButtonOrigin(ImGuiWindow* window, float fontSize, float offset = 0.0f)
	{
		const auto& style = ImGui::GetStyle();
		return ImVec2(window->Rect().Max.x - window->WindowBorderSize - style.FramePadding.x - fontSize - offset - kTitleBarButtonPadding,
			window->Rect().Min.y + style.FramePadding.y - kTitleBarButtonPadding);
	}

	static ImVec2 CollapseTitleBarButtonOrigin(ImGuiWindow* window, bool hasCloseButton, float fontSize)
	{
		const auto& style = ImGui::GetStyle();
		IM_ASSERT(style.WindowMenuButtonPosition == ImGuiDir_Left || style.WindowMenuButtonPosition == ImGuiDir_Right);

		if (style.WindowMenuButtonPosition == ImGuiDir_Right)
			return RightTitleBarButtonOrigin(window, fontSize, hasCloseButton ? fontSize : 0.0f);

		return ImVec2(window->Pos.x + window->WindowBorderSize + style.FramePadding.x - kTitleBarButtonPadding,
			window->Pos.y + style.FramePadding.y - kTitleBarButtonPadding);
	}

	static bool IsTitleBarButtonHovered(ImGuiWindow* window, const ImRect& bb)
	{
		ImGuiContext& g = *ImGui::GetCurrentContext();
		return g.HoveredWindow == window && ImGui::IsMouseHoveringRect(bb.Min, bb.Max, false);
	}

	class NativeTitleBarButtonHighlightGuard
	{
	public:
		NativeTitleBarButtonHighlightGuard()
		{
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kTransparentButtonChrome);
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, kTransparentButtonChrome);
		}

		~NativeTitleBarButtonHighlightGuard() { ImGui::PopStyleColor(2); }
	};

	// Draws a rounded close button overlay, matching native ImGui CloseButton position.
	static void DrawRoundedCloseHighlight(ImGuiWindow* window)
	{
		if (window->Flags & ImGuiWindowFlags_NoTitleBar)
			return;

		const float sz = ImGui::GetFontSize();
		const ImVec2 pos = RightTitleBarButtonOrigin(window, sz);
		const ImRect bb = TitleBarButtonRect(pos, sz);
		const bool hovered = IsTitleBarButtonHovered(window, bb);
		const bool held = hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left);

		window->DrawList->PushClipRect(window->Rect().Min, window->Rect().Max);
		const bool highlighted = DrawRoundedButtonHighlight(bb, hovered, held, window->DrawList);

		// Cross lines match ImGui's internal RenderCloseButton geometry.
		if (highlighted) {
			const ImVec2 c = bb.GetCenter();
			const float d = sz * kCloseCrossDiagonalScale - kCloseCrossInset;
			const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
			window->DrawList->AddLine({ c.x - d, c.y - d }, { c.x + d, c.y + d }, col);
			window->DrawList->AddLine({ c.x + d, c.y - d }, { c.x - d, c.y + d }, col);
		}
		window->DrawList->PopClipRect();
	}

	// Draws a rounded highlight for the collapse/triangle button in the title bar.
	static void DrawRoundedCollapseHighlight(ImGuiWindow* window, bool hasCloseButton)
	{
		if (window->Flags & ImGuiWindowFlags_NoTitleBar)
			return;
		if (window->Flags & ImGuiWindowFlags_NoCollapse)
			return;
		if (ImGui::GetStyle().WindowMenuButtonPosition == ImGuiDir_None)
			return;

		const float sz = ImGui::GetFontSize();
		const ImVec2 pos = CollapseTitleBarButtonOrigin(window, hasCloseButton, sz);
		const ImRect bb = TitleBarButtonRect(pos, sz);
		const bool hovered = IsTitleBarButtonHovered(window, bb);
		const bool held = hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left);

		window->DrawList->PushClipRect(window->Rect().Min, window->Rect().Max);
		const bool highlighted = DrawRoundedButtonHighlight(bb, hovered, held, window->DrawList);

		if (highlighted) {
			const ImVec2 arrowPos(pos.x + kTitleBarButtonPadding, pos.y + kTitleBarButtonPadding);
			const ImGuiDir dir = window->Collapsed ? ImGuiDir_Right : ImGuiDir_Down;
			ImGui::RenderArrow(window->DrawList, arrowPos, ImGui::GetColorU32(ImGuiCol_Text), dir, 1.0f);
		}

		window->DrawList->PopClipRect();
	}

	static void DrawRoundedTitleBarButtonHighlights(ImGuiWindow* window, bool hasCloseButton, bool hasCollapseButton)
	{
		if (!window)
			return;

		if (hasCollapseButton)
			DrawRoundedCollapseHighlight(window, hasCloseButton);
		if (hasCloseButton)
			DrawRoundedCloseHighlight(window);
	}

	bool BeginWithRoundedClose(const char* name, bool* p_open, ImGuiWindowFlags flags)
	{
		DialogSizeAnimation animation;
		PrepareDialogSizeAnimation(ImGui::FindWindowByName(name), flags, animation);
		bool visible = false;
		{
			NativeTitleBarButtonHighlightGuard guard;
			visible = ImGui::Begin(name, p_open, flags);
		}
		DrawRoundedTitleBarButtonHighlights(ImGui::GetCurrentWindowRead(), p_open != nullptr, true);
		return visible;
	}

	bool BeginPopupModalWithRoundedClose(const char* name, bool* p_open, ImGuiWindowFlags flags)
	{
		DialogSizeAnimation animation;
		PrepareDialogSizeAnimation(GetOpenDialogWindow(name), flags, animation);
		bool visible = false;
		{
			NativeTitleBarButtonHighlightGuard guard;
			visible = ImGui::BeginPopupModal(name, p_open, flags);
		}
		if (visible)
			DrawRoundedTitleBarButtonHighlights(ImGui::GetCurrentWindowRead(), p_open != nullptr, false);
		return visible;
	}

	// SectionWrapper implementation
	SectionWrapper::SectionWrapper(const char* title, const char* description, const ImVec4& titleColor, bool isVisible) :
		m_shouldDraw(isVisible),
		m_treeNodeOpened(false)
	{
		if (!m_shouldDraw) {
			return;
		}

		ImGui::TextColored(titleColor, "%s", title);
		ImGui::Spacing();

		if (description && strlen(description) > 0) {
			ImGui::TextWrapped("%s", description);
			ImGui::Spacing();
		}
	}

	SectionWrapper::~SectionWrapper()
	{
		if (m_shouldDraw) {
			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();
		}
	}

	SectionWrapper::operator bool() const
	{
		return m_shouldDraw;
	}

	ID3D11ShaderResourceView* GetCategoryIcon(std::string_view category)
	{
		ID3D11ShaderResourceView* categoryIcon = nullptr;
		auto& menu = globals::menu->uiIcons;

		if (category == "Characters") {
			categoryIcon = menu.characters.texture;
		} else if (category == "Display") {
			categoryIcon = menu.display.texture;
		} else if (category == "Foliage") {
			categoryIcon = menu.foliage.texture;
		} else if (category == "Lighting") {
			categoryIcon = menu.lighting.texture;
		} else if (category == "Sky") {
			categoryIcon = menu.sky.texture;
		} else if (category == "Landscape & Textures") {
			categoryIcon = menu.landscape.texture;
		} else if (category == "Water") {
			categoryIcon = menu.water.texture;
		} else if (category == "Utility") {
			categoryIcon = menu.debug.texture;
		} else if (category == "Materials") {
			categoryIcon = menu.materials.texture;
		} else if (category == "Post-Processing") {
			categoryIcon = menu.postProcessing.texture;
		}

		return categoryIcon;
	}

	bool DrawSectionHeader(const char* sectionName, bool useWhiteText, bool isCollapsible, bool* isExpanded, IconDrawCallback icon)
	{
		bool stateChanged = false;

		// Use Menu theme colors for consistent styling
		auto& theme = globals::menu->GetTheme().FeatureHeading;
		auto& palette = globals::menu->GetTheme().Palette;
		// When useWhiteText is true, use the theme's text color instead of hardcoded white
		ImVec4 color = useWhiteText ? palette.Text : theme.ColorDefault;

		ImU32 headerColor = ImGui::GetColorU32(color);

		if (isCollapsible && isExpanded) {
			// Use collapsible header similar to DrawCategoryHeader
			ImGui::PushID(sectionName);

			ImGui::PushStyleColor(ImGuiCol_Text, headerColor);

			if (ImGui::CollapsingHeader(sectionName, ImGuiTreeNodeFlags_DefaultOpen)) {
				if (!*isExpanded) {
					stateChanged = true;
				}
				*isExpanded = true;
			} else {
				if (*isExpanded) {
					stateChanged = true;
				}
				*isExpanded = false;
			}

			ImGui::PopStyleColor();
			ImGui::PopID();
		} else {
			// Non-collapsible header - use custom styled header similar to CategoryHeader
			ImDrawList* drawList = ImGui::GetWindowDrawList();
			ImVec2 pos = ImGui::GetCursorScreenPos();
			float availableWidth = ImGui::GetContentRegionAvail().x;
			ImVec2 textSize = ImGui::CalcTextSize(sectionName);
			const float iconSize = icon ? ImGui::GetFontSize() : 0.0f;
			const float iconWidth = icon ? iconSize + ImGui::GetStyle().ItemInnerSpacing.x : 0.0f;
			const float contentWidth = textSize.x + iconWidth;

			// Calculate line positions
			float lineY = pos.y + textSize.y * 0.5f;
			float lineLength = (availableWidth - contentWidth - 20.0f) * 0.5f;  // 20px for padding

			// Left line
			if (lineLength > 0) {
				drawList->AddLine(ImVec2(pos.x, lineY), ImVec2(pos.x + lineLength, lineY), headerColor, 1.0f);
			}

			// Right line
			float rightLineStart = pos.x + lineLength + 10.0f + contentWidth + 10.0f;
			if (rightLineStart < pos.x + availableWidth) {
				drawList->AddLine(ImVec2(rightLineStart, lineY), ImVec2(pos.x + availableWidth, lineY), headerColor, 1.0f);
			}

			// Center text
			ImVec2 textPos = ImVec2(pos.x + lineLength + 10.0f, pos.y + 2.0f);
			if (icon) {
				const ImVec2 iconMin(textPos.x, textPos.y + (textSize.y - iconSize) * 0.5f);
				icon(drawList, iconMin, ImVec2(iconMin.x + iconSize, iconMin.y + iconSize), headerColor);
				textPos.x += iconWidth;
			}
			drawList->AddText(textPos, headerColor, sectionName);

			// Move cursor to next line
			ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + textSize.y + 8.0f));
			ImGui::Dummy(ImVec2(availableWidth, 0.0f));
		}

		return stateChanged;
	}

	// ColorCodedValueConfig static helper implementations
	ColorCodedValueConfig ColorCodedValueConfig::HighIsBad(float low, float med, float high)
	{
		ColorCodedValueConfig config;
		const auto& theme = globals::menu->GetTheme().StatusPalette;
		config.thresholds = {
			{ low, theme.Disable },    // Very low - gray
			{ med, theme.InfoColor },  // Low - blue
			{ high, theme.Warning },   // Medium - orange
			{ FLT_MAX, theme.Error }   // High - red (bad)
		};
		return config;
	}

	ColorCodedValueConfig ColorCodedValueConfig::HighIsGood(float low, float med, float high)
	{
		ColorCodedValueConfig config;
		const auto& theme = globals::menu->GetTheme().StatusPalette;
		config.thresholds = {
			{ low, theme.Disable },          // Very low - gray
			{ med, theme.InfoColor },        // Low - blue
			{ high, theme.Warning },         // Medium - orange
			{ FLT_MAX, theme.SuccessColor }  // High - green (good)
		};
		return config;
	}

	void DrawColorCodedValue(
		const std::string& label,
		float valueToCheck,
		const std::string& valueStr,
		const ColorCodedValueConfig& config,
		bool useBullet)
	{
		// Display label
		if (useBullet) {
			ImGui::BulletText("%s", label.c_str());
		} else {
			ImGui::Text("%s", label.c_str());
		}
		if (config.sameLine) {
			ImGui::SameLine();
		}

		// Determine color based on thresholds
		ImVec4 valueColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);  // Default white
		for (const auto& tc : config.thresholds) {
			if (valueToCheck < tc.threshold) {
				valueColor = tc.color;
				break;
			}
		}

		// Display colored value (arbitrary string)
		ImGui::TextColored(valueColor, "%s", valueStr.c_str());

		// Add tooltip if provided
		if (config.tooltipText) {
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", config.tooltipText);
			}
		}
	}

	void DrawMultiLineTooltip(const std::vector<std::string>& lines, const std::vector<ImVec4>& colors)
	{
		for (size_t i = 0; i < lines.size(); ++i) {
			const char* lineCStr = lines[i].c_str();
			if (!colors.empty() && i < colors.size()) {
				// Use provided color for this line
				ImGui::TextColored(colors[i], "%s", lineCStr);
			} else {
				// Use default color
				ImGui::Text("%s", lineCStr);
			}
		}
	}

	void DrawColoredMultiLineTooltip(const ColoredTextLines& lines)
	{
		for (const auto& line : lines) {
			ImGui::TextColored(line.color, "%s", line.text.c_str());
		}
	}

	void SortTableRowsByColumn(std::vector<std::vector<std::string>>& rows, size_t column, bool ascending)
	{
		std::sort(rows.begin(), rows.end(), [column, ascending](const auto& a, const auto& b) {
			if (column >= a.size() || column >= b.size())
				return false;
			return ascending ? (a[column] < b[column]) : (a[column] > b[column]);
		});
	}

	bool VersionStringLess(const std::string& a, const std::string& b, bool ascending)
	{
		auto split = [](const std::string& s) {
			std::vector<int> parts;
			size_t start = 0, end = 0;
			while ((end = s.find('.', start)) != std::string::npos) {
				try {
					parts.push_back(std::stoi(s.substr(start, end - start)));
				} catch (...) {
					parts.push_back(0);
				}
				start = end + 1;
			}
			if (start < s.size()) {
				try {
					parts.push_back(std::stoi(s.substr(start)));
				} catch (...) {
					parts.push_back(0);
				}
			}
			return parts;
		};
		auto va = split(a), vb = split(b);
		for (size_t i = 0; i < std::max(va.size(), vb.size()); ++i) {
			int ai = i < va.size() ? va[i] : 0;
			int bi = i < vb.size() ? vb[i] : 0;
			if (ai != bi)
				return ascending ? (ai < bi) : (ai > bi);
		}
		return false;
	}

	bool VersionSortComparator(const std::string& a, const std::string& b, bool asc)
	{
		return VersionStringLess(a, b, asc);
	}

	bool StringSortComparator(const std::string& a, const std::string& b, bool ascending)
	{
		return ascending ? (a < b) : (b < a);
	}

	void RenderTextWithHighlights(const std::string& text, const std::string& searchTerm, ImVec4 highlightColor)
	{
		if (searchTerm.empty()) {
			ImGui::TextUnformatted(text.c_str());
			return;
		}

		std::string lowerText = text;
		std::string lowerSearch = searchTerm;
		std::transform(lowerText.begin(), lowerText.end(), lowerText.begin(), [](unsigned char c) { return static_cast<char>(::tolower(c)); });
		std::transform(lowerSearch.begin(), lowerSearch.end(), lowerSearch.begin(), [](unsigned char c) { return static_cast<char>(::tolower(c)); });

		size_t pos = 0;
		size_t lastPos = 0;

		while ((pos = lowerText.find(lowerSearch, lastPos)) != std::string::npos) {
			// Render text before highlight
			if (pos > lastPos) {
				ImGui::TextUnformatted(text.substr(lastPos, pos - lastPos).c_str());
				ImGui::SameLine(0, 0);
			}

			// Render highlighted text
			ImGui::PushStyleColor(ImGuiCol_Text, highlightColor);
			ImGui::TextUnformatted(text.substr(pos, searchTerm.length()).c_str());
			ImGui::PopStyleColor();
			ImGui::SameLine(0, 0);

			lastPos = pos + searchTerm.length();
		}

		// Render remaining text
		if (lastPos < text.length()) {
			ImGui::TextUnformatted(text.substr(lastPos).c_str());
		}
	}

	ImVec4 GetThresholdColor(float value, float good, float warn, ImVec4 goodColor, ImVec4 warnColor, ImVec4 badColor)
	{
		if (value < good)
			return goodColor;
		else if (value < warn)
			return warnColor;
		else
			return badColor;
	}

	bool FeatureMatchesSearch(Feature* feat, const std::string& searchQuery)
	{
		if (searchQuery.empty())
			return true;

		return StringMatchesSearch(feat->GetShortName(), searchQuery) ||
		       StringMatchesSearch(feat->GetDisplayName(), searchQuery) ||
		       StringMatchesSearch(std::string(feat->GetCategory()), searchQuery) ||
		       StringMatchesSearch(feat->GetDisplayCategory(), searchQuery);
	}

	bool StringMatchesSearch(const std::string& text, const std::string& searchQuery)
	{
		if (searchQuery.empty())
			return true;

		std::string lowerText = text;
		std::string lowerQuery = searchQuery;

		// Convert all to lowercase for case-insensitive search
		std::transform(lowerText.begin(), lowerText.end(), lowerText.begin(), [](unsigned char c) { return static_cast<char>(::tolower(c)); });
		std::transform(lowerQuery.begin(), lowerQuery.end(), lowerQuery.begin(), [](unsigned char c) { return static_cast<char>(::tolower(c)); });

		return lowerText.find(lowerQuery) != std::string::npos;
	}

	void DrawModalBackground(uint8_t alpha)
	{
		auto& io = ImGui::GetIO();
		ImGui::GetBackgroundDrawList()->AddRectFilled(
			ImVec2(0, 0),
			io.DisplaySize,
			IM_COL32(0, 0, 0, alpha));
	}

	void DrawBreathingText(const char* text, float speed, float minAlpha, float maxAlpha)
	{
		float alphaRange = maxAlpha - minAlpha;
		float breathe = minAlpha + alphaRange * 0.5f * (1.0f + sinf((float)ImGui::GetTime() * speed));
		auto& theme = globals::menu->GetTheme().Palette;
		ImVec4 color = ImVec4(theme.Text.x, theme.Text.y, theme.Text.z, breathe);
		ImGui::TextColored(color, "%s", text);
	}

	ImVec4 GetPulsingColor(const ImVec4& baseColor, float speed, float minBrightness, float maxBrightness)
	{
		float brightnessRange = maxBrightness - minBrightness;
		float pulse = minBrightness + brightnessRange * 0.5f * (1.0f + sinf((float)ImGui::GetTime() * speed));
		return ImVec4(
			baseColor.x * pulse,
			baseColor.y * pulse,
			baseColor.z * pulse,
			baseColor.w);
	}

	void DrawSearchIcon(const ImVec2& position, float size, float alpha)
	{
		ImDrawList* drawList = ImGui::GetWindowDrawList();

		ImVec2 center = ImVec2(position.x + size * 0.46f, position.y + size * 0.5f);
		float radius = size * 0.3f;
		const float circleStroke = size * ThemeManager::Constants::SEARCH_ICON_STROKE_RATIO;
		const float handleStroke = size * ThemeManager::Constants::SEARCH_ICON_HANDLE_STROKE_RATIO;

		// Use themed text color with reduced alpha for search icon
		auto& theme = globals::menu->GetTheme().Palette;
		ImVec4 iconColor = theme.Text;
		iconColor.w *= alpha;  // Apply alpha multiplier for subtler appearance
		ImU32 placeholderColor = ImGui::GetColorU32(iconColor);

		// Draw circle
		drawList->AddCircle(center, radius, placeholderColor, 12, circleStroke);

		// Draw handle
		ImVec2 handleStart = ImVec2(center.x + radius * 0.81f, center.y + radius * 0.81f);
		ImVec2 handleEnd = ImVec2(handleStart.x + size * 0.29f, handleStart.y + size * 0.29f);
		drawList->AddLine(handleStart, handleEnd, placeholderColor, handleStroke);
	}

	namespace detail
	{
		constexpr std::size_t kSearchableComboBufferSize = 256;
		constexpr int kSearchableComboPruneIntervalFrames = 600;
		constexpr int kSearchableComboStaleFrames = 3600;

		struct SearchableComboState
		{
			std::array<char, kSearchableComboBufferSize> filter{};
			std::uint64_t filterRevision = 0;
			bool open = false;
			int lastSeenFrame = 0;
			int lastOpenFrame = -1;

			const void* cachedUserData = nullptr;
			SearchableComboLabelGetter cachedGetter = nullptr;
			std::uint64_t cachedItemsRevision = 0;
			std::uint64_t cachedFilterRevision = std::numeric_limits<std::uint64_t>::max();
			int cachedItemCount = -1;
			std::vector<int> filteredIndices;
		};

		struct SearchableComboFrame
		{
			SearchableComboState* state = nullptr;
			ImGuiLastItemData openerItem;
		};

		struct SearchableComboStorage
		{
			ImGuiContext* context = nullptr;
			std::unordered_map<ImGuiID, SearchableComboState> states;
			std::vector<SearchableComboFrame> frames;
			int lastPruneFrame = 0;
		};

		SearchableComboStorage& GetSearchableComboStorage()
		{
			static SearchableComboStorage storage;
			if (storage.context != GImGui) {
				storage = {};
				storage.context = GImGui;
			}
			return storage;
		}

		SearchableComboState& GetSearchableComboState(ImGuiID id)
		{
			auto& storage = GetSearchableComboStorage();
			const int frame = ImGui::GetFrameCount();
			if (frame < storage.lastPruneFrame ||
				frame - storage.lastPruneFrame >= kSearchableComboPruneIntervalFrames) {
				std::erase_if(storage.states, [frame](const auto& entry) {
					return frame >= entry.second.lastSeenFrame &&
					       frame - entry.second.lastSeenFrame >= kSearchableComboStaleFrames;
				});
				storage.lastPruneFrame = frame;
			}

			auto& state = storage.states[id];
			state.lastSeenFrame = frame;
			return state;
		}

		void ClearSearchableComboFilter(SearchableComboState& state)
		{
			if (state.filter.front() == '\0')
				return;
			state.filter.front() = '\0';
			++state.filterRevision;
		}

		constexpr unsigned char FoldAsciiCase(unsigned char value) noexcept
		{
			return value >= 'A' && value <= 'Z' ? static_cast<unsigned char>(value + ('a' - 'A')) : value;
		}

		bool ContainsCaseInsensitive(std::string_view text, std::string_view filter) noexcept
		{
			if (filter.empty())
				return true;
			if (filter.size() > text.size())
				return false;
			return std::search(text.begin(), text.end(), filter.begin(), filter.end(), [](char left, char right) {
				return FoldAsciiCase(static_cast<unsigned char>(left)) ==
				       FoldAsciiCase(static_cast<unsigned char>(right));
			}) != text.end();
		}

		int GetSearchableComboMaxItemCount(ImGuiComboFlags flags) noexcept
		{
			if ((flags & ImGuiComboFlags_HeightSmall) != 0)
				return 4;
			if ((flags & ImGuiComboFlags_HeightLarge) != 0)
				return 20;
			if ((flags & ImGuiComboFlags_HeightLargest) != 0)
				return -1;
			return 8;
		}

		float GetSearchableComboPopupHeight(ImGuiComboFlags flags, int maxVisibleItems)
		{
			const int itemCount = maxVisibleItems > 0 ?
			                          maxVisibleItems :
			                          GetSearchableComboMaxItemCount(flags);
			if (itemCount < 0)
				return FLT_MAX;

			const auto& style = ImGui::GetStyle();
			const float listHeight = ImGui::GetTextLineHeightWithSpacing() * itemCount - style.ItemSpacing.y;
			const float searchHeight = ImGui::GetFrameHeightWithSpacing() + style.ItemSpacing.y;
			return searchHeight + listHeight + style.WindowPadding.y * 2.0f;
		}

		void SetSearchableComboPopupConstraints(
			const char* previewValue, ImGuiComboFlags flags, int maxVisibleItems)
		{
			if ((GImGui->NextWindowData.HasFlags & ImGuiNextWindowDataFlags_HasSizeConstraint) != 0)
				return;

			const auto& style = ImGui::GetStyle();
			float popupWidth = ImGui::CalcItemWidth();
			if ((flags & ImGuiComboFlags_NoPreview) != 0) {
				popupWidth = ImGui::GetFrameHeight();
			} else if ((flags & ImGuiComboFlags_WidthFitPreview) != 0) {
				const float arrowWidth = (flags & ImGuiComboFlags_NoArrowButton) != 0 ? 0.0f : ImGui::GetFrameHeight();
				const float previewWidth = previewValue ? ImGui::CalcTextSize(previewValue, nullptr, true).x : 0.0f;
				popupWidth = arrowWidth + previewWidth + style.FramePadding.x * 2.0f;
			}

			const float scale = GetSearchUIScale();
			const float searchWidth = ImGui::CalcTextSize(T("ui.search", "Search...")).x +
			                          ThemeManager::Constants::COMBO_SEARCH_ICON_SIZE * scale +
			                          style.FramePadding.x * 4.0f;
			popupWidth = std::max(popupWidth, searchWidth);
			ImGui::SetNextWindowSizeConstraints(
				ImVec2(popupWidth, 0.0f),
				ImVec2(popupWidth, GetSearchableComboPopupHeight(flags, maxVisibleItems)));
		}

		struct ComboSearchState
		{
			char buffer[256] = {};
			bool needsFocus = true;
		};

		static std::unordered_map<std::string, ComboSearchState>& GetComboSearchStates()
		{
			static std::unordered_map<std::string, ComboSearchState> states;
			return states;
		}
	}

	bool BeginSearchableCombo(
		const char* label, const char* previewValue, ImGuiComboFlags flags,
		const void* storageAddress, int maxVisibleItems)
	{
		const ImGuiID id = ImGui::GetID(label);
		auto& state = detail::GetSearchableComboState(id);
		detail::SetSearchableComboPopupConstraints(previewValue, flags, maxVisibleItems);

		bool open = false;
		if (storageAddress) {
			ActiveControlStorageGuard storageGuard(storageAddress);
			open = ImGui::BeginCombo(label, previewValue, flags);
		} else {
			open = ImGui::BeginCombo(label, previewValue, flags);
		}
		const ImGuiLastItemData openerItem = GImGui->LastItemData;
		if (!open) {
			if (state.open)
				detail::ClearSearchableComboFilter(state);
			state.open = false;
			return false;
		}

		const int frame = ImGui::GetFrameCount();
		const bool popupAppearing = ImGui::IsWindowAppearing();
		const bool continuingOpen = state.open && state.lastOpenFrame == frame - 1 && !popupAppearing;
		if (state.open && !continuingOpen)
			detail::ClearSearchableComboFilter(state);
		const bool focusSearch = !continuingOpen;
		state.open = true;
		state.lastOpenFrame = frame;
		auto& comboStorage = detail::GetSearchableComboStorage();
		comboStorage.frames.push_back({ &state, openerItem });

		ImGui::PushID(id);
		if (focusSearch)
			ImGui::SetKeyboardFocusHere();
		ImGui::SetNextItemWidth(-FLT_MIN);
		const float scale = GetSearchUIScale();
		const float iconSize = ThemeManager::Constants::COMBO_SEARCH_ICON_SIZE * scale;
		const float iconOffsetX = ThemeManager::Constants::COMBO_SEARCH_ICON_OFFSET_X * scale;
		const float paddingLeft = ThemeManager::Constants::COMBO_SEARCH_PADDING_LEFT * scale;
		ImGui::PushStyleVar(
			ImGuiStyleVar_FramePadding, ImVec2(paddingLeft, ImGui::GetStyle().FramePadding.y));
		bool filterChanged = false;
		{
			ActiveControlStorageGuard storageGuard(nullptr);
			filterChanged = ImGui::InputTextWithHint("##search", T("ui.search", "Search..."),
				state.filter.data(), state.filter.size());
		}
		ImGui::PopStyleVar();
		if (filterChanged)
			++state.filterRevision;

		const ImVec2 iconPosition(
			ImGui::GetItemRectMin().x + iconOffsetX,
			ImGui::GetItemRectMin().y + (ImGui::GetItemRectSize().y - iconSize) * 0.5f);
		DrawSearchIcon(iconPosition, iconSize, ThemeManager::Constants::COMBO_SEARCH_ICON_ALPHA);
		ImGui::Separator();
		ImGui::PopID();
		GImGui->LastItemData = openerItem;
		return true;
	}

	void EndSearchableCombo()
	{
		auto& storage = detail::GetSearchableComboStorage();
		IM_ASSERT(!storage.frames.empty() && "EndSearchableCombo called without BeginSearchableCombo");
		if (storage.frames.empty())
			return;

		const ImGuiLastItemData openerItem = storage.frames.back().openerItem;
		storage.frames.pop_back();
		ImGui::EndCombo();
		GImGui->LastItemData = openerItem;
	}

	std::string_view GetSearchableComboFilter()
	{
		const auto& frames = detail::GetSearchableComboStorage().frames;
		return frames.empty() ? std::string_view{} : std::string_view(frames.back().state->filter.data());
	}

	bool SearchableComboMatches(std::string_view text)
	{
		return detail::ContainsCaseInsensitive(text, GetSearchableComboFilter());
	}

	void SearchableComboClipper::Begin(int itemCount, SearchableComboLabelGetter getter,
		const void* userData, std::uint64_t itemsRevision, float itemHeight)
	{
		filteredIndices = nullptr;
		itemCount = std::max(itemCount, 0);
		auto& frames = detail::GetSearchableComboStorage().frames;
		if (frames.empty() || !getter || GetSearchableComboFilter().empty()) {
			clipper.Begin(itemCount, itemHeight);
			return;
		}

		auto& state = *frames.back().state;
		if (state.cachedUserData != userData || state.cachedGetter != getter ||
			state.cachedItemsRevision != itemsRevision || state.cachedItemCount != itemCount ||
			state.cachedFilterRevision != state.filterRevision) {
			state.filteredIndices.clear();
			state.filteredIndices.reserve(static_cast<std::size_t>(itemCount));
			const std::string_view filter(state.filter.data());
			for (int itemIndex = 0; itemIndex < itemCount; ++itemIndex) {
				const char* label = getter(userData, itemIndex);
				if (label && detail::ContainsCaseInsensitive(label, filter))
					state.filteredIndices.push_back(itemIndex);
			}
			state.cachedUserData = userData;
			state.cachedGetter = getter;
			state.cachedItemsRevision = itemsRevision;
			state.cachedItemCount = itemCount;
			state.cachedFilterRevision = state.filterRevision;
		}

		filteredIndices = &state.filteredIndices;
		clipper.Begin(static_cast<int>(filteredIndices->size()), itemHeight);
	}

	void SearchableComboClipper::IncludeItemByIndex(int itemIndex)
	{
		if (!filteredIndices) {
			if (itemIndex >= 0 && itemIndex < clipper.ItemsCount)
				clipper.IncludeItemByIndex(itemIndex);
			return;
		}

		const auto found = std::lower_bound(filteredIndices->begin(), filteredIndices->end(), itemIndex);
		if (found != filteredIndices->end() && *found == itemIndex)
			clipper.IncludeItemByIndex(static_cast<int>(found - filteredIndices->begin()));
	}

	bool SearchableComboClipper::Step()
	{
		return clipper.Step();
	}

	int SearchableComboClipper::GetDisplayStart() const noexcept
	{
		return clipper.DisplayStart;
	}

	int SearchableComboClipper::GetDisplayEnd() const noexcept
	{
		return clipper.DisplayEnd;
	}

	int SearchableComboClipper::GetItemIndex(int displayIndex) const noexcept
	{
		if (!filteredIndices)
			return displayIndex;
		if (displayIndex < 0 || displayIndex >= static_cast<int>(filteredIndices->size()))
			return -1;
		return (*filteredIndices)[displayIndex];
	}

	bool SearchableCombo(const char* label, int* currentItem, int itemCount,
		SearchableComboLabelGetter getter, const void* userData,
		ImGuiComboFlags flags, std::uint64_t itemsRevision)
	{
		if (!currentItem || !getter)
			return false;

		const char* previewValue = *currentItem >= 0 && *currentItem < itemCount ?
		                               getter(userData, *currentItem) :
		                               nullptr;
		if (!BeginSearchableCombo(label, previewValue, flags, currentItem))
			return false;

		bool changed = false;
		const bool scrollToSelection = ImGui::IsWindowAppearing() && GetSearchableComboFilter().empty();
		SearchableComboClipper itemClipper;
		itemClipper.Begin(itemCount, getter, userData, itemsRevision);
		itemClipper.IncludeItemByIndex(*currentItem);
		while (itemClipper.Step()) {
			for (int displayIndex = itemClipper.GetDisplayStart();
				displayIndex < itemClipper.GetDisplayEnd(); ++displayIndex) {
				const int itemIndex = itemClipper.GetItemIndex(displayIndex);
				const char* itemLabel = getter(userData, itemIndex);
				if (!itemLabel)
					continue;
				const bool selected = itemIndex == *currentItem;
				ImGui::PushID(itemIndex);
				if (ImGui::Selectable(itemLabel, selected)) {
					changed = !selected;
					*currentItem = itemIndex;
				}
				if (selected && scrollToSelection)
					ImGui::SetScrollHereY(0.5f);
				ImGui::PopID();
			}
		}
		EndSearchableCombo();
		if (changed)
			ImGui::MarkItemEdited(GImGui->LastItemData.ID);
		return changed;
	}

	std::string DrawComboSearchInput(const char* id)
	{
		auto& state = detail::GetComboSearchStates()[id];

		if (state.needsFocus) {
			ImGui::SetKeyboardFocusHere();
			state.needsFocus = false;
		}

		const float scale = GetSearchUIScale();
		const float iconSize = ThemeManager::Constants::COMBO_SEARCH_ICON_SIZE * scale;
		constexpr float iconAlpha = ThemeManager::Constants::COMBO_SEARCH_ICON_ALPHA;
		const float iconOffsetX = ThemeManager::Constants::COMBO_SEARCH_ICON_OFFSET_X * scale;
		const float paddingLeft = ThemeManager::Constants::COMBO_SEARCH_PADDING_LEFT * scale;

		char widgetId[128];
		snprintf(widgetId, sizeof(widgetId), "##%s_search", id);

		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(paddingLeft, ImGui::GetStyle().FramePadding.y));
		ImGui::InputTextWithHint(widgetId, T("ui.search", "Search..."), state.buffer, IM_ARRAYSIZE(state.buffer));
		ImGui::PopStyleVar();

		ImVec2 iconPos = ImVec2(
			ImGui::GetItemRectMin().x + iconOffsetX,
			ImGui::GetItemRectMin().y + (ImGui::GetItemRectSize().y - iconSize) * 0.5f);
		DrawSearchIcon(iconPos, iconSize, iconAlpha);

		ImGui::Separator();

		return state.buffer;
	}

	void ClearComboSearch(const char* id)
	{
		auto& state = detail::GetComboSearchStates()[id];
		state.buffer[0] = '\0';
		state.needsFocus = true;
	}

	void DrawFeatureSearchBar(std::string& searchString, float availableWidth)
	{
		MenuFonts::FontRoleGuard fontGuard(Menu::FontRole::Subheading);
		ImGui::PushID("FeatureSearchBar");

		const float iconColumnWidth = ImGui::GetTextLineHeight();
		const float iconSize = std::min(iconColumnWidth, ThemeManager::Constants::SEARCH_ICON_SIZE * GetSearchUIScale());
		const float iconSpace = iconColumnWidth + ImGui::GetStyle().ItemSpacing.x + ImGui::CalcTextSize(" ").x;

		if (availableWidth <= 0.0f) {
			availableWidth = ImGui::GetContentRegionAvail().x;
		}

		// Custom style - always transparent background to avoid click blocking
		ImVec4 bgColor = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
		ImVec4 bgColorActive = ImVec4(0.3f, 0.3f, 0.3f, 0.9f);
		// Use theme text color instead of hardcoded color
		auto& palette = globals::menu->GetTheme().Palette;
		ImVec4 textColor = palette.Text;

		ImGui::PushStyleColor(ImGuiCol_FrameBg, bgColor);
		ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, bgColor);
		ImGui::PushStyleColor(ImGuiCol_FrameBgActive, bgColorActive);
		ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_Text, textColor);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(iconSpace, 0.0f));

		// Draw the input field
		ImGui::SetNextItemWidth(availableWidth);
		char buffer[256];
		strncpy_s(buffer, searchString.c_str(), sizeof(buffer) - 1);
		buffer[sizeof(buffer) - 1] = '\0';

		if (ImGui::InputTextWithHint("##feature_search", T("ui.search_features", "Search Features..."), buffer, sizeof(buffer))) {
			searchString = buffer;
		}

		// Draw search icon using the reusable function
		const ImVec2 inputMin = ImGui::GetItemRectMin();
		const ImVec2 iconPos(inputMin.x + (iconColumnWidth - iconSize) * 0.5f, inputMin.y + (ImGui::GetItemRectSize().y - iconSize) * 0.5f);
		DrawSearchIcon(iconPos, iconSize, ThemeManager::Constants::SEARCH_ICON_ALPHA);

		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor(5);
		ImGui::PopID();
	}

	void ShowSortedStringTableStrings(
		const char* table_id,
		const std::vector<std::string>& headers,
		const std::vector<std::vector<std::string>>& rows,
		size_t sortColumn,
		bool ascending,
		const std::vector<TableSortFunc>& customSorts,
		TableCellRenderFunc cellRender)
	{
		ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Sortable;
		if (ImGui::BeginTable(table_id, static_cast<int>(headers.size()), flags)) {
			for (const auto& header : headers)
				ImGui::TableSetupColumn(header.c_str());
			ImGui::TableHeadersRow();

			// Determine sorting
			int sortCol = static_cast<int>(sortColumn);
			bool sortAsc = ascending;
			if (const ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs()) {
				if (sortSpecs->SpecsCount > 0) {
					sortCol = sortSpecs->Specs->ColumnIndex;
					sortAsc = sortSpecs->Specs->SortDirection == ImGuiSortDirection_Ascending;
				}
			}

			// Make a copy if sorting is needed
			std::vector<std::vector<std::string>> sortedRows = rows;
			if (sortCol >= 0 && static_cast<size_t>(sortCol) < headers.size()) {
				// Fallback to default string sort if no custom sort is provided
				auto cmp = (sortCol < static_cast<int>(customSorts.size()) && customSorts[sortCol]) ? customSorts[sortCol] : StringSortComparator;
				std::sort(sortedRows.begin(), sortedRows.end(), [sortCol, sortAsc, &cmp](const std::vector<std::string>& a, const std::vector<std::string>& b) {
					const std::string& aVal = (sortCol < a.size()) ? a[sortCol] : std::string();
					const std::string& bVal = (sortCol < b.size()) ? b[sortCol] : std::string();
					return cmp(aVal, bVal, sortAsc);
				});
			}

			// Render rows
			for (size_t rowIdx = 0; rowIdx < sortedRows.size(); ++rowIdx) {
				const auto& row = sortedRows[rowIdx];
				ImGui::TableNextRow();
				for (size_t col = 0; col < headers.size(); ++col) {
					ImGui::TableSetColumnIndex(static_cast<int>(col));
					if (cellRender) {
						const std::string& value = (col < row.size()) ? row[col] : std::string();
						cellRender(static_cast<int>(rowIdx), static_cast<int>(col), value);
					} else {
						if (col < row.size())
							ImGui::TextUnformatted(row[col].c_str());
					}
				}
			}
			ImGui::EndTable();
		}
	}

	void DrawDllVersionTable(
		const char* label,
		const wchar_t* pluginDir,
		const std::vector<std::pair<std::string, std::string>>& dllVersions,
		const char* tableId)
	{
		if (ImGui::Selectable(label)) {
			auto realPath = Util::PathHelpers::GetRealPathFromDataRelative(pluginDir);
			ShellExecuteW(nullptr, L"open", realPath.empty() ? pluginDir : realPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		}
		std::vector<std::string> headers = { T("ui.table.dll_name", "DLL Name"), T("ui.table.version", "Version") };
		std::vector<std::vector<std::string>> rows;
		rows.reserve(dllVersions.size());
		for (const auto& [name, version] : dllVersions)
			rows.push_back({ name, version });
		std::vector<TableSortFunc> sorters = { nullptr, VersionSortComparator };
		ShowSortedStringTableStrings(tableId, headers, rows, 0, true, sorters);
	}

	// Theme-aware color accessor functions
	namespace Colors
	{
		ImVec4 GetTimerGood()
		{
			return globals::menu->GetTheme().StatusPalette.SuccessColor;
		}

		ImVec4 GetTimerWarning()
		{
			return globals::menu->GetTheme().StatusPalette.Warning;
		}

		ImVec4 GetTimerCritical()
		{
			return globals::menu->GetTheme().StatusPalette.Error;
		}

		ImVec4 GetDefault()
		{
			return globals::menu->GetTheme().Palette.Text;
		}

		ImVec4 GetSuccess()
		{
			return globals::menu->GetTheme().StatusPalette.SuccessColor;
		}

		ImVec4 GetWarning()
		{
			return globals::menu->GetTheme().StatusPalette.Warning;
		}

		ImVec4 GetError()
		{
			return globals::menu->GetTheme().StatusPalette.Error;
		}

		ImVec4 GetInfo()
		{
			return globals::menu->GetTheme().StatusPalette.InfoColor;
		}

		ImVec4 GetDisabled()
		{
			return globals::menu->GetTheme().StatusPalette.Disable;
		}

		ImVec4 GetRestartNeeded()
		{
			return globals::menu->GetTheme().StatusPalette.RestartNeeded;
		}

	}

	namespace Text
	{
		static void ColoredTextV(ImVec4 color, const char* fmt, va_list args)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, color);
			ImGui::TextV(fmt, args);
			ImGui::PopStyleColor();
		}

		static void ColoredTextWrappedV(ImVec4 color, const char* fmt, va_list args)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, color);
			ImGui::TextWrappedV(fmt, args);
			ImGui::PopStyleColor();
		}

#define UTIL_TEXT(Name, ColorFn)                    \
	void Name(const char* fmt, ...)                 \
	{                                               \
		va_list args;                               \
		va_start(args, fmt);                        \
		ColoredTextV(Colors::ColorFn(), fmt, args); \
		va_end(args);                               \
	}
#define UTIL_TEXT_WRAPPED(Name, ColorFn)                   \
	void Name(const char* fmt, ...)                        \
	{                                                      \
		va_list args;                                      \
		va_start(args, fmt);                               \
		ColoredTextWrappedV(Colors::ColorFn(), fmt, args); \
		va_end(args);                                      \
	}

		UTIL_TEXT(Warning, GetWarning)
		UTIL_TEXT_WRAPPED(WrappedWarning, GetWarning)
		UTIL_TEXT(Error, GetError)
		UTIL_TEXT_WRAPPED(WrappedError, GetError)
		UTIL_TEXT(Success, GetSuccess)
		UTIL_TEXT_WRAPPED(WrappedSuccess, GetSuccess)
		UTIL_TEXT(Info, GetInfo)
		UTIL_TEXT_WRAPPED(WrappedInfo, GetInfo)
		UTIL_TEXT(Disabled, GetDisabled)
		UTIL_TEXT_WRAPPED(WrappedDisabled, GetDisabled)
		UTIL_TEXT(RestartNeeded, GetRestartNeeded)
		UTIL_TEXT_WRAPPED(WrappedRestartNeeded, GetRestartNeeded)

#undef UTIL_TEXT
#undef UTIL_TEXT_WRAPPED
	}

	namespace Input
	{
#define IM_VK_KEYPAD_ENTER (VK_RETURN + 256)

		ImGuiKey VirtualKeyToImGuiKey(WPARAM vkKey)
		{
			switch (vkKey) {
			case VK_TAB:
				return ImGuiKey_Tab;
			case VK_LEFT:
				return ImGuiKey_LeftArrow;
			case VK_RIGHT:
				return ImGuiKey_RightArrow;
			case VK_UP:
				return ImGuiKey_UpArrow;
			case VK_DOWN:
				return ImGuiKey_DownArrow;
			case VK_PRIOR:
				return ImGuiKey_PageUp;
			case VK_NEXT:
				return ImGuiKey_PageDown;
			case VK_HOME:
				return ImGuiKey_Home;
			case VK_END:
				return ImGuiKey_End;
			case VK_INSERT:
				return ImGuiKey_Insert;
			case VK_DELETE:
				return ImGuiKey_Delete;
			case VK_BACK:
				return ImGuiKey_Backspace;
			case VK_SPACE:
				return ImGuiKey_Space;
			case VK_RETURN:
				return ImGuiKey_Enter;
			case VK_ESCAPE:
				return ImGuiKey_Escape;
			case VK_OEM_7:
				return ImGuiKey_Apostrophe;
			case VK_OEM_COMMA:
				return ImGuiKey_Comma;
			case VK_OEM_MINUS:
				return ImGuiKey_Minus;
			case VK_OEM_PERIOD:
				return ImGuiKey_Period;
			case VK_OEM_2:
				return ImGuiKey_Slash;
			case VK_OEM_1:
				return ImGuiKey_Semicolon;
			case VK_OEM_PLUS:
				return ImGuiKey_Equal;
			case VK_OEM_4:
				return ImGuiKey_LeftBracket;
			case VK_OEM_5:
				return ImGuiKey_Backslash;
			case VK_OEM_6:
				return ImGuiKey_RightBracket;
			case VK_OEM_3:
				return ImGuiKey_GraveAccent;
			case VK_CAPITAL:
				return ImGuiKey_CapsLock;
			case VK_SCROLL:
				return ImGuiKey_ScrollLock;
			case VK_NUMLOCK:
				return ImGuiKey_NumLock;
			case VK_SNAPSHOT:
				return ImGuiKey_PrintScreen;
			case VK_PAUSE:
				return ImGuiKey_Pause;
			case VK_NUMPAD0:
				return ImGuiKey_Keypad0;
			case VK_NUMPAD1:
				return ImGuiKey_Keypad1;
			case VK_NUMPAD2:
				return ImGuiKey_Keypad2;
			case VK_NUMPAD3:
				return ImGuiKey_Keypad3;
			case VK_NUMPAD4:
				return ImGuiKey_Keypad4;
			case VK_NUMPAD5:
				return ImGuiKey_Keypad5;
			case VK_NUMPAD6:
				return ImGuiKey_Keypad6;
			case VK_NUMPAD7:
				return ImGuiKey_Keypad7;
			case VK_NUMPAD8:
				return ImGuiKey_Keypad8;
			case VK_NUMPAD9:
				return ImGuiKey_Keypad9;
			case VK_DECIMAL:
				return ImGuiKey_KeypadDecimal;
			case VK_DIVIDE:
				return ImGuiKey_KeypadDivide;
			case VK_MULTIPLY:
				return ImGuiKey_KeypadMultiply;
			case VK_SUBTRACT:
				return ImGuiKey_KeypadSubtract;
			case VK_ADD:
				return ImGuiKey_KeypadAdd;
			case IM_VK_KEYPAD_ENTER:
				return ImGuiKey_KeypadEnter;
			case VK_LSHIFT:
				return ImGuiKey_LeftShift;
			case VK_LCONTROL:
				return ImGuiKey_LeftCtrl;
			case VK_LMENU:
				return ImGuiKey_LeftAlt;
			case VK_LWIN:
				return ImGuiKey_LeftSuper;
			case VK_RSHIFT:
				return ImGuiKey_RightShift;
			case VK_RCONTROL:
				return ImGuiKey_RightCtrl;
			case VK_RMENU:
				return ImGuiKey_RightAlt;
			case VK_RWIN:
				return ImGuiKey_RightSuper;
			case VK_APPS:
				return ImGuiKey_Menu;
			case '0':
				return ImGuiKey_0;
			case '1':
				return ImGuiKey_1;
			case '2':
				return ImGuiKey_2;
			case '3':
				return ImGuiKey_3;
			case '4':
				return ImGuiKey_4;
			case '5':
				return ImGuiKey_5;
			case '6':
				return ImGuiKey_6;
			case '7':
				return ImGuiKey_7;
			case '8':
				return ImGuiKey_8;
			case '9':
				return ImGuiKey_9;
			case 'A':
				return ImGuiKey_A;
			case 'B':
				return ImGuiKey_B;
			case 'C':
				return ImGuiKey_C;
			case 'D':
				return ImGuiKey_D;
			case 'E':
				return ImGuiKey_E;
			case 'F':
				return ImGuiKey_F;
			case 'G':
				return ImGuiKey_G;
			case 'H':
				return ImGuiKey_H;
			case 'I':
				return ImGuiKey_I;
			case 'J':
				return ImGuiKey_J;
			case 'K':
				return ImGuiKey_K;
			case 'L':
				return ImGuiKey_L;
			case 'M':
				return ImGuiKey_M;
			case 'N':
				return ImGuiKey_N;
			case 'O':
				return ImGuiKey_O;
			case 'P':
				return ImGuiKey_P;
			case 'Q':
				return ImGuiKey_Q;
			case 'R':
				return ImGuiKey_R;
			case 'S':
				return ImGuiKey_S;
			case 'T':
				return ImGuiKey_T;
			case 'U':
				return ImGuiKey_U;
			case 'V':
				return ImGuiKey_V;
			case 'W':
				return ImGuiKey_W;
			case 'X':
				return ImGuiKey_X;
			case 'Y':
				return ImGuiKey_Y;
			case 'Z':
				return ImGuiKey_Z;
			case VK_F1:
				return ImGuiKey_F1;
			case VK_F2:
				return ImGuiKey_F2;
			case VK_F3:
				return ImGuiKey_F3;
			case VK_F4:
				return ImGuiKey_F4;
			case VK_F5:
				return ImGuiKey_F5;
			case VK_F6:
				return ImGuiKey_F6;
			case VK_F7:
				return ImGuiKey_F7;
			case VK_F8:
				return ImGuiKey_F8;
			case VK_F9:
				return ImGuiKey_F9;
			case VK_F10:
				return ImGuiKey_F10;
			case VK_F11:
				return ImGuiKey_F11;
			case VK_F12:
				return ImGuiKey_F12;
			default:
				return ImGuiKey_None;
			};
		}

		uint32_t DIKToVK(uint32_t dikKey)
		{
			switch (dikKey) {
			case DIK_LEFTARROW:
				return VK_LEFT;
			case DIK_RIGHTARROW:
				return VK_RIGHT;
			case DIK_UPARROW:
				return VK_UP;
			case DIK_DOWNARROW:
				return VK_DOWN;
			case DIK_DELETE:
				return VK_DELETE;
			case DIK_END:
				return VK_END;
			case DIK_HOME:
				return VK_HOME;  // pos1
			case DIK_PRIOR:
				return VK_PRIOR;  // page up
			case DIK_NEXT:
				return VK_NEXT;  // page down
			case DIK_INSERT:
				return VK_INSERT;
			case DIK_NUMPAD0:
				return VK_NUMPAD0;
			case DIK_NUMPAD1:
				return VK_NUMPAD1;
			case DIK_NUMPAD2:
				return VK_NUMPAD2;
			case DIK_NUMPAD3:
				return VK_NUMPAD3;
			case DIK_NUMPAD4:
				return VK_NUMPAD4;
			case DIK_NUMPAD5:
				return VK_NUMPAD5;
			case DIK_NUMPAD6:
				return VK_NUMPAD6;
			case DIK_NUMPAD7:
				return VK_NUMPAD7;
			case DIK_NUMPAD8:
				return VK_NUMPAD8;
			case DIK_NUMPAD9:
				return VK_NUMPAD9;
			case DIK_DECIMAL:
				return VK_DECIMAL;
			case DIK_NUMPADENTER:
				return IM_VK_KEYPAD_ENTER;
			case DIK_RMENU:
				return VK_RMENU;  // right alt
			case DIK_RCONTROL:
				return VK_RCONTROL;  // right control
			case DIK_LWIN:
				return VK_LWIN;  // left win
			case DIK_RWIN:
				return VK_RWIN;  // right win
			case DIK_APPS:
				return VK_APPS;
			case DIK_SYSRQ:
				return VK_SNAPSHOT;
			default:
				return dikKey;
			}
		}

		const char* KeyIdToString(uint32_t key)
		{
			if (key >= 256)
				return "";

			static const char* keyboard_keys_international[256] = {
				"", "Left Mouse", "Right Mouse", "Cancel", "Middle Mouse", "X1 Mouse", "X2 Mouse", "", "Backspace", "Tab", "", "", "Clear", "Enter", "", "",
				"Shift", "Control", "Alt", "Pause", "Caps Lock", "", "", "", "", "", "", "Escape", "", "", "", "",
				"Space", "Page Up", "Page Down", "End", "Home", "Left Arrow", "Up Arrow", "Right Arrow", "Down Arrow", "Select", "", "", "Print Screen", "Insert", "Delete", "Help",
				"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "", "", "", "", "", "",
				"", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O",
				"P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "Left Windows", "Right Windows", "Apps", "", "Sleep",
				"Numpad 0", "Numpad 1", "Numpad 2", "Numpad 3", "Numpad 4", "Numpad 5", "Numpad 6", "Numpad 7", "Numpad 8", "Numpad 9", "Numpad *", "Numpad +", "", "Numpad -", "Numpad Decimal", "Numpad /",
				"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12", "F13", "F14", "F15", "F16",
				"F17", "F18", "F19", "F20", "F21", "F22", "F23", "F24", "", "", "", "", "", "", "", "",
				"Num Lock", "Scroll Lock", "", "", "", "", "", "", "", "", "", "", "", "", "", "",
				"Left Shift", "Right Shift", "Left Control", "Right Control", "Left Menu", "Right Menu", "Browser Back", "Browser Forward", "Browser Refresh", "Browser Stop", "Browser Search", "Browser Favorites", "Browser Home", "Volume Mute", "Volume Down", "Volume Up",
				"Next Track", "Previous Track", "Media Stop", "Media Play/Pause", "Mail", "Media Select", "Launch App 1", "Launch App 2", "", "", "OEM ;", "OEM +", "OEM ,", "OEM -", "OEM .", "OEM /",
				"OEM ~", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "",
				"", "", "", "", "", "", "", "", "", "", "", "OEM [", "OEM \\", "OEM ]", "OEM '", "OEM 8",
				"", "", "OEM <", "", "", "", "", "", "", "", "", "", "", "", "", "",
				"", "", "", "", "", "", "Attn", "CrSel", "ExSel", "Erase EOF", "Play", "Zoom", "", "PA1", "OEM Clear", ""
			};

			return keyboard_keys_international[key];
		}

		std::string KeyIdToString(const std::vector<InputCombo>& combo)
		{
			if (combo.empty())
				return "None";

			bool hasVRInput = false;
			for (const auto& input : combo) {
				if (input.GetDevice() != InputDeviceType::Keyboard && input.GetDevice() != InputDeviceType::Mouse) {
					hasVRInput = true;
					break;
				}
			}

			if (hasVRInput && globals::game::isVR) {
				return InputCombo::GetVRString(combo);
			}

			std::string result;
			for (size_t i = 0; i < combo.size(); ++i) {
				if (i > 0)
					result += " + ";
				result += KeyIdToString(combo[i].GetKey());
			}
			return result;
		}
	}  // namespace Input

	bool ButtonWithFlash(const char* label, const ImVec2& size, int flashDurationMs)
	{
		static std::unordered_map<std::string, std::chrono::steady_clock::time_point> flashTimers;
		static std::mutex flashTimersMutex;

		std::string buttonId = std::string(label);
		auto now = std::chrono::steady_clock::now();

		// Check if this button has active flash (thread-safe)
		bool hasActiveFlash = false;
		{
			std::lock_guard<std::mutex> lock(flashTimersMutex);
			auto it = flashTimers.find(buttonId);
			if (it != flashTimers.end()) {
				auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second);
				if (elapsed.count() < flashDurationMs) {
					hasActiveFlash = true;
				} else {
					// Flash expired, remove it
					flashTimers.erase(it);
				}
			}
		}

		// Style the button with flash effect if active.
		bool styleChanged = false;
		if (hasActiveFlash) {
			// Use subtle white overlay similar to action icon hover effect
			ImVec4 normalButton = ImGui::GetStyleColorVec4(ImGuiCol_Button);
			ImVec4 flashColor = ImVec4(
				normalButton.x + 0.2f,  // Brighten slightly
				normalButton.y + 0.2f,
				normalButton.z + 0.2f,
				normalButton.w);
			ImVec4 flashHovered = ImVec4(flashColor.x * 1.1f, flashColor.y * 1.1f, flashColor.z * 1.1f, flashColor.w);
			ImVec4 flashActive = ImVec4(flashColor.x * 0.9f, flashColor.y * 0.9f, flashColor.z * 0.9f, flashColor.w);

			ImGui::PushStyleColor(ImGuiCol_Button, flashColor);
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, flashHovered);
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, flashActive);
			styleChanged = true;
		}

		bool clicked = ImGui::Button(label, size);

		if (styleChanged) {
			ImGui::PopStyleColor(3);
		}

		// If clicked, start the flash timer (thread-safe)
		if (clicked) {
			std::lock_guard<std::mutex> lock(flashTimersMutex);
			flashTimers[buttonId] = now;
		}

		return clicked;
	}

	bool LoadDDSTextureFromFile(ID3D11Device* device,
		const char* filename,
		ID3D11ShaderResourceView** out_srv,
		ImVec2& out_size)
	{
		if (!device || !out_srv) {
			logger::warn("LoadDDSTextureFromFile: Invalid parameters");
			return false;
		}

		*out_srv = nullptr;

		// Try to load from BSA using Skyrim's resource system
		RE::BSResourceNiBinaryStream bsaStream(filename);
		if (!bsaStream.good()) {
			logger::warn("LoadDDSTextureFromFile: Failed to open resource: {}", filename);
			return false;
		}

		// Read entire DDS file into memory
		std::vector<uint8_t> ddsData;
		auto size = bsaStream.stream->totalSize;
		if (size == 0) {
			logger::warn("LoadDDSTextureFromFile: Resource has zero size: {}", filename);
			return false;
		}

		ddsData.resize(size);
		bsaStream.read(reinterpret_cast<char*>(ddsData.data()), size);

		// Load DDS from memory
		DirectX::ScratchImage image;
		try {
			DX::ThrowIfFailed(DirectX::LoadFromDDSMemory(
				ddsData.data(),
				ddsData.size(),
				DirectX::DDS_FLAGS_NONE,
				nullptr,
				image));
		} catch (const DX::com_exception& e) {
			logger::warn("LoadDDSTextureFromFile: Failed to load DDS data from {}: {}", filename, e.what());
			return false;
		}

		ID3D11Resource* pResource = nullptr;
		try {
			DX::ThrowIfFailed(DirectX::CreateTexture(device,
				image.GetImages(), image.GetImageCount(),
				image.GetMetadata(), &pResource));
		} catch (const DX::com_exception& e) {
			logger::warn("LoadDDSTextureFromFile: Failed to create texture: {}", e.what());
			return false;
		}

		ID3D11Texture2D* pTexture = reinterpret_cast<ID3D11Texture2D*>(pResource);
		D3D11_TEXTURE2D_DESC desc;
		pTexture->GetDesc(&desc);

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = {
				.MostDetailedMip = 0,
				.MipLevels = desc.MipLevels }
		};

		HRESULT hr = device->CreateShaderResourceView(pTexture, &srvDesc, out_srv);
		if (SUCCEEDED(hr) && *out_srv)
			Util::SetResourceName(*out_srv, "UI::DDS:%s", filename);
		pTexture->Release();

		if (FAILED(hr) || !*out_srv) {
			logger::warn("LoadDDSTextureFromFile: Failed to create SRV, HRESULT: 0x{:08X}", static_cast<uint32_t>(hr));
			return false;
		}

		out_size = ImVec2((float)desc.Width, (float)desc.Height);
		logger::debug("LoadDDSTextureFromFile: Successfully loaded {} ({}x{})", filename, desc.Width, desc.Height);
		return true;
	}

	bool FeatureToggle(const char* label, bool* enabled, const ImVec2& size)
	{
		if (!enabled)
			return false;

		// Calculate appropriate size if not specified - make it smaller
		ImVec2 toggleSize = size;
		if (toggleSize.x <= 0) {
			toggleSize.x = ImGui::GetFrameHeight() * 1.6f;  // Smaller 1.6:1 aspect ratio
		}
		if (toggleSize.y <= 0) {
			toggleSize.y = ImGui::GetFrameHeight() * 0.8f;  // Smaller height
		}

		// Get theme colors for better integration
		auto& style = ImGui::GetStyle();
		auto& colors = style.Colors;

		// Use theme header colors instead of bright green/red
		ImVec4 toggleBg = *enabled ?
		                      colors[ImGuiCol_Header] :  // Use header color when enabled
		                      colors[ImGuiCol_FrameBg];  // Use frame background when disabled

		ImVec4 toggleBgHovered = *enabled ?
		                             colors[ImGuiCol_HeaderHovered] :  // Use header hovered when enabled
		                             colors[ImGuiCol_FrameBgHovered];  // Use frame hovered when disabled

		ImVec4 toggleBgActive = *enabled ?
		                            colors[ImGuiCol_HeaderActive] :  // Use header active when enabled
		                            colors[ImGuiCol_FrameBgActive];  // Use frame active when disabled

		// Apply toggle styling with border
		ImGui::PushStyleColor(ImGuiCol_Button, toggleBg);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toggleBgHovered);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, toggleBgActive);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, toggleSize.y * 0.5f);  // Round ends
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.5f);               // Larger border

		// Create unique ID for the toggle
		ImGui::PushID(label);

		// Draw the toggle button
		bool clicked = ImGui::Button("", toggleSize);

		// Draw the toggle knob
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		ImVec2 buttonMin = ImGui::GetItemRectMin();
		ImVec2 buttonMax = ImGui::GetItemRectMax();

		// Calculate knob position and size
		float knobRadius = (toggleSize.y - 4.0f) * 0.5f;
		float knobPadding = 2.0f;
		float knobTravel = toggleSize.x - (knobRadius * 2.0f) - (knobPadding * 2.0f);
		float knobX = *enabled ?
		                  buttonMin.x + knobPadding + knobRadius + knobTravel :
		                  buttonMin.x + knobPadding + knobRadius;
		float knobY = buttonMin.y + toggleSize.y * 0.5f;

		// Draw knob
		ImU32 knobColor = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
		drawList->AddCircleFilled(ImVec2(knobX, knobY), knobRadius, knobColor);

		ImGui::PopID();
		ImGui::PopStyleVar(2);  // Pop both FrameRounding and FrameBorderSize
		ImGui::PopStyleColor(3);

		// Handle toggle action
		if (clicked) {
			*enabled = !*enabled;
		}

		return clicked;
	}

	bool InputComboWidget(
		const char* label,
		std::vector<InputCombo>& combo,
		bool& isRecording,
		const char* recordingLabel)
	{
		bool changed = false;
		ImGui::Text("%s", label);
		ImGui::SameLine();

		// Use theme colors for consistent styling
		auto& theme = globals::menu->GetTheme().StatusPalette;

		if (isRecording) {
			// Recording state visual
			ImGui::PushStyleColor(ImGuiCol_Button, theme.CurrentHotkey);
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme.CurrentHotkey);
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme.CurrentHotkey);
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 1));  // Black text on recording color

			// Show current pending combo if available, otherwise prompt
			std::string buttonText;
			if (!combo.empty()) {
				buttonText = Util::Input::KeyIdToString(combo) + "...";  // Indicate it's still capturing
			} else {
				buttonText = T("ui.input.recording", "Recording... (Esc to cancel)");
			}

			if (ImGui::Button(buttonText.c_str(), ImVec2(0, 0))) {
				isRecording = false;
			}

			ImGui::PopStyleColor(4);

			// Add tooltip explaining how to record
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", T("ui.input.record_hint", "Press any key combination.\nModifiers (Ctrl, Shift, Alt) are supported.\nPress Escape to cancel."));
			}
		} else {
			// Display current binding with unique button ID
			std::string keyString = Util::Input::KeyIdToString(combo);
			std::string btnLabel = keyString + "##" + recordingLabel;
			if (ImGui::Button(btnLabel.c_str(), ImVec2(0, 0))) {
				isRecording = true;
			}

			// Context menu for clearing
			if (ImGui::BeginPopupContextItem()) {
				if (ImGui::Selectable(T("ui.input.clear_binding", "Clear Binding"))) {
					combo.clear();
					changed = true;
				}
				ImGui::EndPopup();
			}

			// First run / empty state hint
			if (combo.empty()) {
				ImGui::SameLine();
				ImGui::TextDisabled("%s", T("ui.input.click_to_bind", "(Click to bind)"));
			}
		}

		// Draw VR-specific color coding if applicable
		if (globals::game::isVR && !combo.empty()) {
			ImGui::SameLine();

			// Check if we have mixed devices
			bool hasPrimary = false;
			bool hasSecondary = false;
			bool hasBoth = false;

			for (const auto& input : combo) {
				switch (input.GetDevice()) {
				case InputDeviceType::Primary:
					hasPrimary = true;
					break;
				case InputDeviceType::Secondary:
					hasSecondary = true;
					break;
				case InputDeviceType::Both:
					hasBoth = true;
					break;
				default:
					break;
				}
			}

			ImVec4 indicatorColor = GetControllerDefaultColor();
			const char* indicatorText = "";

			if (hasBoth || (hasPrimary && hasSecondary)) {
				indicatorColor = GetControllerBothColor();
				indicatorText = hasBoth ? T("ui.input.indicator_both", "(Both)") : T("ui.input.indicator_mixed", "(Mixed)");
			} else if (hasPrimary) {
				indicatorColor = GetControllerPrimaryColor();
				indicatorText = T("ui.input.indicator_primary", "(Primary)");
			} else if (hasSecondary) {
				indicatorColor = GetControllerSecondaryColor();
				indicatorText = T("ui.input.indicator_secondary", "(Secondary)");
			}

			if (indicatorText[0] != '\0') {
				ImGui::TextColored(indicatorColor, "%s", indicatorText);
			}
		}

		return changed;
	}

	namespace ConstrainedUI
	{
		namespace
		{
			// Helper to render constraint tooltip
			void RenderConstraintTooltip(const FeatureConstraints::ConstraintResult& constraint)
			{
				if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
					return;

				ImGui::BeginTooltip();
				ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
				Util::Text::Warning("%s", T("ui.constraint.setting_constrained", "Setting Constrained"));
				ImGui::Text("%s", T("ui.constraint.constrained_by", "This setting is constrained by:"));
				ImGui::Spacing();
				for (const auto& src : constraint.sources) {
					ImGui::BulletText("%s", src.featureName.c_str());
					ImGui::Indent();
					ImGui::TextWrapped("%s", src.reason.c_str());
					if (src.recommendDisableAtBoot) {
						Util::Text::WrappedError("%s", T("ui.constraint.consider_disable", "Consider disabling this feature at boot for best compatibility."));
					}
					ImGui::Unindent();
				}
				ImGui::Separator();
				ImGui::Text(T("ui.constraint.forced_value", "Forced value: %s"), FeatureConstraints::FormatConstraintValue(constraint.forcedValue).c_str());
				ImGui::PopTextWrapPos();
				ImGui::EndTooltip();
			}
		}

		bool Checkbox(const char* label, bool* value, const FeatureConstraints::SettingId& settingId)
		{
			auto constraint = FeatureConstraints::GetConstraints(settingId);

			if (constraint.isConstrained) {
				// Display the forced value instead of the stored value
				if (auto* forcedBool = std::get_if<bool>(&constraint.forcedValue)) {
					bool displayValue = *forcedBool;
					ImGui::BeginDisabled();
					ImGui::Checkbox(label, &displayValue);
					ImGui::EndDisabled();
				} else {
					// Fallback: wrong type, show disabled with stored value
					ImGui::BeginDisabled();
					ImGui::Checkbox(label, value);
					ImGui::EndDisabled();
				}
				RenderConstraintTooltip(constraint);
				return false;
			}

			return ImGui::Checkbox(label, value);
		}

		bool SliderFloat(const char* label, float* value, float min, float max,
			const FeatureConstraints::SettingId& settingId, const char* format)
		{
			auto constraint = FeatureConstraints::GetConstraints(settingId);

			if (constraint.isConstrained) {
				// Display the forced value instead of the stored value
				if (auto* forcedFloat = std::get_if<float>(&constraint.forcedValue)) {
					float displayValue = *forcedFloat;
					ImGui::BeginDisabled();
					ImGui::SliderFloat(label, &displayValue, min, max, format);
					ImGui::EndDisabled();
				} else {
					// Fallback: wrong type, show disabled with stored value
					ImGui::BeginDisabled();
					ImGui::SliderFloat(label, value, min, max, format);
					ImGui::EndDisabled();
				}
				RenderConstraintTooltip(constraint);
				return false;
			}

			return ImGui::SliderFloat(label, value, min, max, format);
		}

		bool SliderInt(const char* label, int* value, int min, int max,
			const FeatureConstraints::SettingId& settingId, const char* format)
		{
			auto constraint = FeatureConstraints::GetConstraints(settingId);

			if (constraint.isConstrained) {
				// Display the forced value instead of the stored value
				if (auto* forcedInt = std::get_if<int>(&constraint.forcedValue)) {
					int displayValue = *forcedInt;
					ImGui::BeginDisabled();
					ImGui::SliderInt(label, &displayValue, min, max, format);
					ImGui::EndDisabled();
				} else {
					// Fallback: wrong type, show disabled with stored value
					ImGui::BeginDisabled();
					ImGui::SliderInt(label, value, min, max, format);
					ImGui::EndDisabled();
				}
				RenderConstraintTooltip(constraint);
				return false;
			}

			return ImGui::SliderInt(label, value, min, max, format);
		}
	}

	namespace
	{
		constexpr float kFlyoutCloseDelay = 0.10f;
		constexpr float kFlyoutSlideOpenSpeed = 10.0f;
		constexpr float kFlyoutSlideCloseSpeed = 14.0f;
		constexpr float kFlyoutSlideDistance = 6.0f;
		constexpr float kFlyoutAlphaScale = 4.0f;
		constexpr std::string_view kFlyoutWindowPrefix = "##flyout_";
		constexpr std::string_view kBlurredFlyoutWindowPrefix = "##blurred_flyout_";

		void ResetFlyout(FlyoutState& state, bool preserveHoverBlock = false) noexcept
		{
			const ImGuiID blockedHoverId = preserveHoverBlock ? state.blockedHoverId : 0;
			state.isOpen = false;
			state.closing = false;
			state.flyoutHovered = false;
			state.activeId = 0;
			state.blockedHoverId = blockedHoverId;
			state.closeTimer = 0.0f;
			state.openProgress = 0.0f;
			state.keepOpenForNavigation = false;
			state.draggedFromFlyout = false;
		}

		bool ContainsPoint(const ImVec2& min, const ImVec2& max, const ImVec2& point)
		{
			return point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y;
		}

		ImRect GetFlyoutWindowRect()
		{
			auto* viewport = ImGui::GetMainViewport();
			auto* rootWindow = ImGui::GetCurrentWindow()->RootWindow;
			return ImRect(
				ImVec2(std::max(viewport->Pos.x, rootWindow->Pos.x), std::max(viewport->Pos.y, rootWindow->Pos.y)),
				ImVec2(std::min(viewport->Pos.x + viewport->Size.x, rootWindow->Pos.x + rootWindow->Size.x),
					std::min(viewport->Pos.y + viewport->Size.y, rootWindow->Pos.y + rootWindow->Size.y)));
		}

		bool IsFlyoutSourceHovered(const ImVec2& min, const ImVec2& max)
		{
			constexpr auto popupFlags = ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel;
			return ContainsPoint(min, max, ImGui::GetIO().MousePos) &&
			       !ImGui::IsMouseDragging(ImGuiMouseButton_Left) &&
			       !ImGui::IsPopupOpen(nullptr, popupFlags) &&
			       ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
		}

		ImVec2 GetFlyoutPos(FlyoutState& state, const ImVec2& sourceMin, const ImVec2& sourceMax,
			float slideOffset, float scale, const ImRect& visible, const FlyoutStyle& style, bool& canOpen)
		{
			const float gap = style.verticalGap * scale;
			const float horizontalPivot = style.centerOnSource ? 0.5f : 1.0f;
			ImVec2 pos(
				style.centerOnSource ? (sourceMin.x + sourceMax.x) * 0.5f : sourceMax.x,
				sourceMax.y + gap - slideOffset);

			canOpen = state.lastSize.y <= 0.0f || sourceMax.y + gap + state.lastSize.y <= visible.Max.y;
			if (state.lastSize.x > 0.0f) {
				const float minimumAnchor = visible.Min.x + state.lastSize.x * horizontalPivot;
				const float maximumAnchor = visible.Max.x - state.lastSize.x * (1.0f - horizontalPivot);
				pos.x = minimumAnchor <= maximumAnchor ?
				            std::clamp(pos.x, minimumAnchor, maximumAnchor) :
				            (visible.Min.x + visible.Max.x) * 0.5f;
			}

			pos.x = std::floor(pos.x + 0.5f);
			pos.y = std::floor(pos.y + 0.5f);
			return pos;
		}

		bool BeginFlyoutImpl(FlyoutState& state, ImGuiID itemId, bool sourcePressed,
			const FlyoutStyle& flyoutStyle, const ImVec2* explicitSourceMin = nullptr,
			const ImVec2* explicitSourceMax = nullptr)
		{
			const ImGuiID submittedItemId = ImGui::GetItemID();
			const ImVec2 sourceMin = explicitSourceMin ? *explicitSourceMin : ImGui::GetItemRectMin();
			const ImVec2 sourceMax = explicitSourceMax ? *explicitSourceMax : ImGui::GetItemRectMax();

			const int currentFrame = ImGui::GetFrameCount();
			if (state.lastFrame >= 0 && currentFrame > state.lastFrame + 1)
				ResetFlyout(state);
			state.lastFrame = currentFrame;

			if (state.pendingFocusReturnId == itemId) {
				if (submittedItemId == itemId) {
					ImGui::FocusItem();
					ImGui::SetNavCursorVisible(true);
				}
				state.pendingFocusReturnId = 0;
			}

			const float deltaTime = ImGui::GetIO().DeltaTime;
			const bool hovered = IsFlyoutSourceHovered(sourceMin, sourceMax);
			const bool navigationPressed = sourcePressed && GImGui->NavActivatePressedId == itemId;
			const bool active = state.isOpen && state.activeId == itemId;
			bool focusOnOpen = false;

			if (state.blockedHoverId == itemId && !hovered)
				state.blockedHoverId = 0;

			if (sourcePressed && active) {
				if (state.closing) {
					state.closing = false;
					state.blockedHoverId = 0;
					state.closeTimer = 0.0f;
					state.keepOpenForNavigation = navigationPressed;
					focusOnOpen = navigationPressed;
				} else if (!flyoutStyle.keepOpenOnSourcePress) {
					RequestCloseFlyout(state);
				} else {
					state.closeTimer = 0.0f;
				}
			} else {
				const bool wantsOpen = (hovered && state.blockedHoverId != itemId) || sourcePressed;
				if (wantsOpen && (!state.isOpen || (state.activeId != itemId && !state.flyoutHovered))) {
					const bool freshOpen = !state.isOpen || state.activeId != itemId;
					state.activeId = itemId;
					state.isOpen = true;
					state.closing = false;
					state.closeTimer = 0.0f;
					if (freshOpen) {
						state.openProgress = 0.0f;
						state.keepOpenForNavigation = navigationPressed;
						state.windowName.clear();
						std::format_to(std::back_inserter(state.windowName), "{}{}",
							flyoutStyle.blurBackground ? kBlurredFlyoutWindowPrefix : kFlyoutWindowPrefix, itemId);
					}
				}

				if (wantsOpen && state.closing && state.activeId == itemId) {
					state.closing = false;
					state.closeTimer = 0.0f;
				}
				focusOnOpen = navigationPressed;
			}

			if (!state.isOpen || state.activeId != itemId)
				return false;

			if (!state.closing)
				state.openProgress = std::min(state.openProgress + kFlyoutSlideOpenSpeed * deltaTime, 1.0f);

			state.sourceMin = sourceMin;
			state.sourceMax = sourceMax;

			const float scale = GetUIScale();
			const float easedProgress = GetFlyoutEasedProgress(state);
			const float slideOffset = (1.0f - easedProgress) * kFlyoutSlideDistance * scale;
			const float alpha = std::min(state.openProgress * kFlyoutAlphaScale, 1.0f);
			bool canOpen = true;
			const ImVec2 flyoutPos = GetFlyoutPos(
				state, sourceMin, sourceMax, slideOffset, scale, GetFlyoutWindowRect(), flyoutStyle, canOpen);
			if (!canOpen) {
				state.blockedHoverId = itemId;
				ResetFlyout(state, true);
				return false;
			}

			ImGui::SetNextWindowPos(
				flyoutPos, ImGuiCond_Always, ImVec2(flyoutStyle.centerOnSource ? 0.5f : 1.0f, 0.0f));
			ImGui::SetNextWindowBgAlpha(flyoutStyle.windowBackgroundAlpha * alpha);
			if (focusOnOpen)
				ImGui::SetNextWindowFocus();

			constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
			                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
			                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
			                                   ImGuiWindowFlags_NoFocusOnAppearing;

			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, flyoutStyle.windowPadding);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, flyoutStyle.windowRounding);
			ImGui::PushStyleVar(ImGuiStyleVar_Alpha, flyoutStyle.contentAlpha * alpha);
			SKSE::stl::scope_exit restoreStyle([]() noexcept { ImGui::PopStyleVar(3); });

			const bool visible = ImGui::Begin(state.windowName.c_str(), nullptr, flags);
			state.lastSize = ImGui::GetWindowSize();
			state.flyoutMin = ImGui::GetWindowPos();
			state.flyoutMax = ImVec2(state.flyoutMin.x + state.lastSize.x, state.flyoutMin.y + state.lastSize.y);
			constexpr auto popupFlags = ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel;
			if (visible && !ImGui::IsPopupOpen(nullptr, popupFlags))
				ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

			if (!visible) {
				ImGui::End();
				return false;
			}

			restoreStyle.release();
			return true;
		}

		void EndFlyoutImpl(FlyoutState& state)
		{
			const bool flyoutHovered = ImGui::IsWindowHovered(
				ImGuiHoveredFlags_AllowWhenBlockedByActiveItem | ImGuiHoveredFlags_RootAndChildWindows);
			const bool flyoutFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
			const bool dismissRequested = flyoutFocused && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
			const bool navigationActionClosed =
				state.closing && flyoutFocused && GImGui->NavActivatePressedId != 0;
			if (dismissRequested)
				RequestCloseFlyout(state);
			if ((dismissRequested || navigationActionClosed) && state.activeId != 0)
				state.pendingFocusReturnId = state.activeId;

			state.flyoutHovered = flyoutHovered;
			if (flyoutHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
				state.keepOpenForNavigation = false;
			if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && flyoutHovered)
				state.draggedFromFlyout = true;
			else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
				state.draggedFromFlyout = false;

			ImGui::End();
			ImGui::PopStyleVar(3);

			const ImVec2 mousePos = ImGui::GetIO().MousePos;
			const bool overSource = ContainsPoint(state.sourceMin, state.sourceMax, mousePos);
			if (!dismissRequested && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !flyoutHovered && !overSource) {
				ResetFlyout(state);
				return;
			}

			const float deltaTime = ImGui::GetIO().DeltaTime;
			const bool mouseDragging = ImGui::IsMouseDragging(ImGuiMouseButton_Left);
			const ImVec2 bridgeMin(
				std::min(state.sourceMin.x, state.flyoutMin.x), std::min(state.sourceMax.y, state.flyoutMin.y));
			const ImVec2 bridgeMax(
				std::max(state.sourceMax.x, state.flyoutMax.x), std::max(state.sourceMax.y, state.flyoutMin.y));
			const bool sourceHovered = overSource || ContainsPoint(bridgeMin, bridgeMax, mousePos);

			if (state.closing) {
				state.openProgress = std::max(state.openProgress - kFlyoutSlideCloseSpeed * deltaTime, 0.0f);
				if (state.openProgress <= 0.0f)
					ResetFlyout(state, true);
			} else if (flyoutHovered || (flyoutFocused && state.keepOpenForNavigation) || sourceHovered || state.draggedFromFlyout || mouseDragging) {
				state.closeTimer = 0.0f;
			} else {
				state.closeTimer += deltaTime;
				if (state.closeTimer >= kFlyoutCloseDelay)
					state.closing = true;
			}
		}
	}

	FlyoutScope::FlyoutScope(
		FlyoutState& flyoutState, ImGuiID itemId, bool sourcePressed, const FlyoutStyle& flyoutStyle)
	{
		if (BeginFlyoutImpl(flyoutState, itemId, sourcePressed, flyoutStyle))
			state = &flyoutState;
	}

	FlyoutScope::FlyoutScope(FlyoutState& flyoutState, ImGuiID itemId, bool sourcePressed,
		const ImVec2& sourceMin, const ImVec2& sourceMax, const FlyoutStyle& flyoutStyle)
	{
		if (BeginFlyoutImpl(flyoutState, itemId, sourcePressed, flyoutStyle, &sourceMin, &sourceMax))
			state = &flyoutState;
	}

	FlyoutScope::~FlyoutScope()
	{
		if (state)
			EndFlyoutImpl(*state);
	}

	bool IsFlyoutWindowName(const char* name) noexcept
	{
		return IsUnblurredFlyoutWindowName(name) ||
		       (name && std::string_view(name).starts_with(kBlurredFlyoutWindowPrefix));
	}

	bool IsUnblurredFlyoutWindowName(const char* name) noexcept
	{
		return name && std::string_view(name).starts_with(kFlyoutWindowPrefix);
	}

	void RequestCloseFlyout(FlyoutState& state) noexcept
	{
		if (!state.isOpen)
			return;

		state.closing = true;
		state.closeTimer = 0.0f;
		state.blockedHoverId = state.activeId;
	}

	void CloseFlyout(FlyoutState& state) noexcept
	{
		ResetFlyout(state);
		state.pendingFocusReturnId = 0;
	}

	float GetFlyoutEasedProgress(const FlyoutState& state) noexcept
	{
		const float progress = std::clamp(state.openProgress, 0.0f, 1.0f);
		return 1.0f - (1.0f - progress) * (1.0f - progress);
	}

	ImU32 GetColorChannelMarker(ColorChannel channel)
	{
		constexpr std::array markers{
			IM_COL32(240, 20, 20, 255),
			IM_COL32(20, 240, 20, 255),
			IM_COL32(20, 20, 240, 255),
		};
		return markers[static_cast<size_t>(channel)];
	}

	void SetNextItemColorMarker(ColorChannel channel)
	{
		ImGui::SetNextItemColorMarker(GetColorChannelMarker(channel));
	}
}  // namespace Util
