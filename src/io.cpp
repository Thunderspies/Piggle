#include <piggle/piggle.h>
#include "internal.hpp"
#include "digest.hpp"
#include "archive_read.hpp"
#include "native_path.hpp"

#include <zlib-ng.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>

struct pg_archive_checkpoint;

struct pg_archive_decoder {
	zng_stream stream;
	uint8_t input[65536];
	uint64_t loaded;
	int ended;
	int indexed;
	size_t checkpoint_count;
	pg_archive_checkpoint *checkpoints;
};

struct pg_archive_checkpoint {
	pg_archive_checkpoint *next;
	uint64_t position;
	pg_archive_decoder decoder;
};

static const uint64_t pg_checkpoint_interval = 1048576;
static const size_t pg_checkpoint_limit = 64;

/* Decoder copies own their stream state and pending compressed input. */
static int pg_decoder_copy(pg_archive_decoder *to,
		pg_archive_decoder *from)
{
	int result = zng_inflateCopy(&to->stream, &from->stream);

	if (result != Z_OK)
		return result;
	memcpy(to->input, from->input, sizeof(to->input));
	if (from->stream.next_in)
		to->stream.next_in = to->input +
			(from->stream.next_in - from->input);
	to->stream.next_out = NULL;
	to->stream.avail_out = 0;
	to->loaded = from->loaded;
	to->ended = from->ended;
	return Z_OK;
}

static void pg_checkpoint_dispose(pg_archive_checkpoint *checkpoint)
{
	zng_inflateEnd(&checkpoint->decoder.stream);
	free(checkpoint);
}

/* Optional cache failures never turn a successful read into a failure. */
static void pg_checkpoint_save(pg_reader *reader)
{
	pg_archive_decoder *decoder = (pg_archive_decoder *)reader->decoder;
	pg_archive_checkpoint **link;
	pg_archive_checkpoint *checkpoint;

	if (!decoder->indexed || !reader->position ||
	    reader->position % pg_checkpoint_interval || decoder->ended)
		return;
	for (link = &decoder->checkpoints; *link; link = &(*link)->next) {
		if ((*link)->position != reader->position)
			continue;
		checkpoint = *link;
		*link = checkpoint->next;
		checkpoint->next = decoder->checkpoints;
		decoder->checkpoints = checkpoint;
		return;
	}
	checkpoint = (pg_archive_checkpoint *)calloc(1, sizeof(*checkpoint));
	if (!checkpoint)
		return;
	if (pg_decoder_copy(&checkpoint->decoder, decoder) != Z_OK) {
		free(checkpoint);
		return;
	}
	checkpoint->position = reader->position;
	checkpoint->next = decoder->checkpoints;
	decoder->checkpoints = checkpoint;
	if (decoder->checkpoint_count < pg_checkpoint_limit) {
		decoder->checkpoint_count++;
		return;
	}
	for (link = &decoder->checkpoints; (*link)->next;
	     link = &(*link)->next)
		;
	pg_checkpoint_dispose(*link);
	*link = NULL;
}

/* Restore the nearest preceding cached state, or the initial decoder. */
static pg_status pg_decoder_reposition(pg_reader *reader, uint64_t offset)
{
	pg_archive_decoder *decoder = (pg_archive_decoder *)reader->decoder;
	pg_archive_checkpoint **best = NULL;

	for (pg_archive_checkpoint **link = &decoder->checkpoints;
	     *link; link = &(*link)->next) {
		if ((*link)->position <= offset &&
		    (!best || (*link)->position > (*best)->position))
			best = link;
	}
	if (reader->position <= offset &&
	    (!best || (*best)->position <= reader->position))
		return PG_OK;
	int result;

	if (best) {
		pg_archive_checkpoint *checkpoint = *best;

		zng_inflateEnd(&decoder->stream);
		memset(&decoder->stream, 0, sizeof(decoder->stream));
		result = pg_decoder_copy(decoder, &checkpoint->decoder);
		if (result == Z_OK)
			reader->position = checkpoint->position;
		*best = checkpoint->next;
		checkpoint->next = decoder->checkpoints;
		decoder->checkpoints = checkpoint;
	} else {
		result = zng_inflateReset(&decoder->stream);
		if (result == Z_OK) {
			decoder->stream.next_in = NULL;
			decoder->stream.avail_in = 0;
			decoder->stream.next_out = NULL;
			decoder->stream.avail_out = 0;
			decoder->loaded = 0;
			decoder->ended = 0;
			reader->position = 0;
		}
	}
	return result == Z_OK ? PG_OK :
		(result == Z_MEM_ERROR ? PG_NOMEM : PG_CORRUPT);
}

/* Copy through verified EOF; leave closing and publication to the caller. */
pg_status pg_transfer_bytes(pg_reader *reader, pg_writer *writer,
		pg_error *error)
{
	uint8_t bytes[65536];

	for (;;) {
		size_t count;
		pg_status status = pg_reader_read(reader, bytes, sizeof(bytes),
			&count, error);

		if (status == PG_END)
			return pg_result(PG_OK, error);
		if (status != PG_OK)
			return status;
		size_t accepted;

		status = pg_writer_write(writer, bytes, count, &accepted,
			error);
		if (status != PG_OK)
			return status;
		if (accepted != count)
			return pg_result(PG_IO, error);
	}
}

static int pg_native_same(const struct stat *before, const struct stat *after)
{
	return before->st_dev == after->st_dev &&
		before->st_ino == after->st_ino &&
		before->st_size == after->st_size &&
		before->st_mtime == after->st_mtime
#if defined(__linux__)
		&& before->st_mtim.tv_nsec == after->st_mtim.tv_nsec
#elif defined(__APPLE__)
		&& before->st_mtimespec.tv_nsec == after->st_mtimespec.tv_nsec
#elif defined(_WIN32)
		&& before->st_mtime_nsec == after->st_mtime_nsec
#endif
		;
}

static pg_status pg_reader_native_check(pg_reader *reader, int *native_code)
{
	struct stat current;

	if (reader->source && reader->source->format == PG_HOGG10) {
		pg_source *source = reader->source;
		pg_source_record *record;

		if (source->recovery_required)
			return PG_RECOVERY_REQUIRED;
		for (record = source->records; record; record = record->next) {
			if (record->info.copy_id == reader->file_info.copy_id)
				break;
		}
		if (!record || record->info.copy_generation !=
		    reader->file_info.copy_generation)
			return PG_STALE;
		reader->identity = source->identity;
		reader->payload_offset = record->archive->payload_offset;
	}
	if (fstat(reader->fd, &current)) {
		*native_code = errno;
		return PG_IO;
	}
	if (!pg_native_same(&reader->identity, &current))
		return PG_STALE;
	/* Native readers retain their opened object across namespace changes.
	 * PIGG/HOGG retain their namespace and archive validation.
	 */
	if (!reader->source || reader->source->format == PG_LOOSE)
		return PG_OK;
	if (lstat(reader->native_path, &current)) {
		if (errno == ENOENT || errno == ENOTDIR)
			return PG_STALE;
		*native_code = errno;
		return PG_IO;
	}
	if (!S_ISREG(current.st_mode) ||
	    !pg_native_same(&reader->identity, &current))
		return PG_STALE;
	return PG_OK;
}

static pg_status pg_archive_read_decoded(pg_reader *reader,
		void *buffer, size_t capacity, size_t *bytes)
{
	pg_archive_decoder *decoder =
		(pg_archive_decoder *)reader->decoder;
	uint64_t remaining_output = reader->info.size - reader->position;
	size_t limit = capacity < 65536 ? capacity : 65536;

	if (limit > remaining_output)
		limit = (size_t)remaining_output;
	if (decoder->indexed) {
		uint64_t boundary = pg_checkpoint_interval -
			reader->position % pg_checkpoint_interval;

		if (limit > boundary)
			limit = (size_t)boundary;
	}
	while (!*bytes) {
		if (!decoder->stream.avail_in &&
		    decoder->loaded < reader->file_info.stored_size) {
			uint64_t remaining = reader->file_info.stored_size -
				decoder->loaded;
			size_t amount = remaining < sizeof(decoder->input) ?
				(size_t)remaining : sizeof(decoder->input);
			pg_status status = pg_read_span(reader->fd,
				(uint64_t)reader->identity.st_size,
				reader->payload_offset + decoder->loaded,
				decoder->input, amount);

			if (status != PG_OK)
				return status;
			decoder->loaded += amount;
			decoder->stream.next_in = decoder->input;
			decoder->stream.avail_in = (uint32_t)amount;
		}
		decoder->stream.next_out = (uint8_t *)buffer;
		decoder->stream.avail_out = (uint32_t)limit;
		uint32_t before = decoder->stream.avail_in;
		int result = zng_inflate(&decoder->stream, Z_NO_FLUSH);

		*bytes = limit - decoder->stream.avail_out;
		reader->position += *bytes;
		if (result == Z_STREAM_END) {
			decoder->ended = 1;
			if (reader->position != reader->info.size ||
			    decoder->stream.avail_in ||
			    decoder->loaded != reader->file_info.stored_size)
				return PG_CORRUPT;
		} else if (result != Z_OK && result != Z_BUF_ERROR) {
			return result == Z_MEM_ERROR ? PG_NOMEM : PG_CORRUPT;
		}
		if (*bytes)
			return PG_OK;
		if (decoder->ended ||
		    (before == decoder->stream.avail_in &&
			decoder->loaded == reader->file_info.stored_size))
			return PG_CORRUPT;
	}
	return PG_OK;
}

static pg_status pg_normalize_owned(pg_context *context, const char *name,
		char **out)
{
	size_t required = 0;
	pg_status status;
	char *canonical;

	*out = NULL;
	status = pg_name_normalize(context, name, NULL, 0, &required, NULL);
	if (status != PG_CAPACITY)
		return status;
	canonical = (char *)malloc(required);
	if (!canonical)
		return PG_NOMEM;
	status = pg_name_normalize(context, name, canonical, required,
		&required, NULL);
	if (status != PG_OK) {
		free(canonical);
		return status;
	}
	*out = canonical;
	return PG_OK;
}

static char *pg_display_owned(const char *name)
{
	char *display = (char *)malloc(strlen(name) + 1);
	const char *at = name;
	size_t used = 0;

	if (!display)
		return NULL;
	while (*at) {
		const char *start;
		size_t component;

		while (*at == '/' || *at == '\\')
			at++;
		start = at;
		while (*at && *at != '/' && *at != '\\')
			at++;
		component = (size_t)(at - start);
		if (!component || (component == 1 && *start == '.'))
			continue;
		if (used)
			display[used++] = '/';
		memcpy(display + used, start, component);
		used += component;
	}
	display[used] = '\0';
	return display;
}

static pg_status pg_builder_options_check(pg_archive_builder *builder,
		const pg_write_options *options)
{
	const pg_entry_options *entry = &options->entry;
	size_t i;

	if (options->encoding != PG_LOGICAL && options->encoding != PG_ZLIB)
		return PG_INVALID;
	if (options->encoding == PG_LOGICAL &&
	    options->input_size != options->logical_size)
		return PG_INVALID;
	if (options->encoding == PG_ZLIB && !options->input_size)
		return PG_INVALID;
	if (entry->compression > PG_COMPRESS_FORCE ||
	    entry->digest_kind > PG_DIGEST_MD5_32 ||
	    entry->expected_digest_domain > PG_CHECKSUM_STORED ||
	    (entry->cached_header_size && !entry->cached_header))
		return PG_INVALID;
	if (entry->cached_header_size > UINT32_MAX)
		return PG_LIMIT;
	if (builder->format == PG_PIGG2) {
		if (entry->mtime < 0 || entry->mtime > UINT32_MAX ||
		    options->logical_size > UINT32_MAX ||
		    options->input_size > UINT32_MAX)
			return PG_LIMIT;
	} else if (entry->mtime < INT32_MIN || entry->mtime > INT32_MAX ||
		   options->logical_size > UINT32_MAX ||
		   options->input_size > UINT32_MAX) {
		return PG_LIMIT;
	}
	for (i = entry->digest_kind == PG_DIGEST_MD5 ? 16 :
	     entry->digest_kind == PG_DIGEST_MD5_32 ? 4 : 0;
	     i < sizeof(entry->expected_digest); i++) {
		if (entry->expected_digest[i])
			return PG_INVALID;
	}
	return PG_OK;
}

static void pg_writer_release_memory(pg_writer *writer)
{
	free(writer->canonical_name);
	free(writer->original_name);
	free(writer->cached_header);
	free(writer);
}

static pg_status pg_writer_hash_raw(pg_writer *writer, pg_md5 *hash,
		FILE *output)
{
	uint8_t bytes[65536];
	uint64_t remaining = writer->options.logical_size;

	if (fseeko(writer->input, 0, SEEK_SET))
		return PG_IO;
	while (remaining) {
		size_t amount = remaining < sizeof(bytes) ?
			(size_t)remaining : sizeof(bytes);

		if (fread(bytes, 1, amount, writer->input) != amount)
			return PG_IO;
		pg_md5_update(hash, bytes, amount);
		if (output && fwrite(bytes, 1, amount, output) != amount)
			return PG_IO;
		remaining -= amount;
	}
	return PG_OK;
}

static pg_status pg_writer_verify_zlib(pg_writer *writer, pg_md5 *hash,
		FILE *output_file)
{
	zng_stream stream = {};
	uint8_t input[65536];
	uint8_t output[65536];
	uint64_t remaining = writer->options.input_size;
	uint64_t decoded = 0;
	pg_status status = PG_OK;
	int ended = 0;

	if (fseeko(writer->input, 0, SEEK_SET))
		return PG_IO;
	int result = zng_inflateInit(&stream);
	if (result != Z_OK)
		return result == Z_MEM_ERROR ? PG_NOMEM : PG_CORRUPT;
	while (remaining && !ended) {
		size_t amount = remaining < sizeof(input) ?
			(size_t)remaining : sizeof(input);

		if (fread(input, 1, amount, writer->input) != amount) {
			status = PG_IO;
			break;
		}
		remaining -= amount;
		stream.next_in = input;
		stream.avail_in = (uint32_t)amount;
		do {
			uint32_t before = stream.avail_in;
			size_t produced;

			stream.next_out = output;
			stream.avail_out = sizeof(output);
			result = zng_inflate(&stream, Z_NO_FLUSH);
			produced = sizeof(output) - stream.avail_out;
			if (produced > writer->options.logical_size - decoded) {
				status = PG_CORRUPT;
				break;
			}
			pg_md5_update(hash, output, produced);
			if (output_file && fwrite(output, 1, produced,
				output_file) != produced) {
				status = PG_IO;
				break;
			}
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
		if (status != PG_OK)
			break;
	}
	zng_inflateEnd(&stream);
	if (status == PG_OK && (!ended ||
	    decoded != writer->options.logical_size))
		status = PG_CORRUPT;
	return status;
}

static pg_status pg_writer_compress(pg_writer *writer, FILE **out,
		uint64_t *stored_size)
{
	zng_stream stream = {};
	uint8_t input[65536];
	uint8_t output[65536];
	uint64_t remaining = writer->options.logical_size;
	pg_status status = PG_OK;
	int result;

	*out = tmpfile();
	if (!*out)
		return PG_IO;
	if (fseeko(writer->input, 0, SEEK_SET)) {
		status = PG_IO;
		goto cleanup_file;
	}
	result = zng_deflateInit(&stream, Z_DEFAULT_COMPRESSION);
	if (result != Z_OK) {
		status = result == Z_MEM_ERROR ? PG_NOMEM : PG_IO;
		goto cleanup_file;
	}
	while (remaining) {
		size_t amount = remaining < sizeof(input) ?
			(size_t)remaining : sizeof(input);
		int flush;

		if (fread(input, 1, amount, writer->input) != amount) {
			status = PG_IO;
			break;
		}
		remaining -= amount;
		flush = remaining ? Z_NO_FLUSH : Z_FINISH;
		stream.next_in = input;
		stream.avail_in = (uint32_t)amount;
		do {
			size_t produced;

			stream.next_out = output;
			stream.avail_out = sizeof(output);
			result = zng_deflate(&stream, flush);
			if (result != Z_OK && result != Z_STREAM_END) {
				status = result == Z_MEM_ERROR ? PG_NOMEM : PG_IO;
				break;
			}
			produced = sizeof(output) - stream.avail_out;
			if (produced > UINT32_MAX - *stored_size) {
				status = PG_LIMIT;
				break;
			}
			if (fwrite(output, 1, produced, *out) != produced) {
				status = PG_IO;
				break;
			}
			*stored_size += produced;
		} while (result != Z_STREAM_END && (stream.avail_in ||
			(flush == Z_FINISH && result != Z_STREAM_END) ||
			stream.avail_out == 0));
		if (status != PG_OK)
			break;
	}
	zng_deflateEnd(&stream);
	if (status != PG_OK)
		goto cleanup_file;
	if (fflush(*out)) {
		status = PG_IO;
		goto cleanup_file;
	}
	return PG_OK;

cleanup_file:
	fclose(*out);
	*out = NULL;
	return status;
}

static pg_status pg_writer_stage(pg_writer *writer)
{
	pg_archive_builder *builder = writer->builder;
	pg_builder_entry *entry = NULL;
	pg_md5 hash;
	uint8_t digest[16], stored_digest[16];
	pg_md5 stored_hash;
	uint64_t stored_size = writer->options.input_size;
	uint32_t encoding = writer->options.encoding;
	FILE *compressed = NULL;
	FILE *payload = writer->input;
	pg_status status;
	off_t offset = 0;
	uint8_t bytes[65536];
	uint64_t remaining;

	if (writer->accepted != writer->options.input_size)
		return PG_INVALID;
	if (fflush(writer->input))
		return PG_IO;
	pg_md5_init(&hash);
	if (writer->options.encoding == PG_ZLIB)
		status = pg_writer_verify_zlib(writer, &hash, NULL);
	else
		status = pg_writer_hash_raw(writer, &hash, NULL);
	if (status != PG_OK)
		return status;
	pg_md5_finish(&hash, digest);
	if (writer->options.entry.digest_kind != PG_DIGEST_NONE &&
	    writer->options.entry.expected_digest_domain ==
		PG_CHECKSUM_LOGICAL) {
		size_t count = writer->options.entry.digest_kind ==
			PG_DIGEST_MD5 ? 16 : 4;

		if (memcmp(digest, writer->options.entry.expected_digest,
			count) != 0)
			return PG_CHECKSUM;
	}
	if (writer->options.encoding == PG_ZLIB &&
	    !writer->options.logical_size) {
		stored_size = 0;
		encoding = PG_LOGICAL;
	}
	if (writer->options.encoding == PG_LOGICAL &&
	    writer->options.logical_size &&
	    writer->options.entry.compression != PG_COMPRESS_NEVER) {
		stored_size = 0;
		status = pg_writer_compress(writer, &compressed, &stored_size);
		if (status != PG_OK)
			return status;
		if (writer->options.entry.compression == PG_COMPRESS_FORCE ||
		    stored_size < writer->options.logical_size) {
			payload = compressed;
			encoding = PG_ZLIB;
		} else {
			if (fclose(compressed))
				return PG_IO;
			compressed = NULL;
			stored_size = writer->options.logical_size;
		}
	}
	if (builder->format == PG_HOGG10 && stored_size > 0xfffffffeu) {
		status = PG_LIMIT;
		goto cleanup;
	}
	entry = (pg_builder_entry *)calloc(1, sizeof(*entry));
	if (!entry) {
		status = PG_NOMEM;
		goto cleanup;
	}
	if (fseeko(builder->staging, 0, SEEK_END)) {
		status = PG_IO;
		goto cleanup;
	}
	offset = ftello(builder->staging);
	if (offset < 0 || fseeko(payload, 0, SEEK_SET)) {
		status = PG_IO;
		goto cleanup;
	}
	pg_md5_init(&stored_hash);
	remaining = stored_size;
	while (remaining) {
		size_t amount = remaining < sizeof(bytes) ?
			(size_t)remaining : sizeof(bytes);

		if (fread(bytes, 1, amount, payload) != amount ||
		    fwrite(bytes, 1, amount, builder->staging) != amount) {
			status = PG_IO;
			goto cleanup;
		}
		pg_md5_update(&stored_hash, bytes, amount);
		remaining -= amount;
	}
	if (fflush(builder->staging)) {
		status = PG_IO;
		goto cleanup;
	}
	pg_md5_finish(&stored_hash, stored_digest);
	if (writer->options.entry.digest_kind != PG_DIGEST_NONE &&
	    writer->options.entry.expected_digest_domain ==
		PG_CHECKSUM_STORED) {
		size_t count = writer->options.entry.digest_kind ==
			PG_DIGEST_MD5 ? 16 : 4;

		if (memcmp(stored_digest, writer->options.entry.expected_digest,
			count)) {
			status = PG_CHECKSUM;
			goto cleanup;
		}
	}
	if (builder->checksum_domain == PG_CHECKSUM_STORED)
		memcpy(digest, stored_digest, sizeof(digest));
	status = PG_OK;

cleanup:
	if (compressed && fclose(compressed) && status == PG_OK)
		status = PG_IO;
	if (fclose(writer->input) && status == PG_OK)
		status = PG_IO;
	writer->input = NULL;
	if (status != PG_OK) {
		free(entry);
		return status;
	}
	entry->canonical_name = writer->canonical_name;
	entry->original_name = writer->original_name;
	entry->cached_header = writer->cached_header;
	entry->cached_header_size = writer->options.entry.cached_header_size;
	entry->stage_offset = (uint64_t)offset;
	entry->logical_size = writer->options.logical_size;
	entry->stored_size = stored_size;
	entry->mtime = writer->options.entry.mtime;
	entry->encoding = encoding;
	memcpy(entry->digest, digest, sizeof(digest));
	if (builder->format == PG_PIGG2 && !entry->logical_size)
		memset(entry->digest, 0, sizeof(entry->digest));
	writer->canonical_name = NULL;
	writer->original_name = NULL;
	writer->cached_header = NULL;
	pg_builder_entry **tail = &builder->entries;

	while (*tail)
		tail = &(*tail)->next;
	*tail = entry;
	return PG_OK;
}

static pg_status pg_writer_native_recheck(pg_writer *writer,
		int *native_code)
{
	struct stat current, pinned;
	char *leaf = NULL;
	int parent = -1;
	pg_status status = pg_native_parent_open(writer->native_path, 0,
		&parent, &leaf, native_code);

	if (status != PG_OK)
		return status == PG_CONFLICT || *native_code == ENOENT ?
			PG_STALE : status;
	if (fstat(parent, &current) || fstat(writer->parent_fd, &pinned)) {
		*native_code = errno;
		status = PG_IO;
	} else if (current.st_dev != pinned.st_dev ||
		   current.st_ino != pinned.st_ino ||
		   strcmp(leaf, writer->leaf)) {
		status = PG_STALE;
	}
	close(parent);
	free(leaf);
	if (status != PG_OK)
		return status;
	if (!fstatat(writer->parent_fd, writer->leaf, &current,
		AT_SYMLINK_NOFOLLOW)) {
		if (!S_ISREG(current.st_mode))
			return PG_CONFLICT;
		if (!writer->target_exists ||
		    !pg_native_stat_same(&current, &writer->target))
			return PG_STALE;
	} else if (errno == ENOENT) {
		if (writer->target_exists)
			return PG_STALE;
	} else {
		*native_code = errno;
		return PG_IO;
	}
	return PG_OK;
}

static pg_status pg_writer_native_stage_locked(pg_writer *writer,
		int *native_code, pg_status *cause)
{
	char temporary_leaf[80];
	FILE *out;
	pg_md5 hash;
	uint8_t digest[16];
	pg_status status;
	int fd;
	int published = 0;
	static unsigned long sequence;
	struct timespec times[2];
	struct stat created, current;
	unsigned int attempt;

	if (writer->accepted != writer->options.input_size)
		return PG_INVALID;
	if ((int64_t)(time_t)writer->options.entry.mtime !=
	    writer->options.entry.mtime)
		return PG_LIMIT;
	for (attempt = 0; attempt < 128; attempt++) {
		snprintf(temporary_leaf, sizeof(temporary_leaf),
			".piggle-%ld-%lu", (long)getpid(), ++sequence);
		fd = openat(writer->parent_fd, temporary_leaf,
			O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW | O_CLOEXEC,
			0600);
		if (fd >= 0 || errno != EEXIST)
			break;
	}
	if (fd < 0) {
		*native_code = errno;
		return errno == EEXIST ? PG_RETRY : PG_IO;
	}
	out = fdopen(fd, "wb");
	if (!out) {
		*native_code = errno;
		close(fd);
		status = PG_IO;
		goto cleanup;
	}
	if (fflush(writer->input)) {
		*native_code = errno;
		status = PG_IO;
		goto close_output;
	}
	pg_md5_init(&hash);
	status = writer->options.encoding == PG_ZLIB ?
		pg_writer_verify_zlib(writer, &hash, out) :
		pg_writer_hash_raw(writer, &hash, out);
	if (status != PG_OK)
		goto close_output;
	pg_md5_finish(&hash, digest);
	if (writer->options.entry.digest_kind != PG_DIGEST_NONE) {
		size_t count = writer->options.entry.digest_kind ==
			PG_DIGEST_MD5 ? 16 : 4;

		if (memcmp(digest, writer->options.entry.expected_digest,
			count)) {
			status = PG_CHECKSUM;
			goto close_output;
		}
	}
	if (fflush(out)) {
		*native_code = errno;
		status = PG_IO;
		goto close_output;
	}
	times[0].tv_sec = (time_t)writer->options.entry.mtime;
	times[0].tv_nsec = 0;
	times[1] = times[0];
	if (futimens(fd, times) || fsync(fd) || fstat(fd, &created)) {
		*native_code = errno;
		status = PG_IO;
	}
close_output:
	if (fclose(out) && status == PG_OK) {
		*native_code = errno;
		status = PG_IO;
	}
	if (status != PG_OK)
		goto cleanup;
	status = pg_writer_native_recheck(writer, native_code);
	if (status != PG_OK)
		goto cleanup;
	status = pg_native_alias(writer->context, writer->parent_fd,
		&writer->target, writer->target_exists, writer->source,
		native_code);
	if (status != PG_OK)
		goto cleanup;
	if (fstatat(writer->parent_fd, temporary_leaf, &current,
		AT_SYMLINK_NOFOLLOW) ||
	    !pg_native_stat_same(&created, &current)) {
		*native_code = errno;
		status = PG_RETRY;
		goto cleanup;
	}
	if (writer->flags & PG_OVERWRITE) {
		if (renameat(writer->parent_fd, temporary_leaf,
			writer->parent_fd, writer->leaf)) {
			*native_code = errno;
			status = PG_IO;
			goto cleanup;
		}
	} else if (linkat(writer->parent_fd, temporary_leaf,
			writer->parent_fd, writer->leaf, 0)) {
		*native_code = errno;
		status = errno == EEXIST ? PG_STALE : PG_IO;
		goto cleanup;
	}
	published = 1;
	if (!(writer->flags & PG_OVERWRITE) &&
	    unlinkat(writer->parent_fd, temporary_leaf, 0)) {
		*native_code = errno;
		status = PG_IO;
		goto cleanup;
	}
	if (fsync(writer->parent_fd)) {
		*native_code = errno;
		status = PG_IO;
		goto cleanup;
	}
	status = PG_OK;
cleanup:
	if (!published)
		unlinkat(writer->parent_fd, temporary_leaf, 0);
	if (published && status != PG_OK) {
		*cause = status;
		return PG_COMMITTED;
	}
	return status;
}

static pg_status pg_unpack_output_path(const char *root,
		const char *name, char **out)
{
	size_t prefix = strlen(root);
	size_t length = strlen(name);
	char *path;

	*out = NULL;
	if (prefix > SIZE_MAX - length - 2)
		return PG_LIMIT;
	path = (char *)malloc(prefix + length + 2);
	if (!path)
		return PG_NOMEM;
	memcpy(path, root, prefix);
	path[prefix] = '/';
	memcpy(path + prefix + 1, name, length + 1);
	*out = path;
	return PG_OK;
}

static pg_status pg_unpack_root_check(pg_unpack_target *target,
		int *native_code)
{
	struct stat pinned, current;

	if (fstat(target->root_fd, &pinned) ||
	    lstat(target->root_path, &current)) {
		*native_code = errno;
		return errno == ENOENT ? PG_STALE : PG_IO;
	}
	if (!S_ISDIR(current.st_mode) ||
	    pinned.st_dev != target->root_identity.st_dev ||
	    pinned.st_ino != target->root_identity.st_ino ||
	    current.st_dev != pinned.st_dev ||
	    current.st_ino != pinned.st_ino)
		return PG_STALE;
	return PG_OK;
}

extern "C" {

/* Validate name and representation; select one source-visible file. */
/* Open an independent reader and release temporary selection. */
/* Publish reader after cleanup; preserve first failure. */
PG_API pg_status PG_CALL pg_reader_open_source(pg_source *source,
		const char *name, uint32_t representation, pg_reader **out,
		pg_error *error)
{
	pg_file *file = NULL;
	pg_reader *reader = NULL;
	pg_error work_error;
	pg_error close_error;
	pg_status status;
	pg_status close_status;

	if (!out)
		return pg_result(PG_INVALID, error);
	*out = NULL;
	if (!source || !name || representation > PG_READ_STORED)
		return pg_result(PG_INVALID, error);
	status = pg_source_find(source, name, &file, &work_error);
	if (status != PG_OK) {
		if (error)
			*error = work_error;
		return status;
	}
	status = pg_reader_open(file, representation, &reader,
		&work_error);
	close_status = pg_file_close(&file, &close_error);
	if (status == PG_OK && close_status != PG_OK) {
		status = close_status;
		work_error = close_error;
	}
	if (status == PG_OK) {
		*out = reader;
	} else if (reader) {
		pg_reader_close(&reader, NULL);
	}
	if (error)
		*error = work_error;
	return status;
}

/* Resolve one visible file and open its independent reader. */
PG_API pg_status PG_CALL pg_reader_open_tree(pg_tree *tree,
		const char *name, uint32_t representation, pg_reader **out,
		pg_error *error)
{
	pg_file *file = NULL;
	pg_reader *reader = NULL;
	pg_error work_error, close_error;
	pg_status status, closed;

	if (!out)
		return pg_result(PG_INVALID, error);
	*out = NULL;
	if (!tree || !name || representation > PG_READ_STORED)
		return pg_result(PG_INVALID, error);
	status = pg_tree_find(tree, name, &file, &work_error);
	if (status != PG_OK) {
		if (error)
			*error = work_error;
		return status;
	}
	status = pg_reader_open(file, representation, &reader,
		&work_error);
	closed = pg_file_close(&file, &close_error);
	if (status == PG_OK && closed != PG_OK) {
		status = closed;
		work_error = close_error;
	}
	if (status == PG_OK)
		*out = reader;
	else
		pg_reader_close(&reader, NULL);
	if (error)
		*error = work_error;
	return status;
}

static pg_status pg_reader_track_tree(pg_file *file,
		pg_reader *reader, pg_error *error)
{
	pg_tree *tree = file->origin_tree;
	pg_tree_scope *scope = NULL;
	pg_status status;

	if (!tree || tree->closed)
		return PG_OK;
	status = pg_tree_control_status(tree, 0);
	if (status != PG_OK)
		return pg_result(status, error);
	status = pg_tree_retain(tree);
	if (status != PG_OK)
		return pg_result(status, error);
	status = pg_tree_reader_scope_add(tree, file, &scope, error);
	if (status != PG_OK) {
		pg_tree_release(tree);
		return status;
	}
	reader->watch_tree = tree;
	reader->watch_scope = scope;
	return PG_OK;
}

/* Validate representation and captured copy identity. */
/* Open independent reader and retain selected file. */
/* Publish reader after setup; unwind on failure. */
static pg_status pg_reader_open_locked(
		pg_file *file,
		uint32_t representation,
		pg_reader **out,
		pg_error *error)
{
	pg_reader *reader;
	struct stat opened;
#ifndef _WIN32
	struct stat current;
#endif
	pg_status status = PG_OK;
	int native_code = 0;
	int fd;

	if (!out)
		return pg_result(PG_INVALID, error);
	*out = NULL;
	if (!file || !file->source || representation > PG_READ_STORED)
		return pg_result(PG_INVALID, error);
	if (file->source->recovery_required)
		return pg_result(PG_RECOVERY_REQUIRED, error);
	if (pg_atomic_load(&file->source->live_readers) == SIZE_MAX)
		return pg_result(PG_LIMIT, error);
	if (file->source->format == PG_LOOSE) {
		status = pg_file_prepare_native(file);
		if (status != PG_OK) return pg_result(status, error);
		status = pg_reader_open_native(file->source->context,
			file->native_path, out, error);
		if (status == PG_NOT_FOUND)
			return pg_result(PG_STALE, error);
		if (status != PG_OK)
			return status;
		if (!pg_native_stat_same(&file->source_identity,
			&(*out)->identity)) {
			pg_reader_close(out, NULL);
			return pg_result(PG_STALE, error);
		}
		status = pg_source_retain(file->source);
		if (status != PG_OK) {
			pg_reader_close(out, NULL);
			return pg_result(status, error);
		}
		(*out)->source = file->source;
		pg_atomic_increment(&file->source->live_readers);
		pg_context_child_drop((*out)->context);
		return pg_result(PG_OK, error);
	}
	int found_copy = 0;
	uint64_t payload_offset = file->payload_offset;
	for (pg_source_record *record = file->source->records; record;
	     record = record->next) {
		if (record->info.copy_id == file->info.copy_id) {
			found_copy = 1;
			payload_offset = record->archive->payload_offset;
			if (record->info.copy_generation !=
			    file->info.copy_generation)
				return pg_result(PG_STALE, error);
		}
	}
	if (!found_copy)
		return pg_result(PG_STALE, error);
	fd = open(file->source->native_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) {
		native_code = errno;
		return pg_native_result(PG_IO, native_code, error);
	}
	if (fstat(fd, &opened)
#ifndef _WIN32
	    || lstat(file->source->native_path, &current)
#endif
	    ) {
		native_code = errno;
		close(fd);
		return pg_native_result(PG_IO, native_code, error);
	}
	if (!pg_native_stat_same(file->source->format == PG_HOGG10 ?
		&file->source->identity : &file->source_identity, &opened)
#ifndef _WIN32
	    || !pg_native_stat_same(&opened, &current)
#endif
	    ) {
		close(fd);
		return pg_result(PG_STALE, error);
	}
	reader = (pg_reader *)calloc(1, sizeof(*reader));
	if (!reader) {
		close(fd);
		return pg_result(PG_NOMEM, error);
	}
	reader->native_path = pg_native_absolute(file->source->native_path,
		&status, &native_code);
	if (!reader->native_path) {
		free(reader);
		close(fd);
		return pg_native_result(status, native_code, error);
	}
	if (representation == PG_READ_LOGICAL &&
	    file->info.encoding == PG_ZLIB) {
		pg_archive_decoder *decoder =
			(pg_archive_decoder *)calloc(1, sizeof(*decoder));

		if (!decoder) {
			free(reader->native_path);
			free(reader);
			close(fd);
			return pg_result(PG_NOMEM, error);
		}
		int result = zng_inflateInit(&decoder->stream);

		if (result != Z_OK) {
			free(decoder);
			free(reader->native_path);
			free(reader);
			close(fd);
			return pg_result(result == Z_MEM_ERROR ?
				PG_NOMEM : PG_CORRUPT, error);
		}
		reader->decoder = decoder;
	}
	status = pg_source_retain(file->source);
	if (status != PG_OK) {
		if (reader->decoder) {
			pg_archive_decoder *decoder =
				(pg_archive_decoder *)reader->decoder;

			zng_inflateEnd(&decoder->stream);
			free(decoder);
		}
		free(reader->native_path);
		free(reader);
		close(fd);
		return pg_result(status, error);
	}
	reader->context = file->source->context;
	reader->source = file->source;
	pg_atomic_increment(&file->source->live_readers);
	reader->fd = fd;
	reader->identity = opened;
	reader->file_info = file->info;
	reader->payload_offset = payload_offset;
	reader->representation = representation;
	reader->info.size = representation == PG_READ_STORED ?
		file->info.stored_size : file->info.logical_size;
	reader->info.logical_size = file->info.logical_size;
	reader->info.mtime = file->info.mtime;
	reader->info.encoding = representation == PG_READ_STORED ?
		file->info.encoding : (uint32_t)PG_LOGICAL;
	*out = reader;
	return pg_result(PG_OK, error);
}

/* Validate context and native path; reject links and specials. */
/* Capture identity, size and mtime in an independent reader. */
/* Publish only after setup; unwind native resources on failure. */
PG_API pg_status PG_CALL pg_reader_open_native(
		pg_context *context,
		const char *native_path,
		pg_reader **out,
		pg_error *error)
{
#ifndef _WIN32
	struct stat identity;
#endif
	pg_reader *reader;
	int fd;
	int flags = O_RDONLY;

	if (!out)
		return pg_result(PG_INVALID, error);
	*out = NULL;
	if (!context || !native_path || !*native_path)
		return pg_result(PG_INVALID, error);
#ifndef _WIN32
	if (lstat(native_path, &identity)) {
		int code = errno;

		if (code == ENOENT || code == ENOTDIR)
			return pg_native_result(PG_NOT_FOUND, code, error);
		return pg_native_result(PG_IO, code, error);
	}
	if (!S_ISREG(identity.st_mode))
		return pg_result(PG_CONFLICT, error);
#endif
#ifdef O_CLOEXEC
	flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
	flags |= O_NOFOLLOW;
#endif
	struct stat opened;

#ifdef _WIN32
	fd = pg_win_open_capture(native_path, flags, 0666, &opened);
#else
	fd = open(native_path, flags);
#endif
	if (fd < 0) {
		int code = errno;

		if (code == ELOOP)
			return pg_native_result(PG_CONFLICT, code, error);
		if (code == ENOENT || code == ENOTDIR)
			return pg_native_result(PG_NOT_FOUND, code, error);
		return pg_native_result(PG_IO, code, error);
	}
#ifndef _WIN32
	if (fstat(fd, &opened)) {
		int code = errno;

		close(fd);
		return pg_native_result(PG_IO, code, error);
	}
#endif
	if (!S_ISREG(opened.st_mode)
#ifndef _WIN32
	    || !pg_native_same(&identity, &opened)
#endif
	    ) {
		close(fd);
		return pg_result(PG_STALE, error);
	}
	if (opened.st_size < 0) {
		close(fd);
		return pg_result(PG_LIMIT, error);
	}
	reader = (pg_reader *)calloc(1, sizeof(*reader));
	if (!reader) {
		close(fd);
		return pg_result(PG_NOMEM, error);
	}
	if (pg_context_children(context) == SIZE_MAX) {
		free(reader);
		close(fd);
		return pg_result(PG_LIMIT, error);
	}
	reader->context = context;
	reader->fd = fd;
	reader->identity = opened;
	reader->info.size = (uint64_t)opened.st_size;
	reader->info.logical_size = (uint64_t)opened.st_size;
	reader->info.mtime = (int64_t)opened.st_mtime;
	reader->info.encoding = PG_LOGICAL;
	pg_context_child_add(context);
	*out = reader;
	return pg_result(PG_OK, error);
}

/* Validate reader and copy its captured size, encoding and mtime. */
PG_API pg_status PG_CALL pg_reader_inspect(pg_reader *reader,
		pg_reader_info *out, pg_error *error)
{
	if (out)
		memset(out, 0, sizeof(*out));
	if (!reader || !out)
		return pg_result(PG_INVALID, error);
	*out = reader->info;
	return pg_result(PG_OK, error);
}

/* Validate capacity and current reader state. */
/* Transfer at most caller capacity and advance position. */
/* Verify available logical hashes at EOF; report exact bytes. */
static pg_status pg_reader_read_locked(
		pg_reader *reader,
		void *buffer,
		size_t capacity,
		size_t *bytes,
		pg_error *error)
{
	pg_status status;
	int archive = reader && reader->source &&
		reader->source->format != PG_LOOSE;
	size_t remaining;
	ssize_t count;
	int native_code = 0;

	if (bytes)
		*bytes = 0;
	if (!reader || !bytes || (capacity && !buffer) || reader->failed)
		return pg_result(PG_INVALID, error);
	if (!capacity)
		return pg_result(PG_OK, error);
	/* Native transfers validate once after reporting delivered bytes. */
	if (archive || reader->position == reader->info.size) {
		status = pg_reader_native_check(reader, &native_code);
		if (status != PG_OK) {
			reader->failed = 1;
			return pg_native_result(status, native_code, error);
		}
	}
	if (reader->position == reader->info.size) {
		if (archive && !reader->verified) {
			status = pg_archive_verify_payload(reader->fd,
				(uint64_t)reader->identity.st_size,
				&reader->file_info, reader->payload_offset, 0);
			if (status != PG_OK) {
				reader->failed = 1;
				return pg_result(status, error);
			}
			reader->verified = 1;
		}
		return pg_result(PG_END, error);
	}
	if (archive && reader->decoder) {
		status = pg_archive_read_decoded(reader, buffer,
			capacity, bytes);
		if (status != PG_OK) {
			reader->failed = 1;
			return pg_result(status, error);
		}
		pg_checkpoint_save(reader);
	} else {
		remaining = (size_t)((reader->info.size - reader->position) <
			capacity ? reader->info.size - reader->position :
			capacity);
		if (archive && remaining > 65536)
			remaining = 65536;
#ifdef SSIZE_MAX
		if (remaining > (size_t)SSIZE_MAX)
			remaining = (size_t)SSIZE_MAX;
#endif
		do {
			count = pread(reader->fd, buffer, remaining,
				(off_t)(reader->payload_offset +
					reader->position));
		} while (count < 0 && errno == EINTR);
		if (count < 0) {
			native_code = errno;
			reader->failed = 1;
			return pg_native_result(PG_IO, native_code, error);
		}
		if (!count) {
			reader->failed = 1;
			return pg_result(PG_STALE, error);
		}
		*bytes = (size_t)count;
		reader->position += (uint64_t)count;
	}
	if (!archive || reader->position == reader->info.size) {
		status = pg_reader_native_check(reader, &native_code);
		if (status != PG_OK) {
			reader->failed = 1;
			return pg_native_result(status, native_code, error);
		}
	}
	if (archive && reader->position == reader->info.size) {
		status = pg_archive_verify_payload(reader->fd,
			(uint64_t)reader->identity.st_size,
			&reader->file_info, reader->payload_offset, 0);
		if (status != PG_OK) {
			reader->failed = 1;
			return pg_result(status, error);
		}
		reader->verified = 1;
	}
	return pg_result(PG_OK, error);
}

static pg_status pg_reader_seek_locked(pg_reader *reader,
		uint64_t offset, pg_error *error)
{
	/* Validate offset/state without changing position. */
	if (!reader || reader->failed || offset > reader->info.size)
		return pg_result(PG_INVALID, error);
	/* Revalidate the selected native identity. */
	int native_code = 0;
	pg_status status = pg_reader_native_check(reader, &native_code);

	if (status != PG_OK) {
		reader->failed = 1;
		return pg_native_result(status, native_code, error);
	}
	/* Seek directly or restore/replay a decoder checkpoint. */
	if (!reader->decoder) {
		reader->position = offset;
		return pg_result(PG_OK, error);
	}
	pg_archive_decoder *decoder = (pg_archive_decoder *)reader->decoder;

	decoder->indexed = 1;
	if (offset == reader->info.size) {
		reader->position = offset;
		return pg_result(PG_OK, error);
	}
	status = pg_decoder_reposition(reader, offset);
	while (status == PG_OK && reader->position < offset) {
		uint8_t scratch[65536];
		uint64_t remaining = offset - reader->position;
		size_t amount = remaining < sizeof(scratch) ?
			(size_t)remaining : sizeof(scratch);
		size_t count = 0;

		status = pg_archive_read_decoded(reader, scratch, amount,
			&count);
		if (status == PG_OK)
			pg_checkpoint_save(reader);
	}
	if (status == PG_OK)
		status = pg_reader_native_check(reader, &native_code);
	if (status != PG_OK)
		reader->failed = 1;
	return pg_native_result(status, native_code, error);
}

PG_API pg_status PG_CALL pg_reader_tell(pg_reader *reader,
		uint64_t *out, pg_error *error)
{
	/* Initialize output and expose the serialized reader position. */
	if (out)
		*out = 0;
	if (!reader || !out)
		return pg_result(PG_INVALID, error);
	*out = reader->position;
	return pg_result(PG_OK, error);
}

/* Accept a null owned handle as a no-op. */
/* Finish reader cleanup and remove its exact-name watch scope. */
/* Release references and clear the pointer once close is accepted. */
static pg_status PG_CALL pg_reader_close_coordinated(
		pg_reader **reader,
		pg_error *error)
{
	if (!reader)
		return pg_result(PG_INVALID, error);
	if (!*reader)
		return pg_result(PG_OK, error);
	pg_reader *owned = *reader;
	*reader = NULL;
	if (owned->decoder) {
		pg_archive_decoder *decoder =
			(pg_archive_decoder *)owned->decoder;

		zng_inflateEnd(&decoder->stream);
		while (decoder->checkpoints) {
			pg_archive_checkpoint *checkpoint =
				decoder->checkpoints;

			decoder->checkpoints = checkpoint->next;
			pg_checkpoint_dispose(checkpoint);
		}
		free(decoder);
	}
	int closed = close(owned->fd);
	int code = closed ? errno : 0;

	if (owned->source) {
		int release_code = 0;

		pg_atomic_decrement(&owned->source->live_readers);
		pg_status release_status = pg_source_release(owned->source,
			&release_code);

		if (!closed && release_status != PG_OK) {
			closed = -1;
			code = release_code;
		}
	} else {
		pg_context_child_drop(owned->context);
	}
	if (owned->watch_tree) {
		pg_tree_reader_scope_remove(owned->watch_tree,
			owned->watch_scope);
		pg_tree_release(owned->watch_tree);
	}
	free(owned->native_path);
	free(owned);
	return pg_native_result(closed ? PG_IO : PG_OK, code, error);
}

PG_API pg_status PG_CALL pg_reader_close(
		pg_reader **reader,
		pg_error *error)
{
	pg_context *context = reader && *reader ? (*reader)->context : NULL;

	pg_context_lock(context);
	pg_status status = pg_reader_close_coordinated(reader, error);

	pg_context_unlock(context);
	return status;
}

static pg_status pg_writer_open_archive_source(pg_source *source,
		const char *name, const pg_write_options *options,
		pg_writer **out, pg_error *error, pg_source_record *selection)
{
	pg_archive_builder *builder;
	pg_writer *writer;
	pg_source_record *record = selection;
	pg_write_options effective;
	char *canonical = NULL;
	pg_status status;

	if (source->recovery_required)
		return pg_result(PG_RECOVERY_REQUIRED, error);
	if (source->stale)
		return pg_result(PG_STALE, error);
	if (source->live_writers)
		return pg_result(PG_BUSY, error);
	status = pg_normalize_owned(source->context, name, &canonical);
	if (status != PG_OK)
		return pg_result(status, error);
	if (!selection) {
		status = pg_source_writer_name_check(source, canonical);
		if (status != PG_OK) {
			free(canonical);
			return pg_result(status, error);
		}
	}
	effective = *options;
	if (!selection) {
		for (pg_source_record *item = source->records; item;
		     item = item->next) {
			if (!strcmp(item->info.canonical_name, canonical))
				record = item;
		}
	}
	if (record && options->entry.original_name) {
		free(canonical);
		return pg_result(PG_INVALID, error);
	}
	if (record)
		effective.entry.original_name = record->info.original_name;
	free(canonical);
	builder = (pg_archive_builder *)calloc(1, sizeof(*builder));
	if (!builder)
		return pg_result(PG_NOMEM, error);
	builder->context = source->context;
	builder->format = source->format;
	builder->checksum_domain = source->checksum_domain;
	builder->staging = tmpfile();
	if (!builder->staging) {
		int code = errno;

		free(builder);
		return pg_native_result(PG_IO, code, error);
	}
	status = pg_writer_open_archive_builder(builder, name, &effective,
		&writer, error);
	if (status != PG_OK) {
		fclose(builder->staging);
		free(builder);
		return status;
	}
	status = pg_source_retain(source);
	if (status != PG_OK) {
		pg_writer_close(&writer, NULL);
		fclose(builder->staging);
		free(builder);
		return pg_result(status, error);
	}
	writer->kind = PG_WRITER_SOURCE_ARCHIVE;
	writer->source = source;
	writer->context = source->context;
	writer->archive_identity = source->identity;
	writer->named_target = selection == NULL;
	writer->target_record = UINT32_MAX;
	if (record) {
		writer->target_copy_id = record->info.copy_id;
		writer->target_copy_generation = record->info.copy_generation;
		writer->target_record = record->archive->archive_record;
	}
	source->live_writers++;
	*out = writer;
	return pg_result(PG_OK, error);
}

/* Open a staged writer for one source path. */
static pg_status PG_CALL pg_writer_open_source_coordinated(
		pg_source *destination,
		const char *name,
		const pg_write_options *options,
		pg_writer **out,
		pg_error *error)
{
	pg_file *existing = NULL;
	pg_error find_error;
	pg_status status;
	char *canonical = NULL;
	char *native = NULL;
	uint32_t flags = 0;

	if (!out)
		return pg_result(PG_INVALID, error);
	*out = NULL;
	if (!destination || !name || !options)
		return pg_result(PG_INVALID, error);
	if (destination->access != PG_WRITE)
		return pg_result(PG_READ_ONLY, error);
	status = pg_source_control_status(destination, 1);
	if (status != PG_OK)
		return pg_result(status, error);
	if (destination->format != PG_LOOSE)
		return pg_writer_open_archive_source(destination, name,
			options, out, error, NULL);
	status = pg_normalize_owned(destination->context, name,
		&canonical);
	if (status != PG_OK)
		return pg_result(status, error);
	for (char *at = canonical; *at; at++) {
		if (*at != '/')
			continue;
		*at = '\0';
		status = pg_source_find_fresh(destination, canonical,
			&existing, &find_error);
		*at = '/';
		pg_file_close(&existing, NULL);
		if (status == PG_NOT_FOUND || status == PG_CONFLICT)
			continue;
		free(canonical);
		if (status == PG_OK)
			return pg_result(PG_CONFLICT, error);
		if (error)
			*error = find_error;
		return status;
	}
	status = pg_source_find_fresh(destination, canonical, &existing,
		&find_error);
	if (status == PG_OK) {
		status = pg_file_prepare_native(existing);
		if (status == PG_OK) {
			native = strdup(existing->native_path);
			if (!native) status = PG_NOMEM;
		}
		flags = PG_OVERWRITE;
		pg_file_close(&existing, NULL);
	} else if (status == PG_NOT_FOUND) {
		size_t prefix = strlen(destination->native_path);
		size_t length = strlen(canonical);

		if (length > SIZE_MAX - prefix - 2) {
			status = PG_LIMIT;
		} else {
			native = (char *)malloc(prefix + length + 2);
			if (!native)
				status = PG_NOMEM;
			else {
				memcpy(native, destination->native_path,
					prefix);
				native[prefix] = '/';
				memcpy(native + prefix + 1, canonical,
					length + 1);
				status = PG_OK;
			}
		}
	}
	free(canonical);
	if (status != PG_OK) {
		free(native);
		if (status == find_error.status &&
		    status != PG_NOT_FOUND && error)
			*error = find_error;
		else
			pg_result(status, error);
		return status;
	}
	status = pg_writer_open_native_for_source(destination->context,
		destination, native, options, flags, out, error);
	free(native);
	return status;
}

PG_API pg_status PG_CALL pg_writer_open_source(
		pg_source *destination,
		const char *name,
		const pg_write_options *options,
		pg_writer **out,
		pg_error *error)
{
	pg_context *context = destination ? destination->context : NULL;

	pg_context_lock(context);
	pg_source_lock(destination);
	pg_status status = pg_writer_open_source_coordinated(destination,
		name, options, out, error);

	pg_source_unlock(destination);
	pg_context_unlock(context);
	return status;
}

/* Stage a replacement for one captured loose-file identity. */
static pg_status PG_CALL pg_writer_open_file_coordinated(
		pg_file *file,
		const pg_write_options *options,
		pg_writer **out,
		pg_error *error)
{
	pg_status status;

	if (!out)
		return pg_result(PG_INVALID, error);
	*out = NULL;
	if (!file || !options || options->entry.original_name)
		return pg_result(PG_INVALID, error);
	if (file->source->recovery_required)
		return pg_result(PG_RECOVERY_REQUIRED, error);
	if (file->source->access != PG_WRITE)
		return pg_result(PG_READ_ONLY, error);
	status = pg_source_control_status(file->source, 1);
	if (status != PG_OK)
		return pg_result(status, error);
	if (file->source->format != PG_LOOSE) {
		pg_source_record *record;

		for (record = file->source->records; record;
		     record = record->next) {
			if (record->info.copy_id == file->info.copy_id &&
			    record->info.copy_generation ==
				file->info.copy_generation)
				break;
		}
		if (!record || (file->source->format != PG_HOGG10 &&
		    !pg_native_stat_same(&file->source_identity,
			&file->source->identity)))
			return pg_result(PG_STALE, error);
		return pg_writer_open_archive_source(file->source,
			file->info.canonical_name, options, out, error, record);
	}
	status = pg_file_prepare_native(file);
	if (status != PG_OK) return pg_result(status, error);
	status = pg_writer_open_native_for_source(file->source->context,
		file->source, file->native_path, options, PG_OVERWRITE,
		out, error);
	if (status != PG_OK)
		return status;
	if (!(*out)->target_exists ||
	    !pg_native_stat_same(&file->source_identity, &(*out)->target)) {
		pg_writer_close(out, NULL);
		return pg_result(PG_STALE, error);
	}
	return pg_result(PG_OK, error);
}

PG_API pg_status PG_CALL pg_writer_open_file(
		pg_file *file,
		const pg_write_options *options,
		pg_writer **out,
		pg_error *error)
{
	pg_context *context = file ? file->source->context : NULL;

	pg_context_lock(context);
	pg_source_lock(file ? file->source : NULL);
	pg_status status = pg_writer_open_file_coordinated(file, options, out,
		error);

	pg_source_unlock(file ? file->source : NULL);
	pg_context_unlock(context);
	return status;
}

/* Validate builder state, unique name and known input length. */
/* Copy entry metadata and create private entry staging. */
/* Retain builder and mark its live writer before publishing handle. */
PG_API pg_status PG_CALL pg_writer_open_archive_builder(
		pg_archive_builder *builder,
		const char *name,
		const pg_write_options *options,
		pg_writer **out,
		pg_error *error)
{
	pg_writer *writer;
	pg_builder_entry *entry;
	pg_status status;
	char *canonical = NULL;
	char *original = NULL;

	if (!out)
		return pg_result(PG_INVALID, error);
	*out = NULL;
	if (!builder || !name || !options || builder->finished)
		return pg_result(PG_INVALID, error);
	if (builder->live_writers)
		return pg_result(PG_BUSY, error);
	status = pg_builder_options_check(builder, options);
	if (status != PG_OK)
		return pg_result(status, error);
	status = pg_normalize_owned(builder->context, name, &canonical);
	if (status != PG_OK)
		return pg_result(status, error);
	for (entry = builder->entries; entry; entry = entry->next) {
		if (strcmp(entry->canonical_name, canonical) == 0) {
			free(canonical);
			return pg_result(PG_EXISTS, error);
		}
	}
	if (options->entry.original_name) {
		status = pg_normalize_owned(builder->context,
			options->entry.original_name, &original);
		if (status != PG_OK || strcmp(original, canonical) != 0) {
			free(canonical);
			free(original);
			return pg_result(status == PG_OK ? PG_INVALID : status,
				error);
		}
		free(original);
		original = pg_display_owned(options->entry.original_name);
		if (!original) {
			free(canonical);
			return pg_result(PG_NOMEM, error);
		}
	}
	if (!original) {
		original = pg_display_owned(name);
		if (!original) {
			free(canonical);
			return pg_result(PG_NOMEM, error);
		}
	}
	if (builder->format == PG_HOGG10 &&
	    strcmp(canonical, "?datalist") == 0) {
		free(canonical);
		free(original);
		return pg_result(PG_CONFLICT, error);
	}
	writer = (pg_writer *)calloc(1, sizeof(*writer));
	if (!writer) {
		free(canonical);
		free(original);
		return pg_result(PG_NOMEM, error);
	}
	writer->canonical_name = canonical;
	writer->original_name = original;
	writer->options = *options;
	writer->options.entry.original_name = writer->original_name;
	if (options->entry.cached_header_size) {
		writer->cached_header = malloc(options->entry.cached_header_size);
		if (!writer->cached_header) {
			pg_writer_release_memory(writer);
			return pg_result(PG_NOMEM, error);
		}
		memcpy(writer->cached_header, options->entry.cached_header,
			options->entry.cached_header_size);
	}
	writer->options.entry.cached_header = writer->cached_header;
	writer->input = tmpfile();
	if (!writer->input) {
		int code = errno;

		pg_writer_release_memory(writer);
		return pg_native_result(PG_IO, code, error);
	}
	writer->builder = builder;
	writer->kind = PG_WRITER_BUILDER;
	writer->state = PG_WRITER_ACTIVE;
	builder->live_writers++;
	*out = writer;
	return pg_result(PG_OK, error);
}

static pg_status pg_writer_native_stage(pg_writer *writer,
		int *native_code, pg_status *cause)
{
	int lease = -1;
	pg_status status = pg_native_target_lease(writer->native_path,
		writer->target_exists, &lease, native_code);

	if (status == PG_OK)
		status = pg_writer_native_stage_locked(writer, native_code,
			cause);
	if (lease >= 0)
		close(lease);
	return status;
}

/* Validate options and aliases, then retain a pinned native parent. */
static pg_status pg_writer_open_native_for_source_coordinated(
		pg_context *context,
		pg_source *allowed_source,
		const char *native_path,
		const pg_write_options *options,
		uint32_t flags,
		pg_writer **out,
		pg_error *error)
{
	pg_writer *writer;
	pg_status status = PG_OK;
	struct stat target;
	int native_code = 0;
	int exists = 0;
	size_t i;

	if (!out)
		return pg_result(PG_INVALID, error);
	*out = NULL;
	if (!context || !native_path || !*native_path || !options ||
	    (allowed_source && allowed_source->context != context) ||
	    (flags & ~PG_OVERWRITE) ||
	    (options->encoding != PG_LOGICAL &&
	     options->encoding != PG_ZLIB) ||
	    (options->encoding == PG_LOGICAL &&
	     options->input_size != options->logical_size) ||
	    (options->encoding == PG_ZLIB && !options->input_size) ||
	    options->entry.compression > PG_COMPRESS_FORCE ||
	    options->entry.digest_kind > PG_DIGEST_MD5_32 ||
	    options->entry.expected_digest_domain > PG_CHECKSUM_STORED)
		return pg_result(PG_INVALID, error);
	if (options->entry.compression == PG_COMPRESS_FORCE ||
	    options->entry.cached_header_size ||
	    options->entry.original_name)
		return pg_result(PG_UNSUPPORTED, error);
	if ((int64_t)(time_t)options->entry.mtime !=
	    options->entry.mtime)
		return pg_result(PG_LIMIT, error);
	for (i = options->entry.digest_kind == PG_DIGEST_MD5 ? 16 :
	     options->entry.digest_kind == PG_DIGEST_MD5_32 ? 4 : 0;
	     i < sizeof(options->entry.expected_digest); i++) {
		if (options->entry.expected_digest[i])
			return pg_result(PG_INVALID, error);
	}
	if (pg_context_children(context) == SIZE_MAX)
		return pg_result(PG_LIMIT, error);
	if (allowed_source && allowed_source->live_writers)
		return pg_result(PG_BUSY, error);
	writer = (pg_writer *)calloc(1, sizeof(*writer));
	if (!writer)
		return pg_result(PG_NOMEM, error);
	writer->parent_fd = -1;
	writer->native_path = pg_native_absolute(native_path, &status,
		&native_code);
	if (!writer->native_path)
		goto cleanup_native;
	status = pg_native_parent_open(writer->native_path, 1,
		&writer->parent_fd, &writer->leaf, &native_code);
	if (status != PG_OK)
		goto cleanup_native;
	if (!fstatat(writer->parent_fd, writer->leaf, &target,
		AT_SYMLINK_NOFOLLOW)) {
		if (!S_ISREG(target.st_mode)) {
			status = PG_CONFLICT;
			goto cleanup_native;
		}
		if (!(flags & PG_OVERWRITE)) {
			status = PG_EXISTS;
			goto cleanup_native;
		}
		exists = 1;
	} else if (errno != ENOENT) {
		native_code = errno;
		status = PG_IO;
		goto cleanup_native;
	}
	status = pg_native_alias(context, writer->parent_fd,
		&target, exists, allowed_source, &native_code);
	if (status != PG_OK)
		goto cleanup_native;
	writer->input = tmpfile();
	if (!writer->input) {
		native_code = errno;
		status = PG_IO;
		goto cleanup_native;
	}
	if (allowed_source) {
		status = pg_source_retain(allowed_source);
		if (status != PG_OK) {
			fclose(writer->input);
			writer->input = NULL;
			goto cleanup_native;
		}
		allowed_source->live_writers++;
	}
	writer->context = context;
	writer->source = allowed_source;
	writer->options = *options;
	writer->flags = flags;
	writer->target_exists = exists;
	if (exists)
		writer->target = target;
	writer->kind = PG_WRITER_NATIVE;
	writer->state = PG_WRITER_ACTIVE;
	pg_context_child_add(context);
	*out = writer;
	return pg_result(PG_OK, error);

cleanup_native:
	if (writer->parent_fd >= 0)
		close(writer->parent_fd);
	free(writer->leaf);
	free(writer->native_path);
	pg_writer_release_memory(writer);
	return pg_native_result(status, native_code, error);
}

pg_status pg_writer_open_native_for_source(pg_context *context,
		pg_source *allowed_source,
		const char *native_path,
		const pg_write_options *options,
		uint32_t flags,
		pg_writer **out,
		pg_error *error)
{
	pg_context_lock(context);
	pg_status status = pg_writer_open_native_for_source_coordinated(
		context, allowed_source, native_path, options, flags, out,
		error);

	pg_context_unlock(context);
	return status;
}

PG_API pg_status PG_CALL pg_writer_open_native(
		pg_context *context, const char *native_path,
		const pg_write_options *options, uint32_t flags,
		pg_writer **out, pg_error *error)
{
	if (!context) {
		if (out)
			*out = NULL;
		return pg_result(PG_INVALID, error);
	}
	return pg_writer_open_native_for_source(context, NULL, native_path,
		options, flags, out, error);
}

/* Pin an existing directory and preflight every captured output. */
static pg_status PG_CALL pg_unpack_target_open_coordinated(
		pg_cursor *cursor, const char *directory, uint32_t flags,
		pg_unpack_target **out, pg_error *error)
{
	pg_unpack_target *target;
	pg_status status = PG_OK;
	struct stat path_state;
	int native_code = 0;
	size_t i;

	if (!out)
		return pg_result(PG_INVALID, error);
	*out = NULL;
	if (!cursor || !cursor->context || !directory || !*directory ||
	    (flags & ~PG_OVERWRITE) || cursor->position)
		return pg_result(PG_INVALID, error);
	if (pg_context_children(cursor->context) == SIZE_MAX)
		return pg_result(PG_LIMIT, error);
	if (lstat(directory, &path_state)) {
		native_code = errno;
		return pg_native_result(errno == ENOENT ? PG_NOT_FOUND :
			PG_IO, native_code, error);
	}
	if (!S_ISDIR(path_state.st_mode))
		return pg_result(PG_CONFLICT, error);
	target = (pg_unpack_target *)calloc(1, sizeof(*target));
	if (!target)
		return pg_result(PG_NOMEM, error);
	target->root_fd = -1;
	target->root_path = pg_native_absolute(directory, &status,
		&native_code);
	if (!target->root_path)
		goto target_fail;
	target->root_fd = open(directory,
		O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (target->root_fd < 0) {
		native_code = errno;
		status = PG_IO;
		goto target_fail;
	}
	if (fstat(target->root_fd, &target->root_identity) ||
	    !pg_native_stat_same(&path_state,
		&target->root_identity)) {
		status = PG_RETRY;
		goto target_fail;
	}
	target->items = cursor->count ? (pg_unpack_item *)calloc(
		cursor->count, sizeof(*target->items)) : NULL;
	if (cursor->count && !target->items) {
		status = PG_NOMEM;
		goto target_fail;
	}
	target->context = cursor->context;
	target->flags = flags;
	for (i = 0; i < cursor->count; i++) {
		pg_file *file = cursor->files[i];
		pg_write_options options;
		pg_writer *writer = NULL;
		char *path = NULL;

		status = pg_unpack_output_path(target->root_path,
			file->info.canonical_name, &path);
		if (status != PG_OK)
			goto target_fail;
		pg_write_options_init(&options, file->info.logical_size);
		options.entry.mtime = file->info.mtime;
		status = pg_writer_open_native(target->context, path,
			&options, flags, &writer, error);
		free(path);
		if (status != PG_OK)
			goto target_fail;
		if (fstat(writer->parent_fd,
			&target->items[i].parent_identity)) {
			native_code = errno;
			pg_writer_close(&writer, NULL);
			status = PG_IO;
			goto target_fail;
		}
		target->items[i].target_exists = writer->target_exists;
		if (writer->target_exists)
			target->items[i].target_identity = writer->target;
		status = pg_writer_close(&writer, error);
		if (status != PG_OK)
			goto target_fail;
		target->items[i].name = strdup(
			file->info.canonical_name);
		if (!target->items[i].name) {
			status = PG_NOMEM;
			goto target_fail;
		}
		status = pg_source_retain(file->source);
		if (status != PG_OK)
			goto target_fail;
		target->items[i].source = file->source;
		target->items[i].selection = file;
		target->items[i].copy_id = file->info.copy_id;
		target->items[i].copy_generation =
			file->info.copy_generation;
		target->count++;
	}
	pg_context_child_add(target->context);
	*out = target;
	return pg_result(PG_OK, error);

target_fail:
	for (i = 0; i < target->count; i++) {
		pg_source_release(target->items[i].source, &native_code);
		free(target->items[i].name);
	}
	if (target->items && target->count < cursor->count)
		free(target->items[target->count].name);
	free(target->items);
	if (target->root_fd >= 0)
		close(target->root_fd);
	free(target->root_path);
	free(target);
	return pg_native_result(status, native_code, error);
}

PG_API pg_status PG_CALL pg_unpack_target_open(
		pg_cursor *cursor, const char *directory, uint32_t flags,
		pg_unpack_target **out, pg_error *error)
{
	pg_context *context = cursor ? cursor->context : NULL;

	pg_context_lock(context);
	pg_status status = pg_unpack_target_open_coordinated(cursor,
		directory, flags, out, error);

	pg_context_unlock(context);
	return status;
}

/* Open a staged native writer for one captured unpack selection. */
PG_API pg_status PG_CALL pg_writer_open_unpack(
		pg_unpack_target *target, pg_file *file,
		pg_writer **out, pg_error *error)
{
	pg_write_options options;
	pg_status status;
	char *path = NULL;
	int native_code = 0;
	size_t i;

	if (!out)
		return pg_result(PG_INVALID, error);
	*out = NULL;
	if (!target || !file)
		return pg_result(PG_INVALID, error);
	if (target->live_writers == SIZE_MAX)
		return pg_result(PG_LIMIT, error);
	for (i = 0; i < target->count; i++) {
		if (target->items[i].selection == file &&
		    target->items[i].source == file->source &&
		    target->items[i].copy_id == file->info.copy_id &&
		    target->items[i].copy_generation ==
			file->info.copy_generation)
			break;
	}
	if (i == target->count)
		return pg_result(PG_INVALID, error);
	status = pg_unpack_root_check(target, &native_code);
	if (status != PG_OK)
		return pg_native_result(status, native_code, error);
	status = pg_unpack_output_path(target->root_path,
		target->items[i].name, &path);
	if (status != PG_OK)
		return pg_result(status, error);
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
	size_t digest_size = options.entry.digest_kind ==
		PG_DIGEST_MD5 ? 16 : 4;

	if (memcmp(options.entry.expected_digest, zero,
		digest_size) == 0)
		options.entry.digest_kind = PG_DIGEST_NONE;
	status = pg_writer_open_native(target->context, path,
		&options, target->flags, out, error);
	free(path);
	if (status != PG_OK)
		return status;
	struct stat parent_state;

	if (fstat((*out)->parent_fd, &parent_state)) {
		native_code = errno;
		pg_writer_close(out, NULL);
		return pg_native_result(PG_IO, native_code, error);
	}
	if (parent_state.st_dev !=
	    target->items[i].parent_identity.st_dev ||
	    parent_state.st_ino !=
	    target->items[i].parent_identity.st_ino ||
	    (*out)->target_exists != target->items[i].target_exists ||
	    ((*out)->target_exists &&
	     !pg_native_stat_same(&(*out)->target,
		&target->items[i].target_identity))) {
		pg_writer_close(out, NULL);
		return pg_result(PG_STALE, error);
	}
	status = pg_unpack_root_check(target, &native_code);
	if (status != PG_OK) {
		pg_writer_close(out, NULL);
		return pg_native_result(status, native_code, error);
	}
	(*out)->unpack_target = target;
	target->live_writers++;
	return pg_result(PG_OK, error);
}

/* Close a pinned target once its writers have been released. */
PG_API pg_status PG_CALL pg_unpack_target_close(
		pg_unpack_target **target, pg_error *error)
{
	pg_unpack_target *owned;
	pg_status status = PG_OK;
	int native_code = 0;
	size_t i;

	if (!target)
		return pg_result(PG_INVALID, error);
	if (!*target)
		return pg_result(PG_OK, error);
	owned = *target;
	if (owned->live_writers)
		return pg_result(PG_BUSY, error);
	*target = NULL;
	for (i = 0; i < owned->count; i++) {
		int code = 0;
		pg_status released = pg_source_release(
			owned->items[i].source, &code);

		if (status == PG_OK && released != PG_OK) {
			status = released;
			native_code = code;
		}
		free(owned->items[i].name);
	}
	if (close(owned->root_fd) && status == PG_OK) {
		native_code = errno;
		status = PG_IO;
	}
	pg_context_child_drop(owned->context);
	free(owned->items);
	free(owned->root_path);
	free(owned);
	return pg_native_result(status, native_code, error);
}

/* Validate writer state, span and declared remaining length. */
/* Append or decode caller chunk into private staging. */
/* Report accepted bytes; make accepted failures close-only. */
PG_API pg_status PG_CALL pg_writer_write(
		pg_writer *writer,
		const void *buffer,
		size_t size,
		size_t *bytes,
		pg_error *error)
{
	size_t count;

	if (bytes)
		*bytes = 0;
	if (!writer || !bytes || (size && !buffer) ||
	    writer->state != PG_WRITER_ACTIVE)
		return pg_result(PG_INVALID, error);
	if ((uint64_t)size > writer->options.input_size - writer->accepted)
		return pg_result(PG_INVALID, error);
	if (!size)
		return pg_result(PG_OK, error);
	count = fwrite(buffer, 1, size, writer->input);
	*bytes = count;
	writer->accepted += count;
	if (count != size) {
		writer->state = PG_WRITER_CLOSE_ONLY;
		return pg_native_result(PG_IO, errno, error);
	}
	return pg_result(PG_OK, error);
}

/* Check exact lengths, codec, digest and staged output. */
/* Publish replacement or install private builder entry. */
/* Report publication effects and make writer close-only. */
static pg_status PG_CALL pg_writer_finish_coordinated(
		pg_writer *writer,
		pg_error *error)
{
	pg_status status;

	if (!writer || writer->state != PG_WRITER_ACTIVE)
		return pg_result(PG_INVALID, error);
	if (writer->source) {
		status = pg_source_control_status(writer->source, 1);
		if (status != PG_OK)
			return pg_result(status, error);
	}
	writer->state = PG_WRITER_CLOSE_ONLY;
	int native_code = 0;
	pg_status cause = PG_OK;

	if (writer->kind == PG_WRITER_NATIVE)
		status = pg_writer_native_stage(writer,
			&native_code, &cause);
	else {
		status = pg_writer_stage(writer);
		if (status == PG_OK &&
		    writer->kind == PG_WRITER_SOURCE_ARCHIVE)
			status = pg_source_archive_finish(writer, error);
	}
	if ((status == PG_OK || status == PG_COMMITTED) &&
	    writer->source && writer->source->format == PG_LOOSE) {
		pg_error refresh_error;
		pg_status refreshed = pg_source_rescan(writer->source,
			&refresh_error);

		if (refreshed != PG_OK && status == PG_OK) {
			status = PG_COMMITTED;
			cause = refreshed;
			if (error)
				*error = refresh_error;
		}
	}
	if ((status == PG_OK || status == PG_COMMITTED) &&
	    writer->source && writer->source->attached) {
		pg_error queue_error;
		pg_status queued = pg_tree_queue_changes(
			writer->source->attached, &queue_error);

		if (queued != PG_OK && status == PG_OK) {
			status = PG_COMMITTED;
			cause = queued;
			if (error)
				*error = queue_error;
		}
	}
	if (status == PG_OK)
		writer->state = PG_WRITER_FINISHED;
	if (writer->kind != PG_WRITER_SOURCE_ARCHIVE ||
	    status == PG_OK || !error || error->status != status)
		pg_native_result(status, native_code, error);
	if (status == PG_COMMITTED && error && cause != PG_OK)
		error->cause = cause;
	return status;
}

PG_API pg_status PG_CALL pg_writer_finish(
		pg_writer *writer,
		pg_error *error)
{
	pg_context *context = writer ? writer->context : NULL;

	pg_context_lock(context);
	pg_status status = pg_writer_finish_coordinated(writer, error);

	pg_context_unlock(context);
	return status;
}

/* Accept a null owned handle as a no-op. */
/* Discard unfinished staging without publishing content. */
/* Release parent and live builder-writer reference. */
/* Clear the owned pointer once close is accepted. */
static pg_status PG_CALL pg_writer_close_coordinated(
		pg_writer **writer,
		pg_error *error)
{
	pg_writer *owned;
	int closed;
	int code;

	if (!writer)
		return pg_result(PG_INVALID, error);
	if (!*writer)
		return pg_result(PG_OK, error);
	owned = *writer;
	*writer = NULL;
	closed = owned->input ? fclose(owned->input) : 0;
	code = closed ? errno : 0;
	if (owned->kind == PG_WRITER_NATIVE) {
		if (close(owned->parent_fd) && !closed) {
			closed = -1;
			code = errno;
		}
		pg_context_child_drop(owned->context);
		if (owned->unpack_target)
			owned->unpack_target->live_writers--;
		if (owned->source) {
			int release_code = 0;
			pg_status released;

			owned->source->live_writers--;
			released = pg_source_release(owned->source,
				&release_code);
			if (!closed && released != PG_OK) {
				closed = -1;
				code = release_code;
			}
		}
		free(owned->native_path);
		free(owned->leaf);
	} else if (owned->kind == PG_WRITER_SOURCE_ARCHIVE) {
		pg_builder_entry *entry = owned->builder->entries;
		int release_code = 0;
		pg_status released;

		while (entry) {
			pg_builder_entry *next = entry->next;

			free(entry->canonical_name);
			free(entry->original_name);
			free(entry->cached_header);
			free(entry);
			entry = next;
		}
		if (fclose(owned->builder->staging) && !closed) {
			closed = -1;
			code = errno;
		}
		free(owned->builder);
		owned->source->live_writers--;
		released = pg_source_release(owned->source, &release_code);
		if (!closed && released != PG_OK) {
			closed = -1;
			code = release_code;
		}
	} else {
		owned->builder->live_writers--;
	}
	pg_writer_release_memory(owned);
	return pg_native_result(closed ? PG_IO : PG_OK, code, error);
}

PG_API pg_status PG_CALL pg_writer_close(
		pg_writer **writer,
		pg_error *error)
{
	pg_context *context = writer && *writer ? (*writer)->context : NULL;

	pg_context_lock(context);
	pg_status status = pg_writer_close_coordinated(writer, error);

	pg_context_unlock(context);
	return status;
}

} /* extern "C" */

static pg_status pg_reader_open_coordinated(
		pg_file *file, uint32_t representation,
		pg_reader **out, pg_error *error)
{
	pg_source *source = file ? file->source : NULL;

	pg_source_lock(source);
	pg_status status = pg_reader_open_locked(file, representation, out,
		error);

	pg_source_unlock(source);
	/* Tree reconciliation can visit other sources; hold no source lock. */
	if (status == PG_OK) {
		status = pg_reader_track_tree(file, *out, error);
		if (status != PG_OK)
			pg_reader_close(out, NULL);
	}
	return status;
}

pg_status pg_reader_open(pg_file *file, uint32_t representation,
		pg_reader **out, pg_error *error)
{
	pg_context *context = file ? file->source->context : NULL;

	pg_context_lock(context);
	pg_status status = pg_reader_open_coordinated(file, representation,
		out, error);

	pg_context_unlock(context);
	return status;
}

pg_status pg_reader_read(pg_reader *reader, void *buffer, size_t capacity,
		size_t *bytes, pg_error *error)
{
	pg_source *source = reader ? reader->source : NULL;

	pg_source_lock(source);
	pg_status status = pg_reader_read_locked(reader, buffer, capacity,
		bytes, error);

	pg_source_unlock(source);
	return status;
}

pg_status pg_reader_seek(pg_reader *reader, uint64_t offset, pg_error *error)
{
	pg_source *source = reader ? reader->source : NULL;

	pg_source_lock(source);
	pg_status status = pg_reader_seek_locked(reader, offset, error);

	pg_source_unlock(source);
	return status;
}
