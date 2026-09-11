#include "ProfilingRenderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <imgui.h>
#include <string_view>
#include <unordered_map>

#include "Globals.h"
#include "GpuPass.h"
#include "I18n/I18n.h"
#include "Menu.h"
#include "State.h"
#include "Utils/UI.h"

static constexpr uint32_t kDisplayedRollingFrameCount = 60;
static constexpr float kMaxDisplayTimingSampleMs = 1000.0f;
static constexpr float kGraphHeadroomScale = 1.2f;
static constexpr float kMainGraphHeight = 180.0f;
static constexpr float kFeatureGraphHeight = 100.0f;
static constexpr float kFeatureOverviewGraphHeight = 85.0f;
static constexpr float kFeatureOverviewNameColumnWidth = 150.0f;
static constexpr float kMainGraphMinFrameTimeSec = 0.0001f;
static constexpr float kFeatureGraphMinFrameTimeSec = 0.00001f;
static constexpr float kTimingTableMetricColumnWidth = 55.0f;
static constexpr float kTimingTablePercentColumnWidth = 45.0f;
static constexpr float kStatsRefreshSeconds = 1.0f;
static constexpr float kFeatureDisclosureChevronSpeed = 10.0f;
static constexpr float kFeatureTimingHeightTolerance = 1.0f;
static std::unordered_map<std::string, bool> g_featureProfilingDisclosuresOpen;
static std::unordered_map<std::string, float> g_featureProfilingChevronProgress;
static std::string g_currentFeatureProfilingPage;

static ImU32 HslToImU32(float h, float s, float l)
{
	auto hue2rgb = [](float p, float q, float t) -> float {
		if (t < 0.0f)
			t += 1.0f;
		if (t > 1.0f)
			t -= 1.0f;
		if (t < 1.0f / 6.0f)
			return p + (q - p) * 6.0f * t;
		if (t < 0.5f)
			return q;
		if (t < 2.0f / 3.0f)
			return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
		return p;
	};

	float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
	float p = 2.0f * l - q;
	float r = hue2rgb(p, q, h + 1.0f / 3.0f);
	float g = hue2rgb(p, q, h);
	float b = hue2rgb(p, q, h - 1.0f / 3.0f);

	return IM_COL32(
		static_cast<uint8_t>(r * 255.0f),
		static_cast<uint8_t>(g * 255.0f),
		static_cast<uint8_t>(b * 255.0f),
		255);
}

// Guards rolling stats against disjoint-frame spikes and non-finite samples,
// mirroring Profiler.cpp's own IsValidProfilerSample check on the source data.
static bool IsDisplayTimingSampleValid(float value)
{
	return std::isfinite(value) && value >= 0.0f && value <= kMaxDisplayTimingSampleMs;
}

static int ScaleToUiInt(float value)
{
	return std::max(1, static_cast<int>(std::round(value * Util::GetUIScale())));
}

// FNV-1a + a MurmurHash3-style finalizer: a deterministic per-name color that
// stays stable across sessions and discovery order, unlike an insertion-order
// counter (which reshuffles colors whenever a new pass is first seen).
static uint32_t FinalizeHash(uint32_t hash)
{
	hash ^= hash >> 16;
	hash *= 0x7feb352du;
	hash ^= hash >> 15;
	hash *= 0x846ca68bu;
	hash ^= hash >> 16;
	return hash;
}

static uint32_t StableHash(std::string_view value)
{
	uint32_t hash = 2166136261u;
	for (const unsigned char c : value) {
		hash ^= c;
		hash *= 16777619u;
	}
	return FinalizeHash(hash);
}

static uint32_t MixHash(uint32_t hash, uint32_t salt)
{
	hash ^= salt + 0x9e3779b9u + (hash << 6) + (hash >> 2);
	return FinalizeHash(hash);
}

static float HashToUnitFloat(uint32_t hash)
{
	return static_cast<float>(static_cast<double>(hash) / 4294967296.0);
}

static float GetColorMarkerExtraWidth()
{
	return std::ceil(std::max(6.0f, ImGui::GetTextLineHeight() * 0.65f) + ImGui::GetStyle().ItemInnerSpacing.x);
}

// Small colored swatch ties a table row to its matching graph segment.
static void RenderColorMarker(ImU32 color)
{
	const float lineHeight = ImGui::GetTextLineHeight();
	const float markerSize = std::max(6.0f, std::floor(lineHeight * 0.65f));
	const ImVec2 cursor = ImGui::GetCursorScreenPos();
	const float markerY = cursor.y + (lineHeight - markerSize) * 0.5f;
	const ImVec2 markerMin(cursor.x, markerY);
	const ImVec2 markerMax(cursor.x + markerSize, markerY + markerSize);

	auto* drawList = ImGui::GetWindowDrawList();
	drawList->AddRectFilled(markerMin, markerMax, color, 2.0f);
	drawList->AddRect(markerMin, markerMax, ImGui::GetColorU32(ImGuiCol_Border), 2.0f);

	ImGui::Dummy(ImVec2(markerSize, lineHeight));
	ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
}

static void ReplaceAll(std::string& value, std::string_view from, std::string_view to)
{
	if (from.empty())
		return;

	size_t pos = 0;
	while ((pos = value.find(from.data(), pos, from.size())) != std::string::npos) {
		value.replace(pos, from.size(), to.data(), to.size());
		pos += to.size();
	}
}

// Abbreviates common long substrings and truncates so graph legends fit
// their column instead of being clipped mid-word.
static std::string BuildProfilerGraphLabel(std::string_view label)
{
	std::string result(label.data(), label.size());
	ReplaceAll(result, "ScreenSpaceShadows", "SSShadows");
	ReplaceAll(result, "ScreenSpace", "SS");
	ReplaceAll(result, "CommunityShaders", "CS");
	ReplaceAll(result, "SubsurfaceScattering", "SSS");
	ReplaceAll(result, "DynamicResolution", "DynRes");
	ReplaceAll(result, "Visualization", "Viz");
	ReplaceAll(result, "Composite", "Comp");
	ReplaceAll(result, "Dispatch", "Disp");
	ReplaceAll(result, "Foveated", "Fov");
	ReplaceAll(result, "Periphery", "Periph");
	ReplaceAll(result, "Temporal", "Temp");
	ReplaceAll(result, "Dynamic", "Dyn");
	ReplaceAll(result, "Resolution", "Res");
	ReplaceAll(result, "Upscaling", "Upscale");
	ReplaceAll(result, "Render", "Rnd");
	ReplaceAll(result, "Shader", "Shd");
	ReplaceAll(result, "::", ":");

	constexpr size_t kMaxGraphLabelLength = 34;
	if (result.size() > kMaxGraphLabelLength)
		result = result.substr(0, kMaxGraphLabelLength - 2) + "..";

	return result;
}

static int ComputeGraphLegendWidth(int totalWidth, int minGraphWidth, float widthFraction, int minLegendWidth, int maxLegendWidth)
{
	const int reservedGraphWidth = std::min(minGraphWidth, totalWidth);
	const int availableLegendWidth = std::max(0, totalWidth - reservedGraphWidth);
	if (availableLegendWidth <= 0)
		return 0;

	const int desiredLegendWidth = std::clamp(static_cast<int>(totalWidth * widthFraction), minLegendWidth, maxLegendWidth);
	return std::min(desiredLegendWidth, availableLegendWidth);
}

int ProfilingRenderer::ComputeFeatureGraphLegendWidth(const FeatureTimingData& data, int totalWidth)
{
	if (data.entries.empty())
		return 0;

	const float uiScale = Util::GetUIScale();
	constexpr float legendTextScale = 0.74f;
	const float markerAndConnectorWidth = (3.0f + 5.0f + 18.0f + 8.0f + 5.0f) * uiScale;
	const float textColumnWidth = std::max(
		48.0f * uiScale,
		ImGui::CalcTextSize("000.00ms").x * legendTextScale + 5.0f * uiScale);
	float labelWidth = 0.0f;
	for (const auto& entry : data.entries) {
		const auto label = BuildProfilerGraphLabel(entry.label);
		labelWidth = std::max(labelWidth, ImGui::CalcTextSize(label.c_str()).x * legendTextScale);
	}

	const int desiredLegendWidth = static_cast<int>(std::ceil(markerAndConnectorWidth + textColumnWidth + labelWidth + 10.0f * uiScale));
	const int minGraphWidth = ScaleToUiInt(24.0f);
	const int availableLegendWidth = std::max(0, totalWidth - std::min(minGraphWidth, totalWidth));
	return std::min(desiredLegendWidth, availableLegendWidth);
}

static float GetTextColumnWidth(const char* header, const std::vector<std::string>& labels, float extraWidth = 0.0f)
{
	float width = ImGui::CalcTextSize(header).x;
	for (const auto& label : labels)
		width = std::max(width, ImGui::CalcTextSize(label.c_str()).x);

	return std::ceil(width + ImGui::GetStyle().CellPadding.x * 2.0f + extraWidth);
}

ImU32 ProfilingRenderer::GetGroupColor(std::string_view groupName)
{
	const uint32_t hash = StableHash(groupName);
	const float hue = HashToUnitFloat(hash);
	const float saturation = 0.68f + HashToUnitFloat(MixHash(hash, 0xA511E9B3u)) * 0.12f;
	const float lightness = 0.50f + HashToUnitFloat(MixHash(hash, 0x63D83595u)) * 0.10f;
	return HslToImU32(hue, saturation, lightness);
}

uint32_t ProfilingRenderer::ToLegitColor(ImU32 imColor)
{
	uint8_t r = (imColor >> 0) & 0xFF;
	uint8_t g = (imColor >> 8) & 0xFF;
	uint8_t b = (imColor >> 16) & 0xFF;
	return (0xFF << 24) | (b << 16) | (g << 8) | r;
}

ImVec4 ProfilingRenderer::HeatColor(float value, float maxValue)
{
	if (maxValue <= 0.0f)
		return Util::Colors::GetDefault();

	float x = std::clamp(value / maxValue, 0.0f, 1.0f);

	float x2 = x * x;
	float x3 = x2 * x;
	float x4 = x2 * x2;
	float x5 = x3 * x2;

	float r = 0.13572138f + 4.61539260f * x - 42.66032258f * x2 + 132.13108234f * x3 - 152.94239396f * x4 + 59.28637943f * x5;
	float g = 0.09140261f + 2.19418839f * x + 4.84296658f * x2 - 14.18503333f * x3 + 4.27729857f * x4 + 2.82956604f * x5;
	float b = 0.10667330f + 12.64194608f * x - 60.58204836f * x2 + 110.36276771f * x3 - 89.90310912f * x4 + 27.34824973f * x5;

	float alpha = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg).w;

	return ImVec4(std::clamp(r, 0.0f, 1.0f), std::clamp(g, 0.0f, 1.0f), std::clamp(b, 0.0f, 1.0f), alpha);
}

void ProfilingRenderer::TextHeat(const char* fmt, float value, float maxValue)
{
	ImVec4 bg = HeatColor(value, maxValue);
	ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, ImGui::GetColorU32(bg));
	ImGui::TextColored(Util::Colors::GetDefault(), fmt, value);
}

void ProfilingRenderer::RenderTimingModeToggle()
{
	int mode = static_cast<int>(timingMode);

	ImGui::PushID("ProfilingTimingMode");
	ImGui::RadioButton(T("menu.profiling.gpu", "GPU"), &mode, static_cast<int>(TimingMode::GPU));
	ImGui::SameLine();
	ImGui::RadioButton(T("menu.profiling.cpu", "CPU"), &mode, static_cast<int>(TimingMode::CPU));
	ImGui::PopID();

	const auto newMode = static_cast<TimingMode>(mode);
	if (newMode != timingMode) {
		timingMode = newMode;
		timeSinceLastUpdate = kStatsRefreshSeconds;
	}
}

void ProfilingRenderer::SetupTimingTableColumns(float passColumnWidth, bool includePercentColumn)
{
	const float scale = Util::GetUIScale();
	ImGui::TableSetupColumn(T("menu.profiling.pass", "Pass"), ImGuiTableColumnFlags_WidthFixed, passColumnWidth);
	ImGui::TableSetupColumn(T("menu.profiling.avg", "Avg"), ImGuiTableColumnFlags_WidthFixed, kTimingTableMetricColumnWidth * scale);
	ImGui::TableSetupColumn(T("menu.profiling.p95", "P95"), ImGuiTableColumnFlags_WidthFixed, kTimingTableMetricColumnWidth * scale);
	ImGui::TableSetupColumn(T("menu.profiling.p99", "P99"), ImGuiTableColumnFlags_WidthFixed, kTimingTableMetricColumnWidth * scale);
	if (includePercentColumn)
		ImGui::TableSetupColumn(T("menu.profiling.percent", "%"), ImGuiTableColumnFlags_WidthFixed, kTimingTablePercentColumnWidth * scale);
}

// hasGpu/hasCpu, not activeGpu/activeCpu: a pass that doesn't run every frame
// (e.g. Skylighting::ProbeUpdate) would otherwise flicker in and out of the
// view, and the zero-backfilled rolling average already accounts for a
// missed cycle on its own.
static bool HasLiveTimingMode(const Profiler::TimerResult& result, bool cpuMode)
{
	return cpuMode ? result.hasCpu : result.hasGpu;
}

void ProfilingRenderer::RenderGraph()
{
	auto& profiler = (*globals::profiler);
	const auto& results = profiler.GetResults();
	bool cpuMode = (timingMode == TimingMode::CPU);

	if (results.empty())
		return;

	std::vector<legit::ProfilerTask> tasks;

	double accumulated = 0.0;
	for (const auto& result : results) {
		if (!result.valid || !HasLiveTimingMode(result, cpuMode))
			continue;

		float timeMs = cpuMode ? result.cpuTimeMs : result.gpuTimeMs;

		std::string groupName;
		auto pos = result.name.find("::");
		if (pos != std::string::npos)
			groupName = result.name.substr(0, pos);
		else
			groupName = result.name;

		legit::ProfilerTask task;
		task.startTime = accumulated / 1000.0;
		task.endTime = (accumulated + timeMs) / 1000.0;
		task.name = result.name;
		task.displayName = BuildProfilerGraphLabel(result.name);
		task.color = ToLegitColor(GetGroupColor(groupName));
		tasks.push_back(task);
		accumulated += timeMs;
	}

	if (tasks.empty())
		return;

	gpuGraph.LoadFrameData(tasks.data(), tasks.size());

	float maxFrameTimeSec = gpuGraph.GetPeakFrameTime() * kGraphHeadroomScale;
	if (maxFrameTimeSec < kMainGraphMinFrameTimeSec)
		maxFrameTimeSec = kMainGraphMinFrameTimeSec;

	const float uiScale = Util::GetUIScale();
	const int totalWidth = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x));
	const int legendWidth = ComputeGraphLegendWidth(totalWidth, ScaleToUiInt(100.0f), 0.42f, ScaleToUiInt(280.0f), ScaleToUiInt(420.0f));
	const int graphWidth = std::max(1, totalWidth - legendWidth);
	const float graphHeight = kMainGraphHeight * uiScale;

	gpuGraph.RenderTimings(static_cast<float>(graphWidth), static_cast<float>(legendWidth), graphHeight, 0, maxFrameTimeSec, uiScale);

	ImGui::Spacing();
}

void ProfilingRenderer::RenderStatistics(bool showTable, bool showModeToggle)
{
	auto& profiler = (*globals::profiler);
	// Being drawn at all means something wants fresh data this frame.
	profiler.RequestCapture();

	// Only the full profiling page offers the toggle; the overlay-embedded
	// row (showModeToggle=false) just shows the off-state below rather than
	// silently re-enabling what the user turned off.
	if (showModeToggle) {
		bool enabled = profiler.IsUserEnabled();
		if (ImGui::Checkbox(T("menu.profiling.enable_profiling", "Enable Profiling"), &enabled))
			profiler.SetUserEnabled(enabled);
	}

	if (!profiler.IsUserEnabled()) {
		ImGui::TextDisabled("%s", T("menu.profiling.disabled", "Profiling is disabled"));
		return;
	}

	bool cpuMode = (timingMode == TimingMode::CPU);
	if (showModeToggle) {
		RenderTimingModeToggle();
		cpuMode = (timingMode == TimingMode::CPU);
		ImGui::Separator();
	}

	float currentTime = static_cast<float>(ImGui::GetTime());
	float deltaTime = currentTime - lastFrameTime;
	lastFrameTime = currentTime;
	timeSinceLastUpdate += deltaTime;

	if (timeSinceLastUpdate >= kStatsRefreshSeconds) {
		timeSinceLastUpdate = 0.0f;

		cachedGroups.clear();
		cachedTotalAvgMs = 0.0f;
		cachedTotalP95Ms = 0.0f;
		cachedTotalP99Ms = 0.0f;
		cachedMaxAvgMs = 0.0f;
		cachedMaxP95Ms = 0.0f;
		cachedMaxP99Ms = 0.0f;
		std::unordered_map<std::string, size_t> groupIndex;

		for (const auto& result : profiler.GetResults()) {
			if (!result.valid || !HasLiveTimingMode(result, cpuMode))
				continue;

			float avg = cpuMode ? result.cpuAvgMs : result.avgMs;
			float p95 = cpuMode ? result.cpuP95Ms : result.p95Ms;
			float p99 = cpuMode ? result.cpuP99Ms : result.p99Ms;

			cachedTotalAvgMs += avg;
			cachedTotalP95Ms += p95;
			cachedTotalP99Ms += p99;

			auto pos = result.name.find("::");
			if (pos != std::string::npos) {
				std::string groupName = result.name.substr(0, pos);
				std::string passLabel = result.name.substr(pos + 2);

				auto it = groupIndex.find(groupName);
				if (it == groupIndex.end()) {
					groupIndex[groupName] = cachedGroups.size();
					cachedGroups.push_back({ groupName, 0, 0, 0 });
				}

				auto& group = cachedGroups[groupIndex[groupName]];
				group.totalAvgMs += avg;
				group.totalP95Ms += p95;
				group.totalP99Ms += p99;
				group.passes.push_back({ passLabel, avg, p95, p99 });
			} else {
				groupIndex[result.name] = cachedGroups.size();
				cachedGroups.push_back({ result.name, avg, p95, p99 });
			}
		}

		for (const auto& group : cachedGroups) {
			cachedMaxAvgMs = std::max(cachedMaxAvgMs, group.totalAvgMs);
			cachedMaxP95Ms = std::max(cachedMaxP95Ms, group.totalP95Ms);
			cachedMaxP99Ms = std::max(cachedMaxP99Ms, group.totalP99Ms);
		}
	}

	const bool renderedFeatureOverview = showModeToggle && RenderFeatureOverview();
	if (cachedGroups.empty()) {
		if (!renderedFeatureOverview)
			ImGui::TextDisabled("%s", T("menu.profiling.no_timing_data_world", "No timing data available (enter game world)"));
		return;
	}

	if (renderedFeatureOverview)
		ImGui::SeparatorText(T("menu.profiling.all_timings", "All Timings"));

	RenderGraph();

	if (showTable) {
		std::vector<std::string> passLabels;
		passLabels.reserve(cachedGroups.size());
		for (const auto& group : cachedGroups) {
			passLabels.push_back(group.name);
			for (const auto& pass : group.passes)
				passLabels.push_back(pass.label);
		}
		const float passColumnWidth = GetTextColumnWidth(T("menu.profiling.pass", "Pass"), passLabels,
			GetColorMarkerExtraWidth() + ImGui::GetTreeNodeToLabelSpacing() + ImGui::GetStyle().IndentSpacing);

		if (ImGui::BeginTable("##Profiler", 5,
				ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX)) {
			SetupTimingTableColumns(passColumnWidth, true);
			ImGui::TableHeadersRow();

			for (const auto& group : cachedGroups) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();

				const ImU32 groupColor = GetGroupColor(group.name);
				if (group.passes.empty()) {
					RenderColorMarker(groupColor);
					ImGui::TreeNodeEx(group.name.c_str(), ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen);
					ImGui::TableNextColumn();
					TextHeat("%.3f", group.totalAvgMs, cachedMaxAvgMs);
					ImGui::TableNextColumn();
					TextHeat("%.3f", group.totalP95Ms, cachedMaxP95Ms);
					ImGui::TableNextColumn();
					TextHeat("%.3f", group.totalP99Ms, cachedMaxP99Ms);
					ImGui::TableNextColumn();
					if (cachedTotalAvgMs > 0.0f)
						TextHeat("%5.1f", (group.totalAvgMs / cachedTotalAvgMs) * 100.0f, 100.0f);
				} else {
					RenderColorMarker(groupColor);
					bool open = ImGui::TreeNodeEx(group.name.c_str(), 0);
					ImGui::TableNextColumn();
					TextHeat("%.3f", group.totalAvgMs, cachedMaxAvgMs);
					ImGui::TableNextColumn();
					TextHeat("%.3f", group.totalP95Ms, cachedMaxP95Ms);
					ImGui::TableNextColumn();
					TextHeat("%.3f", group.totalP99Ms, cachedMaxP99Ms);
					ImGui::TableNextColumn();
					if (cachedTotalAvgMs > 0.0f)
						TextHeat("%5.1f", (group.totalAvgMs / cachedTotalAvgMs) * 100.0f, 100.0f);
					if (open) {
						for (const auto& pass : group.passes) {
							ImGui::TableNextRow();
							ImGui::TableNextColumn();
							RenderColorMarker(groupColor);
							ImGui::TreeNodeEx(pass.label.c_str(), ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen);
							ImGui::TableNextColumn();
							TextHeat("%.3f", pass.avgMs, cachedMaxAvgMs);
							ImGui::TableNextColumn();
							TextHeat("%.3f", pass.p95Ms, cachedMaxP95Ms);
							ImGui::TableNextColumn();
							TextHeat("%.3f", pass.p99Ms, cachedMaxP99Ms);
							ImGui::TableNextColumn();
							if (cachedTotalAvgMs > 0.0f)
								TextHeat("%5.1f", (pass.avgMs / cachedTotalAvgMs) * 100.0f, 100.0f);
						}
						ImGui::TreePop();
					}
				}
			}
			ImGui::EndTable();
		}
	}
}

// Rebuilds a TimerResult's rolling history into a compact, oldest-first
// sample array so a Total row can sum actual per-frame samples instead of
// summing each pass's own percentile (percentile(A)+percentile(B) overstates
// percentile(A+B)).
static uint32_t CollectDisplayTimingSamples(const Profiler::TimerResult& result, bool cpuMode, std::array<float, kDisplayedRollingFrameCount>& samples)
{
	samples.fill(0.0f);

	const uint32_t historyCount = cpuMode ? result.cpuHistoryCount : result.historyCount;
	if (historyCount == 0)
		return 0;

	std::array<float, kDisplayedRollingFrameCount> collectedSamples{};
	uint32_t sampleCount = 0;
	for (uint32_t offset = 0; offset < historyCount && sampleCount < kDisplayedRollingFrameCount; ++offset) {
		const uint32_t historyIndex = historyCount - 1 - offset;
		const float sample = cpuMode ?
		                         result.GetCpuHistorySample(historyIndex) :
		                         result.GetHistorySample(historyIndex);
		if (!IsDisplayTimingSampleValid(sample))
			continue;

		collectedSamples[kDisplayedRollingFrameCount - 1 - sampleCount] = sample;
		sampleCount++;
	}

	const uint32_t sourceOffset = kDisplayedRollingFrameCount - sampleCount;
	for (uint32_t i = 0; i < sampleCount; ++i)
		samples[i] = collectedSamples[sourceOffset + i];

	return sampleCount;
}

static float GetSortedPercentile(const std::array<float, kDisplayedRollingFrameCount>& samples, uint32_t sampleCount, float percentile)
{
	if (sampleCount == 0)
		return 0.0f;

	const float idx = (percentile / 100.0f) * static_cast<float>(sampleCount - 1);
	const uint32_t lo = static_cast<uint32_t>(idx);
	const uint32_t hi = std::min(lo + 1, sampleCount - 1);
	const float frac = idx - static_cast<float>(lo);
	return samples[lo] * (1.0f - frac) + samples[hi] * frac;
}

struct DisplayTimingStats
{
	float avgMs = 0.0f;
	float p95Ms = 0.0f;
	float p99Ms = 0.0f;
};

static DisplayTimingStats ComputeDisplayTimingStats(std::array<float, kDisplayedRollingFrameCount> samples, uint32_t sampleCount)
{
	DisplayTimingStats stats;
	if (sampleCount == 0)
		return stats;

	float sum = 0.0f;
	for (uint32_t i = 0; i < sampleCount; ++i)
		sum += samples[i];

	stats.avgMs = sum / static_cast<float>(sampleCount);
	std::sort(samples.begin(), samples.begin() + sampleCount);
	stats.p95Ms = GetSortedPercentile(samples, sampleCount, 95.0f);
	stats.p99Ms = GetSortedPercentile(samples, sampleCount, 99.0f);
	return stats;
}

// Accumulates several passes' per-frame samples (aligned to the same ring
// position -- relies on Profiler::CollectResults pushing exactly one sample
// per timer per cycle) so a feature's Total row can compute avg/P95/P99 from
// the actual summed-per-frame series, not from summing each pass's own stats.
struct DisplayTimingSampleAccumulator
{
	void Add(const std::array<float, kDisplayedRollingFrameCount>& sourceSamples, uint32_t sourceSampleCount)
	{
		if (sourceSampleCount == 0)
			return;

		sampleCount = std::max(sampleCount, sourceSampleCount);
		const uint32_t sampleOffset = kDisplayedRollingFrameCount - sourceSampleCount;
		for (uint32_t i = 0; i < sourceSampleCount; ++i)
			samples[sampleOffset + i] += sourceSamples[i];
	}

	[[nodiscard]] DisplayTimingStats GetStats() const
	{
		if (sampleCount == 0)
			return {};

		std::array<float, kDisplayedRollingFrameCount> compactSamples{};
		const uint32_t sampleOffset = kDisplayedRollingFrameCount - sampleCount;
		for (uint32_t i = 0; i < sampleCount; ++i)
			compactSamples[i] = samples[sampleOffset + i];

		return ComputeDisplayTimingStats(compactSamples, sampleCount);
	}

	std::array<float, kDisplayedRollingFrameCount> samples{};
	uint32_t sampleCount = 0;
};

ProfilingRenderer::FeatureTimingData ProfilingRenderer::CollectFeatureTimingData(const std::string& featurePrefix, bool cpuMode)
{
	FeatureTimingData data;
	DisplayTimingSampleAccumulator totalSamples;
	const auto prefix = GetFeatureTimerPrefix(featurePrefix);
	for (const auto& r : globals::profiler->GetResults()) {
		if (!IsFeatureTimerResult(r, prefix) || !HasLiveTimingMode(r, cpuMode))
			continue;

		std::array<float, kDisplayedRollingFrameCount> samples{};
		const uint32_t sampleCount = CollectDisplayTimingSamples(r, cpuMode, samples);
		if (sampleCount == 0)
			continue;

		const auto stats = ComputeDisplayTimingStats(samples, sampleCount);
		std::string label = r.name.substr(prefix.size());
		float timeMs = cpuMode ? r.cpuTimeMs : r.gpuTimeMs;
		data.entries.push_back({ label, r.name, timeMs, stats.avgMs, stats.p95Ms, stats.p99Ms });
		data.maxAvg = std::max(data.maxAvg, stats.avgMs);
		data.maxP95 = std::max(data.maxP95, stats.p95Ms);
		data.maxP99 = std::max(data.maxP99, stats.p99Ms);
		totalSamples.Add(samples, sampleCount);
	}

	const auto totalStats = totalSamples.GetStats();
	data.totalAvg = totalStats.avgMs;
	data.totalP95 = totalStats.p95Ms;
	data.totalP99 = totalStats.p99Ms;
	data.maxAvg = std::max(data.maxAvg, data.totalAvg);
	data.maxP95 = std::max(data.maxP95, data.totalP95);
	data.maxP99 = std::max(data.maxP99, data.totalP99);

	return data;
}

float ProfilingRenderer::GetFeatureTimingDataHeight(const FeatureTimingData& data)
{
	if (data.entries.empty())
		return ImGui::GetTextLineHeightWithSpacing();

	const auto& style = ImGui::GetStyle();
	const float rowHeight = ImGui::GetTextLineHeight() + style.CellPadding.y * 2.0f;
	const float tableHeight = rowHeight * static_cast<float>(data.entries.size() + 2);
	return kFeatureGraphHeight * Util::GetUIScale() + tableHeight + style.ItemSpacing.y * 3.0f;
}

bool ProfilingRenderer::RenderFeatureTimingGraph(const FeatureTimingData& data, ImGuiUtils::ProfilerGraph& graph, float graphHeight)
{
	if (data.entries.empty())
		return false;

	std::vector<legit::ProfilerTask> tasks;
	double accumulated = 0.0;
	for (const auto& e : data.entries) {
		legit::ProfilerTask task;
		task.startTime = accumulated / 1000.0;
		task.endTime = (accumulated + e.timeMs) / 1000.0;
		task.name = e.label;
		task.displayName = BuildProfilerGraphLabel(e.label);
		task.color = ToLegitColor(GetGroupColor(e.colorKey));
		tasks.push_back(task);
		accumulated += e.timeMs;
	}
	if (tasks.empty())
		return false;

	graph.LoadFrameData(tasks.data(), tasks.size());

	float maxFrameTimeSec = graph.GetPeakFrameTime() * kGraphHeadroomScale;
	if (maxFrameTimeSec < kFeatureGraphMinFrameTimeSec)
		maxFrameTimeSec = kFeatureGraphMinFrameTimeSec;

	const float uiScale = Util::GetUIScale();
	const int totalWidth = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x));
	const int legendWidth = ComputeFeatureGraphLegendWidth(data, totalWidth);
	const int graphWidth = std::max(1, totalWidth - legendWidth);
	const float scaledGraphHeight = graphHeight * uiScale;

	graph.RenderTimings(static_cast<float>(graphWidth), static_cast<float>(legendWidth), scaledGraphHeight, 0, maxFrameTimeSec, uiScale);
	return true;
}

void ProfilingRenderer::RenderFeatureTimingData(const std::string& featurePrefix, FeatureTimingMode featureMode, const FeatureTimingData& data)
{
	bool cpuMode = featureMode == FeatureTimingMode::CPU;

	if (data.entries.empty()) {
		ImGui::TextDisabled("%s", T("menu.profiling.no_timing_data", "No timing data"));
		return;
	}

	auto& state = featureGraphs[featurePrefix];
	if (RenderFeatureTimingGraph(data, cpuMode ? state.cpuGraph : state.gpuGraph, kFeatureGraphHeight))
		ImGui::Spacing();

	if (ImGui::BeginTable("##FeatureTimers", 4, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
		std::vector<std::string> passLabels;
		passLabels.reserve(data.entries.size() + 1);
		for (const auto& e : data.entries)
			passLabels.push_back(e.label);
		passLabels.emplace_back(T("menu.profiling.total", "Total"));
		SetupTimingTableColumns(GetTextColumnWidth(T("menu.profiling.pass", "Pass"), passLabels, GetColorMarkerExtraWidth()), false);
		ImGui::TableHeadersRow();

		for (const auto& e : data.entries) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			RenderColorMarker(GetGroupColor(e.colorKey));
			ImGui::TextUnformatted(e.label.c_str());
			ImGui::TableNextColumn();
			TextHeat("%.3f", e.avgMs, data.maxAvg);
			ImGui::TableNextColumn();
			TextHeat("%.3f", e.p95Ms, data.maxP95);
			ImGui::TableNextColumn();
			TextHeat("%.3f", e.p99Ms, data.maxP99);
		}

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		const auto totalColor = globals::menu->GetTheme().StatusPalette.InfoColor;
		ImGui::TextColored(totalColor, "%s", T("menu.profiling.total", "Total"));
		ImGui::TableNextColumn();
		ImGui::TextColored(totalColor, "%.3f", data.totalAvg);
		ImGui::TableNextColumn();
		ImGui::TextColored(totalColor, "%.3f", data.totalP95);
		ImGui::TableNextColumn();
		ImGui::TextColored(totalColor, "%.3f", data.totalP99);

		ImGui::EndTable();
	}
}

bool ProfilingRenderer::RenderFeatureOverview()
{
	std::vector<std::string> activeFeatures;
	for (const auto& [featurePrefix, featureMode] : featureTimingModes) {
		if (featureMode != FeatureTimingMode::Off)
			activeFeatures.push_back(featurePrefix);
	}
	if (activeFeatures.empty())
		return false;

	std::sort(activeFeatures.begin(), activeFeatures.end());

	ImGui::SeparatorText(T("menu.profiling.feature_overview", "Feature Profiling Overview"));

	if (ImGui::BeginTable("##FeatureProfilingOverview", 3,
			ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_RowBg)) {
		ImGui::TableSetupColumn(T("menu.profiling.feature", "Feature"), ImGuiTableColumnFlags_WidthFixed, GetTextColumnWidth(T("menu.profiling.feature", "Feature"), activeFeatures));
		ImGui::TableSetupColumn(T("menu.profiling.gpu", "GPU"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableSetupColumn(T("menu.profiling.cpu", "CPU"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableHeadersRow();

		for (const auto& featurePrefix : activeFeatures) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(featurePrefix.c_str());

			const auto gpuData = CollectFeatureTimingData(featurePrefix, false);
			const auto cpuData = CollectFeatureTimingData(featurePrefix, true);
			// Distinct key from RenderFeatureTimingData's: the two views are
			// mutually exclusive today (a single-page menu), but sharing one
			// ProfilerGraph would silently double-advance it if that changes.
			auto& state = featureGraphs[featurePrefix + "::overview"];

			ImGui::TableNextColumn();
			ImGui::PushID((featurePrefix + "::GPU").c_str());
			if (!RenderFeatureTimingGraph(gpuData, state.gpuGraph, kFeatureOverviewGraphHeight))
				ImGui::TextDisabled("%s", T("menu.profiling.no_gpu_timing_data", "No GPU timing data"));
			ImGui::PopID();

			ImGui::TableNextColumn();
			ImGui::PushID((featurePrefix + "::CPU").c_str());
			if (!RenderFeatureTimingGraph(cpuData, state.cpuGraph, kFeatureOverviewGraphHeight))
				ImGui::TextDisabled("%s", T("menu.profiling.no_cpu_timing_data", "No CPU timing data"));
			ImGui::PopID();
		}

		ImGui::EndTable();
	}

	ImGui::Spacing();
	return true;
}

void ProfilingRenderer::RenderFeatureTimingModeControls(const std::string& featurePrefix, FeatureTimingMode& featureMode)
{
	ImGui::PushID(featurePrefix.c_str());
	int mode = static_cast<int>(featureMode);
	ImGui::PushID("FeatureProfilingMode");
	ImGui::RadioButton(T("menu.profiling.cpu", "CPU"), &mode, static_cast<int>(FeatureTimingMode::CPU));
	ImGui::SameLine();
	ImGui::RadioButton(T("menu.profiling.gpu", "GPU"), &mode, static_cast<int>(FeatureTimingMode::GPU));
	ImGui::SameLine();
	ImGui::RadioButton(T("menu.profiling.off", "Off"), &mode, static_cast<int>(FeatureTimingMode::Off));
	ImGui::PopID();

	const auto selectedMode = static_cast<FeatureTimingMode>(mode);
	if (selectedMode != featureMode) {
		featureMode = selectedMode;
		if (featureMode != FeatureTimingMode::Off)
			globals::profiler->SetUserEnabled(true);
	}
	ImGui::PopID();
}

void ProfilingRenderer::RenderFeatureTimingButton(const std::string& featurePrefix)
{
	ImGui::PushID(featurePrefix.c_str());
	auto& disclosureOpen = g_featureProfilingDisclosuresOpen[featurePrefix];
	auto& chevronProgress = g_featureProfilingChevronProgress.try_emplace(featurePrefix, disclosureOpen ? 0.0f : 1.0f).first->second;
	const char* label = T("menu.features.profiling", "Profiling");
	const auto& style = ImGui::GetStyle();
	const float buttonHeight = ImGui::GetFrameHeight();
	const float chevronSize = buttonHeight;
	const float chevronSlotWidth = ImGui::GetTextLineHeight();
	IM_ASSERT(chevronSize > 0.0f && chevronSlotWidth > 0.0f);
	const ImVec2 labelSize = ImGui::CalcTextSize(label);
	const float contentWidth = labelSize.x + style.ItemInnerSpacing.x + chevronSlotWidth;
	const ImVec2 buttonSize(
		std::ceil(contentWidth + style.FramePadding.x * 2.0f),
		buttonHeight);

	const bool pressed = ImGui::Button("##FeatureProfiling", buttonSize);
	const ImVec2 buttonMin = ImGui::GetItemRectMin();
	const ImVec2 buttonMax = ImGui::GetItemRectMax();
	auto* drawList = ImGui::GetWindowDrawList();
	if (pressed)
		disclosureOpen = !disclosureOpen;

	const float targetProgress = disclosureOpen ? 0.0f : 1.0f;
	const float progressStep = kFeatureDisclosureChevronSpeed * ImGui::GetIO().DeltaTime;
	if (chevronProgress < targetProgress)
		chevronProgress = std::min(chevronProgress + progressStep, targetProgress);
	else
		chevronProgress = std::max(chevronProgress - progressStep, targetProgress);

	const float contentX = buttonMin.x + (buttonSize.x - contentWidth) * 0.5f;
	const float textY = buttonMin.y + (buttonHeight - labelSize.y) * 0.5f;
	drawList->AddText(ImVec2(contentX, textY), ImGui::GetColorU32(ImGuiCol_Text), label);
	const float chevronY = buttonMin.y + (buttonHeight - chevronSize) * 0.5f;
	const float chevronCenterX = contentX + labelSize.x + style.ItemInnerSpacing.x + chevronSlotWidth * 0.5f;
	const ImVec2 chevronMin(chevronCenterX - chevronSize * 0.5f, chevronY);
	const ImVec2 chevronMax(chevronMin.x + chevronSize, chevronY + chevronSize);
	Util::DrawDisclosureChevron(drawList, chevronMin, chevronMax, chevronProgress);

	ImGui::PopID();
}

float ProfilingRenderer::PrepareFeatureTimers(const std::string& featurePrefix, bool showProfiling)
{
	if (g_currentFeatureProfilingPage != featurePrefix) {
		DeactivateFeatureTimers();
		g_currentFeatureProfilingPage = featurePrefix;
	}
	if (!showProfiling || globals::profiler == nullptr) {
		featureTimingViews.erase(featurePrefix);
		return 0.0f;
	}

	auto& profiler = (*globals::profiler);
	auto& featureMode = featureTimingModes[featurePrefix];
	auto& view = featureTimingViews[featurePrefix];
	view.controlsVisible = g_featureProfilingDisclosuresOpen[featurePrefix];
	view.mode = featureMode;
	view.profilingDisabled = view.controlsVisible && view.mode != FeatureTimingMode::Off && !profiler.IsUserEnabled();
	view.data = {};

	float timingDataHeight = 0.0f;
	if (view.profilingDisabled) {
		timingDataHeight = ImGui::GetTextLineHeightWithSpacing();
	} else if (view.controlsVisible && view.mode != FeatureTimingMode::Off) {
		profiler.RequestCapture();
		view.data = CollectFeatureTimingData(featurePrefix, view.mode == FeatureTimingMode::CPU);
		timingDataHeight = GetFeatureTimingDataHeight(view.data);
	}

	view.contentHeight = timingDataHeight +
	                     (view.controlsVisible ? ImGui::GetFrameHeightWithSpacing() : 0.0f) +
	                     ImGui::GetFrameHeight();
	return view.contentHeight;
}

void ProfilingRenderer::DeactivateFeatureTimers()
{
	if (g_currentFeatureProfilingPage.empty())
		return;

	const auto modeIt = featureTimingModes.find(g_currentFeatureProfilingPage);
	if (modeIt != featureTimingModes.end() && modeIt->second == FeatureTimingMode::Off) {
		g_featureProfilingDisclosuresOpen[g_currentFeatureProfilingPage] = false;
		g_featureProfilingChevronProgress[g_currentFeatureProfilingPage] = 1.0f;
	}
	featureTimingViews.erase(g_currentFeatureProfilingPage);
	g_currentFeatureProfilingPage.clear();
}

void ProfilingRenderer::RenderFeatureTimers(const std::string& featurePrefix)
{
	auto viewIt = featureTimingViews.find(featurePrefix);
	IM_ASSERT(viewIt != featureTimingViews.end());
	if (viewIt == featureTimingViews.end())
		return;

	const auto& view = viewIt->second;
	[[maybe_unused]] const float contentStartY = ImGui::GetCursorPosY();
	if (view.profilingDisabled)
		ImGui::TextDisabled("%s", T("menu.profiling.disabled", "Profiling is disabled"));
	else if (view.controlsVisible && view.mode != FeatureTimingMode::Off)
		RenderFeatureTimingData(featurePrefix, view.mode, view.data);

	if (view.controlsVisible)
		RenderFeatureTimingModeControls(featurePrefix, featureTimingModes[featurePrefix]);

	RenderFeatureTimingButton(featurePrefix);
	IM_ASSERT(std::abs(ImGui::GetCursorPosY() - contentStartY - ImGui::GetStyle().ItemSpacing.y - view.contentHeight) <= kFeatureTimingHeightTolerance);
}

bool ProfilingRenderer::IsFeatureProfilingAvailable(const std::string& featurePrefix)
{
	if (!globals::profiler)
		return false;
	if (GpuPassCapabilities::Contains(featurePrefix))
		return true;

	const auto prefix = GetFeatureTimerPrefix(featurePrefix);
	const auto& results = globals::profiler->GetResults();
	return std::any_of(results.begin(), results.end(), [&](const auto& result) {
		return IsFeatureTimerResult(result, prefix);
	});
}

std::string ProfilingRenderer::GetFeatureTimerPrefix(const std::string& featurePrefix)
{
	return featurePrefix + "::";
}

bool ProfilingRenderer::IsFeatureTimerResult(const Profiler::TimerResult& result, std::string_view prefix)
{
	return result.valid && result.name.starts_with(prefix);
}
