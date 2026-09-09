#include "Utils/SettingsPatch.h"

#include <algorithm>
#include <cmath>
#include <set>

// Separate translation unit from SettingsPatch.cpp: these helpers use pure
// nlohmann::json recursion with no Feature dependency, so they can compile
// (and be unit-tested) without the engine headers ApplyPatch needs.
namespace Util::Settings
{
	json SelectSettingPaths(const json& values, const std::vector<std::string>& paths)
	{
		const std::set<std::string> selected(paths.begin(), paths.end());
		const auto visit = [&](auto&& self, const json& node, const json::json_pointer& parent) -> json {
			json result = json::object();
			if (!node.is_object())
				return result;
			for (const auto& [key, value] : node.items()) {
				if (key.starts_with('_'))
					continue;
				const auto path = parent / key;
				if (value.is_object()) {
					auto nested = self(self, value, path);
					if (!nested.empty())
						result[key] = std::move(nested);
				} else if (selected.contains(path.to_string())) {
					result[key] = value;
				}
			}
			return result;
		};
		return visit(visit, values, json::json_pointer{});
	}

	json SelectSettings(const json& values, const json& mask)
	{
		json selected = json::object();
		if (!values.is_object() || !mask.is_object())
			return selected;
		for (const auto& [key, masked] : mask.items()) {
			const auto value = values.find(key);
			if (value == values.end())
				continue;
			if (masked.is_object() && value->is_object()) {
				auto nested = SelectSettings(*value, masked);
				if (!nested.empty())
					selected[key] = std::move(nested);
			} else {
				selected[key] = *value;
			}
		}
		return selected;
	}

	void RestoreSettings(json& target, const json& source, const json& mask)
	{
		if (!target.is_object() || !mask.is_object())
			return;
		for (const auto& [key, masked] : mask.items()) {
			const auto value = source.is_object() ? source.find(key) : source.end();
			if (masked.is_object() && target.contains(key) && target[key].is_object()) {
				RestoreSettings(target[key], value != source.end() ? *value : json::object(), masked);
				if (target[key].empty() && value == source.end())
					target.erase(key);
			} else if (value != source.end()) {
				target[key] = *value;
			} else {
				target.erase(key);
			}
		}
	}

	json BuildUserOverride(const json& a_current, const json& a_override)
	{
		json userOverride = json::object();
		if (!a_current.is_object() || !a_override.is_object())
			return userOverride;

		for (const auto& [key, overrideValue] : a_override.items()) {
			if (!a_current.contains(key))
				continue;

			const auto& currentValue = a_current[key];
			if (currentValue.is_object() && overrideValue.is_object()) {
				json nestedOverride = BuildUserOverride(currentValue, overrideValue);
				if (!nestedOverride.empty())
					userOverride[key] = std::move(nestedOverride);
			} else if (currentValue.is_number() && overrideValue.is_number()) {
				double current = currentValue.get<double>();
				double overridden = overrideValue.get<double>();
				double tolerance = std::max(1e-6, std::abs(overridden) * 1e-5);
				if (std::abs(current - overridden) > tolerance)
					userOverride[key] = currentValue;
			} else if (currentValue != overrideValue) {
				userOverride[key] = currentValue;
			}
		}

		return userOverride;
	}

	void CollectUnknownSettingKeys(const json& a_incoming, const json& a_known,
		const std::string& a_prefix, std::vector<std::string>& a_out)
	{
		if (!a_incoming.is_object())
			return;
		for (auto it = a_incoming.begin(); it != a_incoming.end(); ++it) {
			const std::string path = a_prefix.empty() ? it.key() : a_prefix + "." + it.key();
			if (!a_known.is_object() || !a_known.contains(it.key()))
				a_out.push_back(path);
			else if (it.value().is_object())
				CollectUnknownSettingKeys(it.value(), a_known[it.key()], path, a_out);
		}
	}
}
