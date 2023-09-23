#pragma once

#include "Buffer.h"
#include "Feature.h"

struct ScreenSpaceGI : Feature
{
	static ScreenSpaceGI* GetSingleton()
	{
		static ScreenSpaceGI singleton;
		return &singleton;
	}

	virtual inline std::string GetName() { return "Screen Space GI"; }
	virtual inline std::string GetShortName() { return "ScreenSpaceGI"; }

	struct alignas(16) SSGICB
	{
		int32_t ViewportSize[2];
		Vector2 ViewportPixelSize;  // .zw == 1.0 / ViewportSize.xy

		Vector2 DepthUnpackConsts;
		Vector2 CameraTanHalfFOV;

		Vector2 NDCToViewMul;
		Vector2 NDCToViewAdd;

		Vector2 NDCToViewMul_x_PixelSize;

		uint32_t SliceCount;
		uint32_t StepsPerSlice;

		float EffectRadius;  // world (viewspace) maximum size of the shadow
		float EffectFalloffRange;

		float RadiusMultiplier;
		float FinalValuePower;
		float DenoiseBlurBeta;

		float SampleDistributionPower;
		float ThinOccluderCompensation;
		float DepthMIPSamplingOffset;
		int NoiseIndex;  // frameIndex % 64 if using TAA or 0 otherwise

		Vector3 Padding;
	};
	ConstantBuffer* ssgiCB = nullptr;

	bool normalSwap = false;
	bool hilbertLUTGenFlag = true;

	ID3D11ComputeShader* hilbertLutCompute = nullptr;
	ID3D11ComputeShader* prefilterDepthsCompute = nullptr;
	ID3D11ComputeShader* ssgiCompute = nullptr;
	ID3D11ComputeShader* denoiseCompute = nullptr;
	ID3D11ComputeShader* denoiseFinalCompute = nullptr;
	ID3D11ComputeShader* mixCompute = nullptr;

	Texture2D* texHilbertLUT = nullptr;
	Texture2D* texWorkingDepth = nullptr;
	ID3D11UnorderedAccessView* uavWorkingDepth[5] = { nullptr };
	Texture2D* texGI0 = nullptr;
	Texture2D* texGI1 = nullptr;
	Texture2D* texEdge = nullptr;
	Texture2D* texColor0 = nullptr;
	Texture2D* texColor1 = nullptr;

	ID3D11SamplerState* linearSampler = nullptr;
	ID3D11SamplerState* pointSampler = nullptr;

	void ClearComputeShader();
	virtual void SetupResources();
	virtual inline void Reset(){};

	struct Settings
	{
		bool Enabled = true;

		uint32_t SliceCount = 2;
		uint32_t StepsPerSlice = 2;

		uint32_t DenoisePasses = 0;

		// visual

		float EffectRadius = 100.f;
		float EffectFalloffRange = .615f;

		float FinalValuePower = 2.2f;

		float SampleDistributionPower = 2.f;
		float ThinOccluderCompensation = 0.f;
		float DepthMIPSamplingOffset = 3.3f;

		// denoise

	} settings;

	virtual void DrawSettings();

	virtual void Draw(const RE::BSShader* shader, const uint32_t descriptor);

	virtual void Load(json& o_json);
	virtual void Save(json& o_json);

	void DrawDeferred();
	void GenerateHilbertLUT();
	void UpdateBuffer();
};