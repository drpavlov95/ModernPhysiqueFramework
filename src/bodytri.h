// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Lightweight index reader for RaceMenu/BodySlide body-morph TRI files.
// It deliberately keeps names only: Shift+F7 needs to answer whether a live body's
// BODYTRI contains the configured morph, not retain its vertex deltas.

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace rfm
{
	struct BodyTriShape
	{
		std::string                     name;
		std::unordered_set<std::string> morphs;
	};

	class BodyTriIndex
	{
	public:
		// Supports the TRI\0 and packed TRIP formats consumed by RaceMenu/SKEE.
		// Every offset is bounds-checked; malformed assets are reported instead
		// of being partially indexed.
		bool Load(const std::filesystem::path& a_path, std::string& a_error);

		[[nodiscard]] const std::vector<BodyTriShape>& Shapes() const { return _shapes; }
		[[nodiscard]] std::size_t CountShapesWith(std::string_view a_morph) const;

	private:
		std::vector<BodyTriShape> _shapes;
	};
}
