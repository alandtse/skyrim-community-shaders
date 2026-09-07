// Shadow rendering operations for LightLimitFix.
// Contains: resource setup, per-frame data copy, and shadow-specific UI.

#include "../LightLimitFix.h"
#include "Deferred.h"
#include "GpuPass.h"
#include "I18n/I18n.h"
#include "Menu/ThemeManager.h"
#include "ShadowCasterInternal.h"
#include "State.h"
#include "Util.h"

// False means no usable descriptors; see the ShadowParam.y sentinel contract
// below (CopyShadowLightData) for what the caller must do with that.
template <typename T>
static bool SetShadowParameters(T& lightData, Deferred::ShadowLightData& sd)
{
	if (lightData.shadowmapDescriptors.empty())
		return false;

	auto& desc = lightData.shadowmapDescriptors[0];
	DirectX::XMMATRIX proj = DirectX::XMLoadFloat4x4(reinterpret_cast<const DirectX::XMFLOAT4X4*>(&desc.lightTransform));
	DirectX::XMStoreFloat4x4(&sd.ShadowProj, proj);

	DirectX::XMMATRIX invProj = DirectX::XMMatrixInverse(nullptr, proj);
	DirectX::XMStoreFloat4x4(&sd.InvShadowProj, invProj);

	sd.ShadowParam.z = lightData.shadowBiasScale * 0.00025f;
	return true;
}

// ─── Per-frame shadow data copy ───────────────────────────────────────────────

void LightLimitFix::EarlyPrepass()
{
	CopyShadowLightData();
}

void LightLimitFix::CopyShadowLightData()
{
	ZoneScoped;
	CS_GPU_PASS("LightLimitFix::CopyShadowLightData");

	// Shared by every early-out below: clears slot state and unbinds t102 so
	// overlay/shaders degrade to unshadowed instead of stale prior-frame data.
	auto degradeToUnshadowed = [this]() {
		ShadowCasterManager::BeginSlotFrame(0);
		shadowLightCount = 0;
		shadowUnshadowedLightCount = 0;
		ID3D11ShaderResourceView* nullSRVs[2]{ nullptr, nullptr };
		globals::d3d::context->PSSetShaderResources(102, ARRAYSIZE(nullSRVs), nullSRVs);
	};

	// A pending teardown (ClearLightArrays) frees shadow lights/GPU resources;
	// iterating shadowLightsAccum or binding SRVs here can deref freed memory
	// (the cell-transition CTD). Degrade to unshadowed until the reset drains.
	if (ShadowCasterManager::IsSessionResetPending()) {
		degradeToUnshadowed();
		return;
	}

	uint32_t slots = ShadowCasterManager::GetInstalledSlotCount();
	if (slots == 0) {
		degradeToUnshadowed();
		return;
	}

	auto* shadowSceneNode = globals::game::smState->shadowSceneNode[0];
	if (!shadowSceneNode) {
		degradeToUnshadowed();
		return;
	}

	// Lazy (re)allocation when slot count changes (e.g. on resolution change).
	if (!shadowLights || shadowLightsCapacity != slots) {
		delete shadowLights;
		shadowLights = nullptr;

		D3D11_BUFFER_DESC sbDesc{};
		sbDesc.Usage = D3D11_USAGE_DYNAMIC;
		sbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		sbDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		sbDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		sbDesc.StructureByteStride = sizeof(Deferred::ShadowLightData);
		sbDesc.ByteWidth = slots * sizeof(Deferred::ShadowLightData);

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = slots;

		shadowLights = new Buffer(sbDesc, nullptr, "LLF::ShadowLights");
		shadowLights->CreateSRV(srvDesc);
		shadowLightsCapacity = slots;
	}

	// Static + assign(), not a fresh vector(slots) each call: avoids a
	// heap-alloc every frame in this render hot path when slot count is stable.
	static std::vector<Deferred::ShadowLightData> sd;
	sd.assign(slots, {});
	uint32_t prevSlotUsage = ShadowCasterManager::GetSlotUsage();
	ShadowCasterManager::BeginSlotFrame(slots);
	auto context = globals::d3d::context;

	ID3D11ShaderResourceView* shadowMapsSRV =
		globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGET_DEPTHSTENCIL::kSHADOWMAPS].depthSRV;

	uint32_t plCount = 0;
	uint32_t unshadowedLights = 0;
	// The sun's accumulate doesn't advance shadowLightsAccum's stride like a
	// point light's, so ForEachShadowLight can silently drop slot(s) after it;
	// the backstop below re-sources any pool-occupied slot the walk missed.
	static std::vector<bool> visited;
	visited.assign(slots, false);
	auto visitLight = [&](RE::BSShadowLight* light) {
		// Stable container-slot index, not shadowmapDescriptors[0].shadowmapIndex,
		// which can drift if ReturnShadowmaps fires between scheduling and here.
		int32_t stableSlot = ShadowCasterManager::GetShadowSlot(light);
		if (stableSlot < 0) {
			// Sun (BSShadowDirectionalLight): sampled via a separate path
			// (DirectionalShadowCascades), no kSHADOWMAPS slice here.
			return;
		}
		if (static_cast<uint32_t>(stableSlot) >= slots) {
			unshadowedLights++;
			plCount++;
			return;
		}
		uint32_t depthSlot = static_cast<uint32_t>(stableSlot);
		visited[depthSlot] = true;

		auto* ni = light->light.get();

		{
			float shadowTypeF = light->GetIsParabolicLight() ? float(light->shadowMapCount == 2 ? 2 : 1) : 0.f;
			sd[depthSlot].ShadowParam.x = shadowTypeF;

			const bool projValid = globals::game::isVR ?
			                           SetShadowParameters(light->GetVRRuntimeData(), sd[depthSlot]) :
			                           SetShadowParameters(light->GetRuntimeData(), sd[depthSlot]);

			// No NiLight means no radius; range 0 drives the safe sentinel below.
			float range = ni ? ni->GetLightRuntimeData().radius.x : 0.0f;
			// ShadowParam.y: >0 valid radius (sample kSHADOWMAPS), ==0 safe-lit
			// sentinel, <0 suppression sentinel (fully dark). Must stay 0 when
			// projValid is false, or the shader samples a stale zero projection as shadowed.
			uintptr_t lightKey = reinterpret_cast<uintptr_t>(light);
			const bool suppressed = ShadowCasterManager::IsSuppressed(lightKey);
			sd[depthSlot].ShadowParam.y = suppressed ? -1.0f : (projValid ? range : 0.0f);
			// ShadowParam.w: rasterized tile scale; shader treats <=0 as full slice,
			// so zero-filled slots or a mismatched DLL/shader build fall back to vanilla sampling.
			sd[depthSlot].ShadowParam.w = ShadowCasterManager::GetRenderedTileScale(stableSlot);
			// FadeParam.x: 0 = just gained a shadow, 1 = fade complete; only read
			// past the ShadowParam.y sentinel checks, so harmless here for suppressed slots.
			sd[depthSlot].FadeParam.x = ShadowCasterManager::GetShadowFadeAlpha(stableSlot);
			// AtlasRect: advertise the tile only once it holds rendered
			// content; zero keeps the shader on the array-slice path.
			if (ShadowCasterManager::AtlasActive()) {
				ShadowCasterManager::AtlasRectUV rect{};
				if (ShadowCasterManager::GetSlotAtlasRectUV(stableSlot, rect)) {
					sd[depthSlot].AtlasRect = float4{ rect.scaleX, rect.scaleY, rect.biasX, rect.biasY };
					// Must come from the SAME tile as the rect -- per-light renderedScale
					// can go stale across reallocs, and a mismatched class bias on a small tile causes acne.
					sd[depthSlot].ShadowParam.w = rect.classScale;
					// Between redraws, advertise the tile's baked radius/bias, not the
					// live light's -- otherwise flame flicker drifts against stale depth as pulsing false occlusion.
					if (sd[depthSlot].ShadowParam.y > 0.0f) {
						ShadowCasterManager::ShadowBakeSnapshot snap{};
						if (ShadowCasterManager::SlotBakeSnapshotPending(stableSlot)) {
							snap.radius = sd[depthSlot].ShadowParam.y;
							snap.bias = sd[depthSlot].ShadowParam.z;
							ShadowCasterManager::StoreSlotBakeSnapshot(stableSlot, snap);
						} else if (ShadowCasterManager::LoadSlotBakeSnapshot(stableSlot, snap)) {
							sd[depthSlot].ShadowParam.y = snap.radius;
							sd[depthSlot].ShadowParam.z = snap.bias;
						}
					}
				} else if (sd[depthSlot].ShadowParam.y > 0.0f) {
					// No rendered tile behind this slot; atlas mode never writes the
					// engine slices, so force the safe sentinel instead of sampling
					// stale array depth (suppressed lights keep their -1 sentinel).
					sd[depthSlot].ShadowParam.y = 0.0f;
				}
			}
			// Name resolved once per NiLight (owner ref, then scenegraph node,
			// then form ID) to identify the light for the diagnostics table.
			static std::unordered_map<const RE::NiLight*, std::string> s_lightNames;
			ShadowCasterManager::PruneIfOversized(s_lightNames, 1024);
			std::string lightName;
			if (ni) {
				auto [nameIt, nameNew] = s_lightNames.try_emplace(ni);
				if (nameNew) {
					if (auto* ref = ni->GetUserData()) {
						if (auto* base = ref->GetObjectReference()) {
							const char* n = base->GetName();
							nameIt->second = (n && n[0]) ? n : std::format("{:08X}", ref->GetFormID());
						}
					}
					if (nameIt->second.empty() && !ni->name.empty())
						nameIt->second = ni->name.c_str();
				}
				lightName = nameIt->second;
			}
			ShadowCasterManager::RecordSlot(depthSlot,
				{ static_cast<uint32_t>(shadowTypeF), range, true, lightKey, sd[depthSlot].ShadowParam.y, std::move(lightName) });
		}

		plCount++;
	};
	ShadowCasterManager::ForEachShadowLight(shadowSceneNode->GetRuntimeData().shadowLightsAccum, visitLight);

	// Backstop for the silent-drop bug above: sources any pool slot SCM believes
	// occupied but the walk missed, from the pool's own light pointer. Registers
	// no engine pass and frees nothing, so none of EnableLight's freed-pass-group risk.
	{
		const auto& pool = ShadowCasterManager::GetLights();
		const int32_t first = pool.PointLightFirst();
		const int32_t end = std::min(static_cast<int32_t>(slots), pool.Size);
		for (int32_t i = first; i < end; i++) {
			if (visited[i] || !pool.Lights[i].Light)
				continue;
			visitLight(pool.Lights[i].Light);
		}
	}

	if (plCount != shadowLightCount || ShadowCasterManager::GetSlotUsage() != prevSlotUsage || unshadowedLights != shadowUnshadowedLightCount) {
		shadowLightCount = plCount;
		shadowUnshadowedLightCount = unshadowedLights;

		// Throttled: logs only on a significant count move (>= 4, or any
		// unshadowed-count change) and at most once per second.
		static int s_lastLoggedShadowCount = -1;
		static uint32_t s_lastLoggedUnshadowed = 0;
		static LARGE_INTEGER s_lastLogQpc = { .QuadPart = 0 };
		static LARGE_INTEGER s_qpcFrequency = []() {
			LARGE_INTEGER f{};
			QueryPerformanceFrequency(&f);
			return f;
		}();
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		const bool unshadowedChanged = unshadowedLights != s_lastLoggedUnshadowed;
		const bool countSignificant = s_lastLoggedShadowCount < 0 ||
		                              std::abs(static_cast<int>(plCount) - s_lastLoggedShadowCount) >= 4;
		const bool rateOk = (now.QuadPart - s_lastLogQpc.QuadPart) >= s_qpcFrequency.QuadPart;
		if ((countSignificant || unshadowedChanged) && rateOk) {
			s_lastLoggedShadowCount = static_cast<int>(plCount);
			s_lastLoggedUnshadowed = unshadowedLights;
			s_lastLogQpc = now;
			if (unshadowedLights > 0)
				logger::debug("[LLF] {} shadow lights, {} / {} slots used; {} lights dropped (no shadow)",
					plCount, ShadowCasterManager::GetSlotUsage(), slots, unshadowedLights);
			else
				logger::debug("[LLF] {} shadow lights, {} / {} slots used", plCount, ShadowCasterManager::GetSlotUsage(), slots);
		}
	}

	{
		D3D11_MAPPED_SUBRESOURCE mapped{};
		DX::ThrowIfFailed(context->Map(shadowLights->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
		memcpy(mapped.pData, sd.data(), slots * sizeof(Deferred::ShadowLightData));
		context->Unmap(shadowLights->resource.get(), 0);
		ID3D11ShaderResourceView* srv = shadowLights->srv.get();
		context->PSSetShaderResources(102, 1, &srv);
	}

	context->PSSetShaderResources(103, 1, &shadowMapsSRV);

	ID3D11ShaderResourceView* atlasSRV = ShadowCasterManager::AtlasSRV();
	context->PSSetShaderResources(104, 1, &atlasSRV);
}

// ─── Debug helpers ────────────────────────────────────────────────────────────

std::string LightLimitFix::BuildShadowSlotColorLegend() const
{
	const auto& shadowSlotInfos = ShadowCasterManager::GetSlotInfos();
	if (shadowSlotInfos.empty())
		return {};

	std::string out = "Shadow Slot Color Map (Mode 8):\n";
	for (uint32_t i = 0; i < static_cast<uint32_t>(shadowSlotInfos.size()); ++i) {
		const auto& info = shadowSlotInfos[i];
		if (!info.valid)
			continue;

		float hue = fmodf(float(i) * 0.618033988f, 1.0f);
		ImVec4 c = ShadowCasterManager::ShadowSlotHueColor(i);
		auto ri = static_cast<uint8_t>(c.x * 255.0f);
		auto gi = static_cast<uint8_t>(c.y * 255.0f);
		auto bi = static_cast<uint8_t>(c.z * 255.0f);

		out += std::format("  Slot {:2d} | hue {:5.3f} | #{:02X}{:02X}{:02X} | {:11s} | r={:.0f}\n",
			i, hue, ri, gi, bi, ShadowCasterManager::GetShadowTypeName(info.type), info.range);
	}
	return out;
}

// ─── Overlay ─────────────────────────────────────────────────────────────────

void LightLimitFix::DrawOverlay()
{
	// Auto-shown for viz modes, suppressed lights, or debug overrides (so users
	// can find what they pinned); Show Shadow Overlay lets the user opt in directly.
	bool vizOn = EnableLightsVisualisation;
	bool hasSuppressed = ShadowCasterManager::HasSuppressedLights();
	bool hasOverrides = ShadowCasterManager::HasAnyOverrides();
	bool showOverlay = settings.ShowShadowOverlay;
	if (!vizOn && !hasSuppressed && !hasOverrides && !showOverlay)
		return;

	// Menu open: draggable/resizable window so the user can reposition/expand it.
	// Menu closed: compact pinned overlay with no title bar/chrome.
	bool menuOpen = globals::menu->IsEnabled;
	const float pos = ThemeManager::Constants::OVERLAY_WINDOW_POSITION * Util::GetUIScale();

	// Same ImGui ID across menu open/closed so the user's resize persists;
	// title bar/move are toggled via flags instead. No NoSavedSettings, so
	// ImGui retains the picked size across sessions.
	ImGuiWindowFlags flags = ImGuiWindowFlags_None;
	if (!menuOpen)
		flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove;

	ImGui::SetNextWindowPos(ImVec2(pos, pos), menuOpen ? ImGuiCond_Appearing : ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(340, 480), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSizeConstraints(ImVec2(280, 200), ImVec2(800, 1200));
	ImGui::Begin("LLF Shadow Slots", nullptr, flags);

	if (vizOn) {
		static const char* kVizNames[] = {
			"Light Limit", "Strict Lights Count", "Clustered Lights Count",
			"Shadow Mask", "Shadow Light Count", "Point Light Shadow Factor",
			"Unshadowed Point Lights", "Shadow Caster Density",
			"Shadow Slot Index Color", "Light Type Visualization"
		};
		uint32_t m = LightsVisualisationMode;
		const char* vizName = (m < IM_ARRAYSIZE(kVizNames)) ? kVizNames[m] : "Unknown";
		ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), T("feature.light_limit_fix.overlay_debug_label", "LLF DEBUG - %s"), vizName);
	} else
		ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "%s", T("feature.light_limit_fix.overlay_shadow_suppression", "LLF - Shadow Suppression"));
	ImGui::Separator();

	uint32_t mode = vizOn ? LightsVisualisationMode : UINT32_MAX;

	// ── All stats grouped above the table (same order as menu) ─────────
	// Summary always visible; scheduler stats only when not in a viz mode
	// that already draws its own legend in the same space.
	ShadowCasterManager::DrawShadowSummary(lightCount, MAX_LIGHTS, shadowUnshadowedLightCount);
	if (!vizOn)
		ShadowCasterManager::DrawShadowSchedulerStats();

	// ── Per-mode informational panels (visualization-mode-specific only) ──
	if (vizOn) {
		if (mode == 2) {
			uint32_t cx = clusterSize[0], cy = clusterSize[1], cz = clusterSize[2];
			ImGui::Text("Cluster grid       : %ux%ux%u  (%u total)", cx, cy, cz, cx * cy * cz);
			ImGui::Text("Max lights/cluster : %u", CLUSTER_MAX_LIGHTS);
		} else if (mode >= 3) {
			ShadowCasterManager::DrawOverlayShadowModeInfo(mode, shadowUnshadowedLightCount, lightCount);
		}
	}

	// ── Shadow slot toggle table ─────────────────────────────────────
	// Shown for shadow-related viz modes or suppressed lights. readOnly when
	// the menu is closed avoids dead-pixel buttons and bounds table height
	// so the stats above stay visible (table scrolls internally instead).
	bool shadowRelatedMode = !vizOn || (mode >= 4);
	// Also shown for Show Shadow Overlay / overrides -- the tooltip promises
	// debug controls stay reachable, but viz modes 0-3 leave shadowRelatedMode false.
	if (showOverlay || hasOverrides || hasSuppressed || shadowRelatedMode) {
		ImGui::Separator();
		// compact=false: table fills the remaining region and scrolls internally
		// (ScrollY in ShowSortedStringTableCustom), keeping stats above visible
		// regardless of light count or window size; readOnly stays true when menu closed.
		ShadowCasterManager::DrawShadowLightTable(false, vizOn && (mode == 8), true, !menuOpen);
	}

	ImGui::End();
}
