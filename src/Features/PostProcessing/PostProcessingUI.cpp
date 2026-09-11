#include "PostProcessingUI.h"

#include "Util.h"

#include "IconsFontAwesome5.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <imgui.h>
#include <imgui_internal.h>
#include <string>

namespace PostProcessingUI
{
	namespace
	{
		constexpr float ActionButtonHeight1080p = 29.f;
		constexpr float ButtonIconScale = 0.95f;
		constexpr float ButtonIconTextExtraSpacing1080p = 2.f;
		constexpr std::array<const char*, 4> FFTResolutionLabels = { "128", "256", "512", "1024" };
		constexpr std::array<int, FFTResolutionLabels.size()> FFTResolutionValues = { 128, 256, 512, 1024 };
		constexpr int FFTResolutionDefaultIndex = 1;
		constexpr int Float3ComponentCount = 3;
		constexpr std::array<const char*, Float3ComponentCount> Float3ComponentIds = { "##X", "##Y", "##Z" };
		constexpr std::array<Util::ColorChannel, Float3ComponentCount> RGBFloatDragChannels = {
			Util::ColorChannel::Red,
			Util::ColorChannel::Green,
			Util::ColorChannel::Blue
		};

		struct LeadingIcon
		{
			ImWchar codepoint;
			const ImFontGlyph* glyph;
			float scale;
			float fontSize;
			const char* text;
			const char* textEnd;
		};

		float GetActionButtonHeight()
		{
			return std::max(ImGui::GetFrameHeight(), ActionButtonHeight1080p * Util::GetUIScale());
		}

		float GetFloat3ComponentWidth(float availableWidth, float& previousSplit, int componentIndex)
		{
			const float nextSplit = std::floor(availableWidth * static_cast<float>(componentIndex + 1) / Float3ComponentCount);
			const float width = std::max(nextSplit - previousSplit, Util::GetUIScale());
			previousSplit = nextSplit;
			return width;
		}

		bool GetLeadingIcon(const char* label, LeadingIcon& icon)
		{
			unsigned int codepoint = 0;
			const int byteCount = ImTextCharFromUtf8(&codepoint, label, nullptr);
			if (byteCount <= 0 || codepoint < ICON_MIN_FA || codepoint > ICON_MAX_FA)
				return false;

			auto* font = ImGui::GetFont();
			const float fontSize = ImGui::GetFontSize() * ButtonIconScale;
			font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, label, label + byteCount);
			auto* fontBaked = font->GetFontBaked(fontSize);
			auto* glyph = fontBaked->FindGlyphNoFallback(static_cast<ImWchar>(codepoint));
			if (!glyph)
				return false;

			const char* text = label + byteCount;
			while (*text == ' ')
				text++;

			icon = {
				.codepoint = static_cast<ImWchar>(codepoint),
				.glyph = glyph,
				.scale = fontSize / fontBaked->Size,
				.fontSize = fontSize,
				.text = text,
				.textEnd = ImGui::FindRenderedTextEnd(text)
			};
			return true;
		}

		float GetIconWidth(const LeadingIcon& icon)
		{
			return (icon.glyph->X1 - icon.glyph->X0) * icon.scale;
		}

		float GetIconTextGap(const LeadingIcon& icon)
		{
			return icon.text != icon.textEnd ?
			           ImGui::GetStyle().ItemInnerSpacing.x + ButtonIconTextExtraSpacing1080p * Util::GetUIScale() :
			           0.f;
		}

		float GetIconButtonContentWidth(const LeadingIcon& icon)
		{
			return GetIconWidth(icon) + GetIconTextGap(icon) + ImGui::CalcTextSize(icon.text, icon.textEnd, false).x;
		}

		void DrawCenteredIconButtonContents(const LeadingIcon& icon)
		{
			if (!ImGui::IsItemVisible())
				return;

			const ImVec2 itemMin = ImGui::GetItemRectMin();
			const ImVec2 itemMax = ImGui::GetItemRectMax();
			const ImVec2 itemCenter{
				(itemMin.x + itemMax.x) * 0.5f,
				(itemMin.y + itemMax.y) * 0.5f
			};
			const ImVec2 textSize = ImGui::CalcTextSize(icon.text, icon.textEnd, false);
			const float contentWidth = GetIconWidth(icon) + GetIconTextGap(icon) + textSize.x;
			const float contentStartX = itemCenter.x - contentWidth * 0.5f;
			const ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);

			const ImVec2 iconPos{
				ImTrunc(contentStartX - icon.glyph->X0 * icon.scale),
				ImTrunc(itemCenter.y - (icon.glyph->Y0 + icon.glyph->Y1) * icon.scale * 0.5f)
			};
			ImGui::GetFont()->RenderChar(ImGui::GetWindowDrawList(), icon.fontSize, iconPos, textColor, icon.codepoint);

			if (icon.text != icon.textEnd) {
				const ImVec2 textPos{
					ImTrunc(contentStartX + GetIconWidth(icon) + GetIconTextGap(icon)),
					ImTrunc(itemCenter.y - textSize.y * 0.5f)
				};
				ImGui::GetWindowDrawList()->AddText(textPos, textColor, icon.text, icon.textEnd);
			}
		}

		bool CenteredIconButton(const char* label, const ImVec2& size, const LeadingIcon& icon)
		{
			const ImVec4 transparentText{ 0.f, 0.f, 0.f, 0.f };
			ImGui::PushStyleColor(ImGuiCol_Text, transparentText);
			const bool pressed = ImGui::Button(label, size);
			ImGui::PopStyleColor();
			DrawCenteredIconButtonContents(icon);
			return pressed;
		}
	}

	bool ActionButton(const char* label)
	{
		LeadingIcon icon{};
		if (!GetLeadingIcon(label, icon))
			return ImGui::Button(label, { 0.f, GetActionButtonHeight() });

		const float buttonWidth = GetIconButtonContentWidth(icon) + ImGui::GetStyle().FramePadding.x * 2.f;
		return CenteredIconButton(label, { buttonWidth, GetActionButtonHeight() }, icon);
	}

	bool IconButton(const char* icon)
	{
		const float buttonSize = GetActionButtonHeight();
		LeadingIcon leadingIcon{};
		if (!GetLeadingIcon(icon, leadingIcon))
			return ImGui::Button(icon, { buttonSize, buttonSize });

		return CenteredIconButton(icon, { buttonSize, buttonSize }, leadingIcon);
	}

	bool FFTResolutionCombo(const char* label, int& resolution)
	{
		int currentIndex = FFTResolutionDefaultIndex;
		bool matched = false;
		for (int i = 0; i < static_cast<int>(FFTResolutionValues.size()); i++) {
			if (FFTResolutionValues[i] == resolution) {
				currentIndex = i;
				matched = true;
				break;
			}
		}

		if (!matched)
			resolution = FFTResolutionValues[currentIndex];

		if (!ImGui::Combo(label, &currentIndex, FFTResolutionLabels.data(), static_cast<int>(FFTResolutionLabels.size())))
			return !matched;

		resolution = FFTResolutionValues[currentIndex];
		return true;
	}

	bool LabeledSliderFloat3(
		const char* label,
		float* values,
		const std::array<const char*, 3>& componentLabels,
		float min,
		float max,
		const char* format,
		ImGuiSliderFlags flags)
	{
		const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
		const float availableWidth = ImGui::CalcItemWidth() - spacing * (Float3ComponentCount - 1);
		float previousSplit = 0.f;
		bool changed = false;

		ImGui::BeginGroup();
		ImGui::PushID(label);
		for (int i = 0; i < Float3ComponentCount; i++) {
			if (i > 0)
				ImGui::SameLine(0.f, spacing);

			ImGui::SetNextItemWidth(GetFloat3ComponentWidth(availableWidth, previousSplit, i));
			const std::string componentFormat = std::string(format) + " " + componentLabels[i];
			changed |= ImGui::SliderFloat(Float3ComponentIds[i], &values[i], min, max, componentFormat.c_str(), flags);
		}
		ImGui::PopID();

		const char* labelEnd = ImGui::FindRenderedTextEnd(label);
		if (label != labelEnd) {
			ImGui::SameLine(0.f, spacing);
			ImGui::TextUnformatted(label, labelEnd);
		}
		ImGui::EndGroup();
		return changed;
	}

	bool RGBFloatDrag3(const char* label, float* values, float speed, const char* format, ImGuiSliderFlags flags)
	{
		return RGBFloatDrag3(label, values, speed, 0.f, 0.f, format, flags);
	}

	bool RGBFloatDrag3(const char* label, float* values, float speed, float min, float max, const char* format, ImGuiSliderFlags flags)
	{
		const auto& style = ImGui::GetStyle();
		const float spacing = style.ItemInnerSpacing.x;
		const float availableWidth = ImGui::CalcItemWidth() - spacing * (Float3ComponentCount - 1);
		float previousSplit = 0.f;

		bool changed = false;
		ImGui::BeginGroup();
		ImGui::PushID(label);
		for (int i = 0; i < Float3ComponentCount; i++) {
			if (i > 0)
				ImGui::SameLine(0.f, spacing);
			ImGui::SetNextItemWidth(GetFloat3ComponentWidth(availableWidth, previousSplit, i));
			ImGui::SetNextItemColorMarker(Util::GetColorChannelMarker(RGBFloatDragChannels[i]));
			changed |= ImGui::DragFloat(Float3ComponentIds[i], &values[i], speed, min, max, format, flags | ImGuiSliderFlags_ColorMarkers);
		}
		ImGui::PopID();

		const char* labelEnd = ImGui::FindRenderedTextEnd(label);
		if (label != labelEnd) {
			ImGui::SameLine(0.f, spacing);
			ImGui::TextUnformatted(label, labelEnd);
		}
		ImGui::EndGroup();
		return changed;
	}
}
