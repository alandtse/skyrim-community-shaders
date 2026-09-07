#include "RainRendering.h"

#include "DynamicCubemaps.h"
#include "Globals.h"
#include "GpuPass.h"
#include "I18n/I18n.h"
#include "InverseSquareLighting.h"
#include "LightLimitFix.h"
#include "Skylighting.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/FileSystem.h"
#include "Utils/Game.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

#define I18N_KEY_PREFIX "feature.rain_rendering."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	RainRendering::Settings,
	EnableRainRendering,
	ForceRainRendering,
	EnableRainRoofOcclusion,
	EnableRainWind,
	RainDropCount,
	RainOverheadDropCount,
	RainDensity,
	RainFallSpeed,
	RainWindInfluence,
	RainStreakLength,
	RainVelocityStretch,
	RainStreakWidth,
	RainOpacity,
	RainBrightness,
	RainLightingResponse,
	RainMinimumVisibility,
	RainNearCutoffDistance,
	RainFarDistance,
	RainNearLayerDistance,
	RainMidLayerDistance,
	RainNearBudgetWeight,
	RainMidBudgetWeight,
	RainFarBudgetWeight,
	RainDensityNoiseScale,
	RainDensityNoiseStrength,
	RainCurtainScale,
	RainCurtainStrength,
	RainCurtainContrast,
	RainCurtainMinDensity,
	RainCurtainMaxDensity,
	RainIntersectionFadeDistance,
	RainDebugMode,
	EnableRainRefraction,
	RainCoreDarkening,
	RainEdgeHighlight,
	RainRefractionStrength,
	RainRefractionDistance,
	RainStreakVariation,
	RainLocalLightResponse,
	RainTexturePath,
	RainTextureNormalStrength,
	RainTextureReflectionStrength,
	RainTextureUVWidth,
	RainEnvironmentTransmission,
	RainSceneRefractionMix,
	RainHighlightRoughness,
	RainLightScattering,
	RainRoofOcclusionFadeStart,
	RainRoofOcclusionFadeEnd)

namespace
{
	constexpr uint32_t kGridWidth = 96;
	constexpr uint32_t kGridDepth = 96;
	constexpr uint32_t kGridHeight = 4;
	constexpr float kMaximumRainParticleDensity = 3.0f;
	constexpr float kReferenceRainGravity = 675.0f;

	struct RainPerformancePreset
	{
		uint32_t dropCount;
		float density;
		float farDistance;
	};

	constexpr RainPerformancePreset GetRainPerformancePreset(Feature::PerfProfile a_profile)
	{
		switch (a_profile) {
		case Feature::PerfProfile::Performance:
			return { 8192, 0.8f, 4500.0f };
		case Feature::PerfProfile::Balanced:
			return { 16384, 1.0f, 6000.0f };
		default:
			return { 24576, 1.15f, 9000.0f };
		}
	}

	float ClampFinite(float a_value, float a_minimum, float a_maximum, float a_default)
	{
		return std::clamp(std::isfinite(a_value) ? a_value : a_default, a_minimum, a_maximum);
	}

	float LinearStep(float a_minimum, float a_maximum, float a_value)
	{
		if (a_minimum >= a_maximum)
			return a_value >= a_maximum ? 1.0f : 0.0f;
		return std::clamp((a_value - a_minimum) / (a_maximum - a_minimum), 0.0f, 1.0f);
	}

	template <class T>
	void ReleasePointer(T*& a_pointer)
	{
		if (a_pointer) {
			a_pointer->Release();
			a_pointer = nullptr;
		}
	}

	/** @brief Saves only the D3D11 state modified by the procedural rain draw. */
	class RainPipelineState
	{
	public:
		explicit RainPipelineState(ID3D11DeviceContext* a_context) : context(a_context)
		{
			context->IAGetInputLayout(&inputLayout);
			context->IAGetPrimitiveTopology(&topology);

			context->VSGetShader(&vertexShader, nullptr, nullptr);
			context->HSGetShader(&hullShader, nullptr, nullptr);
			context->DSGetShader(&domainShader, nullptr, nullptr);
			context->GSGetShader(&geometryShader, nullptr, nullptr);
			context->VSGetConstantBuffers(0, static_cast<UINT>(vertexConstantBuffers.size()), vertexConstantBuffers.data());
			context->VSGetShaderResources(1, 1, &vertexShaderResource);
			context->VSGetShaderResources(39, 1, &visibleIndexResource);

			context->PSGetShader(&pixelShader, nullptr, nullptr);
			context->PSGetConstantBuffers(0, 1, &pixelConstantBuffer);
			context->PSGetConstantBuffers(5, static_cast<UINT>(pixelSharedConstantBuffers.size()), pixelSharedConstantBuffers.data());
			context->PSGetShaderResources(0, static_cast<UINT>(pixelShaderResources.size()), pixelShaderResources.data());
			context->PSGetSamplers(0, 1, &pixelSampler);

			context->RSGetState(&rasterizer);
			viewportCount = static_cast<UINT>(viewports.size());
			context->RSGetViewports(&viewportCount, viewports.data());

			context->OMGetRenderTargets(static_cast<UINT>(renderTargets.size()), renderTargets.data(), &depthStencilView);
			context->OMGetBlendState(&blend, blendFactor.data(), &sampleMask);
			context->OMGetDepthStencilState(&depthStencil, &stencilReference);
		}

		RainPipelineState(const RainPipelineState&) = delete;
		RainPipelineState& operator=(const RainPipelineState&) = delete;

		~RainPipelineState()
		{
			std::array<ID3D11ShaderResourceView*, 6> nullResources{};
			context->PSSetShaderResources(0, static_cast<UINT>(nullResources.size()), nullResources.data());
			ID3D11ShaderResourceView* nullVertexResource = nullptr;
			context->VSSetShaderResources(1, 1, &nullVertexResource);
			context->VSSetShaderResources(39, 1, &nullVertexResource);

			context->IASetInputLayout(inputLayout);
			context->IASetPrimitiveTopology(topology);
			context->VSSetShader(vertexShader, nullptr, 0);
			context->HSSetShader(hullShader, nullptr, 0);
			context->DSSetShader(domainShader, nullptr, 0);
			context->GSSetShader(geometryShader, nullptr, 0);
			context->VSSetConstantBuffers(0, static_cast<UINT>(vertexConstantBuffers.size()), vertexConstantBuffers.data());
			context->VSSetShaderResources(1, 1, &vertexShaderResource);
			context->VSSetShaderResources(39, 1, &visibleIndexResource);
			context->PSSetShader(pixelShader, nullptr, 0);
			context->PSSetConstantBuffers(0, 1, &pixelConstantBuffer);
			context->PSSetConstantBuffers(5, static_cast<UINT>(pixelSharedConstantBuffers.size()), pixelSharedConstantBuffers.data());
			context->RSSetState(rasterizer);
			context->RSSetViewports(viewportCount, viewports.data());
			context->OMSetRenderTargets(static_cast<UINT>(renderTargets.size()), renderTargets.data(), depthStencilView);
			context->OMSetBlendState(blend, blendFactor.data(), sampleMask);
			context->OMSetDepthStencilState(depthStencil, stencilReference);
			context->PSSetShaderResources(0, static_cast<UINT>(pixelShaderResources.size()), pixelShaderResources.data());
			context->PSSetSamplers(0, 1, &pixelSampler);

			ReleasePointer(inputLayout);
			ReleasePointer(vertexShader);
			ReleasePointer(hullShader);
			ReleasePointer(domainShader);
			ReleasePointer(geometryShader);
			for (auto*& buffer : vertexConstantBuffers)
				ReleasePointer(buffer);
			ReleasePointer(vertexShaderResource);
			ReleasePointer(visibleIndexResource);
			ReleasePointer(pixelShader);
			ReleasePointer(pixelConstantBuffer);
			for (auto*& buffer : pixelSharedConstantBuffers)
				ReleasePointer(buffer);
			for (auto*& resource : pixelShaderResources)
				ReleasePointer(resource);
			ReleasePointer(pixelSampler);
			ReleasePointer(rasterizer);
			for (auto*& target : renderTargets)
				ReleasePointer(target);
			ReleasePointer(depthStencilView);
			ReleasePointer(blend);
			ReleasePointer(depthStencil);
		}

	private:
		ID3D11DeviceContext* context;
		ID3D11InputLayout* inputLayout = nullptr;
		D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
		ID3D11VertexShader* vertexShader = nullptr;
		ID3D11HullShader* hullShader = nullptr;
		ID3D11DomainShader* domainShader = nullptr;
		ID3D11GeometryShader* geometryShader = nullptr;
		std::array<ID3D11Buffer*, 13> vertexConstantBuffers{};
		ID3D11ShaderResourceView* vertexShaderResource = nullptr;
		ID3D11ShaderResourceView* visibleIndexResource = nullptr;
		ID3D11PixelShader* pixelShader = nullptr;
		ID3D11Buffer* pixelConstantBuffer = nullptr;
		std::array<ID3D11Buffer*, 2> pixelSharedConstantBuffers{};
		std::array<ID3D11ShaderResourceView*, 6> pixelShaderResources{};
		ID3D11SamplerState* pixelSampler = nullptr;
		ID3D11RasterizerState* rasterizer = nullptr;
		std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports{};
		UINT viewportCount = 0;
		std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> renderTargets{};
		ID3D11DepthStencilView* depthStencilView = nullptr;
		ID3D11BlendState* blend = nullptr;
		std::array<float, 4> blendFactor{};
		UINT sampleMask = 0;
		ID3D11DepthStencilState* depthStencil = nullptr;
		UINT stencilReference = 0;
	};

	/** @brief Preserves the compute slots used to update the shared rain-drop buffer. */
	class RainComputeState
	{
	public:
		explicit RainComputeState(ID3D11DeviceContext* a_context) : context(a_context)
		{
			context->CSGetShader(&shader, nullptr, nullptr);
			context->CSGetConstantBuffers(0, 1, &rainConstantBuffer);
			context->CSGetConstantBuffers(5, static_cast<UINT>(sharedConstantBuffers.size()), sharedConstantBuffers.data());
			context->CSGetConstantBuffers(12, 1, &frameConstantBuffer);
			context->CSGetShaderResources(0, 1, &occlusionDepthResource);
			context->CSGetShaderResources(1, 1, &dropResource);
			context->CSGetShaderResources(39, static_cast<UINT>(compactionResources.size()), compactionResources.data());
			context->CSGetShaderResources(35, static_cast<UINT>(computeResources.size()), computeResources.data());
			context->CSGetSamplers(0, 1, &computeSampler);
			context->CSGetUnorderedAccessViews(0, static_cast<UINT>(computeUAVs.size()), computeUAVs.data());
		}

		RainComputeState(const RainComputeState&) = delete;
		RainComputeState& operator=(const RainComputeState&) = delete;

		~RainComputeState()
		{
			ID3D11ShaderResourceView* nullOcclusionDepth = nullptr;
			context->CSSetShaderResources(0, 1, &nullOcclusionDepth);
			ID3D11ShaderResourceView* nullDropResource = nullptr;
			context->CSSetShaderResources(1, 1, &nullDropResource);
			std::array<ID3D11ShaderResourceView*, 2> nullResources{};
			context->CSSetShaderResources(39, static_cast<UINT>(nullResources.size()), nullResources.data());
			std::array<ID3D11UnorderedAccessView*, 5> nullUAVs{};
			context->CSSetUnorderedAccessViews(0, static_cast<UINT>(nullUAVs.size()), nullUAVs.data(), nullptr);
			context->CSSetShader(shader, nullptr, 0);
			context->CSSetConstantBuffers(0, 1, &rainConstantBuffer);
			context->CSSetConstantBuffers(5, static_cast<UINT>(sharedConstantBuffers.size()), sharedConstantBuffers.data());
			context->CSSetConstantBuffers(12, 1, &frameConstantBuffer);
			context->CSSetShaderResources(0, 1, &occlusionDepthResource);
			context->CSSetShaderResources(1, 1, &dropResource);
			context->CSSetShaderResources(39, static_cast<UINT>(compactionResources.size()), compactionResources.data());
			context->CSSetShaderResources(35, static_cast<UINT>(computeResources.size()), computeResources.data());
			context->CSSetSamplers(0, 1, &computeSampler);
			context->CSSetUnorderedAccessViews(0, static_cast<UINT>(computeUAVs.size()), computeUAVs.data(), nullptr);

			ReleasePointer(shader);
			ReleasePointer(rainConstantBuffer);
			for (auto*& buffer : sharedConstantBuffers)
				ReleasePointer(buffer);
			ReleasePointer(frameConstantBuffer);
			ReleasePointer(occlusionDepthResource);
			ReleasePointer(dropResource);
			for (auto*& resource : compactionResources)
				ReleasePointer(resource);
			for (auto*& resource : computeResources)
				ReleasePointer(resource);
			ReleasePointer(computeSampler);
			for (auto*& uav : computeUAVs)
				ReleasePointer(uav);
		}

	private:
		ID3D11DeviceContext* context;
		ID3D11ComputeShader* shader = nullptr;
		ID3D11Buffer* rainConstantBuffer = nullptr;
		std::array<ID3D11Buffer*, 2> sharedConstantBuffers{};
		ID3D11Buffer* frameConstantBuffer = nullptr;
		ID3D11ShaderResourceView* occlusionDepthResource = nullptr;
		ID3D11ShaderResourceView* dropResource = nullptr;
		std::array<ID3D11ShaderResourceView*, 2> compactionResources{};
		std::array<ID3D11ShaderResourceView*, 4> computeResources{};
		ID3D11SamplerState* computeSampler = nullptr;
		std::array<ID3D11UnorderedAccessView*, 5> computeUAVs{};
	};
}

std::pair<std::string, std::vector<std::string>> RainRendering::GetFeatureSummary()
{
	return {
		T("feature.rain_rendering.description",
			"Airborne Rain renders dense world-space precipitation with stereo-stable depth and storm curtains."),
		{ T("feature.rain_rendering.key_feature_1", "The same world-space streak geometry is projected into both VR eyes"),
			T("feature.rain_rendering.key_feature_2", "GPU-generated drops with no per-particle CPU simulation"),
			T("feature.rain_rendering.key_feature_3", "Stable world-space density variation and rain curtains"),
			T("feature.rain_rendering.key_feature_4", "Scene-depth occlusion with distance-layered precipitation") }
	};
}

void RainRendering::SetupResources()
{
	auto* device = globals::d3d::device;
	if (!device)
		return;

	if (!perFrameCB)
		perFrameCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<PerFrame>(), "RainRendering::PerFrame");
	if (!dropBuffer) {
		dropBuffer = std::make_unique<StructuredBuffer>(
			StructuredBufferDesc<DropData>(static_cast<uint64_t>(kMaximumDropCount), true, false),
			kMaximumDropCount,
			"RainRendering::Drops");
		dropBuffer->CreateSRV();
		dropBuffer->CreateUAV();
	}
	if (!dropLocalOffsetBuffer) {
		dropLocalOffsetBuffer = std::make_unique<StructuredBuffer>(
			StructuredBufferDesc<uint32_t>(static_cast<uint64_t>(kMaximumDropCount), true, false),
			kMaximumDropCount,
			"RainRendering::DropLocalOffsets");
		dropLocalOffsetBuffer->CreateSRV();
		dropLocalOffsetBuffer->CreateUAV();
	}
	if (!dropGroupOffsetBuffer) {
		dropGroupOffsetBuffer = std::make_unique<StructuredBuffer>(
			StructuredBufferDesc<uint32_t>(static_cast<uint64_t>(kMaximumCompactionGroupCount), true, false),
			kMaximumCompactionGroupCount,
			"RainRendering::DropGroupOffsets");
		dropGroupOffsetBuffer->CreateSRV();
		dropGroupOffsetBuffer->CreateUAV();
	}
	if (!visibleDropIndexBuffer) {
		visibleDropIndexBuffer = std::make_unique<StructuredBuffer>(
			StructuredBufferDesc<uint32_t>(static_cast<uint64_t>(kMaximumDropCount), true, false),
			kMaximumDropCount,
			"RainRendering::VisibleDropIndices");
		visibleDropIndexBuffer->CreateSRV();
		visibleDropIndexBuffer->CreateUAV();
	}
	if (!indirectDrawArgsBuffer) {
		D3D11_BUFFER_DESC description{};
		description.ByteWidth = sizeof(D3D11_DRAW_INSTANCED_INDIRECT_ARGS);
		description.Usage = D3D11_USAGE_DEFAULT;
		description.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		description.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		indirectDrawArgsBuffer = std::make_unique<Buffer>(description, nullptr, "RainRendering::IndirectDrawArgs");
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDescription{};
		uavDescription.Format = DXGI_FORMAT_R32_TYPELESS;
		uavDescription.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uavDescription.Buffer.NumElements = description.ByteWidth / sizeof(uint32_t);
		uavDescription.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		indirectDrawArgsBuffer->CreateUAV(uavDescription);
	}

	if (!blendState) {
		D3D11_BLEND_DESC description{};
		description.RenderTarget[0].BlendEnable = TRUE;
		description.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
		description.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		description.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		description.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
		description.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
		description.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		description.RenderTarget[0].RenderTargetWriteMask =
			D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
		DX::ThrowIfFailed(device->CreateBlendState(&description, blendState.put()));
		Util::SetResourceName(blendState.get(), "RainRendering::AlphaBlend");
	}

	if (!rasterizerState) {
		D3D11_RASTERIZER_DESC description{};
		description.FillMode = D3D11_FILL_SOLID;
		description.CullMode = D3D11_CULL_NONE;
		description.DepthClipEnable = TRUE;
		DX::ThrowIfFailed(device->CreateRasterizerState(&description, rasterizerState.put()));
		Util::SetResourceName(rasterizerState.get(), "RainRendering::Rasterizer");
	}

	if (!depthStencilState) {
		D3D11_DEPTH_STENCIL_DESC description{};
		description.DepthEnable = FALSE;
		description.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		DX::ThrowIfFailed(device->CreateDepthStencilState(&description, depthStencilState.put()));
		Util::SetResourceName(depthStencilState.get(), "RainRendering::DepthDisabled");
	}

	renderPathReady = perFrameCB && dropBuffer && dropLocalOffsetBuffer && dropGroupOffsetBuffer &&
	                  visibleDropIndexBuffer && indirectDrawArgsBuffer && EnsureShaders();
}

bool RainRendering::CanUseRoofOcclusion() const
{
	const auto& skylighting = globals::features::skylighting;
	return settings.EnableRainRoofOcclusion && skylighting.loaded && skylighting.texProbeArray &&
	       skylighting.texProbeArray->srv.get();
}

bool RainRendering::EnsureShaders()
{
	if (rainUpdateCS && rainCountCS && rainPrefixCS && rainScatterCS && rainVS && rainPS)
		return true;
	if (shaderCompileAttempted)
		return false;
	shaderCompileAttempted = true;

	std::vector<std::pair<const char*, const char*>> defines;
	if (globals::game::isVR)
		defines.emplace_back("VR", "");
	if (globals::features::lightLimitFix.loaded) {
		defines.emplace_back("RAIN_LOCAL_LIGHTS", "");
		if (globals::features::inverseSquareLighting.loaded)
			defines.emplace_back("RAIN_INVERSE_SQUARE", "");
	}
	if (globals::features::skylighting.loaded)
		defines.emplace_back("RAIN_SKYLIGHTING_OCCLUSION", "");

	auto* updateShader = static_cast<ID3D11ComputeShader*>(Util::CompileShader(
		L"Data\\Shaders\\RainRendering\\RainRendering.hlsl", defines, "cs_5_0", "RainUpdateCS"));
	auto* countShader = static_cast<ID3D11ComputeShader*>(Util::CompileShader(
		L"Data\\Shaders\\RainRendering\\RainRendering.hlsl", defines, "cs_5_0", "RainCountCS"));
	auto* prefixShader = static_cast<ID3D11ComputeShader*>(Util::CompileShader(
		L"Data\\Shaders\\RainRendering\\RainRendering.hlsl", defines, "cs_5_0", "RainPrefixCS"));
	auto* scatterShader = static_cast<ID3D11ComputeShader*>(Util::CompileShader(
		L"Data\\Shaders\\RainRendering\\RainRendering.hlsl", defines, "cs_5_0", "RainScatterCS"));
	auto* vertexShader = static_cast<ID3D11VertexShader*>(Util::CompileShader(
		L"Data\\Shaders\\RainRendering\\RainRendering.hlsl", defines, "vs_5_0", "RainVS"));
	auto* pixelShader = static_cast<ID3D11PixelShader*>(Util::CompileShader(
		L"Data\\Shaders\\RainRendering\\RainRendering.hlsl", defines, "ps_5_0", "RainPS"));
	if (!updateShader || !countShader || !prefixShader || !scatterShader || !vertexShader || !pixelShader) {
		if (updateShader)
			updateShader->Release();
		if (countShader)
			countShader->Release();
		if (prefixShader)
			prefixShader->Release();
		if (scatterShader)
			scatterShader->Release();
		if (vertexShader)
			vertexShader->Release();
		if (pixelShader)
			pixelShader->Release();
		logger::error("[RainRendering] Disabling the render path because its runtime shaders failed to compile");
		return false;
	}

	rainUpdateCS.attach(updateShader);
	rainCountCS.attach(countShader);
	rainPrefixCS.attach(prefixShader);
	rainScatterCS.attach(scatterShader);
	rainVS.attach(vertexShader);
	rainPS.attach(pixelShader);
	Util::SetResourceName(rainUpdateCS.get(), "RainRendering::RainUpdateCS");
	Util::SetResourceName(rainCountCS.get(), "RainRendering::RainCountCS");
	Util::SetResourceName(rainPrefixCS.get(), "RainRendering::RainPrefixCS");
	Util::SetResourceName(rainScatterCS.get(), "RainRendering::RainScatterCS");
	Util::SetResourceName(rainVS.get(), "RainRendering::RainVS");
	Util::SetResourceName(rainPS.get(), "RainRendering::RainPS");
	return true;
}

void RainRendering::ClearShaderCache()
{
	rainUpdateCS = nullptr;
	rainCountCS = nullptr;
	rainPrefixCS = nullptr;
	rainScatterCS = nullptr;
	rainVS = nullptr;
	rainPS = nullptr;
	sceneColorDownsampleVS = nullptr;
	sceneColorDownsamplePS = nullptr;
	shaderCompileAttempted = false;
	renderPathReady = false;
	rainTextureSRV = nullptr;
	rainTextureLoadAttempted = false;
	sceneColorCopy = nullptr;
	sceneColorRTV = nullptr;
	sceneColorSRV = nullptr;
	sceneDepthCopy = nullptr;
	sceneDepthRTV = nullptr;
	sceneDepthSRV = nullptr;
	sceneColorDescription = {};
	sceneColorViewFormat = DXGI_FORMAT_UNKNOWN;
	sceneColorCopyFailed = false;
}

void RainRendering::RestoreDefaultSettings()
{
	settings = GetDefaultSettings();
	NormalizeSettings();
}

void RainRendering::LoadSettings(json& o_json)
{
	settings = o_json;
	NormalizeSettings();
}

void RainRendering::SaveSettings(json& o_json)
{
	NormalizeSettings();
	Util::FileHelpers::EnsureDirectoryExists(
		Util::PathHelpers::GetDataPath().parent_path() / std::filesystem::path(kCustomRainTexturePath).parent_path());
	o_json = settings;
}

const RainRendering::Settings& RainRendering::GetDefaultSettings()
{
	static const Settings defaults{};
	return defaults;
}

float RainRendering::GetNearLayerMaximum(float a_farDistance)
{
	return std::min(kMaximumNearLayerDistance, a_farDistance * kNearLayerFarDistanceRatio);
}

float RainRendering::GetMidLayerMinimum(float a_nearDistance)
{
	return std::max(kMinimumMidLayerDistance, a_nearDistance * kMidLayerNearDistanceRatio);
}

float RainRendering::GetMidLayerMaximum(float a_farDistance)
{
	return std::min(kMaximumMidLayerDistance, a_farDistance * kMidLayerFarDistanceRatio);
}

void RainRendering::NormalizeSettings()
{
	const auto& defaults = GetDefaultSettings();
	settings.EnableRainRendering = settings.EnableRainRendering ? 1u : 0u;
	settings.ForceRainRendering = settings.ForceRainRendering ? 1u : 0u;
	settings.EnableRainRoofOcclusion = settings.EnableRainRoofOcclusion ? 1u : 0u;
	settings.EnableRainWind = settings.EnableRainWind ? 1u : 0u;
	settings.EnableRainRefraction = settings.EnableRainRefraction ? 1u : 0u;
	if (settings.RainTexturePath.empty())
		settings.RainTexturePath = kDefaultRainTexturePath;

	settings.RainDropCount = std::clamp(settings.RainDropCount, kDropCountRange.minimum, kDropCountRange.maximum);
	settings.RainOverheadDropCount = std::clamp(settings.RainOverheadDropCount, kOverheadDropCountRange.minimum, kOverheadDropCountRange.maximum);
	settings.RainDensity = ClampFinite(settings.RainDensity, kDoubleUnitRange.minimum, kDoubleUnitRange.maximum, defaults.RainDensity);
	settings.RainWindInfluence = ClampFinite(settings.RainWindInfluence, kDoubleUnitRange.minimum, kDoubleUnitRange.maximum, defaults.RainWindInfluence);
	settings.RainMinimumVisibility = ClampFinite(settings.RainMinimumVisibility, kUnitRange.minimum, kUnitRange.maximum, defaults.RainMinimumVisibility);
	settings.RainNearCutoffDistance = ClampFinite(settings.RainNearCutoffDistance, kNearCutoffDistanceRange.minimum, kNearCutoffDistanceRange.maximum, defaults.RainNearCutoffDistance);
	settings.RainFarDistance = ClampFinite(settings.RainFarDistance, kFarDistanceRange.minimum, kFarDistanceRange.maximum, defaults.RainFarDistance);
	settings.RainCurtainMinDensity = ClampFinite(settings.RainCurtainMinDensity, kCurtainMinimumDensityRange.minimum, kCurtainMinimumDensityRange.maximum, defaults.RainCurtainMinDensity);
	settings.RainCurtainMaxDensity = ClampFinite(settings.RainCurtainMaxDensity,
		std::max(settings.RainCurtainMinDensity, kCurtainMaximumDensityRange.minimum),
		kCurtainMaximumDensityRange.maximum, defaults.RainCurtainMaxDensity);

	const float nearLayerMaximum = GetNearLayerMaximum(settings.RainFarDistance);
	settings.RainNearLayerDistance = ClampFinite(settings.RainNearLayerDistance, kMinimumNearLayerDistance, nearLayerMaximum, defaults.RainNearLayerDistance);
	const float midLayerMinimum = GetMidLayerMinimum(settings.RainNearLayerDistance);
	const float midLayerMaximum = GetMidLayerMaximum(settings.RainFarDistance);
	settings.RainMidLayerDistance = ClampFinite(settings.RainMidLayerDistance, midLayerMinimum, midLayerMaximum, defaults.RainMidLayerDistance);

	settings.RainRefractionStrength = ClampFinite(
		settings.RainRefractionStrength, kUnitRange.minimum, kMaximumRefractionPixels, defaults.RainRefractionStrength);

	settings.RainRoofOcclusionFadeStart = ClampFinite(settings.RainRoofOcclusionFadeStart, kRoofFadeStartRange.minimum, kRoofFadeStartRange.maximum, defaults.RainRoofOcclusionFadeStart);
	settings.RainRoofOcclusionFadeEnd = ClampFinite(settings.RainRoofOcclusionFadeEnd,
		settings.RainRoofOcclusionFadeStart + 0.01f, kUnitRange.maximum, defaults.RainRoofOcclusionFadeEnd);
	settings.RainDebugMode = std::min(settings.RainDebugMode, kMaximumDebugMode);
}

bool RainRendering::UsesWaterMaterial() const
{
	return settings.RainDebugMode == 0 || settings.RainDebugMode >= 6;
}

void RainRendering::ApplyPerformanceProfile(PerfProfile a_profile)
{
	const auto preset = GetRainPerformancePreset(a_profile);
	settings.RainDropCount = preset.dropCount;
	settings.RainDensity = preset.density;
	settings.RainFarDistance = preset.farDistance;
	NormalizeSettings();
}

bool RainRendering::MatchesPerformanceProfile(PerfProfile a_profile) const
{
	const auto preset = GetRainPerformancePreset(a_profile);
	constexpr float kEpsilon = 1e-4f;
	return settings.RainDropCount == preset.dropCount &&
	       std::abs(settings.RainDensity - preset.density) <= kEpsilon &&
	       std::abs(settings.RainFarDistance - preset.farDistance) <= kEpsilon;
}

std::string RainRendering::GetProfilePreviewText(PerfProfile a_profile) const
{
	const auto preset = GetRainPerformancePreset(a_profile);
	return std::vformat(T(TKEY("profile_preview"), "{} drops, {:.2f} density, {:.0f} unit range"),
		std::make_format_args(preset.dropCount, preset.density, preset.farDistance));
}

std::array<uint32_t, 4> RainRendering::GetLayerDropCounts() const
{
	const auto& defaults = GetDefaultSettings();
	const uint32_t count = std::clamp(settings.RainDropCount, kMinimumRuntimeDropCount, kMaximumDropCount);
	float nearWeight = ClampFinite(settings.RainNearBudgetWeight, kBudgetWeightRange.minimum, kBudgetWeightRange.maximum, defaults.RainNearBudgetWeight);
	float midWeight = ClampFinite(settings.RainMidBudgetWeight, kBudgetWeightRange.minimum, kBudgetWeightRange.maximum, defaults.RainMidBudgetWeight);
	float farWeight = ClampFinite(settings.RainFarBudgetWeight, kBudgetWeightRange.minimum, kBudgetWeightRange.maximum, defaults.RainFarBudgetWeight);
	if (nearWeight + midWeight + farWeight < 1e-4f) {
		nearWeight = 1.0f;
		midWeight = 2.0f;
		farWeight = 1.0f;
	}
	const float totalWeight = nearWeight + midWeight + farWeight;
	const float candidates = static_cast<float>(count);
	const uint32_t nearCount = std::min(count, static_cast<uint32_t>(candidates * (nearWeight / totalWeight)));
	const uint32_t midEnd = std::clamp(static_cast<uint32_t>(candidates * ((nearWeight + midWeight) / totalWeight)), nearCount, count);
	return { nearCount, midEnd - nearCount, count - midEnd, count };
}

float4 RainRendering::GetLayerRadii(float a_farDistance) const
{
	const auto& defaults = GetDefaultSettings();
	const float nearDistance = ClampFinite(settings.RainNearLayerDistance, kMinimumNearLayerDistance, a_farDistance * kNearLayerFarDistanceRatio, defaults.RainNearLayerDistance);
	const float midDistance = ClampFinite(settings.RainMidLayerDistance, nearDistance * kMidLayerNearDistanceRatio, a_farDistance * kMidLayerFarDistanceRatio, defaults.RainMidLayerDistance);
	return { nearDistance, midDistance, a_farDistance, 0.2f };
}

RainRendering::WeatherRainState RainRendering::GetWeatherRainState() const
{
	WeatherRainState state{};
	const auto* sky = globals::game::sky;
	if (!sky || sky->mode.get() != RE::Sky::Mode::kFull || Util::IsInterior() ||
		sky->flags.any(RE::Sky::Flags::kHideSky) || !sky->precip)
		return state;

	struct WeatherSample
	{
		float intensity = 0.0f;
		float gravity = kReferenceRainGravity;
		float2 windSlope{};
	};
	const auto getWeatherSample = [](const RE::TESWeather* a_weather, const RE::BSGeometry* a_precipitation) {
		WeatherSample sample{};
		if (!a_weather || !a_weather->precipitationData)
			return sample;
		const auto particleType = a_weather->precipitationData->GetSettingValue(
																  RE::BGSShaderParticleGeometryData::DataID::kParticleType)
		                              .i;
		if (particleType != static_cast<uint32_t>(RE::BGSShaderParticleGeometryData::ParticleType::kRain))
			return sample;

		const float density = a_weather->precipitationData->GetSettingValue(
															  RE::BGSShaderParticleGeometryData::DataID::kParticleDensity)
		                          .f;
		if (std::isfinite(density) && density > 0.0f)
			sample.intensity = std::min(1.0f, density / kMaximumRainParticleDensity);
		const float gravity = a_weather->precipitationData->GetSettingValue(
															  RE::BGSShaderParticleGeometryData::DataID::kGravityVelocity)
		                          .f;
		if (std::isfinite(gravity) && gravity > 0.0f)
			sample.gravity = gravity;
		if (const auto* rainEmitter = GetRainEmitter(a_precipitation)) {
			const auto& wind = rainEmitter->windVelocity;
			const float verticalGravity = std::abs(rainEmitter->gravityVelocity.z);
			if (std::isfinite(wind.x) && std::isfinite(wind.y) &&
				std::isfinite(verticalGravity) && verticalGravity > 1.0f) {
				sample.windSlope = { wind.x / verticalGravity, wind.y / verticalGravity };
				const float slopeLength = sample.windSlope.Length();
				if (slopeLength > 2.0f)
					sample.windSlope *= 2.0f / slopeLength;
			}
		}
		return sample;
	};

	const WeatherSample currentSample = getWeatherSample(sky->currentWeather, sky->precip->currentPrecip.get());
	float currentIntensity = 0.0f;
	if (sky->currentWeather && currentSample.intensity > 0.0f) {
		const float fadeStart = sky->currentWeather->data.precipitationBeginFadeIn * (1.0f / 255.0f);
		currentIntensity = currentSample.intensity *
		                   LinearStep(fadeStart, 1.0f, sky->currentWeatherPct);
	}

	const WeatherSample previousSample = getWeatherSample(sky->lastWeather, sky->precip->lastPrecip.get());
	float previousIntensity = 0.0f;
	if (sky->lastWeather && previousSample.intensity > 0.0f) {
		const float fadeEnd = sky->lastWeather->data.precipitationEndFadeOut * (1.0f / 255.0f);
		previousIntensity = previousSample.intensity *
		                    (1.0f - LinearStep(0.0f, fadeEnd, sky->currentWeatherPct));
	}

	const float combinedIntensity = currentIntensity + previousIntensity;
	if (!std::isfinite(combinedIntensity) || combinedIntensity <= 0.0f)
		return state;
	state.intensity = std::clamp(combinedIntensity, 0.0f, 1.0f);
	const float blendedGravity =
		(currentSample.gravity * currentIntensity + previousSample.gravity * previousIntensity) /
		combinedIntensity;
	state.fallSpeedScale = std::clamp(blendedGravity / kReferenceRainGravity, 0.25f, 4.0f);
	state.windSlope = (currentSample.windSlope * currentIntensity + previousSample.windSlope * previousIntensity) /
	                  combinedIntensity;
	return state;
}

const RE::BSParticleShaderRainEmitter* RainRendering::GetRainEmitter(const RE::BSGeometry* a_precipitation)
{
	if (!a_precipitation)
		return nullptr;

	const auto* particleProperty = netimmerse_cast<RE::BSParticleShaderProperty*>(
		a_precipitation->GetGeometryRuntimeData().shaderProperty.get());
	return particleProperty ?
	           skyrim_cast<RE::BSParticleShaderRainEmitter*>(particleProperty->particleEmitter) :
	           nullptr;
}

RainRendering::CommonBuffer RainRendering::GetCommonBufferData() const
{
	return { IsReplacingVanillaRain() ? 1u : 0u };
}

bool RainRendering::IsReplacingVanillaRain() const
{
	return loaded && settings.EnableRainRendering && renderPathReady;
}

float3 RainRendering::GetRainLightColor() const
{
	if (const auto* sky = globals::game::sky) {
		const auto& sunlight = sky->skyColor[static_cast<uint>(RE::TESWeather::ColorTypes::kSunlight)];
		return {
			ClampFinite(sunlight.red, 0.0f, 8.0f, 1.0f),
			ClampFinite(sunlight.green, 0.0f, 8.0f, 1.0f),
			ClampFinite(sunlight.blue, 0.0f, 8.0f, 1.0f)
		};
	}
	return { 1.0f, 1.0f, 1.0f };
}

bool RainRendering::EnsureRainSampler()
{
	if (refractionSampler)
		return true;
	D3D11_SAMPLER_DESC description{};
	description.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	description.AddressU = description.AddressV = description.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	description.MaxAnisotropy = 1;
	description.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
	description.MaxLOD = D3D11_FLOAT32_MAX;
	if (FAILED(globals::d3d::device->CreateSamplerState(&description, refractionSampler.put())))
		return false;
	Util::SetResourceName(refractionSampler.get(), "RainRendering::LinearClampSampler");
	return true;
}

std::filesystem::path RainRendering::ResolveRainTexturePath(const std::filesystem::path& a_path) const
{
	if (a_path.empty())
		return {};
	if (a_path.is_absolute())
		return Util::PathHelpers::SafeExists(a_path) ? a_path : std::filesystem::path{};

	const auto virtualPath = Util::PathHelpers::GetDataPath().parent_path() / a_path;
	if (Util::PathHelpers::SafeExists(virtualPath))
		return virtualPath;

	const auto realPath = Util::PathHelpers::GetRealPathFromDataRelative(a_path);
	return Util::PathHelpers::SafeExists(realPath) ? realPath : std::filesystem::path{};
}

bool RainRendering::EnsureRainTexture()
{
	if (rainTextureLoadAttempted)
		return rainTextureSRV != nullptr;
	rainTextureLoadAttempted = true;
	if (!EnsureRainSampler()) {
		logger::warn("[RainRendering] Drop texture sampler unavailable; glassy rain cannot render");
		return false;
	}

	ImVec2 dimensions{};
	auto loadTexture = [&](const std::filesystem::path& a_source) {
		const auto resolvedPath = ResolveRainTexturePath(a_source);
		return !resolvedPath.empty() &&
		       Util::LoadTextureFromFile(globals::d3d::device, resolvedPath.string().c_str(), rainTextureSRV.put(), dimensions);
	};

	const std::filesystem::path selectedPath{ settings.RainTexturePath };
	bool loadedTexture = loadTexture(selectedPath);
	if (!loadedTexture && selectedPath.lexically_normal() != std::filesystem::path(kDefaultRainTexturePath).lexically_normal()) {
		logger::warn("[RainRendering] Drop texture unavailable: {}; loading bundled default", settings.RainTexturePath);
		loadedTexture = loadTexture(kDefaultRainTexturePath);
		if (loadedTexture)
			settings.RainTexturePath = kDefaultRainTexturePath;
	}

	if (!loadedTexture) {
		rainTextureSRV = nullptr;
		logger::warn("[RainRendering] Bundled drop texture unavailable; glassy rain cannot render");
		return false;
	}
	rainTextureSize = { dimensions.x, dimensions.y };
	winrt::com_ptr<ID3D11Resource> resource;
	rainTextureSRV->GetResource(resource.put());
	Util::SetResourceName(resource.get(), "RainRendering::DropNormalOpacity");
	Util::SetResourceName(rainTextureSRV.get(), "RainRendering::DropNormalOpacity SRV");
	logger::info("[RainRendering] Loaded drop normal/opacity texture: {} ({}x{})", settings.RainTexturePath, dimensions.x, dimensions.y);
	return true;
}

ID3D11ShaderResourceView* RainRendering::GetRainEnvironment() const
{
	const auto& cubemaps = globals::features::dynamicCubemaps;
	const auto* environment = cubemaps.activeReflections ? cubemaps.envReflectionsTextureBC6H : cubemaps.envTextureBC6H;
	return cubemaps.loaded && environment ? environment->srv.get() : nullptr;
}

bool RainRendering::EnsureSceneColorShaders()
{
	if (sceneColorDownsampleVS && sceneColorDownsamplePS)
		return true;

	std::vector<std::pair<const char*, const char*>> defines;
	if (globals::game::isVR)
		defines.emplace_back("VR", "");
	auto* vertexShader = static_cast<ID3D11VertexShader*>(Util::CompileShader(
		L"Data\\Shaders\\RainRendering\\RainRendering.hlsl", defines, "vs_5_0", "RainSceneColorVS"));
	auto* pixelShader = static_cast<ID3D11PixelShader*>(Util::CompileShader(
		L"Data\\Shaders\\RainRendering\\RainRendering.hlsl", defines, "ps_5_0", "RainSceneColorPS"));
	if (!vertexShader || !pixelShader) {
		if (vertexShader)
			vertexShader->Release();
		if (pixelShader)
			pixelShader->Release();
		return false;
	}

	sceneColorDownsampleVS.attach(vertexShader);
	sceneColorDownsamplePS.attach(pixelShader);
	Util::SetResourceName(sceneColorDownsampleVS.get(), "RainRendering::SceneColorDownsampleVS");
	Util::SetResourceName(sceneColorDownsamplePS.get(), "RainRendering::SceneColorDownsamplePS");
	return true;
}

bool RainRendering::EnsureSceneColorCopy(ID3D11Texture2D* a_source, ID3D11RenderTargetView* a_view)
{
	D3D11_TEXTURE2D_DESC sourceDescription{};
	a_source->GetDesc(&sourceDescription);
	D3D11_RENDER_TARGET_VIEW_DESC viewDescription{};
	a_view->GetDesc(&viewDescription);
	if (sourceDescription.SampleDesc.Count != 1 || sourceDescription.ArraySize != 1)
		return false;

	const bool changed = sourceDescription.Width != sceneColorDescription.Width ||
	                     sourceDescription.Height != sceneColorDescription.Height ||
	                     sourceDescription.Format != sceneColorDescription.Format || viewDescription.Format != sceneColorViewFormat;
	if (!changed && (sceneColorSRV || sceneColorCopyFailed))
		return sceneColorSRV && sceneDepthSRV;

	sceneColorCopy = nullptr;
	sceneColorRTV = nullptr;
	sceneColorSRV = nullptr;
	sceneDepthCopy = nullptr;
	sceneDepthRTV = nullptr;
	sceneDepthSRV = nullptr;
	sceneColorDescription = sourceDescription;
	sceneColorViewFormat = viewDescription.Format;
	sceneColorCopyFailed = false;
	if (!EnsureSceneColorShaders()) {
		sceneColorCopyFailed = true;
		logger::warn("[RainRendering] Refraction downsample shaders are unavailable; using shading-only rain");
		return false;
	}
	D3D11_TEXTURE2D_DESC copyDescription = sourceDescription;
	const UINT sourceEyeWidth = globals::game::isVR ? (sourceDescription.Width + 1u) / 2u : sourceDescription.Width;
	copyDescription.Width = globals::game::isVR ? ((sourceEyeWidth + 1u) / 2u) * 2u : (sourceDescription.Width + 1u) / 2u;
	copyDescription.Height = (sourceDescription.Height + 1u) / 2u;
	copyDescription.MipLevels = 1;
	copyDescription.Usage = D3D11_USAGE_DEFAULT;
	copyDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	copyDescription.CPUAccessFlags = 0;
	copyDescription.MiscFlags = 0;
	auto* device = globals::d3d::device;
	HRESULT result = device->CreateTexture2D(&copyDescription, nullptr, sceneColorCopy.put());
	if (SUCCEEDED(result)) {
		Util::SetResourceName(sceneColorCopy.get(), "RainRendering::HalfResolutionSceneColor");
		D3D11_RENDER_TARGET_VIEW_DESC rtvDescription{};
		rtvDescription.Format = viewDescription.Format;
		rtvDescription.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		result = device->CreateRenderTargetView(sceneColorCopy.get(), &rtvDescription, sceneColorRTV.put());
	}
	if (SUCCEEDED(result)) {
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDescription{};
		srvDescription.Format = viewDescription.Format;
		srvDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDescription.Texture2D.MipLevels = 1;
		result = device->CreateShaderResourceView(sceneColorCopy.get(), &srvDescription, sceneColorSRV.put());
	}
	D3D11_TEXTURE2D_DESC depthDescription = copyDescription;
	depthDescription.Format = DXGI_FORMAT_R32_FLOAT;
	if (SUCCEEDED(result))
		result = device->CreateTexture2D(&depthDescription, nullptr, sceneDepthCopy.put());
	if (SUCCEEDED(result)) {
		Util::SetResourceName(sceneDepthCopy.get(), "RainRendering::HalfResolutionSceneDepth");
		result = device->CreateRenderTargetView(sceneDepthCopy.get(), nullptr, sceneDepthRTV.put());
	}
	if (SUCCEEDED(result))
		result = device->CreateShaderResourceView(sceneDepthCopy.get(), nullptr, sceneDepthSRV.put());
	if (SUCCEEDED(result) && !EnsureRainSampler())
		result = E_FAIL;
	if (FAILED(result)) {
		sceneColorCopy = nullptr;
		sceneColorRTV = nullptr;
		sceneColorSRV = nullptr;
		sceneDepthCopy = nullptr;
		sceneDepthRTV = nullptr;
		sceneDepthSRV = nullptr;
		sceneColorCopyFailed = true;
		logger::warn("[RainRendering] Refraction unavailable (HRESULT {:#x}); using shading-only rain", static_cast<uint32_t>(result));
		return false;
	}
	Util::SetResourceName(sceneColorRTV.get(), "RainRendering::HalfResolutionSceneColor RTV");
	Util::SetResourceName(sceneColorSRV.get(), "RainRendering::HalfResolutionSceneColor SRV");
	Util::SetResourceName(sceneDepthRTV.get(), "RainRendering::HalfResolutionSceneDepth RTV");
	Util::SetResourceName(sceneDepthSRV.get(), "RainRendering::HalfResolutionSceneDepth SRV");
	return true;
}

void RainRendering::DownsampleSceneColor(
	ID3D11ShaderResourceView* a_color, ID3D11ShaderResourceView* a_depth, const float2& a_size)
{
	CS_GPU_PASS("RainRendering::SceneColorDownsample");
	auto* context = globals::d3d::context;
	std::array<ID3D11RenderTargetView*, 2> renderTargets{ sceneColorRTV.get(), sceneDepthRTV.get() };
	context->OMSetRenderTargets(static_cast<UINT>(renderTargets.size()), renderTargets.data(), nullptr);
	context->OMSetBlendState(nullptr, nullptr, UINT_MAX);
	context->OMSetDepthStencilState(depthStencilState.get(), 0);
	context->RSSetState(rasterizerState.get());
	const float sourceEyeWidth = globals::game::isVR ? std::ceil(a_size.x * 0.5f) : a_size.x;
	const float targetWidth = globals::game::isVR ? std::ceil(sourceEyeWidth * 0.5f) * 2.0f : std::ceil(a_size.x * 0.5f);
	const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, targetWidth, std::ceil(a_size.y * 0.5f), 0.0f, 1.0f };
	context->RSSetViewports(1, &viewport);
	context->IASetInputLayout(nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->VSSetShader(sceneColorDownsampleVS.get(), nullptr, 0);
	context->HSSetShader(nullptr, nullptr, 0);
	context->DSSetShader(nullptr, nullptr, 0);
	context->GSSetShader(nullptr, nullptr, 0);
	context->PSSetShader(sceneColorDownsamplePS.get(), nullptr, 0);
	ID3D11Buffer* rainBuffer = perFrameCB->CB();
	context->PSSetConstantBuffers(0, 1, &rainBuffer);
	context->PSSetShaderResources(0, 1, &a_depth);
	context->PSSetShaderResources(2, 1, &a_color);
	ID3D11SamplerState* sampler = refractionSampler.get();
	context->PSSetSamplers(0, 1, &sampler);
	context->Draw(3, 0);
	ID3D11ShaderResourceView* nullResource = nullptr;
	context->PSSetShaderResources(0, 1, &nullResource);
	context->PSSetShaderResources(2, 1, &nullResource);
}

void RainRendering::UpdateGlassyConstants(PerFrame& a_data, const D3D11_TEXTURE2D_DESC& a_description, const float2& a_size, bool a_hasSceneColor) const
{
	const auto& defaults = GetDefaultSettings();
	const bool glassy = UsesWaterMaterial();
	a_data.Glassy = { glassy ? 1.0f : 0.0f,
		ClampFinite(settings.RainCoreDarkening, kCoreDarkeningRange.minimum, kCoreDarkeningRange.maximum, defaults.RainCoreDarkening),
		ClampFinite(settings.RainEdgeHighlight, kEdgeHighlightRange.minimum, kEdgeHighlightRange.maximum, defaults.RainEdgeHighlight),
		ClampFinite(settings.RainRefractionStrength, kUnitRange.minimum, kMaximumRefractionPixels, defaults.RainRefractionStrength) };
	a_data.Refraction = { ClampFinite(settings.RainRefractionDistance, kRefractionDistanceRange.minimum, kRefractionDistanceRange.maximum, defaults.RainRefractionDistance),
		a_hasSceneColor ? 1.0f : 0.0f,
		glassy ? ClampFinite(settings.RainStreakVariation, kUnitRange.minimum, kUnitRange.maximum, defaults.RainStreakVariation) : 0.0f,
		ClampFinite(settings.RainEnvironmentTransmission, kUnitRange.minimum, kUnitRange.maximum, defaults.RainEnvironmentTransmission) };
	a_data.ScreenSize = { a_size.x, a_size.y, 1.0f / a_description.Width, 1.0f / a_description.Height };
	a_data.TexturedRain = { glassy && rainTextureSRV && refractionSampler ? 1.0f : 0.0f,
		ClampFinite(settings.RainTextureNormalStrength, kDoubleUnitRange.minimum, kDoubleUnitRange.maximum, defaults.RainTextureNormalStrength),
		ClampFinite(settings.RainTextureReflectionStrength, kDoubleUnitRange.minimum, kDoubleUnitRange.maximum, defaults.RainTextureReflectionStrength), refractionSampler && GetRainEnvironment() ? 1.0f : 0.0f };
	a_data.RainTextureShape = { rainTextureSize.x, rainTextureSize.y,
		ClampFinite(settings.RainTextureUVWidth, kTextureUVWidthRange.minimum, kTextureUVWidthRange.maximum, defaults.RainTextureUVWidth), 0.0f };
	a_data.MaterialLighting = { ClampFinite(settings.RainHighlightRoughness, kHighlightRoughnessRange.minimum, kHighlightRoughnessRange.maximum, defaults.RainHighlightRoughness),
		ClampFinite(settings.RainLightScattering, kUnitRange.minimum, kUnitRange.maximum, defaults.RainLightScattering),
		ClampFinite(settings.RainSceneRefractionMix, kUnitRange.minimum, kUnitRange.maximum, defaults.RainSceneRefractionMix), 0.0f };
	const auto& lightLimitFix = globals::features::lightLimitFix;
	if (glassy && lightLimitFix.loaded && lightLimitFix.lights && lightLimitFix.lightGrid && lightLimitFix.lightIndexList) {
		a_data.LocalLighting = { ClampFinite(settings.RainLocalLightResponse, kDoubleUnitRange.minimum, kDoubleUnitRange.maximum, defaults.RainLocalLightResponse),
			a_data.Refraction.x, std::max(lightLimitFix.lightsNear, 0.1f), std::max(lightLimitFix.lightsFar, lightLimitFix.lightsNear + 1.0f) };
		a_data.LightGrid = { lightLimitFix.clusterSize[0], lightLimitFix.clusterSize[1], lightLimitFix.clusterSize[2], lightLimitFix.lightCount };
	}
}

RainRendering::PerFrame RainRendering::BuildPerFrameData(
	const WeatherRainState& a_weather,
	const D3D11_TEXTURE2D_DESC& a_description,
	const float2& a_size,
	bool a_hasSceneColor) const
{
	const auto& defaults = GetDefaultSettings();
	const float farDistance = ClampFinite(settings.RainFarDistance, kRuntimeFarDistanceRange.minimum, kRuntimeFarDistanceRange.maximum, defaults.RainFarDistance);
	const auto head = Util::GetAverageEyePosition();
	const auto lightColor = GetRainLightColor();

	PerFrame data{};
	data.HeadPositionAndTime = { head.x, head.y, head.z, globals::state->timer };
	data.VolumeSizeAndDensity = {
		farDistance * 2.0f,
		farDistance * 2.0f,
		farDistance * 2.0f,
		ClampFinite(settings.RainDensity, kDoubleUnitRange.minimum, kDoubleUnitRange.maximum, defaults.RainDensity)
	};
	data.WeatherFallDepth = {
		a_weather.intensity,
		ClampFinite(settings.RainFallSpeed, kRuntimeFallSpeedRange.minimum, kRuntimeFallSpeedRange.maximum, defaults.RainFallSpeed) * a_weather.fallSpeedScale,
		0.0f,
		ClampFinite(settings.RainIntersectionFadeDistance, kRuntimeIntersectionFadeRange.minimum, kRuntimeIntersectionFadeRange.maximum, defaults.RainIntersectionFadeDistance)
	};
	data.Streak = {
		ClampFinite(settings.RainStreakLength, kRuntimeStreakLengthRange.minimum, kRuntimeStreakLengthRange.maximum, defaults.RainStreakLength),
		ClampFinite(settings.RainVelocityStretch, kUnitRange.minimum, kUnitRange.maximum, defaults.RainVelocityStretch),
		ClampFinite(settings.RainStreakWidth, kRuntimeStreakWidthRange.minimum, kRuntimeStreakWidthRange.maximum, defaults.RainStreakWidth),
		ClampFinite(settings.RainOpacity, kUnitRange.minimum, kUnitRange.maximum, defaults.RainOpacity)
	};
	data.Appearance = {
		ClampFinite(settings.RainBrightness, kRuntimeBrightnessRange.minimum, kRuntimeBrightnessRange.maximum, defaults.RainBrightness),
		ClampFinite(settings.RainLightingResponse, kUnitRange.minimum, kUnitRange.maximum, defaults.RainLightingResponse),
		ClampFinite(settings.RainMinimumVisibility, kUnitRange.minimum, kUnitRange.maximum, defaults.RainMinimumVisibility),
		ClampFinite(settings.RainNearCutoffDistance, kNearCutoffDistanceRange.minimum, kNearCutoffDistanceRange.maximum, defaults.RainNearCutoffDistance)
	};
	data.DistanceNoise = {
		farDistance,
		ClampFinite(settings.RainDensityNoiseScale, kRuntimeDensityNoiseScaleRange.minimum, kRuntimeDensityNoiseScaleRange.maximum, defaults.RainDensityNoiseScale),
		ClampFinite(settings.RainDensityNoiseStrength, kUnitRange.minimum, kUnitRange.maximum, defaults.RainDensityNoiseStrength),
		0.0f
	};
	data.Curtain = {
		ClampFinite(settings.RainCurtainScale, kRuntimeCurtainScaleRange.minimum, kRuntimeCurtainScaleRange.maximum, defaults.RainCurtainScale),
		ClampFinite(settings.RainCurtainStrength, kUnitRange.minimum, kUnitRange.maximum, defaults.RainCurtainStrength),
		ClampFinite(settings.RainCurtainContrast, kRuntimeCurtainContrastRange.minimum, kRuntimeCurtainContrastRange.maximum, defaults.RainCurtainContrast),
		0.0f
	};
	data.CurtainDensity = {
		ClampFinite(settings.RainCurtainMinDensity, kRuntimeCurtainDensityRange.minimum, kRuntimeCurtainDensityRange.maximum, defaults.RainCurtainMinDensity),
		ClampFinite(settings.RainCurtainMaxDensity, kRuntimeCurtainDensityRange.minimum, kRuntimeCurtainDensityRange.maximum, defaults.RainCurtainMaxDensity),
		0.0f,
		0.0f
	};
	data.LightColor = { lightColor.x, lightColor.y, lightColor.z, 0.0f };
	data.CameraData = Util::GetCameraData();
	data.GridAndDebug = {
		kGridWidth,
		kGridDepth,
		kGridHeight,
		std::min<uint>(settings.RainDebugMode, kMaximumDebugMode)
	};
	data.LayerRadii = GetLayerRadii(farDistance);
	data.LayerCounts = GetLayerDropCounts();
	const bool hasRoofOcclusion = CanUseRoofOcclusion();
	const float roofFadeStart = ClampFinite(
		settings.RainRoofOcclusionFadeStart, kRoofFadeStartRange.minimum, kRoofFadeStartRange.maximum, defaults.RainRoofOcclusionFadeStart);
	const float roofFadeEnd = std::max(
		ClampFinite(settings.RainRoofOcclusionFadeEnd, kUnitRange.minimum, kUnitRange.maximum, defaults.RainRoofOcclusionFadeEnd),
		roofFadeStart + 0.01f);
	const uint32_t overheadDropCount = std::min(settings.RainOverheadDropCount, data.LayerCounts[0]);
	data.RoofOcclusion = {
		hasRoofOcclusion ? 1.0f : 0.0f,
		roofFadeStart,
		roofFadeEnd,
		static_cast<float>(overheadDropCount)
	};
	data.VanillaWind = {
		a_weather.windSlope.x,
		a_weather.windSlope.y,
		settings.EnableRainWind ?
			ClampFinite(settings.RainWindInfluence, kDoubleUnitRange.minimum, kDoubleUnitRange.maximum, defaults.RainWindInfluence) :
			0.0f,
		0.0f
	};
	UpdateGlassyConstants(data, a_description, a_size, a_hasSceneColor);
	return data;
}

void RainRendering::DrawAtVanillaRainPass()
{
	DrawRain();
}

void RainRendering::DrawForcedRainFallback()
{
	if (settings.ForceRainRendering)
		DrawRain();
}

void RainRendering::DrawRain()
{
	const uint32_t frame = globals::state->frameCount;
	if (lastDrawFrame == frame)
		return;

	if (!settings.EnableRainRendering || globals::state->IsFullScreenMenuOpen())
		return;

	const WeatherRainState weather = settings.ForceRainRendering ? WeatherRainState{ 1.0f, 1.0f } : GetWeatherRainState();
	if (weather.intensity <= 0.0f)
		return;

	auto* renderer = globals::game::renderer;
	auto* context = globals::d3d::context;
	auto* frameBuffer = globals::game::perFrame.get() ? *globals::game::perFrame.get() : nullptr;
	if (!renderer || !context || !frameBuffer)
		return;
	auto& mainTarget = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	auto& mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	auto& stableWorldDepth =
		renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
	auto* depthResource = stableWorldDepth.depthSRV ? stableWorldDepth.depthSRV : mainDepth.depthSRV;
	if (!mainTarget.texture || !mainTarget.RTV || !depthResource)
		return;
	D3D11_TEXTURE2D_DESC mainDescription{};
	mainTarget.texture->GetDesc(&mainDescription);
	const float2 dynamicSize = Util::ConvertToDynamic({ static_cast<float>(mainDescription.Width), static_cast<float>(mainDescription.Height) });
	if (dynamicSize.x < 2.0f || dynamicSize.y < 1.0f)
		return;

	SetupResources();
	if (!perFrameCB || !dropBuffer || !dropLocalOffsetBuffer || !dropGroupOffsetBuffer ||
		!visibleDropIndexBuffer || !indirectDrawArgsBuffer || !EnsureShaders()) {
		renderPathReady = false;
		return;
	}
	const bool usesWaterMaterial = UsesWaterMaterial();
	if (usesWaterMaterial && !EnsureRainTexture()) {
		renderPathReady = false;
		return;
	}
	renderPathReady = true;
	lastDrawFrame = frame;

	CS_GPU_PASS("RainRendering::AirborneRain");
	const bool hasSceneColor = mainTarget.SRV && usesWaterMaterial && settings.EnableRainRefraction &&
	                           settings.RainSceneRefractionMix > 0.0f && settings.RainRefractionStrength > 0.0f &&
	                           EnsureSceneColorCopy(mainTarget.texture, mainTarget.RTV);

	PerFrame data = BuildPerFrameData(weather, mainDescription, dynamicSize, hasSceneColor);
	const auto& skylighting = globals::features::skylighting;
	const bool hasRoofOcclusion = data.RoofOcclusion.x > 0.5f;
	perFrameCB->Update(data);

	const uint32_t dropCount = data.LayerCounts[3];
	const uint32_t groupCount = (dropCount + kRainComputeGroupSize - 1u) / kRainComputeGroupSize;
	ID3D11Buffer* rainBuffer = perFrameCB->CB();
	ID3D11Buffer* sharedBuffer = globals::state->sharedDataCB->CB();
	{
		CS_GPU_PASS("RainRendering::UpdateDrops");
		RainComputeState savedComputeState(context);
		context->CSSetShader(rainUpdateCS.get(), nullptr, 0);
		context->CSSetConstantBuffers(0, 1, &rainBuffer);
		context->CSSetConstantBuffers(5, 1, &sharedBuffer);
		ID3D11Buffer* featureBuffer = globals::state->featureDataCB->CB();
		context->CSSetConstantBuffers(6, 1, &featureBuffer);
		context->CSSetConstantBuffers(12, 1, &frameBuffer);
		std::array<ID3D11ShaderResourceView*, 4> computeResources{};
		if (data.LocalLighting.x > 0.0f) {
			const auto& lightLimitFix = globals::features::lightLimitFix;
			computeResources[0] = lightLimitFix.lights->srv.get();
			computeResources[1] = lightLimitFix.lightIndexList->srv.get();
			computeResources[2] = lightLimitFix.lightGrid->srv.get();
		}
		if (hasRoofOcclusion)
			computeResources[3] = skylighting.texProbeArray->srv.get();
		context->CSSetShaderResources(35, static_cast<UINT>(computeResources.size()), computeResources.data());
		std::array<ID3D11UnorderedAccessView*, 5> computeUAVs{};
		computeUAVs[0] = dropBuffer->UAV();
		context->CSSetUnorderedAccessViews(0, static_cast<UINT>(computeUAVs.size()), computeUAVs.data(), nullptr);
		context->Dispatch(groupCount, 1, 1);

		computeUAVs.fill(nullptr);
		context->CSSetUnorderedAccessViews(0, static_cast<UINT>(computeUAVs.size()), computeUAVs.data(), nullptr);
		ID3D11ShaderResourceView* dropResource = dropBuffer->SRV();
		context->CSSetShaderResources(1, 1, &dropResource);
		computeUAVs[1] = dropLocalOffsetBuffer->UAV();
		computeUAVs[2] = dropGroupOffsetBuffer->UAV();
		context->CSSetUnorderedAccessViews(0, static_cast<UINT>(computeUAVs.size()), computeUAVs.data(), nullptr);
		context->CSSetShader(rainCountCS.get(), nullptr, 0);
		context->Dispatch(groupCount, 1, 1);

		computeUAVs.fill(nullptr);
		computeUAVs[2] = dropGroupOffsetBuffer->UAV();
		computeUAVs[4] = indirectDrawArgsBuffer->uav.get();
		context->CSSetUnorderedAccessViews(0, static_cast<UINT>(computeUAVs.size()), computeUAVs.data(), nullptr);
		context->CSSetShader(rainPrefixCS.get(), nullptr, 0);
		context->Dispatch(1, 1, 1);

		computeUAVs.fill(nullptr);
		context->CSSetUnorderedAccessViews(0, static_cast<UINT>(computeUAVs.size()), computeUAVs.data(), nullptr);
		ID3D11ShaderResourceView* compactionResources[]{ dropLocalOffsetBuffer->SRV(), dropGroupOffsetBuffer->SRV() };
		context->CSSetShaderResources(39, static_cast<UINT>(std::size(compactionResources)), compactionResources);
		computeUAVs[3] = visibleDropIndexBuffer->UAV();
		context->CSSetUnorderedAccessViews(0, static_cast<UINT>(computeUAVs.size()), computeUAVs.data(), nullptr);
		context->CSSetShader(rainScatterCS.get(), nullptr, 0);
		context->Dispatch(groupCount, 1, 1);
	}

	RainPipelineState savedState(context);
	if (hasSceneColor)
		DownsampleSceneColor(mainTarget.SRV, depthResource, dynamicSize);
	ID3D11RenderTargetView* renderTarget = mainTarget.RTV;
	context->OMSetRenderTargets(1, &renderTarget, nullptr);
	context->OMSetBlendState(blendState.get(), nullptr, UINT_MAX);
	context->OMSetDepthStencilState(depthStencilState.get(), 0);
	context->RSSetState(rasterizerState.get());

	D3D11_VIEWPORT viewport{ 0.0f, 0.0f, dynamicSize.x, dynamicSize.y, 0.0f, 1.0f };
	context->RSSetViewports(1, &viewport);

	context->IASetInputLayout(nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->VSSetShader(rainVS.get(), nullptr, 0);
	context->HSSetShader(nullptr, nullptr, 0);
	context->DSSetShader(nullptr, nullptr, 0);
	context->GSSetShader(nullptr, nullptr, 0);
	context->PSSetShader(rainPS.get(), nullptr, 0);

	context->VSSetConstantBuffers(0, 1, &rainBuffer);
	context->VSSetConstantBuffers(5, 1, &sharedBuffer);
	context->VSSetConstantBuffers(12, 1, &frameBuffer);
	ID3D11ShaderResourceView* dropSRV = dropBuffer->SRV();
	ID3D11ShaderResourceView* visibleIndexSRV = visibleDropIndexBuffer->SRV();
	context->VSSetShaderResources(1, 1, &dropSRV);
	context->VSSetShaderResources(39, 1, &visibleIndexSRV);
	context->PSSetConstantBuffers(0, 1, &rainBuffer);
	ID3D11Buffer* featureBuffer = globals::state->featureDataCB->CB();
	ID3D11Buffer* pixelSharedBuffers[]{ sharedBuffer, featureBuffer };
	context->PSSetConstantBuffers(5, 2, pixelSharedBuffers);
	context->PSSetShaderResources(0, 1, &depthResource);
	ID3D11ShaderResourceView* colorResource = hasSceneColor ? sceneColorSRV.get() : nullptr;
	context->PSSetShaderResources(2, 1, &colorResource);
	ID3D11ShaderResourceView* waterResources[]{ rainTextureSRV.get(), GetRainEnvironment() };
	context->PSSetShaderResources(3, 2, waterResources);
	ID3D11ShaderResourceView* refractionDepthResource = hasSceneColor ? sceneDepthSRV.get() : nullptr;
	context->PSSetShaderResources(5, 1, &refractionDepthResource);
	ID3D11SamplerState* sampler = refractionSampler.get();
	context->PSSetSamplers(0, 1, &sampler);

	context->DrawInstancedIndirect(indirectDrawArgsBuffer->resource.get(), 0);
}
