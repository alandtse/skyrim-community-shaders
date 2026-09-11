#include "ActorWind.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string_view>

namespace ActorWind
{
	bool IsDragon(const RE::Actor& a_actor)
	{
		const auto* race = a_actor.GetRace();
		if (!race)
			return false;
		if (race->HasKeywordString("ActorTypeDragon"))
			return true;

		constexpr std::string_view dragonGraph = "dragonbehavior.hkx";
		for (const auto& behaviorGraph : race->behaviorGraphs) {
			const char* model = behaviorGraph.GetModel();
			if (!model)
				continue;
			const std::string_view path(model);
			const auto match = std::search(path.begin(), path.end(), dragonGraph.begin(), dragonGraph.end(),
				[](unsigned char a_left, unsigned char a_right) {
					return std::tolower(a_left) == std::tolower(a_right);
				});
			if (match != path.end())
				return true;
		}
		return false;
	}

	float3 GetVisualOrigin(RE::Actor& a_actor) noexcept
	{
		if (auto* root = a_actor.Get3D(false)) {
			const auto& origin = root->world.translate;
			return { origin.x, origin.y, origin.z };
		}

		auto origin = a_actor.GetPosition();
		origin.z += (a_actor.GetBoundMax().z - a_actor.GetBoundMin().z) * 0.5f;
		return { origin.x, origin.y, origin.z };
	}

	float3 GetMagicOrigin(RE::Actor& a_actor) noexcept
	{
		if (auto* caster = a_actor.GetMagicCaster(RE::MagicSystem::CastingSource::kOther)) {
			if (auto* magicNode = caster->GetMagicNode()) {
				const auto& origin = magicNode->world.translate;
				return { origin.x, origin.y, origin.z };
			}
		}
		auto origin = a_actor.GetPosition();
		origin.z += (a_actor.GetBoundMax().z - a_actor.GetBoundMin().z) * 0.7f;
		return { origin.x, origin.y, origin.z };
	}

	float3 GetAimDirection(RE::Actor& a_actor) noexcept
	{
		float aimAngle = a_actor.GetAimAngle();
		float aimHeading = a_actor.GetAimHeading();
		if (!std::isfinite(aimAngle))
			aimAngle = a_actor.GetAngleX();
		if (!std::isfinite(aimHeading))
			aimHeading = a_actor.GetAngleZ();
		const float horizontalScale = std::cos(aimAngle);
		return {
			horizontalScale * std::sin(aimHeading),
			horizontalScale * std::cos(aimHeading),
			-std::sin(aimAngle)
		};
	}
}
