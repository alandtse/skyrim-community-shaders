#include "ExponentialHeightFog/VolumetricFogCSCommon.hlsli"

RWTexture3D<float4> VBufferA : register(u0);

[numthreads(8, 8, 4)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	if (!ExponentialHeightFog::IsInsideVolumetricGrid(dispatchID))
		return;

	uint eyeIndex;
	float viewDepth;
	float3 positionWS = ExponentialHeightFog::ComputeCellWorldPosition(dispatchID, 0.5f.xxx, eyeIndex, viewDepth);

	uint boundaryEye;
	float boundaryDepth;
	float3 frontPositionWS = ExponentialHeightFog::ComputeCellWorldPosition(dispatchID, float3(0.5f, 0.5f, 0.0f), boundaryEye, boundaryDepth);
	float3 backPositionWS = ExponentialHeightFog::ComputeCellWorldPosition(dispatchID, float3(0.5f, 0.5f, 1.0f), boundaryEye, boundaryDepth);
	float nearDistance = dispatchID.z == 0u && ExponentialHeightFog::GetVolumetricStartDistance() == 0.0f ? 0.0f : length(frontPositionWS);
	float extinction = ExponentialHeightFog::EvaluateFogExtinctionSegment(
		nearDistance, length(backPositionWS), positionWS, FrameBuffer::CameraPosAdjust[eyeIndex].xyz);
	float3 scattering = extinction * saturate(SharedData::exponentialHeightFogSettings.volumetricFogAlbedo.rgb) *
	                    SharedData::exponentialHeightFogSettings.volumetricFogAlbedo.a;

	VBufferA[dispatchID] = float4(scattering, extinction);
}
