#pragma once

namespace VRNearClipMeshes
{
	/** @brief Mark listed NIF geometry during model creation, using cloneable extra data. */
	void Install();
	/** @brief Test model metadata without string matching or reference lookup during drawing. */
	bool IsIgnored(const RE::BSGeometry* geometry);
	/** @brief Select the fog clearance radius for a draw using the world camera. */
	void UpdateFogClearance(const RE::BSRenderPass* pass);
	/** @brief Number of matching model loads tagged since startup. */
	uint32_t TaggedModelCount();
}
