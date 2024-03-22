#pragma once

#include "Buffer.h"

class Bindings
{
public:
	static Bindings* GetSingleton()
	{
		static Bindings singleton;
		return &singleton;
	}

	void DepthStencilStateSetDepthMode(RE::BSGraphics::DepthStencilDepthMode a_mode);

	void AlphaBlendStateSetMode(uint32_t a_mode);
	void AlphaBlendStateSetAlphaToCoverage(uint32_t a_value);
	void AlphaBlendStateSetWriteMode(uint32_t a_value);

	void SetupResources();
	void Reset();

	void StartDeferred();
	void DeferredPasses();
	void EndDeferred();

	ID3D11BlendState* deferredBlendStates[4];
	ID3D11BlendState* forwardBlendStates[4];
	RE::RENDER_TARGET forwardRenderTargets[4];

	ID3D11ComputeShader* deferredCompositeCS = nullptr;

	void ClearShaderCache();
	ID3D11ComputeShader* GetComputeDeferredComposite();

	bool inWorld = false;
	bool deferredPass = false;

	struct alignas(16) DeferredCB
	{
		float4 DirLightDirectionVS[2];
		float4 DirLightColor;
		float4 CameraData;
		float2 BufferDim;
		float2 RcpBufferDim;
		DirectX::XMFLOAT4X4 ViewMatrix[2];
		DirectX::XMFLOAT4X4 ProjMatrix[2];
		DirectX::XMFLOAT4X4 ViewProjMatrix[2];
		DirectX::XMFLOAT4X4 InvViewMatrix[2];
		DirectX::XMFLOAT4X4 InvProjMatrix[2];
		DirectX::XMFLOAT3X4 DirectionalAmbient;
		uint FrameCount;
		uint pad0[3];
	};

	ConstantBuffer* deferredCB = nullptr;

	void UpdateConstantBuffer();

	struct Hooks
	{
		struct Main_RenderWorld
		{
			static void thunk(bool a1)
			{
				GetSingleton()->inWorld = true;
				func(a1);
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct Main_RenderWorld_Start
		{
			static void thunk(RE::BSBatchRenderer* This, uint32_t StartRange, uint32_t EndRanges, uint32_t RenderFlags, int GeometryGroup)
			{
				// Here is where the first opaque objects start rendering
				GetSingleton()->StartDeferred();		
				func(This, StartRange, EndRanges, RenderFlags, GeometryGroup);  // RenderBatches
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct Main_RenderWorld_End
		{
			static void thunk(RE::BSShaderAccumulator* This, uint32_t RenderFlags)
			{
				func(This, RenderFlags);	
				// After this point, water starts rendering
				GetSingleton()->EndDeferred();
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install()
		{
			stl::write_thunk_call<Main_RenderWorld>(REL::RelocationID(35560, 36559).address() + REL::Relocate(0x831, 0x841));

			stl::write_thunk_call<Main_RenderWorld_Start>(REL::RelocationID(99938, 106583).address() + REL::Relocate(0x8E, 0x84));
			stl::write_thunk_call<Main_RenderWorld_End>(REL::RelocationID(99938, 106583).address() + REL::Relocate(0x319, 0x308));

			logger::info("[Bindings] Installed hooks");
		}
	};
};