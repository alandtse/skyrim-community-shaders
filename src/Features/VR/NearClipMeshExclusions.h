#pragma once

namespace VRNearClipMeshes
{
	/** @brief Mark listed NIF geometry during model creation, using cloneable extra data. */
	void Install();
	/** @brief Return whether the model metadata hook was installed. */
	bool IsInstalled();
	/** @brief Test model metadata without string matching or reference lookup during drawing. */
	bool IsIgnored(const RE::BSGeometry* geometry);
	/** @brief Test whether geometry belongs to a camera-attached fog model. */
	bool IsCameraAttachedFog(const RE::BSGeometry* geometry);
	/** @brief Return whether a recognized fog mesh draw should be skipped entirely. */
	bool ShouldSkipFogMesh(const RE::BSRenderPass* pass);
	/** @brief Select the fog clearance radius for a draw using the world camera. */
	void UpdateFogClearance(const RE::BSRenderPass* pass);
	/** @brief Report whether camera-attached fog rendered in the preceding frames. */
	bool WasCameraFogVisibleRecently(uint32_t frame, uint32_t maximumAge = 1);
	/** @brief Number of matching model loads tagged since startup. */
	uint32_t TaggedModelCount();
	/** @brief Number of recognized fog mesh draws skipped since startup. */
	uint32_t SkippedDrawCount();
}
