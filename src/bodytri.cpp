// SPDX-License-Identifier: GPL-3.0-only
#include "bodytri.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <format>
#include <limits>
#include <vector>

namespace rfm
{
	namespace
	{
		class Cursor
		{
		public:
			explicit Cursor(const std::vector<std::uint8_t>& a_data) :
				_data(a_data)
			{}

			[[nodiscard]] bool Take(std::size_t a_size, const std::uint8_t*& a_out)
			{
				if (_position > _data.size() || a_size > _data.size() - _position) {
					return false;
				}
				a_out = _data.data() + _position;
				_position += a_size;
				return true;
			}

			[[nodiscard]] bool Skip(std::size_t a_size)
			{
				const std::uint8_t* ignored = nullptr;
				return Take(a_size, ignored);
			}

			template <class T>
			[[nodiscard]] bool Read(T& a_out)
			{
				const std::uint8_t* bytes = nullptr;
				if (!Take(sizeof(T), bytes)) {
					return false;
				}
				std::memcpy(&a_out, bytes, sizeof(T));
				return true;
			}

			[[nodiscard]] bool String8(std::string& a_out)
			{
				std::uint8_t length = 0;
				if (!Read(length)) {
					return false;
				}
				const std::uint8_t* bytes = nullptr;
				if (!Take(length, bytes)) {
					return false;
				}
				a_out.assign(reinterpret_cast<const char*>(bytes), length);
				return true;
			}

		private:
			const std::vector<std::uint8_t>& _data;
			std::size_t                      _position{ 0 };
		};

		bool SkipRecords(Cursor& a_cursor, std::uint32_t a_count, std::size_t a_stride)
		{
			if (a_count > (std::numeric_limits<std::size_t>::max)() / a_stride) {
				return false;
			}
			return a_cursor.Skip(static_cast<std::size_t>(a_count) * a_stride);
		}
	}

	bool BodyTriIndex::Load(const std::filesystem::path& a_path, std::string& a_error)
	{
		_shapes.clear();
		a_error.clear();

		std::ifstream file(a_path, std::ios::binary | std::ios::ate);
		if (!file) {
			a_error = "cannot open file";
			return false;
		}
		const auto end = file.tellg();
		if (end < 0) {
			a_error = "cannot determine file size";
			return false;
		}
		constexpr std::size_t kMaxTriBytes = 512u * 1024u * 1024u;
		const auto size64 = static_cast<std::uintmax_t>(end);
		if (size64 > kMaxTriBytes ||
			size64 > static_cast<std::uintmax_t>((std::numeric_limits<std::streamsize>::max)())) {
			a_error = "file is unreasonably large";
			return false;
		}

		const auto size = static_cast<std::size_t>(size64);
		file.seekg(0);
		std::vector<std::uint8_t> data(size);
		if (!file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size))) {
			a_error = "cannot read file";
			return false;
		}

		Cursor cursor(data);
		const std::uint8_t* magic = nullptr;
		if (!cursor.Take(4, magic)) {
			a_error = "truncated header";
			return false;
		}
		// RaceMenu compares the header as an MSVC multi-character integer
		// ('TRIP' / 'TRI\0'). On little-endian x64 those bytes are reversed
		// in the file.
		const bool packed = std::memcmp(magic, "PIRT", 4) == 0;
		if (!packed && std::memcmp(magic, "\0IRT", 4) != 0) {
			a_error = "not a RaceMenu TRI or TRIP file";
			return false;
		}

		std::uint32_t shapeCount = 0;
		if (packed) {
			std::uint16_t count = 0;
			if (!cursor.Read(count)) {
				a_error = "truncated shape count";
				return false;
			}
			shapeCount = count;
		} else if (!cursor.Read(shapeCount)) {
			a_error = "truncated shape count";
			return false;
		}

		_shapes.reserve(shapeCount);
		for (std::uint32_t shapeIndex = 0; shapeIndex < shapeCount; ++shapeIndex) {
			BodyTriShape shape;
			if (!cursor.String8(shape.name)) {
				a_error = std::format("truncated at shape {} name", shapeIndex);
				return false;
			}
			if (!packed) {
				std::uint32_t blockSize = 0;
				if (!cursor.Read(blockSize)) {
					a_error = std::format("truncated at shape '{}' block size", shape.name);
					return false;
				}
			}

			std::uint32_t morphCount = 0;
			if (packed) {
				std::uint16_t count = 0;
				if (!cursor.Read(count)) {
					a_error = std::format("truncated at shape '{}' morph count", shape.name);
					return false;
				}
				morphCount = count;
			} else if (!cursor.Read(morphCount)) {
				a_error = std::format("truncated at shape '{}' morph count", shape.name);
				return false;
			}

			shape.morphs.reserve(morphCount);
			for (std::uint32_t morphIndex = 0; morphIndex < morphCount; ++morphIndex) {
				std::string morphName;
				if (!cursor.String8(morphName)) {
					a_error = std::format("truncated at shape '{}' morph {} name", shape.name, morphIndex);
					return false;
				}
				if (morphName.empty()) {
					a_error = std::format("shape '{}' has an empty morph name", shape.name);
					return false;
				}

				std::uint32_t vertexCount = 0;
				if (packed) {
					float multiplier = 0.0f;
					std::uint16_t count = 0;
					if (!cursor.Read(multiplier) || !cursor.Read(count)) {
						a_error = std::format("truncated at morph '{}' header", morphName);
						return false;
					}
					vertexCount = count;
				} else {
					std::uint32_t blockSize = 0;
					if (!cursor.Read(blockSize) || !cursor.Read(vertexCount)) {
						a_error = std::format("truncated at morph '{}' header", morphName);
						return false;
					}
				}

				const std::size_t stride = packed ? 8u : 16u;
				if (!SkipRecords(cursor, vertexCount, stride)) {
					a_error = std::format("truncated at morph '{}' deltas", morphName);
					return false;
				}
				shape.morphs.insert(std::move(morphName));
			}
			_shapes.push_back(std::move(shape));
		}

		return true;
	}

	std::size_t BodyTriIndex::CountShapesWith(std::string_view a_morph) const
	{
		return static_cast<std::size_t>(std::count_if(_shapes.begin(), _shapes.end(),
			[a_morph](const BodyTriShape& a_shape) {
				return a_shape.morphs.contains(std::string(a_morph));
			}));
	}
}
