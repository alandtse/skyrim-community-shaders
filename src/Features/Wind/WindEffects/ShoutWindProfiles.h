#pragma once

#include <array>

namespace ShoutWindProfiles
{
	/** Authored values for one rank of a directional shout wave. */
	struct DirectionalProfile
	{
		float strength;
		float distance;
		float waveHalfWidth;
		float propagationSpeed;
		float coneHalfAngle;
		float decayTime;
	};

	/** Player Fire Breath values. */
	inline constexpr std::array<DirectionalProfile, 3> kFireBreathProfiles{
		DirectionalProfile{ 0.8f, 1000.0f, 220.0f, 1800.0f, 24.0f, 0.6f },
		DirectionalProfile{ 1.1f, 1500.0f, 280.0f, 2000.0f, 28.0f, 0.75f },
		DirectionalProfile{ 1.5f, 2100.0f, 360.0f, 2200.0f, 32.0f, 0.9f }
	};

	/** Generic dragon shout values. */
	inline constexpr std::array<DirectionalProfile, 3> kGenericDragonShoutProfiles{
		DirectionalProfile{ 0.8f, 1000.0f, 220.0f, 1800.0f, 24.0f, 0.6f },
		DirectionalProfile{ 1.1f, 1500.0f, 280.0f, 2000.0f, 28.0f, 0.75f },
		DirectionalProfile{ 1.5f, 2100.0f, 360.0f, 2200.0f, 32.0f, 0.9f }
	};
}
