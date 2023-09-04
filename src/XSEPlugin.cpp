#include "Hooks.h"

#include "Menu.h"
#include "ShaderCache.h"
#include "State.h"

#include "ENB/ENBSeriesAPI.h"
#include "Features/ExtendedMaterials.h"
#include "Features/LightLimitFIx/ParticleLights.h"
#include "Features/LightLimitFix.h"
#include "higgsinterface001.h"

#define DLLEXPORT __declspec(dllexport)

std::list<std::string> errors;
const SKSE::MessagingInterface* messaging;

bool Load();

void InitializeLog([[maybe_unused]] spdlog::level::level_enum a_level = spdlog::level::info)
{
#ifndef NDEBUG
	auto sink = std::make_shared<spdlog::sinks::msvc_sink_mt>();
#else
	auto path = logger::log_directory();
	if (!path) {
		util::report_and_fail("Failed to find standard logging directory"sv);
	}

	*path /= std::format("{}.log"sv, Plugin::NAME);
	auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
#endif

#ifndef NDEBUG
	const auto level = spdlog::level::trace;
#else
	const auto level = a_level;
#endif

	auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));
	log->set_level(level);
	log->flush_on(spdlog::level::info);

	spdlog::set_default_logger(std::move(log));
	spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] [%s:%#] %v");
}

extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
#ifndef NDEBUG
	while (!WinAPI::IsDebuggerPresent()) {};
#endif
	InitializeLog();
	logger::info("Loaded {} {}", Plugin::NAME, Plugin::VERSION.string());
	SKSE::Init(a_skse);
	return Load();
}

extern "C" DLLEXPORT constinit auto SKSEPlugin_Version = []() noexcept {
	SKSE::PluginVersionData v;
	v.PluginName(Plugin::NAME.data());
	v.PluginVersion(Plugin::VERSION);
	v.UsesAddressLibrary(true);
	v.HasNoStructUse();
	return v;
}();

extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Query(const SKSE::QueryInterface*, SKSE::PluginInfo* pluginInfo)
{
	pluginInfo->name = SKSEPlugin_Version.pluginName;
	pluginInfo->infoVersion = SKSE::PluginInfo::kVersion;
	pluginInfo->version = SKSEPlugin_Version.pluginVersion;
	return true;
}

void MessageHandler(SKSE::MessagingInterface::Message* message)
{
	switch (message->type) {
	case SKSE::MessagingInterface::kPostPostLoad:
		{
			const std::array dlls = {
				L"ShaderTools.dll",
				L"SSEShaderTools.dll"
			};

			for (const auto dll : dlls) {
				if (GetModuleHandle(dll)) {
					auto errorMessage = std::format("Incompatible DLL {} detected", stl::utf16_to_utf8(dll).value_or("<unicode conversion error>"s));
					logger::error("{}", errorMessage);

					errors.push_back(errorMessage);
				}
			}

			if (errors.empty()) {
				Hooks::Install();

				auto& shaderCache = SIE::ShaderCache::Instance();

				shaderCache.SetEnabled(true);
				shaderCache.SetAsync(true);
				shaderCache.SetDiskCache(true);
				shaderCache.SetDump(false);

				State::GetSingleton()->Load();

				shaderCache.ValidateDiskCache();

				if (LightLimitFix::GetSingleton()->loaded) {
					ParticleLights::GetSingleton()->GetConfigs();
					LightLimitFix::InstallHooks();
				}
			}
			if (REL::Module::IsVR())
			{
				HiggsPluginAPI::GetHiggsInterface001(SKSE::GetPluginHandle(), messaging);
				if (g_higgsInterface) {
					logger::info("Got higgs interface!");

					unsigned int higgsVersion = g_higgsInterface->GetBuildNumber();
					if (higgsVersion < 1060000) {
						logger::warn("[CRITICAL] HIGGS is present but is a lower version than is required by this mod. Get the latest version of HIGGS and try again.");
					}

				} else {
					logger::warn("[WARN] Did NOT get higgs interface. Hand collisions disabled.");
				}
			}
			break;
		}
	case SKSE::MessagingInterface::kDataLoaded:
		{
			for (auto it = errors.begin(); it != errors.end(); ++it) {
				auto& errorMessage = *it;
				RE::DebugMessageBox(std::format("Community Shaders\n{}, will disable all hooks and features", errorMessage).c_str());
			}

			if (errors.empty()) {
				RE::BSInputDeviceManager::GetSingleton()->AddEventSink(Menu::GetSingleton());

				auto& shaderCache = SIE::ShaderCache::Instance();

				while (shaderCache.GetCompletedTasks() != shaderCache.GetTotalTasks()) {
					std::this_thread::sleep_for(100ms);
				}

				if (shaderCache.IsDiskCache()) {
					shaderCache.WriteDiskCacheInfo();
				}

				if (LightLimitFix::GetSingleton()->loaded)
					LightLimitFix::GetSingleton()->DataLoaded();
			}

			break;
		}
	}
}

bool Load()
{
	if (ENB_API::RequestENBAPI()) {
		logger::info("ENB detected, disabling all hooks and features");
		return true;
	}

	messaging = SKSE::GetMessagingInterface();
	messaging->RegisterListener("SKSE", MessageHandler);

	auto state = State::GetSingleton();
	state->Load();
	auto log = spdlog::default_logger();
	log->set_level(state->GetLogLevel());

	return true;
}