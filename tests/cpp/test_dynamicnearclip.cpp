#include "Features/VR/NearClipController.h"
#include "Features/VR/NearClipProjection.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <limits>

TEST_CASE("VR near extraction handles small near planes and explicit matrix layouts", "[vr][nearclip]")
{
	for (float nearDistance : { 0.01f, 0.018f, 0.1f, 1.0f, 5.0f, 13.0f }) {
		const float4x4 projection(DirectX::XMMatrixPerspectiveOffCenterLH(-nearDistance, nearDistance * 0.8f, -nearDistance * 0.9f, nearDistance, nearDistance, 6000.0f));
		const float4x4 gpu = projection.Transpose();
		REQUIRE(VRNearClipMath::PerspectiveNear(projection._33, projection._43, projection._34, projection._44) == Catch::Approx(nearDistance));
		REQUIRE(VRNearClipMath::PerspectiveNear(gpu._33, gpu._34, gpu._43, gpu._44) == Catch::Approx(nearDistance));
		const float clipZ = nearDistance * projection._33 + projection._43;
		REQUIRE(clipZ == Catch::Approx(0.0f).margin(0.00001f));
	}
	const auto& identity = float4x4::Identity;
	REQUIRE(std::isnan(VRNearClipMath::PerspectiveNear(identity._33, identity._43, identity._34, identity._44)));
	REQUIRE(std::isnan(VRNearClipMath::PerspectiveNear(0.0f, -0.1f, 1.0f, 0.0f)));
	REQUIRE(std::isnan(VRNearClipMath::PerspectiveNear(1.0f, std::numeric_limits<float>::quiet_NaN(), 1.0f, 0.0f)));
}

TEST_CASE("VR near clip attacks immediately and holds before release", "[vr][nearclip]")
{
	VRNearClipSettings settings;
	VRNearClipController controller;
	controller.current = settings.NormalNearClip;
	REQUIRE(controller.Update(0.2f, 1.0f / 90.0f, true, settings) == 0.2f);
	for (int frame = 0; frame < 20; ++frame)
		REQUIRE(controller.Update(5.0f, 1.0f / 90.0f, true, settings) == 0.2f);
	for (int frame = 0; frame < 90; ++frame)
		controller.Update(5.0f, 1.0f / 90.0f, true, settings);
	REQUIRE(controller.current > 0.2f);
	REQUIRE(controller.current < settings.NormalNearClip);
	REQUIRE(controller.Update(0.1f, 1.0f / 90.0f, true, settings) == settings.MinimumNearClip);
}

TEST_CASE("VR near clip release is independent of headset refresh rate", "[vr][nearclip]")
{
	VRNearClipSettings settings;
	const auto afterOneSecond = [&](int refreshRate) {
		VRNearClipController controller;
		controller.Reset(settings);
		controller.current = settings.MinimumNearClip;
		for (int frame = 0; frame < refreshRate; ++frame)
			controller.Update(settings.NormalNearClip, 1.0f / refreshRate, true, settings);
		return controller.current;
	};
	REQUIRE(afterOneSecond(72) == Catch::Approx(afterOneSecond(120)).margin(0.00001f));
}

TEST_CASE("VR near clip ignores missing readbacks and small release noise", "[vr][nearclip]")
{
	VRNearClipSettings settings;
	VRNearClipController controller;
	controller.current = 1.0f;
	for (int frame = 0; frame < 300; ++frame) {
		REQUIRE(controller.Update(5.0f, 0.01f, false, settings) == 1.0f);
		REQUIRE(controller.Update(1.04f, 0.01f, true, settings) == 1.0f);
	}
	REQUIRE(controller.Update(0.9f, 0.01f, true, settings) == 0.9f);
}

TEST_CASE("VR near clip sanitizes invalid configuration and obeys new limits", "[vr][nearclip]")
{
	VRNearClipSettings settings;
	settings.NormalNearClip = std::numeric_limits<float>::quiet_NaN();
	settings.MinimumNearClip = 100.0f;
	settings.NearDistanceScale = -1.0f;
	settings.RestoreSpeed = std::numeric_limits<float>::infinity();
	settings.ClampNearClipSettings();
	REQUIRE(settings.NormalNearClip == 5.0f);
	REQUIRE(settings.MinimumNearClip == settings.NormalNearClip);
	REQUIRE(settings.NearDistanceScale == 0.05f);
	REQUIRE(settings.RestoreSpeed == 0.3f);
	VRNearClipController controller;
	REQUIRE(controller.Update(0.01f, 0.01f, true, settings) == settings.MinimumNearClip);
}
