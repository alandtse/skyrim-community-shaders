#include "Game.h"

#include <atomic>
#include <mutex>

#include "Globals.h"
#include "State.h"

namespace
{
	std::atomic_bool celestialTransitionHandlerAvailable{ false };
	std::atomic_bool timeJumpTransitionRequested{ false };
	std::atomic_bool gameLoadTransitionRequested{ false };
	std::atomic_uint32_t completedCelestialTransitionGeneration{ 0 };

	void MarkCelestialTransitionComplete()
	{
		completedCelestialTransitionGeneration.fetch_add(1, std::memory_order_release);
	}

	void RequestCelestialTransition(std::atomic_bool& a_request)
	{
		a_request.store(true, std::memory_order_release);
		if (!celestialTransitionHandlerAvailable.load(std::memory_order_acquire) &&
			a_request.exchange(false, std::memory_order_acq_rel)) {
			MarkCelestialTransitionComplete();
		}
	}
}

namespace Util
{
	void SetCelestialTransitionHandlerAvailable(bool a_available)
	{
		celestialTransitionHandlerAvailable.store(a_available, std::memory_order_release);
		if (a_available)
			return;

		// Bitwise or, not ||: both requests must be drained even when the first one was already pending.
		const bool hadPendingTransition = timeJumpTransitionRequested.exchange(false, std::memory_order_acq_rel) |
		                                  gameLoadTransitionRequested.exchange(false, std::memory_order_acq_rel);
		if (hadPendingTransition)
			MarkCelestialTransitionComplete();
	}

	void RequestTimeJumpTransition()
	{
		RequestCelestialTransition(timeJumpTransitionRequested);
	}

	void RequestGameLoadTransition()
	{
		RequestCelestialTransition(gameLoadTransitionRequested);
	}

	CelestialTransitionRequest ConsumeCelestialTransitionRequest()
	{
		if (!celestialTransitionHandlerAvailable.load(std::memory_order_acquire))
			return {};

		return {
			.timeJump = timeJumpTransitionRequested.exchange(false, std::memory_order_acq_rel),
			.gameLoad = gameLoadTransitionRequested.exchange(false, std::memory_order_acq_rel),
		};
	}

	void CompleteCelestialTransition()
	{
		MarkCelestialTransitionComplete();
	}

	std::uint32_t GetCompletedCelestialTransitionGeneration()
	{
		return completedCelestialTransitionGeneration.load(std::memory_order_acquire);
	}

	void StoreTransform3x4NoScale(DirectX::XMFLOAT3X4& Dest, const RE::NiTransform& Source)
	{
		//
		// Shove a Matrix3+Point3 directly into a float[3][4] with no modifications
		//
		// Dest[0][#] = Source.m_Rotate.m_pEntry[0][#];
		// Dest[0][3] = Source.m_Translate.x;
		// Dest[1][#] = Source.m_Rotate.m_pEntry[1][#];
		// Dest[1][3] = Source.m_Translate.x;
		// Dest[2][#] = Source.m_Rotate.m_pEntry[2][#];
		// Dest[2][3] = Source.m_Translate.x;
		//
		static_assert(sizeof(RE::NiTransform::rotate) == 3 * 3 * sizeof(float));  // NiMatrix3
		static_assert(sizeof(RE::NiTransform::translate) == 3 * sizeof(float));   // NiPoint3
		static_assert(offsetof(RE::NiTransform, translate) > offsetof(RE::NiTransform, rotate));

		_mm_store_ps(Dest.m[0], _mm_loadu_ps(Source.rotate.entry[0]));
		_mm_store_ps(Dest.m[1], _mm_loadu_ps(Source.rotate.entry[1]));
		_mm_store_ps(Dest.m[2], _mm_loadu_ps(Source.rotate.entry[2]));
		Dest.m[0][3] = Source.translate.x;
		Dest.m[1][3] = Source.translate.y;
		Dest.m[2][3] = Source.translate.z;
	}

	float4 TryGetWaterData(float offsetX, float offsetY)
	{
		if (globals::game::shadowState) {
			if (auto tes = RE::TES::GetSingleton()) {
				auto position = GetEyePosition(0);
				position.x += offsetX;
				position.y += offsetY;
				if (auto cell = tes->GetCell(position)) {
					float4 data = float4(1.0f, 1.0f, 1.0f, -FLT_MAX);

					bool extraCellWater = false;

					if (auto extraCellWaterType = cell->extraList.GetByType<RE::ExtraCellWaterType>()) {
						if (auto water = extraCellWaterType->water) {
							{
								data = { float(water->data.deepWaterColor.red) + float(water->data.shallowWaterColor.red),
									float(water->data.deepWaterColor.green) + float(water->data.shallowWaterColor.green),
									float(water->data.deepWaterColor.blue) + float(water->data.shallowWaterColor.blue) };

								data.x /= 255.0f;
								data.y /= 255.0f;
								data.z /= 255.0f;

								data.x *= 0.5;
								data.y *= 0.5;
								data.z *= 0.5;
								extraCellWater = true;
							}
						}
					}

					if (!extraCellWater) {
						if (auto worldSpace = tes->GetRuntimeData2().worldSpace) {
							if (auto water = worldSpace->worldWater) {
								data = { float(water->data.deepWaterColor.red) + float(water->data.shallowWaterColor.red),
									float(water->data.deepWaterColor.green) + float(water->data.shallowWaterColor.green),
									float(water->data.deepWaterColor.blue) + float(water->data.shallowWaterColor.blue) };

								data.x /= 255.0f;
								data.y /= 255.0f;
								data.z /= 255.0f;

								data.x *= 0.5;
								data.y *= 0.5;
								data.z *= 0.5;
							}
						}
					}

					if (auto sky = globals::game::sky) {
						const auto& color = sky->skyColor[RE::TESWeather::ColorTypes::kWaterMultiplier];
						data.x *= color.red;
						data.y *= color.green;
						data.z *= color.blue;
					}

					data.w = cell->GetExteriorWaterHeight() - position.z;

					return data;
				}
			}
		}
		return float4(1.0f, 1.0f, 1.0f, -FLT_MAX);
	}

	RE::NiPoint3 GetAverageEyePosition()
	{
		auto shadowState = globals::game::shadowState;
		if (!globals::game::isVR)
			return shadowState->GetRuntimeData().posAdjust.getEye();
		return (shadowState->GetVRRuntimeData().posAdjust.getEye(0) + shadowState->GetVRRuntimeData().posAdjust.getEye(1)) * 0.5f;
	}

	RE::NiPoint3 GetEyePosition(int eyeIndex)
	{
		auto shadowState = globals::game::shadowState;
		if (!globals::game::isVR)
			return shadowState->GetRuntimeData().posAdjust.getEye();
		return shadowState->GetVRRuntimeData().posAdjust.getEye(eyeIndex);
	}

	RE::BSGraphics::ViewData GetCameraData(int eyeIndex)
	{
		auto shadowState = globals::game::shadowState;
		if (!globals::game::isVR) {
			return shadowState->GetRuntimeData().cameraData.getEye();
		}
		return shadowState->GetVRRuntimeData().cameraData.getEye(eyeIndex);
	}

	float4 GetCameraData()
	{
		const float cameraNear = *globals::game::cameraNear;
		const float cameraFar = *globals::game::cameraFar;

		float4 cameraData{};
		cameraData.x = cameraFar;
		cameraData.y = cameraNear;
		cameraData.z = cameraFar - cameraNear;
		cameraData.w = cameraFar * cameraNear;

		return cameraData;
	}

	bool GetTemporal()
	{
		auto* imageSpaceManager = globals::game::imageSpaceManager;
		if (!imageSpaceManager)
			return false;
		// VR keeps its own ISTemporalAA instance at a different offset.
		GET_INSTANCE_MEMBER_VRPTR(BSImagespaceShaderISTemporalAA, imageSpaceManager);
		return BSImagespaceShaderISTemporalAA && BSImagespaceShaderISTemporalAA->taaEnabled;
	}

	void SetTemporal(bool enabled)
	{
		auto* imageSpaceManager = globals::game::imageSpaceManager;
		if (!imageSpaceManager)
			return;
		GET_INSTANCE_MEMBER_VRPTR(BSImagespaceShaderISTemporalAA, imageSpaceManager);
		if (BSImagespaceShaderISTemporalAA)
			BSImagespaceShaderISTemporalAA->taaEnabled = enabled;
	}

	void DisableVanillaTAA()
	{
		if (auto* setting = RE::GetINISetting("bUseTAA:Display"))
			setting->data.b = false;
	}

	float GetVerticalFOVRad()
	{
		static float& cameraFOVDeg = (*(float*)(REL::RelocationID(513786, 388785).address()));  // FOV degrees
		float hFOVRad = cameraFOVDeg * (3.14159265359f / 180.0f);
		float unitHalfWidth = tan(hFOVRad / 2);                                                                // This is same as camera frustum RL
		float unitHalfHeight = unitHalfWidth / (globals::state->screenSize.x / globals::state->screenSize.y);  // frustum TB
		float vFOVRad = 2.0f * atan(unitHalfHeight);
		return vFOVRad;
	}

	float2 ConvertToDynamic(float2 a_size, bool a_ignoreLock)
	{
		auto viewport = globals::game::graphicsState;
		auto& runtimeData = viewport->GetRuntimeData();

		if (runtimeData.dynamicResolutionLock && !a_ignoreLock)
			return a_size;

		return float2(
			a_size.x * runtimeData.dynamicResolutionWidthRatio,
			a_size.y * runtimeData.dynamicResolutionHeightRatio);
	}

	DispatchCount GetScreenDispatchCount(bool a_dynamic)
	{
		float2 resolution = globals::state->screenSize;

		// Feature passes use the scaled render area even when the vanilla dynamic-resolution lock is set.
		if (a_dynamic)
			resolution = ConvertToDynamic(resolution, true);

		uint dispatchX = (uint)std::ceil(resolution.x / 8.0f);
		uint dispatchY = (uint)std::ceil(resolution.y / 8.0f);

		return { dispatchX, dispatchY };
	}

	bool IsDynamicResolution()
	{
		const static auto address = REL::RelocationID{ 508794, 380760 }.address();
		bool* bDynamicResolution = reinterpret_cast<bool*>(address);
		return *bDynamicResolution;
	}

	std::string FormatTESForm(const RE::TESForm* form)
	{
		if (!form) {
			return "nullptr";
		}

		// Get name and editor ID
		const char* rawName = form->GetName();
		const char* rawEditorID = form->GetFormEditorID();

		std::string name;
		std::string editorID = rawEditorID ? rawEditorID : "Unknown";

		// Check if name exists and is not just whitespace
		if (rawName && strlen(rawName) > 0) {
			std::string tempName(rawName);
			// Check if name is only whitespace
			bool isOnlyWhitespace = std::all_of(tempName.begin(), tempName.end(),
				[](unsigned char c) { return std::isspace(c); });

			if (!isOnlyWhitespace) {
				name = tempName;
			}
		}
		// Format the FormID part once
		std::string formIDStr = " - 0x" + std::format("{:08X}", form->GetFormID());

		// If no valid name, use editor ID as name and don't show it twice
		if (name.empty()) {
			return editorID + formIDStr;
		} else {
			return name + " " + editorID + formIDStr;
		}
	}
	std::string FormatWeather(const RE::TESWeather* weather)
	{
		if (!weather) {
			return "nullptr";
		}

		std::string baseFormat = FormatTESForm(weather);

		// Get all flag names for this weather using magic_enum
		std::vector<std::string> flagNames;
		uint32_t flags = weather->data.flags.underlying();

		if (flags == 0) {
			flagNames.push_back("None");
		} else {
			// Use magic_enum to iterate through all weather flags
			for (auto flagValue : magic_enum::enum_values<RE::TESWeather::WeatherDataFlag>()) {
				if (flagValue != RE::TESWeather::WeatherDataFlag::kNone &&
					weather->data.flags.any(flagValue)) {
					// Convert enum name to human-readable format
					std::string flagName = std::string(magic_enum::enum_name(flagValue));

					// Remove 'k' prefix and convert to readable format
					if (flagName.starts_with("k")) {
						flagName = flagName.substr(1);
					}

					// Convert specific cases to more readable names
					if (flagName == "PermAurora") {
						flagName = "Aurora";
					} else if (flagName == "AuroraFollowsSun") {
						flagName = "Aurora Sun";
					}

					flagNames.push_back(flagName);
				}
			}

			// Check for any unknown flags (flags not covered by the enum)
			uint32_t knownFlags = 0;
			for (auto flagValue : magic_enum::enum_values<RE::TESWeather::WeatherDataFlag>()) {
				if (flagValue != RE::TESWeather::WeatherDataFlag::kNone) {
					knownFlags |= static_cast<uint32_t>(flagValue);
				}
			}

			uint32_t unknownFlags = flags & ~knownFlags;
			if (unknownFlags != 0) {
				flagNames.push_back("Unknown(" + std::to_string(unknownFlags) + ")");
			}
		}

		// Join flag names with commas
		std::string flagsStr;
		for (size_t i = 0; i < flagNames.size(); ++i) {
			if (i > 0) {
				flagsStr += ", ";
			}
			flagsStr += flagNames[i];
		}

		return baseFormat + " [" + flagsStr + "]";
	}

	bool FrameChecker::IsNewFrame()
	{
		return IsNewFrame(globals::state->frameCount);
	}

	RE::BGSTextureSet* GetSeasonalSwap(RE::BGSTextureSet* textureSet)
	{
		if (textureSet == nullptr) {
			return nullptr;
		}

		if (textureSet->pad12C > 0) {
			if (auto* form = RE::TESForm::LookupByID<RE::BGSTextureSet>(textureSet->pad12C)) {
				return form;
			}
		}

		return textureSet;
	}

	bool IsInterior()
	{
		auto tes = RE::TES::GetSingleton();
		if (tes && !tes->interiorCell) {
			if (auto worldSpace = tes->GetRuntimeData2().worldSpace) {
				if (!worldSpace->flags.any(RE::TESWorldSpace::Flag::kNoSky, RE::TESWorldSpace::Flag::kFixedDimensions)) {
					return false;
				}
			}
		}
		return true;
	}

	void WorldToCell(const RE::NiPoint2& worldPos, int32_t& x, int32_t& y)
	{
		x = static_cast<int32_t>(floor(worldPos.x / 4096.0f));
		y = static_cast<int32_t>(floor(worldPos.y / 4096.0f));
	}

	void WorldToCell(const RE::NiPoint3& worldPos, int32_t& x, int32_t& y)
	{
		WorldToCell(RE::NiPoint2(worldPos.x, worldPos.y), x, y);
	}
}  // namespace Util

namespace Util::EnvironmentControls
{
	namespace
	{
		std::recursive_mutex environmentMutex;
		std::atomic<RE::TESWeather*> lockedWeather{ nullptr };
		std::atomic_bool weatherLockAvailable{ false };
		bool timePaused = false;
		float savedTimeScale = kDefaultTimeScale;
		bool timeRunningForMenu = false;
		bool restorePauseAfterMenu = false;
		constexpr float kHourLockTolerance = 0.001f;
		constexpr float kWeatherTransitionMidpoint = 0.5f;
		std::optional<RE::TESWeather*> scrubPreviousWeather;

		struct Preview
		{
			RE::TESWeather* weather = nullptr;
			std::optional<float> hour;
			RE::TESWeather* previousWeather = nullptr;
			bool previousTimePaused = false;
			float previousTimeScale = kDefaultTimeScale;
			float previousSavedTimeScale = kDefaultTimeScale;
		};
		std::optional<Preview> preview;

		RE::TESWeather* GetWeatherForTimeChange()
		{
			if (auto* weather = GetLockedWeather())
				return weather;
			auto* sky = globals::game::sky;
			return sky ? sky->currentWeather : nullptr;
		}

		void ApplyWeatherLock(RE::TESWeather* weather)
		{
			const auto* previous = lockedWeather.exchange(weather, std::memory_order_acq_rel);
			if (!weather && previous)
				if (auto* sky = globals::game::sky)
					sky->ReleaseWeatherOverride();
		}

		void PauseTimeInternal()
		{
			if (timePaused)
				return;
			if (auto* calendar = globals::game::calendar; calendar && calendar->timeScale) {
				savedTimeScale = calendar->timeScale->value;
				calendar->timeScale->value = 0.0f;
				timePaused = true;
			}
		}

		void ResumeTimeInternal()
		{
			if (!timePaused)
				return;
			if (auto* calendar = globals::game::calendar; calendar && calendar->timeScale) {
				calendar->timeScale->value = savedTimeScale;
				timePaused = false;
			}
		}

		void RestorePreviewTime(const Preview& previous)
		{
			savedTimeScale = previous.previousSavedTimeScale;
			timePaused = previous.previousTimePaused && !timeRunningForMenu;
			if (timeRunningForMenu)
				restorePauseAfterMenu = previous.previousTimePaused;
			if (auto* calendar = globals::game::calendar; calendar && calendar->timeScale) {
				calendar->timeScale->value = timeRunningForMenu && previous.previousTimeScale <= 0.0f ?
				                                 (savedTimeScale > 0.0f ? savedTimeScale : kDefaultTimeScale) :
				                                 previous.previousTimeScale;
			}
		}
	}

	void SetWeatherLockAvailable()
	{
		weatherLockAvailable.store(true, std::memory_order_release);
	}

	bool IsWeatherLockAvailable()
	{
		return weatherLockAvailable.load(std::memory_order_acquire);
	}

	RE::TESWeather* GetLockedWeather()
	{
		return lockedWeather.load(std::memory_order_acquire);
	}

	void MaintainLocks()
	{
		std::scoped_lock lock(environmentMutex);
		if (auto* weather = GetLockedWeather()) {
			if (auto* sky = globals::game::sky) {
				const bool releasePending = sky->flags.any(RE::Sky::Flags::kReleaseWeatherOverride);
				if (releasePending || sky->currentWeather != weather || sky->overrideWeather != weather) {
					sky->flags.reset(RE::Sky::Flags::kReleaseWeatherOverride);
					sky->ForceWeather(weather, true);
				}
			}
		}
		if (!preview || !preview->hour || timeRunningForMenu)
			return;
		if (auto* calendar = globals::game::calendar; calendar && calendar->gameHour && calendar->timeScale) {
			calendar->timeScale->value = 0.0f;
			timePaused = true;
			if (std::abs(calendar->gameHour->value - *preview->hour) > kHourLockTolerance) {
				calendar->gameHour->value = *preview->hour;
				RequestTimeJumpTransition();
			}
		}
	}

	void SetLockedWeather(RE::TESWeather* weather)
	{
		std::scoped_lock lock(environmentMutex);
		EndGameHourScrub();
		StopPreview();
		ApplyWeatherLock(weather);
		MaintainLocks();
	}

	void ChangeWeather(RE::TESWeather* weather, bool instant)
	{
		std::scoped_lock lock(environmentMutex);
		auto* sky = globals::game::sky;
		if (!sky || !weather)
			return;
		EndGameHourScrub();
		const bool wasLocked = GetLockedWeather() != nullptr;
		StopPreview();
		if (wasLocked) {
			ApplyWeatherLock(weather);
			MaintainLocks();
		} else if (instant) {
			sky->ForceWeather(weather, false);
		} else {
			sky->SetWeather(weather, true, false);
		}
	}

	void ResetWeather()
	{
		std::scoped_lock lock(environmentMutex);
		SetLockedWeather(nullptr);
		if (auto* sky = globals::game::sky)
			sky->ResetWeather();
	}

	void RefreshWeather(RE::TESWeather* weather)
	{
		std::scoped_lock lock(environmentMutex);
		if (auto* sky = globals::game::sky; sky && weather && sky->currentWeather == weather) {
			sky->ForceWeather(weather, true);
			if (!GetLockedWeather())
				sky->ReleaseWeatherOverride();
			else
				MaintainLocks();
		}
	}

	void BeginGameHourScrub()
	{
		std::scoped_lock lock(environmentMutex);
		auto* calendar = globals::game::calendar;
		if (scrubPreviousWeather || timeRunningForMenu || !calendar || !calendar->gameHour)
			return;
		auto* weather = GetWeatherForTimeChange();
		if (auto* sky = globals::game::sky; sky && !GetLockedWeather() && sky->lastWeather &&
											(!weather || sky->currentWeatherPct <= kWeatherTransitionMidpoint))
			weather = sky->lastWeather;
		if (!weather || !globals::game::sky)
			return;
		scrubPreviousWeather = preview && preview->weather ? preview->previousWeather : GetLockedWeather();
		// Keep the preview weather through takeover, restoring its original lock when the slider releases.
		if (preview)
			preview->previousWeather = weather;
		StopPreview();
		ApplyWeatherLock(weather);
		MaintainLocks();
	}

	void EndGameHourScrub()
	{
		std::scoped_lock lock(environmentMutex);
		if (!scrubPreviousWeather)
			return;
		auto* previousWeather = *scrubPreviousWeather;
		scrubPreviousWeather.reset();
		ApplyWeatherLock(previousWeather);
		MaintainLocks();
	}

	bool SetGameHour(float hour, bool synchronize)
	{
		std::scoped_lock lock(environmentMutex);
		auto* calendar = globals::game::calendar;
		if (!calendar || !calendar->gameHour || !std::isfinite(hour) || hour < 0.0f || hour >= kHoursPerDay)
			return false;
		StopPreview();
		calendar->gameHour->value = hour;
		if (synchronize)
			RequestTimeJumpTransition();
		return true;
	}

	void SetTimeScale(float timeScale)
	{
		std::scoped_lock lock(environmentMutex);
		auto* calendar = globals::game::calendar;
		if (!calendar || !calendar->timeScale || !std::isfinite(timeScale) || timeScale < 0.0f)
			return;
		StopPreview();
		if (IsTimePaused())
			savedTimeScale = timeScale;
		else
			calendar->timeScale->value = timeScale;
	}

	bool IsTimePaused()
	{
		std::scoped_lock lock(environmentMutex);
		if (auto* calendar = globals::game::calendar; calendar && calendar->timeScale && calendar->timeScale->value > 0.0f)
			timePaused = false;
		return timePaused;
	}

	float GetSavedTimeScale()
	{
		std::scoped_lock lock(environmentMutex);
		return savedTimeScale;
	}

	void PauseTime()
	{
		std::scoped_lock lock(environmentMutex);
		StopPreview();
		if (timeRunningForMenu)
			restorePauseAfterMenu = true;
		else
			PauseTimeInternal();
	}

	void ResumeTime()
	{
		std::scoped_lock lock(environmentMutex);
		StopPreview();
		restorePauseAfterMenu = false;
		ResumeTimeInternal();
	}

	void ResetTimeScale()
	{
		std::scoped_lock lock(environmentMutex);
		SetTimeScale(kDefaultTimeScale);
	}

	void SetTimeRunningForMenu(bool needsRunningTime)
	{
		std::scoped_lock lock(environmentMutex);
		auto* calendar = globals::game::calendar;
		if (!calendar || !calendar->timeScale || timeRunningForMenu == needsRunningTime)
			return;
		timeRunningForMenu = needsRunningTime;
		if (needsRunningTime) {
			EndGameHourScrub();
			restorePauseAfterMenu = IsTimePaused();
			if (calendar->timeScale->value == 0.0f) {
				if (savedTimeScale <= 0.0f)
					savedTimeScale = kDefaultTimeScale;
				ResumeTimeInternal();
				if (calendar->timeScale->value == 0.0f)
					calendar->timeScale->value = savedTimeScale;
			}
		} else {
			if (restorePauseAfterMenu)
				PauseTimeInternal();
			restorePauseAfterMenu = false;
			MaintainLocks();
		}
	}

	bool StartPreview(RE::TESWeather* weather, std::optional<float> hour)
	{
		std::scoped_lock lock(environmentMutex);
		auto* calendar = globals::game::calendar;
		if (!weather && hour)
			weather = GetWeatherForTimeChange();
		if ((!weather && !hour) || timeRunningForMenu ||
			(weather && !globals::game::sky) ||
			(hour && (!std::isfinite(*hour) || *hour < 0.0f || *hour >= kHoursPerDay ||
						 !calendar || !calendar->gameHour || !calendar->timeScale)))
			return false;
		EndGameHourScrub();
		if (!preview)
			preview.emplace();
		if (weather && !preview->weather)
			preview->previousWeather = GetLockedWeather();
		if (hour && !preview->hour) {
			preview->previousTimePaused = IsTimePaused();
			preview->previousTimeScale = calendar->timeScale->value;
			preview->previousSavedTimeScale = savedTimeScale;
		} else if (!hour && preview->hour) {
			RestorePreviewTime(*preview);
		}
		preview->weather = weather;
		preview->hour = hour;
		if (weather)
			ApplyWeatherLock(weather);
		if (hour)
			PauseTimeInternal();
		MaintainLocks();
		return true;
	}

	void StopPreview()
	{
		std::scoped_lock lock(environmentMutex);
		if (!preview)
			return;
		const auto previous = *preview;
		preview.reset();
		if (previous.weather)
			ApplyWeatherLock(previous.previousWeather);
		if (previous.hour)
			RestorePreviewTime(previous);
		MaintainLocks();
	}

	bool IsPreviewActive()
	{
		std::scoped_lock lock(environmentMutex);
		return preview.has_value();
	}
}
