#include <piggle/piggle.h>
#include "internal.hpp"
#include "archive_read.hpp"
#include "archive_wire.hpp"
#include "native_path.hpp"

#include <fcntl.h>
#ifndef _WIN32
#include <dirent.h>
#endif

static pg_status pg_source_write_span(int fd, uint64_t offset,
		const void *data, size_t size, int *native_code)
{
	const uint8_t *bytes = (const uint8_t *)data;

	if (offset > INT64_MAX || size > (uint64_t)INT64_MAX - offset)
		return PG_LIMIT;
	while (size) {
		ssize_t count = pwrite(fd, bytes, size, (off_t)offset);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			*native_code = count < 0 ? errno : EIO;
			return PG_IO;
		}
		bytes += count;
		offset += (uint64_t)count;
		size -= (size_t)count;
	}
	return PG_OK;
}

extern "C" {
static int pg_source_record_order(const void *left, const void *right);
static int pg_source_items_directory(pg_source_record **items, size_t count,
		const char *name);
}

/* Build once per structural refresh; source controls serialize its lifetime. */
static pg_status pg_source_index_get(pg_source *source,
		pg_source_record ***out, size_t *count)
{
	if (!source->record_index) {
		size_t size = 0;

		for (pg_source_record *r = source->records; r; r = r->next)
			size++;
		if (size > SIZE_MAX / sizeof(*source->record_index))
			return PG_LIMIT;
		pg_source_record **items = (pg_source_record **)malloc(
			(size ? size : 1) * sizeof(*items));

		if (!items)
			return PG_NOMEM;
		size_t i = 0;

		for (pg_source_record *r = source->records; r; r = r->next)
			items[i++] = r;
		qsort(items, size, sizeof(*items), pg_source_record_order);
		source->record_index = items;
		source->record_count = size;
	}
	*out = source->record_index;
	*count = source->record_count;
	return PG_OK;
}

static pg_source_record *pg_source_index_last(pg_source_record **items,
		size_t count, const char *name)
{
	size_t low = 0, high = count;

	while (low < high) {
		size_t middle = low + (high - low) / 2;

		if (strcmp(items[middle]->info.canonical_name, name) <= 0)
			low = middle + 1;
		else
			high = middle;
	}
	return low && !strcmp(items[low - 1]->info.canonical_name, name) ?
		items[low - 1] : NULL;
}

static void pg_source_index_scope(pg_source_record **items, size_t count,
		const char *prefix, size_t *begin, size_t *end)
{
	size_t low = 0, high = count;

	while (low < high) {
		size_t middle = low + (high - low) / 2;

		if (pg_descendant_order(items[middle]->info.canonical_name,
			prefix) < 0)
			low = middle + 1;
		else
			high = middle;
	}
	*begin = low;
	high = count;
	while (low < high) {
		size_t middle = low + (high - low) / 2;

		if (pg_descendant_order(items[middle]->info.canonical_name,
			prefix) <= 0)
			low = middle + 1;
		else
			high = middle;
	}
	*end = low;
}

pg_status pg_source_records_scope(pg_source *source, const char *prefix,
		pg_source_record ***out, size_t *count)
{
	pg_status status = pg_source_index_get(source, out, count);

	if (status != PG_OK)
		return status;
	if (prefix && *prefix) {
		size_t begin, end;

		pg_source_index_scope(*out, *count, prefix, &begin, &end);
		*out += begin;
		*count = end - begin;
	}
	return PG_OK;
}

static int pg_source_field_mix(uint32_t current, uint32_t before,
		uint32_t after)
{
	unsigned int i;

	for (i = 0; i < 32; i += 8) {
		uint32_t byte = (current >> i) & 0xffu;

		if (byte != ((before >> i) & 0xffu) &&
		    byte != ((after >> i) & 0xffu))
			return 0;
	}
	return 1;
}

static pg_status pg_source_transfer_reader(pg_source *destination,
		const char *name, pg_reader **reader,
		const pg_write_options *options, pg_error *error)
{
	pg_writer *writer = NULL;
	pg_error work_error, close_error;
	pg_status status, closed;

	status = pg_writer_open_source(destination, name, options,
		&writer, &work_error);
	if (status != PG_OK)
		goto transfer_done;
	if (writer->target_exists &&
	    writer->target.st_dev == (*reader)->identity.st_dev &&
	    writer->target.st_ino == (*reader)->identity.st_ino) {
		status = pg_result(PG_CONFLICT, &work_error);
		goto transfer_done;
	}
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

/* Preflight every destination, then publish captured files in order. */
pg_status pg_unpack_cursor(pg_cursor *cursor, pg_context *context,
		const char *native_directory, uint32_t flags,
		pg_error *error)
{
	pg_unpack_target *target = NULL;
	pg_status status, target_closed;
	pg_error work_error, close_error;
	size_t i;
	size_t published = 0;

	if (!cursor || !context || !native_directory ||
	    !*native_directory || (flags & ~PG_OVERWRITE) ||
	    cursor->position || cursor->context != context)
		return pg_result(PG_INVALID, error);
	status = pg_unpack_target_open(cursor, native_directory,
		flags, &target, &work_error);
	if (status != PG_OK)
		goto unpack_done;
	for (i = 0; i < cursor->count; i++) {
		pg_file *file = NULL;
		pg_reader *reader = NULL;
		pg_writer *writer = NULL;
		pg_status closed;

		status = pg_cursor_next(cursor, &file, &work_error);
		if (status != PG_OK)
			break;
		status = pg_reader_open(file, PG_READ_LOGICAL,
			&reader, &work_error);
		if (status == PG_OK)
			status = pg_writer_open_unpack(target, file,
				&writer, &work_error);
		if (status == PG_OK)
			status = pg_transfer_bytes(reader, writer, &work_error);
		closed = pg_reader_close(&reader, &close_error);
		if (status == PG_OK && closed != PG_OK) {
			status = closed;
			work_error = close_error;
		}
		if (status == PG_OK)
			status = pg_writer_finish(writer, &work_error);
		if (status == PG_OK || status == PG_COMMITTED)
			published++;
		closed = pg_writer_close(&writer, &close_error);
		if (status == PG_OK && closed != PG_OK) {
			status = PG_COMMITTED;
			work_error = close_error;
			pg_result(status, &work_error);
			work_error.cause = PG_IO;
		}
		closed = pg_file_close(&file, &close_error);
		if (status == PG_OK && closed != PG_OK) {
			status = PG_COMMITTED;
			work_error = close_error;
			pg_result(status, &work_error);
			work_error.cause = PG_IO;
		}
		if (status != PG_OK)
			break;
	}
	target_closed = pg_unpack_target_close(&target,
		&close_error);

	if (status == PG_OK && target_closed != PG_OK) {
		status = published ? PG_COMMITTED : target_closed;
		work_error = close_error;
		if (published) {
			pg_result(status, &work_error);
			work_error.cause = target_closed;
		}
	}

unpack_done:
	if (status != PG_OK && published &&
	    published < cursor->count) {
		pg_status cause = status == PG_COMMITTED ?
			work_error.cause : status;

		status = PG_PARTIAL;
		pg_result(status, &close_error);
		close_error.cause = cause;
		close_error.native_code = work_error.native_code;
		work_error = close_error;
	}
	if (status == PG_OK)
		pg_result(PG_OK, &work_error);
	if (error)
		*error = work_error;
	return status;
}

static void pg_source_records_free(pg_source_record *record)
{
	while (record) {
		pg_source_record *next = record->next;

		free((void *)record->info.canonical_name);
		free((void *)record->info.original_name);
		free((void *)record->info.cached_header);
		free(record->native_path);
		free(record);
		record = next;
	}
}

static void pg_source_requests_free(pg_source_request *request)
{
	while (request) {
		pg_source_request *next = request->next;

		free(request->name);
		free(request);
		request = next;
	}
}

static pg_status pg_source_request_add(pg_source_request **head,
		const char *name)
{
	pg_source_request *request;

	for (request = *head; request; request = request->next) {
		if (strcmp(request->name, name) == 0)
			return PG_OK;
	}
	request = (pg_source_request *)calloc(1, sizeof(*request));
	if (!request)
		return PG_NOMEM;
	request->name = strdup(name);
	if (!request->name) {
		free(request);
		return PG_NOMEM;
	}
	request->next = *head;
	*head = request;
	return PG_OK;
}

static pg_status pg_source_append_component(char **path,
		const char *component)
{
	size_t prefix = strlen(*path);
	size_t length = strlen(component);
	char *grown;

	if (prefix > SIZE_MAX - length - 2)
		return PG_LIMIT;
	grown = (char *)realloc(*path, prefix + length + 2);
	if (!grown)
		return PG_NOMEM;
	grown[prefix] = '/';
	memcpy(grown + prefix + 1, component, length + 1);
	*path = grown;
	return PG_OK;
}

/* Explore only canonical-matching branches and compare complete spellings. */
static pg_status pg_source_probe_walk(pg_source *source, int dir,
		const char *remaining, const char *prefix, char **original,
		struct stat *identity, int *directory, int *native_code)
{
	const char *slash = strchr(remaining, '/');
	size_t length = slash ? (size_t)(slash - remaining) : strlen(remaining);
	struct stat before, after;
	pg_status status = PG_OK;
	DIR *listing;
	struct dirent *item;
	int scan = openat(dir, ".", O_RDONLY | O_DIRECTORY |
		O_NOFOLLOW | O_CLOEXEC);

	if (scan < 0 || fstat(dir, &before)) {
		*native_code = errno;
		if (scan >= 0)
			close(scan);
		return PG_IO;
	}
	listing = fdopendir(scan);
	if (!listing) {
		*native_code = errno;
		close(scan);
		return PG_IO;
	}
	errno = 0;
	while ((item = readdir(listing))) {
		size_t required = 0;
		char *folded, *next;
		struct stat found, opened;
		int child = -1;

		if (pg_name_normalize(source->context, item->d_name,
			NULL, 0, &required, NULL) != PG_CAPACITY)
			continue;
		folded = (char *)malloc(required);
		if (!folded) {
			status = PG_NOMEM;
			break;
		}
		status = pg_name_normalize(source->context, item->d_name,
			folded, required, &required, NULL);
		int match = status == PG_OK && strlen(folded) == length &&
			!memcmp(folded, remaining, length);

		free(folded);
		if (!match) {
			errno = 0;
			continue;
		}
		if (fstatat(dir, item->d_name, &found, AT_SYMLINK_NOFOLLOW)) {
			*native_code = errno;
			status = errno == ENOENT ? PG_RETRY : PG_IO;
			break;
		}
		if (slash && S_ISREG(found.st_mode)) {
			errno = 0;
			continue;
		}
		if (!S_ISDIR(found.st_mode) && !S_ISREG(found.st_mode)) {
			status = PG_CONFLICT;
			break;
		}
		if (slash) {
			child = openat(dir, item->d_name, O_RDONLY |
				O_NOFOLLOW | O_CLOEXEC | O_DIRECTORY);
			if (child < 0) {
				*native_code = errno;
				status = PG_RETRY;
				break;
			}
			if (fstat(child, &opened) ||
			    !pg_native_stat_same(&found, &opened)) {
				close(child);
				status = PG_RETRY;
				break;
			}
		}
		next = strdup(prefix);
		status = next ? pg_source_append_component(&next,
			item->d_name) : PG_NOMEM;
		if (status == PG_OK && slash) {
			status = pg_source_probe_walk(source, child, slash + 1,
				next, original, identity, directory,
				native_code);
		} else if (status == PG_OK && S_ISDIR(found.st_mode)) {
			*directory = 1;
		} else if (status == PG_OK &&
			(!*original || strcmp(next, *original) > 0)) {
			free(*original);
			*original = next;
			next = NULL;
			*identity = found;
		}
		free(next);
		if (child >= 0)
			close(child);
		if (status != PG_OK)
			break;
		errno = 0;
	}
	if (!item && errno && status == PG_OK) {
		*native_code = errno;
		status = PG_IO;
	}
	if (status == PG_OK && (fstat(dir, &after) ||
	    !pg_native_stat_same(&before, &after)))
		status = PG_RETRY;
	closedir(listing);
	return status;
}

static pg_status pg_source_loose_probe(pg_source *source,
		const char *canonical, pg_source_record **selected,
		int *native_code)
{
	char *path = NULL, *original = NULL;
	pg_source_record *record;
	struct stat found;
	int directory = 0;
	pg_status status;

	*selected = NULL;
	status = pg_source_probe_walk(source, source->fd, canonical, "",
		&original, &found, &directory, native_code);
	if (status != PG_OK)
		goto cleanup_probe;
	if (directory || !original) {
		status = directory ? PG_CONFLICT : PG_NOT_FOUND;
		goto cleanup_probe;
	}
	path = strdup(source->native_path);
	status = path ? pg_source_append_component(&path, original + 1) :
		PG_NOMEM;
	if (status != PG_OK)
		goto cleanup_probe;
	for (record = source->records; record; record = record->next) {
		if (strcmp(record->info.canonical_name, canonical) == 0)
			break;
	}
	if (record && record->identity.st_dev == found.st_dev &&
	    record->identity.st_ino == found.st_ino) {
		char *spelling = strdup(original + 1);

		if (!spelling) {
			status = PG_NOMEM;
			goto cleanup_probe;
		}
		if (!pg_native_stat_same(&record->identity, &found)) {
			if (record->info.copy_generation == UINT64_MAX) {
				free(spelling);
				status = PG_LIMIT;
				goto cleanup_probe;
			}
			record->info.copy_generation++;
		}
		free(record->native_path);
		record->native_path = path;
		path = NULL;
		free((void *)record->info.original_name);
		record->info.original_name = spelling;
	} else {
		if (!source->context->next_id) {
			status = PG_LIMIT;
			goto cleanup_probe;
		}
		record = (pg_source_record *)calloc(1, sizeof(*record));
		if (!record) {
			status = PG_NOMEM;
			goto cleanup_probe;
		}
		record->info.canonical_name = strdup(canonical);
		record->info.original_name = strdup(original + 1);
		if (!record->info.canonical_name ||
		    !record->info.original_name) {
			pg_source_records_free(record);
			status = PG_NOMEM;
			goto cleanup_probe;
		}
		record->native_path = path;
		path = NULL;
		record->info.source_id = source->id;
		record->info.source_generation = source->generation;
		record->info.copy_id = source->context->next_id++;
		record->info.copy_generation = 1;
		pg_source_index_clear(source);
		record->next = source->records;
		source->records = record;
	}
	record->identity = found;
	record->info.archive_record = UINT64_MAX;
	record->info.logical_size = (uint64_t)found.st_size;
	record->info.stored_size = (uint64_t)found.st_size;
	record->info.mtime = found.st_mtime;
	record->info.encoding = PG_LOGICAL;
	*selected = record;

cleanup_probe:
	free(path);
	free(original);
	return status;
}

static pg_status pg_source_loose_add(pg_source *source,
		const char *canonical, const char *original,
		const struct stat *identity, uint32_t attributes)
{
	pg_source_record *record;
	char *native;
	pg_status status;

	if (identity->st_size < 0)
		return PG_LIMIT;
	if (!source->context->next_id)
		return PG_LIMIT;
	record = (pg_source_record *)calloc(1, sizeof(*record));
	if (!record)
		return PG_NOMEM;
	native = strdup(source->native_path);
	record->info.canonical_name = strdup(canonical);
	record->info.original_name = strdup(original);
	if (!native || !record->info.canonical_name ||
	    !record->info.original_name) {
		free(native);
		pg_source_records_free(record);
		return PG_NOMEM;
	}
	status = pg_source_append_component(&native, original);
	if (status != PG_OK) {
		free(native);
		pg_source_records_free(record);
		return status;
	}
	record->native_path = native;
	record->identity = *identity;
	record->attributes = attributes;
#ifdef _WIN32
	if (identity->st_attributes & FILE_ATTRIBUTE_HIDDEN)
		record->attributes |= PG_ENTRY_HIDDEN;
	if (identity->st_attributes & FILE_ATTRIBUTE_SYSTEM)
		record->attributes |= PG_ENTRY_SYSTEM;
	if (!(identity->st_mode & _S_IWRITE))
#else
	const char *basename = strrchr(original, '/');

	if ((basename ? basename[1] : original[0]) == '.')
		record->attributes |= PG_ENTRY_HIDDEN;
	if (!(identity->st_mode & 0222))
#endif
		record->attributes |= PG_ENTRY_READ_ONLY;
	record->info.source_id = source->id;
	record->info.source_generation = source->generation;
	record->info.copy_id = source->context->next_id++;
	record->info.copy_generation = 1;
	record->info.archive_record = UINT64_MAX;
	record->info.logical_size = S_ISDIR(identity->st_mode) ? 0 :
		(uint64_t)identity->st_size;
	record->info.stored_size = record->info.logical_size;
	record->info.mtime = identity->st_mtime;
	record->info.encoding = PG_LOGICAL;
	pg_source_index_clear(source);
	record->next = source->records;
	source->records = record;
	return PG_OK;
}

static int pg_source_record_name_compare(const void *left,
		const void *right)
{
	const pg_source_record *a = *(pg_source_record *const *)left;
	const pg_source_record *b = *(pg_source_record *const *)right;
	int order = strcmp(a->info.canonical_name, b->info.canonical_name);

	if (!order &&
		S_ISDIR(a->identity.st_mode) != S_ISDIR(b->identity.st_mode))
		return S_ISDIR(a->identity.st_mode) ? 1 : -1;
	return order ? order : strcmp(a->info.original_name,
		b->info.original_name);
}

/* Select one original spelling for each canonical loose name. */
static pg_status pg_source_loose_compact(pg_source *source)
{
	pg_source_index_clear(source);
	pg_source_record **items;
	pg_source_record *record;
	size_t count = 0;
	size_t kept = 0;
	size_t i;

	for (record = source->records; record; record = record->next)
		count++;
	if (!count)
		return PG_OK;
	if (count > SIZE_MAX / sizeof(*items))
		return PG_LIMIT;
	items = (pg_source_record **)malloc(count * sizeof(*items));
	if (!items)
		return PG_NOMEM;
	for (record = source->records, i = 0; record;
	     record = record->next)
		items[i++] = record;
	qsort(items, count, sizeof(*items), pg_source_record_name_compare);
	for (i = 0; i < count; i++) {
		if (i + 1 < count && strcmp(items[i]->info.canonical_name,
			items[i + 1]->info.canonical_name) == 0) {
			items[i]->next = NULL;
			pg_source_records_free(items[i]);
			continue;
		}
		items[kept++] = items[i];
	}
	for (i = 0; i < kept; i++)
		items[i]->next = i + 1 < kept ? items[i + 1] : NULL;
	source->records = items[0];
	free(items);
	return PG_OK;
}

static pg_status pg_source_record_index(pg_source_record *head,
		pg_source_record ***out, size_t *size)
{
	pg_source_record **items;
	pg_source_record *record;
	size_t count = 0, i = 0;

	*out = NULL;
	*size = 0;
	for (record = head; record; record = record->next)
		count++;
	if (count > SIZE_MAX / sizeof(*items))
		return PG_LIMIT;
	items = (pg_source_record **)malloc((count ? count : 1) *
		sizeof(*items));
	if (!items)
		return PG_NOMEM;
	for (record = head; record; record = record->next)
		items[i++] = record;
	qsort(items, count, sizeof(*items), pg_source_record_name_compare);
	*out = items;
	*size = count;
	return PG_OK;
}

static pg_source_record *pg_source_record_index_find(
		pg_source_record **items, size_t count, const char *name)
{
	size_t low = 0, high = count;

	while (low < high) {
		size_t middle = low + (high - low) / 2;
		int order = strcmp(items[middle]->info.canonical_name,
			name);

		if (!order)
			return items[middle];
		if (order < 0)
			low = middle + 1;
		else
			high = middle;
	}
	return NULL;
}

static pg_status pg_source_loose_scan_fallback(pg_source *source, int dir,
		const char *prefix, const char *wanted,
		int *native_code)
{
	DIR *listing;
	struct dirent *item;
	int scan = openat(dir, ".", O_RDONLY | O_DIRECTORY |
		O_NOFOLLOW | O_CLOEXEC);
	pg_status status = PG_OK;

	if (scan < 0) {
		*native_code = errno;
		return PG_IO;
	}
	listing = fdopendir(scan);
	if (!listing) {
		*native_code = errno;
		close(scan);
		return PG_IO;
	}
	errno = 0;
	while ((item = readdir(listing))) {
		struct stat found = {};
		char *next;
		char *canonical = NULL;
		size_t required = 0;
		int child;

		if (strcmp(item->d_name, ".") == 0 ||
		    strcmp(item->d_name, "..") == 0)
			continue;
		next = strdup(prefix);
		if (!next) {
			status = PG_NOMEM;
			break;
		}
		status = pg_source_append_component(&next, item->d_name);
		if (status != PG_OK) {
			free(next);
			break;
		}
		if (pg_name_normalize(source->context, next + 1,
			NULL, 0, &required, NULL) != PG_CAPACITY) {
			free(next);
			continue;
		}
		canonical = (char *)malloc(required);
		if (!canonical) {
			free(next);
			status = PG_NOMEM;
			break;
		}
		status = pg_name_normalize(source->context, next + 1,
			canonical, required, &required, NULL);
		if (status == PG_OK && wanted) {
			size_t candidate = strlen(canonical);
			size_t requested = strlen(wanted);

			if (strcmp(canonical, wanted) != 0 &&
			    !(candidate < requested &&
			      strncmp(canonical, wanted, candidate) == 0 &&
			      wanted[candidate] == '/') &&
			    !(requested < candidate &&
			      strncmp(canonical, wanted, requested) == 0 &&
			      canonical[requested] == '/')) {
				free(canonical);
				free(next);
				continue;
			}
		}
		if (status == PG_OK && fstatat(dir, item->d_name,
			&found, AT_SYMLINK_NOFOLLOW)) {
			*native_code = errno;
			status = errno == ENOENT ? PG_RETRY : PG_IO;
		}
		if (status == PG_OK && !S_ISREG(found.st_mode) &&
		    !S_ISDIR(found.st_mode)) {
			free(canonical);
			free(next);
			continue;
		}
		if (status == PG_OK && S_ISDIR(found.st_mode)) {
			status = pg_source_loose_add(source, canonical,
				next + 1, &found, 0);
			if (status != PG_OK) {
				free(canonical);
				free(next);
				break;
			}
			if (source->scan_shallow &&
			    (!wanted || pg_descendant_order(canonical,
				wanted) == 0 ||
			     (source->scan_shallow == 2 && !strcmp(canonical,
				wanted)))) {
				free(canonical);
				free(next);
				errno = 0;
				continue;
			}
			child = openat(dir, item->d_name,
				O_RDONLY | O_DIRECTORY | O_NOFOLLOW |
				O_CLOEXEC);
			if (child < 0) {
				*native_code = errno;
				status = PG_RETRY;
			} else {
				status = pg_source_loose_scan_fallback(source, child,
					next, wanted, native_code);
				close(child);
			}
		} else if (status == PG_OK &&
			(!wanted || strcmp(canonical, wanted) == 0 ||
			 (strncmp(canonical, wanted, strlen(wanted)) == 0 &&
			  canonical[strlen(wanted)] == '/'))) {
			status = pg_source_loose_add(source, canonical,
				next + 1, &found, 0);
		}
		free(canonical);
		free(next);
		if (status != PG_OK)
			break;
		errno = 0;
	}
	if (!item && errno && status == PG_OK) {
		*native_code = errno;
		status = PG_IO;
	}
	closedir(listing);
	return status;
}

#ifdef _WIN32
static int pg_source_loose_wanted(const char *canonical,
		const char *wanted)
{
	size_t candidate, requested;

	if (!wanted)
		return 1;
	candidate = strlen(canonical);
	requested = strlen(wanted);
	return strcmp(canonical, wanted) == 0 ||
		(candidate < requested &&
		 strncmp(canonical, wanted, candidate) == 0 &&
		 wanted[candidate] == '/') ||
		(requested < candidate &&
		 strncmp(canonical, wanted, requested) == 0 &&
		 canonical[requested] == '/');
}

/* Match the opened directory to its enumeration entry. */
static pg_status pg_source_windows_dir_same(HANDLE child,
		uint64_t volume, uint64_t file_id, int *native_code)
{
	FILE_ID_INFO id_info;
	BY_HANDLE_FILE_INFORMATION opened;
	uint64_t observed;
	uint8_t zero[8] = {};

	if (GetFileInformationByHandleEx(child, FileIdInfo,
	    &id_info, sizeof(id_info))) {
		memcpy(&observed, id_info.FileId.Identifier,
			sizeof(observed));
		if (id_info.VolumeSerialNumber == volume &&
		    observed == file_id &&
		    memcmp(id_info.FileId.Identifier + 8, zero, 8) == 0)
			return PG_OK;
	}
	if (!GetFileInformationByHandle(child, &opened)) {
		*native_code = (int)GetLastError();
		return PG_IO;
	}
	if ((opened.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
	    !(opened.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
	    opened.dwVolumeSerialNumber != volume ||
	    (((uint64_t)opened.nFileIndexHigh << 32) |
	     opened.nFileIndexLow) != file_id)
		return PG_RETRY;
	return PG_OK;
}

static pg_status pg_source_loose_scan_windows(pg_source *source,
		HANDLE handle, uint64_t volume, const WCHAR *directory,
		const char *prefix, const char *wanted, int *native_code,
		int *unsupported)
{
	const size_t buffer_size = 65536;
	uint8_t *buffer = (uint8_t *)malloc(buffer_size);
	size_t prefix_length = strlen(prefix);
	size_t scratch_capacity;
	char *next, *canonical;
	pg_status status = PG_OK;
	int first = 1;

	if (!buffer)
		return PG_NOMEM;
	if (prefix_length > SIZE_MAX - 512) {
		free(buffer);
		return PG_LIMIT;
	}
	scratch_capacity = prefix_length + 512;
	next = (char *)malloc(scratch_capacity);
	canonical = (char *)malloc(scratch_capacity);
	if (!next || !canonical) {
		free(next);
		free(canonical);
		free(buffer);
		return PG_NOMEM;
	}
	for (;;) {
		DWORD code;
		size_t offset = 0;

		if (!GetFileInformationByHandleEx(handle,
			first ? FileIdBothDirectoryRestartInfo :
			FileIdBothDirectoryInfo, buffer, buffer_size)) {
			code = GetLastError();
			if (code == ERROR_NO_MORE_FILES ||
			    (first && code == ERROR_FILE_NOT_FOUND))
				break;
			if (first && unsupported &&
			    (code == ERROR_INVALID_PARAMETER ||
			     code == ERROR_INVALID_FUNCTION ||
			     code == ERROR_NOT_SUPPORTED)) {
				*unsupported = 1;
				break;
			}
			*native_code = (int)code;
			status = PG_IO;
			break;
		}
		first = 0;
		for (;;) {
			if (offset > buffer_size -
			    offsetof(FILE_ID_BOTH_DIR_INFO, FileName)) {
				status = PG_CORRUPT;
				goto scan_done;
			}
			FILE_ID_BOTH_DIR_INFO *entry =
				(FILE_ID_BOTH_DIR_INFO *)(buffer + offset);
			size_t span = entry->NextEntryOffset ?
				entry->NextEntryOffset : buffer_size - offset;
			char *name;
			struct stat found = {};
			size_t required = 0;
			int length;

			if (span > buffer_size - offset ||
			    span < offsetof(FILE_ID_BOTH_DIR_INFO, FileName) ||
			    entry->FileNameLength > span -
				offsetof(FILE_ID_BOTH_DIR_INFO, FileName) ||
			    entry->FileNameLength % sizeof(WCHAR)) {
				status = PG_CORRUPT;
				goto scan_done;
			}
			length = WideCharToMultiByte(CP_UTF8,
				WC_ERR_INVALID_CHARS,
				entry->FileName,
				entry->FileNameLength / sizeof(WCHAR),
				NULL, 0, NULL, NULL);
			if (!length) {
				*native_code = (int)GetLastError();
				status = PG_IO;
				goto scan_done;
			}
			if (prefix_length > SIZE_MAX - (size_t)length - 2) {
				status = PG_LIMIT;
				goto scan_done;
			}
			if (prefix_length + (size_t)length + 2 >
			    scratch_capacity) {
				size_t grown_size = prefix_length +
					(size_t)length + 2;
				char *grown = (char *)realloc(next, grown_size);

				if (!grown) {
					status = PG_NOMEM;
					goto scan_done;
				}
				next = grown;
				grown = (char *)realloc(canonical,
					grown_size);
				if (!grown) {
					status = PG_NOMEM;
					goto scan_done;
				}
				canonical = grown;
				scratch_capacity = grown_size;
			}
			memcpy(next, prefix, prefix_length);
			next[prefix_length] = '/';
			name = next + prefix_length + 1;
			if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
			    entry->FileName,
			    entry->FileNameLength / sizeof(WCHAR),
			    name, length, NULL, NULL)) {
				*native_code = (int)GetLastError();
				status = PG_IO;
				goto next_entry;
			}
			name[length] = '\0';
			if (strcmp(name, ".") == 0 ||
			    strcmp(name, "..") == 0 ||
			    (entry->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
				goto next_entry;
			status = pg_name_normalize(source->context, next + 1,
				canonical, scratch_capacity, &required, NULL);
			if (status != PG_OK) {
				status = PG_OK;
				goto next_entry;
			}
			if (!pg_source_loose_wanted(canonical, wanted))
				goto next_entry;
			found.st_dev = volume;
			found.st_ino = (uint64_t)entry->FileId.QuadPart;
			found.st_size = entry->FileAttributes &
				FILE_ATTRIBUTE_DIRECTORY ? 0 :
				entry->EndOfFile.QuadPart;
			found.st_mtime = entry->LastWriteTime.QuadPart /
				10000000LL - 11644473600LL;
			found.st_mtime_nsec = (uint32_t)(
				entry->LastWriteTime.QuadPart %
				10000000LL) * 100u;
			found.st_atime = found.st_mtime;
			found.st_ctime = found.st_mtime;
			found.st_mode = entry->FileAttributes &
				FILE_ATTRIBUTE_DIRECTORY ? _S_IFDIR : _S_IFREG;
			found.st_mode |= _S_IREAD;
			if (!(entry->FileAttributes & FILE_ATTRIBUTE_READONLY))
				found.st_mode |= _S_IWRITE;
			if (S_ISDIR(found.st_mode)) {
				WCHAR *child_path;
				HANDLE child;
				size_t root_length = wcslen(directory);
				size_t name_length =
					entry->FileNameLength / sizeof(WCHAR);
				uint32_t attributes =
					(entry->FileAttributes &
					 FILE_ATTRIBUTE_HIDDEN ?
					 PG_ENTRY_HIDDEN : 0) |
					(entry->FileAttributes &
					 FILE_ATTRIBUTE_SYSTEM ?
					 PG_ENTRY_SYSTEM : 0);

				status = pg_source_loose_add(source, canonical,
					next + 1, &found, attributes);
				if (status != PG_OK)
					goto next_entry;

				if (source->scan_shallow &&
				    (!wanted || pg_descendant_order(canonical,
					wanted) == 0 ||
				     (source->scan_shallow == 2 &&
				      !strcmp(canonical, wanted))))
					goto next_entry;

				if (root_length > SIZE_MAX / sizeof(WCHAR) -
				    name_length - 2) {
					status = PG_LIMIT;
					goto next_entry;
				}
				child_path = (WCHAR *)malloc((root_length +
					name_length + 2) * sizeof(WCHAR));
				if (!child_path) {
					status = PG_NOMEM;
					goto next_entry;
				}
				memcpy(child_path, directory,
					root_length * sizeof(WCHAR));
				child_path[root_length] = L'\\';
				memcpy(child_path + root_length + 1,
					entry->FileName,
					entry->FileNameLength);
				child_path[root_length + name_length + 1] = 0;

				child = CreateFileW(child_path,
					FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
					FILE_SHARE_READ | FILE_SHARE_WRITE |
					FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
					FILE_FLAG_BACKUP_SEMANTICS |
					FILE_FLAG_OPEN_REPARSE_POINT, NULL);

				if (child == INVALID_HANDLE_VALUE) {
					*native_code = (int)GetLastError();
					status = PG_RETRY;
				} else {
					status = pg_source_windows_dir_same(child,
						volume, found.st_ino,
						native_code);
					if (status == PG_OK)
						status = pg_source_loose_scan_windows(source,
							child, volume, child_path,
							next, wanted,
							native_code, NULL);
				}
				if (child != INVALID_HANDLE_VALUE)
					CloseHandle(child);
				free(child_path);
			} else if (!wanted || strcmp(canonical, wanted) == 0 ||
				   (strncmp(canonical, wanted,
				    strlen(wanted)) == 0 &&
				    canonical[strlen(wanted)] == '/')) {
				status = pg_source_loose_add(source, canonical,
					next + 1, &found,
					(entry->FileAttributes &
					 FILE_ATTRIBUTE_HIDDEN ?
					 PG_ENTRY_HIDDEN : 0) |
					(entry->FileAttributes &
					 FILE_ATTRIBUTE_SYSTEM ?
					 PG_ENTRY_SYSTEM : 0));
			}
next_entry:
			if (status != PG_OK)
				goto scan_done;
			if (!entry->NextEntryOffset)
				break;
			offset += entry->NextEntryOffset;
		}
	}
scan_done:
	free(canonical);
	free(next);
	free(buffer);
	return status;
}
#endif

static pg_status pg_source_loose_scan(pg_source *source, int dir,
		const char *prefix, const char *wanted, int *native_code)
{
#ifdef _WIN32
	int unsupported = 0;
	BY_HANDLE_FILE_INFORMATION opened;
	HANDLE root_handle;
	WCHAR *root;
	int length;
	pg_status status;

	length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
		source->native_path, -1, NULL, 0);
	if (!length) {
		*native_code = (int)GetLastError();
		return PG_IO;
	}
	root = (WCHAR *)malloc((size_t)length * sizeof(WCHAR));
	if (!root)
		return PG_NOMEM;
	if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
	    source->native_path, -1, root, length)) {
		*native_code = (int)GetLastError();
		free(root);
		return PG_IO;
	}
	/* A new handle sees additions after an earlier enumeration. */
	root_handle = CreateFileW(root,
		FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS |
		FILE_FLAG_OPEN_REPARSE_POINT, NULL);
	if (root_handle == INVALID_HANDLE_VALUE) {
		*native_code = (int)GetLastError();
		free(root);
		return PG_IO;
	}
	if (!GetFileInformationByHandle(root_handle, &opened)) {
		*native_code = (int)GetLastError();
		CloseHandle(root_handle);
		free(root);
		return PG_IO;
	}
	if ((opened.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
	    !(opened.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
	    opened.dwVolumeSerialNumber != source->identity.st_dev ||
	    (((uint64_t)opened.nFileIndexHigh << 32) |
	     opened.nFileIndexLow) != source->identity.st_ino) {
		CloseHandle(root_handle);
		free(root);
		return PG_RETRY;
	}
	status = pg_source_loose_scan_windows(source,
		root_handle, opened.dwVolumeSerialNumber,
		root, prefix, wanted, native_code, &unsupported);
	CloseHandle(root_handle);
	free(root);

	if (!unsupported)
		return status;
#endif
	return pg_source_loose_scan_fallback(source, dir, prefix,
		wanted, native_code);
}

static int pg_source_in_prefix(const char *name, const char *prefix)
{
	size_t length;

	if (!prefix)
		return 1;
	length = strlen(prefix);
	return strncmp(name, prefix, length) == 0 &&
		(name[length] == '\0' || name[length] == '/');
}

static int pg_source_request_covered(pg_source *source,
		const char *prefix)
{
	pg_source_request *request;

	if (source->loose_root_requested)
		return 1;
	if (!prefix)
		return 0;
	for (request = source->prefix_requests; request;
	     request = request->next) {
		if (pg_source_in_prefix(prefix, request->name))
			return 1;
	}
	return 0;
}

static int pg_source_lookup_covered(pg_source *source, const char *name)
{
	if (pg_source_request_covered(source, name))
		return 1;
	for (pg_source_request *r = source->shallow_requests; r; r = r->next)
		if (pg_name_in_scope(name, r->name, 0))
			return 1;
	return 0;
}

static pg_status pg_source_loose_scan_private(pg_source *source,
		const char *prefix, int *native_code, int tree_request,
		int shallow = 0)
{
	pg_source probe = *source;
	probe.record_index = NULL;
	probe.record_count = 0;
	pg_source_record *old, *next;
	pg_source_record **items = NULL;
	size_t count = 0;
	pg_status status;

	probe.records = NULL;
	probe.scan_shallow = shallow;
	status = source->fd < 0 ? PG_OK :
		pg_source_loose_scan(&probe, source->fd, "", prefix,
			native_code);
	if (status != PG_OK) {
		pg_source_records_free(probe.records);
		return status;
	}
	/* Ancestors traversed to reach the prefix are not new coverage. */
	pg_source_record **at = &probe.records;

	while (*at) {
		pg_source_record *record = *at;
		int keep = shallow == 2 ?
			!strcmp(record->info.canonical_name, prefix) :
			pg_name_in_scope(record->info.canonical_name, prefix,
				!shallow);

		if (keep) {
			at = &record->next;
			continue;
		}
		*at = record->next;
		record->next = NULL;
		pg_source_records_free(record);
	}
	status = pg_source_loose_compact(&probe);
	if (status != PG_OK) {
		pg_source_records_free(probe.records);
		return status;
	}
	if (prefix && !tree_request &&
	    pg_source_name_kind(&probe, prefix) == PG_OK) {
		pg_source_record *record;

		for (record = probe.records; record;
		     record = record->next) {
			if (strcmp(record->info.canonical_name,
				prefix) == 0) {
				pg_source_records_free(probe.records);
				return PG_CONFLICT;
			}
		}
	}
	status = pg_source_record_index(probe.records, &items, &count);
	if (status != PG_OK) {
		pg_source_records_free(probe.records);
		return status;
	}
	for (old = source->records; old; old = old->next) {
		pg_source_record *found;

		if (shallow == 2 ? strcmp(old->info.canonical_name, prefix) :
		    !pg_name_in_scope(old->info.canonical_name, prefix,
			!shallow))
			continue;
		found = pg_source_record_index_find(items, count,
			old->info.canonical_name);
		if (found && old->info.copy_generation == UINT64_MAX &&
		    !pg_native_stat_same(&found->identity,
			&old->identity)) {
			free(items);
			pg_source_records_free(probe.records);
			return PG_LIMIT;
		}
	}
	old = source->records;
	pg_source_index_clear(source);
	source->records = probe.records;
	while (old) {
		pg_source_record *found;

		next = old->next;
		if (shallow == 2 ? strcmp(old->info.canonical_name, prefix) :
		    !pg_name_in_scope(old->info.canonical_name, prefix,
			!shallow)) {
			old->next = source->records;
			source->records = old;
			old = next;
			continue;
		}
		found = pg_source_record_index_find(items, count,
			old->info.canonical_name);
		if (found) {
			found->info.copy_id = old->info.copy_id;
			found->info.copy_generation =
				old->info.copy_generation;
			if (!pg_native_stat_same(&found->identity,
				&old->identity))
				found->info.copy_generation++;
		}
		old->next = NULL;
		pg_source_records_free(old);
		old = next;
	}
	free(items);
	return PG_OK;
}

/* Rebind a managed root after replacement without traversing its files. */
extern "C" pg_status pg_source_rebind_root(pg_source *source,
		int *changed, pg_root_binding **saved, pg_error *error)
{
	struct stat current, opened = {};
	int fd = -1;

	*changed = 0;
	if (lstat(source->native_path, &current)) {
		if (errno != ENOENT && errno != ENOTDIR)
			return pg_native_result(PG_IO, errno, error);
		if (source->fd < 0)
			return PG_OK;
	} else {
		if (!S_ISDIR(current.st_mode))
			return pg_result(PG_CONFLICT, error);
		if (source->fd >= 0 &&
			current.st_dev == source->identity.st_dev &&
		    current.st_ino == source->identity.st_ino)
			return PG_OK;
		fd = open(source->native_path, O_RDONLY | O_DIRECTORY |
			O_NOFOLLOW | O_CLOEXEC);
		if (fd < 0)
			return pg_native_result(PG_IO, errno, error);
		if (fstat(fd, &opened) || !pg_native_stat_same(&current,
			&opened)) {
			close(fd);
			return pg_result(PG_RETRY, error);
		}
	}
	if (source->generation == UINT64_MAX) {
		if (fd >= 0)
			close(fd);
		return pg_result(PG_LIMIT, error);
	}
	pg_root_binding *binding = (pg_root_binding *)malloc(sizeof(*binding));

	if (!binding) {
		if (fd >= 0)
			close(fd);
		return pg_result(PG_NOMEM, error);
	}
	binding->source = source;
	binding->fd = source->fd;
	binding->records = source->records;
	binding->identity = source->identity;
	binding->generation = source->generation;
	binding->next = *saved;
	*saved = binding;
	source->fd = fd;
	if (fd >= 0)
		source->identity = opened;
	source->generation++;
	pg_source_index_clear(source);
	source->records = NULL;
	*changed = 1;
	return PG_OK;
}

/* Keep prior observations available until the whole refresh succeeds. */
extern "C" void pg_source_rebind_finish(pg_root_binding *saved, int commit)
{
	while (saved) {
		pg_root_binding *next = saved->next;
		pg_source *source = saved->source;

		if (commit) {
			if (saved->fd >= 0)
				close(saved->fd);
			pg_source_records_free(saved->records);
		} else {
			if (source->fd >= 0)
				close(source->fd);
			pg_source_index_clear(source);
			pg_source_records_free(source->records);
			source->fd = saved->fd;
			source->records = saved->records;
			source->identity = saved->identity;
			source->generation = saved->generation;
		}
		free(saved);
		saved = next;
	}
}

extern "C" pg_status pg_source_refresh_name(pg_source *source,
		const char *name, int recursive, pg_error *error)
{
	int native_code = 0;
	pg_status status = pg_source_loose_scan_private(source, name,
		&native_code, 1, recursive ? 0 : 2);

	return pg_native_result(status, native_code, error);
}

static pg_status pg_source_loose_validate_dir(int dir, int *native_code)
{
	DIR *listing;
	struct dirent *item;
	int scan = openat(dir, ".", O_RDONLY | O_DIRECTORY |
		O_NOFOLLOW | O_CLOEXEC);
	pg_status status = PG_OK;

	if (scan < 0) {
		*native_code = errno;
		return PG_IO;
	}
	listing = fdopendir(scan);
	if (!listing) {
		*native_code = errno;
		close(scan);
		return PG_IO;
	}
	errno = 0;
	while ((item = readdir(listing))) {
		struct stat before, opened, after;
		int child;

		if (strcmp(item->d_name, ".") == 0 ||
		    strcmp(item->d_name, "..") == 0)
			continue;
		if (fstatat(dir, item->d_name, &before,
			AT_SYMLINK_NOFOLLOW)) {
			*native_code = errno;
			status = errno == ENOENT ? PG_RETRY : PG_IO;
			break;
		}
		if (!S_ISREG(before.st_mode) && !S_ISDIR(before.st_mode))
			continue;
		child = openat(dir, item->d_name, O_RDONLY |
			O_NOFOLLOW | O_CLOEXEC |
			(S_ISDIR(before.st_mode) ? O_DIRECTORY : 0));
		if (child < 0) {
			*native_code = errno;
			status = errno == ENOENT || errno == ELOOP ?
				PG_RETRY : PG_IO;
			break;
		}
		if (fstat(child, &opened) ||
		    fstatat(dir, item->d_name, &after,
			AT_SYMLINK_NOFOLLOW)) {
			*native_code = errno;
			status = PG_IO;
		} else if (!pg_native_stat_same(&before, &opened) ||
			   !pg_native_stat_same(&opened, &after)) {
			status = PG_RETRY;
		} else if (S_ISDIR(before.st_mode)) {
			status = pg_source_loose_validate_dir(child,
				native_code);
		} else {
			uint8_t bytes[65536];
			uint64_t offset = 0;

			while (offset < (uint64_t)before.st_size) {
				size_t amount = (uint64_t)before.st_size - offset <
					sizeof(bytes) ?
					(size_t)((uint64_t)before.st_size -
						offset) : sizeof(bytes);
				ssize_t count = pread(child, bytes, amount,
					(off_t)offset);

				if (count < 0 && errno == EINTR)
					continue;
				if (count <= 0) {
					*native_code = count < 0 ? errno : 0;
					status = count < 0 ? PG_IO : PG_RETRY;
					break;
				}
				offset += (uint64_t)count;
			}
			if (status == PG_OK &&
			    (fstat(child, &after) ||
			     !pg_native_stat_same(&before, &after)))
				status = PG_RETRY;
		}
		close(child);
		if (status != PG_OK)
			break;
		errno = 0;
	}
	if (!item && errno && status == PG_OK) {
		*native_code = errno;
		status = PG_IO;
	}
	closedir(listing);
	return status;
}

static pg_status pg_source_remove_directory(int dir, int *deleted,
		int *native_code)
{
	DIR *listing;
	struct dirent *item;
	int scan = openat(dir, ".", O_RDONLY | O_DIRECTORY |
		O_NOFOLLOW | O_CLOEXEC);
	pg_status status = PG_OK;

	if (scan < 0) {
		*native_code = errno;
		return PG_IO;
	}
	listing = fdopendir(scan);
	if (!listing) {
		*native_code = errno;
		close(scan);
		return PG_IO;
	}
	errno = 0;
	while ((item = readdir(listing))) {
		struct stat before, opened;
		int child;

		if (strcmp(item->d_name, ".") == 0 ||
		    strcmp(item->d_name, "..") == 0)
			continue;
		if (fstatat(dir, item->d_name, &before,
			AT_SYMLINK_NOFOLLOW)) {
			*native_code = errno;
			status = errno == ENOENT ? PG_RETRY : PG_IO;
			break;
		}
		if (S_ISDIR(before.st_mode)) {
			child = openat(dir, item->d_name,
				O_RDONLY | O_DIRECTORY | O_NOFOLLOW |
				O_CLOEXEC);
			if (child < 0 || fstat(child, &opened) ||
			    before.st_dev != opened.st_dev ||
			    before.st_ino != opened.st_ino) {
				*native_code = errno;
				if (child >= 0)
					close(child);
				status = PG_RETRY;
				break;
			}
			status = pg_source_remove_directory(child,
				deleted, native_code);
			close(child);
			if (status != PG_OK)
				break;
			if (unlinkat(dir, item->d_name, AT_REMOVEDIR)) {
				*native_code = errno;
				status = PG_IO;
				break;
			}
		} else if (unlinkat(dir, item->d_name, 0)) {
			*native_code = errno;
			status = PG_IO;
			break;
		}
		*deleted = 1;
		errno = 0;
	}
	if (!item && errno && status == PG_OK) {
		*native_code = errno;
		status = PG_IO;
	}
	closedir(listing);
	return status;
}

pg_status pg_source_retain(pg_source *source)
{
	size_t refs = pg_atomic_load(&source->refs);

	do {
		if (refs == SIZE_MAX)
			return PG_LIMIT;
	} while (!pg_atomic_compare_exchange(&source->refs,
		&refs, refs + 1));
	return PG_OK;
}

pg_status pg_source_release(pg_source *source, int *native_code)
{
	pg_source **at;
	int closed;

	if (pg_atomic_decrement(&source->refs))
		return PG_OK;
	at = &source->context->sources;
	while (*at && *at != source)
		at = &(*at)->next_in_context;
	if (*at)
		*at = source->next_in_context;
	closed = source->fd >= 0 ? close(source->fd) : 0;
	*native_code = closed ? errno : 0;
	pg_context_child_drop(source->context);
	pg_source_index_clear(source);
	pg_source_records_free(source->records);
	pg_source_requests_free(source->exact_requests);
	pg_source_requests_free(source->prefix_requests);
	pg_source_requests_free(source->shallow_requests);
	free(source->native_path);
	pg_source_sync_destroy(source);
	free(source);
	return closed ? PG_IO : PG_OK;
}

static pg_status pg_source_hogg_commit(pg_source *source,
		const uint8_t *frame, uint32_t frame_size,
		pg_error *error)
{
	uint8_t header[24];
	uint8_t length[4];
	uint8_t terminator[4];
	pg_status status;
	int native_code = 0;
	uint32_t op_size;

	status = pg_read_span(source->fd,
		(uint64_t)source->identity.st_size, 0,
		header, sizeof(header));
	if (status != PG_OK)
		return pg_result(status, error);
	op_size = pg_read_u16(header + 6);
	if (pg_read_u32(header) != 0xdeadf00d ||
	    pg_read_u16(header + 4) != 10 ||
	    op_size < 8 || frame_size > op_size - 8)
		return pg_result(PG_CORRUPT, error);
	status = pg_read_span(source->fd,
		(uint64_t)source->identity.st_size, 24,
		length, sizeof(length));
	if (status != PG_OK)
		return pg_result(status, error);
	if (pg_read_u32(length)) {
		uint32_t prior = pg_read_u32(length);
		uint8_t old_end[4];

		if (prior <= op_size - 8) {
			status = pg_read_span(source->fd,
				(uint64_t)source->identity.st_size,
				28 + prior, old_end,
				sizeof(old_end));
			if (status != PG_OK)
				return pg_result(status, error);
			if (pg_read_u32(old_end) == 0xdeabac05) {
				source->recovery_required = 1;
				return pg_result(PG_RECOVERY_REQUIRED,
					error);
			}
		}
		pg_wire_u32(length, 0);
		status = pg_source_write_span(source->fd, 24,
			length, sizeof(length), &native_code);
		if (status != PG_OK)
			return pg_native_result(status, native_code,
				error);
	}
	pg_wire_u32(length, frame_size);
	pg_wire_u32(terminator, 0xdeabac05);
	status = pg_source_write_span(source->fd, 24,
		length, sizeof(length), &native_code);
	if (status == PG_OK)
		status = pg_source_write_span(source->fd, 28,
			frame, frame_size, &native_code);
	if (status == PG_OK)
		status = pg_source_write_span(source->fd,
			28 + frame_size, terminator,
			sizeof(terminator), &native_code);
	if (status == PG_OK && fsync(source->fd)) {
		native_code = errno;
		source->recovery_required = 1;
		pg_native_result(PG_INDETERMINATE, native_code, error);
		if (error)
			error->cause = PG_IO;
		return PG_INDETERMINATE;
	}
	if (status != PG_OK)
		return pg_native_result(status, native_code, error);
	source->recovery_required = 1;
	status = pg_source_recover(source->context, source->native_path, error);
	if (status != PG_OK && status != PG_COMMITTED &&
	    status != PG_RECOVERY_REQUIRED && status != PG_INDETERMINATE) {
		if (error) {
			error->cause = status;
			error->status = PG_RECOVERY_REQUIRED;
		}
		status = PG_RECOVERY_REQUIRED;
	}
	return status;
}

static pg_status pg_source_pigg_copy(FILE *out, int fd,
		uint64_t offset, uint64_t size, uint64_t file_size)
{
	uint8_t bytes[65536];

	while (size) {
		size_t amount = size < sizeof(bytes) ?
			(size_t)size : sizeof(bytes);
		pg_status status = pg_read_span(fd, file_size,
			offset, bytes, amount);

		if (status != PG_OK)
			return status;
		if (pg_wire_write(out, bytes, amount) != PG_OK)
			return PG_IO;
		offset += amount;
		size -= amount;
	}
	return PG_OK;
}

static pg_status pg_source_pigg_clone(pg_source *source,
		pg_builder_entry *entry, FILE *staging,
		uint32_t removed, uint32_t replacement,
		pg_error *error)
{
	struct stat current, path_state, created;
	uint8_t header[16], pool[12];
	uint8_t *archive_header = NULL, *records = NULL;
	uint8_t *names = NULL, *headers = NULL;
	char *temporary = NULL, *leaf = NULL;
	const char *name = entry ? pg_wire_name(entry) : NULL;
	uint64_t table_size, names_at, headers_at, old_base;
	uint64_t new_base, payload_at, extra_names = 0;
	uint64_t extra_headers = 0;
	uint32_t count, new_count, names_count, headers_count;
	uint32_t names_bytes, headers_bytes, target = UINT32_MAX;
	uint32_t i, stride, archive_stride;
	pg_source_record *record;
	pg_source probe = {};
	pg_status status = PG_OK;
	FILE *out = NULL;
	int parent = -1, fd = -1, native_code = 0;
	int stream_fd;
	size_t path_size;
	int published = 0;

	if (pg_atomic_load(&source->live_readers) || source->stale)
		return pg_result(source->stale ? PG_STALE : PG_BUSY, error);
	if (fstat(source->fd, &current) ||
	    lstat(source->native_path, &path_state))
		return pg_native_result(PG_IO, errno, error);
	if (!pg_native_stat_same(&source->identity, &current) ||
	    !pg_native_stat_same(&current, &path_state))
		return pg_result(PG_STALE, error);
	status = pg_read_span(source->fd, current.st_size,
		0, header, sizeof(header));
	if (status != PG_OK)
		goto pigg_done;
	if (pg_read_u32(header) != 0x123 ||
	    pg_read_u16(header + 4) != 2 ||
	    pg_read_u16(header + 6) != 2) {
		status = PG_CORRUPT;
		goto pigg_done;
	}
	archive_stride = pg_read_u16(header + 8);
	stride = pg_read_u16(header + 10);
	count = pg_read_u32(header + 12);
	if (archive_stride < 16 || stride < 48 ||
	    (uint64_t)count * stride > SIZE_MAX ||
	    count == UINT32_MAX) {
		status = PG_LIMIT;
		goto pigg_done;
	}
	table_size = (uint64_t)count * stride;
	names_at = archive_stride + table_size;
	status = pg_read_span(source->fd, current.st_size,
		names_at, pool, sizeof(pool));
	if (status != PG_OK)
		goto pigg_done;
	names_count = pg_read_u32(pool + 4);
	names_bytes = pg_read_u32(pool + 8);
	headers_at = names_at + 12 + names_bytes;
	status = pg_read_span(source->fd, current.st_size,
		headers_at, pool, sizeof(pool));
	if (status != PG_OK)
		goto pigg_done;
	headers_count = pg_read_u32(pool + 4);
	headers_bytes = pg_read_u32(pool + 8);
	old_base = headers_at + 12 + headers_bytes;
	if (old_base > (uint64_t)current.st_size) {
		status = PG_CORRUPT;
		goto pigg_done;
	}
	if (entry)
		target = replacement;
	if (removed != UINT32_MAX) {
		if (removed >= count) {
			status = PG_STALE;
			goto pigg_done;
		}
		new_count = count - 1;
	} else {
		new_count = count + (target == UINT32_MAX);
		if (target == UINT32_MAX)
			extra_names = 4 + strlen(name) + 1;
		else {
			for (record = source->records; record;
			     record = record->next) {
				if (record->info.archive_record == target)
					break;
			}
			if (!record || strcmp(name,
				record->info.original_name) != 0)
				extra_names = 4 + strlen(name) + 1;
		}
		if (entry->cached_header_size)
			extra_headers = 4 + entry->cached_header_size;
	}
	if (names_bytes > UINT32_MAX - extra_names ||
	    headers_bytes > UINT32_MAX - extra_headers ||
	    names_count == UINT32_MAX ||
	    headers_count == UINT32_MAX ||
	    (extra_headers && entry->cached_header_size > UINT32_MAX)) {
		status = PG_LIMIT;
		goto pigg_done;
	}
	new_base = archive_stride + (uint64_t)new_count * stride +
		12 + names_bytes + extra_names +
		12 + headers_bytes + extra_headers;
	if (new_base > UINT32_MAX) {
		status = PG_LIMIT;
		goto pigg_done;
	}
	archive_header = (uint8_t *)malloc(archive_stride);
	records = (uint8_t *)malloc(table_size ? (size_t)table_size : 1);
	names = (uint8_t *)malloc(names_bytes ? names_bytes : 1);
	headers = (uint8_t *)malloc(headers_bytes ? headers_bytes : 1);
	if (!archive_header || !records || !names || !headers) {
		status = PG_NOMEM;
		goto pigg_done;
	}
	status = pg_read_span(source->fd, current.st_size,
		0, archive_header, archive_stride);
	if (status == PG_OK)
		status = pg_read_span(source->fd, current.st_size,
			archive_stride, records, (size_t)table_size);
	if (status == PG_OK)
		status = pg_read_span(source->fd, current.st_size,
			names_at + 12, names, names_bytes);
	if (status == PG_OK)
		status = pg_read_span(source->fd, current.st_size,
			headers_at + 12, headers, headers_bytes);
	if (status != PG_OK)
		goto pigg_done;
	payload_at = new_base;
	for (i = 0; i < count; i++) {
		uint8_t *raw = records + (size_t)i * stride;
		uint32_t logical = pg_read_u32(raw + 8);
		uint32_t compressed = pg_read_u32(raw + 44);
		uint32_t stored = compressed ? compressed : logical;

		if (i == removed)
			continue;
		if (i == target)
			stored = (uint32_t)entry->stored_size;
		if (payload_at > UINT32_MAX - stored) {
			status = PG_LIMIT;
			goto pigg_done;
		}
		payload_at += stored;
	}
	if (entry && target == UINT32_MAX) {
		if (payload_at > UINT32_MAX - entry->stored_size) {
			status = PG_LIMIT;
			goto pigg_done;
		}
		payload_at += entry->stored_size;
	}
	status = pg_native_parent_open(source->native_path, 0,
		&parent, &leaf, &native_code);
	if (status != PG_OK)
		goto pigg_done;
	if (fstatat(parent, leaf, &path_state,
		AT_SYMLINK_NOFOLLOW) ||
	    !pg_native_stat_same(&current, &path_state)) {
		status = PG_STALE;
		goto pigg_done;
	}
	path_size = strlen(source->native_path);

	if (path_size > SIZE_MAX - sizeof(".piggle-XXXXXX")) {
		status = PG_LIMIT;
		goto pigg_done;
	}
	temporary = (char *)malloc(path_size +
		sizeof(".piggle-XXXXXX"));
	if (!temporary) {
		status = PG_NOMEM;
		goto pigg_done;
	}
	memcpy(temporary, source->native_path, path_size);
	memcpy(temporary + path_size, ".piggle-XXXXXX",
		sizeof(".piggle-XXXXXX"));
	fd = mkstemp(temporary);
	if (fd < 0) {
		status = PG_IO;
		native_code = errno;
		goto pigg_done;
	}
	stream_fd = dup(fd);

	if (stream_fd < 0) {
		status = PG_IO;
		native_code = errno;
		goto pigg_done;
	}
	out = fdopen(stream_fd, "wb");
	if (!out) {
		status = PG_IO;
		native_code = errno;
		close(stream_fd);
		goto pigg_done;
	}
	pg_wire_u32(archive_header + 12, new_count);
	if (pg_wire_write(out, archive_header, archive_stride) != PG_OK) {
		status = PG_IO;
		goto pigg_done;
	}
	payload_at = new_base;
	for (i = 0; i < count + (entry && target == UINT32_MAX);
	     i++) {
		uint8_t *raw = i < count ?
			records + (size_t)i * stride : NULL;
		uint8_t *copy = (uint8_t *)malloc(stride);
		uint32_t stored;

		if (!copy) {
			status = PG_NOMEM;
			goto pigg_done;
		}
		if (i == removed) {
			free(copy);
			continue;
		}
		if (raw)
			memcpy(copy, raw, stride);
		else {
			memset(copy, 0, stride);
			pg_wire_u32(copy, 0x3456);
			pg_wire_u32(copy + 4, names_count);
		}
		if (entry && (i == target || !raw)) {
			pg_wire_u32(copy + 8,
				(uint32_t)entry->logical_size);
			pg_wire_u32(copy + 12,
				(uint32_t)entry->mtime);
			if (extra_names)
				pg_wire_u32(copy + 4, names_count);
			pg_wire_u32(copy + 24,
				entry->cached_header_size ?
				headers_count : UINT32_MAX);
			memcpy(copy + 28, entry->digest, 16);
			pg_wire_u32(copy + 44,
				entry->encoding == PG_ZLIB ?
				(uint32_t)entry->stored_size : 0);
			stored = (uint32_t)entry->stored_size;
		} else {
			uint32_t compressed = pg_read_u32(copy + 44);

			stored = compressed ? compressed :
				pg_read_u32(copy + 8);
		}
		pg_wire_u32(copy + 16, (uint32_t)payload_at);
		payload_at += stored;
		status = pg_wire_write(out, copy, stride);
		free(copy);
		if (status != PG_OK)
			goto pigg_done;
	}
	pg_wire_u32(pool, 0x6789);
	pg_wire_u32(pool + 4, names_count + !!extra_names);
	pg_wire_u32(pool + 8, (uint32_t)(names_bytes + extra_names));
	status = pg_wire_write(out, pool, 12);
	if (status == PG_OK)
		status = pg_wire_write(out, names, names_bytes);
	if (status == PG_OK && extra_names) {
		status = pg_wire_u32_write(out,
			(uint32_t)strlen(name) + 1);
		if (status == PG_OK)
			status = pg_wire_write(out, name,
				strlen(name) + 1);
	}
	pg_wire_u32(pool, 0x9abc);
	pg_wire_u32(pool + 4, headers_count + !!extra_headers);
	pg_wire_u32(pool + 8, (uint32_t)(headers_bytes + extra_headers));
	if (status == PG_OK)
		status = pg_wire_write(out, pool, 12);
	if (status == PG_OK)
		status = pg_wire_write(out, headers, headers_bytes);
	if (status == PG_OK && extra_headers) {
		status = pg_wire_u32_write(out,
			(uint32_t)entry->cached_header_size);
		if (status == PG_OK)
			status = pg_wire_write(out, entry->cached_header,
				entry->cached_header_size);
	}
	for (i = 0; status == PG_OK && i < count; i++) {
		uint8_t *raw = records + (size_t)i * stride;
		uint32_t encoded = pg_read_u32(raw + 44);
		uint32_t stored = encoded ? encoded :
			pg_read_u32(raw + 8);

		if (i == removed)
			continue;
		if (i == target)
			status = pg_source_pigg_copy(out,
				fileno(staging),
				entry->stage_offset, entry->stored_size,
				entry->stage_offset + entry->stored_size);
		else
			status = pg_source_pigg_copy(out, source->fd,
				pg_read_u32(raw + 16), stored,
				current.st_size);
	}
	if (status == PG_OK && entry && target == UINT32_MAX)
		status = pg_source_pigg_copy(out,
			fileno(staging),
			entry->stage_offset, entry->stored_size,
			entry->stage_offset + entry->stored_size);
	if (status == PG_OK && fflush(out)) {
		status = PG_IO;
		native_code = errno;
	}
	if (status != PG_OK)
		goto pigg_done;
	if (fsync(fd) || fstat(fd, &created)) {
		status = PG_IO;
		native_code = errno;
		goto pigg_done;
	}
	probe = *source;
	probe.record_index = NULL;
	probe.record_count = 0;
	probe.fd = fd;
	probe.identity = created;
	probe.records = NULL;
	status = pg_pigg_index(&probe);
	if (status != PG_OK)
		goto pigg_done;
	for (record = probe.records; record; record = record->next) {
		status = pg_archive_verify_payload(fd, created.st_size,
			&record->info, record->payload_offset, 0);
		if (status != PG_OK)
			goto pigg_done;
	}
	if (source->generation == UINT64_MAX ||
	    source->context->next_id > UINT64_MAX - new_count) {
		status = PG_LIMIT;
		goto pigg_done;
	}
	if (fstatat(parent, leaf, &path_state,
		AT_SYMLINK_NOFOLLOW) ||
	    !pg_native_stat_same(&current, &path_state)) {
		status = PG_STALE;
		goto pigg_done;
	}
#ifdef _WIN32
	if (fclose(out)) {
		out = NULL;
		status = PG_IO;
		native_code = errno;
		goto pigg_done;
	}
	out = NULL;
	close(fd);
	fd = -1;
	close(source->fd);
	source->fd = -1;
#endif
	if (renameat(parent, pg_native_basename(temporary),
		parent, leaf)) {
		status = PG_IO;
		native_code = errno;
#ifdef _WIN32
		source->fd = open(source->native_path, O_RDWR |
			O_NOFOLLOW | O_CLOEXEC);
		if (source->fd < 0) {
			source->stale = 1;
			status = PG_INDETERMINATE;
		}
#endif
		goto pigg_done;
	}
	published = 1;
	if (source->attached &&
	    source->attached->watch_mode == PG_WATCH_NATIVE)
		source->attached->native_repair = 1;
	for (record = probe.records; record; record = record->next) {
		record->info.source_id = source->id;
		record->info.source_generation =
			source->generation + 1;
		record->info.copy_id = source->context->next_id++;
		record->info.copy_generation = 1;
	}
	source->generation++;
	pg_source_index_clear(source);
	pg_source_records_free(source->records);
	source->records = probe.records;
	probe.records = NULL;
#ifdef _WIN32
	source->fd = open(source->native_path, O_RDWR |
		O_NOFOLLOW | O_CLOEXEC);
	if (source->fd < 0) {
		source->stale = 1;
		status = PG_COMMITTED;
		native_code = errno;
	}
#else
	close(source->fd);
	source->fd = fd;
	fd = -1;
#endif
	source->identity = created;
	if (status == PG_OK && fsync(parent)) {
		status = PG_COMMITTED;
		native_code = errno;
	}

pigg_done:
	if (out && fclose(out) && status == PG_OK) {
		status = published ? PG_COMMITTED : PG_IO;
		native_code = errno;
	}
	if (fd >= 0)
		close(fd);
	if (!published && temporary)
		unlink(temporary);
	if (parent >= 0)
		close(parent);
	free(leaf);
	free(temporary);
	free(archive_header);
	free(records);
	free(names);
	free(headers);
	if (status != PG_OK && status != PG_COMMITTED &&
	    probe.records)
		pg_source_records_free(probe.records);
	pg_native_result(status, native_code, error);
	if (status == PG_COMMITTED && error)
		error->cause = PG_IO;
	return status;
}

pg_status pg_source_pigg_delete(pg_file *file, pg_error *error)
{
	pg_source *source = file->source;
	pg_source_record *record;

	if (source->format != PG_PIGG2)
		return pg_result(PG_INVALID, error);
	if (source->live_writers)
		return pg_result(PG_BUSY, error);
	if (!pg_native_stat_same(&file->source_identity,
		&source->identity))
		return pg_result(PG_STALE, error);
	for (record = source->records; record; record = record->next) {
		if (record->info.copy_id == file->info.copy_id &&
		    record->info.copy_generation ==
			file->info.copy_generation)
			break;
	}
	if (!record)
		return pg_result(PG_STALE, error);
	return pg_source_pigg_clone(source, NULL, NULL,
		(uint32_t)record->info.archive_record, UINT32_MAX, error);
}

static pg_status pg_source_hogg_delete_locked(pg_file *file, pg_error *error)
{
	pg_source *source = file->source;
	pg_source_record *record;
	uint8_t disk_record[32];
	uint8_t header[24];
	uint8_t frame[12] = {};
	struct stat current, path_state;
	uint32_t slot;
	uint32_t ea_slot;
	uint64_t table_base;
	pg_status status;
	int native_code = 0;

	if (source->recovery_required)
		return pg_result(PG_RECOVERY_REQUIRED, error);
	if (source->format != PG_HOGG10)
		return pg_result(PG_INVALID, error);
	if (source->live_writers)
		return pg_result(PG_BUSY, error);
	if (fstat(source->fd, &current) ||
	    lstat(source->native_path, &path_state)) {
		native_code = errno;
		return pg_native_result(PG_IO, native_code, error);
	}
	if (!pg_native_stat_same(&source->identity, &current) ||
	    !pg_native_stat_same(&current, &path_state))
		return pg_result(PG_STALE, error);
	for (record = source->records; record; record = record->next) {
		if (record->info.copy_id == file->info.copy_id &&
		    record->info.copy_generation ==
			file->info.copy_generation)
			break;
	}
	if (!record)
		return pg_result(PG_STALE, error);
	status = pg_read_span(source->fd,
		(uint64_t)current.st_size, 0, header,
		sizeof(header));
	if (status != PG_OK)
		return pg_result(status, error);
	table_base = 24 + pg_read_u16(header + 6) +
		pg_read_u16(header + 20);
	slot = (uint32_t)file->info.archive_record;
	status = pg_read_span(source->fd,
		(uint64_t)current.st_size,
		table_base + 32 * (uint64_t)slot,
		disk_record, sizeof(disk_record));
	if (status != PG_OK)
		return pg_result(status, error);
	ea_slot = pg_read_u16(disk_record + 24) == 0xfffe ?
		pg_read_u32(disk_record + 28) : UINT32_MAX;
	pg_wire_u32(frame, 1);
	pg_wire_u32(frame + 4, slot);
	pg_wire_u32(frame + 8, ea_slot);
	return pg_source_hogg_commit(source, frame,
		sizeof(frame), error);
}

static pg_status pg_source_hogg_compact(pg_source *source,
		pg_error *error)
{
	uint8_t header[24], disk[32], frame[56] = {};
	uint8_t *base = NULL, *journal = NULL, *packed = NULL;
	pg_pool_value *slots = NULL;
	uint64_t table, ea_base, journal_at, metadata_end, offset;
	uint32_t files, eas, dl_file, count = 0, selected, i, at;
	uint64_t packed_size = 8;
	pg_md5 hash;
	uint8_t digest[16];
	pg_status status;
	int native_code = 0;

	status = pg_read_span(source->fd, source->identity.st_size,
		0, header, sizeof(header));
	if (status != PG_OK)
		return pg_result(status, error);
	files = pg_read_u32(header + 8) / 32;
	eas = pg_read_u32(header + 12) / 16;
	dl_file = pg_read_u32(header + 16);
	journal_at = 24 + pg_read_u16(header + 6);
	table = journal_at + pg_read_u16(header + 20);
	ea_base = table + 32 * (uint64_t)files;
	metadata_end = ea_base + 16 * (uint64_t)eas;
	status = pg_read_span(source->fd, source->identity.st_size,
		journal_at, header, 12);
	if (status != PG_OK)
		return pg_result(status, error);
	selected = pg_read_u32(header) ?
		pg_read_u32(header + 8) : pg_read_u32(header + 4);
	status = pg_hogg_data(source, table, ea_base, metadata_end,
		files, eas, dl_file, &base, &slots, &count);
	if (status != PG_OK)
		return pg_result(status, error);
	journal = selected ? (uint8_t *)malloc(selected) : NULL;
	if (selected && !journal) {
		status = PG_NOMEM;
		goto done;
	}
	status = pg_read_span(source->fd, source->identity.st_size,
		journal_at + 12, journal, selected);
	if (status != PG_OK)
		goto done;
	for (at = 0; at < selected;) {
		uint8_t opcode = journal[at++];
		uint32_t slot, size;

		if (selected - at < 4) {
			status = PG_CORRUPT;
			goto done;
		}
		slot = pg_read_u32(journal + at);
		at += 4;
		if (slot > INT32_MAX) {
			status = PG_CORRUPT;
			goto done;
		}
		if (opcode == 1) {
			if (selected - at < 4) {
				status = PG_CORRUPT;
				goto done;
			}
			size = pg_read_u32(journal + at);
			at += 4;
			if (!size || size > selected - at) {
				status = PG_CORRUPT;
				goto done;
			}
			if (slot >= count) {
				pg_pool_value *grown;
				size_t length = (size_t)slot + 1;

				if (length > SIZE_MAX / sizeof(*slots)) {
					status = PG_LIMIT;
					goto done;
				}
				grown = (pg_pool_value *)realloc(slots,
					length * sizeof(*slots));
				if (!grown) {
					status = PG_NOMEM;
					goto done;
				}
				memset(grown + count, 0,
					(length - count) * sizeof(*slots));
				slots = grown;
				count = (uint32_t)length;
			}
			slots[slot].data = journal + at;
			slots[slot].size = size;
			at += size;
		} else if (opcode == 2) {
			if (slot < count) {
				slots[slot].data = NULL;
				slots[slot].size = 0;
			}
		} else {
			status = PG_UNSUPPORTED;
			goto done;
		}
	}
	for (i = 0; i < count; i++) {
		packed_size += 4 + slots[i].size;
		if (packed_size > UINT32_MAX) {
			status = PG_LIMIT;
			goto done;
		}
	}
	packed = (uint8_t *)malloc((size_t)packed_size);
	if (!packed) {
		status = PG_NOMEM;
		goto done;
	}
	pg_wire_u32(packed, 0);
	pg_wire_u32(packed + 4, count);
	at = 8;
	for (i = 0; i < count; i++) {
		pg_wire_u32(packed + at, slots[i].size);
		at += 4;
		if (slots[i].size) {
			memcpy(packed + at, slots[i].data,
				slots[i].size);
			at += slots[i].size;
		}
	}
	status = pg_read_span(source->fd, source->identity.st_size,
		table + 32 * (uint64_t)dl_file, disk, sizeof(disk));
	if (status != PG_OK)
		goto done;
	offset = (uint64_t)source->identity.st_size;
	status = pg_source_write_span(source->fd, offset, packed,
		(size_t)packed_size, &native_code);
	if (status != PG_OK)
		goto done;
	if (fsync(source->fd)) {
		native_code = errno;
		status = PG_IO;
		goto done;
	}
	pg_md5_init(&hash);
	pg_md5_update(&hash, packed, (size_t)packed_size);
	pg_md5_finish(&hash, digest);
	pg_wire_u32(frame, 3);
	pg_wire_u32(frame + 8, dl_file);
	pg_wire_u32(frame + 12, (uint32_t)packed_size);
	memcpy(frame + 16, disk + 12, 4);
	memcpy(frame + 24, disk + 24, 8);
	memcpy(frame + 32, digest, 4);
	pg_wire_u64(frame + 40, offset);
	status = pg_source_hogg_commit(source, frame,
		sizeof(frame), error);
	if (status != PG_OK)
		goto done;
	memset(frame, 0, 16);
	pg_wire_u32(frame, 6);
	pg_wire_u64(frame + 8, journal_at);
	status = pg_source_hogg_commit(source, frame, 16, error);

done:
	free(packed);
	free(journal);
	free(slots);
	free(base);
	if (status != PG_OK && (!error || error->status != status))
		pg_native_result(status, native_code, error);
	return status;
}

static pg_status pg_source_hogg_grow(pg_source *source,
		pg_error *error)
{
	uint8_t header[24], disk[32], frame[32] = {};
	uint8_t bytes[65536];
	uint8_t *new_eas = NULL;
	uint64_t table, old_ea, new_ea, new_end;
	uint32_t files, eas, capacity, new_file_bytes;
	uint32_t new_ea_bytes, i;
	pg_status status;
	int native_code = 0;

	status = pg_read_span(source->fd, source->identity.st_size,
		0, header, sizeof(header));
	if (status != PG_OK)
		return pg_result(status, error);
	files = pg_read_u32(header + 8);
	eas = pg_read_u32(header + 12);
	if (files % 32 || eas % 16 || files / 32 != eas / 16 ||
	    files > UINT32_MAX / 2 || eas > UINT32_MAX / 2)
		return pg_result(PG_LIMIT, error);
	capacity = files / 32;
	if (capacity < 16)
		capacity = 16;
	if (capacity > UINT32_MAX / 64)
		return pg_result(PG_LIMIT, error);
	new_file_bytes = capacity * 64;
	new_ea_bytes = capacity * 32;
	table = 24 + pg_read_u16(header + 6) +
		pg_read_u16(header + 20);
	old_ea = table + files;
	new_ea = table + new_file_bytes;
	new_end = new_ea + new_ea_bytes;
	if (new_ea > UINT32_MAX || old_ea > UINT32_MAX ||
	    new_end > INT64_MAX)
		return pg_result(PG_LIMIT, error);
	for (i = 0; i < files / 32; i++) {
		uint64_t old_offset, new_offset, remaining;
		uint32_t stored;

		status = pg_read_span(source->fd,
			source->identity.st_size,
			table + 32 * (uint64_t)i, disk,
			sizeof(disk));
		if (status != PG_OK)
			return pg_result(status, error);
		stored = pg_read_u32(disk + 8);
		old_offset = pg_read_u64(disk);
		if (stored == UINT32_MAX || !stored ||
		    old_offset >= new_end)
			continue;
		if (old_offset > (uint64_t)source->identity.st_size ||
		    stored > (uint64_t)source->identity.st_size - old_offset)
			return pg_result(PG_CORRUPT, error);
		new_offset = (uint64_t)source->identity.st_size;
		if (new_offset < new_end)
			new_offset = new_end;
		remaining = stored;
		while (remaining) {
			size_t amount = remaining < sizeof(bytes) ?
				(size_t)remaining : sizeof(bytes);

			status = pg_read_span(source->fd,
				source->identity.st_size,
				old_offset + stored - remaining,
				bytes, amount);
			if (status != PG_OK)
				return pg_result(status, error);
			status = pg_source_write_span(source->fd,
				new_offset + stored - remaining,
				bytes, amount, &native_code);
			if (status != PG_OK)
				return pg_native_result(status,
					native_code, error);
			remaining -= amount;
		}
		if (fsync(source->fd))
			return pg_native_result(PG_IO, errno, error);
		pg_wire_u32(frame, 4);
		pg_wire_u32(frame + 8, i);
		pg_wire_u32(frame + 12, stored);
		pg_wire_u64(frame + 16, old_offset);
		pg_wire_u64(frame + 24, new_offset);
		status = pg_source_hogg_commit(source, frame,
			sizeof(frame), error);
		if (status != PG_OK)
			return status;
	}
	new_eas = (uint8_t *)calloc(1, new_ea_bytes);
	if (!new_eas)
		return pg_result(PG_NOMEM, error);
	status = pg_read_span(source->fd, source->identity.st_size,
		old_ea, new_eas, eas);
	if (status != PG_OK)
		goto done_grow;
	for (i = eas; i < new_ea_bytes; i += 16)
		pg_wire_u32(new_eas + i + 12, 1);
	status = pg_source_write_span(source->fd, new_ea,
		new_eas, new_ea_bytes, &native_code);
	if (status != PG_OK)
		goto done_grow;
	if (fsync(source->fd)) {
		native_code = errno;
		status = PG_IO;
		goto done_grow;
	}
	memset(frame, 0, sizeof(frame));
	pg_wire_u32(frame, 5);
	pg_wire_u32(frame + 4, new_file_bytes);
	pg_wire_u32(frame + 8, (uint32_t)old_ea);
	pg_wire_u32(frame + 12, eas);
	pg_wire_u32(frame + 16, (uint32_t)new_ea);
	pg_wire_u32(frame + 20, new_ea_bytes);
	status = pg_source_hogg_commit(source, frame, 28, error);

done_grow:
	free(new_eas);
	if (status != PG_OK && (!error || error->status != status))
		pg_native_result(status, native_code, error);
	return status;
}

static pg_status pg_source_hogg_stage(pg_writer *writer,
		pg_error *error)
{
	pg_source *source = writer->source;
	pg_builder_entry *entry = writer->builder->entries;
	pg_source_record *record;
	struct stat current, path_state;
	uint8_t header[24], disk[32], ea[16];
	uint8_t journal[12], frame[64] = {}, action[9];
	uint64_t table, ea_base, journal_at, payload;
	uint32_t files, eas, dl_file, slot = UINT32_MAX;
	uint32_t ea_slot = UINT32_MAX, name_id = UINT32_MAX;
	uint32_t header_id = UINT32_MAX, data_count = 0;
	uint32_t selected;
	uint64_t action_size = 0;
	int native_code = 0;
	pg_status status;

	/* Reject conflicts before appending payload or changing metadata. */
	if (fstat(source->fd, &current) ||
	    lstat(source->native_path, &path_state))
		return pg_native_result(PG_IO, errno, error);
	if (!pg_native_stat_same(&source->identity, &current) ||
	    !pg_native_stat_same(&current, &path_state))
		return pg_result(PG_STALE, error);
	status = pg_read_span(source->fd, current.st_size, 0,
		header, sizeof(header));
	if (status != PG_OK)
		return pg_result(status, error);
	if (pg_read_u32(header) != 0xdeadf00d ||
	    pg_read_u16(header + 4) != 10)
		return pg_result(PG_CORRUPT, error);
	files = pg_read_u32(header + 8) / 32;
	eas = pg_read_u32(header + 12) / 16;
	dl_file = pg_read_u32(header + 16);
	journal_at = 24 + pg_read_u16(header + 6);
	table = journal_at + pg_read_u16(header + 20);
	ea_base = table + 32 * (uint64_t)files;
	if (dl_file >= files || files != eas)
		return pg_result(PG_CORRUPT, error);
	status = pg_read_span(source->fd, current.st_size,
		journal_at, journal, sizeof(journal));
	if (status != PG_OK)
		return pg_result(status, error);
	selected = pg_read_u32(journal) ?
		pg_read_u32(journal + 8) : pg_read_u32(journal + 4);
	if (selected > (uint32_t)pg_read_u16(header + 20) - 12u)
		return pg_result(PG_CORRUPT, error);
	uint8_t *data_bytes = NULL;
	pg_pool_value *data_slots = NULL;

	status = pg_hogg_data(source, table, ea_base, ea_base + 16 *
		(uint64_t)eas, files, eas, dl_file, &data_bytes, &data_slots,
		&data_count);
	free(data_slots);
	free(data_bytes);
	if (status != PG_OK)
		return pg_result(status, error);
	if (selected) {
		uint8_t *stream = (uint8_t *)malloc(selected);
		uint32_t at = 0;

		if (!stream)
			return pg_result(PG_NOMEM, error);
		status = pg_read_span(source->fd, current.st_size,
			journal_at + 12, stream, selected);
		if (status == PG_OK) {
			while (at < selected) {
				uint32_t id, size;
				uint8_t opcode = stream[at++];

				if (selected - at < 4) {
					status = PG_CORRUPT;
					break;
				}
				id = pg_read_u32(stream + at);
				at += 4;
				if (id == UINT32_MAX) {
					status = PG_CORRUPT;
					break;
				}
				if (id >= data_count)
					data_count = id + 1;
				if (opcode == 2)
					continue;
				if (opcode != 1 || selected - at < 4) {
					status = PG_CORRUPT;
					break;
				}
				size = pg_read_u32(stream + at);
				at += 4;
				if (!size || size > selected - at) {
					status = PG_CORRUPT;
					break;
				}
				at += size;
			}
		}
		free(stream);
		if (status != PG_OK)
			return pg_result(status, error);
	}
	pg_source_record *latest = NULL;

	for (record = source->records; record; record = record->next)
		if (writer->target_copy_id && record->info.copy_id ==
		    writer->target_copy_id)
			latest = record;
	record = latest;
	if (record) {
		slot = (uint32_t)record->info.archive_record;
		status = pg_read_span(source->fd, current.st_size,
			table + 32 * (uint64_t)slot, disk,
			sizeof(disk));
		if (status != PG_OK)
			return pg_result(status, error);
		ea_slot = pg_read_u32(disk + 28);
		if (ea_slot >= eas)
			return pg_result(PG_CORRUPT, error);
		status = pg_read_span(source->fd, current.st_size,
			ea_base + 16 * (uint64_t)ea_slot, ea,
			sizeof(ea));
		if (status != PG_OK)
			return pg_result(status, error);
		name_id = pg_read_u32(ea);
	} else {
		uint32_t i;

		for (i = 0; i < files; i++) {
			status = pg_read_span(source->fd, current.st_size,
				table + 32 * (uint64_t)i, disk,
				sizeof(disk));
			if (status != PG_OK)
				return pg_result(status, error);
			if (i != dl_file &&
			    pg_read_u32(disk + 8) == UINT32_MAX) {
				slot = i;
				break;
			}
		}
		for (i = 0; i < eas; i++) {
			status = pg_read_span(source->fd, current.st_size,
				ea_base + 16 * (uint64_t)i, ea,
				sizeof(ea));
			if (status != PG_OK)
				return pg_result(status, error);
			if (pg_read_u32(ea + 12) & 1) {
				ea_slot = i;
				break;
			}
		}
		if (slot == UINT32_MAX || ea_slot == UINT32_MAX) {
			status = pg_source_hogg_grow(source, error);
			if (status != PG_OK)
				return status;
			return pg_source_hogg_stage(writer, error);
		}
		if (data_count == UINT32_MAX)
			return pg_result(PG_LIMIT, error);
		name_id = data_count++;
	}
	if (entry->cached_header_size && data_count == UINT32_MAX)
		return pg_result(PG_LIMIT, error);
	if (entry->cached_header_size)
		header_id = data_count++;
	if (!record)
		action_size += 9 + strlen(entry->original_name ?
			entry->original_name : entry->canonical_name) + 1;
	if (entry->cached_header_size)
		action_size += 9 + entry->cached_header_size;
	if (action_size > pg_read_u16(header + 20) - 12 - selected) {
		if (!selected || action_size >
		    (uint32_t)pg_read_u16(header + 20) - 12u)
			return pg_result(PG_LIMIT, error);
		status = pg_source_hogg_compact(source, error);
		if (status != PG_OK)
			return status;
		return pg_source_hogg_stage(writer, error);
	}
	if (!writer->metadata_only &&
	    fseeko(writer->builder->staging, 0, SEEK_SET))
		return pg_native_result(PG_IO, errno, error);
	payload = writer->metadata_only ? record->payload_offset :
		(uint64_t)current.st_size;
	uint8_t bytes[65536];
	uint64_t remaining = writer->metadata_only ? 0 : entry->stored_size;

	while (remaining) {
		size_t amount = remaining < sizeof(bytes) ?
			(size_t)remaining : sizeof(bytes);

		if (fread(bytes, 1, amount,
			writer->builder->staging) != amount)
			return pg_native_result(PG_IO, errno, error);
		status = pg_source_write_span(source->fd,
			payload + entry->stored_size - remaining,
			bytes, amount, &native_code);
		if (status != PG_OK)
			return pg_native_result(status, native_code, error);
		remaining -= amount;
	}
	if (fsync(source->fd))
		return pg_native_result(PG_IO, errno, error);
	uint32_t at = selected;

	if (!record) {
		const char *name = entry->original_name ?
			entry->original_name : entry->canonical_name;
		uint32_t size = (uint32_t)strlen(name) + 1;

		action[0] = 1;
		pg_wire_u32(action + 1, name_id);
		pg_wire_u32(action + 5, size);
		status = pg_source_write_span(source->fd,
			journal_at + 12 + at, action, 9,
			&native_code);
		if (status == PG_OK)
			status = pg_source_write_span(source->fd,
				journal_at + 21 + at, name, size,
				&native_code);
		at += 9 + size;
	}
	if (status == PG_OK && entry->cached_header_size) {
		action[0] = 1;
		pg_wire_u32(action + 1, header_id);
		pg_wire_u32(action + 5,
			(uint32_t)entry->cached_header_size);
		status = pg_source_write_span(source->fd,
			journal_at + 12 + at, action, 9,
			&native_code);
		if (status == PG_OK)
			status = pg_source_write_span(source->fd,
				journal_at + 21 + at,
				entry->cached_header,
				entry->cached_header_size,
				&native_code);
		at += 9 + (uint32_t)entry->cached_header_size;
	}
	if (status != PG_OK)
		return pg_native_result(status, native_code, error);
	if (action_size) {
		uint8_t number[4];

		pg_wire_u32(number, 1);
		status = pg_source_write_span(source->fd,
			journal_at, number, 4, &native_code);
		pg_wire_u32(number, at);
		if (status == PG_OK)
			status = pg_source_write_span(source->fd,
				journal_at + 4, number, 4,
				&native_code);
		pg_wire_u32(number, 0);
		if (status == PG_OK)
			status = pg_source_write_span(source->fd,
				journal_at, number, 4, &native_code);
		pg_wire_u32(number, at);
		if (status == PG_OK)
			status = pg_source_write_span(source->fd,
				journal_at + 8, number, 4,
				&native_code);
		if (status != PG_OK)
			return pg_native_result(status, native_code, error);
		if (fsync(source->fd))
			return pg_native_result(PG_IO, errno, error);
	}
	pg_wire_u32(frame, 2);
	pg_wire_u32(frame + 8, slot);
	pg_wire_u32(frame + 12, (uint32_t)entry->stored_size);
	pg_wire_u32(frame + 16, (uint32_t)entry->mtime);
	pg_wire_u16(frame + 24, 0xfffe);
	pg_wire_u32(frame + 28, ea_slot);
	memcpy(frame + 32, entry->digest, 4);
	pg_wire_u64(frame + 40, payload);
	pg_wire_u32(frame + 48, entry->encoding == PG_ZLIB ?
		(uint32_t)entry->logical_size : 0);
	pg_wire_u32(frame + 52, name_id);
	pg_wire_u32(frame + 56, header_id);
	source->changed_record = slot;
	return pg_source_hogg_commit(source, frame, sizeof(frame), error);
}

static pg_status pg_source_archive_finish_locked(pg_writer *writer,
		pg_error *error)
{
	pg_source *source = writer->source;
	struct stat current, path_state;
	pg_source_record *record;

	if (source->recovery_required)
		return pg_result(PG_RECOVERY_REQUIRED, error);
	if (fstat(source->fd, &current) ||
	    lstat(source->native_path, &path_state))
		return pg_native_result(PG_IO, errno, error);
	if (!pg_native_stat_same(&writer->archive_identity, &current) ||
	    !pg_native_stat_same(&current, &path_state))
		return pg_result(PG_STALE, error);
	if (writer->target_copy_id) {
		for (record = source->records; record; record = record->next) {
			if (record->info.copy_id == writer->target_copy_id &&
			    record->info.copy_generation ==
				writer->target_copy_generation &&
			    record->info.archive_record ==
				writer->target_record)
				break;
		}
		if (!record)
			return pg_result(PG_STALE, error);
	}
	if (writer->named_target) {
		pg_status status = pg_source_writer_name_check(source,
			writer->builder->entries->canonical_name);

		if (status != PG_OK)
			return pg_result(status, error);
	}
	if (writer->source->format == PG_HOGG10)
		return pg_source_hogg_stage(writer, error);
	if (writer->source->format == PG_PIGG2)
		return pg_source_pigg_clone(writer->source,
			writer->builder->entries,
			writer->builder->staging, UINT32_MAX,
			(uint32_t)writer->target_record, error);
	return pg_result(PG_INVALID, error);
}

static void pg_source_mutation_refresh(pg_source *source, pg_status status)
{
	/* Unpublished writes can change native identity without changing
	 * copies. */
	if (status != PG_OK && status != PG_STALE && status != PG_BUSY &&
	    !source->recovery_required && source->format == PG_HOGG10) {
		pg_error ignored;

		source->changed_record = UINT64_MAX;
		pg_source_rescan(source, &ignored);
	}
}

pg_status pg_source_hogg_delete(pg_file *file, pg_error *error)
{
	pg_source *source = file->source;

	pg_source_lock(source);
	source->internal_mutation = 1;
	pg_status status = pg_source_hogg_delete_locked(file, error);

	pg_source_mutation_refresh(source, status);
	source->internal_mutation = 0;
	source->changed_record = UINT64_MAX;
	pg_source_unlock(source);
	return status;
}

pg_status pg_source_archive_finish(pg_writer *writer, pg_error *error)
{
	pg_source *source = writer->source;

	pg_source_lock(source);
	source->internal_mutation = 1;
	pg_status status = pg_source_archive_finish_locked(writer, error);

	pg_source_mutation_refresh(source, status);
	source->internal_mutation = 0;
	source->changed_record = UINT64_MAX;
	pg_source_unlock(source);
	return status;
}

static pg_status pg_source_metadata_locked(pg_file *file,
		const pg_metadata_options *options, pg_error *error)
{
	pg_source *source = file->source;
	pg_source_record *record;
	struct stat current, path_state;
	pg_builder_entry entry = {};
	pg_status status;

	if (source->live_writers)
		return pg_result(PG_BUSY, error);
	for (record = source->records; record; record = record->next)
		if (record->info.copy_id == file->info.copy_id &&
		    record->info.copy_generation == file->info.copy_generation)
			break;
	if (!record)
		return pg_result(PG_STALE, error);
	if (fstat(source->fd, &current) ||
	    lstat(source->native_path, &path_state))
		return pg_native_result(PG_IO, errno, error);
	if (!pg_native_stat_same(&source->identity, &current) ||
	    !pg_native_stat_same(&current, &path_state))
		return pg_result(PG_STALE, error);
	entry.canonical_name = (char *)record->info.canonical_name;
	entry.original_name = (char *)record->info.original_name;
	entry.cached_header = (void *)record->info.cached_header;
	entry.cached_header_size = record->info.cached_header_size;
	entry.mtime = record->info.mtime;
	if (options->fields & PG_METADATA_MTIME)
		entry.mtime = options->mtime;
	if ((source->format == PG_HOGG10 &&
	     (entry.mtime < INT32_MIN || entry.mtime > INT32_MAX)) ||
	    (source->format == PG_PIGG2 &&
	     (entry.mtime < 0 || entry.mtime > UINT32_MAX)))
		return pg_result(PG_LIMIT, error);
	if (options->fields & PG_METADATA_HEADER) {
		entry.cached_header = (void *)options->cached_header;
		entry.cached_header_size = options->cached_header_size;
	}
	if (entry.mtime == record->info.mtime &&
	    entry.cached_header_size == record->info.cached_header_size &&
	    (!entry.cached_header_size || !memcmp(entry.cached_header,
		record->info.cached_header, entry.cached_header_size)))
		return pg_result(PG_OK, error);
	entry.logical_size = record->info.logical_size;
	entry.stored_size = record->info.stored_size;
	entry.encoding = record->info.encoding;
	entry.stage_offset = record->payload_offset;
	memcpy(entry.digest, record->info.digest, sizeof(entry.digest));
	if (source->format == PG_PIGG2) {
		int fd = dup(source->fd);
		FILE *staging;

		if (fd < 0)
			return pg_native_result(PG_IO, errno, error);
		staging = fdopen(fd, "rb");
		if (!staging) {
			int code = errno;

			close(fd);
			return pg_native_result(PG_IO, code, error);
		}
		status = pg_source_pigg_clone(source, &entry, staging,
			UINT32_MAX, (uint32_t)record->info.archive_record,
				error);
		if (fclose(staging) && status == PG_OK) {
			pg_native_result(PG_COMMITTED, errno, error);
			if (error)
				error->cause = PG_IO;
			status = PG_COMMITTED;
		}
		return status;
	}
	if (!(options->fields & PG_METADATA_HEADER)) {
		uint8_t header[24], disk[32], frame[56] = {};
		uint32_t slot = (uint32_t)record->info.archive_record;

		status = pg_read_span(source->fd, current.st_size, 0,
			header, sizeof(header));
		if (status != PG_OK)
			return pg_result(status, error);
		uint64_t table = 24 + pg_read_u16(header + 6) +
			pg_read_u16(header + 20);

		status = pg_read_span(source->fd, current.st_size,
			table + 32 * (uint64_t)slot, disk, sizeof(disk));
		if (status != PG_OK)
			return pg_result(status, error);
		pg_wire_u32(frame, 3);
		pg_wire_u32(frame + 8, slot);
		memcpy(frame + 12, disk + 8, 4);
		pg_wire_u32(frame + 16, (uint32_t)entry.mtime);
		memcpy(frame + 24, disk + 24, 8);
		memcpy(frame + 32, disk + 16, 4);
		memcpy(frame + 40, disk, 8);
		source->changed_record = slot;
		return pg_source_hogg_commit(source, frame, sizeof(frame),
			error);
	}
	pg_archive_builder builder = {};
	pg_writer writer = {};

	builder.entries = &entry;
	writer.builder = &builder;
	writer.source = source;
	writer.target_copy_id = record->info.copy_id;
	writer.metadata_only = 1;
	return pg_source_hogg_stage(&writer, error);
}

pg_status pg_source_update_metadata(pg_file *file,
		const pg_metadata_options *options, pg_error *error)
{
	pg_source *source = file->source;

	pg_source_lock(source);
	source->internal_mutation = 1;
	pg_status status = pg_source_metadata_locked(file, options, error);

	pg_source_mutation_refresh(source, status);
	source->internal_mutation = 0;
	source->changed_record = UINT64_MAX;
	pg_source_unlock(source);
	return status;
}

extern "C" {

/* Validate options and resolve native identity. */
/* Detect format; index archive names or prepare lazy loose discovery. */
/* Publish source only after complete setup; unwind on failure. */
PG_API pg_status PG_CALL pg_source_open(
		pg_context *context,
		const char *native_path,
		const pg_source_options *options,
		pg_source **out,
		pg_error *error)
{
	pg_source_options defaults = { PG_AUTO, PG_READ, PG_CHECKSUM_LOGICAL };
	const pg_source_options *chosen = options ? options : &defaults;
	pg_source *source;
	pg_source *other;
	struct stat before;
	struct stat opened;
	uint8_t magic[4];
	pg_status status = PG_OK;
	int native_code = 0;
	int flags = O_RDONLY;
	uint32_t format;

	if (!out)
		return pg_result(PG_INVALID, error);
	*out = NULL;
	if (!context || !native_path || !*native_path ||
	    chosen->format > PG_LOOSE || chosen->access > PG_WRITE ||
	    chosen->checksum_domain > PG_CHECKSUM_STORED)
		return pg_result(PG_INVALID, error);
	if (lstat(native_path, &before)) {
		native_code = errno;
		status = errno == ENOENT || errno == ENOTDIR ?
			PG_NOT_FOUND : PG_IO;
		return pg_native_result(status, native_code, error);
	}
	if (!S_ISREG(before.st_mode) && !S_ISDIR(before.st_mode))
		return pg_result(PG_CONFLICT, error);
	if (chosen->access == PG_WRITE && S_ISREG(before.st_mode))
		flags = O_RDWR;
#ifdef O_NOFOLLOW
	flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
	flags |= O_CLOEXEC;
#endif
	int fd = open(native_path, flags);
	if (fd < 0) {
		native_code = errno;
		status = errno == EACCES || errno == EROFS ?
			PG_READ_ONLY : PG_IO;
		return pg_native_result(status, native_code, error);
	}
	if (fstat(fd, &opened)) {
		native_code = errno;
		close(fd);
		return pg_native_result(PG_IO, native_code, error);
	}
	if (!pg_native_stat_same(&before, &opened)) {
		close(fd);
		return pg_result(PG_RETRY, error);
	}
	for (other = context->sources; other; other = other->next_in_context) {
		if (other->identity.st_dev == opened.st_dev &&
		    other->identity.st_ino == opened.st_ino) {
			close(fd);
			return pg_result(PG_BUSY, error);
		}
	}
	if (S_ISDIR(opened.st_mode)) {
		format = PG_LOOSE;
	} else {
		if (opened.st_size < 4) {
			close(fd);
			return pg_result(PG_UNSUPPORTED, error);
		}
		status = pg_read_span(fd, (uint64_t)opened.st_size,
			0, magic, sizeof(magic));
		if (status != PG_OK) {
			close(fd);
			return pg_result(status, error);
		}
		if (pg_read_u32(magic) == 0x123)
			format = PG_PIGG2;
		else if (pg_read_u32(magic) == 0xdeadf00d)
			format = PG_HOGG10;
		else
			format = PG_AUTO;
	}
	if ((chosen->checksum_domain == PG_CHECKSUM_STORED &&
	     format != PG_HOGG10) || format == PG_AUTO ||
	    (chosen->format != PG_AUTO && chosen->format != format)) {
		close(fd);
		return pg_result(PG_UNSUPPORTED, error);
	}
	if (pg_context_children(context) == SIZE_MAX) {
		close(fd);
		return pg_result(PG_LIMIT, error);
	}
	source = (pg_source *)calloc(1, sizeof(*source));
	if (!source) {
		close(fd);
		return pg_result(PG_NOMEM, error);
	}
	source->fd = fd;
	source->watch_wd = -1;
	source->context = context;
	source->identity = opened;
	source->format = format;
	source->access = chosen->access;
	source->checksum_domain = chosen->checksum_domain;
	source->generation = 1;
	source->changed_record = UINT64_MAX;
	if (format == PG_HOGG10) {
		status = pg_source_sync_create(source);
		if (status != PG_OK)
			goto cleanup;
		if (source->access == PG_WRITE) {
			status = pg_native_writer_lease(fd, &native_code);
			if (status != PG_OK)
				goto cleanup;
		}
	}
	source->native_path = pg_native_absolute(native_path, &status,
		&native_code);
	if (!source->native_path)
		goto cleanup;
	if (format == PG_PIGG2) {
		status = pg_pigg_index(source);
		if (status != PG_OK)
			goto cleanup;
	} else if (format == PG_HOGG10) {
		status = pg_hogg_index(source);
		if (status != PG_OK)
			goto cleanup;
	}
	if (fstat(source->fd, &opened) ||
	    lstat(source->native_path, &before)) {
		native_code = errno;
		status = PG_IO;
		goto cleanup;
	}
	if (!pg_native_stat_same(&source->identity, &opened) ||
	    !pg_native_stat_same(&opened, &before)) {
		status = PG_RETRY;
		goto cleanup;
	}
	if (!context->next_id) {
		status = PG_LIMIT;
		goto cleanup;
	}
	source->id = context->next_id++;
	for (pg_source_record *record = source->records; record;
	     record = record->next) {
		if (!context->next_id) {
			status = PG_LIMIT;
			goto cleanup;
		}
		record->info.source_id = source->id;
		record->info.copy_id = context->next_id++;
		record->info.source_generation = 1;
		record->info.copy_generation = 1;
	}
	source->refs = 1;
	source->next_in_context = context->sources;
	context->sources = source;
	pg_context_child_add(context);
	*out = source;
	return pg_result(PG_OK, error);

cleanup:
	close(source->fd);
	pg_source_index_clear(source);
	pg_source_records_free(source->records);
	free(source->native_path);
	pg_source_sync_destroy(source);
	free(source);
	return pg_native_result(status, native_code, error);
}

/* Validate handle and output slot. */
/* Copy captured metadata and preserve borrowed-span lifetime. */
PG_API pg_status PG_CALL pg_source_inspect(pg_source *source,
		pg_source_info *out, pg_error *error)
{
	if (out)
		memset(out, 0, sizeof(*out));
	if (!source || !out)
		return pg_result(PG_INVALID, error);
	out->id = source->id;
	out->generation = source->generation;
	out->native_path = source->native_path;
	out->format = source->format;
	out->access = source->access;
	out->checksum_domain = source->checksum_domain;
	return pg_result(PG_OK, error);
}

/* Select the exact file and inspect its logical size. */
/* Check capacity before touching caller storage. */
/* Read through verified EOF and return exact byte count. */
PG_API pg_status PG_CALL pg_source_read_all(pg_source *source,
		const char *name, void *buffer, size_t capacity, size_t *bytes,
		pg_error *error)
{
	pg_file *file = NULL;
	pg_error work_error;
	pg_error close_error;
	pg_status status;
	pg_status close_status;

	if (bytes)
		*bytes = 0;
	if (!source || !name || !bytes || (capacity && !buffer))
		return pg_result(PG_INVALID, error);
	status = pg_source_find(source, name, &file, &work_error);
	if (status != PG_OK) {
		if (error)
			*error = work_error;
		return status;
	}
	status = pg_file_read_all(file, buffer, capacity, bytes,
		&work_error);
	close_status = pg_file_close(&file, &close_error);
	if (status == PG_OK) {
		status = close_status;
		work_error = close_error;
	}
	if (error)
		*error = work_error;
	return status;
}

/* Select the exact file and inspect its logical size. */
/* Enforce max_bytes, allocate output and read through verified EOF. */
/* Release private storage on failure; publish only complete bytes. */
PG_API pg_status PG_CALL pg_source_read_all_alloc(pg_source *source,
		const char *name, size_t max_bytes, pg_buffer *out,
		pg_error *error)
{
	pg_file *file = NULL;
	pg_buffer result = { NULL, 0 };
	pg_error work_error;
	pg_error close_error;
	pg_status status;
	pg_status close_status;

	if (!source || !name || !out || out->data || out->size)
		return pg_result(PG_INVALID, error);
	status = pg_source_find(source, name, &file, &work_error);
	if (status != PG_OK) {
		if (error)
			*error = work_error;
		return status;
	}
	status = pg_file_read_all_alloc(file, max_bytes, &result,
		&work_error);
	close_status = pg_file_close(&file, &close_error);
	if (status == PG_OK) {
		status = close_status;
		work_error = close_error;
	}
	if (status == PG_OK)
		*out = result;
	else
		pg_buffer_free(&result);
	if (error)
		*error = work_error;
	return status;
}

/* Stage one complete source file through its writer. */
PG_API pg_status PG_CALL pg_source_write_all(pg_source *destination,
		const char *name, const void *buffer, size_t size,
		const pg_entry_options *entry,
		pg_error *error)
{
	pg_write_options options;
	pg_writer *writer = NULL;
	pg_error work_error, close_error;
	pg_status status, closed;
	size_t accepted = 0;

	if (!destination || !name || (size && !buffer))
		return pg_result(PG_INVALID, error);
	pg_write_options_init(&options, size);
	if (entry)
		options.entry = *entry;
	status = pg_writer_open_source(destination, name, &options,
		&writer, &work_error);
	if (status != PG_OK)
		goto write_done;
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

write_done:
	if (error)
		*error = work_error;
	return status;
}

/* Import one native input through a verified reader and staged writer. */
PG_API pg_status PG_CALL pg_source_import(
		pg_source *destination,
		const char *name,
		const char *native_input,
		uint32_t compression,
		pg_error *error)
{
	pg_reader *reader = NULL;
	pg_reader_info info;
	pg_write_options options;
	pg_status status;

	if (!destination || !name || !native_input || !*native_input ||
	    compression > PG_COMPRESS_FORCE)
		return pg_result(PG_INVALID, error);
	if (destination->access != PG_WRITE)
		return pg_result(PG_READ_ONLY, error);
	status = pg_reader_open_native(destination->context, native_input,
		&reader, error);
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
	return pg_source_transfer_reader(destination, name, &reader,
		&options, error);
}

/* Copy one captured file through a staged source writer. */
PG_API pg_status PG_CALL pg_source_copy(
		pg_source *destination,
		const char *name,
		pg_file *input,
		uint32_t compression,
		pg_error *error)
{
	pg_reader *reader = NULL;
	pg_write_options options;
	pg_status status;
	char *canonical = NULL;
	int stored;

	if (!destination || !name || !input ||
		destination->context != input->source->context ||
		compression > PG_COMPRESS_FORCE)
		return pg_result(PG_INVALID, error);
	if (destination->access != PG_WRITE)
		return pg_result(PG_READ_ONLY, error);
	size_t required = 0;

	status = pg_name_normalize(destination->context, name,
		NULL, 0, &required, NULL);
	if (status != PG_CAPACITY)
		return pg_result(status, error);
	canonical = (char *)malloc(required);
	if (!canonical)
		return pg_result(PG_NOMEM, error);
	status = pg_name_normalize(destination->context, name,
		canonical, required, &required, NULL);
	if (status != PG_OK) {
		free(canonical);
		return pg_result(status, error);
	}
	if (destination == input->source &&
	    strcmp(canonical, input->info.canonical_name) == 0) {
		free(canonical);
		return pg_result(PG_CONFLICT, error);
	}
	free(canonical);
	stored = destination->format != PG_LOOSE &&
		input->info.encoding == PG_ZLIB &&
		(compression == PG_COMPRESS_FORCE ||
		 (compression == PG_COMPRESS_AUTO &&
		  input->info.stored_size < input->info.logical_size));
	status = pg_reader_open(input,
		stored ? PG_READ_STORED : PG_READ_LOGICAL,
		&reader, error);
	if (status != PG_OK)
		return status;
	pg_write_options_init(&options, input->info.logical_size);
	options.entry.mtime = input->info.mtime;
	options.entry.compression = compression;
	if (destination->format != PG_LOOSE) {
		options.entry.cached_header = input->info.cached_header;
		options.entry.cached_header_size =
			input->info.cached_header_size;
	}
	if (stored) {
		uint8_t zero[16] = {};
		size_t digest_size = input->info.digest_kind ==
			PG_DIGEST_MD5 ? 16 : 4;

		options.encoding = PG_ZLIB;
		options.input_size = input->info.stored_size;
		if (input->info.digest_kind != PG_DIGEST_NONE &&
		    memcmp(input->info.digest, zero,
			digest_size) != 0) {
			options.entry.digest_kind =
				input->info.digest_kind;
			options.entry.expected_digest_domain =
				input->info.checksum_domain;
			memcpy(options.entry.expected_digest,
				input->info.digest, digest_size);
		}
	}
	return pg_source_transfer_reader(destination, name, &reader,
		&options, error);
}

/* Export one source-visible selection and preserve its first outcome. */
PG_API pg_status PG_CALL pg_source_export(pg_source *source,
		const char *name, const char *native_output, uint32_t flags, pg_error *error)
{
	pg_file *file = NULL;
	pg_error work_error, close_error;
	pg_status status, closed;

	if (!source || !name || !native_output || !*native_output ||
	    (flags & ~PG_OVERWRITE))
		return pg_result(PG_INVALID, error);
	status = pg_source_find(source, name, &file, &work_error);
	if (status != PG_OK) {
		if (error)
			*error = work_error;
		return status;
	}
	status = pg_file_export(file, native_output, flags,
		&work_error);
	closed = pg_file_close(&file, &close_error);
	if (status == PG_OK && closed != PG_OK) {
		status = PG_COMMITTED;
		pg_result(status, &work_error);
		work_error.cause = PG_IO;
		work_error.native_code = close_error.native_code;
	}
	if (error)
		*error = work_error;
	return status;
}

/* Pack one source-visible snapshot through a private archive builder. */
PG_API pg_status PG_CALL pg_source_pack(
		pg_source *source,
		const char *native_archive,
		uint32_t format,
		const pg_pack_options *options,
		pg_error *error)
{
	pg_pack_options defaults = { PG_COMPRESS_AUTO, 0, PG_CHECKSUM_LOGICAL };
	const pg_pack_options *chosen = options ? options : &defaults;
	pg_cursor *cursor = NULL;

	if (!source || !native_archive || !*native_archive ||
	    (format != PG_PIGG2 && format != PG_HOGG10) ||
	    chosen->compression > PG_COMPRESS_FORCE ||
	    chosen->checksum_domain > PG_CHECKSUM_STORED ||
	    (chosen->flags & ~PG_OVERWRITE))
		return pg_result(PG_INVALID, error);
	if (chosen->checksum_domain == PG_CHECKSUM_STORED &&
	    format != PG_HOGG10)
		return pg_result(PG_UNSUPPORTED, error);
	pg_status status = pg_source_request_subtree(source, NULL, error);

	if (status != PG_OK)
		return status;
	status = pg_source_files(source, NULL, &cursor, error);
	if (status != PG_OK)
		return status;
	return pg_pack_cursor(&cursor, native_archive, format, chosen, error);
}

/* Capture source entries and unpack them under an existing directory. */
PG_API pg_status PG_CALL pg_source_unpack(
		pg_source *source,
		const char *native_directory,
		uint32_t flags,
		pg_error *error)
{
	pg_cursor *cursor = NULL;
	pg_error work_error, close_error;
	pg_status status, closed;

	if (!source || !native_directory || !*native_directory ||
	    (flags & ~PG_OVERWRITE))
		return pg_result(PG_INVALID, error);
	status = pg_source_request_subtree(source, NULL, &work_error);
	if (status != PG_OK) {
		if (error)
			*error = work_error;
		return status;
	}
	status = pg_source_files(source, NULL, &cursor, &work_error);
	if (status != PG_OK) {
		if (error)
			*error = work_error;
		return status;
	}
	status = pg_unpack_cursor(cursor, source->context,
		native_directory, flags, &work_error);
	closed = pg_cursor_close(&cursor, &close_error);
	if (status == PG_OK && closed != PG_OK) {
		status = PG_COMMITTED;
		pg_result(status, &work_error);
		work_error.cause = PG_IO;
		work_error.native_code = close_error.native_code;
	}
	if (error)
		*error = work_error;
	return status;
}

/* Normalize exact name and probe requested loose path. */
/* Resolve indexed archive copies and capture visible winner. */
/* Return a retained selection; unwind on failure. */
pg_status pg_source_select(pg_source *source,
		pg_source_record *selected, pg_file **out, pg_error *error)
{
	pg_file *file = (pg_file *)calloc(1, sizeof(*file));
	pg_status status;

	if (!file)
		return pg_result(PG_NOMEM, error);
	file->info = selected->info;
	file->info.canonical_name = strdup(selected->info.canonical_name);
	file->info.original_name = strdup(selected->info.original_name);
	file->info.cached_header = NULL;
	if (selected->info.cached_header_size) {
		file->info.cached_header = malloc(
			selected->info.cached_header_size);
		if (file->info.cached_header)
			memcpy((void *)file->info.cached_header,
				selected->info.cached_header,
				selected->info.cached_header_size);
	}
	if (selected->native_path)
		file->native_path = strdup(selected->native_path);
	if (!file->info.canonical_name || !file->info.original_name ||
	    (selected->native_path && !file->native_path) ||
	    (selected->info.cached_header_size &&
	     !file->info.cached_header)) {
		free((void *)file->info.canonical_name);
		free((void *)file->info.original_name);
		free((void *)file->info.cached_header);
		free(file->native_path);
		free(file);
		return pg_result(PG_NOMEM, error);
	}
	status = pg_source_retain(source);
	if (status != PG_OK) {
		free((void *)file->info.canonical_name);
		free((void *)file->info.original_name);
		free((void *)file->info.cached_header);
		free(file->native_path);
		free(file);
		return pg_result(status, error);
	}
	file->source = source;
	file->attributes = selected->attributes;
	file->payload_offset = selected->payload_offset;
	file->source_identity = selected->native_path ?
		selected->identity : source->identity;
	*out = file;
	return pg_result(PG_OK, error);
}

static pg_status pg_source_find_mode(
		pg_source *source,
		const char *name,
		pg_file **out,
		pg_error *error, int fresh)
{
	pg_source_record *selected = NULL;
	pg_source_record *transient = NULL;
	char *canonical;
	size_t required = 0;
	pg_status status;
	int native_code = 0;

	if (!out)
		return pg_result(PG_INVALID, error);
	*out = NULL;
	if (!source || !name)
		return pg_result(PG_INVALID, error);
	if (source->stale)
		return pg_result(PG_STALE, error);
	if (source->format == PG_LOOSE && source->fd < 0)
		return pg_result(PG_NOT_FOUND, error);
	status = pg_source_control_status(source, 0);
	if (status != PG_OK)
		return pg_result(status, error);
	status = pg_name_normalize(source->context, name, NULL, 0,
		&required, NULL);
	if (status != PG_CAPACITY)
		return pg_result(status, error);
	canonical = (char *)malloc(required);
	if (!canonical)
		return pg_result(PG_NOMEM, error);
	status = pg_name_normalize(source->context, name, canonical,
		required, &required, NULL);
	if (status != PG_OK) {
		free(canonical);
		return pg_result(status, error);
	}
	if (source->format == PG_LOOSE &&
	    (fresh || !pg_source_lookup_covered(source, canonical))) {
		pg_source probe = *source;
		probe.record_index = NULL;
		probe.record_count = 0;

		if (!fresh) {
			status = pg_source_request_add(
				&source->exact_requests, canonical);
			if (status != PG_OK) {
				free(canonical);
				return pg_result(status, error);
			}
		}
		if (fresh)
			probe.records = NULL;
		status = pg_source_loose_probe(fresh ? &probe : source,
			canonical, &selected,
			&native_code);
		if (status != PG_OK) {
			if (fresh)
				pg_source_records_free(probe.records);
			free(canonical);
			return pg_native_result(status, native_code, error);
		}
		if (fresh)
			transient = probe.records;
	} else {
		pg_source_record **items;
		size_t count;

		status = pg_source_index_get(source, &items, &count);
		if (status != PG_OK) {
			free(canonical);
			return pg_result(status, error);
		}
		if (pg_source_items_directory(items, count, canonical)) {
			free(canonical);
			return pg_result(PG_CONFLICT, error);
		}
		selected = pg_source_index_last(items, count, canonical);
	}
	free(canonical);
	if (!selected)
		return pg_result(PG_NOT_FOUND, error);
	if (S_ISDIR(selected->identity.st_mode)) {
		pg_source_records_free(transient);
		return pg_result(PG_CONFLICT, error);
	}
	status = pg_source_select(source, selected, out, error);
	pg_source_records_free(transient);
	return status;
}

PG_API pg_status PG_CALL pg_source_find(pg_source *source,
		const char *name, pg_file **out, pg_error *error)
{
	return pg_source_find_mode(source, name, out, error, 0);
}

pg_status pg_source_find_fresh(pg_source *source, const char *name,
		pg_file **out, pg_error *error)
{
	return pg_source_find_mode(source, name, out, error, 1);
}

static int pg_source_record_order(const void *left, const void *right)
{
	const pg_source_record *a = *(pg_source_record *const *)left;
	const pg_source_record *b = *(pg_source_record *const *)right;
	int order = strcmp(a->info.canonical_name,
		b->info.canonical_name);

	if (order)
		return order;
	if (a->info.copy_id < b->info.copy_id)
		return -1;
	return a->info.copy_id > b->info.copy_id;
}

pg_status pg_source_name_kind(pg_source *source, const char *name)
{
	pg_status status = PG_NOT_FOUND;
	if (source->record_index) {
		if (pg_source_items_directory(source->record_index,
			source->record_count, name))
			return PG_CONFLICT;
		pg_source_record *r = pg_source_index_last(source->record_index,
			source->record_count, name);

		return !r ? PG_NOT_FOUND : S_ISDIR(r->identity.st_mode) ?
			PG_CONFLICT : PG_OK;
	}

	for (pg_source_record *record = source->records; record;
	     record = record->next) {
		const char *candidate = record->info.canonical_name;

		if (!pg_descendant_order(candidate, name))
			return PG_CONFLICT;
		if (!strcmp(candidate, name) &&
			S_ISDIR(record->identity.st_mode))
			return PG_CONFLICT;
		if (!strcmp(candidate, name))
			status = PG_OK;
	}
	return status;
}

pg_status pg_source_writer_name_check(pg_source *source, const char *name)
{
	if (pg_source_name_kind(source, name) == PG_CONFLICT)
		return PG_CONFLICT;
	for (pg_source_record *record = source->records; record;
	     record = record->next) {
		const char *ancestor = record->info.canonical_name;

		if (!pg_descendant_order(name, ancestor) &&
		    pg_source_name_kind(source, ancestor) == PG_OK)
			return PG_CONFLICT;
	}
	return PG_OK;
}

static int pg_source_items_directory(pg_source_record **items, size_t count,
		const char *name)
{
	size_t low = 0, high = count;

	while (low < high) {
		size_t middle = low + (high - low) / 2;
		int order = pg_descendant_order(
			items[middle]->info.canonical_name, name);

		if (!order)
			return 1;
		if (order < 0)
			low = middle + 1;
		else
			high = middle;
	}
	return 0;
}

static pg_status pg_source_discover_mode(pg_source *source,
		const char *prefix, uint32_t depth, pg_error *error,
		int tree_request)
{
	char *canonical = NULL;
	size_t required = 0;
	pg_status status;
	int native_code = 0;
	pg_source_request *added = NULL;
	pg_source_request **requests;

	if (!source || depth > PG_DISCOVER_RECURSIVE)
		return pg_result(PG_INVALID, error);
	if (source->stale)
		return pg_result(PG_STALE, error);
	status = pg_source_control_status(source, 1);
	if (status != PG_OK)
		return pg_result(status, error);
	if (prefix && *prefix) {
		status = pg_name_normalize(source->context, prefix, NULL, 0,
			&required, NULL);
		if (status != PG_CAPACITY)
			return pg_result(status, error);
		canonical = (char *)malloc(required);
		if (!canonical)
			return pg_result(PG_NOMEM, error);
		status = pg_name_normalize(source->context, prefix, canonical,
			required, &required, NULL);
	}
	requests = depth == PG_DISCOVER_CHILDREN ?
		&source->shallow_requests : &source->prefix_requests;
	if (source->format == PG_LOOSE) {
		pg_source_request *before = *requests;

		if (canonical || depth == PG_DISCOVER_CHILDREN)
			status = pg_source_request_add(requests,
				canonical ? canonical : "");
		if (status == PG_OK && before != *requests)
			added = *requests;
		if (status == PG_OK)
			status = pg_source_loose_scan_private(source, canonical,
				&native_code, tree_request,
				depth == PG_DISCOVER_CHILDREN);
		if (status != PG_OK && added) {
			*requests = added->next;
			added->next = NULL;
			pg_source_requests_free(added);
		}
		if (status == PG_OK && !canonical &&
		    depth == PG_DISCOVER_RECURSIVE)
			source->loose_root_requested = 1;
	} else if (canonical && !tree_request &&
		   pg_source_name_kind(source, canonical) == PG_OK) {
		status = PG_CONFLICT;
	}
	free(canonical);
	return pg_native_result(status, native_code, error);
}

pg_status pg_source_discover(pg_source *source, const char *prefix,
		uint32_t depth, pg_error *error)
{
	return pg_source_discover_mode(source, prefix, depth, error, 0);
}

pg_status pg_source_discover_tree(pg_source *source, const char *prefix,
		uint32_t depth, pg_error *error)
{
	return pg_source_discover_mode(source, prefix, depth, error, 1);
}

PG_API pg_status PG_CALL pg_source_request_subtree(pg_source *source,
		const char *prefix, pg_error *error)
{
	return pg_source_discover(source, prefix, PG_DISCOVER_RECURSIVE, error);
}

pg_status pg_source_request_tree_subtree(pg_source *source,
		const char *prefix, pg_error *error)
{
	return pg_source_discover_tree(source, prefix,
		PG_DISCOVER_RECURSIVE, error);
}

static int pg_source_children_covered(pg_source *source, const char *prefix)
{
	for (pg_source_request *r = source->shallow_requests; r; r = r->next) {
		if (!strcmp(r->name, prefix ? prefix : ""))
			return 1;
	}
	return 0;
}

/* Capture visible source files in canonical order without native probes. */
pg_status pg_source_files_depth(pg_source *source, const char *prefix,
		int recursive, pg_cursor **out, pg_error *error)
{
	pg_source_record **items = NULL;
	pg_cursor *cursor;
	char *canonical = NULL;
	size_t required = 0, count = 0, i;
	pg_status status;

	if (!out)
		return pg_result(PG_INVALID, error);
	*out = NULL;
	if (!source)
		return pg_result(PG_INVALID, error);
	if (source->stale)
		return pg_result(PG_STALE, error);
	status = pg_source_control_status(source, 0);
	if (status != PG_OK)
		return pg_result(status, error);
	if (prefix && *prefix) {
		status = pg_name_normalize(source->context, prefix,
			NULL, 0, &required, NULL);
		if (status != PG_CAPACITY)
			return pg_result(status, error);
		canonical = (char *)malloc(required);
		if (!canonical)
			return pg_result(PG_NOMEM, error);
		status = pg_name_normalize(source->context, prefix,
			canonical, required, &required, NULL);
		if (status != PG_OK)
			goto fail_early;
	}
	if (source->format == PG_LOOSE &&
	    !pg_source_request_covered(source, canonical) &&
	    (recursive || !pg_source_children_covered(source, canonical))) {
		status = PG_INVALID;
		goto fail_early;
	}
	status = pg_source_index_get(source, &items, &count);
	if (status != PG_OK)
		goto fail_early;
	if (canonical && pg_source_name_kind(source, canonical) == PG_OK) {
		status = PG_CONFLICT;
		goto fail_early;
	}
	if (canonical) {
		size_t begin, end;

		pg_source_index_scope(items, count, canonical, &begin, &end);
		items += begin;
		count = end - begin;
	}
	cursor = (pg_cursor *)calloc(1, sizeof(*cursor));
	if (!cursor) {
		status = PG_NOMEM;
		goto fail_early;
	}
	status = pg_source_retain(source);
	if (status != PG_OK) {
		free(cursor);
		goto fail_early;
	}
	cursor->source = source;
	cursor->context = source->context;
	pg_context_child_add(cursor->context);
	cursor->files = (pg_file **)calloc(count ? count : 1,
		sizeof(*cursor->files));
	if (!cursor->files) {
		status = PG_NOMEM;
		goto fail_cursor;
	}
	for (i = 0; i < count;) {
		size_t next = i + 1;
		pg_source_record *selected = items[i];
		const char *name = selected->info.canonical_name;

		while (next < count && strcmp(name,
			items[next]->info.canonical_name) == 0) {
			selected = items[next];
			next++;
		}
		if (pg_name_in_scope(name, canonical, recursive) &&
		    !S_ISDIR(selected->identity.st_mode) &&
		    !pg_source_items_directory(items, count, name)) {
			status = pg_source_select(source, selected,
				&cursor->files[cursor->count], error);
			if (status != PG_OK)
				goto fail_cursor;
			cursor->count++;
		}
		i = next;
	}
	free(canonical);
	*out = cursor;
	return pg_result(PG_OK, error);

fail_cursor:
	pg_cursor_close(&cursor, NULL);
fail_early:
	free(canonical);
	return pg_result(status, error);
}

/* Refresh indexed archives without changing retained selections on failure. */
PG_API pg_status PG_CALL pg_source_files(pg_source *source,
		const char *prefix, pg_cursor **out, pg_error *error)
{
	return pg_source_files_depth(source, prefix, 1, out, error);
}

static pg_status pg_source_rescan_locked(
		pg_source *source,
		pg_error *error, int invalidate = 0)
{
	pg_source probe;
	pg_source_record *record;
	pg_source_record *old;
	struct stat current, path_state, final;
	pg_status status;
	int native_code = 0;
	uint64_t next_id;
	uint64_t generation;

	if (!source)
		return pg_result(PG_INVALID, error);
	if (source->stale)
		return pg_result(PG_STALE, error);
	if (source->recovery_required)
		return pg_result(PG_RECOVERY_REQUIRED, error);
	status = pg_source_control_status(source, 1);
	if (status != PG_OK)
		return pg_result(status, error);
	if (source->format == PG_LOOSE && source->fd < 0) {
		pg_source_index_clear(source);
		pg_source_records_free(source->records);
		source->records = NULL;
		return pg_result(PG_OK, error);
	}
	if (source->format == PG_LOOSE) {
		pg_source_request *request;
		pg_source_record **items = NULL;
		size_t count = 0;

		probe = *source;
		probe.record_index = NULL;
		probe.record_count = 0;
		probe.records = NULL;
		if (source->loose_root_requested) {
			status = pg_source_loose_scan(&probe,
				source->fd, "", NULL, &native_code);
		} else {
			status = PG_OK;
			for (request = source->prefix_requests;
			     request && status == PG_OK;
			     request = request->next)
				status = pg_source_loose_scan(&probe,
					source->fd, "", request->name,
					&native_code);
			probe.scan_shallow = 1;
			for (request = source->shallow_requests;
			     request && status == PG_OK; request =
				request->next)
				status = pg_source_loose_scan(&probe,
					source->fd, "",
					*request->name ? request->name : NULL,
					&native_code);
			probe.scan_shallow = 2;
			for (request = source->exact_requests;
			     request && status == PG_OK;
			     request = request->next)
				status = pg_source_loose_scan(&probe,
					source->fd, "", request->name,
					&native_code);
		}
		if (status != PG_OK) {
			pg_source_records_free(probe.records);
			return pg_native_result(status, native_code,
				error);
		}
		status = pg_source_loose_compact(&probe);
		if (status != PG_OK) {
			pg_source_records_free(probe.records);
			return pg_result(status, error);
		}
		status = pg_source_record_index(source->records,
			&items, &count);
		if (status != PG_OK) {
			pg_source_records_free(probe.records);
			return pg_result(status, error);
		}
		for (record = probe.records; record;
		     record = record->next) {
			pg_source_record *prior;

			prior = pg_source_record_index_find(items, count,
				record->info.canonical_name);
			if (!prior)
				continue;
			record->info.copy_id = prior->info.copy_id;
			record->info.copy_generation =
				prior->info.copy_generation;
			if (!pg_native_stat_same(&record->identity,
				&prior->identity)) {
				if (record->info.copy_generation ==
				    UINT64_MAX) {
					free(items);
					pg_source_records_free(probe.records);
					return pg_result(PG_LIMIT, error);
				}
				record->info.copy_generation++;
			}
		}
		free(items);
		pg_source_index_clear(source);
		pg_source_records_free(source->records);
		source->records = probe.records;
		return pg_result(PG_OK, error);
	}
	if (fstat(source->fd, &current) ||
	    lstat(source->native_path, &path_state)) {
		native_code = errno;
		return pg_native_result(PG_IO, native_code, error);
	}
	if (current.st_dev != source->identity.st_dev ||
	    current.st_ino != source->identity.st_ino ||
	    !pg_native_stat_same(&current, &path_state))
		return pg_result(PG_STALE, error);
	probe = *source;
	probe.record_index = NULL;
	probe.record_count = 0;
	probe.records = NULL;
	probe.identity = current;
	status = source->format == PG_PIGG2 ?
		pg_pigg_index(&probe) : pg_hogg_index(&probe);
	if (status != PG_OK) {
		if (status == PG_RECOVERY_REQUIRED)
			source->recovery_required = 1;
		pg_source_records_free(probe.records);
		return pg_result(status, error);
	}
	if (fstat(source->fd, &final) ||
	    lstat(source->native_path, &path_state)) {
		native_code = errno;
		pg_source_records_free(probe.records);
		return pg_native_result(PG_IO, native_code, error);
	}
	if (!pg_native_stat_same(&current, &final) ||
	    !pg_native_stat_same(&final, &path_state)) {
		pg_source_records_free(probe.records);
		return pg_result(PG_RETRY, error);
	}
	next_id = source->context->next_id;
	generation = source->generation;
	if (invalidate || !pg_native_stat_same(&source->identity, &current)) {
		if (generation == UINT64_MAX) {
			pg_source_records_free(probe.records);
			return pg_result(PG_LIMIT, error);
		}
		generation++;
	}
	for (record = probe.records; record; record = record->next) {
		const pg_file_info *a = &record->info;
		const pg_file_info *b;

		record->info.source_id = source->id;
		record->info.source_generation = generation;
		for (old = source->records; old; old = old->next) {
			if ((source->internal_mutation ?
			     old->info.archive_record ==
				record->info.archive_record :
			     old->payload_offset == record->payload_offset) &&
			    strcmp(old->info.canonical_name,
				a->canonical_name) == 0 &&
			    strcmp(old->info.original_name,
				a->original_name) == 0)
				break;
		}
		if (!old) {
			if (!next_id)
				goto limit;
			record->info.copy_id = next_id++;
			record->info.copy_generation = 1;
			continue;
		}
		b = &old->info;
		record->info.copy_id = b->copy_id;
		record->info.copy_generation = b->copy_generation;
		if (invalidate || (!source->internal_mutation &&
		     !pg_native_stat_same(&source->identity, &current)) ||
		    (source->internal_mutation && a->archive_record ==
		     source->changed_record) ||
		    a->logical_size != b->logical_size ||
		    a->stored_size != b->stored_size ||
		    a->mtime != b->mtime ||
		    a->encoding != b->encoding ||
		    a->cached_header_size != b->cached_header_size ||
		    memcmp(a->digest, b->digest, sizeof(a->digest)) ||
		    (a->cached_header_size && memcmp(a->cached_header,
			b->cached_header, a->cached_header_size))) {
			if (record->info.copy_generation == UINT64_MAX)
				goto limit;
			record->info.copy_generation++;
		}
	}
	pg_source_index_clear(source);
	pg_source_records_free(source->records);
	source->records = probe.records;
	source->identity = current;
	source->generation = generation;
	source->context->next_id = next_id;
	return pg_result(PG_OK, error);

limit:
	pg_source_records_free(probe.records);
	return pg_result(PG_LIMIT, error);
}

/* Validate every physical record, structure and payload. */
/* Return precise corruption, checksum or native failure status. */
static pg_status pg_source_validate_locked(
		pg_source *source,
		pg_error *error)
{
	struct stat current;
	struct stat path_state;
	pg_source_record *record;
	pg_status status;
	int native_code = 0;

	if (!source)
		return pg_result(PG_INVALID, error);
	if (source->stale)
		return pg_result(PG_STALE, error);
	if (source->recovery_required)
		return pg_result(PG_RECOVERY_REQUIRED, error);
	status = pg_source_control_status(source, 0);
	if (status != PG_OK)
		return pg_result(status, error);
	if (source->format == PG_LOOSE) {
		if (fstat(source->fd, &current) ||
		    lstat(source->native_path, &path_state)) {
			native_code = errno;
			return pg_native_result(PG_IO, native_code, error);
		}
		if (current.st_dev != source->identity.st_dev ||
		    current.st_ino != source->identity.st_ino ||
		    current.st_dev != path_state.st_dev ||
		    current.st_ino != path_state.st_ino)
			return pg_result(PG_STALE, error);
		status = pg_source_loose_validate_dir(source->fd,
			&native_code);
		return pg_native_result(status, native_code, error);
	}
	if (fstat(source->fd, &current) ||
	    lstat(source->native_path, &path_state)) {
		native_code = errno;
		return pg_native_result(PG_IO, native_code, error);
	}
	if (!pg_native_stat_same(&source->identity, &current) ||
	    !pg_native_stat_same(&current, &path_state))
		return pg_result(PG_STALE, error);
	if (source->format == PG_HOGG10) {
		uint8_t header[24];
		uint64_t table, ea_base, metadata_end;
		uint32_t files, eas, i;

		status = pg_read_span(source->fd, current.st_size,
			0, header, sizeof(header));
		if (status != PG_OK)
			return pg_result(status, error);
		if (pg_read_u32(header) != 0xdeadf00d ||
		    pg_read_u16(header + 4) != 10 ||
		    pg_read_u32(header + 8) % 32 ||
		    pg_read_u32(header + 12) % 16)
			return pg_result(PG_CORRUPT, error);
		files = pg_read_u32(header + 8) / 32;
		eas = pg_read_u32(header + 12) / 16;
		table = 24 + pg_read_u16(header + 6) +
			pg_read_u16(header + 20);
		ea_base = table + 32 * (uint64_t)files;
		metadata_end = ea_base + 16 * (uint64_t)eas;
		if (metadata_end > (uint64_t)current.st_size)
			return pg_result(PG_CORRUPT, error);
		for (i = 0; i < files; i++) {
			uint8_t disk[32], ea[16];
			pg_file_info info = {};
			uint32_t ea_id;

			status = pg_read_span(source->fd,
				current.st_size,
				table + 32 * (uint64_t)i, disk,
				sizeof(disk));
			if (status != PG_OK)
				return pg_result(status, error);
			info.stored_size = pg_read_u32(disk + 8);
			if (info.stored_size == UINT32_MAX)
				continue;
			info.logical_size = info.stored_size;
			info.encoding = PG_LOGICAL;
			info.digest_kind = PG_DIGEST_MD5_32;
			info.checksum_domain = i == pg_read_u32(header + 16) ?
				(uint32_t)PG_CHECKSUM_LOGICAL :
				source->checksum_domain;
			memcpy(info.digest, disk + 16, 4);
			if (pg_read_u16(disk + 24) == 0xfffe) {
				ea_id = pg_read_u32(disk + 28);
				if (ea_id != UINT32_MAX) {
					if (ea_id >= eas)
						return pg_result(PG_CORRUPT,
							error);
					status = pg_read_span(source->fd,
						current.st_size,
						ea_base + 16 *
						(uint64_t)ea_id,
						ea, sizeof(ea));
					if (status != PG_OK)
						return pg_result(status,
							error);
					if (pg_read_u32(ea + 12) & 1)
						return pg_result(PG_CORRUPT,
							error);
					uint32_t unpacked =
						pg_read_u32(ea + 8);

					if (unpacked) {
						info.encoding = PG_ZLIB;
						info.logical_size = unpacked;
					}
				}
			}
			uint64_t offset = pg_read_u64(disk);

			if (info.stored_size && offset < metadata_end)
				return pg_result(PG_CORRUPT, error);
			status = pg_archive_verify_payload(source->fd,
				current.st_size, &info, offset, 0);
			if (status != PG_OK)
				return pg_result(status, error);
		}
		return pg_result(PG_OK, error);
	}
	for (record = source->records; record; record = record->next) {
		status = pg_archive_verify_payload(source->fd,
			(uint64_t)source->identity.st_size,
			&record->info, record->payload_offset, 0);
		if (status != PG_OK)
			return pg_result(status, error);
	}
	return pg_result(PG_OK, error);
}

/* Replay a committed HOGG journal before exposing its updated index. */
PG_API pg_status PG_CALL pg_source_recover(
		pg_context *context,
		const char *native_path,
		pg_error *error)
{
	pg_source *source = NULL;
	uint8_t header[24];
	uint8_t frame[72];
	uint8_t record[32] = {};
	uint8_t zero[8] = {};
	struct stat path_state, opened;
	uint64_t table_base, ea_base, metadata_end, file_size;
	uint32_t op_size, dl_size, file_bytes, ea_bytes, frame_size;
	uint32_t action, slot;
	pg_status status = PG_OK;
	int native_code = 0;
	int replay_started = 0;
	int journal_cleared = 0;
	int fd;

	if (!context || !native_path || !*native_path)
		return pg_result(PG_INVALID, error);
	if (lstat(native_path, &path_state)) {
		native_code = errno;
		return pg_native_result(errno == ENOENT ? PG_NOT_FOUND :
			PG_IO, native_code, error);
	}
	if (!S_ISREG(path_state.st_mode))
		return pg_result(PG_CONFLICT, error);
	fd = open(native_path, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) {
		native_code = errno;
		return pg_native_result(errno == EACCES || errno == EROFS ?
			PG_READ_ONLY : PG_IO, native_code, error);
	}
	if (fstat(fd, &opened)) {
		native_code = errno;
		status = PG_IO;
		goto cleanup_recover;
	}
	if (!pg_native_stat_same(&path_state, &opened)) {
		status = PG_RETRY;
		goto cleanup_recover;
	}
	for (source = context->sources; source;
	     source = source->next_in_context) {
		if (source->identity.st_dev == opened.st_dev &&
		    source->identity.st_ino == opened.st_ino)
			break;
	}
	if (source)
		pg_source_lock(source);
	if (source && !source->internal_mutation && source->live_writers) {
		status = PG_BUSY;
		goto cleanup_recover;
	}
	if (!source || source->access != PG_WRITE) {
		status = pg_native_writer_lease(fd, &native_code);
		if (status != PG_OK)
			goto cleanup_recover;
	}
	file_size = (uint64_t)opened.st_size;
	status = pg_read_span(fd, file_size, 0, header, sizeof(header));
	if (status != PG_OK)
		goto cleanup_recover;
	if (pg_read_u32(header) != 0xdeadf00d ||
	    pg_read_u16(header + 4) != 10) {
		status = PG_UNSUPPORTED;
		goto cleanup_recover;
	}
	op_size = pg_read_u16(header + 6);
	file_bytes = pg_read_u32(header + 8);
	ea_bytes = pg_read_u32(header + 12);
	dl_size = pg_read_u16(header + 20);
	table_base = 24 + op_size + dl_size;
	ea_base = table_base + file_bytes;
	metadata_end = ea_base + ea_bytes;
	if (op_size < 8 || dl_size < 12 ||
	    table_base > file_size) {
		status = PG_CORRUPT;
		goto cleanup_recover;
	}
	status = pg_read_span(fd, file_size, 24, frame, 4);
	if (status != PG_OK)
		goto cleanup_recover;
	frame_size = pg_read_u32(frame);
	if (!frame_size || frame_size > op_size - 8) {
		status = file_bytes % 32 || ea_bytes % 16 ||
			metadata_end > file_size ? PG_CORRUPT : PG_OK;
		goto cleanup_recover;
	}
	status = pg_read_span(fd, file_size, 24 + 4 + frame_size,
		frame, 4);
	if (status != PG_OK)
		goto cleanup_recover;
	if (pg_read_u32(frame) != 0xdeabac05) {
		status = file_bytes % 32 || ea_bytes % 16 ||
			metadata_end > file_size ? PG_CORRUPT : PG_OK;
		goto cleanup_recover;
	}
	if (source)
		source->recovery_required = 1;
	if (frame_size > sizeof(frame)) {
		status = PG_UNSUPPORTED;
		goto cleanup_recover;
	}
	status = pg_read_span(fd, file_size, 28, frame, frame_size);
	if (status != PG_OK)
		goto cleanup_recover;
	action = pg_read_u32(frame);
	if (action != 5 && (file_bytes % 32 || ea_bytes % 16 ||
	    metadata_end > file_size)) {
		status = PG_CORRUPT;
		goto cleanup_recover;
	}
	if (action == 1 && frame_size == 12) {
		int32_t ea_slot = (int32_t)pg_read_u32(frame + 8);
		uint8_t ea[16] = {};

		slot = pg_read_u32(frame + 4);
		if (slot >= file_bytes / 32 ||
		    (ea_slot != -1 &&
		     ((uint32_t)ea_slot >= ea_bytes / 16))) {
			status = PG_CORRUPT;
			goto cleanup_recover;
		}
		memset(record + 8, 0xff, 4);
		replay_started = 1;
		status = pg_source_write_span(fd, table_base + 32 * slot,
			record, sizeof(record), &native_code);
		if (status != PG_OK)
			goto cleanup_recover;
		if (ea_slot != -1) {
			ea[12] = 1;
			status = pg_source_write_span(fd,
				ea_base + 16 * (uint32_t)ea_slot,
				ea, sizeof(ea), &native_code);
			if (status != PG_OK)
				goto cleanup_recover;
		}
	} else if (action == 2 &&
		   (frame_size == 64 || frame_size == 72)) {
		uint64_t payload = pg_read_u64(frame + 40);
		uint32_t stored = pg_read_u32(frame + 12);
		uint32_t ea_slot = pg_read_u32(frame + 28);
		uint8_t ea[16] = {};

		slot = pg_read_u32(frame + 8);
		if (slot >= file_bytes / 32 || stored == UINT32_MAX ||
		    payload > file_size || stored > file_size - payload ||
		    (stored && payload < metadata_end) ||
		    (pg_read_u16(frame + 24) == 0xfffe &&
		     ea_slot != UINT32_MAX && ea_slot >= ea_bytes / 16)) {
			status = PG_CORRUPT;
			goto cleanup_recover;
		}
		memcpy(record, frame + 40, 8);
		memcpy(record + 8, frame + 12, 4);
		memcpy(record + 12, frame + 16, 4);
		memcpy(record + 16, frame + 32, 4);
		memcpy(record + 24, frame + 24, 8);
		replay_started = 1;
		status = pg_source_write_span(fd, table_base + 32 * slot,
			record, sizeof(record), &native_code);
		if (status != PG_OK)
			goto cleanup_recover;
		if (pg_read_u16(frame + 24) == 0xfffe &&
		    ea_slot != UINT32_MAX) {
			memcpy(ea, frame + 52, 4);
			memcpy(ea + 4, frame + 56, 4);
			memcpy(ea + 8, frame + 48, 4);
			status = pg_source_write_span(fd,
				ea_base + 16 * ea_slot, ea,
				sizeof(ea), &native_code);
			if (status != PG_OK)
				goto cleanup_recover;
		}
	} else if (action == 3 && frame_size == 56) {
		uint64_t payload = pg_read_u64(frame + 40);
		uint32_t stored = pg_read_u32(frame + 12);

		slot = pg_read_u32(frame + 8);
		if (slot >= file_bytes / 32 || stored == UINT32_MAX ||
		    payload > file_size || stored > file_size - payload ||
		    (stored && payload < metadata_end) ||
		    (pg_read_u16(frame + 24) == 0xfffe &&
		     pg_read_u32(frame + 28) != UINT32_MAX &&
		     pg_read_u32(frame + 28) >= ea_bytes / 16)) {
			status = PG_CORRUPT;
			goto cleanup_recover;
		}
		memcpy(record, frame + 40, 8);
		memcpy(record + 8, frame + 12, 4);
		memcpy(record + 12, frame + 16, 4);
		memcpy(record + 16, frame + 32, 4);
		memcpy(record + 24, frame + 24, 8);
		replay_started = 1;
		status = pg_source_write_span(fd, table_base + 32 * slot,
			record, sizeof(record), &native_code);
		if (status != PG_OK)
			goto cleanup_recover;
	} else if (action == 4 && frame_size == 32) {
		uint32_t stored = pg_read_u32(frame + 12);
		uint64_t old_offset = pg_read_u64(frame + 16);
		uint64_t new_offset = pg_read_u64(frame + 24);

		slot = pg_read_u32(frame + 8);
		if (slot >= file_bytes / 32 || stored == UINT32_MAX ||
		    new_offset > file_size ||
		    stored > file_size - new_offset ||
		    (stored && new_offset < metadata_end)) {
			status = PG_CORRUPT;
			goto cleanup_recover;
		}
		status = pg_read_span(fd, file_size,
			table_base + 32 * slot, record,
			sizeof(record));
		if (status != PG_OK)
			goto cleanup_recover;
		if (pg_read_u32(record + 8) != stored ||
		    (pg_read_u64(record) != old_offset &&
		     pg_read_u64(record) != new_offset)) {
			status = PG_CORRUPT;
			goto cleanup_recover;
		}
		replay_started = 1;
		status = pg_source_write_span(fd,
			table_base + 32 * slot, frame + 24,
			8, &native_code);
		if (status != PG_OK)
			goto cleanup_recover;
	} else if (action == 5 &&
		   (frame_size == 28 || frame_size == 32)) {
		uint32_t new_file_bytes = pg_read_u32(frame + 4);
		uint32_t old_ea_offset = pg_read_u32(frame + 8);
		uint32_t old_ea_bytes = pg_read_u32(frame + 12);
		uint32_t new_ea_offset = pg_read_u32(frame + 16);
		uint32_t new_ea_bytes = pg_read_u32(frame + 20);
		uint32_t old_file_bytes;
		uint8_t free_record[32] = {};
		uint64_t at;

		if (old_ea_offset < table_base ||
		    old_ea_offset - table_base > UINT32_MAX ||
		    (old_ea_offset - table_base) % 32 ||
		    new_file_bytes % 32 ||
		    old_ea_bytes % 16 || new_ea_bytes % 16 ||
		    new_ea_offset != table_base + new_file_bytes ||
		    new_ea_bytes < old_ea_bytes ||
		    old_ea_offset > file_size ||
		    old_ea_bytes > file_size - old_ea_offset ||
		    new_ea_offset > file_size ||
		    new_ea_bytes > file_size - new_ea_offset) {
			status = PG_CORRUPT;
			goto cleanup_recover;
		}
		old_file_bytes = (uint32_t)(old_ea_offset -
			table_base);
		if (new_file_bytes < old_file_bytes ||
		    !pg_source_field_mix(file_bytes,
			old_file_bytes, new_file_bytes) ||
		    !pg_source_field_mix(ea_bytes,
			old_ea_bytes, new_ea_bytes)) {
			status = PG_CORRUPT;
			goto cleanup_recover;
		}
		memset(free_record + 8, 0xff, 4);
		replay_started = 1;
		for (at = old_ea_offset; at < new_ea_offset;
		     at += sizeof(free_record)) {
			status = pg_source_write_span(fd, at,
				free_record, sizeof(free_record),
				&native_code);
			if (status != PG_OK)
				goto cleanup_recover;
		}
		status = pg_source_write_span(fd, 8, frame + 4,
			4, &native_code);
		if (status != PG_OK)
			goto cleanup_recover;
		status = pg_source_write_span(fd, 12, frame + 20,
			4, &native_code);
		if (status != PG_OK)
			goto cleanup_recover;
	} else if (action == 6 && frame_size == 16) {
		uint64_t offset = pg_read_u64(frame + 8);
		uint8_t inuse[4] = { 1, 0, 0, 0 };

		if (offset != 24 + op_size) {
			status = PG_CORRUPT;
			goto cleanup_recover;
		}
		replay_started = 1;
		status = pg_source_write_span(fd, offset, inuse, 4,
			&native_code);
		if (status != PG_OK)
			goto cleanup_recover;
		status = pg_source_write_span(fd, offset + 8, zero, 4,
			&native_code);
		if (status != PG_OK)
			goto cleanup_recover;
		status = pg_source_write_span(fd, offset, zero, 8,
			&native_code);
		if (status != PG_OK)
			goto cleanup_recover;
	} else {
		status = action >= 1 && action <= 6 ?
			PG_CORRUPT : PG_UNSUPPORTED;
		goto cleanup_recover;
	}
	if (fsync(fd)) {
		native_code = errno;
		status = PG_IO;
		goto cleanup_recover;
	}
	status = pg_source_write_span(fd, 28 + frame_size, zero, 4,
		&native_code);
	if (status != PG_OK)
		goto cleanup_recover;
	journal_cleared = 1;
	if (fsync(fd)) {
		native_code = errno;
		status = PG_IO;
		goto cleanup_recover;
	}
	status = pg_source_write_span(fd, 24, zero, 4, &native_code);
	if (status != PG_OK)
		goto cleanup_recover;
	if (fsync(fd)) {
		native_code = errno;
		status = PG_IO;
		goto cleanup_recover;
	}
cleanup_recover:
	if (status == PG_OK && source && source->recovery_required) {
		if (fsync(fd)) {
			native_code = errno;
			status = PG_IO;
		} else {
			source->recovery_required = 0;
			status = pg_source_rescan_locked(source, NULL,
				!source->internal_mutation);
			source->recovery_required = 1;
		}
	}
	if (close(fd) && status == PG_OK) {
		native_code = errno;
		status = PG_IO;
	}
	if (source && status == PG_OK)
		source->recovery_required = 0;
	pg_status cause = status;

	if (status != PG_OK && replay_started) {
		status = journal_cleared ? PG_COMMITTED :
			PG_RECOVERY_REQUIRED;
	}
	pg_native_result(status, native_code, error);
	if ((status == PG_RECOVERY_REQUIRED ||
	     status == PG_COMMITTED) && error)
		error->cause = cause;
	pg_source_unlock(source);
	return status;
}

/* Remove a detached writable source without following native links. */
PG_API pg_status PG_CALL pg_source_delete(
		pg_source *source,
		pg_error *error)
{
	struct stat current;
	char *leaf = NULL;
	int parent = -1;
	int native_code = 0;
	int deleted = 0;
	pg_status status;
	pg_status cause;

	if (!source)
		return pg_result(PG_INVALID, error);
	if (source->recovery_required)
		return pg_result(PG_RECOVERY_REQUIRED, error);
	status = pg_source_control_status(source, 1);
	if (status != PG_OK)
		return pg_result(status, error);
	if (source->access != PG_WRITE)
		return pg_result(PG_READ_ONLY, error);
	if (source->attached ||
	    pg_atomic_load(&source->live_readers) ||
	    source->live_writers)
		return pg_result(PG_BUSY, error);
	if (source->stale)
		return pg_result(PG_STALE, error);
	status = pg_native_parent_open(source->native_path, 0,
		&parent, &leaf, &native_code);
	if (status != PG_OK)
		return pg_native_result(status, native_code, error);
	if (fstatat(parent, leaf, &current, AT_SYMLINK_NOFOLLOW)) {
		native_code = errno;
		status = errno == ENOENT ? PG_STALE : PG_IO;
		goto delete_done;
	}
	if (current.st_dev != source->identity.st_dev ||
	    current.st_ino != source->identity.st_ino ||
	    (source->format == PG_LOOSE &&
	     !S_ISDIR(current.st_mode)) ||
	    (source->format != PG_LOOSE &&
	     !S_ISREG(current.st_mode))) {
		status = PG_STALE;
		goto delete_done;
	}
	if (source->format == PG_LOOSE) {
		status = pg_source_remove_directory(source->fd,
			&deleted, &native_code);
		if (status != PG_OK)
			goto delete_done;
	}
	if (unlinkat(parent, leaf,
		source->format == PG_LOOSE ? AT_REMOVEDIR : 0)) {
		native_code = errno;
		status = PG_IO;
		goto delete_done;
	}
	deleted = 1;
	source->stale = 1;
	/* Windows completes deletion when the last native handle closes. */
	if (close(source->fd)) {
		source->fd = -1;
		native_code = errno;
		status = PG_IO;
		goto delete_done;
	}
	source->fd = -1;
	if (fsync(parent)) {
		native_code = errno;
		status = PG_IO;
		goto delete_done;
	}
	status = PG_OK;

delete_done:
	close(parent);
	free(leaf);
	if (deleted)
		source->stale = 1;
	if (deleted && status != PG_OK) {
		cause = status;
		status = PG_PARTIAL;
		pg_native_result(status, native_code, error);
		if (error)
			error->cause = cause;
		return status;
	}
	return pg_native_result(status, native_code, error);
}

/* Accept a null owned handle as a no-op. */
/* Check close constraints; release resources and references. */
/* Clear the owned pointer once close is accepted. */
PG_API pg_status PG_CALL pg_source_close(
		pg_source **source,
		pg_error *error)
{
	pg_source *owned;
	pg_status status;
	int native_code = 0;

	if (!source)
		return pg_result(PG_INVALID, error);
	if (!*source)
		return pg_result(PG_OK, error);
	owned = *source;
	pg_status control = pg_source_control_status(owned, 1);

	if (control != PG_OK)
		return pg_result(control, error);
	*source = NULL;
	status = pg_source_release(owned, &native_code);
	return pg_native_result(status, native_code, error);
}

} /* extern "C" */

pg_status pg_source_rescan(pg_source *source, pg_error *error)
{
	pg_source_lock(source);
	pg_status status = pg_source_rescan_locked(source, error);

	pg_source_unlock(source);
	return status;
}

pg_status pg_source_validate(pg_source *source, pg_error *error)
{
	pg_source_lock(source);
	pg_status status = pg_source_validate_locked(source, error);

	pg_source_unlock(source);
	return status;
}
