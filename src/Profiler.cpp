#include "Profiler.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace
{
	constexpr float kMaxSaneProfilerSampleMs = 1000.0f;

	// Guards rolling stats against disjoint-frame spikes and non-finite samples.
	bool IsValidProfilerSample(float ms)
	{
		return std::isfinite(ms) && ms >= 0.0f && ms <= kMaxSaneProfilerSampleMs;
	}
}

float Profiler::RollingHistory::GetAverage() const
{
	if (count == 0)
		return lastMs;
	float sum = 0.0f;
	for (uint32_t i = 0; i < count; i++)
		sum += history[i];
	return sum / static_cast<float>(count);
}

float Profiler::RollingHistory::GetPercentile(float p) const
{
	if (count == 0)
		return lastMs;

	thread_local std::vector<float> sorted;
	sorted.resize(count);
	for (uint32_t i = 0; i < count; i++)
		sorted[i] = history[i];
	std::sort(sorted.begin(), sorted.end());

	float idx = (p / 100.0f) * static_cast<float>(count - 1);
	uint32_t lo = static_cast<uint32_t>(idx);
	uint32_t hi = std::min(lo + 1, count - 1);
	float frac = idx - static_cast<float>(lo);
	return sorted[lo] * (1.0f - frac) + sorted[hi] * frac;
}

void Profiler::Initialize(ID3D11Device* a_device, ID3D11DeviceContext* a_context)
{
	Release();

	device = a_device;
	context = a_context;

	LARGE_INTEGER freq;
	QueryPerformanceFrequency(&freq);
	cpuTicksToMs = 1000.0 / static_cast<double>(freq.QuadPart);

	for (auto& frame : frames) {
		frame.batch.Configure(kMaxTimers, "Profiler::Frame");
		frame.batch.Preallocate(a_device);
		frame.timers.resize(kMaxTimers);
		frame.activeStack.clear();
		frame.acquiredTimerCount = 0;
		frame.inFlight = false;
	}

	writeFrame = 0;
	readFrame = 0;
	framesSinceInit = 0;
	frameActive = false;
	initialized = true;
	// userEnabled is left as-is: a device teardown/re-init (e.g. display
	// mode change) must not silently re-enable a user's "off" preference.
	captureRequested.store(false, std::memory_order_release);
	captureActive.store(false, std::memory_order_release);
	activeCpuTimers.clear();
	completedCpuTimers.clear();
	resolvedTotalMs = 0.0f;
	resolvedCpuTotalMs = 0.0f;
	capturedFrameCount = 0;
	acquiredSlotsThisFrame = 0;
	acquiredSlots = 0;
	peakAcquiredSlots = 0;
	slotRefusals = 0;
}

void Profiler::Release()
{
	for (auto& frame : frames) {
		frame.batch.ReleaseQueries();
		frame.timers.clear();
		frame.activeStack.clear();
		frame.inFlight = false;
		frame.cpuTimers.clear();
	}
	results.clear();
	knownTimers.clear();
	knownTimerIndex.clear();
	collectedFrames = 0;
	totalTimeMs = 0.0f;
	cpuTotalTimeMs = 0.0f;
	activeCpuTimers.clear();
	completedCpuTimers.clear();
	frameActive = false;
	initialized = false;
	device = nullptr;
	context = nullptr;
	// userEnabled is left as-is; see the matching note in Initialize().
	captureRequested.store(false, std::memory_order_release);
	captureActive.store(false, std::memory_order_release);
}

void Profiler::SetUserEnabled(bool a_enabled)
{
	userEnabled.store(a_enabled, std::memory_order_release);
	if (!a_enabled) {
		captureRequested.store(false, std::memory_order_release);
		captureActive.store(false, std::memory_order_release);
	}
}

void Profiler::RequestCapture()
{
	if (!IsUserEnabled())
		return;
	captureRequested.store(true, std::memory_order_release);
}

void Profiler::BeginFrame()
{
	if (!initialized || !context || frameActive || !IsEnabled())
		return;

	if (!CollectResults())
		return;

	auto& frame = frames[writeFrame];
	frame.batch.Reset();
	frame.activeStack.clear();
	frame.acquiredTimerCount = 0;
	frame.inFlight = true;
	frameActive = true;
	acquiredSlotsThisFrame = 0;
	frame.batch.BeginBatch(device, context);
}

bool Profiler::BeginPass(std::string_view name, bool fireCallbacks)
{
	if (!initialized || !context || !IsEnabled())
		return false;

	if (!frameActive)
		BeginFrame();
	if (!frameActive)
		return false;

	auto& frame = frames[writeFrame];
	const int slot = frame.batch.AcquireInterval(device, context);
	if (slot < 0) {
		slotRefusals++;
		return false;
	}
	acquiredSlotsThisFrame++;

	auto& timer = frame.timers[slot];
	timer.name = name;
	timer.depth = static_cast<uint32_t>(frame.activeStack.size());
	timer.parentSlot = frame.activeStack.empty() ? -1 : frame.activeStack.back();
	QueryPerformanceCounter(&timer.cpuBegin);
	frame.activeStack.push_back(slot);
	frame.acquiredTimerCount = std::max(frame.acquiredTimerCount, static_cast<uint32_t>(slot + 1));

	if (fireCallbacks && beginPerfEvent)
		beginPerfEvent(name);
	return true;
}

void Profiler::EndPass(bool fireCallbacks)
{
	if (!initialized || !context || !frameActive)
		return;

	auto& frame = frames[writeFrame];
	if (frame.activeStack.empty())
		return;

	const int slot = frame.activeStack.back();
	frame.activeStack.pop_back();

	auto& timer = frame.timers[slot];

	LARGE_INTEGER cpuEnd;
	QueryPerformanceCounter(&cpuEnd);
	timer.cpuMs = static_cast<float>(static_cast<double>(cpuEnd.QuadPart - timer.cpuBegin.QuadPart) * cpuTicksToMs);

	frame.batch.CloseInterval(context, static_cast<uint32_t>(slot));

	if (fireCallbacks && endPerfEvent)
		endPerfEvent({});
}

void Profiler::EndFrame(uint32_t a_frameCount)
{
	if (!initialized || !context) {
		captureRequested.store(false, std::memory_order_release);
		captureActive.store(false, std::memory_order_release);
		activeCpuTimers.clear();
		completedCpuTimers.clear();
		return;
	}

	const bool hasCpuTimers = !completedCpuTimers.empty();

	// Fully idle: no frame open now, no CPU-only scopes closed this cycle,
	// and the user has profiling off. Nothing to drain -- publish zero
	// totals so always-visible overlay rows don't show a stale sum, and
	// latch whatever capture state was requested.
	if (!IsUserEnabled() && !frameActive && !hasCpuTimers) {
		totalTimeMs = 0.0f;
		cpuTotalTimeMs = 0.0f;
		captureRequested.store(false, std::memory_order_release);
		captureActive.store(false, std::memory_order_release);
		return;
	}

	if (!frameActive) {
		// No GPU frame open this cycle (capture was off, or nothing called
		// BeginPass), but the ring may still hold an older frame's results
		// pending resolution -- keep draining it even while otherwise idle.
		if (!CollectResults()) {
			// Oldest frame's GPU data isn't ready yet; still let capture
			// toggle on/off rather than blocking the latch on it.
			captureActive.store(captureRequested.exchange(false, std::memory_order_acq_rel), std::memory_order_release);
			return;
		}

		if (!hasCpuTimers) {
			totalTimeMs = 0.0f;
			cpuTotalTimeMs = 0.0f;
			// Idle: walk the ring so every slot left over from the capture that
			// just ended drains, instead of re-checking the same already-drained
			// slot forever -- otherwise the next capture's first frames would
			// replay this session's leftover samples as if they were current.
			// Does not stamp capturedFrame on the skipped slot -- its lagging
			// stamp on resolve IS the stale-session-replay signal.
			if (std::ranges::any_of(frames, [](const FrameQueries& f) { return f.inFlight || !f.cpuTimers.empty(); }))
				writeFrame = (writeFrame + 1) % kFrameLatency;
			captureActive.store(captureRequested.exchange(false, std::memory_order_acq_rel), std::memory_order_release);
			return;
		}
		// Otherwise, CPU-only scopes closed with no GPU pass active this
		// cycle -- fall through and advance the ring so they flow through
		// the same latency pipeline as a GPU frame instead of being
		// dropped or merged into the wrong cycle.
		acquiredSlots = 0;
	} else {
		frameActive = false;
		frames[writeFrame].batch.EndBatch(context);
		acquiredSlots = acquiredSlotsThisFrame;
		peakAcquiredSlots = std::max(peakAcquiredSlots, acquiredSlotsThisFrame);
	}

	if (hasCpuTimers) {
		frames[writeFrame].cpuTimers = std::move(completedCpuTimers);
		completedCpuTimers.clear();
	}

	frames[writeFrame].capturedFrame = a_frameCount;
	writeFrame = (writeFrame + 1) % kFrameLatency;
	framesSinceInit++;
	captureActive.store(captureRequested.exchange(false, std::memory_order_acq_rel), std::memory_order_release);
}

Profiler::KnownTimer& Profiler::GetOrCreateTimer(const std::string& name)
{
	auto [it, inserted] = knownTimerIndex.try_emplace(name, knownTimers.size());
	if (inserted) {
		KnownTimer kt;
		kt.name = name;
		knownTimers.push_back(std::move(kt));
	}
	return knownTimers[it->second];
}

bool Profiler::BeginCpuPass(std::string_view name)
{
	if (!IsEnabled())
		return false;
	CpuTimer timer;
	timer.name = name;
	QueryPerformanceCounter(&timer.cpuBegin);
	// A CPU scope opened inside an enclosing CS_GPU_PASS is never top-level:
	// that GPU pass's own paired cpuMs already spans it, so counting it again
	// here would double it. activeCpuTimers alone can't see the GPU pass
	// stack, so check frame.activeStack too.
	const bool insideGpuPass = frameActive && !frames[writeFrame].activeStack.empty();
	timer.depth = static_cast<uint32_t>(activeCpuTimers.size()) + (insideGpuPass ? 1u : 0u);
	activeCpuTimers.push_back(std::move(timer));
	return true;
}

void Profiler::EndCpuPass()
{
	if (activeCpuTimers.empty())
		return;

	CpuTimer timer = std::move(activeCpuTimers.back());
	activeCpuTimers.pop_back();

	LARGE_INTEGER cpuEnd;
	QueryPerformanceCounter(&cpuEnd);
	const float cpuMs = static_cast<float>(static_cast<double>(cpuEnd.QuadPart - timer.cpuBegin.QuadPart) * cpuTicksToMs);
	if (!IsValidProfilerSample(cpuMs))
		return;

	completedCpuTimers.push_back({ std::move(timer.name), cpuMs, timer.depth });
}

bool Profiler::CollectResults()
{
	if (framesSinceInit < kFrameLatency)
		return true;

	readFrame = writeFrame;
	auto& frame = frames[readFrame];
	if (!frame.inFlight && frame.cpuTimers.empty())
		return true;

	struct ActiveTimerData
	{
		/// Sum of interval self times after direct nested passes are removed.
		float gpuMs = 0.0f;
		/// Inclusive depth-0 duration for the exact frame-total check.
		float topLevelMs = 0.0f;
		float cpuMs = 0.0f;
		bool hasGpu = false;
		bool hasCpu = false;
	};
	std::unordered_map<std::string, ActiveTimerData> activeTimers;
	float activeTotalMs = 0.0f;
	float activeCpuTotalMs = 0.0f;
	// Whether a GPU bracket actually resolved this cycle -- distinct from a
	// cycle where only CPU-only scopes advanced the ring -- so GPU-side
	// rolling stats for timers that missed this frame only get a zero
	// sample when a GPU frame genuinely completed.
	bool gpuFrameResolved = false;

	if (frame.inFlight) {
		struct IntervalTiming
		{
			float gpuMs = 0.0f;
			bool valid = false;
		};
		std::vector<IntervalTiming> intervals(frame.acquiredTimerCount);
		const auto status = frame.batch.TryResolve(context,
			[&](uint32_t i, uint64_t deltaTicks, uint64_t frequency) {
				if (i >= intervals.size())
					return;
				auto& timer = frame.timers[i];
				float ms = static_cast<float>(static_cast<double>(deltaTicks) * 1000.0 / static_cast<double>(frequency));
				// Checked independently: an unrelated CPU-side stall (e.g. a
				// loading hitch spanning this pass) must not discard an
				// otherwise-valid GPU timestamp, and vice versa.
				const bool gpuValid = IsValidProfilerSample(ms);
				const bool cpuValid = IsValidProfilerSample(timer.cpuMs);
				if (!gpuValid && !cpuValid)
					return;

				auto& entry = activeTimers[timer.name];
				GetOrCreateTimer(timer.name);
				if (gpuValid)
					intervals[i] = { ms, true };
				if (cpuValid) {
					entry.cpuMs += timer.cpuMs;
					entry.hasCpu = true;
					if (timer.depth == 0)
						activeCpuTotalMs += timer.cpuMs;
				}
			});
		if (status == Util::TimestampQueryBatch::Status::NotReady)
			return false;

		std::vector<float> nestedGpuMs(intervals.size(), 0.0f);
		for (uint32_t i = 0; i < intervals.size(); ++i) {
			if (!intervals[i].valid)
				continue;
			int32_t parentSlot = frame.timers[i].parentSlot;
			while (parentSlot >= 0 && static_cast<uint32_t>(parentSlot) < intervals.size() && !intervals[parentSlot].valid)
				parentSlot = frame.timers[parentSlot].parentSlot;
			if (parentSlot >= 0 && static_cast<uint32_t>(parentSlot) < intervals.size())
				nestedGpuMs[parentSlot] += intervals[i].gpuMs;
		}

		for (uint32_t i = 0; i < intervals.size(); ++i) {
			auto& timer = frame.timers[i];
			if (!intervals[i].valid)
				continue;

			// Aggregate same-name call sites only after interval self time is known.
			auto& entry = activeTimers[timer.name];
			entry.gpuMs += std::max(intervals[i].gpuMs - nestedGpuMs[i], 0.0f);
			entry.hasGpu = true;
			if (timer.depth == 0) {
				activeTotalMs += intervals[i].gpuMs;
				entry.topLevelMs += intervals[i].gpuMs;
			}
		}

		frame.inFlight = false;
		gpuFrameResolved = (status == Util::TimestampQueryBatch::Status::Ok);
	}

	collectedFrames++;

	// CPU-only scopes queued for this cycle, independent of whether a GPU
	// pass ran (e.g. CPU-side bookkeeping with no GPU cost of its own).
	const bool hadCpuTimers = !frame.cpuTimers.empty();
	for (auto& timer : frame.cpuTimers) {
		auto& entry = activeTimers[timer.name];
		entry.cpuMs += timer.cpuMs;
		entry.hasCpu = true;
		// Only top-level scopes count toward the frame CPU total -- a nested
		// CS_PROFILE_CPU_SCOPE, or one opened inside a CS_GPU_PASS, already
		// falls within its parent's measured time.
		if (timer.depth == 0)
			activeCpuTotalMs += timer.cpuMs;

		GetOrCreateTimer(timer.name);
	}
	frame.cpuTimers.clear();

	totalTimeMs = activeTotalMs;
	cpuTotalTimeMs = activeCpuTotalMs;
	// Never zeroed while idle, unlike totalTimeMs/cpuTotalTimeMs; see
	// GetResolvedTotalTimeMs().
	resolvedTotalMs = activeTotalMs;
	resolvedCpuTotalMs = activeCpuTotalMs;
	capturedFrameCount = frame.capturedFrame;

	// Exactly one sample per known timer per cycle (load-bearing:
	// ProfilingRenderer's history alignment assumes this). A timer missing
	// this cycle gets a zero sample instead, but only for a side that had a
	// chance to report -- not on a disjoint bracket or skipped BeginFrame.
	const bool cpuCycleResolved = gpuFrameResolved || hadCpuTimers;
	for (auto& known : knownTimers) {
		auto it = activeTimers.find(known.name);
		const bool freshGpu = it != activeTimers.end() && it->second.hasGpu;
		const bool freshCpu = it != activeTimers.end() && it->second.hasCpu;
		if (freshGpu) {
			known.hasGpu = true;
			known.gpu.PushSample(it->second.gpuMs);
		} else if (gpuFrameResolved && known.hasGpu) {
			known.gpu.PushSample(0.0f);
		}
		if (freshCpu) {
			known.hasCpu = true;
			known.cpu.PushSample(it->second.cpuMs);
		} else if (cpuCycleResolved && known.hasCpu) {
			known.cpu.PushSample(0.0f);
		}
		if (freshGpu || freshCpu)
			known.lastSampleFrame = collectedFrames;
	}

	RetireStaleTimers();

	results.clear();
	results.reserve(knownTimers.size());
	for (const auto& known : knownTimers) {
		TimerResult result;
		result.name = known.name;
		result.hasGpu = known.hasGpu;
		result.hasCpu = known.hasCpu;
		auto it = activeTimers.find(known.name);
		result.activeGpu = it != activeTimers.end() && it->second.hasGpu;
		result.activeCpu = it != activeTimers.end() && it->second.hasCpu;
		result.gpuTimeMs = result.activeGpu ? it->second.gpuMs : known.gpu.lastMs;
		result.topLevelMs = result.activeGpu ? it->second.topLevelMs : 0.0f;
		result.cpuTimeMs = result.activeCpu ? it->second.cpuMs : known.cpu.lastMs;
		result.avgMs = known.gpu.GetAverage();
		result.p95Ms = known.gpu.GetPercentile(95.0f);
		result.p99Ms = known.gpu.GetPercentile(99.0f);
		result.cpuAvgMs = known.cpu.GetAverage();
		result.cpuP95Ms = known.cpu.GetPercentile(95.0f);
		result.cpuP99Ms = known.cpu.GetPercentile(99.0f);
		result.valid = true;
		result.historyBuffer = known.gpu.history;
		result.historyHead = known.gpu.head;
		result.historyCount = known.gpu.count;
		result.cpuHistoryBuffer = known.cpu.history;
		result.cpuHistoryHead = known.cpu.head;
		result.cpuHistoryCount = known.cpu.count;
		results.push_back(std::move(result));
	}
	return true;
}
void Profiler::RetireStaleTimers()
{
	const size_t before = knownTimers.size();
	std::erase_if(knownTimers, [this](const KnownTimer& known) {
		return collectedFrames - known.lastSampleFrame >= kTimerRetireFrames;
	});
	if (knownTimers.size() != before)
		RebuildTimerIndex();
}

void Profiler::RebuildTimerIndex()
{
	knownTimerIndex.clear();
	for (size_t i = 0; i < knownTimers.size(); i++)
		knownTimerIndex[knownTimers[i].name] = i;
}
