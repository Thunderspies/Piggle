#include <piggle/piggle.h>
#include "internal.hpp"
#include "archive_read.hpp"
#include "archive_wire.hpp"
#include "native_path.hpp"
#ifndef _WIN32
#include <dirent.h>
#endif

static char *pg_session_path(const char *directory, const char *leaf)
{
	size_t size = strlen(directory), suffix = strlen(leaf) + 2;

	if (size > SIZE_MAX - suffix)
		return NULL;
	char *path = (char *)malloc(size + suffix);

	if (path)
		snprintf(path, size + suffix, "%s/%s", directory, leaf);
	return path;
}

static int pg_session_remove_directory(const char *path)
{
	int parent = -1, native_code = 0;
	char *leaf = NULL;
	pg_status status =
		pg_native_parent_open(path, 0, &parent, &leaf, &native_code);

	if (status != PG_OK)
		return -1;
	int removed = unlinkat(parent, leaf, AT_REMOVEDIR);
	int code = errno;

	close(parent);
	free(leaf);
	errno = code;
	return removed;
}

static pg_status pg_session_recheck(int fd, const char *path)
{
	struct stat pinned, named;

	if (fstat(fd, &pinned) || lstat(path, &named))
		return errno == ENOENT ? PG_STALE : PG_IO;
	return S_ISDIR(named.st_mode) && pinned.st_dev == named.st_dev &&
			pinned.st_ino == named.st_ino
		? PG_OK
		: PG_STALE;
}

static uint64_t pg_session_nsec(const struct stat *identity)
{
#ifdef __linux__
	return identity->st_mtim.tv_nsec;
#elif defined(__APPLE__)
	return identity->st_mtimespec.tv_nsec;
#elif defined(_WIN32)
	return identity->st_mtime_nsec;
#else
	(void)identity;
	return 0;
#endif
}

static void pg_session_stat_save(uint8_t *out, const struct stat *identity)
{
	pg_wire_u64(out, (uint64_t)identity->st_dev);
	pg_wire_u64(out + 8, (uint64_t)identity->st_ino);
	pg_wire_u64(out + 16, (uint64_t)identity->st_size);
	pg_wire_u64(out + 24, (uint64_t)identity->st_mtime);
	pg_wire_u64(out + 32, pg_session_nsec(identity));
}

static void pg_session_stat_restore(struct stat *out, const uint8_t *in)
{
	memset(out, 0, sizeof(*out));
	out->st_mode = S_IFREG;
	out->st_dev = (decltype(out->st_dev))pg_read_u64(in);
	out->st_ino = (decltype(out->st_ino))pg_read_u64(in + 8);
	out->st_size = (decltype(out->st_size))pg_read_u64(in + 16);
	out->st_mtime = (decltype(out->st_mtime))pg_read_u64(in + 24);
#ifdef __linux__
	out->st_mtim.tv_nsec = (long)pg_read_u64(in + 32);
#elif defined(__APPLE__)
	out->st_mtimespec.tv_nsec = (long)pg_read_u64(in + 32);
#elif defined(_WIN32)
	out->st_mtime_nsec = (long)pg_read_u64(in + 32);
#endif
}

static pg_status pg_session_write(
	int fd, uint64_t at, const void *bytes, size_t size)
{
	const uint8_t *data = (const uint8_t *)bytes;

	if (at > INT64_MAX || size > INT64_MAX - at)
		return PG_LIMIT;
	while (size) {
		ssize_t done = pwrite(fd, data, size, (off_t)at);

		if (done < 0 && errno == EINTR)
			continue;
		if (done <= 0)
			return PG_IO;
		size -= (size_t)done;
		data += done;
		at += (uint64_t)done;
	}
	return PG_OK;
}

/* Select only a completely flushed image; retain the prior selection until
 * rename succeeds. The lease and pinned directory serialize selection.
 */
static pg_status pg_session_select(pg_archive_builder *builder,
	const char *leaf, const void *bytes, size_t size, pg_error *error)
{
	pg_status checked =
		pg_session_recheck(builder->session_fd, builder->session_path);

	if (checked != PG_OK)
		return pg_native_result(checked, errno, error);
	char *temporary = NULL;
	FILE *out = pg_staging_open(builder->session_path, &temporary);

	if (!out)
		return pg_native_result(PG_IO, errno, error);
	pg_status status = pg_wire_write(out, bytes, size);
	int native_code = 0;
	struct stat created, pinned;
	const char *temporary_leaf = pg_native_basename(temporary);

	if (status == PG_OK && (fflush(out) || fsync(fileno(out))))
		status = PG_IO;
	if (status == PG_OK &&
		(fstat(fileno(out), &created) ||
			fstatat(builder->session_fd, temporary_leaf, &pinned,
				AT_SYMLINK_NOFOLLOW)))
		status = PG_IO;
	if (status == PG_OK && !pg_native_stat_same(&created, &pinned))
		status = PG_STALE;
	if (status != PG_OK)
		native_code = errno;
	if (fclose(out) && status == PG_OK) {
		status = PG_IO;
		native_code = errno;
	}
	if (status == PG_OK &&
		renameat(builder->session_fd, temporary_leaf,
			builder->session_fd, leaf)) {
		status = PG_IO;
		native_code = errno;
	}
	if (status == PG_OK && fsync(builder->session_fd)) {
		status = PG_COMMITTED;
		native_code = errno;
	}
	if (status != PG_OK && status != PG_COMMITTED)
		unlink(temporary);
	free(temporary);
	pg_native_result(status, native_code, error);
	if (status == PG_COMMITTED && error)
		error->cause = PG_IO;
	return status;
}

static pg_status pg_session_read(
	const char *directory, const char *leaf, uint8_t **out, size_t *size)
{
	*out = NULL;
	*size = 0;
	char *path = pg_session_path(directory, leaf);

	if (!path)
		return PG_NOMEM;
	int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	free(path);
	if (fd < 0)
		return errno == ENOENT ? PG_NOT_FOUND : PG_IO;
	struct stat identity;
	pg_status status = PG_OK;

	if (fstat(fd, &identity))
		status = PG_IO;
	else if (!S_ISREG(identity.st_mode) || identity.st_size < 0)
		status = PG_CORRUPT;
	else if ((uint64_t)identity.st_size > SIZE_MAX)
		status = PG_LIMIT;
	if (status == PG_OK) {
		*size = (size_t)identity.st_size;
		*out = (uint8_t *)malloc(*size ? *size : 1);
		status = *out ? pg_read_span(fd, *size, 0, *out, *size)
			: PG_NOMEM;
	}
	if (close(fd) && status == PG_OK)
		status = PG_IO;
	if (status != PG_OK) {
		free(*out);
		*out = NULL;
		*size = 0;
	}
	return status;
}

static pg_status pg_session_payload_hash(int fd, uint64_t physical,
	uint64_t offset, uint64_t size, uint8_t digest[16])
{
	pg_md5 hash;
	uint8_t bytes[65536];

	pg_md5_init(&hash);
	while (size) {
		size_t amount =
			size < sizeof(bytes) ? (size_t)size : sizeof(bytes);
		pg_status status =
			pg_read_span(fd, physical, offset, bytes, amount);

		if (status != PG_OK)
			return status;
		pg_md5_update(&hash, bytes, amount);
		offset += amount;
		size -= amount;
	}
	pg_md5_finish(&hash, digest);
	return PG_OK;
}

pg_status pg_archive_builder_checkpoint(
	pg_archive_builder *builder, pg_error *error)
{
	if (!builder || !builder->session_path || builder->finished)
		return pg_result(PG_INVALID, error);
	if (builder->visiting)
		return pg_result(PG_REENTRANT, error);
	if (builder->live_writers)
		return pg_result(PG_BUSY, error);
	FILE *image = pg_staging_open(builder->staging_directory, NULL);

	if (!image)
		return pg_native_result(PG_IO, errno, error);
	pg_status status = pg_hogg_write(builder, image);
	uint8_t *bytes = NULL;
	uint8_t header[192] = {};
	struct stat stage = {};
	uint64_t image_size = 0, dl_size = 0, dl_offset = 0;
	size_t path_size = strlen(builder->native_path) + 1;
	size_t stage_size = strlen(builder->staging_path) + 1;
	size_t scratch_size = strlen(builder->staging_directory) + 1;
	uint64_t total = 0;

	if (status == PG_OK &&
		(fflush(image) || fflush(builder->staging) ||
			fsync(fileno(builder->staging)) ||
			fstat(fileno(builder->staging), &stage)))
		status = PG_IO;
	if (status == PG_OK) {
		off_t end = ftello(image);
		uint8_t record[32];

		if (end < 0)
			status = PG_IO;
		else {
			image_size = (uint64_t)end;
			status = pg_read_span(fileno(image), image_size,
				4144 + 32 * builder->entry_count, record, 32);
			if (status == PG_OK) {
				dl_size = pg_read_u32(record + 8);
				dl_offset = pg_read_u64(record);
			}
		}
	}
	if (status == PG_OK) {
		total = sizeof(header) + (uint64_t)path_size + stage_size +
			scratch_size + image_size + dl_size +
			builder->entry_count * 16;
		if (path_size > UINT32_MAX || stage_size > UINT32_MAX ||
			scratch_size > UINT32_MAX || total > SIZE_MAX)
			status = PG_LIMIT;
		else {
			bytes = (uint8_t *)malloc((size_t)total);
			if (!bytes)
				status = PG_NOMEM;
		}
	}
	if (status == PG_OK) {
		memcpy(header, "PGSESS01", 8);
		pg_wire_u32(header + 8, 1);
		pg_wire_u32(header + 12, builder->checksum_domain);
		pg_wire_u32(header + 16, builder->flags);
		pg_wire_u32(header + 20, builder->target_exists);
		pg_wire_u64(header + 24, builder->hogg_user_capacity);
		pg_wire_u64(header + 32, builder->entry_count);
		pg_wire_u64(header + 40, image_size);
		pg_wire_u64(header + 48, dl_size);
		pg_wire_u64(header + 56, (uint64_t)stage.st_dev);
		pg_wire_u64(header + 64, (uint64_t)stage.st_ino);
		builder->original_stage_device = (uint64_t)stage.st_dev;
		builder->original_stage_inode = (uint64_t)stage.st_ino;
		pg_session_stat_save(header + 72, &builder->target);
		pg_wire_u32(header + 112, (uint32_t)path_size);
		pg_wire_u32(header + 116, (uint32_t)stage_size);
		pg_wire_u32(header + 120, (uint32_t)scratch_size);
		memcpy(bytes, header, sizeof(header));
		size_t at = sizeof(header);

		memcpy(bytes + at, builder->native_path, path_size);
		at += path_size;
		memcpy(bytes + at, builder->staging_path, stage_size);
		at += stage_size;
		memcpy(bytes + at, builder->staging_directory, scratch_size);
		at += scratch_size;
		status = pg_read_span(fileno(image), image_size, 0, bytes + at,
			(size_t)image_size);
		at += (size_t)image_size;
		if (status == PG_OK)
			status = pg_read_span(fileno(builder->staging),
				stage.st_size, dl_offset, bytes + at,
				(size_t)dl_size);
		at += (size_t)dl_size;
		for (pg_builder_entry *entry = builder->entries;
			entry && status == PG_OK; entry = entry->next) {
			status = pg_session_payload_hash(
				fileno(builder->staging), stage.st_size,
				entry->stage_offset, entry->stored_size,
				bytes + at);
			at += 16;
		}
		if (status == PG_OK) {
			pg_md5 hash;

			pg_md5_init(&hash);
			pg_md5_update(&hash, bytes, (size_t)total);
			pg_md5_finish(&hash, bytes + 128);
		}
	}
	if (fclose(image) && status == PG_OK)
		status = PG_IO;
	if (status == PG_OK)
		status = pg_session_select(
			builder, "checkpoint", bytes, (size_t)total, error);
	else
		pg_native_result(status, errno, error);
	free(bytes);
	return status;
}

pg_status pg_session_publication_prepare(pg_archive_builder *builder,
	const struct stat *created, pg_error *error)
{
	if (!builder->session_path)
		return PG_OK;
	uint8_t bytes[80] = {};
	pg_md5 hash;

	memcpy(bytes, "PGINT001", 8);
	pg_session_stat_save(bytes + 8, created);
	pg_md5_init(&hash);
	pg_md5_update(&hash, bytes, sizeof(bytes));
	pg_md5_finish(&hash, bytes + 48);
	return pg_session_select(
		builder, "intent", bytes, sizeof(bytes), error);
}

pg_status pg_archive_builder_create_session(pg_context *context,
	const char *path, const char *session,
	const pg_archive_build_options *options, pg_archive_builder **out,
	pg_error *error)
{
	if (out)
		*out = NULL;
	if (!out || !context || !path || !session || !*session || !options)
		return pg_result(PG_INVALID, error);
	if (options->archive.checksum_domain > PG_CHECKSUM_STORED ||
		(options->archive.flags & ~PG_OVERWRITE) ||
		(options->staging_directory && !*options->staging_directory))
		return pg_result(PG_INVALID, error);
	if (options->hogg_user_capacity >= UINT32_MAX / 48)
		return pg_result(PG_LIMIT, error);
	if (options->archive.format != PG_HOGG10)
		return pg_result(PG_UNSUPPORTED, error);
	pg_status status = PG_OK;
	int native_code = 0;
	char *directory = pg_native_absolute(session, &status, &native_code);

	if (!directory)
		return pg_native_result(status, native_code, error);
	int creation_parent = -1;
	char *creation_leaf = NULL;
	status = pg_native_parent_open(
		directory, 0, &creation_parent, &creation_leaf, &native_code);
	if (status != PG_OK) {
		free(directory);
		return pg_native_result(status, native_code, error);
	}
	int made = mkdirat(creation_parent, creation_leaf, 0700);
	native_code = errno;
	close(creation_parent);
	free(creation_leaf);
	errno = native_code;
	if (made) {
		native_code = errno;
		free(directory);
		return pg_native_result(
			native_code == EEXIST ? PG_EXISTS : PG_IO, native_code,
			error);
	}
	pg_archive_builder *builder = NULL;
	status = pg_archive_builder_create_config(
		context, path, options, &builder, error);
	if (status != PG_OK) {
		pg_session_remove_directory(directory);
		free(directory);
		return status;
	}
	builder->session_path = directory;
	builder->session_fd = open(
		directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	builder->session_lock = openat(builder->session_fd, "lock",
		O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (builder->session_fd < 0 || builder->session_lock < 0)
		status = pg_native_result(PG_IO, errno, error);
	else {
		status = pg_native_writer_lease(
			builder->session_lock, &native_code);
		if (status == PG_OK)
			status = pg_native_writer_lease(
				fileno(builder->staging), &native_code);
		if (status != PG_OK)
			pg_native_result(status, native_code, error);
	}
	if (status == PG_OK)
		status = pg_archive_builder_checkpoint(builder, error);
	if (status != PG_OK) {
		pg_archive_builder_discard(&builder, NULL);
		return status;
	}
	*out = builder;
	return pg_result(PG_OK, error);
}

static pg_status pg_session_restore(pg_archive_builder *builder,
	const uint8_t *image, size_t image_size, const uint8_t *datalist,
	size_t dl_size, const uint8_t *verifiers, uint64_t count, int published)
{
	uint64_t table = 24 + pg_read_u16(image + 6) + pg_read_u16(image + 20);
	uint64_t base =
		table + pg_read_u32(image + 8) + pg_read_u32(image + 12);
	uint32_t dl_slot = pg_read_u32(image + 16);

	if (pg_read_u32(image) != 0xdeadf00d || pg_read_u16(image + 4) != 10 ||
		pg_read_u16(image + 6) != 1024 ||
		pg_read_u16(image + 20) != 3096 || base != image_size ||
		dl_slot != count || count >= pg_read_u32(image + 8) / 32 ||
		pg_read_u32(image + 8) % 32 || pg_read_u32(image + 12) % 16)
		return PG_CORRUPT;
	const uint8_t *dl_record = image + table + 32 * count;
	uint64_t dl_offset = pg_read_u64(dl_record);

	if (dl_size != pg_read_u32(dl_record + 8))
		return PG_CORRUPT;
	int fd = fileno(builder->staging);
	pg_status status = PG_OK;

	if (!published) {
		status = pg_session_write(fd, 0, image, image_size);
		if (status == PG_OK)
			status = pg_session_write(
				fd, dl_offset, datalist, dl_size);
		if (status == PG_OK && fsync(fd))
			status = PG_IO;
	}
	pg_source probe = {};

	probe.context = builder->context;
	probe.format = PG_HOGG10;
	probe.checksum_domain = builder->checksum_domain;
	probe.fd = fd;
	if (status == PG_OK && fstat(fd, &probe.identity))
		status = PG_IO;
	if (status == PG_OK)
		status = pg_hogg_index_records(&probe);
	uint64_t index = 0;

	for (pg_source_record *record = probe.records;
		record && status == PG_OK; record = record->next) {
		if (index >= count ||
			record->info.archive_record != index ||
			pg_builder_name_find(
				builder, record->info.canonical_name)) {
			status = PG_CORRUPT;
			break;
		}
		pg_file_info info = record->info;
		uint8_t digest[16];

		status = pg_archive_verify_payload(fd, probe.identity.st_size,
			&info, record->payload_offset, 0);
		if (status == PG_OK)
			status = pg_session_payload_hash(fd,
				probe.identity.st_size,
				record->payload_offset,
				info.stored_size, digest);
		if (status == PG_OK &&
			memcmp(digest, verifiers + 16 * index, 16))
			status = PG_CHECKSUM;
		if (status != PG_OK)
			break;
		pg_builder_entry *entry =
			(pg_builder_entry *)calloc(1, sizeof(*entry));

		if (!entry) {
			status = PG_NOMEM;
			break;
		}
		if (info.cached_header_size) {
			entry->cached_header = malloc(info.cached_header_size);
			if (!entry->cached_header) {
				free(entry);
				status = PG_NOMEM;
				break;
			}
			memcpy(entry->cached_header, info.cached_header,
				info.cached_header_size);
		}
		entry->canonical_name = (char *)record->info.canonical_name;
		entry->original_name = (char *)record->info.original_name;
		entry->cached_header_size = info.cached_header_size;
		entry->stage_offset = record->payload_offset;
		entry->logical_size = info.logical_size;
		entry->stored_size = info.stored_size;
		entry->mtime = info.mtime;
		entry->encoding = info.encoding;
		memcpy(entry->digest, info.digest, 16);
		record->info.canonical_name = NULL;
		record->info.original_name = NULL;
		pg_builder_entry_insert(builder, entry);
		index++;
	}
	if (status == PG_OK && index != count)
		status = PG_CORRUPT;
	while (probe.records) {
		pg_source_record *next = probe.records->next;

		pg_record_release(probe.records);
		probe.records = next;
	}
	return status;
}

pg_status pg_archive_builder_resume(pg_context *context, const char *session,
	pg_archive_builder **out, pg_error *error)
{
	if (out)
		*out = NULL;
	if (!out || !context || !session || !*session)
		return pg_result(PG_INVALID, error);
	pg_status status = PG_OK;
	int native_code = 0;
	char *directory = pg_native_absolute(session, &status, &native_code);

	if (!directory)
		return pg_native_result(status, native_code, error);
	int parent = open(
		directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	int lock = parent >= 0
		? openat(parent, "lock", O_RDWR | O_NOFOLLOW | O_CLOEXEC)
		: -1;
	pg_archive_builder *builder = NULL;
	uint8_t *bytes = NULL, *intent = NULL;
	size_t size = 0, intent_size = 0;
	int published = 0;
	struct stat destination;
	char *path = NULL, *stage_path = NULL, *scratch = NULL;
	uint64_t image_size = 0, dl_size = 0, count = 0;
	size_t at = 192;
	pg_archive_build_options options = {};

	if (lock < 0) {
		status = errno == ENOENT ? PG_NOT_FOUND : PG_IO;
		goto done_resume;
	}
	status = pg_native_writer_lease(lock, &native_code);
	if (status != PG_OK)
		goto done_resume;
	status = pg_session_recheck(parent, directory);
	if (status != PG_OK)
		goto done_resume;
	status = pg_session_read(directory, "checkpoint", &bytes, &size);
	if (status != PG_OK)
		goto done_resume;
	if (size < 192 || memcmp(bytes, "PGSESS01", 8) ||
		pg_read_u32(bytes + 8) != 1) {
		status = PG_CORRUPT;
		goto done_resume;
	}
	{
		uint8_t saved[16], digest[16];
		pg_md5 hash;

		memcpy(saved, bytes + 128, 16);
		memset(bytes + 128, 0, 16);
		pg_md5_init(&hash);
		pg_md5_update(&hash, bytes, size);
		pg_md5_finish(&hash, digest);
		if (memcmp(saved, digest, 16)) {
			status = PG_CORRUPT;
			goto done_resume;
		}
	}
	for (unsigned i = 0; i < 3; i++) {
		uint32_t length = pg_read_u32(bytes + 112 + 4 * i);

		if (!length || length > size - at || bytes[at + length - 1] ||
			memchr(bytes + at, 0, length - 1)) {
			status = PG_CORRUPT;
			goto done_resume;
		}
		if (i == 0)
			path = (char *)bytes + at;
		else if (i == 1)
			stage_path = (char *)bytes + at;
		else
			scratch = (char *)bytes + at;
		at += length;
	}
	count = pg_read_u64(bytes + 32);
	image_size = pg_read_u64(bytes + 40);
	dl_size = pg_read_u64(bytes + 48);
	if (count >= UINT32_MAX / 48 || image_size < 4144 ||
		image_size > size - at || dl_size > size - at - image_size ||
		count > (size - at - image_size - dl_size) / 16 ||
		count * 16 != size - at - image_size - dl_size) {
		status = PG_CORRUPT;
		goto done_resume;
	}
	status = pg_session_read(directory, "intent", &intent, &intent_size);
	if (status == PG_OK) {
		uint8_t saved[16], digest[16];
		pg_md5 hash;
		struct stat expected;

		if (intent_size != 80 || memcmp(intent, "PGINT001", 8)) {
			status = PG_CORRUPT;
			goto done_resume;
		}
		memcpy(saved, intent + 48, 16);
		memset(intent + 48, 0, 16);
		pg_md5_init(&hash);
		pg_md5_update(&hash, intent, intent_size);
		pg_md5_finish(&hash, digest);
		if (memcmp(saved, digest, 16)) {
			status = PG_CORRUPT;
			goto done_resume;
		}
		pg_session_stat_restore(&expected, intent + 8);
		if (!lstat(path, &destination) &&
			pg_native_stat_same(&expected, &destination))
			published = 1;
	} else if (status != PG_NOT_FOUND)
		goto done_resume;
	options.archive = {PG_HOGG10, PG_OVERWRITE, pg_read_u32(bytes + 12)};
	options.staging_directory = scratch;
	options.hogg_user_capacity = pg_read_u64(bytes + 24);
	status = pg_archive_builder_create_config(
		context, path, &options, &builder, error);
	if (status != PG_OK)
		goto done_resume;
	fclose(builder->staging);
	builder->staging = NULL;
	unlink(builder->staging_path);
	close(builder->staging_parent_fd);
	free(builder->staging_path);
	free(builder->staging_leaf);
	builder->staging_path = strdup(stage_path);
	builder->staging_leaf = NULL;
	builder->staging_parent_fd = -1;
	if (!builder->staging_path) {
		status = PG_NOMEM;
		goto done_resume;
	}
	{
		int fd = open(published ? path : stage_path,
			(published ? O_RDONLY : O_RDWR) | O_NOFOLLOW |
				O_CLOEXEC);
		struct stat stage;

		if (fd < 0) {
			status = PG_STALE;
			goto done_resume;
		}
		if (fstat(fd, &stage) || !S_ISREG(stage.st_mode) ||
			(!published &&
				((uint64_t)stage.st_dev !=
						pg_read_u64(bytes + 56) ||
					(uint64_t)stage.st_ino !=
						pg_read_u64(bytes + 64)))) {
			close(fd);
			status = PG_STALE;
			goto done_resume;
		}
		if (!published) {
			status = pg_native_writer_lease(fd, &native_code);
			if (status != PG_OK) {
				close(fd);
				goto done_resume;
			}
		}
		builder->staging = fdopen(fd, published ? "rb" : "r+b");
		if (!builder->staging) {
			close(fd);
			status = PG_IO;
			goto done_resume;
		}
	}
	status = pg_native_parent_open(stage_path, 0,
		&builder->staging_parent_fd, &builder->staging_leaf,
		&native_code);
	if (status != PG_OK)
		goto done_resume;
	builder->flags = pg_read_u32(bytes + 16);
	builder->target_exists = (int)pg_read_u32(bytes + 20);
	pg_session_stat_restore(&builder->target, bytes + 72);
	builder->session_path = directory;
	directory = NULL;
	builder->session_fd = parent;
	parent = -1;
	builder->session_lock = lock;
	lock = -1;
	builder->original_stage_device = pg_read_u64(bytes + 56);
	builder->original_stage_inode = pg_read_u64(bytes + 64);
	builder->published = published;
	builder->finished = published;
	status = pg_session_restore(builder, bytes + at, (size_t)image_size,
		bytes + at + image_size, (size_t)dl_size,
		bytes + at + image_size + dl_size, count, published);
	if (status == PG_OK) {
		*out = builder;
		builder = NULL;
	}
done_resume:
	if (builder) {
		/* Failure retains the last checkpoint and its payloads. */
		builder->retain_staging = 1;
		pg_archive_builder_close(&builder, NULL);
	}
	if (lock >= 0)
		close(lock);
	if (parent >= 0)
		close(parent);
	free(directory);
	free(bytes);
	free(intent);
	return pg_native_result(status, native_code, error);
}

pg_status pg_archive_builder_suspend(
	pg_archive_builder **builder, pg_error *error)
{
	if (!builder)
		return pg_result(PG_INVALID, error);
	if (!*builder)
		return pg_result(PG_OK, error);
	pg_status status = pg_archive_builder_checkpoint(*builder, error);

	if (status != PG_OK && status != PG_COMMITTED)
		return status;
	pg_error close_error;
	pg_status closed = pg_archive_builder_close(builder, &close_error);

	if (status == PG_OK && closed != PG_OK) {
		if (error)
			*error = close_error;
		return closed;
	}
	return status;
}

pg_status pg_archive_builder_visit_inventory(pg_archive_builder *builder,
	pg_archive_inventory_fn visit, void *state, pg_error *error)
{
	if (!builder || !visit)
		return pg_result(PG_INVALID, error);
	if (builder->visiting)
		return pg_result(PG_REENTRANT, error);
	builder->visiting = 1;
	pg_status status = PG_OK;
	uint64_t ordinal = 0;

	for (pg_builder_entry *entry = builder->entries; entry;
		entry = entry->next) {
		pg_file_info info = {};

		info.canonical_name = entry->canonical_name;
		info.original_name = pg_wire_name(entry);
		info.archive_record = ordinal++;
		info.logical_size = entry->logical_size;
		info.stored_size = entry->stored_size;
		info.mtime = entry->mtime;
		info.encoding = entry->encoding;
		info.digest_kind = builder->format == PG_HOGG10
			? PG_DIGEST_MD5_32
			: PG_DIGEST_MD5;
		info.checksum_domain = builder->checksum_domain;
		memcpy(info.digest, entry->digest,
			builder->format == PG_HOGG10 ? 4 : 16);
		info.cached_header = entry->cached_header;
		info.cached_header_size = entry->cached_header_size;
		status = visit(state, &info);
		if (status != PG_OK)
			break;
	}
	builder->visiting = 0;
	return pg_result(status, error);
}

pg_status pg_archive_builder_discard(
	pg_archive_builder **builder, pg_error *error)
{
	if (!builder)
		return pg_result(PG_INVALID, error);
	if (!*builder)
		return pg_result(PG_OK, error);
	pg_archive_builder *owned = *builder;

	if (!owned->session_path)
		return pg_result(PG_INVALID, error);
	if (owned->visiting)
		return pg_result(PG_REENTRANT, error);
	if (owned->live_writers)
		return pg_result(PG_BUSY, error);
	pg_status status =
		pg_session_recheck(owned->session_fd, owned->session_path);
	if (status != PG_OK)
		return pg_native_result(status, errno, error);
	int native_code = 0;

	const char *leaves[] = {"checkpoint", "intent", "lock"};
	for (const char *leaf : leaves) {
		if (unlinkat(owned->session_fd, leaf, 0) && errno != ENOENT) {
			status = PG_IO;
			native_code = errno;
		}
	}
	int scan = openat(owned->session_fd, ".",
		O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	DIR *directory = scan >= 0 ? fdopendir(scan) : NULL;

	if (!directory) {
		if (scan >= 0)
			close(scan);
		status = PG_IO;
		native_code = errno;
	} else {
		struct dirent *item;

		errno = 0;
		while ((item = readdir(directory))) {
			if (strncmp(item->d_name, ".piggle-stage-", 14))
				continue;
			if (unlinkat(owned->session_fd, item->d_name, 0)) {
				status = PG_IO;
				native_code = errno;
			}
			errno = 0;
		}
		if (errno) {
			status = PG_IO;
			native_code = errno;
		}
		if (closedir(directory) && status == PG_OK) {
			status = PG_IO;
			native_code = errno;
		}
	}
	if (pg_session_remove_directory(owned->session_path) &&
		status == PG_OK) {
		status = PG_IO;
		native_code = errno;
	}
	owned->discard_session = 1;
	pg_error close_error;
	pg_status closed = pg_archive_builder_close(builder, &close_error);

	if (status == PG_OK && closed != PG_OK) {
		status = closed;
		native_code = close_error.native_code;
	}
	return pg_native_result(status, native_code, error);
}
