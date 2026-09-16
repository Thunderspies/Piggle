#ifndef PIGGLE_ARCHIVE_READ_HPP
#define PIGGLE_ARCHIVE_READ_HPP

#include "internal.hpp"
#include "digest.hpp"

#include <zlib-ng.h>

#include <limits.h>

struct pg_pool_value {
	void *data;
	uint32_t size;
};

static inline uint16_t pg_read_u16(const uint8_t *at)
{
	return (uint16_t)at[0] | ((uint16_t)at[1] << 8);
}

static inline uint32_t pg_read_u32(const uint8_t *at)
{
	return (uint32_t)at[0] | ((uint32_t)at[1] << 8) |
		((uint32_t)at[2] << 16) | ((uint32_t)at[3] << 24);
}

static inline uint64_t pg_read_u64(const uint8_t *at)
{
	return (uint64_t)pg_read_u32(at) |
		((uint64_t)pg_read_u32(at + 4) << 32);
}

static inline pg_status pg_read_span(int fd, uint64_t file_size,
		uint64_t offset, void *out, size_t size)
{
	uint8_t *bytes = (uint8_t *)out;

	if (offset > file_size || size > file_size - offset ||
	    offset > INT64_MAX || size > (uint64_t)INT64_MAX - offset)
		return PG_CORRUPT;
	while (size) {
		size_t amount = size;
		ssize_t count;

#ifdef SSIZE_MAX
		if (amount > (size_t)SSIZE_MAX)
			amount = (size_t)SSIZE_MAX;
#endif
		count = pread(fd, bytes, amount, (off_t)offset);
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0)
			return PG_IO;
		if (!count)
			return PG_RETRY;
		bytes += count;
		offset += (uint64_t)count;
		size -= (size_t)count;
	}
	return PG_OK;
}

static inline void pg_pool_free(pg_pool_value *values, uint32_t count)
{
	uint32_t i;

	if (!values)
		return;
	for (i = 0; i < count; i++)
		free(values[i].data);
	free(values);
}

static inline pg_status pg_pigg_pool(pg_source *source, uint64_t offset,
		uint32_t expected_magic, int names, pg_pool_value **values_out,
		uint32_t *count_out, uint64_t *end_out)
{
	uint8_t header[12];
	uint64_t file_size = (uint64_t)source->identity.st_size;
	uint64_t end;
	size_t cursor = 0;
	uint32_t count;
	uint32_t i;
	uint32_t bytes_size;
	uint8_t *bytes;
	pg_pool_value *values;
	pg_status status = pg_read_span(source->fd, file_size,
		offset, header, sizeof(header));

	if (status != PG_OK)
		return status;
	if (pg_read_u32(header) != expected_magic)
		return PG_CORRUPT;
	count = pg_read_u32(header + 4);
	bytes_size = pg_read_u32(header + 8);
	if (count > bytes_size / 4)
		return PG_CORRUPT;
	end = offset + sizeof(header) + bytes_size;
	if (end < offset || end > file_size ||
	    count > SIZE_MAX / sizeof(*values))
		return PG_CORRUPT;
	bytes = (uint8_t *)malloc(bytes_size ? bytes_size : 1);
	if (!bytes)
		return PG_NOMEM;
	status = pg_read_span(source->fd, file_size,
		offset + sizeof(header), bytes, bytes_size);
	if (status != PG_OK) {
		free(bytes);
		return status;
	}
	values = count ? (pg_pool_value *)calloc(count, sizeof(*values)) :
		NULL;
	if (count && !values) {
		free(bytes);
		return PG_NOMEM;
	}
	for (i = 0; i < count; i++) {
		uint32_t size;

		if (cursor > bytes_size || bytes_size - cursor < 4) {
			status = PG_CORRUPT;
			goto fail;
		}
		size = pg_read_u32(bytes + cursor);
		cursor += 4;
		if (size > bytes_size - cursor || (names && !size)) {
			status = PG_CORRUPT;
			goto fail;
		}
		values[i].size = size;
		if (size) {
			values[i].data = malloc(size);
			if (!values[i].data) {
				status = PG_NOMEM;
				goto fail;
			}
			memcpy(values[i].data, bytes + cursor, size);
		}
		if (names) {
			const char *name = (const char *)values[i].data;

			if (name[size - 1] != '\0' ||
			    memchr(name, '\0', size - 1)) {
				status = PG_CORRUPT;
				goto fail;
			}
		}
		cursor += size;
	}
	if (cursor != bytes_size) {
		status = PG_CORRUPT;
		goto fail;
	}
	free(bytes);
	*values_out = values;
	*count_out = count;
	*end_out = end;
	return PG_OK;

fail:
	free(bytes);
	pg_pool_free(values, count);
	return status;
}

static inline pg_status pg_pigg_index(pg_source *source)
{
	uint8_t header[16];
	uint64_t file_size = (uint64_t)source->identity.st_size;
	uint64_t table_end;
	uint64_t names_end;
	uint64_t metadata_end;
	uint32_t count;
	uint32_t names_count = 0;
	uint32_t headers_count = 0;
	uint32_t i;
	uint16_t archive_stride;
	uint16_t file_stride;
	uint8_t *table = NULL;
	size_t table_size;
	pg_pool_value *names = NULL;
	pg_pool_value *headers = NULL;
	pg_source_record **tail = &source->records;
	pg_status status = pg_read_span(source->fd, file_size,
		0, header, sizeof(header));

	if (status != PG_OK)
		return status;
	if (pg_read_u32(header) != 0x123 ||
	    pg_read_u16(header + 4) != 2 ||
	    pg_read_u16(header + 6) != 2)
		return PG_UNSUPPORTED;
	archive_stride = pg_read_u16(header + 8);
	file_stride = pg_read_u16(header + 10);
	count = pg_read_u32(header + 12);
	if (archive_stride < 16 || file_stride < 48)
		return PG_CORRUPT;
	table_end = (uint64_t)archive_stride + (uint64_t)count * file_stride;
	if (table_end > file_size || file_size - table_end < 12)
		return PG_CORRUPT;
	if (table_end - archive_stride > SIZE_MAX)
		return PG_LIMIT;
	table_size = (size_t)(table_end - archive_stride);
	table = (uint8_t *)malloc(table_size ? table_size : 1);
	if (!table)
		return PG_NOMEM;
	status = pg_read_span(source->fd, file_size,
		archive_stride, table, table_size);
	if (status != PG_OK)
		goto cleanup;
	status = pg_pigg_pool(source, table_end, 0x6789, 1,
		&names, &names_count, &names_end);
	if (status != PG_OK)
		goto cleanup;
	status = pg_pigg_pool(source, names_end, 0x9abc, 0,
		&headers, &headers_count, &metadata_end);
	if (status != PG_OK)
		goto cleanup;
	for (i = 0; i < count; i++) {
		const uint8_t *record = table + (size_t)i * file_stride;
		uint32_t name_id;
		uint32_t header_id;
		uint32_t encoded_size;
		uint64_t payload_offset;
		uint64_t stored_size;
		pg_source_record *copy;
		const char *original;
		size_t name_size;
		size_t required = 0;

		name_id = pg_read_u32(record + 4);
		header_id = pg_read_u32(record + 24);
		encoded_size = pg_read_u32(record + 44);
		stored_size = encoded_size ? encoded_size :
			pg_read_u32(record + 8);
		payload_offset = pg_read_u32(record + 16);
		if (pg_read_u32(record) != 0x3456 ||
		    name_id >= names_count ||
		    (header_id != UINT32_MAX && header_id >= headers_count) ||
		    payload_offset < metadata_end ||
		    payload_offset > file_size ||
		    stored_size > file_size - payload_offset) {
			status = PG_CORRUPT;
			goto cleanup;
		}
		original = (const char *)names[name_id].data;
		name_size = names[name_id].size;
		if (name_size < 2 || original[name_size - 2] == '/' ||
		    original[name_size - 2] == '\\') {
			status = PG_CORRUPT;
			goto cleanup;
		}
		copy = (pg_source_record *)calloc(1, sizeof(*copy));
		if (!copy) {
			status = PG_NOMEM;
			goto cleanup;
		}
		copy->info.original_name = (char *)malloc(name_size);
		copy->info.canonical_name = (char *)malloc(name_size);
		if (!copy->info.original_name || !copy->info.canonical_name) {
			free((void *)copy->info.original_name);
			free((void *)copy->info.canonical_name);
			free(copy);
			status = PG_NOMEM;
			goto cleanup;
		}
		memcpy((void *)copy->info.original_name, original, name_size);
		status = pg_name_normalize(source->context, original,
			(char *)copy->info.canonical_name, name_size,
			&required, NULL);
		if (status != PG_OK) {
			free((void *)copy->info.original_name);
			free((void *)copy->info.canonical_name);
			free(copy);
			status = PG_CORRUPT;
			goto cleanup;
		}
		if (header_id != UINT32_MAX && headers[header_id].size) {
			copy->info.cached_header =
				malloc(headers[header_id].size);
			if (!copy->info.cached_header) {
				free((void *)copy->info.original_name);
				free((void *)copy->info.canonical_name);
				free(copy);
				status = PG_NOMEM;
				goto cleanup;
			}
			memcpy((void *)copy->info.cached_header,
				headers[header_id].data,
				headers[header_id].size);
			copy->info.cached_header_size = headers[header_id].size;
		}
		copy->info.archive_record = i;
		copy->info.logical_size = pg_read_u32(record + 8);
		copy->info.stored_size = stored_size;
		copy->info.mtime = (int32_t)pg_read_u32(record + 12);
		copy->info.encoding = encoded_size ? PG_ZLIB : PG_LOGICAL;
		copy->info.digest_kind = PG_DIGEST_MD5;
		memcpy(copy->info.digest, record + 28, 16);
		copy->payload_offset = payload_offset;
		*tail = copy;
		tail = &copy->next;
	}
	status = PG_OK;

cleanup:
	free(table);
	pg_pool_free(names, names_count);
	pg_pool_free(headers, headers_count);
	return status;
}

static inline pg_status pg_hogg_data(pg_source *source,
		uint64_t table_base, uint64_t ea_base, uint64_t metadata_end,
		uint32_t file_count, uint32_t ea_count, uint32_t data_file,
		uint8_t **bytes_out, pg_pool_value **slots_out,
		uint32_t *count_out)
{
	uint64_t file_size = (uint64_t)source->identity.st_size;
	uint8_t file_record[32];
	uint8_t ea[16];
	uint8_t *bytes = NULL;
	pg_pool_value *slots = NULL;
	uint32_t stored_size;
	uint32_t unpacked_size;
	uint32_t ea_id;
	uint32_t count;
	uint32_t i;
	uint64_t payload_offset;
	uint64_t cursor;
	pg_status status;
	pg_md5 hash;
	uint8_t digest[16];

	if (data_file == UINT32_MAX)
		return PG_OK;
	if (data_file >= file_count)
		return PG_CORRUPT;
	status = pg_read_span(source->fd, file_size,
		table_base + (uint64_t)data_file * 32,
		file_record, sizeof(file_record));
	if (status != PG_OK)
		return status;
	stored_size = pg_read_u32(file_record + 8);
	payload_offset = pg_read_u64(file_record);
	if (stored_size == UINT32_MAX ||
	    (stored_size && (payload_offset < metadata_end ||
		payload_offset > file_size ||
		stored_size > file_size - payload_offset)))
		return PG_CORRUPT;
	if (pg_read_u16(file_record + 24) != 0xfffe)
		return PG_CORRUPT;
	ea_id = pg_read_u32(file_record + 28);
	if (ea_id >= ea_count)
		return PG_CORRUPT;
	status = pg_read_span(source->fd, file_size,
		ea_base + (uint64_t)ea_id * 16, ea, sizeof(ea));
	if (status != PG_OK)
		return status;
	if (pg_read_u32(ea + 12) & 1)
		return PG_CORRUPT;
	unpacked_size = pg_read_u32(ea + 8);
	if (stored_size < 8)
		return PG_CORRUPT;
	bytes = (uint8_t *)malloc(stored_size);
	if (!bytes)
		return PG_NOMEM;
	status = pg_read_span(source->fd, file_size,
		payload_offset, bytes, stored_size);
	if (status != PG_OK)
		goto fail;
	if (unpacked_size) {
		uint8_t *logical = (uint8_t *)malloc(unpacked_size);
		zng_stream stream = {};
		int result;

		if (!logical) {
			status = PG_NOMEM;
			goto fail;
		}
		stream.next_in = bytes;
		stream.avail_in = stored_size;
		stream.next_out = logical;
		stream.avail_out = unpacked_size;
		result = zng_inflateInit(&stream);
		if (result == Z_OK) {
			result = zng_inflate(&stream, Z_FINISH);
			zng_inflateEnd(&stream);
		}
		if (result != Z_STREAM_END || stream.avail_in ||
		    stream.avail_out) {
			free(logical);
			status = result == Z_MEM_ERROR ? PG_NOMEM :
				PG_CORRUPT;
			goto fail;
		}
		free(bytes);
		bytes = logical;
		stored_size = unpacked_size;
	}
	if (memcmp(file_record + 16, "\0\0\0\0", 4) != 0) {
		pg_md5_init(&hash);
		pg_md5_update(&hash, bytes, stored_size);
		pg_md5_finish(&hash, digest);
		if (memcmp(file_record + 16, digest, 4) != 0) {
			status = PG_CHECKSUM;
			goto fail;
		}
	}
	if (stored_size < 8 || pg_read_u32(bytes) != 0) {
		status = PG_CORRUPT;
		goto fail;
	}
	count = pg_read_u32(bytes + 4);
	if (count > (stored_size - 8) / 4 ||
	    count > SIZE_MAX / sizeof(*slots)) {
		status = PG_CORRUPT;
		goto fail;
	}
	slots = count ? (pg_pool_value *)calloc(count, sizeof(*slots)) : NULL;
	if (count && !slots) {
		status = PG_NOMEM;
		goto fail;
	}
	cursor = 8;
	for (i = 0; i < count; i++) {
		uint32_t size;

		if (cursor > stored_size || stored_size - cursor < 4) {
			status = PG_CORRUPT;
			goto fail;
		}
		size = pg_read_u32(bytes + cursor);

		cursor += 4;
		if (size > stored_size - cursor) {
			status = PG_CORRUPT;
			goto fail;
		}
		slots[i].data = bytes + cursor;
		slots[i].size = size;
		cursor += size;
	}
	if (cursor != stored_size) {
		status = PG_CORRUPT;
		goto fail;
	}
	*bytes_out = bytes;
	*slots_out = slots;
	*count_out = count;
	return PG_OK;

fail:
	free(slots);
	free(bytes);
	return status;
}

static inline pg_status pg_hogg_index(pg_source *source)
{
	uint64_t file_size = (uint64_t)source->identity.st_size;
	uint8_t header[24];
	uint8_t journal[12];
	uint8_t *data_bytes = NULL;
	uint8_t *journal_bytes = NULL;
	pg_pool_value *slots = NULL;
	uint64_t table_base;
	uint64_t ea_base;
	uint64_t metadata_end;
	uint32_t data_count = 0;
	uint32_t file_count;
	uint32_t ea_count;
	uint32_t data_file;
	uint32_t i;
	pg_source_record **tail = &source->records;
	pg_status status = pg_read_span(source->fd, file_size,
		0, header, sizeof(header));

	if (status != PG_OK)
		return status;
	if (pg_read_u32(header) != 0xdeadf00d ||
	    pg_read_u16(header + 4) != 10)
		return PG_UNSUPPORTED;
	uint32_t op_size = pg_read_u16(header + 6);
	uint32_t file_bytes = pg_read_u32(header + 8);
	uint32_t ea_bytes = pg_read_u32(header + 12);
	uint32_t dl_size = pg_read_u16(header + 20);

	if (op_size < 8 || dl_size < 12 || file_bytes % 32 ||
	    ea_bytes % 16)
		return PG_CORRUPT;
	table_base = 24 + op_size + dl_size;
	ea_base = table_base + file_bytes;
	metadata_end = ea_base + ea_bytes;
	if (metadata_end > file_size)
		return PG_CORRUPT;
	file_count = file_bytes / 32;
	ea_count = ea_bytes / 16;
	data_file = pg_read_u32(header + 16);
	status = pg_read_span(source->fd, file_size, 24,
		journal, sizeof(uint32_t));
	if (status != PG_OK)
		return status;
	uint32_t frame_size = pg_read_u32(journal);

	if (frame_size && frame_size <= op_size - 8) {
		status = pg_read_span(source->fd, file_size,
			24 + 4 + frame_size, journal, 4);
		if (status != PG_OK)
			return status;
		if (pg_read_u32(journal) == 0xdeabac05)
			return PG_RECOVERY_REQUIRED;
	}
	status = pg_read_span(source->fd, file_size,
		24 + op_size, journal, sizeof(journal));
	if (status != PG_OK)
		return status;
	uint32_t selected = pg_read_u32(journal) ?
		pg_read_u32(journal + 8) : pg_read_u32(journal + 4);

	if (selected > dl_size - sizeof(journal))
		return PG_CORRUPT;
	status = pg_hogg_data(source, table_base, ea_base, metadata_end,
		file_count, ea_count, data_file, &data_bytes, &slots,
		&data_count);
	if (status != PG_OK)
		return status;
	if (selected) {
		uint32_t cursor = 0;

		journal_bytes = (uint8_t *)malloc(selected);
		if (!journal_bytes) {
			status = PG_NOMEM;
			goto cleanup;
		}
		status = pg_read_span(source->fd, file_size,
			24 + op_size + sizeof(journal),
			journal_bytes, selected);
		if (status != PG_OK)
			goto cleanup;
		while (cursor < selected) {
			uint8_t action = journal_bytes[cursor++];
			uint32_t slot;

			if (selected - cursor < 4) {
				status = PG_CORRUPT;
				goto cleanup;
			}
			slot = pg_read_u32(journal_bytes + cursor);
			cursor += 4;
			if (slot > INT32_MAX) {
				status = PG_CORRUPT;
				goto cleanup;
			}
			if (action == 1) {
				uint32_t length;

				if (selected - cursor < 4) {
					status = PG_CORRUPT;
					goto cleanup;
				}
				length = pg_read_u32(journal_bytes +
					cursor);
				cursor += 4;
				if (!length || length > selected - cursor) {
					status = PG_CORRUPT;
					goto cleanup;
				}
				if (slot >= data_count) {
					pg_pool_value *grown;
					size_t count = (size_t)slot + 1;

					if (count > SIZE_MAX / sizeof(*grown)) {
						status = PG_LIMIT;
						goto cleanup;
					}
					grown = (pg_pool_value *)realloc(
						slots, count * sizeof(*grown));
					if (!grown) {
						status = PG_NOMEM;
						goto cleanup;
					}
					memset(grown + data_count, 0,
						(count - data_count) *
						sizeof(*grown));
					slots = grown;
					data_count = (uint32_t)count;
				}
				slots[slot].data = journal_bytes + cursor;
				slots[slot].size = length;
				cursor += length;
			} else if (action == 2) {
				if (slot < data_count) {
					slots[slot].data = NULL;
					slots[slot].size = 0;
				}
			} else {
				status = PG_UNSUPPORTED;
				goto cleanup;
			}
		}
	}
	for (i = 0; i < file_count; i++) {
		uint8_t record[32];
		uint8_t ea[16];
		uint32_t stored_size;
		uint32_t ea_id;
		uint32_t name_id;
		uint32_t header_id;
		uint32_t unpacked_size;
		uint64_t payload_offset;
		const char *original;
		size_t name_size;
		size_t required = 0;
		pg_source_record *copy;

		status = pg_read_span(source->fd, file_size,
			table_base + (uint64_t)i * 32,
			record, sizeof(record));
		if (status != PG_OK)
			goto cleanup;
		stored_size = pg_read_u32(record + 8);
		if (stored_size == UINT32_MAX)
			continue;
		payload_offset = pg_read_u64(record);
		if (stored_size && (payload_offset < metadata_end ||
		    payload_offset > file_size ||
		    stored_size > file_size - payload_offset)) {
			status = PG_CORRUPT;
			goto cleanup;
		}
		if (i == data_file)
			continue;
		if (pg_read_u16(record + 24) != 0xfffe)
			continue;
		ea_id = pg_read_u32(record + 28);
		if (ea_id == UINT32_MAX)
			continue;
		if (ea_id >= ea_count) {
			status = PG_CORRUPT;
			goto cleanup;
		}
		status = pg_read_span(source->fd, file_size,
			ea_base + (uint64_t)ea_id * 16, ea, sizeof(ea));
		if (status != PG_OK)
			goto cleanup;
		if (pg_read_u32(ea + 12) & 1) {
			status = PG_CORRUPT;
			goto cleanup;
		}
		name_id = pg_read_u32(ea);
		header_id = pg_read_u32(ea + 4);
		unpacked_size = pg_read_u32(ea + 8);
		if (name_id == UINT32_MAX)
			continue;
		if (name_id >= data_count || !slots[name_id].size ||
		    (header_id != UINT32_MAX &&
			(header_id >= data_count || !slots[header_id].size))) {
			status = PG_CORRUPT;
			goto cleanup;
		}
		original = (const char *)slots[name_id].data;
		name_size = slots[name_id].size;
		if (name_size < 2 || original[name_size - 1] != '\0' ||
		    memchr(original, '\0', name_size - 1) ||
		    original[name_size - 2] == '/' ||
		    original[name_size - 2] == '\\') {
			status = PG_CORRUPT;
			goto cleanup;
		}
		copy = (pg_source_record *)calloc(1, sizeof(*copy));
		if (!copy) {
			status = PG_NOMEM;
			goto cleanup;
		}
		copy->info.original_name = (char *)malloc(name_size);
		copy->info.canonical_name = (char *)malloc(name_size);
		if (!copy->info.original_name || !copy->info.canonical_name) {
			free((void *)copy->info.original_name);
			free((void *)copy->info.canonical_name);
			free(copy);
			status = PG_NOMEM;
			goto cleanup;
		}
		memcpy((void *)copy->info.original_name, original, name_size);
		status = pg_name_normalize(source->context, original,
			(char *)copy->info.canonical_name, name_size,
			&required, NULL);
		if (status != PG_OK) {
			free((void *)copy->info.original_name);
			free((void *)copy->info.canonical_name);
			free(copy);
			status = PG_CORRUPT;
			goto cleanup;
		}
		if (strcmp(copy->info.canonical_name, "?datalist") == 0) {
			free((void *)copy->info.original_name);
			free((void *)copy->info.canonical_name);
			free(copy);
			status = PG_CORRUPT;
			goto cleanup;
		}
		if (header_id != UINT32_MAX) {
			copy->info.cached_header =
				malloc(slots[header_id].size);
			if (!copy->info.cached_header) {
				free((void *)copy->info.original_name);
				free((void *)copy->info.canonical_name);
				free(copy);
				status = PG_NOMEM;
				goto cleanup;
			}
			memcpy((void *)copy->info.cached_header,
				slots[header_id].data,
				slots[header_id].size);
			copy->info.cached_header_size = slots[header_id].size;
		}
		copy->info.archive_record = i;
		copy->info.logical_size = unpacked_size ? unpacked_size :
			stored_size;
		copy->info.stored_size = stored_size;
		copy->info.mtime = (int32_t)pg_read_u32(record + 12);
		copy->info.encoding = unpacked_size ? PG_ZLIB : PG_LOGICAL;
		copy->info.digest_kind = PG_DIGEST_MD5_32;
		memcpy(copy->info.digest, record + 16, 4);
		copy->payload_offset = payload_offset;
		*tail = copy;
		tail = &copy->next;
	}
	status = PG_OK;

cleanup:
	free(slots);
	free(data_bytes);
	free(journal_bytes);
	return status;
}

static inline pg_status pg_archive_verify_payload(int fd,
		uint64_t file_size, const pg_file_info *info,
		uint64_t payload_offset, int explicit_verify)
{
	uint8_t input[65536];
	uint8_t output[65536];
	uint8_t digest[16];
	pg_md5 hash;
	uint64_t remaining = info->stored_size;
	uint64_t decoded = 0;
	pg_status status = PG_OK;
	int ended = info->encoding == PG_LOGICAL;
	zng_stream stream = {};
	int initialized = 0;

	pg_md5_init(&hash);
	if (info->encoding == PG_ZLIB) {
		int result = zng_inflateInit(&stream);

		if (result != Z_OK)
			return result == Z_MEM_ERROR ? PG_NOMEM : PG_CORRUPT;
		initialized = 1;
	}
	while (remaining) {
		size_t amount = remaining < sizeof(input) ?
			(size_t)remaining : sizeof(input);

		status = pg_read_span(fd, file_size,
			payload_offset + info->stored_size - remaining,
			input, amount);
		if (status != PG_OK)
			break;
		remaining -= amount;
		if (info->encoding == PG_LOGICAL) {
			if (amount > info->logical_size - decoded) {
				status = PG_CORRUPT;
				break;
			}
			pg_md5_update(&hash, input, amount);
			decoded += amount;
			continue;
		}
		stream.next_in = input;
		stream.avail_in = (uint32_t)amount;
		do {
			uint32_t before = stream.avail_in;
			size_t produced;
			int result;

			stream.next_out = output;
			stream.avail_out = sizeof(output);
			result = zng_inflate(&stream, Z_NO_FLUSH);
			produced = sizeof(output) - stream.avail_out;
			if (produced > info->logical_size - decoded) {
				status = PG_CORRUPT;
				break;
			}
			pg_md5_update(&hash, output, produced);
			decoded += produced;
			if (result == Z_STREAM_END) {
				ended = 1;
				if (stream.avail_in || remaining)
					status = PG_CORRUPT;
				break;
			}
			if (result != Z_OK && result != Z_BUF_ERROR) {
				status = result == Z_MEM_ERROR ?
					PG_NOMEM : PG_CORRUPT;
				break;
			}
			if (before == stream.avail_in && !produced) {
				if (stream.avail_in)
					status = PG_CORRUPT;
				break;
			}
		} while (stream.avail_in || stream.avail_out == 0);
		if (status != PG_OK || ended)
			break;
	}
	if (initialized)
		zng_inflateEnd(&stream);
	if (status != PG_OK)
		return status;
	if (!ended || decoded != info->logical_size)
		return PG_CORRUPT;
	pg_md5_finish(&hash, digest);
	if (info->digest_kind == PG_DIGEST_NONE)
		return explicit_verify ? PG_NO_CHECKSUM : PG_OK;
	size_t digest_size = info->digest_kind == PG_DIGEST_MD5 ? 16 : 4;
	uint8_t zero[16] = {};

	if (memcmp(info->digest, zero, digest_size) == 0) {
		if (info->digest_kind == PG_DIGEST_MD5 &&
		    !info->logical_size)
			return PG_OK;
		return explicit_verify ? PG_NO_CHECKSUM : PG_OK;
	}
	return memcmp(info->digest, digest, digest_size) == 0 ?
		PG_OK : PG_CHECKSUM;
}

#endif
