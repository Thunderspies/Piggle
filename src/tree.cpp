#include <piggle/piggle.h>
#include "internal.hpp"

pg_status pg_tree_retain(pg_tree *tree)
{
	pg_status status = PG_OK;

#ifdef _WIN32
	EnterCriticalSection(&tree->scope_lock);
#else
	pthread_mutex_lock(&tree->scope_lock);
#endif
	if (tree->refs == SIZE_MAX)
		status = PG_LIMIT;
	else
		tree->refs++;
#ifdef _WIN32
	LeaveCriticalSection(&tree->scope_lock);
#else
	pthread_mutex_unlock(&tree->scope_lock);
#endif
	return status;
}

void pg_tree_release(pg_tree *tree)
{
	int last;

#ifdef _WIN32
	EnterCriticalSection(&tree->scope_lock);
#else
	pthread_mutex_lock(&tree->scope_lock);
#endif
	last = !--tree->refs;
#ifdef _WIN32
	LeaveCriticalSection(&tree->scope_lock);
	if (last)
		DeleteCriticalSection(&tree->scope_lock);
#else
	pthread_mutex_unlock(&tree->scope_lock);
	if (last)
		pthread_mutex_destroy(&tree->scope_lock);
#endif
	if (last)
		free(tree);
}

void pg_tree_scope_lock(pg_tree *tree)
{
#ifdef _WIN32
	EnterCriticalSection(&tree->scope_lock);
#else
	pthread_mutex_lock(&tree->scope_lock);
#endif
}

void pg_tree_scope_unlock(pg_tree *tree)
{
#ifdef _WIN32
	LeaveCriticalSection(&tree->scope_lock);
#else
	pthread_mutex_unlock(&tree->scope_lock);
#endif
}

int pg_tree_on_control_thread(const pg_tree *tree)
{
#ifdef _WIN32
	return tree->control_thread == GetCurrentThreadId();
#else
	return pthread_equal(tree->control_thread, pthread_self());
#endif
}

pg_status pg_tree_control_status(const pg_tree *tree,
		int mutation)
{
	if (!pg_tree_on_control_thread(tree))
		return PG_BUSY;
	if (mutation && tree->polling && !tree->reconciling)
		return PG_REENTRANT;
	return PG_OK;
}

pg_status pg_source_control_status(const pg_source *source,
		int mutation)
{
	if (source->attached && mutation &&
	    source->attached->reconciling &&
	    pg_tree_on_control_thread(source->attached))
		return PG_OK;
	return source->attached ?
		pg_tree_control_status(source->attached, mutation) : PG_OK;
}

extern "C" {

static int pg_tree_scope_covers(pg_tree *tree, const char *name);
static pg_status pg_tree_sources_save_mode(pg_tree *tree,
		pg_tree_source_state **out, int loose_only);

/* Allocate an empty tree and retain its context. */
PG_API pg_status PG_CALL pg_tree_create(pg_context *context, pg_tree **out,
		pg_error *error)
{
	pg_tree *tree;

	if (!out)
		return pg_result(PG_INVALID, error);
	*out = NULL;
	if (!context)
		return pg_result(PG_INVALID, error);
	if (pg_context_children(context) == SIZE_MAX)
		return pg_result(PG_LIMIT, error);
	tree = (pg_tree *)calloc(1, sizeof(*tree));
	if (!tree)
		return pg_result(PG_NOMEM, error);
	tree->context = context;
	tree->next_sequence = 1;
	tree->native_fd = -1;
	tree->refs = 1;
#ifdef _WIN32
	InitializeCriticalSection(&tree->scope_lock);
	tree->control_thread = GetCurrentThreadId();
#else
	pthread_mutexattr_t attributes;

	if (pthread_mutexattr_init(&attributes)) {
		free(tree);
		return pg_result(PG_IO, error);
	}
	if (pthread_mutexattr_settype(&attributes,
		PTHREAD_MUTEX_RECURSIVE) ||
	    pthread_mutex_init(&tree->scope_lock, &attributes)) {
		pthread_mutexattr_destroy(&attributes);
		free(tree);
		return pg_result(PG_IO, error);
	}
	pthread_mutexattr_destroy(&attributes);
	tree->control_thread = pthread_self();
#endif
	pg_context_child_add(context);
	*out = tree;
	return pg_result(PG_OK, error);
}

/* Copy current attachment count and watch mode. */
PG_API pg_status PG_CALL pg_tree_inspect(pg_tree *tree, pg_tree_info *out,
		pg_error *error)
{
	if (out)
		memset(out, 0, sizeof(*out));
	if (!tree || !out)
		return pg_result(PG_INVALID, error);
	out->source_count = tree->count;
	out->watch_mode = tree->watch_mode;
	return pg_result(PG_OK, error);
}

/* Open and attach sources in caller order, unwinding a failed build. */
PG_API pg_status PG_CALL pg_tree_open(
		pg_context *context,
		const pg_source_spec *sources,
		size_t count,
		pg_tree **out,
		pg_error *error)
{
	pg_tree *tree = NULL;
	pg_error work_error;
	pg_status status;
	size_t i;

	if (!out)
		return pg_result(PG_INVALID, error);
	*out = NULL;
	if (!context || (count && !sources))
		return pg_result(PG_INVALID, error);
	status = pg_tree_create(context, &tree, &work_error);
	if (status != PG_OK) {
		if (error)
			*error = work_error;
		return status;
	}
	for (i = 0; i < count; i++) {
		pg_source *source = NULL;

		status = pg_source_open(context, sources[i].native_path,
			&sources[i].options, &source, &work_error);
		if (status != PG_OK)
			break;
		status = pg_tree_attach(tree, source, &work_error);
		pg_source_close(&source, NULL);
		if (status != PG_OK)
			break;
	}
	if (status != PG_OK) {
		pg_tree_close(&tree, NULL);
		if (error)
			*error = work_error;
		return status;
	}
	*out = tree;
	return pg_result(PG_OK, error);
}

static int pg_tree_path_contains(const char *root, const char *path)
{
	size_t length = strlen(root);

	while (length > 1 &&
	       (root[length - 1] == '/' || root[length - 1] == '\\'))
		length--;
#ifndef _WIN32
	if (length == 1 && root[0] == '/')
		return path[0] == '/';
#endif
#ifdef _WIN32
	return _strnicmp(root, path, length) == 0 &&
		(path[length] == '\0' || path[length] == '/' ||
		 path[length] == '\\');
#else
	return strncmp(root, path, length) == 0 &&
		(path[length] == '\0' || path[length] == '/');
#endif
}

/* Attach one source after existing lower-precedence sources. */
PG_API pg_status PG_CALL pg_tree_attach(
		pg_tree *tree,
		pg_source *source,
		pg_error *error)
{
	pg_source **grown;
	pg_status status;
	size_t next;
	int native_code = 0;

	if (!tree || !source)
		return pg_result(PG_INVALID, error);
	status = pg_tree_control_status(tree, 1);
	if (status != PG_OK)
		return pg_result(status, error);
	if (source->context != tree->context)
		return pg_result(PG_INVALID, error);
	if (source->attached)
		return pg_result(PG_BUSY, error);
	char *new_path = realpath(source->native_path, NULL);

	if (!new_path)
		return pg_native_result(PG_RETRY, errno, error);
	for (size_t i = 0; i < tree->count; i++) {
		char *old_path = realpath(
			tree->sources[i]->native_path, NULL);
		int overlap;

		if (!old_path) {
			int code = errno;

			free(new_path);
			return pg_native_result(PG_RETRY, code,
				error);
		}
		overlap = pg_tree_path_contains(old_path, new_path) ||
			pg_tree_path_contains(new_path, old_path);
		free(old_path);
		if (overlap) {
			free(new_path);
			return pg_result(PG_CONFLICT, error);
		}
	}
	free(new_path);
	if (tree->count == tree->capacity) {
		next = tree->capacity ? tree->capacity * 2 : 4;
		if (next < tree->capacity ||
		    next > SIZE_MAX / sizeof(*grown))
			return pg_result(PG_LIMIT, error);
		grown = (pg_source **)realloc(tree->sources,
			next * sizeof(*grown));
		if (!grown)
			return pg_result(PG_NOMEM, error);
		tree->sources = grown;
		tree->capacity = next;
	}
	status = pg_source_retain(source);
	if (status != PG_OK)
		return pg_result(status, error);
	if (source->format == PG_LOOSE) {
		pg_tree_scope *scope;

		for (scope = tree->scopes; scope; scope = scope->next) {
			if (scope->exact)
				continue;
			status = pg_source_discover_tree(source, scope->prefix,
				scope->shallow ? PG_DISCOVER_CHILDREN :
				PG_DISCOVER_RECURSIVE, error);
			if (status != PG_OK) {
				pg_source_release(source, &native_code);
				return status;
			}
		}
	}
	if (tree->watch_mode == PG_WATCH_NATIVE) {
		status = pg_tree_native_add_source(tree, source,
			&native_code);
		if (status != PG_OK) {
			pg_tree_native_remove_source(tree, source);
			pg_source_release(source, &native_code);
			return pg_native_result(status, native_code,
				error);
		}
	}
	tree->sources[tree->count++] = source;
	source->attached = tree;
	status = pg_tree_queue_topology(tree, error);
	if (status != PG_OK) {
		tree->count--;
		source->attached = NULL;
		if (tree->watch_mode == PG_WATCH_NATIVE)
			pg_tree_native_remove_source(tree, source);
		pg_source_release(source, &native_code);
		return status;
	}
	return pg_result(PG_OK, error);
}

/* Remove one source without invalidating retained selections. */
PG_API pg_status PG_CALL pg_tree_detach(
		pg_tree *tree,
		pg_source *source,
		pg_error *error)
{
	size_t i;
	int native_code = 0;
	pg_status status;

	if (!tree || !source || tree->context != source->context)
		return pg_result(PG_INVALID, error);
	status = pg_tree_control_status(tree, 1);
	if (status != PG_OK)
		return pg_result(status, error);
	if (source->attached != tree)
		return pg_result(PG_NOT_ATTACHED, error);
	for (i = 0; i < tree->count; i++) {
		if (tree->sources[i] == source)
			break;
	}
	if (i == tree->count)
		return pg_result(PG_NOT_ATTACHED, error);
	memmove(tree->sources + i, tree->sources + i + 1,
		(tree->count - i - 1) * sizeof(*tree->sources));
	tree->count--;
	status = pg_tree_queue_topology(tree, error);
	if (status != PG_OK) {
		memmove(tree->sources + i + 1,
			tree->sources + i,
			(tree->count - i) * sizeof(*tree->sources));
		tree->sources[i] = source;
		tree->count++;
		return status;
	}
	if (tree->watch_mode == PG_WATCH_NATIVE)
		pg_tree_native_remove_source(tree, source);
	source->attached = NULL;
	status = pg_source_release(source, &native_code);
	return pg_native_result(status, native_code, error);
}

/* Return an owned reference to the indexed attachment. */
PG_API pg_status PG_CALL pg_tree_source(pg_tree *tree, size_t index,
		pg_source **out, pg_error *error)
{
	pg_status status;

	if (!out)
		return pg_result(PG_INVALID, error);
	*out = NULL;
	if (!tree)
		return pg_result(PG_INVALID, error);
	status = pg_tree_control_status(tree, 0);
	if (status != PG_OK)
		return pg_result(status, error);
	if (index >= tree->count)
		return pg_result(PG_END, error);
	status = pg_source_retain(tree->sources[index]);
	if (status != PG_OK)
		return pg_result(status, error);
	*out = tree->sources[index];
	return pg_result(PG_OK, error);
}

/* Read one selected overlay file and preserve the first failure. */
PG_API pg_status PG_CALL pg_tree_read_all(pg_tree *tree,
		const char *name, void *buffer, size_t capacity, size_t *bytes,
		pg_error *error)
{
	pg_file *file = NULL;
	pg_error work_error, close_error;
	pg_status status, closed;

	if (bytes)
		*bytes = 0;
	if (!tree || !name || !bytes || (capacity && !buffer))
		return pg_result(PG_INVALID, error);
	status = pg_tree_find(tree, name, &file, &work_error);
	if (status != PG_OK) {
		if (error)
			*error = work_error;
		return status;
	}
	status = pg_file_read_all(file, buffer, capacity, bytes,
		&work_error);
	closed = pg_file_close(&file, &close_error);
	if (status == PG_OK) {
		status = closed;
		work_error = close_error;
	}
	if (error)
		*error = work_error;
	return status;
}

/* Allocate one selected overlay file and retain the first failure. */
PG_API pg_status PG_CALL pg_tree_read_all_alloc(pg_tree *tree,
		const char *name, size_t max_bytes, pg_buffer *out,
		pg_error *error)
{
	pg_file *file = NULL;
	pg_error work_error, close_error;
	pg_status status, closed;

	if (!tree || !name || !out || out->data || out->size)
		return pg_result(PG_INVALID, error);
	status = pg_tree_find(tree, name, &file, &work_error);
	if (status != PG_OK) {
		if (error)
			*error = work_error;
		return status;
	}
	status = pg_file_read_all_alloc(file, max_bytes, out,
		&work_error);
	closed = pg_file_close(&file, &close_error);
	if (status == PG_OK && closed != PG_OK) {
		pg_buffer_free(out);
		status = closed;
		work_error = close_error;
	}
	if (error)
		*error = work_error;
	return status;
}

/* Export one overlay selection and preserve its first outcome. */
PG_API pg_status PG_CALL pg_tree_export(pg_tree *tree, const char *name,
		const char *native_output, uint32_t flags,
		pg_error *error)
{
	pg_file *file = NULL;
	pg_error work_error, close_error;
	pg_status status, closed;

	if (!tree || !name || !native_output || !*native_output ||
	    (flags & ~PG_OVERWRITE))
		return pg_result(PG_INVALID, error);
	status = pg_tree_find(tree, name, &file, &work_error);
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

/* Pack one overlay snapshot into a private archive builder. */
PG_API pg_status PG_CALL pg_tree_pack(
		pg_tree *tree,
		const char *native_archive,
		uint32_t format,
		const pg_pack_options *options,
		pg_error *error)
{
	pg_pack_options defaults = { PG_COMPRESS_AUTO, 0, PG_CHECKSUM_LOGICAL };
	const pg_pack_options *chosen = options ? options : &defaults;
	pg_cursor *cursor = NULL;

	if (!tree || !native_archive || !*native_archive ||
	    (format != PG_PIGG2 && format != PG_HOGG10) ||
	    chosen->compression > PG_COMPRESS_FORCE ||
	    chosen->checksum_domain > PG_CHECKSUM_STORED ||
	    (chosen->flags & ~PG_OVERWRITE))
		return pg_result(PG_INVALID, error);
	if (chosen->checksum_domain == PG_CHECKSUM_STORED &&
	    format != PG_HOGG10)
		return pg_result(PG_UNSUPPORTED, error);
	pg_status status = pg_tree_request_subtree(tree, NULL, error);

	if (status != PG_OK)
		return status;
	status = pg_tree_files(tree, NULL, &cursor, error);
	if (status != PG_OK)
		return status;
	return pg_pack_cursor(&cursor, native_archive, format, chosen, error);
}

/* Unpack one overlay snapshot under an existing directory. */
PG_API pg_status PG_CALL pg_tree_unpack(
		pg_tree *tree,
		const char *native_directory,
		uint32_t flags,
		pg_error *error)
{
	pg_cursor *cursor = NULL;
	pg_error work_error, close_error;
	pg_status status, closed;

	if (!tree || !native_directory || !*native_directory ||
	    (flags & ~PG_OVERWRITE))
		return pg_result(PG_INVALID, error);
	status = pg_tree_request_subtree(tree, NULL, &work_error);
	if (status != PG_OK) {
		if (error)
			*error = work_error;
		return status;
	}
	status = pg_tree_files(tree, NULL, &cursor, &work_error);
	if (status != PG_OK) {
		if (error)
			*error = work_error;
		return status;
	}
	status = pg_unpack_cursor(cursor, tree->context,
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

/* Resolve overlay visibility in attachment order. */
static pg_status pg_tree_find_canonical(
		pg_tree *tree,
		const char *name,
		pg_file **out,
		pg_error *error)
{
	pg_file *selected = NULL;
	pg_status status;
	size_t i;

	if (!out)
		return pg_result(PG_INVALID, error);
	*out = NULL;
	if (!tree || !name)
		return pg_result(PG_INVALID, error);
	status = pg_tree_control_status(tree, 0);
	if (status != PG_OK)
		return pg_result(status, error);
	tree->query_name = name;
	status = pg_tree_native_reconcile(tree, error);
	tree->query_name = NULL;
	if (status != PG_OK)
		return status;
	for (i = tree->count; i; i--) {
		pg_file *candidate = NULL;
		pg_error find_error;

		status = (tree->reconciling ||
			pg_tree_scope_covers(tree, name) ||
			pg_tree_manages(tree, name)) ?
			pg_source_find(tree->sources[i - 1], name,
				&candidate, &find_error) :
			pg_source_find_fresh(tree->sources[i - 1], name,
				&candidate, &find_error);
		if (status == PG_CONFLICT) {
			pg_file_close(&selected, NULL);
			if (error)
				*error = find_error;
			return status;
		}
		if (status != PG_OK && status != PG_NOT_FOUND) {
			pg_file_close(&selected, NULL);
			if (error)
				*error = find_error;
			return status;
		}
		if (candidate && !selected)
			selected = candidate;
		else
			pg_file_close(&candidate, NULL);
	}
	if (!selected)
		return pg_result(PG_NOT_FOUND, error);
	status = pg_tree_retain(tree);
	if (status != PG_OK) {
		pg_file_close(&selected, NULL);
		return pg_result(status, error);
	}
	selected->origin_tree = tree;
	*out = selected;
	return pg_result(PG_OK, error);
}

PG_API pg_status PG_CALL pg_tree_find(pg_tree *tree, const char *name,
		pg_file **out, pg_error *error)
{
	size_t required = 0;
	pg_status status;
	char *canonical;

	if (out)
		*out = NULL;
	if (!tree || !name || !out)
		return pg_result(PG_INVALID, error);
	status = pg_name_normalize(tree->context, name, NULL, 0,
		&required, NULL);
	if (status != PG_CAPACITY)
		return pg_result(status, error);
	canonical = (char *)malloc(required);
	if (!canonical)
		return pg_result(PG_NOMEM, error);
	status = pg_name_normalize(tree->context, name, canonical,
		required, &required, error);
	if (status == PG_OK)
		status = pg_tree_find_canonical(tree, canonical, out, error);
	if ((status == PG_OK || status == PG_NOT_FOUND ||
	     status == PG_CONFLICT) && !tree->reconciling &&
	    pg_tree_manages(tree, canonical)) {
		pg_status observed = pg_tree_observe_name(tree, canonical,
			NULL);

		if (observed != PG_OK) {
			pg_file_close(out, NULL);
			status = pg_result(observed, error);
		}
	}
	free(canonical);
	return status;
}

static int pg_tree_scope_covers(pg_tree *tree, const char *name)
{
	pg_tree_scope *scope;

	for (scope = tree->scopes; scope; scope = scope->next) {
		size_t length;

		if (scope->exact || scope->shallow)
			continue;
		if (!scope->prefix)
			return 1;
		if (!name)
			continue;
		length = strlen(scope->prefix);
		if (strncmp(name, scope->prefix, length) == 0 &&
		    (name[length] == '\0' || name[length] == '/'))
			return 1;
	}
	return 0;
}

pg_status pg_tree_cursor_clone(pg_cursor *source,
		pg_cursor **out)
{
	pg_cursor *copy = (pg_cursor *)calloc(1, sizeof(*copy));
	size_t i;

	*out = NULL;
	if (!copy)
		return PG_NOMEM;
	copy->context = source->context;
	copy->files = (pg_file **)calloc(source->count ?
		source->count : 1, sizeof(*copy->files));
	if (!copy->files) {
		free(copy);
		return PG_NOMEM;
	}
	pg_context_child_add(copy->context);
	for (i = 0; i < source->count; i++) {
		pg_file *from = source->files[i];
		pg_file *file = (pg_file *)calloc(1, sizeof(*file));
		pg_status status;

		if (!file)
			goto fail_clone;
		file->info = from->info;
		file->info.canonical_name = strdup(
			from->info.canonical_name);
		file->info.original_name = strdup(
			from->info.original_name);
		file->info.cached_header = NULL;
		file->native_path = from->native_path ?
			strdup(from->native_path) : NULL;
		if (from->info.cached_header_size) {
			file->info.cached_header = malloc(
				from->info.cached_header_size);
			if (file->info.cached_header)
				memcpy((void *)file->info.cached_header,
					from->info.cached_header,
					from->info.cached_header_size);
		}
		if (!file->info.canonical_name ||
		    !file->info.original_name ||
		    (from->native_path && !file->native_path) ||
		    (from->info.cached_header_size &&
		     !file->info.cached_header)) {
			free((void *)file->info.canonical_name);
			free((void *)file->info.original_name);
			free((void *)file->info.cached_header);
			free(file->native_path);
			free(file);
			goto fail_clone;
		}
		file->source = from->source;
		file->attributes = from->attributes;
		file->origin_tree = from->origin_tree;
		file->source_identity = from->source_identity;
		file->payload_offset = from->payload_offset;
		status = pg_source_retain(file->source);
		if (status != PG_OK) {
			free((void *)file->info.canonical_name);
			free((void *)file->info.original_name);
			free((void *)file->info.cached_header);
			free(file->native_path);
			free(file);
			goto fail_clone;
		}
		if (file->origin_tree) {
			status = pg_tree_retain(file->origin_tree);
			if (status != PG_OK) {
				file->origin_tree = NULL;
				pg_file_close(&file, NULL);
				goto fail_clone;
			}
		}
		copy->files[copy->count++] = file;
	}
	*out = copy;
	return PG_OK;

fail_clone:
	pg_cursor_close(&copy, NULL);
	return PG_NOMEM;
}

static pg_status pg_tree_exact_snapshot(pg_tree *tree,
		const char *name, pg_cursor **out, pg_error *error)
{
	pg_cursor *cursor = (pg_cursor *)calloc(1, sizeof(*cursor));
	pg_file *file = NULL;
	pg_status status;

	*out = NULL;
	if (!cursor)
		return pg_result(PG_NOMEM, error);
	cursor->context = tree->context;
	int reconciling = tree->reconciling;

	tree->reconciling = 1;
	status = pg_tree_find(tree, name, &file, error);
	tree->reconciling = reconciling;
	if (status == PG_CONFLICT) {
		for (size_t i = tree->count; i && !file; i--) {
			pg_source *source = tree->sources[i - 1];

			for (pg_source_record *r = source->records; r; r =
				r->next) {
				if (S_ISDIR(r->identity.st_mode) &&
				    !strcmp(r->info.canonical_name, name)) {
					status = pg_source_select(source, r,
						&file, error);
					break;
				}
			}
		}
	}
	if (status == PG_NOT_FOUND || status == PG_CONFLICT)
		status = PG_OK;
	if (status != PG_OK) {
		free(cursor);
		return status;
	}
	if (file) {
		cursor->files = (pg_file **)malloc(
			sizeof(*cursor->files));
		if (!cursor->files) {
			pg_file_close(&file, NULL);
			free(cursor);
			return pg_result(PG_NOMEM, error);
		}
		cursor->files[0] = file;
		cursor->count = 1;
	}
	pg_context_child_add(cursor->context);
	*out = cursor;
	return pg_result(PG_OK, error);
}

pg_status pg_tree_reader_scope_add(pg_tree *tree, pg_file *file,
		pg_tree_scope **out, pg_error *error)
{
	pg_tree_scope *scope;
	const char *name = file->info.canonical_name;
	pg_status status;

	*out = NULL;
	pg_tree_scope_lock(tree);
	if (tree->closed) {
		pg_tree_scope_unlock(tree);
		return pg_result(PG_OK, error);
	}
	for (scope = tree->scopes; scope; scope = scope->next) {
		if (scope->exact && (scope->reader_refs || scope->managed) &&
		    strcmp(scope->prefix, name) == 0) {
			if (scope->reader_refs == SIZE_MAX) {
				pg_tree_scope_unlock(tree);
				return pg_result(PG_LIMIT, error);
			}
			scope->reader_refs++;
			*out = scope;
			pg_tree_scope_unlock(tree);
			return pg_result(PG_OK, error);
		}
	}
	scope = (pg_tree_scope *)calloc(1, sizeof(*scope));
	if (!scope) {
		pg_tree_scope_unlock(tree);
		return pg_result(PG_NOMEM, error);
	}
	scope->prefix = strdup(name);
	if (!scope->prefix) {
		free(scope);
		pg_tree_scope_unlock(tree);
		return pg_result(PG_NOMEM, error);
	}
	status = pg_tree_exact_snapshot(tree, name,
		&scope->baseline, error);
	if (status != PG_OK) {
		free(scope->prefix);
		free(scope);
		pg_tree_scope_unlock(tree);
		return status;
	}
	scope->exact = 1;
	scope->reader_refs = 1;
	scope->next = tree->scopes;
	tree->scopes = scope;
	*out = scope;
	pg_tree_scope_unlock(tree);
	return pg_result(PG_OK, error);
}

void pg_tree_reader_scope_remove(pg_tree *tree, pg_tree_scope *scope)
{
	pg_tree_scope **at;

	pg_tree_scope_lock(tree);
	if (!scope || tree->closed) {
		pg_tree_scope_unlock(tree);
		return;
	}
	if (--scope->reader_refs || scope->managed) {
		pg_tree_scope_unlock(tree);
		return;
	}
	if (tree->polling || !pg_tree_on_control_thread(tree)) {
		pg_tree_scope_unlock(tree);
		return;
	}
	at = &tree->scopes;
	while (*at && *at != scope)
		at = &(*at)->next;
	if (*at)
		*at = scope->next;
	pg_cursor_close(&scope->baseline, NULL);
	free(scope->prefix);
	free(scope);
	pg_tree_scope_unlock(tree);
}

void pg_tree_reader_scope_sweep(pg_tree *tree)
{
	if (!pg_tree_on_control_thread(tree))
		return;
	pg_tree_scope_lock(tree);
	pg_tree_scope **at = &tree->scopes;

	while (*at) {
		pg_tree_scope *scope = *at;

		if (!scope->exact || scope->reader_refs || scope->managed) {
			at = &scope->next;
			continue;
		}
		*at = scope->next;
		pg_cursor_close(&scope->baseline, NULL);
		free(scope->prefix);
		free(scope);
	}
	pg_tree_scope_unlock(tree);
}

pg_status pg_tree_scope_snapshot(pg_tree *tree,
		pg_tree_scope *scope, pg_cursor **out,
		pg_error *error)
{
	return scope->exact ?
		pg_tree_exact_snapshot(tree, scope->prefix, out, error) :
		pg_tree_entry_snapshot(tree, scope, out, error);
}

static int pg_tree_scope_covers_depth(pg_tree *tree, const char *prefix,
		int recursive)
{
	if (pg_tree_scope_covers(tree, prefix))
		return 1;
	if (!recursive) {
		for (pg_tree_scope *s = tree->scopes; s; s = s->next) {
			if (!s->exact && s->shallow &&
			    !strcmp(s->prefix ? s->prefix : "",
				prefix ? prefix : ""))
				return 1;
		}
	}
	return 0;
}

PG_API pg_status PG_CALL pg_tree_files(pg_tree *tree, const char *prefix,
		pg_cursor **out, pg_error *error)
{
	return pg_tree_files_depth(tree, prefix, 1, out, error);
}

static int pg_tree_file_compare(const void *left, const void *right)
{
	const pg_file *a = *(pg_file *const *)left;
	const pg_file *b = *(pg_file *const *)right;

	return strcmp(a->info.canonical_name, b->info.canonical_name);
}

static int pg_tree_items_directory(pg_file **items, size_t count,
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

static pg_status pg_tree_prefix_check(pg_tree *tree, const char *prefix)
{
	pg_status status = PG_OK;

	if (!prefix)
		return PG_OK;
	for (size_t i = 0; i < tree->count; i++) {
		pg_status kind = pg_source_name_kind(tree->sources[i], prefix);

		if (kind == PG_CONFLICT)
			return PG_OK;
		if (kind == PG_OK)
			status = PG_CONFLICT;
	}
	return status;
}

/* Merge captured source selections without per-file lookups. */
pg_status pg_tree_files_depth(pg_tree *tree, const char *prefix,
		int recursive, pg_cursor **out, pg_error *error)
{
	pg_cursor *merged;
	pg_file **visible = NULL;
	const char **directories = NULL;
	size_t directory_count = 0;
	char *canonical = NULL;
	pg_status status;
	size_t required = 0, capacity = 0, i, j, kept = 0;

	if (!out)
		return pg_result(PG_INVALID, error);
	*out = NULL;
	if (!tree)
		return pg_result(PG_INVALID, error);
	status = pg_tree_control_status(tree, 0);
	if (status != PG_OK)
		return pg_result(status, error);
	status = pg_tree_native_reconcile(tree, error);
	if (status != PG_OK)
		return status;
	if (prefix && *prefix) {
		status = pg_name_normalize(tree->context, prefix,
			NULL, 0, &required, NULL);
		if (status != PG_CAPACITY)
			return pg_result(status, error);
		canonical = (char *)malloc(required);
		if (!canonical)
			return pg_result(PG_NOMEM, error);
		status = pg_name_normalize(tree->context, prefix,
			canonical, required, &required, NULL);
		if (status != PG_OK)
			goto fail_early;
	}
	if (!pg_tree_scope_covers_depth(tree, canonical, recursive)) {
		for (i = 0; i < tree->count; i++) {
			if (tree->sources[i]->format == PG_LOOSE) {
				status = PG_INVALID;
				goto fail_early;
			}
		}
	}
	status = pg_tree_prefix_check(tree, canonical);
	if (status != PG_OK)
		goto fail_early;
	status = pg_indexed_directories(tree->sources, tree->count,
		&directories, &directory_count);
	if (status != PG_OK)
		goto fail_early;
	merged = (pg_cursor *)calloc(1, sizeof(*merged));
	if (!merged) {
		status = PG_NOMEM;
		goto fail_early;
	}
	merged->context = tree->context;
	pg_context_child_add(merged->context);
	for (i = 0; i < tree->count; i++) {
		pg_cursor *source_cursor = NULL;
		pg_error work_error;

		status = pg_source_files_depth(tree->sources[i], canonical,
			recursive,
			&source_cursor, &work_error);
		if (status == PG_CONFLICT)
			continue;
		if (status != PG_OK) {
			if (error)
				*error = work_error;
			goto fail_files;
		}
		for (;;) {
			pg_file *candidate = NULL;
			pg_file **grown;
			size_t next;

			status = pg_cursor_next(source_cursor, &candidate,
				&work_error);
			if (status == PG_END) {
				status = PG_OK;
				break;
			}
			if (status != PG_OK)
				break;
			if (merged->count == capacity) {
				next = capacity ? capacity * 2 : 8;
				if (next < capacity ||
				    next > SIZE_MAX / sizeof(*grown)) {
					pg_file_close(&candidate, NULL);
					status = PG_LIMIT;
					break;
				}
				grown = (pg_file **)realloc(merged->files,
					next * sizeof(*grown));
				if (!grown) {
					pg_file_close(&candidate, NULL);
					status = PG_NOMEM;
					break;
				}
				merged->files = grown;
				capacity = next;
			}
			merged->files[merged->count++] = candidate;
		}
		pg_cursor_close(&source_cursor, NULL);
		if (status != PG_OK)
			goto fail_files;
	}
	if (merged->count > 1)
		qsort(merged->files, merged->count, sizeof(*merged->files),
			pg_tree_file_compare);
	visible = (pg_file **)calloc(merged->count ? merged->count : 1,
		sizeof(*visible));
	if (!visible) {
		status = PG_NOMEM;
		goto fail_files;
	}
	for (i = 0; i < merged->count;) {
		size_t next = i + 1;
		pg_file *winner = merged->files[i];
		const char *name = winner->info.canonical_name;

		while (next < merged->count && strcmp(name,
			merged->files[next]->info.canonical_name) == 0) {
			pg_file *other = merged->files[next];
			size_t winner_rank = 0, other_rank = 0;

			for (j = 0; j < tree->count; j++) {
				if (tree->sources[j] == winner->source)
					winner_rank = j;
				if (tree->sources[j] == other->source)
					other_rank = j;
			}
			if (other_rank > winner_rank)
				winner = other;
			next++;
		}
		for (j = i; j < next; j++) {
			if (merged->files[j] != winner)
				pg_file_close(&merged->files[j], NULL);
		}
		name = winner->info.canonical_name;
		if (pg_directory_names_contain(directories, directory_count,
			name) ||
		    pg_tree_items_directory(merged->files + next,
			merged->count - next, name)) {
			for (j = i; j < next; j++) {
				if (merged->files[j] == winner)
					pg_file_close(&merged->files[j], NULL);
			}
		} else {
			status = pg_tree_retain(tree);
			if (status != PG_OK)
				goto fail_files;
			winner->origin_tree = tree;
			visible[kept++] = winner;
			for (j = i; j < next; j++) {
				if (merged->files[j] == winner)
					merged->files[j] = NULL;
			}
		}
		i = next;
	}
	free(merged->files);
	merged->files = visible;
	visible = NULL;
	merged->count = kept;
	free(canonical);
	free(directories);
	*out = merged;
	return pg_result(PG_OK, error);

fail_files:
	for (j = 0; j < kept; j++)
		pg_file_close(&visible[j], NULL);
	free(visible);
	pg_cursor_close(&merged, NULL);
fail_early:
	free(canonical);
	free(directories);
	return pg_result(status, error);
}

/* Refresh and retain a subtree as an explicit tree watch scope. */
pg_status pg_tree_discover(pg_tree *tree, const char *prefix,
		uint32_t depth, pg_error *error)
{
	pg_tree_scope *scope = NULL;
	pg_tree_source_state *saved = NULL;
	pg_cursor *snapshot = NULL;
	char *canonical = NULL;
	size_t required = 0, i;
	pg_status status;
	int reconciling;
	int created = 0;

	if (!tree || depth > PG_DISCOVER_RECURSIVE)
		return pg_result(PG_INVALID, error);
	status = pg_tree_control_status(tree, 1);
	if (status != PG_OK)
		return pg_result(status, error);
	if (prefix && *prefix) {
		status = pg_name_normalize(tree->context, prefix,
			NULL, 0, &required, NULL);
		if (status != PG_CAPACITY)
			return pg_result(status, error);
		canonical = (char *)malloc(required);
		if (!canonical)
			return pg_result(PG_NOMEM, error);
		status = pg_name_normalize(tree->context, prefix,
			canonical, required, &required, NULL);
		if (status != PG_OK)
			goto request_done;
	}
	status = pg_tree_sources_save_mode(tree, &saved, 1);
	if (status != PG_OK)
		goto request_done;
	for (i = 0; i < tree->count; i++) {
		status = pg_source_discover_tree(tree->sources[i],
			canonical, depth, error);
		if (status != PG_OK)
			goto request_done;
	}
	status = pg_tree_prefix_check(tree, canonical);
	if (status != PG_OK) {
		pg_result(status, error);
		goto request_done;
	}
	for (scope = tree->scopes; scope; scope = scope->next) {
		if (!scope->exact &&
		    scope->shallow == (depth == PG_DISCOVER_CHILDREN) &&
			((!scope->prefix && !canonical) ||
		    (scope->prefix && canonical &&
		     strcmp(scope->prefix, canonical) == 0)))
			break;
	}
	if (!scope) {
		scope = (pg_tree_scope *)calloc(1, sizeof(*scope));
		if (!scope) {
			status = PG_NOMEM;
			goto request_done;
		}
		scope->shallow = depth == PG_DISCOVER_CHILDREN;
		scope->prefix = canonical;
		canonical = NULL;
		scope->next = tree->scopes;
		tree->scopes = scope;
		created = 1;
	}
	if (tree->watch_mode) {
		reconciling = tree->reconciling;
		tree->reconciling = 1;
		status = pg_tree_scope_snapshot(tree, scope, &snapshot, error);
		tree->reconciling = reconciling;
	}
	if (status == PG_OK) {
		pg_cursor_close(&scope->baseline, NULL);
		scope->baseline = snapshot;
		snapshot = NULL;
	}
request_done:
	if (status != PG_OK && saved)
		pg_tree_sources_restore(tree, saved);
	if (status != PG_OK && created) {
		tree->scopes = scope->next;
		free(scope->prefix);
		free(scope);
	}
	pg_tree_sources_discard(tree, saved);
	pg_cursor_close(&snapshot, NULL);
	free(canonical);
	return status == PG_OK ? pg_result(PG_OK, error) : status;
}

PG_API pg_status PG_CALL pg_tree_request_subtree(pg_tree *tree,
		const char *prefix, pg_error *error)
{
	return pg_tree_discover(tree, prefix, PG_DISCOVER_RECURSIVE, error);
}

static void pg_tree_records_free(pg_source_record *record)
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

static pg_source_record *pg_tree_records_clone(pg_source_record *source)
{
	pg_source_record *head = NULL;
	pg_source_record **tail = &head;

	for (; source; source = source->next) {
		pg_source_record *copy = (pg_source_record *)calloc(1,
			sizeof(*copy));

		if (!copy)
			goto clone_failed;
		*copy = *source;
		copy->next = NULL;
		copy->info.canonical_name = strdup(
			source->info.canonical_name);
		copy->info.original_name = strdup(
			source->info.original_name);
		copy->info.cached_header = NULL;
		copy->native_path = source->native_path ?
			strdup(source->native_path) : NULL;
		if (source->info.cached_header_size) {
			copy->info.cached_header = malloc(
				source->info.cached_header_size);
			if (copy->info.cached_header)
				memcpy((void *)copy->info.cached_header,
					source->info.cached_header,
					source->info.cached_header_size);
		}
		*tail = copy;
		tail = &copy->next;
		if (!copy->info.canonical_name ||
		    !copy->info.original_name ||
		    (source->native_path && !copy->native_path) ||
		    (source->info.cached_header_size &&
		     !copy->info.cached_header))
			goto clone_failed;
	}
	return head;

clone_failed:
	pg_tree_records_free(head);
	return NULL;
}

static void pg_tree_requests_free(pg_source_request *request)
{
	while (request) {
		pg_source_request *next = request->next;

		free(request->name);
		free(request);
		request = next;
	}
}

static pg_source_request *pg_tree_requests_clone(
		pg_source_request *request)
{
	pg_source_request *head = NULL;
	pg_source_request **tail = &head;

	for (; request; request = request->next) {
		pg_source_request *copy = (pg_source_request *)calloc(1,
			sizeof(*copy));

		if (!copy)
			goto clone_failed;
		copy->name = strdup(request->name);
		if (!copy->name) {
			free(copy);
			goto clone_failed;
		}
		*tail = copy;
		tail = &copy->next;
	}
	return head;

clone_failed:
	pg_tree_requests_free(head);
	return NULL;
}

void pg_tree_sources_discard(pg_tree *tree,
		pg_tree_source_state *saved)
{
	if (!saved)
		return;
	for (size_t i = 0; i < tree->count; i++) {
		pg_tree_records_free(saved[i].records);
		pg_tree_requests_free(saved[i].exact_requests);
		pg_tree_requests_free(saved[i].prefix_requests);
		pg_tree_requests_free(saved[i].shallow_requests);
	}
	free(saved);
}

static pg_status pg_tree_sources_save_mode(pg_tree *tree,
		pg_tree_source_state **out, int loose_only)
{
	pg_tree_source_state *saved = (pg_tree_source_state *)calloc(
		tree->count ? tree->count : 1, sizeof(*saved));

	*out = NULL;
	if (!saved)
		return PG_NOMEM;
	for (size_t i = 0; i < tree->count; i++) {
		pg_source *source = tree->sources[i];

		if (loose_only && source->format != PG_LOOSE)
			continue;
		saved[i].captured = 1;
		saved[i].identity = source->identity;
		saved[i].generation = source->generation;
		saved[i].loose_root_requested =
			source->loose_root_requested;
		if (source->records) {
			saved[i].records = pg_tree_records_clone(
				source->records);
			if (!saved[i].records)
				goto save_failed;
		}
		if (source->exact_requests) {
			saved[i].exact_requests = pg_tree_requests_clone(
				source->exact_requests);
			if (!saved[i].exact_requests)
				goto save_failed;
		}
		if (source->shallow_requests) {
			saved[i].shallow_requests = pg_tree_requests_clone(
				source->shallow_requests);
			if (!saved[i].shallow_requests)
				goto save_failed;
		}
		if (source->prefix_requests) {
			saved[i].prefix_requests = pg_tree_requests_clone(
				source->prefix_requests);
			if (!saved[i].prefix_requests)
				goto save_failed;
		}
	}
	*out = saved;
	return PG_OK;

save_failed:
	pg_tree_sources_discard(tree, saved);
	return PG_NOMEM;
}

pg_status pg_tree_sources_save(pg_tree *tree,
		pg_tree_source_state **out)
{
	return pg_tree_sources_save_mode(tree, out, 0);
}

void pg_tree_sources_restore(pg_tree *tree,
		pg_tree_source_state *saved)
{
	for (size_t i = 0; i < tree->count; i++) {
		pg_source *source = tree->sources[i];

		if (!saved[i].captured)
			continue;
		pg_tree_records_free(source->records);
		pg_tree_requests_free(source->exact_requests);
		pg_tree_requests_free(source->prefix_requests);
		pg_tree_requests_free(source->shallow_requests);
		source->records = saved[i].records;
		source->exact_requests = saved[i].exact_requests;
		source->prefix_requests = saved[i].prefix_requests;
		source->shallow_requests = saved[i].shallow_requests;
		source->identity = saved[i].identity;
		source->generation = saved[i].generation;
		source->loose_root_requested =
			saved[i].loose_root_requested;
		saved[i].records = NULL;
		saved[i].exact_requests = NULL;
		saved[i].prefix_requests = NULL;
		saved[i].shallow_requests = NULL;
	}
}

static pg_status pg_tree_refresh_scope(pg_source *source, pg_tree_scope *scope,
		pg_error *error)
{
	if (scope->exact) {
		if (!scope->reader_refs && !scope->managed)
			return PG_OK;
		return pg_source_refresh_name(source, scope->prefix, 0, error);
	}
	return pg_source_discover_tree(source, scope->prefix,
		scope->shallow ? PG_DISCOVER_CHILDREN : PG_DISCOVER_RECURSIVE,
		error);
}

/* Refresh requested tree scopes or all source observations. */
static pg_status pg_tree_rescan_mode(pg_tree *tree, pg_error *error,
		int scopes_only)
{
	pg_status status;
	pg_tree_source_state *saved = NULL;
	size_t i;

	if (!tree)
		return pg_result(PG_INVALID, error);
	status = pg_tree_control_status(tree, 1);
	if (status != PG_OK)
		return pg_result(status, error);
	status = pg_tree_sources_save(tree, &saved);
	if (status != PG_OK)
		return pg_result(status, error);
	for (i = 0; i < tree->count; i++) {
		pg_source *source = tree->sources[i];

		if (source->format == PG_LOOSE && scopes_only) {
			pg_tree_scope *scope;

			for (scope = tree->scopes; scope;
			     scope = scope->next) {
				status = pg_tree_refresh_scope(source, scope,
					error);
				if (status != PG_OK)
					break;
			}
		} else {
			status = pg_source_rescan(source, error);
		}
		if (status != PG_OK) {
			pg_tree_sources_restore(tree, saved);
			goto rescan_done;
		}
	}

rescan_done:
	pg_tree_sources_discard(tree, saved);
	if (status != PG_OK && (!error || error->status != status))
		pg_result(status, error);
	return status == PG_OK ? pg_result(PG_OK, error) : status;
}

pg_status pg_tree_rescan_scopes(pg_tree *tree, pg_error *error)
{
	return pg_tree_rescan_mode(tree, error, 1);
}

PG_API pg_status PG_CALL pg_tree_rescan(pg_tree *tree, pg_error *error)
{
	return pg_tree_rescan_mode(tree, error, 0);
}

/* Release attachments and the context reference. */
PG_API pg_status PG_CALL pg_tree_close(
		pg_tree **tree,
		pg_error *error)
{
	pg_tree *owned;
	pg_status status = PG_OK;
	int native_code = 0;

	if (!tree)
		return pg_result(PG_INVALID, error);
	if (!*tree)
		return pg_result(PG_OK, error);
	owned = *tree;
	status = pg_tree_control_status(owned, 1);
	if (status != PG_OK)
		return pg_result(status, error);
	*tree = NULL;
	pg_tree_scope_lock(owned);
	owned->closed = 1;
	pg_tree_unwatch(owned, NULL);
	while (owned->scopes) {
		pg_tree_scope *scope = owned->scopes;

		owned->scopes = scope->next;
		pg_cursor_close(&scope->baseline, NULL);
		free(scope->prefix);
		free(scope);
	}
	pg_tree_scope_unlock(owned);
	pg_tree_manage_discard(owned);
	while (owned->count) {
		pg_source *source = owned->sources[--owned->count];
		int code = 0;
		pg_status released;

		source->attached = NULL;
		released = pg_source_release(source, &code);
		if (status == PG_OK && released != PG_OK) {
			status = released;
			native_code = code;
		}
	}
	pg_context_child_drop(owned->context);
	free(owned->sources);
	pg_tree_release(owned);
	return pg_native_result(status, native_code, error);
}

} /* extern "C" */
