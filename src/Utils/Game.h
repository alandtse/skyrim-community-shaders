// functions that get or set game data

#pragma once

#include <cstdint>
#include <optional>

/**
 @def GET_INSTANCE_MEMBER
 @brief Set variable in current namespace based on instance member from GetRuntimeData or GetVRRuntimeData.

 @warning The class must have both a GetRuntimeData() and GetVRRuntimeData() function.

 @param a_value The instance member value to access (e.g., renderTargets).
 @param a_source The instance of the class (e.g., state).
 @result The a_value will be set as a variable in the current namespace. (e.g., auto& renderTargets = state->renderTargets;)
 */
#define GET_INSTANCE_MEMBER(a_value, a_source)                                     \
	/* Keep raw runtime check: this macro can be used before globals::ReInit(). */ \
	auto& a_value = !REL::Module::IsVR() ? a_source->GetRuntimeData().a_value : a_source->GetVRRuntimeData().a_value;

/**
 @def GET_INSTANCE_MEMBER_PTR
 @brief Return refptr to runtimedata in current namespace based on instance member from GetRuntimeData or GetVRRuntimeData.

 @warning The class must have both a GetRuntimeData() and GetVRRuntimeData() function.

 @param a_value The instance member value to access (e.g., renderTargets).
 @param a_source The instance of the class (e.g., state).
 @result The a_value will be returned as a refptr. (e.g., &state->renderTargets;)
 */
#define GET_INSTANCE_MEMBER_PTR(a_value, a_source)                                 \
	/* Keep raw runtime check: this macro can be used before globals::ReInit(). */ \
	&(!REL::Module::IsVR() ? a_source->GetRuntimeData().a_value : a_source->GetVRRuntimeData().a_value)

/**
 @def GET_INSTANCE_MEMBER_VRPTR
 @brief Like GET_INSTANCE_MEMBER, for classes whose GetVRRuntimeData() returns
 a pointer instead of a reference (e.g. ImageSpaceManager).

 @warning The class must have GetRuntimeData() (reference) and GetVRRuntimeData() (pointer).

 @param a_value The instance member value to access (e.g., BSImagespaceShaderApplyReflections).
 @param a_source The instance of the class (e.g., imageSpaceManager).
 @result The a_value will be set as a variable in the current namespace.
 */
#define GET_INSTANCE_MEMBER_VRPTR(a_value, a_source)                               \
	/* Keep raw runtime check: this macro can be used before globals::ReInit(). */ \
	auto& a_value = !REL::Module::IsVR() ? a_source->GetRuntimeData().a_value : a_source->GetVRRuntimeData()->a_value;

/**
 @def CALL_WITH_RUNTIME_DATA
 @brief Invoke a_fn with a_source's VR-or-flat RuntimeData, picking by IsVR(). Use when
 GetRuntimeData()/GetVRRuntimeData() return differently-laid-out struct types (so a single
 ternary can't unify them) but the same logic still applies to either -- a_fn must be a
 generic callable (e.g. a lambda taking `auto&`).

 @warning The class must have both a GetRuntimeData() and GetVRRuntimeData() function.

 @param a_source The instance of the class (e.g., shadowState).
 @param a_fn A generic callable invoked with the resolved runtime data.
 */
#define CALL_WITH_RUNTIME_DATA(a_source, a_fn)                                                \
	/* do/while(0): makes this act like one statement, safe inside a caller's own if/else. */ \
	do {                                                                                      \
		/* Keep raw runtime check: this macro can be used before globals::ReInit(). */        \
		if (!REL::Module::IsVR())                                                             \
			(a_fn)((a_source)->GetRuntimeData());                                             \
		else                                                                                  \
			(a_fn)((a_source)->GetVRRuntimeData());                                           \
	} while (0)

namespace Util::EnvironmentControls
{
	inline constexpr float kDefaultTimeScale = 20.0f;
	inline constexpr float kHoursPerDay = 24.0f;

	/// Select a weather, retargeting an existing lock and ending any temporary preview.
	void ChangeWeather(RE::TESWeather* weather, bool instant);
	/// Release locks and return to the engine's default weather.
	void ResetWeather();
	/// Refresh an edited current weather without releasing an active lock.
	void RefreshWeather(RE::TESWeather* weather);
	/// Hold the dominant weather during a slider interaction, ending any temporary preview.
	void BeginGameHourScrub();
	/// Release the slider's temporary weather lock, preserving any earlier explicit lock.
	void EndGameHourScrub();
	/// Set the hour and end any preview; optionally defer sky synchronization while scrubbing.
	bool SetGameHour(float hour, bool synchronize = true);
	/// Update the running or saved timescale and end any temporary preview.
	void SetTimeScale(float timeScale);

	/// Report successful installation of the existing engine weather guards.
	void SetWeatherLockAvailable();
	/// Return whether engine weather changes can be guarded.
	bool IsWeatherLockAvailable();
	/// Return the weather held by either editor, or nullptr.
	RE::TESWeather* GetLockedWeather();
	/// Replace a preview with an explicit weather lock; nullptr releases the lock.
	void SetLockedWeather(RE::TESWeather* weather);
	/// Maintain shared weather and preview-time locks once per frame.
	void MaintainLocks();
	/// Pause game time, ending any temporary preview first.
	void PauseTime();
	/// Restore game-time progression, ending any temporary preview first.
	void ResumeTime();
	/// Return whether the shared time control is paused.
	bool IsTimePaused();
	/// Return the timescale to restore when resuming.
	float GetSavedTimeScale();
	/// Reset the running or saved timescale to its default.
	void ResetTimeScale();
	/// Keep time running across loading, sleep/wait and fast-travel menus.
	void SetTimeRunningForMenu(bool needsRunningTime);
	/// Start or retarget a weather/time lock; time-only previews hold current weather and Stop restores prior controls.
	bool StartPreview(RE::TESWeather* weather, std::optional<float> hour);
	/// Release the preview and restore the controls that preceded it, not the old game hour.
	void StopPreview();
	/// Return whether a temporary weather or time lock is active.
	bool IsPreviewActive();
}

namespace Util
{
	/** @brief Pending celestial synchronization requests consumed by the sky update hook. */
	struct CelestialTransitionRequest
	{
		bool timeJump = false;
		bool gameLoad = false;
	};

	/** @brief Sets whether a sky update hook can synchronize celestial transitions. */
	void SetCelestialTransitionHandlerAvailable(bool a_available);
	/** @brief Requests celestial synchronization after an abrupt game-time change. */
	void RequestTimeJumpTransition();
	/** @brief Requests celestial synchronization after loading an existing save. */
	void RequestGameLoadTransition();
	/** @brief Consumes pending celestial synchronization requests. */
	[[nodiscard]] CelestialTransitionRequest ConsumeCelestialTransitionRequest();
	/** @brief Marks a celestial transition ready for dependent rendering updates. */
	void CompleteCelestialTransition();
	/** @brief Returns the latest completed celestial-transition generation. */
	[[nodiscard]] std::uint32_t GetCompletedCelestialTransitionGeneration();

	void StoreTransform3x4NoScale(DirectX::XMFLOAT3X4& Dest, const RE::NiTransform& Source);

	float4 TryGetWaterData(float offsetX, float offsetY);
	float4 GetCameraData();
	bool GetTemporal();
	// Toggle the ISTemporalAA scene-resolve flag (companion to GetTemporal).
	void SetTemporal(bool enabled);
	// Disable vanilla TAA (bUseTAA:Display). CS drives TAA itself.
	void DisableVanillaTAA();
	float GetVerticalFOVRad();

	RE::NiPoint3 GetAverageEyePosition();
	RE::NiPoint3 GetEyePosition(int eyeIndex);
	RE::BSGraphics::ViewData GetCameraData(int eyeIndex);

	float2 ConvertToDynamic(float2 a_size, bool a_ignoreLock = false);

	// Game unit conversions
	namespace Units
	{
		// Conversion constants
		constexpr float GAME_UNIT_TO_CM = 1.428f;
		constexpr float GAME_UNIT_TO_M = GAME_UNIT_TO_CM / 100.0f;
		constexpr float GAME_UNIT_TO_FEET = GAME_UNIT_TO_CM / 30.48f;
		constexpr float GAME_UNIT_TO_INCHES = GAME_UNIT_TO_CM / 2.54f;

		// Wind speed conversions
		constexpr float WIND_RAW_TO_NORMALIZED = 1.0f / 255.0f;  // Raw to 0-1 scale
		constexpr float WIND_RAW_TO_PERCENT = 100.0f / 255.0f;   // Raw to percentage

		// Direction conversions
		constexpr float DIR_RAW_TO_DEGREES = 360.0f / 256.0f;    // Raw 0-256 to 0-360 degrees
		constexpr float DIR_RANGE_TO_DEGREES = 180.0f / 256.0f;  // Range 0-256 to 0-180 degrees
		constexpr float RADIANS_TO_DEGREES = 180.0f / DirectX::XM_PI;

		// Distance conversions
		inline float GameUnitsToMeters(float gameUnits) { return gameUnits * GAME_UNIT_TO_M; }
		inline float GameUnitsToCm(float gameUnits) { return gameUnits * GAME_UNIT_TO_CM; }
		inline float GameUnitsToFeet(float gameUnits) { return gameUnits * GAME_UNIT_TO_FEET; }
		inline float GameUnitsToInches(float gameUnits) { return gameUnits * GAME_UNIT_TO_INCHES; }

		// Wind speed conversions
		inline float WindRawToNormalized(uint8_t rawWind) { return rawWind * WIND_RAW_TO_NORMALIZED; }
		inline float WindRawToPercent(uint8_t rawWind) { return rawWind * WIND_RAW_TO_PERCENT; }

		// Direction conversions
		inline float DirectionRawToDegrees(uint8_t rawDirection) { return rawDirection * DIR_RAW_TO_DEGREES; }
		inline float DirectionRangeToDegrees(uint8_t rawRange) { return rawRange * DIR_RANGE_TO_DEGREES; }
		inline float RadiansToDegrees(float radians) { return radians * RADIANS_TO_DEGREES; }

		// Angle normalization helpers
		inline float NormalizeDegrees0To360(float degrees)
		{
			if (!std::isfinite(degrees))
				return 0.0f;
			while (degrees < 0.0f) degrees += 360.0f;
			while (degrees >= 360.0f) degrees -= 360.0f;
			return degrees;
		}

		inline float NormalizeDegreesToSignedRange(float degrees)
		{
			if (!std::isfinite(degrees))
				return 0.0f;
			while (degrees > 180.0f) degrees -= 360.0f;
			while (degrees < -180.0f) degrees += 360.0f;
			return degrees;
		}

		// Formatted string helpers for tooltips
		inline std::string FormatDistance(float gameUnits)
		{
			return std::format("{:.1f} units ({:.2f} m, {:.1f} ft)",
				gameUnits, GameUnitsToMeters(gameUnits), GameUnitsToFeet(gameUnits));
		}
		inline std::string FormatWindSpeed(uint8_t rawWind)
		{
			return std::format("{:.1f}% (raw {}, {:.2f} normalized)",
				WindRawToPercent(rawWind), rawWind, WindRawToNormalized(rawWind));
		}

		inline std::string FormatDirection(uint8_t rawDirection)
		{
			return std::format("{:.1f}° (raw {})",
				DirectionRawToDegrees(rawDirection), rawDirection);
		}
	}

	struct DispatchCount
	{
		uint x;
		uint y;
	};
	DispatchCount GetScreenDispatchCount(bool a_dynamic = true);

	/**
	 * @brief Checks if dynamic resolution is currently enabled.
	 *
	 * @return true if dynamic resolution is enabled, false otherwise.
	 */
	bool IsDynamicResolution();

	/**
	 * Usage:
	 * static FrameChecker frame_checker;
	 * if(frame_checker.isNewFrame())
	 *     ...
	*/
	class FrameChecker
	{
	private:
		uint32_t last_frame = UINT32_MAX;

	public:
		bool IsNewFrame(uint32_t frame)
		{
			bool retval = last_frame != frame;
			last_frame = frame;
			return retval;
		}
		bool IsNewFrame();
	};

	/**
     * @brief Retrieves the seasonal texture swap for a given texture set, if available.
     *
     * This function checks if a given texture set has been swapped by Seasons of Skyrim.
     * If swapped, pad12C will be > 0 and will be the formid of the swapped texture set.
     *
     * @param textureSet Pointer to the original BGSTextureSet to check for seasonal swaps.
     *                   Can be nullptr.
     *
     * @return Pointer to the seasonal swap texture set if found and valid, otherwise
     *         returns the original textureSet parameter. Returns nullptr if the input
     *         textureSet is nullptr.
     */
	[[nodiscard]] RE::BGSTextureSet* GetSeasonalSwap(RE::BGSTextureSet* textureSet);

	// TESForm formatting helpers
	std::string FormatTESForm(const RE::TESForm* form);
	std::string FormatWeather(const RE::TESWeather* weather);

	bool IsInterior();

	/**
	 * @brief Converts a 2D world position to cell coordinates.
	 *
	 * @param worldPos The world position as a 2D point.
	 * @param x Output parameter for the cell X coordinate.
	 * @param y Output parameter for the cell Y coordinate.
	 */
	void WorldToCell(const RE::NiPoint2& worldPos, int32_t& x, int32_t& y);

	/**
	 * @brief Converts a 3D world position to cell coordinates.
	 *
	 * @param worldPos The world position as a 3D point.
	 * @param x Output parameter for the cell X coordinate.
	 * @param y Output parameter for the cell Y coordinate.
	 */
	void WorldToCell(const RE::NiPoint3& worldPos, int32_t& x, int32_t& y);

	/**
	 * @brief Converts a four-character string to a uint32_t for efficient comparison.
	 *
	 * Four Character Code (FCC) allows representing section names in plugin files
	 * as human-readable strings in code, while compiling to simple integer comparisons
	 * with no runtime cost compared to string comparisons.
	 *
	 * @param s A null-terminated array of exactly 4 characters.
	 * @return The four characters packed into a uint32_t (little-endian).
	 */
	constexpr uint32_t FCC(const char s[4]) noexcept
	{
		return s[0] | s[1] << 8 | s[2] << 16 | s[3] << 24;
	}
}  // namespace Util
