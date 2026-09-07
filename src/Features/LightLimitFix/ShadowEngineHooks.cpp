// ShadowEngineHooks.cpp
// Every game-engine touchpoint of the shadow caster scheduler: hook thunks, engine accessors, and Install().

#include <atomic>

#include "../../Deferred.h"
#include "../../Globals.h"
#include "../../GpuPass.h"
#include "../../State.h"
#include "../../Utils/Game.h"
#include "../../Utils/UI.h"
#include "../Upscaling.h"
#include "../VR.h"
#include "I18n/I18n.h"
#include "ShadowCasterInternal.h"

#include <Windows.h>  // CONTEXT for the register-context hook thunks

namespace ShadowCasterManager
{
	// =========================================================================
	// Helpers for depth-target index globals
	// SE: 14304EEE8 / AE: n/a (adjacent) / VR: 143180df0
	// =========================================================================
	static int32_t GetDepthTargetType()
	{
		static REL::RelocationID uid(524780, 388826);
		return *reinterpret_cast<int32_t*>(uid.address());
	}

	static int32_t GetDepthTargetSubIndex()
	{
		static REL::RelocationID uid(524780, 388826);
		return *reinterpret_cast<int32_t*>(uid.address() + 4);
	}

	// =========================================================================
	// Hook implementations
	// =========================================================================

	// -------------------------------------------------------------------------
	// Expanded accumulated-lights array
	// The game allocates a local array sized for 8 lights (with +1 sentinel).
	// When using more than 8 shadow casters we extend RDI (SE) / RBX (AE/VR)
	// which is the loop-end counter, and RDX (SE) which is the copy-end counter.
	// -------------------------------------------------------------------------
	static void Hook_AccumulatedLightsArray(CONTEXT& ctx)
	{
		int needed = (s_settings.ShadowLightCount + s_settings.ConvertedShadowSlots + 1) * 2;
		int have = 10;  // game default: (4+1)*2
		int extra = needed - have;
		if (extra > 0) {
			ctx.Rdi += extra;
			// SE/VR latch EDX from EDI inside the patched 5 bytes (before the
			// stub captures CONTEXT), so BSTArray::resize runs with the un-
			// bumped count while the fill loop runs with the bumped one --
			// OOB heap write scaling with ShadowLightCount. AE inlines the
			// resize and re-reads EDI after the stub, so RDX is dead here.
			if (!REL::Module::IsAE())
				ctx.Rdx += extra;
		}
	}

	// -------------------------------------------------------------------------
	// Redirect depth-stencil-view creation to our extended arrays
	// The game loops 0..7 creating depth stencil views and stores each pointer
	// in a game-managed struct at R9.  We redirect R9 to our own arrays so
	// views >= 8 land in globals::features::llf::normalDepthBuffer / globals::features::llf::readOnlyDepthBuffer.
	// -------------------------------------------------------------------------
	static void Hook_CreateNormalDepthBuffer(CONTEXT& ctx)
	{
		// R12 (SE/AE) or R13 (VR) holds a_target * 0x13; value 4*19=76 identifies
		// the shadow-map depth target.  RDI (SE) / RBX (AE/VR) is the loop index.
		if (REL::Relocate(ctx.R12, ctx.R12, ctx.R13) != 4 * 19)
			return;
		int idx = (int)REL::Relocate(ctx.Rdi, ctx.Rbx, ctx.Rbx);
		ctx.R9 = reinterpret_cast<DWORD64>(&globals::features::llf::normalDepthBuffer[idx]);
	}

	static void Hook_CreateReadOnlyDepthBuffer(CONTEXT& ctx)
	{
		if (REL::Relocate(ctx.R12, ctx.R12, ctx.R13) != 4 * 19)
			return;
		int idx = (int)REL::Relocate(ctx.Rdi, ctx.Rbx, ctx.Rbx);
		ctx.R9 = reinterpret_cast<DWORD64>(&globals::features::llf::readOnlyDepthBuffer[idx]);
	}

	// -------------------------------------------------------------------------
	// Copy first 8 views into the game's own DepthStencilData array
	// Called after the creation loop finishes; syncs the game struct so existing
	// code reading depthStencils[4].views[0..7] still works correctly.
	// -------------------------------------------------------------------------
	static void Hook_SetupGameArray(CONTEXT& ctx)
	{
		if (REL::Relocate(ctx.R12, ctx.R12, ctx.R13) != 4 * 19)
			return;
		auto* renderer = reinterpret_cast<RE::BSGraphics::Renderer*>(ctx.R15);
		for (int i = 0; i < 8; i++) {
			renderer->GetDepthStencilData().depthStencils[4].views[i] = reinterpret_cast<ID3D11DepthStencilView*>(globals::features::llf::normalDepthBuffer[i]);
			renderer->GetDepthStencilData().depthStencils[4].readOnlyViews[i] = reinterpret_cast<ID3D11DepthStencilView*>(globals::features::llf::readOnlyDepthBuffer[i]);
		}
	}

	// -------------------------------------------------------------------------
	// Redirect depth-buffer selection at draw time
	// When the active depth target is type 4 (shadow maps), route sub-index
	// lookups through our extended arrays instead of the game struct.
	// Hook #1: renderer in R8, result -> RBX.
	// -------------------------------------------------------------------------
	static void Hook_SelectDepthBuffer1(CONTEXT& ctx)
	{
		auto* data = reinterpret_cast<RE::BSGraphics::RendererData*>(ctx.R8);
		int type = GetDepthTargetType();
		int sub = GetDepthTargetSubIndex();

		if (type == 4 && AtlasActive()) {
			// All type-4 rendering while SCM owns scheduling is point/spot
			// cascades; they share the one atlas DSV and select their region
			// via the tile viewport. During a static-cache bake pass the same
			// tile region is redirected into the parallel static atlas instead.
			ctx.Rbx = reinterpret_cast<DWORD64>(StaticPassRedirectActive() ?
													StaticAtlasDSV(data->readOnlyDepth) :
													AtlasDSV(data->readOnlyDepth));
		} else if (type == 4 && globals::features::llf::normalDepthBuffer) {
			ctx.Rbx = data->readOnlyDepth ? reinterpret_cast<DWORD64>(globals::features::llf::readOnlyDepthBuffer[sub]) : reinterpret_cast<DWORD64>(globals::features::llf::normalDepthBuffer[sub]);
		} else {
			ctx.Rbx = data->readOnlyDepth ? reinterpret_cast<DWORD64>(RE::BSGraphics::Renderer::GetSingleton()->GetDepthStencilData().depthStencils[type].readOnlyViews[sub]) : reinterpret_cast<DWORD64>(RE::BSGraphics::Renderer::GetSingleton()->GetDepthStencilData().depthStencils[type].views[sub]);
		}
	}

	// Hook #2: VR: renderer in R14, result -> RBP; SE/AE: renderer in RBP, result -> R14.
	static void Hook_SelectDepthBuffer2(CONTEXT& ctx)
	{
		bool isVR = globals::game::isVR;
		bool readOnly = isVR ? reinterpret_cast<RE::BSGraphics::Renderer*>(ctx.R14)->GetRuntimeData().readOnlyDepth : reinterpret_cast<RE::BSGraphics::Renderer*>(ctx.Rbp)->GetRuntimeData().readOnlyDepth;

		int type = GetDepthTargetType();
		int sub = GetDepthTargetSubIndex();

		DWORD64 result;
		if (type == 4 && AtlasActive()) {
			result = reinterpret_cast<DWORD64>(StaticPassRedirectActive() ?
												   StaticAtlasDSV(readOnly) :
												   AtlasDSV(readOnly));
		} else if (type == 4 && globals::features::llf::normalDepthBuffer) {
			result = readOnly ? reinterpret_cast<DWORD64>(globals::features::llf::readOnlyDepthBuffer[sub]) : reinterpret_cast<DWORD64>(globals::features::llf::normalDepthBuffer[sub]);
		} else {
			result = readOnly ? reinterpret_cast<DWORD64>(RE::BSGraphics::Renderer::GetSingleton()->GetDepthStencilData().depthStencils[type].readOnlyViews[sub]) : reinterpret_cast<DWORD64>(RE::BSGraphics::Renderer::GetSingleton()->GetDepthStencilData().depthStencils[type].views[sub]);
		}

		if (isVR)
			ctx.Rbp = result;
		else
			ctx.R14 = result;
	}

	// -------------------------------------------------------------------------
	// Release extended depth buffers at renderer shutdown
	// -------------------------------------------------------------------------
	static void ReleaseExtendedDepthBuffers(int shadowCount)
	{
		for (int i = 8; i < shadowCount; i++) {
			if (globals::features::llf::normalDepthBuffer[i]) {
				reinterpret_cast<ID3D11DepthStencilView*>(globals::features::llf::normalDepthBuffer[i])->Release();
				globals::features::llf::normalDepthBuffer[i] = nullptr;
			}
			if (globals::features::llf::readOnlyDepthBuffer[i]) {
				reinterpret_cast<ID3D11DepthStencilView*>(globals::features::llf::readOnlyDepthBuffer[i])->Release();
				globals::features::llf::readOnlyDepthBuffer[i] = nullptr;
			}
		}
	}

	static void Hook_DeleteDepthBuffers_SE(CONTEXT& ctx)
	{
		// Only fire when RBX points at depthStencils[4], not at other delete calls.
		auto* data = reinterpret_cast<RE::BSGraphics::DepthStencilData*>(ctx.Rbx);
		if (data == &RE::BSGraphics::Renderer::GetSingleton()->GetDepthStencilData().depthStencils[4])
			ReleaseExtendedDepthBuffers(s_settings.ShadowLightCount);
	}

	static void Hook_DeleteDepthBuffers_AE(CONTEXT& /*ctx*/)
	{
		ReleaseExtendedDepthBuffers(s_settings.ShadowLightCount);
	}

	// -------------------------------------------------------------------------
	// Force each light to use its assigned shadow map slot
	// RenderCascade would otherwise recalculate a slot index from a global
	// counter, causing lights that weren't re-rendered this frame to corrupt
	// each other's shadow maps.
	// SE: light pointer in R15, slot index out in RSI.
	// VR: light pointer in R14, slot index out in RDX.
	// -------------------------------------------------------------------------
	static void Hook_OverwriteShadowMapIndex(CONTEXT& ctx)
	{
		// Enabled is a boot-time gate (see Init early-return) -- this
		// hook is only installed when SCM is enabled at boot, so it
		// runs unconditionally per-frame from there. Toggling Enabled
		// off at runtime no longer affects the hook; restart is the
		// only safe way to revert. See Hook_CalculateActiveShadowCasters
		// comment for the crash rationale.

		auto* light = reinterpret_cast<RE::BSShadowLight*>(REL::Relocate(ctx.R15, ctx.R15, ctx.R14));
		int32_t idx = s_lights.FindLight(light, s_settings.ShadowLightCount);
		if (idx < 0)
			idx = 0;  // should not happen; fail-safe to slot 0
		// This hook runs inside BSShadowParabolicLight::RenderCascade's
		// `renderTarget == kNONE` block, so it only fires for point/spot
		// lights (the sun's RenderShadowmaps presets renderTarget to 2/3/4
		// before each call, skipping the block). FindLight must therefore
		// cover the same range as FindFreeIndex; a mismatch means a light
		// silently gets idx=0 and corrupts the slot at index 0.

		if (globals::game::isVR)
			ctx.Rdx = static_cast<DWORD64>(idx);
		else
			ctx.Rsi = static_cast<DWORD64>(idx);
	}

	// The engine recomputes the shadow viewport per cascade recompute, so the
	// tile scale is re-derived fresh each time from the bound slice, not cached.
	struct Hook_UpdateViewPort
	{
		static void thunk(RE::BSGraphics::Renderer* a_renderer, std::uint32_t a_width, std::uint32_t a_height, bool a_disableScale)
		{
			func(a_renderer, a_width, a_height, a_disableScale);
			const bool atlas = AtlasActive();
			if (!atlas)
				return;
			auto* state = globals::game::shadowState;
			if (!state || ShadowField(state, depthStencil) != RE::RENDER_TARGET_DEPTHSTENCIL::kSHADOWMAPS)
				return;
			const auto slice = static_cast<int32_t>(ShadowField(state, depthStencilSlice));
			if (slice < s_lights.PointLightFirst() || slice >= s_lights.PointLightEnd(s_settings.ShadowLightCount))
				return;
			auto& viewPort = ShadowField(state, viewPort);
			if (atlas) {
				// The cascade's SRTM_CLEAR would wipe every other tile on the
				// shared atlas; kill it before UpdateRenderTargetsAndStates
				// issues it (D3D-level intercepts miss interposed contexts).
				ShadowField(state, setDepthStencilMode) = RE::BSGraphics::SRTM_NO_CLEAR;
				// Map the engine rect (fractions of the slice, encoding the
				// paraboloid half) into the slot's atlas tile. Runs for EVERY
				// class -- full-size lights need their tile offset too.
				AtlasTileTexels tile{};
				const float sliceDim = static_cast<float>(AtlasBaseTile());
				if (GetSlotTileTexels(slice, tile) && sliceDim > 0.0f) {
					const float inv = 1.0f / sliceDim;
					viewPort.TopLeftX = tile.x + viewPort.TopLeftX * inv * tile.size;
					viewPort.TopLeftY = tile.y + viewPort.TopLeftY * inv * tile.size;
					viewPort.Width = viewPort.Width * inv * tile.size;
					viewPort.Height = viewPort.Height * inv * tile.size;
				} else {
					// No tile behind this slice: collapse the viewport so a
					// stray raster clips to nothing instead of stomping other
					// lights' tiles in the shared atlas.
					viewPort.Width = 0.0f;
					viewPort.Height = 0.0f;
				}
				return;
			}
			const float scale = s_lights.Lights[slice].pendingScale;
			if (scale <= 0.0f || scale >= 1.0f)
				return;
			viewPort.TopLeftX *= scale;
			viewPort.TopLeftY *= scale;
			viewPort.Width *= scale;
			viewPort.Height *= scale;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// Vanilla's RenderShadowLightsWithUtilityShader indexes a hard-coded
	// 4-entry table by BSShadowLight::maskIndex, which SLF's extended
	// scheduler can push past 4 (OOB read) or leave uninitialized for
	// focus-shadow lights -- skip it entirely rather than bound it.
	// LIGHT_LIMIT_FIX doesn't consume the mask (shaders read
	// GetDirectionalShadow / GetShadowLightShadow directly), so this loses
	// no functionality. Do NOT call ReturnShadowmaps here: it clears
	// shadowmapDescriptors and breaks the cascade matrix upload.
	struct Hook_RenderShadowLightsWithUtilityShader
	{
		static void thunk()
		{
			(void)func;  // suppress "unused" warning while keeping the relocation
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// BSBatchRenderer::StartGroupingAlphas bump-allocates a global array with no
	// capacity check; an extended shadow pool's demand can exceed it, AV'ing on
	// adjacent .rdata read as bogus `this`. Return null: callers already handle it.
	static std::uint32_t* s_alphaGroupCount = nullptr;
	static std::uint32_t s_alphaGroupLimit = 0;

	struct Hook_StartGroupingAlphas
	{
		static void* thunk(RE::BSBatchRenderer* a_this, void* a_bound, RE::NiCamera* a_camera,
			bool a_sortByClosestPoint)
		{
			if (s_alphaGroupCount && a_camera) {
				const std::uint32_t live = *s_alphaGroupCount;
				// High-water mark; CAS so a concurrent worker cannot lose a higher peak.
				std::uint32_t seen = s_alphaGroupPeak.load(std::memory_order_relaxed);
				while (live > seen && !s_alphaGroupPeak.compare_exchange_weak(
										  seen, live, std::memory_order_relaxed)) {
				}
				if (live >= s_alphaGroupLimit) {
					const uint64_t n = s_alphaGroupDrops.fetch_add(1, std::memory_order_relaxed) + 1;
					if (n == 1u || (n % 10000u) == 0u)
						logger::warn("[SCM] Alpha GeometryGroup ceiling reached ({} live, {} refused)", live, n);
					return nullptr;
				}
			}
			return func(a_this, a_bound, a_camera, a_sortByClosestPoint);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// =========================================================================
	// Game accessor helpers
	//
	// Thin wrappers around game globals and engine functions.
	// All REL::RelocationID pairs are (SE_id, AE_id).
	// VR addresses verified against the VR address library CSV.
	// =========================================================================

	// ---------- globals ----------

	RE::ShadowSceneNode* GetShadowSceneNode()
	{
		static REL::RelocationID uid(513211, 390951);
		return *reinterpret_cast<RE::ShadowSceneNode**>(uid.address());
	}

	RE::NiCamera* GetWorldCamera()
	{
		// world scene graph -> camera
		static REL::RelocationID uid(528087, 415032);
		auto* sg = *reinterpret_cast<RE::BSSceneGraph**>(uid.address());
		return sg ? sg->GetRuntimeData().camera.get() : nullptr;
	}

	// True while an interior cell's BSPortalGraph is transiently null mid-transition. Engine
	// portal accumulation derefs ssn->portalGraph (+0x228) unguarded, so we pause scheduling
	// for the 1-2 null frames. Interior-only: exteriors legitimately have no portal graph.
	bool IsPortalGraphTransitioning()
	{
		if (!Util::IsInterior())
			return false;
		auto* ssn = GetShadowSceneNode();
		return !ssn || ssn->GetRuntimeData().portalGraph == nullptr;
	}

	static bool GetSunBool1()
	{
		static REL::RelocationID uid(513201, 390932);
		return *reinterpret_cast<bool*>(uid.address());
	}
	// Engine's per-frame count of focus shadow actors (player + tracked NPCs);
	// max is iNumFocusShadow:Display (default 4). The engine renders one
	// high-resolution shadow per entry into kSHADOWMAPS slots
	// [g_focusShadowBaseSlotIndex .. +count). Used by the scheduler to
	// dynamically reserve that range out of the point-light pool.
	int GetFocusShadowActorCount()
	{
		static REL::RelocationID uid(527703, 414625);
		return *reinterpret_cast<int*>(uid.address());
	}
	bool GetSunBool2()
	{
		static REL::RelocationID uid(528095, 415040);
		return *reinterpret_cast<bool*>(uid.address());
	}

	// Recompute the engine's cached shadow-cull square from the live shadow-distance
	// settings; the engine self-refreshes it only on a cell transition, so a slider
	// edit otherwise wouldn't reach the cull until a reload.
	void CallUpdateShadowDistance(bool a_interior)
	{
		static REL::Relocation<void(bool)> fn{ REL::RelocationID(98978, 105631) };
		fn(a_interior);
	}

	// Engine's current light LOD fade-out distance (squared), recomputed each frame
	// by Sky::UpdateLightLODFadeDistances with interior-cell / weather overrides.
	static float GetLightLODEndFadeSquared()
	{
		static REL::Relocation<float*> p{ REL::RelocationID(527669, 414583) };
		return *p;
	}

	// Engine shadow-cull distance cache, compared against (dist²-radius²) in the
	// per-frame light cull. The engine recomputes it only on a cell transition, so a
	// per-frame writer must re-apply every frame to stay live.
	static float& ShadowDistanceCurrent()
	{
		static REL::Relocation<float*> p{ REL::RelocationID(528314, 415263) };
		return *p;
	}
	static float& ShadowDistanceSquaredCurrent()
	{
		static REL::Relocation<float*> p{ REL::RelocationID(528316, 415264) };
		return *p;
	}

	// Couple the point-light shadow-cull distance to the light fade-out distance so a
	// caster's shadow lasts as long as its light stays lit (#161 pop-in). Re-apply each
	// frame: the engine refreshes this cache only on a cell transition.
	void ApplyShadowToLightFadeMatch()
	{
		if (!s_settings.MatchShadowToLightFade)
			return;
		const float endSq = GetLightLODEndFadeSquared();
		if (!std::isfinite(endSq) || endSq <= 0.0f)
			return;  // light fade disabled / not yet computed -- leave the cull as-is
		auto* prefColl = RE::INIPrefSettingCollection::GetSingleton();
		if (!prefColl)
			return;
		const bool interior = Util::IsInterior();
		auto* setting = prefColl->GetSetting(interior ? "fInteriorShadowDistance:Display" : "fShadowDistance:Display");
		if (!setting)
			return;
		const float base = setting->GetFloat();
		if (!std::isfinite(base) || base < 0.0f)
			return;  // malformed INI -- don't poison the engine cull with NaN/inf
		const float targetSq = std::max(base * base, endSq);
		ShadowDistanceSquaredCurrent() = targetSq;
		// Pin the sun-cascade far plane (the non-squared global) to the configured
		// shadow distance, NOT the extended cull: stretching the engine's 2 cascades
		// to the far light fade coarsens them into distant over-shadow (#194).
		ShadowDistanceCurrent() = base;
	}

	bool* GetFocusShadowSelected()
	{
		static REL::RelocationID uid(528096, 415041);
		return reinterpret_cast<bool*>(uid.address());
	}
	uint64_t* GetSunPtr()
	{
		static REL::RelocationID uid(528315, 415267);
		return reinterpret_cast<uint64_t*>(uid.address());
	}

	// Current accumulated shadow slot (used as Accumulate() first arg).
	uint32_t* GetAccumLightSlot()
	{
		static REL::RelocationID uid(528091, 415036);
		return reinterpret_cast<uint32_t*>(uid.address());
	}
	// Running mask index counter (incremented each time a light is slotted).
	uint32_t* GetMaskIndex()
	{
		static REL::RelocationID uid(528091, 415036);
		return reinterpret_cast<uint32_t*>(uid.address() + 4);
	}
	// Active shadow caster bitmask (ORed per slot).
	uint32_t* GetShadowMask()
	{
		static REL::RelocationID uid(528093, 415038);
		return reinterpret_cast<uint32_t*>(uid.address());
	}

	// Same test EnableLight uses to admit a light to firstPersonShadowMask:
	// does the camera sit inside the light's sphere of influence. Shared so
	// the redraw path (EnableLight), the demand-skip reinsert path, and the
	// extended-pool surface admission below can't drift out of sync.
	bool LightContainsCamera(const RE::NiLight* a_niLight, const RE::NiCamera* a_camera)
	{
		if (!a_niLight || !a_camera)
			return false;
		auto delta = a_niLight->world.translate - a_camera->world.translate;
		float dist = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
		float radius = a_niLight->GetLightRuntimeData().radius.x;
		return dist < radius + a_camera->GetNearPlane();
	}
	// Written back to the game at the end of scheduling.
	uint32_t* GetFrameLightCount()
	{
		static REL::RelocationID uid(528090, 415035);
		return reinterpret_cast<uint32_t*>(uid.address());
	}

	// VR-only globals
	bool GetVRDrawShadows()
	{
		static REL::Offset uid{ 0x1ed3cb0 };
		return *reinterpret_cast<bool*>(uid.address());
	}
	bool GetVRAccumFirst()
	{
		static REL::Offset uid{ 0x1ed4118 };
		return *reinterpret_cast<bool*>(uid.address());
	}
	float GetVRDRSWidthRatio()
	{
		static REL::Offset bDis{ 0x3186d28 }, r{ 0x3186d14 };
		return *reinterpret_cast<int*>(bDis.address()) ? 1.0f : *reinterpret_cast<float*>(r.address());
	}
	float GetVRDRSHeightRatio()
	{
		static REL::Offset bDis{ 0x3186d28 }, r{ 0x3186d18 };
		return *reinterpret_cast<int*>(bDis.address()) ? 1.0f : *reinterpret_cast<float*>(r.address());
	}

	// ---------- engine function wrappers ----------

	// Engine BSShadowDirectionalLight::SetupFocusShadowAccumulators: allocates + registers
	// the per-focus-actor BSShaderAccumulators (one per FocusShadowActors entry).
	void GameSetupFocusShadowAccumulators(RE::BSShadowLight* light)
	{
		using F = void (*)(RE::BSShadowLight*);
		static REL::Relocation<F> func{ REL::RelocationID(100819, 107603) };
		func(light);
	}

	// Engine BSShadowDirectionalLight::SetupFocusShadowMaps (RelocationID 100817/107601);
	// populates the per-actor focusShadowmapDescriptors -- a no-op without focus-shadow
	// actors, so the old "DirectionalLight" name was misleading.
	void GameSetupFocusShadowMaps(RE::BSShadowLight* light, RE::NiCamera* cam)
	{
		using F = void (*)(RE::BSShadowLight*, RE::NiCamera*);
		static REL::Relocation<F> func{ REL::RelocationID(100817, 107601) };
		func(light, cam);
	}

	void GameEnableLight(RE::ShadowSceneNode* ssn, RE::BSLight* light)
	{
		using F = void (*)(RE::ShadowSceneNode*, RE::BSLight*);
		static REL::Relocation<F> func{ REL::RelocationID(99708, 106342) };
		func(ssn, light);
	}

	void GameSetShadowCasterSlot(RE::ShadowSceneNode* ssn, RE::BSLight* light, uint32_t index, uint32_t unk)
	{
		using F = void (*)(RE::ShadowSceneNode*, RE::BSLight*, uint32_t, uint32_t);
		static REL::Relocation<F> func{ REL::RelocationID(99728, 106365) };
		func(ssn, light, index, unk);
	}

	void GameClearPortalVisibility(RE::BSPortalGraphEntry* entry)
	{
		using F = void (*)(RE::BSPortalGraphEntry*);
		static REL::Relocation<F> func{ REL::RelocationID(74395, 76119) };
		func(entry);
	}

	bool GamePortalHasSharedVisibility(RE::BSPortalGraphEntry* a, RE::BSPortalGraphEntry* b)
	{
		using F = bool (*)(RE::BSPortalGraphEntry*, RE::BSPortalGraphEntry*);
		static REL::Relocation<F> func{ REL::RelocationID(74397, 76121) };
		return func(a, b);
	}

	void GameClearGeometryList(RE::BSLight* light)
	{
		using F = void (*)(RE::BSLight*);
		static REL::Relocation<F> func{ REL::RelocationID(101298, 108285) };
		func(light);
	}

	void GameAttachGeometry(RE::BSLight* light, RE::BSGeometry* geom)
	{
		using F = void (*)(RE::BSLight*, RE::BSGeometry*);
		static REL::Relocation<F> func{ REL::RelocationID(101296, 108283) };
		func(light, geom);
	}

	bool GameLightIsInRange(RE::BSLight* light, const RE::NiBound* bound, RE::NiLight* niLight, float scale)
	{
		using F = bool (*)(RE::BSLight*, const RE::NiBound*, RE::NiLight*, float);
		static REL::Relocation<F> func{ REL::RelocationID(101299, 108286) };
		return func(light, bound, niLight, scale);
	}

	static bool GameIsLightAffectingSurface(RE::BSLightingShaderProperty* p, RE::BSLight* light)
	{
		using F = bool (*)(RE::BSLightingShaderProperty*, RE::BSLight*);
		static REL::Relocation<F> func{ REL::RelocationID(98902, 105550) };
		return func(p, light);
	}

	void GameApplyLensFlare(RE::BSLight* light)
	{
		// SE/AE only -- no VR equivalent (ID 100440)
		if (globals::game::isVR)
			return;
		using F = void (*)(RE::BSLight*);
		static REL::Relocation<F> func{ REL::RelocationID(100440, 107157) };
		func(light);
	}

	// VR-only
	void GameVRPrepareShadowMaps(RE::BSLight* light)
	{
		using F = void (*)(RE::BSLight*);
		static REL::Relocation<F> func{ REL::Offset(0x1356e50) };
		func(light);
	}

	void GameVRAccumulateShadowMaps(RE::BSLight* light)
	{
		using F = void (*)(RE::BSLight*);
		static REL::Relocation<F> func{ REL::Offset(0x1357450) };
		func(light);
	}

	void GameFrustumOverlap(RE::NiCamera* cam, float* coord, float* r1, float* r2, float eps)
	{
		// Non-VR: (cam, coord, r1, r2, eps)
		// VR:     (cam, coord, r1, r2, eyeIndex, eps)  -- pass 0xffffffff for combined frustum
		static REL::Relocation<uintptr_t> addr{ REL::RelocationID(69265, 70632) };
		auto ptr = addr.address();
		if (globals::game::isVR) {
			using VR = void (*)(RE::NiCamera*, float*, float*, float*, uint32_t, float);
			reinterpret_cast<VR>(ptr)(cam, coord, r1, r2, 0xffffffffu, eps);
		} else {
			using SE = void (*)(RE::NiCamera*, float*, float*, float*, float);
			reinterpret_cast<SE>(ptr)(cam, coord, r1, r2, eps);
		}
	}

	// Returns the culling process for the first shadow descriptor of a light,
	// or nullptr if it has none (callers already treat that as valid).
	RE::BSCullingProcess* GetLightCullingProcess(RE::BSShadowLight* light)
	{
		if (globals::game::isVR) {
			auto& descs = light->GetVRRuntimeData().shadowmapDescriptors;
			return descs.empty() ? nullptr : descs.front().cullingProcess;
		}
		auto& descs = light->GetRuntimeData().shadowmapDescriptors;
		return descs.empty() ? nullptr : descs.front().cullingProcess;
	}

	// True if this NiLight was promoted normal->shadow (PromoteNormalToShadow). Takes the
	// tracking lock; safe to call from the render thread while hooks mutate the set.
	bool IsPromotedLight(RE::NiLight* ni)
	{
		if (!ni)
			return false;
		std::scoped_lock lk(s_shadowConvertMutex);
		return s_shadowConvert.contains(ni);
	}

	// Vtable Render hook on BSShadowParabolicLight. The engine copies cascade 0's renderTarget
	// to descriptor[1] but NOT its shadowmapIndex, so descriptor[1] keeps a stale out-of-range
	// idx that teardown frees as the wrong pool slot -> nvwgf2umx OOB walk. Mirror renderTarget:
	// copy descriptor[0].shadowmapIndex into [1]. Render-thread per-light, no cross-thread race.
	struct Hook_ParabolicRender
	{
		static void thunk(RE::BSShadowParabolicLight* light, std::uint32_t& a_index)
		{
			func(light, a_index);
			if (!light)
				return;
			if (globals::game::isVR) {
				auto& descs = light->GetVRRuntimeData().shadowmapDescriptors;
				if (descs.size() >= 2)
					descs[1].shadowmapIndex = descs[0].shadowmapIndex;
			} else {
				auto& descs = light->GetRuntimeData().shadowmapDescriptors;
				if (descs.size() >= 2)
					descs[1].shadowmapIndex = descs[0].shadowmapIndex;
			}
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// install_context_hook (RtlRestoreContext) is required so all volatile registers (r8, etc.)
	// are restored before the game continues past the patched call site.
	//
	// Non-VR (SE/AE): set ctx.Rax = 0 so the conditional between 107133+0x192 and
	// +0x1AE skips "call [r8+0x50]" -- r8 is loaded from rax there; if rax != 0,
	// r8 gets a stale pointer whose [+0x50] slot is null -> crash at execute 0x0.
	static void Hook_RenderShadowLights(CONTEXT& ctx)
	{
		if (!globals::game::isVR)
			ctx.Rax = 0;
		RenderScheduledShadowLights();
	};

	// Hook struct for stl::detour_thunk.
	//
	// `s_settings.Enabled` is now a BOOT-TIME flag only -- toggling at
	// runtime has no effect on this thunk, the same way ShadowLightCount
	// and atlas texture sizes are restart-gated. See Init() at the
	// settings.Enabled early-return for the boot-time gate.
	//
	// Rationale (Ghidra-verified by crash 2026-05-17 20:31:12): the AV
	// at BSBatchRenderer::sub_SE100843_AE107633 +0x54
	// (`mov rax, [r14+0x48]`, r14=1 = vfunc bool returned as pointer)
	// is reached via:
	//   NiCamera::CalculateAndDrawShadowCasterLights
	//   -> CalculateActiveShadowCasterLights  (the engine's vanilla
	//                                          scheduler -- what we'd
	//                                          route to on disable)
	//     -> BSShadowDirectionalLight::sub_SE100818_AE107602 (sun
	//                                                        shadow)
	//       -> FUN_1414bf320 (BSCullingProcess inner)
	//         -> BSCullingProcess::sub
	//           -> FUN_1414f50d0
	//             -> BSBatchRenderer::sub_SE100843_AE107633  (AV)
	//
	// The crash is in the vanilla scheduler itself. SCM's boot-time
	// modifications (kSHADOWMAPS texture sized to ShadowLightCount,
	// depth-buffer creation loop redirected via Hook_CreateNormalDepthBuffer
	// and Hook_CreateReadOnlyDepthBuffer, screen-space mask pass wrapped
	// by Hook_RenderShadowLightsWithUtilityShader) make the engine state
	// incompatible with
	// the vanilla traversal even when our runtime tracking is left
	// untouched (soft-disable still crashed). The deep engine hooking
	// is not safely reversible at runtime; restart is the only safe
	// way to revert to vanilla.
	struct Hook_CalculateActiveShadowCasters
	{
		static void thunk()
		{
			ScheduleShadowCasters();
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// =========================================================================
	// Surface lights hook
	// Replaces CalculateActiveNonShadowCasterLights (ID 100997/107784).
	// Uses install_context_hook because the function has 10 args (11 in VR)
	// with VR-specific stack layout -- CONTEXT is the simplest cross-runtime approach.
	// =========================================================================

	static void Hook_CalculateActiveLightsForSurface(CONTEXT& ctx)
	{
		// Args from registers/stack (x64 fastcall, shadow space at RSP+0x00..0x20):
		auto* lightData = reinterpret_cast<RE::BSShaderPropertyLightData*>(ctx.Rcx);           // a1
		auto** lights = reinterpret_cast<RE::BSLight**>(ctx.Rdx);                              // a2
		int maxCount = static_cast<int>(ctx.R8);                                               // a3
		int* shadowCount = reinterpret_cast<int*>(ctx.R9);                                     // a4
		auto* ssn = *reinterpret_cast<RE::ShadowSceneNode**>(ctx.Rsp + 0x28);                  // a5
		auto* shaderProp = *reinterpret_cast<RE::BSLightingShaderProperty**>(ctx.Rsp + 0x30);  // a6
		bool addShadow = *reinterpret_cast<bool*>(ctx.Rsp + 0x38);                             // a7
		bool* useShadowSun = *reinterpret_cast<bool**>(ctx.Rsp + 0x40);                        // a8
		bool firstPerson = *reinterpret_cast<bool*>(ctx.Rsp + 0x48);                           // a9
		uint32_t fpMask = *reinterpret_cast<uint32_t*>(ctx.Rsp + 0x50);                        // a10

		// VR passes an 11th arg: if non-zero, skip accumulation (vanilla early-out).
		if (globals::game::isVR && *reinterpret_cast<char*>(ctx.Rsp + 0x58) != 0) {
			ctx.Rax = 1;  // addedLightCount = sun only
			return;
		}

		// Determine the sun light for this surface.
		RE::BSLight* sunLight;
		if (*useShadowSun)
			sunLight = ssn->GetRuntimeData().sunShadowDirLight;
		else
			sunLight = ssn->GetRuntimeData().sunLight;
		if (shaderProp->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kCloudLOD))
			sunLight = ssn->GetRuntimeData().cloudLight;

		lights[0] = sunLight;
		*shadowCount = 0;
		int added = 1;

		if (addShadow) {
			auto& casters = ssn->GetRuntimeData().shadowLightsAccum;

			// Step 1: vanilla shadow lights gated by activeLightMask / first-person mask.
			for (uint32_t slot = 0; slot < casters.size() && added < maxCount; slot++) {
				// Step 2 below still picks up any real caster here via a
				// direct array scan once the vanilla mask runs out of bits.
				if (slot >= kShadowMaskBits)
					break;
				uint32_t bit = 1u << slot;
				if (!((firstPerson && (fpMask & bit)) || (lightData->activeLightMask & bit)))
					continue;
				auto* sl = reinterpret_cast<RE::BSLight*>(casters[slot]);
				if (!sl || sl == sunLight)
					continue;
				if (GameIsLightAffectingSurface(shaderProp, sl)) {
					lights[added++] = sl;
					(*shadowCount)++;
				}
			}

			// Step 2: extended pool lights not covered by the vanilla mask.
			// Only inject lights that are present in this scene's caster array
			// (prevents world lights leaking into menu / special scenes).
			// Iterate the point-light range (sun-aware via PointLightFirst /
			// PointLightEnd; pre-helper loops missed pool[ShadowLightCount]
			// when Sun=true, dropping one shadow caster from per-surface lists).
			for (int i = s_lights.PointLightFirst(); i < s_lights.PointLightEnd(s_settings.ShadowLightCount) && added < maxCount; i++) {
				auto& e = s_lights.Lights[i];
				if (!e.Light || reinterpret_cast<RE::BSLight*>(e.Light) == sunLight)
					continue;

				bool inScene = false;
				for (uint32_t s = 0; s < casters.size() && !inScene; s++)
					if (reinterpret_cast<RE::BSLight*>(casters[s]) == reinterpret_cast<RE::BSLight*>(e.Light))
						inScene = true;
				if (!inScene)
					continue;

				bool alreadyAdded = false;
				for (int j = 1; j < added && !alreadyAdded; j++)
					if (lights[j] == reinterpret_cast<RE::BSLight*>(e.Light))
						alreadyAdded = true;
				if (alreadyAdded)
					continue;

				// GameIsLightAffectingSurface has no distance test, so use the
				// vanilla mask's own camera-inside-light-radius test instead --
				// lets lights past kShadowMaskBits (32) still reach first person.
				bool admits = firstPerson ?
				                  LightContainsCamera(e.Light->light.get(), GetWorldCamera()) :
				                  GameIsLightAffectingSurface(shaderProp, reinterpret_cast<RE::BSLight*>(e.Light));
				if (admits) {
					lights[added++] = reinterpret_cast<RE::BSLight*>(e.Light);
					(*shadowCount)++;
				}
			}
		}

		// Step 3: non-shadow lights from the per-surface accumulation list.
		// Skip parabolic shadow-casters (frustrumCull == 0xFF) and hidden NiLights.
		for (uint32_t i = 0; i < lightData->lights.size() && added < maxCount; i++) {
			auto* l = lightData->lights[i];
			if (!l || l == sunLight)
				continue;
			auto* ni = l->light.get();
			if (ni && (l->frustrumCull == 0xFFu || ni->GetFlags().any(RE::NiAVObject::Flag::kHidden)))
				continue;
			lights[added++] = l;
		}

		// Step 4: Inject converted shadow lights (s_normalConvert)
		// into the per-surface lights array. These lights have frustrumCull == 0xFF
		// (parabolic shadow-caster marker) and are skipped by Step 3, while Steps
		// 1/2 don't include them either (ReturnShadowmaps cleared shadowLightsAccum).
		//
		// The cluster pipeline picks them up separately via LightLimitFix::UpdateLights'
		// activeShadowLights iteration; this Step 4 ensures the engine's vanilla
		// strict-light loop (which consumes lights[] passed to this function) also
		// sees them so non-LLF code paths and shaders without LIGHT_LIMIT_FIX still
		// receive the diffuse contribution.
		for (auto& c : s_normalConvert) {
			if (added >= maxCount)
				break;
			auto* l = reinterpret_cast<RE::BSLight*>(c.light);
			if (!l || l == sunLight)
				continue;
			auto* ni = l->light.get();
			if (!ni || ni->GetFlags().any(RE::NiAVObject::Flag::kHidden))
				continue;

			// Skip if already added in any prior step.
			bool alreadyAdded = false;
			for (int j = 1; j < added && !alreadyAdded; j++)
				if (lights[j] == l)
					alreadyAdded = true;
			if (alreadyAdded)
				continue;

			if (GameIsLightAffectingSurface(shaderProp, l))
				lights[added++] = l;
			// Note: do NOT increment *shadowCount; this is a non-shadow contribution.
		}

		ctx.Rax = static_cast<uint64_t>(added);
	}

	// IsShadowLight returns false for s_normalConvert lights so the engine
	// treats them as non-shadow; Add/Remove/SetLight below keep
	// s_normalConvert/s_shadowConvert in sync with scene changes.

	static bool Hook_IsShadowLight(RE::BSShadowLight* light)
	{
		for (auto& c : s_normalConvert)
			if (c.light == light)
				return false;
		return true;
	}

	// Fires at start of ShadowSceneNode::RemoveLight (ID 99697/106331).
	static void Hook_ConvertLights_Remove(CONTEXT& ctx)
	{
		auto* ssn = reinterpret_cast<RE::ShadowSceneNode*>(ctx.Rcx);
		auto* light = reinterpret_cast<RE::NiLight*>(ctx.Rdx);
		if (ssn != GetShadowSceneNode())
			return;
		for (auto it = s_normalConvert.begin(); it != s_normalConvert.end(); ++it) {
			auto* nl = it->light->light.get();
			if (nl && nl == light) {
				GameClearGeometryList(it->light);
				s_normalConvert.erase(it);
				break;
			}
		}
		if (light) {
			std::scoped_lock lk(s_shadowConvertMutex);
			s_shadowConvert.erase(light);
			s_shadowConvertDescriptorInited.erase(light);
		}
	}

	// Detours the engine's bulk shadow-light teardown (bypasses RemoveLight):
	// flags a session reset, then bounded-waits out any in-flight shadow render
	// before letting the engine free -- freeing mid-render-walk zeroes nodes.
	struct Hook_ClearLightArrays
	{
		static void thunk(RE::ShadowSceneNode* a_ssn, std::uint64_t a_2, std::uint64_t a_3, std::uint64_t a_4)
		{
			if (a_ssn == GetShadowSceneNode()) {
				s_pendingSessionReset.store(true, std::memory_order_release);
				s_teardownWaiting.store(true, std::memory_order_release);
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
				while (s_shadowFlushReaders.load(std::memory_order_acquire) > 0) {
					if (std::chrono::steady_clock::now() > deadline) {
						logger::warn("[SCM] ClearLightArrays proceeding with a shadow render still in flight (reader wait timed out)");
						break;
					}
					std::this_thread::yield();
				}
				func(a_ssn, a_2, a_3, a_4);
				s_teardownWaiting.store(false, std::memory_order_release);
				return;
			}
			func(a_ssn, a_2, a_3, a_4);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// Detours ShadowSceneNode::ResetScene (99741/106385) -- the portalGraph setter, called from
	// ResetCellGrid on cell transitions. This is the coc-time nuller (not ClearSceneAndFog, which
	// only fires on SSN teardown). Hold the graph lock exclusive so it can't swap mid-read.
	struct Hook_ResetScene
	{
		static void thunk(RE::ShadowSceneNode* a_ssn, RE::BSPortalGraph* a_graph)
		{
			std::unique_lock lock(s_portalGraphMutex);
			func(a_ssn, a_graph);
			// Cell-grid shift: freed caster geometry can have its address recycled
			// by the new cell, aliasing s_casterMobility's stale identity onto it.
			// Deferred to the render thread (both are render-thread-only).
			s_pendingCellReset.store(true, std::memory_order_release);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// Detours ShadowSceneNode::AccumulateLight (99753/106401). It derefs ssn->portalGraph after
	// an entry guard passed, so the teardown nulling it mid-call is a TOCTOU. Take the graph lock
	// SHARED with try_to_lock: skip the light if ResetScene holds it exclusive. try_to_lock is
	// load-bearing -- a blocking acquire deadlocks against ResetScene's wait for render progress.
	struct Hook_AccumulateLight
	{
		static void thunk(RE::ShadowSceneNode* a_ssn, RE::BSLight* a_light, void* a3, std::uint8_t a4)
		{
			std::shared_lock lock(s_portalGraphMutex, std::try_to_lock);
			if (!lock.owns_lock())
				return;  // ResetScene is swapping portalGraph; skip rather than read a null graph
			func(a_ssn, a_light, a3, a4);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// Fires at start of BSShadowLight::ctor (ID 100810/107594). No ctor writes
	// BSLight::cullingProcess: recycled dirty pages leave dangling garbage that
	// CTDs room culling or silently skips AccumulateLight rewiring. Zero it here
	// ONLY -- nulling a live light's process is an instant CTD.
	static void Hook_ShadowLightCtor(CONTEXT& ctx)
	{
		if (auto* light = reinterpret_cast<RE::BSShadowLight*>(ctx.Rcx))
			light->cullingProcess = nullptr;
	}

	// Fires at start of ShadowSceneNode::AddLight (ID 99692/106326).
	// Optionally promotes normal light to shadow light; always forces portal-strict.
	static void Hook_ConvertLights_Add(CONTEXT& ctx)
	{
		auto* ssn = reinterpret_cast<RE::ShadowSceneNode*>(ctx.Rcx);
		auto* light = reinterpret_cast<RE::NiLight*>(ctx.Rdx);
		auto* p = reinterpret_cast<RE::ShadowSceneNode::LIGHT_CREATE_PARAMS*>(ctx.R8);
		if (ssn != GetShadowSceneNode() || !light || !p)
			return;

		bool justPromoted = false;
		if (s_settings.PromoteNormalToShadow && !p->shadowLight) {
			p->shadowLight = true;
			p->fov = 6.2831855f;
			p->dynamic = true;
			p->restrictedNode = nullptr;
			p->falloff = 1.0f;
			p->depthBias = 1.0f;
			p->nearDistance = (light->GetLightRuntimeData().radius.x / 512.0f) * 219.6356f;
			{
				std::scoped_lock lk(s_shadowConvertMutex);
				s_shadowConvert.insert(light);
			}
			justPromoted = true;
		} else {
			// This NiLight is not one we promoted (native shadow caster, or promotion off).
			// If the allocator reused its address from a freed promoted light, drop the stale
			// entry now -- synchronously, before the scheduler can misread it as promoted.
			// A re-added promoted light always arrives with shadowLight=false and takes the
			// branch above, so this never un-tracks one of ours.
			std::scoped_lock lk(s_shadowConvertMutex);
			s_shadowConvert.erase(light);
			s_shadowConvertDescriptorInited.erase(light);
		}
		// Portal-strict policy by shadow type (engine picks the shadow class by FOV: >=2pi omni,
		// >=pi hemi, <pi spot). Tighten on omnis/hemis; on spots it drops culled-but-visible spots.
		// NEVER force it on a light WE just promoted: a promoted light has no stable portal-graph
		// entry, so portalStrict takes the engine's unguarded strict branch and faults on the
		// transiently-null graph during a cell transition (flat: null deref; VR: nvwgf2umx).
		if (!justPromoted) {
			constexpr float kFovHemiThreshold = 3.0f;  // ~pi
			constexpr float kFovOmniThreshold = 6.0f;  // ~2pi
			bool enforce = false;
			if (p->fov >= kFovOmniThreshold)
				enforce = s_settings.ForceEnablePortalStrictOmni;
			else if (p->fov >= kFovHemiThreshold)
				enforce = s_settings.ForceEnablePortalStrictHemi;
			else
				enforce = s_settings.ForceEnablePortalStrictSpot;
			// Portals only exist in interiors; portalStrict outdoors has no graph to
			// test against -- a no-op at best, a mis-cull of visible casters at worst.
			if (enforce && Util::IsInterior())
				p->portalStrict = true;
		}
	}

	// Fires at start of BSLight::SetLight (ID 101302/108289).
	// Tracks NiLight pointer reassignments in s_shadowConvert.
	static void Hook_ConvertLights_SetLight(CONTEXT& ctx)
	{
		auto* bslight = reinterpret_cast<RE::BSLight*>(ctx.Rcx);
		auto* nilight = reinterpret_cast<RE::NiLight*>(ctx.Rdx);
		if (!bslight)
			return;
		// A promoted shadow light is allocated non-zeroed and no ctor in the chain inits
		// BSLight::cullingProcess (+0x128). AccumulateLight reuses a non-null cullingProcess
		// as-is, so stale heap garbage there becomes a fake BSParabolicCullingProcess that
		// room-light culling calls a vfunc on -> garbage-vtable CTD. Null it at creation
		// (SetLight runs during AddLight, before any accumulate) so the engine builds a real
		// one, exactly as a fresh zeroed page would.
		{
			std::scoped_lock lk(s_shadowConvertMutex);
			if (nilight && s_shadowConvert.count(nilight))
				bslight->cullingProcess = nullptr;
			auto* oldlight = bslight->light.get();
			if (oldlight && oldlight != nilight) {
				bool did = s_shadowConvert.erase(oldlight) != 0;
				s_shadowConvertDescriptorInited.erase(oldlight);  // reassigned nilight re-inits via scheduler
				if (nilight && did)
					s_shadowConvert.insert(nilight);
			}
		}
	}

	// =========================================================================
	// Stealth detection fix
	//
	// GetLightLevel (AIProcess::CalculateLightValue, ID 38900/39946) uses the
	// engine shadow-light iteration internally. When we replace shadow caster
	// selection, the vanilla per-light affect-player loop no longer sees our
	// chosen lights correctly. We replace it with our own pass that iterates
	// activeShadowLights and calls IsLightAffectingActor() directly.
	// =========================================================================

	// Temporary set of lights that affect the player -- populated each frame
	// in Hook_UpdateLightLevelPlayer, consumed in Hook_CheckLightLevelPlayer.
	static std::set<uint64_t> s_stealthDetectionTmp;

	static void* GetUnkDetectionGlobal()
	{
		// SE: 142F6DB98 -- a ~80-byte detection struct; GetSingleton equivalent
		static REL::RelocationID uid(518074, 404596);
		return *reinterpret_cast<void**>(uid.address());
	}

	static bool IsLightAffectingActor(RE::BSShadowLight* light, RE::Actor* actor, RE::NiPoint3* pos)
	{
		// SE: 14071A380 (ID 41661)
		using F = bool (*)(void*, RE::BSShadowLight*, RE::Actor*, RE::NiPoint3*);
		static REL::Relocation<F> func{ REL::RelocationID(41661, 42744) };
		return func(GetUnkDetectionGlobal(), light, actor, pos);
	}

	// Replaces the vanilla shadow-light-affect-player loop.
	// RBP-33 holds the player's position (NiPoint3*).
	static void Hook_UpdateLightLevelPlayer(CONTEXT& ctx)
	{
		auto* pos = reinterpret_cast<RE::NiPoint3*>(ctx.Rbp - 33);
		auto* player = RE::PlayerCharacter::GetSingleton();

		s_stealthDetectionTmp.clear();
		auto* ssn = GetShadowSceneNode();
		if (!ssn)
			return;

		for (auto& sp : ssn->GetRuntimeData().activeShadowLights) {
			auto* l = sp.get();
			if (!l)
				continue;
			auto* ni = l->light.get();
			if (!ni || ni->GetFlags().any(RE::NiAVObject::Flag::kHidden))
				continue;
			if (IsLightAffectingActor(l, player, pos))
				s_stealthDetectionTmp.insert(reinterpret_cast<uint64_t>(l));
		}
	}

	// Per-light check inside the vanilla affect-player path.
	// If the light is not in our set, skip the branch (ctx.Rip += 0x16).
	// Note: Execute() sets ctx.Rip = resumeAddr BEFORE calling this, so
	// ctx.Rip += 0x16 skips 0x16 bytes past the hook site -- correct.
	static void Hook_CheckLightLevelPlayer(CONTEXT& ctx)
	{
		auto* light = reinterpret_cast<RE::BSShadowLight*>(ctx.Rcx);
		if (s_stealthDetectionTmp.find(reinterpret_cast<uint64_t>(light)) == s_stealthDetectionTmp.end())
			ctx.Rip += 0x16;
	}

	void Install(const Settings& settings)
	{
		s_settings = settings;
		s_installedShadowLightCount = settings.ShadowLightCount;
		// kSHADOWMAPS is point/spot only -- the sun renders to a separate
		// kSHADOWMAPS_ESRAM texture (cascade descriptors live there, not
		// here). So the engine allocates exactly ShadowLightCount slices
		// in kSHADOWMAPS; no +1 for the sun.
		s_requestedSlotCount = static_cast<uint32_t>(settings.ShadowLightCount);

		// One-shot capture of the boot Enabled value. Install() is called
		// once at startup, but guard anyway in case it's ever re-invoked.
		if (!s_bootEnabledCaptured) {
			s_bootEnabled = settings.Enabled;
			s_bootEnabledCaptured = true;
		}
		s_bootSnapshot.LatchIfNeeded(settings);

		if (s_externalConflict)
			return;

		if (!settings.Enabled) {
			logger::info("[SCM] Shadow caster manager disabled -- skipping hook installation.");
			return;
		}

		bool extended = settings.ShadowLightCount > 4;

		// Atlas mode keeps the engine array at its vanilla slice count (the
		// VRAM win); boot-latched because the allocation below cannot change
		// at runtime.
		s_bootAtlasEnabled = settings.ShadowAtlas && extended;
		if (s_bootAtlasEnabled) {
			s_requestedSlotCount = kVanillaShadowSliceCount;
			logger::info("[SCM] Shadow atlas boot-enabled: engine kSHADOWMAPS stays at {} slices", kVanillaShadowSliceCount);
		}

		bool needExtraBuffers = !s_bootAtlasEnabled && settings.ShadowLightCount > kVanillaShadowSliceCount;

		// ---- Extended depth buffer infrastructure -------------------------

		if (needExtraBuffers) {
			globals::features::llf::normalDepthBuffer = new void*[settings.ShadowLightCount + 1]();
			globals::features::llf::readOnlyDepthBuffer = new void*[settings.ShadowLightCount + 1]();

			// Patch the creation-loop count from 8 to ShadowLightCount.
			// SE/VR: pattern "C7 44 24 68 08 00 00 00" (+4 = the imm32 0x00000008)
			// AE:    same pattern at different offset
			//
			// The instruction encodes a 32-bit immediate; we overwrite all four
			// bytes so values >255 don't silently truncate (a single-byte write
			// to the low byte would leave higher bytes stale, capping us at 255
			// while making the cap silent).
			{
				static REL::RelocationID uid(100458, 107175);
				uintptr_t addr = uid.address() + REL::Relocate(0xD326 - 0xC940, 0xBF6 - 0x210, 0xc91);
				int immOff = REL::Relocate(4, 4, 3);
				uint32_t newCount = static_cast<uint32_t>(settings.ShadowLightCount);
				REL::safe_write(addr + immOff, &newCount, sizeof(newCount));
			}

			// Redirect depth-buffer pointer storage in the creation loop.
			{
				// Normal DSV creation: SE 140D6AB52 / VR 140DBCA00
				static REL::RelocationID uid(75469, 77255);
				uintptr_t base = uid.address();
				uintptr_t off = REL::Relocate(0xB52 - 0x9E0, 0x2EB - 0x180, 0x1a0);
				int sz = REL::Relocate(7, 7, 8);
				if (!SKSE::stl::install_context_hook(base + off, sz, Hook_CreateNormalDepthBuffer, sz))
					logger::error("[SCM] Failed to install Hook_CreateNormalDepthBuffer");
			}
			{
				// ReadOnly DSV creation: SE 140D6AB71 / VR 140DBCA24
				static REL::RelocationID uid(75469, 77255);
				uintptr_t base = uid.address();
				uintptr_t off = REL::Relocate(0xB71 - 0x9E0, 0x2FC - 0x180, 0x1c4);
				int sz = REL::Relocate(8, 7, 7);
				if (!SKSE::stl::install_context_hook(base + off, sz, Hook_CreateReadOnlyDepthBuffer, sz))
					logger::error("[SCM] Failed to install Hook_CreateReadOnlyDepthBuffer");
			}

			// Sync the first 8 slots into the game's own DepthStencilData array.
			{
				// SE 140D6AC00 / VR 140DBCAB0
				static REL::RelocationID uid(75469, 77255);
				uintptr_t base = uid.address();
				uintptr_t off = REL::Relocate(0xC00 - 0x9E0, 0x384 - 0x180, 0x250);
				if (!SKSE::stl::install_context_hook(base + off, 8, Hook_SetupGameArray, 8))
					logger::error("[SCM] Failed to install Hook_SetupGameArray");
			}
		}

		// Depth-buffer selection at draw time. Also required in atlas mode
		// (no extra buffers): the type-4 redirect is what routes point/spot
		// cascades into the atlas DSV.
		if (needExtraBuffers || s_bootAtlasEnabled) {
			{
				// SE 140D70444
				static REL::RelocationID uid(75580, 77386);
				uintptr_t base = uid.address();
				uintptr_t off = REL::Relocate(0x444 - 0x2F0, 0x704 - 0x5B0, 0x1c3);
				if (!SKSE::stl::install_context_hook(base + off, 21, Hook_SelectDepthBuffer1))
					logger::error("[SCM] Failed to install Hook_SelectDepthBuffer1");
			}
			{
				// SE 140D6A1A5 / VR 140DBBFFC
				static REL::RelocationID uid(75462, 77247);
				uintptr_t base = uid.address();
				uintptr_t off = REL::Relocate(0x1A5 - 0x070, 0x985 - 0x850, 0x19c);
				int sz = REL::Relocate(10, 10, 0x2e);
				if (!SKSE::stl::install_context_hook(base + off, sz, Hook_SelectDepthBuffer2))
					logger::error("[SCM] Failed to install Hook_SelectDepthBuffer2");
			}
		}

		if (needExtraBuffers) {
			// Release extended buffers at renderer shutdown.
			// SE: ZeroDepthStencilData; AE/VR: Renderer::Shutdown and related dtor paths.
			if (REL::Module::GetRuntime() != REL::Module::Runtime::AE) {
				// SE + VR share the same pattern.
				static REL::RelocationID uid(75628, 0 /*AE unused*/);
				uintptr_t addr = uid.address() + (0xE27 - 0xDD0);
				if (!SKSE::stl::install_context_hook(addr, 9, Hook_DeleteDepthBuffers_SE, -9))
					logger::error("[SCM] Failed to install Hook_DeleteDepthBuffers_SE");
			} else {
				// AE has three separate shutdown paths.
				static REL::RelocationID uid1(0, 77228);
				if (!SKSE::stl::install_context_hook(uid1.address() + (0x3195 - 0x2E10), 7, Hook_DeleteDepthBuffers_AE, 7))
					logger::error("[SCM] Failed to install Hook_DeleteDepthBuffers_AE (path 1)");

				static REL::RelocationID uid2(0, 77237);
				if (!SKSE::stl::install_context_hook(uid2.address() + (0x3B8C - 0x34A0), 7, Hook_DeleteDepthBuffers_AE, 7))
					logger::error("[SCM] Failed to install Hook_DeleteDepthBuffers_AE (path 2)");

				static REL::RelocationID uid3(0, 77238);
				if (!SKSE::stl::install_context_hook(uid3.address() + (0x3E79 - 0x3BC0), 6, Hook_DeleteDepthBuffers_AE, -6))
					logger::error("[SCM] Failed to install Hook_DeleteDepthBuffers_AE (path 3)");
			}
		}

		// Expanded accumulated-lights array (needed when ShadowLightCount > 4).
		if (extended) {
			// SE: BSShadowFrustumLight accumulation setup
			static REL::RelocationID uid(99686, 106320);
			uintptr_t base = uid.address();
			// The legacy-AE offset decodes as a different, invalid instruction on 1.7.99 -- not interchangeable.
			const std::uintptr_t aeAccumOffset = REL::Module::IsAtLeast(REL::Version(1, 7, 99, 0)) ? 0x381 : (0xF05 - 0xBB0);
			uintptr_t off = REL::Relocate<std::uintptr_t>(0xFCA4 - 0xF950, aeAccumOffset, 0x387);
			if (!SKSE::stl::install_context_hook(base + off, 5, Hook_AccumulatedLightsArray, 5))
				logger::error("[SCM] Failed to install Hook_AccumulatedLightsArray");
		}

		// Force per-light shadow map slot assignment.
		// Required whenever our temporal scheduler is active (ShadowLightCount >= 4):
		// RenderCascade recalculates the slot from a global counter each call; without
		// this hook, a light not redrawn this frame gets a different slot than last
		// frame and corrupts another light's shadow map.
		{
			// SE: RenderCascade+0xBE; VR: RenderCascade+0xE0
			static REL::RelocationID uid(100820, 107604);
			uintptr_t base = uid.address();
			uintptr_t off = REL::Relocate(0xA9E - 0x9E0, 0xDB0 - 0xCF0, 0xe0);
			if (!SKSE::stl::install_context_hook(base + off, 0x25, Hook_OverwriteShadowMapIndex))
				logger::error("[SCM] Failed to install Hook_OverwriteShadowMapIndex");
		}

		// Variable-resolution tiles: shrink the shadow viewport right after the
		// engine computes it. Installed whenever SCM is active; the thunk is a
		// no-op unless a cascade armed s_pendingTileScale.
		if (long rc = stl::detour_thunk<Hook_UpdateViewPort>(REL::RelocationID(75455, 77240)))
			logger::error("[SCM] Failed to install Hook_UpdateViewPort ({})", rc);

		// Suppress the engine's focus shadow path in extended mode (matches
		// Intellightent's mitigation). In extended mode parabolic lights
		// occupy kSHADOWMAPS slots [4..7] -- the same range g_focusShadow-
		// BaseSlotIndex (=4) reserves for focus rendering. If the engine
		// enters BSShadowParabolicLight::Render's focus loop on a parabolic
		// light in those slots it CTDs without a crashlog. Two layers of
		// defense: these byte patches zero the engine's global gate, and
		// ScheduleShadowCasters scrubs drawFocusShadows on every light
		// per-frame to clear stale flags. The per-frame scrub alone would
		// suffice; the patches make the suppression robust against any
		// engine path that bypasses the per-light flag.
		if (extended) {
			// ids 10245/10247 are absent from the 1.7.99 address library: 1.7.99 inlines both
			// standalone thunks into their caller and consolidates them to a single live
			// copy, so this patches that one instruction directly instead of a function entry.
			const uint8_t xorRax[6] = { 0x48, 0x31, 0xC0, 0x90, 0x90, 0x90 };
			if (REL::Module::get().version() == REL::Version(1, 7, 104, 0)) {
				// Raw offset (unstable across 1.7.x point releases) -- verify the bytes
				// before writing so a version that moved this instruction fails safe.
				const uint8_t expected[6] = { 0x8B, 0x05, 0xAE, 0xE5, 0xBE, 0x00 };
				const uint8_t xorEax[6] = { 0x31, 0xC0, 0x90, 0x90, 0x90, 0x90 };
				const auto target = REL::Offset(0x14eaab4).address();
				if (!REL::safe_write(target, xorEax, sizeof(xorEax), expected))
					logger::warn("[SCM] focus-shadow suppression: unexpected bytes at 0x14eaab4 (game version moved this instruction), skipping patch");
			} else if (REL::Module::IsAtLeast(REL::Version(1, 7, 99, 0))) {
				// Raw offset (unstable across 1.7.x point releases) -- verify the bytes
				// before writing so a version that moved this instruction fails safe.
				const uint8_t expected[6] = { 0x8B, 0x05, 0x0E, 0xE8, 0xBE, 0x00 };
				const uint8_t xorEax[6] = { 0x31, 0xC0, 0x90, 0x90, 0x90, 0x90 };
				const auto target = REL::Offset(0x14ea854).address();
				if (!REL::safe_write(target, xorEax, sizeof(xorEax), expected))
					logger::warn("[SCM] focus-shadow suppression: unexpected bytes at 0x14ea854 (game version moved this instruction), skipping patch");
			} else {
				static REL::RelocationID uid1(10209, 10247);
				REL::safe_write(uid1.address(), xorRax, 6);

				static REL::RelocationID uid2(10207, 10245);
				REL::safe_write(uid2.address(), xorRax, 6);
			}

			static REL::RelocationID uid3(513201, 390932);
			const uint8_t zero = 0;
			REL::safe_write(uid3.address(), &zero, 1);
		}

		// Screen-space shadow-mask pass: no-op RenderShadowLightsWithUtilityShader (100423/107141)
		// on all runtimes. Its loop indexes a 4-entry blend-mode table by BSShadowLight::maskIndex;
		// SLF's extended slots (and teardown's 0xFF sentinel) push it >=4 -> OOB -> corrupt shadow
		// state (VR CTD on transitions). LLF serves casters from its own pipeline, so this loses
		// nothing. One detour for all runtimes -- don't re-split for VR (a since-corrected mismap).
		stl::detour_thunk<Hook_RenderShadowLightsWithUtilityShader>(
			REL::RelocationID(100423, 107141));

		// Alpha GeometryGroup ceiling (100874/107670, see Hook_StartGroupingAlphas).
		// The counter has no address-library id of its own; it's decoded out of
		// ClearAlphaGeometryGroups (100856/107646), an 11-byte `mov dword
		// [counter], 0; ret` whose RIP-relative operand IS the counter -- the
		// opcode check below is what makes that safe rather than a guess.
		{
			// `mov dword ptr [rip+disp32], imm32` (C7 /0, RIP-relative ModRM), then
			// `ret`; the operand is relative to the end of the 10-byte store.
			constexpr std::uint8_t kMovDwordImmOpcode = 0xC7;
			constexpr std::uint8_t kRipRelativeModRM = 0x05;
			constexpr std::uint8_t kRetOpcode = 0xC3;
			constexpr std::size_t kMovDwordImmSize = 10;

			const auto clearFn = REL::RelocationID(100856, 107646).address();
			const auto* code = reinterpret_cast<const std::uint8_t*>(clearFn);
			std::uint32_t immediate = 1;
			std::int32_t displacement = 0;
			if (clearFn) {
				std::memcpy(&displacement, code + 2, sizeof(displacement));
				std::memcpy(&immediate, code + 6, sizeof(immediate));
			}
			// The immediate must be the zero this function exists to store: a
			// patched sentinel would mean the counter no longer means what the
			// ceiling check assumes.
			const bool shapeOk = clearFn && code[0] == kMovDwordImmOpcode &&
			                     code[1] == kRipRelativeModRM && immediate == 0u &&
			                     code[kMovDwordImmSize] == kRetOpcode;
			const auto counter = shapeOk ? clearFn + kMovDwordImmSize + displacement : 0;
			// Containment check: a decode this guard trusts enough to dereference
			// every frame must land in the module's own data, not wherever a
			// displacement happened to point.
			const auto data = REL::Module::get().segment(REL::Segment::data);
			if (counter >= data.address() && counter < data.address() + data.size()) {
				s_alphaGroupCount = reinterpret_cast<std::uint32_t*>(counter);
				s_alphaGroupLimit = (globals::game::isVR ? kAlphaGeometryGroupCapacityVR :
														   kAlphaGeometryGroupCapacityFlat) -
				                    kAlphaGeometryGroupReserve;
				if (long rc = stl::detour_thunk<Hook_StartGroupingAlphas>(REL::RelocationID(100874, 107670)))
					logger::error("[SCM] Failed to install Hook_StartGroupingAlphas ({})", rc);
			} else {
				s_alphaGroupCount = nullptr;
				logger::error(
					"[SCM] Alpha GeometryGroup guard not installed: "
					"ClearAlphaGeometryGroups did not decode to a counter in .data");
			}
		}

		// ---- Shadow caster selection -----------------------------------------

		// Replace CalculateActiveShadowCasterLights entirely (ID 100419/107137).
		// VR confirmed: 0x1413226e0
		stl::detour_thunk<Hook_CalculateActiveShadowCasters>(REL::RelocationID(100419, 107137));

		// Replace the CALL to RenderActiveShadowCasterLights inside the render loop.
		// ID 100415/107133; VR confirmed: 0x141322130
		// Offsets: SE = 0xF76-0xE30 (0x146), AE = 0xC17D-0xBFF0 (0x18D), VR = 0x1CA
		// Must use install_context_hook (not write_thunk_call) so RtlRestoreContext restores
		// volatile registers (r8, etc.) before the game continues past the call site.
		{
			static REL::RelocationID uid(100415, 107133);
			uintptr_t addr = uid.address() + REL::Relocate(0xF76 - 0xE30, 0xC17D - 0xBFF0, 0x1CA);
			if (!SKSE::stl::install_context_hook(addr, 5, Hook_RenderShadowLights))
				logger::error("[SCM] Failed to install Hook_RenderShadowLights");
		}

		// Replace CalculateActiveNonShadowCasterLights (surface light injection).
		// ID 100997/107784; VR confirmed: 0x141354d20
		// Uses install_context_hook because the function has 10 args (11 in VR) with
		// platform-specific stack layout. We write a RET at func+5 so
		// RtlRestoreContext lands on ret and the function returns cleanly.
		{
			static REL::RelocationID uid(100997, 107784);
			if (!SKSE::stl::install_context_hook(uid.address(), 5, Hook_CalculateActiveLightsForSurface))
				logger::error("[SCM] Failed to install Hook_CalculateActiveLightsForSurface");
			const uint8_t ret = 0xC3;
			REL::safe_write(uid.address() + 5, &ret, 1);
		}

		// ---- Stealth detection fix -------------------------------------------
		// GetLightLevel (ID 38900/39946) iterates shadow lights to check which
		// affect the player. We replace that iteration with our own.
		// VR: 38900 confirmed (0x1406892e0); offsets assumed same as SE for VR.
		{
			static REL::RelocationID uid(38900, 39946);

			// Hook at the start of the affect-player loop.
			// Original bytes: "41 83 CE FF 33 C0" (6 bytes) -- keep them running first.
			uintptr_t off1 = REL::Relocate(0x185 - 0x050, 0x847 - 0x710, 0x185 - 0x050);
			if (!SKSE::stl::install_context_hook(uid.address() + off1, 6, Hook_UpdateLightLevelPlayer, 6))
				logger::error("[SCM] Failed to install Hook_UpdateLightLevelPlayer");

			// Byte patch: change JA (0x73) to JMP (0xEB) to skip the vanilla iteration.
			uintptr_t off2 = REL::Relocate(0x194 - 0x050, 0x856 - 0x710, 0x194 - 0x050);
			const uint8_t jmp = 0xEB;
			REL::safe_write(uid.address() + off2, &jmp, 1);
		}
		// Per-light check inside ShadowSceneNode::GetLuminanceAtPoint (ID 99725/106362).
		{
			static REL::RelocationID uid(99725, 106362);
			uintptr_t off = REL::Relocate(0x648 - 0x560, 0xB49 - 0xA60, 0x648 - 0x560);
			if (!SKSE::stl::install_context_hook(uid.address() + off, 5, Hook_CheckLightLevelPlayer))
				logger::error("[SCM] Failed to install Hook_CheckLightLevelPlayer");
		}

		// ---- Light conversion ------------------------------------------------
		// All conversion hooks install unconditionally; runtime behaviour is
		// gated by s_settings.ConvertExcessToNormal / PromoteNormalToShadow
		// and container membership. When both flags are false the hooks fire
		// but are no-ops -- required so toggling either flag on at runtime
		// takes effect without a restart.

		{
			// BSShadowLight vtable slot 3 = IsShadowLight; replace on all 4 shadow light types.
			// Reads s_normalConvert membership -- empty when ConvertExcessToNormal
			// off, so the hook returns vanilla truth for every light.
			REL::Relocation<uintptr_t> vtbl1{ RE::BSShadowLight::VTABLE[0] };
			vtbl1.write_vfunc(3, Hook_IsShadowLight);
			REL::Relocation<uintptr_t> vtbl2{ RE::BSShadowDirectionalLight::VTABLE[0] };
			vtbl2.write_vfunc(3, Hook_IsShadowLight);
			REL::Relocation<uintptr_t> vtbl3{ RE::BSShadowFrustumLight::VTABLE[0] };
			vtbl3.write_vfunc(3, Hook_IsShadowLight);
			REL::Relocation<uintptr_t> vtbl4{ RE::BSShadowParabolicLight::VTABLE[0] };
			vtbl4.write_vfunc(3, Hook_IsShadowLight);
		}

		// Parabolic Render (vtable 0x0A): repair the engine's omission of copying
		// cascade 0's shadowmapIndex to cascade 1, so teardown frees the right slot.
		stl::write_vfunc<0x0A, Hook_ParabolicRender>(RE::VTABLE_BSShadowParabolicLight[0]);

		// Contribution-cull point-light shadow casters (parabolic AppendVirtual).
		InstallCasterCullHook();

		{
			// ShadowSceneNode::RemoveLight -- fires at +0x9 (SE: 6 bytes, AE: 5 bytes).
			// Drains s_normalConvert / s_shadowConvert entries for the removed light.
			// No-op when both containers are empty.
			static REL::RelocationID uid(99697, 106331);
			int sz = REL::Relocate(6, 5, 6);
			if (!SKSE::stl::install_context_hook(uid.address() + REL::Relocate(0x9, 0x9, 0x9), sz, Hook_ConvertLights_Remove, sz))
				logger::error("[SCM] Failed to install Hook_ConvertLights_Remove");
		}

		{
			// ShadowSceneNode::ClearLightArrays -- bulk shadow-light teardown.
			// Full detour (not a prologue context hook): the thunk must
			// bracket the engine's frees to wait out in-flight shadow renders.
			if (long rc = stl::detour_thunk<Hook_ClearLightArrays>(REL::RelocationID(99704, 106338)))
				logger::error("[SCM] Failed to install Hook_ClearLightArrays ({})", rc);
		}

		{
			// ShadowSceneNode::ResetScene -- the portalGraph setter; the coc-time nuller
			// (via ResetCellGrid). Exclusive lock so it can't swap portalGraph while the
			// render-thread AccumulateLight holds it shared (crash#1 TOCTOU).
			if (long rc = stl::detour_thunk<Hook_ResetScene>(REL::RelocationID(99741, 106385)))
				logger::error("[SCM] Hook_ResetScene detour FAILED (DetourTransactionCommit={})", rc);
		}

		{
			// ShadowSceneNode::AccumulateLight -- hold s_portalGraphMutex shared across
			// the engine's per-light accumulate so ResetScene can't swap portalGraph
			// mid-call (crash#1 mid-function TOCTOU). Read side of the ResetScene lock.
			if (long rc = stl::detour_thunk<Hook_AccumulateLight>(REL::RelocationID(99753, 106401)))
				logger::error("[SCM] Hook_AccumulateLight detour FAILED (DetourTransactionCommit={})", rc);
		}

		{
			// BSShadowLight::ctor -- at function start (5 bytes). Zeroes the
			// never-initialized cullingProcess before the light exists to any
			// other system.
			static REL::RelocationID uid(100810, 107594);
			if (!SKSE::stl::install_context_hook(uid.address(), 5, Hook_ShadowLightCtor, 5))
				logger::error("[SCM] Failed to install Hook_ShadowLightCtor");
		}

		{
			// ShadowSceneNode::AddLight -- at function start (5 bytes).
			// Applies portal-strict per type (always) and PromoteNormalToShadow
			// flag mutation (when enabled).
			static REL::RelocationID uid(99692, 106326);
			if (!SKSE::stl::install_context_hook(uid.address(), 5, Hook_ConvertLights_Add, 5))
				logger::error("[SCM] Failed to install Hook_ConvertLights_Add");
		}

		{
			// BSLight::SetLight -- at function start (5 bytes).
			// Tracks NiLight* reassignments for s_shadowConvert. No-op when
			// PromoteNormalToShadow is off (s_shadowConvert is empty).
			static REL::RelocationID uid(101302, 108289);
			if (!SKSE::stl::install_context_hook(uid.address(), 5, Hook_ConvertLights_SetLight, 5))
				logger::error("[SCM] Failed to install Hook_ConvertLights_SetLight");
		}

		logger::info("[SCM] Hooks installed (ShadowLightCount={})", settings.ShadowLightCount);

		// The LoadingMenu reset is driven via LightLimitFix::OnSceneTransitionReset (render thread)
		// rather than a local main-thread sink, so ResetSession can't race the settings-menu table
		// iteration. See Feature::DrainSceneTransitions.

		// DXGI budget snapshot at install. Per-slice geometry follows once
		// Update() sees a non-null kSHADOWMAPS SRV.
		if (auto* menu = Menu::GetSingleton()) {
			if (auto adapter3 = menu->GetDXGIAdapter3()) {
				DXGI_QUERY_VIDEO_MEMORY_INFO vmem{};
				if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vmem)) && vmem.Budget > 0) {
					const float budgetMB = static_cast<float>(vmem.Budget) / (1024.f * 1024.f);
					const float usageMB = static_cast<float>(vmem.CurrentUsage) / (1024.f * 1024.f);
					logger::info("[SCM] VRAM at install: {:.1f}/{:.1f} MB used", usageMB, budgetMB);
				}
			}
		}
	}

}
