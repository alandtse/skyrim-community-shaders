#pragma once

#include <atomic>
#include <cstdint>
#include <d3d11.h>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <winrt/base.h>

#include "Utils/GpuTimestamps.h"
#include "Utils/Macros.h"

/**
 * @brief GPU and CPU profiler using D3D11 timestamp queries.
 *
 * Maintains a ring buffer of frames with paired begin/end timestamp queries
 * and rolling statistics (average, p95, p99) per named pass. Capture is
 * request-driven: no queries or QPC reads are issued unless a capture is
 * active for the current frame.
 */
class Profiler
{
public:
	static constexpr uint32_t kMaxTimers = 256;
	static constexpr uint32_t kFrameLatency = 3;
	static constexpr uint32_t kHistorySize = 300;
	/** @brief Collection-cycle gap before retiring an inactive timer.
		Must exceed the longest legitimate gap between samples of a still-running pass;
		the DynamicCubemaps state machine spreads its passes over 6 frames. */
	static constexpr uint64_t kTimerRetireFrames = 60;

	using PerfEventCallback = std::function<void(std::string_view)>;

	/** @brief Circular buffer tracking per-timer timing samples with statistics. */
	struct RollingHistory
	{
		float history[kHistorySize]{};
		uint32_t head = 0;
		uint32_t count = 0;
		float lastMs = 0.0f;

		/** @brief Appends a timing sample, overwriting the oldest if full. */
		void PushSample(float ms)
		{
			history[head] = ms;
			head = (head + 1) % kHistorySize;
			if (count < kHistorySize)
				count++;
			lastMs = ms;
		}

		/** @brief Gets the arithmetic mean of all buffered samples. */
		float GetAverage() const;

		/**
		 * @brief Gets an interpolated percentile from the buffered samples.
		 * @param p Percentile in [0, 100].
		 */
		float GetPercentile(float p) const;
	};

	/** @brief Snapshot of GPU/CPU timing data for a single named pass. */
	struct TimerResult
	{
		std::string name;
		/// GPU self time, excluding all directly nested profiled passes.
		float gpuTimeMs = 0.0f;
		/// Inclusive duration contributed by depth-0 intervals this cycle.
		/// Zero when not active this cycle; sums exactly to the frame total.
		float topLevelMs = 0.0f;
		float avgMs = 0.0f;
		float p95Ms = 0.0f;
		float p99Ms = 0.0f;
		float cpuTimeMs = 0.0f;
		float cpuAvgMs = 0.0f;
		float cpuP95Ms = 0.0f;
		float cpuP99Ms = 0.0f;
		/// Whether this pass has ever recorded a GPU/CPU sample, vs. one
		/// mode never having applied to it (a CPU-only scope has no GPU side).
		bool hasGpu = false;
		bool hasCpu = false;
		/// Whether the LAST collected frame specifically had a fresh sample
		/// for this side (vs. the stats reflecting an older, stale sample).
		bool activeGpu = false;
		bool activeCpu = false;
		bool valid = false;

		const float* historyBuffer = nullptr;
		uint32_t historyHead = 0;
		uint32_t historyCount = 0;
		const float* cpuHistoryBuffer = nullptr;
		uint32_t cpuHistoryHead = 0;
		uint32_t cpuHistoryCount = 0;

		/**
		 * @brief Gets a GPU history sample by age-ordered index (0 = oldest).
		 * @param index Zero-based index into the history ring buffer.
		 */
		float GetHistorySample(uint32_t index) const
		{
			if (!historyBuffer || index >= historyCount)
				return 0.0f;
			return historyBuffer[(historyHead - historyCount + index + kHistorySize) % kHistorySize];
		}

		/**
		 * @brief Gets a CPU history sample by age-ordered index (0 = oldest).
		 * @param index Zero-based index into the history ring buffer.
		 */
		float GetCpuHistorySample(uint32_t index) const
		{
			if (!cpuHistoryBuffer || index >= cpuHistoryCount)
				return 0.0f;
			return cpuHistoryBuffer[(cpuHistoryHead - cpuHistoryCount + index + kHistorySize) % kHistorySize];
		}
	};

	/**
	 * @brief Creates timestamp query objects and prepares the frame ring buffer.
	 * @param device D3D11 device used to create query objects.
	 * @param context Device context used for issuing and collecting queries.
	 */
	void Initialize(ID3D11Device* device, ID3D11DeviceContext* context);

	/** @brief Releases all D3D11 query objects and resets state. */
	void Release();

	/** @brief Enables or disables runtime profiling; disabling cancels any pending capture. */
	void SetUserEnabled(bool a_enabled);

	/** @brief Gets whether the user has runtime profiling enabled. */
	bool IsUserEnabled() const { return userEnabled.load(std::memory_order_acquire); }

	/** @brief Requests a timing capture for the next frame; consumers must re-request every frame. */
	void RequestCapture();

	/** @brief True while a capture is active; gates all query issuance and CPU timing. */
	bool IsEnabled() const { return IsUserEnabled() && captureActive.load(std::memory_order_acquire); }

	/**
	 * @brief Registers optional callbacks invoked at pass begin/end (e.g. for RenderDoc markers).
	 * @param beginCb Called with the pass name when a pass begins.
	 * @param endCb Called when a pass ends.
	 */
	void SetPerfEventCallbacks(PerfEventCallback beginCb, PerfEventCallback endCb)
	{
		beginPerfEvent = std::move(beginCb);
		endPerfEvent = std::move(endCb);
	}

	/** @brief Begins a new profiling frame; collects results from the oldest in-flight frame. */
	void BeginFrame();

	/**
	 * @brief Opens a named pass; returns false (no-op EndPass expected) at
	 * capacity or before Initialize(). Passes may nest: each call acquires
	 * its own slot, and EndPass closes the innermost still-open one.
	 */
	bool BeginPass(std::string_view name, bool fireCallbacks = true);
	void EndPass(bool fireCallbacks = true);

	/**
	 * @brief Ends the current profiling frame.
	 * @param a_frameCount The engine's own frame counter for this tick,
	 *        stamped onto the ring slot so a later GetCapturedFrameCount()
	 *        tells a caller exactly which frame the current results describe
	 *        (results lag live by up to kFrameLatency frames).
	 */
	void EndFrame(uint32_t a_frameCount);

	/**
	 * @brief Opens a CPU-only named scope (no GPU query); returns false (no-op
	 * EndCpuPass expected) while disabled. Scopes may nest: each successful
	 * call pushes its own slot, and EndCpuPass closes the innermost open one.
	 */
	bool BeginCpuPass(std::string_view name);
	void EndCpuPass();

	/** @brief Gets the per-pass timing results from the last collected frame. */
	const std::vector<TimerResult>& GetResults() const { return results; }

	/** @brief Gets the total GPU time in milliseconds for the last collected frame. */
	float GetTotalTimeMs() const { return totalTimeMs; }

	/** @brief Gets the total CPU time in milliseconds for the last collected frame. */
	float GetCpuTotalTimeMs() const { return cpuTotalTimeMs; }

	/**
	 * @brief Gets the total GPU time as of the results GetCapturedFrameCount() describes.
	 *
	 * Unlike GetTotalTimeMs(), this is never zeroed while idle, so it stays
	 * resolve-consistent with GetResults()'s timers for exact checks like
	 * totalMs == sum(topLevelMs); GetTotalTimeMs() is the live/UI value that
	 * intentionally zeroes so an overlay row doesn't show a stale sum.
	 */
	float GetResolvedTotalTimeMs() const { return resolvedTotalMs; }

	/** @brief Gets the resolve-consistent CPU total; see GetResolvedTotalTimeMs(). */
	float GetResolvedCpuTotalTimeMs() const { return resolvedCpuTotalMs; }

	/** @brief Gets the engine frame count the current results were captured on. */
	uint32_t GetCapturedFrameCount() const { return capturedFrameCount; }

	/** @brief Gets the GPU timer slots acquired in the most recently completed frame. */
	uint32_t GetAcquiredSlots() const { return acquiredSlots; }

	/** @brief Gets the session high-water mark of GPU timer slots acquired in one frame. */
	uint32_t GetPeakAcquiredSlots() const { return peakAcquiredSlots; }

	/** @brief Gets the cumulative count of BeginPass calls refused for slot capacity since Initialize(). */
	uint32_t GetSlotRefusals() const { return slotRefusals; }

	/** @brief Resets all timer history and results. */
	void ClearTimers()
	{
		results.clear();
		knownTimers.clear();
		knownTimerIndex.clear();
		collectedFrames = 0;
		totalTimeMs = 0.0f;
		cpuTotalTimeMs = 0.0f;
		activeCpuTimers.clear();
		completedCpuTimers.clear();
		// A cleared timer must not resurrect from a still-in-flight ring slot.
		ResetPendingFrames();
	}

	/**
	 * @brief Removes all timers whose names start with the given feature prefix.
	 * @param featureName Feature name; timers matching "featureName::*" are removed.
	 */
	void ClearTimersForFeature(const std::string& featureName)
	{
		std::string prefix = featureName + "::";
		std::erase_if(knownTimers, [&prefix](const KnownTimer& kt) {
			return kt.name.starts_with(prefix);
		});
		RebuildTimerIndex();
		std::erase_if(activeCpuTimers, [&prefix](const CpuTimer& ct) {
			return ct.name.starts_with(prefix);
		});
		std::erase_if(completedCpuTimers, [&prefix](const CompletedCpuTimer& ct) {
			return ct.name.starts_with(prefix);
		});
		// The removed feature's name may still be pending in a non-live ring
		// slot; purge those rather than filter by name. The currently-open
		// slot (if any) is left alone, so a pending sample for this feature
		// there can still repopulate knownTimers once it resolves.
		ResetPendingFrames();
	}

private:
	/// A CPU-only scope still open, awaiting EndCpuPass.
	struct CpuTimer
	{
		std::string name;
		LARGE_INTEGER cpuBegin{};
		/// Nesting depth at acquisition, stamped once at Begin rather than
		/// derived from activeCpuTimers.size() at End: ClearTimersForFeature
		/// can erase a middle stack entry, which would desync a derived depth.
		uint32_t depth = 0;
	};

	/// A CPU-only scope that finished this cycle, pending collection.
	struct CompletedCpuTimer
	{
		std::string name;
		float cpuMs = 0.0f;
		/// Nesting depth at acquisition (0 = top-level); gates the frame
		/// CPU total so a parent scope's time isn't double-counted.
		uint32_t depth = 0;
	};

	struct FrameQueries
	{
		Util::TimestampQueryBatch batch;
		struct TimerMeta
		{
			std::string name;
			LARGE_INTEGER cpuBegin{};
			float cpuMs = 0.0f;
			/// Slot of the directly enclosing GPU pass, or -1 at frame depth 0.
			int32_t parentSlot = -1;
			/// Nesting depth at acquisition (0 = top-level); gates the
			/// frame totals so a parent's time isn't double-counted.
			uint32_t depth = 0;
		};
		std::vector<TimerMeta> timers;
		uint32_t acquiredTimerCount = 0;
		/// LIFO stack of acquired-but-not-yet-closed slot indices, so EndPass
		/// closes the innermost open pass regardless of nesting depth/order.
		std::vector<int> activeStack;
		bool inFlight = false;
		/// CPU-only timers completed during this ring slot's cycle, held
		/// until CollectResults drains them (mirrors the GPU query payload).
		std::vector<CompletedCpuTimer> cpuTimers;
		/// Engine frame count this slot's queries were stamped on; surfaced
		/// via GetCapturedFrameCount() once this slot resolves.
		uint32_t capturedFrame = 0;
	};

	ID3D11Device* device = nullptr;
	ID3D11DeviceContext* context = nullptr;

	FrameQueries frames[kFrameLatency];
	uint32_t writeFrame = 0;
	uint32_t readFrame = 0;
	uint32_t framesSinceInit = 0;
	bool initialized = false;
	bool frameActive = false;
	// Enabled by default so profiling pages show data without an extra toggle;
	// zero-overhead idling still holds because captureActive stays false until requested.
	std::atomic_bool userEnabled{ true };
	std::atomic_bool captureRequested{ false };
	std::atomic_bool captureActive{ false };
	double cpuTicksToMs = 0.0;

	PerfEventCallback beginPerfEvent;
	PerfEventCallback endPerfEvent;

	std::vector<TimerResult> results;

	struct KnownTimer
	{
		std::string name;
		RollingHistory gpu;
		RollingHistory cpu;
		/// Whether this pass has ever recorded a sample of that kind; a
		/// CPU-only scope never has a GPU sample and vice versa.
		bool hasGpu = false;
		bool hasCpu = false;
		uint64_t lastSampleFrame = 0;
	};
	std::vector<KnownTimer> knownTimers;
	std::unordered_map<std::string, size_t> knownTimerIndex;
	uint64_t collectedFrames = 0;
	float totalTimeMs = 0.0f;
	float cpuTotalTimeMs = 0.0f;
	/// Set only in CollectResults; see GetResolvedTotalTimeMs().
	float resolvedTotalMs = 0.0f;
	float resolvedCpuTotalMs = 0.0f;
	uint32_t capturedFrameCount = 0;
	/// GPU slots acquired so far in the ring slot currently being written;
	/// copied to acquiredSlots once that frame ends (a gauge, not a resolve-
	/// latency-bound value -- acquisition count is known immediately).
	uint32_t acquiredSlotsThisFrame = 0;
	uint32_t acquiredSlots = 0;
	/// Session high-water mark of acquiredSlots, since a sparse poll would
	/// otherwise likely miss the peak of a heavy-scene spike entirely.
	uint32_t peakAcquiredSlots = 0;
	/// Cumulative since Initialize(): a capacity refusal is exceptional, so
	/// this is a lifetime counter, not a per-frame gauge like acquiredSlots.
	uint32_t slotRefusals = 0;

	/// CPU-only scopes currently open (LIFO), so EndCpuPass closes the
	/// innermost one regardless of nesting.
	std::vector<CpuTimer> activeCpuTimers;
	/// CPU-only scopes completed this cycle, not yet stored into a frame slot.
	std::vector<CompletedCpuTimer> completedCpuTimers;

	/// Drains the oldest in-flight frame's results if resolved; returns false
	/// only when GPU data is still pending (retry next frame), never when
	/// there's simply nothing to collect.
	bool CollectResults();

	/** @brief Drops timers that have not been sampled for kTimerRetireFrames, so disabled passes stop reporting stale values. */
	void RetireStaleTimers();

	/** @brief Repoints knownTimerIndex at the current knownTimers positions after an erase. */
	void RebuildTimerIndex();

	KnownTimer& GetOrCreateTimer(const std::string& name);

	/// Clears a ring slot's transient GPU/CPU state so a later ClearTimers
	/// call can't have it resurrect a stale sample once it resolves.
	static void ResetFrameState(FrameQueries& frame)
	{
		frame.batch.Reset();
		frame.activeStack.clear();
		frame.acquiredTimerCount = 0;
		frame.inFlight = false;
		frame.cpuTimers.clear();
	}

	/// Resets every ring slot except one with an open scope: frames[writeFrame]
	/// while frameActive, whose batch/activeStack a still-open BeginPass/
	/// BeginCpuPass call is relying on.
	void ResetPendingFrames()
	{
		for (uint32_t i = 0; i < kFrameLatency; i++) {
			if (frameActive && i == writeFrame)
				continue;
			ResetFrameState(frames[i]);
		}
	}
};

/**
 * @brief RAII CPU-only profiling scope; pairs with CS_PROFILE_CPU_SCOPE.
 */
class ScopedCpuPass
{
public:
	ScopedCpuPass(Profiler* a_profiler, std::string_view a_name)
	{
		// Only remember the profiler if BeginCpuPass actually opened a slot;
		// a no-op Begin paired with an unconditional End would pop and
		// mistime whatever scope is innermost when this one is disabled.
		if (a_profiler && a_profiler->BeginCpuPass(a_name))
			profiler = a_profiler;
	}
	~ScopedCpuPass()
	{
		if (profiler)
			profiler->EndCpuPass();
	}

	ScopedCpuPass(const ScopedCpuPass&) = delete;
	ScopedCpuPass& operator=(const ScopedCpuPass&) = delete;

private:
	Profiler* profiler = nullptr;
};

/// Times a CPU-only scope (no GPU query); see ScopedCpuPass.
#define CS_PROFILE_CPU_SCOPE(profiler, name) \
	ScopedCpuPass CS_DETAIL_CONCAT(cs_profile_cpu_scope_, __LINE__) { profiler, name }
