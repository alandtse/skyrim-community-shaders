#pragma once

namespace NeuralRendering
{
	/** Updates overlay state and consumes any render-thread history-reset request. */
	void UpdateFrameState();

	/** Requests a temporal-history reset from a non-render-thread event callback. */
	void RequestHistoryReset();

	/** Resets Feature 18 temporal history while keeping the runtime initialized. */
	void ResetHistory();

	/**
	 * Runs the opt-in experimental pre-upscale route at Skyrim's native render
	 * resolution, immediately before the normal DLSS/FSR dispatch. A false
	 * result means the caller should continue with the normal upscaler; the
	 * post-upscale NR route remains the safe fallback.
	 */
	bool ApplyPreUpscale();

	/** Runs DLSS Neural Rendering on the LDR foveated regions immediately before UI composite. */
	bool ApplyFoveatedLdr();

	/** Releases all runtime and shared-resource state. */
	void Reset();
}
