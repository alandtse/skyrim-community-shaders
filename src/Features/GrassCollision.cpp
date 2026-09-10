#include "GrassCollision.h"

#include "FeatureBuffer.h"
#include "Globals.h"
#include "GpuPass.h"
#include "I18n/I18n.h"
#include "State.h"
#include "Utils/ActorUtils.h"
#include "Utils/D3D.h"
#include "Utils/UI.h"

#define I18N_KEY_PREFIX "feature.grass_collision."

static constexpr uint MAX_BOUNDING_BOXES = 64;
static constexpr uint MAX_COLLISIONS_PER_BOUNDING_BOX = 3;
static constexpr uint MAX_COLLISIONS = MAX_BOUNDING_BOXES * MAX_COLLISIONS_PER_BOUNDING_BOX;
static constexpr float MAX_ACTOR_DISTANCE = 4096.0f;
static constexpr float MAX_ACTOR_SQ_DISTANCE = MAX_ACTOR_DISTANCE * MAX_ACTOR_DISTANCE;
static constexpr float MIN_COLLISION_RADIUS_DISTANCE_SCALE = 0.001f;
static constexpr float MIN_COLLISION_RADIUS_SCALE = 1.0f;
static constexpr float MAX_COLLISION_RADIUS_SCALE = 10.0f;
static constexpr float MIN_GRASS_INTERACTION_RADIUS = 0.0f;
static constexpr float MAX_GRASS_INTERACTION_RADIUS = 128.0f;
static constexpr float MIN_COLLISION_IMPACT_STRENGTH = 0.0f;
static constexpr float MAX_COLLISION_IMPACT_STRENGTH = 4.0f;
static constexpr float MIN_SPRING_STRENGTH = 0.05f;
static constexpr float MAX_SPRING_STRENGTH = 40.0f;
static constexpr float MIN_DAMPING = 0.0f;
static constexpr float MAX_DAMPING = 20.0f;
static constexpr float MIN_MAXIMUM_BEND = 5.0f;
static constexpr float MAX_MAXIMUM_BEND = 89.0f;
static constexpr float MIN_MAXIMUM_COMPRESSION = 0.0f;
static constexpr float MAX_MAXIMUM_COMPRESSION = 1.0f;
static constexpr float MIN_COMPRESSION_HEIGHT = 10.0f;
static constexpr float MAX_COMPRESSION_HEIGHT = 100.0f;
static constexpr float MIN_MAXIMUM_COMPRESSIBLE_GRASS_HEIGHT = 16.0f;
static constexpr float MAX_MAXIMUM_COMPRESSIBLE_GRASS_HEIGHT = 512.0f;
static constexpr float MIN_COMPRESSION_RECOVERY = 0.1f;
static constexpr float MAX_COMPRESSION_RECOVERY = 10.0f;

struct GrassCollisionActorCandidate
{
	RE::ActorHandle handle;
	float sqDistance;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	GrassCollision::Settings,
	EnableGrassCollision,
	TrackRagdolls,
	CollisionRadiusScale,
	GrassInteractionRadius,
	CollisionImpactStrength,
	SpringStrength,
	Damping,
	MaximumBend,
	MaximumCompression,
	CompressionHeight,
	MaximumCompressibleGrassHeight,
	CompressionRecovery)

void GrassCollision::DrawSettings()
{
	if (ImGui::TreeNodeEx(T(TKEY("grass_collision"), "Grass Collision"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox(T(TKEY("enable"), "Enable Grass Collision"), (bool*)&settings.EnableGrassCollision);
		ImGui::SliderFloat(T(TKEY("radius_scale"), "Collision Radius Scale"), &settings.CollisionRadiusScale,
			MIN_COLLISION_RADIUS_SCALE, MAX_COLLISION_RADIUS_SCALE, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("grass_interaction_radius"), "Grass Interaction Radius"),
			&settings.GrassInteractionRadius, MIN_GRASS_INTERACTION_RADIUS, MAX_GRASS_INTERACTION_RADIUS,
			"%.0f units", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("impact_strength"), "Collision Impact"), &settings.CollisionImpactStrength,
			MIN_COLLISION_IMPACT_STRENGTH, MAX_COLLISION_IMPACT_STRENGTH, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("spring_strength"), "Spring Strength"), &settings.SpringStrength,
			MIN_SPRING_STRENGTH, MAX_SPRING_STRENGTH, "%.2f", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
		ImGui::SliderFloat(T(TKEY("damping"), "Damping"), &settings.Damping,
			MIN_DAMPING, MAX_DAMPING, "%.1f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("maximum_bend"), "Maximum Bend"), &settings.MaximumBend,
			MIN_MAXIMUM_BEND, MAX_MAXIMUM_BEND, "%.0f degrees", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("maximum_compression"), "Maximum Compression"), &settings.MaximumCompression,
			MIN_MAXIMUM_COMPRESSION, MAX_MAXIMUM_COMPRESSION, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("compression_height"), "Compression Reach"), &settings.CompressionHeight,
			MIN_COMPRESSION_HEIGHT, MAX_COMPRESSION_HEIGHT, "%.0f%% of blade", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("maximum_compressible_grass_height"), "Maximum Compressible Grass Height"),
			&settings.MaximumCompressibleGrassHeight, MIN_MAXIMUM_COMPRESSIBLE_GRASS_HEIGHT,
			MAX_MAXIMUM_COMPRESSIBLE_GRASS_HEIGHT, "%.0f units", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("maximum_compressible_grass_height_tooltip"),
				"Grass taller than this can still bend around actors but will not be vertically compressed."));
		ImGui::SliderFloat(T(TKEY("compression_recovery"), "Compression Recovery"), &settings.CompressionRecovery,
			MIN_COMPRESSION_RECOVERY, MAX_COMPRESSION_RECOVERY, "%.2f", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("compression_recovery_tooltip"), "Lower values keep grass compressed longer after an actor moves away."));
		ImGui::TreePop();
	}
}

GrassCollision::ShaderData GrassCollision::GetCommonBufferData() const noexcept
{
	return {
		shaderPosOffset,
		shaderArrayOrigin,
		previousShaderPosOffset,
		previousShaderArrayOrigin,
		std::clamp(settings.CompressionHeight, MIN_COMPRESSION_HEIGHT, MAX_COMPRESSION_HEIGHT) * 0.01f,
		std::clamp(settings.MaximumCompressibleGrassHeight,
			MIN_MAXIMUM_COMPRESSIBLE_GRASS_HEIGHT, MAX_MAXIMUM_COMPRESSIBLE_GRASS_HEIGHT),
		{}
	};
}

void GrassCollision::QueueCollisions()
{
	if (!settings.EnableGrassCollision)
		return;
	const float collisionRadiusScale = std::clamp(settings.CollisionRadiusScale,
		MIN_COLLISION_RADIUS_SCALE, MAX_COLLISION_RADIUS_SCALE);
	const float grassInteractionRadius = std::clamp(settings.GrassInteractionRadius,
		MIN_GRASS_INTERACTION_RADIUS, MAX_GRASS_INTERACTION_RADIUS);

	eastl::vector<GrassCollisionActorCandidate> actorCandidates{};
	std::unordered_set<uint32_t> candidateActors;
	RE::NiPoint3 cameraPosition = Util::GetEyePosition(0);

	auto addActorCandidate = [&](RE::ActorHandle a_handle) {
		auto actor = a_handle.get();
		if (actor && actor->Is3DLoaded()) {
			float sqDistance = cameraPosition.GetSquaredDistance(actor->GetPosition());
			if (sqDistance <= MAX_ACTOR_SQ_DISTANCE && candidateActors.insert(actor->GetFormID()).second)
				actorCandidates.push_back({ a_handle, sqDistance });
		}
	};

	// Actor query code from po3 under MIT
	// https://github.com/powerof3/PapyrusExtenderSSE/blob/7a73b47bc87331bec4e16f5f42f2dbc98b66c3a7/include/Papyrus/Functions/Faction.h#L24C7-L46
	if (const auto processLists = RE::ProcessLists::GetSingleton(); processLists) {
		for (auto& actorHandle : processLists->highActorHandles) {
			addActorCandidate(actorHandle);
		}
	}

	if (auto player = RE::PlayerCharacter::GetSingleton()) {
		addActorCandidate(player->GetHandle());
	}

	std::sort(actorCandidates.begin(), actorCandidates.end(), [](const GrassCollisionActorCandidate& a, const GrassCollisionActorCandidate& b) {
		return a.sqDistance < b.sqDistance;
	});

	eastl::vector<BoundingBoxPacked> boundingBoxData{};
	boundingBoxData.reserve(MAX_BOUNDING_BOXES);

	eastl::vector<CollisionShapePacked> collisionsData{};
	collisionsData.reserve(MAX_COLLISIONS);
	std::unordered_set<uint32_t> activeActors;

	uint collisionIndexExtent = 0;

	for (const auto& actorCandidate : actorCandidates) {
		auto actor = actorCandidate.handle.get();
		if (actor && actor->Is3DLoaded()) {
			auto root = actor->Get3D(false);
			if (!root)
				continue;

			float distance = std::sqrt(actorCandidate.sqDistance);
			eastl::vector<Util::ShapeCollisionCapsule> collisionShapes{};

			RE::BSVisit::TraverseScenegraphCollision(root, [&](RE::bhkNiCollisionObject* a_object) -> RE::BSVisit::BSVisitControl {
				Util::ShapeCollisionCapsule capsule;
				if (Util::GetShapeCollisionCapsule(a_object, capsule)) {
					capsule.radius *= collisionRadiusScale;
					if (capsule.radius < distance * MIN_COLLISION_RADIUS_DISTANCE_SCALE)
						return RE::BSVisit::BSVisitControl::kContinue;
					collisionShapes.push_back(capsule);
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});

			std::sort(collisionShapes.begin(), collisionShapes.end(), [](const auto& a, const auto& b) {
				return a.radius > b.radius;
			});
			if (collisionShapes.size() > MAX_COLLISIONS_PER_BOUNDING_BOX)
				collisionShapes.resize(MAX_COLLISIONS_PER_BOUNDING_BOX);

			const uint32_t actorID = actor->GetFormID();
			auto historyIt = actorCollisionHistory.find(actorID);
			const bool hasHistory = historyIt != actorCollisionHistory.end();
			std::vector<CapsuleHistory> newHistory;
			newHistory.reserve(collisionShapes.size());

			BoundingBoxPacked boundingBox;
			boundingBox.MinExtent = { std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
			boundingBox.MaxExtent = { std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest() };
			boundingBox.IndexStart = collisionIndexExtent;
			boundingBox.IndexEnd = collisionIndexExtent;

			for (size_t shapeIndex = 0; shapeIndex < collisionShapes.size(); ++shapeIndex) {
				const auto& shape = collisionShapes[shapeIndex];
				const CapsuleHistory currentShape{
					{ shape.pointA.x, shape.pointA.y, shape.pointA.z },
					{ shape.pointB.x, shape.pointB.y, shape.pointB.z }
				};
				const CapsuleHistory& previousShape = hasHistory && shapeIndex < historyIt->second.size() ?
				                                          historyIt->second[shapeIndex] :
				                                          currentShape;
				newHistory.push_back(currentShape);

				auto toCameraRelative = [&](const float3& point) {
					return float3{ point.x - cameraPosition.x, point.y - cameraPosition.y, point.z - cameraPosition.z };
				};
				const float3 currentA = toCameraRelative(currentShape.pointA);
				const float3 currentB = toCameraRelative(currentShape.pointB);
				const float3 previousA = toCameraRelative(previousShape.pointA);
				const float3 previousB = toCameraRelative(previousShape.pointB);
				CollisionShapePacked data{
					{ currentA.x, currentA.y, currentA.z, shape.radius },
					{ currentB.x, currentB.y, currentB.z, 0.0f },
					{ previousA.x, previousA.y, previousA.z, 0.0f },
					{ previousB.x, previousB.y, previousB.z, 0.0f }
				};
				collisionsData.push_back(data);

				const float projectedHalfLength = 0.5f * std::max(
															 std::hypot(currentA.x - currentB.x, currentA.y - currentB.y),
															 std::hypot(previousA.x - previousB.x, previousA.y - previousB.y));
				const float radius = shape.radius + projectedHalfLength + grassInteractionRadius;
				float2 pointMin(
					std::min({ currentA.x, currentB.x, previousA.x, previousB.x }) - radius,
					std::min({ currentA.y, currentB.y, previousA.y, previousB.y }) - radius);
				float2 pointMax(
					std::max({ currentA.x, currentB.x, previousA.x, previousB.x }) + radius,
					std::max({ currentA.y, currentB.y, previousA.y, previousB.y }) + radius);

				boundingBox.MinExtent.x = std::min(boundingBox.MinExtent.x, pointMin.x);
				boundingBox.MinExtent.y = std::min(boundingBox.MinExtent.y, pointMin.y);

				boundingBox.MaxExtent.x = std::max(boundingBox.MaxExtent.x, pointMax.x);
				boundingBox.MaxExtent.y = std::max(boundingBox.MaxExtent.y, pointMax.y);

				boundingBox.IndexEnd++;
			}

			if (boundingBox.IndexStart != boundingBox.IndexEnd) {
				activeActors.insert(actorID);
				actorCollisionHistory[actorID] = std::move(newHistory);
				boundingBoxData.push_back(boundingBox);
				collisionIndexExtent = boundingBox.IndexEnd;
				if (boundingBoxData.size() == MAX_BOUNDING_BOXES)
					break;
			}
		}
	}

	std::erase_if(actorCollisionHistory, [&](const auto& entry) {
		return !activeActors.contains(entry.first);
	});

	queuedBoundingBoxes = std::move(boundingBoxData);
	queuedCollisions = std::move(collisionsData);
}

void GrassCollision::Update()
{
	static Util::FrameChecker frameChecker;
	if (frameChecker.IsNewFrame()) {
		PerFrame perFrameData{};
		static float2 prevCellID = { 0, 0 };
		static bool fieldInitialized = false;
		auto eyePosNI = Util::GetEyePosition(0);
		auto eyePos = float2{ eyePosNI.x, eyePosNI.y };

		float worldSize = 8192.0f;
		uint textureArrayDims = 1024;

		float cellSize = worldSize / textureArrayDims;

		auto cellID = eyePos / cellSize;
		cellID = float2{ round(cellID.x), round(cellID.y) };
		auto cellOrigin = cellID * cellSize;
		if (!fieldInitialized)
			prevCellID = cellID;

		float2 cellIDDiff = prevCellID - cellID;
		perFrameData.PosOffset = cellOrigin - eyePos;

		perFrameData.ArrayOrigin = {
			((int)cellID.x - textureArrayDims / 2) % textureArrayDims,
			((int)cellID.y - textureArrayDims / 2) % textureArrayDims
		};
		previousShaderPosOffset = fieldInitialized ? shaderPosOffset : perFrameData.PosOffset;
		previousShaderArrayOrigin = fieldInitialized ? shaderArrayOrigin : perFrameData.ArrayOrigin;
		shaderPosOffset = perFrameData.PosOffset;
		shaderArrayOrigin = perFrameData.ArrayOrigin;

		perFrameData.ValidMargin = { (int)cellIDDiff.x, (int)cellIDDiff.y };

		perFrameData.TimeDelta = std::clamp(
			*globals::game::deltaTime * !globals::game::ui->GameIsPaused(), 0.0f, 1.0f / 15.0f);
		perFrameData.GrassInteractionRadius = std::clamp(settings.GrassInteractionRadius,
			MIN_GRASS_INTERACTION_RADIUS, MAX_GRASS_INTERACTION_RADIUS);
		perFrameData.CollisionStrength = std::clamp(settings.CollisionImpactStrength,
			MIN_COLLISION_IMPACT_STRENGTH, MAX_COLLISION_IMPACT_STRENGTH);
		perFrameData.SpringStrength = std::clamp(settings.SpringStrength,
			MIN_SPRING_STRENGTH, MAX_SPRING_STRENGTH);
		perFrameData.Damping = std::clamp(settings.Damping, MIN_DAMPING, MAX_DAMPING);
		perFrameData.MaximumBend = DirectX::XMConvertToRadians(std::clamp(settings.MaximumBend,
			MIN_MAXIMUM_BEND, MAX_MAXIMUM_BEND));
		perFrameData.MaximumCompression = std::clamp(settings.MaximumCompression,
			MIN_MAXIMUM_COMPRESSION, MAX_MAXIMUM_COMPRESSION);
		perFrameData.CompressionRecovery = std::clamp(settings.CompressionRecovery,
			MIN_COMPRESSION_RECOVERY, MAX_COMPRESSION_RECOVERY);

		perFrameData.BoundingBoxCount = std::min((uint)queuedBoundingBoxes.size(), MAX_BOUNDING_BOXES);

		auto context = globals::d3d::context;

		if (!queuedCollisions.empty()) {
			D3D11_MAPPED_SUBRESOURCE mapped;
			DX::ThrowIfFailed(context->Map(collisionInstances->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
			size_t bytes = sizeof(CollisionShapePacked) * queuedCollisions.size();
			memcpy_s(mapped.pData, bytes, queuedCollisions.data(), bytes);
			context->Unmap(collisionInstances->resource.get(), 0);
		}

		if (perFrameData.BoundingBoxCount > 0) {
			D3D11_MAPPED_SUBRESOURCE mapped;
			DX::ThrowIfFailed(context->Map(collisionBoundingBoxes->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
			size_t bytes = sizeof(BoundingBoxPacked) * perFrameData.BoundingBoxCount;
			memcpy_s(mapped.pData, bytes, queuedBoundingBoxes.data(), bytes);
			context->Unmap(collisionBoundingBoxes->resource.get(), 0);
		}

		queuedBoundingBoxes.clear();
		queuedCollisions.clear();

		perFrame->Update(perFrameData);

		UpdateCollisionTexture();
		auto [featureData, featureDataSize] = GetFeatureBufferData(globals::state->inWorld);
		globals::state->featureDataCB->Update(featureData, featureDataSize);

		prevCellID = cellID;
		fieldInitialized = true;

		BindDeformationResources();
	}
}

void GrassCollision::BindDeformationResources(bool a_compute)
{
	auto* context = globals::d3d::context;
	ID3D11ShaderResourceView* srvs[] = {
		deformationTextures[currentTextureIndex]->srv.get(),
		deformationTextures[currentTextureIndex ^ 1]->srv.get()
	};
	ID3D11SamplerState* samplers[] = { deformationSampler.get() };
	if (a_compute) {
		context->CSSetShaderResources(100, ARRAYSIZE(srvs), srvs);
		context->CSSetSamplers(15, ARRAYSIZE(samplers), samplers);
	} else {
		context->VSSetShaderResources(100, ARRAYSIZE(srvs), srvs);
		context->VSSetSamplers(15, ARRAYSIZE(samplers), samplers);
	}
}

void GrassCollision::LoadSettings(json& o_json)
{
	settings = o_json;
	const Settings defaults{};
	if (!std::isfinite(settings.CompressionHeight))
		settings.CompressionHeight = defaults.CompressionHeight;
	else if (settings.CompressionHeight <= 1.0f)
		settings.CompressionHeight *= 100.0f;
	settings.CompressionHeight = std::clamp(settings.CompressionHeight,
		MIN_COMPRESSION_HEIGHT, MAX_COMPRESSION_HEIGHT);
	if (!std::isfinite(settings.MaximumCompressibleGrassHeight))
		settings.MaximumCompressibleGrassHeight = defaults.MaximumCompressibleGrassHeight;
	settings.MaximumCompressibleGrassHeight = std::clamp(settings.MaximumCompressibleGrassHeight,
		MIN_MAXIMUM_COMPRESSIBLE_GRASS_HEIGHT, MAX_MAXIMUM_COMPRESSIBLE_GRASS_HEIGHT);
	if (!std::isfinite(settings.CompressionRecovery))
		settings.CompressionRecovery = defaults.CompressionRecovery;
	settings.CompressionRecovery = std::clamp(settings.CompressionRecovery,
		MIN_COMPRESSION_RECOVERY, MAX_COMPRESSION_RECOVERY);
}

void GrassCollision::SaveSettings(json& o_json)
{
	o_json = settings;
}

void GrassCollision::RestoreDefaultSettings()
{
	settings = {};
}

void GrassCollision::PostPostLoad()
{
	Hooks::Install();
}

void GrassCollision::SetupResources()
{
	perFrame = new ConstantBuffer(ConstantBufferDesc<PerFrame>());

	for (uint textureIndex = 0; textureIndex < 2; ++textureIndex) {
		D3D11_TEXTURE2D_DESC texDesc = {
			.Width = 1024,
			.Height = 1024,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
			.SampleDesc = { .Count = 1 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS
		};

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = {
				.MostDetailedMip = 0,
				.MipLevels = 1 }
		};

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};

		const std::string deformationName = std::format("GrassCollision::Deformation{}", textureIndex);
		deformationTextures[textureIndex] = new Texture2D(texDesc, deformationName.c_str());
		deformationTextures[textureIndex]->CreateSRV(srvDesc);
		deformationTextures[textureIndex]->CreateUAV(uavDesc);

		texDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
		srvDesc.Format = texDesc.Format;
		uavDesc.Format = texDesc.Format;
		const std::string velocityName = std::format("GrassCollision::Velocity{}", textureIndex);
		velocityTextures[textureIndex] = new Texture2D(texDesc, velocityName.c_str());
		velocityTextures[textureIndex]->CreateSRV(srvDesc);
		velocityTextures[textureIndex]->CreateUAV(uavDesc);

		const float clearValue[4] = {};
		globals::d3d::context->ClearUnorderedAccessViewFloat(deformationTextures[textureIndex]->uav.get(), clearValue);
		globals::d3d::context->ClearUnorderedAccessViewFloat(velocityTextures[textureIndex]->uav.get(), clearValue);
	}

	D3D11_SAMPLER_DESC samplerDesc{};
	samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
	samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
	samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
	samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
	DX::ThrowIfFailed(globals::d3d::device->CreateSamplerState(&samplerDesc, deformationSampler.put()));
	Util::SetResourceName(deformationSampler.get(), "GrassCollision::DeformationSampler");

	{
		D3D11_BUFFER_DESC sbDesc{};
		sbDesc.Usage = D3D11_USAGE_DYNAMIC;
		sbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		sbDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		sbDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		sbDesc.StructureByteStride = sizeof(BoundingBoxPacked);
		sbDesc.ByteWidth = sizeof(BoundingBoxPacked) * MAX_BOUNDING_BOXES;
		collisionBoundingBoxes = eastl::make_unique<Buffer>(sbDesc, nullptr, "GrassCollision::BoundingBoxes");

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = MAX_BOUNDING_BOXES;
		collisionBoundingBoxes->CreateSRV(srvDesc);
	}

	{
		D3D11_BUFFER_DESC sbDesc{};
		sbDesc.Usage = D3D11_USAGE_DYNAMIC;
		sbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		sbDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		sbDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		sbDesc.StructureByteStride = sizeof(CollisionShapePacked);
		sbDesc.ByteWidth = sizeof(CollisionShapePacked) * MAX_COLLISIONS;
		collisionInstances = eastl::make_unique<Buffer>(sbDesc, nullptr, "GrassCollision::Instances");

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = MAX_COLLISIONS;
		collisionInstances->CreateSRV(srvDesc);
	}
}

bool GrassCollision::HasShaderDefine(RE::BSShader::Type shaderType)
{
	switch (shaderType) {
	case RE::BSShader::Type::Grass:
		return true;
	default:
		return false;
	}
}

void GrassCollision::Hooks::MainUpdate_QueueCollisions::thunk()
{
	func();
	globals::features::grassCollision.QueueCollisions();
}

void GrassCollision::Hooks::BSGrassShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	globals::features::grassCollision.Update();
	func(This, Pass, RenderFlags);
}

void GrassCollision::ClearShaderCache()
{
	collisionUpdateCS.Reset();
}

ID3D11ComputeShader* GrassCollision::GetCollisionUpdateCS()
{
	return collisionUpdateCS.Get(L"Data\\Shaders\\GrassCollision\\CollisionUpdateCS.hlsl", {}, "cs_5_0");
}

void GrassCollision::UpdateCollisionTexture()
{
	auto context = globals::d3d::context;
	ID3D11ShaderResourceView* nullVertexSrvs[2] = {};
	context->VSSetShaderResources(100, ARRAYSIZE(nullVertexSrvs), nullVertexSrvs);

	if (!settings.EnableGrassCollision) {
		const float clearColor[4] = {};
		for (uint textureIndex = 0; textureIndex < 2; ++textureIndex) {
			context->ClearUnorderedAccessViewFloat(deformationTextures[textureIndex]->uav.get(), clearColor);
			context->ClearUnorderedAccessViewFloat(velocityTextures[textureIndex]->uav.get(), clearColor);
		}
		return;
	}

	{
		const uint outputTextureIndex = currentTextureIndex ^ 1;
		ID3D11Buffer* buffers[1] = { perFrame->CB() };
		context->CSSetConstantBuffers(0, 1, buffers);

		ID3D11ShaderResourceView* srvs[] = {
			collisionBoundingBoxes->srv.get(),
			collisionInstances->srv.get(),
			deformationTextures[currentTextureIndex]->srv.get(),
			velocityTextures[currentTextureIndex]->srv.get()
		};

		context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

		ID3D11UnorderedAccessView* uavs[] = {
			deformationTextures[outputTextureIndex]->uav.get(),
			velocityTextures[outputTextureIndex]->uav.get()
		};
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

		if (auto* shader = GetCollisionUpdateCS()) {
			context->CSSetShader(shader, nullptr, 0);
			CS_GPU_PASS("GrassCollision::CollisionUpdate");
			context->Dispatch(1024 / 8, 1024 / 8, 1);
		} else {
			// Same no-collision fallback as the feature-disabled path above --
			// don't let the vertex shader read this frame's stale collision data.
			float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			context->ClearUnorderedAccessViewFloat(uavs[0], clearColor);
			context->ClearUnorderedAccessViewFloat(uavs[1], clearColor);
		}
		currentTextureIndex = outputTextureIndex;
	}

	context->CSSetShader(nullptr, nullptr, 0);

	ID3D11Buffer* null_buffer = nullptr;
	context->CSSetConstantBuffers(0, 1, &null_buffer);

	ID3D11ShaderResourceView* nullSrvs[4] = {};
	context->CSSetShaderResources(0, ARRAYSIZE(nullSrvs), nullSrvs);
	ID3D11UnorderedAccessView* nullUavs[2] = {};
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(nullUavs), nullUavs, nullptr);
}
#undef I18N_KEY_PREFIX
