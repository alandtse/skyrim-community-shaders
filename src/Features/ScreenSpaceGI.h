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
		float DenoiseBlurBeta;

		float SampleDistributionPower;
		float ThinOccluderCompensation;
		float DepthMIPSamplingOffset;
		int NoiseIndex;  // frameIndex % 64 if using TAA or 0 otherwise

		// bitmask
		float Thickness;

		// gi
		uint32_t EnableGI;
		uint32_t CheckBackface;
		float BackfaceStrength;
		float GIBounceFade;
		float GIDistanceCompensation;

		// mix
		Vector2 AOClamp;
		float AOPower;
		Vector2 AORemap;

		float GIStrength;

		// debug
		uint32_t DebugView;

		float GICompensationMaxDist;  // idk why
		float AmbientSource;
		float DirectLightAO;
	};
	ConstantBuffer* ssgiCB = nullptr;

	bool normalSwap = false;
	bool hilbertLUTGenFlag = true;

	ID3D11ComputeShader* hilbertLutCompute = nullptr;
	ID3D11ComputeShader* prefilterDepthsCompute = nullptr;
	ID3D11ComputeShader* fetchRadianceCompute = nullptr;
	ID3D11ComputeShader* ssgiCompute = nullptr;
	ID3D11ComputeShader* ssgiBitmaskCompute = nullptr;
	ID3D11ComputeShader* denoiseCompute = nullptr;
	ID3D11ComputeShader* denoiseFinalCompute = nullptr;
	ID3D11ComputeShader* mixCompute = nullptr;

	Texture2D* texHilbertLUT = nullptr;
	Texture2D* texWorkingDepth = nullptr;
	ID3D11UnorderedAccessView* uavWorkingDepth[5] = { nullptr };
	Texture2D* texGI0 = nullptr;
	Texture2D* texGI1 = nullptr;
	Texture2D* texEdge = nullptr;
	Texture2D* texRadiance = nullptr;
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
		bool EnableGI = true;
		bool UseBitmask = true;

		bool CheckBackface = true;

		uint32_t SliceCount = 2;
		uint32_t StepsPerSlice = 6;

		// visual
		float EffectRadius = 400.f;
		float EffectFalloffRange = .615f;

		float SampleDistributionPower = 2.f;
		float ThinOccluderCompensation = 0.f;
		float DepthMIPSamplingOffset = 3.3f;

		float Thickness = 50.f;

		// gi
		float AmbientSource = 0.2f;
		float BackfaceStrength = 0.1f;
		float GIBounceFade = 0.5f;
		float GIDistanceCompensation = 2;
		float GICompensationMaxDist = 300;

		// mix
		Vector2 AOClamp = { 0.03, 1 };
		float AOPower = 2.2f;
		Vector2 AORemap = { 0.03, 1 };
		float DirectLightAO = 0.1f;
		float GIStrength = 1;

		// denoise
		uint32_t DenoisePasses = 0;

		// debug
		uint32_t DebugView = 0;

	} settings;

	virtual void DrawSettings();

	virtual void Draw(const RE::BSShader* shader, const uint32_t descriptor);

	virtual void Load(json& o_json);
	virtual void Save(json& o_json);

	void DrawDeferred();
	void GenerateHilbertLUT();
	void UpdateBuffer();
};