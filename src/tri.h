// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Reader for FaceGen FRTRI003 head-part morph files - the format Skyrim and
// RaceMenu use for chargen morphs, and the one the author's morphs already ship
// in. Only what this mod needs is decoded: the vertex count and the named
// morphs' per-vertex deltas. Every other section is skipped by size.
//
// Layout follows fmp/tri.py (FaceMorphsPatcher), which was verified against
// AHPH, COtR and vanilla chargen TRIs, and matches SKEE's TRIFile::Load.

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace rfm
{
	struct Vec3
	{
		float x{ 0.0f }, y{ 0.0f }, z{ 0.0f };
	};

	// One named morph: a delta per vertex, already scaled into world units.
	using MorphDeltas = std::vector<Vec3>;

	class TriFile
	{
	public:
		// Returns false and fills a_error when the file is not a complete
		// FRTRI003; a truncated or unexpected file is refused rather than
		// half-read, because a wrong offset here would write nonsense into
		// somebody's face.
		bool Load(const std::filesystem::path& a_path, std::string& a_error);

		[[nodiscard]] std::uint32_t VertexCount() const { return _vertexCount; }

		// The rest-pose vertices, kept so a morph set can be checked against the
		// geometry it is about to be written into. Names cannot do that job:
		// Ordinary People's FaceGeom calls the head "KWA_FemaleHead" while the
		// head part it comes from is named "FemaleHead".
		[[nodiscard]] const std::vector<Vec3>& BaseVerts() const { return _verts; }
		[[nodiscard]] const MorphDeltas* Find(const std::string& a_morphName) const
		{
			auto it = _morphs.find(a_morphName);
			return it == _morphs.end() ? nullptr : &it->second;
		}
		[[nodiscard]] std::vector<std::string> MorphNames() const
		{
			std::vector<std::string> names;
			names.reserve(_morphs.size());
			for (const auto& [name, _] : _morphs) {
				names.push_back(name);
			}
			return names;
		}

	private:
		std::uint32_t                                  _vertexCount{ 0 };
		std::vector<Vec3>                              _verts;
		std::unordered_map<std::string, MorphDeltas>   _morphs;
	};
}
