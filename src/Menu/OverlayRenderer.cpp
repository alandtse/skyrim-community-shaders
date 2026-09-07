#include "OverlayRenderer.h"
#include "BackgroundBlur.h"
#include "Features/VR/VRVariableRateShading.h"
#include "HomePageRenderer.h"
#include "ThemeManager.h"

#include <dxgi.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <imgui_internal.h>
#include <winrt/base.h>

#include "CSEditor/EditorWindow.h"
#include "Feature.h"
#include "FeatureIssues.h"
#if defined(ENABLE_EFFECTS11)
#	include "Features/Effects11/EffectManager.h"
#endif
#include "Features/RenderDoc.h"
#include "Features/SceneSelector.h"
#include "Features/VR.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "Menu.h"
#include "Menu/CursorLoader.h"
#include "ShaderCache.h"
#include "State.h"
#include "Util.h"

#include "Features/PerformanceOverlay.h"
#include "Features/PerformanceOverlay/ABTesting/ABTesting.h"

namespace
{
	std::unordered_map<ImGuiID, float> s_windowOverlapAlpha;

	constexpr ImGuiWindowFlags SKIP_WINDOW_FLAGS = ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove;
	// Prefix-match against the display title set by Menu.cpp ("Open Shaders <ver>").
	// Must track that title — if the display name changes the prefix here must
	// change too, or IsMainWindow() will silently start returning false and
	// overlay-alpha logic loses its anchor.
	constexpr const char* MAIN_WINDOW_PREFIX = "Open Shaders";

	bool IsMainWindow(ImGuiWindow* win) { return win->Name && strncmp(win->Name, MAIN_WINDOW_PREFIX, strlen(MAIN_WINDOW_PREFIX)) == 0; }

	void DrawShaderCompilationFailures(uint64_t failed, const Menu::ThemeSettings& themeSettings)
	{
		if (failed) {
			ImGui::TextColored(themeSettings.StatusPalette.Error,
				T("overlay.shaders_failed", "ERROR: %llu shaders failed to compile. Check installation and CommunityShaders.log"),
				static_cast<unsigned long long>(failed));

			if (FeatureIssues::HasPotentialShaderModifyingFeatures()) {
				ImGui::TextColored(themeSettings.StatusPalette.Error, "%s", T("overlay.modified_features", "Features that may have modified shaders detected. Check Feature Issues in the Menu."));
			}
		}
	}

#if defined(ENABLE_EFFECTS11)
	void DrawEffects11Errors(const Menu::ThemeSettings& themeSettings)
	{
		auto& effectManager = EffectManager::GetSingleton();
		if (!effectManager.IsInitialized())
			return;

		uint32_t effectFailed = effectManager.GetFailedEffectCount();
		if (effectFailed) {
			ImGui::TextColored(themeSettings.StatusPalette.Error,
				"ERROR: %u effect(s) failed to compile",
				effectFailed);
			for (const auto& err : effectManager.GetAllErrors())
				ImGui::TextColored(themeSettings.StatusPalette.Error, "  %s", err.c_str());
		}
	}
#endif

	bool IsVisibleRootWindow(ImGuiWindow* win)
	{
		if (!win || !win->WasActive || win->Hidden)
			return false;
		return !(win->ParentWindow && !win->DockIsActive) && !(win->Flags & SKIP_WINDOW_FLAGS);
	}

	// Patches DrawList background vertices for windows involved in overlap.
	void PatchOverlappingWindowBackgrounds()
	{
		auto* ctx = ImGui::GetCurrentContext();
		if (!ctx)
			return;

		using C = ThemeManager::Constants;
		const float dt = ImGui::GetIO().DeltaTime;

		struct WinInfo
		{
			ImGuiWindow* win;
			ImRect rect;
		};
		std::vector<WinInfo> windows;
		for (int i = 0; i < ctx->Windows.Size; i++) {
			auto* win = ctx->Windows[i];
			if (IsVisibleRootWindow(win))
				windows.push_back({ win, win->Rect() });
		}

		std::unordered_set<ImGuiID> overlapping;
		for (size_t i = 0; i < windows.size(); i++)
			for (size_t j = i + 1; j < windows.size(); j++)
				if (windows[i].rect.Overlaps(windows[j].rect)) {
					auto* a = windows[i].win;
					auto* b = windows[j].win;
					// Main CS window never dims; other windows yield to it
					if (IsMainWindow(a))
						overlapping.insert(b->ID);
					else if (IsMainWindow(b))
						overlapping.insert(a->ID);
					else
						overlapping.insert(a->FocusOrder > b->FocusOrder ? a->ID : b->ID);
				}

		const ImU32 bgRGB = ImGui::GetColorU32(ImGuiCol_WindowBg) & ~IM_COL32_A_MASK;

		for (auto& [win, rect] : windows) {
			const float target = overlapping.count(win->ID) ? C::OVERLAP_MIN_ALPHA : 0.0f;
			float& alpha = s_windowOverlapAlpha[win->ID];
			const float speed = (target > alpha) ? C::OVERLAP_FADEIN_SPEED : C::OVERLAP_FADEOUT_SPEED;
			alpha += (target - alpha) * (std::min)(1.0f, dt * speed);

			if (alpha < C::OVERLAP_ALPHA_EPSILON) {
				alpha = 0.0f;
				continue;
			}

			auto* dl = win->DrawList;
			if (!dl || dl->VtxBuffer.Size == 0)
				continue;

			// Clamp background rect vertex alpha (contiguous bgRGB block at start of DrawList)
			const ImU32 minA = static_cast<ImU32>(alpha * 255.0f);
			for (int v = 0; v < dl->VtxBuffer.Size; v++) {
				auto& vtx = dl->VtxBuffer[v];
				if ((vtx.col & ~IM_COL32_A_MASK) != bgRGB)
					break;
				ImU32 a = (vtx.col >> IM_COL32_A_SHIFT) & 0xFF;
				if (a > 0 && a < minA)
					vtx.col = bgRGB | (minA << IM_COL32_A_SHIFT);
			}
		}

		// Prune stale entries
		for (auto it = s_windowOverlapAlpha.begin(); it != s_windowOverlapAlpha.end();)
			it->second < C::OVERLAP_ALPHA_EPSILON ? it = s_windowOverlapAlpha.erase(it) : ++it;
	}
}  // namespace

void OverlayRenderer::RenderOverlay(
	Menu& menu,
	const std::function<void()>& processInputEventQueue,
	const std::function<void()>& drawSettings,
	const std::function<const char*(std::vector<InputCombo>)>& keyIdToString,
	float& cachedFontSize,
	float currentFontSize)
{
	// Apply the VR panel size before pumping input: PumpInput reads
	// io.DisplaySize to map wand UV to pixels for this frame.
	ApplyVRPanelDisplaySize();
	processInputEventQueue();

	if (ShouldSkipRendering()) {
		auto& io = ImGui::GetIO();
		io.ClearInputKeys();
		io.ClearEventsQueue();
		s_windowOverlapAlpha.clear();
		return;
	}

	HandleFontReload(menu, cachedFontSize, currentFontSize);
	InitializeImGuiFrame(menu);

	RenderShaderCompilationStatus(keyIdToString);
	RenderShaderBlockingStatus();

	auto* editorWindow = EditorWindow::GetSingleton();
	if (editorWindow->open && !EditorWindow::CanBeOpen()) {
		editorWindow->open = false;
		if (editorWindow->IsInPreviewMode())
			editorWindow->ExitPreviewMode();
	}
	editorWindow->UpdateOpenState();
	if (editorWindow->open) {
		bool flying = editorWindow->IsPreviewFlying();
		auto& io = ImGui::GetIO();
		io.MouseDrawCursor = !flying;
		if (flying)
			io.MousePos = { -FLT_MAX, -FLT_MAX };  // prevent hover/tooltips during active flying
		editorWindow->Draw();
	} else if (menu.IsEnabled || HomePageRenderer::ShouldShowFirstTimeSetup() ||
			   globals::features::vr.HelperRequestsRender()) {
		ImGui::GetIO().MouseDrawCursor = true;
		// Helper-requested render: the helper's in-scene focus model routed
		// focus here. Draw the settings UI even though Menu::IsEnabled hasn't
		// been flipped by the local TAB hotkey. This honors the
		// kClientFlag_RendersOnFocus contract we advertised at registration.
		if (menu.IsEnabled || globals::features::vr.HelperRequestsRender()) {
			drawSettings();
		}
	} else {
		ImGui::GetIO().MouseDrawCursor = false;
	}

	RenderFeatureOverlays();
	RenderFirstTimeSetupOverlay();
	HandleABTesting();
	PatchOverlappingWindowBackgrounds();
	FinalizeImGuiFrame();
}

bool OverlayRenderer::ShouldSkipRendering()
{
	auto shaderCache = globals::shaderCache;
	auto failed = shaderCache->GetCurrentFailedCount();
	auto hide = shaderCache->IsHideErrors();
	auto* abTestingManager = ABTestingManager::GetSingleton();
	auto* renderDoc = RenderDoc::GetSingleton();

#if defined(ENABLE_EFFECTS11)
	uint32_t effectFailed = EffectManager::GetSingleton().IsInitialized() ? EffectManager::GetSingleton().GetFailedEffectCount() : 0;
#else
	uint32_t effectFailed = 0;
#endif

	return !(shaderCache->IsCompiling() ||
			 Menu::GetSingleton()->IsEnabled ||
			 EditorWindow::GetSingleton()->open ||
			 abTestingManager->IsEnabled() ||
			 (failed && !hide) ||
			 effectFailed ||
			 globals::features::performanceOverlay.settings.ShowInOverlay ||
			 globals::features::sceneSelector.IsOverlayVisible() ||
			 renderDoc->IsAvailable() ||
			 HomePageRenderer::ShouldShowFirstTimeSetup() ||
			 globals::features::vr.HelperRequestsRender());
}

void OverlayRenderer::HandleFontReload(Menu& menu, float& cachedFontSize, float currentFontSize)
{
	bool fontSizeChanged = std::abs(cachedFontSize - currentFontSize) > ThemeManager::Constants::FONT_CACHE_EPSILON;
	std::string desiredSignature = menu.BuildFontSignature(currentFontSize);
	bool signatureChanged = desiredSignature != menu.cachedFontSignature;

	if (fontSizeChanged || signatureChanged) {
		if (!ThemeManager::ReloadFont(menu, cachedFontSize)) {
			logger::warn("OverlayRenderer::HandleFontReload() - Font reload failed");
		}
	}
}

bool OverlayRenderer::ApplyVRPanelDisplaySize()
{
	uint32_t panelW = 0, panelH = 0;
	if (!globals::game::isVR || !globals::features::vr.GetHelperPanelSize(panelW, panelH))
		return false;

	// VR: canvas must equal the helper panel's pixel size 1:1, or wand
	// clicks (mapped via the same DisplaySize-based UV) drift from the
	// resized content. The desktop mirror shares this size and can clip on
	// a differing swapchain resolution; pre-existing, not a regression.
	auto& io = ImGui::GetIO();
	io.DisplaySize = ImVec2(static_cast<float>(panelW), static_cast<float>(panelH));
	io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
	return true;
}

void OverlayRenderer::InitializeImGuiFrame(Menu& menu)
{
	// Start the Dear ImGui frame
	ImGui_ImplDX11_NewFrame();
	ImGui_ImplWin32_NewFrame();

	// ImGui_ImplWin32_NewFrame() above overwrites DisplaySize from the window
	// rect, so the panel size must be re-applied here before ImGui::NewFrame.
	const bool vrPanel = ApplyVRPanelDisplaySize();
	if (!vrPanel) {
		DXGI_SWAP_CHAIN_DESC desc{};
		globals::d3d::swapChain->GetDesc(&desc);
		const float displayW = static_cast<float>(desc.BufferDesc.Width);
		const float displayH = static_cast<float>(desc.BufferDesc.Height);
		Util::UpdateImGuiInput(desc.OutputWindow, displayW, displayH);
	}
	// The wand drives the cursor in VR (PumpInput), so skip the desktop
	// cursor injection above for the panel case, since it would fight the wand position.

	ImGui::NewFrame();

	// Detect display size change to reset window layout. The VR canvas size
	// is not invariant (it tracks panel resolution/supersampling), which is
	// exactly the kind of change this catches.
	const float2 currentDisplaySize{ ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y };
	if (menu.lastDisplaySize.x > 0.f && menu.lastDisplaySize != currentDisplaySize) {
		logger::info("Display size changed: {}x{} -> {}x{}, resetting window layout",
			menu.lastDisplaySize.x, menu.lastDisplaySize.y, currentDisplaySize.x, currentDisplaySize.y);
		menu.resetLayout = true;
		EditorWindow::GetSingleton()->resetLayout = true;
		globals::features::performanceOverlay.ResetWindowLayout();
		globals::features::sceneSelector.ResetWindowLayout();
	}
	menu.lastDisplaySize = currentDisplaySize;

	ThemeManager::SetupImGuiStyle(menu);
}

void OverlayRenderer::RenderShaderCompilationStatus(const std::function<const char*(std::vector<InputCombo>)>& keyIdToString)
{
	auto shaderCache = globals::shaderCache;
	auto failed = shaderCache->GetCurrentFailedCount();
	auto hide = shaderCache->IsHideErrors();

	const float scale = Util::GetUIScale();
	const float pos = ThemeManager::Constants::OVERLAY_WINDOW_POSITION * scale;

	uint64_t totalShaders = shaderCache->GetTotalTasks();
	uint64_t compiledShaders = shaderCache->GetCompletedTasks();

	auto state = globals::state;
	auto& themeSettings = Menu::GetSingleton()->GetTheme();
	auto* renderDoc = RenderDoc::GetSingleton();
	bool renderDocAvailable = renderDoc->IsAvailable();
	const auto renderDocInformation = renderDoc->GetOverlayWarningMessage();

	std::string compilePrefix = shaderCache->backgroundCompilation ? T("overlay.background_prefix", "Background ") : "";
	std::string shaderStats = shaderCache->GetShaderStatsString(!state->IsDeveloperMode());
	auto progressTitle = std::vformat(T("overlay.compiling_shaders", "{}Compiling Shaders: {}"),
		std::make_format_args(compilePrefix, shaderStats));
	auto percent = (float)compiledShaders / (float)totalShaders;
	auto progressOverlay = fmt::format("{}/{} ({:2.1f}%)", compiledShaders, totalShaders, 100 * percent);

#if defined(ENABLE_EFFECTS11)
	uint32_t effectFailed = EffectManager::GetSingleton().IsInitialized() ? EffectManager::GetSingleton().GetFailedEffectCount() : 0;
#else
	uint32_t effectFailed = 0;
#endif

	if (shaderCache->IsCompiling()) {
		// VR immersion: suppress only the routine background-compile readout; the
		// blocking-compile path and anything exceptional still show below.
		const bool hideRoutineHud = globals::game::isVR && shaderCache->backgroundCompilation &&
		                            Menu::GetSingleton()->GetSettings().HideCompilationHUDInVR;
		const bool hasExceptionalInfo = shaderCache->IsDiskCacheHeld() || FeatureIssues::HasFeatureIssues() ||
		                                (failed && !hide) || renderDocAvailable || state->IsDeveloperMode();
		if (hideRoutineHud && !hasExceptionalInfo)
			return;

		ImGui::SetNextWindowPos(ImVec2(pos, pos));
		if (!ImGui::Begin("ShaderCompilationInfo", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
			ImGui::End();
			return;
		}
		if (!hideRoutineHud) {
			ImGui::TextUnformatted(progressTitle.c_str());
			ImGui::ProgressBar(percent, ImVec2(0.0f, 0.0f), progressOverlay.c_str());
		}
		if (shaderCache->HasFeatureSetRevertPending()) {
			ImGui::TextColored(themeSettings.StatusPalette.Warning, "%s",
				T("overlay.revert_pending",
					"Previous cache restored.\n"
					"Restart to use it."));
		} else if (shaderCache->HasFeatureSetChanges()) {
			if (shaderCache->HasFeatureSetCacheBackup()) {
				ImGui::TextColored(themeSettings.StatusPalette.Warning, "%s",
					T("overlay.feature_changed_backup",
						"Feature setup changed. Building a new shader cache for this setup.\n"
						"Previous cache saved."));
			} else {
				ImGui::TextColored(themeSettings.StatusPalette.Warning, "%s",
					T("overlay.feature_changed_no_backup",
						"Feature setup changed. Building shaders in memory until the cache can be rebuilt.\n"
						"Previous cache is not available for restore."));
			}
		} else if (shaderCache->IsDiskCacheHeld()) {
			ImGui::TextColored(themeSettings.StatusPalette.Warning, "%s",
				T("overlay.cache_held",
					"Saved shader cache cannot be used.\n"
					"A required feature is missing or failed to load."));
		}
		// Surface bad-install feature problems during the compile itself; otherwise
		// the user only learns from the Feature Issues tab after baking the wrong set.
		if (FeatureIssues::HasFeatureIssues()) {
			const size_t issueCount = FeatureIssues::GetFeatureIssues().size();
			ImGui::TextColored(themeSettings.StatusPalette.Error,
				T("overlay.feature_issues_compiling",
					"WARNING: %zu feature(s) failed to load (bad install or version mismatch).\n"
					"Quit, fix them in the menu's Feature Issues tab, and restart - compiling now\n"
					"bakes the wrong shaders and you will have to recompile after fixing."),
				issueCount);
		}
		if (state->IsDeveloperMode()) {
			int32_t threadLimit = shaderCache->backgroundCompilation ? shaderCache->backgroundCompilationThreadCount : shaderCache->compilationThreadCount;
			int compilationRunning = (int)shaderCache->compilationPool.get_tasks_running();
			int heavyInFlight = shaderCache->GetHeavyTasksInFlight();
			int heavyLimit = static_cast<int>(Util::GetPerformanceCoreCount());
			uint64_t slow = shaderCache->GetSlowTasks();
			uint64_t verySlow = shaderCache->GetVerySlowTasks();
			ImGui::Text(T("overlay.threads_status", "Threads: %d / %d limit | Heavy: %d / %d P-cores | %d workers"),
				compilationRunning,
				threadLimit,
				heavyInFlight,
				heavyLimit,
				(int)shaderCache->compilationPool.get_thread_count());
			if (slow > 0) {
				ImGui::Text(T("overlay.slow_shaders", "Slow shaders: %llu (very slow: %llu)"), slow, verySlow);
			}
		}
		if (!shaderCache->backgroundCompilation && shaderCache->menuLoaded) {
			const char* skipKeyName = keyIdToString(Menu::GetSingleton()->GetSettings().SkipCompilationKey);
			auto skipShadersText = std::vformat(
				T("overlay.skip_compilation", "Press {} to proceed without completing shader compilation. "),
				std::make_format_args(skipKeyName));
			ImGui::TextUnformatted(skipShadersText.c_str());
			ImGui::TextUnformatted(T("overlay.uncompiled_warning", "WARNING: Uncompiled shaders will have visual errors or cause stuttering when loading."));
		}
		if (failed && !hide)
			DrawShaderCompilationFailures(failed, themeSettings);
#if defined(ENABLE_EFFECTS11)
		DrawEffects11Errors(themeSettings);
#endif

		if (renderDocAvailable)
			ImGui::TextColored(themeSettings.StatusPalette.Warning, renderDocInformation.c_str());

		ImGui::End();
		return;
	}

	if ((failed && !hide) || effectFailed) {
		ImGui::SetNextWindowPos(ImVec2(pos, pos));
		if (!ImGui::Begin("ShaderCompilationInfo", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
			ImGui::End();
			return;
		}

		DrawShaderCompilationFailures(failed, themeSettings);
#if defined(ENABLE_EFFECTS11)
		DrawEffects11Errors(themeSettings);
#endif

		if (renderDocAvailable)
			ImGui::TextColored(themeSettings.StatusPalette.Warning, renderDocInformation.c_str());

		ImGui::End();
	} else if (renderDocAvailable) {
		ImGui::SetNextWindowPos(ImVec2(pos, pos));
		if (!ImGui::Begin("ShaderCompilationInfo", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
			ImGui::End();
			return;
		}
		ImGui::TextColored(themeSettings.StatusPalette.Warning, renderDocInformation.c_str());
		ImGui::End();
	}
}

void OverlayRenderer::RenderFeatureOverlays()
{
	// load overlays
	for (Feature* feat : Feature::GetFeatureList()) {
		if (feat && feat->loaded) {
			if (auto* overlay = dynamic_cast<OverlayFeature*>(feat)) {
				overlay->DrawOverlay();
			}
		}
	}
}

void OverlayRenderer::HandleABTesting()
{
	// A/B Testing management
	auto* abTestingManager = ABTestingManager::GetSingleton();
	abTestingManager->Update();

	// Always update test data during TEST phase, regardless of overlay visibility
	if (abTestingManager->IsEnabled()) {
		globals::features::performanceOverlay.UpdateAllShaderTestData();

		// Add A/B test aggregator data collection here
		auto& overlay = globals::features::performanceOverlay;
		auto [mainRows, summaryRows] = overlay.BuildDrawCallRows();
		std::vector<DrawCallRow> allRows = mainRows;
		allRows.insert(allRows.end(), summaryRows.begin(), summaryRows.end());

		// Update the A/B test aggregator with current frame data
		abTestingManager->GetAggregator().OnFrame(allRows);
	}

	// Draw A/B testing overlay
	abTestingManager->DrawOverlayUI();
}

void OverlayRenderer::FinalizeImGuiFrame()
{
	if (auto* menu = Menu::GetSingleton();
		menu && menu->GetSettings().Theme.UseCustomCursor && Util::CursorLoader::GetLoadedCount() > 0) {
		Util::CursorLoader::DrawCustomCursor(*menu);
	}

	ImGui::Render();

	// UI text/icons are thin, high-frequency content -- coarse VRS shading rate
	// (still active from the last 3D pass) pixelates them badly. Force full rate for
	// the rest of this frame's compositing; EarlyPrepass reapplies the coarse pattern
	// next frame.
	if (auto* vrs = VRFeatures::VRVariableRateShading::GetSingleton(); vrs->IsEnabled()) {
		vrs->ForceFullRate(globals::d3d::context);
	}

	// Apply background blur behind ImGui windows before rendering them
	BackgroundBlur::RenderBackgroundBlur();

	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

	// Render the same draw data into the ImGuiVRHelper's panel RTV so the
	// helper can composite our menu as a 3D quad in the HMD. The helper owns
	// VR overlay compositing — Community Shaders no longer submits its own.
	globals::features::vr.RenderHelperToPanel();
}

void OverlayRenderer::RenderFirstTimeSetupOverlay()
{
	if (HomePageRenderer::ShouldShowFirstTimeSetup()) {
		HomePageRenderer::RenderFirstTimeSetupDialog();
	}
}

void OverlayRenderer::RenderShaderBlockingStatus()
{
	auto shaderCache = globals::shaderCache;
	auto state = globals::state;

	if (!state->IsDeveloperMode() || shaderCache->blockedKey.empty()) {
		return;
	}

	const float scale = Util::GetUIScale();
	const float pos = ThemeManager::Constants::OVERLAY_WINDOW_POSITION * scale;

	// Stack below shader compilation window if visible
	float yPos = pos;
	if (auto* shaderWin = ImGui::FindWindowByName("ShaderCompilationInfo")) {
		if (shaderWin->Active) {
			yPos = shaderWin->Pos.y + shaderWin->Size.y + ImGui::GetStyle().ItemSpacing.y;
		}
	}
	// Also stack below water cache overlay if visible
	if (auto* waterWin = ImGui::FindWindowByName("UWCacheCreationInfo")) {
		if (waterWin->Active && waterWin->Pos.y + waterWin->Size.y > yPos) {
			yPos = waterWin->Pos.y + waterWin->Size.y + ImGui::GetStyle().ItemSpacing.y;
		}
	}
	ImGui::SetNextWindowPos(ImVec2(pos, yPos));
	if (!ImGui::Begin("ShaderBlockingInfo", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
		ImGui::End();
		return;
	}

	Util::Text::Error(T("overlay.shader_blocking_active", "Shader Blocking Active"));
	ImGui::Text(T("overlay.blocked_key", "Blocked: %s"), shaderCache->blockedKey.c_str());

	// Try to get more details from active shaders
	auto activeShaders = shaderCache->GetActiveShaders();

	// Find the index of the blocked shader in the active list (or show N/A if not found)
	size_t blockedIndex = 0;
	bool foundBlocked = false;
	for (size_t i = 0; i < activeShaders.size(); ++i) {
		if (activeShaders[i].key == shaderCache->blockedKey) {
			blockedIndex = i + 1;  // 1-based indexing for display
			foundBlocked = true;
			break;
		}
	}

	if (foundBlocked) {
		ImGui::Text(T("overlay.blocked_index", "Index: %zu/%zu"), blockedIndex, activeShaders.size());
	} else {
		ImGui::Text(T("overlay.blocked_index_na", "Index: N/A (%zu active)"), activeShaders.size());
	}

	for (const auto& shader : activeShaders) {
		if (shader.key == shaderCache->blockedKey) {
			ImGui::Text(T("overlay.blocked_shader_detail", "Type: %s | Class: %s | Descriptor: 0x%X"),
				magic_enum::enum_name(shader.shaderType).data(),
				magic_enum::enum_name(shader.shaderClass).data(),
				shader.descriptor);
			break;
		}
	}

	ImGui::End();
}
