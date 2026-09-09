#ifndef __RAIN_LIGHTING_HLSLI__
#define __RAIN_LIGHTING_HLSLI__

#if defined(RAIN_LOCAL_LIGHTS) && defined(COMPUTESHADER)
#	include "Common/Color.hlsli"
namespace LightLimitFix
{
#	include "LightLimitFix/Common.hlsli"
}
#	ifdef RAIN_INVERSE_SQUARE
#		include "InverseSquareLighting/InverseSquareLighting.hlsli"
#	endif
StructuredBuffer<LightLimitFix::Light> RainLights : register(t35);
StructuredBuffer<uint> RainLightIndices : register(t36);
StructuredBuffer<LightLimitFix::LightGrid> RainLightCells : register(t37);
#endif

namespace RainLighting
{
	/** @brief Incident light and a shared, intensity-weighted direction for the water highlight. */
	struct Sample
	{
		float3 Irradiance;
		float3 Direction;
		float Scattering;
	};

	/** @brief Evaluates incident local lighting at one world-space sample shared by both eyes. */
	Sample Evaluate(float3 worldPosition, float3 headPosition, float headDistance)
	{
		Sample result = (Sample)0;
#if defined(RAIN_LOCAL_LIGHTS) && defined(COMPUTESHADER)
		[branch] if (LocalLighting.x <= 0.0f || RainLightGrid.w == 0u || any(RainLightGrid.xyz == 0u) || headDistance >= LocalLighting.y) return result;
		uint eyeIndex = 0u;
		float3 relativePosition = worldPosition - FrameBuffer::CameraPosAdjust[0].xyz;
		float3 viewPosition = FrameBuffer::WorldToView(relativePosition, true, 0);
		float2 uv = FrameBuffer::ViewToUV(viewPosition, true, 0);
#	ifdef VR
		if (viewPosition.z <= 0.0f || any(uv < 0.0f) || any(uv >= 1.0f)) {
			eyeIndex = 1u;
			relativePosition = worldPosition - FrameBuffer::CameraPosAdjust[1].xyz;
			viewPosition = FrameBuffer::WorldToView(relativePosition, true, 1);
			uv = FrameBuffer::ViewToUV(viewPosition, true, 1);
		}
#	endif
		if (viewPosition.z < LocalLighting.z || viewPosition.z >= LocalLighting.w || any(uv < 0.0f) || any(uv >= 1.0f))
			return result;
		uint clusterZ = uint(log(viewPosition.z / LocalLighting.z) * RainLightGrid.z / log(LocalLighting.w / LocalLighting.z));
		uint3 cell = uint3(uint2(uv * RainLightGrid.xy), clusterZ);
		if (any(cell >= RainLightGrid.xyz))
			return result;
		uint cellIndex = cell.x + RainLightGrid.x * (cell.y + RainLightGrid.y * cell.z);
		uint cellCount, stride, indexCount, lightCount;
		RainLightCells.GetDimensions(cellCount, stride);
		RainLightIndices.GetDimensions(indexCount, stride);
		RainLights.GetDimensions(lightCount, stride);
		if (cellIndex >= cellCount)
			return result;
		LightLimitFix::LightGrid lightCell = RainLightCells[cellIndex];
		if (lightCell.offset >= indexCount)
			return result;
		uint count = min(min(lightCell.lightCount, MAX_CLUSTER_LIGHTS), indexCount - lightCell.offset);
		float3 illumination = 0.0f;
		float3 weightedDirection = 0.0f;
		float weightedScattering = 0.0f;
		float totalWeight = 0.0f;
		float3 viewDirection = (headPosition - worldPosition) / max(headDistance, 1.0f);
		[loop] for (uint index = 0u; index < count; ++index)
		{
			uint lightIndex = RainLightIndices[lightCell.offset + index];
			if (lightIndex >= min(RainLightGrid.w, lightCount))
				continue;
			LightLimitFix::Light light = RainLights[lightIndex];
			// These light types need directional/room data that the airborne pass does not carry.
			if ((light.lightFlags & (LightLimitFix::LightFlags::Disabled | LightLimitFix::LightFlags::PortalStrict | LightLimitFix::LightFlags::Spot)) != 0u)
				continue;
			float3 toLight = light.positionWS[eyeIndex].xyz - relativePosition;
			float lightDistance = length(toLight);
			if (lightDistance >= light.radius)
				continue;
#	ifdef RAIN_INVERSE_SQUARE
			float attenuation = InverseSquareLighting::GetAttenuation(max(lightDistance, 1.0f), light);
#	else
			float attenuation = saturate(1.0f - lightDistance * lightDistance * light.invRadius * light.invRadius);
#	endif
			float3 lightDirection = toLight / max(lightDistance, 1.0f);
			float forwardScatter = pow(saturate(dot(-lightDirection, viewDirection)), 4.0f);
			float3 lightColor = Color::PointLight(light.color, (light.lightFlags & LightLimitFix::LightFlags::Linear) != 0u, light.lightFlags);
			float3 irradiance = Color::IrradianceToLinear(max(lightColor, 0.0f)) * max(light.fade, 0.0f) * attenuation;
			float weight = Color::RGBToLuminance(irradiance);
			illumination += irradiance;
			weightedDirection += lightDirection * weight;
			weightedScattering += (0.25f + forwardScatter) * weight;
			totalWeight += weight;
		}
		result.Irradiance = min(illumination, 16.0f.xxx) * LocalLighting.x * (1.0f - smoothstep(LocalLighting.y * 0.65f, LocalLighting.y, headDistance));
		result.Direction = weightedDirection / max(totalWeight, 1e-6f);
		result.Scattering = weightedScattering / max(totalWeight, 1e-6f);
#endif
		return result;
	}
}

#endif
