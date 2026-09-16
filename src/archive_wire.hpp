#ifndef PIGGLE_ARCHIVE_WIRE_HPP
#define PIGGLE_ARCHIVE_WIRE_HPP

#include "internal.hpp"
#include "digest.hpp"

static inline void pg_wire_u16(uint8_t *out, uint16_t value)
{
	out[0] = (uint8_t)value;
	out[1] = (uint8_t)(value >> 8);
}

static inline void pg_wire_u32(uint8_t *out, uint32_t value)
{
	unsigned i;

	for (i = 0; i < 4; i++)
		out[i] = (uint8_t)(value >> (8 * i));
}

static inline void pg_wire_u64(uint8_t *out, uint64_t value)
{
	unsigned i;

	for (i = 0; i < 8; i++)
		out[i] = (uint8_t)(value >> (8 * i));
}

static inline pg_status pg_wire_write(FILE *out, const void *data, size_t size)
{
	return fwrite(data, 1, size, out) == size ? PG_OK : PG_IO;
}

static inline pg_status pg_wire_zeros(FILE *out, uint64_t size)
{
	static const uint8_t zeros[4096] = {};

	while (size) {
		size_t amount = size < sizeof(zeros) ?
			(size_t)size : sizeof(zeros);

		if (pg_wire_write(out, zeros, amount) != PG_OK)
			return PG_IO;
		size -= amount;
	}
	return PG_OK;
}

static inline pg_status pg_wire_u32_write(FILE *out, uint32_t value)
{
	uint8_t bytes[4];

	pg_wire_u32(bytes, value);
	return pg_wire_write(out, bytes, sizeof(bytes));
}

static inline const char *pg_wire_name(const pg_builder_entry *entry)
{
	return entry->original_name ? entry->original_name :
		entry->canonical_name;
}

static inline pg_status pg_wire_payload(FILE *out,
		pg_archive_builder *builder, const pg_builder_entry *entry)
{
	uint8_t bytes[65536];
	uint64_t remaining = entry->stored_size;

	if (entry->stage_offset > INT64_MAX ||
	    fseeko(builder->staging, (off_t)entry->stage_offset, SEEK_SET))
		return PG_IO;
	while (remaining) {
		size_t amount = remaining < sizeof(bytes) ?
			(size_t)remaining : sizeof(bytes);

		if (fread(bytes, 1, amount, builder->staging) != amount ||
		    pg_wire_write(out, bytes, amount) != PG_OK)
			return PG_IO;
		remaining -= amount;
	}
	return PG_OK;
}

static inline pg_status pg_pigg_measure(pg_archive_builder *builder,
		uint32_t *count_out, uint32_t *names_out,
		uint32_t *headers_out, uint32_t *base_out)
{
	uint64_t count = 0;
	uint64_t names = 0;
	uint64_t headers = 0;
	uint64_t payloads = 0;
	pg_builder_entry *entry;
	uint64_t base;

	for (entry = builder->entries; entry; entry = entry->next) {
		size_t name_size = strlen(pg_wire_name(entry)) + 1;

		if (++count > UINT32_MAX || name_size > UINT32_MAX - 4 ||
		    names > UINT32_MAX - 4 - name_size ||
		    entry->logical_size > UINT32_MAX ||
		    entry->stored_size > UINT32_MAX)
			return PG_LIMIT;
		names += 4 + name_size;
		if (entry->cached_header_size) {
			if (entry->cached_header_size > UINT32_MAX - 4 ||
			    headers > UINT32_MAX - 4 -
				entry->cached_header_size)
				return PG_LIMIT;
			headers += 4 + entry->cached_header_size;
		}
		if (payloads > UINT32_MAX - entry->stored_size)
			return PG_LIMIT;
		payloads += entry->stored_size;
	}
	base = 16 + 48 * count + 12 + names + 12 + headers;
	if (base > UINT32_MAX || payloads > UINT32_MAX - base)
		return PG_LIMIT;
	*count_out = (uint32_t)count;
	*names_out = (uint32_t)names;
	*headers_out = (uint32_t)headers;
	*base_out = (uint32_t)base;
	return PG_OK;
}

static inline pg_status pg_pigg_write(pg_archive_builder *builder, FILE *out)
{
	uint32_t count;
	uint32_t names;
	uint32_t headers;
	uint32_t base;
	uint32_t payload_offset;
	uint32_t record_index = 0;
	uint32_t header_index = 0;
	pg_builder_entry *entry;
	uint8_t archive_header[16] = {};
	uint8_t pool_header[12] = {};
	pg_status status = pg_pigg_measure(builder, &count, &names,
		&headers, &base);

	if (status != PG_OK)
		return status;
	pg_wire_u32(archive_header, 0x123);
	pg_wire_u16(archive_header + 4, 2);
	pg_wire_u16(archive_header + 6, 2);
	pg_wire_u16(archive_header + 8, 16);
	pg_wire_u16(archive_header + 10, 48);
	pg_wire_u32(archive_header + 12, count);
	if (pg_wire_write(out, archive_header, sizeof(archive_header)) != PG_OK)
		return PG_IO;
	payload_offset = base;
	for (entry = builder->entries; entry; entry = entry->next) {
		uint8_t record[48] = {};

		pg_wire_u32(record, 0x3456);
		pg_wire_u32(record + 4, record_index++);
		pg_wire_u32(record + 8, (uint32_t)entry->logical_size);
		pg_wire_u32(record + 12, (uint32_t)entry->mtime);
		pg_wire_u32(record + 16, payload_offset);
		pg_wire_u32(record + 24, entry->cached_header_size ?
			header_index++ : UINT32_MAX);
		memcpy(record + 28, entry->digest, 16);
		if (entry->encoding == PG_ZLIB)
			pg_wire_u32(record + 44, (uint32_t)entry->stored_size);
		if (pg_wire_write(out, record, sizeof(record)) != PG_OK)
			return PG_IO;
		payload_offset += (uint32_t)entry->stored_size;
	}
	pg_wire_u32(pool_header, 0x6789);
	pg_wire_u32(pool_header + 4, count);
	pg_wire_u32(pool_header + 8, names);
	if (pg_wire_write(out, pool_header, sizeof(pool_header)) != PG_OK)
		return PG_IO;
	for (entry = builder->entries; entry; entry = entry->next) {
		const char *name = pg_wire_name(entry);
		uint32_t size = (uint32_t)strlen(name) + 1;

		if (pg_wire_u32_write(out, size) != PG_OK ||
		    pg_wire_write(out, name, size) != PG_OK)
			return PG_IO;
	}
	memset(pool_header, 0, sizeof(pool_header));
	pg_wire_u32(pool_header, 0x9abc);
	pg_wire_u32(pool_header + 4, header_index);
	pg_wire_u32(pool_header + 8, headers);
	if (pg_wire_write(out, pool_header, sizeof(pool_header)) != PG_OK)
		return PG_IO;
	for (entry = builder->entries; entry; entry = entry->next) {
		if (!entry->cached_header_size)
			continue;
		if (pg_wire_u32_write(out,
			(uint32_t)entry->cached_header_size) != PG_OK ||
		    pg_wire_write(out, entry->cached_header,
			entry->cached_header_size) != PG_OK)
			return PG_IO;
	}
	for (entry = builder->entries; entry; entry = entry->next) {
		if (pg_wire_payload(out, builder, entry) != PG_OK)
			return PG_IO;
	}
	return PG_OK;
}

static inline pg_status pg_hogg_measure(pg_archive_builder *builder,
		uint32_t *users_out, uint32_t *capacity_out,
		uint32_t *slots_out, uint32_t *data_size_out,
		uint64_t *base_out)
{
	uint64_t users = 0;
	uint64_t slots = 1;
	uint64_t data_size = 8 + 4 + 10;
	uint64_t payloads = 0;
	uint64_t capacity;
	pg_builder_entry *entry;

	for (entry = builder->entries; entry; entry = entry->next) {
		size_t name_size = strlen(pg_wire_name(entry)) + 1;

		if (++users >= UINT32_MAX || name_size > UINT32_MAX - 4 ||
		    data_size > UINT32_MAX - 4 - name_size ||
		    entry->stored_size > 0xfffffffeu ||
		    entry->logical_size > UINT32_MAX)
			return PG_LIMIT;
		data_size += 4 + name_size;
		slots++;
		if (entry->cached_header_size) {
			if (entry->cached_header_size > UINT32_MAX - 4 ||
			    data_size > UINT32_MAX - 4 -
				entry->cached_header_size)
				return PG_LIMIT;
			data_size += 4 + entry->cached_header_size;
			slots++;
		}
		if (payloads > INT64_MAX - entry->stored_size)
			return PG_LIMIT;
		payloads += entry->stored_size;
	}
	capacity = users + 1 < 16 ? 16 : users + 1;
	if (slots > UINT32_MAX || capacity > UINT32_MAX / 48 ||
	    4144 + 48 * capacity > INT64_MAX - payloads ||
	    data_size > INT64_MAX - (4144 + 48 * capacity + payloads))
		return PG_LIMIT;
	*users_out = (uint32_t)users;
	*capacity_out = (uint32_t)capacity;
	*slots_out = (uint32_t)slots;
	*data_size_out = (uint32_t)data_size;
	*base_out = 4144 + 48 * capacity;
	return PG_OK;
}

static inline pg_status pg_hogg_emit(FILE *out, pg_md5 *hash,
		const void *data, size_t size)
{
	pg_md5_update(hash, data, size);
	return out ? pg_wire_write(out, data, size) : PG_OK;
}

static inline pg_status pg_hogg_element(FILE *out, pg_md5 *hash,
		const void *data, uint32_t size)
{
	uint8_t length[4];

	pg_wire_u32(length, size);
	if (pg_hogg_emit(out, hash, length, sizeof(length)) != PG_OK ||
	    pg_hogg_emit(out, hash, data, size) != PG_OK)
		return PG_IO;
	return PG_OK;
}

static inline pg_status pg_hogg_datalist(pg_archive_builder *builder,
		FILE *out, uint32_t slots, uint8_t digest[16])
{
	static const char internal_name[10] = "?DataList";
	uint8_t header[8] = {};
	pg_md5 hash;
	pg_builder_entry *entry;

	pg_wire_u32(header + 4, slots);
	pg_md5_init(&hash);
	if (pg_hogg_emit(out, &hash, header, sizeof(header)) != PG_OK)
		return PG_IO;
	for (entry = builder->entries; entry; entry = entry->next) {
		const char *name = pg_wire_name(entry);
		uint32_t size = (uint32_t)strlen(name) + 1;

		if (pg_hogg_element(out, &hash, name, size) != PG_OK)
			return PG_IO;
		if (entry->cached_header_size &&
		    pg_hogg_element(out, &hash, entry->cached_header,
			(uint32_t)entry->cached_header_size) != PG_OK)
			return PG_IO;
	}
	if (pg_hogg_element(out, &hash, internal_name,
		sizeof(internal_name)) != PG_OK)
		return PG_IO;
	pg_md5_finish(&hash, digest);
	return PG_OK;
}

static inline pg_status pg_hogg_write(pg_archive_builder *builder, FILE *out)
{
	uint32_t users;
	uint32_t capacity;
	uint32_t slots;
	uint32_t data_size;
	uint64_t base;
	uint64_t offset;
	uint32_t index = 0;
	uint32_t data_id = 0;
	uint8_t datalist_digest[16];
	uint8_t header[24] = {};
	pg_builder_entry *entry;
	pg_status status = pg_hogg_measure(builder, &users, &capacity,
		&slots, &data_size, &base);

	if (status != PG_OK)
		return status;
	status = pg_hogg_datalist(builder, NULL, slots, datalist_digest);
	if (status != PG_OK)
		return status;
	pg_wire_u32(header, 0xdeadf00d);
	pg_wire_u16(header + 4, 10);
	pg_wire_u16(header + 6, 1024);
	pg_wire_u32(header + 8, capacity * 32);
	pg_wire_u32(header + 12, capacity * 16);
	pg_wire_u32(header + 16, users);
	pg_wire_u16(header + 20, 3096);
	if (pg_wire_write(out, header, sizeof(header)) != PG_OK ||
	    pg_wire_zeros(out, 1024 + 3096) != PG_OK)
		return PG_IO;
	offset = base;
	for (entry = builder->entries; entry; entry = entry->next) {
		uint8_t record[32] = {};

		pg_wire_u64(record, offset);
		pg_wire_u32(record + 8, (uint32_t)entry->stored_size);
		pg_wire_u32(record + 12, (uint32_t)entry->mtime);
		memcpy(record + 16, entry->digest, 4);
		pg_wire_u16(record + 24, 0xfffe);
		pg_wire_u32(record + 28, index++);
		if (pg_wire_write(out, record, sizeof(record)) != PG_OK)
			return PG_IO;
		offset += entry->stored_size;
	}
	uint8_t datalist_record[32] = {};

	pg_wire_u64(datalist_record, offset);
	pg_wire_u32(datalist_record + 8, data_size);
	memcpy(datalist_record + 16, datalist_digest, 4);
	pg_wire_u16(datalist_record + 24, 0xfffe);
	pg_wire_u32(datalist_record + 28, users);
	if (pg_wire_write(out, datalist_record,
		sizeof(datalist_record)) != PG_OK)
		return PG_IO;
	for (index = users + 1; index < capacity; index++) {
		uint8_t free_record[32] = {};

		pg_wire_u32(free_record + 8, UINT32_MAX);
		if (pg_wire_write(out, free_record,
			sizeof(free_record)) != PG_OK)
			return PG_IO;
	}
	for (entry = builder->entries; entry; entry = entry->next) {
		uint8_t ea[16] = {};

		pg_wire_u32(ea, data_id++);
		pg_wire_u32(ea + 4, entry->cached_header_size ?
			data_id++ : UINT32_MAX);
		if (entry->encoding == PG_ZLIB)
			pg_wire_u32(ea + 8, (uint32_t)entry->logical_size);
		if (pg_wire_write(out, ea, sizeof(ea)) != PG_OK)
			return PG_IO;
	}
	uint8_t datalist_ea[16] = {};

	pg_wire_u32(datalist_ea, data_id);
	pg_wire_u32(datalist_ea + 4, UINT32_MAX);
	if (pg_wire_write(out, datalist_ea,
		sizeof(datalist_ea)) != PG_OK)
		return PG_IO;
	for (index = users + 1; index < capacity; index++) {
		uint8_t free_ea[16] = {};

		pg_wire_u32(free_ea + 12, 1);
		if (pg_wire_write(out, free_ea, sizeof(free_ea)) != PG_OK)
			return PG_IO;
	}
	for (entry = builder->entries; entry; entry = entry->next) {
		if (pg_wire_payload(out, builder, entry) != PG_OK)
			return PG_IO;
	}
	uint8_t written_digest[16];

	if (pg_hogg_datalist(builder, out, slots,
		written_digest) != PG_OK)
		return PG_IO;
	if (memcmp(datalist_digest, written_digest, 16) != 0)
		return PG_CORRUPT;
	return PG_OK;
}

#endif
