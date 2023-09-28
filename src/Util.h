#pragma once

namespace Util
{
	void StoreTransform3x4NoScale(DirectX::XMFLOAT3X4& Dest, const RE::NiTransform& Source);
	ID3D11ShaderResourceView* GetSRVFromRTV(ID3D11RenderTargetView* a_rtv);
	ID3D11UnorderedAccessView* GetUAVFromRTV(ID3D11RenderTargetView* a_rtv);
	ID3D11RenderTargetView* GetRTVFromSRV(ID3D11ShaderResourceView* a_srv);
	ID3D11UnorderedAccessView* GetUAVFromSRV(ID3D11ShaderResourceView* a_srv);
	std::string GetNameFromSRV(ID3D11ShaderResourceView* a_srv);
	std::string GetNameFromRTV(ID3D11RenderTargetView* a_rtv);
	void SetResourceName(ID3D11DeviceChild* Resource, const char* Format, ...);
	ID3D11DeviceChild* CompileShader(const wchar_t* FilePath, const std::vector<std::pair<const char*, const char*>>& Defines, const char* ProgramType, const char* Program = "main");
	std::string DefinesToString(std::vector<std::pair<const char*, const char*>>& defines);
	std::string DefinesToString(std::vector<D3D_SHADER_MACRO>& defines);
}

// json io helpers
namespace nlohmann
{
	void to_json(json& j, const Vector3& v);
	void from_json(const json& j, Vector3& v);
	void to_json(json& j, const Vector2& v);
	void from_json(const json& j, Vector2& v);
}