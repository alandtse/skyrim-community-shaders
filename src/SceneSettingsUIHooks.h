#pragma once

struct Feature;

namespace SceneSettingsUIHooks
{
	class FeatureDrawGuard
	{
	public:
		FeatureDrawGuard(Feature* feature, bool sceneControlled, bool sceneEditing = false);
		~FeatureDrawGuard();

		FeatureDrawGuard(const FeatureDrawGuard&) = delete;
		FeatureDrawGuard& operator=(const FeatureDrawGuard&) = delete;

	private:
		Feature* previousFeature = nullptr;
		bool previousSceneControlled = false;
		bool previousSceneEditing = false;
	};

	void Install();
}
