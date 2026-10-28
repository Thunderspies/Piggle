#include <piggle/piggle.h>
#include "internal.hpp"
#include "archive_wire.hpp"
#include "native_path.hpp"

#include <fcntl.h>

static unsigned pg_builder_height(pg_builder_entry *entry)
{
	return entry ? entry->height : 0;
}

static void pg_builder_height_update(pg_builder_entry *entry)
{
	unsigned left = pg_builder_height(entry->left);
	unsigned right = pg_builder_height(entry->right);

	entry->height = 1 + (left > right ? left : right);
}

static pg_builder_entry *pg_builder_rotate_left(pg_builder_entry *entry)
{
	pg_builder_entry *root = entry->right;

	entry->right = root->left;
	root->left = entry;
	pg_builder_height_update(entry);
	pg_builder_height_update(root);
	return root;
}

static pg_builder_entry *pg_builder_rotate_right(pg_builder_entry *entry)
{
	pg_builder_entry *root = entry->left;

	entry->left = root->right;
	root->right = entry;
	pg_builder_height_update(entry);
	pg_builder_height_update(root);
	return root;
}

pg_builder_entry *pg_builder_name_find(
	pg_archive_builder *builder, const char *name)
{
	pg_builder_entry *entry = builder->name_index;

	while (entry) {
		builder->name_comparisons++;
		int order = strcmp(name, entry->canonical_name);

		if (!order)
			return entry;
		entry = order < 0 ? entry->left : entry->right;
	}
	return NULL;
}

static pg_builder_entry *pg_builder_index_insert(pg_archive_builder *builder,
	pg_builder_entry *root, pg_builder_entry *entry)
{
	if (!root) {
		entry->height = 1;
		return entry;
	}
	builder->name_comparisons++;
	if (strcmp(entry->canonical_name, root->canonical_name) < 0)
		root->left =
			pg_builder_index_insert(builder, root->left, entry);
	else
		root->right =
			pg_builder_index_insert(builder, root->right, entry);
	pg_builder_height_update(root);
	int balance = (int)pg_builder_height(root->left) -
		(int)pg_builder_height(root->right);

	if (balance > 1) {
		if (pg_builder_height(root->left->right) >
			pg_builder_height(root->left->left))
			root->left = pg_builder_rotate_left(root->left);
		return pg_builder_rotate_right(root);
	}
	if (balance < -1) {
		if (pg_builder_height(root->right->left) >
			pg_builder_height(root->right->right))
			root->right = pg_builder_rotate_right(root->right);
		return pg_builder_rotate_left(root);
	}
	return root;
}

void pg_builder_entry_insert(
	pg_archive_builder *builder, pg_builder_entry *entry)
{
	builder->name_index =
		pg_builder_index_insert(builder, builder->name_index, entry);
	if (builder->entries_tail)
		builder->entries_tail->next = entry;
	else
		builder->entries = entry;
	builder->entries_tail = entry;
	builder->entry_count++;
}

static pg_status pg_builder_recheck(
	pg_archive_builder *builder, int *native_code)
{
	struct stat current;
	struct stat pinned;
	int parent = -1;
	char *leaf = NULL;
	pg_status status = pg_native_parent_open(
		builder->native_path, 0, &parent, &leaf, native_code);

	if (status != PG_OK) {
		if (status == PG_CONFLICT ||
			(status == PG_IO && *native_code == ENOENT))
			return PG_STALE;
		return status;
	}
	if (fstat(parent, &current) ||
		fstat(builder->parent_fd, &pinned)) {
		*native_code = errno;
		status = PG_IO;
	} else if (current.st_dev != pinned.st_dev ||
		current.st_ino != pinned.st_ino ||
		strcmp(leaf, builder->leaf) != 0) {
		status = PG_STALE;
	}
	close(parent);
	free(leaf);
	if (status != PG_OK)
		return status;
	if (fstatat(builder->parent_fd, builder->leaf, &current,
			AT_SYMLINK_NOFOLLOW) == 0) {
		if (!S_ISREG(current.st_mode))
			return PG_CONFLICT;
		if (!builder->target_exists ||
			!pg_native_stat_same(
				&builder->target, &current))
			return PG_STALE;
	} else if (errno == ENOENT) {
		if (builder->target_exists)
			return PG_STALE;
	} else {
		*native_code = errno;
		return PG_IO;
	}
	return PG_OK;
}

static pg_status pg_builder_effect(pg_status status, pg_status cause,
	int native_code, pg_error *error)
{
	pg_native_result(status, native_code, error);
	if (error)
		error->cause = cause;
	return status;
}

static pg_status pg_builder_transfer_reader(pg_archive_builder *builder,
	const char *name, pg_reader **reader,
	const pg_write_options *options, pg_error *error)
{
	pg_writer *writer = NULL;
	pg_error work_error, close_error;
	pg_status status, closed;

	status = pg_writer_open_archive_builder(
		builder, name, options, &writer, &work_error);
	if (status != PG_OK)
		goto transfer_done;
	status = pg_transfer_bytes(*reader, writer, &work_error);
	closed = pg_reader_close(reader, &close_error);
	if (status == PG_OK && closed != PG_OK) {
		status = closed;
		work_error = close_error;
	}
	if (status == PG_OK)
		status = pg_writer_finish(writer, &work_error);

transfer_done:
	if (*reader) {
		closed = pg_reader_close(reader, &close_error);
		if (status == PG_OK && closed != PG_OK) {
			status = closed;
			work_error = close_error;
		}
	}
	if (writer) {
		closed = pg_writer_close(&writer, &close_error);
		if (status == PG_OK && closed != PG_OK) {
			status = closed;
			work_error = close_error;
		}
	}
	if (error)
		*error = work_error;
	return status;
}

/* Consume a captured cursor and publish one archive with validated
 * options. */
pg_status pg_pack_cursor(pg_cursor **cursor, const char *native_archive,
	uint32_t format, const pg_pack_options *options,
	pg_error *error)
{
	pg_archive_builder *builder = NULL;
	pg_file *file = NULL;
	pg_error work_error, close_error;
	pg_status closed;
	pg_archive_options profile = {
		format, options->flags, options->checksum_domain};
	pg_archive_build_options config = {
		profile, NULL, format == PG_HOGG10 ? (*cursor)->count : 0};
	pg_status status = pg_archive_builder_create_config(
		(*cursor)->context, native_archive, &config, &builder,
		&work_error);

	if (status != PG_OK)
		goto pack_done;
	for (;;) {
		status = pg_cursor_next(*cursor, &file, &work_error);
		if (status == PG_END) {
			status = PG_OK;
			break;
		}
		if (status != PG_OK)
			break;
		status = pg_archive_builder_copy(builder,
			file->info.canonical_name, file,
			options->compression, &work_error);
		closed = pg_file_close(&file, &close_error);
		if (status == PG_OK && closed != PG_OK) {
			status = closed;
			work_error = close_error;
		}
		if (status != PG_OK)
			break;
	}
	closed = pg_cursor_close(cursor, &close_error);
	if (status == PG_OK && closed != PG_OK) {
		status = closed;
		work_error = close_error;
	}
	if (status == PG_OK)
		status =
			pg_archive_builder_finish(builder, &work_error);

pack_done:
	if (file)
		pg_file_close(&file, NULL);
	if (*cursor)
		pg_cursor_close(cursor, NULL);
	if (builder) {
		closed = pg_archive_builder_close(
			&builder, &close_error);
		if (status == PG_OK && closed != PG_OK) {
			status = PG_COMMITTED;
			pg_result(status, &work_error);
			work_error.cause = PG_IO;
			work_error.native_code =
				close_error.native_code;
		}
	}
	if (error)
		*error = work_error;
	return status;
}

extern "C" {

/* Validate format, flags and exclusive native destination. */
/* Create private staging and builder index; publish handle on success.
 */
static pg_status pg_builder_create(pg_context *context, const char *native_path,
	uint32_t format, uint32_t flags, const char *staging_directory,
	uint64_t capacity, pg_archive_builder **out, pg_error *error)
{
	pg_context_lock(context);
	auto coordinated = [&]() -> pg_status {
		pg_archive_builder *builder;
		pg_status path_status = PG_OK;
		int native_code = 0;
		struct stat target;
		int target_exists = 0;
		pg_status status;

		if (!out)
			return pg_result(PG_INVALID, error);
		*out = NULL;
		if (!context || !native_path || !*native_path ||
			(format != PG_PIGG2 && format != PG_HOGG10) ||
			(flags & ~PG_OVERWRITE))
			return pg_result(PG_INVALID, error);
		if (pg_context_children(context) == SIZE_MAX)
			return pg_result(PG_LIMIT, error);
		builder = (pg_archive_builder *)calloc(
			1, sizeof(*builder));
		if (!builder)
			return pg_result(PG_NOMEM, error);
		builder->staging_parent_fd = -1;
		builder->native_path = pg_native_absolute(
			native_path, &path_status, &native_code);
		if (!builder->native_path) {
			free(builder);
			return pg_native_result(
				path_status, native_code, error);
		}
		status = pg_native_parent_open(builder->native_path, 1,
			&builder->parent_fd, &builder->leaf,
			&native_code);
		if (status != PG_OK) {
			free(builder->native_path);
			free(builder);
			return pg_native_result(
				status, native_code, error);
		}
		if (fstatat(builder->parent_fd, builder->leaf, &target,
				AT_SYMLINK_NOFOLLOW) == 0) {
			if (!S_ISREG(target.st_mode)) {
				status = PG_CONFLICT;
				goto cleanup_parent;
			}
			if (!(flags & PG_OVERWRITE)) {
				status = PG_EXISTS;
				goto cleanup_parent;
			}
			target_exists = 1;
		} else if (errno != ENOENT) {
			native_code = errno;
			status = PG_IO;
			goto cleanup_parent;
		}
		status = pg_native_alias(context, builder->parent_fd,
			&target, target_exists, NULL, &native_code);
		if (status != PG_OK)
			goto cleanup_parent;
		builder->staging_parent_fd = -1;
		builder->hogg_user_capacity = capacity;
		if (staging_directory) {
			status = pg_staging_resolve(staging_directory,
				&builder->staging_directory, &native_code);
		} else {
			size_t length = strlen(builder->native_path) -
				strlen(builder->leaf);
			builder->staging_directory = (char *)malloc(length + 1);
			if (!builder->staging_directory)
				status = PG_NOMEM;
			else {
				memcpy(builder->staging_directory,
					builder->native_path, length);
				builder->staging_directory[length] = 0;
			}
		}
		if (status != PG_OK)
			goto cleanup_parent;
		builder->staging = pg_staging_open(
			builder->staging_directory, &builder->staging_path);
		if (!builder->staging) {
			native_code = errno;
			status = PG_IO;
			goto cleanup_parent;
		}
		status = pg_native_parent_open(builder->staging_path, 0,
			&builder->staging_parent_fd, &builder->staging_leaf,
			&native_code);
		if (status != PG_OK)
			goto cleanup_parent;
		if (format == PG_HOGG10) {
			uint64_t physical =
				capacity + 1 < 16 ? 16 : capacity + 1;

			if (fseeko(builder->staging,
				(off_t)(4144 + 48 * physical - 1),
				SEEK_SET) ||
				fputc(0, builder->staging) == EOF ||
				fflush(builder->staging)) {
				status = PG_IO;
				native_code = errno;
				goto cleanup_parent;
			}
			builder->private_hogg = 1;
			builder->metadata_reserved = 4144 + 48 * physical;
		}
		builder->context = context;
		builder->format = format;
		builder->flags = flags;
		builder->target_exists = target_exists;
		if (target_exists)
			builder->target = target;
		pg_context_child_add(context);
		*out = builder;
		return pg_result(PG_OK, error);

	cleanup_parent:
		if (builder->staging)
			fclose(builder->staging);
		if (builder->staging_parent_fd >= 0)
			close(builder->staging_parent_fd);
		if (builder->staging_path)
			unlink(builder->staging_path);
		free(builder->staging_directory);
		free(builder->staging_path);
		free(builder->staging_leaf);
		close(builder->parent_fd);
		free(builder->leaf);
		free(builder->native_path);
		free(builder);
		return pg_native_result(status, native_code, error);
	};
	pg_status status = coordinated();

	pg_context_unlock(context);
	return status;
}

/* Validate entry metadata, name uniqueness and complete input span. */
/* Stage one entry through a private writer without disk publication. */
/* Release the writer and discard an incomplete entry on failure. */
PG_API pg_status PG_CALL pg_archive_builder_write_all(
	pg_archive_builder *builder, const char *name,
	const void *buffer, size_t size, const pg_entry_options *entry,
	pg_error *error)
{
	pg_write_options options;
	pg_writer *writer = NULL;
	pg_error first_error;
	pg_error close_error;
	pg_status status;
	pg_status close_status;
	size_t accepted = 0;

	if (!builder || !name || (size && !buffer))
		return pg_result(PG_INVALID, error);
	pg_write_options_init(&options, size);
	if (entry)
		options.entry = *entry;
	status = pg_writer_open_archive_builder(
		builder, name, &options, &writer, &first_error);
	if (status != PG_OK) {
		if (error)
			*error = first_error;
		return status;
	}
	status = pg_writer_write(
		writer, buffer, size, &accepted, &first_error);
	if (status == PG_OK)
		status = pg_writer_finish(writer, &first_error);
	close_status = pg_writer_close(&writer, &close_error);
	if (status == PG_OK) {
		status = close_status;
		first_error = close_error;
	}
	if (error)
		*error = first_error;
	return status;
}

/* Import a native file into private archive staging. */
PG_API pg_status PG_CALL pg_archive_builder_import(
	pg_archive_builder *builder, const char *name,
	const char *native_input, uint32_t compression, pg_error *error)
{
	pg_reader *reader = NULL;
	pg_reader_info info;
	pg_write_options options;
	pg_status status;

	if (!builder || !name || !native_input || !*native_input ||
		compression > PG_COMPRESS_FORCE)
		return pg_result(PG_INVALID, error);
	status = pg_reader_open_native(
		builder->context, native_input, &reader, error);
	if (status != PG_OK)
		return status;
	status = pg_reader_inspect(reader, &info, error);
	if (status != PG_OK) {
		pg_reader_close(&reader, NULL);
		return status;
	}
	pg_write_options_init(&options, info.size);
	options.entry.mtime = info.mtime;
	options.entry.compression = compression;
	return pg_builder_transfer_reader(
		builder, name, &reader, &options, error);
}

/* Copy a selected file into private archive staging. */
PG_API pg_status PG_CALL pg_archive_builder_copy(
	pg_archive_builder *builder, const char *name, pg_file *input,
	uint32_t compression, pg_error *error)
{
	pg_reader *reader = NULL;
	pg_write_options options;
	pg_status status;
	int stored;

	if (!builder || !name || !input ||
		builder->context != input->source->context ||
		compression > PG_COMPRESS_FORCE)
		return pg_result(PG_INVALID, error);
	stored = input->info.encoding == PG_ZLIB &&
		(compression == PG_COMPRESS_FORCE ||
			(compression == PG_COMPRESS_AUTO &&
				input->info.stored_size <
					input->info.logical_size));
	status = pg_reader_open(input,
		stored ? PG_READ_STORED : PG_READ_LOGICAL_PAYLOAD, &reader,
		error);
	if (status != PG_OK)
		return status;
	pg_write_options_init(&options, input->info.logical_size);
	options.entry.mtime = input->info.mtime;
	options.entry.compression = compression;
	if (strcmp(name, input->info.canonical_name) == 0 ||
		strcmp(name, input->info.original_name) == 0)
		options.entry.original_name = input->info.original_name;
	options.entry.cached_header = input->info.cached_header;
	options.entry.cached_header_size =
		input->info.cached_header_size;
	if (input->source->format != PG_LOOSE)
		options.entry.cached_header_policy = PG_HEADER_CACHE_EXPLICIT;
	if (stored) {
		uint8_t zero[16] = {};
		size_t digest_size =
			input->info.digest_kind == PG_DIGEST_MD5 ? 16
									: 4;

		options.encoding = PG_ZLIB;
		options.input_size = input->info.stored_size;
		if (input->info.digest_kind != PG_DIGEST_NONE &&
			memcmp(input->info.digest, zero, digest_size) !=
				0) {
			options.entry.digest_kind =
				input->info.digest_kind;
			options.entry.expected_digest_domain =
				input->info.checksum_domain;
			memcpy(options.entry.expected_digest,
				input->info.digest, digest_size);
		}
	}
	return pg_builder_transfer_reader(
		builder, name, &reader, &options, error);
}

/* Reject live entry writers; finalize all staged entries. */
/* Validate complete archive and publish once at native destination. */
/* Report commitment or uncertain outcome after cleanup. */
static pg_status pg_archive_builder_finish_locked(
	pg_archive_builder *builder, pg_error *error)
{
	char *temporary;
	const char *temporary_leaf;
	FILE *out;
	int fd;
	int native_code = 0;
	pg_status status;
	struct stat created;
	struct stat pinned;
	size_t length;
	int published = 0;
	int uncertain = 0;
	int temporary_parent = builder ? builder->parent_fd : -1;
	int direct = 0;

	if (!builder)
		return pg_result(PG_INVALID, error);
	if (builder->finished)
		return pg_result(PG_INVALID, error);
	if (builder->live_writers)
		return pg_result(PG_BUSY, error);
	builder->finished = 1;
	if (builder->private_hogg) {
		status = pg_hogg_write(builder, builder->staging);
		if (status != PG_OK)
			return pg_native_result(status, errno, error);
		if (fflush(builder->staging) || fsync(fileno(builder->staging)))
			return pg_native_result(PG_IO, errno, error);
		struct stat stage_parent, destination_parent;

		if (fstat(builder->staging_parent_fd, &stage_parent) ||
			fstat(builder->parent_fd, &destination_parent))
			return pg_native_result(PG_IO, errno, error);
		direct = stage_parent.st_dev == destination_parent.st_dev;
	}
prepare_temporary:
	if (direct) {
		temporary = strdup(builder->staging_path);
		if (!temporary)
			return pg_result(PG_NOMEM, error);
		temporary_parent = builder->staging_parent_fd;
		fd = dup(fileno(builder->staging));
		if (fd < 0) {
			free(temporary);
			return pg_native_result(PG_IO, errno, error);
		}
	} else {
		length = strlen(builder->native_path);
		if (length > SIZE_MAX - sizeof(".piggle-XXXXXX"))
			return pg_result(PG_LIMIT, error);
		temporary = (char *)malloc(length + sizeof(".piggle-XXXXXX"));
		if (!temporary)
			return pg_result(PG_NOMEM, error);
		memcpy(temporary, builder->native_path, length);
		memcpy(temporary + length, ".piggle-XXXXXX",
			sizeof(".piggle-XXXXXX"));
		fd = mkstemp(temporary);
		if (fd < 0) {
			native_code = errno;
			free(temporary);
			return pg_native_result(PG_IO, native_code, error);
		}
	}
	temporary_leaf = pg_native_basename(temporary);
	if (fstat(fd, &created) ||
		fstatat(temporary_parent, temporary_leaf, &pinned,
			AT_SYMLINK_NOFOLLOW) ||
		!pg_native_stat_same(&created, &pinned)) {
		native_code = errno;
		close(fd);
		status = PG_RETRY;
		goto remove_temporary;
	}
	out = fdopen(fd, direct ? "r+b" : "wb");
	if (!out) {
		native_code = errno;
		close(fd);
		status = PG_IO;
		goto remove_temporary;
	}
	if (direct)
		status = PG_OK;
	else if (builder->private_hogg) {
		uint8_t bytes[65536];

		status = fseeko(builder->staging, 0, SEEK_SET) ? PG_IO : PG_OK;
		while (status == PG_OK) {
			size_t count = fread(
				bytes, 1, sizeof(bytes), builder->staging);

			if (count && fwrite(bytes, 1, count, out) != count)
				status = PG_IO;
			builder->whole_archive_copied_bytes += count;
			if (count < sizeof(bytes)) {
				if (ferror(builder->staging))
					status = PG_IO;
				break;
			}
		}
	} else
		status = pg_pigg_write(builder, out);
	if (status == PG_OK && fflush(out)) {
		native_code = errno;
		status = PG_IO;
	}
	if (status == PG_OK && fsync(fd)) {
		native_code = errno;
		status = PG_IO;
	}
	if (status == PG_OK && fstat(fd, &created)) {
		native_code = errno;
		status = PG_IO;
	}
	if (fclose(out) && status == PG_OK) {
		native_code = errno;
		status = PG_IO;
	}
	if (status != PG_OK)
		goto remove_temporary;
	status = pg_builder_recheck(builder, &native_code);
	if (status != PG_OK)
		goto remove_temporary;
	status = pg_native_alias(builder->context, builder->parent_fd,
		&builder->target, builder->target_exists, NULL, &native_code);
	if (status != PG_OK)
		goto remove_temporary;
	pg_error intent_error;

	status = pg_session_publication_prepare(
		builder, &created, &intent_error);
	if (status != PG_OK) {
		native_code = intent_error.native_code;
		/* Selecting intent does not publish the destination archive. */
		if (status == PG_COMMITTED)
			status = PG_IO;
		goto remove_temporary;
	}
	if (builder->flags & PG_OVERWRITE) {
		if (renameat(temporary_parent, temporary_leaf,
			builder->parent_fd, builder->leaf)) {
			int target_read;

			native_code = errno;
			if (direct && native_code == EXDEV)
				goto copy_temporary;
			target_read = !fstatat(builder->parent_fd,
				builder->leaf, &pinned,
				AT_SYMLINK_NOFOLLOW);
			if (target_read &&
				pg_native_stat_same(&created, &pinned))
				published = 1;
			else if (target_read &&
				builder->target_exists &&
				pg_native_stat_same(
					&builder->target, &pinned))
				status = PG_IO;
			else
				uncertain = 1;
			if (!published && uncertain)
				status = PG_INDETERMINATE;
			else if (published)
				status = PG_IO;
			goto remove_temporary;
		}
		published = 1;
	} else {
		if (linkat(temporary_parent, temporary_leaf, builder->parent_fd,
			builder->leaf, 0)) {
			native_code = errno;
			if (direct && native_code == EXDEV)
				goto copy_temporary;
			if (!fstatat(builder->parent_fd, builder->leaf,
					&pinned, AT_SYMLINK_NOFOLLOW) &&
				pg_native_stat_same(
					&created, &pinned)) {
				published = 1;
				status = PG_IO;
			} else if (native_code == EEXIST) {
				status = PG_STALE;
			} else {
				status = PG_INDETERMINATE;
			}
			goto remove_temporary;
		}
		published = 1;
		if (unlinkat(temporary_parent, temporary_leaf, 0)) {
			native_code = errno;
			status = PG_IO;
			goto remove_temporary;
		}
	}
	if (fsync(builder->parent_fd)) {
		native_code = errno;
		status = PG_IO;
		goto remove_temporary;
	}
	builder->published = 1;
	free(temporary);
	return pg_result(PG_OK, error);

copy_temporary:
	/* Bind mounts can reject publication even when device IDs match. */
	free(temporary);
	direct = 0;
	temporary_parent = builder->parent_fd;
	native_code = 0;
	goto prepare_temporary;

remove_temporary:
	builder->published = published;
	/* Only the original payload file belongs to the checkpoint. */
	if (!published && (!builder->session_path || !direct) &&
		status != PG_INDETERMINATE)
		unlink(temporary);
	free(temporary);
	if (published)
		return pg_builder_effect(
			PG_COMMITTED, status, native_code, error);
	if (status == PG_INDETERMINATE)
		return pg_builder_effect(
			status, PG_IO, native_code, error);
	return pg_native_result(status, native_code, error);
}

/* Accept a null owned handle as a no-op. */
/* Check close constraints; release resources and references. */
/* Free the installed entry index. */
/* Clear the owned pointer once close is accepted. */
PG_API pg_status PG_CALL pg_archive_builder_close(
	pg_archive_builder **builder, pg_error *error)
{
	pg_archive_builder *owned;
	int closed;
	int native_code;

	if (!builder)
		return pg_result(PG_INVALID, error);
	if (!*builder)
		return pg_result(PG_OK, error);
	owned = *builder;
	if (owned->visiting)
		return pg_result(PG_REENTRANT, error);
	if (owned->live_writers)
		return pg_result(PG_BUSY, error);
	*builder = NULL;
	struct stat staged = {};
	int staged_identity = owned->staging
		? fstat(fileno(owned->staging), &staged) == 0
		: 0;
	if (owned->published && owned->session_path) {
		staged.st_dev =
			(decltype(staged.st_dev))owned->original_stage_device;
		staged.st_ino =
			(decltype(staged.st_ino))owned->original_stage_inode;
		staged_identity = 1;
	}
	closed = owned->staging ? fclose(owned->staging) : 0;
	native_code = closed ? errno : 0;
	if (close(owned->parent_fd) && !closed) {
		closed = -1;
		native_code = errno;
	}
	if (owned->staging_path && !owned->retain_staging &&
		(!owned->session_path || owned->discard_session ||
			owned->published)) {
		struct stat named;
		int removed = 0;

		if (fstatat(owned->staging_parent_fd, owned->staging_leaf,
			&named, AT_SYMLINK_NOFOLLOW)) {
			if (errno != ENOENT)
				removed = -1;
		} else if (!staged_identity || staged.st_dev != named.st_dev ||
			staged.st_ino != named.st_ino) {
			removed = -1;
			errno = EIO;
		} else
			removed = unlinkat(owned->staging_parent_fd,
				owned->staging_leaf, 0);
		if (removed && !closed) {
			closed = -1;
			native_code = errno;
		}
	}
	if (owned->staging_parent_fd >= 0 && close(owned->staging_parent_fd) &&
		!closed) {
		closed = -1;
		native_code = errno;
	}
	free(owned->staging_directory);
	free(owned->staging_path);
	free(owned->staging_leaf);
	if (owned->session_path) {
		if (owned->session_lock >= 0 && close(owned->session_lock) &&
			!closed) {
			closed = -1;
			native_code = errno;
		}
		if (owned->session_fd >= 0 && close(owned->session_fd) &&
			!closed) {
			closed = -1;
			native_code = errno;
		}
		free(owned->session_path);
	}
	pg_builder_entry *entry = owned->entries;

	while (entry) {
		pg_builder_entry *next = entry->next;

		free(entry->canonical_name);
		free(entry->original_name);
		free(entry->cached_header);
		free(entry);
		entry = next;
	}
	pg_context_child_drop(owned->context);
	free(owned->leaf);
	free(owned->native_path);
	free(owned);
	return pg_native_result(
		closed ? PG_IO : PG_OK, native_code, error);
}

} /* extern "C" */

/* Validate profile and create private archive staging. */
pg_status pg_archive_builder_create_options(pg_context *context,
	const char *path, const pg_archive_options *options,
	pg_archive_builder **out, pg_error *error)
{
	if (out)
		*out = NULL;
	if (!out || !options ||
		options->checksum_domain > PG_CHECKSUM_STORED)
		return pg_result(PG_INVALID, error);
	if (options->checksum_domain == PG_CHECKSUM_STORED &&
		options->format != PG_HOGG10)
		return pg_result(PG_UNSUPPORTED, error);
	pg_archive_build_options config = {*options, NULL, 0};
	pg_status status = pg_archive_builder_create_config(
		context, path, &config, out, error);

	if (status == PG_OK)
		(*out)->checksum_domain = options->checksum_domain;
	return status;
}

static pg_status pg_archive_builder_finish_coordinated(
	pg_archive_builder *builder, pg_error *error)
{
	if (!builder || builder->finished)
		return pg_result(PG_INVALID, error);
	if (builder->visiting)
		return pg_result(PG_REENTRANT, error);
	if (builder->live_writers)
		return pg_result(PG_BUSY, error);
	if (builder->session_path) {
		pg_status checkpointed =
			pg_archive_builder_checkpoint(builder, error);

		if (checkpointed != PG_OK) {
			builder->finished = 1;
			int native_code = error ? error->native_code : 0;
			return pg_native_result(PG_IO, native_code, error);
		}
	}
	int lease = -1, native_code = 0;
	pg_status status = pg_native_target_lease(builder->native_path,
		builder->target_exists, &lease, &native_code);

	if (status == PG_OK)
		status = pg_archive_builder_finish_locked(
			builder, error);
	else {
		builder->finished = 1;
		pg_native_result(status, native_code, error);
	}
	if (lease >= 0)
		close(lease);
	return status;
}

pg_status pg_archive_builder_finish(
	pg_archive_builder *builder, pg_error *error)
{
	pg_context *context = builder ? builder->context : NULL;

	pg_context_lock(context);
	pg_status status =
		pg_archive_builder_finish_coordinated(builder, error);

	pg_context_unlock(context);
	return status;
}

/* Validate capacity before allocating destination resources. */

pg_status pg_archive_builder_create_config(pg_context *context,
	const char *path, const pg_archive_build_options *options,
	pg_archive_builder **out, pg_error *error)
{
	if (out)
		*out = NULL;
	if (!out || !options ||
		options->archive.checksum_domain > PG_CHECKSUM_STORED)
		return pg_result(PG_INVALID, error);
	if ((options->hogg_user_capacity ||
		options->archive.checksum_domain == PG_CHECKSUM_STORED) &&
		options->archive.format == PG_PIGG2)
		return pg_result(PG_UNSUPPORTED, error);
	if (options->hogg_user_capacity >= UINT32_MAX / 48)
		return pg_result(PG_LIMIT, error);
	pg_status status =
		pg_builder_create(context, path, options->archive.format,
			options->archive.flags, options->staging_directory,
			options->hogg_user_capacity, out, error);

	if (status == PG_OK)
		(*out)->checksum_domain = options->archive.checksum_domain;
	return status;
}

pg_status pg_archive_builder_create(pg_context *context, const char *path,
	uint32_t format, uint32_t flags, pg_archive_builder **out,
	pg_error *error)
{
	pg_archive_build_options options = {{format, flags, 0}, NULL, 0};

	return pg_archive_builder_create_config(
		context, path, &options, out, error);
}
