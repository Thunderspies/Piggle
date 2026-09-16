#include <piggle/piggle.h>
#include "internal.hpp"
#include "archive_read.hpp"
#include "native_path.hpp"

#include <fcntl.h>

extern "C" {

/* Release the owned allocation, if present. */
/* Reset data and size to the canonical empty pair. */
PG_API void PG_CALL pg_buffer_free(pg_buffer *buffer)
{
	free(buffer->data);
	buffer->data = NULL;
	buffer->size = 0;
}

/* Inspect the captured file copy and its logical size. */
/* Check capacity before touching caller storage. */
/* Read through verified EOF and return exact byte count. */
PG_API pg_status PG_CALL pg_file_read_all(pg_file *file, void *buffer,
		size_t capacity, size_t *bytes, pg_error *error)
{
	pg_reader *reader = NULL;
	pg_error work_error;
	pg_error close_error;
	pg_status status;
	pg_status close_status;
	uint8_t scratch;
	size_t total = 0;
	size_t count = 0;

	if (bytes)
		*bytes = 0;
	if (!file || !bytes || (capacity && !buffer))
		return pg_result(PG_INVALID, error);
	if (file->info.logical_size > SIZE_MAX)
		return pg_result(PG_LIMIT, error);
	if (file->info.logical_size > capacity)
		return pg_result(PG_CAPACITY, error);
	status = pg_reader_open(file, PG_READ_LOGICAL, &reader,
		&work_error);
	if (status != PG_OK) {
		if (error)
			*error = work_error;
		return status;
	}
	while (status == PG_OK) {
		size_t remaining = (size_t)file->info.logical_size - total;
		void *output = remaining ? (uint8_t *)buffer + total :
			&scratch;
		size_t amount = remaining ? remaining : 1;

		status = pg_reader_read(reader, output, amount, &count,
			&work_error);
		total += count;
		if (status == PG_OK && !count) {
			status = pg_result(PG_IO, &work_error);
			break;
		}
	}
	*bytes = total;
	if (status == PG_END)
		status = pg_result(PG_OK, &work_error);
	close_status = pg_reader_close(&reader, &close_error);
	if (status == PG_OK) {
		status = close_status;
		work_error = close_error;
	}
	if (error)
		*error = work_error;
	return status;
}

/* Inspect the captured file copy and its logical size. */
/* Enforce max_bytes, allocate output and read through verified EOF. */
/* Release private storage on failure; publish only complete bytes. */
PG_API pg_status PG_CALL pg_file_read_all_alloc(pg_file *file,
		size_t max_bytes, pg_buffer *out, pg_error *error)
{
	void *storage = NULL;
	size_t size;
	size_t read_bytes = 0;
	pg_status status;

	if (!file || !out || out->data || out->size)
		return pg_result(PG_INVALID, error);
	if (file->info.logical_size > SIZE_MAX ||
	    file->info.logical_size > max_bytes)
		return pg_result(PG_LIMIT, error);
	size = (size_t)file->info.logical_size;
	if (size) {
		storage = malloc(size);
		if (!storage)
			return pg_result(PG_NOMEM, error);
	}
	status = pg_file_read_all(file, storage, size, &read_bytes, error);
	if (status != PG_OK) {
		free(storage);
		return status;
	}
	out->data = storage;
	out->size = read_bytes;
	return status;
}

/* Write a complete replacement for one captured physical copy. */
PG_API pg_status PG_CALL pg_file_write_all(pg_file *file,
		const void *buffer, size_t size,
		const pg_entry_options *entry, pg_error *error)
{
	pg_write_options options;
	pg_writer *writer = NULL;
	pg_error work_error, close_error;
	pg_status status, closed;
	size_t accepted = 0;

	if (!file || (size && !buffer))
		return pg_result(PG_INVALID, error);
	pg_write_options_init(&options, size);
	if (entry)
		options.entry = *entry;
	else
		options.entry.mtime = file->info.mtime;
	status = pg_writer_open_file(file, &options, &writer,
		&work_error);
	if (status != PG_OK)
		goto file_write_done;
	status = pg_writer_write(writer, buffer, size, &accepted,
		&work_error);
	if (status == PG_OK && accepted != size)
		status = pg_result(PG_IO, &work_error);
	if (status == PG_OK)
		status = pg_writer_finish(writer, &work_error);
	closed = pg_writer_close(&writer, &close_error);
	if (status == PG_OK && closed != PG_OK) {
		status = PG_COMMITTED;
		pg_result(status, &work_error);
		work_error.cause = PG_IO;
		work_error.native_code = close_error.native_code;
	}

file_write_done:
	if (error)
		*error = work_error;
	return status;
}

/* Stream one selected logical file into an atomic native writer. */
PG_API pg_status PG_CALL pg_file_export(
		pg_file *file,
		const char *native_output,
		uint32_t flags,
		pg_error *error)
{
	pg_write_options options;
	pg_reader *reader = NULL;
	pg_writer *writer = NULL;
	pg_error work_error, close_error;
	pg_status status, closed;

	if (!file || !native_output || !*native_output ||
	    (flags & ~PG_OVERWRITE))
		return pg_result(PG_INVALID, error);
	pg_write_options_init(&options, file->info.logical_size);
	options.entry.mtime = file->info.mtime;
	options.entry.digest_kind = file->info.digest_kind;
	memcpy(options.entry.expected_digest, file->info.digest,
		sizeof(options.entry.expected_digest));
	if (file->info.checksum_domain == PG_CHECKSUM_STORED) {
		options.entry.digest_kind = PG_DIGEST_NONE;
		memset(options.entry.expected_digest, 0,
			sizeof(options.entry.expected_digest));
	}
	uint8_t zero[16] = {};

	if (memcmp(file->info.digest, zero, sizeof(zero)) == 0)
		options.entry.digest_kind = PG_DIGEST_NONE;
	status = pg_reader_open(file, PG_READ_LOGICAL, &reader,
		&work_error);
	if (status != PG_OK)
		goto export_done;
	status = pg_writer_open_native(file->source->context,
		native_output, &options, flags, &writer, &work_error);
	if (status != PG_OK)
		goto export_done;
	status = pg_transfer_bytes(reader, writer, &work_error);
	closed = pg_reader_close(&reader, &close_error);
	if (status == PG_OK && closed != PG_OK) {
		status = closed;
		work_error = close_error;
	}
	if (status == PG_OK)
		status = pg_writer_finish(writer, &work_error);

export_done:
	if (reader) {
		closed = pg_reader_close(&reader, &close_error);
		if (status == PG_OK && closed != PG_OK) {
			status = closed;
			work_error = close_error;
		}
	}
	if (writer) {
		closed = pg_writer_close(&writer, &close_error);
		if (status == PG_OK && closed != PG_OK) {
			status = PG_COMMITTED;
			pg_result(status, &work_error);
			work_error.cause = PG_IO;
			work_error.native_code = close_error.native_code;
		}
	}
	if (error)
		*error = work_error;
	return status;
}

/* Validate selected copy and copy captured immutable metadata. */
/* Keep borrowed spans tied to the file handle lifetime. */
PG_API pg_status PG_CALL pg_file_inspect(pg_file *file, pg_file_info *out,
		pg_error *error)
{
	if (out)
		memset(out, 0, sizeof(*out));
	if (!file || !out)
		return pg_result(PG_INVALID, error);
	*out = file->info;
	return pg_result(PG_OK, error);
}

/* Transfer the next captured selection without changing its metadata. */
PG_API pg_status PG_CALL pg_cursor_next(pg_cursor *cursor, pg_file **out,
		pg_error *error)
{
	if (!out)
		return pg_result(PG_INVALID, error);
	*out = NULL;
	if (!cursor)
		return pg_result(PG_INVALID, error);
	if (cursor->position == cursor->count)
		return pg_result(PG_END, error);
	*out = cursor->files[cursor->position];
	cursor->files[cursor->position++] = NULL;
	return pg_result(PG_OK, error);
}

/* Read the selected logical stream and verify its stored digest. */
/* Report NO_CHECKSUM when explicit verification lacks a digest. */
static pg_status pg_file_verify_locked(
		pg_file *file,
		pg_error *error)
{
	struct stat opened;
	struct stat current;
	pg_status status;
	int fd;
	int native_code = 0;
	uint64_t offset = file ? file->payload_offset : 0;
	const struct stat *identity;

	if (!file)
		return pg_result(PG_INVALID, error);
	if (file->source->recovery_required)
		return pg_result(PG_RECOVERY_REQUIRED, error);
	status = pg_source_control_status(file->source, 0);
	if (status != PG_OK)
		return pg_result(status, error);
	identity = &file->source_identity;
	if (file->source->format == PG_HOGG10) {
		pg_source_record *record;

		for (record = file->source->records; record; record =
			record->next)
			if (record->info.copy_id == file->info.copy_id &&
			    record->info.copy_generation ==
				file->info.copy_generation)
				break;
		if (!record)
			return pg_result(PG_STALE, error);
		offset = record->payload_offset;
		identity = &file->source->identity;
	}
	fd = open(file->source->format == PG_LOOSE ?
		file->native_path : file->source->native_path,
		O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) {
		native_code = errno;
		return pg_native_result(PG_IO, native_code, error);
	}
	if (fstat(fd, &opened) ||
	    lstat(file->source->format == PG_LOOSE ?
		file->native_path : file->source->native_path, &current)) {
		native_code = errno;
		close(fd);
		return pg_native_result(PG_IO, native_code, error);
	}
	if (!pg_native_stat_same(identity, &opened) ||
	    !pg_native_stat_same(&opened, &current)) {
		close(fd);
		return pg_result(PG_STALE, error);
	}
	if (file->source->format == PG_LOOSE) {
		close(fd);
		return pg_result(PG_NO_CHECKSUM, error);
	}
	status = pg_archive_verify_payload(fd, (uint64_t)opened.st_size,
		&file->info, offset, 1);
	if (close(fd) && status == PG_OK) {
		native_code = errno;
		status = PG_IO;
	}
	return pg_native_result(status, native_code, error);
}

PG_API pg_status PG_CALL pg_file_verify(pg_file *file, pg_error *error)
{
	if (!file)
		return pg_result(PG_INVALID, error);
	pg_source_lock(file->source);
	pg_status status = pg_file_verify_locked(file, error);

	pg_source_unlock(file->source);
	return status;
}

/* Delete one captured loose file after checking its physical identity. */
static pg_status pg_file_delete_queued(pg_file *file,
		pg_status status, pg_error *error)
{
	pg_error queue_error;
	pg_status queued;

	if (status != PG_OK && status != PG_COMMITTED)
		return status;
	if (file->source->format == PG_LOOSE) {
		queued = pg_source_rescan(file->source, &queue_error);
		if (queued != PG_OK && status == PG_OK) {
			pg_result(PG_COMMITTED, error);
			if (error)
				error->cause = queued;
			status = PG_COMMITTED;
		}
	}
	if (!file->source->attached)
		return status;
	queued = pg_tree_queue_changes(file->source->attached,
		&queue_error);
	if (queued == PG_OK || status == PG_COMMITTED)
		return status;
	pg_result(PG_COMMITTED, error);
	if (error)
		error->cause = queued;
	return PG_COMMITTED;
}

PG_API pg_status PG_CALL pg_file_delete(
		pg_file *file,
		pg_error *error)
{
	struct stat current;
	pg_status status;
	char *leaf = NULL;
	int parent = -1;
	int native_code = 0;

	if (!file)
		return pg_result(PG_INVALID, error);
	if (file->source->recovery_required)
		return pg_result(PG_RECOVERY_REQUIRED, error);
	status = pg_source_control_status(file->source, 1);
	if (status != PG_OK)
		return pg_result(status, error);
	if (file->source->access != PG_WRITE)
		return pg_result(PG_READ_ONLY, error);
	if (file->source->format == PG_HOGG10)
		return pg_file_delete_queued(file,
			pg_source_hogg_delete(file, error), error);
	if (file->source->format == PG_PIGG2)
		return pg_file_delete_queued(file,
			pg_source_pigg_delete(file, error), error);
	if (file->source->format != PG_LOOSE)
		return pg_result(PG_UNSUPPORTED, error);
	if (file->source->live_writers)
		return pg_result(PG_BUSY, error);
	status = pg_native_parent_open(file->native_path, 0,
		&parent, &leaf, &native_code);
	if (status != PG_OK)
		return pg_native_result(status, native_code, error);
	if (fstatat(parent, leaf, &current, AT_SYMLINK_NOFOLLOW)) {
		status = errno == ENOENT ? PG_STALE : PG_IO;
		native_code = errno;
	} else if (!S_ISREG(current.st_mode) ||
		   !pg_native_stat_same(&file->source_identity, &current)) {
		status = PG_STALE;
	} else if (unlinkat(parent, leaf, 0)) {
		status = PG_IO;
		native_code = errno;
	} else if (fsync(parent)) {
		status = PG_COMMITTED;
		native_code = errno;
	} else {
		status = PG_OK;
	}
	close(parent);
	free(leaf);
	pg_native_result(status, native_code, error);
	if (status == PG_COMMITTED && error)
		error->cause = PG_IO;
	return pg_file_delete_queued(file, status, error);
}

/* Accept a null owned handle as a no-op. */
/* Check close constraints; release resources and references. */
/* Clear the owned pointer once close is accepted. */
PG_API pg_status PG_CALL pg_file_close(
		pg_file **file,
		pg_error *error)
{
	pg_file *owned;
	pg_status status;
	int native_code = 0;

	if (!file)
		return pg_result(PG_INVALID, error);
	if (!*file)
		return pg_result(PG_OK, error);
	owned = *file;
	status = pg_source_control_status(owned->source, 0);
	if (status != PG_OK)
		return pg_result(status, error);
	*file = NULL;
	pg_tree *origin = owned->origin_tree;
	free((void *)owned->info.canonical_name);
	free((void *)owned->info.original_name);
	free((void *)owned->info.cached_header);
	free(owned->native_path);
	status = pg_source_release(owned->source, &native_code);
	free(owned);
	if (origin)
		pg_tree_release(origin);
	return pg_native_result(status, native_code, error);
}

/* Release every unconsumed selection and clear the owned cursor. */
PG_API pg_status PG_CALL pg_cursor_close(
		pg_cursor **cursor,
		pg_error *error)
{
	pg_cursor *owned;
	pg_status status = PG_OK;
	pg_error first_error = {};
	size_t i;

	if (!cursor)
		return pg_result(PG_INVALID, error);
	if (!*cursor)
		return pg_result(PG_OK, error);
	owned = *cursor;
	if (owned->source) {
		status = pg_source_control_status(owned->source, 0);
		if (status != PG_OK)
			return pg_result(status, error);
	}
	for (i = owned->position; i < owned->count; i++) {
		if (!owned->files[i])
			continue;
		status = pg_source_control_status(owned->files[i]->source, 0);
		if (status != PG_OK)
			return pg_result(status, error);
	}
	*cursor = NULL;
	for (i = owned->position; i < owned->count; i++) {
		pg_error close_error;
		pg_status closed = pg_file_close(&owned->files[i],
			&close_error);

		if (status == PG_OK && closed != PG_OK) {
			status = closed;
			first_error = close_error;
		}
	}
	free(owned->files);
	if (owned->source) {
		int native_code = 0;
		pg_status released = pg_source_release(owned->source,
			&native_code);

		if (status == PG_OK && released != PG_OK)
			pg_native_result(released, native_code, &first_error);
		if (status == PG_OK)
			status = released;
	}
	pg_context_child_drop(owned->context);
	free(owned);
	if (status != PG_OK) {
		if (error)
			*error = first_error;
		return status;
	}
	return pg_result(PG_OK, error);
}

} /* extern "C" */

/* Validate capture and fields, then publish only selected metadata. */
pg_status pg_file_update_metadata(pg_file *file,
		const pg_metadata_options *options, pg_error *error)
{
	pg_status status;
	struct stat current;
	int fd, parent = -1, native_code = 0, published = 0;
	char *leaf = NULL;

	if (!file || !options ||
	    (options->fields & ~(PG_METADATA_MTIME | PG_METADATA_HEADER)) ||
	    ((options->fields & PG_METADATA_HEADER) &&
	     options->cached_header_size && !options->cached_header))
		return pg_result(PG_INVALID, error);
	if (file->source->recovery_required)
		return pg_result(PG_RECOVERY_REQUIRED, error);
	status = pg_source_control_status(file->source, 1);
	if (status != PG_OK)
		return pg_result(status, error);
	if (file->source->access != PG_WRITE)
		return pg_result(PG_READ_ONLY, error);
	if ((options->fields & PG_METADATA_HEADER) &&
	    options->cached_header_size > UINT32_MAX)
		return pg_result(PG_LIMIT, error);
	if (file->source->format != PG_LOOSE)
		return pg_file_delete_queued(file,
			pg_source_update_metadata(file, options, error), error);
	if (options->fields & PG_METADATA_HEADER)
		return pg_result(PG_UNSUPPORTED, error);
	if (file->source->live_writers)
		return pg_result(PG_BUSY, error);
	if ((options->fields & PG_METADATA_MTIME) &&
	    (int64_t)(time_t)options->mtime != options->mtime)
		return pg_result(PG_LIMIT, error);
	status = pg_native_parent_open(file->native_path, 0, &parent, &leaf,
		&native_code);
	if (status != PG_OK)
		return pg_native_result(status, native_code, error);
	fd = openat(parent, leaf, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
	free(leaf);
	close(parent);
	if (fd < 0)
		return pg_native_result(errno == ENOENT ? PG_STALE : PG_IO,
			errno, error);
	if (fstat(fd, &current)) {
		status = PG_IO;
		native_code = errno;
	} else if (!pg_native_stat_same(&file->source_identity, &current)) {
		status = PG_STALE;
	} else if ((options->fields & PG_METADATA_MTIME) &&
		   options->mtime != file->info.mtime) {
		struct timespec times[2] = {};

		times[0].tv_sec = current.st_atime;
		times[1].tv_sec = (time_t)options->mtime;
		if (futimens(fd, times)) {
			status = PG_IO;
			native_code = errno;
		} else {
			published = 1;
			if (fsync(fd)) {
				status = PG_COMMITTED;
				native_code = errno;
			}
		}
	}
	if (close(fd) && status == PG_OK) {
		status = published ? PG_COMMITTED : PG_IO;
		native_code = errno;
	}
	pg_native_result(status, native_code, error);
	if (status == PG_COMMITTED && error)
		error->cause = PG_IO;
	return pg_file_delete_queued(file, status, error);
}
