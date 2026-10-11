// SPDX-License-Identifier: GPL-3.0-only
#include "ini_edit.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <vector>

namespace mpf::ini
{
	namespace
	{
		std::string Trimmed(std::string_view a_text)
		{
			while (!a_text.empty() && std::isspace(static_cast<unsigned char>(a_text.front()))) {
				a_text.remove_prefix(1);
			}
			while (!a_text.empty() && std::isspace(static_cast<unsigned char>(a_text.back()))) {
				a_text.remove_suffix(1);
			}
			return std::string(a_text);
		}

		std::string Lower(std::string a_text)
		{
			std::ranges::transform(a_text, a_text.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return a_text;
		}

		std::optional<std::string> SectionName(const std::string& a_line)
		{
			const auto clean = Trimmed(a_line);
			if (clean.size() < 3 || clean.front() != '[' || clean.back() != ']') {
				return std::nullopt;
			}
			return Lower(Trimmed(std::string_view(clean).substr(1, clean.size() - 2)));
		}

		bool IsKey(const std::string& a_line, const std::string& a_key)
		{
			const auto clean = Trimmed(a_line);
			if (clean.empty() || clean.front() == ';' || clean.front() == '#') {
				return false;
			}
			const auto equals = clean.find('=');
			return equals != std::string::npos &&
				Lower(Trimmed(std::string_view(clean).substr(0, equals))) == a_key;
		}

		std::vector<std::string> SplitLines(const std::string& a_text)
		{
			std::vector<std::string> lines;
			std::size_t begin = 0;
			while (begin < a_text.size()) {
				const auto end = a_text.find('\n', begin);
				auto line = a_text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
				if (!line.empty() && line.back() == '\r') {
					line.pop_back();
				}
				lines.push_back(std::move(line));
				if (end == std::string::npos) {
					break;
				}
				begin = end + 1;
			}
			return lines;
		}
	}

	bool SetValue(const std::filesystem::path& a_path, std::string_view a_section,
		std::string_view a_key, const std::optional<std::string>& a_value,
		std::string& a_error)
	{
		a_error.clear();
		if (a_section.empty() || a_key.empty()) {
			a_error = "section and key are required";
			return false;
		}

		std::error_code ec;
		std::filesystem::create_directories(a_path.parent_path(), ec);
		if (ec) {
			a_error = "cannot create the INI directory: " + ec.message();
			return false;
		}

		std::string original;
		if (std::filesystem::is_regular_file(a_path, ec)) {
			std::ifstream input(a_path, std::ios::binary);
			if (!input) {
				a_error = "cannot open the source INI";
				return false;
			}
			original.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
		}
		const bool bom = original.size() >= 3 &&
			static_cast<unsigned char>(original[0]) == 0xEF &&
			static_cast<unsigned char>(original[1]) == 0xBB &&
			static_cast<unsigned char>(original[2]) == 0xBF;
		if (bom) {
			original.erase(0, 3);
		}
		const std::string newline = original.contains("\r\n") ? "\r\n" : "\n";
		auto lines = SplitLines(original);

		const auto wantedSection = Lower(Trimmed(a_section));
		const auto wantedKey = Lower(Trimmed(a_key));
		std::size_t sectionBegin = lines.size();
		std::size_t sectionEnd = lines.size();
		for (std::size_t i = 0; i < lines.size(); ++i) {
			const auto section = SectionName(lines[i]);
			if (!section) {
				continue;
			}
			if (sectionBegin != lines.size()) {
				sectionEnd = i;
				break;
			}
			if (*section == wantedSection) {
				sectionBegin = i;
			}
		}

		if (sectionBegin == lines.size()) {
			if (!a_value) {
				return true;
			}
			if (!lines.empty() && !lines.back().empty()) {
				lines.emplace_back();
			}
			lines.push_back("[" + std::string(a_section) + "]");
			lines.push_back(std::string(a_key) + " = " + *a_value);
		} else {
			if (sectionEnd == lines.size()) {
				sectionEnd = lines.size();
			}
			std::vector<std::size_t> matches;
			for (std::size_t i = sectionBegin + 1; i < sectionEnd; ++i) {
				if (IsKey(lines[i], wantedKey)) {
					matches.push_back(i);
				}
			}
			if (a_value) {
				const std::string replacement = std::string(a_key) + " = " + *a_value;
				if (matches.empty()) {
					lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(sectionEnd), replacement);
				} else {
					lines[matches.front()] = replacement;
					for (auto it = matches.rbegin(); it != matches.rend(); ++it) {
						if (*it != matches.front()) {
							lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(*it));
						}
					}
				}
			} else {
				for (auto it = matches.rbegin(); it != matches.rend(); ++it) {
					lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(*it));
				}
			}
		}

		const auto backup = std::filesystem::path(a_path.string() + ".mpf.bak");
		if (std::filesystem::is_regular_file(a_path, ec) && !std::filesystem::exists(backup, ec)) {
			std::filesystem::copy_file(a_path, backup, std::filesystem::copy_options::none, ec);
			if (ec) {
				a_error = "cannot create the first-edit backup: " + ec.message();
				return false;
			}
		}

		const auto temporary = std::filesystem::path(a_path.string() + ".mpf.tmp");
		{
			std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
			if (!output) {
				a_error = "cannot create the temporary INI";
				return false;
			}
			if (bom) {
				output.write("\xEF\xBB\xBF", 3);
			}
			for (std::size_t i = 0; i < lines.size(); ++i) {
				output << lines[i];
				if (i + 1 < lines.size() || !lines.empty()) {
					output << newline;
				}
			}
			output.flush();
			if (!output) {
				a_error = "cannot finish writing the temporary INI";
				return false;
			}
		}

		if (!MoveFileExW(temporary.c_str(), a_path.c_str(),
				MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
			a_error = "cannot replace the source INI (Windows error " +
				std::to_string(GetLastError()) + ")";
			std::filesystem::remove(temporary, ec);
			return false;
		}
		return true;
	}
}
