#include "stdafx.h"
#include "PSF.h"

#include "util/asm.hpp"

LOG_CHANNEL(psf_log, "PSF");

template<>
void fmt_class_string<psf::format>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](auto fmt)
	{
		switch (fmt)
		{
		STR_CASE(psf::format::array);
		STR_CASE(psf::format::string);
		STR_CASE(psf::format::integer);
		}

		return unknown;
	});
}

template<>
void fmt_class_string<psf::error>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](auto fmt)
	{
		switch (fmt)
		{
		case psf::error::stream: return "File doesn't exist";
		case psf::error::not_psf: return "File is not of PSF format";
		case psf::error::corrupt: return "PSF is truncated or corrupted";
		default: break;
		}

		return unknown;
	});
}

namespace psf
{
	struct header_t
	{
		le_t<u32> magic;
		le_t<u32> version;
		le_t<u32> off_key_table;
		le_t<u32> off_data_table;
		le_t<u32> entries_num;
	};

	struct def_table_t
	{
		le_t<u16> key_off;
		le_t<format> param_fmt;
		le_t<u32> param_len;
		le_t<u32> param_max;
		le_t<u32> data_off;
	};


	entry::entry(format type, u32 max_size, std::string_view value)
		: m_type(type)
		, m_max_size(max_size)
		, m_value_string(value)
	{
		ensure(type == format::string || type == format::array);
		ensure(max_size);
	}

	entry::entry(u32 value)
		: m_type(format::integer)
		, m_max_size(sizeof(u32))
		, m_value_integer(value)
	{
	}

	entry::~entry()
	{
	}

	const std::string& entry::as_string() const
	{
		ensure(m_type == format::string || m_type == format::array);
		return m_value_string;
	}

	u32 entry::as_integer() const
	{
		ensure(m_type == format::integer);
		return m_value_integer;
	}

	entry& entry::operator =(std::string_view value)
	{
		ensure(m_type == format::string || m_type == format::array);
		m_value_string = value;
		return *this;
	}

	entry& entry::operator =(u32 value)
	{
		ensure(m_type == format::integer);
		m_value_integer = value;
		return *this;
	}

	u32 entry::size() const
	{
		switch (m_type)
		{
		case format::string:
		case format::array:
			return std::min(m_max_size, ::narrow<u32>(m_value_string.size() + (m_type == format::string)));

		case format::integer:
			return sizeof(u32);
		}

		fmt::throw_exception("Invalid format (0x%x)", m_type);
	}

	load_result_t load(const fs::file& stream)
	{
#define PSF_CHECK(cond, err) if (!static_cast<bool>(cond)) { if (error::err != error::stream) psf_log.error("Error loading PSF: %s%s", error::err, \
			src_loc{__builtin_LINE(), __builtin_COLUMN(), __builtin_FILE(), __builtin_FUNCTION()}); \
			result.clear(); \
			errc = error::err; \
			return pair; }

		load_result_t pair{};
		auto& [result, errc] = pair;

		PSF_CHECK(stream, stream);

		stream.seek(0);

		// Get header
		header_t header;
		PSF_CHECK(stream.read(header), not_psf);

		// Check magic and version
		PSF_CHECK(header.magic == "\0PSF"_u32, not_psf);
		PSF_CHECK(header.version == 0x101u, not_psf);
		PSF_CHECK(header.off_key_table >= sizeof(header_t), corrupt);
		PSF_CHECK(header.off_key_table <= header.off_data_table, corrupt);
		PSF_CHECK(header.off_data_table <= stream.size(), corrupt);

		// Get indices
		std::vector<def_table_t> indices;
		PSF_CHECK(stream.read<true>(indices, header.entries_num), corrupt);

		// Get keys
		std::string keys;
		PSF_CHECK(stream.seek(header.off_key_table) == header.off_key_table, corrupt);
		PSF_CHECK(stream.read<true>(keys, header.off_data_table - header.off_key_table), corrupt);

		// Load entries
		for (u32 i = 0; i < header.entries_num; ++i)
		{
			PSF_CHECK(indices[i].key_off < header.off_data_table - header.off_key_table, corrupt);

			// Get key name (null-terminated string)
			std::string key(keys.data() + indices[i].key_off);

			// Check entry
			PSF_CHECK(result.count(key) == 0, corrupt);
			PSF_CHECK(indices[i].param_len <= indices[i].param_max, corrupt);
			PSF_CHECK(indices[i].data_off < stream.size() - header.off_data_table, corrupt);
			PSF_CHECK(indices[i].param_max < stream.size() - indices[i].data_off, corrupt);

			// Seek data pointer
			stream.seek(header.off_data_table + indices[i].data_off);

			if (indices[i].param_fmt == format::integer && indices[i].param_max == sizeof(u32) && indices[i].param_len == sizeof(u32))
			{
				// Integer data
				le_t<u32> value;
				PSF_CHECK(stream.read(value), corrupt);

				result.emplace(std::piecewise_construct,
					std::forward_as_tuple(std::move(key)),
					std::forward_as_tuple(value));
			}
			else if (indices[i].param_fmt == format::string || indices[i].param_fmt == format::array)
			{
				// String/array data
				std::string value;
				PSF_CHECK(stream.read<true>(value, indices[i].param_len), corrupt);

				if (indices[i].param_fmt == format::string)
				{
					// Find null terminator
					value.resize(std::strlen(value.c_str()));
				}

				result.emplace(std::piecewise_construct,
					std::forward_as_tuple(std::move(key)),
					std::forward_as_tuple(indices[i].param_fmt, indices[i].param_max, std::move(value)));
			}
			else
			{
				// Possibly unsupported format, entry ignored
				psf_log.error("Unknown entry format (key='%s', fmt=0x%x, len=0x%x, max=0x%x)", key, indices[i].param_fmt, indices[i].param_len, indices[i].param_max);
			}
		}

#undef PSF_CHECK
		return pair;
	}

	load_result_t load(const std::string& filename)
	{
		return load(fs::file(filename));
	}

	std::vector<u8> save_object(const psf::registry& psf, std::vector<u8>&& init)
	{
		fs::file stream = fs::make_stream<std::vector<u8>>(std::move(init));

		std::vector<def_table_t> indices; indices.reserve(psf.size());

		// Generate indices and calculate key table length
		usz key_offset = 0, data_offset = 0;

		for (const auto& entry : psf)
		{
			def_table_t index;
			index.key_off = ::narrow<u32>(key_offset);
			index.param_fmt = entry.second.type();
			index.param_len = entry.second.size();
			index.param_max = entry.second.max();
			index.data_off = ::narrow<u32>(data_offset);

			// Update offsets:
			key_offset += ::narrow<u32>(entry.first.size() + 1); // key size
			data_offset += index.param_max;

			indices.push_back(index);
		}

		// Align next section (data) offset
		key_offset = utils::align(key_offset, 4);

		// Generate header
		header_t header;
		header.magic = "\0PSF"_u32;
		header.version = 0x101;
		header.off_key_table = ::narrow<u32>(sizeof(header_t) + sizeof(def_table_t) * psf.size());
		header.off_data_table = ::narrow<u32>(header.off_key_table + key_offset);
		header.entries_num = ::narrow<u32>(psf.size());

		// Save header and indices
		stream.write(header);
		stream.write(indices);

		// Save key table
		for (const auto& entry : psf)
		{
			stream.write(entry.first);
			stream.write('\0');
		}

		// Skip padding
		stream.trunc(stream.seek(header.off_data_table));

		// Save data
		for (const auto& entry : psf)
		{
			const auto fmt = entry.second.type();
			const u32 max = entry.second.max();

			if (fmt == format::integer && max == sizeof(u32))
			{
				const le_t<u32> value = entry.second.as_integer();
				stream.write(value);
			}
			else if (fmt == format::string || fmt == format::array)
			{
				const std::string& value = entry.second.as_string();
				const usz size = std::min<usz>(max, value.size());

				if (value.size() + (fmt == format::string) > max)
				{
					// TODO: check real limitations of PSF format
					psf_log.error("Entry value shrinkage (key='%s', value='%s', size=0x%zx, max=0x%x)", entry.first, value, size, max);
				}

				stream.write(value);
				stream.trunc(stream.seek(max - size, fs::seek_cur)); // Skip up to max_size
			}
			else
			{
				fmt::throw_exception("Invalid entry format (key='%s', fmt=0x%x)", entry.first, fmt);
			}
		}

		return std::move(static_cast<fs::container_stream<std::vector<u8>>*>(stream.release().get())->obj);
	}

	std::string_view get_string(const registry& psf, const std::string& key, std::string_view def)
	{
		const auto found = psf.find(key);

		if (found == psf.end() || (found->second.type() != format::string && found->second.type() != format::array))
		{
			return def;
		}

		return found->second.as_string();
	}

	u32 get_integer(const registry& psf, const std::string& key, u32 def)
	{
		const auto found = psf.find(key);

		if (found == psf.end() || found->second.type() != format::integer)
		{
			return def;
		}

		return found->second.as_integer();
	}
}
