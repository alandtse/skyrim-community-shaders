#pragma once

#include <array>
#include <string_view>

namespace StormCallRecords
{
	inline constexpr std::string_view kPlugin = "Skyrim.esm";
	inline constexpr RE::FormID kEffectFormID = 0xE4CB6;

	/** Identifies one vanilla Storm Call rank and its relative wind strength. */
	struct Bolt
	{
		RE::FormID localFormID;
		std::string_view editorID;
		float strength;
	};

	inline constexpr std::array<Bolt, 3> kBolts{
		Bolt{ 0xE4CB7, "StormCallLightningBolt01", 0.85f },
		Bolt{ 0xE98A2, "StormCallLightningBolt02", 1.0f },
		Bolt{ 0xE98A3, "StormCallLightningBolt03", 1.2f }
	};

	/** Resolves a vanilla Storm Call bolt while tolerating form overrides. */
	[[nodiscard]] inline RE::SpellItem* ResolveBolt(RE::TESDataHandler& a_dataHandler, const Bolt& a_bolt)
	{
		if (auto* spell = a_dataHandler.LookupForm<RE::SpellItem>(a_bolt.localFormID, kPlugin))
			return spell;
		return RE::TESForm::LookupByEditorID<RE::SpellItem>(a_bolt.editorID);
	}
}
