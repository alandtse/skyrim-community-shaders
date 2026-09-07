#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace NeuralRendering
{
	struct Tuning
	{
		float intensity = 1.70f;
		float localToneStrength = 1.70f;
		float localStructureStrength = 1.70f;
		float skinStructureStrength = -1.0f;
		std::uint32_t style = 0;
		bool useAutoMask = true;
		bool uiCorrection = false;
		std::uint32_t modelResolutionPercent = 100;
		// 0 = current bounded full-resolution resolve; 1 = exact-area input plus
		// conservative matched-residual composition for reduced model resolutions.
		std::uint32_t modelResolveMode = 0;
		// Experimental screenshot/benchmark cascade: 0 = single pass, 1 = 2x,
		// 2 = 3x sequential Feature 18 evaluations. Each stage has separate
		// resources/history; the renderer gates this away from cropped VR paths.
		std::uint32_t multiPass = 0;
	};

	enum class RuntimeStatus
	{
		NotProbed,
		NotFound,
		VersionUnavailable,
		UnsupportedVersion,
		LoadFailed,
		MissingExport,
		Ready,
		InitializationFailed,
		CoreUnavailable,
		ParameterAllocationFailed,
		Initialized,
	};

	class Runtime
	{
	public:
		static Runtime& Instance();
		~Runtime();
		Runtime(const Runtime&) = delete;
		Runtime& operator=(const Runtime&) = delete;

		bool Probe(const std::filesystem::path& explicitPath = {});
		bool Initialize(ID3D12Device* device, const std::filesystem::path& dataPath = {});
		bool Execute(ID3D12GraphicsCommandList* commandList, std::uint32_t slot,
			ID3D12Resource* color, ID3D12Resource* depth, ID3D12Resource* motionVectors, ID3D12Resource* output,
			std::uint32_t inputWidth, std::uint32_t inputHeight, std::uint32_t outputWidth, std::uint32_t outputHeight,
			std::uint32_t guideWidth, std::uint32_t guideHeight,
			float motionVectorScaleX, float motionVectorScaleY, const Tuning& tuning, bool reset);
		void ResetFeature(std::uint32_t slot);
		void ResetFeatures();
		void Shutdown();

		[[nodiscard]] RuntimeStatus Status() const { return status_; }
		[[nodiscard]] const std::string& Version() const { return version_; }
		[[nodiscard]] const std::string& Detail() const { return detail_; }
		[[nodiscard]] std::uint32_t NgxResult() const { return ngxResult_; }
		[[nodiscard]] std::uint32_t ApplicationId() const { return applicationId_; }
		[[nodiscard]] std::uint32_t ApiVersion() const { return apiVersion_; }
		[[nodiscard]] std::uint64_t SuccessfulFrames() const { return successfulFrames_; }

	private:
		static constexpr std::uint32_t kFeatureSlotCount = 6;  // two eyes x three cascade stages
		Runtime() = default;
		void* module_ = nullptr;
		void* parameters_ = nullptr;
		void* featureHandles_[kFeatureSlotCount]{};
		std::uint32_t featureInputWidth_[kFeatureSlotCount]{};
		std::uint32_t featureInputHeight_[kFeatureSlotCount]{};
		std::uint32_t featureOutputWidth_[kFeatureSlotCount]{};
		std::uint32_t featureOutputHeight_[kFeatureSlotCount]{};
		ID3D12Device* device_ = nullptr;
		RuntimeStatus status_ = RuntimeStatus::NotProbed;
		std::filesystem::path path_;
		std::string version_;
		std::string detail_;
		std::uint32_t ngxResult_ = 0;
		std::uint32_t applicationId_ = 0;
		std::uint32_t apiVersion_ = 0;
		std::uint64_t successfulFrames_ = 0;
	};

	const char* ToString(RuntimeStatus status);
}
