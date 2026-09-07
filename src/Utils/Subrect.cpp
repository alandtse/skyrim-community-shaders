#include "Utils/Subrect.h"

#include "../I18n/I18n.h"
#include <algorithm>
#include <cmath>
#include <d3d11.h>
#include <imgui.h>

// OpaquePreviewBlendCallback lives in Subrect_PreviewBlend.cpp — that TU
// reaches into the plugin's d3d singletons, which the unit-test target
// (tests/cpp pulls Subrect.cpp standalone) can't link against.

namespace
{
	Util::Subrect::UVRegion ClampUV(Util::Subrect::UVRegion uv)
	{
		constexpr float kMinExtent = 0.01f;
		if (!std::isfinite(uv.x) || !std::isfinite(uv.y) || !std::isfinite(uv.w) || !std::isfinite(uv.h))
			return {};

		// Cap x/y so 1.0f - x/y always leaves room for the w/h floor below --
		// otherwise an endpoint (x=1) forces w to 0, producing a 1px box that
		// starts outside the eye once downstream code applies its own floor.
		uv.x = std::clamp(uv.x, 0.0f, 1.0f - kMinExtent);
		uv.y = std::clamp(uv.y, 0.0f, 1.0f - kMinExtent);
		uv.w = std::clamp(uv.w, kMinExtent, 1.0f - uv.x);
		uv.h = std::clamp(uv.h, kMinExtent, 1.0f - uv.y);

		return uv;
	}

	Util::Subrect::UVRegion DefaultUV()
	{
		return {};
	}

	Util::Subrect::UVRegion LoadUVArray(const json& arr)
	{
		Util::Subrect::UVRegion uv = DefaultUV();
		if (arr.is_array() && arr.size() == 4) {
			uv.x = arr[0];
			uv.y = arr[1];
			uv.w = arr[2];
			uv.h = arr[3];
		}
		return ClampUV(uv);
	}

	Util::Subrect::UVRegion MirrorUVHorizontal(const Util::Subrect::UVRegion& uv)
	{
		// HMD nose-side overlap: left-eye nose-side region is on the right
		// half of the eye texture; mirror around x=0.5 maps it to the
		// right-eye's left half.
		Util::Subrect::UVRegion mirrored = uv;
		mirrored.x = 1.0f - uv.x - uv.w;
		return ClampUV(mirrored);
	}

	json SaveUVToJson(const Util::Subrect::UVRegion& uv)
	{
		return { uv.x, uv.y, uv.w, uv.h };
	}

	Util::Subrect::PixelRegion UVToPixelRegion(const Util::Subrect::UVRegion& uv, uint32_t width, uint32_t height)
	{
		Util::Subrect::PixelRegion result;
		result.x = std::min<uint32_t>(width - 1, static_cast<uint32_t>(uv.x * width));
		result.y = std::min<uint32_t>(height - 1, static_cast<uint32_t>(uv.y * height));
		result.w = std::max<uint32_t>(1, static_cast<uint32_t>(uv.w * width));
		result.h = std::max<uint32_t>(1, static_cast<uint32_t>(uv.h * height));
		result.w = std::min<uint32_t>(result.w, width - result.x);
		result.h = std::min<uint32_t>(result.h, height - result.y);
		return result;
	}
}

namespace Util::Subrect
{
	void Controller::LoadSettings(const json& a_json)
	{
		placeholderDefaultPreset = false;
		if (a_json.contains("CropX"))
			currentUV.x = a_json["CropX"];
		if (a_json.contains("CropY"))
			currentUV.y = a_json["CropY"];
		if (a_json.contains("CropW"))
			currentUV.w = a_json["CropW"];
		if (a_json.contains("CropH"))
			currentUV.h = a_json["CropH"];

		// Require the full quartet before declaring either eye's UV explicit --
		// a partial config (e.g. only CropX present) would otherwise skip
		// ApplyPreset() below while CropY/W/H stay at stale/default values.
		const bool hasExplicitLeft =
			a_json.contains("CropX") && a_json.contains("CropY") &&
			a_json.contains("CropW") && a_json.contains("CropH");
		const bool hasExplicitRight =
			a_json.contains("CropRightX") && a_json.contains("CropRightY") &&
			a_json.contains("CropRightW") && a_json.contains("CropRightH");
		if (a_json.contains("CropRightX"))
			currentRightUV.x = a_json["CropRightX"];
		if (a_json.contains("CropRightY"))
			currentRightUV.y = a_json["CropRightY"];
		if (a_json.contains("CropRightW"))
			currentRightUV.w = a_json["CropRightW"];
		if (a_json.contains("CropRightH"))
			currentRightUV.h = a_json["CropRightH"];
		// Reset every load — a later LoadSettings without CropRight* keys
		// should let SetStereoEnabled(true) auto-mirror again rather than
		// preserving stale state from a prior load.
		rightUVLoadedFromJson = hasExplicitRight;

		// Reset every load -- a later LoadSettings without this key should not
		// keep treating a seeded default as deleted from a prior load.
		seenDefaultNames.clear();
		if (a_json.contains("SeenDefaultPresetNames") && a_json["SeenDefaultPresetNames"].is_array()) {
			for (auto& name : a_json["SeenDefaultPresetNames"]) {
				if (name.is_string())
					seenDefaultNames.push_back(name.get<std::string>());
			}
		}

		if (a_json.contains("CropPresets") && a_json["CropPresets"].is_array()) {
			presets.clear();
			for (auto& entry : a_json["CropPresets"]) {
				Preset preset;
				preset.name = entry.value("name", "Unknown");
				if (entry.contains("uv")) {
					preset.uv = LoadUVArray(entry["uv"]);
				}
				// Right-eye UV is optional in JSON; leave nullopt when absent so
				// ApplyPreset auto-mirrors the left eye on demand. Explicit
				// right_uv in JSON wins over any mirror — but only when it
				// looks structurally valid. LoadUVArray falls back to a
				// full-frame UV on malformed input, so without this guard a
				// bad `right_uv` payload would suppress auto-mirroring AND
				// land the right eye as full-frame, which is the worst of
				// both worlds.
				if (entry.contains("right_uv") &&
					entry["right_uv"].is_array() &&
					entry["right_uv"].size() == 4) {
					preset.rightUV = LoadUVArray(entry["right_uv"]);
				}
				presets.push_back(std::move(preset));
			}
		}

		EnsureDefaultPreset();
		ClampCurrentUV();

		// Legacy upgrade: if the JSON has the mono crop keys but no right-eye
		// keys, mirror left → right so existing user settings transition
		// cleanly. If neither side is present, leave currentRightUV alone so
		// EnsureDefaultPreset's seeded right-eye value survives.
		if (stereoEnabled && hasExplicitLeft && !hasExplicitRight) {
			SyncRightUV();
		}

		if (a_json.contains("SelectedPresetIndex")) {
			selectedPresetIndex = a_json["SelectedPresetIndex"];
			if (selectedPresetIndex >= 0 && selectedPresetIndex < static_cast<int>(presets.size())) {
				// Explicit CropX/Y/W/H already won above -- ApplyPreset would silently
				// discard them (e.g. a hand-edited crop saved with a stale/default
				// SelectedPresetIndex) in favor of the preset's UV. Only derive from
				// the preset when no explicit crop keys were present.
				if (!hasExplicitLeft) {
					ApplyPreset(selectedPresetIndex);
				}
			} else {
				selectedPresetIndex = -1;
			}
		}
	}

	void Controller::SaveSettings(json& a_json) const
	{
		a_json["CropX"] = currentUV.x;
		a_json["CropY"] = currentUV.y;
		a_json["CropW"] = currentUV.w;
		a_json["CropH"] = currentUV.h;

		if (stereoEnabled) {
			a_json["CropRightX"] = currentRightUV.x;
			a_json["CropRightY"] = currentRightUV.y;
			a_json["CropRightW"] = currentRightUV.w;
			a_json["CropRightH"] = currentRightUV.h;
		} else {
			// Caller may pass a JSON object with prior stereo keys (e.g. a
			// host that re-saves into the same in-memory config). Drop them
			// so the next load doesn't look like it had explicit stereo data.
			a_json.erase("CropRightX");
			a_json.erase("CropRightY");
			a_json.erase("CropRightW");
			a_json.erase("CropRightH");
		}

		json presetsJson = json::array();
		for (const auto& preset : presets) {
			json entry;
			entry["name"] = preset.name;
			entry["uv"] = SaveUVToJson(preset.uv);
			// Only serialize right_uv when stereo is enabled AND we have an
			// explicit value to persist. A nullopt preset implicitly means
			// "auto-mirror at apply time" and shouldn't be locked into JSON.
			if (stereoEnabled && preset.rightUV.has_value()) {
				entry["right_uv"] = SaveUVToJson(*preset.rightUV);
			}
			presetsJson.push_back(std::move(entry));
		}
		a_json["CropPresets"] = presetsJson;
		a_json["SelectedPresetIndex"] = selectedPresetIndex;
		a_json["SeenDefaultPresetNames"] = seenDefaultNames;
	}

	void Controller::SeedDefaultPresets(std::vector<Preset> defaults, std::string a_defaultPresetName)
	{
		seededDefaults = std::move(defaults);
		this->defaultPresetName = std::move(a_defaultPresetName);
	}

	void Controller::MaterializeNewDefaults()
	{
		// LoadSettings may have created a temporary Full Frame placeholder before
		// the host had a chance to seed its named defaults. Replace only that
		// internally-created placeholder so a named first-run selection can win.
		if (placeholderDefaultPreset) {
			presets.clear();
			placeholderDefaultPreset = false;
			selectedPresetIndex = 0;
			currentUV = {};
			currentRightUV = {};
		}
		EnsureDefaultPreset();
		for (const auto& preset : seededDefaults) {
			if (std::find(seenDefaultNames.begin(), seenDefaultNames.end(), preset.name) != seenDefaultNames.end())
				continue;  // already offered once (includes user-deleted defaults)
			bool alreadyPresent = false;
			for (const auto& existing : presets) {
				if (existing.name == preset.name) {
					alreadyPresent = true;
					break;
				}
			}
			if (alreadyPresent) {
				seenDefaultNames.push_back(preset.name);
				continue;
			}
			presets.push_back(preset);
			seenDefaultNames.push_back(preset.name);
		}
	}

	void Controller::SetStereoEnabled(bool enabled)
	{
		if (stereoEnabled == enabled) {
			return;
		}
		stereoEnabled = enabled;
		// Only auto-mirror left→right when the right-eye UV hasn't been
		// explicitly loaded from JSON. Otherwise a caller that does
		// `LoadSettings` (stereo off) then `SetStereoEnabled(true)` would
		// silently overwrite a deliberate persisted right-eye crop.
		if (stereoEnabled && !rightUVLoadedFromJson) {
			SyncRightUV();
		}
	}

	void Controller::DrawEditor(ID3D11ShaderResourceView* previewSrv, ID3D11Texture2D* previewTexture, float uvVisibleWidth, float uvStartX, ImDrawCallback imageRenderCallback)
	{
		// Hosts that render without first calling LoadSettings would otherwise
		// see an empty presets vector and the combo would mislabel as "(Custom)".
		EnsureDefaultPreset();
		if (selectedPresetIndex < 0 || selectedPresetIndex >= static_cast<int>(presets.size())) {
			selectedPresetIndex = 0;
		}

		std::string currentPreview =
			(selectedPresetIndex >= 0 && selectedPresetIndex < static_cast<int>(presets.size())) ? presets[selectedPresetIndex].name : T("ui.subrect.custom", "(Custom)");

		if (ImGui::BeginCombo(T("ui.subrect.crop_preset", "Crop Preset"), currentPreview.c_str())) {
			for (int i = 0; i < static_cast<int>(presets.size()); ++i) {
				const bool isSelected = selectedPresetIndex == i;
				if (ImGui::Selectable(presets[i].name.c_str(), isSelected)) {
					ApplyPreset(i);
				}
				if (isSelected) {
					ImGui::SetItemDefaultFocus();
				}
			}
			ImGui::EndCombo();
		}

		ImGui::InputText(T("ui.subrect.save_as", "Save As"), newPresetName, sizeof(newPresetName));
		ImGui::SameLine();
		if (ImGui::Button(T("ui.subrect.save_preset", "Save Preset"))) {
			std::string presetName = newPresetName;
			if (!presetName.empty()) {
				// Preserve the right-eye UV only when stereo is on. In mono
				// mode currentRightUV is not tracked against currentUV, so
				// snapshotting it would falsely mark the preset as having an
				// explicit right eye and disable the auto-mirror fallback
				// once stereo is later enabled. Leave rightUV as nullopt in
				// mono — ApplyPreset will mirror left at apply time.
				Preset newPreset{ .name = presetName, .uv = currentUV };
				if (stereoEnabled) {
					newPreset.rightUV = currentRightUV;
				}
				presets.push_back(std::move(newPreset));
				selectedPresetIndex = static_cast<int>(presets.size()) - 1;
				newPresetName[0] = '\0';
			}
		}

		if (selectedPresetIndex > 0) {
			ImGui::SameLine();
			if (ImGui::Button(T("ui.subrect.delete_preset", "Delete Preset"))) {
				presets.erase(presets.begin() + selectedPresetIndex);
				ApplyPreset(0);
			}
		}

		ImGui::SameLine();
		if (ImGui::Button(T("ui.subrect.reset_crop", "Reset Crop"))) {
			ApplyPreset(0);
		}

		ImGui::Spacing();
		ImGui::PushItemWidth(250.0f);
		bool changed = false;
		changed |= ImGui::SliderFloat2(T("ui.subrect.position_uv", "Position UV (X, Y)"), &currentUV.x, 0.0f, 1.0f, "%.3f");
		changed |= ImGui::SliderFloat2(T("ui.subrect.size_uv", "Size UV (W, H)"), &currentUV.w, 0.01f, 1.0f, "%.3f");
		ImGui::PopItemWidth();

		if (changed) {
			selectedPresetIndex = -1;
			ClampCurrentUV();
			if (stereoEnabled) {
				SyncRightUV();
			}
		}

		ImGui::Spacing();
		ImGui::Text("%s", T("ui.subrect.interactive_cropping", "Interactive Cropping (Drag on the image to select)"));

		if (!previewSrv || !previewTexture) {
			ImGui::TextDisabled("%s", T("ui.subrect.preview_unavailable", "Preview unavailable."));
			return;
		}

		D3D11_TEXTURE2D_DESC desc{};
		previewTexture->GetDesc(&desc);
		float maxWidth = std::min(400.0f, ImGui::GetContentRegionAvail().x);
		float aspectRatio = (static_cast<float>(desc.Width) * uvVisibleWidth) / static_cast<float>(desc.Height);
		ImVec2 imageSize(maxWidth, maxWidth / aspectRatio);
		ImVec2 cursorPos = ImGui::GetCursorScreenPos();

		ImDrawList* hostDrawList = ImGui::GetWindowDrawList();
		if (imageRenderCallback) {
			hostDrawList->AddCallback(imageRenderCallback, nullptr);
		}
		ImGui::Image(reinterpret_cast<ImTextureID>(previewSrv), imageSize,
			ImVec2(uvStartX, 0.0f), ImVec2(uvStartX + uvVisibleWidth, 1.0f));
		if (imageRenderCallback) {
			hostDrawList->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
		}

		ImGui::SetCursorScreenPos(cursorPos);
		ImGui::SetNextItemAllowOverlap();
		ImGui::InvisibleButton("##subrectCanvas", imageSize);

		ImVec2 mousePos = ImGui::GetIO().MousePos;
		ImVec2 relativeMouseP(mousePos.x - cursorPos.x, mousePos.y - cursorPos.y);
		float mouseUVX = std::clamp(relativeMouseP.x / imageSize.x, 0.0f, 1.0f);
		float mouseUVY = std::clamp(relativeMouseP.y / imageSize.y, 0.0f, 1.0f);

		if (ImGui::IsItemActive() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
			isDraggingCrop = true;
			selectedPresetIndex = -1;
			dragStartUV[0] = mouseUVX;
			dragStartUV[1] = mouseUVY;
			currentUV.x = mouseUVX;
			currentUV.y = mouseUVY;
			currentUV.w = 0.0f;
			currentUV.h = 0.0f;
		}

		if (isDraggingCrop) {
			float minX = std::min(dragStartUV[0], mouseUVX);
			float minY = std::min(dragStartUV[1], mouseUVY);
			float maxX = std::max(dragStartUV[0], mouseUVX);
			float maxY = std::max(dragStartUV[1], mouseUVY);

			currentUV.x = minX;
			currentUV.y = minY;
			currentUV.w = maxX - minX;
			currentUV.h = maxY - minY;
			ClampCurrentUV();
			if (stereoEnabled) {
				SyncRightUV();
			}

			if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
				isDraggingCrop = false;
			}
		}

		ImDrawList* drawList = ImGui::GetWindowDrawList();
		ImVec2 pMin(cursorPos.x + currentUV.x * imageSize.x, cursorPos.y + currentUV.y * imageSize.y);
		ImVec2 pMax(cursorPos.x + (currentUV.x + currentUV.w) * imageSize.x,
			cursorPos.y + (currentUV.y + currentUV.h) * imageSize.y);
		drawList->AddRect(pMin, pMax, IM_COL32(0, 255, 0, 255), 0.0f, 0, 2.0f);
	}

	PixelRegion Controller::GetPixelRegion(uint32_t width, uint32_t height) const
	{
		return UVToPixelRegion(currentUV, width, height);
	}

	StereoPixelRegions Controller::GetStereoPixelRegions(uint32_t fullWidth, uint32_t fullHeight) const
	{
		// Degenerate inputs would underflow UVToPixelRegion's `width - 1` /
		// `height - 1` computations into huge values. Fail safe with empty
		// regions so callers can detect the bad-input case via .w == 0.
		if (fullWidth < 2 || fullHeight == 0) {
			return { PixelRegion{ 0, 0, 0, 0 }, PixelRegion{ 0, 0, 0, 0 } };
		}
		// Each eye occupies half the SBS texture width. In mono mode, both
		// eyes report the same region so callers don't need to branch.
		const uint32_t eyeWidth = fullWidth / 2;
		StereoPixelRegions regions;
		regions.leftEye = UVToPixelRegion(currentUV, eyeWidth, fullHeight);
		regions.rightEye = UVToPixelRegion(stereoEnabled ? currentRightUV : currentUV, eyeWidth, fullHeight);
		return regions;
	}

	void Controller::EnsureDefaultPreset()
	{
		if (!presets.empty()) {
			return;
		}
		if (!seededDefaults.empty()) {
			presets = seededDefaults;
			for (const auto& preset : presets) {
				if (std::find(seenDefaultNames.begin(), seenDefaultNames.end(), preset.name) == seenDefaultNames.end())
					seenDefaultNames.push_back(preset.name);
			}
			int defaultIndex = 0;
			if (!defaultPresetName.empty()) {
				for (int i = 0; i < static_cast<int>(presets.size()); ++i) {
					if (presets[i].name == defaultPresetName) {
						defaultIndex = i;
						break;
					}
				}
			}
			// currentUV must match what the combo shows as selected; otherwise
			// the selected preset appears chosen but the crop region stays stale.
			currentUV = presets[defaultIndex].uv;
			// nullopt rightUV means "auto-mirror" — match the same fallback
			// ApplyPreset uses below.
			currentRightUV = presets[defaultIndex].rightUV.value_or(MirrorUVHorizontal(currentUV));
			selectedPresetIndex = defaultIndex;
			placeholderDefaultPreset = false;
		} else {
			presets.push_back(Preset{ .name = "Full Frame", .uv = DefaultUV() });
			placeholderDefaultPreset = true;
		}
	}

	void Controller::ClampCurrentUV()
	{
		currentUV = ClampUV(currentUV);
		currentRightUV = ClampUV(currentRightUV);
	}

	bool Controller::ApplyPresetByName(const std::string& name)
	{
		EnsureDefaultPreset();
		for (int i = 0; i < static_cast<int>(presets.size()); ++i) {
			if (presets[i].name == name) {
				ApplyPreset(i);
				return true;
			}
		}
		// Not yet in `presets`: materialize a not-yet-offered seeded default,
		// unless it.s already in seenDefaultNames (user saw and deleted it).
		for (const auto& preset : seededDefaults) {
			if (preset.name != name)
				continue;
			if (std::find(seenDefaultNames.begin(), seenDefaultNames.end(), name) != seenDefaultNames.end())
				return false;
			presets.push_back(preset);
			seenDefaultNames.push_back(name);
			ApplyPreset(static_cast<int>(presets.size()) - 1);
			return true;
		}
		return false;
	}

	std::optional<UVRegion> Controller::FindPresetUV(const std::string& name) const
	{
		for (const auto& preset : presets) {
			if (preset.name == name)
				return preset.uv;
		}
		for (const auto& preset : seededDefaults) {
			if (preset.name == name)
				return preset.uv;
		}
		return std::nullopt;
	}

	void Controller::ApplyPreset(int index)
	{
		EnsureDefaultPreset();
		selectedPresetIndex = std::clamp(index, 0, static_cast<int>(presets.size()) - 1);
		currentUV = presets[selectedPresetIndex].uv;
		// Nullopt right-UV → mirror left around x=0.5. This is the safe default
		// for presets created without a stereo-specific intent (e.g. via
		// SeedDefaultPresets with only .name + .uv specified).
		currentRightUV = presets[selectedPresetIndex].rightUV.value_or(MirrorUVHorizontal(currentUV));
		ClampCurrentUV();
	}

	void Controller::SyncRightUV()
	{
		currentRightUV = MirrorUVHorizontal(currentUV);
	}
}  // namespace Util::Subrect
