// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace mpf::ini
{
	// Replaces, inserts, or removes one INI key without reserializing unrelated
	// sections and comments. The write uses a same-directory temporary file and
	// keeps the first pre-edit version as <file>.mpf.bak.
	bool SetValue(const std::filesystem::path& a_path, std::string_view a_section,
		std::string_view a_key, const std::optional<std::string>& a_value,
		std::string& a_error);
}
