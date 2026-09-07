#pragma once

#include <cstdint>
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

// Forward-declared so the header doesn't drag in <d3d11.h>. The plugin's PCH
// brings the real types into scope at definition sites.
struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;

// Mirrors the global `using json = nlohmann::json;` from the plugin PCH so
// the header builds standalone (e.g. in unit-test targets that don't
// precompile PCH). Identical aliases in the same scope are well-defined.
using json = nlohmann::json;

namespace Util::Subrect
{
	/** @brief A sub-region of a texture expressed in normalised UV coordinates [0,1]. */
	struct UVRegion
	{
		float x = 0.0f;
		float y = 0.0f;
		float w = 1.0f;
		float h = 1.0f;

		/** @brief True when this region covers the full frame (no crop). */
		bool IsFullEye() const { return w >= 0.999f && h >= 0.999f; }
	};

	/** @brief A sub-region of a texture expressed in absolute pixel coordinates. */
	struct PixelRegion
	{
		uint32_t x = 0;
		uint32_t y = 0;
		uint32_t w = 1;
		uint32_t h = 1;
	};

	struct StereoPixelRegions
	{
		PixelRegion leftEye;
		PixelRegion rightEye;
	};

	/** @brief A named crop preset storing a UV region and its display name. */
	struct Preset
	{
		std::string name;
		UVRegion uv;  // Left-eye UV when stereo is enabled; sole UV otherwise.
		// Right-eye UV. `std::nullopt` means "no explicit right eye — auto-mirror
		// left around x=0.5 when stereo is enabled". A default-constructed
		// `UVRegion{}` (full frame) would otherwise be ambiguous: it could mean
		// "the user wants full frame" or "the caller didn't supply one", and
		// the silent-full-frame case bites SeedDefaultPresets callers that only
		// fill `.name` and `.uv`.
		std::optional<UVRegion> rightUV{};
	};

	// "User picks a sub-rectangle of an image" controller. Crop UV is in [0,1]
	// of the source the caller passes to GetPixelRegion(). Hosts that want
	// preset-based eye selection seed Left/Right/Full Frame via SeedDefaultPresets.
	//
	// Stereo: hosts that consume a side-by-side stereo texture call
	// SetStereoEnabled(true) to track a separate right-eye UV. Right-eye UV
	// auto-mirrors left around x=0.5 unless explicitly edited; this matches
	// HMD nose-side overlap symmetry.
	/**
	 * @brief Interactive sub-rectangle selection controller for cropping a texture.
	 *
	 * Provides an ImGui editor for picking a crop region in UV space [0,1],
	 * with support for named presets (e.g. Left Eye / Right Eye / Full Frame).
	 */
	class Controller
	{
	public:
		/**
		 * @brief Load crop settings (UV region and presets) from a JSON object.
		 * @param a_json The JSON source to read from.
		 */
		void LoadSettings(const json& a_json);

		/**
		 * @brief Save current crop settings (UV region and presets) to a JSON object.
		 * @param a_json The JSON target to write into.
		 */
		void SaveSettings(json& a_json) const;

		/**
		 * @brief Provide default presets used when no user presets exist yet.
		 *
		 * Replaces the built-in "Full Frame" placeholder used when JSON has no
		 * CropPresets entry. User edits and deletions of presets persist across saves.
		 *
		 * @param defaults The default presets to seed.
		 * @param defaultPresetName Optional preset selected when no persisted
		 *                          crop state exists. The first preset remains
		 *                          the Reset Crop target.
		 */
		void SeedDefaultPresets(std::vector<Preset> defaults, std::string a_defaultPresetName = {});

		/**
		 * @brief Add any seeded default not yet in the live preset list to it, so a
		 * preset name added to SeedDefaultPresets after a user already has a persisted
		 * (non-empty) preset list still shows up in the DrawEditor dropdown -- not just
		 * on demand via ApplyPresetByName. Skips names in `seenDefaultNames` (a default
		 * the user explicitly deleted stays deleted). Does not change the current
		 * selection. Call after both SeedDefaultPresets and LoadSettings.
		 */
		void MaterializeNewDefaults();

		// Toggles right-eye UV tracking. Off by default (mono).
		// When enabled, edits to the primary UV auto-mirror to the right-eye
		// UV (around x=0.5), and SaveSettings emits the extra right-eye keys.
		void SetStereoEnabled(bool enabled);
		bool IsStereoEnabled() const { return stereoEnabled; }

		// uvStartX/uvVisibleWidth window the preview onto a sub-region of the
		// texture; crop UV stays in [0,1] of that window. imageRenderCallback,
		// when non-null, is queued via ImDrawList::AddCallback around the
		// preview Image draw (paired with ImDrawCallback_ResetRenderState) so
		// hosts can override blend state for the image specifically. Pass
		// OpaquePreviewBlendCallback when the preview texture is an RT with
		// non-1 alpha (kMAIN, etc.) to suppress menu-background bleed-through.
		/**
		 * @brief Draw the interactive crop editor widget using ImGui.
		 *
		 * @param previewSrv SRV of the texture to display as the preview.
		 * @param previewTexture The texture resource (used to query dimensions).
		 * @param uvVisibleWidth Fraction of the texture width visible in the preview window.
		 * @param uvStartX Starting U coordinate for the visible preview window.
		 * @param imageRenderCallback Optional ImDrawList callback queued around the preview
		 *        image draw for overriding blend state (paired with ImDrawCallback_ResetRenderState).
		 */
		void DrawEditor(ID3D11ShaderResourceView* previewSrv, ID3D11Texture2D* previewTexture,
			float uvVisibleWidth = 1.0f, float uvStartX = 0.0f,
			ImDrawCallback imageRenderCallback = nullptr);

		/**
		 * @brief Resolve the current crop UV region to pixel coordinates.
		 * @param width The full texture width in pixels.
		 * @param height The full texture height in pixels.
		 * @return The crop region in pixel coordinates.
		 */
		PixelRegion GetPixelRegion(uint32_t width, uint32_t height) const;

		// In stereo mode, resolves both eyes' UVs against an SBS texture by
		// dividing width by 2. In mono mode, both eyes resolve from currentUV.
		//
		// Coordinate space: both leftEye.x and rightEye.x are in PER-EYE
		// space (i.e. x in [0, fullWidth/2)) - the right eye is NOT
		// pre-offset by eyeWidth. Callers that draw into the full SBS
		// texture must add `fullWidth / 2` to rightEye.x themselves.
		StereoPixelRegions GetStereoPixelRegions(uint32_t fullWidth, uint32_t fullHeight) const;

		/** @brief Get the current crop region in UV coordinates. */
		const UVRegion& GetUV() const { return currentUV; }
		const UVRegion& GetRightEyeUV() const { return stereoEnabled ? currentRightUV : currentUV; }

		/** @brief True while the user is actively drag-resizing the crop region. */
		bool IsDragging() const { return isDraggingCrop; }

		/** @brief True when the current crop isn't one of the named presets (a user-dragged
		 *  region) -- callers that apply presets programmatically (e.g. a performance-tier
		 *  profile) use this to avoid silently overwriting it. */
		bool HasCustomCrop() const { return selectedPresetIndex < 0 || selectedPresetIndex >= static_cast<int>(presets.size()); }

		/**
		 * @brief Apply a seeded/named preset by exact name match (e.g. for a caller driving
		 * this controller from outside its own DrawEditor UI, such as a performance-tier preset).
		 *
		 * Resolves against the live `presets` list first, then falls back to materializing
		 * a not-yet-offered seeded default on demand (so a name added to SeedDefaultPresets
		 * after the user already has a persisted preset list still becomes available). A
		 * seeded default the user has already seen and explicitly deleted is not resurrected.
		 * @param name The preset's display name.
		 * @return true if a matching preset was found and applied; false (no-op) otherwise.
		 */
		bool ApplyPresetByName(const std::string& name);

		/**
		 * @brief Look up a preset's UV region by name without applying it or mutating state.
		 *
		 * Checks the live `presets` list first, then falls back to `seededDefaults` so a
		 * caller (e.g. a settings-profile matcher) can compare against a not-yet-materialized
		 * seeded default's UVs. Reuses the same name space ApplyPresetByName resolves against.
		 * @param name The preset's display name.
		 * @return The preset's UV region if found; std::nullopt otherwise.
		 */
		std::optional<UVRegion> FindPresetUV(const std::string& name) const;

	private:
		std::vector<Preset> presets;
		std::vector<Preset> seededDefaults;
		std::string defaultPresetName;
		bool placeholderDefaultPreset = false;
		// Names of seeded defaults ever offered via presets/ApplyPresetByName --
		// lets a later-added seed stay reachable while an explicitly deleted
		// default is never silently resurrected.
		std::vector<std::string> seenDefaultNames;
		int selectedPresetIndex = 0;
		char newPresetName[64] = "";

		UVRegion currentUV{};
		UVRegion currentRightUV{};
		bool stereoEnabled = false;
		// True once LoadSettings sees an explicit CropRight* key. Suppresses
		// the auto-mirror in SetStereoEnabled(true) so a deliberate JSON
		// right-eye crop survives a mono→stereo transition that happens
		// after the load.
		bool rightUVLoadedFromJson = false;

		bool isDraggingCrop = false;
		float dragStartUV[2] = { 0.0f, 0.0f };

		void EnsureDefaultPreset();
		void ClampCurrentUV();
		void ApplyPreset(int index);
		void SyncRightUV();
	};

	// Opaque-RGB blend state callback for Controller::DrawEditor. Pass when the
	// preview SRV is a render target with non-1 alpha (kMAIN, kTOTAL, etc.).
	// ImGui's default SRC_ALPHA blend would let the menu background bleed
	// through where the source alpha is < 1, making the preview look like a
	// transparency mask. This callback switches to opaque RGB-only writes
	// around the Image draw; DrawEditor queues ImDrawCallback_ResetRenderState
	// immediately after to restore default state.
	//
	// Two non-obvious regression risks if reimplemented:
	//   1. BlendEnable must stay FALSE — SRC_ALPHA causes the bleed-through.
	//   2. WriteMask must exclude alpha (RGB only). In VR, Skyrim's menu UI
	//      shader recomposites the menu plate over the SBS framebuffer with
	//      alpha blending; writing texture alpha into the menu plate RT
	//      produces a cutout visible only through the HMD. ImageOpaque seeds
	//      the destination alpha to 1 before this RGB-only draw.
	void OpaquePreviewBlendCallback(const ImDrawList*, const ImDrawCmd*);

	// Draws an ImGui image of a render-target SRV with opaque RGB blending, seeding destination
	// alpha to 1 before the draw. Use instead of a raw
	// ImGui::Image whenever the SRV is an RT with non-1 alpha, or the preview shows as a
	// transparency mask (and a cutout through the HMD in VR). No-op if a_srv is null.
	void ImageOpaque(ID3D11ShaderResourceView* a_srv, const ImVec2& a_size,
		const ImVec2& a_uv0 = ImVec2(0, 0), const ImVec2& a_uv1 = ImVec2(1, 1));
}  // namespace Util::Subrect
