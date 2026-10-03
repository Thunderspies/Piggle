#ifndef PIGGLE_INTERNAL_HPP
#define PIGGLE_INTERNAL_HPP

#include <piggle/types.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include "win_posix.hpp"
#else
#include <unistd.h>
#include <pthread.h>
#endif

static inline size_t pg_atomic_load(const size_t *value)
{
#ifdef _WIN64
	return (size_t)InterlockedCompareExchange64(
		(volatile LONG64 *)value, 0, 0);
#elif defined(_WIN32)
	return (size_t)InterlockedCompareExchange(
		(volatile LONG *)value, 0, 0);
#else
	return __atomic_load_n(value, __ATOMIC_ACQUIRE);
#endif
}

static inline void pg_atomic_increment(size_t *value)
{
#ifdef _WIN64
	InterlockedIncrement64((volatile LONG64 *)value);
#elif defined(_WIN32)
	InterlockedIncrement((volatile LONG *)value);
#else
	__atomic_add_fetch(value, 1, __ATOMIC_ACQ_REL);
#endif
}

static inline size_t pg_atomic_decrement(size_t *value)
{
#ifdef _WIN64
	return (size_t)InterlockedDecrement64((volatile LONG64 *)value);
#elif defined(_WIN32)
	return (size_t)InterlockedDecrement((volatile LONG *)value);
#else
	return __atomic_sub_fetch(value, 1, __ATOMIC_ACQ_REL);
#endif
}

static inline int pg_atomic_compare_exchange(size_t *value,
		size_t *expected, size_t desired)
{
#ifdef _WIN32
	size_t before = *expected;
#ifdef _WIN64
	size_t found = (size_t)InterlockedCompareExchange64(
		(volatile LONG64 *)value, (LONG64)desired, (LONG64)before);
#else
	size_t found = (size_t)InterlockedCompareExchange(
		(volatile LONG *)value, (LONG)desired, (LONG)before);
#endif
	*expected = found;
	return found == before;
#else
	return __atomic_compare_exchange_n(value, expected, desired, 0,
		__ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
#endif
}

struct pg_context {
	uint64_t next_id;
	size_t children;
	pg_source *sources;
#ifdef _WIN32
	HANDLE record_heap;
	CRITICAL_SECTION coordination;
#else
	pthread_mutex_t coordination;
#endif
};

/* Coordinate cached views and references before taking source locks. */
pg_status pg_context_sync_create(pg_context *context);
void pg_context_sync_destroy(pg_context *context);
void pg_context_lock(pg_context *context);
void pg_context_unlock(pg_context *context);

static inline size_t pg_context_children(const pg_context *context)
{
	return pg_atomic_load(&context->children);
}

static inline void pg_context_child_add(pg_context *context)
{
	pg_atomic_increment(&context->children);
}

static inline void pg_context_child_drop(pg_context *context)
{
	pg_atomic_decrement(&context->children);
}

/* Loose records need file identity and nanosecond precision, not a second
 * copy of size/mtime or unused Windows stat fields for every indexed name.
 */
#ifdef _WIN32
struct pg_record_identity {
	uint64_t st_dev, st_ino;
	uint32_t st_mtime_nsec, st_mode;
};
#else
typedef struct stat pg_record_identity;
#endif

/* Loose records contain only their physical metadata. Archive-only payload
 * details are stored inline after archive records, never after loose records.
 */
struct pg_record_info {
	pg_id copy_id;
	uint64_t copy_generation;
	const char *canonical_name, *original_name;
	uint64_t logical_size;
	int64_t mtime;
};
struct pg_record_archive {
	uint64_t archive_record, stored_size, payload_offset;
	uint32_t encoding, digest_kind, checksum_domain;
	uint8_t digest[16];
	const void *cached_header;
	size_t cached_header_size;
};
struct pg_source_record {
	pg_record_info info;
	pg_record_identity identity;
	size_t extra_refs;
	pg_source_record *next;
	pg_record_archive *archive;
	uint32_t attributes;
	int packed_names;
#ifdef _WIN32
	HANDLE native_heap;
#endif
};

static inline pg_source_record *pg_record_archive_new()
{
	pg_source_record *record = (pg_source_record *)calloc(
		1, sizeof(*record) + sizeof(pg_record_archive));
	if (record) record->archive = (pg_record_archive *)(record + 1);
	return record;
}
static inline pg_file_info pg_record_file_info(const pg_source_record *record,
	pg_id source_id, uint64_t source_generation)
{
	pg_file_info info = {};
	info.source_id = source_id;
	info.copy_id = record->info.copy_id;
	info.source_generation = source_generation;
	info.copy_generation = record->info.copy_generation;
	info.canonical_name = record->info.canonical_name;
	info.original_name = record->info.original_name;
	info.logical_size = record->info.logical_size;
	info.mtime = record->info.mtime;
	info.archive_record = UINT64_MAX;
	info.stored_size = info.logical_size;
	info.encoding = PG_LOGICAL;
	if (record->archive) {
		info.archive_record = record->archive->archive_record;
		info.stored_size = record->archive->stored_size;
		info.encoding = record->archive->encoding;
		info.digest_kind = record->archive->digest_kind;
		info.checksum_domain = record->archive->checksum_domain;
		memcpy(info.digest, record->archive->digest,
			sizeof(info.digest));
		info.cached_header = record->archive->cached_header;
		info.cached_header_size = record->archive->cached_header_size;
	}
	return info;
}

/* Sorted descendants share one contiguous prefix; skip it for child lists. */
static inline size_t pg_records_after_directory(pg_source_record **items,
	size_t count, size_t begin, const char *name, size_t length)
{
	while (begin < count) {
		size_t middle = begin + (count - begin) / 2;
		const char *candidate = items[middle]->info.canonical_name;

		if (!strncmp(candidate, name, length) &&
			candidate[length] == '/')
			begin = middle + 1;
		else
			count = middle;
	}
	return begin;
}

static inline void pg_record_identity_set(
	pg_source_record *record, const struct stat *state)
{
#ifdef _WIN32
	record->identity.st_dev = state->st_dev;
	record->identity.st_ino = state->st_ino;
	record->identity.st_mode = state->st_mode;
	record->identity.st_mtime_nsec = state->st_mtime_nsec;
#else
	record->identity = *state;
#endif
}

static inline struct stat pg_record_stat(const pg_source_record *record)
{
#ifdef _WIN32
	struct stat state = {};

	state.st_dev = record->identity.st_dev;
	state.st_ino = record->identity.st_ino;
	state.st_mode = record->identity.st_mode;
	state.st_size = (int64_t)record->info.logical_size;
	state.st_mtime = record->info.mtime;
	state.st_mtime_nsec = record->identity.st_mtime_nsec;
	return state;
#else
	return record->identity;
#endif
}

/* Loose indexes retain relative names only. A captured file builds its native
 * path from the stable source root. Packed names share the record allocation.
 */
static inline void pg_record_names_free(pg_source_record *record)
{
	if (record->packed_names) return;
	if (record->info.canonical_name != record->info.original_name)
		free((void *)record->info.canonical_name);
	free((void *)record->info.original_name);
}

/* Context coordination protects record-name references held by snapshots. */
static inline pg_status pg_record_retain(pg_source_record *record)
{
	if (record->extra_refs == SIZE_MAX) return PG_LIMIT;
	record->extra_refs++;
	return PG_OK;
}

static inline void pg_record_release(pg_source_record *record)
{
	if (record->extra_refs) {
		record->extra_refs--;
		return;
	}
	pg_record_names_free(record);
	if (record->archive) free((void *)record->archive->cached_header);
#ifdef _WIN32
	if (record->native_heap) {
		HeapFree(record->native_heap, 0, record);
		return;
	}
#endif
	free(record);
}

static inline int pg_record_native_names(
	pg_source_record *record, const char *canonical, const char *original)
{
	record->info.original_name = strdup(original);
	record->info.canonical_name = !strcmp(canonical, original)
		? record->info.original_name
		: strdup(canonical);
	return record->info.original_name && record->info.canonical_name;
}

static inline pg_source_record *pg_record_native_new(
	pg_context *context, const char *canonical, const char *original)
{
	size_t original_size = strlen(original) + 1;
	size_t canonical_size =
		strcmp(canonical, original) ? strlen(canonical) + 1 : 0;
	if (original_size > SIZE_MAX - sizeof(pg_source_record) ||
		canonical_size >
			SIZE_MAX - sizeof(pg_source_record) - original_size)
		return NULL;
	size_t bytes =
		sizeof(pg_source_record) + original_size + canonical_size;
#ifdef _WIN32
	/* Isolate persistent names from short-lived client parser allocations.

	 * * Context coordination protects lazy heap creation and record
	 * ownership.
	 */
	if (!context->record_heap) {
		HANDLE heap = HeapCreate(0, 0, 0);
		if (!heap) return NULL;
		ULONG compatibility = 2;
		HeapSetInformation(heap, HeapCompatibilityInformation,
			&compatibility, sizeof(compatibility));
		context->record_heap = heap;
	}
	pg_source_record *record = (pg_source_record *)HeapAlloc(
		context->record_heap, HEAP_ZERO_MEMORY, bytes);
	if (record) record->native_heap = context->record_heap;
#else
	(void)context;
	pg_source_record *record = (pg_source_record *)calloc(1, bytes);
#endif
	if (!record) return NULL;
	char *names = (char *)(record + 1);
	memcpy(names, original, original_size);
	record->info.original_name = names;
	record->info.canonical_name = names;
	if (canonical_size) {
		memcpy(names + original_size, canonical, canonical_size);
		record->info.canonical_name = names + original_size;
	}
	record->packed_names = 1;
	return record;
}

/* Order a name against the contiguous prefix/ descendant range. */
static inline int pg_descendant_order(const char *name, const char *prefix)
{
	size_t length = strlen(prefix);
	int order = strncmp(name, prefix, length);

	return order ? order : (unsigned char)name[length] - '/';
}

/* A shallow scope includes the prefix itself and its immediate children. */
static inline int pg_name_in_scope(const char *name, const char *prefix,
		int recursive)
{
	const char *relative = name;

	if (prefix && *prefix) {
		size_t length = strlen(prefix);

		if (strncmp(name, prefix, length))
			return 0;
		if (!name[length])
			return 1;
		if (name[length] != '/')
			return 0;
		relative += length + 1;
	}
	return recursive || !strchr(relative, '/');
}

extern "C" pg_status pg_source_files_depth(pg_source *source,
	const char *prefix,
		int recursive, pg_cursor **out, pg_error *error);
extern "C" pg_status pg_tree_files_depth(pg_tree *tree, const char *prefix,
		int recursive, pg_cursor **out, pg_error *error);
extern "C" pg_status pg_source_discover_tree(pg_source *source,
	const char *prefix,
		uint32_t depth, pg_error *error);

extern "C" pg_status pg_source_name_kind(pg_source *source, const char *name);
extern "C" pg_status pg_source_writer_name_check(pg_source *source,
		const char *name);
extern "C" pg_status pg_source_request_tree_subtree(pg_source *source,
		const char *prefix, pg_error *error);

struct pg_source_request {
	pg_source_request *next;
	char *name;
};

struct pg_source_sync;

struct pg_source {
	pg_source_sync *sync;
	int internal_mutation;
	uint64_t changed_record;
	pg_context *context;
	pg_source *next_in_context;
	char *native_path;
	pg_source_record *records;
	pg_source_record **record_index;
	size_t record_count;
	pg_source_request *exact_requests;
	pg_source_request *prefix_requests;
	pg_source_request *shallow_requests;
	struct stat identity;
	pg_id id;
	uint64_t generation;
	size_t refs;
	size_t live_readers;
	size_t live_writers;
	uint32_t format;
	uint32_t access;
	uint32_t checksum_domain;
	int fd;
	int stale;
	int recovery_required;
	int loose_root_requested;
	int scan_shallow;
	int watch_wd;
#ifdef _WIN32
	HANDLE native_watch;
	HANDLE native_event;
	int native_armed;
	OVERLAPPED native_overlapped;
	uint8_t native_buffer[8192];
#endif
	struct pg_tree *attached;
};

static inline void pg_source_index_clear(pg_source *source)
{
	free(source->record_index);
	source->record_index = NULL;
	source->record_count = 0;
}

pg_status pg_source_records_scope(pg_source *source, const char *prefix,
		pg_source_record ***out, size_t *count);

pg_status pg_source_sync_create(pg_source *source);
void pg_source_sync_destroy(pg_source *source);
void pg_source_lock(pg_source *source);
void pg_source_unlock(pg_source *source);
pg_status pg_native_writer_lease(int fd, int *native_code);
pg_status pg_native_target_lease(const char *path, int exists, int *out,
		int *native_code);

struct pg_tree_source_state {
	pg_source_record *records;
	const char *prefix;
	int scoped;
	int recursive;
	pg_source_request *exact_requests;
	pg_source_request *prefix_requests;
	pg_source_request *shallow_requests;
	struct stat identity;
	uint64_t generation;
	int loose_root_requested;
	int captured;
};

struct pg_root_binding {
	pg_root_binding *next;
	pg_source *source;
	pg_source_record *records;
	struct stat identity;
	uint64_t generation;
	int fd;
};

struct pg_managed_scope {
	pg_managed_scope *next;
	char *prefix;
	uint32_t depth;
};

struct pg_native_hint {
	pg_native_hint *next;
	pg_source *source;
	char *name;
	int subtree;
	int processed;
};

struct pg_native_watch {
	pg_native_watch *next;
	pg_source *source;
	char *relative;
	int wd;
};

struct pg_tree {
	pg_context *context;
	pg_source **sources;
	struct pg_tree_scope *scopes;
	pg_managed_scope *managed;
	pg_native_hint *hints;
	pg_native_watch *watches;
	struct pg_tree_batch *pending_head;
	struct pg_tree_batch *pending_tail;
	size_t count;
	size_t capacity;
	uint64_t next_sequence;
	uint32_t watch_mode;
	int polling;
	int reconciling;
	int loss_reported;
	int loss_pending;
	int native_dirty;
	const char *query_name;
	int partial_changes;
	int native_repair;
	int native_fd;
	size_t refs;
	int closed;
#ifdef _WIN32
	DWORD control_thread;
#else
	pthread_t control_thread;
#endif
};

struct pg_tree_batch {
	pg_tree_batch *next;
	char *invalid_name;
	char *invalid_scope;
	pg_cursor *before;
	pg_cursor *after;
	pg_cursor *replacement;
	pg_tree_scope *scope;
	size_t before_index;
	size_t after_index;
};

struct pg_tree_scope {
	pg_tree_scope *next;
	char *prefix;
	pg_cursor *baseline;
	size_t reader_refs;
	int exact;
	int shallow;
	int managed;
	int dirty;
};

pg_status pg_tree_retain(pg_tree *tree);
void pg_tree_release(pg_tree *tree);
void pg_tree_scope_lock(pg_tree *tree);
void pg_tree_scope_unlock(pg_tree *tree);
int pg_tree_on_control_thread(const pg_tree *tree);
pg_status pg_tree_control_status(const pg_tree *tree, int mutation);
pg_status pg_source_control_status(const pg_source *source, int mutation);

struct pg_file {
	pg_source_record *names_record;
	int packed;
	pg_source *source;
	uint32_t attributes;
	pg_tree *origin_tree;
	pg_file_info info;
	uint64_t payload_offset;
	struct stat source_identity;
	char *native_path;
};

/* Materialize a native filename only for payload or mutation operations. */
extern "C" pg_status pg_file_prepare_native(pg_file *file);

struct pg_cursor {
	pg_context *context;
	pg_source *source;
	pg_file **files;
	size_t count;
	size_t position;
};

pg_status pg_indexed_directories(pg_source **sources, size_t count,
		const char *prefix, const char ***out, size_t *size);
int pg_directory_names_contain(const char **names, size_t count,
		const char *name);

struct pg_unpack_item {
	pg_source *source;
	pg_file *selection;
	char *name;
	pg_id copy_id;
	uint64_t copy_generation;
	struct stat parent_identity;
	struct stat target_identity;
	int target_exists;
};

struct pg_unpack_target {
	pg_context *context;
	char *root_path;
	int root_fd;
	struct stat root_identity;
	pg_unpack_item *items;
	size_t count;
	size_t live_writers;
	uint32_t flags;
};

pg_status pg_source_retain(pg_source *source);
pg_status pg_source_release(pg_source *source, int *native_code);

struct pg_reader {
	pg_context *context;
	pg_source *source;
	pg_tree *watch_tree;
	pg_tree_scope *watch_scope;
	char *native_path;
	pg_reader_info info;
	pg_file_info file_info;
	struct stat identity;
	uint64_t position;
	uint64_t payload_offset;
	void *decoder;
	uint32_t representation;
	int fd;
	int failed;
	int verified;
};

extern "C" pg_status pg_tree_reader_scope_add(pg_tree *tree, pg_file *file,
		pg_tree_scope **out, pg_error *error);
extern "C" void pg_tree_reader_scope_remove(pg_tree *tree,
		pg_tree_scope *scope);
extern "C" void pg_tree_reader_scope_sweep(pg_tree *tree);
extern "C" pg_status pg_tree_scope_snapshot(pg_tree *tree,
		pg_tree_scope *scope, pg_cursor **out,
		pg_error *error);
extern "C" pg_status pg_tree_cursor_clone(pg_cursor *source,
		pg_cursor **out);
pg_status pg_tree_queue_changes(pg_tree *tree, pg_error *error);
pg_status pg_tree_queue_topology(pg_tree *tree, pg_error *error);
extern "C" pg_status pg_source_find_fresh(pg_source *source, const char *name,
		pg_file **out, pg_error *error);
int pg_tree_manages(pg_tree *tree, const char *name);
pg_status pg_tree_observe_name(pg_tree *tree, const char *name,
		pg_error *error);
pg_status pg_tree_manage_scan(pg_tree *tree, pg_error *error);
void pg_tree_manage_discard(pg_tree *tree);
pg_status pg_tree_hint_add(pg_tree *tree, pg_source *source,
		const char *name, int subtree);
void pg_tree_hint_discard(pg_tree *tree, pg_source *source);
pg_status pg_tree_hints_reconcile(pg_tree *tree, pg_error *error);
pg_status pg_tree_invalidate_managed(pg_tree *tree);
pg_status pg_tree_entry_snapshot(pg_tree *tree, pg_tree_scope *scope,
		pg_cursor **out, pg_error *error);
extern "C" pg_status pg_source_rebind_root(pg_source *source,
		int *changed, pg_root_binding **saved, pg_error *error);
extern "C" void pg_source_rebind_finish(pg_root_binding *saved, int commit);
extern "C" pg_status pg_source_select(pg_source *source,
		pg_source_record *selected, pg_file **out, pg_error *error);

extern "C" pg_status pg_source_entry_records(pg_source *source,
	const char *prefix, int recursive, pg_source_record ***out,
	size_t *count);

extern "C" pg_status pg_tree_entry_records(pg_tree *tree, const char *prefix,
	int recursive, pg_source_record ***out, size_t *count, pg_error *error);
extern "C" pg_status pg_source_refresh_name(pg_source *source,
		const char *name, int recursive, pg_error *error);
pg_status pg_tree_native_reconcile(pg_tree *tree, pg_error *error);
void pg_tree_pending_discard(pg_tree *tree);
extern "C" pg_status pg_tree_sources_save(pg_tree *tree,
		pg_tree_source_state **out);
extern "C" pg_status pg_tree_rescan_scopes(pg_tree *tree,
		pg_error *error);
extern "C" void pg_tree_sources_restore(pg_tree *tree,
		pg_tree_source_state *saved);
extern "C" void pg_tree_sources_discard(pg_tree *tree,
		pg_tree_source_state *saved);

struct pg_builder_entry {
	pg_builder_entry *next;
	char *canonical_name;
	char *original_name;
	void *cached_header;
	size_t cached_header_size;
	uint64_t stage_offset;
	uint64_t logical_size;
	uint64_t stored_size;
	int64_t mtime;
	uint32_t encoding;
	uint8_t digest[16];
};

struct pg_archive_builder {
	pg_context *context;
	char *native_path;
	char *leaf;
	int parent_fd;
	FILE *staging;
	pg_builder_entry *entries;
	struct stat target;
	uint32_t format;
	uint32_t flags;
	uint32_t checksum_domain;
	size_t live_writers;
	int target_exists;
	int finished;
};

enum pg_writer_kind {
	PG_WRITER_BUILDER = 1,
	PG_WRITER_NATIVE = 2,
	PG_WRITER_SOURCE_ARCHIVE = 3
};

enum pg_writer_state {
	PG_WRITER_ACTIVE = 0,
	PG_WRITER_CLOSE_ONLY,
	PG_WRITER_FINISHED
};

struct pg_writer {
	pg_archive_builder *builder;
	pg_context *context;
	pg_source *source;
	pg_unpack_target *unpack_target;
	FILE *input;
	char *native_path;
	char *leaf;
	int parent_fd;
	struct stat target;
	struct stat archive_identity;
	pg_id target_copy_id;
	uint64_t target_copy_generation;
	uint64_t target_record;
	int named_target;
	int metadata_only;
	uint32_t flags;
	int target_exists;
	char *canonical_name;
	char *original_name;
	void *cached_header;
	pg_write_options options;
	uint64_t accepted;
	uint32_t kind;
	uint32_t state;
};

extern "C" pg_status pg_writer_open_native_for_source(pg_context *context,
		pg_source *source,
		const char *native_path, const pg_write_options *options,
		uint32_t flags, pg_writer **out, pg_error *error);

pg_status pg_transfer_bytes(pg_reader *reader, pg_writer *writer,
		pg_error *error);
pg_status pg_pack_cursor(pg_cursor **cursor, const char *native_archive,
		uint32_t format, const pg_pack_options *options,
		pg_error *error);

pg_status pg_unpack_cursor(pg_cursor *cursor, pg_context *context,
		const char *native_directory, uint32_t flags,
		pg_error *error);

pg_status pg_source_hogg_delete(pg_file *file, pg_error *error);
pg_status pg_source_pigg_delete(pg_file *file, pg_error *error);
pg_status pg_source_archive_finish(pg_writer *writer, pg_error *error);
struct pg_metadata_options;
pg_status pg_source_update_metadata(pg_file *file,
		const pg_metadata_options *options, pg_error *error);

pg_status pg_tree_native_add_source(pg_tree *tree, pg_source *source,
		int *native_code);
void pg_tree_native_remove_source(pg_tree *tree, pg_source *source);

static inline pg_status pg_result(pg_status status, pg_error *error)
{
	if (error) {
		memset(error, 0, sizeof(*error));
		error->status = status;
		error->cause = status;
		error->offset = UINT64_MAX;
	}
	return status;
}

static inline pg_status pg_native_result(pg_status status, int native_code,
		pg_error *error)
{
	pg_result(status, error);
	if (error)
		error->native_code = native_code;
	return status;
}

static inline const char *pg_native_basename(const char *path)
{
	const char *leaf = path;

	for (; *path; path++) {
		if (*path == '/'
#ifdef _WIN32
		    || *path == '\\'
#endif
		    )
			leaf = path + 1;
	}
	return leaf;
}

char *pg_native_absolute(const char *path, pg_status *status,
		int *native_code);

#endif
