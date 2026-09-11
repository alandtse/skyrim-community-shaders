#pragma once

#include "Utils/Game.h"

/**
 * @brief Shared eye-tracked gaze service for VR foveation consumers.
 *
 * Resolves OpenVR's eye-tracked foveation center (hardware-gated: only
 * Vive Pro Eye / Pimax-Tobii / Varjo / PSVR2-class HMDs report real data)
 * independently of the game's own OpenVR session, requested from the
 * already-loaded openvr_api.dll -- no new DLL load, no second OpenVR
 * client, no risk to the game's own VR session.
 *
 * Not a Feature: gaze has more than one consumer (FoveatedRender today;
 * VRS/dynamic-resolution biasing are plausible future callers), so its
 * lifetime must not be tied to any one feature's enable toggle.
 */
namespace Util::VR
{
	class GazeTracker
	{
	public:
		static constexpr int kBothEyes = -1;

		static GazeTracker& GetSingleton();

		/// Resolve the eye-tracking interface. Safe to call every frame -- retries on a
		/// transient "OpenVR not initialized yet" result (an ordering race, not a
		/// capability negative), then latches once resolved or genuinely unsupported.
		/// Never throws; failure just leaves the tracker unavailable.
		void Init();

		/// Poll the current gaze sample, smooth it, and advance the staleness/fallback
		/// state machine. Call once per frame, on the render thread, before foveation
		/// params are filled for the frame. No-op (and cheap) when unavailable.
		/// forceSyntheticOnly: skip the real hardware query even when available (used by
		/// EyeTrackedFoveationMode::kSynthetic) -- with no override/sweep active yet this
		/// just holds at the caller's fallback, and is never counted as a hardware failure.
		void Update(bool forceSyntheticOnly = false);

		/// Per-eye center offset in the same [-0.5, 0.5]-ish convention FoveatedRender's
		/// centerOffsets already use. Returns `fallback` verbatim whenever gaze isn't
		/// live -- this call can never fail the caller.
		float2 GetCenterOffset(uint32_t eyeIndex, float2 fallback) const;

		/// True once Init() has successfully resolved the interface (does not imply a
		/// headset with an eye tracker is actually connected -- see IsLive()).
		bool IsAvailable() const { return available; }

		/// True when the most recent sample was accepted (not stale/invalid) for either
		/// eye. False under IsAvailable()==true just means no valid sample has arrived
		/// yet/lately.
		bool IsLive() const { return eyeLive[0] || eyeLive[1]; }

		//=============================================================================
		// DevBench test surface (Upscaling::RegisterUxActions) -- synthetic gaze
		// injection for validating the pipeline without eye-tracking hardware.
		//=============================================================================

		/// Force a synthetic sample (through the same state the hardware path writes),
		/// optionally scoped to one eye. ttlMs == 0 means "until cleared".
		void SetSyntheticOverride(float2 uv, int eyeIndex, uint32_t ttlMs);

		enum class SweepPattern
		{
			kCircle,
			kLissajous,
			kSaccade,
		};

		/// Scripted synthetic motion (both eyes) -- reveals mask popping, smoothing lag,
		/// and per-eye desync that a static override can't. amplitude is in UV units
		/// (0.5 == full eye radius).
		void StartSweep(SweepPattern pattern, float hz, float amplitude);

		/// Drop any synthetic override/sweep and resume hardware/fallback.
		void ClearSyntheticOverride();
		bool HasSyntheticOverride() const { return syntheticActive; }

		struct StatusSnapshot
		{
			bool available = false;
			bool live = false;
			bool synthetic = false;
			float2 centerOffset[2] = {};
			uint64_t lastValidAgeMs = 0;
			uint32_t consecutiveFailures = 0;
			const char* apiVersion = "";
		};
		StatusSnapshot GetStatus() const;

	private:
		GazeTracker() = default;

		bool QueryHardwareSample(float2 outCenters[2]);

		bool initGaveUp = false;
		uint32_t initRetryCount = 0;
		bool available = false;  // hardware-confirmed only; synthetic mode never sets this
		bool eyeLive[2] = { false, false };
		uint32_t consecutiveFailures = 0;
		void* fnTable = nullptr;  // VR_IVRSystem_FnTable*, opaque here to keep openvr.h out of this header
		const char* apiVersion = "";

		float2 currentOffset[2] = {};   // smoothed, what GetCenterOffset returns while live
		float2 targetOffset[2] = {};    // latest accepted sample (post-clamp)
		float2 lerpFromOffset[2] = {};  // snapshot at the moment staleness/recovery lerp starts
		uint64_t lastValidTickMs = 0;
		uint64_t staleSinceTickMs = 0;
		uint64_t lastUpdateTickMs = 0;          ///< real elapsed-time base for smoothing/velocity clamp
		Util::FrameChecker updateFrameChecker;  ///< Update() runs at most once per real frame
		bool wasStale = true;

		bool syntheticActive = false;
		int syntheticEye = kBothEyes;
		float2 syntheticUV{ 0.5f, 0.5f };
		uint64_t syntheticExpiryTickMs = 0;  // 0 == no expiry

		bool sweepActive = false;
		SweepPattern sweepPattern = SweepPattern::kCircle;
		float sweepHz = 0.25f;
		float sweepAmplitude = 0.3f;
		uint64_t sweepStartTickMs = 0;
		uint64_t sweepNextSaccadeTickMs = 0;
		float2 sweepSaccadeTarget{ 0.5f, 0.5f };

		/// Recomputes syntheticUV from elapsed time when sweepActive; called from Update().
		void AdvanceSweep(uint64_t nowMs);
	};
}
