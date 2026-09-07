// Particle-light recognition + JSON-placed light scaling, split from
// LightLimitFix.cpp to keep the core clustering pipeline focused.

#include "Features/LightLimitFix.h"

#include "Globals.h"
#include "ShaderCache.h"
#include "Util.h"
#include "Utils/StringUtils.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
	constexpr uint MAX_LIGHTS = 1024;

	bool IsNearWhiteTint(const RE::NiColorA& a_color)
	{
		const float avg = (a_color.red + a_color.green + a_color.blue) / 3.0f;
		return std::abs(a_color.red - avg) < 0.02f &&
		       std::abs(a_color.green - avg) < 0.02f &&
		       std::abs(a_color.blue - avg) < 0.02f &&
		       avg > 0.92f;
	}

	struct EmissiveTintCandidate
	{
		bool valid = false;
		float distanceSq = std::numeric_limits<float>::max();
		float luma = -1.0f;
		RE::NiColorA tint{};
	};

	void UpdateEmissiveTintCandidate(EmissiveTintCandidate& a_candidate, float a_distanceSq, float a_luma, const RE::NiColorA& a_tint)
	{
		const bool isCloser = a_distanceSq + 1e-3f < a_candidate.distanceSq;
		const bool sameDistance = std::abs(a_distanceSq - a_candidate.distanceSq) <= 1e-3f;
		if (!a_candidate.valid || isCloser || (sameDistance && a_luma > a_candidate.luma)) {
			a_candidate.valid = true;
			a_candidate.distanceSq = a_distanceSq;
			a_candidate.luma = a_luma;
			a_candidate.tint = a_tint;
		}
	}

	RE::NiColorA BuildBillboardFallbackTint(
		const ParticleLights::Config& a_config,
		bool a_hasGradientConfig,
		const ParticleLights::GradientConfig& a_gradientConfig)
	{
		RE::NiColorA fallback{ 1.0f, 1.0f, 1.0f, 1.0f };
		if (a_hasGradientConfig) {
			fallback.red = a_gradientConfig.color.red;
			fallback.green = a_gradientConfig.color.green;
			fallback.blue = a_gradientConfig.color.blue;
		} else {
			fallback.red = a_config.colorMult.red;
			fallback.green = a_config.colorMult.green;
			fallback.blue = a_config.colorMult.blue;
		}
		return fallback;
	}

	RE::BSLightingShaderProperty* GetLightingShaderProperty(RE::NiProperty* a_property)
	{
		if (!a_property || a_property->GetRTTI() != globals::rtti::BSLightingShaderPropertyRTTI.get())
			return nullptr;
		return static_cast<RE::BSLightingShaderProperty*>(a_property);
	}

	void ConsiderLightingEmissiveTint(
		RE::BSGeometry* a_geometry,
		RE::BSGeometry* a_ignoreGeometry,
		const RE::NiPoint3& a_targetPosition,
		EmissiveTintCandidate& a_bestAnyTint,
		EmissiveTintCandidate& a_bestNonWhiteTint)
	{
		if (!a_geometry || a_geometry == a_ignoreGeometry)
			return;

		auto* lightingProperty = GetLightingShaderProperty(a_geometry->GetGeometryRuntimeData().shaderProperty.get());
		if (!lightingProperty || !lightingProperty->emissiveColor || lightingProperty->emissiveMult <= 1e-4f)
			return;

		RE::NiColorA emissiveTint{
			std::max(lightingProperty->emissiveColor->red, 0.0f) * lightingProperty->emissiveMult,
			std::max(lightingProperty->emissiveColor->green, 0.0f) * lightingProperty->emissiveMult,
			std::max(lightingProperty->emissiveColor->blue, 0.0f) * lightingProperty->emissiveMult,
			1.0f
		};

		const float emissiveLuma =
			std::max(emissiveTint.red, 0.0f) +
			std::max(emissiveTint.green, 0.0f) +
			std::max(emissiveTint.blue, 0.0f);
		if (emissiveLuma <= 1e-4f)
			return;

		const auto& center = a_geometry->worldBound.center;
		const float dx = center.x - a_targetPosition.x;
		const float dy = center.y - a_targetPosition.y;
		const float dz = center.z - a_targetPosition.z;
		const float distanceSq = (dx * dx) + (dy * dy) + (dz * dz);
		UpdateEmissiveTintCandidate(a_bestAnyTint, distanceSq, emissiveLuma, emissiveTint);
		if (!IsNearWhiteTint(emissiveTint))
			UpdateEmissiveTintCandidate(a_bestNonWhiteTint, distanceSq, emissiveLuma, emissiveTint);
	}

	void CollectNearbyLightingTint(
		RE::NiNode* a_root,
		RE::BSGeometry* a_ignoreGeometry,
		std::uint32_t a_depthRemaining,
		const RE::NiPoint3& a_targetPosition,
		EmissiveTintCandidate& a_bestAnyTint,
		EmissiveTintCandidate& a_bestNonWhiteTint)
	{
		if (!a_root)
			return;

		for (const auto& child : a_root->GetChildren()) {
			auto* childObject = child.get();
			if (!childObject)
				continue;

			if (auto* childGeometry = childObject->AsGeometry())
				ConsiderLightingEmissiveTint(childGeometry, a_ignoreGeometry, a_targetPosition, a_bestAnyTint, a_bestNonWhiteTint);

			if (a_depthRemaining > 0) {
				if (auto* childNode = childObject->AsNode())
					CollectNearbyLightingTint(childNode, a_ignoreGeometry, a_depthRemaining - 1, a_targetPosition, a_bestAnyTint, a_bestNonWhiteTint);
			}
		}
	}

	bool TryGetBillboardSiblingEmissiveTint(RE::BSGeometry* a_billboardGeometry, RE::NiColorA& a_outTint)
	{
		if (!a_billboardGeometry)
			return false;

		auto* billboardParentNode = a_billboardGeometry->parent ? a_billboardGeometry->parent->AsNode() : nullptr;
		if (!billboardParentNode)
			return false;

		RE::NiNode* searchRoot = billboardParentNode;
		if (auto* ownerNode = billboardParentNode->parent ? billboardParentNode->parent->AsNode() : nullptr)
			searchRoot = ownerNode;

		const RE::NiPoint3 targetPosition = a_billboardGeometry->world.translate;
		EmissiveTintCandidate bestAnyTint{};
		EmissiveTintCandidate bestNonWhiteTint{};
		CollectNearbyLightingTint(searchRoot, a_billboardGeometry, 2u, targetPosition, bestAnyTint, bestNonWhiteTint);
		if (!bestAnyTint.valid)
			return false;

		// Prefer non-white sibling emissive tint when available; fall back to closest emissive tint otherwise.
		a_outTint = bestNonWhiteTint.valid ? bestNonWhiteTint.tint : bestAnyTint.tint;
		return true;
	}

	RE::NiColorA BuildEffectMaterialEmissiveTint(RE::BSEffectShaderMaterial* a_material, RE::BSEffectShaderProperty* a_shaderProperty)
	{
		RE::NiColorA materialEmissiveTint{
			a_material->baseColor.red * a_material->baseColorScale,
			a_material->baseColor.green * a_material->baseColorScale,
			a_material->baseColor.blue * a_material->baseColorScale,
			1.0f
		};
		// Fold in the runtime external-emittance override (set for kExternalEmittance effects)
		// so the particle light matches the tint vanilla renders. See Utils/ExternalEmittance.cpp.
		if (auto emittance = a_shaderProperty->emittanceColor) {
			materialEmissiveTint.red *= emittance->red;
			materialEmissiveTint.green *= emittance->green;
			materialEmissiveTint.blue *= emittance->blue;
		}
		return materialEmissiveTint;
	}

	float GetEmissiveTintLuma(const RE::NiColorA& a_tint)
	{
		return std::max(a_tint.red, 0.0f) +
		       std::max(a_tint.green, 0.0f) +
		       std::max(a_tint.blue, 0.0f);
	}

	struct VertexColor
	{
		std::uint8_t data[4];
	};

	bool TryGetMaxAlphaVertexColor(const std::uint8_t* a_rawVertexData, std::uint32_t a_vertexSize, std::uint32_t a_colorOffset, std::uint32_t a_vertexCount, VertexColor& a_outVertexColor)
	{
		if (!a_rawVertexData || a_vertexSize < sizeof(VertexColor) || a_vertexCount == 0)
			return false;
		if (a_colorOffset > (a_vertexSize - sizeof(VertexColor)))
			return false;

		std::uint8_t maxAlpha = 0;
		bool found = false;
		VertexColor bestColor{};

#if defined(_MSC_VER)
		__try
#endif
		{
			for (std::uint32_t v = 0; v < a_vertexCount; ++v) {
				const std::size_t byteOffset = static_cast<std::size_t>(a_vertexSize) * static_cast<std::size_t>(v) + static_cast<std::size_t>(a_colorOffset);
				const auto* vertex = reinterpret_cast<const VertexColor*>(a_rawVertexData + byteOffset);
				const std::uint8_t alpha = vertex->data[3];
				if (alpha > maxAlpha) {
					maxAlpha = alpha;
					bestColor = *vertex;
					found = true;
				}
			}
		}
#if defined(_MSC_VER)
		__except (1) {
			return false;
		}
#endif

		if (found)
			a_outVertexColor = bestColor;
		return found;
	}
}

float LightLimitFix::CalculateLightDistance(float3 a_lightPosition, float a_radius)
{
	return (a_lightPosition.x * a_lightPosition.x) + (a_lightPosition.y * a_lightPosition.y) + (a_lightPosition.z * a_lightPosition.z) - (a_radius * a_radius);
}

float LightLimitFix::CalculateLuminance(CachedParticleLight& light, RE::NiPoint3& point)
{
	// Mirrors BSLight::CalculateLuminance — keeps NPC detection identical to the GPU lighting math.
	auto lightDirection = light.position - point;
	float lightDist = lightDirection.Length();
	float intensityFactor = std::clamp(lightDist / light.radius, 0.0f, 1.0f);
	float intensityMultiplier = 1 - intensityFactor * intensityFactor;
	return light.grey * intensityMultiplier;
}

void LightLimitFix::AddParticleLightLuminance(RE::NiPoint3& targetPosition, int& numHits, float& lightLevel)
{
	auto shaderCache = globals::shaderCache;
	if (!shaderCache->IsEnabled())
		return;

	std::shared_lock<std::shared_mutex> lk{ cachedParticleLightsMutex };
	int particleLightsDetectionHits = 0;
	if (settings.EnableParticleLightsDetection) {
		for (auto& light : cachedParticleLights) {
			auto luminance = CalculateLuminance(light, targetPosition);
			lightLevel += luminance;
			if (luminance > 0.0)
				particleLightsDetectionHits++;
		}
	}
	numHits += particleLightsDetectionHits;
}

LightLimitFix::ParticleLightReference LightLimitFix::GetParticleLightConfigs(RE::BSRenderPass* a_pass)
{
	if (!a_pass || !a_pass->geometry || !a_pass->shaderProperty)
		return {};

	auto cacheInvalidReference = [&](RE::BSGeometry* node) {
		ParticleLightReference invalidReference{};
		invalidReference.valid = false;
		invalidReference.configVersion = particleLights.configVersion;
		std::lock_guard<std::mutex> queueLock{ particleLightsQueueMutex };
		particleLightsReferences[node] = invalidReference;
		return invalidReference;
	};

	// see https://www.nexusmods.com/skyrimspecialedition/articles/1391
	if (!settings.EnableParticleLights)
		return {};

	auto shaderProperty = a_pass->shaderProperty->GetRTTI() == globals::rtti::BSEffectShaderPropertyRTTI.get() ?
	                          static_cast<RE::BSEffectShaderProperty*>(a_pass->shaderProperty) :
	                          nullptr;
	if (!shaderProperty || shaderProperty->lightData)
		return {};

	auto material = shaderProperty->GetMaterial();
	if (!material)
		return {};

	bool billboard = a_pass->geometry->GetRTTI() != globals::rtti::NiParticleSystemRTTI.get();
	if (billboard) {
		auto parent = a_pass->geometry->parent;
		if (!parent || parent->GetRTTI() != globals::rtti::NiBillboardNodeRTTI.get())
			return {};
	}

	auto* node = a_pass->geometry;

	{
		std::lock_guard<std::mutex> queueLock{ particleLightsQueueMutex };
		auto it = particleLightsReferences.find(node);
		if (it != particleLightsReferences.end()) {
			if (it->second.configVersion == particleLights.configVersion)
				return it->second;
			particleLightsReferences.erase(it);
		}
	}

	if (material->sourceTexturePath.empty())
		return {};

	auto textureName = Util::GetLowercaseStem(material->sourceTexturePath.c_str(), ".dds");
	if (!textureName)
		return cacheInvalidReference(node);

	auto& configs = particleLights.particleLightConfigs;
	auto it = configs.find(*textureName);
	if (it == configs.end())
		return cacheInvalidReference(node);

	ParticleLights::Config config = it->second;
	bool hasGradientConfig = false;
	ParticleLights::GradientConfig gradientConfig{};
	if (!material->greyscaleTexturePath.empty()) {
		// A greyscale texture must resolve to a known gradient, or the reference is
		// invalid -- falling back to the base config here let ungated effects (e.g.
		// Spellforge) get lit and blow out depending on the NIF.
		const auto gradientName = Util::GetLowercaseStem(material->greyscaleTexturePath.c_str(), ".dds");
		if (!gradientName)
			return cacheInvalidReference(node);
		auto& gradientConfigs = particleLights.particleLightGradientConfigs;
		auto itGradient = gradientConfigs.find(*gradientName);
		if (itGradient == gradientConfigs.end())
			return cacheInvalidReference(node);
		hasGradientConfig = true;
		gradientConfig = itGradient->second;
	}

	ParticleLightReference reference{};
	reference.valid = true;
	reference.billboard = billboard;
	reference.applyEffectMaterialTint = true;
	reference.config = config;
	reference.hasGradientConfig = hasGradientConfig;
	reference.gradientConfig = gradientConfig;
	reference.baseColor = { 1, 1, 1, 1 };
	reference.configVersion = particleLights.configVersion;

	if (billboard) {
		bool hasVertexTint = false;
		if (auto rendererData = a_pass->geometry->GetGeometryRuntimeData().rendererData) {
			if (auto triShape = a_pass->geometry->AsTriShape()) {
				const std::uint32_t vertexSize = rendererData->vertexDesc.GetSize();
				if (rendererData->vertexDesc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_COLORS) && rendererData->rawVertexData && vertexSize > 0u) {
					const std::uint32_t offset = rendererData->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_COLOR);
					const std::uint32_t vertexCount = static_cast<std::uint32_t>(triShape->GetTrishapeRuntimeData().vertexCount);

					VertexColor maxAlphaVertexColor{};
					if (TryGetMaxAlphaVertexColor(rendererData->rawVertexData, vertexSize, offset, vertexCount, maxAlphaVertexColor)) {
						reference.baseColor.red *= maxAlphaVertexColor.data[0] / 255.f;
						reference.baseColor.green *= maxAlphaVertexColor.data[1] / 255.f;
						reference.baseColor.blue *= maxAlphaVertexColor.data[2] / 255.f;
						hasVertexTint = true;
						if (shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kVertexAlpha))
							reference.baseColor.alpha *= maxAlphaVertexColor.data[3] / 255.f;
					}
				}
			}
		}

		RE::NiColorA siblingEmissiveTint{};
		bool hasSiblingEmissiveTint = false;
		const bool vertexTintLooksWhite = hasVertexTint && IsNearWhiteTint(reference.baseColor);
		if (!hasVertexTint || vertexTintLooksWhite) {
			hasSiblingEmissiveTint = TryGetBillboardSiblingEmissiveTint(node, siblingEmissiveTint);
			const bool siblingTintIsNonWhite = hasSiblingEmissiveTint && !IsNearWhiteTint(siblingEmissiveTint);

			const RE::NiColorA materialEmissiveTint = BuildEffectMaterialEmissiveTint(material, shaderProperty);
			const float materialEmissiveLuma = GetEmissiveTintLuma(materialEmissiveTint);
			const bool hasMaterialEmissiveTint = materialEmissiveLuma > 1e-4f;
			const bool materialTintIsNonWhite = hasMaterialEmissiveTint && !IsNearWhiteTint(materialEmissiveTint);

			// Resolve the fallback tint from a single source so a white tint never gets re-tinted
			// from an adjacent emissive — that double-application produces washed-out particle lights.
			if (materialTintIsNonWhite) {
				reference.baseColor = materialEmissiveTint;
				reference.applyEffectMaterialTint = false;
			} else if (siblingTintIsNonWhite) {
				reference.baseColor = siblingEmissiveTint;
				reference.applyEffectMaterialTint = false;
			} else if (hasMaterialEmissiveTint) {
				reference.baseColor = materialEmissiveTint;
				reference.applyEffectMaterialTint = false;
			} else if (hasSiblingEmissiveTint) {
				reference.baseColor = siblingEmissiveTint;
				reference.applyEffectMaterialTint = false;
			} else {
				reference.baseColor = BuildBillboardFallbackTint(config, hasGradientConfig, gradientConfig);
				reference.applyEffectMaterialTint = true;
			}
		}
	}

	{
		std::lock_guard<std::mutex> queueLock{ particleLightsQueueMutex };
		particleLightsReferences[node] = reference;
	}
	return reference;
}

bool LightLimitFix::CheckParticleLights(RE::BSRenderPass* a_pass, uint32_t)
{
	if (!a_pass || !a_pass->geometry || !a_pass->shaderProperty)
		return true;

	auto shaderCache = globals::shaderCache;
	if (!shaderCache->IsEnabled())
		return true;

	auto reference = GetParticleLightConfigs(a_pass);
	if (reference.valid) {
		if (AddParticleLight(a_pass, reference))
			return !(settings.EnableParticleLightsCulling && reference.config.cull);
	}
	return true;
}

bool LightLimitFix::AddParticleLight(RE::BSRenderPass* a_pass, ParticleLightReference a_reference)
{
	if (!a_pass || !a_pass->geometry || !a_pass->shaderProperty)
		return false;

	auto shaderProperty = a_pass->shaderProperty->GetRTTI() == globals::rtti::BSEffectShaderPropertyRTTI.get() ?
	                          static_cast<RE::BSEffectShaderProperty*>(a_pass->shaderProperty) :
	                          nullptr;
	if (!shaderProperty)
		return false;

	auto material = shaderProperty->GetMaterial();
	if (!material)
		return false;
	const auto& config = a_reference.config;

	a_pass->geometry->IncRefCount();

	if (!a_reference.billboard) {
		if (auto particleSystem = static_cast<RE::NiParticleSystem*>(a_pass->geometry)) {
			if (auto particleData = particleSystem->GetParticlesRuntimeData().particleData.get())
				particleData->IncRefCount();
		}
	}

	RE::NiColorA color = a_reference.baseColor;
	if (a_reference.applyEffectMaterialTint) {
		color.red *= material->baseColor.red * material->baseColorScale;
		color.green *= material->baseColor.green * material->baseColorScale;
		color.blue *= material->baseColor.blue * material->baseColorScale;

		if (auto emittance = shaderProperty->emittanceColor) {
			color.red *= emittance->red;
			color.green *= emittance->green;
			color.blue *= emittance->blue;
		}
	}

	if (a_reference.hasGradientConfig) {
		auto grey = float3(config.colorMult.red, config.colorMult.green, config.colorMult.blue).Dot(float3(0.3f, 0.59f, 0.11f));
		color.red *= grey * a_reference.gradientConfig.color.red;
		color.green *= grey * a_reference.gradientConfig.color.green;
		color.blue *= grey * a_reference.gradientConfig.color.blue;
	} else {
		color.red *= config.colorMult.red;
		color.green *= config.colorMult.green;
		color.blue *= config.colorMult.blue;
	}
	// Stash radiusMult as alpha for the downstream cluster pass.
	color.alpha = std::max(config.radiusMult, 0.0f);

	ParticleLightInfo info;
	info.billboard = a_reference.billboard;
	info.node = a_pass->geometry;
	info.color = color;
	info.radiusMult = config.radiusMult;

	bool enqueued = false;
	{
		std::lock_guard<std::mutex> queueLock{ particleLightsQueueMutex };
		constexpr std::size_t kMaxQueuedParticleLights = static_cast<std::size_t>(MAX_LIGHTS) * 16u;
		if (queuedParticleLights.size() < kMaxQueuedParticleLights) {
			queuedParticleLights.push_back(info);
			enqueued = true;
		}
	}

	if (!enqueued) {
		if (!a_reference.billboard) {
			if (auto particleSystem = static_cast<RE::NiParticleSystem*>(a_pass->geometry)) {
				if (auto particleData = particleSystem->GetParticlesRuntimeData().particleData.get())
					particleData->DecRefCount();
			}
		}
		a_pass->geometry->DecRefCount();
		return false;
	}

	return true;
}

void LightLimitFix::AddCachedParticleLights(eastl::vector<LightData>& lightsData, LightLimitFix::LightData& light)
{
	if (lightsData.size() >= MAX_LIGHTS)
		return;

	static float& lightFadeStart = *reinterpret_cast<float*>(REL::RelocationID(527668, 414582).address());
	static float& lightFadeEnd = *reinterpret_cast<float*>(REL::RelocationID(527669, 414583).address());
	const float3 luminanceWeights = float3(0.3f, 0.59f, 0.11f);

	if (settings.MaxParticleDistance > 0.0f) {
		const float maxDistSq = settings.MaxParticleDistance * settings.MaxParticleDistance;
		const auto& pos = light.positionWS[0].data;  // camera-relative
		const float distSq = (pos.x * pos.x) + (pos.y * pos.y) + (pos.z * pos.z);
		if (distSq > maxDistSq)
			return;
	}

	float distance = CalculateLightDistance(light.positionWS[0].data, light.radius);

	float dimmer = 0.0f;
	// lightFadeEnd <= lightFadeStart guards a zero/negative denominator below
	// (equal engine globals would otherwise divide by zero → NaN into lighting).
	if (distance < lightFadeStart || lightFadeEnd == 0.0f || lightFadeEnd <= lightFadeStart)
		dimmer = 1.0f;
	else if (distance <= lightFadeEnd)
		dimmer = 1.0f - ((distance - lightFadeStart) / (lightFadeEnd - lightFadeStart));
	else
		dimmer = 0.0f;

	light.fade *= dimmer;
	const float luminanceScale = light.fade;
	if ((light.color.x + light.color.y + light.color.z) * luminanceScale > 1e-4 && light.radius > 1e-4) {
		light.invRadius = 1.f / light.radius;
		lightsData.push_back(light);

		if (cachedParticleLights.size() < MAX_LIGHTS) {
			CachedParticleLight cachedParticleLight{};
			cachedParticleLight.grey = float3(light.color.x, light.color.y, light.color.z).Dot(luminanceWeights) * luminanceScale;
			cachedParticleLight.radius = light.radius;
			cachedParticleLight.position = {
				light.positionWS[0].data.x + eyePositionCached[0].x,
				light.positionWS[0].data.y + eyePositionCached[0].y,
				light.positionWS[0].data.z + eyePositionCached[0].z
			};
			cachedParticleLights.push_back(cachedParticleLight);
		}
	}
}

void LightLimitFix::ProcessQueuedParticleLights(eastl::vector<LightData>& lightsData)
{
	std::lock_guard<std::shared_mutex> lk{ cachedParticleLightsMutex };
	cachedParticleLights.clear();

	LightData clusteredLight{};
	uint32_t clusteredLights = 0;

	auto flushClusteredLight = [&]() {
		if (!clusteredLights)
			return;

		const float clusterCount = static_cast<float>(clusteredLights);
		clusteredLight.radius /= clusterCount;
		clusteredLight.positionWS[0].data /= clusterCount;
		clusteredLight.positionWS[1].data = clusteredLight.positionWS[0].data;

		if (eyeCount == 2) {
			// Second-eye cache is only populated in VR; read it only here.
			const auto eyePositionOffset = eyePositionCached[0] - eyePositionCached[1];
			clusteredLight.positionWS[1].data.x += eyePositionOffset.x;
			clusteredLight.positionWS[1].data.y += eyePositionOffset.y;
			clusteredLight.positionWS[1].data.z += eyePositionOffset.z;
		}

		clusteredLight.lightFlags.set(LightFlags::Simple);
		clusteredLight.lightFlags.set(LightFlags::Particle);
		AddCachedParticleLights(lightsData, clusteredLight);

		clusteredLights = 0;
		clusteredLight = {};
	};

	std::lock_guard<std::mutex> queueLock{ particleLightsQueueMutex };
	for (const auto& particleLight : currentParticleLights) {
		if (!particleLight.node)
			continue;

		if (!particleLight.billboard) {
			auto particleSystem = static_cast<RE::NiParticleSystem*>(particleLight.node);
			if (particleSystem && particleSystem->GetParticlesRuntimeData().particleData.get()) {
				auto particleData = particleSystem->GetParticlesRuntimeData().particleData.get();
				auto& particleSystemRuntimeData = particleSystem->GetParticleSystemRuntimeData();
				auto& particleRuntimeData = particleData->GetParticlesRuntimeData();

				if (!particleRuntimeData.radii || !particleRuntimeData.sizes || !particleRuntimeData.positions)
					continue;

				std::uint32_t numVertices = static_cast<std::uint32_t>(particleData->GetActiveVertexCount());
				const std::uint32_t runtimeMaxVertices = static_cast<std::uint32_t>(particleRuntimeData.maxNumVertices);
				const std::uint32_t runtimeNumVertices = static_cast<std::uint32_t>(particleRuntimeData.numVertices);
				if (runtimeMaxVertices == 0)
					continue;
				numVertices = std::min(numVertices, runtimeMaxVertices);
				if (runtimeNumVertices > 0)
					numVertices = std::min(numVertices, runtimeNumVertices);

				std::uint32_t maxPerEmitter = static_cast<std::uint32_t>(std::max(1, settings.MaxParticlesPerEmitter));
				if (numVertices > maxPerEmitter)
					numVertices = maxPerEmitter;

				for (std::uint32_t p = 0; p < numVertices; p++) {
					float radius = particleRuntimeData.radii[p] * particleRuntimeData.sizes[p];

					auto initialPosition = particleRuntimeData.positions[p];
					if (!particleSystemRuntimeData.isWorldspace) {
						// First-person meshes report a scaled model bound vs world bound mismatch.
						if ((particleLight.node->GetModelData().modelBound.radius * particleLight.node->world.scale) != particleLight.node->worldBound.radius) {
							const auto& center = particleLight.node->worldBound.center;
							initialPosition = { initialPosition.x + center.x, initialPosition.y + center.y, initialPosition.z + center.z };
						} else {
							const auto& translate = particleLight.node->world.translate;
							initialPosition = { initialPosition.x + translate.x, initialPosition.y + translate.y, initialPosition.z + translate.z };
						}
					}

					RE::NiPoint3 positionWS{
						initialPosition.x - eyePositionCached[0].x,
						initialPosition.y - eyePositionCached[0].y,
						initialPosition.z - eyePositionCached[0].z
					};

					if (clusteredLights) {
						auto averageRadius = clusteredLight.radius / (float)clusteredLights;
						float radiusDiff = abs(averageRadius - radius);

						auto averagePosition = clusteredLight.positionWS[0].data / (float)clusteredLights;
						float positionDiff = positionWS.GetDistance({ averagePosition.x, averagePosition.y, averagePosition.z });

						if ((radiusDiff + positionDiff) > settings.ParticleClusterThreshold ||
							!settings.EnableParticleLightsOptimization) {
							flushClusteredLight();
						}
					}

					float alpha = particleLight.color.alpha;
					float3 color{
						particleLight.color.red,
						particleLight.color.green,
						particleLight.color.blue
					};
					if (particleRuntimeData.color) {
						alpha *= particleRuntimeData.color[p].alpha;
						color.x *= particleRuntimeData.color[p].red;
						color.y *= particleRuntimeData.color[p].green;
						color.z *= particleRuntimeData.color[p].blue;
					}
					clusteredLight.color += Saturation(color, settings.ParticleLightsSaturation) * alpha * settings.ParticleBrightness;

					clusteredLight.radius += radius * particleLight.radiusMult * settings.ParticleRadius;

					clusteredLight.positionWS[0].data.x += positionWS.x;
					clusteredLight.positionWS[0].data.y += positionWS.y;
					clusteredLight.positionWS[0].data.z += positionWS.z;

					clusteredLights++;
				}
			}
		} else {
			LightData light{};

			light.color.x = particleLight.color.red;
			light.color.y = particleLight.color.green;
			light.color.z = particleLight.color.blue;

			light.color = Saturation(light.color, settings.ParticleLightsSaturation);

			light.color *= particleLight.color.alpha * settings.BillboardBrightness;
			light.radius = particleLight.node->worldBound.radius * particleLight.radiusMult * settings.BillboardRadius * 0.5f;

			auto position = particleLight.node->world.translate;
			SetLightPosition(light, position);

			light.lightFlags.set(LightFlags::Simple);
			light.lightFlags.set(LightFlags::Particle);

			AddCachedParticleLights(lightsData, light);
		}
	}

	flushClusteredLight();
}

float LightLimitFix::Hooks::AIProcess_CalculateLightValue_GetLuminance::thunk(
	RE::ShadowSceneNode* shadowSceneNode,
	RE::NiPoint3& targetPosition,
	int& numHits,
	float& sunLightLevel,
	float& lightLevel,
	RE::NiLight& refLight,
	int32_t shadowBitMask)
{
	auto ret = func(shadowSceneNode, targetPosition, numHits, sunLightLevel, lightLevel, refLight, shadowBitMask);
	globals::features::lightLimitFix.AddParticleLightLuminance(targetPosition, numHits, ret);
	return ret;
}
