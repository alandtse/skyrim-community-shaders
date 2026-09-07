#include "Precipitation.h"

#include "Features/Skylighting.h"
#include "Globals.h"

namespace
{
	struct Main_RenderPrecipitation
	{
		static void thunk()
		{
			auto* graphicsState = globals::game::graphicsState;
			if (!graphicsState) {
				func();
				return;
			}

			auto& runtimeData = graphicsState->GetRuntimeData();
			const auto dynamicResolutionLock = runtimeData.dynamicResolutionLock;
			runtimeData.dynamicResolutionLock = 1;

			const SKSE::stl::scope_exit restoreState([&]() noexcept {
				runtimeData.dynamicResolutionLock = dynamicResolutionLock;
			});

			auto& skylighting = globals::features::skylighting;
			if (skylighting.loaded)
				skylighting.RenderOcclusion();
			else
				func();
		}

		static inline REL::Relocation<decltype(thunk)> func;
	};

	bool precipitationHookInstalled = false;
}

void Precipitation::Install()
{
	if (precipitationHookInstalled)
		return;

	stl::write_thunk_call<Main_RenderPrecipitation>(REL::RelocationID(35560, 36559).address() + REL::Relocate<std::uintptr_t>(0x3A1, REL::Module::IsAtLeast(REL::Version(1, 7, 99, 0)) ? 0x3BF : 0x3A1, 0x2FA));
	precipitationHookInstalled = true;
	logger::info("[Precipitation] Installed shared render hook");
}

void Precipitation::RenderOriginal()
{
	Main_RenderPrecipitation::func();
}
