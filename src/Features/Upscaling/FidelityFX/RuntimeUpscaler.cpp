// Runtime FSR4/FSR3-provider DLL path (falls back to the host SDK on any failure). Split
// into its own file per the LightLimitFix/*.cpp convention -- these are FidelityFX methods.

#include "../FidelityFX.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <directx/d3dx12.h>
#include <format>
#include <limits>
#include <string>
#include <vector>

#include "../../../GpuPass.h"
#include "../../../State.h"
#include "../../Upscaling.h"
#include "../DX12SwapChain.h"

extern ffxFunctions ffxModule;

// Defined in FidelityFX.cpp; used by UpscaleRegion's host-FSR3 fallback path below.
FfxResource ffxGetResource(ID3D11Resource* dx11Resource, wchar_t const* ffxResName, FfxResourceStates state = FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);

namespace
{
	constexpr uint32_t kAmdVendorId = 0x1002u;
	constexpr uint32_t kNvidiaVendorId = 0x10DEu;

	enum class D3D11IdleFenceResult : uint8_t
	{
		Ready,
		Pending,
		Failed
	};

	void ReleaseD3D11IdleFence(ID3D11Query*& a_query)
	{
		if (!a_query)
			return;

		a_query->Release();
		a_query = nullptr;
	}

	D3D11IdleFenceResult BeginOrPollD3D11IdleFence(ID3D11DeviceContext* a_context, ID3D11Query*& a_query)
	{
		if (!a_context) {
			ReleaseD3D11IdleFence(a_query);
			return D3D11IdleFenceResult::Ready;
		}

		const auto pollFence = [&]() {
			BOOL completed = FALSE;
			const HRESULT dataResult = a_context->GetData(a_query, &completed, sizeof(completed), 0);
			if (dataResult == S_OK && completed) {
				ReleaseD3D11IdleFence(a_query);
				return D3D11IdleFenceResult::Ready;
			}

			if (dataResult == S_FALSE || dataResult == S_OK)
				return D3D11IdleFenceResult::Pending;

			ReleaseD3D11IdleFence(a_query);
			return D3D11IdleFenceResult::Failed;
		};

		if (a_query)
			return pollFence();

		ID3D11Device* device = nullptr;
		a_context->GetDevice(&device);
		if (!device) {
			a_context->Flush();
			return D3D11IdleFenceResult::Ready;
		}

		D3D11_QUERY_DESC queryDesc{};
		queryDesc.Query = D3D11_QUERY_EVENT;

		const HRESULT createResult = device->CreateQuery(&queryDesc, &a_query);
		device->Release();

		if (FAILED(createResult) || !a_query) {
			a_context->Flush();
			return D3D11IdleFenceResult::Failed;
		}

		a_context->End(a_query);
		a_context->Flush();
		return pollFence();
	}

	bool UseSplitPerEyeFSRContexts()
	{
		return globals::game::isVR;
	}

	bool TryGetTexture2DDesc(ID3D11Resource* a_resource, D3D11_TEXTURE2D_DESC& a_outDesc)
	{
		if (!a_resource)
			return false;

		winrt::com_ptr<ID3D11Texture2D> texture;
		if (FAILED(a_resource->QueryInterface(IID_PPV_ARGS(texture.put()))))
			return false;

		texture->GetDesc(&a_outDesc);
		return true;
	}

	bool TryGetCurrentAdapterDesc(DXGI_ADAPTER_DESC& a_outDesc)
	{
		if (!globals::d3d::device)
			return false;

		winrt::com_ptr<IDXGIDevice> dxgiDevice;
		if (FAILED(globals::d3d::device->QueryInterface(IID_PPV_ARGS(dxgiDevice.put()))))
			return false;

		winrt::com_ptr<IDXGIAdapter> adapter;
		if (FAILED(dxgiDevice->GetAdapter(adapter.put())))
			return false;

		a_outDesc = {};
		if (FAILED(adapter->GetDesc(&a_outDesc)))
			return false;

		return true;
	}

	std::string ToUpperAscii(std::string a_value)
	{
		std::transform(a_value.begin(), a_value.end(), a_value.begin(), [](unsigned char c) {
			return static_cast<char>(std::toupper(c));
		});
		return a_value;
	}

	FidelityFX::Fsr4AdapterSupport ClassifyFsr4AdapterSupport(const DXGI_ADAPTER_DESC& a_desc)
	{
		if (a_desc.VendorId != kAmdVendorId)
			return FidelityFX::Fsr4AdapterSupport::Unsupported;

		std::wstring wideDescription(a_desc.Description);
		const std::string description = ToUpperAscii(stl::utf16_to_utf8(wideDescription).value_or(""));

		// Pre-RDNA "HD"-branded GCN cards (e.g. "Radeon HD 7970") fall in the same 7000-7999
		// numeric range as RX 7000 without being FSR 4.1.1-capable; reject them before the scan.
		if (description.find("RADEON HD") != std::string::npos)
			return FidelityFX::Fsr4AdapterSupport::Unsupported;

		// Only accept explicit discrete RDNA3 dies here, never a bare "RDNA 3"/"RDNA3" marker --
		// that also matches RDNA3 integrated GPUs, which FSR 4.1.1 does not support.
		if (description.find("NAVI31") != std::string::npos ||
			description.find("NAVI 31") != std::string::npos ||
			description.find("NAVI32") != std::string::npos ||
			description.find("NAVI 32") != std::string::npos ||
			description.find("NAVI33") != std::string::npos ||
			description.find("NAVI 33") != std::string::npos) {
			return FidelityFX::Fsr4AdapterSupport::RadeonRx7000;
		}

		if (description.find("RDNA4") != std::string::npos ||
			description.find("RDNA 4") != std::string::npos ||
			description.find("NAVI4") != std::string::npos ||
			description.find("NAVI 4") != std::string::npos) {
			return FidelityFX::Fsr4AdapterSupport::RadeonRx9000;
		}

		size_t searchPosition = 0;
		while (searchPosition < description.length()) {
			while (searchPosition < description.length() && !std::isdigit(static_cast<unsigned char>(description[searchPosition])))
				searchPosition++;

			const size_t modelStart = searchPosition;
			while (searchPosition < description.length() && std::isdigit(static_cast<unsigned char>(description[searchPosition])))
				searchPosition++;

			if (searchPosition > modelStart) {
				const std::string modelText = description.substr(modelStart, searchPosition - modelStart);
				char* parseEnd = nullptr;
				const unsigned long modelNumber = std::strtoul(modelText.c_str(), &parseEnd, 10);
				if (parseEnd != modelText.c_str()) {
					if (modelNumber >= 7000ul && modelNumber < 8000ul)
						return FidelityFX::Fsr4AdapterSupport::RadeonRx7000;
					if (modelNumber >= 9000ul && modelNumber < 10000ul)
						return FidelityFX::Fsr4AdapterSupport::RadeonRx9000;
				}
			}
		}

		// Keep fallbacks for abbreviated naming variants that don't include full numeric model text.
		if (description.find("RX 70") != std::string::npos ||
			description.find("RX70") != std::string::npos ||
			description.find("RADEON 70") != std::string::npos) {
			return FidelityFX::Fsr4AdapterSupport::RadeonRx7000;
		}
		if (description.find("RX 90") != std::string::npos ||
			description.find("RX90") != std::string::npos ||
			description.find("RADEON 90") != std::string::npos) {
			return FidelityFX::Fsr4AdapterSupport::RadeonRx9000;
		}

		return FidelityFX::Fsr4AdapterSupport::Unsupported;
	}

	std::string UpscalerVersionToString(uint32_t a_version)
	{
		const uint32_t major = (a_version >> 22) & 0x3FFu;
		const uint32_t minor = (a_version >> 12) & 0x3FFu;
		const uint32_t patch = a_version & 0xFFFu;
		return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
	}

	const std::string& PendingFsrDispatchLabel()
	{
		static const std::string label = "Pending FSR dispatch";
		return label;
	}

	const std::string& HostFsrFallbackLabel()
	{
		static const std::string label = std::format("{} fallback", FidelityFX::GetHostFsrSdkLabel());
		return label;
	}

	bool RuntimeProviderNameMatchesVersion(const std::string& a_providerName, uint32_t a_version)
	{
		return !a_providerName.empty() &&
		       a_providerName.find(UpscalerVersionToString(a_version)) != std::string::npos;
	}

	bool RuntimeProviderIdMatchesVersion(uint64_t a_providerId, uint32_t a_version)
	{
		return a_providerId != 0 &&
		       static_cast<uint32_t>(a_providerId & 0xFFFFFFFFull) == a_version;
	}

	bool RuntimeProviderMatchesVersion(uint64_t a_providerId, const std::string& a_providerName, uint32_t a_version)
	{
		return RuntimeProviderIdMatchesVersion(a_providerId, a_version) ||
		       RuntimeProviderNameMatchesVersion(a_providerName, a_version);
	}

	std::string RuntimeProviderDisplayName(uint64_t a_providerId, const std::string& a_providerName)
	{
		if (!a_providerName.empty())
			return a_providerName;
		if (a_providerId != 0)
			return std::format("id 0x{:X}", a_providerId);

		return {};
	}

	bool QueryRuntimeUpscalerVersionId(ID3D12Device* a_device, uint32_t a_requestedVersion, uint64_t& a_versionId, std::string& a_versionName)
	{
		a_versionId = 0;
		a_versionName.clear();

		if (!a_device || !ffxModule.Query) {
			return false;
		}

		uint64_t versionCount = 0;
		ffxQueryDescGetVersions countQuery{};
		countQuery.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
		countQuery.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
		countQuery.device = a_device;
		countQuery.outputCount = &versionCount;

		auto countResult = ffxModule.Query(nullptr, &countQuery.header);
		if (countResult != FFX_API_RETURN_OK || versionCount == 0) {
			return false;
		}

		std::vector<uint64_t> versionIds(versionCount, 0);
		std::vector<const char*> versionNames(versionCount, nullptr);

		uint64_t returnedVersionCount = versionCount;
		ffxQueryDescGetVersions versionsQuery{};
		versionsQuery.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
		versionsQuery.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
		versionsQuery.device = a_device;
		versionsQuery.outputCount = &returnedVersionCount;
		versionsQuery.versionIds = versionIds.data();
		versionsQuery.versionNames = versionNames.data();

		auto versionsResult = ffxModule.Query(nullptr, &versionsQuery.header);
		if (versionsResult != FFX_API_RETURN_OK) {
			return false;
		}

		const auto resultCount = std::min<size_t>(versionIds.size(), returnedVersionCount);

		for (size_t i = 0; i < resultCount; ++i) {
			const std::string versionName = versionNames[i] ? versionNames[i] : "";
			if (versionIds[i] == a_requestedVersion ||
				RuntimeProviderIdMatchesVersion(versionIds[i], a_requestedVersion) ||
				RuntimeProviderNameMatchesVersion(versionName, a_requestedVersion)) {
				a_versionId = versionIds[i];
				a_versionName = versionName;
				return true;
			}
		}

		return false;
	}

	std::string FfxCreateResultText(bool a_attempted, ffxReturnCode_t a_result)
	{
		if (!a_attempted)
			return "not attempted";

		return std::format("code {}", static_cast<uint32_t>(a_result));
	}

	enum class RuntimeUpscalerCreateAttempt : uint8_t
	{
		kGenericOverrideWithUpscalerVersion,
		kUpscalerVersionDescriptor,
		kDefaultProvider
	};

	struct RuntimeUpscalerCreateAttemptResult
	{
		RuntimeUpscalerCreateAttempt attempt;
		bool enabled;
		bool attempted = false;
		ffxReturnCode_t result = FFX_API_RETURN_ERROR;
	};

	ffxReturnCode_t TryCreateRuntimeUpscalerContext(
		ffx::Context& a_context,
		RuntimeUpscalerCreateAttempt a_attempt,
		ffx::CreateContextDescUpscale& a_createDesc,
		ffx::CreateBackendDX12Desc& a_backendDesc,
		ffx::CreateContextDescUpscaleVersion& a_versionDesc,
		ffx::CreateContextDescOverrideVersion& a_overrideVersionDesc)
	{
		a_context = nullptr;
		a_createDesc.header.pNext = nullptr;
		a_backendDesc.header.pNext = nullptr;
		a_versionDesc.header.pNext = nullptr;
		a_overrideVersionDesc.header.pNext = nullptr;

		switch (a_attempt) {
		case RuntimeUpscalerCreateAttempt::kGenericOverrideWithUpscalerVersion:
			a_createDesc.header.pNext = &a_backendDesc.header;
			a_backendDesc.header.pNext = &a_versionDesc.header;
			a_versionDesc.header.pNext = &a_overrideVersionDesc.header;
			break;
		case RuntimeUpscalerCreateAttempt::kUpscalerVersionDescriptor:
			a_createDesc.header.pNext = &a_versionDesc.header;
			a_versionDesc.header.pNext = &a_backendDesc.header;
			break;
		case RuntimeUpscalerCreateAttempt::kDefaultProvider:
			a_createDesc.header.pNext = &a_backendDesc.header;
			break;
		}

		ffx::Context createdContext = nullptr;
		const auto result = ffxModule.CreateContext(&createdContext, &a_createDesc.header, nullptr);
		if (result == FFX_API_RETURN_OK) {
			if (createdContext) {
				a_context = createdContext;
				return result;
			}

			return FFX_API_RETURN_ERROR;
		} else if (createdContext && ffxModule.DestroyContext) {
			(void)ffxModule.DestroyContext(&createdContext, nullptr);
		}

		return result;
	}

	D3D11_TEXTURE2D_DESC MakeSharedTextureDesc(const D3D11_TEXTURE2D_DESC& a_sourceDesc, uint32_t a_width, uint32_t a_height, UINT a_bindFlags)
	{
		D3D11_TEXTURE2D_DESC desc = a_sourceDesc;
		desc.Width = a_width;
		desc.Height = a_height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.SampleDesc.Count = 1;
		desc.SampleDesc.Quality = 0;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.CPUAccessFlags = 0;
		desc.BindFlags = a_bindFlags;
		desc.MiscFlags = 0;
		return desc;
	}

	bool SameTextureDesc(const D3D11_TEXTURE2D_DESC& a_left, const D3D11_TEXTURE2D_DESC& a_right)
	{
		return a_left.Width == a_right.Width &&
		       a_left.Height == a_right.Height &&
		       a_left.MipLevels == a_right.MipLevels &&
		       a_left.ArraySize == a_right.ArraySize &&
		       a_left.Format == a_right.Format &&
		       a_left.SampleDesc.Count == a_right.SampleDesc.Count &&
		       a_left.SampleDesc.Quality == a_right.SampleDesc.Quality &&
		       a_left.Usage == a_right.Usage &&
		       a_left.BindFlags == a_right.BindFlags &&
		       a_left.CPUAccessFlags == a_right.CPUAccessFlags &&
		       a_left.MiscFlags == a_right.MiscFlags;
	}

	template <size_t N>
	void DeleteWrappedResourceArray(WrappedResource* (&a_resources)[N])
	{
		for (auto*& resource : a_resources) {
			delete resource;
			resource = nullptr;
		}
	}

	bool DispatchHostFsr3UpscaleProtected(FfxFsr3Context& a_context, FfxFsr3DispatchUpscaleDescription& a_dispatchParameters)
	{
		bool dispatchOk = true;

		__try {
			dispatchOk = ffxFsr3ContextDispatchUpscale(&a_context, &a_dispatchParameters) == FFX_OK;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			dispatchOk = false;
		}

		return dispatchOk;
	}

	// A faulting dispatch inside the AMD-provided DLL is not guaranteed to raise a C++
	// exception the caller's try/catch can observe; wrap it the same way as the host path.
	bool DispatchRuntimeUpscalerProtected(ffx::Context& a_context, ffx::DispatchDescUpscale& a_dispatchParameters, bool& a_faulted)
	{
		a_faulted = false;
		bool dispatchOk = true;

		__try {
			dispatchOk = ffx::Dispatch(a_context, a_dispatchParameters) == ffx::ReturnCode::Ok;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			a_faulted = true;
			dispatchOk = false;
		}

		return dispatchOk;
	}
}

FidelityFX::~FidelityFX()
{
	ReleaseRuntimeUpscalerResourcesForRelatch();
	ResetFSRIdleFence();
}

void FidelityFX::ResetFSRIdleFence()
{
	ReleaseD3D11IdleFence(pendingFSRResourceFreeIdleFence);
	pendingRuntimeTeardownD3D11FenceValue = 0;
	pendingRuntimeTeardownD3D12FenceValue = 0;
}

void FidelityFX::WaitForHostFsrIdle()
{
	// Bounded poll before ffxFsr3ContextDestroy -- the GPU may still have in-flight FSR3
	// work referencing the context being torn down.
	const ULONGLONG waitStart = GetTickCount64();
	while (true) {
		const auto result = BeginOrPollD3D11IdleFence(globals::d3d::context, pendingFSRResourceFreeIdleFence);
		if (result != D3D11IdleFenceResult::Pending)
			break;

		if (GetTickCount64() - waitStart >= 5000)
			break;

		Sleep(1);
	}
}

bool FidelityFX::HasRuntimeUpscalerSupportCheckResult() const
{
	return runtimeUpscalerSupportCheckKnown;
}

bool FidelityFX::IsRuntimeUpscalerSupportConfirmed() const
{
	return runtimeUpscalerSupportCheckKnown && runtimeUpscalerSupportConfirmed;
}

bool FidelityFX::IsRuntimeUpscalerProviderMatchingRequestedVersion() const
{
	if (!runtimeUpscalerSupportCheckKnown || !runtimeUpscalerSupportConfirmed)
		return false;

	if (runtimeUpscalerProviderMatchedVersionName.empty() && runtimeUpscalerProviderMatchedVersionId == 0)
		return false;

	const uint32_t requestedVersion = runtimeUpscalerRequestedVersion ? runtimeUpscalerRequestedVersion : GetPreferredRuntimeUpscalerVersion();
	return RuntimeProviderMatchesVersion(runtimeUpscalerProviderMatchedVersionId, runtimeUpscalerProviderMatchedVersionName, requestedVersion);
}

bool FidelityFX::IsRuntimeUpscalerFailureLatched() const
{
	return runtimeUpscalerFailureLatched;
}

bool FidelityFX::IsRuntimeFsr4FailureLatched() const
{
	return runtimeFsr4FailureLatched;
}

const std::string& FidelityFX::GetHostFsrSdkLabel()
{
	static const std::string label = std::format("Host FSR3 SDK {}", UpscalerVersionToString(Fsr3Version));
	return label;
}

const std::string& FidelityFX::GetRuntimeUpscalerLabel(uint32_t a_version)
{
	static const std::string runtimeFsr3Label = std::format("Runtime FSR3 {} ({})", UpscalerVersionToString(Fsr3Version), RuntimeUpscalerDllNameUtf8);
	static const std::string runtimeFsr4Label = std::format("Runtime FSR4 ({})", RuntimeUpscalerDllNameUtf8);

	if (a_version == Fsr3Version)
		return runtimeFsr3Label;
	if (a_version == FFX_UPSCALER_VERSION)
		return runtimeFsr4Label;

	thread_local std::string fallbackLabel;
	fallbackLabel = std::format("Runtime FSR {} ({})", UpscalerVersionToString(a_version), RuntimeUpscalerDllNameUtf8);
	return fallbackLabel;
}

const std::string& FidelityFX::GetRuntimeUpscalerLastFramePathLabel() const
{
	if (!runtimeUpscalerLastFramePathValid)
		return PendingFsrDispatchLabel();

	switch (runtimeUpscalerLastFramePath) {
	case RuntimeUpscalerFramePath::kHostFsr31:
		return GetHostFsrSdkLabel();
	case RuntimeUpscalerFramePath::kRuntimeFsr31:
		return GetRuntimeUpscalerLabel(Fsr3Version);
	case RuntimeUpscalerFramePath::kRuntimeFsr4:
		return GetRuntimeUpscalerLabel(FFX_UPSCALER_VERSION);
	case RuntimeUpscalerFramePath::kHostFsr31Fallback:
		return HostFsrFallbackLabel();
	case RuntimeUpscalerFramePath::kInactive:
	default:
		return PendingFsrDispatchLabel();
	}
}

const std::string& FidelityFX::GetConfiguredFsrPathLabel() const
{
	if (runtimeUpscalerFailureLatched)
		return HostFsrFallbackLabel();

	if (runtimeFsr4FailureLatched) {
		if (ShouldUseRuntimeUpscalerForFSR())
			return GetRuntimeUpscalerLabel(Fsr3Version);

		return HostFsrFallbackLabel();
	}

	if (runtimeUpscalerSupportCheckKnown && !runtimeUpscalerSupportConfirmed)
		return HostFsrFallbackLabel();

	if (ShouldRequestRuntimeFsr4())
		return GetRuntimeUpscalerLabel(FFX_UPSCALER_VERSION);

	if (ShouldUseRuntimeUpscalerForFSR())
		return GetRuntimeUpscalerLabel(Fsr3Version);

	return GetHostFsrSdkLabel();
}

const std::string& FidelityFX::GetDisplayedFsrPathLabel() const
{
	const uint32_t currentFrame = globals::state ? globals::state->frameCount : 0;
	if (runtimeUpscalerLastFramePathValid && runtimeUpscalerLastFrameIndex == currentFrame)
		return GetRuntimeUpscalerLastFramePathLabel();

	return GetConfiguredFsrPathLabel();
}

std::string FidelityFX::GetRuntimeUpscalerProviderName() const
{
	return RuntimeProviderDisplayName(runtimeUpscalerProviderMatchedVersionId, runtimeUpscalerProviderMatchedVersionName);
}

std::string FidelityFX::GetRuntimeUpscalerRequestedVersionString() const
{
	const uint32_t requestedVersion = runtimeUpscalerRequestedVersion ? runtimeUpscalerRequestedVersion : GetPreferredRuntimeUpscalerVersion();
	return UpscalerVersionToString(requestedVersion);
}

void FidelityFX::ResetRuntimeUpscalerTracking(bool a_invalidateProviderCache)
{
	runtimeUpscalerFailureLatched = false;
	runtimeFsr4FailureLatched = false;
	runtimeFallbackResetDispatchesRemaining = 0;
	runtimeUpscalerLastFramePathValid = false;
	runtimeUpscalerLastFrameIndex = 0;
	runtimeUpscalerLastFramePath = RuntimeUpscalerFramePath::kInactive;

	if (!a_invalidateProviderCache)
		return;

	runtimeUpscalerSupportCheckKnown = false;
	runtimeUpscalerSupportConfirmed = false;
	runtimeUpscalerProviderMatchedVersionId = 0;
	runtimeUpscalerProviderMatchedVersionName.clear();
}

void FidelityFX::LatchRuntimeUpscalerFailure()
{
	if (runtimeUpscalerFailureLatched)
		return;

	runtimeUpscalerFailureLatched = true;
}

void FidelityFX::QuarantineRuntimeUpscalerForSession(const char* a_reason)
{
	LatchRuntimeUpscalerFailure();
	if (runtimeUpscalerQuarantined)
		return;

	runtimeUpscalerQuarantined = true;
	logger::error("[FidelityFX] Quarantining runtime upscaler provider for this session: {}", a_reason ? a_reason : "unknown reason");
}

void FidelityFX::LatchRuntimeFsr4Failure()
{
	if (runtimeFsr4FailureLatched)
		return;

	logger::warn("[FidelityFX] Runtime FSR4 failed this session; falling back to the runtime FSR3 provider");
	runtimeFsr4FailureLatched = true;
}

FidelityFX::RuntimeUpscalerFramePath FidelityFX::GetRuntimeUpscalerProviderFramePath(uint32_t a_requestedVersion) const
{
	if (RuntimeProviderMatchesVersion(runtimeUpscalerProviderMatchedVersionId, runtimeUpscalerProviderMatchedVersionName, FFX_UPSCALER_VERSION))
		return RuntimeUpscalerFramePath::kRuntimeFsr4;

	if (RuntimeProviderMatchesVersion(runtimeUpscalerProviderMatchedVersionId, runtimeUpscalerProviderMatchedVersionName, Fsr3Version))
		return RuntimeUpscalerFramePath::kRuntimeFsr31;

	return a_requestedVersion == FFX_UPSCALER_VERSION ? RuntimeUpscalerFramePath::kRuntimeFsr4 : RuntimeUpscalerFramePath::kRuntimeFsr31;
}

bool FidelityFX::WasRuntimeUpscalerUsedThisFrame() const
{
	if (!runtimeUpscalerLastFramePathValid)
		return false;

	const uint32_t frame = globals::state ? globals::state->frameCount : 0;
	if (runtimeUpscalerLastFrameIndex != frame)
		return false;

	return runtimeUpscalerLastFramePath == RuntimeUpscalerFramePath::kRuntimeFsr31 ||
	       runtimeUpscalerLastFramePath == RuntimeUpscalerFramePath::kRuntimeFsr4;
}

void FidelityFX::RecordRuntimeUpscalerFramePath(RuntimeUpscalerFramePath a_path)
{
	const uint32_t frame = globals::state ? globals::state->frameCount : 0;
	if (!runtimeUpscalerLastFramePathValid || runtimeUpscalerLastFrameIndex != frame) {
		runtimeUpscalerLastFramePathValid = true;
		runtimeUpscalerLastFrameIndex = frame;
		runtimeUpscalerLastFramePath = a_path;
		return;
	}

	if (static_cast<uint8_t>(a_path) > static_cast<uint8_t>(runtimeUpscalerLastFramePath))
		runtimeUpscalerLastFramePath = a_path;
}

bool FidelityFX::IsAmdAdapterDetected() const
{
	DXGI_ADAPTER_DESC adapterDesc{};
	if (TryGetCurrentAdapterDesc(adapterDesc))
		return adapterDesc.VendorId == kAmdVendorId;

	return false;
}

bool FidelityFX::IsNvidiaAdapterDetected() const
{
	DXGI_ADAPTER_DESC adapterDesc{};
	if (TryGetCurrentAdapterDesc(adapterDesc))
		return adapterDesc.VendorId == kNvidiaVendorId;

	return false;
}

bool FidelityFX::IsRuntimeUpscalerPresent() const
{
	if (!featureRuntimeUpscaler || !runtimeUpscalerModule || !module)
		return false;
	if (!ffxModule.CreateContext || !ffxModule.DestroyContext || !ffxModule.Dispatch || !ffxModule.Query)
		return false;

	return true;
}

FidelityFX::Fsr4AdapterSupport FidelityFX::GetFsr4AdapterSupport(const DXGI_ADAPTER_DESC& a_adapterDesc)
{
	return ClassifyFsr4AdapterSupport(a_adapterDesc);
}

FidelityFX::Fsr4AdapterSupport FidelityFX::GetFsr4AdapterSupport() const
{
	DXGI_ADAPTER_DESC adapterDesc{};
	if (!TryGetCurrentAdapterDesc(adapterDesc))
		return Fsr4AdapterSupport::Unsupported;

	return GetFsr4AdapterSupport(adapterDesc);
}

bool FidelityFX::IsRuntimeFsr4AutoEligible() const
{
	return GetFsr4AdapterSupport() != Fsr4AdapterSupport::Unsupported;
}

bool FidelityFX::IsRuntimeFsr4Available() const
{
	if (!IsRuntimeUpscalerPresent())
		return false;

	return IsRuntimeFsr4AutoEligible();
}

bool FidelityFX::ShouldUseRuntimeUpscalerForFSR() const
{
	return IsRuntimeUpscalerPresent() && IsAmdAdapterDetected();
}

bool FidelityFX::CanUseRuntimeUpscalerPath()
{
	if (runtimeUpscalerQuarantined || runtimeUpscalerFailureLatched)
		return false;
	return true;
}

bool FidelityFX::ShouldRequestRuntimeFsr4() const
{
	// fsr4RuntimeEnable is restart-gated (kRestartFields): read the boot-latched
	// value, not the live setting, so a mid-session toggle can't request FSR4
	// before the restart the UI already told the user this setting needs.
	auto& upscaling = globals::features::upscaling;
	return upscaling.bootSnapshot.Boot(&Upscaling::Settings::fsr4RuntimeEnable) &&
	       !runtimeFsr4FailureLatched &&
	       IsRuntimeFsr4Available();
}

uint32_t FidelityFX::GetPreferredRuntimeUpscalerVersion() const
{
	return ShouldRequestRuntimeFsr4() ? FFX_UPSCALER_VERSION : Fsr3Version;
}

bool FidelityFX::EnsureRuntimeUpscalerInterop()
{
	auto& swapChain = globals::features::upscaling.dx12SwapChain;

	if (!globals::d3d::device || !globals::d3d::context)
		return false;

	try {
		if (!swapChain.d3d11Device)
			swapChain.SetD3D11Device(globals::d3d::device);
		if (!swapChain.d3d11Context)
			swapChain.SetD3D11DeviceContext(globals::d3d::context);

		if (!swapChain.d3d12Device) {
			winrt::com_ptr<IDXGIDevice> dxgiDevice;
			DX::ThrowIfFailed(globals::d3d::device->QueryInterface(IID_PPV_ARGS(dxgiDevice.put())));

			winrt::com_ptr<IDXGIAdapter> adapter;
			DX::ThrowIfFailed(dxgiDevice->GetAdapter(adapter.put()));
			swapChain.CreateD3D12Device(adapter.get());
		}

		if (!runtimeD3D12Fence || !runtimeD3D11Fence) {
			winrt::handle sharedFenceHandle;
			DX::ThrowIfFailed(swapChain.d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&runtimeD3D12Fence)));
			DX::ThrowIfFailed(swapChain.d3d12Device->CreateSharedHandle(runtimeD3D12Fence.get(), nullptr, GENERIC_ALL, nullptr, sharedFenceHandle.put()));
			DX::ThrowIfFailed(swapChain.d3d11Device->OpenSharedFence(sharedFenceHandle.get(), IID_PPV_ARGS(&runtimeD3D11Fence)));
			runtimeFenceValue = 1;
			for (auto& commandContext : runtimeCommandContexts)
				commandContext.fenceValue = 0;
			runtimeCommandContextCursor = 0;
		}

		if (!EnsureRuntimeCommandContexts())
			return false;
	} catch (const std::exception& e) {
		logger::error("[FidelityFX] Runtime upscaler interop setup failed: {}", e.what());
		return false;
	} catch (...) {
		logger::error("[FidelityFX] Runtime upscaler interop setup failed with an unknown exception");
		return false;
	}

	return swapChain.d3d11Device.get() &&
	       swapChain.d3d11Context.get() &&
	       swapChain.d3d12Device.get() &&
	       swapChain.commandQueue.get() &&
	       runtimeD3D11Fence.get() &&
	       runtimeD3D12Fence.get();
}

bool FidelityFX::EnsureRuntimeCommandContexts()
{
	auto& swapChain = globals::features::upscaling.dx12SwapChain;
	if (!swapChain.d3d12Device)
		return false;

	try {
		for (auto& commandContext : runtimeCommandContexts) {
			if (commandContext.commandAllocator && commandContext.commandList)
				continue;

			commandContext.commandList = nullptr;
			commandContext.commandAllocator = nullptr;
			commandContext.fenceValue = 0;

			DX::ThrowIfFailed(swapChain.d3d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandContext.commandAllocator)));
			DX::ThrowIfFailed(swapChain.d3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandContext.commandAllocator.get(), nullptr, IID_PPV_ARGS(&commandContext.commandList)));
			DX::ThrowIfFailed(commandContext.commandList->Close());
		}
	} catch (const std::exception& e) {
		logger::error("[FidelityFX] Runtime upscaler command context creation failed: {}", e.what());
		return false;
	} catch (...) {
		logger::error("[FidelityFX] Runtime upscaler command context creation failed with an unknown exception");
		return false;
	}

	return true;
}

FidelityFX::RuntimeCommandContext* FidelityFX::AcquireRuntimeCommandContext()
{
	if (!runtimeD3D12Fence || !EnsureRuntimeCommandContexts())
		return nullptr;

	const uint64_t completedValue = runtimeD3D12Fence->GetCompletedValue();
	const uint32_t commandContextCount = static_cast<uint32_t>(runtimeCommandContexts.size());
	for (uint32_t i = 0; i < commandContextCount; ++i) {
		const uint32_t index = (runtimeCommandContextCursor + i) % commandContextCount;
		auto& commandContext = runtimeCommandContexts[index];
		if (!commandContext.commandAllocator || !commandContext.commandList)
			continue;
		if (commandContext.fenceValue != 0 && completedValue < commandContext.fenceValue)
			continue;

		commandContext.fenceValue = 0;
		runtimeCommandContextCursor = (index + 1) % commandContextCount;
		return &commandContext;
	}

	uint32_t waitIndex = commandContextCount;
	uint64_t waitValue = std::numeric_limits<uint64_t>::max();
	for (uint32_t i = 0; i < commandContextCount; ++i) {
		auto& commandContext = runtimeCommandContexts[i];
		if (!commandContext.commandAllocator || !commandContext.commandList || commandContext.fenceValue == 0)
			continue;
		if (commandContext.fenceValue < waitValue) {
			waitValue = commandContext.fenceValue;
			waitIndex = i;
		}
	}

	if (waitIndex == commandContextCount)
		return nullptr;

	if (!WaitForRuntimeD3D12Fence(waitValue)) {
		return nullptr;
	}

	auto& commandContext = runtimeCommandContexts[waitIndex];
	commandContext.fenceValue = 0;
	runtimeCommandContextCursor = (waitIndex + 1) % commandContextCount;
	return &commandContext;
}

void FidelityFX::ResetRuntimeCommandContexts()
{
	for (auto& commandContext : runtimeCommandContexts) {
		commandContext.commandList = nullptr;
		commandContext.commandAllocator = nullptr;
		commandContext.fenceValue = 0;
	}
	runtimeCommandContextCursor = 0;
}

bool FidelityFX::WaitForRuntimeD3D12Fence(uint64_t a_value)
{
	if (!runtimeD3D12Fence || a_value == 0)
		return true;
	if (runtimeD3D12Fence->GetCompletedValue() >= a_value)
		return true;

	winrt::handle fenceEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr));
	if (!fenceEvent)
		return false;
	if (FAILED(runtimeD3D12Fence->SetEventOnCompletion(a_value, fenceEvent.get())))
		return false;

	return WaitForSingleObject(fenceEvent.get(), 5000) == WAIT_OBJECT_0;
}

void FidelityFX::WaitForRuntimeUpscalerIdle()
{
	auto& swapChain = globals::features::upscaling.dx12SwapChain;
	if (!runtimeD3D12Fence && !runtimeD3D11Fence)
		return;

	try {
		if (swapChain.d3d11Context && runtimeD3D11Fence && runtimeD3D12Fence) {
			const uint64_t d3d11FenceValue = runtimeFenceValue++;
			DX::ThrowIfFailed(swapChain.d3d11Context->Signal(runtimeD3D11Fence.get(), d3d11FenceValue));
			swapChain.d3d11Context->Flush();
			(void)WaitForRuntimeD3D12Fence(d3d11FenceValue);
		} else if (globals::d3d::context) {
			globals::d3d::context->Flush();
		}

		if (swapChain.commandQueue && runtimeD3D12Fence) {
			const uint64_t d3d12FenceValue = runtimeFenceValue++;
			DX::ThrowIfFailed(swapChain.commandQueue->Signal(runtimeD3D12Fence.get(), d3d12FenceValue));
			(void)WaitForRuntimeD3D12Fence(d3d12FenceValue);
		}

		if (runtimeD3D12Fence) {
			const uint64_t completedValue = runtimeD3D12Fence->GetCompletedValue();
			for (auto& commandContext : runtimeCommandContexts) {
				if (commandContext.fenceValue != 0 && completedValue >= commandContext.fenceValue)
					commandContext.fenceValue = 0;
			}
		}
	} catch (const std::exception& e) {
		logger::debug("[FidelityFX] Runtime upscaler idle wait failed: {}", e.what());
	} catch (...) {
		logger::debug("[FidelityFX] Runtime upscaler idle wait failed with an unknown exception");
	}
}

void FidelityFX::DestroyRuntimeUpscalerContexts(bool a_waitForIdle)
{
	if (a_waitForIdle)
		WaitForRuntimeUpscalerIdle();

	for (uint32_t i = 0; i < std::size(runtimeUpscalerContexts); ++i) {
		if (runtimeUpscalerContexts[i])
			(void)ffx::DestroyContext(runtimeUpscalerContexts[i]);
		runtimeUpscalerContexts[i] = nullptr;
	}

	runtimeUpscalerContextCount = 0;
	runtimeUpscalerMaxRenderWidth = 0;
	runtimeUpscalerMaxRenderHeight = 0;
	runtimeUpscalerMaxDisplayWidth = 0;
	runtimeUpscalerMaxDisplayHeight = 0;
	runtimeUpscalerRequestedVersion = 0;
}

void FidelityFX::DestroyRuntimeUpscalerResources(bool a_waitForIdle)
{
	if (a_waitForIdle)
		WaitForRuntimeUpscalerIdle();

	DeleteWrappedResourceArray(runtimeColorShared);
	DeleteWrappedResourceArray(runtimeDepthShared);
	DeleteWrappedResourceArray(runtimeMotionShared);
	DeleteWrappedResourceArray(runtimeReactiveShared);
	DeleteWrappedResourceArray(runtimeTransparencyShared);
	DeleteWrappedResourceArray(runtimeOutputShared);

	runtimeColorSharedDesc = {};
	runtimeDepthSharedDesc = {};
	runtimeMotionSharedDesc = {};
	runtimeReactiveSharedDesc = {};
	runtimeTransparencySharedDesc = {};
	runtimeOutputSharedDesc = {};
}

bool FidelityFX::HasRuntimeUpscalerResources() const
{
	bool hasRuntimeResources =
		runtimeUpscalerContextCount != 0 ||
		pendingRuntimeTeardownD3D11FenceValue != 0 ||
		pendingRuntimeTeardownD3D12FenceValue != 0;
	for (auto* resource : runtimeColorShared)
		hasRuntimeResources = hasRuntimeResources || resource != nullptr;
	for (auto* resource : runtimeDepthShared)
		hasRuntimeResources = hasRuntimeResources || resource != nullptr;
	for (auto* resource : runtimeMotionShared)
		hasRuntimeResources = hasRuntimeResources || resource != nullptr;
	for (auto* resource : runtimeReactiveShared)
		hasRuntimeResources = hasRuntimeResources || resource != nullptr;
	for (auto* resource : runtimeTransparencyShared)
		hasRuntimeResources = hasRuntimeResources || resource != nullptr;
	for (auto* resource : runtimeOutputShared)
		hasRuntimeResources = hasRuntimeResources || resource != nullptr;
	return hasRuntimeResources;
}

bool FidelityFX::PollRuntimeUpscalerTeardownIdle()
{
	auto& swapChain = globals::features::upscaling.dx12SwapChain;
	if (!runtimeD3D12Fence && !runtimeD3D11Fence) {
		pendingRuntimeTeardownD3D11FenceValue = 0;
		pendingRuntimeTeardownD3D12FenceValue = 0;
		return true;
	}

	try {
		if (pendingRuntimeTeardownD3D11FenceValue != 0 &&
			(!swapChain.d3d11Context || !runtimeD3D11Fence || !runtimeD3D12Fence)) {
			pendingRuntimeTeardownD3D11FenceValue = 0;
		}
		if (pendingRuntimeTeardownD3D12FenceValue != 0 && (!swapChain.commandQueue || !runtimeD3D12Fence)) {
			pendingRuntimeTeardownD3D12FenceValue = 0;
		}

		if (swapChain.d3d11Context && runtimeD3D11Fence && runtimeD3D12Fence) {
			if (pendingRuntimeTeardownD3D11FenceValue == 0) {
				pendingRuntimeTeardownD3D11FenceValue = runtimeFenceValue++;
				DX::ThrowIfFailed(swapChain.d3d11Context->Signal(runtimeD3D11Fence.get(), pendingRuntimeTeardownD3D11FenceValue));
				swapChain.d3d11Context->Flush();
			}

			if (runtimeD3D12Fence->GetCompletedValue() < pendingRuntimeTeardownD3D11FenceValue)
				return false;

			pendingRuntimeTeardownD3D11FenceValue = 0;
		} else if (globals::d3d::context) {
			globals::d3d::context->Flush();
		}

		if (swapChain.commandQueue && runtimeD3D12Fence) {
			if (pendingRuntimeTeardownD3D12FenceValue == 0) {
				pendingRuntimeTeardownD3D12FenceValue = runtimeFenceValue++;
				DX::ThrowIfFailed(swapChain.commandQueue->Signal(runtimeD3D12Fence.get(), pendingRuntimeTeardownD3D12FenceValue));
			}

			if (runtimeD3D12Fence->GetCompletedValue() < pendingRuntimeTeardownD3D12FenceValue)
				return false;

			pendingRuntimeTeardownD3D12FenceValue = 0;
		}
	} catch (const std::exception& e) {
		static bool loggedTeardownPollFailure = false;
		if (!loggedTeardownPollFailure) {
			logger::debug("[FidelityFX] Runtime upscaler teardown poll failed: {}", e.what());
			loggedTeardownPollFailure = true;
		}
		pendingRuntimeTeardownD3D11FenceValue = 0;
		pendingRuntimeTeardownD3D12FenceValue = 0;
		return true;
	} catch (...) {
		pendingRuntimeTeardownD3D11FenceValue = 0;
		pendingRuntimeTeardownD3D12FenceValue = 0;
		return true;
	}

	return true;
}

bool FidelityFX::PollRuntimeUpscalerTeardownReady()
{
	if (!HasRuntimeUpscalerResources()) {
		pendingRuntimeTeardownD3D11FenceValue = 0;
		pendingRuntimeTeardownD3D12FenceValue = 0;
		return true;
	}

	return PollRuntimeUpscalerTeardownIdle();
}

void FidelityFX::ResetRuntimeUpscalerResources(bool a_invalidateProviderCache)
{
	WaitForRuntimeUpscalerIdle();
	DestroyRuntimeUpscalerContexts(false);
	DestroyRuntimeUpscalerResources(false);
	ResetRuntimeUpscalerTracking(a_invalidateProviderCache);
}

void FidelityFX::ReleaseRuntimeUpscalerResourcesForRelatch(bool a_waitForIdle)
{
	if (a_waitForIdle)
		WaitForRuntimeUpscalerIdle();

	DestroyRuntimeUpscalerContexts(false);
	DestroyRuntimeUpscalerResources(false);
	ResetRuntimeCommandContexts();
	pendingRuntimeTeardownD3D11FenceValue = 0;
	pendingRuntimeTeardownD3D12FenceValue = 0;
}

bool FidelityFX::EnsureRuntimeUpscalerContexts(uint32_t a_fullRenderWidth, uint32_t a_fullRenderHeight, uint32_t a_fullDisplayWidth, uint32_t a_fullDisplayHeight, uint32_t a_contextCount, uint32_t a_requestedVersion)
{
	auto recordRuntimeProviderResult = [&](bool a_supported) {
		runtimeUpscalerSupportCheckKnown = true;
		runtimeUpscalerSupportConfirmed = a_supported;
		runtimeUpscalerProviderMatchedVersionId = 0;
		runtimeUpscalerProviderMatchedVersionName.clear();
	};

	if (!a_fullRenderWidth || !a_fullRenderHeight || !a_fullDisplayWidth || !a_fullDisplayHeight) {
		recordRuntimeProviderResult(false);
		return false;
	}
	if (a_contextCount == 0 || a_contextCount > std::size(runtimeUpscalerContexts)) {
		recordRuntimeProviderResult(false);
		return false;
	}
	if (!EnsureRuntimeUpscalerInterop()) {
		recordRuntimeProviderResult(false);
		return false;
	}
	if (!ffxModule.CreateContext || !ffxModule.DestroyContext || !ffxModule.Query) {
		recordRuntimeProviderResult(false);
		return false;
	}

	bool allContextsValid = true;
	for (uint32_t i = 0; i < a_contextCount; ++i) {
		if (!runtimeUpscalerContexts[i]) {
			allContextsValid = false;
			break;
		}
	}

	const bool needsRecreate =
		!allContextsValid ||
		runtimeUpscalerContextCount != a_contextCount ||
		runtimeUpscalerMaxRenderWidth != a_fullRenderWidth ||
		runtimeUpscalerMaxRenderHeight != a_fullRenderHeight ||
		runtimeUpscalerMaxDisplayWidth != a_fullDisplayWidth ||
		runtimeUpscalerMaxDisplayHeight != a_fullDisplayHeight ||
		runtimeUpscalerRequestedVersion != a_requestedVersion;

	if (!needsRecreate && runtimeUpscalerContextCount == a_contextCount)
		return true;

	DestroyRuntimeUpscalerContexts();

	auto& swapChain = globals::features::upscaling.dx12SwapChain;

	ffx::CreateBackendDX12Desc backendDesc{};
	backendDesc.device = swapChain.d3d12Device.get();

	uint64_t runtimeVersionId = 0;
	std::string runtimeVersionName;
	const bool hasRuntimeVersionOverride = QueryRuntimeUpscalerVersionId(swapChain.d3d12Device.get(), a_requestedVersion, runtimeVersionId, runtimeVersionName);

	for (uint32_t i = 0; i < a_contextCount; ++i) {
		ffx::CreateContextDescUpscale createDesc{};
		createDesc.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
		createDesc.maxRenderSize = { a_fullRenderWidth, a_fullRenderHeight };
		createDesc.maxUpscaleSize = { a_fullDisplayWidth, a_fullDisplayHeight };

		ffx::CreateContextDescUpscaleVersion versionDesc{};
		versionDesc.version = a_requestedVersion;

		ffx::CreateContextDescOverrideVersion overrideVersionDesc{};
		overrideVersionDesc.versionId = runtimeVersionId;

		std::array<RuntimeUpscalerCreateAttemptResult, 3> attempts{ {
			{ RuntimeUpscalerCreateAttempt::kGenericOverrideWithUpscalerVersion, hasRuntimeVersionOverride },
			{ RuntimeUpscalerCreateAttempt::kUpscalerVersionDescriptor, true },
			{ RuntimeUpscalerCreateAttempt::kDefaultProvider, a_requestedVersion == FFX_UPSCALER_VERSION },
		} };

		bool contextCreated = false;
		for (auto& attempt : attempts) {
			if (!attempt.enabled)
				continue;

			attempt.attempted = true;
			attempt.result = TryCreateRuntimeUpscalerContext(
				runtimeUpscalerContexts[i],
				attempt.attempt,
				createDesc,
				backendDesc,
				versionDesc,
				overrideVersionDesc);

			if (attempt.result != FFX_API_RETURN_OK)
				continue;

			contextCreated = true;
			break;
		}

		if (!contextCreated) {
			const auto getAttemptResult = [&](RuntimeUpscalerCreateAttempt a_attempt) {
				const auto iter = std::find_if(attempts.begin(), attempts.end(), [&](const RuntimeUpscalerCreateAttemptResult& a_result) {
					return a_result.attempt == a_attempt;
				});

				return iter != attempts.end() ? FfxCreateResultText(iter->attempted, iter->result) : std::string("not attempted");
			};

			logger::error("[FidelityFX] Failed to create runtime upscaler context {} for FSR version {}. Override + upscaler descriptor: {}, upscaler version descriptor: {}, default provider: {} (Render: {}x{}, Display: {}x{}).",
				i,
				UpscalerVersionToString(a_requestedVersion),
				getAttemptResult(RuntimeUpscalerCreateAttempt::kGenericOverrideWithUpscalerVersion),
				getAttemptResult(RuntimeUpscalerCreateAttempt::kUpscalerVersionDescriptor),
				getAttemptResult(RuntimeUpscalerCreateAttempt::kDefaultProvider),
				a_fullRenderWidth,
				a_fullRenderHeight,
				a_fullDisplayWidth,
				a_fullDisplayHeight);
			DestroyRuntimeUpscalerContexts();
			recordRuntimeProviderResult(false);
			return false;
		}
	}

	runtimeUpscalerContextCount = a_contextCount;
	runtimeUpscalerMaxRenderWidth = a_fullRenderWidth;
	runtimeUpscalerMaxRenderHeight = a_fullRenderHeight;
	runtimeUpscalerMaxDisplayWidth = a_fullDisplayWidth;
	runtimeUpscalerMaxDisplayHeight = a_fullDisplayHeight;
	runtimeUpscalerRequestedVersion = a_requestedVersion;

	uint64_t selectedProviderVersionId = 0;
	std::string selectedProviderVersionName;
	for (uint32_t i = 0; i < a_contextCount; ++i) {
		ffxQueryGetProviderVersion providerQuery{};
		providerQuery.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
		providerQuery.header.pNext = nullptr;
		providerQuery.versionId = 0;
		providerQuery.versionName = nullptr;

		const auto queryResult = ffxModule.Query(&runtimeUpscalerContexts[i], &providerQuery.header);
		const std::string providerVersionName = providerQuery.versionName ? providerQuery.versionName : "";
		const bool providerIdentityKnown = queryResult == FFX_API_RETURN_OK &&
		                                   (providerQuery.versionId != 0 || !providerVersionName.empty());
		const bool providerVersionMatches = providerIdentityKnown &&
		                                    RuntimeProviderMatchesVersion(providerQuery.versionId, providerVersionName, a_requestedVersion);
		const bool providerMatchesOtherContexts = i == 0 ||
		                                          (providerQuery.versionId == selectedProviderVersionId && providerVersionName == selectedProviderVersionName);
		if (!providerIdentityKnown || !providerVersionMatches || !providerMatchesOtherContexts) {
			auto providerName = RuntimeProviderDisplayName(providerQuery.versionId, providerVersionName);
			if (providerName.empty())
				providerName = "unknown";

			const char* failureReason = "differs from the other runtime contexts";
			if (!providerIdentityKnown)
				failureReason = "identity is unknown";
			else if (!providerVersionMatches)
				failureReason = "does not match the requested version";
			logger::error("[FidelityFX] Runtime upscaler context {} provider '{}' {} for FSR version {} (query code {}, Render: {}x{}, Display: {}x{}).",
				i,
				providerName,
				failureReason,
				UpscalerVersionToString(a_requestedVersion),
				static_cast<uint32_t>(queryResult),
				a_fullRenderWidth,
				a_fullRenderHeight,
				a_fullDisplayWidth,
				a_fullDisplayHeight);
			DestroyRuntimeUpscalerContexts();
			recordRuntimeProviderResult(false);
			return false;
		}

		if (i == 0) {
			selectedProviderVersionId = providerQuery.versionId;
			selectedProviderVersionName = providerVersionName;
		}
	}

	recordRuntimeProviderResult(true);
	runtimeUpscalerProviderMatchedVersionId = selectedProviderVersionId;
	runtimeUpscalerProviderMatchedVersionName = selectedProviderVersionName;

	return true;
}

bool FidelityFX::EnsureRuntimeUpscalerSharedResources(uint32_t a_contextCount, uint32_t a_fullRenderWidth, uint32_t a_fullRenderHeight, uint32_t a_fullDisplayWidth, uint32_t a_fullDisplayHeight,
	const D3D11_TEXTURE2D_DESC& a_colorDesc,
	const D3D11_TEXTURE2D_DESC& a_depthDesc,
	const D3D11_TEXTURE2D_DESC& a_motionDesc,
	const D3D11_TEXTURE2D_DESC& a_reactiveDesc,
	const D3D11_TEXTURE2D_DESC& a_transparencyDesc,
	const D3D11_TEXTURE2D_DESC& a_outputDesc)
{
	if (!EnsureRuntimeUpscalerInterop())
		return false;
	if (a_contextCount == 0 || a_contextCount > std::size(runtimeColorShared))
		return false;

	const D3D11_TEXTURE2D_DESC desiredColorDesc = MakeSharedTextureDesc(a_colorDesc, a_fullRenderWidth, a_fullRenderHeight, 0);
	const D3D11_TEXTURE2D_DESC desiredDepthDesc = MakeSharedTextureDesc(a_depthDesc, a_fullRenderWidth, a_fullRenderHeight, 0);
	const D3D11_TEXTURE2D_DESC desiredMotionDesc = MakeSharedTextureDesc(a_motionDesc, a_fullRenderWidth, a_fullRenderHeight, 0);
	const D3D11_TEXTURE2D_DESC desiredReactiveDesc = MakeSharedTextureDesc(a_reactiveDesc, a_fullRenderWidth, a_fullRenderHeight, 0);
	const D3D11_TEXTURE2D_DESC desiredTransparencyDesc = MakeSharedTextureDesc(a_transparencyDesc, a_fullRenderWidth, a_fullRenderHeight, 0);
	const D3D11_TEXTURE2D_DESC desiredOutputDesc = MakeSharedTextureDesc(a_outputDesc, a_fullDisplayWidth, a_fullDisplayHeight, D3D11_BIND_UNORDERED_ACCESS);

	bool missingRequiredResource = false;
	for (uint32_t i = 0; i < a_contextCount; ++i) {
		if (!runtimeColorShared[i] ||
			!runtimeDepthShared[i] ||
			!runtimeMotionShared[i] ||
			!runtimeReactiveShared[i] ||
			!runtimeTransparencyShared[i] ||
			!runtimeOutputShared[i] ||
			!runtimeColorShared[i]->resource11 ||
			!runtimeDepthShared[i]->resource11 ||
			!runtimeMotionShared[i]->resource11 ||
			!runtimeReactiveShared[i]->resource11 ||
			!runtimeTransparencyShared[i]->resource11 ||
			!runtimeOutputShared[i]->resource11 ||
			!runtimeColorShared[i]->resource.get() ||
			!runtimeDepthShared[i]->resource.get() ||
			!runtimeMotionShared[i]->resource.get() ||
			!runtimeReactiveShared[i]->resource.get() ||
			!runtimeTransparencyShared[i]->resource.get() ||
			!runtimeOutputShared[i]->resource.get()) {
			missingRequiredResource = true;
			break;
		}
	}

	const bool needsRecreate =
		missingRequiredResource ||
		!SameTextureDesc(runtimeColorSharedDesc, desiredColorDesc) ||
		!SameTextureDesc(runtimeDepthSharedDesc, desiredDepthDesc) ||
		!SameTextureDesc(runtimeMotionSharedDesc, desiredMotionDesc) ||
		!SameTextureDesc(runtimeReactiveSharedDesc, desiredReactiveDesc) ||
		!SameTextureDesc(runtimeTransparencySharedDesc, desiredTransparencyDesc) ||
		!SameTextureDesc(runtimeOutputSharedDesc, desiredOutputDesc);

	if (!needsRecreate) {
		for (uint32_t i = a_contextCount; i < std::size(runtimeColorShared); ++i) {
			delete runtimeColorShared[i];
			runtimeColorShared[i] = nullptr;
			delete runtimeDepthShared[i];
			runtimeDepthShared[i] = nullptr;
			delete runtimeMotionShared[i];
			runtimeMotionShared[i] = nullptr;
			delete runtimeReactiveShared[i];
			runtimeReactiveShared[i] = nullptr;
			delete runtimeTransparencyShared[i];
			runtimeTransparencyShared[i] = nullptr;
			delete runtimeOutputShared[i];
			runtimeOutputShared[i] = nullptr;
		}
		return true;
	}

	DestroyRuntimeUpscalerResources();

	auto& swapChain = globals::features::upscaling.dx12SwapChain;

	try {
		for (uint32_t i = 0; i < a_contextCount; ++i) {
			runtimeColorShared[i] = new WrappedResource(desiredColorDesc, swapChain.d3d11Device.get(), swapChain.d3d12Device.get(), std::format("RuntimeUpscaler::ColorShared{}", i));
			runtimeDepthShared[i] = new WrappedResource(desiredDepthDesc, swapChain.d3d11Device.get(), swapChain.d3d12Device.get(), std::format("RuntimeUpscaler::DepthShared{}", i));
			runtimeMotionShared[i] = new WrappedResource(desiredMotionDesc, swapChain.d3d11Device.get(), swapChain.d3d12Device.get(), std::format("RuntimeUpscaler::MotionShared{}", i));
			runtimeReactiveShared[i] = new WrappedResource(desiredReactiveDesc, swapChain.d3d11Device.get(), swapChain.d3d12Device.get(), std::format("RuntimeUpscaler::ReactiveShared{}", i));
			runtimeTransparencyShared[i] = new WrappedResource(desiredTransparencyDesc, swapChain.d3d11Device.get(), swapChain.d3d12Device.get(), std::format("RuntimeUpscaler::TransparencyShared{}", i));
			runtimeOutputShared[i] = new WrappedResource(desiredOutputDesc, swapChain.d3d11Device.get(), swapChain.d3d12Device.get(), std::format("RuntimeUpscaler::OutputShared{}", i));
		}
	} catch (const std::exception& e) {
		logger::error("[FidelityFX] Runtime upscaler shared resource creation failed: {}", e.what());
		DestroyRuntimeUpscalerResources();
		return false;
	} catch (...) {
		logger::error("[FidelityFX] Runtime upscaler shared resource creation failed with an unknown exception");
		DestroyRuntimeUpscalerResources();
		return false;
	}

	runtimeColorSharedDesc = desiredColorDesc;
	runtimeDepthSharedDesc = desiredDepthDesc;
	runtimeMotionSharedDesc = desiredMotionDesc;
	runtimeReactiveSharedDesc = desiredReactiveDesc;
	runtimeTransparencySharedDesc = desiredTransparencyDesc;
	runtimeOutputSharedDesc = desiredOutputDesc;

	return true;
}

bool FidelityFX::DispatchRuntimeUpscalerSingle(uint32_t a_contextIndex, ID3D11Resource* a_color, ID3D11Resource* a_depth, ID3D11Resource* a_motionVectors,
	ID3D11Resource* a_reactiveMask, ID3D11Resource* a_transparencyCompositionMask, ID3D11Resource* a_output,
	uint32_t a_renderWidth, uint32_t a_renderHeight, uint32_t a_displayWidth, uint32_t a_displayHeight,
	float a_motionVectorScaleX, float a_motionVectorScaleY, float a_sharpness)
{
	if (a_contextIndex >= runtimeUpscalerContextCount || !runtimeUpscalerContexts[a_contextIndex])
		return false;
	if (!a_color || !a_depth || !a_motionVectors || !a_reactiveMask || !a_transparencyCompositionMask || !a_output)
		return false;
	if (!a_renderWidth || !a_renderHeight || !a_displayWidth || !a_displayHeight)
		return false;

	D3D11_TEXTURE2D_DESC colorDesc{};
	D3D11_TEXTURE2D_DESC depthDesc{};
	D3D11_TEXTURE2D_DESC motionDesc{};
	D3D11_TEXTURE2D_DESC reactiveDesc{};
	D3D11_TEXTURE2D_DESC transparencyDesc{};
	D3D11_TEXTURE2D_DESC outputDesc{};
	if (!TryGetTexture2DDesc(a_color, colorDesc) ||
		!TryGetTexture2DDesc(a_depth, depthDesc) ||
		!TryGetTexture2DDesc(a_motionVectors, motionDesc) ||
		!TryGetTexture2DDesc(a_reactiveMask, reactiveDesc) ||
		!TryGetTexture2DDesc(a_transparencyCompositionMask, transparencyDesc) ||
		!TryGetTexture2DDesc(a_output, outputDesc)) {
		return false;
	}

	if (!EnsureRuntimeUpscalerSharedResources(
			runtimeUpscalerContextCount,
			runtimeUpscalerMaxRenderWidth,
			runtimeUpscalerMaxRenderHeight,
			runtimeUpscalerMaxDisplayWidth,
			runtimeUpscalerMaxDisplayHeight,
			colorDesc,
			depthDesc,
			motionDesc,
			reactiveDesc,
			transparencyDesc,
			outputDesc)) {
		return false;
	}

	auto& swapChain = globals::features::upscaling.dx12SwapChain;
	auto& upscaling = globals::features::upscaling;
	auto state = globals::state;

	if (!swapChain.d3d11Context || !swapChain.commandQueue || !runtimeD3D11Fence || !runtimeD3D12Fence)
		return false;
	if (!state)
		return false;

	auto isValidShared = [](WrappedResource* a_resource) {
		return a_resource && a_resource->resource11 && a_resource->resource.get();
	};
	if (!isValidShared(runtimeColorShared[a_contextIndex]) ||
		!isValidShared(runtimeDepthShared[a_contextIndex]) ||
		!isValidShared(runtimeMotionShared[a_contextIndex]) ||
		!isValidShared(runtimeReactiveShared[a_contextIndex]) ||
		!isValidShared(runtimeTransparencyShared[a_contextIndex]) ||
		!isValidShared(runtimeOutputShared[a_contextIndex])) {
		return false;
	}

	auto* commandContext = AcquireRuntimeCommandContext();
	if (!commandContext)
		return false;

	auto* commandAllocator = commandContext->commandAllocator.get();
	auto* commandList = commandContext->commandList.get();
	if (!commandAllocator || !commandList)
		return false;

	const std::string dispatchPassName = globals::game::isVR ? std::format("Upscaling::RuntimeUpscalerDispatch Eye {}", a_contextIndex) : "Upscaling::RuntimeUpscalerDispatch";
	CS_GPU_PASS_DYNAMIC(dispatchPassName);

	bool dispatchOk = false;
	bool commandListSubmitted = false;
	bool commandFenceTracked = false;
	try {
		auto copyIntoShared = [&](ID3D11Resource* a_source, WrappedResource* a_destination, uint32_t a_width, uint32_t a_height, uint32_t a_maxWidth, uint32_t a_maxHeight) {
			if (!a_source || !a_destination || !a_destination->resource11)
				return false;

			const uint32_t copyWidth = std::min(a_width, a_maxWidth);
			const uint32_t copyHeight = std::min(a_height, a_maxHeight);
			if (!copyWidth || !copyHeight)
				return false;

			D3D11_BOX sourceBox{};
			sourceBox.left = 0;
			sourceBox.top = 0;
			sourceBox.front = 0;
			sourceBox.right = copyWidth;
			sourceBox.bottom = copyHeight;
			sourceBox.back = 1;
			swapChain.d3d11Context->CopySubresourceRegion(a_destination->resource11, 0, 0, 0, 0, a_source, 0, &sourceBox);
			return true;
		};

		if (!copyIntoShared(a_color, runtimeColorShared[a_contextIndex], colorDesc.Width, colorDesc.Height, runtimeColorSharedDesc.Width, runtimeColorSharedDesc.Height) ||
			!copyIntoShared(a_depth, runtimeDepthShared[a_contextIndex], depthDesc.Width, depthDesc.Height, runtimeDepthSharedDesc.Width, runtimeDepthSharedDesc.Height) ||
			!copyIntoShared(a_motionVectors, runtimeMotionShared[a_contextIndex], motionDesc.Width, motionDesc.Height, runtimeMotionSharedDesc.Width, runtimeMotionSharedDesc.Height) ||
			!copyIntoShared(a_reactiveMask, runtimeReactiveShared[a_contextIndex], reactiveDesc.Width, reactiveDesc.Height, runtimeReactiveSharedDesc.Width, runtimeReactiveSharedDesc.Height) ||
			!copyIntoShared(a_transparencyCompositionMask, runtimeTransparencyShared[a_contextIndex], transparencyDesc.Width, transparencyDesc.Height, runtimeTransparencySharedDesc.Width, runtimeTransparencySharedDesc.Height)) {
			logger::error("[FidelityFX] Runtime upscaler shared-resource copy failed for eye {}", a_contextIndex);
			dispatchOk = false;
		} else {
			const uint64_t d3d11SubmitFence = runtimeFenceValue++;
			DX::ThrowIfFailed(swapChain.d3d11Context->Signal(runtimeD3D11Fence.get(), d3d11SubmitFence));
			DX::ThrowIfFailed(swapChain.commandQueue->Wait(runtimeD3D12Fence.get(), d3d11SubmitFence));

			DX::ThrowIfFailed(commandAllocator->Reset());
			DX::ThrowIfFailed(commandList->Reset(commandAllocator, nullptr));

			std::array<D3D12_RESOURCE_BARRIER, 6> beginBarriers = {
				CD3DX12_RESOURCE_BARRIER::Transition(runtimeColorShared[a_contextIndex]->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(runtimeDepthShared[a_contextIndex]->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(runtimeMotionShared[a_contextIndex]->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(runtimeReactiveShared[a_contextIndex]->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(runtimeTransparencyShared[a_contextIndex]->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(runtimeOutputShared[a_contextIndex]->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
			};
			commandList->ResourceBarrier(static_cast<UINT>(beginBarriers.size()), beginBarriers.data());

			const auto* submitFrame = upscaling.vrSubmit.GetDispatchParameters();
			const auto jitter = submitFrame ? submitFrame->jitter : upscaling.jitter;
			ffx::DispatchDescUpscale dispatchParameters{};
			dispatchParameters.commandList = commandList;
			dispatchParameters.color = ffxApiGetResourceDX12(runtimeColorShared[a_contextIndex]->resource.get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
			dispatchParameters.depth = ffxApiGetResourceDX12(runtimeDepthShared[a_contextIndex]->resource.get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
			dispatchParameters.motionVectors = ffxApiGetResourceDX12(runtimeMotionShared[a_contextIndex]->resource.get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
			dispatchParameters.exposure = FfxApiResource({});
			dispatchParameters.reactive = ffxApiGetResourceDX12(runtimeReactiveShared[a_contextIndex]->resource.get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
			dispatchParameters.transparencyAndComposition = ffxApiGetResourceDX12(runtimeTransparencyShared[a_contextIndex]->resource.get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
			dispatchParameters.output = ffxApiGetResourceDX12(runtimeOutputShared[a_contextIndex]->resource.get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, FFX_API_RESOURCE_USAGE_UAV);
			dispatchParameters.jitterOffset = { -jitter.x, -jitter.y };
			dispatchParameters.motionVectorScale = { a_motionVectorScaleX, a_motionVectorScaleY };
			dispatchParameters.renderSize = { a_renderWidth, a_renderHeight };
			dispatchParameters.upscaleSize = { a_displayWidth, a_displayHeight };
			dispatchParameters.enableSharpening = true;
			dispatchParameters.sharpness = a_sharpness;
			dispatchParameters.frameTimeDelta = submitFrame ? submitFrame->frameTime : *globals::game::deltaTime * 1000.f;
			dispatchParameters.preExposure = 1.0f;
			// Our repo has no ShouldResetHistoryThisFrame(); mirror Upscale()'s existing
			// menu-reset predicate (menus produce no fresh motion vectors/depth).
			dispatchParameters.reset = upscaling.vrSubmit.ShouldResetHistory() || (state->IsMainOrLoadingMenuOpen() && !upscaling.menuCameraMVsValid);
			dispatchParameters.cameraNear = submitFrame ? submitFrame->cameraNear : *globals::game::cameraNear;
			dispatchParameters.cameraFar = submitFrame ? submitFrame->cameraFar : *globals::game::cameraFar;
			dispatchParameters.cameraFovAngleVertical = submitFrame ? submitFrame->verticalFov : Util::GetVerticalFOVRad();
			dispatchParameters.viewSpaceToMetersFactor = 0.01428222656f;
			dispatchParameters.flags = 0;
			const bool runtimeFallbackReset = runtimeFallbackResetDispatchesRemaining > 0;
			dispatchParameters.reset = dispatchParameters.reset || runtimeFallbackReset;

			bool dispatchFaulted = false;
			dispatchOk = DispatchRuntimeUpscalerProtected(runtimeUpscalerContexts[a_contextIndex], dispatchParameters, dispatchFaulted);
			if (dispatchFaulted) {
				commandContext->fenceValue = 0;
				runtimeFallbackResetDispatchesRemaining = std::max(runtimeFallbackResetDispatchesRemaining, runtimeUpscalerContextCount);
				QuarantineRuntimeUpscalerForSession("runtime upscaler dispatch fault");
				logger::critical("[FidelityFX] Runtime upscaler dispatch faulted for eye {}; the provider will not be used again this session", a_contextIndex);
				return false;
			}
			if (dispatchOk && runtimeFallbackReset)
				runtimeFallbackResetDispatchesRemaining--;

			if (!dispatchOk) {
				// A non-OK/faulted dispatch doesn't necessarily throw, so it would otherwise
				// bypass this function's catch blocks (and their quarantine) entirely.
				const HRESULT deviceRemovedReason = swapChain.d3d12Device ? swapChain.d3d12Device->GetDeviceRemovedReason() : S_OK;
				if (FAILED(deviceRemovedReason)) {
					QuarantineRuntimeUpscalerForSession(std::format("runtime upscaler D3D12 device removed: 0x{:08X}", static_cast<uint32_t>(deviceRemovedReason)).c_str());
				} else {
					logger::error("[FidelityFX] Runtime upscaler dispatch returned a failure code");
				}
			}

			std::array<D3D12_RESOURCE_BARRIER, 6> endBarriers = {
				CD3DX12_RESOURCE_BARRIER::Transition(runtimeColorShared[a_contextIndex]->resource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
				CD3DX12_RESOURCE_BARRIER::Transition(runtimeDepthShared[a_contextIndex]->resource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
				CD3DX12_RESOURCE_BARRIER::Transition(runtimeMotionShared[a_contextIndex]->resource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
				CD3DX12_RESOURCE_BARRIER::Transition(runtimeReactiveShared[a_contextIndex]->resource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
				CD3DX12_RESOURCE_BARRIER::Transition(runtimeTransparencyShared[a_contextIndex]->resource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
				CD3DX12_RESOURCE_BARRIER::Transition(runtimeOutputShared[a_contextIndex]->resource.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON),
			};
			commandList->ResourceBarrier(static_cast<UINT>(endBarriers.size()), endBarriers.data());

			DX::ThrowIfFailed(commandList->Close());

			ID3D12CommandList* commandListsToExecute[] = { commandList };
			const uint64_t d3d12SubmitFence = runtimeFenceValue++;
			swapChain.commandQueue->ExecuteCommandLists(1, commandListsToExecute);
			commandListSubmitted = true;
			DX::ThrowIfFailed(swapChain.commandQueue->Signal(runtimeD3D12Fence.get(), d3d12SubmitFence));
			commandContext->fenceValue = d3d12SubmitFence;
			commandFenceTracked = true;
			DX::ThrowIfFailed(swapChain.d3d11Context->Wait(runtimeD3D11Fence.get(), d3d12SubmitFence));

			if (dispatchOk) {
				const uint32_t copyWidth = std::min({ a_displayWidth, outputDesc.Width, runtimeOutputSharedDesc.Width });
				const uint32_t copyHeight = std::min({ a_displayHeight, outputDesc.Height, runtimeOutputSharedDesc.Height });
				if (!copyWidth || !copyHeight) {
					dispatchOk = false;
				} else {
					D3D11_BOX outputBox{};
					outputBox.left = 0;
					outputBox.top = 0;
					outputBox.front = 0;
					outputBox.right = copyWidth;
					outputBox.bottom = copyHeight;
					outputBox.back = 1;
					swapChain.d3d11Context->CopySubresourceRegion(a_output, 0, 0, 0, 0, runtimeOutputShared[a_contextIndex]->resource11, 0, &outputBox);
				}
			}
		}
	} catch (const std::exception& e) {
		if (!commandListSubmitted) {
			commandContext->fenceValue = 0;
		} else if (!commandFenceTracked) {
			try {
				const uint64_t rescueFence = runtimeFenceValue++;
				DX::ThrowIfFailed(swapChain.commandQueue->Signal(runtimeD3D12Fence.get(), rescueFence));
				commandContext->fenceValue = rescueFence;
			} catch (...) {
				commandContext->fenceValue = 0;
			}
		}
		dispatchOk = false;
		QuarantineRuntimeUpscalerForSession(std::format("runtime upscaler dispatch threw: {}", e.what()).c_str());
	} catch (...) {
		if (!commandListSubmitted) {
			commandContext->fenceValue = 0;
		} else if (!commandFenceTracked) {
			try {
				const uint64_t rescueFence = runtimeFenceValue++;
				DX::ThrowIfFailed(swapChain.commandQueue->Signal(runtimeD3D12Fence.get(), rescueFence));
				commandContext->fenceValue = rescueFence;
			} catch (...) {
				commandContext->fenceValue = 0;
			}
		}
		dispatchOk = false;
		QuarantineRuntimeUpscalerForSession("runtime upscaler dispatch threw an unknown exception");
	}

	return dispatchOk;
}

bool FidelityFX::UpscaleRegion(uint32_t a_contextIndex, ID3D11Resource* a_color, ID3D11Resource* a_depth, ID3D11Resource* a_motionVectors,
	ID3D11Resource* a_reactiveMask, ID3D11Resource* a_transparencyCompositionMask, ID3D11Resource* a_output,
	uint32_t a_renderWidth, uint32_t a_renderHeight, uint32_t a_displayWidth, uint32_t a_displayHeight,
	float a_motionVectorScaleX, float a_motionVectorScaleY, float a_sharpness, bool a_forceHostPath)
{
	if (!a_color || !a_depth || !a_motionVectors || !a_reactiveMask || !a_transparencyCompositionMask || !a_output ||
		!a_renderWidth || !a_renderHeight || !a_displayWidth || !a_displayHeight) {
		return false;
	}

	// Snapshot before this eye's own dispatch -- and before the quarantine teardown below,
	// which clears the frame-path tracking this reads -- so it reflects only a sibling eye.
	const bool runtimeAlreadyUsedThisFrame = WasRuntimeUpscalerUsedThisFrame();

	// A quarantined provider's contexts/resources are never dispatched into again; release
	// them at this natural per-call boundary rather than inside the exception handler that
	// discovered the quarantine, where GPU/command-list state is mid-teardown already.
	if (runtimeUpscalerQuarantined && HasRuntimeUpscalerResources())
		ResetRuntimeUpscalerResources(false);

	const bool runtimeFsr4Requested = !a_forceHostPath && ShouldRequestRuntimeFsr4();
	const bool runtimeRequested = !a_forceHostPath && (runtimeFsr4Requested || ShouldUseRuntimeUpscalerForFSR());
	const uint32_t requestedRuntimeVersion = runtimeFsr4Requested ? FFX_UPSCALER_VERSION : Fsr3Version;
	const bool splitPerEyeContexts = UseSplitPerEyeFSRContexts();
	const uint32_t runtimeContextCount = splitPerEyeContexts ? 2u : 1u;
	const bool runtimeSelected = runtimeRequested && CanUseRuntimeUpscalerPath();

	if (runtimeSelected) {
		auto state = globals::state;
		if (!state)
			return false;

		// Mirror the host VR path: keep runtime contexts/resources sized for the largest
		// per-eye display bounds so render-scale quality changes do not force DX11<->DX12
		// runtime context/shared-resource churn.
		const bool useFullRenderBounds = splitPerEyeContexts || runtimeFsr4Requested;
		const uint32_t fullRenderWidth = useFullRenderBounds ? a_displayWidth : a_renderWidth;
		const uint32_t fullRenderHeight = useFullRenderBounds ? a_displayHeight : a_renderHeight;

		auto tryRuntimeUpscaler = [&](uint32_t a_requestedVersion, uint32_t a_fullRenderWidth, uint32_t a_fullRenderHeight) {
			try {
				if (EnsureRuntimeUpscalerContexts(a_fullRenderWidth, a_fullRenderHeight, a_displayWidth, a_displayHeight, runtimeContextCount, a_requestedVersion) &&
					DispatchRuntimeUpscalerSingle(
						a_contextIndex,
						a_color,
						a_depth,
						a_motionVectors,
						a_reactiveMask,
						a_transparencyCompositionMask,
						a_output,
						a_renderWidth,
						a_renderHeight,
						a_displayWidth,
						a_displayHeight,
						a_motionVectorScaleX,
						a_motionVectorScaleY,
						a_sharpness)) {
					RecordRuntimeUpscalerFramePath(GetRuntimeUpscalerProviderFramePath(a_requestedVersion));
					return true;
				}
			} catch (const std::exception& e) {
				QuarantineRuntimeUpscalerForSession(std::format("runtime upscaler context/resource setup threw: {}", e.what()).c_str());
			} catch (...) {
				QuarantineRuntimeUpscalerForSession("runtime upscaler context/resource setup threw an unknown exception");
			}

			return false;
		};

		if (tryRuntimeUpscaler(requestedRuntimeVersion, fullRenderWidth, fullRenderHeight))
			return true;

		// Try the runtime FSR3 provider in the upscaler DLL before falling back to the host SDK.
		// CanUseRuntimeUpscalerPath() re-check: the FSR4 attempt above may have just quarantined
		// the provider, which must stop this fallback from dispatching into it too.
		if (runtimeFsr4Requested && ShouldUseRuntimeUpscalerForFSR() && CanUseRuntimeUpscalerPath()) {
			LatchRuntimeFsr4Failure();
			runtimeFallbackResetDispatchesRemaining = std::max(runtimeFallbackResetDispatchesRemaining, runtimeContextCount);
			const uint32_t fallbackRenderWidth = splitPerEyeContexts ? fullRenderWidth : a_renderWidth;
			const uint32_t fallbackRenderHeight = splitPerEyeContexts ? fullRenderHeight : a_renderHeight;
			if (tryRuntimeUpscaler(Fsr3Version, fallbackRenderWidth, fallbackRenderHeight))
				return true;
		}

		if (!runtimeUpscalerFailureLatched) {
			runtimeFallbackResetDispatchesRemaining = std::max(runtimeFallbackResetDispatchesRemaining, runtimeContextCount);
		}
		LatchRuntimeUpscalerFailure();

		// A sibling eye already published a runtime-provider frame; falling through to host
		// FSR3 here would present an unretractable mixed-provider stereo pair. Drop this eye
		// instead -- the caller skips finalizing the frame when any eye fails.
		if (runtimeAlreadyUsedThisFrame)
			return false;
	}

	// Defense in depth: CanUseRuntimeUpscalerPath() latching mid-frame (e.g. this eye's own
	// failure above) must not let an eye that never attempted the runtime path mix providers
	// with a sibling eye that already succeeded through it this same frame.
	if (!runtimeSelected && runtimeAlreadyUsedThisFrame)
		return false;

	if (!runtimeRequested)
		runtimeFallbackResetDispatchesRemaining = 0;

	// std::size(fsrContext) is always 2; only fsrContext[0] is initialized in flat mode
	// (CreateFSRResources' numContexts), so check against the actually-created count.
	const uint32_t hostContextCount = globals::game::isVR ? 2u : 1u;
	if (!fsrScratchBuffer || a_contextIndex >= hostContextCount)
		return false;

	auto context = globals::d3d::context;
	auto state = globals::state;
	if (!context || !state)
		return false;

	auto& upscaling = globals::features::upscaling;
	const auto* submitFrame = upscaling.vrSubmit.GetDispatchParameters();
	auto jitter = submitFrame ? submitFrame->jitter : upscaling.jitter;
	const auto fallbackFramePath =
		runtimeRequested ? RuntimeUpscalerFramePath::kHostFsr31Fallback : RuntimeUpscalerFramePath::kHostFsr31;
	RecordRuntimeUpscalerFramePath(fallbackFramePath);

	const std::string dispatchPassName = globals::game::isVR ? std::format("Upscaling::HostFsr3Dispatch Eye {}", a_contextIndex) : "Upscaling::HostFsr3Dispatch";
	CS_GPU_PASS_DYNAMIC(dispatchPassName);

	FfxFsr3DispatchUpscaleDescription dispatchParameters{};
	dispatchParameters.commandList = ffxGetCommandListDX11(context);
	dispatchParameters.color = ffxGetResource(a_color, L"FSR3_Input_OutputColor");
	dispatchParameters.depth = ffxGetResource(a_depth, L"FSR3_InputDepth");
	dispatchParameters.motionVectors = ffxGetResource(a_motionVectors, L"FSR3_InputMotionVectors");
	dispatchParameters.exposure = ffxGetResource(nullptr, L"FSR3_InputExposure");
	dispatchParameters.upscaleOutput = ffxGetResource(a_output, L"FSR3_OutputColor");
	dispatchParameters.reactive = ffxGetResource(a_reactiveMask, L"FSR3_InputReactiveMap");
	dispatchParameters.transparencyAndComposition = ffxGetResource(a_transparencyCompositionMask, L"FSR3_TransparencyAndCompositionMap");
	dispatchParameters.motionVectorScale.x = a_motionVectorScaleX;
	dispatchParameters.motionVectorScale.y = a_motionVectorScaleY;
	dispatchParameters.renderSize.width = a_renderWidth;
	dispatchParameters.renderSize.height = a_renderHeight;
	// Left zero-initialized, the SDK defaults upscaleSize to the full eye --
	// a foveated crop dispatch needs its own output extent here, or FSR3
	// reconstructs the crop magnified onto the full-eye canvas.
	dispatchParameters.upscaleSize.width = a_displayWidth;
	dispatchParameters.upscaleSize.height = a_displayHeight;
	dispatchParameters.jitterOffset.x = -jitter.x;
	dispatchParameters.jitterOffset.y = -jitter.y;
	dispatchParameters.frameTimeDelta = submitFrame ? submitFrame->frameTime : *globals::game::deltaTime * 1000.f;
	dispatchParameters.cameraFar = submitFrame ? submitFrame->cameraFar : *globals::game::cameraFar;
	dispatchParameters.cameraNear = submitFrame ? submitFrame->cameraNear : *globals::game::cameraNear;
	dispatchParameters.enableSharpening = true;
	dispatchParameters.sharpness = a_sharpness;
	// Narrows FOV by the crop's height fraction (a no-op ratio of 1.0 when uncropped);
	// FSR3 has no principal-point field, so an off-center crop still gets a centered FOV.
	const float verticalFovFull = submitFrame ? submitFrame->verticalFov : Util::GetVerticalFOVRad();
	const float cropHeightFraction = a_motionVectorScaleY > 0.0f ? (float)a_renderHeight / a_motionVectorScaleY : 1.0f;
	dispatchParameters.cameraFovAngleVertical = 2.0f * std::atan(std::tan(verticalFovFull * 0.5f) * cropHeightFraction);
	dispatchParameters.viewSpaceToMetersFactor = 0.01428222656f;
	const bool runtimeFallbackReset = runtimeRequested && runtimeFallbackResetDispatchesRemaining > 0;
	if (runtimeFallbackReset)
		runtimeFallbackResetDispatchesRemaining--;
	dispatchParameters.reset = upscaling.vrSubmit.ShouldResetHistory() || (state->IsMainOrLoadingMenuOpen() && !upscaling.menuCameraMVsValid) || runtimeFallbackReset;
	dispatchParameters.preExposure = 1.0f;
	dispatchParameters.flags = 0;

	const bool dispatchOK = DispatchHostFsr3UpscaleProtected(fsrContext[a_contextIndex], dispatchParameters);

	return dispatchOK;
}
