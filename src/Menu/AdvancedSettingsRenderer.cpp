#include "AdvancedSettingsRenderer.h"

#include <algorithm>
#include <format>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <thread>

#include "FeatureIssues.h"
#include "Features/PerformanceOverlay/ABTesting/ABTesting.h"
#include "Features/RemoteControl.h"
#include "Features/RenderDoc.h"
#include "Fonts.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "Menu.h"
#include "ShaderCache.h"
#include "State.h"
#include "Util.h"
#include "Utils/Format.h"
#include "Utils/Game.h"
#include "Utils/UI.h"

void AdvancedSettingsRenderer::RenderAdvancedSettings(
	const std::function<void()>& drawDisableAtBootSettings)
{
	// Tabs ordered alphabetically; each tab is grouped by purpose, not audience.
	// Shaders   = configure & inspect shader compilation
	// Diagnostics = log/inspect runtime state & block individual shaders
	// Disable at Boot = user-facing failsafe toggles
	// Remote Control = devbench bridge status and exposed tools
	// RenderDoc = frame-capture configuration and capture management
	// Testing   = A/B harness + dev-mode test scaffolding
	if (ImGui::BeginTabBar("##AdvancedSettingsTabs", ImGuiTabBarFlags_None)) {
		if (MenuFonts::BeginTabItemWithFont(T("menu.advanced.tab_diagnostics", "Diagnostics"), Menu::FontRole::Subheading)) {
			if (ImGui::BeginChild("##DiagnosticsContent", ImVec2(0, 0), false)) {
				RenderDiagnosticsSection();
			}
			ImGui::EndChild();
			ImGui::EndTabItem();
		}

		if (MenuFonts::BeginTabItemWithFont(T("menu.advanced.tab_disable_at_boot", "Disable at Boot"), Menu::FontRole::Subheading)) {
			if (ImGui::BeginChild("##DisableAtBootContent", ImVec2(0, 0), false)) {
				RenderDisableAtBootSection(drawDisableAtBootSettings);
			}
			ImGui::EndChild();
			ImGui::EndTabItem();
		}

		if (MenuFonts::BeginTabItemWithFont(T("menu.advanced.tab_remote_control", "Remote Control"), Menu::FontRole::Subheading)) {
			if (ImGui::BeginChild("##RemoteControlContent", ImVec2(0, 0), false)) {
				globals::features::remoteControl.DrawSettings();
			}
			ImGui::EndChild();
			ImGui::EndTabItem();
		}

		if (MenuFonts::BeginTabItemWithFont(T("menu.advanced.tab_renderdoc", "RenderDoc"), Menu::FontRole::Subheading)) {
			if (ImGui::BeginChild("##RenderDocContent", ImVec2(0, 0), false)) {
				globals::features::renderDoc.DrawSettings();
			}
			ImGui::EndChild();
			ImGui::EndTabItem();
		}

		if (MenuFonts::BeginTabItemWithFont(T("menu.advanced.tab_shaders", "Shaders"), Menu::FontRole::Subheading)) {
			if (ImGui::BeginChild("##ShadersContent", ImVec2(0, 0), false)) {
				RenderShadersSection();
			}
			ImGui::EndChild();
			ImGui::EndTabItem();
		}

		if (MenuFonts::BeginTabItemWithFont(T("menu.advanced.tab_testing", "Testing"), Menu::FontRole::Subheading)) {
			if (ImGui::BeginChild("##TestingContent", ImVec2(0, 0), false)) {
				RenderTestingSection();
			}
			ImGui::EndChild();
			ImGui::EndTabItem();
		}

		ImGui::EndTabBar();
	}
}

// -----------------------------------------------------------------------------
// Shaders tab
// -----------------------------------------------------------------------------

void AdvancedSettingsRenderer::RenderShadersSection()
{
	RenderShaderCompileFlags();

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	RenderShaderThreading();

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	RenderShaderCacheControls();

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	RenderShaderReplacementTable();

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	RenderShaderCompileStatistics();
}

void AdvancedSettingsRenderer::RenderShaderCompileFlags()
{
	auto shaderCache = globals::shaderCache;

	Util::DrawSectionHeader(T("menu.advanced.compile_flags_header", "Compile Flags"));

	// Shader Defines input
	auto& shaderDefines = globals::state->shaderDefinesString;
	if (ImGui::InputText(T("menu.advanced.shader_defines", "Shader Defines"), &shaderDefines)) {
		globals::state->SetDefines(shaderDefines);
	}
	if (ImGui::IsItemDeactivatedAfterEdit() || (ImGui::IsItemActive() &&
												   (ImGui::IsKeyPressed(ImGuiKey_Enter) ||
													   ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)))) {
		globals::state->SetDefines(shaderDefines);
		shaderCache->Clear();
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("menu.advanced.shader_defines_tooltip", "Defines for Shader Compiler. Semicolon \";\" separated. Clear with space. Rebuild shaders after making change. Compute Shaders require a restart to recompile."));
	}

	// Half-precision (partial precision) shader compile flag
	bool partialPrecision = globals::state->enablePartialPrecision.load(std::memory_order_relaxed);
	if (ImGui::Checkbox(T("menu.advanced.half_precision", "Half Precision (Partial Precision)"), &partialPrecision)) {
		globals::state->enablePartialPrecision.store(partialPrecision, std::memory_order_relaxed);
		// Force a recompile so the flag actually takes effect on subsequent shader builds.
		shaderCache->Clear();
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("menu.advanced.half_precision_tooltip",
							  "Adds D3DCOMPILE_PARTIAL_PRECISION to the shader compiler flags.\n"
							  "Lets fxc downgrade unmarked float ops to FP16 where it can prove safety, "
							  "on top of the existing min16float type hints.\n"
							  "On FP16-capable GPUs (Pascal+ / GCN+ / Skylake+) this can halve register "
							  "pressure and double ALU throughput, but it can also introduce minor visual "
							  "differences in shaders that haven't been audited for precision sensitivity.\n"
							  "Toggling this clears the shader cache and triggers a full recompile."));
	}

	// Avoid flow control compiler flag (transient — not saved to config because the
	// right setting depends on the current scene, not the user).
	bool avoidFlowControl = globals::state->enableAvoidFlowControl.load(std::memory_order_relaxed);
	if (ImGui::Checkbox(T("menu.advanced.avoid_flow_control", "Avoid Flow Control"), &avoidFlowControl)) {
		globals::state->enableAvoidFlowControl.store(avoidFlowControl, std::memory_order_relaxed);
		// Force a recompile so the flag actually takes effect on subsequent shader builds.
		shaderCache->Clear();
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("menu.advanced.avoid_flow_control_tooltip",
							  "Adds D3DCOMPILE_AVOID_FLOW_CONTROL to the shader compiler flags.\n"
							  "Forces fxc to flatten branches into predicated ops rather than emitting "
							  "dynamic flow control. Often a win for short branch bodies and uniformly-"
							  "taken branches; usually a loss for long divergent branches that vanilla "
							  "flow control would skip entirely.\n"
							  "Resets every launch. Toggling this clears the shader cache and triggers a "
							  "full recompile."));
	}
}

void AdvancedSettingsRenderer::RenderShaderThreading()
{
	auto shaderCache = globals::shaderCache;

	Util::DrawSectionHeader(T("menu.advanced.threading_header", "Threading"));

	// hardware_concurrency() is permitted to return 0 if the implementation can't
	// detect it. Fall back to the actual compile-pool thread count we ended up
	// using at startup (which itself defaults to a sensible value when the OS
	// query fails), then clamp to at least 1 so the slider range (min=1, max=N)
	// stays valid and ImGui doesn't assert.
	const uint32_t hwThreads = std::thread::hardware_concurrency();
	const int32_t poolThreads = static_cast<int32_t>(shaderCache->compilationPool.get_thread_count());
	const int32_t maxThreads = std::max({ 1, poolThreads, static_cast<int32_t>(hwThreads) });

	// Snap the persisted values back into the valid range — a stale config can
	// otherwise leave compilationThreadCount above maxThreads, which would
	// render the slider in an out-of-range state.
	shaderCache->compilationThreadCount = std::clamp(shaderCache->compilationThreadCount, 1, maxThreads);
	shaderCache->backgroundCompilationThreadCount = std::clamp(shaderCache->backgroundCompilationThreadCount, 1, maxThreads);

	ImGui::SliderInt(T("menu.advanced.compiler_threads", "Compiler Threads"), &shaderCache->compilationThreadCount, 1, maxThreads);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("menu.advanced.compiler_threads_tooltip",
							  "Number of threads used to compile shaders at startup. "
							  "Defaults to all logical cores minus one for OS headroom (E-cores included). "
							  "Higher values finish compilation faster but may make the system less responsive."));
	}
	ImGui::SliderInt(T("menu.advanced.background_compiler_threads", "Background Compiler Threads"), &shaderCache->backgroundCompilationThreadCount, 1, maxThreads);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("menu.advanced.background_compiler_threads_tooltip",
							  "Number of threads used to compile shaders during gameplay. "
							  "Defaults to half of performance cores to avoid impacting the render thread. "
							  "Higher values finish compilation faster but may cause stuttering."));
	}

	// Persisted in Menu::Settings (not ShaderCache); applied to backgroundCompilation at boot in Menu::Load.
	auto& menuSettings = globals::menu->GetSettings();
	ImGui::Checkbox("Background Compile on Boot", &menuSettings.BackgroundShaderCompilationOnBoot);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text(
			"Load the menu immediately and compile shaders in the background on boot "
			"(same as the Skip Compilation key). Shaders still compile - only the startup wait "
			"is skipped. Takes effect on the next launch. Default off.");
	}

	if (globals::game::isVR) {
		ImGui::Checkbox(T("menu.advanced.hide_compilation_hud_vr", "Hide Compilation HUD in VR"), &menuSettings.HideCompilationHUDInVR);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T("menu.advanced.hide_compilation_hud_vr_tooltip",
								  "Suppresses the background-compilation progress overlay for immersion. "
								  "The blocking foreground-compile warning still shows, since the game is "
								  "genuinely paused then."));
		}
	}
}

void AdvancedSettingsRenderer::RenderShaderCacheControls()
{
	auto shaderCache = globals::shaderCache;

	Util::DrawSectionHeader(T("menu.advanced.cache_watcher_header", "Cache & File Watcher"));

	// File Watcher option
	bool useFileWatcher = shaderCache->UseFileWatcher();
	if (ImGui::Checkbox(T("menu.advanced.enable_file_watcher", "Enable File Watcher"), &useFileWatcher)) {
		shaderCache->SetFileWatcher(useFileWatcher);
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("menu.advanced.enable_file_watcher_tooltip",
							  "Automatically recompile shaders on file change. "
							  "Intended for development."));
	}

	// Dump Shaders option
	bool useDump = shaderCache->IsDump();
	if (ImGui::Checkbox(T("menu.advanced.dump_shaders", "Dump Shaders"), &useDump)) {
		shaderCache->SetDump(useDump);
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("menu.advanced.dump_shaders_tooltip", "Dump shaders at startup. This should be used only when reversing shaders. Normal users don't need this."));
	}

	// Routed through the shared confirmation/scope path so all three clear-cache entry
	// points share identical confirmation, scope resolution, and disk-cache behavior.
	{
		const bool capturing = shaderCache->IsCapturingActiveShaders();
		const bool awaitingMenuClose = shaderCache->IsAwaitingMenuCloseCapture();
		ImGui::BeginDisabled(capturing || awaitingMenuClose);
		if (ImGui::Button(T("menu.advanced.clear_shader_cache", "Clear Shader Cache"), { -1, 0 })) {
			Util::RequestClearShaderCacheConfirmation(Util::ResolveShaderCacheClearScope());
		}
		ImGui::EndDisabled();
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", Util::GetClearShaderCacheTooltip());
		ImGui::Text("%s", T("menu.clear_shader_cache_modifier_hint", "Shift-click for the other clear mode."));
	}
	if (auto count = shaderCache->GetLastScopedClearCount(); count > 0) {
		auto lastClearMs = shaderCache->GetLastScopedClearMs();
		ImGui::TextDisabled("%s",
			std::vformat(T("menu.advanced.last_smart_clear", "Last smart clear: {} shader(s) ({:.1f} ms)"),
				std::make_format_args(count, lastClearMs))
				.c_str());
	}
}

void AdvancedSettingsRenderer::RenderShaderReplacementTable()
{
	auto state = globals::state;

	Util::DrawSectionHeader(T("menu.advanced.replace_original_shaders", "Replace Original Shaders"));

	if (ImGui::BeginTable("##ReplaceToggles", 3, ImGuiTableFlags_SizingStretchSame)) {
		globals::state->ForEachShaderTypeWithIndex([&](auto type, int classIndex) {
			ImGui::TableNextColumn();

			if (!(SIE::ShaderCache::IsSupportedShader(type) || state->IsDeveloperMode())) {
				ImGui::BeginDisabled();
				ImGui::Checkbox(std::format("{}", magic_enum::enum_name(type)).c_str(), &state->enabledClasses[classIndex]);
				ImGui::EndDisabled();
			} else
				ImGui::Checkbox(std::format("{}", magic_enum::enum_name(type)).c_str(), &state->enabledClasses[classIndex]);
		});
		if (state->IsDeveloperMode()) {
			ImGui::Checkbox(T("menu.advanced.vertex", "Vertex"), &state->enableVShaders);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T("menu.advanced.vertex_tooltip",
									  "Replace Vertex Shaders. "
									  "When false, will disable the custom Vertex Shaders for the types above. "
									  "For developers to test whether custom shaders match vanilla behavior. "));
			}

			ImGui::Checkbox(T("menu.advanced.pixel", "Pixel"), &state->enablePShaders);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T("menu.advanced.pixel_tooltip",
									  "Replace Pixel Shaders. "
									  "When false, will disable the custom Pixel Shaders for the types above. "
									  "For developers to test whether custom shaders match vanilla behavior. "));
			}

			ImGui::Checkbox(T("menu.advanced.compute", "Compute"), &state->enableCShaders);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T("menu.advanced.compute_tooltip",
									  "Replace Compute Shaders. "
									  "When false, will disable the custom Compute Shaders for the types above. "
									  "For developers to test whether custom shaders match vanilla behavior. "));
			}
		}
		ImGui::EndTable();
	}
}

void AdvancedSettingsRenderer::RenderShaderCompileStatistics()
{
	auto shaderCache = globals::shaderCache;

	if (!ImGui::TreeNodeEx(T("menu.advanced.statistics", "Statistics"), ImGuiTreeNodeFlags_DefaultOpen)) {
		return;
	}

	ImGui::Text(T("menu.advanced.shader_compiler_stats", "Shader Compiler : %s"), shaderCache->GetShaderStatsString().c_str());

	// Derived parallelism metrics are computed lazily on demand and only shown
	// once compilation has completed to avoid per-frame analysis while compiling.
	if (!shaderCache->IsCompiling()) {
		auto parallelism = shaderCache->GetParallelismStats();
		if (parallelism.has_value()) {
			const auto& p = parallelism.value();
			ImGui::Spacing();
			ImGui::TextDisabled(T("menu.advanced.parallelism_header", "Parallelism (derived from %zu compiled tasks)"), p.sampleCount);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T("menu.advanced.parallelism_tooltip_1", "Computed lazily from the last completed build."));
				ImGui::Text("%s", T("menu.advanced.parallelism_tooltip_2", "Only evaluated when this Statistics section is open."));
			}
			ImGui::Text(T("menu.advanced.work_metric", "Work (W, sum of task wall times): %s"), Util::FormatDuration(p.workMs).c_str());
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T("menu.advanced.work_tooltip_1", "Total compile work: sum of all per-shader wall-clock compile times."));
				ImGui::Text("%s", T("menu.advanced.work_tooltip_2", "This is not CPU time; it is accumulated task elapsed time."));
				ImGui::Text("%s", T("menu.advanced.work_tooltip_3", "Equivalent serial time on one worker if overhead stayed the same."));
			}
			ImGui::Text(T("menu.advanced.span_metric", "Span (S, longest): %s"), Util::FormatDuration(p.spanMs).c_str());
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T("menu.advanced.span_tooltip_1", "Critical-path lower bound, approximated by the single slowest shader."));
				ImGui::Text("%s", T("menu.advanced.span_tooltip_2", "Even infinite cores cannot finish faster than this."));
			}
			ImGui::Text(T("menu.advanced.makespan_metric", "Makespan (T_p): %s"), Util::FormatDuration(p.makespanMs).c_str());
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T("menu.advanced.makespan_tooltip", "Observed wall-clock duration for the full shader build."));
			}
			ImGui::Text(T("menu.advanced.queue_wait_metric", "Queue wait (avg/max): %s / %s"),
				Util::FormatDuration(p.avgQueueWaitMs).c_str(),
				Util::FormatDuration(p.maxQueueWaitMs).c_str());
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T("menu.advanced.queue_wait_tooltip_1", "Time spent waiting in the ready queue before a worker started compilation."));
				ImGui::Text("%s", T("menu.advanced.queue_wait_tooltip_2", "Useful for identifying scheduler-induced delay separate from compile cost."));
			}
			ImGui::Text(T("menu.advanced.avg_parallelism_metric", "Average parallelism (W/S): %.2fx"), p.avgParallelism);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T("menu.advanced.avg_parallelism_tooltip_1", "Average useful concurrency in this workload."));
				ImGui::Text("%s", T("menu.advanced.avg_parallelism_tooltip_2", "Roughly the worker count where adding more cores gives diminishing returns."));
			}
			ImGui::Text(T("menu.advanced.infinite_core_efficiency_metric", "Infinite-core efficiency (S/T_p): %.1f%%"), 100.0 * p.infiniteCoreEfficiency);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T("menu.advanced.infinite_core_efficiency_tooltip_1", "How close runtime is to the infinite-core lower bound."));
				ImGui::Text(T("menu.advanced.infinite_core_efficiency_tooltip_2", "100%% means T_p == S."));
			}
			ImGui::Text(T("menu.advanced.infinite_core_gap_metric", "Infinite-core gap: %.1f%%"), p.infiniteCoreGapPercent);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T("menu.advanced.infinite_core_gap_tooltip_1", "Distance from ideal infinite-core time."));
				ImGui::Text("%s", T("menu.advanced.infinite_core_gap_tooltip_2", "Defined as 100 * (1 - S / T_p). Lower is better."));
			}

			ImGui::Spacing();
			ImGui::TextDisabled("%s", T("menu.advanced.infinite_core_efficiency", "Infinite-core efficiency"));
			float efficiency = static_cast<float>(std::clamp(p.infiniteCoreEfficiency, 0.0, 1.0));
			double effPct = 100.0 * p.infiniteCoreEfficiency;
			double gapPct = p.infiniteCoreGapPercent;
			ImGui::ProgressBar(efficiency, ImVec2(-1.0f, 0.0f), std::vformat(T("menu.advanced.efficiency_progress", "{:.1f}% efficient / {:.1f}% gap"), std::make_format_args(effPct, gapPct)).c_str());

			ImGui::Spacing();
			ImGui::TextDisabled("%s", T("menu.advanced.relative_durations", "Relative durations (normalized)"));
			double maxMs = std::max({ p.workMs, p.spanMs, p.makespanMs, 1.0 });
			auto drawRelativeBar = [maxMs](const char* label, double value) {
				float ratio = static_cast<float>(std::clamp(value / maxMs, 0.0, 1.0));
				ImGui::TextUnformatted(label);
				ImGui::SameLine();
				std::string durStr = Util::FormatDuration(value);
				double pctVal = 100.0 * ratio;
				ImGui::ProgressBar(ratio, ImVec2(-1.0f, 0.0f), std::vformat(T("menu.advanced.relative_bar_format", "{} ({:.1f}%)"), std::make_format_args(durStr, pctVal)).c_str());
			};
			drawRelativeBar(T("menu.advanced.span_label", "Span (S)"), p.spanMs);
			drawRelativeBar(T("menu.advanced.makespan_label", "Makespan (T_p)"), p.makespanMs);
			drawRelativeBar(T("menu.advanced.work_label", "Work (W)"), p.workMs);
		}
	}

	// Top-3 slowest shaders from the last build
	auto topSlow = shaderCache->GetTopSlowTasks(3);
	if (!topSlow.empty()) {
		ImGui::Spacing();
		ImGui::TextDisabled(T("menu.advanced.top_slowest_shaders", "Top %zu Slowest Shaders (last build)"), topSlow.size());
		for (size_t i = 0; i < topSlow.size(); ++i) {
			const auto& rec = topSlow[i];
			ImGui::Text(T("menu.advanced.shader_slow_entry", "#%zu  %s  (weight %d)"), i + 1,
				Util::FormatDuration(rec.elapsedMs).c_str(), rec.priority);
			ImGui::SameLine();
			ImGui::TextDisabled("%s", rec.key.c_str());
			if (ImGui::IsItemHovered()) {
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", rec.key.c_str());
				}
			}
			// Allow copying the full key with a right-click
			if (ImGui::BeginPopupContextItem(std::format("##slowcopy{}", i).c_str())) {
				if (ImGui::MenuItem(T("menu.advanced.copy_key", "Copy key"))) {
					ImGui::SetClipboardText(rec.key.c_str());
				}
				ImGui::EndPopup();
			}
		}
	}

	if (ImGui::TreeNodeEx(T("menu.advanced.all_compiled_tasks", "All Compiled Tasks"), ImGuiTreeNodeFlags_DefaultOpen)) {
		using SlowTaskRecord = SIE::CompilationSet::SlowTaskRecord;

		// Keyed on lastReset's QPC tick, not record count -- two builds can compile the
		// same task count. Also refreshed while compiling so it doesn't freeze mid-build.
		static std::vector<SlowTaskRecord> cachedRows;
		static int64_t cachedResetQpc = -1;
		const int64_t resetQpc = shaderCache->GetLastResetQpc();
		if (resetQpc != cachedResetQpc || shaderCache->IsCompiling()) {
			cachedResetQpc = resetQpc;
			cachedRows = shaderCache->GetAllTaskRecords();
		}

		static char taskFilterText[256] = "";
		static int taskSearchColumn = 0;
		static size_t taskSortColumn = 7;  // default sort by Completed, most recent first
		static bool taskSortAscending = false;

		auto queuePercent = [](const SlowTaskRecord& rec) {
			const double total = rec.queueWaitMs + rec.elapsedMs;
			return total > 0.0 ? 100.0 * rec.queueWaitMs / total : 0.0;
		};

		const int64_t qpcFrequency = shaderCache->GetQpcFrequency();
		// Time from build start to when this task finished, for a completion-order sort/display.
		auto completedSinceStartMs = [resetQpc, qpcFrequency](const SlowTaskRecord& rec) {
			return static_cast<double>(rec.startQpc - resetQpc) * 1000.0 / static_cast<double>(qpcFrequency) + rec.elapsedMs;
		};

		std::vector<Util::TableColumnConfig<SlowTaskRecord>> columns = {
			{ T("menu.advanced.column_task_key", "Key"), T("menu.advanced.column_task_key_tooltip", "Shader file, class, and active defines"), [](const SlowTaskRecord& rec) {
				 return rec.key;
			 },
				/*truncate=*/true, /*widthWeight=*/4.0f },
			{ T("menu.advanced.column_elapsed", "Elapsed"), T("menu.advanced.column_elapsed_tooltip", "Wall-clock compile time for this task"), [](const SlowTaskRecord& rec) {
				 return Util::FormatDuration(rec.elapsedMs);
			 } },
			{ T("menu.advanced.column_queue_wait", "Queue Wait"), T("menu.advanced.column_queue_wait_tooltip", "Time spent waiting for a free worker before compilation started"), [](const SlowTaskRecord& rec) {
				 return Util::FormatDuration(rec.queueWaitMs);
			 } },
			{ T("menu.advanced.column_queue_pct", "Queue %"), T("menu.advanced.column_queue_pct_tooltip", "queueWait / (queueWait + elapsed). High values mean this task waited on a busy scheduler rather than the shader itself being slow to compile."), [queuePercent](const SlowTaskRecord& rec) {
				 return Util::FormatPercent(static_cast<float>(queuePercent(rec)));
			 } },
			{ T("menu.advanced.column_priority", "Weight"), T("menu.advanced.column_priority_tooltip", "Estimated compile-cost priority used for scheduling"), [](const SlowTaskRecord& rec) {
				 return std::to_string(rec.priority);
			 } },
			{ T("menu.advanced.column_defines", "Defines"), T("menu.advanced.column_defines_tooltip", "Number of active define permutations for this task"), [](const SlowTaskRecord& rec) {
				 return std::to_string(rec.defineCount);
			 } },
			{ T("menu.advanced.column_source_kb", "Source KB"), T("menu.advanced.column_source_kb_tooltip", "HLSL source file size at compile time"), [](const SlowTaskRecord& rec) {
				 return std::format("{:.1f}", static_cast<double>(rec.sourceSizeBytes) / 1024.0);
			 } },
			{ T("menu.advanced.column_completed", "Completed"), T("menu.advanced.column_completed_tooltip", "Time from build start to when this task finished compiling"), [completedSinceStartMs](const SlowTaskRecord& rec) {
				 return Util::FormatDuration(completedSinceStartMs(rec));
			 } }
		};

		std::vector<std::function<bool(const SlowTaskRecord&, const SlowTaskRecord&, bool)>> sorters = {
			[](const SlowTaskRecord& a, const SlowTaskRecord& b, bool asc) { return asc ? (a.key < b.key) : (a.key > b.key); },
			[](const SlowTaskRecord& a, const SlowTaskRecord& b, bool asc) { return asc ? (a.elapsedMs < b.elapsedMs) : (a.elapsedMs > b.elapsedMs); },
			[](const SlowTaskRecord& a, const SlowTaskRecord& b, bool asc) { return asc ? (a.queueWaitMs < b.queueWaitMs) : (a.queueWaitMs > b.queueWaitMs); },
			[queuePercent](const SlowTaskRecord& a, const SlowTaskRecord& b, bool asc) {
				const double aPct = queuePercent(a);
				const double bPct = queuePercent(b);
				return asc ? (aPct < bPct) : (aPct > bPct);
			},
			[](const SlowTaskRecord& a, const SlowTaskRecord& b, bool asc) { return asc ? (a.priority < b.priority) : (a.priority > b.priority); },
			[](const SlowTaskRecord& a, const SlowTaskRecord& b, bool asc) { return asc ? (a.defineCount < b.defineCount) : (a.defineCount > b.defineCount); },
			[](const SlowTaskRecord& a, const SlowTaskRecord& b, bool asc) { return asc ? (a.sourceSizeBytes < b.sourceSizeBytes) : (a.sourceSizeBytes > b.sourceSizeBytes); },
			[completedSinceStartMs](const SlowTaskRecord& a, const SlowTaskRecord& b, bool asc) {
				const double aVal = completedSinceStartMs(a);
				const double bVal = completedSinceStartMs(b);
				return asc ? (aVal < bVal) : (aVal > bVal);
			}
		};

		auto getFilterableFields = [queuePercent, completedSinceStartMs](const SlowTaskRecord& rec) -> std::vector<std::string> {
			return {
				rec.key,
				Util::FormatDuration(rec.elapsedMs),
				Util::FormatDuration(rec.queueWaitMs),
				Util::FormatPercent(static_cast<float>(queuePercent(rec))),
				std::to_string(rec.priority),
				std::to_string(rec.defineCount),
				std::format("{:.1f}", static_cast<double>(rec.sourceSizeBytes) / 1024.0),
				Util::FormatDuration(completedSinceStartMs(rec))
			};
		};

		auto onRowRightClick = [](const SlowTaskRecord& rec) {
			ImGui::SetClipboardText(rec.key.c_str());
		};

		Util::TableFilterState<SlowTaskRecord> filterState(getFilterableFields);
		filterState.filterText = std::string(taskFilterText);
		filterState.searchColumn = taskSearchColumn;

		std::vector<Util::TableInputEvent<SlowTaskRecord>> inputEvents = {
			{ Util::TableInputEventType::ContextMenu, onRowRightClick, T("menu.advanced.copy_key", "Copy key"), 1 }
		};

		Util::ShowInteractiveTable<SlowTaskRecord>(
			"##AllCompiledTasksTable",
			columns,
			cachedRows,
			taskSortColumn,
			taskSortAscending,
			sorters,
			filterState,
			inputEvents);

		strncpy_s(taskFilterText, filterState.filterText.c_str(), sizeof(taskFilterText) - 1);
		taskFilterText[sizeof(taskFilterText) - 1] = '\0';
		taskSearchColumn = filterState.searchColumn;

		ImGui::TreePop();
	}

	ImGui::TreePop();

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	RenderCompileTraceExport();
}

// -----------------------------------------------------------------------------
// Diagnostics tab
// -----------------------------------------------------------------------------

void AdvancedSettingsRenderer::RenderDiagnosticsSection()
{
	RenderLoggingControls();

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	RenderRuntimeDebugControls();

	// Shader blocking only meaningful in developer mode (matches prior behavior).
	if (globals::state->IsDeveloperMode()) {
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		RenderShaderBlockingPanel();
	}
}

void AdvancedSettingsRenderer::RenderCompileTraceExport()
{
	Util::DrawSectionHeader(T("menu.advanced.compile_trace_header", "Compile Trace Export"));
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("menu.advanced.compile_trace_tooltip_1",
							  "Writes every compiled task from the current build to a Chrome Trace "
							  "Event Format JSON file, importable at ui.perfetto.dev or chrome://tracing."));
		ImGui::Text("%s", T("menu.advanced.compile_trace_tooltip_2",
							  "A timeline view can distinguish genuine shader compile cost from "
							  "external CPU contention during the build (e.g. another process "
							  "stealing cores), which aggregate stats alone cannot localize in time."));
	}

	auto shaderCache = globals::shaderCache;
	const bool canExport = !shaderCache->IsCompiling();
	if (!canExport && ImGui::IsItemHovered()) {
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T("menu.advanced.compile_trace_busy", "Wait for the current build to finish before exporting."));
		}
	}

	static bool lastExportOk = false;
	static std::string lastExportPath;

	ImGui::BeginDisabled(!canExport);
	if (ImGui::Button(T("menu.advanced.export_compile_trace", "Export Trace (Perfetto)"), { -1, 0 })) {
		const auto path = Util::PathHelpers::GetLogPath().parent_path() / "compile-trace.json";
		lastExportOk = shaderCache->ExportCompileTrace(path);
		lastExportPath = path.string();
	}
	ImGui::EndDisabled();

	if (!lastExportPath.empty()) {
		if (lastExportOk) {
			ImGui::TextColored({ 0.4f, 0.9f, 0.4f, 1.0f }, T("menu.advanced.compile_trace_exported", "Exported: %s"), lastExportPath.c_str());
		} else {
			ImGui::TextColored({ 0.9f, 0.4f, 0.4f, 1.0f }, "%s", T("menu.advanced.compile_trace_export_failed", "Export failed; check CommunityShaders.log for details."));
		}
	}
}

void AdvancedSettingsRenderer::RenderLoggingControls()
{
	Util::DrawSectionHeader(T("menu.advanced.tab_logging", "Logging"));

	// Log Level selection. Resync from state every frame so external changes
	// (config reload, console command, another caller of SetLogLevel) don't
	// leave the combo displaying a stale selection.
	spdlog::level::level_enum logLevel = globals::state->GetLogLevel();
	const char* items[] = {
		T("menu.advanced.log_level_trace", "trace"),
		T("menu.advanced.log_level_debug", "debug"),
		T("menu.advanced.log_level_info", "info"),
		T("menu.advanced.log_level_warn", "warn"),
		T("menu.advanced.log_level_err", "err"),
		T("menu.advanced.log_level_critical", "critical"),
		T("menu.advanced.log_level_off", "off")
	};
	int item_current = static_cast<int>(logLevel);
	if (ImGui::Combo(T("menu.advanced.log_level", "Log Level"), &item_current, items, IM_ARRAYSIZE(items))) {
		globals::state->SetLogLevel(static_cast<spdlog::level::level_enum>(item_current));
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("menu.advanced.log_level_tooltip", "Log level. Trace is most verbose. Default is info. Debug and Trace also enable Developer Mode."));
	}

	ImGui::Columns(2, nullptr, false);

	// Dump Ini Settings button
	if (ImGui::Button(T("menu.advanced.dump_ini_settings", "Dump Ini Settings"), { -1, 0 })) {
		Util::DumpSettingsOptions();
	}

	ImGui::NextColumn();

	// Open Logs button
	std::filesystem::path logPath = Util::PathHelpers::GetLogPath();
	if (!logPath.empty() && ImGui::Button(T("menu.advanced.open_logs", "Open Logs"), { -1, 0 })) {
		ShellExecuteA(NULL, "open", logPath.string().c_str(), NULL, NULL, SW_SHOWNORMAL);
	}

	ImGui::Columns(1);
}

void AdvancedSettingsRenderer::RenderRuntimeDebugControls()
{
	Util::DrawSectionHeader(T("menu.advanced.runtime_debug_header", "Runtime Debug"));

	// Frame annotations toggle
	ImGui::Checkbox(T("menu.advanced.frame_annotations", "Frame Annotations"), &globals::state->frameAnnotations);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("menu.advanced.frame_annotations_tooltip", "Enable detailed frame annotations for debugging render passes and draw calls."));
	}

	// Heat-shimmer (ISRefraction.hlsl) strength. Live per-frame value, no recompile needed.
	ImGui::SliderFloat(T("menu.advanced.refraction_scale", "Refraction Scale"), &globals::state->refractionScale, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("menu.advanced.refraction_scale_tooltip", "Scales the heat-shimmer/refraction distortion around fire and other heat sources. 1.0 is unmodified vanilla strength; 0 disables it."));
	}

	// Debug addresses section
	if (ImGui::TreeNodeEx(T("menu.advanced.addresses", "Addresses"))) {
		auto Renderer = globals::game::renderer;
		auto BSShaderAccumulator = *globals::game::currentAccumulator.get();
		auto RendererShadowState = globals::game::shadowState;
		ADDRESS_NODE(Renderer)
		ADDRESS_NODE(BSShaderAccumulator)
		ADDRESS_NODE(RendererShadowState)
		ImGui::TreePop();
	}
}

void AdvancedSettingsRenderer::RenderShaderBlockingPanel()
{
	auto shaderCache = globals::shaderCache;

	Util::DrawSectionHeader("Shader Blocking");

	// Show blocked shader status as a regular section
	if (!shaderCache->blockedKey.empty()) {
		// Create a visually distinct box for the blocked shader info with rounded corners and border
		const float scale = Util::GetUIScale();
		ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f * scale);
		ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, ImGui::GetStyle().WindowBorderSize);
		ImVec4 blockedBgColor = Util::Colors::GetError();
		blockedBgColor.w = 0.15f;  // Semi-transparent background
		ImGui::PushStyleColor(ImGuiCol_ChildBg, blockedBgColor);

		float maxHeight = ImGui::GetContentRegionAvail().y * 0.3f;  // Limit to 30% to keep Active Shaders visible
		if (ImGui::BeginChild("##BlockedShaderInfo", ImVec2(0, maxHeight), true, ImGuiChildFlags_AutoResizeY)) {
			Util::Text::Error(T("menu.advanced.shader_blocking_active", "Shader Blocking Active"));
			ImGui::SameLine();
			if (ImGui::SmallButton(T("menu.advanced.stop_blocking", "Stop Blocking##Section"))) {
				shaderCache->DisableShaderBlocking();
			}

			ImGui::Text(T("menu.advanced.blocked_shader", "Blocked: %s"), shaderCache->blockedKey.c_str());

			// Try to get more details from active shaders
			auto activeShaders = shaderCache->GetActiveShaders();
			for (const auto& shader : activeShaders) {
				if (shader.key == shaderCache->blockedKey) {
					ImGui::Text(T("menu.advanced.shader_type_label", "Type: %s"), magic_enum::enum_name(shader.shaderType).data());
					ImGui::Text(T("menu.advanced.shader_class_label", "Class: %s"), magic_enum::enum_name(shader.shaderClass).data());
					ImGui::Text(T("menu.advanced.shader_descriptor", "Descriptor: 0x%X"), shader.descriptor);

					// Add button to copy shader info to clipboard
					ImGui::PushID(shader.key.c_str());
					auto copyInfoLabel = std::format("{}##BlockedShader", T("menu.advanced.copy_info", "Copy Info"));
					if (ImGui::SmallButton(copyInfoLabel.c_str())) {
						std::string diskPathStr;
						diskPathStr.reserve(shader.diskPath.size());
						for (wchar_t wc : shader.diskPath) {
							diskPathStr += static_cast<char>(wc);
						}

						std::string fullInfo = std::format("Type: {}\nClass: {}\nDescriptor: 0x{:X}\nKey: {}\nCache Path: {}",
							magic_enum::enum_name(shader.shaderType).data(),
							magic_enum::enum_name(shader.shaderClass).data(),
							shader.descriptor,
							shader.key,
							diskPathStr);
						ImGui::SetClipboardText(fullInfo.c_str());
					}
					ImGui::PopID();
					if (ImGui::IsItemHovered()) {
						if (auto _tt = Util::HoverTooltipWrapper()) {
							ImGui::Text("%s", T("menu.advanced.copy_info_tooltip", "Copy complete shader information including cache path to clipboard"));
						}
					}

					break;
				}
			}
		}
		ImGui::EndChild();

		ImGui::PopStyleVar();    // ChildRounding
		ImGui::PopStyleVar();    // WindowBorderSize
		ImGui::PopStyleColor();  // ChildBg
	}

	// Blocking hotkeys + enable toggle
	{
		auto menu = globals::menu;
		auto& menuSettings = menu->GetSettings();
		auto& themeSettings = menuSettings.Theme;

		if (ImGui::Checkbox(T("menu.advanced.enable_shader_blocking", "Enable Shader Blocking"), &menuSettings.EnableShaderBlocking)) {
			// Setting saved automatically on next save
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T("menu.advanced.enable_shader_blocking_tooltip", "Enables hotkeys to cycle through and block individual shaders for debugging purposes."));
		}

		if (menuSettings.EnableShaderBlocking) {
			ImGui::Indent();

			// Shader Block Previous Key
			if (menu->settingShaderBlockPrevKey) {
				ImGui::Text("%s", T("menu.advanced.press_key_shader_block_prev", "Press any key for Shader Block Previous..."));
			} else {
				ImGui::AlignTextToFramePadding();
				ImGui::Text("%s", T("menu.advanced.block_previous", "Block Previous:"));
				ImGui::SameLine();
				ImGui::AlignTextToFramePadding();
				ImGui::TextColored(themeSettings.StatusPalette.CurrentHotkey, "%s",
					Util::Input::KeyIdToString(menuSettings.ShaderBlockPrevKey).c_str());
				ImGui::SameLine();
				if (ImGui::Button(T("menu.advanced.change_shader_block_prev", "Change##ShaderBlockPrev"))) {
					menu->settingShaderBlockPrevKey = true;
				}
			}

			// Shader Block Next Key
			if (menu->settingShaderBlockNextKey) {
				ImGui::Text("%s", T("menu.advanced.press_key_shader_block_next", "Press any key for Shader Block Next..."));
			} else {
				ImGui::AlignTextToFramePadding();
				ImGui::Text("%s", T("menu.advanced.block_next", "Block Next:"));
				ImGui::SameLine();
				ImGui::AlignTextToFramePadding();
				ImGui::TextColored(themeSettings.StatusPalette.CurrentHotkey, "%s",
					Util::Input::KeyIdToString(menuSettings.ShaderBlockNextKey).c_str());
				ImGui::SameLine();
				if (ImGui::Button(T("menu.advanced.change_shader_block_next", "Change##ShaderBlockNext"))) {
					menu->settingShaderBlockNextKey = true;
				}
			}

			ImGui::Unindent();
		}
	}

	// Active shaders list — rendered inline; the parent panel already says
	// "Shader Blocking", so a nested CollapsingHeader was redundant noise.
	{
		ImGui::Spacing();
		Util::DrawSectionHeader(T("menu.advanced.active_shaders_used_recently", "Active Shaders (Used Recently)"));
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T("menu.advanced.active_shaders_tooltip",
								  "List of shaders that have been used in recent frames. "
								  "Enable Shader Blocking above to use hotkeys to cycle through and block shaders for debugging. "
								  "Shaders not used for ~1 second are removed from this list."));
		}

		// Get fresh active shaders data for accurate count and table
		auto activeShaders = shaderCache->GetActiveShaders();
		uint32_t totalDrawCalls = 0;
		for (const auto& shader : activeShaders) {
			totalDrawCalls += shader.drawCalls;
		}

		// Static variables to maintain table filter state
		static char filterText[256] = "";
		static int searchColumn = 0;        // 0 = All Columns, 1 = Type, 2 = Class, 3 = Descriptor, 4 = Draw Calls, 5 = Key
		static size_t sortColumn = 4;       // Default sort by Frame % (draw calls)
		static bool sortAscending = false;  // Descending by default (highest usage first)		// Create shader rows for the table utility (simplified - no filter data needed)
		struct ShaderRow
		{
			SIE::ShaderCache::ActiveShaderInfo shader;
			uint32_t totalDrawCalls;
		};

		std::vector<ShaderRow> shaderRows;
		for (const auto& shader : activeShaders) {
			shaderRows.push_back({ shader, totalDrawCalls });
		}

		// Build column configurations
		std::vector<Util::TableColumnConfig<ShaderRow>> columns = {
			{ T("menu.advanced.column_type", "Type"), T("menu.advanced.column_type_tooltip", "Shader type"), [](const ShaderRow& row) {
				 return std::string(magic_enum::enum_name(row.shader.shaderType));
			 } },
			{ T("menu.advanced.column_class", "Class"), T("menu.advanced.column_class_tooltip", "Shader class"), [](const ShaderRow& row) {
				 return std::string(magic_enum::enum_name(row.shader.shaderClass));
			 } },
			{ T("menu.advanced.column_descriptor", "Descriptor"), T("menu.advanced.column_descriptor_tooltip", "Shader descriptor"), [](const ShaderRow& row) {
				 return std::format("0x{:X}", row.shader.descriptor);
			 } },
			{ T("menu.advanced.column_frame_pct", "Frame %"), T("menu.advanced.column_frame_pct_tooltip", "Percentage of draw calls this frame"), [](const ShaderRow& row) {
				 float percentage = Util::CalculatePercentage(static_cast<float>(row.shader.drawCalls), static_cast<float>(row.totalDrawCalls));
				 return Util::FormatPercent(percentage);
			 } },
			{ T("menu.advanced.column_key", "Key"), T("menu.advanced.column_key_tooltip", "Shader key"), [](const ShaderRow& row) {
				 return row.shader.key;
			 } }
		};

		// Row click callbacks
		auto onRowLeftClick = [shaderCache](const ShaderRow& row) {
			if (row.shader.key == shaderCache->blockedKey) {
				shaderCache->DisableShaderBlocking();
			} else {
				// Block this shader - use IterateShaderBlock to find and block it
				// Or set blockedKey directly (simpler for click-to-block)
				shaderCache->blockedKey = row.shader.key;
				logger::info("Blocking shader: {}", row.shader.key);
			}
		};

		auto onRowRightClick = [shaderCache](const ShaderRow& row) {
			std::string diskPathStr;
			diskPathStr.reserve(row.shader.diskPath.size());
			for (wchar_t wc : row.shader.diskPath) {
				diskPathStr += static_cast<char>(wc);
			}

			std::string fullInfo = std::format("Type: {}\nClass: {}\nDescriptor: 0x{:X}\nKey: {}\nCache Path: {}",
				magic_enum::enum_name(row.shader.shaderType).data(),
				magic_enum::enum_name(row.shader.shaderClass).data(),
				row.shader.descriptor,
				row.shader.key,
				diskPathStr);
			ImGui::SetClipboardText(fullInfo.c_str());
		};
		auto getRowTooltip = [shaderCache](const ShaderRow& row) {
			std::string clickAction = (row.shader.key == shaderCache->blockedKey) ? T("menu.advanced.click_to_unblock", "Left-click to unblock this shader") : T("menu.advanced.click_to_block", "Left-click to block this shader");
			auto shaderType = magic_enum::enum_name(row.shader.shaderType);
			auto shaderClass = magic_enum::enum_name(row.shader.shaderClass);

			return std::vformat(T("menu.advanced.shader_row_tooltip", "Type: {}\nClass: {}\nDescriptor: 0x{:X}\nKey: {}\n\n{}"), std::make_format_args(
																																	 shaderType,
																																	 shaderClass,
																																	 row.shader.descriptor,
																																	 row.shader.key,
																																	 clickAction));
		};

		// Define function to extract filterable fields (for TableFilterState)
		auto getFilterableFields = [](const ShaderRow& row) -> std::vector<std::string> {
			return {
				std::string(magic_enum::enum_name(row.shader.shaderType)),                                                                         // Type
				std::string(magic_enum::enum_name(row.shader.shaderClass)),                                                                        // Class
				std::format("0x{:X}", row.shader.descriptor),                                                                                      // Descriptor
				Util::FormatPercent(Util::CalculatePercentage(static_cast<float>(row.shader.drawCalls), static_cast<float>(row.totalDrawCalls))),  // Frame %
				row.shader.key                                                                                                                     // Key
			};
		};

		// Define sorting comparators (customSorts parameter)
		std::vector<std::function<bool(const ShaderRow&, const ShaderRow&, bool)>> sorters = {
			// Type - string sort
			[](const ShaderRow& a, const ShaderRow& b, bool ascending) {
				std::string aVal = std::string(magic_enum::enum_name(a.shader.shaderType));
				std::string bVal = std::string(magic_enum::enum_name(b.shader.shaderType));
				return ascending ? (aVal < bVal) : (aVal > bVal);
			},
			// Class - string sort
			[](const ShaderRow& a, const ShaderRow& b, bool ascending) {
				std::string aVal = std::string(magic_enum::enum_name(a.shader.shaderClass));
				std::string bVal = std::string(magic_enum::enum_name(b.shader.shaderClass));
				return ascending ? (aVal < bVal) : (aVal > bVal);
			},
			// Descriptor - numeric sort
			[](const ShaderRow& a, const ShaderRow& b, bool ascending) {
				return ascending ? (a.shader.descriptor < b.shader.descriptor) : (a.shader.descriptor > b.shader.descriptor);
			},
			// Frame % - numeric sort
			[](const ShaderRow& a, const ShaderRow& b, bool ascending) {
				float aPercent = Util::CalculatePercentage(static_cast<float>(a.shader.drawCalls), static_cast<float>(a.totalDrawCalls));
				float bPercent = Util::CalculatePercentage(static_cast<float>(b.shader.drawCalls), static_cast<float>(b.totalDrawCalls));
				return ascending ? (aPercent < bPercent) : (aPercent > bPercent);
			},
			// Key - string sort
			[](const ShaderRow& a, const ShaderRow& b, bool ascending) {
				return ascending ? (a.shader.key < b.shader.key) : (a.shader.key > b.shader.key);
			}
		};

		// Create filter state
		Util::TableFilterState<ShaderRow> filterState(getFilterableFields);

		// Initialize filter state from existing variables
		filterState.filterText = std::string(filterText, filterText + strlen(filterText));
		filterState.searchColumn = searchColumn;

		// Define input events for row interactions
		std::vector<Util::TableInputEvent<ShaderRow>> inputEvents = {
			// Left-click to block/unblock shader
			{ Util::TableInputEventType::MouseClick, onRowLeftClick, "", 0 },
			// Right-click context menu for copying info
			{ Util::TableInputEventType::ContextMenu, onRowRightClick, T("menu.advanced.copy_info", "Copy Info"), 1 }
		};

		// Render the table with all configurations
		Util::ShowInteractiveTable<ShaderRow>(
			"##ActiveShadersTable",
			columns,
			shaderRows,
			sortColumn,
			sortAscending,
			sorters,
			filterState,
			inputEvents,
			getRowTooltip);

		// Update static variables with modified filter state
		strncpy_s(filterText, filterState.filterText.c_str(), sizeof(filterText) - 1);
		filterText[sizeof(filterText) - 1] = '\0';
		searchColumn = filterState.searchColumn;
	}
}

// -----------------------------------------------------------------------------
// Disable at Boot tab
// -----------------------------------------------------------------------------

void AdvancedSettingsRenderer::RenderDisableAtBootSection(const std::function<void()>& drawDisableAtBootSettings)
{
	drawDisableAtBootSettings();
}

// -----------------------------------------------------------------------------
// Testing tab
// -----------------------------------------------------------------------------

void AdvancedSettingsRenderer::RenderTestingSection()
{
	auto state = globals::state;

	if (ImGui::Checkbox(T("menu.advanced.enable_developer_mode", "Enable Developer Mode"), &state->enableDeveloperMode)) {
		logger::info("Developer Mode {}", state->enableDeveloperMode ? "enabled" : "disabled");
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("menu.advanced.enable_developer_mode_tooltip",
							  "Unlocks developer-only options and tooling. "
							  "Also enabled automatically when Log Level is debug or trace. "
							  "Use at your own risk."));
	}
	if (!state->enableDeveloperMode && state->GetLogLevel() <= spdlog::level::debug) {
		ImGui::TextDisabled("%s", T("menu.advanced.developer_mode_via_log_level",
									  "Currently active because Log Level is debug/trace."));
	}

	ImGui::Spacing();

	// A/B Testing settings
	auto* abTestingManager = ABTestingManager::GetSingleton();
	abTestingManager->DrawSettingsUI();

	// Developer Mode Testing UI + scene-prep button (previously on the "Developer" tab)
	if (globals::state->IsDeveloperMode()) {
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		FeatureIssues::Test::DrawDeveloperModeTestingUI();

		ImGui::Spacing();
		// Test Conditions button - runs a set of console commands to prepare the player for testing
		if (ImGui::Button(T("menu.advanced.test_conditions", "Test Conditions"), { -1, 0 })) {
			if (auto ui = RE::UI::GetSingleton(); ui && !ui->menuStack.empty() && RE::PlayerCharacter::GetSingleton()) {
				RE::Console::ExecuteCommand("player.setav speedmult 1000");
				RE::Console::ExecuteCommand("tgm");
				RE::Console::ExecuteCommand("tcl");
				Util::EnvironmentControls::SetGameHour(12.0f);
				Util::EnvironmentControls::PauseTime();
				RE::Console::ExecuteCommand("coc whiterun");
				Util::EnvironmentControls::ChangeWeather(RE::TESForm::LookupByID<RE::TESWeather>(0x81a), true);
			}
		}
	}
}
