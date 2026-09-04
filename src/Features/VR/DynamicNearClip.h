#pragma once

#include "Buffer.h"
#include "NearClipController.h"
#include <array>
#include <chrono>
#include <limits>
#include <memory>

/** @brief VR world-camera near-plane controller and asynchronous two-eye depth probe. */
class VRDynamicNearClip
{
public:
	/** @brief Install the verified Skyrim VR 1.4.15 camera preparation call hook. */
	void Install();
	/** @brief Allocate the tiny probe/readback resources; failure leaves the engine camera intact. */
	void SetupResources();
	/** @brief Recreate the standalone probe shader after a cache clear. */
	void ClearShaderCache();
	/** @brief Set the source frustums before Skyrim rebuilds projections and culling data. */
	void BeforeCameraUpdate();
	/** @brief Sample solid prepass depth before excluded fog draws, or completed opaque depth. */
	void CaptureDepth(const RE::NiCamera* camera, ID3D11ShaderResourceView* prepassDepth = nullptr, ID3D11ShaderResourceView* terrainDepth = nullptr);
	/** @brief Begin collecting excluded draws within the existing world depth prepass. */
	void BeginWorldDepth();
	/** @brief Probe solid depth, then issue each postponed fog depth draw exactly once. */
	void FinishWorldDepth();
	/** @brief Original depth draw entry point, retained for issuing postponed mesh draws. */
	using DepthDrawFunction = void (*)(RE::BSRenderPass*, uint32_t, bool, uint32_t);
	/** @brief Postpone only tagged model draws in the world depth prepass until after probing. */
	bool DeferIgnoredDepth(RE::BSRenderPass* pass, uint32_t technique, bool alphaTest, uint32_t renderFlags, DepthDrawFunction draw);
	/** @brief Whether this camera frame uses the adaptive override, including pending restoration. */
	bool IsControllingCamera() const { return controlledCamera.get() != nullptr; }
	/** @brief Draw controls and observed projection values in the VR settings panel. */
	void DrawSettings();
	/** @brief Draw the optional diagnostic HUD in the helper's headset context. */
	void DrawReadout();

private:
	using Clock = std::chrono::steady_clock;
	struct EyeDepth
	{
		float nearest;
		uint32_t validSamples;
	};
	struct Readback
	{
		winrt::com_ptr<ID3D11Buffer> buffer;
		Clock::time_point captured{};
		uint64_t serial = 0;
		bool pending = false;
	};
	struct alignas(16) ProbeConstants
	{
		float4 cameraData;
		uint32_t width;
		uint32_t height;
		uint32_t hasTerrainDepth;
		float padding[9]{};
	};
	struct DepthDrawState
	{
		D3D11_VIEWPORT viewPort;
		RE::BSGraphics::DepthStencilDepthMode depthMode;
		RE::BSGraphics::DepthStencilDepthMode previousDepthMode;
		uint32_t stencilMode;
		uint32_t stencilRef;
		uint32_t fillMode;
		uint32_t cullMode;
		uint32_t depthBiasMode;
		uint32_t scissorMode;
		uint32_t alphaBlendMode;
		uint32_t alphaToCoverage;
		uint32_t alphaWriteMode;
		bool alphaTest;
		float alphaTestRef;

		static DepthDrawState Capture();
		void Apply() const;
	};
	struct DeferredDepthDraw
	{
		RE::BSRenderPass pass;
		uint32_t technique;
		bool alphaTest;
		uint32_t renderFlags;
		DepthDrawFunction draw;
		DepthDrawState state;
	};
	static_assert(sizeof(ProbeConstants) == 64);
	static_assert(sizeof(EyeDepth) == 8);

	void RestoreCamera();
	void ReadDepth(Clock::time_point now);
	bool CheckProjection(const float4& cameraData);
	void Fail(const char* reason);
	void WaitForCamera(const char* reason);
	void DrawValues();

	VRNearClipController controller;
	RE::NiPointer<RE::NiCamera> controlledCamera;
	std::array<float, 2> savedNear{};
	float savedBufferNear = 0.0f;
	float savedMinimum = 0.0f;
	float savedRatio = 0.0f;
	float appliedNear = 0.0f;
	float targetNear = 0.1f;
	std::array<float, 2> observedNear{};
	std::array<float, 2> cachedNear{};
	float observedEngineNear = 0.0f;
	std::array<float, 2> inputNear{};
	std::array<float, 2> inputFar{};
	float inputMinimumNear = 0.0f;
	float inputFarNearRatio = 0.0f;
	std::array<float, 2> nearest{ std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity() };
	float previousNearest = std::numeric_limits<float>::infinity();
	float approachSpeed = 0.0f;
	uint64_t nextSerial = 0;
	uint64_t acceptedSerial = 0;
	uint32_t updateFrame = UINT32_MAX;
	uint32_t captureFrame = UINT32_MAX;
	Clock::time_point lastUpdate{};
	Clock::time_point lastSample{};
	Clock::time_point lastLog{};
	Clock::time_point lastProjectionLog{};
	bool hookReady = false;
	bool resourcesReady = false;
	bool failed = false;
	bool recoveryProbe = false;
	const char* status = "Waiting for camera";
	winrt::com_ptr<ID3D11ComputeShader> probeShader;
	winrt::com_ptr<ID3D11Buffer> probeResult;
	winrt::com_ptr<ID3D11UnorderedAccessView> probeUAV;
	std::unique_ptr<ConstantBuffer> probeConstants;
	std::array<Readback, 3> readbacks;
	std::array<DeferredDepthDraw, 512> deferredDepthDraws{};
	uint32_t deferredDepthCount = 0;
	uint32_t excludedDepthCount = 0;
	bool collectingWorldDepth = false;
	bool excludedDepthOverflow = false;
};
