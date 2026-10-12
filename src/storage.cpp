#include <piggle/piggle.h>
#include "internal.hpp"
#include "archive_read.hpp"
#include "native_path.hpp"

struct pg_storage_range {
	uint64_t begin;
	uint64_t end;
};

static int pg_storage_order(const void *a, const void *b)
{
	const pg_storage_range *left = (const pg_storage_range *)a;
	const pg_storage_range *right = (const pg_storage_range *)b;

	return left->begin < right->begin ? -1 : left->begin > right->begin;
}

static uint64_t pg_storage_union(pg_storage_range *ranges, size_t count)
{
	qsort(ranges, count, sizeof(*ranges), pg_storage_order);
	uint64_t bytes = 0, end = 0;

	for (size_t i = 0; i < count; i++) {
		uint64_t begin = ranges[i].begin > end ? ranges[i].begin : end;

		if (ranges[i].end > begin)
			bytes += ranges[i].end - begin;
		if (ranges[i].end > end)
			end = ranges[i].end;
	}
	return bytes;
}

static pg_status pg_storage_recheck(pg_source *source, int *native_code)
{
	struct stat opened, path;

	if (source->stale)
		return PG_STALE;
	if (source->recovery_required)
		return PG_RECOVERY_REQUIRED;
	if (fstat(source->fd, &opened) || lstat(source->native_path, &path)) {
		*native_code = errno;
		return errno == ENOENT ? PG_STALE : PG_IO;
	}
	if (!pg_native_stat_same(&source->identity, &opened) ||
		!pg_native_stat_same(&opened, &path))
		return PG_STALE;
	return PG_OK;
}

static pg_status pg_storage_measure(
	pg_source *source, pg_source_storage_info *out)
{
	uint8_t header[24], disk[32];
	uint64_t size = (uint64_t)source->identity.st_size;
	uint64_t base = 0;
	uint32_t files = 0, data_file = UINT32_MAX;
	pg_storage_range internal = {};
	pg_status status = pg_read_span(source->fd, size, 0, header,
		source->format == PG_HOGG10 ? 24 : 16);

	if (status != PG_OK)
		return status;
	if (source->format == PG_HOGG10) {
		files = pg_read_u32(header + 8) / 32;
		data_file = pg_read_u32(header + 16);
		base = 24 + pg_read_u16(header + 6) + pg_read_u16(header + 20) +
			(uint64_t)pg_read_u32(header + 8) +
			pg_read_u32(header + 12);
	} else {
		files = pg_read_u32(header + 12);
		base = pg_read_u16(header + 8) +
			(uint64_t)files * pg_read_u16(header + 10);
		for (unsigned i = 0; i < 2; i++) {
			status = pg_read_span(source->fd, size, base, disk, 12);
			if (status != PG_OK)
				return status;
			base += 12 + pg_read_u32(disk + 8);
		}
	}
	if (base > size ||
		(uint64_t)files + 1 > SIZE_MAX / sizeof(pg_storage_range))
		return PG_CORRUPT;
	pg_storage_range *ranges = (pg_storage_range *)malloc(
		((size_t)files + 1) * sizeof(*ranges));

	if (!ranges)
		return PG_NOMEM;
	size_t count = 0;

	if (source->format == PG_HOGG10) {
		uint64_t table =
			24 + pg_read_u16(header + 6) + pg_read_u16(header + 20);

		for (uint32_t i = 0; i < files; i++) {
			status = pg_read_span(source->fd, size,
				table + 32 * (uint64_t)i, disk, sizeof(disk));
			if (status != PG_OK)
				break;
			uint32_t length = pg_read_u32(disk + 8);
			uint64_t offset = pg_read_u64(disk);

			if (!length || length == UINT32_MAX)
				continue;
			if (offset < base || offset > size ||
				length > size - offset) {
				status = PG_CORRUPT;
				break;
			}
			if (i == data_file)
				internal = {offset, offset + length};
			else
				ranges[count++] = {offset, offset + length};
		}
	} else {
		for (pg_source_record *record = source->records; record;
			record = record->next) {
			uint64_t offset = record->payload_offset;
			uint64_t length = record->info.stored_size;

			if (offset < base || offset > size ||
				length > size - offset) {
				status = PG_CORRUPT;
				break;
			}
			if (length)
				ranges[count++] = {offset, offset + length};
		}
	}
	if (status == PG_OK) {
		out->physical_length = size;
		out->live_payload_bytes = pg_storage_union(ranges, count);
		if (internal.end > internal.begin)
			ranges[count++] = internal;
		uint64_t all = pg_storage_union(ranges, count);

		out->metadata_bytes = base + all - out->live_payload_bytes;
		out->unreferenced_bytes = size - base - all;
	}
	free(ranges);
	return status;
}

pg_status pg_source_storage_inspect(
	pg_source *source, pg_source_storage_info *out, pg_error *error)
{
	if (out)
		memset(out, 0, sizeof(*out));
	if (!source || !out)
		return pg_result(PG_INVALID, error);
	pg_context_lock(source->context);
	pg_source_lock(source);
	pg_source_storage_info measured = {};
	pg_cursor *cursor = NULL;
	int native_code = 0;
	pg_status status = pg_source_control_status(source, 0);

	if (status == PG_OK && source->format == PG_LOOSE)
		status = PG_UNSUPPORTED;
	if (status == PG_OK)
		status = pg_storage_recheck(source, &native_code);
	if (status == PG_OK)
		status = pg_storage_measure(source, &measured);
	if (status == PG_OK)
		status = pg_source_files(source, NULL, &cursor, NULL);
	if (status == PG_OK) {
		uint64_t capacity =
			cursor->count + 1 < 16 ? 16 : cursor->count + 1;
		uint64_t estimate = 4144 + 48 * capacity + 22;

		for (size_t i = 0; i < cursor->count; i++) {
			const pg_file_info *info = &cursor->files[i]->info;
			uint64_t entry = info->stored_size + 4 +
				strlen(info->original_name) + 1;

			if (info->cached_header_size)
				entry += 4 + info->cached_header_size;
			if (entry > UINT64_MAX - estimate) {
				status = PG_LIMIT;
				break;
			}
			estimate += entry;
		}
		measured.compacted_size = estimate;
		measured.reclaimable_bytes = measured.physical_length > estimate
			? measured.physical_length - estimate
			: 0;
	}
	if (cursor) {
		pg_status closed = pg_cursor_close(&cursor, NULL);

		if (status == PG_OK)
			status = closed;
	}
	if (status == PG_OK)
		status = pg_storage_recheck(source, &native_code);
	if (status == PG_OK)
		*out = measured;
	pg_source_unlock(source);
	pg_context_unlock(source->context);
	return pg_native_result(status, native_code, error);
}

pg_status pg_source_compact(pg_source *source, const char *path,
	const pg_archive_build_options *options, pg_error *error)
{
	if (!source || !path || !*path)
		return pg_result(PG_INVALID, error);
	pg_context_lock(source->context);
	pg_source_lock(source);
	pg_archive_build_options config = {
		{PG_HOGG10, 0, source->checksum_domain},
		source->staging_directory, 0};
	pg_archive_builder *builder = NULL;
	pg_cursor *cursor = NULL;
	pg_file *file = NULL;
	pg_error work_error = {};
	int native_code = 0;
	pg_status status = pg_source_control_status(source, 1);
	struct stat target;

	if (options)
		config = *options;
	if (status == PG_OK && source->format == PG_LOOSE)
		status = PG_UNSUPPORTED;
	if (status == PG_OK &&
		(config.archive.format != PG_HOGG10 ||
			config.archive.checksum_domain !=
				source->checksum_domain))
		status = PG_INVALID;
	if (status == PG_OK)
		status = pg_storage_recheck(source, &native_code);
	if (status == PG_OK && !lstat(path, &target) &&
		target.st_dev == source->identity.st_dev &&
		target.st_ino == source->identity.st_ino)
		status = PG_CONFLICT;
	if (status == PG_OK)
		status = pg_source_files(source, NULL, &cursor, &work_error);
	if (status == PG_OK) {
		if (config.hogg_user_capacity < cursor->count)
			config.hogg_user_capacity = cursor->count;
		status = pg_archive_builder_create_config(
			source->context, path, &config, &builder, &work_error);
	}
	while (status == PG_OK) {
		status = pg_cursor_next(cursor, &file, &work_error);
		if (status == PG_END) {
			status = PG_OK;
			break;
		}
		if (status != PG_OK)
			break;
		status = pg_archive_builder_copy(builder,
			file->info.canonical_name, file,
			file->info.encoding == PG_ZLIB ? PG_COMPRESS_FORCE
						: PG_COMPRESS_NEVER,
			&work_error);
		pg_status closed = pg_file_close(&file, NULL);

		if (status == PG_OK)
			status = closed;
	}
	if (cursor) {
		pg_status closed = pg_cursor_close(&cursor, NULL);

		if (status == PG_OK)
			status = closed;
	}
	if (status == PG_OK)
		status = pg_storage_recheck(source, &native_code);
	if (status == PG_OK)
		status = pg_archive_builder_finish(builder, &work_error);
	if (builder) {
		pg_status closed = pg_archive_builder_close(&builder, NULL);

		if (status == PG_OK && closed != PG_OK) {
			status = PG_COMMITTED;
			pg_result(status, &work_error);
			work_error.cause = closed;
		}
	}
	pg_source_unlock(source);
	pg_context_unlock(source->context);
	if (work_error.status == status && error)
		*error = work_error;
	else
		pg_native_result(status, native_code, error);
	return status;
}
