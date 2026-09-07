#include "Features/RemoteControl/DevBenchBridge.h"

#include <algorithm>

#include "Utils/FileSystem.h"

// Registers our tools into the devbench test bench over its C-ABI. Gated by
// DEVBENCH_BRIDGE_ENABLED (set by CMake when the devbench-api port is available);
// otherwise this file compiles to an empty Install(). Inert at runtime when no
// devbench plugin is present (GetDevBenchInterface001() returns null).
//
// The openshaders.* tools below expose Open Shaders' graphics-feature, inspect,
// capture, shadercache, and settings operations through the single devbench
// host over both MCP and REST. Each is namespaced to avoid collisions in devbench's
// shared registry; the action / kind / inputSchema shapes are stable so clients can
// rely on them.

#ifdef DEVBENCH_BRIDGE_ENABLED

#	include "Feature.h"
#	include "FeatureIssues.h"
#	include "Features/LightLimitFix/ShadowCasterManager.h"
#	include "Features/RenderDoc.h"
#	include "Features/ScreenshotFeature.h"
#	include "Globals.h"
#	include "Menu.h"
#	include "Profiler.h"
#	include "ShaderCache.h"
#	include "State.h"
#	include "Utils/DevBenchUx.h"
#	include "Utils/SettingsPatch.h"
#	include "VRAPI/CSpluginapi.h"

#	include <DevBenchAPI.h>
#	include <nlohmann/json.hpp>

#	include <algorithm>
#	include <chrono>
#	include <cstring>
#	include <format>
#	include <functional>
#	include <future>
#	include <memory>
#	include <optional>
#	include <stdexcept>
#	include <unordered_map>

namespace
{
	using json = nlohmann::json;

	// Current render frame, used as a coarse "enqueued at" stamp so callers can poll
	// `inspect kind=openshaders` until frame_count advances past it (i.e. a queued
	// main-thread task has had at least one tick to run). Safe from any thread (atomic load).
	uint EnqueuedFrame()
	{
		return globals::state ? globals::state->frameCountAtomic.load(std::memory_order_relaxed) : 0u;
	}

	// Shared C-ABI handler body. The whole request — parse, dispatch, dump — is wrapped
	// so NO exception ever crosses the DLL boundary, and a_write is called exactly once.
	// `a_build` is a plain function pointer (no captures) so this composes with the
	// captureless-handler contract devbench requires. JSON strings only across the ABI.
	void RunHandler(json (*a_build)(const json&), const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		json out;
		try {
			json args = json::object();
			if (a_argsJson && *a_argsJson)
				args = json::parse(a_argsJson);  // throws on malformed input
			if (!args.is_object())
				throw std::runtime_error("arguments must be a JSON object");
			out = a_build(args);
		} catch (const std::exception& e) {
			out = json{ { "error", "invalid request" }, { "detail", e.what() } };
		} catch (...) {
			out = json{ { "error", "unknown handler error" } };
		}
		const std::string dumped = out.dump();
		a_write(a_sink, dumped.c_str());
	}

	// Run a task on the main thread and return its result. Bridge handlers run on devbench's
	// listener thread; work that touches state the render/UI thread mutates (A/B fields, JSON
	// snapshots, feature settings, capture triggers) must marshal or it races. Blocks the
	// handler briefly, bounded so a stalled main thread (e.g. mid-load) can't hang it.
	//
	// The body may have SIDE EFFECTS (set/reset apply settings; capture triggers a frame), so a
	// `cancelled` flag is checked at task entry: if we already gave up waiting, the task skips
	// the body rather than mutating state after we reported a timeout. shared_ptr state so a
	// task that runs after we return doesn't dangle. (Best-effort: a task that starts exactly at
	// the timeout boundary can still run — the flag eliminates the common stalled-thread case.)
	json RunOnMainThread(std::function<json()> a_run)
	{
		auto* task = SKSE::GetTaskInterface();
		if (!task)
			return json{ { "error", "SKSE task interface unavailable" } };
		auto prom = std::make_shared<std::promise<json>>();
		auto cancelled = std::make_shared<std::atomic<bool>>(false);
		auto fut = prom->get_future();
		task->AddTask([prom, cancelled, run = std::move(a_run)]() {
			if (cancelled->load(std::memory_order_acquire))
				return;  // handler already timed out and returned — don't run a side-effecting body late
			try {
				prom->set_value(run());
			} catch (const std::exception& e) {
				prom->set_value(json{ { "error", "task threw on main thread" }, { "detail", e.what() } });
			} catch (...) {
				prom->set_value(json{ { "error", "task threw on main thread" }, { "detail", "non-std exception" } });
			}
		});
		if (fut.wait_for(std::chrono::milliseconds(5000)) != std::future_status::ready) {
			cancelled->store(true, std::memory_order_release);
			return json{ { "error", "main thread did not run within 5000ms (mid-load?)" } };
		}
		return fut.get();
	}

	// ---- feature: list / get / set / reset / toggle -----------------------------------

	// Build one feature entry, including restart-gated metadata so `list` answers
	// "what exists", "which fields need a restart", and "is anything pending" in one read.
	json FeatureEntry(Feature* f)
	{
		json entry{
			{ "name", f->GetDisplayName() },
			{ "shortName", f->GetShortName() },
			{ "loaded", f->loaded },
			{ "version", f->version },
			{ "category", std::string(f->GetCategory()) },
			{ "isCore", f->IsCore() },
			{ "supportsVR", f->SupportsVR() },
			{ "inMenu", f->IsInMenu() },
		};

		const auto fields = f->GetRestartRequiredFields();
		if (!fields.empty()) {
			json restartFields = json::array();
			const auto* liveBase = reinterpret_cast<const unsigned char*>(f->GetSettingsBlob());
			const size_t liveSize = f->GetSettingsBlobSize();
			for (const auto& field : fields) {
				bool pending = false;
				// Compare against remaining bytes (not offset+size, which can overflow and turn
				// a bad metadata entry into an out-of-bounds memcmp).
				if (liveBase && field.jsonKey && field.size != 0 &&
					field.offset <= liveSize && field.size <= liveSize - field.offset) {
					const void* boot = f->GetBootValue(field.jsonKey);
					if (boot && std::memcmp(boot, liveBase + field.offset, field.size) != 0)
						pending = true;
				}
				restartFields.push_back(json{
					{ "key", field.jsonKey ? field.jsonKey : "" },
					{ "label", field.label ? field.label : "" },
					{ "pending", pending },
				});
			}
			entry["restartFields"] = restartFields;
		}
		return entry;
	}

	json BuildFeatureResult(const json& a_args)
	{
		const std::string action = a_args.value("action", std::string("list"));

		if (action == "list") {
			// Marshal: FeatureEntry reads Feature::loaded and restart-gated settings bytes that
			// main-thread toggles / settings-loads mutate.
			return RunOnMainThread([]() {
				json out = json::array();
				for (auto* f : Feature::GetFeatureList())
					out.push_back(FeatureEntry(f));
				return out;
			});
		}

		const std::string shortName = a_args.value("shortName", std::string{});

		if (action == "toggle") {
			// Match over the full feature list (NOT FindFeatureByShortName, which only
			// matches *loaded* features — that makes toggle one-way: you could disable a
			// feature but never re-enable it).
			Feature* target = nullptr;
			if (!shortName.empty()) {
				for (auto* f : Feature::GetFeatureList()) {
					if (f->GetShortName() == shortName) {
						target = f;
						break;
					}
				}
			}
			if (!target)
				return json{ { "error", "unknown or missing shortName" }, { "shortName", shortName } };
			auto* task = SKSE::GetTaskInterface();
			if (!task)
				return json{ { "error", "SKSE task interface unavailable" }, { "shortName", shortName } };
			const uint frame = EnqueuedFrame();
			// If `enabled` is omitted we flip the CURRENT value — but that read must happen on
			// the main thread INSIDE the task: computing !target->loaded here on the listener
			// thread lets concurrent toggles all observe the same stale value and enqueue
			// identical results. With an explicit `enabled`, apply it verbatim.
			// Threading contract: Feature::loaded is a public flag the render pipeline reads
			// per-frame via ForEachLoadedFeature without synchronization, hot-toggled by direct
			// assignment — touch it ONLY on the main thread. The applied value is reported via
			// the openshaders.feature.changed event (authoritative; the response can't know an
			// implicit flip's result synchronously).
			const bool hasExplicit = a_args.contains("enabled");
			const bool explicitVal = a_args.value("enabled", false);
			task->AddTask([target, hasExplicit, explicitVal, shortName]() {
				const bool applied = hasExplicit ? explicitVal : !target->loaded;
				// Reject enabling a feature unsupported on the current runtime, unless Developer
				// Mode is on (this repo's existing override escape hatch).
				const bool wantsUnsupportedRuntime = applied && globals::game::isVR && !target->SupportsVR();
				const bool devOverride = globals::state && globals::state->IsDeveloperMode();
				if (wantsUnsupportedRuntime && !devOverride) {
					if (auto* dvb = DevBenchAPI::GetDevBenchInterface001()) {
						const std::string payload = json{ { "shortName", shortName }, { "error", "feature does not support the current runtime; enable rejected" } }.dump();
						dvb->EmitEvent("openshaders.feature.changed", payload.c_str());
					}
					logger::warn("DevBenchBridge: refused to enable '{}' -- unsupported on the current runtime", shortName);
					return;
				}
				if (wantsUnsupportedRuntime)
					logger::warn("DevBenchBridge: enabling '{}' via Developer Mode override despite being unsupported on the current runtime", shortName);
				target->loaded = applied;
				if (auto* dvb = DevBenchAPI::GetDevBenchInterface001()) {
					const std::string payload = json{ { "shortName", shortName }, { "enabled", applied } }.dump();
					dvb->EmitEvent("openshaders.feature.changed", payload.c_str());
				}
			});
			json r{ { "action", "toggle" }, { "shortName", shortName }, { "queued", true }, { "enqueued_at_frame", frame } };
			if (hasExplicit)
				r["requested"] = explicitVal;  // implicit flip's result arrives via the event
			return r;
		}

		if (shortName.empty())
			return json{ { "error", "missing required parameter 'shortName'" }, { "action", action } };

		// get / set / reset operate on a loaded feature. FindFeatureByShortName filters on
		// Feature::loaded, which queued toggle tasks mutate on the main thread — so resolve the
		// target INSIDE the main-thread path for each action, never on the listener thread.

		if (action == "get") {
			return RunOnMainThread([shortName]() -> json {
				auto* feature = Feature::FindFeatureByShortName(shortName);
				if (!feature)
					return json{ { "error", "feature not found or not loaded" }, { "shortName", shortName } };
				json blob;
				feature->SaveSettings(blob);
				return blob;
			});
		}

		// Live runtime stats (not persisted settings) via Feature::GetDiagnostics;
		// see LightLimitFix for the reference override. Empty object if unimplemented.
		if (action == "diagnostics") {
			return RunOnMainThread([shortName]() -> json {
				auto* feature = Feature::FindFeatureByShortName(shortName);
				if (!feature)
					return json{ { "error", "feature not found or not loaded" }, { "shortName", shortName } };
				return feature->GetDiagnostics();
			});
		}

		// Live runtime-only debug flags (never persisted to SettingsUser.json)
		// via Feature::GetRuntimeFlags/SetRuntimeFlag; see LightLimitFix for
		// the reference override. Empty object / false if unimplemented.
		if (action == "runtimeGet") {
			return RunOnMainThread([shortName]() -> json {
				auto* feature = Feature::FindFeatureByShortName(shortName);
				if (!feature)
					return json{ { "error", "feature not found or not loaded" }, { "shortName", shortName } };
				return feature->GetRuntimeFlags();
			});
		}

		if (action == "runtimeSet") {
			if (!a_args.contains("name") || !a_args["name"].is_string())
				return json{ { "error", "missing required string parameter 'name'" } };
			if (!a_args.contains("value") || !a_args["value"].is_boolean())
				return json{ { "error", "missing required boolean parameter 'value'" } };
			std::string flagName = a_args["name"];
			bool flagValue = a_args["value"];
			return RunOnMainThread([shortName, flagName, flagValue]() -> json {
				auto* feature = Feature::FindFeatureByShortName(shortName);
				if (!feature)
					return json{ { "error", "feature not found or not loaded" }, { "shortName", shortName } };
				if (!feature->SetRuntimeFlag(flagName, flagValue))
					return json{
						{ "error", "unrecognized runtime flag" },
						{ "shortName", shortName },
						{ "name", flagName },
						{ "hint", "must match a key from feature(action='runtimeGet')" }
					};
				logger::info("DevBenchBridge: feature(runtimeSet, {}, {}) applied", shortName, flagName);
				return json{ { "action", "runtimeSet" }, { "shortName", shortName }, { "name", flagName }, { "applied", true } };
			});
		}

		// set / reset resolve + apply on the main thread and report the real outcome: an invalid
		// shortName must NOT come back as a fake success. Synchronous (LoadSettings is fast), so
		// the lookup race is avoided AND the caller learns whether the mutation actually applied.
		if (action == "set") {
			if (!a_args.contains("settings") || !a_args["settings"].is_object())
				return json{ { "error", "missing required object parameter 'settings'" } };
			json blob = a_args["settings"];
			return RunOnMainThread([blob, shortName]() mutable -> json {
				auto* feature = Feature::FindFeatureByShortName(shortName);
				if (!feature)
					return json{ { "error", "feature not found or not loaded" }, { "shortName", shortName } };
				try {
					std::vector<std::string> unknown;
					if (!Util::Settings::ApplyPatch(*feature, blob, unknown))
						return json{
							{ "error", "unrecognized setting key(s); nothing applied" },
							{ "shortName", shortName },
							{ "unknownKeys", unknown },
							{ "hint", "keys must match the shape from feature(action='get'); nested groups such as ShadowSettings must be nested, not flattened" }
						};
					logger::info("DevBenchBridge: feature(set, {}) applied", shortName);
					return json{ { "action", "set" }, { "shortName", shortName }, { "applied", true } };
				} catch (const std::exception& e) {
					return json{ { "error", "LoadSettings threw" }, { "shortName", shortName }, { "detail", e.what() } };
				}
			});
		}

		if (action == "reset") {
			return RunOnMainThread([shortName]() -> json {
				auto* feature = Feature::FindFeatureByShortName(shortName);
				if (!feature)
					return json{ { "error", "feature not found or not loaded" }, { "shortName", shortName } };
				try {
					feature->RestoreDefaultSettings();
					logger::info("DevBenchBridge: feature(reset, {}) applied", shortName);
					return json{ { "action", "reset" }, { "shortName", shortName }, { "applied", true } };
				} catch (const std::exception& e) {
					return json{ { "error", "RestoreDefaultSettings threw" }, { "shortName", shortName }, { "detail", e.what() } };
				}
			});
		}

		return json{ { "error", "unknown action (list|get|set|reset|toggle|diagnostics|runtimeGet|runtimeSet)" }, { "action", action } };
	}

	void FeatureToolHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler(&BuildFeatureResult, a_argsJson, a_sink, a_write);
	}

	// ---- actions: devbench-registered UX commands / queries ----------------------------
	// Dispatches by name against Util::DevBenchUx::Registry (see Utils/DevBenchUx.h).
	// Named "actions", not "menu" -- devbench already has a `menu` tool (page navigation
	// via the CommunityShaders extension below); this is unrelated to that.

	bool ParsePerfProfile(const std::string& a_name, Feature::PerfProfile& a_out)
	{
		if (a_name == "performance") {
			a_out = Feature::PerfProfile::Performance;
		} else if (a_name == "balanced") {
			a_out = Feature::PerfProfile::Balanced;
		} else if (a_name == "quality") {
			a_out = Feature::PerfProfile::Quality;
		} else {
			return false;
		}
		return true;
	}

	// Registered for every feature (see RegisterDevBenchUx) -- no per-feature opt-in needed.
	json QueryMatchesPerformanceProfile(const Feature* a_feature, const json& a_args)
	{
		Feature::PerfProfile profile;
		if (!ParsePerfProfile(a_args.value("profile", std::string{}), profile))
			return json{ { "error", "unknown profile (performance|balanced|quality)" } };
		return json{ { "matches", a_feature->MatchesPerformanceProfile(profile) } };
	}

	json QueryGetProfilePreviewText(const Feature* a_feature, const json& a_args)
	{
		Feature::PerfProfile profile;
		if (!ParsePerfProfile(a_args.value("profile", std::string{}), profile))
			return json{ { "error", "unknown profile (performance|balanced|quality)" } };
		return json{ { "text", a_feature->GetProfilePreviewText(profile) } };
	}

	// Called once from Install(): registers the two universal queries above for every known
	// feature, then lets each feature register its own commands/queries via the virtual.
	void RegisterDevBenchUx()
	{
		auto& registry = Util::DevBenchUx::Registry::GetSingleton();
		for (auto* f : Feature::GetFeatureList()) {
			registry.RegisterQuery(f->GetShortName(), "matchesPerformanceProfile",
				"True when this feature's current settings already equal what ApplyPerformanceProfile(profile) would set. Params: profile (performance|balanced|quality).",
				&QueryMatchesPerformanceProfile);
			registry.RegisterQuery(f->GetShortName(), "profilePreviewText",
				"The tooltip text ApplyPerformanceProfile(profile) would show for this feature's profile button. Params: profile (performance|balanced|quality).",
				&QueryGetProfilePreviewText);
			f->RegisterUxActions();
		}
	}

	json BuildActionsResult(const json& a_args)
	{
		const std::string action = a_args.value("action", std::string{});
		const std::string shortName = a_args.value("shortName", std::string{});

		if (action == "listCommands" || action == "listQueries") {
			if (!Feature::FindRegisteredFeatureByShortName(shortName))
				return json{ { "error", "unknown or missing shortName" }, { "shortName", shortName } };
			auto& registry = Util::DevBenchUx::Registry::GetSingleton();
			const bool isCommands = action == "listCommands";
			return json{
				{ "shortName", shortName },
				{ isCommands ? "commands" : "queries", isCommands ? registry.ListCommands(shortName) : registry.ListQueries(shortName) },
			};
		}

		const std::string name = a_args.value("name", std::string{});
		const json actionArgs = a_args.value("args", json::object());

		if (action == "invokeCommand") {
			const auto* entry = Util::DevBenchUx::Registry::GetSingleton().FindCommand(shortName, name);
			if (!entry)
				return json{ { "error", "unknown command" }, { "shortName", shortName }, { "name", name } };
			Feature* target = Feature::FindRegisteredFeatureByShortName(shortName);
			if (!target)
				return json{ { "error", "unknown or missing shortName" }, { "shortName", shortName } };
			auto* task = SKSE::GetTaskInterface();
			if (!task)
				return json{ { "error", "SKSE task interface unavailable" } };
			const uint frame = EnqueuedFrame();
			const auto fn = entry->fn;
			task->AddTask([target, fn, actionArgs, shortName, name]() {
				try {
					fn(target, actionArgs);
					logger::info("DevBenchBridge: menu(invokeCommand, {}.{}) applied", shortName, name);
				} catch (const std::exception& e) {
					logger::error("DevBenchBridge: menu(invokeCommand, {}.{}) threw: {}", shortName, name, e.what());
				} catch (...) {
					logger::error("DevBenchBridge: menu(invokeCommand, {}.{}) threw (unknown)", shortName, name);
				}
			});
			return json{ { "action", "invokeCommand" }, { "shortName", shortName }, { "name", name }, { "queued", true }, { "enqueued_at_frame", frame } };
		}

		if (action == "invokeQuery") {
			const auto* entry = Util::DevBenchUx::Registry::GetSingleton().FindQuery(shortName, name);
			if (!entry)
				return json{ { "error", "unknown query" }, { "shortName", shortName }, { "name", name } };
			Feature* target = Feature::FindRegisteredFeatureByShortName(shortName);
			if (!target)
				return json{ { "error", "unknown or missing shortName" }, { "shortName", shortName } };
			// Marshal: query implementations read live feature settings the render/UI thread mutates.
			const auto fn = entry->fn;
			return RunOnMainThread([target, fn, actionArgs]() {
				return fn(target, actionArgs);
			});
		}

		return json{ { "error", "unknown action (listCommands|listQueries|invokeCommand|invokeQuery)" }, { "action", action } };
	}

	void ActionsToolHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler(&BuildActionsResult, a_argsJson, a_sink, a_write);
	}

	// ---- inspect: engine state / shader-cache status ----------------------------------

	// Registered as base-tool extensions on devbench 1.5.0+ (`inspect kind=openshaders`,
	// `inspect kind=shadercache`); the base tool routes by key, so each handler ignores
	// the kind arg and returns its own object.
	json BuildInspectStateResult(const json&)
	{
		return json{
			{ "plugin", "CommunityShaders" },
			{ "frame_count", EnqueuedFrame() },
			{ "vr", globals::game::isVR },
		};
	}

	json MismatchesToJson(const std::vector<Util::CacheInvalidation::CacheMismatch>& a_mismatches)
	{
		json out = json::array();
		for (const auto& mismatch : a_mismatches) {
			out.push_back(json{
				{ "kind", magic_enum::enum_name(mismatch.kind) },
				{ "shortName", mismatch.shortName },
				{ "feature", mismatch.feature },
				{ "detail", mismatch.detail },
				{ "nowPresent", mismatch.nowPresent },
			});
		}
		return out;
	}

	json RecentFailuresToJson(const std::vector<SIE::ShaderCache::CompileFailure>& a_failures)
	{
		json out = json::array();
		for (const auto& f : a_failures) {
			out.push_back(json{
				{ "key", f.key },
				{ "path", f.path },
				{ "error", f.error },
				{ "epoch", f.epoch },
				{ "frame", f.frame },
			});
		}
		return out;
	}

	json BuildInspectShadercacheResult(const json&)
	{
		// Built from thread-safe ShaderCache accessors. Poll completedTasks against a
		// pre-deploy snapshot to know a hot-reloaded shader finished; a rising
		// failedTasks / currentFailedCount surfaces an otherwise-invisible failed compile;
		// recentFailures identifies WHICH shader (source file + feature defines) and why.
		auto* cache = globals::shaderCache;
		if (!cache)
			return json{ { "error", "shader cache unavailable" } };
		return json{
			{ "compiling", cache->IsCompiling() },
			{ "completedTasks", cache->GetCompletedTasks() },
			{ "totalTasks", cache->GetTotalTasks() },
			{ "failedTasks", cache->GetFailedTasks() },
			{ "currentFailedCount", cache->GetCurrentFailedCount() },
			{ "recentFailures", RecentFailuresToJson(cache->GetRecentCompileFailures()) },
			{ "diskHitTasks", cache->GetDiskHitTasks() },
			{ "digestHitTasks", cache->GetDigestHitTasks() },
			{ "digestMissTasks", cache->GetDigestMissTasks() },
			{ "digestComputeCount", cache->GetDigestComputeCount() },
			{ "digestComputeTimeUs", cache->GetDigestComputeTimeUs() },
			{ "frame_count", EnqueuedFrame() },
			// Rollback/backup-slot state, mirrored by the restorePrevious/acceptRebuild actions.
			{ "diskCacheHeld", cache->IsDiskCacheHeld() },
			{ "featureSetChanged", cache->HasFeatureSetChanges() },
			{ "featureSetRevertPending", cache->HasFeatureSetRevertPending() },
			{ "featureSetCacheBackedUp", cache->HasFeatureSetCacheBackup() },
			{ "previousDiskCacheAvailable", cache->HasPreviousDiskCache() },
			{ "cacheMismatches", MismatchesToJson(cache->GetCacheMismatches()) },
			{ "previousCacheMismatches", MismatchesToJson(cache->GetPreviousCacheMismatches()) },
		};
	}

	json FeatureIssueToJson(const FeatureIssues::FeatureIssueInfo& a_issue)
	{
		return json{
			{ "shortName", a_issue.shortName },
			{ "displayName", a_issue.displayName },
			{ "version", a_issue.version },
			{ "issueType", magic_enum::enum_name(a_issue.issueType) },
			{ "rejectionReason", a_issue.rejectionReason },
			{ "replacementFeature", a_issue.replacementFeature },
			{ "replacementFeatureDisplayName", a_issue.replacementFeatureDisplayName },
			{ "replacementFeatureInstalled", a_issue.replacementFeatureInstalled },
			{ "replacementFeatureModLink", a_issue.replacementFeatureModLink },
			{ "userMessage", a_issue.userMessage },
			{ "minimumVersionRequired", a_issue.minimumVersionRequired },
			{ "modifiedShaderDirectory", a_issue.modifiedShaderDirectory },
			{ "iniPath", a_issue.iniPath },
			{ "removedInVersion", a_issue.removedInVersion.string(".") },
		};
	}

	// FeatureIssues' backing vector is main-thread-owned (populated at boot and by the
	// in-menu refresh action) with no lock of its own -- marshal onto the main thread
	// like every other main-thread-state read in this file, rather than racing it.
	json BuildInspectFeatureIssuesResult(const json&)
	{
		return RunOnMainThread([]() -> json {
			json issues = json::array();
			for (const auto& issue : FeatureIssues::GetFeatureIssues())
				issues.push_back(FeatureIssueToJson(issue));
			return json{
				{ "featureIssues", issues },
				{ "hasIssues", FeatureIssues::HasFeatureIssues() },
				{ "hasObsoleteShaderModifyingFeatures", FeatureIssues::HasObsoleteShaderModifyingFeatures() },
				{ "hasPotentialShaderModifyingFeatures", FeatureIssues::HasPotentialShaderModifyingFeatures() },
			};
		});
	}

	void InspectFeatureIssuesHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler(&BuildInspectFeatureIssuesResult, a_argsJson, a_sink, a_write);
	}

	// Light Limit Fix shadow-scheduler diagnostics. RequestSchedSnapshot is thread-safe
	// (snapshot copied under a mutex by the render thread) and primes the scheduler to
	// keep filling it for a short window, so a first idle call may return valid=false --
	// poll again after a frame. Light pointers are hex strings (JSON-number-safe).
	json BuildInspectShadowsResult(const json&)
	{
		const auto snap = ShadowCasterManager::RequestSchedSnapshot();
		json lights = json::array();
		for (const auto& [ptr, reason] : snap.demoted)
			lights.push_back(json{
				{ "ptr", std::format("{:#018x}", ptr) },
				{ "reason", ShadowCasterManager::SchedReasonName(reason) },
			});
		json slots = json::array();
		// Bucketed from tile.size, not renderedScale, which reflects the requested
		// scale -- a starved light can request full and get only the smallest tile.
		int classes[5] = {};
		int classNone = 0;  // tileSize == 0: no atlas tile, distinct from the smallest class
		const float baseTexels = snap.baseTileTexels > 0.0f ? snap.baseTileTexels : 2048.0f;
		for (const auto& s : snap.slots) {
			if (s.tileSize == 0) {
				classNone++;
			} else {
				int cls = 0;
				const float tileScale = static_cast<float>(s.tileSize) / baseTexels;
				for (float step = 1.0f; tileScale < step && cls < 4; step *= 0.5f)
					cls++;
				classes[cls]++;
			}
			slots.push_back(json{
				{ "slot", s.index },
				{ "ptr", std::format("{:#018x}", s.light) },
				{ "importance", s.importance },
				{ "score", s.score },
				{ "desiredScale", s.desiredScale },
				{ "budgetScale", s.budgetScale },
				{ "pendingScale", s.pendingScale },
				{ "renderedScale", s.renderedScale },
				{ "tile", json{ { "x", s.tileX }, { "y", s.tileY }, { "size", s.tileSize }, { "contentValid", s.tileContentValid } } },
				{ "geomListSize", s.geomListSize },
				{ "staticValid", s.staticValid },
				{ "staticEmpty", s.staticEmpty },
				{ "upload", json{ { "recorded", s.uploadRecorded }, { "paramY", s.uploadParamY }, { "range", s.uploadRange } } },
				{ "suppressed", s.suppressed },
				{ "promoted", s.promoted },
				{ "redrawnThisFrame", s.redrawnThisFrame },
				{ "schedDirty", s.schedDirty },
				{ "dirtyStallFrames", s.dirtyStallFrames },
				{ "redrawScore", s.redrawScore },
				{ "lastDrawnFrame", s.lastDrawnFrame },
				{ "cameraHold", s.cameraHold },
			});
		}
		return json{
			{ "valid", snap.valid },
			{ "frame", snap.frame },
			{ "total", snap.total },
			{ "chosen", snap.chosen },
			{ "excess", snap.excess },
			{ "invalidCamera", snap.invalidCamera },
			{ "invalidPortal", snap.invalidPortal },
			{ "invalidFrustum", snap.invalidFrustum },
			{ "invalidLod", snap.invalidLod },
			{ "invalidOther", snap.invalidOther },
			{ "slotsInUse", snap.slotsInUse },
			{ "lights", lights },
			{ "slots", slots },
			{ "classes", json{ { "full", classes[0] }, { "half", classes[1] }, { "quarter", classes[2] }, { "eighth", classes[3] }, { "sixteenth", classes[4] }, { "none", classNone } } },
			{ "atlas", json{
						   { "dim", snap.atlasDim },
						   { "baseTileTexels", snap.baseTileTexels },
						   { "capacityCells", snap.atlasCapacityCells },
						   { "occupancy", snap.atlasOccupancy },
						   { "vramBytes", snap.atlasVramBytes },
						   { "tileReallocs", snap.atlasTileReallocs },
						   { "ownerInvalidations", snap.atlasOwnerInvalidations },
						   { "allocDenied", snap.atlasAllocDenied },
						   { "cpuAccumUsAvg", snap.cpuAccumUsAvg },
						   { "cpuSubmitUsAvg", snap.cpuSubmitUsAvg },
						   { "cpuEnableUsAvg", snap.cpuEnableUsAvg },
					   } },
			{ "budget", json{
							{ "avgLightCostUs", snap.avgLightCostUs },
							{ "avgRedrawsPerFrame", snap.avgRedrawsPerFrame },
							{ "estPassMsPerFrame", snap.avgLightCostUs / 1000.0 * snap.avgRedrawsPerFrame },
							{ "staticBakesTotal", snap.staticBakesTotal },
							{ "cellResetsTotal", snap.cellResetsTotal },
							{ "cullPoolDropsTotal", snap.cullPoolDropsTotal },
							{ "casterCullDropsTotal", snap.casterCullDropsTotal },
							{ "sleepSkips", snap.sleepSkips },
							{ "sleepSkipsTotal", snap.sleepSkipsTotal },
							{ "demandSkips", snap.demandSkips },
							{ "demandSkipsTotal", snap.demandSkipsTotal },
							{ "alphaGroupPeak", snap.alphaGroupPeak },
							{ "alphaGroupDrops", snap.alphaGroupDrops },
						} },
			{ "demandAudit", json{
								 { "frustumCandidates", snap.frustumAuditCandidates },
								 { "frustumKeptSphereOut", snap.frustumAuditKeptOut },
								 { "frustumSuspects", snap.frustumAuditSuspects },
								 { "slotted", snap.demandSlotted },
								 { "zero", snap.demandZero },
								 { "subTap", snap.demandSubTap },
								 { "skipEligible", snap.demandSkipEligible },
								 { "swapIn", snap.demandSwapIn },
								 { "swapInAboveEps", snap.demandSwapInAboveEps },
								 { "redrawsSaved", snap.demandRedrawsSaved },
								 { "budgetSaturated", snap.demandBudgetSaturated },
								 { "phase1Enabled", snap.demandPhase1Enabled },
								 { "skipActive", snap.demandSkipActive },
								 { "skipEligibleTotal", snap.demandSkipEligibleTotal },
								 { "swapInTotal", snap.demandSwapInTotal },
								 { "redrawsSavedTotal", snap.demandRedrawsSavedTotal },
							 } },
			{ "redrawStall", json{
								 { "stallMax", snap.stallMax },
								 { "stallWorstSlot", snap.stallWorstSlot },
								 { "demandRatio", snap.demandRatio },
							 } },
		};
	}

	void InspectStateHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler(&BuildInspectStateResult, a_argsJson, a_sink, a_write);
	}

	void InspectShadercacheHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler(&BuildInspectShadercacheResult, a_argsJson, a_sink, a_write);
	}

	void InspectShadowsHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler(&BuildInspectShadowsResult, a_argsJson, a_sink, a_write);
	}

	// Marshaled to the main thread: GetResults() returns a live reference the
	// render thread clears+rebuilds every frame, and TimerResult's history
	// pointers dangle across a knownTimers reallocation -- nothing from it
	// may be read or retained off that thread.
	json BuildInspectProfilerResult(const json&)
	{
		auto* profiler = globals::profiler;
		if (!profiler)
			return json{ { "error", "profiler unavailable" } };
		return RunOnMainThread([profiler]() -> json {
			// Calling this tool is itself a capture request, the same as any
			// visible consumer (the Profiling menu, a feature panel).
			profiler->RequestCapture();

			json timers = json::array();
			for (const auto& r : profiler->GetResults()) {
				if (!r.valid)
					continue;
				timers.push_back(json{
					{ "name", r.name },
					{ "gpuMs", r.gpuTimeMs },
					{ "topLevelMs", r.topLevelMs },
					{ "avgMs", r.avgMs },
					{ "p95Ms", r.p95Ms },
					{ "p99Ms", r.p99Ms },
					{ "cpuMs", r.cpuTimeMs },
					{ "cpuAvgMs", r.cpuAvgMs },
					{ "cpuP95Ms", r.cpuP95Ms },
					{ "cpuP99Ms", r.cpuP99Ms },
					{ "hasGpu", r.hasGpu },
					{ "hasCpu", r.hasCpu },
					{ "activeGpu", r.activeGpu },
					{ "activeCpu", r.activeCpu },
					{ "historyHead", r.historyHead },
					{ "historyCount", r.historyCount },
					{ "cpuHistoryHead", r.cpuHistoryHead },
					{ "cpuHistoryCount", r.cpuHistoryCount },
				});
			}

			return json{
				{ "enabled", profiler->IsUserEnabled() },
				{ "capturing", profiler->IsEnabled() },
				{ "frame_count", EnqueuedFrame() },
				{ "capturedFrameCount", profiler->GetCapturedFrameCount() },
				{ "totalMs", profiler->GetTotalTimeMs() },
				{ "cpuTotalMs", profiler->GetCpuTotalTimeMs() },
				{ "resolvedTotalMs", profiler->GetResolvedTotalTimeMs() },
				{ "resolvedCpuTotalMs", profiler->GetResolvedCpuTotalTimeMs() },
				{ "acquiredSlots", profiler->GetAcquiredSlots() },
				{ "peakAcquiredSlots", profiler->GetPeakAcquiredSlots() },
				{ "slotRefusals", profiler->GetSlotRefusals() },
				{ "maxTimers", Profiler::kMaxTimers },
				{ "timers", timers },
			};
		});
	}

	void InspectProfilerHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler(&BuildInspectProfilerResult, a_argsJson, a_sink, a_write);
	}

	// ---- shadercache: clear / delete the compiled cache -------------------------------

	json BuildShadercacheResult(const json& a_args)
	{
		const std::string action = a_args.value("action", std::string{});
		auto* cache = globals::shaderCache;
		if (!cache)
			return json{ { "error", "shader cache unavailable" } };
		auto* task = SKSE::GetTaskInterface();
		if (!task)
			return json{ { "error", "SKSE task interface unavailable" } };
		const uint frame = EnqueuedFrame();

		// Mutating cache ops touch the live ShaderCache (and, for deleteDisk, the filesystem)
		// — marshal to the main thread. Recompiles are observable via inspect(kind=shadercache)
		// + the openshaders.shaderRecompiled event. NOTE clear vs deleteDisk: clear only drops
		// the in-memory maps, so with the disk cache enabled shaders reload from Data/ShaderCache
		// rather than recompiling — only deleteDisk guarantees a cold recompile.
		if (action == "clear") {
			task->AddTask([cache]() { cache->Clear(); });
			return json{ { "action", "clear" }, { "queued", true }, { "enqueued_at_frame", frame }, { "note", "in-memory cache dropped; shaders reload from the disk cache if present, else recompile (use deleteDisk to force a cold recompile)" } };
		}
		if (action == "deleteDisk") {
			// Delete on disk AND drop the in-memory cache — otherwise existing variants keep
			// serving from memory and the promised full recompile never happens (mirrors
			// PerformClearShaderCache and the ShaderCache invalidation path).
			task->AddTask([cache]() {
				cache->DeleteDiskCache();
				cache->Clear();
			});
			return json{ { "action", "deleteDisk" }, { "queued", true }, { "enqueued_at_frame", frame }, { "note", "on-disk + in-memory shader cache cleared; a full recompile follows (cold-compile benchmark)" } };
		}
		if (action == "activeOnly") {
			// Mirrors the in-game "smart clear" button's ActiveOnly scope: only evicts
			// shaders actually on screen across two capture windows, exposed here so that
			// scope (CompilationSet::Forget's re-arm path) is exercisable without the UI.
			task->AddTask([cache]() { cache->BeginActiveShaderCapture(); });
			return json{ { "action", "activeOnly" }, { "queued", true }, { "enqueued_at_frame", frame }, { "note", "scoped (smart) clear started; captures on-screen shaders over two windows, then evicts+recompiles just those (see inspect(kind=shadercache) and openshaders.shaderRecompiled)" } };
		}
		if (action == "backgroundCompile") {
			// Same atomic the in-game "Skip Compilation" hotkey flips (Menu.cpp);
			// unblocks XSEPlugin.cpp's boot-wait loop on its next check.
			cache->backgroundCompilation = true;
			return json{ { "action", "backgroundCompile" }, { "backgroundCompilation", true } };
		}
		if (action == "acceptRebuild") {
			// Mirrors the in-game held-cache prompt's "rebuild" button (HomePageRenderer.cpp):
			// discards the held cache's stale-feature blobs and rebuilds for the current setup.
			task->AddTask([cache]() { cache->AcceptCacheRebuild(); });
			return json{ { "action", "acceptRebuild" }, { "queued", true }, { "enqueued_at_frame", frame }, { "note", "accepted the feature-set change; disk cache rebuilding for the current setup, see inspect(kind=shadercache)" } };
		}
		if (action == "restorePrevious") {
			// Mirrors the in-game held-cache prompt's "restore previous" button; cancels an
			// in-progress compile if one is running. Only takes effect after a restart.
			task->AddTask([cache]() { cache->RestorePreviousDiskCache(); });
			return json{ { "action", "restorePrevious" }, { "queued", true }, { "enqueued_at_frame", frame }, { "note", "cancels any in-progress compile and needs a compatible previous cache; check CommunityShaders.log or inspect(kind=shadercache).featureSetRevertPending for the outcome, then restart to load it" } };
		}
		if (action == "exportTrace") {
			// Only reads records (mutex-guarded) and writes a file -- safe off-thread. Don't
			// add render/UI-state access here without marshalling like the actions above.
			const auto logDir = Util::PathHelpers::GetLogPath().parent_path();
			std::filesystem::path path = logDir / "compile-trace.json";
			if (a_args.contains("path")) {
				const std::filesystem::path requested(a_args.value("path", std::string{}));
				if (requested.is_absolute()) {
					return json{ { "action", "exportTrace" }, { "success", false }, { "error", "path must be relative to the log directory, not absolute" } };
				}
				const auto resolved = (logDir / requested).lexically_normal();
				const auto rel = resolved.lexically_relative(logDir);
				const bool escapesLogDir = rel.empty() || std::any_of(rel.begin(), rel.end(), [](const auto& part) { return part == ".."; });
				if (escapesLogDir) {
					return json{ { "action", "exportTrace" }, { "success", false }, { "error", "path escapes the log directory" } };
				}
				path = resolved;
			}
			const bool ok = cache->ExportCompileTrace(path);
			if (!ok) {
				return json{ { "action", "exportTrace" }, { "success", false }, { "error", "export failed or no task records for the current build; see CommunityShaders.log" } };
			}
			return json{ { "action", "exportTrace" }, { "success", true }, { "path", path.string() } };
		}
		return json{ { "error", "unknown action (clear|deleteDisk|activeOnly|backgroundCompile|acceptRebuild|restorePrevious|exportTrace)" }, { "action", action } };
	}

	void ShadercacheToolHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler(&BuildShadercacheResult, a_argsJson, a_sink, a_write);
	}

	// ---- profiler: enable / disable runtime capture ----------------------------------

	json BuildProfilerResult(const json& a_args)
	{
		const std::string action = a_args.value("action", std::string{});
		auto* profiler = globals::profiler;
		if (!profiler)
			return json{ { "error", "profiler unavailable" } };
		// SetUserEnabled only touches std::atomic_bool fields, so this is
		// safe to call directly off the devbench thread -- no marshalling.
		if (action == "enable") {
			profiler->SetUserEnabled(true);
			return json{ { "action", "enable" }, { "enabled", true } };
		}
		if (action == "disable") {
			profiler->SetUserEnabled(false);
			return json{ { "action", "disable" }, { "enabled", false } };
		}
		return json{ { "error", "unknown action (enable|disable)" }, { "action", action } };
	}

	void ProfilerToolHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler(&BuildProfilerResult, a_argsJson, a_sink, a_write);
	}

	// ---- capture: renderdoc / screenshot ----------------------------------------------

	json BuildCaptureResult(const json& a_args)
	{
		const std::string kind = a_args.value("kind", std::string{});
		if (kind.empty())
			return json{ { "error", "missing required parameter 'kind'" } };
		const uint frame = EnqueuedFrame();

		if (kind == "renderdoc") {
			uint32_t frameCount = 1;
			if (a_args.contains("frames") && a_args["frames"].is_number())
				frameCount = static_cast<uint32_t>(std::clamp(a_args["frames"].get<int>(), 1, 120));
			// All on the main thread: Feature::loaded is mutated by queued toggle tasks there,
			// and TriggerCapture invalidates RenderDoc's non-atomic capture-list cache the
			// UI/render thread reads.
			return RunOnMainThread([frameCount, frame]() -> json {
				auto* renderDoc = &globals::features::renderDoc;
				if (!renderDoc->loaded)
					return json{ { "error", "RenderDoc feature is not loaded" }, { "hint", "openshaders.feature(action='list') shows RenderDoc.loaded" } };
				if (!renderDoc->IsAvailable())
					return json{ { "error", "RenderDoc API not available — attach RenderDoc or load the in-app DLL" } };
				if (frameCount == 1)
					renderDoc->TriggerCapture();
				else
					renderDoc->TriggerMultiFrameCapture(frameCount);
				return json{ { "queued", true }, { "kind", "renderdoc" }, { "frames", frameCount }, { "enqueued_at_frame", frame } };
			});
		}

		if (kind == "screenshot") {
			// loaded read on the main thread (toggle tasks mutate it); the request flag is atomic.
			return RunOnMainThread([frame]() -> json {
				auto* shot = &globals::features::screenshotFeature;
				if (!shot->loaded)
					return json{ { "error", "Screenshot feature is not loaded" } };
				shot->captureRequested.store(true, std::memory_order_release);
				return json{ { "queued", true }, { "kind", "screenshot" }, { "enqueued_at_frame", frame } };
			});
		}

		if (kind == "shadowmaps") {
			// Atomic request flags serviced by the render thread's shadow pass;
			// everything lands under CommunityShaders/Captures. Default: one
			// whole-atlas DDS + manifest. With frames=N: a multi-frame recorder
			// (per-slot numeric state per frame; slot>=0 adds that light's
			// caster set per pass mode, and frames<=16 adds per-frame tile DDS).
			const uint32_t frames = a_args.value("frames", 0u);
			const int32_t slot = a_args.value("slot", -1);
			if (frames > 0) {
				ShadowCasterManager::RequestShadowFrameRecord(frames, slot);
				return json{ { "queued", true }, { "kind", "shadowmaps" }, { "frames", frames },
					{ "slot", slot }, { "enqueued_at_frame", frame },
					{ "note", "recorder armed; scm_frames_<N>.json (and scm_rec_slot<S>_f<N>.dds when slot>=0, frames<=16) under CommunityShaders/Captures at completion; watch the log" } };
			}
			ShadowCasterManager::RequestAtlasDump();
			return json{ { "queued", true }, { "kind", "shadowmaps" }, { "enqueued_at_frame", frame },
				{ "note", "written by the next shadow pass to Data/SKSE/Plugins/CommunityShaders/Captures (atlas mode only); watch the log for the path" } };
		}

		return json{ { "error", "unknown kind" }, { "kind", kind }, { "supported", json::array({ "renderdoc", "screenshot", "shadowmaps" }) } };
	}

	void CaptureToolHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler(&BuildCaptureResult, a_argsJson, a_sink, a_write);
	}

	// ---- settings: save / load / reset the GLOBAL CS config ---------------------------

	json BuildSettingsResult(const json& a_args)
	{
		const std::string action = a_args.value("action", std::string{});
		if (action.empty())
			return json{ { "error", "missing required parameter 'action'" } };
		auto* state = globals::state;
		if (!state)
			return json{ { "error", "State singleton unavailable" } };
		auto* task = SKSE::GetTaskInterface();
		if (!task)
			return json{ { "error", "SKSE task interface unavailable" } };
		const uint frame = EnqueuedFrame();

		// State::Save/Load read and write the on-disk USER config and touch every feature's
		// settings; both must run on the main thread for the same reason feature(set) does.
		// Contain failures inside the task: RunHandler's guard no longer applies once this runs
		// on SKSE's queue, so a malformed config / Save|Load throw would unwind on the game
		// thread after we already replied "queued". Degrade to a logged error instead.
		if (action == "save") {
			task->AddTask([state]() {
				try {
					state->Save(State::ConfigMode::USER);
					logger::info("DevBenchBridge: settings(save) applied");
				} catch (const std::exception& e) {
					logger::error("DevBenchBridge: settings(save) failed: {}", e.what());
				} catch (...) {
					logger::error("DevBenchBridge: settings(save) failed (unknown)");
				}
			});
			return json{ { "action", "save" }, { "queued", true }, { "enqueued_at_frame", frame } };
		}
		if (action == "load") {
			task->AddTask([state]() {
				try {
					state->Load(State::ConfigMode::USER, /*allowReload=*/true);
					logger::info("DevBenchBridge: settings(load) applied");
				} catch (const std::exception& e) {
					logger::error("DevBenchBridge: settings(load) failed: {}", e.what());
				} catch (...) {
					logger::error("DevBenchBridge: settings(load) failed (unknown)");
				}
			});
			return json{ { "action", "load" }, { "queued", true }, { "enqueued_at_frame", frame } };
		}
		if (action == "reset") {
			// Restore every feature to its defaults, then persist. Mirrors what the UI's
			// global reset does: per-feature RestoreDefaultSettings followed by a Save.
			task->AddTask([state]() {
				for (auto* f : Feature::GetFeatureList()) {
					try {
						f->RestoreDefaultSettings();
					} catch (const std::exception& e) {
						logger::error("DevBenchBridge: settings(reset) {} threw: {}", f->GetShortName(), e.what());
					}
				}
				try {
					state->Save(State::ConfigMode::USER);
					logger::info("DevBenchBridge: settings(reset) applied");
				} catch (const std::exception& e) {
					logger::error("DevBenchBridge: settings(reset) save failed: {}", e.what());
				} catch (...) {
					logger::error("DevBenchBridge: settings(reset) save failed (unknown)");
				}
			});
			return json{ { "action", "reset" }, { "queued", true }, { "enqueued_at_frame", frame } };
		}
		if (action == "applyVRProfile") {
			// Same broadcast path as the in-game hub button, exposed for headless automation/CI.
			const std::string profileName = a_args.value("profile", std::string{});
			Feature::PerfProfile profile;
			if (profileName == "performance")
				profile = Feature::PerfProfile::Performance;
			else if (profileName == "balanced")
				profile = Feature::PerfProfile::Balanced;
			else if (profileName == "quality")
				profile = Feature::PerfProfile::Quality;
			else
				return json{ { "error", "unknown profile (performance|balanced|quality)" }, { "profile", profileName } };

			task->AddTask([state, profile]() {
				try {
					Feature::ApplyPerformanceProfileToAll(profile);
				} catch (const std::exception& e) {
					logger::error("DevBenchBridge: settings(applyVRProfile) threw: {}", e.what());
				} catch (...) {
					logger::error("DevBenchBridge: settings(applyVRProfile) threw (unknown)");
				}
				try {
					state->Save(State::ConfigMode::USER);
					logger::info("DevBenchBridge: settings(applyVRProfile) applied");
				} catch (const std::exception& e) {
					logger::error("DevBenchBridge: settings(applyVRProfile) save failed: {}", e.what());
				} catch (...) {
					logger::error("DevBenchBridge: settings(applyVRProfile) save failed (unknown)");
				}
			});
			return json{ { "action", "applyVRProfile" }, { "profile", profileName }, { "queued", true }, { "enqueued_at_frame", frame } };
		}
		return json{ { "error", "unknown action (save|load|reset|applyVRProfile)" }, { "action", action } };
	}

	void SettingsToolHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler(&BuildSettingsResult, a_argsJson, a_sink, a_write);
	}

	// ---- menu handler: open / close / toggle the Community Shaders settings menu -------

	json BuildMenuResult(const json& a_args)
	{
		const std::string op = a_args.value("op", std::string("toggle"));
		Menu::VisibilityRequest req;
		if (op == "open")
			req = Menu::VisibilityRequest::Open;
		else if (op == "close")
			req = Menu::VisibilityRequest::Close;
		else if (op == "toggle")
			req = Menu::VisibilityRequest::Toggle;
		else
			return json{ { "error", "unknown op (open|close|toggle)" }, { "op", op } };

		// Optional page navigation, e.g. "Performance" or a feature's GetShortName().
		// RequestFeatureMenu is thread-safe (see its doc comment) for the same reason
		// RequestVisibility is used below instead of calling SetVisible directly.
		std::string page;
		if (auto it = a_args.find("page"); it != a_args.end() && it->is_string())
			page = it->get<std::string>();
		if (!page.empty())
			Menu::GetSingleton()->RequestFeatureMenu(page);

		// SetVisible touches the ImGui context and the IsEnabled flag the render thread owns, so
		// it can't run on this listener thread (nor on the SKSE main thread). Enqueue an atomic
		// request the render loop consumes next frame, mirroring the ToggleKey path.
		Menu::GetSingleton()->RequestVisibility(req);
		return json{ { "op", op }, { "page", page }, { "queued", true } };
	}

	void MenuHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler(&BuildMenuResult, a_argsJson, a_sink, a_write);
	}

	// ---- pluginapi: exercise the real SKSE ICSInterface001 ABI ------------------------

	// GetApi(0) always returns the same static instance for any revision this build
	// supports (see CSpluginapi.cpp) -- no handshake needed since we're in-process.
	CSPluginAPI::ICSInterface001* GetTestPluginApi()
	{
		return static_cast<CSPluginAPI::ICSInterface001*>(CSPluginAPI::GetApi(0));
	}

	// Dispatches directly on the devbench listener thread rather than marshaling to
	// main: API.md documents every method as callable from any thread (setters stage
	// into CSPluginAPI's queue; ProcessStagedSettings applies on the render thread next
	// frame), so calling from here exercises that contract instead of sidestepping it.
	json BuildPluginApiResult(const json& a_args)
	{
		auto* api = GetTestPluginApi();
		if (!api)
			return json{ { "error", "plugin API unavailable" } };

		const std::string method = a_args.value("method", std::string{});
		const json params = a_args.value("params", json::object());
		const uint frame = EnqueuedFrame();

		using CSPluginAPI::DLSSProfile;
		using CSPluginAPI::UpscaleMethod;
		using CSPluginAPI::UpscalePreset;
		using Handler = std::function<json(CSPluginAPI::ICSInterface001*, const json&)>;

		static const std::unordered_map<std::string, Handler> kMethods{
			{ "getBuildNumber", [](auto* i, const json&) { return json{ { "result", i->getBuildNumber() } }; } },
			{ "GetSSSEnabled", [](auto* i, const json&) { return json{ { "result", i->GetSSSEnabled() } }; } },
			{ "SetSSSEnabled", [](auto* i, const json& p) { i->SetSSSEnabled(p.value("enabled", false)); return json{ { "staged", true } }; } },
			{ "GetSSGIEnabled", [](auto* i, const json&) { return json{ { "result", i->GetSSGIEnabled() } }; } },
			{ "SetSSGIEnabled", [](auto* i, const json& p) { i->SetSSGIEnabled(p.value("enabled", false)); return json{ { "staged", true } }; } },
			{ "GetVolumetricLightingExteriorEnabled", [](auto* i, const json&) { return json{ { "result", i->GetVolumetricLightingExteriorEnabled() } }; } },
			{ "SetVolumetricLightingExteriorEnabled", [](auto* i, const json& p) { i->SetVolumetricLightingExteriorEnabled(p.value("enabled", false)); return json{ { "staged", true } }; } },
			{ "GetUpscalePreset", [](auto* i, const json&) { return json{ { "result", static_cast<uint32_t>(i->GetUpscalePreset()) } }; } },
			{ "SetUpscalePreset", [](auto* i, const json& p) { i->SetUpscalePreset(static_cast<UpscalePreset>(p.value("preset", 0u))); return json{ { "staged", true } }; } },
			{ "GetLightLimitFixContactShadowsEnabled", [](auto* i, const json&) { return json{ { "result", i->GetLightLimitFixContactShadowsEnabled() } }; } },
			{ "SetLightLimitFixContactShadowsEnabled", [](auto* i, const json& p) { i->SetLightLimitFixContactShadowsEnabled(p.value("enabled", false)); return json{ { "staged", true } }; } },
			{ "GetDLSSProfile", [](auto* i, const json&) { return json{ { "result", static_cast<uint32_t>(i->GetDLSSProfile()) } }; } },
			{ "SetDLSSProfile", [](auto* i, const json& p) { i->SetDLSSProfile(static_cast<DLSSProfile>(p.value("profile", 0u))); return json{ { "staged", true } }; } },
			{ "GetRenderAtUpscaleResEnabled", [](auto* i, const json&) { return json{ { "result", i->GetRenderAtUpscaleResEnabled() } }; } },
			{ "SetRenderAtUpscaleResEnabled", [](auto* i, const json& p) { i->SetRenderAtUpscaleResEnabled(p.value("enabled", false)); return json{ { "staged", true } }; } },
			{ "GetRenderAtUpscaleResActive", [](auto* i, const json&) { return json{ { "result", i->GetRenderAtUpscaleResActive() } }; } },
			{ "SetVRUpscalingTransitionProfile", [](auto* i, const json& p) {
				 i->SetVRUpscalingTransitionProfile(p.value("renderScaleModeEnabled", false), static_cast<UpscalePreset>(p.value("preset", 0u)), static_cast<DLSSProfile>(p.value("profile", 0u)));
				 return json{ { "staged", true } }; } },
			{ "GetUpscaleMethod", [](auto* i, const json&) { return json{ { "result", static_cast<uint32_t>(i->GetUpscaleMethod()) } }; } },
			{ "SetUpscaleMethod", [](auto* i, const json& p) { i->SetUpscaleMethod(static_cast<UpscaleMethod>(p.value("method", 0u))); return json{ { "staged", true } }; } },
			{ "SetVRUpscalingTransitionProfileForMethod", [](auto* i, const json& p) {
				 i->SetVRUpscalingTransitionProfileForMethod(static_cast<UpscaleMethod>(p.value("method", 0u)), p.value("renderScaleModeEnabled", false), static_cast<UpscalePreset>(p.value("preset", 0u)), static_cast<DLSSProfile>(p.value("profile", 0u)));
				 return json{ { "staged", true } }; } },
			{ "GetVRUpscalingApplyBlockReasons", [](auto* i, const json&) { return json{ { "result", i->GetVRUpscalingApplyBlockReasons() } }; } },
			{ "IsVRUpscalingProfileApplyAllowed", [](auto* i, const json&) { return json{ { "result", i->IsVRUpscalingProfileApplyAllowed() } }; } },
		};

		const auto it = kMethods.find(method);
		if (it == kMethods.end())
			return json{ { "error", "unknown method" }, { "method", method } };
		try {
			json result = it->second(api, params);
			// The ABI setters return void, so "staged" means dispatched, not
			// accepted -- the ABI's own enum validation can silently no-op an
			// unsupported value. Stamp the frame so a caller can poll
			// inspect/openshaders.feature for the actual outcome next tick.
			if (result.contains("staged"))
				result["enqueued_at_frame"] = frame;
			return result;
		} catch (const std::exception& e) {
			return json{ { "error", "method threw" }, { "method", method }, { "detail", e.what() } };
		}
	}

	void PluginApiHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler(&BuildPluginApiResult, a_argsJson, a_sink, a_write);
	}
}

namespace DevBenchBridge
{
	void Install()
	{
		auto* dvb = DevBenchAPI::GetDevBenchInterface001();
		if (!dvb) {
			logger::info("DevBenchBridge: devbench not present; CS tools not registered");
			return;
		}
		logger::info("DevBenchBridge: devbench build {} present — registering CS tools", dvb->GetBuildNumber());

		// Namespaced tool names — devbench's registry is shared across plugins, so bare
		// names ("feature", "inspect"…) could collide with devbench's own or another mod's.
		// Descriptors preserve the actions/kinds/inputSchema of CS's former embedded server
		// so existing MCP clients keep working under the new prefix.

		static constexpr const char* featureDesc =
			R"({"description":"All Open Shaders graphics-feature operations — enumerate, inspect settings, mutate settings, restore defaults, toggle on/off, read live diagnostics, read/write runtime-only debug flags. Action-dispatched. list: returns an array of {name,shortName,loaded,version,category,isCore,supportsVR,inMenu}; features with restart-gated settings also include restartFields:[{key,label,pending}]. get: params shortName, returns the SaveSettings blob (null if the feature has no override; set/reset then no-op). set: params shortName, settings (object) — a partial blob merged over the current settings, so it MUST use the same shape get returns, including nested groups (e.g. LightLimitFix's shadow settings live under settings.ShadowSettings.*, NOT at the top level). Keys the feature does not define are rejected with unknownKeys rather than silently ignored; call get first if unsure of the shape. Restart-gated keys (see list's restartFields) apply on the next launch, so verify with get rather than assuming a set took effect immediately. reset: params shortName, calls RestoreDefaultSettings. toggle: params shortName, enabled (boolean, OPTIONAL — omit to flip the current loaded state); flips Feature::loaded. Rejects enabling a feature unsupported on the current runtime unless Developer Mode is on, which force-enables it with a logged warning instead. diagnostics: params shortName, returns the feature's live runtime stats via GetDiagnostics (an empty object if the feature does not override it); use this instead of adding a new inspect kind for a new counter. runtimeGet: params shortName, returns the feature's live runtime-only debug flags via GetRuntimeFlags as {name: bool} (an empty object if the feature does not override it) — these are deliberately never persisted to SettingsUser.json (e.g. a debug instrumentation toggle that would otherwise cost every user extra GPU work on every load), so 'set' cannot reach them and they reset to their code default on every relaunch. runtimeSet: params shortName, name (string), value (boolean) — sets one flag via SetRuntimeFlag; fails with an error if the feature has no runtime flag by that name (call runtimeGet first to see valid names).","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["list","get","set","reset","toggle","diagnostics","runtimeGet","runtimeSet"]},"shortName":{"type":"string"},"settings":{"type":"object"},"enabled":{"type":"boolean"},"name":{"type":"string"},"value":{"type":"boolean"}}}})";
		dvb->RegisterTool("openshaders.feature", featureDesc, &FeatureToolHandler, nullptr);

		// One-time: builds Util::DevBenchUx::Registry from every feature's
		// RegisterUxActions() override, so openshaders.actions below has something to dispatch.
		RegisterDevBenchUx();

		static constexpr const char* actionsDesc =
			R"({"description":"Devbench-registered one-shot commands and read-only queries -- the imperative/derived-state counterpart to openshaders.feature's settings get/set. A feature exposes these via FEATURE_COMMAND/FEATURE_QUERY in its Feature::RegisterUxActions() override (see Utils/DevBenchUx.h); every feature also gets two built-in queries for free: matchesPerformanceProfile and profilePreviewText (params: profile=performance|balanced|quality), mirroring Feature::MatchesPerformanceProfile/GetProfilePreviewText so a profile button's highlighted/tooltip state is readable without re-deriving it from raw settings. Action-dispatched. listCommands/listQueries: params shortName, returns [{name,description}]. invokeCommand: params shortName, name, args (object, optional) -- queued onto the main thread, fire-and-forget, same as openshaders.feature toggle/set. invokeQuery: params shortName, name, args (object, optional) -- runs synchronously on the main thread and returns the result (queries read live feature state, so they marshal the same way a settings get does). Unknown shortName/name returns a plain error, never a crash.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["listCommands","listQueries","invokeCommand","invokeQuery"]},"shortName":{"type":"string"},"name":{"type":"string"},"args":{"type":"object"}}}})";
		dvb->RegisterTool("openshaders.actions", actionsDesc, &ActionsToolHandler, nullptr);

		static constexpr const char* shadercacheDesc =
			R"({"description":"Manage Open Shaders' compiled shader cache. clear, deleteDisk, activeOnly, acceptRebuild, and restorePrevious are queued onto the main thread, fire-and-forget. backgroundCompile is an immediate atomic state change made on the calling (devbench listener) thread. clear: drop the IN-MEMORY cache only; with the disk cache enabled shaders reload from Data/ShaderCache rather than recompiling, so this does NOT guarantee a recompile. deleteDisk: delete the on-disk cache AND drop the in-memory cache, forcing a full cold recompile (use this for compile benchmarks). activeOnly: the in-game 'smart clear' -- captures whatever shaders are on screen over two windows, then evicts+recompiles just those (needs something rendering; a menu-only screen may capture nothing). backgroundCompile: skip the boot-time wait for the FULL eager compile queue to drain -- same effect as the in-game 'Skip Compilation' hotkey. Compilation keeps running in the background afterward (fewer threads, so it doesn't starve gameplay), but the game becomes playable/scriptable immediately. For a benchmark harness: call this once right after launch, then drive one throwaway replay to demand-compile just that scene's own shaders before the timed run, instead of waiting out every permutation the whole install could ever need (can be 20-30 minutes on a large AIO modlist). acceptRebuild: when a feature-set change is holding the disk cache (see inspect(kind=shadercache).diskCacheHeld/cacheMismatches), accept it and rebuild for the current setup -- mirrors the in-game prompt's 'rebuild' button. restorePrevious: swap the rollback slot (the pre-change cache) back into the active slot -- mirrors the in-game prompt's 'restore previous' button; cancels an in-progress compile if one is running, only takes effect after a restart, check inspect(kind=shadercache).featureSetRevertPending or the log for the outcome. Watch progress via inspect kind=shadercache and the openshaders.shaderRecompiled event. Read-only status (including rollback/backup-slot state) is inspect kind=shadercache. exportTrace: write every task record from the current build to a Chrome Trace Event Format JSON file (importable at ui.perfetto.dev or chrome://tracing) -- a timeline can localize external CPU contention during a build in a way aggregate stats can't. Runs synchronously on the calling thread (read-only over the record set plus a file write). Optional 'path' overrides the default (next to CommunityShaders.log); fails if the current build has no recorded tasks yet.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["clear","deleteDisk","activeOnly","backgroundCompile","acceptRebuild","restorePrevious","exportTrace"]},"path":{"type":"string","description":"exportTrace only: destination file path; defaults to CommunityShaders.log's directory."}},"required":["action"]}})";
		dvb->RegisterTool("openshaders.shadercache", shadercacheDesc, &ShadercacheToolHandler, nullptr);

		static constexpr const char* profilerDesc =
			R"({"description":"Enable or disable Open Shaders' runtime GPU/CPU profiler capture. Action-dispatched. enable/disable: SetUserEnabled(true/false) -- the same flag the in-game Profiling page's checkbox drives, so this is the one state transition (off->on) a script can reach no other way. Read timing data via inspect kind=profiler; that tool also requests a capture on its own, so a script doesn't need to enable AND separately request one, only enable if disabled.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["enable","disable"]}},"required":["action"]}})";
		dvb->RegisterTool("openshaders.profiler", profilerDesc, &ProfilerToolHandler, nullptr);

		static constexpr const char* captureDesc =
			R"({"description":"Trigger a frame capture on the next render. Kind-dispatched. kind=renderdoc: RenderDoc multi-frame capture via the in-app API, honors frames (1-120, default 1); RenderDoc must be attached/loaded (check openshaders.feature list for RenderDoc.loaded). kind=screenshot: lossless screenshot via the Screenshot feature; frames is ignored. kind=shadowmaps: writes the shadow atlas depth texture (DDS) + slot-manifest JSON to Data/SKSE/Plugins/CommunityShaders/Captures on the next shadow pass (atlas mode only): ground truth for tile contents without a RenderDoc attach. Fire-and-forget: no artifact path is returned synchronously.","inputSchema":{"type":"object","properties":{"kind":{"type":"string","enum":["renderdoc","screenshot","shadowmaps"]},"frames":{"type":"number"}},"required":["kind"]}})";
		dvb->RegisterTool("openshaders.capture", captureDesc, &CaptureToolHandler, nullptr);

		static constexpr const char* settingsDesc =
			R"({"description":"Save, load, reset, or apply a performance profile to the GLOBAL Open Shaders user configuration. Action-dispatched, all fire-and-forget on the main thread. save: persist current settings (State::Save). load: re-read settings from disk and apply (State::Load). reset: restore every feature to its defaults then persist. applyVRProfile: broadcast the named performance profile (params profile: performance|balanced|quality) through Feature::ApplyPerformanceProfile across all features (Flat and VR alike), then persist; restart-gated fields (render preset, foveation, reprojection) take effect on next launch. Use after openshaders.feature set/reset to make changes durable, or to roll an A/B session back to the saved baseline.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["save","load","reset","applyVRProfile"]},"profile":{"type":"string","enum":["performance","balanced","quality"]}},"required":["action"]}})";
		dvb->RegisterTool("openshaders.settings", settingsDesc, &SettingsToolHandler, nullptr);

		static constexpr const char* pluginApiDesc =
			R"({"description":"Calls a method on the actual SKSE ICSInterface001 plugin ABI (see API.md) -- the same in-process interface a third-party plugin gets from the CSAP message handshake, not a re-implementation. Exercises the real vtable, including the internal staging/apply path (StagePatch -> ProcessStagedSettings, applied on the render thread next frame) that openshaders.feature's set action bypasses by writing settings directly. method (top-level, not nested under params): one of getBuildNumber, GetSSSEnabled, SetSSSEnabled, GetSSGIEnabled, SetSSGIEnabled, GetVolumetricLightingExteriorEnabled, SetVolumetricLightingExteriorEnabled, GetUpscalePreset, SetUpscalePreset, GetLightLimitFixContactShadowsEnabled, SetLightLimitFixContactShadowsEnabled, GetDLSSProfile, SetDLSSProfile, GetRenderAtUpscaleResEnabled, SetRenderAtUpscaleResEnabled, GetRenderAtUpscaleResActive, SetVRUpscalingTransitionProfile, GetUpscaleMethod, SetUpscaleMethod, SetVRUpscalingTransitionProfileForMethod, GetVRUpscalingApplyBlockReasons, IsVRUpscalingProfileApplyAllowed. params: named args for setters (enabled: bool; preset/profile/method: the ABI enum's underlying uint value; renderScaleModeEnabled: bool) -- omitted args default to 0/false, so always pass every arg a setter takes. Getters return {result}. Setters return {staged:true,enqueued_at_frame} -- staged means the call was dispatched to the ABI, NOT that it was accepted: the ABI validates enum arguments internally and silently ignores unsupported values (kHoshipa/kUltraQuality presets, DLSS profile kF, out-of-range methods, or -- for the two VR transition methods -- any single invalid argument, which aborts the whole call), only logging a warning. Confirm the real outcome with openshaders.feature action=get once frame_count (inspect kind=openshaders) advances past enqueued_at_frame; an unsupported value leaves the setting unchanged. A setter call is safe from this listener thread by the same contract API.md documents for a real plugin thread.","inputSchema":{"type":"object","properties":{"method":{"type":"string"},"params":{"type":"object"}},"required":["method"]}})";
		dvb->RegisterTool("openshaders.pluginapi", pluginApiDesc, &PluginApiHandler, nullptr);

		// devbench 1.5.0+ generalized tool extensions: route the CS settings menu and the
		// non-feature reads UNDER the base `menu` / `inspect` tools (menu invoke name=…,
		// inspect kind=…) instead of as top-level tools, keeping the agent-facing surface
		// small. RegisterToolExtension's vtable slot exists only on build >= 10500; no
		// fallback on older hosts (the menu + inspect reads are simply unavailable there).
		// "CommunityShaders" matches the ImGui window id after `###`, so the menu name lines
		// up with the on-screen window.
		if (dvb->GetBuildNumber() >= 10500) {
			static constexpr const char* menuDesc =
				R"({"description":"Open, close, or toggle the Open Shaders in-game settings menu headlessly, the same window the ToggleKey (default End) shows. op: open|close|toggle (default toggle). page: OPTIONAL built-in page name (e.g. \"Performance\", \"Home\") or a feature's shortName (see openshaders.feature list) to navigate to on the next frame, same as clicking it in the left pane. Returns {op,page,queued:true}; the change is applied on the render thread on the next frame (open is a no-op while first-time setup is pending).","inputSchema":{"type":"object","properties":{"op":{"type":"string","enum":["open","close","toggle"]},"page":{"type":"string"}}}})";
			dvb->RegisterToolExtension("menu", "CommunityShaders", menuDesc, &MenuHandler, nullptr);

			static constexpr const char* inspectStateDesc =
				R"({"description":"Open Shaders engine state -> {plugin,frame_count,vr}. frame_count increases each render tick — use it as ground truth that a queued operation has had a frame to run.","readOnly":true,"inputSchema":{"type":"object"}})";
			dvb->RegisterToolExtension("inspect", "openshaders", inspectStateDesc, &InspectStateHandler, nullptr);

			static constexpr const char* inspectCacheDesc =
				R"({"description":"Open Shaders shader-cache status -> {compiling,completedTasks,totalTasks,failedTasks,currentFailedCount,recentFailures,diskHitTasks,digestHitTasks,digestMissTasks,digestComputeCount,digestComputeTimeUs,frame_count,diskCacheHeld,featureSetChanged,featureSetRevertPending,featureSetCacheBackedUp,previousDiskCacheAvailable,cacheMismatches,previousCacheMismatches}. Poll completedTasks against a pre-deploy snapshot to know a hot-reloaded shader finished; watch failedTasks/currentFailedCount for failed compiles. recentFailures is the last 32 compile failures this session, each {key,path,error,epoch,frame} -- key is GetShaderString's cache key (shader class + merged feature #defines, identifying WHICH feature's shader broke without a separate lookup; for ImageSpace shaders the filename embedded in key can differ from path), path is the actual source file that was compiled, error is the compiler's own message (truncated to 2000 chars). digestHitTasks/digestMissTasks count disk-cache validity checks where the content-digest manifest matched (hit) or differed (miss); the later blob load or recompilation can still fail; digestComputeTimeUs is the cumulative microseconds spent computing content digests this session. diskCacheHeld true means a feature-set mismatch (see cacheMismatches, each {kind,shortName,feature,detail,nowPresent}) is holding the whole disk cache this session -- resolve with openshaders.shadercache acceptRebuild or restorePrevious. previousDiskCacheAvailable/previousCacheMismatches describe the rollback slot (the pre-change cache) a restorePrevious call would swap back in.","readOnly":true,"inputSchema":{"type":"object"}})";
			dvb->RegisterToolExtension("inspect", "shadercache", inspectCacheDesc, &InspectShadercacheHandler, nullptr);

			static constexpr const char* inspectShadowsDesc =
				R"({"description":"Open Shaders Light Limit Fix shadow-scheduler diagnostics -> {valid,frame,total,chosen,excess,invalid*,slotsInUse,lights:[{ptr,reason}],slots:[{slot,ptr,importance,score,desiredScale,budgetScale,pendingScale,renderedScale,tile:{x,y,size,contentValid}}],classes:{full,half,quarter,eighth,sixteenth},atlas:{dim,capacityCells,occupancy,vramBytes},budget:{avgLightCostUs,avgRedrawsPerFrame,estPassMsPerFrame,staticBakesTotal}}. reason: portal|frustum|lod|excess|other -- why a non-chosen light was demoted from a shadow caster. slots covers occupied point-light pool slots (tile.size 0 = no atlas tile); classes buckets renderedScale; atlas is all-zero when the shadow atlas is inactive; budget is the GPU-timestamp tracker (estPassMsPerFrame = avg cost x avg redraws, the REST perf A/B metric). The scheduler fills this only while the settings menu is open or a dump was recently requested; calling this primes it, so if valid==false (idle) poll again after a frame (use inspect kind=openshaders frame_count to know a tick passed).","readOnly":true,"inputSchema":{"type":"object"}})";
			dvb->RegisterToolExtension("inspect", "llfshadows", inspectShadowsDesc, &InspectShadowsHandler, nullptr);

			static constexpr const char* inspectProfilerDesc =
				R"({"description":"Open Shaders GPU/CPU profiler snapshot -> {enabled,capturing,frame_count,capturedFrameCount,totalMs,cpuTotalMs,resolvedTotalMs,resolvedCpuTotalMs,acquiredSlots,peakAcquiredSlots,slotRefusals,maxTimers,timers:[{name,gpuMs,topLevelMs,avgMs,p95Ms,p99Ms,cpuMs,cpuAvgMs,cpuP95Ms,cpuP99Ms,hasGpu,hasCpu,activeGpu,activeCpu,historyHead,historyCount,cpuHistoryHead,cpuHistoryCount}]}. Calling this primes a capture (like llfshadows), so an idle first call may show stale data -- capturedFrameCount is the engine frame the returned timers were actually recorded on (both it and frame_count are read in the same call, so they're always comparable). Results lag capture by kFrameLatency (3) frames by construction, so frame_count - capturedFrameCount == 3 is a maximally fresh snapshot and it is never less than 3; larger values mean staleness (e.g. capture was off, or the game is paused/loading). totalMs/cpuTotalMs are the LIVE values shown in the menu (zeroed while idle so the overlay doesn't show a stale sum); resolvedTotalMs/resolvedCpuTotalMs pair with capturedFrameCount and the timers array instead, so they never zero on their own and are the ones to use for exact checks. gpuMs and its rolling statistics report self time with directly nested GPU passes removed, so displayed GPU rows can be summed without double-counting. topLevelMs is the inclusive contribution from top-level intervals only; resolvedTotalMs should equal the sum of topLevelMs across timers. historyHead advances by exactly 1 per rolling-history sample regardless of how many call sites shared a name -- CollectResults sums same-name intervals into one entry before pushing, so a pass invoked twice in one frame (e.g. by a screenshot/crop-preview path recomposing the same output) never doubles its delta; comparing historyHead's delta between two polls across every hasGpu timer PRESENT IN BOTH polls should be identical (a timer first seen between polls starts fresh and is exempt) -- a mismatched delta means a missed/skipped cycle, not a duplicate call site. acquiredSlots is the GPU timer-slot count used in the most recently completed cycle (0 for a CPU-only cycle with no GPU frame; vs maxTimers, currently 256); peakAcquiredSlots is the session high-water mark of the same, since a sparse poll would likely miss a transient spike that acquiredSlots alone would show. slotRefusals is a cumulative session count of BeginPass calls that failed for lack of a free slot -- any nonzero value means timing data was silently dropped at some point this session, not necessarily the current frame. Enable/disable capture via openshaders.profiler.","readOnly":true,"inputSchema":{"type":"object"}})";
			dvb->RegisterToolExtension("inspect", "profiler", inspectProfilerDesc, &InspectProfilerHandler, nullptr);

			static constexpr const char* inspectFeatureIssuesDesc =
				R"({"description":"Open Shaders feature-issue tracking -> {featureIssues:[{shortName,displayName,version,issueType,rejectionReason,replacementFeature,replacementFeatureDisplayName,replacementFeatureInstalled,replacementFeatureModLink,userMessage,minimumVersionRequired,modifiedShaderDirectory,iniPath,removedInVersion}],hasIssues,hasObsoleteShaderModifyingFeatures,hasPotentialShaderModifyingFeatures}. Surfaces the same detection FeatureIssues.h already does at boot for the in-menu warning banner -- obsolete features with a known replacement, INI version mismatches, override files that failed to apply, and unrecognized/unknown feature INIs left in Data/Shaders/Features. issueType: OBSOLETE|VERSION_MISMATCH|OVERRIDE_FAILED|UNKNOWN. modifiedShaderDirectory/hasObsoleteShaderModifyingFeatures flag features whose obsolete files could still be holding stale shader source -- the more likely of the two shader-modifying flags to explain an otherwise-unexplained compile problem elsewhere.","readOnly":true,"inputSchema":{"type":"object"}})";
			dvb->RegisterToolExtension("inspect", "featureissues", inspectFeatureIssuesDesc, &InspectFeatureIssuesHandler, nullptr);
		} else {
			logger::info("DevBenchBridge: devbench build {} < 10500; CS menu + inspect extensions need 1.5.0", dvb->GetBuildNumber());
		}
	}
}

#else

namespace DevBenchBridge
{
	void Install() {}  // inert until built with DEVBENCH_BRIDGE_ENABLED
}

#endif
