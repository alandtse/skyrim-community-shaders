#pragma once

struct Feature;

namespace SceneManagerUI
{
	void Draw();
	bool CanEditFeaturePage(Feature* feature);
	bool BeginFeaturePageEditing(Feature* feature);
	bool IsFeaturePageEditing(Feature* feature);
	bool DrawFeaturePageControls(Feature* feature, bool enabled);
	/// Hide the toolbar while retaining its unsaved preview.
	void HideFeaturePageEditing();
}
