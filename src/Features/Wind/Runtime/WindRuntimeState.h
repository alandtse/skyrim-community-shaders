#pragma once

#include "Buffer.h"

#include <cstdint>

enum class WindFieldDebugView : uint32_t
{
	Blended,
	Current,
	Previous,
	Both,
	Comparison,
	Spring,
	TransientImpulses
};

struct RuntimeWindTest
{
	bool enabled = false;
	float speed = 1.0f;
	float gustScale = 2048.0f;
	float gustAmplitude = 0.35f;
	float gustAdvectionMultiplier = 1.0f;
};

/** Runtime-only wind overrides and visualization controls. */
struct WindRuntimeState
{
	bool visualizeWindField = false;
	bool windFieldUseRealSpeed = true;
	bool windFieldUseRealDirection = true;
	float windFieldOverrideSpeed = 1.0f;
	float windFieldPendingDirectionDegrees = 0.0f;
	float windFieldAppliedDirectionDegrees = 0.0f;
	WindFieldDebugView windFieldDebugView = WindFieldDebugView::Blended;
	RuntimeWindTest treeWindTest;
};

struct alignas(16) WindPerFrameData
{
	uint32_t windFieldDebugEnabled;
	uint32_t windFieldDebugView;
	float2 padding;
};

STATIC_ASSERT_ALIGNAS_16(WindPerFrameData);
static_assert(sizeof(WindPerFrameData) == 16);
