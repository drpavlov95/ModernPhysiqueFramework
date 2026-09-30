// SPDX-License-Identifier: GPL-3.0-only
#include "tri.h"

#include <cstring>
#include <fstream>
#include <format>

namespace rfm
{
	namespace
	{
		constexpr char kMagic[8] = { 'F', 'R', 'T', 'R', 'I', '0', '0', '3' };

		// Bounded cursor over the file bytes. Every read is checked, so a
		// malformed file ends as a refusal instead of an out-of-range read.
		class Cursor
		{
		public:
			Cursor(const std::vector<std::uint8_t>& a_data) :
				_d(a_data) {}

			[[nodiscard]] bool Take(std::size_t a_n, const std::uint8_t*& a_out)
			{
				if (_pos + a_n > _d.size()) {
					return false;
				}
				a_out = _d.data() + _pos;
				_pos += a_n;
				return true;
			}

			[[nodiscard]] bool Skip(std::size_t a_n)
			{
				const std::uint8_t* ignored = nullptr;
				return Take(a_n, ignored);
			}

			[[nodiscard]] bool U32(std::uint32_t& a_out)
			{
				const std::uint8_t* p = nullptr;
				if (!Take(4, p)) {
					return false;
				}
				std::memcpy(&a_out, p, 4);
				return true;
			}

			[[nodiscard]] bool F32(float& a_out)
			{
				const std::uint8_t* p = nullptr;
				if (!Take(4, p)) {
					return false;
				}
				std::memcpy(&a_out, p, 4);
				return true;
			}

			[[nodiscard]] std::size_t Position() const { return _pos; }
			[[nodiscard]] std::size_t Size() const { return _d.size(); }

		private:
			const std::vector<std::uint8_t>& _d;
			std::size_t                      _pos{ 0 };
		};
	}

	bool TriFile::Load(const std::filesystem::path& a_path, std::string& a_error)
	{
		std::ifstream file(a_path, std::ios::binary | std::ios::ate);
		if (!file) {
			a_error = "cannot open file";
			return false;
		}
		const auto size = static_cast<std::size_t>(file.tellg());
		file.seekg(0);
		std::vector<std::uint8_t> data(size);
		if (!file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size))) {
			a_error = "cannot read file";
			return false;
		}

		Cursor            cur(data);
		const std::uint8_t* magic = nullptr;
		if (!cur.Take(8, magic) || std::memcmp(magic, kMagic, 8) != 0) {
			a_error = "not an FRTRI003 file";
			return false;
		}

		std::uint32_t header[14]{};
		for (auto& h : header) {
			if (!cur.U32(h)) {
				a_error = "truncated header";
				return false;
			}
		}
		const std::uint32_t vertexCount = header[0];
		const std::uint32_t polyTris = header[1];
		const std::uint32_t polyQuads = header[2];
		const std::uint32_t uvVerts = header[5];
		const std::uint32_t numMorphs = header[7];
		const std::uint32_t modVerts = header[9];

		if (vertexCount == 0) {
			a_error = "zero vertices";
			return false;
		}

		// Base vertices are kept; the rest is skipped by its declared size.
		const std::uint8_t* baseVertBytes = nullptr;
		if (!cur.Take(static_cast<std::size_t>(vertexCount) * 12, baseVertBytes)) {
			a_error = "truncated in the base vertex block";
			return false;
		}
		_verts.resize(vertexCount);
		for (std::uint32_t v = 0; v < vertexCount; ++v) {
			float xyz[3];
			std::memcpy(xyz, baseVertBytes + static_cast<std::size_t>(v) * 12, 12);
			_verts[v] = Vec3{ xyz[0], xyz[1], xyz[2] };
		}

		const bool skipped =
			cur.Skip(static_cast<std::size_t>(modVerts) * 12) &&     // mod verts
			cur.Skip(static_cast<std::size_t>(polyTris) * 12) &&     // faces
			cur.Skip(static_cast<std::size_t>(polyQuads) * 16) &&    // quads
			cur.Skip(static_cast<std::size_t>(uvVerts) * 8) &&       // uv
			cur.Skip(static_cast<std::size_t>(polyTris) * 12) &&     // uv faces
			cur.Skip(static_cast<std::size_t>(polyQuads) * 16);      // uv quads
		if (!skipped) {
			a_error = "truncated before the morph section";
			return false;
		}

		for (std::uint32_t m = 0; m < numMorphs; ++m) {
			std::uint32_t nameLength = 0;
			if (!cur.U32(nameLength)) {
				a_error = std::format("truncated at morph {} name length", m);
				return false;
			}
			const std::uint8_t* nameBytes = nullptr;
			if (!cur.Take(nameLength, nameBytes)) {
				a_error = std::format("truncated at morph {} name", m);
				return false;
			}
			std::string name(reinterpret_cast<const char*>(nameBytes), nameLength);
			while (!name.empty() && name.back() == '\0') {
				name.pop_back();
			}

			float multiplier = 0.0f;
			if (!cur.F32(multiplier)) {
				a_error = std::format("truncated at morph '{}' multiplier", name);
				return false;
			}

			const std::uint8_t* raw = nullptr;
			if (!cur.Take(static_cast<std::size_t>(vertexCount) * 6, raw)) {
				a_error = std::format("truncated at morph '{}' deltas", name);
				return false;
			}

			MorphDeltas deltas(vertexCount);
			for (std::uint32_t v = 0; v < vertexCount; ++v) {
				std::int16_t xyz[3]{};
				std::memcpy(xyz, raw + static_cast<std::size_t>(v) * 6, 6);
				deltas[v] = Vec3{
					static_cast<float>(xyz[0]) * multiplier,
					static_cast<float>(xyz[1]) * multiplier,
					static_cast<float>(xyz[2]) * multiplier
				};
			}
			_morphs.emplace(std::move(name), std::move(deltas));
		}

		_vertexCount = vertexCount;
		return true;
	}
}
