#pragma once

#include "Buffer.h"
#include "Utils/LazyShader.h"

#include <atomic>
#include <chrono>
#include <d3d11_1.h>
#include <functional>
#include <memory>
#include <mutex>
#include <openvr.h>
#include <sl_consts.h>
#include <string>
#include <string_view>

/** @brief Owns the boot-latched VR resolution plan and final eye reconstruction. */
class VRSubmitUpscaling
{
public:
	/** @brief Installs compositor interception before reducing engine allocations. */
	void InstallRenderTargetSizeHook();
	/** @brief Invalidates captured frames and recreates resources on the render thread. */
	void SetupResources();
	/** @brief Invalidates standalone shaders and temporal inputs. */
	void ClearShaderCache();
	/** @brief Captures world depth and motion before the engine post chain. */
	void CaptureInputs();
	/** @brief Reconstructs the menu frame from the engine's post-processing output. */
	void ReconstructMenuBackground(uint32_t postProcessingTarget);
	/** @brief True when engine allocations use the immutable render-size plan. */
	bool IsHookActive() const { return active; }
	/** @brief True while a submit-stage vendor dispatch is executing. */
	bool IsDispatching() const { return dispatching; }
	/** @brief Whether the current submit dispatch must discard temporal history. */
	bool ShouldResetHistory() const { return dispatching && resetHistory; }
	/** @brief World-camera parameters retained alongside the captured depth and motion. */
	struct TemporalParameters
	{
		float2 jitter{};
		float cameraNear = 0, cameraFar = 0, verticalFov = 0, frameTime = 0;
	};
	/** @brief Returns captured parameters only during this component's vendor dispatch. */
	const TemporalParameters* GetDispatchParameters() const { return dispatching ? &temporal : nullptr; }
	/** @brief Whether reconstruction can consume projection jitter this frame. */
	bool CanJitter() const;
	/** @brief Whether the active VR frame uses menu projection and presentation handling. */
	bool IsMenuFrame() const;
	/** @brief Whether the current menu frame lacks captured inputs for background reconstruction. */
	bool ShouldApplyMenuTAA() const;
	/** @brief Returns the latest presentation result and successful stereo-pair count. */
	std::string GetStatus() const;
	/** @brief Returns the latched output width per eye. */
	uint32_t GetDisplayEyeWidth() const { return plan.outputWidth; }
	/** @brief Returns the latched output height per eye. */
	uint32_t GetDisplayEyeHeight() const { return plan.outputHeight; }
	/** @brief Returns the latched render width per eye. */
	uint32_t GetRenderEyeWidth() const { return plan.renderWidth; }
	/** @brief Returns the latched render height per eye. */
	uint32_t GetRenderEyeHeight() const { return plan.renderHeight; }
	/** @brief Returns the effective quality preset for the allocated dimensions. */
	uint32_t GetLatchedQualityMode() const { return plan.qualityMode; }
	/** @brief Returns the backend selected when render targets were allocated. */
	uint32_t GetLatchedMethod() const { return plan.method; }
	/** @brief True if explicit scale owns the allocation rather than the preset. */
	bool IsExplicitScaleLatched() const { return plan.explicitScale; }
	/** @brief Returns output dimensions in the engine's stereo coordinate space. */
	float2 GetDisplayScreenSize() const { return { float(plan.outputWidth * 2), float(plan.outputHeight) }; }

private:
	struct ResolutionPlan
	{
		uint32_t renderWidth = 0, renderHeight = 0;
		uint32_t outputWidth = 0, outputHeight = 0;
		uint32_t qualityMode = 0, method = 0;
		bool explicitScale = false;
	} plan;

	struct EyeResources
	{
		std::unique_ptr<Texture2D> color, output, sharpened, submit;
		std::unique_ptr<Texture2D> depth, motion, reactive, transparency;
	};
	EyeResources eyes[2];
	struct FoveatedEye
	{
		sl::Extent input{}, output{};
		EyeResources crop;
		std::unique_ptr<Texture2D> history[2];
		uint32_t historyIndex = 0;
		bool historyValid = false;
	};
	FoveatedEye foveatedEyes[2];
	bool foveatedPair = false, foveationUnavailable = false;
	Util::LazyShader<ID3D11ComputeShader> peripheryShader;
	std::unique_ptr<ConstantBuffer> peripheryBuffer;
	winrt::com_ptr<ID3D11SamplerState> peripherySampler;
	std::unique_ptr<ConstantBuffer> encodeBuffer, colorBuffer;
	winrt::com_ptr<ID3D11DeviceContext1> context;
	winrt::com_ptr<ID3DDeviceContextState> isolatedState;
	winrt::com_ptr<ID3D11Texture2D> sourceCopy, submittedSource;
	winrt::com_ptr<ID3D11ShaderResourceView> sourceView;
	Util::LazyShader<ID3D11ComputeShader> encodeShaders[2], colorShader;
	std::mutex mutex;
	mutable std::mutex statusMutex;
	std::string status = "Waiting for world inputs";
	std::atomic<uint64_t> reconstructedPairs = 0;
	std::atomic_bool failed = false;
	std::atomic<uint64_t> cycle = 0;
	std::atomic<DWORD> renderThread = 0;
	std::atomic_bool active = false;
	bool captured = false, attempted = false, pairReady = false;
	bool dispatching = false, resetHistory = true;
	uint32_t capturedMethod = 0;
	uint64_t captureCycle = 0, lastSuccessCycle = UINT64_MAX;
	float2 capturedJitter{};
	TemporalParameters temporal;
	sl::Constants cameraConstants[2];
	vr::EColorSpace sourceColorSpace = vr::ColorSpace_Auto;
	std::atomic_bool shaderResetPending = false;
	std::unique_ptr<Texture2D> menuColor, menuDepth;
	std::unique_ptr<Texture2D> menuReference, menuResolved, menuMismatch, menuScene;
	std::unique_ptr<Texture2D> menuSceneReference, menuSceneMismatch, menuUILayer;
	winrt::com_ptr<ID3D11BlendState> menuLayerBlend[2];
	uint64_t menuUILayerCycle = UINT64_MAX;
	uint32_t menuLayerDraws = 0, menuLayerRejected = 0;
	std::string menuLayerRejection;
	bool CaptureMenuDraw(ID3D11DeviceContext* drawContext, const std::function<void()>& draw);
	Util::LazyShader<ID3D11ComputeShader> menuValidateCS, menuResolveCS;
	Util::LazyShader<ID3D11VertexShader> menuCopyVS;
	Util::LazyShader<ID3D11PixelShader> menuCopyPS;
	winrt::com_ptr<ID3D11SamplerState> menuSampler;
	uint64_t menuCycle = UINT64_MAX;
	uint64_t menuResolvedCycle = UINT64_MAX;
	ID3D11Texture2D* menuResolvedSource = nullptr;
	bool menuReady = false;
	std::atomic_bool menuDrawing = false;
	bool menuUnavailable = false;
	bool menuSceneUnavailable = false, capturedMenuScene = false, lastSuccessMenuScene = false;
	bool menuSceneReconstructed = false;
	uint64_t menuSceneCycle = UINT64_MAX;
	std::string menuSceneFallback = "waiting for world inputs";
	bool capturedMapScene = false, lastSuccessMapScene = false;
	std::chrono::steady_clock::time_point lastCaptureTime{};
	std::chrono::steady_clock::time_point menuDiagnosticTimes[4]{};
	void LogMenuDiagnostics(uint32_t stage, uint32_t postProcessingTarget = UINT32_MAX);

	bool WantsNativeMenuUI() const;
	void SetupMenuResources();
	void CopyMenuColor(ID3D11ShaderResourceView* source, ID3D11RenderTargetView* destination, uint32_t width, uint32_t height);
	bool DrawNativeMenuUI(RE::BSGraphics::BSShaderAccumulator* accumulator, uint32_t flags);
	bool ResolveNativeMenu(ID3D11Texture2D* source);
	bool CanCaptureMenuScene();
	bool ReconstructMenuScene(ID3D11Texture2D* source);

	void Invalidate();
	void SetStatus(std::string_view message);
	void Fail(std::string_view reason);
	bool EnsureResources();
	static std::unique_ptr<Texture2D> MakeTexture(uint32_t width, uint32_t height, DXGI_FORMAT format, const std::string& name);
	void ClearFoveationResources();
	bool PrepareFoveation();
	ID3D11ShaderResourceView* SmoothPeriphery(uint32_t eye);
	sl::Constants GetEyeConstants(uint32_t eye) const;
	void ComposeFoveatedEye(uint32_t eye);
	bool ReconstructPair(ID3D11Texture2D* source, vr::EColorSpace colorSpace);
	bool ValidateSource(ID3D11Texture2D* source, vr::EVREye eye, const vr::VRTextureBounds_t* bounds) const;
	void ConvertColor(ID3D11ShaderResourceView* source, ID3D11UnorderedAccessView* output,
		uint32_t width, uint32_t height, uint32_t offset, uint32_t conversion);

	struct RenderTargetSizeHook
	{
		static void thunk(RE::BSOpenVR* self, uint32_t* width, uint32_t* height);
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct MenuUIHook
	{
		static void thunk(RE::BSGraphics::BSShaderAccumulator* accumulator, uint32_t flags);
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct MenuDrawIndexed
	{
		static void thunk(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base);
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct MenuDraw
	{
		static void thunk(ID3D11DeviceContext* ctx, UINT count, UINT start);
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct MenuDrawIndexedInstanced
	{
		static void thunk(ID3D11DeviceContext* ctx, UINT count, UINT instances, UINT start, INT base, UINT firstInstance);
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct MenuDrawInstanced
	{
		static void thunk(ID3D11DeviceContext* ctx, UINT count, UINT instances, UINT start, UINT firstInstance);
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct MenuViewportHook
	{
		static void thunk(RE::BSGraphics::Renderer* renderer, uint32_t width, uint32_t height, bool matchTarget);
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct SubmitHook
	{
		static vr::EVRCompositorError thunk(vr::IVRCompositor* self, vr::EVREye eye,
			const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds, vr::EVRSubmitFlags flags);
		static inline decltype(&thunk) func = nullptr;
	};
	struct WaitGetPosesHook
	{
		static vr::EVRCompositorError thunk(vr::IVRCompositor* self, vr::TrackedDevicePose_t* renderPoses,
			uint32_t renderCount, vr::TrackedDevicePose_t* gamePoses, uint32_t gameCount);
		static inline decltype(&thunk) func = nullptr;
	};
};
