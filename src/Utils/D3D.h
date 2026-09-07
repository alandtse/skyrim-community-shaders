#pragma once
#include <array>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <winrt/base.h>

namespace Util
{
	/**
	 * @brief ID3DInclude handler resolving #include paths under Data\Shaders,
	 *        shared by every HLSL compile call site in this codebase.
	 */
	struct CustomInclude : public ID3DInclude
	{
		HRESULT Open([[maybe_unused]] D3D_INCLUDE_TYPE IncludeType, LPCSTR pFileName, [[maybe_unused]] LPCVOID pParentData, LPCVOID* ppData, UINT* pBytes) override
		{
			std::filesystem::path filePath = pFileName;
			filePath = L"Data\\Shaders" / filePath;

			std::ifstream file(filePath, std::ios::binary);
			if (!file.is_open()) {
				*ppData = NULL;
				*pBytes = 0;
				return E_FAIL;
			}

			file.seekg(0, std::ios::end);
			UINT size = static_cast<UINT>(file.tellg());
			file.seekg(0, std::ios::beg);

			char* data = new char[size];
			if (!file.read(data, size)) {
				delete[] data;
				*ppData = NULL;
				*pBytes = 0;
				return E_FAIL;
			}
			*ppData = data;
			*pBytes = size;
			return S_OK;
		}

		HRESULT Close(LPCVOID pData) override
		{
			delete[] static_cast<const char*>(pData);
			return S_OK;
		}
	};
	/**
	 * @brief Look up the matching SRV for a given render target view.
	 * @param a_rtv The render target view to look up.
	 * @return The corresponding shader resource view, or nullptr if not found.
	 */
	ID3D11ShaderResourceView* GetSRVFromRTV(ID3D11RenderTargetView* a_rtv);

	/**
	 * @brief Look up the matching RTV for a given shader resource view.
	 * @param a_srv The shader resource view to look up.
	 * @return The corresponding render target view, or nullptr if not found.
	 */
	ID3D11RenderTargetView* GetRTVFromSRV(ID3D11ShaderResourceView* a_srv);

	/**
	 * @brief Get the Skyrim render target name associated with an SRV.
	 * @param a_srv The shader resource view to identify.
	 * @return The render target enum name, or "NONE" if not found.
	 */
	std::string GetNameFromSRV(ID3D11ShaderResourceView* a_srv);

	/**
	 * @brief Get the Skyrim render target name associated with an RTV.
	 * @param a_rtv The render target view to identify.
	 * @return The render target enum name, or "NONE" if not found.
	 */
	std::string GetNameFromRTV(ID3D11RenderTargetView* a_rtv);

	/**
	 * @brief Set a debug name on a D3D11 resource for RenderDoc debuggability.
	 * @param Resource The D3D11 device child to name.
	 * @param Format A printf-style format string for the name.
	 */
	void SetResourceName(ID3D11DeviceChild* Resource, const char* Format, ...);

	/**
	 * @brief Reads the Texture2D description backing an SRV/UAV/RTV/DSV.
	 * @param View Any D3D11 view (ID3D11View is the common base for all four).
	 * @param OutDesc Filled with the underlying texture's description on success.
	 * @return true if the view's resource is a Texture2D and OutDesc was filled.
	 */
	bool GetTexture2DDesc(ID3D11View* View, D3D11_TEXTURE2D_DESC& OutDesc);

	/**
	 * @brief Compile an HLSL shader from file and create the appropriate D3D11 shader object.
	 * @param FilePath Path to the HLSL source file.
	 * @param Defines Preprocessor macro name/value pairs to pass to the compiler.
	 * @param ProgramType Shader model target (e.g. "ps_5_0", "vs_5_0", "cs_5_0").
	 * @param Program Entry point function name (defaults to "main").
	 * @return The compiled shader object, or nullptr on failure.
	 */
	ID3D11DeviceChild* CompileShader(const wchar_t* FilePath, const std::vector<std::pair<const char*, const char*>>& Defines, const char* ProgramType, const char* Program = "main");

	/**
	 * @brief Log a D3DCompile error/warning blob verbatim, if present.
	 *
	 * Every call site that invokes D3DCompile/D3DCompileFromFile directly (i.e. cannot
	 * go through CompileShader) should route its errorBlob here instead of hand-rolling
	 * its own filter, so shader-validation log capture sees the full, unfiltered text.
	 * @param ErrorBlob The blob returned by D3DCompile in its error-output parameter.
	 * @param Context A short identifier (e.g. file name) prefixed to the log line.
	 */
	void LogShaderCompileWarnings(ID3DBlob* ErrorBlob, const std::string& Context);

	/**
	 * @brief Apply an alpha-blended highlight tint to a texture via CPU staging copy.
	 * @param texture The texture to tint.
	 * @param isHighlighted When false the function is a no-op.
	 * @param highlightColor RGBA colour to blend, each component in [0, 1].
	 */
	void ApplyHighlightTintToTexture(ID3D11Texture2D* texture, bool isHighlighted, const std::array<float, 4>& highlightColor = { 1.0f, 0.5f, 0.0f, 0.3f });

	/**
	 * @brief Create an RGBA overlay texture and its render target view.
	 * @param device The D3D11 device to use.
	 * @param width Texture width in pixels.
	 * @param height Texture height in pixels.
	 * @param outTex Receives the created texture.
	 * @param outRTV Receives the created render target view.
	 * @return S_OK on success, or an error HRESULT.
	 */
	HRESULT CreateOverlayTextureAndRTV(ID3D11Device* device, int width, int height, ID3D11Texture2D** outTex, ID3D11RenderTargetView** outRTV);

	/**
	 * @brief Release and null each listed shader com_ptr in one call.
	 *
	 * Prefer this over hand-rolling `for (auto ptr : ptrs) ...`: a loop over raw
	 * pointers-to-com_ptr silently no-ops if a call site forgets to dereference
	 * (it nulls the loop variable, not the shader). Passing references here
	 * makes that mistake a compile error instead.
	 * @param shaders Shader com_ptr members to clear, e.g. `Util::ClearShaders<ID3D11ComputeShader>({ myCS, otherCS })`.
	 */
	template <typename T>
	void ClearShaders(std::initializer_list<std::reference_wrapper<winrt::com_ptr<T>>> shaders)
	{
		for (auto& shader : shaders)
			shader.get() = nullptr;
	}

	// VR-aware counts for render targets
	inline int GetRenderTargetCount()
	{
		return globals::game::isVR ? RE::RENDER_TARGETS::kVRTOTAL : RE::RENDER_TARGETS::kTOTAL;
	}

	inline int GetDepthStencilCount()
	{
		return globals::game::isVR ? RE::RENDER_TARGETS_DEPTHSTENCIL::kVRTOTAL : RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL;
	}

	/**
	 * @brief Save a GPU texture to a DDS file on disk.
	 * @param device The D3D11 device.
	 * @param context The immediate device context used for staging.
	 * @param path Destination file path (parent directories are created if needed).
	 * @param tex The texture to save.
	 * @return S_OK on success, or an error HRESULT.
	 */
	HRESULT SaveTextureToFile(ID3D11Device* device, ID3D11DeviceContext* context, const std::filesystem::path& path, ID3D11Texture2D* tex);

	/**
	 * @brief Load a DDS texture from disk and create a texture with its SRV.
	 * @param device The D3D11 device.
	 * @param path Path to the DDS file.
	 * @param outTex Receives the loaded texture.
	 * @param outSRV Receives the shader resource view for the texture.
	 * @return S_OK on success, or an error HRESULT.
	 */
	HRESULT LoadTextureFromFile(ID3D11Device* device, const std::filesystem::path& path, ID3D11Texture2D** outTex, ID3D11ShaderResourceView** outSRV);

	/**
	 * @brief Get the current scene depth SRV, preferring terrain-blended depth when active.
	 *
	 * The caller does NOT own the returned pointer.
	 *
	 * @param prefer16bit When false (default) returns R32_FLOAT for compute shaders doing
	 *        arithmetic on depth; when true returns R16_UNORM for pixel shaders via
	 *        slot 17 / SharedData::GetDepth.
	 * @return The depth SRV, or nullptr if unavailable.
	 */
	ID3D11ShaderResourceView* GetCurrentSceneDepthSRV(bool prefer16bit = false);

	/**
	 * @brief RAII guard that snapshots the D3D11 pipeline state a fullscreen
	 * draw pass clobbers and restores+Releases it on scope exit.
	 *
	 * Covers OM (RTV/DSV, blend, depth-stencil), RS (state, viewports),
	 * the VS/PS/GS/HS/DS shaders, IA (input layout, vertex/index buffers,
	 * topology), PS sampler/SRV slot 0, and PS constant-buffer slot 1. The
	 * destructor nulls PS SRV slot 0 before restoring to break any SRV-vs-RTV
	 * hazard left by the wrapped pass. Construct it, set up + issue the pass,
	 * then let it go out of scope.
	 */
	struct FullscreenPassScope
	{
		explicit FullscreenPassScope(ID3D11DeviceContext* a_context);
		~FullscreenPassScope();
		FullscreenPassScope(const FullscreenPassScope&) = delete;
		FullscreenPassScope& operator=(const FullscreenPassScope&) = delete;

	private:
		ID3D11DeviceContext* ctx = nullptr;
		ID3D11RenderTargetView* savedRTV[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
		ID3D11DepthStencilView* savedDSV = nullptr;
		UINT numVP = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		D3D11_VIEWPORT savedVP[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
		ID3D11BlendState* savedBlend = nullptr;
		FLOAT savedBlendFactor[4] = {};
		UINT savedSampleMask = 0;
		ID3D11DepthStencilState* savedDSState = nullptr;
		UINT savedStencilRef = 0;
		ID3D11VertexShader* savedVS = nullptr;
		ID3D11PixelShader* savedPS = nullptr;
		ID3D11GeometryShader* savedGS = nullptr;
		ID3D11HullShader* savedHS = nullptr;
		ID3D11DomainShader* savedDS = nullptr;
		ID3D11RasterizerState* savedRS = nullptr;
		ID3D11SamplerState* savedSampler0 = nullptr;
		ID3D11ShaderResourceView* savedSRV0 = nullptr;
		ID3D11Buffer* savedPSCB1 = nullptr;
		ID3D11InputLayout* savedIL = nullptr;
		ID3D11Buffer* savedVB[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
		UINT savedVBStride[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
		UINT savedVBOffset[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
		ID3D11Buffer* savedIB = nullptr;
		DXGI_FORMAT savedIBFormat = DXGI_FORMAT_UNKNOWN;
		UINT savedIBOffset = 0;
		D3D11_PRIMITIVE_TOPOLOGY savedTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
	};
}  // namespace Util
