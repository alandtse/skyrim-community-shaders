#pragma once

#include <Tracy/Tracy.hpp>
#include <Tracy/TracyD3D11.hpp>

#include <optional>
#include <string_view>

#include "Utils/Macros.h"

namespace GpuPassCapabilities
{
	/** @brief Registers the feature prefix owned by a named GPU pass. */
	bool Register(std::string_view passName);

	/** @brief Checks whether an executed GPU pass declared this feature prefix. */
	bool Contains(std::string_view featurePrefix);
}

/// RAII scope that fans a single pass name to all three instrumentation sinks:
///   1. Internal profiler (GPU timestamp + CPU QPC → Profiling table)
///   2. Tracy CPU zone (always-on when TRACY_ENABLE; not gated on frameAnnotations)
///   3. Tracy GPU zone (always-on when TRACY_ENABLE and a D3D11 context exists)
///   4. RenderDoc/PIX ID3DUserDefinedAnnotation (when frameAnnotations is true)
///
/// Use via the convenience macros at render-pass entry points:
///   CS_GPU_PASS("Feature::PassName");                     // literal name, zero allocation
///   CS_GPU_PASS_SELECT(cond, "A", "B");                   // literal ternary, zero allocation
///   CS_GPU_PASS_DYNAMIC(runtimeName);                     // runtime name, allocates
struct ScopedGpuPass
{
	/** @brief Opens a pass using Tracy's dynamic source-location path. name is copied, not retained. */
	explicit ScopedGpuPass(std::string_view name);
#ifdef TRACY_ENABLE
	/** @brief Opens a pass using a caller-supplied static source location (zero allocation). name is copied, not retained. */
	ScopedGpuPass(const tracy::SourceLocationData* srcloc, std::string_view name);
#endif
	~ScopedGpuPass();

	ScopedGpuPass(const ScopedGpuPass&) = delete;
	ScopedGpuPass& operator=(const ScopedGpuPass&) = delete;
	ScopedGpuPass(ScopedGpuPass&&) = delete;
	ScopedGpuPass& operator=(ScopedGpuPass&&) = delete;

private:
#ifdef TRACY_ENABLE
	std::optional<tracy::ScopedZone> cpuZone;
	std::optional<tracy::D3D11ZoneScope> gpuZone;
#endif
	bool annotationOpen = false;
	bool profilerActive = false;
};

// tracy::SourceLocationData only exists when TRACY_ENABLE is defined (same rule
// Tracy's own ZoneNamedN follows) -- fall back to the dynamic-name constructor
// when disabled, since no allocation-avoidance is needed with Tracy compiled out.
#ifdef TRACY_ENABLE
#	define CS_GPU_PASS(name)                                                                                                                              \
		[[maybe_unused]] static const bool CS_DETAIL_CONCAT(cs_gpu_pass_capability_, __LINE__) = GpuPassCapabilities::Register(name);                      \
		static constexpr tracy::SourceLocationData CS_DETAIL_CONCAT(cs_gpu_pass_srcloc_, __LINE__){ name, __FUNCTION__, __FILE__, (uint32_t)__LINE__, 0 }; \
		ScopedGpuPass CS_DETAIL_CONCAT(cs_gpu_pass_, __LINE__) { &CS_DETAIL_CONCAT(cs_gpu_pass_srcloc_, __LINE__), name }

/// Two static srclocs, not one: a shared static would latch onto whichever branch
/// evaluated first and never reflect the other one again.
#	define CS_GPU_PASS_SELECT(cond, name1, name2)                                                                                                           \
		[[maybe_unused]] static const bool CS_DETAIL_CONCAT(cs_gpu_pass_capability1_, __LINE__) = GpuPassCapabilities::Register(name1);                      \
		[[maybe_unused]] static const bool CS_DETAIL_CONCAT(cs_gpu_pass_capability2_, __LINE__) = GpuPassCapabilities::Register(name2);                      \
		static constexpr tracy::SourceLocationData CS_DETAIL_CONCAT(cs_gpu_pass_srcloc1_, __LINE__){ name1, __FUNCTION__, __FILE__, (uint32_t)__LINE__, 0 }; \
		static constexpr tracy::SourceLocationData CS_DETAIL_CONCAT(cs_gpu_pass_srcloc2_, __LINE__){ name2, __FUNCTION__, __FILE__, (uint32_t)__LINE__, 0 }; \
		const bool CS_DETAIL_CONCAT(cs_gpu_pass_cond_, __LINE__) = (cond);                                                                                   \
		ScopedGpuPass CS_DETAIL_CONCAT(cs_gpu_pass_, __LINE__) { CS_DETAIL_CONCAT(cs_gpu_pass_cond_, __LINE__) ? &CS_DETAIL_CONCAT(cs_gpu_pass_srcloc1_, __LINE__) : &CS_DETAIL_CONCAT(cs_gpu_pass_srcloc2_, __LINE__), CS_DETAIL_CONCAT(cs_gpu_pass_cond_, __LINE__) ? std::string_view(name1) : std::string_view(name2) }
#else
#	define CS_GPU_PASS(name)                                                                    \
		[[maybe_unused]] static const bool CS_DETAIL_CONCAT(cs_gpu_pass_capability_, __LINE__) = \
			GpuPassCapabilities::Register(name);                                                 \
		ScopedGpuPass CS_DETAIL_CONCAT(cs_gpu_pass_, __LINE__) { name }

#	define CS_GPU_PASS_SELECT(cond, name1, name2)                                                                                      \
		[[maybe_unused]] static const bool CS_DETAIL_CONCAT(cs_gpu_pass_capability1_, __LINE__) = GpuPassCapabilities::Register(name1); \
		[[maybe_unused]] static const bool CS_DETAIL_CONCAT(cs_gpu_pass_capability2_, __LINE__) = GpuPassCapabilities::Register(name2); \
		const bool CS_DETAIL_CONCAT(cs_gpu_pass_cond_, __LINE__) = (cond);                                                              \
		ScopedGpuPass CS_DETAIL_CONCAT(cs_gpu_pass_, __LINE__) { CS_DETAIL_CONCAT(cs_gpu_pass_cond_, __LINE__) ? std::string_view(name1) : std::string_view(name2) }
#endif

#define CS_GPU_PASS_DYNAMIC(name)                                                     \
	const auto& CS_DETAIL_CONCAT(cs_gpu_pass_name_, __LINE__) = (name);               \
	[[maybe_unused]] const bool CS_DETAIL_CONCAT(cs_gpu_pass_capability_, __LINE__) = \
		GpuPassCapabilities::Register(CS_DETAIL_CONCAT(cs_gpu_pass_name_, __LINE__)); \
	ScopedGpuPass CS_DETAIL_CONCAT(cs_gpu_pass_, __LINE__) { CS_DETAIL_CONCAT(cs_gpu_pass_name_, __LINE__) }
