#include "Utils/VRGaze.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <windows.h>

namespace
{
	// VR_IVRSystem_FnTable (openvr_capi.h): a plain C function-pointer table, not a
	// hand-declared C++ vtable, so a version/interface mismatch fails cleanly (null +
	// error code) instead of faulting on a misaligned vtable call.
	struct HmdVector2Raw
	{
		float v[2];
	};

	using GetEyeTrackedFoveationCenterFn = bool(__stdcall*)(HmdVector2Raw* pNdcLeft, HmdVector2Raw* pNdcRight);

	// 0-indexed FnTable slot for IVRSystem_026 (counted from upstream openvr_capi.h --
	// the vendored copy here predates this method, pinned at v1.0.15/IVRSystem_019).
	constexpr size_t kGetEyeTrackedFoveationCenterSlot = 35;

	using VR_GetGenericInterfaceFn = void*(__cdecl*)(const char* pchInterfaceVersion, int* peError);

	constexpr const char* kFnTableVersion = "FnTable:IVRSystem_026";

	// vr::VRInitError_Init_NotInitialized (openvr.h EVRInitError) -- the generic "no
	// caller in this process has completed OpenVR init yet" error, distinct from a
	// genuine "this SteamVR doesn't have this interface" negative. Retryable.
	constexpr int kVRInitError_Init_NotInitialized = 109;
	constexpr uint32_t kMaxInitRetries = 300;  // a few seconds' worth of per-frame retries

	constexpr uint32_t kMaxConsecutiveFailures = 30;  // ~1/3 second at 90Hz

	uint64_t NowMs()
	{
		using namespace std::chrono;
		return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
	}

	float2 NdcToOffset(const HmdVector2Raw& ndc)
	{
		// NDC is Y-up [-1, 1]; FoveatedRender's centerOffsets are Y-down UV offsets in
		// roughly [-0.5, 0.5] (see its subrect-derived leftUV/rightUV.y computation), so
		// the Y axis is negated on the way in.
		return float2{ ndc.v[0] * 0.5f, ndc.v[1] * -0.5f };
	}

	bool IsFiniteAndInRange(float2 v)
	{
		return std::isfinite(v.x) && std::isfinite(v.y) &&
		       v.x >= -0.75f && v.x <= 0.75f && v.y >= -0.75f && v.y <= 0.75f;
	}
}

namespace Util::VR
{
	GazeTracker& GazeTracker::GetSingleton()
	{
		static GazeTracker singleton;
		return singleton;
	}

	bool GazeTracker::QueryHardwareSample(float2 outCenters[2])
	{
		// Every hardware call -- Init()'s and Update()'s alike -- routes through here so
		// a fault anywhere permanently disables the tracker instead of crashing.
		bool ok = false;
		__try {
			// fnTable is the FnTable ABI's own array of function pointers (see the
			// namespace-level comment) -- index it directly rather than computing a
			// byte offset by hand.
			auto* const table = static_cast<void* const*>(fnTable);
			auto fn = reinterpret_cast<GetEyeTrackedFoveationCenterFn>(table[kGetEyeTrackedFoveationCenterSlot]);
			HmdVector2Raw left{}, right{};
			ok = fn && fn(&left, &right);
			if (ok) {
				outCenters[0] = NdcToOffset(left);
				outCenters[1] = NdcToOffset(right);
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			ok = false;
			fnTable = nullptr;
		}
		return ok;
	}

	void GazeTracker::Init()
	{
		if (available || initGaveUp)
			return;

		HMODULE openVRModule = GetModuleHandleW(L"openvr_api.dll");
		if (!openVRModule) {
			initGaveUp = true;
			logger::info("[VRGaze] openvr_api.dll not loaded; eye-tracked foveation unavailable.");
			return;
		}

		auto getGenericInterface = reinterpret_cast<VR_GetGenericInterfaceFn>(
			GetProcAddress(openVRModule, "VR_GetGenericInterface"));
		if (!getGenericInterface) {
			initGaveUp = true;
			logger::info("[VRGaze] VR_GetGenericInterface not exported; eye-tracked foveation unavailable.");
			return;
		}

		int error = 0;
		void* table = getGenericInterface(kFnTableVersion, &error);
		if (!table) {
			// NotInitialized means no caller has completed OpenVR init in this process
			// yet -- e.g. this ran before the game's own OpenVR session came up. Retry
			// on a later call rather than giving up on a transient ordering race.
			if (error == kVRInitError_Init_NotInitialized && ++initRetryCount < kMaxInitRetries)
				return;
			initGaveUp = true;
			logger::info("[VRGaze] SteamVR does not expose {} (error {}); eye-tracked foveation unavailable.", kFnTableVersion, error);
			return;
		}

		fnTable = table;
		apiVersion = kFnTableVersion;

		float2 probe[2]{};
		if (!QueryHardwareSample(probe)) {
			// A cleared fnTable means the guarded call faulted -- never retry a
			// faulting table. A still-valid fnTable with a false result just means no
			// eye tracker is reporting yet (cold boot, calibration); retry like the
			// interface-resolution step above before giving up.
			if (fnTable && ++initRetryCount < kMaxInitRetries)
				return;
			fnTable = nullptr;
			initGaveUp = true;
			logger::info("[VRGaze] No eye-tracked foveation data available from SteamVR (no eye tracker, or hardware not ready yet).");
			return;
		}

		available = true;
		currentOffset[0] = targetOffset[0] = probe[0];
		currentOffset[1] = targetOffset[1] = probe[1];
		lastValidTickMs = NowMs();
		eyeLive[0] = eyeLive[1] = true;
		logger::info("[VRGaze] Eye-tracked foveation available via {}.", apiVersion);
	}

	void GazeTracker::Update(bool forceSyntheticOnly)
	{
		if (!available && !syntheticActive)
			return;

		// Called from more than one per-frame site (State::UpdateSharedData has several
		// call sites, plus ScreenSpaceShadows.cpp); without this, the two sites could
		// sample gaze at different sub-frame instants, making the SSS and SSR/upscaling
		// foveation masks disagree within the same frame.
		if (!updateFrameChecker.IsNewFrame())
			return;

		const uint64_t now = NowMs();
		const uint64_t dtMs = lastUpdateTickMs == 0 ? 17 : std::clamp<uint64_t>(now - lastUpdateTickMs, 1, 250);
		lastUpdateTickMs = now;

		if (syntheticActive && syntheticExpiryTickMs != 0 && now >= syntheticExpiryTickMs)
			ClearSyntheticOverride();
		if (sweepActive)
			AdvanceSweep(now);

		float2 sample[2];
		bool sampleOk;
		bool countsAsFailure = false;
		// Which eye(s) this call actually has fresh data for -- an untouched eye's
		// eyeLive must not be forced by this call, so a single-eye override doesn't
		// also mark the other eye's stale/fallback state as live.
		bool eyeTouched[2] = { true, true };
		if (syntheticActive) {
			const float2 syntheticOffset{ syntheticUV.x - 0.5f, syntheticUV.y - 0.5f };
			// Leaves the other eye tracking its last real/hardware-fallback target, so a
			// single-eye override is visibly distinguishable from a both-eyes one.
			eyeTouched[0] = (syntheticEye == kBothEyes || syntheticEye == 0);
			eyeTouched[1] = (syntheticEye == kBothEyes || syntheticEye == 1);
			sample[0] = eyeTouched[0] ? syntheticOffset : targetOffset[0];
			sample[1] = eyeTouched[1] ? syntheticOffset : targetOffset[1];
			sampleOk = true;
		} else if (forceSyntheticOnly || !available) {
			// kSynthetic mode with no override issued yet, or hardware never resolved:
			// intentionally inert (not a hardware failure -- must never count toward the
			// consecutive-failure latch), so the caller's fallback center applies until a
			// devbench override arrives or hardware becomes available.
			sampleOk = false;
			eyeTouched[0] = eyeTouched[1] = false;
			// eyeTouched skips the per-eye update below, so a switch away from a live
			// Auto/hardware session into an empty Synthetic mode would otherwise leave
			// eyeLive stuck true forever on its last hardware value.
			if (forceSyntheticOnly)
				eyeLive[0] = eyeLive[1] = false;
		} else {
			sampleOk = QueryHardwareSample(sample) && IsFiniteAndInRange(sample[0]) && IsFiniteAndInRange(sample[1]);
			countsAsFailure = !sampleOk;
		}

		if (sampleOk) {
			consecutiveFailures = 0;
			lastValidTickMs = now;
			if (wasStale) {
				// Recovery: lerp inbound from wherever we'd drifted to, same as the
				// staleness lerp below, so a reconnect is never a snap either.
				lerpFromOffset[0] = currentOffset[0];
				lerpFromOffset[1] = currentOffset[1];
				staleSinceTickMs = now;
				wasStale = false;
			}
			targetOffset[0] = sample[0];
			targetOffset[1] = sample[1];
		} else if (countsAsFailure) {
			++consecutiveFailures;
			if (consecutiveFailures >= kMaxConsecutiveFailures) {
				available = false;
				initGaveUp = true;
				eyeLive[0] = eyeLive[1] = false;
				logger::warn("[VRGaze] {} consecutive failed eye-tracking queries; disabling for this session.", consecutiveFailures);
				return;
			}
		}

		const uint64_t ageMs = now - lastValidTickMs;
		const bool stale = ageMs > 150;
		if (stale && !wasStale) {
			lerpFromOffset[0] = currentOffset[0];
			lerpFromOffset[1] = currentOffset[1];
			staleSinceTickMs = now;
			wasStale = true;
		}

		constexpr float kLerpMs = 250.0f;
		constexpr float kSmoothingTauMs = 27.0f;           // ~alpha 0.35 at a 90Hz (11ms) tick
		constexpr float kMaxVelocityPerMs = 0.5f / 90.0f;  // UV-offset units per ms, ~half-eye in one 90Hz frame
		const float dtMsF = static_cast<float>(dtMs);
		const float smoothingAlpha = 1.0f - std::exp(-dtMsF / kSmoothingTauMs);

		for (int eye = 0; eye < 2; ++eye) {
			float2 goal;
			if (stale) {
				const float t = std::min(1.0f, static_cast<float>(now - staleSinceTickMs) / kLerpMs);
				goal = float2{
					lerpFromOffset[eye].x + (0.0f - lerpFromOffset[eye].x) * t,
					lerpFromOffset[eye].y + (0.0f - lerpFromOffset[eye].y) * t
				};
			} else if (!wasStale && (now - staleSinceTickMs) < static_cast<uint64_t>(kLerpMs) && staleSinceTickMs != 0) {
				// Recovery lerp window: blend from the pre-recovery drift point toward
				// the live target rather than snapping straight to it.
				const float t = std::min(1.0f, static_cast<float>(now - staleSinceTickMs) / kLerpMs);
				goal = float2{
					lerpFromOffset[eye].x + (targetOffset[eye].x - lerpFromOffset[eye].x) * t,
					lerpFromOffset[eye].y + (targetOffset[eye].y - lerpFromOffset[eye].y) * t
				};
			} else {
				goal = targetOffset[eye];
			}

			float2 smoothed{
				currentOffset[eye].x + (goal.x - currentOffset[eye].x) * smoothingAlpha,
				currentOffset[eye].y + (goal.y - currentOffset[eye].y) * smoothingAlpha
			};

			// Max-velocity clamp: one bad/noisy sample can't whip the mask across the eye.
			float2 delta{ smoothed.x - currentOffset[eye].x, smoothed.y - currentOffset[eye].y };
			const float maxStep = kMaxVelocityPerMs * dtMsF;
			const float dist = std::sqrt(delta.x * delta.x + delta.y * delta.y);
			if (dist > maxStep && dist > 1e-6f) {
				const float scale = maxStep / dist;
				delta.x *= scale;
				delta.y *= scale;
			}
			currentOffset[eye] = float2{ currentOffset[eye].x + delta.x, currentOffset[eye].y + delta.y };
		}

		for (int eye = 0; eye < 2; ++eye) {
			if (eyeTouched[eye])
				eyeLive[eye] = !stale || syntheticActive;
		}
	}

	float2 GazeTracker::GetCenterOffset(uint32_t eyeIndex, float2 fallback) const
	{
		const uint32_t idx = std::min<uint32_t>(eyeIndex, 1);
		if (!eyeLive[idx])
			return fallback;
		return currentOffset[idx];
	}

	void GazeTracker::SetSyntheticOverride(float2 uv, int eyeIndex, uint32_t ttlMs)
	{
		syntheticActive = true;
		syntheticEye = eyeIndex;
		syntheticUV = uv;
		syntheticExpiryTickMs = ttlMs == 0 ? 0 : NowMs() + ttlMs;
		// Deliberately doesn't touch `available` (hardware-confirmed only) -- Update()'s
		// own syntheticActive check already lets this run without hardware ever
		// resolving, so gazeStatus.available stays truthful.
	}

	void GazeTracker::StartSweep(SweepPattern pattern, float hz, float amplitude)
	{
		sweepActive = true;
		sweepPattern = pattern;
		sweepHz = std::clamp(hz, 0.01f, 5.0f);
		sweepAmplitude = std::clamp(amplitude, 0.0f, 0.5f);
		sweepStartTickMs = NowMs();
		sweepNextSaccadeTickMs = sweepStartTickMs;
		syntheticActive = true;
		syntheticEye = kBothEyes;
		syntheticExpiryTickMs = 0;
	}

	void GazeTracker::AdvanceSweep(uint64_t nowMs)
	{
		const float t = static_cast<float>(nowMs - sweepStartTickMs) / 1000.0f;
		switch (sweepPattern) {
		case SweepPattern::kCircle:
			{
				const float angle = 2.0f * 3.14159265f * sweepHz * t;
				syntheticUV = float2{ 0.5f + sweepAmplitude * std::cos(angle), 0.5f + sweepAmplitude * std::sin(angle) };
				break;
			}
		case SweepPattern::kLissajous:
			{
				const float angle = 2.0f * 3.14159265f * sweepHz * t;
				syntheticUV = float2{ 0.5f + sweepAmplitude * std::sin(angle * 3.0f), 0.5f + sweepAmplitude * std::sin(angle * 2.0f + 1.5707963f) };
				break;
			}
		case SweepPattern::kSaccade:
			{
				if (nowMs >= sweepNextSaccadeTickMs) {
					// Deterministic: reproducible across sessions for bug-report comparison.
					const float seed = static_cast<float>(nowMs % 100000) * 12.9898f;
					const float rx = std::fmod(std::abs(std::sin(seed)) * 43758.5453f, 1.0f);
					const float ry = std::fmod(std::abs(std::sin(seed * 1.7f)) * 43758.5453f, 1.0f);
					sweepSaccadeTarget = float2{ 0.5f + (rx - 0.5f) * 2.0f * sweepAmplitude, 0.5f + (ry - 0.5f) * 2.0f * sweepAmplitude };
					sweepNextSaccadeTickMs = nowMs + static_cast<uint64_t>(1000.0f / sweepHz);
				}
				syntheticUV = sweepSaccadeTarget;
				break;
			}
		}
	}

	void GazeTracker::ClearSyntheticOverride()
	{
		syntheticActive = false;
		sweepActive = false;
		syntheticExpiryTickMs = 0;
	}

	GazeTracker::StatusSnapshot GazeTracker::GetStatus() const
	{
		StatusSnapshot s;
		s.available = available;
		s.live = eyeLive[0] || eyeLive[1];
		s.synthetic = syntheticActive;
		s.centerOffset[0] = currentOffset[0];
		s.centerOffset[1] = currentOffset[1];
		s.lastValidAgeMs = NowMs() - lastValidTickMs;
		s.consecutiveFailures = consecutiveFailures;
		s.apiVersion = apiVersion;
		return s;
	}
}
