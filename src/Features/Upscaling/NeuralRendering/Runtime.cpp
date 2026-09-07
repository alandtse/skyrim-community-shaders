#include "Runtime.h"

#include "Util.h"

#include <Psapi.h>
#include <Windows.h>
#include <d3d12.h>
#include <nvsdk_ngx.h>

#include <array>
#include <cstring>
#include <format>
#include <vector>

#include <nvsdk_ngx_helpers.h>

namespace NeuralRendering
{
	namespace
	{
		constexpr wchar_t kRuntimeName[] = L"nvngx_dlssnr.dll";
		constexpr auto kFeatureDlssNr = static_cast<NVSDK_NGX_Feature>(18);
		constexpr std::array<const char*, 5> kRequiredExports{
			"NVSDK_NGX_D3D12_Init_Ext",
			"NVSDK_NGX_D3D12_CreateFeature",
			"NVSDK_NGX_D3D12_EvaluateFeature",
			"NVSDK_NGX_D3D12_ReleaseFeature",
			"NVSDK_NGX_D3D12_Shutdown1",
		};

		using GetUnsignedValue = unsigned int(NVSDK_CONV*)();
		using InitD3D12 = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version, const NVSDK_NGX_Parameter*);
		using ShutdownD3D12 = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12Device*);
		using AllocateParameters = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
		using DestroyParameters = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);
		using CreateFeature = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
		using EvaluateFeature = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
		using ReleaseFeature = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
		using GetModuleFileNameWFunction = DWORD(WINAPI*)(HMODULE, LPWSTR, DWORD);

		GetModuleFileNameWFunction g_originalGetModuleFileNameW = nullptr;
		HMODULE g_callerModule = nullptr;
		std::wstring g_spoofedRuntimePath;
		std::uint32_t g_proxyHits = 0;

		HMODULE FindNgxCoreModule()
		{
			std::array<HMODULE, 1024> modules{};
			DWORD bytesNeeded = 0;
			if (!K32EnumProcessModules(GetCurrentProcess(), modules.data(), static_cast<DWORD>(sizeof(modules)), &bytesNeeded))
				return nullptr;
			const std::size_t count = std::min<std::size_t>(modules.size(), bytesNeeded / sizeof(HMODULE));
			for (std::size_t index = 0; index < count; ++index) {
				if (GetProcAddress(modules[index], "NVSDK_NGX_D3D12_AllocateParameters") &&
					GetProcAddress(modules[index], "NVSDK_NGX_D3D12_DestroyParameters"))
					return modules[index];
			}
			return nullptr;
		}

		DWORD WINAPI SignedRuntimeGetModuleFileNameW(HMODULE module, LPWSTR filename, DWORD size)
		{
			if (module == g_callerModule && filename && size && !g_spoofedRuntimePath.empty()) {
				++g_proxyHits;
				const DWORD length = static_cast<DWORD>(g_spoofedRuntimePath.size());
				const DWORD copyLength = std::min(length, size - 1);
				std::memcpy(filename, g_spoofedRuntimePath.data(), copyLength * sizeof(wchar_t));
				filename[copyLength] = L'\0';
				return copyLength < length ? size : length;
			}
			return g_originalGetModuleFileNameW ? g_originalGetModuleFileNameW(module, filename, size) : 0;
		}

		class SignedRuntimePathScope
		{
		public:
			SignedRuntimePathScope(HMODULE runtime, const std::filesystem::path& ngxPath)
			{
				if (!runtime)
					return;
				GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCWSTR>(&SignedRuntimeGetModuleFileNameW), &g_callerModule);
				g_spoofedRuntimePath = ngxPath.wstring();
				g_proxyHits = 0;

				auto* base = reinterpret_cast<std::byte*>(runtime);
				auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
				if (dos->e_magic != IMAGE_DOS_SIGNATURE)
					return;
				auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
				if (nt->Signature != IMAGE_NT_SIGNATURE)
					return;
				const auto& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
				if (!imports.VirtualAddress)
					return;

				auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
				for (; descriptor->Name; ++descriptor) {
					if (!descriptor->OriginalFirstThunk)
						continue;
					auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);
					auto* functions = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
					for (; names->u1.AddressOfData; ++names, ++functions) {
						if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
							continue;
						auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
						if (std::strcmp(reinterpret_cast<const char*>(import->Name), "GetModuleFileNameW"))
							continue;
						slot_ = reinterpret_cast<void**>(&functions->u1.Function);
						g_originalGetModuleFileNameW = reinterpret_cast<GetModuleFileNameWFunction>(*slot_);
						DWORD oldProtection = 0;
						if (VirtualProtect(slot_, sizeof(*slot_), PAGE_READWRITE, &oldProtection)) {
							*slot_ = reinterpret_cast<void*>(&SignedRuntimeGetModuleFileNameW);
							DWORD ignored = 0;
							VirtualProtect(slot_, sizeof(*slot_), oldProtection, &ignored);
							FlushInstructionCache(GetCurrentProcess(), slot_, sizeof(*slot_));
							installed_ = true;
						}
						return;
					}
				}
			}

			~SignedRuntimePathScope()
			{
				if (installed_ && slot_) {
					DWORD oldProtection = 0;
					if (VirtualProtect(slot_, sizeof(*slot_), PAGE_READWRITE, &oldProtection)) {
						*slot_ = reinterpret_cast<void*>(g_originalGetModuleFileNameW);
						DWORD ignored = 0;
						VirtualProtect(slot_, sizeof(*slot_), oldProtection, &ignored);
						FlushInstructionCache(GetCurrentProcess(), slot_, sizeof(*slot_));
					}
				}
				g_spoofedRuntimePath.clear();
				g_callerModule = nullptr;
				g_originalGetModuleFileNameW = nullptr;
			}

			[[nodiscard]] bool IsInstalled() const { return installed_; }
			[[nodiscard]] std::uint32_t Hits() const { return g_proxyHits; }

		private:
			void** slot_ = nullptr;
			bool installed_ = false;
		};

		std::filesystem::path ResolveRuntimePath(const std::filesystem::path& explicitPath)
		{
			if (!explicitPath.empty())
				return std::filesystem::is_directory(explicitPath) ? explicitPath / kRuntimeName : explicitPath;
			const auto dataPath = Util::PathHelpers::GetDataPath();
			const std::array candidates{
				dataPath / L"Shaders/Upscaling/Streamline" / kRuntimeName,
				dataPath / L"Shaders/Upscaling/StreamlineDX12" / kRuntimeName,
			};
			for (const auto& candidate : candidates) {
				std::error_code error;
				if (std::filesystem::is_regular_file(candidate, error))
					return candidate;
			}
			return {};
		}
	}

	Runtime& Runtime::Instance()
	{
		static Runtime instance;
		return instance;
	}

	Runtime::~Runtime() { Shutdown(); }

	bool Runtime::Probe(const std::filesystem::path& explicitPath)
	{
		Shutdown();
		path_ = ResolveRuntimePath(explicitPath);
		if (path_.empty()) {
			status_ = RuntimeStatus::NotFound;
			detail_ = "nvngx_dlssnr.dll was not found";
			return false;
		}
		const auto version = Util::GetDllVersion(path_.wstring());
		if (!version) {
			status_ = RuntimeStatus::VersionUnavailable;
			detail_ = "DLL version resource is unavailable";
			return false;
		}
		version_ = Util::GetFormattedVersion(*version);
		if (version->major() != 310 || version->minor() != 8) {
			status_ = RuntimeStatus::UnsupportedVersion;
			detail_ = std::format("expected DLSSNR 310.8.x, found {}", version_);
			return false;
		}
		module_ = LoadLibraryExW(path_.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
		if (!module_) {
			status_ = RuntimeStatus::LoadFailed;
			detail_ = std::format("LoadLibraryExW failed with {}", GetLastError());
			return false;
		}
		for (const char* exportName : kRequiredExports) {
			if (!GetProcAddress(static_cast<HMODULE>(module_), exportName)) {
				status_ = RuntimeStatus::MissingExport;
				detail_ = std::format("missing export {}", exportName);
				FreeLibrary(static_cast<HMODULE>(module_));
				module_ = nullptr;
				return false;
			}
		}
		auto getAppId = reinterpret_cast<GetUnsignedValue>(GetProcAddress(static_cast<HMODULE>(module_), "NVSDK_NGX_GetApplicationId"));
		auto getApi = reinterpret_cast<GetUnsignedValue>(GetProcAddress(static_cast<HMODULE>(module_), "NVSDK_NGX_GetAPIVersion"));
		if (!getAppId || !getApi) {
			status_ = RuntimeStatus::MissingExport;
			detail_ = "signed runtime identity exports are missing";
			FreeLibrary(static_cast<HMODULE>(module_));
			module_ = nullptr;
			return false;
		}
		applicationId_ = getAppId();
		apiVersion_ = getApi();
		status_ = RuntimeStatus::Ready;
		return true;
	}

	bool Runtime::Initialize(ID3D12Device* device, const std::filesystem::path& dataPath)
	{
		if (!device)
			return false;
		if (status_ == RuntimeStatus::Initialized && device_ != device)
			Shutdown();
		if (!module_ && !Probe())
			return false;
		if (status_ == RuntimeStatus::Initialized && device_ == device)
			return true;

		auto initialize = reinterpret_cast<InitD3D12>(GetProcAddress(static_cast<HMODULE>(module_), "NVSDK_NGX_D3D12_Init_Ext"));
		std::filesystem::path writablePath = dataPath;
		if (writablePath.empty()) {
			wchar_t tempPath[MAX_PATH]{};
			GetTempPathW(MAX_PATH, tempPath);
			writablePath = std::filesystem::path(tempPath) / L"OpenShaders-NGX";
		}
		std::error_code error;
		std::filesystem::create_directories(writablePath, error);
		SignedRuntimePathScope scope(static_cast<HMODULE>(module_), path_.parent_path() / L"nvngx.dll");
		if (!scope.IsInstalled()) {
			status_ = RuntimeStatus::InitializationFailed;
			detail_ = "failed to install signed-runtime path proxy";
			return false;
		}
		ngxResult_ = static_cast<std::uint32_t>(initialize(applicationId_, writablePath.c_str(), device,
			static_cast<NVSDK_NGX_Version>(apiVersion_), nullptr));
		if (ngxResult_ != NVSDK_NGX_Result_Success) {
			status_ = RuntimeStatus::InitializationFailed;
			detail_ = std::format("NGX init failed 0x{:08X}, proxyHits={}", ngxResult_, scope.Hits());
			return false;
		}
		device_ = device;
		device_->AddRef();

		HMODULE core = FindNgxCoreModule();
		if (!core) {
			status_ = RuntimeStatus::CoreUnavailable;
			detail_ = "NGX parameter API module was not found";
			Shutdown();
			status_ = RuntimeStatus::CoreUnavailable;
			detail_ = "NGX parameter API module was not found";
			return false;
		}
		auto allocate = reinterpret_cast<AllocateParameters>(GetProcAddress(core, "NVSDK_NGX_D3D12_AllocateParameters"));
		NVSDK_NGX_Parameter* parameters = nullptr;
		ngxResult_ = static_cast<std::uint32_t>(allocate(&parameters));
		if (ngxResult_ != NVSDK_NGX_Result_Success || !parameters) {
			const auto allocationResult = ngxResult_;
			Shutdown();
			status_ = RuntimeStatus::ParameterAllocationFailed;
			ngxResult_ = allocationResult;
			detail_ = std::format("NGX parameter allocation failed 0x{:08X}", allocationResult);
			return false;
		}
		parameters_ = parameters;
		status_ = RuntimeStatus::Initialized;
		return true;
	}

	bool Runtime::Execute(ID3D12GraphicsCommandList* commandList, std::uint32_t slot,
		ID3D12Resource* color, ID3D12Resource* depth, ID3D12Resource* motionVectors, ID3D12Resource* output,
		std::uint32_t inputWidth, std::uint32_t inputHeight, std::uint32_t outputWidth, std::uint32_t outputHeight,
		std::uint32_t guideWidth, std::uint32_t guideHeight,
		float motionVectorScaleX, float motionVectorScaleY, const Tuning& tuning, bool reset)
	{
		if (status_ != RuntimeStatus::Initialized || !commandList || slot >= kFeatureSlotCount || !color || !depth || !motionVectors || !output)
			return false;
		auto* parameters = static_cast<NVSDK_NGX_Parameter*>(parameters_);
		auto create = reinterpret_cast<CreateFeature>(GetProcAddress(static_cast<HMODULE>(module_), "NVSDK_NGX_D3D12_CreateFeature"));
		auto evaluate = reinterpret_cast<EvaluateFeature>(GetProcAddress(static_cast<HMODULE>(module_), "NVSDK_NGX_D3D12_EvaluateFeature"));
		auto release = reinterpret_cast<ReleaseFeature>(GetProcAddress(static_cast<HMODULE>(module_), "NVSDK_NGX_D3D12_ReleaseFeature"));
		SignedRuntimePathScope scope(static_cast<HMODULE>(module_), path_.parent_path() / L"nvngx.dll");
		if (!scope.IsInstalled())
			return false;

		const bool dimensionsChanged = featureInputWidth_[slot] != inputWidth || featureInputHeight_[slot] != inputHeight ||
		                               featureOutputWidth_[slot] != outputWidth || featureOutputHeight_[slot] != outputHeight;
		if (featureHandles_[slot] && dimensionsChanged) {
			release(static_cast<NVSDK_NGX_Handle*>(featureHandles_[slot]));
			featureHandles_[slot] = nullptr;
		}

		if (!featureHandles_[slot]) {
			parameters->Reset();
			parameters->Set("Width", outputWidth);
			parameters->Set("Height", outputHeight);
			parameters->Set("OutWidth", outputWidth);
			parameters->Set("OutHeight", outputHeight);
			parameters->Set("DLSSNR.Width", outputWidth);
			parameters->Set("DLSSNR.Height", outputHeight);
			parameters->Set("DLSSNR.InputWidth", inputWidth);
			parameters->Set("DLSSNR.InputHeight", inputHeight);
			parameters->Set("DLSSNR.OutputWidth", outputWidth);
			parameters->Set("DLSSNR.OutputHeight", outputHeight);
			parameters->Set("DLSSNR.Output.Width", outputWidth);
			parameters->Set("DLSSNR.Output.Height", outputHeight);
			parameters->Set("DLSSNR.Scale", static_cast<float>(outputWidth) / inputWidth);
			parameters->Set("DLSSNR.Upscaling", 1u);
			parameters->Set("DLSSNR.ScalingRatio", static_cast<float>(outputWidth) / inputWidth);
			parameters->Set("DLSSNR.Hint.Render.Preset", 0u);
			NVSDK_NGX_Handle* handle = nullptr;
			ngxResult_ = static_cast<std::uint32_t>(create(commandList, kFeatureDlssNr, parameters, &handle));
			if (ngxResult_ != NVSDK_NGX_Result_Success || !handle) {
				detail_ = std::format("Feature 18 create failed slot={} result=0x{:08X} proxyHits={}", slot, ngxResult_, scope.Hits());
				return false;
			}
			featureHandles_[slot] = handle;
			featureInputWidth_[slot] = inputWidth;
			featureInputHeight_[slot] = inputHeight;
			featureOutputWidth_[slot] = outputWidth;
			featureOutputHeight_[slot] = outputHeight;
			reset = true;
		}

		parameters->Reset();
		parameters->Set("DLSSNR.Color", color);
		parameters->Set("DLSSNR.Depth", depth);
		parameters->Set("DLSSNR.MVec", motionVectors);
		parameters->Set("DLSSNR.Output", output);
		parameters->Set("DLSSNR.ColorSubrectBaseX", 0u);
		parameters->Set("DLSSNR.ColorSubrectBaseY", 0u);
		parameters->Set("DLSSNR.ColorSubrectWidth", outputWidth);
		parameters->Set("DLSSNR.ColorSubrectHeight", outputHeight);
		parameters->Set("DLSSNR.DepthSubrectBaseX", 0u);
		parameters->Set("DLSSNR.DepthSubrectBaseY", 0u);
		parameters->Set("DLSSNR.DepthSubrectWidth", guideWidth);
		parameters->Set("DLSSNR.DepthSubrectHeight", guideHeight);
		parameters->Set("DLSSNR.MVecSubrectBaseX", 0u);
		parameters->Set("DLSSNR.MVecSubrectBaseY", 0u);
		parameters->Set("DLSSNR.MVecSubrectWidth", guideWidth);
		parameters->Set("DLSSNR.MVecSubrectHeight", guideHeight);
		parameters->Set("DLSSNR.OutputSubrectBaseX", 0u);
		parameters->Set("DLSSNR.OutputSubrectBaseY", 0u);
		parameters->Set("DLSSNR.OutputSubrectWidth", outputWidth);
		parameters->Set("DLSSNR.OutputSubrectHeight", outputHeight);
		parameters->Set("DLSSNR.MVecScaleX", motionVectorScaleX);
		parameters->Set("DLSSNR.MVecScaleY", motionVectorScaleY);
		parameters->Set("DLSSNR.DepthInverted", 0u);
		parameters->Set("DLSSNR.Enabled", 1u);
		parameters->Set("DLSSNR.Reset", reset ? 1u : 0u);
		parameters->Set("DLSSNR.Intensity", tuning.intensity);
		parameters->Set("DLSSNR.LocalToneStrength", tuning.localToneStrength);
		parameters->Set("DLSSNR.LocalStructureStrength", tuning.localStructureStrength);
		parameters->Set("DLSSNR.SkinStructureStrength", tuning.skinStructureStrength);
		parameters->Set("DLSSNR.UseAutoMask", tuning.useAutoMask ? 1u : 0u);
		parameters->Set("DLSSNR.Style", tuning.style);
		parameters->Set("DLSSNR.UICorrection", tuning.uiCorrection ? 1u : 0u);
		ngxResult_ = static_cast<std::uint32_t>(evaluate(commandList,
			static_cast<NVSDK_NGX_Handle*>(featureHandles_[slot]), parameters, nullptr));
		if (ngxResult_ != NVSDK_NGX_Result_Success) {
			detail_ = std::format("Feature 18 evaluate failed slot={} result=0x{:08X}", slot, ngxResult_);
			return false;
		}
		++successfulFrames_;
		return true;
	}

	void Runtime::ResetFeature(std::uint32_t slot)
	{
		if (!module_ || slot >= kFeatureSlotCount)
			return;
		SignedRuntimePathScope scope(static_cast<HMODULE>(module_), path_.parent_path() / L"nvngx.dll");
		auto release = reinterpret_cast<ReleaseFeature>(GetProcAddress(static_cast<HMODULE>(module_), "NVSDK_NGX_D3D12_ReleaseFeature"));
		if (featureHandles_[slot] && release) {
			const auto result = release(static_cast<NVSDK_NGX_Handle*>(featureHandles_[slot]));
			if (result != NVSDK_NGX_Result_Success)
				logger::warn("[DLSSNR] Feature 18 release failed slot={} result=0x{:08X} proxyInstalled={}",
					slot, static_cast<std::uint32_t>(result), scope.IsInstalled());
		}
		featureHandles_[slot] = nullptr;
		featureInputWidth_[slot] = featureInputHeight_[slot] = 0;
		featureOutputWidth_[slot] = featureOutputHeight_[slot] = 0;
	}

	void Runtime::ResetFeatures()
	{
		for (std::uint32_t slot = 0; slot < kFeatureSlotCount; ++slot)
			ResetFeature(slot);
		successfulFrames_ = 0;
	}

	void Runtime::Shutdown()
	{
		if (device_ && module_) {
			ResetFeatures();
			HMODULE core = FindNgxCoreModule();
			if (parameters_ && core) {
				auto destroy = reinterpret_cast<DestroyParameters>(GetProcAddress(core, "NVSDK_NGX_D3D12_DestroyParameters"));
				if (destroy)
					destroy(static_cast<NVSDK_NGX_Parameter*>(parameters_));
			}
			parameters_ = nullptr;
			auto shutdown = reinterpret_cast<ShutdownD3D12>(GetProcAddress(static_cast<HMODULE>(module_), "NVSDK_NGX_D3D12_Shutdown1"));
			SignedRuntimePathScope scope(static_cast<HMODULE>(module_), path_.parent_path() / L"nvngx.dll");
			if (shutdown) {
				const auto result = shutdown(device_);
				if (result != NVSDK_NGX_Result_Success)
					logger::warn("[DLSSNR] NGX shutdown failed result=0x{:08X} proxyInstalled={}",
						static_cast<std::uint32_t>(result), scope.IsInstalled());
			}
			device_->Release();
			device_ = nullptr;
		}
		if (module_)
			FreeLibrary(static_cast<HMODULE>(module_));
		module_ = nullptr;
		status_ = RuntimeStatus::NotProbed;
		path_.clear();
		version_.clear();
		detail_.clear();
		ngxResult_ = applicationId_ = apiVersion_ = 0;
		successfulFrames_ = 0;
	}

	const char* ToString(RuntimeStatus status)
	{
		switch (status) {
		case RuntimeStatus::NotProbed:
			return "not-probed";
		case RuntimeStatus::NotFound:
			return "not-found";
		case RuntimeStatus::VersionUnavailable:
			return "version-unavailable";
		case RuntimeStatus::UnsupportedVersion:
			return "unsupported-version";
		case RuntimeStatus::LoadFailed:
			return "load-failed";
		case RuntimeStatus::MissingExport:
			return "missing-export";
		case RuntimeStatus::Ready:
			return "ready";
		case RuntimeStatus::InitializationFailed:
			return "initialization-failed";
		case RuntimeStatus::CoreUnavailable:
			return "core-unavailable";
		case RuntimeStatus::ParameterAllocationFailed:
			return "parameter-allocation-failed";
		case RuntimeStatus::Initialized:
			return "initialized";
		}
		return "unknown";
	}
}
