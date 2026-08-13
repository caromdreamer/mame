// license:BSD-3-Clause
// copyright-holders:Aaron Giles
/***************************************************************************

    save.cpp

    Save state management functions.

****************************************************************************

    Save state file format:

    00..07  'MAMESAVE'
    08      Format version (this is format 2)
    09      Flags
    0A..1B  Game name padded with \0
    1C..1F  Signature
    20..end Save game data (compressed)

    Data is always written as native-endian.
    Data is converted from the endiannness it was written upon load.

***************************************************************************/

#include "emu.h"
#include "emuopts.h"

#include "main.h"

#include "util/ioprocs.h"
#include "util/ioprocsfilter.h"
#include "util/multibyte.h"

#include <algorithm>
#include <array>
#include <limits>
#include <unordered_set>


//**************************************************************************
//  DEBUGGING
//**************************************************************************

#define VERBOSE 0

#define LOG(x) do { if (VERBOSE) machine().logerror x; } while (0)



//**************************************************************************
//  CONSTANTS
//**************************************************************************

const int SAVE_VERSION      = 2;
const int HEADER_SIZE       = 32;

// Available flags
enum
{
	SS_MSB_FIRST = 0x02
};

#define STATE_MAGIC_NUM         "MAMESAVE"

namespace {

constexpr std::array<u8, 8> EXTENDED_STATE_MAGIC = { 'K', 'N', 'S', 'T', 'A', 'E', 'X', '1' };
constexpr u32 EXTENDED_STATE_VERSION = 2;
constexpr size_t EXTENDED_STATE_MAX_BYTES = 64U * 1024U * 1024U;
constexpr size_t EXTENDED_STATE_MAX_NAME_BYTES = 255U;
constexpr size_t EXTENDED_STATE_MERGE_GAP = 16U;

enum : u32
{
	EXTENDED_ENCODING_BASELINE = 0,
	EXTENDED_ENCODING_FULL = 1,
	EXTENDED_ENCODING_REFERENCE = 2
};

void append_u32(std::vector<u8> &bytes, u32 value)
{
	size_t const offset = bytes.size();
	bytes.resize(offset + sizeof(value));
	put_u32le(bytes.data() + offset, value);
}

bool take_u32(u8 const *&cursor, u8 const *end, u32 &value)
{
	if (size_t(end - cursor) < sizeof(value))
		return false;
	value = get_u32le(cursor);
	cursor += sizeof(value);
	return true;
}

} // anonymous namespace

//**************************************************************************
//  INITIALIZATION
//**************************************************************************

//-------------------------------------------------
//  save_manager - constructor
//-------------------------------------------------

save_manager::save_manager(running_machine &machine)
	: m_machine(machine)
	, m_reg_allowed(true)
	, m_supported(false)
{
	m_rewind = std::make_unique<rewinder>(*this);
}


//-------------------------------------------------
//  allow_registration - allow/disallow
//  registrations to happen
//-------------------------------------------------

void save_manager::allow_registration(bool allowed)
{
	// allow/deny registration
	m_reg_allowed = allowed;
	if (!allowed)
	{
		// look for duplicates
		std::sort(m_entry_list.begin(), m_entry_list.end(),
				[] (std::unique_ptr<state_entry> const& a, std::unique_ptr<state_entry> const& b) { return a->m_name < b->m_name; });

		int dupes_found = 0;
		for (int i = 1; i < m_entry_list.size(); i++)
		{
			if (m_entry_list[i - 1]->m_name == m_entry_list[i]->m_name)
			{
				osd_printf_error("Duplicate save state registration entry (%s)\n", m_entry_list[i]->m_name);
				dupes_found++;
			}
		}

		if (dupes_found)
			fatalerror("%d duplicate save state entries found.\n", dupes_found);

		m_supported = true;
		for (device_t &device : device_enumerator(machine().root_device()))
		{
			if (device.type().emulation_flags() & device_t::flags::SAVE_UNSUPPORTED)
			{
				m_supported = false;
				break;
			}
		}

		dump_registry();

		// everything is registered by now, evaluate the savestate size
		m_rewind->clamp_capacity();
	}
}


//-------------------------------------------------
//  indexed_item - return an item with the given
//  index
//-------------------------------------------------

const char *save_manager::indexed_item(int index, void *&base, u32 &valsize, u32 &valcount, u32 &blockcount, u32 &stride) const
{
	if (index >= m_entry_list.size() || index < 0)
		return nullptr;

	state_entry *entry = m_entry_list.at(index).get();
	base = entry->m_data;
	valsize = entry->m_typesize;
	valcount = entry->m_typecount;
	blockcount = entry->m_blockcount;
	stride = entry->m_stride;

	return entry->m_name.c_str();
}


//-------------------------------------------------
//  register_presave - register a pre-save
//  function callback
//-------------------------------------------------

void save_manager::register_presave(save_prepost_delegate func)
{
	// check for invalid timing
	if (!m_reg_allowed)
		fatalerror("Attempt to register callback function after state registration is closed!\n");

	// scan for duplicates and push through to the end
	for (auto &cb : m_presave_list)
		if (cb->m_func == func)
			fatalerror("Duplicate save state function (%s/%s)\n", cb->m_func.name(), func.name());

	// allocate a new entry
	m_presave_list.push_back(std::make_unique<state_callback>(func));
}


//-------------------------------------------------
//  state_save_register_postload -
//  register a post-load function callback
//-------------------------------------------------

void save_manager::register_postload(save_prepost_delegate func)
{
	// check for invalid timing
	if (!m_reg_allowed)
		fatalerror("Attempt to register callback function after state registration is closed!\n");

	// scan for duplicates and push through to the end
	for (auto &cb : m_postload_list)
		if (cb->m_func == func)
			fatalerror("Duplicate save state function (%s/%s)\n", cb->m_func.name(), func.name());

	// allocate a new entry
	m_postload_list.push_back(std::make_unique<state_callback>(func));
}


//-------------------------------------------------
//  register_extended_state_region - register a
//  disk/bootstrap-only region and capture its clean
//  baseline immediately
//-------------------------------------------------

void save_manager::register_extended_state_region(std::string name, void *data, size_t size, bool portable_full)
{
	if (name.empty() || name.size() > EXTENDED_STATE_MAX_NAME_BYTES || !data ||
			size == 0 || size > std::numeric_limits<u32>::max())
		fatalerror("Invalid extended save state region registration (%s, %u bytes)\n", name.c_str(), u32(size));

	for (extended_state_region const &region : m_extended_regions)
		if (region.m_name == name)
			fatalerror("Duplicate extended save state region (%s)\n", name.c_str());

	extended_state_region region;
	region.m_name = std::move(name);
	region.m_data = static_cast<u8 *>(data);
	region.m_baseline.assign(region.m_data, region.m_data + size);
	region.m_baseline_crc = util::crc32_creator::simple(region.m_baseline.data(), u32(size));
	region.m_portable_full = portable_full;
	m_extended_regions.emplace_back(std::move(region));
}


//-------------------------------------------------
//  register_memory_region_overlays - capture the
//  post-initialisation ROM/data image used by MAME
//-------------------------------------------------

void save_manager::register_memory_region_overlays()
{
	// The program region is deliberately self-contained.  A state made while
	// a translated or hacked ROM is running must load on the base set without
	// needing the launcher to identify and compare two ROM archives.  Large
	// graphics/sample regions remain outside this default portable policy.
	memory_region *region = machine().memory().region_find(":maincpu");
	if (!region)
	{
		// Cartridge/slot-based systems can keep the active program image below
		// the root device (Neo Geo uses :cslot1:maincpu).  Accept a unique nested
		// maincpu region, but do not guess when a multi-slot machine exposes more
		// than one candidate.
		constexpr std::string_view suffix = ":maincpu";
		for (auto const &[tag, candidate] : machine().memory().regions())
		{
			if (tag.size() < suffix.size() ||
					tag.compare(tag.size() - suffix.size(), suffix.size(), suffix) != 0)
				continue;
			if (region)
			{
				region = nullptr;
				break;
			}
			region = candidate.get();
		}
	}
	if (region && region->base() && region->bytes())
		register_extended_state_region(
				"memory-region::maincpu",
				region->base(),
				region->bytes(),
				true);
}


//-------------------------------------------------
//  write_extended_state - serialize changed spans
//  from disk/bootstrap-only regions
//-------------------------------------------------

bool save_manager::write_extended_state(std::vector<u8> &bytes, bool self_contained) const
{
	struct encoded_region
	{
		extended_state_region const *region;
		extended_state_region const *reference;
		u32 encoding;
		std::vector<std::pair<u32, u32>> spans;
		u32 current_crc;
		u32 reference_crc;
	};

	auto difference_spans = [] (u8 const *data, u8 const *reference, size_t size)
	{
		std::vector<std::pair<u32, u32>> spans;
		size_t cursor = 0;
		while (cursor < size)
		{
			while (cursor < size && data[cursor] == reference[cursor])
				cursor++;
			if (cursor == size)
				break;

			size_t const start = cursor;
			size_t last_difference = cursor;
			for (cursor++; cursor < size; cursor++)
			{
				if (data[cursor] != reference[cursor])
					last_difference = cursor;
				else if ((cursor - last_difference) > EXTENDED_STATE_MERGE_GAP)
					break;
			}
			spans.emplace_back(u32(start), u32(last_difference + 1 - start));
		}
		return spans;
	};
	auto encoded_span_bytes = [] (std::vector<std::pair<u32, u32>> const &spans)
	{
		size_t result = spans.size() * 2 * sizeof(u32);
		for (auto const [offset, length] : spans)
			result += length;
		return result;
	};

	std::vector<encoded_region> encoded_regions;
	std::vector<extended_state_region const *> references;
	for (extended_state_region const &region : m_extended_regions)
	{
		if (!self_contained && region_is_save_backed(region))
		{
			references.emplace_back(&region);
			continue;
		}

		size_t const size = region.m_baseline.size();
		encoded_region encoded{
				&region,
				nullptr,
				EXTENDED_ENCODING_BASELINE,
				{},
				util::crc32_creator::simple(region.m_data, u32(size)),
				0 };
		if (region.m_portable_full)
		{
			size_t best_bytes = size + 2 * sizeof(u32);
			for (extended_state_region const *candidate : references)
			{
				if (candidate->m_baseline.size() != size)
					continue;
				auto spans = difference_spans(region.m_data, candidate->m_data, size);
				size_t const span_bytes = encoded_span_bytes(spans);
				if (span_bytes < best_bytes)
				{
					best_bytes = span_bytes;
					encoded.reference = candidate;
					encoded.spans = std::move(spans);
				}
			}
			if (encoded.reference)
			{
				encoded.encoding = EXTENDED_ENCODING_REFERENCE;
				encoded.reference_crc = util::crc32_creator::simple(
						encoded.reference->m_data,
						u32(size));
			}
			else
			{
				encoded.encoding = EXTENDED_ENCODING_FULL;
				encoded.spans.emplace_back(0, u32(size));
			}
		}
		else
		{
			encoded.spans = difference_spans(region.m_data, region.m_baseline.data(), size);
		}
		encoded_regions.emplace_back(std::move(encoded));
		references.emplace_back(&region);
	}

	bytes.clear();
	if (m_extended_regions.empty())
		return true;

	bytes.insert(bytes.end(), EXTENDED_STATE_MAGIC.begin(), EXTENDED_STATE_MAGIC.end());
	append_u32(bytes, EXTENDED_STATE_VERSION);
	append_u32(bytes, 0); // total size, filled after the CRC is appended
	append_u32(bytes, u32(encoded_regions.size()));
	for (encoded_region const &encoded : encoded_regions)
	{
		extended_state_region const &region = *encoded.region;
		std::string const reference_name = encoded.reference ? encoded.reference->m_name : std::string();
		append_u32(bytes, u32(region.m_name.size()));
		append_u32(bytes, u32(region.m_baseline.size()));
		append_u32(bytes, region.m_baseline_crc);
		append_u32(bytes, encoded.current_crc);
		append_u32(bytes, encoded.encoding);
		append_u32(bytes, u32(reference_name.size()));
		append_u32(bytes, encoded.reference_crc);
		append_u32(bytes, u32(encoded.spans.size()));
		bytes.insert(bytes.end(), region.m_name.begin(), region.m_name.end());
		bytes.insert(bytes.end(), reference_name.begin(), reference_name.end());
		for (auto const [offset, length] : encoded.spans)
		{
			append_u32(bytes, offset);
			append_u32(bytes, length);
			bytes.insert(bytes.end(), region.m_data + offset, region.m_data + offset + length);
			if (bytes.size() > EXTENDED_STATE_MAX_BYTES)
			{
				bytes.clear();
				return false;
			}
		}
	}

	if (bytes.size() > (EXTENDED_STATE_MAX_BYTES - sizeof(u32)))
	{
		bytes.clear();
		return false;
	}
	append_u32(bytes, util::crc32_creator::simple(bytes.data(), u32(bytes.size())));
	put_u32le(bytes.data() + 12, u32(bytes.size()));
	// The total-size field participates in the checksum, so refresh it after
	// filling the placeholder.
	put_u32le(bytes.data() + bytes.size() - sizeof(u32),
			util::crc32_creator::simple(bytes.data(), u32(bytes.size() - sizeof(u32))));
	return true;
}


//-------------------------------------------------
//  read_extended_state - validate and restore an
//  optional disk/bootstrap-only extension
//-------------------------------------------------

bool save_manager::read_extended_state(void const *source, size_t size)
{
	if (size == 0)
		return true;
	if (!source || size < (EXTENDED_STATE_MAGIC.size() + 4 * sizeof(u32)) ||
			size > EXTENDED_STATE_MAX_BYTES || size > std::numeric_limits<u32>::max())
		return false;

	u8 const *const begin = static_cast<u8 const *>(source);
	u8 const *cursor = begin;
	u8 const *const payload_end = begin + size - sizeof(u32);
	if (std::memcmp(cursor, EXTENDED_STATE_MAGIC.data(), EXTENDED_STATE_MAGIC.size()) != 0)
		return false;
	cursor += EXTENDED_STATE_MAGIC.size();

	u32 version = 0;
	u32 total_size = 0;
	u32 region_count = 0;
	if (!take_u32(cursor, payload_end, version) ||
			!take_u32(cursor, payload_end, total_size) ||
			!take_u32(cursor, payload_end, region_count) ||
			(version != 1 && version != EXTENDED_STATE_VERSION) || total_size != size ||
			region_count > m_extended_regions.size())
		return false;

	u32 const expected_crc = get_u32le(payload_end);
	if (u32(util::crc32_creator::simple(begin, u32(size - sizeof(u32)))) != expected_crc)
		return false;

	struct decoded_region
	{
		extended_state_region const *region;
		std::vector<u8> image;
	};
	std::vector<decoded_region> decoded;
	std::unordered_set<std::string> names;
	for (u32 index = 0; index < region_count; index++)
	{
		u32 name_size = 0;
		u32 region_size = 0;
		u32 baseline_crc = 0;
		u32 current_crc = 0;
		u32 encoding = EXTENDED_ENCODING_BASELINE;
		u32 reference_name_size = 0;
		u32 reference_crc = 0;
		u32 span_count = 0;
		if (!take_u32(cursor, payload_end, name_size) ||
				!take_u32(cursor, payload_end, region_size) ||
				!take_u32(cursor, payload_end, baseline_crc) ||
				!take_u32(cursor, payload_end, current_crc))
			return false;
		if (version == 1)
		{
			if (!take_u32(cursor, payload_end, span_count))
				return false;
		}
		else if (!take_u32(cursor, payload_end, encoding) ||
				!take_u32(cursor, payload_end, reference_name_size) ||
				!take_u32(cursor, payload_end, reference_crc) ||
				!take_u32(cursor, payload_end, span_count))
			return false;
		if (name_size == 0 || name_size > EXTENDED_STATE_MAX_NAME_BYTES ||
				reference_name_size > EXTENDED_STATE_MAX_NAME_BYTES ||
				size_t(payload_end - cursor) < size_t(name_size) + reference_name_size)
			return false;

		std::string name(reinterpret_cast<char const *>(cursor), name_size);
		cursor += name_size;
		std::string reference_name(reinterpret_cast<char const *>(cursor), reference_name_size);
		cursor += reference_name_size;
		if (!names.emplace(name).second)
			return false;

		auto const found = std::find_if(
				m_extended_regions.begin(),
				m_extended_regions.end(),
				[&name] (extended_state_region const &candidate) { return candidate.m_name == name; });
		if (found == m_extended_regions.end() || found->m_baseline.size() != region_size || span_count > region_size)
			return false;

		decoded_region item{ &*found, {} };
		if (version == 1 || encoding == EXTENDED_ENCODING_BASELINE)
		{
			if ((!found->m_portable_full || version != 1) && found->m_baseline_crc != baseline_crc)
				return false;
			item.image = found->m_baseline;
		}
		else if (encoding == EXTENDED_ENCODING_FULL)
		{
			if (!reference_name.empty() || reference_crc != 0 || span_count != 1)
				return false;
			item.image.assign(region_size, 0);
		}
		else if (encoding == EXTENDED_ENCODING_REFERENCE)
		{
			if (reference_name.empty())
				return false;
			auto const reference_region = std::find_if(
					m_extended_regions.begin(),
					m_extended_regions.end(),
					[&reference_name] (extended_state_region const &candidate) { return candidate.m_name == reference_name; });
			if (reference_region == m_extended_regions.end() || reference_region->m_baseline.size() != region_size)
				return false;
			auto const reference_decoded = std::find_if(
					decoded.begin(),
					decoded.end(),
					[&reference_region] (decoded_region const &candidate) { return candidate.region == &*reference_region; });
			u8 const *reference_data = reference_decoded != decoded.end()
					? reference_decoded->image.data()
					: reference_region->m_data;
			if (u32(util::crc32_creator::simple(reference_data, region_size)) != reference_crc)
				return false;
			item.image.assign(reference_data, reference_data + region_size);
		}
		else
		{
			return false;
		}

		u32 previous_end = 0;
		bool portable_full_image = found->m_portable_full && span_count == 1;
		for (u32 span = 0; span < span_count; span++)
		{
			u32 offset = 0;
			u32 length = 0;
			if (!take_u32(cursor, payload_end, offset) ||
					!take_u32(cursor, payload_end, length) ||
					length == 0 || offset < previous_end || offset > region_size ||
					length > (region_size - offset) || size_t(payload_end - cursor) < length)
				return false;
			std::memcpy(item.image.data() + offset, cursor, length);
			cursor += length;
			previous_end = offset + length;
			portable_full_image = portable_full_image && offset == 0 && length == region_size;
		}
		if (version == 1 && found->m_baseline_crc != baseline_crc && !portable_full_image)
			return false;
		if (version != 1 && encoding == EXTENDED_ENCODING_FULL && !portable_full_image)
			return false;
		if (u32(util::crc32_creator::simple(item.image.data(), u32(item.image.size()))) != current_crc)
			return false;
		decoded.emplace_back(std::move(item));
	}
	if (cursor != payload_end)
		return false;

	if (version == 1)
		for (extended_state_region const &region : m_extended_regions)
			std::memcpy(region.m_data, region.m_baseline.data(), region.m_baseline.size());
	for (decoded_region const &item : decoded)
		std::memcpy(item.region->m_data, item.image.data(), item.image.size());
	return true;
}


//-------------------------------------------------
//  entry_is_extended - identify a normal MAME save
//  registration promoted to disk/bootstrap-only
//-------------------------------------------------

bool save_manager::entry_is_extended(state_entry const &entry) const
{
	size_t const entry_size = size_t(entry.m_typesize) * entry.m_typecount * entry.m_blockcount;
	for (extended_state_region const &region : m_extended_regions)
		if (entry.m_data == region.m_data && entry_size == region.m_baseline.size())
			return true;
	return false;
}

bool save_manager::region_is_save_backed(extended_state_region const &region) const
{
	for (auto const &entry : m_entry_list)
	{
		size_t const entry_size = size_t(entry->m_typesize) * entry->m_typecount * entry->m_blockcount;
		if (entry->m_data == region.m_data && entry_size == region.m_baseline.size())
			return true;
	}
	return false;
}


//-------------------------------------------------
//  transient state helpers - fixed-size snapshots
//  for rollback/runahead that exclude extended
//  disk/bootstrap-only regions
//-------------------------------------------------

size_t save_manager::transient_state_size() const
{
	size_t size = HEADER_SIZE;
	for (auto const &entry : m_entry_list)
		if (!entry_is_extended(*entry))
			size += size_t(entry->m_typesize) * entry->m_typecount * entry->m_blockcount;
	return size;
}

u32 save_manager::transient_state_signature() const
{
	util::crc32_creator crc;
	for (auto const &entry : m_entry_list)
	{
		if (entry_is_extended(*entry))
			continue;
		crc.append(entry->m_name.data(), entry->m_name.length());
		u32 temp[4];
		temp[0] = little_endianize_int32(entry->m_typesize);
		temp[1] = little_endianize_int32(entry->m_typecount);
		temp[2] = little_endianize_int32(entry->m_blockcount);
		temp[3] = 0;
		crc.append(&temp[0], sizeof(temp));
	}
	return crc.finish();
}

bool save_manager::indexed_item_is_extended(int index) const
{
	return index >= 0 && index < m_entry_list.size() && entry_is_extended(*m_entry_list[index]);
}

save_error save_manager::write_transient_buffer(void *buf, size_t size, u32 signature)
{
	if (!buf || size != transient_state_size() || signature != transient_state_signature())
		return STATERR_WRITE_ERROR;

	u8 *cursor = static_cast<u8 *>(buf);
	u8 header[HEADER_SIZE] = { 0 };
	std::memcpy(header, STATE_MAGIC_NUM, 8);
	header[8] = SAVE_VERSION;
	header[9] = NATIVE_ENDIAN_VALUE_LE_BE(0, SS_MSB_FIRST);
	std::strncpy(reinterpret_cast<char *>(&header[0x0a]), machine().system().name, 0x1c - 0x0a);
	put_u32le(&header[0x1c], signature);
	std::memcpy(cursor, header, sizeof(header));
	cursor += sizeof(header);

	dispatch_presave();
	for (auto const &entry : m_entry_list)
	{
		if (entry_is_extended(*entry))
			continue;
		u32 const block_size = entry->m_typesize * entry->m_typecount;
		u8 const *data = static_cast<u8 const *>(entry->m_data);
		for (u32 block = 0; block < entry->m_blockcount; block++, data += entry->m_stride)
		{
			std::memcpy(cursor, data, block_size);
			cursor += block_size;
		}
	}
	return cursor == (static_cast<u8 *>(buf) + size) ? STATERR_NONE : STATERR_WRITE_ERROR;
}

save_error save_manager::read_transient_buffer(void const *buf, size_t size, u32 signature)
{
	if (!buf || size != transient_state_size() || signature != transient_state_signature())
		return STATERR_READ_ERROR;

	u8 const *cursor = static_cast<u8 const *>(buf);
	if (validate_header(cursor, machine().system().name, signature).first != STATERR_NONE)
		return STATERR_INVALID_HEADER;
	bool const flip = NATIVE_ENDIAN_VALUE_LE_BE(
			(cursor[9] & SS_MSB_FIRST) != 0,
			(cursor[9] & SS_MSB_FIRST) == 0);
	cursor += HEADER_SIZE;

	for (auto const &entry : m_entry_list)
	{
		if (entry_is_extended(*entry))
			continue;
		u32 const block_size = entry->m_typesize * entry->m_typecount;
		u8 *data = static_cast<u8 *>(entry->m_data);
		for (u32 block = 0; block < entry->m_blockcount; block++, data += entry->m_stride)
		{
			std::memcpy(data, cursor, block_size);
			cursor += block_size;
		}
		if (flip)
			entry->flip_data();
	}
	if (cursor != (static_cast<u8 const *>(buf) + size))
		return STATERR_READ_ERROR;
	dispatch_postload();
	return STATERR_NONE;
}


//-------------------------------------------------
//  save_memory - register an array of data in
//  memory
//-------------------------------------------------

void save_manager::save_memory(device_t *device, const char *module, const char *tag, u32 index, const char *name, void *val, u32 valsize, u32 valcount, u32 blockcount, u32 stride)
{
	assert(valsize == 1 || valsize == 2 || valsize == 4 || valsize == 8);
	assert(((blockcount <= 1) && (stride == 0)) || (stride >= valcount));

	// check for invalid timing
	if (!m_reg_allowed)
	{
		machine().logerror("Attempt to register save state entry after state registration is closed!\nModule %s tag %s name %s\n", module, tag, name);
		fatalerror("Attempt to register save state entry after state registration is closed!\nModule %s tag %s name %s\n", module, tag, name);
		return;
	}

	// create the full name
	std::string totalname;
	if (tag)
		totalname = string_format("%s/%s/%X/%s", module, tag, index, name);
	else
		totalname = string_format("%s/%X/%s", module, index, name);

	// insert us into the list
	m_entry_list.emplace_back(std::make_unique<state_entry>(val, std::move(totalname), device, module, tag ? tag : "", index, valsize, valcount, blockcount, stride));
}


//-------------------------------------------------
//  check_file - check if a file is a valid save
//  state
//-------------------------------------------------

std::pair<save_error, std::string> save_manager::check_file(running_machine &machine, util::core_file &file, const char *gamename)
{
	// if we want to validate the signature, compute it
	u32 sig;
	sig = machine.save().signature();

	// seek to the beginning and read the header
	file.seek(0, SEEK_SET);
	u8 header[HEADER_SIZE];
	auto const [err, actual] = read(file, header, sizeof(header));
	if (err || (actual != sizeof(header)))
		return std::make_pair(STATERR_READ_ERROR, util::string_format("Could not read %s save file header", emulator_info::get_appname()));

	// let the generic header check work out the rest
	return validate_header(header, gamename, sig);
}


//-------------------------------------------------
//  dispatch_postload - invoke all registered
//  postload callbacks for updates
//-------------------------------------------------

void save_manager::dispatch_postload()
{
	for (auto &func : m_postload_list)
		func->m_func();
}


//-------------------------------------------------
//  dispatch_presave - invoke all registered
//  presave callbacks for updates
//-------------------------------------------------

void save_manager::dispatch_presave()
{
	for (auto &func : m_presave_list)
		func->m_func();
}


//-------------------------------------------------
//  write_file - writes the data to a file
//-------------------------------------------------

save_error save_manager::write_file(util::core_file &file)
{
	util::write_stream::ptr writer;
	save_error err = do_write(
			[] (size_t total_size) { return true; },
			[&writer] (const void *data, size_t size)
			{
				auto const [filerr, written] = write(*writer, data, size);
				return !filerr;
			},
			[&file, &writer] ()
			{
				if (file.seek(0, SEEK_SET))
					return false;
				util::core_file::ptr proxy;
				std::error_condition filerr = util::core_file::open_proxy(file, proxy);
				writer = std::move(proxy);
				return !filerr && writer;
			},
			[&file, &writer] ()
			{
				writer = util::zlib_write(file, 6, 16384);
				return bool(writer);
			});
	if (STATERR_NONE != err)
		return err;

	std::vector<u8> extension;
	// The ordinary MAME stream already carries save-registered extended
	// regions.  Let the extension reference those restored images instead of
	// writing them a second time.  Explicit network bootstraps remain fully
	// self-contained through write_extended_state's default mode.
	if (!write_extended_state(extension, false))
		return STATERR_WRITE_ERROR;
	if (!extension.empty())
	{
		auto const [writeerr, written] = write(*writer, extension.data(), extension.size());
		if (writeerr || written != extension.size())
			return STATERR_WRITE_ERROR;
	}
	return writer->finalize() ? STATERR_WRITE_ERROR : STATERR_NONE;
}


//-------------------------------------------------
//  read_file - read the data from a file
//-------------------------------------------------

save_error save_manager::read_file(util::core_file &file)
{
	util::read_stream::ptr reader;
	save_error const err = do_read(
			[] (size_t total_size) { return true; },
			[&reader] (void *data, size_t size)
			{
				auto const [filerr, actual] = read(*reader, data, size);
				return !filerr && (actual == size);
			},
			[&file, &reader] ()
			{
				if (file.seek(0, SEEK_SET))
					return false;
				util::core_file::ptr proxy;
				std::error_condition filerr = util::core_file::open_proxy(file, proxy);
				reader = std::move(proxy);
				return !filerr && reader;
			},
			[&file, &reader] ()
			{
				reader = util::zlib_read(file, 16384);
				return bool(reader);
			});
	if (STATERR_NONE != err)
		return err;

	std::vector<u8> extension;
	std::array<u8, 16384> chunk;
	for (;;)
	{
		std::size_t actual = 0;
		std::error_condition const readerr = reader->read_some(chunk.data(), chunk.size(), actual);
		if (readerr)
			return STATERR_READ_ERROR;
		if (!actual)
			break;
		if (extension.size() > EXTENDED_STATE_MAX_BYTES - actual)
			return STATERR_READ_ERROR;
		extension.insert(extension.end(), chunk.begin(), chunk.begin() + actual);
	}
	return read_extended_state(extension.data(), extension.size()) ? STATERR_NONE : STATERR_READ_ERROR;
}


//-------------------------------------------------
//  write_stream - write the current machine state
//  to an output stream
//-------------------------------------------------

save_error save_manager::write_stream(std::ostream &str)
{
	return do_write(
			[] (size_t total_size) { return true; },
			[&str] (const void *data, size_t size)
			{
				return bool(str.write(reinterpret_cast<const char *>(data), size));
			},
			[] () { return true; },
			[] () { return true; });
}


//-------------------------------------------------
//  read_stream - restore the machine state from
//  an input stream
//-------------------------------------------------

save_error save_manager::read_stream(std::istream &str)
{
	return do_read(
			[] (size_t total_size) { return true; },
			[&str] (void *data, size_t size)
			{
				return bool(str.read(reinterpret_cast<char *>(data), size));
			},
			[] () { return true; },
			[] () { return true; });
}


//-------------------------------------------------
//  write_buffer - write the current machine state
//  to an allocated buffer
//-------------------------------------------------

save_error save_manager::write_buffer(void *buf, size_t size)
{
	return do_write(
			[size] (size_t total_size) { return size == total_size; },
			[ptr = reinterpret_cast<u8 *>(buf)] (const void *data, size_t size) mutable
			{
				memcpy(ptr, data, size);
				ptr += size;
				return true;
			},
			[] () { return true; },
			[] () { return true; });
}


//-------------------------------------------------
//  read_buffer - restore the machine state from a
//  buffer
//-------------------------------------------------

save_error save_manager::read_buffer(const void *buf, size_t size)
{
	const u8 *ptr = reinterpret_cast<const u8 *>(buf);
	const u8 *const end = ptr + size;
	return do_read(
			[size] (size_t total_size) { return size == total_size; },
			[&ptr, &end] (void *data, size_t size) -> bool
			{
				if ((ptr + size) > end)
					return false;
				memcpy(data, ptr, size);
				ptr += size;
				return true;
			},
			[] () { return true; },
			[] () { return true; });
}


//-------------------------------------------------
//  write_buffer_with_signature - write using a
//  known buffer size and state signature
//-------------------------------------------------

save_error save_manager::write_buffer_with_signature(void *buf, size_t size, u32 signature)
{
	return do_write_known(
			size,
			signature,
			[size] (size_t total_size) { return size == total_size; },
			[ptr = reinterpret_cast<u8 *>(buf)] (const void *data, size_t size) mutable
			{
				memcpy(ptr, data, size);
				ptr += size;
				return true;
			},
			[] () { return true; },
			[] () { return true; });
}


//-------------------------------------------------
//  read_buffer_with_signature - restore using a
//  known buffer size and state signature
//-------------------------------------------------

save_error save_manager::read_buffer_with_signature(const void *buf, size_t size, u32 signature)
{
	const u8 *ptr = reinterpret_cast<const u8 *>(buf);
	const u8 *const end = ptr + size;
	return do_read_known(
			size,
			signature,
			[size] (size_t total_size) { return size == total_size; },
			[&ptr, &end] (void *data, size_t size) -> bool
			{
				if ((ptr + size) > end)
					return false;
				memcpy(data, ptr, size);
				ptr += size;
				return true;
			},
			[] () { return true; },
			[] () { return true; });
}


//-------------------------------------------------
//  do_write - serialisation logic
//-------------------------------------------------

template <typename T, typename U, typename V, typename W>
inline save_error save_manager::do_write(T check_space, U write_block, V start_header, W start_data)
{
	// check for sufficient space
	size_t total_size = HEADER_SIZE;
	for (const auto &entry : m_entry_list)
		total_size += entry->m_typesize * entry->m_typecount * entry->m_blockcount;
	if (!check_space(total_size))
		return STATERR_WRITE_ERROR;

	// generate the header
	u8 header[HEADER_SIZE];
	memcpy(&header[0], STATE_MAGIC_NUM, 8);
	header[8] = SAVE_VERSION;
	header[9] = NATIVE_ENDIAN_VALUE_LE_BE(0, SS_MSB_FIRST);
	strncpy((char *)&header[0x0a], machine().system().name, 0x1c - 0x0a);
	u32 sig = signature();
	put_u32le(&header[0x1c], sig);

	// write the header and turn on compression for the rest of the file
	if (!start_header() || !write_block(header, sizeof(header)) || !start_data())
		return STATERR_WRITE_ERROR;

	// call the pre-save functions
	dispatch_presave();

	// then write all the data
	for (auto &entry : m_entry_list)
	{
		const u32 blocksize = entry->m_typesize * entry->m_typecount;
		const u8 *data = reinterpret_cast<const u8 *>(entry->m_data);
		for (u32 b = 0; entry->m_blockcount > b; ++b, data += entry->m_stride)
			if (!write_block(data, blocksize))
				return STATERR_WRITE_ERROR;
	}
	return STATERR_NONE;
}


//-------------------------------------------------
//  do_write_known - serialisation logic using
//  caller-provided layout metadata
//-------------------------------------------------

template <typename T, typename U, typename V, typename W>
inline save_error save_manager::do_write_known(size_t total_size, u32 signature, T check_space, U write_block, V start_header, W start_data)
{
	if (!check_space(total_size))
		return STATERR_WRITE_ERROR;

	u8 header[HEADER_SIZE];
	memcpy(&header[0], STATE_MAGIC_NUM, 8);
	header[8] = SAVE_VERSION;
	header[9] = NATIVE_ENDIAN_VALUE_LE_BE(0, SS_MSB_FIRST);
	strncpy((char *)&header[0x0a], machine().system().name, 0x1c - 0x0a);
	put_u32le(&header[0x1c], signature);

	if (!start_header() || !write_block(header, sizeof(header)) || !start_data())
		return STATERR_WRITE_ERROR;

	dispatch_presave();

	for (auto &entry : m_entry_list)
	{
		const u32 blocksize = entry->m_typesize * entry->m_typecount;
		const u8 *data = reinterpret_cast<const u8 *>(entry->m_data);
		for (u32 b = 0; entry->m_blockcount > b; ++b, data += entry->m_stride)
			if (!write_block(data, blocksize))
				return STATERR_WRITE_ERROR;
	}
	return STATERR_NONE;
}


//-------------------------------------------------
//  do_read - deserialisation logic
//-------------------------------------------------

template <typename T, typename U, typename V, typename W>
inline save_error save_manager::do_read(T check_length, U read_block, V start_header, W start_data)
{
	// check for sufficient space
	size_t total_size = HEADER_SIZE;
	for (const auto &entry : m_entry_list)
		total_size += entry->m_typesize * entry->m_typecount * entry->m_blockcount;
	if (!check_length(total_size))
		return STATERR_READ_ERROR;

	// read the header and turn on compression for the rest of the file
	u8 header[HEADER_SIZE];
	if (!start_header() || !read_block(header, sizeof(header)) || !start_data())
		return STATERR_READ_ERROR;

	// verify the header and report an error if it doesn't match
	u32 const sig = signature();
	if (validate_header(header, machine().system().name, sig).first != STATERR_NONE)
		return STATERR_INVALID_HEADER;

	// determine whether or not to flip the data when done
	const bool flip = NATIVE_ENDIAN_VALUE_LE_BE((header[9] & SS_MSB_FIRST) != 0, (header[9] & SS_MSB_FIRST) == 0);

	// read all the data, flipping if necessary
	for (auto &entry : m_entry_list)
	{
		const u32 blocksize = entry->m_typesize * entry->m_typecount;
		u8 *data = reinterpret_cast<u8 *>(entry->m_data);
		for (u32 b = 0; entry->m_blockcount > b; ++b, data += entry->m_stride)
			if (!read_block(data, blocksize))
				return STATERR_READ_ERROR;

		// handle flipping
		if (flip)
			entry->flip_data();
	}

	// call the post-load functions
	dispatch_postload();

	return STATERR_NONE;
}


//-------------------------------------------------
//  do_read_known - deserialisation logic using
//  caller-provided layout metadata
//-------------------------------------------------

template <typename T, typename U, typename V, typename W>
inline save_error save_manager::do_read_known(size_t total_size, u32 signature, T check_length, U read_block, V start_header, W start_data)
{
	if (!check_length(total_size))
		return STATERR_READ_ERROR;

	u8 header[HEADER_SIZE];
	if (!start_header() || !read_block(header, sizeof(header)) || !start_data())
		return STATERR_READ_ERROR;

	if (validate_header(header, machine().system().name, signature).first != STATERR_NONE)
		return STATERR_INVALID_HEADER;

	const bool flip = NATIVE_ENDIAN_VALUE_LE_BE((header[9] & SS_MSB_FIRST) != 0, (header[9] & SS_MSB_FIRST) == 0);

	for (auto &entry : m_entry_list)
	{
		const u32 blocksize = entry->m_typesize * entry->m_typecount;
		u8 *data = reinterpret_cast<u8 *>(entry->m_data);
		for (u32 b = 0; entry->m_blockcount > b; ++b, data += entry->m_stride)
			if (!read_block(data, blocksize))
				return STATERR_READ_ERROR;

		if (flip)
			entry->flip_data();
	}

	dispatch_postload();

	return STATERR_NONE;
}


//-------------------------------------------------
//  signature - compute the signature, which
//  is a CRC over the structure of the data
//-------------------------------------------------

u32 save_manager::signature() const
{
	// iterate over entries
	util::crc32_creator crc;
	for (auto &entry : m_entry_list)
	{
		// add the entry name to the CRC
		crc.append(entry->m_name.data(), entry->m_name.length());

		// add the type and size to the CRC
		u32 temp[4];
		temp[0] = little_endianize_int32(entry->m_typesize);
		temp[1] = little_endianize_int32(entry->m_typecount);
		temp[2] = little_endianize_int32(entry->m_blockcount);
		temp[3] = 0;
		crc.append(&temp[0], sizeof(temp));
	}
	return crc.finish();
}


//-------------------------------------------------
//  dump_registry - dump the registry to the
//  logfile
//-------------------------------------------------

void save_manager::dump_registry() const
{
	for (auto &entry : m_entry_list)
		LOG(("%s: %u x %u x %u (%u)\n", entry->m_name.c_str(), entry->m_typesize, entry->m_typecount, entry->m_blockcount, entry->m_stride));
}


//-------------------------------------------------
//  validate_header - validate the data in the
//  header
//-------------------------------------------------

std::pair<save_error, std::string> save_manager::validate_header(const u8 *header, const char *gamename, u32 signature)
{
	// check magic number
	if (memcmp(header, STATE_MAGIC_NUM, 8))
		return std::make_pair(STATERR_INVALID_HEADER, util::string_format("This is not a %s save file", emulator_info::get_appname()));

	// check save state version
	if (header[8] != SAVE_VERSION)
		return std::make_pair(STATERR_INVALID_HEADER, util::string_format("Wrong version in save file (version %d, expected %d)", header[8], SAVE_VERSION));

	// check gamename, if we were asked to
	if (gamename && strncmp(gamename, (const char *)&header[0x0a], 0x1c - 0x0a))
		return std::make_pair(STATERR_INVALID_HEADER, util::string_format("'File is not a valid savestate file for game '%s'.", gamename));

	// check signature, if we were asked to
	if (signature != 0)
	{
		u32 const rawsig = get_u32le(&header[0x1c]);
		if (signature != rawsig)
			return std::make_pair(STATERR_INVALID_HEADER, util::string_format("Incompatible save file (signature %08x, expected %08x)", rawsig, signature));
	}
	return std::make_pair(STATERR_NONE, std::string());
}


//-------------------------------------------------
//  state_callback - constructor
//-------------------------------------------------

save_manager::state_callback::state_callback(save_prepost_delegate callback)
	: m_func(std::move(callback))
{
}


//-------------------------------------------------
//  ram_state - constructor
//-------------------------------------------------

ram_state::ram_state(save_manager &save)
	: m_save(save)
	, m_data()
	, m_valid(false)
	, m_time(m_save.machine().time())
{
	m_data.reserve(get_size(save));
	m_data.clear();
	m_data.rdbuf()->clear();
	m_data.seekp(0);
	m_data.seekg(0);
}


//-------------------------------------------------
//  get_size - utility function to get the
//  uncompressed size of a state
//-------------------------------------------------

size_t ram_state::get_size(save_manager &save)
{
	size_t totalsize = 0;

	for (auto &entry : save.m_entry_list)
		totalsize += entry->m_typesize * entry->m_typecount * entry->m_blockcount;

	return totalsize + HEADER_SIZE;
}


//-------------------------------------------------
//  save - write the current machine state to the
//  allocated stream
//-------------------------------------------------

save_error ram_state::save()
{
	// initialize
	m_valid = false;
	m_data.seekp(0);

	// get the save manager to write state
	const save_error err = m_save.write_stream(m_data);
	if (err != STATERR_NONE)
		return err;

	// final confirmation
	m_valid = true;
	m_time = m_save.machine().time();

	return STATERR_NONE;
}


//-------------------------------------------------
//  load - restore the machine state from the
//  stream
//-------------------------------------------------

save_error ram_state::load()
{
	// initialize
	m_data.seekg(0);

	// get the save manager to load state
	return m_save.read_stream(m_data);
}


//-------------------------------------------------
//  rewinder - constuctor
//-------------------------------------------------

rewinder::rewinder(save_manager &save)
	: m_save(save)
	, m_enabled(save.machine().options().rewind())
	, m_capacity(save.machine().options().rewind_capacity())
	, m_current_index(REWIND_INDEX_NONE)
	, m_first_invalid_index(REWIND_INDEX_NONE)
	, m_first_time_warning(true)
	, m_first_time_note(true)
{
}


//-------------------------------------------------
//  clamp_capacity - safety checks for commandline
//  override
//-------------------------------------------------

void rewinder::clamp_capacity()
{
	if (!m_enabled)
		return;

	const size_t total = m_capacity * 1024 * 1024;
	const size_t single = ram_state::get_size(m_save);

	// can't set below zero, but allow commandline to override options' upper limit
	if (total < 0)
		m_capacity = 0;

	// if capacity is below savestate size, can't save anything
	if (total < single)
	{
		m_enabled = false;
		m_save.machine().logerror("Rewind has been disabled, because rewind capacity is smaller than savestate size.\n");
		m_save.machine().logerror("Rewind buffer size: %d bytes. Savestate size: %d bytes.\n", total, single);
		m_save.machine().popmessage("Rewind has been disabled. See error.log for details");
	}
}


//-------------------------------------------------
//  invalidate - mark all the future states as
//  invalid to prevent loading them, as the
//  current input might have changed
//-------------------------------------------------

void rewinder::invalidate()
{
	if (!m_enabled)
		return;

	// is there anything to invalidate?
	if (!current_index_is_last())
	{
		// all states starting from the current one will be invalid
		m_first_invalid_index = m_current_index;

		// actually invalidate
		for (auto it = m_state_list.begin() + m_first_invalid_index; it < m_state_list.end(); ++it)
			it->get()->m_valid = false;
	}
}


//-------------------------------------------------
//  capture - record a single state, returns true
//  on success
//-------------------------------------------------

bool rewinder::capture()
{
	if (!m_enabled)
	{
		report_error(STATERR_DISABLED, rewind_operation::SAVE);
		return false;
	}

	if (current_index_is_last())
	{
		// we need to create a new state
		std::unique_ptr<ram_state> state = std::make_unique<ram_state>(m_save);
		const save_error error = state->save();

		// validate the state
		if (error == STATERR_NONE)
			// it's safe to append
			m_state_list.push_back(std::move(state));
		else
		{
			// internal error, complain and evacuate
			report_error(error, rewind_operation::SAVE);
			return false;
		}
	}
	else
	{
		// invalidate the future states
		invalidate();

		// update the existing state
		ram_state *state = m_state_list.at(m_current_index).get();
		const save_error error = state->save();

		// validate the state
		if (error != STATERR_NONE)
		{
			// internal error, complain and evacuate
			report_error(error, rewind_operation::SAVE);
			return false;
		}
	}

	// make sure we will fit in
	if (!check_size())
		// the list keeps growing
		m_current_index++;

	// update first invalid index
	if (current_index_is_last())
		m_first_invalid_index = REWIND_INDEX_NONE;
	else
		m_first_invalid_index = m_current_index + 1;

	// success
	report_error(STATERR_NONE, rewind_operation::SAVE);
	return true;
}


//-------------------------------------------------
//  step - single step back in time, returns true
//  on success
//-------------------------------------------------

bool rewinder::step()
{
	if (!m_enabled)
	{
		report_error(STATERR_DISABLED, rewind_operation::LOAD);
		return false;
	}

	// do we have states to load?
	if (m_current_index <= REWIND_INDEX_FIRST || m_first_invalid_index == REWIND_INDEX_FIRST)
	{
		// no valid states, complain and evacuate
		report_error(STATERR_NOT_FOUND, rewind_operation::LOAD);
		return false;
	}

	// prepare to load the last valid index if we're too far ahead
	if (m_first_invalid_index > REWIND_INDEX_NONE && m_current_index > m_first_invalid_index)
		m_current_index = m_first_invalid_index;

	// step back and obtain the state pointer
	ram_state *state = m_state_list.at(--m_current_index).get();

	// try to load and report the result
	const save_error error = state->load();
	report_error(error, rewind_operation::LOAD);

	if (error == save_error::STATERR_NONE)
		return true;

	return false;
}


//-------------------------------------------------
//  check_size - shrink the state list if it is
//  about to hit the capacity. returns true if
//  the list got shrank
//-------------------------------------------------

bool rewinder::check_size()
{
	if (!m_enabled)
		return false;

	// state sizes in bytes
	const size_t singlesize = ram_state::get_size(m_save);
	size_t totalsize = m_state_list.size() * singlesize;

	// convert our limit from megabytes
	const size_t capsize = m_capacity * 1024 * 1024;

	// safety check that shouldn't be allowed to trigger
	if (totalsize > capsize)
	{
		// states to remove
		const u32 count = (totalsize - capsize) / singlesize;

		// drop everything that's beyond capacity
		m_state_list.erase(m_state_list.begin(), m_state_list.begin() + count);
	}

	// update before new check
	totalsize = m_state_list.size() * singlesize;

	// check if capacity will be hit by the newly captured state
	if (totalsize + singlesize >= capsize)
	{
		// check if we have spare states ahead
		if (!current_index_is_last())
			// no need to move states around
			return false;

		// we can now get the first state and invalidate it
		std::unique_ptr<ram_state> first(std::move(m_state_list.front()));
		first->m_valid = false;

		// move it to the end for future use
		m_state_list.push_back(std::move(first));
		m_state_list.erase(m_state_list.begin());

		if (m_first_time_note)
		{
			m_save.machine().logerror("Rewind note: Capacity has been reached. Old savestates will be erased.\n");
			m_save.machine().logerror("Capacity: %d bytes. Savestate size: %d bytes. Savestate count: %d.\n",
				totalsize, singlesize, m_state_list.size());
			m_first_time_note = false;
		}

		return true;
	}

	return false;
}


//-------------------------------------------------
//  report_error - report rewind results
//-------------------------------------------------

void rewinder::report_error(save_error error, rewind_operation operation)
{
	const char *const opname = (operation == rewind_operation::LOAD) ? "load" : "save";
	switch (error)
	{
	// internal saveload failures
	case STATERR_INVALID_HEADER:
		m_save.machine().logerror("Rewind error: Unable to %s state due to an invalid header. "
			"Make sure the save state is correct for this machine.\n", opname);
		m_save.machine().popmessage("Rewind error occured. See error.log for details.");
		break;

	case STATERR_READ_ERROR:
		m_save.machine().logerror("Rewind error: Unable to %s state due to a read error.\n", opname);
		m_save.machine().popmessage("Rewind error occured. See error.log for details.");
		break;

	case STATERR_WRITE_ERROR:
		m_save.machine().logerror("Rewind error: Unable to %s state due to a write error.\n", opname);
		m_save.machine().popmessage("Rewind error occured. See error.log for details.");
		break;

	// external saveload failures
	case STATERR_NOT_FOUND:
		if (operation == rewind_operation::LOAD)
		{
			m_save.machine().logerror("Rewind error: No rewind state to load.\n");
			m_save.machine().popmessage("Rewind error occured. See error.log for details.");
		}
		break;

	case STATERR_DISABLED:
		if (operation == rewind_operation::LOAD)
		{
			m_save.machine().logerror("Rewind error: Rewind is disabled.\n");
			m_save.machine().popmessage("Rewind error occured. See error.log for details.");
		}
		break;

	// success
	case STATERR_NONE:
		{
			const u64 supported = m_save.supported();
			const char *const warning = supported || !m_first_time_warning ? "" :
				"Rewind warning: Save states are not officially supported for this machine.\n";
			const char *const opnamed = (operation == rewind_operation::LOAD) ? "loaded" : "captured";

			// for rewinding outside of debugger, give some indication that rewind has worked, as screen doesn't update
			m_save.machine().popmessage("Rewind state %i %s.\n%s", m_current_index + 1, opnamed, warning);
			if (m_first_time_warning && operation == rewind_operation::LOAD && !supported)
			{
				m_save.machine().logerror(warning);
				m_first_time_warning = false;
			}
		}
		break;

	// something that shouldn't be allowed to happen
	default:
		m_save.machine().logerror("Error: Unknown error during state %s.\n", opname);
		m_save.machine().popmessage("Rewind error occured. See error.log for details.");
		break;
	}
}


//-------------------------------------------------
//  state_entry - constructor
//-------------------------------------------------

save_manager::state_entry::state_entry(
		void *data,
		std::string &&name, device_t *device, std::string &&module, std::string &&tag, int index,
		u8 size, u32 valcount, u32 blockcount, u32 stride)
	: m_data(data)
	, m_name(std::move(name))
	, m_device(device)
	, m_module(std::move(module))
	, m_tag(std::move(tag))
	, m_index(index)
	, m_typesize(size)
	, m_typecount(valcount)
	, m_blockcount(blockcount)
	, m_stride(stride)
{
}


//-------------------------------------------------
//  flip_data - reverse the endianness of a
//  block of data
//-------------------------------------------------

void save_manager::state_entry::flip_data()
{
	u8 *data = reinterpret_cast<u8 *>(m_data);
	for (u32 b = 0; m_blockcount > b; ++b, data += m_stride)
	{
		u16 *data16;
		u32 *data32;
		u64 *data64;

		switch (m_typesize)
		{
		case 2:
			data16 = reinterpret_cast<u16 *>(data);
			for (u32 count = 0; count < m_typecount; count++)
				data16[count] = swapendian_int16(data16[count]);
			break;

		case 4:
			data32 = reinterpret_cast<u32 *>(data);
			for (u32 count = 0; count < m_typecount; count++)
				data32[count] = swapendian_int32(data32[count]);
			break;

		case 8:
			data64 = reinterpret_cast<u64 *>(data);
			for (u32 count = 0; count < m_typecount; count++)
				data64[count] = swapendian_int64(data64[count]);
			break;
		}
	}
}
