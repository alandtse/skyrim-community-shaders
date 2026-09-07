#pragma once

#include <BS_thread_pool.hpp>
#include <deque>
#include <efsw/efsw.hpp>
#include <functional>
#include <optional>
#include <unordered_set>
#include <variant>
#include <vector>

#include "Utils/CacheInvalidation.h"
#include "Utils/WinApi.h"

struct ID3D11ComputeShader;

using namespace std::chrono;

namespace ShaderConstants
{
	struct LightingPS
	{
		static const LightingPS& Get()
		{
			// Keep raw runtime check: this static can initialize before globals::ReInit().
			static LightingPS instance = REL::Module::IsVR() ? GetVR() : GetFlat();
			return instance;
		}

		static LightingPS GetFlat()
		{
			return LightingPS{};
		}

		static LightingPS GetVR()
		{
			return LightingPS{
				.AmbientColor = 24,
				.FogColor = 25,
				.ColourOutputClamp = 26,
				.EnvmapData = 27,
				.ParallaxOccData = 28,
				.TintColor = 29,
				.LODTexParams = 30,
				.SpecularColor = 31,
				.SparkleParams = 32,
				.MultiLayerParallaxData = 33,
				.LightingEffectParams = 34,
				.IBLParams = 35,
				.LandscapeTexture1to4IsSnow = 36,
				.LandscapeTexture5to6IsSnow = 37,
				.LandscapeTexture1to4IsSpecPower = 38,
				.LandscapeTexture5to6IsSpecPower = 39,
				.SnowRimLightParameters = 40,
				.CharacterLightParams = 41,
				.PBRFlags = 44,
				.PBRParams1 = 45,
				.LandscapeTexture2PBRParams = 46,
				.LandscapeTexture3PBRParams = 47,
				.LandscapeTexture4PBRParams = 48,
				.LandscapeTexture5PBRParams = 49,
				.LandscapeTexture6PBRParams = 50,
				.PBRParams2 = 51,
				.LandscapeTexture1GlintParameters = 52,
				.LandscapeTexture2GlintParameters = 53,
				.LandscapeTexture3GlintParameters = 54,
				.LandscapeTexture4GlintParameters = 55,
				.LandscapeTexture5GlintParameters = 56,
				.LandscapeTexture6GlintParameters = 57,
				.MaterialObjectRGBScale = 58,  // RGB multipliers for material objects

				.ShadowSampleParam = 18,
				.EndSplitDistances = 19,
				.StartSplitDistances = 20,
				.DephBiasParam = 21,
				.ShadowLightParam = 22,
				.ShadowMapProj = 23,
				.InvWorldMat = 42,
				.PreviousWorldMat = 43,
			};
		}

		const int32_t NumLightNumShadowLight = 0;
		const int32_t PointLightPosition = 1;
		const int32_t PointLightColor = 2;
		const int32_t DirLightDirection = 3;
		const int32_t DirLightColor = 4;
		const int32_t DirectionalAmbient = 5;
		const int32_t AmbientSpecularTintAndFresnelPower = 6;
		const int32_t MaterialData = 7;
		const int32_t EmitColor = 8;
		const int32_t AlphaTestRef = 9;
		const int32_t ShadowLightMaskSelect = 10;
		const int32_t VPOSOffset = 11;
		const int32_t ProjectedUVParams = 12;
		const int32_t ProjectedUVParams2 = 13;
		const int32_t ProjectedUVParams3 = 14;
		const int32_t SplitDistance = 15;
		const int32_t SSRParams = 16;
		const int32_t WorldMapOverlayParametersPS = 17;
		const int32_t AmbientColor = 18;
		const int32_t FogColor = 19;
		const int32_t ColourOutputClamp = 20;
		const int32_t EnvmapData = 21;
		const int32_t ParallaxOccData = 22;
		const int32_t TintColor = 23;
		const int32_t LODTexParams = 24;
		const int32_t SpecularColor = 25;
		const int32_t SparkleParams = 26;
		const int32_t MultiLayerParallaxData = 27;
		const int32_t LightingEffectParams = 28;
		const int32_t IBLParams = 29;
		const int32_t LandscapeTexture1to4IsSnow = 30;
		const int32_t LandscapeTexture5to6IsSnow = 31;
		const int32_t LandscapeTexture1to4IsSpecPower = 32;
		const int32_t LandscapeTexture5to6IsSpecPower = 33;
		const int32_t SnowRimLightParameters = 34;
		const int32_t CharacterLightParams = 35;
		const int32_t PBRFlags = 36;
		const int32_t PBRParams1 = 37;
		const int32_t LandscapeTexture2PBRParams = 38;
		const int32_t LandscapeTexture3PBRParams = 39;
		const int32_t LandscapeTexture4PBRParams = 40;
		const int32_t LandscapeTexture5PBRParams = 41;
		const int32_t LandscapeTexture6PBRParams = 42;
		const int32_t PBRParams2 = 43;
		const int32_t LandscapeTexture1GlintParameters = 44;
		const int32_t LandscapeTexture2GlintParameters = 45;
		const int32_t LandscapeTexture3GlintParameters = 46;
		const int32_t LandscapeTexture4GlintParameters = 47;
		const int32_t LandscapeTexture5GlintParameters = 48;
		const int32_t LandscapeTexture6GlintParameters = 49;

		const int32_t MaterialObjectRGBScale = 50;  // RGB multipliers for material objects

		const int32_t ShadowSampleParam = -1;
		const int32_t EndSplitDistances = -1;
		const int32_t StartSplitDistances = -1;
		const int32_t DephBiasParam = -1;
		const int32_t ShadowLightParam = -1;
		const int32_t ShadowMapProj = -1;
		const int32_t InvWorldMat = -1;
		const int32_t PreviousWorldMat = -1;
	};

	struct GrassPS
	{
		static const GrassPS& Get()
		{
			static GrassPS instance = REL::Module::IsVR() ? GetVR() : GetFlat();
			return instance;
		}

		static GrassPS GetFlat()
		{
			return GrassPS{};
		}

		static GrassPS GetVR()
		{
			return GrassPS{};
		}

		const int32_t WorldViewProj = 0;
		const int32_t WorldView = 1;
		const int32_t World = 2;
		const int32_t PreviousWorld = 3;
		const int32_t FogNearColor = 4;
		const int32_t WindVector = 5;
		const int32_t WindTimer = 6;
		const int32_t DirLightDirection = 7;
		const int32_t PreviousWindTimer = 8;
		const int32_t DirLightColor = 9;
		const int32_t AlphaParam1 = 10;
		const int32_t AmbientColor = 11;
		const int32_t AlphaParam2 = 12;
		const int32_t ScaleMask = 13;

		const int32_t PBRFlags = 14;
		const int32_t PBRParams1 = 15;
		const int32_t PBRParams2 = 16;
	};

	struct EffectPS
	{
		static const EffectPS& Get()
		{
			static EffectPS instance = REL::Module::IsVR() ? GetVR() : GetFlat();
			return instance;
		}

		static EffectPS GetFlat()
		{
			return EffectPS{};
		}

		static EffectPS GetVR()
		{
			return EffectPS{};
		}

		const int32_t PropertyColor = 0;
		const int32_t AlphaTestRef = 1;
		const int32_t MembraneRimColor = 2;
		const int32_t MembraneVars = 3;
		const int32_t PLightPositionX = 4;
		const int32_t PLightPositionY = 5;
		const int32_t PLightPositionZ = 6;
		const int32_t PLightingRadiusInverseSquared = 7;
		const int32_t PLightColorR = 8;
		const int32_t PLightColorG = 9;
		const int32_t PLightColorB = 10;
		const int32_t DLightColor = 11;
		const int32_t VPOSOffset = 12;
		const int32_t CameraData = 13;
		const int32_t FilteringParam = 14;
		const int32_t BaseColor = 15;
		const int32_t BaseColorScale = 16;
		const int32_t LightingInfluence = 17;
		const int32_t ExtendedFlags = 18;
	};
}

namespace SIE
{
	enum class ShaderClass
	{
		Vertex,
		Pixel,
		Compute,
		Total,
	};

	class ShaderCompilationTask
	{
	public:
		enum Status
		{
			Pending,
			Failed,
			Completed
		};
		ShaderCompilationTask(ShaderClass shaderClass, const RE::BSShader& shader,
			uint32_t descriptor);
		/** @brief Compiles the shader, writing the result to the ShaderCache. */
		void Perform() const;

		/** @brief Returns a unique hash identifying this shader class, type, and descriptor combo. */
		size_t GetId() const;
		/** @brief Packs a task identity: descriptor in bits 0-31, shader type in bits 32-59,
		 *  shader class in bits 60-63. Adding a RE::BSShader::Type value beyond 2^28 or a
		 *  ShaderClass value beyond 2^4 silently collides task ids. */
		static size_t MakeId(ShaderClass shaderClass, RE::BSShader::Type shaderType, uint32_t descriptor);
		/** @brief Returns a human-readable string describing this task (shader file, class, defines). */
		std::string GetString() const;
		/** @brief Path to the actual HLSL source this task compiles from (not always fxpFilename -- see ImageSpace shaders). */
		std::wstring GetSourcePath() const;

		/**
		 * LPT scheduling score: higher = more expensive = should be dispatched first.
		 * Based on shader type, class, descriptor complexity, and known heavy defines.
		 * Computed once at construction and cached.
		 */
		/** Gets the cached LPT scheduling priority. */
		int GetPriority() const { return cachedPriority; }
		/** @brief Records the QPC timestamp when this task was enqueued. */
		void SetEnqueuedQpc(int64_t qpc) { enqueuedQpc = qpc; }
		/** @brief Gets the QPC timestamp when this task was enqueued. */
		int64_t GetEnqueuedQpc() const { return enqueuedQpc; }
		/** @brief Records the CompilationSet batch generation this task was enqueued under. */
		void SetGeneration(uint64_t gen) { generation = gen; }
		/** @brief Gets the batch generation this task was enqueued under, for staleness checks against a later Clear(). */
		uint64_t GetGeneration() const { return generation; }

		bool operator==(const ShaderCompilationTask& other) const;

	protected:
		ShaderClass shaderClass;
		const RE::BSShader& shader;
		uint32_t descriptor;

	private:
		static int ComputePriority(ShaderClass shaderClass, const RE::BSShader& shader, uint32_t descriptor);
		int cachedPriority;
		int64_t enqueuedQpc = 0;
		uint64_t generation = 0;
	};
}

template <>
struct std::hash<SIE::ShaderCompilationTask>
{
	std::size_t operator()(const SIE::ShaderCompilationTask& task) const noexcept
	{
		return task.GetId();
	}
};

struct TaskPriorityLess
{
	bool operator()(const SIE::ShaderCompilationTask& a, const SIE::ShaderCompilationTask& b) const
	{
		if (a.GetPriority() != b.GetPriority()) {
			return a.GetPriority() < b.GetPriority();
		}
		return a.GetId() < b.GetId();
	}
};

namespace SIE
{
	/**
	 * Threshold above which a shader task is considered "heavy" and benefits
	 * from P-core placement on hybrid CPUs. Used for thread-priority hints,
	 * telemetry, and developer-facing diagnostics.
	 */
	constexpr int kHeavyPriorityThreshold = 500;

	class CompilationSet
	{
	public:
		LARGE_INTEGER lastReset;
		std::atomic<int64_t> lastResetQpc{ 0 };  // Lock-free mirror of lastReset.QuadPart for GetLastResetQpc().
		LARGE_INTEGER lastCalculation;
		std::atomic<int64_t> completionTime;  // When compilation completed (QuadPart equivalent)
		LARGE_INTEGER frequency;
		LARGE_INTEGER totalTime = { 0 };

		CompilationSet()
		{
			QueryPerformanceFrequency(&frequency);
			QueryPerformanceCounter(&lastReset);
			lastResetQpc.store(lastReset.QuadPart, std::memory_order_relaxed);
			QueryPerformanceCounter(&lastCalculation);
			completionTime.store(0, std::memory_order_relaxed);
		}

		/** @brief Blocks until a dispatch slot and some work (a queued permutation-matrix
		 *  task, or -- only once the matrix is empty -- a queued aux closure) are both
		 *  available, or the stop token is signalled. The matrix always wins admission
		 *  over aux when both are ready, preserving LPT priority ordering. */
		std::optional<std::variant<ShaderCompilationTask, std::function<void()>>> TryTakeNext(std::stop_token stoken);
		/** @brief Enqueues a task for compilation. */
		void Add(const ShaderCompilationTask& task);
		/** @brief Marks a task as finished and records its timing metrics. */
		void Complete(const ShaderCompilationTask& task);
		/** @brief Frees a dispatch slot and wakes TryTakeNext(). Call even on a stale-generation
		 *  task -- it still held a real slot, and nothing else wakes a waiter blocked on it. */
		void ReleaseDispatchSlot();
		/** @brief Queues a compile closure that isn't a ShaderCompilationTask (currently:
		 *  standalone compute-shader compiles from PostProcessFeature::CompileComputeShadersAsync)
		 *  to share the same dispatchedTasksInFlight budget as the main permutation matrix,
		 *  instead of being submitted to compilationPool independently of it. Wakes TryTakeNext(). */
		void EnqueueAux(std::function<void()> work);
		/** @brief Latches the compilation-phase clock at the moment a real (non-disk-hit)
		 *  compile begins, so ETA and the "started" log reflect the actual first compile
		 *  rather than when it finishes. Logs once per phase. */
		void MarkPhaseStarted();
		/** @brief Resets all task queues and counters for a fresh compilation pass. */
		void Clear();
		/** @brief Atomically advances the generation counter without touching queues/counters.
		 *  Call before ShaderCache::Clear()'s map-wipe locks so a worker's write-site check
		 *  (see MakeAndAdd*Shader) sees the new value once it can observe the wipe. */
		void BumpGeneration() { generation.fetch_add(1, std::memory_order_release); }
		/** @brief Drops the given task ids from the completed/in-progress bookkeeping so a
		 *  scoped cache evict can re-enqueue them. Leaves queued work in availableTasks
		 *  and does not touch progress counters. */
		void Forget(const std::unordered_set<size_t>& a_taskIds);
		/** @brief Formats a millisecond duration into a human-readable time string. */
		static std::string GetHumanTime(double a_totalMs);
		/** @brief Estimates remaining compilation time based on completed task throughput. */
		double GetEta();
		/** @brief Returns a formatted summary of compilation progress and timing. */
		std::string GetStatsString(bool a_timeOnly = false, bool a_elapsedOnly = false);
		std::atomic<uint64_t> completedTasks = 0;
		std::atomic<uint64_t> totalTasks = 0;
		std::atomic<uint64_t> failedTasks = 0;
		std::atomic<uint32_t> dispatchedTasksInFlight = 0;  // Admission budget enforced by TryTakeNext()
		std::atomic<uint64_t> cacheHitTasks = 0;            // number of compiles of a previously seen shader combo
		std::atomic<uint64_t> diskHitTasks = 0;             // tasks resolved from disk cache rather than compiled
		std::atomic<uint64_t> diskHitPriorityWeight = 0;    // cumulative priority weight of disk-hit tasks
		std::atomic<uint64_t> digestComputeCount = 0;       // content-digest computations performed (disk-cache checks + post-compile manifest writes)
		std::atomic<int64_t> digestComputeTimeUs = 0;       // cumulative microseconds spent computing content digests
		std::atomic<uint64_t> digestHitTasks = 0;           // disk-cache validity checks where the manifest digest confirmed the cached blob is still valid
		std::atomic<uint64_t> digestMissTasks = 0;          // disk-cache validity checks where the manifest digest marked the cached blob stale (recompile)
		LARGE_INTEGER compilationPhaseStart = { 0 };        // time of first non-disk-hit task dispatch
		std::atomic<bool> compilationPhaseStarted = false;  // set when first actual compilation begins
		std::atomic<uint64_t> slowTasks = 0;                // shaders taking >= 2s
		std::atomic<uint64_t> verySlowTasks = 0;            // shaders taking >= 8s
		std::atomic<uint64_t> totalPriorityWeight = 0;      // sum of (GetPriority()+1) for all queued tasks
		std::atomic<uint64_t> completedPriorityWeight = 0;  // sum of (GetPriority()+1) for completed/failed tasks
		std::atomic<uint32_t> heavyTasksInFlight = 0;       // number of dispatched heavy (>= kHeavyPriorityThreshold) tasks still running
		std::atomic<uint64_t> generation = 0;               // bumped by Clear(); tags tasks so a post-Clear() Complete() can detect staleness
		std::mutex compilationMutex;

		/** Per-task timing record stored for post-mortem analysis and developer UI. */
		struct SlowTaskRecord
		{
			std::string key;  // ShaderCompilationTask::GetString() — "fxpFile:Class:defines"
			double elapsedMs = 0.0;
			double queueWaitMs = 0.0;
			int priority = 0;               // estimated compile weight (see ComputePriority)
			int defineCount = 0;            // popcount of descriptor — active define permutations
			uintmax_t sourceSizeBytes = 0;  // HLSL source file size at compile time
			uint32_t threadId = 0;          // worker thread id at compile time, for trace export
			int64_t startQpc = 0;           // QueryPerformanceCounter ticks at compile start, for trace export
		};

		/** On-demand parallelism metrics derived from task timings. */
		struct ParallelismStats
		{
			double workMs = 0.0;                  // W = sum of all task times
			double spanMs = 0.0;                  // S ~= longest single task
			double makespanMs = 0.0;              // T_p = wall-clock compile duration
			double avgParallelism = 0.0;          // W / S
			double infiniteCoreEfficiency = 0.0;  // S / T_p
			double infiniteCoreGapPercent = 0.0;  // 100 * (1 - S / T_p)
			double avgQueueWaitMs = 0.0;          // average enqueue -> dispatch delay
			double maxQueueWaitMs = 0.0;          // worst enqueue -> dispatch delay
			size_t sampleCount = 0;
		};

		/**
		 * All per-task timing records for this build (appended from multiple threads).
		 * Protected by slowTasksMutex.
		 */
		std::vector<SlowTaskRecord> slowTaskRecords;
		mutable std::mutex slowTasksMutex;

		/** @brief Returns a copy of the N records with the highest elapsedMs, sorted descending. */
		std::vector<SlowTaskRecord> GetTopSlowTasks(size_t n = 3) const;

		/** @brief Returns a copy of every task record collected for the current build. */
		std::vector<SlowTaskRecord> GetAllTaskRecords() const;

		/** @brief QPC tick of the last Clear(), a per-build generation marker so UI caches
		 *  invalidate on a fresh build even if it happens to complete the same task count. */
		int64_t GetLastResetQpc() const { return lastResetQpc.load(std::memory_order_relaxed); }

		/** @brief Ticks per second for converting QPC-based timestamps (e.g. SlowTaskRecord::startQpc). */
		int64_t GetQpcFrequency() const { return frequency.QuadPart; }

		/** @brief Computes parallelism metrics on demand from collected task timings. */
		std::optional<ParallelismStats> GetParallelismStats() const;

	private:
		/** Tasks awaiting dispatch, ordered by cached priority and task id. */
		std::set<ShaderCompilationTask, TaskPriorityLess> availableTasks;
		std::set<ShaderCompilationTask, TaskPriorityLess> tasksInProgress;
		std::set<ShaderCompilationTask, TaskPriorityLess> processedTasks;  // completed or failed
		std::deque<std::function<void()>> pendingAuxTasks;                 // see EnqueueAux/TryTakeNext
		std::condition_variable_any conditionVariable;
	};

	struct ShaderCacheResult
	{
		ID3DBlob* blob;
		ShaderCompilationTask::Status status;
		system_clock::time_point compileTime = system_clock::now();
		bool loadedFromDisk = false; /**< true when the shader blob was read from the disk cache rather than compiled */
		/** Generation this entry was claimed/written under. Only meaningfully read for a
		 *  Pending entry, to decide whether a stale writer's own claim still owns it
		 *  (see AddCompletedShader); a Completed/Failed entry's stamp is never re-checked. */
		uint64_t generation = 0;
	};

	class UpdateListener;

	class ShaderCache
	{
	public:
		static ShaderCache& Instance()
		{
			static ShaderCache instance;
			return instance;
		}

		/** @brief Returns true if the shader type is one Community Shaders can replace. */
		inline static bool IsSupportedShader(const RE::BSShader::Type type)
		{
			if (!REL::Module::IsVR())
				return type == RE::BSShader::Type::Lighting ||
				       type == RE::BSShader::Type::BloodSplatter ||
				       type == RE::BSShader::Type::DistantTree ||
				       type == RE::BSShader::Type::Sky ||
				       type == RE::BSShader::Type::Grass ||
				       type == RE::BSShader::Type::Particle ||
				       type == RE::BSShader::Type::Water ||
				       type == RE::BSShader::Type::Effect ||
				       type == RE::BSShader::Type::Utility ||
				       type == RE::BSShader::Type::ImageSpace;
			return type == RE::BSShader::Type::Lighting ||
			       type == RE::BSShader::Type::BloodSplatter ||
			       type == RE::BSShader::Type::DistantTree ||
			       type == RE::BSShader::Type::Sky ||
			       type == RE::BSShader::Type::Grass ||
			       type == RE::BSShader::Type::Particle ||
			       type == RE::BSShader::Type::Water ||
			       type == RE::BSShader::Type::Effect ||
			       type == RE::BSShader::Type::Utility ||
			       type == RE::BSShader::Type::ImageSpace;
		}

		/** @brief Returns true if the shader type is one Community Shaders can replace. */
		inline static bool IsSupportedShader(const RE::BSShader& shader)
		{
			return IsSupportedShader(shader.shaderType.get());
		}

		/** @brief Returns true if the HLSL source file exists on disk for this shader. */
		inline static bool IsShaderSourceAvailable(const RE::BSShader& shader);

		/** @brief Returns true if any shader compilation tasks are in progress. */
		bool IsCompiling();
		/** @brief True if a_taskGeneration is set and predates the live generation -- mirrors
		 * Util::GenerationClaim::TryPublish's own staleness rule (see AddCompletedShader). */
		bool IsGenerationStale(std::optional<uint64_t> a_taskGeneration) const;
		/** Gets whether the shader cache is enabled. */
		bool IsEnabled() const;
		/** Sets whether the shader cache is enabled. */
		void SetEnabled(bool value);
		/** Gets whether shader compilation is asynchronous. */
		bool IsAsync() const;
		/** Sets whether shader compilation is asynchronous. */
		void SetAsync(bool value);
		/** Gets whether compiled shaders are dumped to disk as raw blobs. */
		bool IsDump() const;
		/** Sets whether compiled shaders are dumped to disk as raw blobs. */
		void SetDump(bool value);
		/** @brief Signals all compilation threads to stop and clears pending tasks. */
		void StopCompilation();
		/** @brief Drops queued/in-flight compilation work without stopping the management
		 * thread (unlike StopCompilation(), which is terminal). Returns immediately. */
		void CancelCompilation();

		/** Gets whether the persistent disk cache is enabled. */
		bool IsDiskCache() const;
		/** Sets whether the persistent disk cache is enabled. */
		void SetDiskCache(bool value);
		/** @brief Deletes the on-disk shader cache directory plus the rollback and swap slots. Main-thread only: also resets UI-facing mismatch state. */
		void DeleteDiskCache();
		/** @brief Deletes the same on-disk directories as DeleteDiskCache(), without touching UI-facing mismatch state. Safe to call from the file-watcher thread. */
		void DeleteDiskCacheFiles();
		/** @brief Validates disk cache integrity against current shader sources and feature set. */
		void ValidateDiskCache();
		/** @brief Finalizes a boot-detected feature set change: refresh the manifest and clear the change state. */
		void CommitFeatureSetChange();
		/** @brief Swaps the rollback cache back into use and matches boot toggles to it. Restart required. */
		bool RestorePreviousDiskCache();
		/** @brief Writes cache metadata (version, feature list) to the disk cache directory. */
		void WriteDiskCacheInfo();
		/// One disk-cache/runtime state divergence found by ValidateDiskCache.
		/// (Logic lives in Utils/CacheInvalidation.h so tests/cpp can exercise it.)
		using CacheMismatch = Util::CacheInvalidation::CacheMismatch;

		/// Mismatches found at boot (empty when the disk cache validated clean). Returned by
		/// value: devbench's listener thread reads this while the main thread (via
		/// ValidateDiskCache/rollback actions) reassigns it, so a reference would be unsafe.
		std::vector<CacheMismatch> GetCacheMismatches() const
		{
			std::lock_guard lock{ mismatchesMutex };
			return cacheMismatches;
		}
		/// Mismatches between the restorable rollback cache and the current setup. See
		/// GetCacheMismatches for why this is returned by value.
		std::vector<CacheMismatch> GetPreviousCacheMismatches() const
		{
			std::lock_guard lock{ mismatchesMutex };
			return previousCacheMismatches;
		}
		/// True when boot detected a pure feature-toggle change and rotated the old
		/// cache into the rollback slot; cleared once the new cache is committed.
		bool HasFeatureSetChanges() const { return featureSetChanged; }
		/// True after a rollback restore this session: it takes effect on restart.
		bool HasFeatureSetRevertPending() const { return featureSetRevertPending; }
		/// True if the previous cache backup succeeded in this session.
		bool HasFeatureSetCacheBackup() const { return featureSetCacheBackedUp; }
		/// True if Data/ShaderCache.Previous holds a cache restorable for the current setup.
		bool HasPreviousDiskCache() const { return previousDiskCacheAvailable; }

		/// True while an enabled-flip mismatch holds the disk cache: blobs preserved on
		/// disk, this session compiles memory-only, and the menu offers rebuild vs
		/// fix-setup-and-restart (after a fix the cache revalidates untouched).
		bool IsDiskCacheHeld() const { return diskCacheHeld; }

		/// Disk-cache IO gate: user setting AND not held. The hold must not flip
		/// isDiskCache itself -- that value persists as "Enable Disk Cache" in user
		/// settings, so a save during a held session would disable the cache forever.
		/// A pending rollback also gates IO so new blobs can't dirty the restored cache.
		bool IsDiskCacheActive() const { return isDiskCache && !diskCacheHeld && !featureSetRevertPending; }

		/// User accepted the new feature state: wipe the held cache and rebuild to disk.
		void AcceptCacheRebuild();

		/// Records the enabled state a Disable-at-Boot save is about to persist, per
		/// feature, so a future EnabledFlip mismatch that matches exactly what was
		/// recorded here can auto-resolve at boot instead of holding for user input.
		void MarkExpectedFeatureFlip();

		/** Gets whether unchanged shaders are skipped during recompilation. */
		bool IsSkipUnchangedShaders() const;
		/** Sets whether unchanged shaders are skipped during recompilation. */
		void SetSkipUnchangedShaders(bool value);
		/** Gets whether the filesystem watcher for hot-reload is active. */
		bool UseFileWatcher() const;
		/** Sets whether the filesystem watcher for hot-reload is active. */
		void SetFileWatcher(bool value);

		/** @brief Starts the efsw filesystem watcher for shader hot-reload. */
		void StartFileWatcher();
		/** @brief Stops the filesystem watcher and releases its resources. */
		void StopFileWatcher();

		/**
		 * @brief Updates the shader modification time for the given shader type.
		 *
		 * This function checks if the shader's file modification time has changed or
		 * forces an update based on the a_forceUpdate flag. If the file does not exist,
		 * or the shader type is invalid, the update is skipped.
		 *
		 * @param a_type The shader type as a string (case insensitive).
		 * @param a_forceUpdate If true, forces an update regardless of the actual file modification time.
		 * @return true if the shader modification time was updated, false otherwise.
		 */
		bool UpdateShaderModifiedTime(const std::string& a_type, boolean a_forceUpdate = false);
		/**
		 * @brief Checks if the shader has been modified since the given time.
		 *
		 * This function compares the shader's last modification time against the provided
		 * time point to determine if it has been updated.
		 *
		 * @param a_type The shader type as a string (case insensitive).
		 * @param a_current The time point to compare against.
		 * @return true if the shader has been modified after the given time point, false otherwise.
		 */
		bool ShaderModifiedSince(const std::string& a_type, system_clock::time_point a_current);

		void Clear();
		void Clear(RE::BSShader::Type a_type);
		/**
		 * @brief Requests a full Clear() on the render thread instead of running it inline.
		 *
		 * Clear() resets every feature's LazyShader instances, which release their cached
		 * shader without synchronizing with concurrent use of the raw pointer a feature
		 * may still be dispatching from this frame. The file-watcher thread cannot safely
		 * call Clear() directly for this reason; it calls this instead, and the request is
		 * drained by ProcessPendingClear() from the render thread once per frame.
		 */
		void RequestClear();
		/** @brief Drains a pending RequestClear() by running Clear() on the calling (render) thread. Must be called once per frame from the render thread. */
		void ProcessPendingClear();
		/**
   		* @brief Clears and marks shaders for recompilation based on the given path.
 		*
 		* This function looks up the provided `a_path` in the `hlslToShaderMap`.
		* If the path exists in the map, it iterates through all the shader entries associated
		* with that path, clears the shaders, and marks them for recompilation by updating their
		* modified times, and logs the operation.
		*
		* @param a_path The file path associated with the shaders to be marked for recompilation.
		*
		* @returns bool whether a shader was found in the `hlslToShaderMap`
		*
		* @note The function assumes that `a_path` corresponds to shaders stored in `hlslToShaderMap`.
		* If the path is not found in the map, the function does nothing. Also, only files compiled
		* during session will be identified. Disk cached shaders will not be cleared and a further
		* cache clear may be necessary.
		*
		* @threadsafe The function locks the internal map (`mapMutex`) to ensure thread safety when
		* accessing or modifying shared shader map data.
		*/
		bool Clear(const std::string& a_path);

		/** @brief Publishes a_blob as this task's result. Returns false if the compile
		 *  failed, a_taskGeneration is stale, or Clear(path) evicted this key mid-flight. */
		bool AddCompletedShader(ShaderClass shaderClass, const RE::BSShader& shader, uint32_t descriptor, ID3DBlob* a_blob, bool fromDisk = false, std::optional<uint64_t> a_taskGeneration = std::nullopt);

		enum class ClaimResult
		{
			CacheHit,  // Already compiled; use the returned blob
			Claimed    // Claimed as Pending; caller must compile and call AddCompletedShader
		};
		std::pair<ClaimResult, ID3DBlob*> ClaimCompilation(const std::string& key, std::optional<uint64_t> a_taskGeneration = std::nullopt);
		void ResolvePendingFailure(const std::string& key);

		ID3DBlob* GetCompletedShader(const std::string& a_key);
		ID3DBlob* GetCompletedShader(const SIE::ShaderCompilationTask& a_task);
		ID3DBlob* GetCompletedShader(ShaderClass shaderClass, const RE::BSShader& shader, uint32_t descriptor);
		bool IsShaderLoadedFromDisk(const std::string& a_key);
		ShaderCompilationTask::Status GetShaderStatus(const std::string& a_key);
		/** @brief True if a_key has no shaderMap entry at all, distinct from Pending/
		 *  Completed/Failed. A task whose key vanished between publish and this call was
		 *  evicted mid-flight (see ApplyDeferredEviction), not merely failed. */
		bool IsShaderKeyAbsent(const std::string& a_key);
		std::string GetShaderStatsString(bool a_timeOnly = false, bool a_elapsedOnly = false);

		RE::BSGraphics::VertexShader* GetVertexShader(const RE::BSShader& shader, uint32_t descriptor);
		RE::BSGraphics::PixelShader* GetPixelShader(const RE::BSShader& shader,
			uint32_t descriptor);
		RE::BSGraphics::ComputeShader* GetComputeShader(const RE::BSShader& shader,
			uint32_t descriptor);

		/// Callback fired once a standalone compute shader finishes compiling or
		/// loads from disk. Runs on a compilation-pool worker thread; the pointer
		/// is null on failure (never throws). The caller owns thread-safe storage.
		/// A non-null pointer is one owned reference: the callback must either
		/// take ownership (e.g. winrt::com_ptr::attach) or Release() it.
		using ComputeShaderReadyCallback = std::function<void(ID3D11ComputeShader*)>;

		/// @brief Compile (or load from the disk cache) a standalone compute shader
		///        on the shared compilation pool, off the calling thread.
		/// @param sourcePath  HLSL source path under Data/Shaders (e.g.
		///                    Data\\Shaders\\PostProcessing\\DoF\\dof.cs.hlsl).
		/// @param entryPoint  HLSL entry function name.
		/// @param defines     Preprocessor macro name/value pairs; the caller must
		///                    keep each string alive until the callback fires
		///                    (string literals satisfy this).
		/// @param onReady     Invoked exactly once when the shader is ready.
		void EnqueueComputeShaderCompile(
			std::wstring sourcePath,
			std::string entryPoint,
			std::vector<std::pair<const char*, const char*>> defines,
			ComputeShaderReadyCallback onReady);

		/// @brief Deletes a standalone compute-shader feature's own disk-cache
		///        subtree and manifest entries (e.g. L"PostProcessing/DoF"),
		///        without touching any other feature's cache.
		void ClearStandaloneComputeCache(std::wstring_view relativeDir);

		RE::BSGraphics::VertexShader* MakeAndAddVertexShader(const RE::BSShader& shader,
			uint32_t descriptor, std::optional<uint64_t> a_taskGeneration = std::nullopt);
		RE::BSGraphics::PixelShader* MakeAndAddPixelShader(const RE::BSShader& shader,
			uint32_t descriptor, std::optional<uint64_t> a_taskGeneration = std::nullopt);
		RE::BSGraphics::ComputeShader* MakeAndAddComputeShader(const RE::BSShader& shader,
			uint32_t descriptor, std::optional<uint64_t> a_taskGeneration = std::nullopt);

		static std::string GetDefinesString(const RE::BSShader& shader, uint32_t descriptor);

		uint64_t GetCachedHitTasks();
		uint64_t GetCompletedTasks();
		uint64_t GetFailedTasks();
		/**
		 * @brief Count currently failed shader entries in the shader map.
		 *
		 * This inspects the `shaderMap` under lock and returns the number of
		 * entries whose status is `ShaderCompilationTask::Status::Failed`.
		 */
		uint64_t GetCurrentFailedCount();
		/// One shader compile failure: `key` is GetShaderString's cache key (shader class +
		/// merged feature defines, plus a filename that is fxpFilename even for ImageSpace
		/// shaders), identifying the technique/permutation without a separate descriptor
		/// lookup; `path` is the actual source file compiled (originalShaderName for
		/// ImageSpace shaders), which can differ from the filename embedded in `key`.
		struct CompileFailure
		{
			std::string key;
			std::string path;
			std::string error;
			uint64_t epoch = 0;
			uint32_t frame = 0;
		};
		/// Records a compile failure (bounded ring, newest last). Thread-safe; called from
		/// whichever thread ran the failed compile.
		void RecordCompileFailure(std::string a_key, std::string a_path, std::string a_error);
		/// Most recent compile failures, oldest first. Returned by value: devbench's listener
		/// thread reads this while compiles run concurrently on other threads.
		std::vector<CompileFailure> GetRecentCompileFailures() const
		{
			std::lock_guard lock{ compileFailuresMutex };
			return { recentCompileFailures.begin(), recentCompileFailures.end() };
		}
		uint64_t GetTotalTasks();
		uint64_t GetDiskHitTasks();
		uint64_t GetDigestComputeCount();
		int64_t GetDigestComputeTimeUs();
		uint64_t GetDigestHitTasks();
		uint64_t GetDigestMissTasks();
		void IncCacheHitTasks();
		/** @brief Forwards to CompilationSet::MarkPhaseStarted(); call right before a real compile begins. */
		void MarkCompilationPhaseStarted();
		void RecordDigestComputeTime(int64_t a_elapsedUs);
		void IncDigestHitTasks();
		void IncDigestMissTasks();
		void ToggleErrorMessages();
		void DisableShaderBlocking();
		void IterateShaderBlock(bool a_forward = true);
		bool IsHideErrors();

		// Overlay stats
		int GetHeavyTasksInFlight();
		uint64_t GetSlowTasks();
		uint64_t GetVerySlowTasks();

		/** @brief Returns a copy of the top-N slowest task records from the last build, sorted descending. */
		std::vector<CompilationSet::SlowTaskRecord> GetTopSlowTasks(size_t n = 3);
		/** @brief Returns a copy of every task record collected for the current build. */
		std::vector<CompilationSet::SlowTaskRecord> GetAllTaskRecords();
		/** @brief QPC tick of the last build reset, a generation marker for UI caches. */
		int64_t GetLastResetQpc();
		/** @brief Ticks per second for converting QPC-based timestamps (e.g. SlowTaskRecord::startQpc). */
		int64_t GetQpcFrequency();
		std::optional<CompilationSet::ParallelismStats> GetParallelismStats();

		/**
		 * @brief Writes every collected task record for the current build to a_path as a
		 * Chrome Trace Event Format JSON array (importable directly by ui.perfetto.dev or
		 * chrome://tracing) for diagnosing whether a slow build is genuine shader compile
		 * cost or external CPU contention during the build window.
		 * @return true on success; false on an empty record set or a file-write failure
		 *  (logged, never throws).
		 */
		bool ExportCompileTrace(const std::filesystem::path& a_path);

		/**
		 * @brief Clears all shaders of a specific type from the shader map.
		 *
		 * This function removes all shaders of the specified type (`RE::BSShader::Type`) from the shader map.
		 *
		 * @param a_type The shader type (e.g., Grass, Sky, Water) to be cleared from the map.
		 */
		void ClearShaderMap(RE::BSShader::Type a_type);
		void InsertModifiedShaderMap(const std::string& a_shader, std::chrono::time_point<std::chrono::system_clock> a_time);
		std::chrono::time_point<std::chrono::system_clock> GetModifiedShaderMapTime(const std::string& a_shader);

		ShaderFileDependencyTracker* GetDependencyTracker() { return dependencyTracker.get(); }

		static constexpr int32_t kLowCoreCompilationThreadThreshold = 8;
		static constexpr int32_t kLowCoreReservedCompilationThreads = 1;
		static constexpr int32_t kDefaultReservedCompilationThreads = 2;

		static int32_t GetDefaultCompilationThreadCount()
		{
			const auto threadCount = static_cast<int32_t>(std::thread::hardware_concurrency());
			const auto reservedThreads = threadCount <= kLowCoreCompilationThreadThreshold ? kLowCoreReservedCompilationThreads : kDefaultReservedCompilationThreads;
			return std::max(threadCount - reservedThreads, 1);
		}

		// Reserve fewer threads on low-core systems to avoid overly slow startup compilation.
		// Management and file watcher run on dedicated jthreads, not pool slots.
		// Background (in-game): half of P-cores only, to avoid starving the render thread.
		int32_t compilationThreadCount = GetDefaultCompilationThreadCount();
		int32_t backgroundCompilationThreadCount = std::max(static_cast<int32_t>(Util::GetPerformanceCoreCount()) / 2, 1);
		BS::thread_pool<> compilationPool{ static_cast<std::size_t>(compilationThreadCount) };
		std::jthread managementJthread;  // dedicated thread for ManageCompilationSet (not in pool)
		// atomic: written from the menu/input thread (boot setting + Skip Compilation hotkey),
		// read on the management/compile and render threads.
		std::atomic<bool> backgroundCompilation = false;
		// atomic: written from the SKSE messaging handler (kDataLoaded),
		// read on the render/UI threads (OverlayRenderer, BackgroundBlur).
		std::atomic<bool> menuLoaded = false;

		enum class LightingShaderTechniques
		{
			None = 0,
			Envmap = 1,
			Glowmap = 2,
			Parallax = 3,
			Facegen = 4,
			FacegenRGBTint = 5,
			Hair = 6,
			ParallaxOcc = 7,
			MTLand = 8,
			LODLand = 9,
			Snow = 10,  // unused
			MultilayerParallax = 11,
			TreeAnim = 12,
			LODObjects = 13,
			MultiIndexSparkle = 14,
			LODObjectHD = 15,
			Eye = 16,
			Cloud = 17,  // unused
			LODLandNoise = 18,
			MTLandLODBlend = 19,
		};

		enum class LightingShaderFlags
		{
			VC = 1 << 0,
			Skinned = 1 << 1,
			ModelSpaceNormals = 1 << 2,
			// flags 3 to 8 are unused by vanilla
			// Community Shaders start
			TruePbr = 1 << 3,
			Deferred = 1 << 4,
			// Community Shaders end
			Specular = 1 << 9,
			SoftLighting = 1 << 10,
			RimLighting = 1 << 11,
			BackLighting = 1 << 12,
			ShadowDir = 1 << 13,
			DefShadow = 1 << 14,
			ProjectedUV = 1 << 15,
			AnisoLighting = 1 << 16,  // Reused for glint with PBR
			AmbientSpecular = 1 << 17,
			WorldMap = 1 << 18,
			BaseObjectIsSnow = 1 << 19,
			DoAlphaTest = 1 << 20,
			Snow = 1 << 21,
			CharacterLight = 1 << 22,
			AdditionalAlphaMask = 1 << 23
		};

		enum class BloodSplatterShaderTechniques
		{
			Splatter = 0,
			Flare = 1,
		};

		enum class DistantTreeShaderTechniques
		{
			DistantTreeBlock = 0,
			Depth = 1,
		};

		enum class DistantTreeShaderFlags
		{
			Deferred = 1 << 8,
			AlphaTest = 1 << 16,
		};

		enum class SkyShaderTechniques
		{
			SunOcclude = 0,
			SunGlare = 1,
			MoonAndStarsMask = 2,
			Stars = 3,
			Clouds = 4,
			CloudsLerp = 5,
			CloudsFade = 6,
			Texture = 7,
			Sky = 8,
		};

		enum class GrassShaderTechniques
		{
			RenderDepthStencil = 7,
			RenderDepth = 8,
		};

		enum class GrassShaderFlags
		{
			AlphaTest = 0x10000,
		};

		enum class ParticleShaderTechniques
		{
			Particles = 0,
			ParticlesGryColor = 1,
			ParticlesGryAlpha = 2,
			ParticlesGryColorAlpha = 3,
			EnvCubeSnow = 4,
			EnvCubeRain = 5,
		};

		enum class WaterShaderTechniques
		{
			Underwater = 8,  // 0x8
			Lod = 9,         // 0x9
			Stencil = 10,    // 0xA
			Simple = 11,     // 0xB
		};

		enum class WaterShaderFlags
		{
			Vc = 1 << 0,                // 0x1
			NormalTexCoord = 1 << 1,    // 0x2
			Reflections = 1 << 2,       // 0x4
			Refractions = 1 << 3,       // 0x8
			Depth = 1 << 4,             // 0x10
			Interior = 1 << 5,          // 0x20
			Wading = 1 << 6,            // 0x40
			VertexAlphaDepth = 1 << 7,  // 0x80
			Cubemap = 1 << 8,           // 0x100
			Flowmap = 1 << 9,           // 0x200
			BlendNormals = 1 << 10,     // 0x400
		};

		enum class EffectShaderFlags
		{
			Vc = 1 << 0,
			TexCoord = 1 << 1,
			TexCoordIndex = 1 << 2,
			Skinned = 1 << 3,
			Normals = 1 << 4,
			BinormalTangent = 1 << 5,
			Texture = 1 << 6,
			IndexedTexture = 1 << 7,
			Falloff = 1 << 8,
			AddBlend = 1 << 10,
			MultBlend = 1 << 11,
			Particles = 1 << 12,
			StripParticles = 1 << 13,
			Blood = 1 << 14,
			Membrane = 1 << 15,
			Lighting = 1 << 16,
			ProjectedUv = 1 << 17,
			Soft = 1 << 18,
			GrayscaleToColor = 1 << 19,
			GrayscaleToAlpha = 1 << 20,
			IgnoreTexAlpha = 1 << 21,
			MultBlendDecal = 1 << 22,
			AlphaTest = 1 << 23,
			SkyObject = 1 << 24,
			MsnSpuSkinned = 1 << 25,
			MotionVectorsNormals = 1 << 26,
			Deferred = 1 << 27
		};

		enum class UtilityShaderFlags : uint64_t
		{
			Vc = 1 << 0,
			Texture = 1 << 1,
			Skinned = 1 << 2,
			Normals = 1 << 3,
			BinormalTangent = 1 << 4,
			AlphaTest = 1 << 7,
			LodLandscape = 1 << 8,
			RenderNormal = 1 << 9,
			RenderNormalFalloff = 1 << 10,
			RenderNormalClamp = 1 << 11,
			RenderNormalClear = 1 << 12,
			RenderDepth = 1 << 13,
			RenderShadowmap = 1 << 14,
			RenderShadowmapClamped = 1 << 15,
			GrayscaleToAlpha = 1 << 15,
			RenderShadowmapPb = 1 << 16,
			AdditionalAlphaMask = 1 << 16,
			DepthWriteDecals = 1 << 17,
			DebugShadowSplit = 1 << 18,
			DebugColor = 1 << 19,
			GrayscaleMask = 1 << 20,
			RenderShadowmask = 1 << 21,
			RenderShadowmaskSpot = 1 << 22,
			RenderShadowmaskPb = 1 << 23,
			RenderShadowmaskDpb = 1 << 24,
			RenderBaseTexture = 1 << 25,
			TreeAnim = 1 << 26,
			LodObject = 1 << 27,
			LocalMapFogOfWar = 1 << 28,
			OpaqueEffect = 1 << 29,
		};

		// Shader blocking data for developer mode
		int blockedKeyIndex = -1;  // index in shaderMap; negative value indicates disabled
		std::string blockedKey = "";
		std::vector<uint32_t> blockedIDs;  // more than one descriptor could be blocked based on shader hash

		// Active shader tracking for developer mode
		struct ActiveShaderInfo
		{
			std::string key;
			RE::BSShader::Type shaderType;
			ShaderClass shaderClass;
			uint32_t descriptor;
			std::wstring diskPath;
			uint32_t drawCalls = 0;
			bool isActive = false;  // Used in current/recent frames
			std::chrono::steady_clock::time_point lastUsed;

			bool operator<(const ActiveShaderInfo& other) const
			{
				return key < other.key;
			}
		};

		ankerl::unordered_dense::map<std::string, ActiveShaderInfo> activeShaders;
		mutable std::mutex activeShadersMutex;

		void TrackActiveShader(ShaderClass shaderClass, const RE::BSShader& shader, uint32_t descriptor);
		void ResetFrameShaderTracking();
		std::vector<ActiveShaderInfo> GetActiveShaders() const;

		/** @brief Bounded scene-capture window for the scoped ("smart") cache clear. */
		enum class ActiveShaderCaptureStage
		{
			Idle,
			FirstWindow,        ///< armed by the click; menu typically still open
			AwaitingMenuClose,  ///< first batch already evicted; waiting to sample occluded passes
			SecondWindow        ///< post-close sample
		};

		/// Frames sampled per capture window. ~1s at 60fps, ~0.67s at 90fps (VR).
		static constexpr uint32_t kActiveShaderCaptureFrames = 60;
		/// Wall-clock ceiling per window; guards a scene rendering at single-digit fps.
		static constexpr std::chrono::milliseconds kActiveShaderCaptureTimeout{ 2000 };

		/** @brief Arms a bounded capture of the shaders drawing the current scene, then
		 *  evicts only those. Two windows are sampled: one immediately, and one after the
		 *  settings menu closes to catch passes occluded by menu chrome. Ignored if a
		 *  capture is already in progress. */
		void BeginActiveShaderCapture();
		/** @brief True while either capture window is sampling. */
		bool IsCapturingActiveShaders() const;
		/** @brief Frames left in the current window; 0 when not sampling. */
		uint32_t GetActiveShaderCaptureFramesRemaining() const;
		/** @brief True once the first batch is evicted and the menu has yet to close. */
		bool IsAwaitingMenuCloseCapture() const;
		/** @brief Advances the capture state machine. Render thread only; call once per
		 *  presented frame. Fires ClearActive() when a window expires. */
		void TickActiveShaderCapture(bool a_menuVisible);
		/** @brief True while dev mode or a capture window requires shader tracking. */
		bool IsTrackingActiveShaders() const;
		/** @brief Evicts every shader in the current capture batch from memory, disk, and
		 *  the compilation set. a_clearFeatures also clears Deferred and every loaded
		 *  feature's shader cache - pass false on a cycle's second window, since that
		 *  work isn't scoped by the capture and doesn't need repeating.
		 *  @return Number of cache entries evicted. */
		size_t ClearActive(bool a_clearFeatures = true);
		/** @brief Result of the most recent ClearActive() call, for UI status display. */
		size_t GetLastScopedClearCount() const { return lastScopedClearCount; }
		/** @brief Elapsed time in milliseconds of the most recent ClearActive() call. */
		double GetLastScopedClearMs() const { return lastScopedClearMs; }

		HANDLE managementThread = nullptr;

	private:
		/** @brief True when a_taskGeneration is set and no longer matches the live
		 *  generation; a Clear() invalidated this task while it was compiling. */
		bool IsTaskStale(std::optional<uint64_t> a_taskGeneration) const
		{
			return a_taskGeneration && *a_taskGeneration != compilationSet.generation.load(std::memory_order_acquire);
		}

		void StartActiveShaderCaptureWindow(ActiveShaderCaptureStage a_stage);

		/** @brief Releases one compiled shader from memory and, unless a_deleteDiskBlob is
		 *  false, deletes its disk blob. Does not touch the compilation set; callers must
		 *  Forget() the task id. */
		void EvictShader(const std::string& a_key, RE::BSShader::Type a_type, uint32_t a_descriptor,
			ShaderClass a_shaderClass, const std::wstring& a_diskPath, bool a_deleteDiskBlob = true);

		std::atomic<uint32_t> activeShaderCaptureFramesRemaining{ 0 };                       // read cross-thread (TrackActiveShader)
		ActiveShaderCaptureStage activeShaderCaptureStage = ActiveShaderCaptureStage::Idle;  // render thread only
		std::chrono::steady_clock::time_point activeShaderCaptureDeadline;                   // render thread only
		bool activeShaderCaptureMenuWasVisible = false;                                      // render thread only
		std::atomic<std::thread::id> activeShaderCaptureThread;                              // read cross-thread (TrackActiveShader)
		ankerl::unordered_dense::map<std::string, ActiveShaderInfo> capturedShaders;         // guarded by activeShadersMutex
		std::unordered_set<std::string> clearedThisCaptureCycle;                             // render thread only; reset per BeginActiveShaderCapture()
		size_t lastScopedClearCount = 0;
		double lastScopedClearMs = 0.0;

		struct hlslRecord
		{
			std::string key;
			RE::BSShader::Type type;
			std::uint32_t descriptor;
			SIE::ShaderClass shaderClass;
			std::wstring diskPath;

			bool operator<(const hlslRecord& other) const
			{
				return key < other.key;
			}
		};
		ShaderCache();
		void ManageCompilationSet(std::stop_token stoken);
		void ProcessCompilationSet(std::stop_token stoken, SIE::ShaderCompilationTask task);
		bool BackupActiveDiskCache();
		void DeleteActiveDiskCache();
		void RefreshPreviousDiskCacheInfo();

		~ShaderCache();

		template <typename ShaderType>
		using ShaderMapArray = std::array<
			ankerl::unordered_dense::map<uint32_t, std::unique_ptr<ShaderType>>,
			RE::BSShader::Type::Total>;

		ShaderMapArray<RE::BSGraphics::VertexShader> vertexShaders;
		ShaderMapArray<RE::BSGraphics::PixelShader> pixelShaders;
		ShaderMapArray<RE::BSGraphics::ComputeShader> computeShaders;

		bool isEnabled = true;
		bool isDiskCache = true;
		bool diskCacheHeld = false;
		bool featureSetChanged = false;
		bool featureSetRevertPending = false;
		bool featureSetCacheBackedUp = false;
		bool previousDiskCacheAvailable = false;
		// Guards cacheMismatches/previousCacheMismatches: reassigned/cleared on the main
		// thread (ValidateDiskCache, rollback actions), read from devbench's listener
		// thread via GetCacheMismatches/GetPreviousCacheMismatches.
		mutable std::mutex mismatchesMutex;
		std::vector<CacheMismatch> cacheMismatches;
		std::vector<CacheMismatch> previousCacheMismatches;
		// Guards recentCompileFailures: appended from whichever thread runs a failed compile,
		// read from devbench's listener thread via GetRecentCompileFailures.
		static constexpr size_t kMaxRecentCompileFailures = 32;
		mutable std::mutex compileFailuresMutex;
		std::deque<CompileFailure> recentCompileFailures;
		std::vector<std::string> heldMismatchDefines;
		bool isSkipUnchangedShaders = true;  ///< when true, recompile a disk-cached shader only if its source is newer
		bool isAsync = true;
		bool isDump = false;
		bool hideError = false;
		bool useFileWatcher = false;
		// Set by RequestClear() (file-watcher thread), drained by ProcessPendingClear() (render thread).
		std::atomic<bool> pendingClear{ false };

		std::stop_source ssource;
		std::mutex vertexShadersMutex;
		std::mutex pixelShadersMutex;
		std::mutex computeShadersMutex;
		CompilationSet compilationSet;
		ankerl::unordered_dense::map<std::string, ShaderCacheResult> shaderMap{};
		std::mutex mapMutex;                                                                      // guard for shaderMap
		std::condition_variable mapCV;                                                            // signalled when a Pending entry transitions to Completed/Failed
		ankerl::unordered_dense::map<std::string, system_clock::time_point> modifiedShaderMap{};  // hashmap when a shader source file last modified
		std::mutex modifiedMapMutex;                                                              // guard for modifiedShaderMap
		ankerl::unordered_dense::map<std::string, std::set<hlslRecord>> hlslToShaderMap{};        // hashmap linking specific hlsl files to shader keys in shaderMap
		std::mutex hlslMapMutex;                                                                  // guard for hlslToShaderMap

		// Evictions parked while their key was Pending; guarded by mapMutex.
		// deferredEvictionCount is a lock-free fast path for the common empty case.
		ankerl::unordered_dense::map<std::string, hlslRecord> deferredEvictions;
		std::atomic<size_t> deferredEvictionCount{ 0 };

		/** @brief Parks a_record's eviction if its key is Pending, returning true;
		 *  otherwise returns false and the caller should EvictShader it immediately. */
		bool TryDeferEviction(const hlslRecord& a_record);
		/** @brief Applies a parked eviction for a_key, if any, returning true if it did.
		 *  Must be called with no ShaderCache mutex held: it calls EvictShader, which
		 *  takes compilationMutex. */
		bool ApplyDeferredEviction(const std::string& a_key);

		// efsw file watcher
		efsw::FileWatcher* fileWatcher = nullptr;
		efsw::WatchID watchID;
		UpdateListener* listener = nullptr;

		std::unique_ptr<ShaderFileDependencyTracker> dependencyTracker;
	};

	// Inherits from the abstract listener class, and implements the the file action handler
	class UpdateListener : public efsw::FileWatchListener
	{
	public:
		UpdateListener(ShaderFileDependencyTracker* deps);
		/**
		 * @brief Updates the shader cache for a specific file path and determines whether to clear the cache.
		 *
		 * This function checks if the given file exists and is a shader file (with the ".hlsl" extension).
		 * It then updates the cache with the modified time for the shader file and marks shaders for recompilation
		 * based on the given path. If a specific shader is not found in the cache, it may trigger a cache clear.
		 *
		 * @param filePath The path of the shader file to update.
		 * @param cache Reference to the shader cache to update.
		 * @param clearCache A boolean flag indicating whether the entire cache should be cleared.
		 * @param fileDone A boolean flag that signals whether the update process is done for the current file.
		 *
		 * @note The function only processes files with an ".hlsl" extension and ignores directories.
		 * It assumes case-insensitive handling for shader types and extensions.
		 *
		 * @return Void. Updates internal state and modifies `clearCache` and `fileDone` by reference.
		 */
		void UpdateCache(const std::filesystem::path& filePath, SIE::ShaderCache* cache, bool& clearCache, bool& retFlag);
		void processQueue();
		void handleFileAction(efsw::WatchID, const std::string& dir, const std::string& filename, efsw::Action action, std::string) override;

		std::jthread fileWatcherThread;  // dedicated thread for processQueue (not in pool)

	private:
		ShaderFileDependencyTracker* deps;
		struct fileAction
		{
			efsw::WatchID watchID;
			std::string dir;
			std::string filename;
			efsw::Action action;
			std::string oldFilename;
		};
		std::mutex actionMutex;
		std::vector<fileAction> queue{};
	};
}
