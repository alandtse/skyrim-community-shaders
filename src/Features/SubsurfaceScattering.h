#pragma once

#include "Buffer.h"
#include "Feature.h"

struct SubsurfaceScattering : Feature
{
public:
	static SubsurfaceScattering* GetSingleton()
	{
		static SubsurfaceScattering singleton;
		return &singleton;
	}

	struct alignas(16) BlurCB
	{
		float4 DynamicRes;
		float4 CameraData;
		float2 BufferDim;
		float2 RcpBufferDim;
		uint FrameCount;
		float SSSS_FOVY;
		float SSSWidth;
		float pad;
	};

	ConstantBuffer* blurCB = nullptr;

	Texture2D* deferredTexture = nullptr;
	Texture2D* specularTexture = nullptr;

	Texture2D* colorTextureTemp = nullptr;
	Texture2D* colorTextureTemp2 = nullptr;
	Texture2D* depthTextureTemp = nullptr;

	ID3D11ComputeShader* horizontalSSBlur = nullptr;
	ID3D11ComputeShader* verticalSSBlur = nullptr;

	ID3D11SamplerState* linearSampler = nullptr;
	ID3D11SamplerState* pointSampler = nullptr;

	std::set<ID3D11BlendState*> mappedBlendStates;
	std::map<ID3D11BlendState*, ID3D11BlendState*> modifiedBlendStates;

	virtual inline std::string GetName() { return "Subsurface Scattering"; }
	virtual inline std::string GetShortName() { return "SubsurfaceScattering"; }

	virtual void SetupResources();
	virtual inline void Reset() {}

	virtual void DataLoaded();
	virtual void DrawSettings();

	virtual void Draw(const RE::BSShader* shader, const uint32_t descriptor);

	virtual void Load(json& o_json);
	virtual void Save(json& o_json);

	void DrawDeferred();

	void ClearComputeShader();
	ID3D11ComputeShader* GetComputeShaderHorizontalBlur();
	ID3D11ComputeShader* GetComputeShaderVerticalBlur();

};
