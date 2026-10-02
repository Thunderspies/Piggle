#include <piggle/piggle.h>
#include "internal.hpp"

struct pg_entry_item {
	pg_entry_info info;
	pg_file *file;
	size_t rank;
	int explicit_entry;
};

struct pg_entry_cursor {
	pg_context *context;
	pg_source **sources;
	size_t source_count;
	pg_entry_item *items;
	size_t count;
	size_t capacity;
	size_t position;
};

static int pg_directory_compare(const void *left, const void *right)
{
	return strcmp(*(const char *const *)left,
		*(const char *const *)right);
}

pg_status pg_indexed_directories(pg_source **sources, size_t count,
		const char *prefix, const char ***out, size_t *size)
{
	size_t total = 0;

	*out = NULL;
	*size = 0;
	for (size_t i = 0; i < count; i++) {
		pg_source_record **records;
		size_t scoped_count;
		pg_status status = pg_source_records_scope(sources[i], prefix,
			&records, &scoped_count);

		if (status != PG_OK)
			return status;
		for (size_t j = 0; j < scoped_count; j++) {
			pg_source_record *r = records[j];

			if (!S_ISDIR(r->identity.st_mode))
				continue;
			if (total == SIZE_MAX / sizeof(**out))
				return PG_LIMIT;
			total++;
		}
	}
	if (!total)
		return PG_OK;
	const char **names = (const char **)malloc(total * sizeof(*names));

	if (!names)
		return PG_NOMEM;
	for (size_t i = 0; i < count; i++) {
		pg_source_record **records;
		size_t scoped_count;

		pg_status status = pg_source_records_scope(sources[i], prefix,
			&records, &scoped_count);

		if (status != PG_OK) {
			free(names);
			*size = 0;
			return status;
		}
		for (size_t j = 0; j < scoped_count; j++) {
			pg_source_record *r = records[j];

			if (S_ISDIR(r->identity.st_mode))
				names[(*size)++] = r->info.canonical_name;
		}
	}
	qsort(names, total, sizeof(*names), pg_directory_compare);
	*out = names;
	return PG_OK;
}

int pg_directory_names_contain(const char **names, size_t count,
		const char *name)
{
	size_t low = 0;

	while (low < count) {
		size_t middle = low + (count - low) / 2;
		int order = strcmp(names[middle], name);

		if (!order)
			return 1;
		if (order < 0)
			low = middle + 1;
		else
			count = middle;
	}
	return 0;
}

static int pg_entry_in_prefix(const char *name, const char *prefix,
		uint32_t flags)
{
	const char *relative = name;

	if (prefix) {
		size_t size = strlen(prefix);

		if (strncmp(name, prefix, size) || name[size] != '/')
			return 0;
		relative = name + size + 1;
	}
	return (flags & PG_ENTRIES_RECURSIVE) || !strchr(relative, '/');
}

static pg_status pg_entry_add(pg_entry_cursor *cursor,
		const pg_entry_info *info, pg_file *file, size_t rank,
		int explicit_entry)
{
	if (cursor->count == cursor->capacity) {
		size_t capacity = cursor->capacity ? cursor->capacity * 2 : 16;

		if (capacity < cursor->capacity ||
		    capacity > SIZE_MAX / sizeof(*cursor->items))
			return PG_LIMIT;
		pg_entry_item *items = (pg_entry_item *)realloc(cursor->items,
			capacity * sizeof(*items));

		if (!items)
			return PG_NOMEM;
		cursor->items = items;
		cursor->capacity = capacity;
	}
	pg_entry_item item = {};

	item.info = *info;
	item.info.canonical_name = strdup(info->canonical_name);
	item.info.original_name = strdup(info->original_name);
	if (!item.info.canonical_name || !item.info.original_name) {
		free((void *)item.info.canonical_name);
		free((void *)item.info.original_name);
		return PG_NOMEM;
	}
	item.file = file;
	item.rank = rank;
	item.explicit_entry = explicit_entry;
	cursor->items[cursor->count++] = item;
	return PG_OK;
}

static void pg_entry_dispose(pg_entry_item *item)
{
	free((void *)item->info.canonical_name);
	free((void *)item->info.original_name);
	pg_file_close(&item->file, NULL);
	memset(item, 0, sizeof(*item));
}

static int pg_entry_compare(const void *left, const void *right)
{
	const pg_entry_item *a = (const pg_entry_item *)left;
	const pg_entry_item *b = (const pg_entry_item *)right;
	int order = strcmp(a->info.canonical_name, b->info.canonical_name);

	if (order)
		return order;
	if (a->info.kind != b->info.kind)
		return a->info.kind < b->info.kind ? -1 : 1;
	if (a->rank != b->rank)
		return a->rank < b->rank ? -1 : 1;
	if (a->explicit_entry != b->explicit_entry)
		return a->explicit_entry ? 1 : -1;
	return strcmp(a->info.original_name, b->info.original_name);
}

static pg_status pg_entry_directories(pg_entry_cursor *cursor,
		pg_source *source, size_t rank, const char *prefix,
			uint32_t flags)
{
	pg_source_record **records;
	size_t count;
	pg_status status = pg_source_records_scope(source, prefix,
		&records, &count);

	if (status != PG_OK)
		return status;
	for (size_t index = 0; index < count && status == PG_OK; index++) {
		pg_source_record *record = records[index];
		pg_entry_info info = {};
		char *name = strdup(record->info.canonical_name);

		if (!name) {
			status = PG_NOMEM;
			break;
		}
		info.kind = PG_ENTRY_DIRECTORY;
		info.source_id = source->id;
		info.attributes = PG_ENTRY_IMPLIED |
			(source->access == PG_READ ? PG_ENTRY_READ_ONLY : 0);
		info.canonical_name = info.original_name = name;
		for (char *slash = strchr(name, '/'); slash;
		     slash = strchr(slash + 1, '/')) {
			size_t length = (size_t)(slash - name);
			const char *previous = index ?
				records[index - 1]->info.canonical_name : "";

			if (!strncmp(name, previous, length) &&
			    (previous[length] == '/' || !previous[length]))
				continue;
			*slash = 0;
			if (pg_entry_in_prefix(name, prefix, flags))
				status = pg_entry_add(cursor, &info, NULL, rank,
					0);
			*slash = '/';
			if (status != PG_OK)
				break;
		}
		if (status == PG_OK && S_ISDIR(record->identity.st_mode) &&
		    pg_entry_in_prefix(name, prefix, flags)) {
			info.original_name = record->info.original_name;
			info.attributes = record->attributes;
			info.mtime = record->info.mtime;
			status = pg_entry_add(cursor, &info, NULL, rank, 1);
		}
		free(name);
	}
	return status;
}

static pg_status pg_entries_capture(pg_context *context, pg_source **sources,
		size_t source_count, pg_cursor *files, const char *prefix,
		uint32_t flags, pg_entry_cursor **out, pg_error *error)
{
	pg_entry_cursor *cursor = NULL;
	char *canonical = NULL;
	pg_status status = PG_OK;
	size_t required = 0;

	if (prefix && *prefix) {
		status = pg_name_normalize(context, prefix, NULL, 0,
			&required, NULL);
		if (status != PG_CAPACITY)
			return pg_result(status, error);
		canonical = (char *)malloc(required);
		if (!canonical)
			return pg_result(PG_NOMEM, error);
		status = pg_name_normalize(context, prefix, canonical,
			required, &required, NULL);
		if (status != PG_OK)
			goto done;
	}
	if (pg_context_children(context) == SIZE_MAX ||
	    source_count > SIZE_MAX / sizeof(*sources)) {
		status = PG_LIMIT;
		goto done;
	}
	cursor = (pg_entry_cursor *)calloc(1, sizeof(*cursor));
	if (!cursor) {
		status = PG_NOMEM;
		goto done;
	}
	cursor->context = context;
	pg_context_child_add(context);
	cursor->sources = (pg_source **)calloc(source_count ? source_count : 1,
		sizeof(*sources));
	if (!cursor->sources) {
		status = PG_NOMEM;
		goto done;
	}
	for (size_t i = 0; i < source_count; i++) {
		status = pg_source_retain(sources[i]);
		if (status != PG_OK)
			goto done;
		cursor->sources[cursor->source_count++] = sources[i];
		status = pg_entry_directories(cursor, sources[i], i,
			canonical, flags);
		if (status != PG_OK)
			goto done;
	}
	for (;;) {
		pg_file *file = NULL;
		pg_entry_info info = {};

		status = pg_cursor_next(files, &file, NULL);
		if (status == PG_END) {
			status = PG_OK;
			break;
		}
		if (status != PG_OK)
			goto done;
		if (!pg_entry_in_prefix(file->info.canonical_name,
			canonical, flags)) {
			pg_file_close(&file, NULL);
			continue;
		}
		info.kind = PG_ENTRY_FILE;
		info.source_id = file->info.source_id;
		info.canonical_name = file->info.canonical_name;
		info.original_name = file->info.original_name;
		info.size = file->info.logical_size;
		info.mtime = file->info.mtime;
		info.attributes = file->attributes;
		if (file->source->format != PG_LOOSE &&
		    file->source->access == PG_READ)
			info.attributes |= PG_ENTRY_READ_ONLY;
		status = pg_entry_add(cursor, &info, file, 0, 1);
		if (status != PG_OK) {
			pg_file_close(&file, NULL);
			goto done;
		}
	}
	if (cursor->count > 1)
		qsort(cursor->items, cursor->count, sizeof(*cursor->items),
			pg_entry_compare);
	{
		size_t kept = 0;

		for (size_t i = 0; i < cursor->count;) {
			size_t next = i + 1;

			while (next < cursor->count &&
			       !strcmp(cursor->items[i].info.canonical_name,
				cursor->items[next].info.canonical_name))
				next++;
			for (size_t j = i; j + 1 < next; j++)
				pg_entry_dispose(&cursor->items[j]);
			cursor->items[kept++] = cursor->items[next - 1];
			i = next;
		}
		cursor->count = kept;
	}
	*out = cursor;
	cursor = NULL;
done:
	free(canonical);
	pg_entry_cursor_close(&cursor, NULL);
	return pg_result(status, error);
}

static pg_status PG_CALL pg_source_entries_coordinated(pg_source *source,
		const char *prefix, uint32_t flags, pg_entry_cursor **out,
		pg_error *error)
{
	/* Validate coverage through source_files, capture directories. */
	if (out)
		*out = NULL;
	if (!out || !source || (flags & ~PG_ENTRIES_RECURSIVE))
		return pg_result(PG_INVALID, error);
	pg_cursor *files = NULL;
	pg_status status = pg_source_files_depth(source, prefix,
		(flags & PG_ENTRIES_RECURSIVE) != 0, &files, error);

	if (status != PG_OK)
		return status;
	/* Merge canonical entries and retain snapshot ownership. */
	status = pg_entries_capture(source->context, &source, 1, files,
		prefix, flags, out, error);
	pg_cursor_close(&files, NULL);
	return status;
}

PG_API pg_status PG_CALL pg_source_entries(pg_source *source,
		const char *prefix, uint32_t flags, pg_entry_cursor **out,
		pg_error *error)
{
	pg_context *context = source ? source->context : NULL;

	pg_context_lock(context);
	pg_status status = pg_source_entries_coordinated(source, prefix,
		flags, out, error);

	pg_context_unlock(context);
	return status;
}

static pg_status PG_CALL pg_tree_entries_coordinated(pg_tree *tree,
		const char *prefix, uint32_t flags, pg_entry_cursor **out,
		pg_error *error)
{
	/* Validate coverage through tree_files, capture source directories. */
	if (out)
		*out = NULL;
	if (!out || !tree || (flags & ~PG_ENTRIES_RECURSIVE))
		return pg_result(PG_INVALID, error);
	pg_cursor *files = NULL;
	pg_status status = pg_tree_files_depth(tree, prefix,
		(flags & PG_ENTRIES_RECURSIVE) != 0, &files, error);

	if (status != PG_OK)
		return status;
	/* Resolve directory metadata and apply immediate/recursive filter. */
	status = pg_entries_capture(tree->context, tree->sources, tree->count,
		files, prefix, flags, out, error);
	pg_cursor_close(&files, NULL);
	return status;
}

PG_API pg_status PG_CALL pg_tree_entries(pg_tree *tree,
		const char *prefix, uint32_t flags, pg_entry_cursor **out,
		pg_error *error)
{
	pg_context *context = tree ? tree->context : NULL;

	pg_context_lock(context);
	pg_status status = pg_tree_entries_coordinated(tree, prefix, flags,
		out, error);

	pg_context_unlock(context);
	return status;
}

PG_API pg_status PG_CALL pg_entry_cursor_next(pg_entry_cursor *cursor,
		pg_entry_info *out, pg_file **file, pg_error *error)
{
	/* Transfer captured selection without consulting live source state. */
	if (out)
		memset(out, 0, sizeof(*out));
	if (file)
		*file = NULL;
	if (!cursor || !out || !file)
		return pg_result(PG_INVALID, error);
	if (cursor->position == cursor->count)
		return pg_result(PG_END, error);
	pg_entry_item *item = &cursor->items[cursor->position++];

	*out = item->info;
	*file = item->file;
	item->file = NULL;
	return pg_result(PG_OK, error);
}

static pg_status PG_CALL pg_entry_cursor_close_coordinated(
		pg_entry_cursor **cursor,
		pg_error *error)
{
	/* Consume ownership and release all captured resources. */
	if (!cursor)
		return pg_result(PG_INVALID, error);
	if (!*cursor)
		return pg_result(PG_OK, error);
	pg_entry_cursor *owned = *cursor;

	*cursor = NULL;
	pg_status status = PG_OK;
	int native_code = 0;

	for (size_t i = 0; i < owned->count; i++)
		pg_entry_dispose(&owned->items[i]);
	for (size_t i = 0; i < owned->source_count; i++) {
		int code = 0;
		pg_status released = pg_source_release(owned->sources[i],
			&code);

		if (status == PG_OK && released != PG_OK) {
			status = released;
			native_code = code;
		}
	}
	free(owned->items);
	free(owned->sources);
	pg_context_child_drop(owned->context);
	free(owned);
	return pg_native_result(status, native_code, error);
}

PG_API pg_status PG_CALL pg_entry_cursor_close(pg_entry_cursor **cursor,
		pg_error *error)
{
	pg_context *context = cursor && *cursor ? (*cursor)->context : NULL;

	pg_context_lock(context);
	pg_status status = pg_entry_cursor_close_coordinated(cursor, error);

	pg_context_unlock(context);
	return status;
}

/* Capture directory metadata in internal cursors used by the visible feed. */
pg_status pg_tree_entry_snapshot(pg_tree *tree, pg_tree_scope *scope,
		pg_cursor **out, pg_error *error)
{
	pg_entry_cursor *entries = NULL;
	pg_cursor *cursor = NULL;
	pg_status status = pg_tree_entries(tree, scope->prefix,
		scope->shallow ? 0 : PG_ENTRIES_RECURSIVE, &entries, error);

	*out = NULL;
	if (status != PG_OK)
		return status;
	cursor = (pg_cursor *)calloc(1, sizeof(*cursor));
	if (!cursor) {
		pg_entry_cursor_close(&entries, NULL);
		return pg_result(PG_NOMEM, error);
	}
	cursor->context = tree->context;
	pg_context_child_add(cursor->context);
	cursor->files = (pg_file **)calloc(entries->count ? entries->count : 1,
		sizeof(*cursor->files));
	if (!cursor->files) {
		status = PG_NOMEM;
		goto done;
	}
	for (size_t i = 0; i < entries->count; i++) {
		pg_entry_item *item = &entries->items[i];
		pg_file *file = item->file;

		if (file) {
			item->file = NULL;
		} else {
			pg_source *source = NULL;

			for (size_t j = 0; j < tree->count; j++)
				if (tree->sources[j]->id ==
				    item->info.source_id)
					source = tree->sources[j];
			file = (pg_file *)calloc(1, sizeof(*file));
			if (!file) {
				status = PG_NOMEM;
				goto done;
			}
			status = pg_source_retain(source);
			if (status != PG_OK) {
				free(file);
				goto done;
			}
			file->source = source;
			file->attributes = item->info.attributes;
			file->source_identity.st_mode = S_IFDIR;
			file->info.source_id = source->id;
			file->info.canonical_name =
				strdup(item->info.canonical_name);
			file->info.original_name =
				strdup(item->info.original_name);
			file->info.mtime = item->info.mtime;
			if (!file->info.canonical_name ||
				!file->info.original_name) {
				pg_file_close(&file, NULL);
				status = PG_NOMEM;
				goto done;
			}
		}
		cursor->files[cursor->count++] = file;
	}
	*out = cursor;
	cursor = NULL;
done:
	pg_cursor_close(&cursor, NULL);
	pg_entry_cursor_close(&entries, NULL);
	return pg_result(status, error);
}
