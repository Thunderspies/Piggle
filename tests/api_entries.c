#include "api_test.h"
#ifdef _WIN32
#include <windows.h>
#define remove_dir(path) _rmdir(path)
#else
#include <unistd.h>
#define remove_dir(path) rmdir(path)
#endif

static int next(pg_entry_cursor *cursor, const char *name, uint32_t kind,
		int implied)
{
	pg_entry_info info;
	pg_file *file = NULL;
	pg_error error;

	STATUS(pg_entry_cursor_next(cursor, &info, &file, &error), PG_OK);
	CHECK(info.kind == kind && strcmp(info.canonical_name, name) == 0);
	CHECK(info.source_id != 0 && info.original_name != NULL);
	CHECK(!!(info.attributes & PG_ENTRY_IMPLIED) == implied);
	if (kind == PG_ENTRY_FILE) {
		unsigned char data[16];
		size_t size;
		CHECK(file != NULL && info.size == sizeof(payload));
		STATUS(pg_file_read_all(file, data, sizeof(data), &size,
			&error), PG_OK);
		CHECK(size == sizeof(payload) && !memcmp(data, payload, size));
		STATUS(pg_file_close(&file, &error), PG_OK);
	} else {
		CHECK(file == NULL && info.size == 0);
		if (implied)
			CHECK(info.mtime == 0);
	}
	return 0;
}

static int end(pg_entry_cursor *cursor)
{
	pg_entry_info info;
	pg_file *file = (pg_file *)1;
	pg_error error;

	memset(&info, 0xa5, sizeof(info));
	STATUS(pg_entry_cursor_next(cursor, &info, &file, &error), PG_END);
	CHECK(!file && !info.kind && !info.canonical_name);
	return 0;
}

static int loose_case(int tree_mode)
{
	pg_context *context = NULL;
	pg_source *source = NULL, *base = NULL;
	pg_tree *tree = NULL;
	pg_entry_cursor *cursor = NULL, *saved = NULL;
	pg_cursor *files = NULL;
	pg_file *file = NULL;
	pg_error error;

	CHECK(directory("root") == 0);
	CHECK(directory("root/Empty") == 0);
	CHECK(directory("root/Nested") == 0);
	CHECK(directory("root/Nested/Void") == 0);
	CHECK(put_bytes("root/Nested/File", payload, sizeof(payload)) == 0);
	CHECK(put_bytes("root/a", payload, sizeof(payload)) == 0);
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_source_open(context, "root", NULL, &source, &error), PG_OK);
	STATUS(pg_source_entries(source, NULL, 0, &cursor, &error), PG_INVALID);
	CHECK(cursor == NULL);
	STATUS(pg_source_request_subtree(source, NULL, &error), PG_OK);
	STATUS(pg_source_entries(source, NULL, 2, &cursor, &error), PG_INVALID);
	if (tree_mode) {
		CHECK(directory("base") == 0);
		CHECK(put_bytes("base/empty", payload, sizeof(payload)) == 0);
		STATUS(pg_source_open(context, "base", NULL, &base,
			&error), PG_OK);
		STATUS(pg_tree_create(context, &tree, &error), PG_OK);
		STATUS(pg_tree_attach(tree, base, &error), PG_OK);
		STATUS(pg_tree_attach(tree, source, &error), PG_OK);
		STATUS(pg_tree_entries(tree, NULL, 0, &cursor,
			&error), PG_INVALID);
		STATUS(pg_tree_request_subtree(tree, NULL, &error), PG_OK);
		STATUS(pg_tree_entries(tree, NULL, 0, &cursor, &error), PG_OK);
		STATUS(pg_tree_find(tree, "empty", &file,
			&error), PG_CONFLICT);
		STATUS(pg_tree_files(tree, NULL, &files, &error), PG_OK);
	} else {
		STATUS(pg_source_entries(source, NULL, 0, &cursor,
			&error), PG_OK);
		STATUS(pg_source_find(source, "empty", &file,
			&error), PG_CONFLICT);
		STATUS(pg_source_files(source, NULL, &files, &error), PG_OK);
	}
	CHECK(next(cursor, "a", PG_ENTRY_FILE, 0) == 0);
	CHECK(next(cursor, "empty", PG_ENTRY_DIRECTORY, 0) == 0);
	CHECK(next(cursor, "nested", PG_ENTRY_DIRECTORY, 0) == 0);
	CHECK(end(cursor) == 0);
	STATUS(pg_entry_cursor_close(&cursor, &error), PG_OK);
	for (int i = 0; i < 2; i++) {
		STATUS(pg_cursor_next(files, &file, &error), PG_OK);
		STATUS(pg_file_close(&file, &error), PG_OK);
	}
	STATUS(pg_cursor_next(files, &file, &error), PG_END);
	STATUS(pg_cursor_close(&files, &error), PG_OK);
	STATUS(pg_source_entries(source, "nested", PG_ENTRIES_RECURSIVE,
		&saved, &error), PG_OK);
	pg_entry_cursor *names = NULL;
	pg_entry_info captured;
	STATUS(pg_source_entries(source, "nested", 0, &names, &error), PG_OK);
	STATUS(pg_entry_cursor_next(names, &captured, &file, &error), PG_OK);
	STATUS(pg_file_close(&file, &error), PG_OK);
	CHECK(remove_dir("root/Nested/Void") == 0);
	STATUS(pg_source_rescan(source, &error), PG_OK);
	/* Metadata spans survive transferred-file cleanup and index
	 * replacement. */
	CHECK(!strcmp(captured.canonical_name, "nested/file"));
	CHECK(!strcmp(captured.original_name, "Nested/File"));
	CHECK(captured.size == sizeof(payload));
	STATUS(pg_entry_cursor_close(&names, &error), PG_OK);
	CHECK(next(saved, "nested/file", PG_ENTRY_FILE, 0) == 0);
	CHECK(next(saved, "nested/void", PG_ENTRY_DIRECTORY, 0) == 0);
	CHECK(end(saved) == 0);
	STATUS(pg_entry_cursor_close(&saved, &error), PG_OK);
	STATUS(pg_source_entries(source, "nested", 0, &cursor, &error), PG_OK);
	CHECK(next(cursor, "nested/file", PG_ENTRY_FILE, 0) == 0);
	CHECK(end(cursor) == 0);
	STATUS(pg_entry_cursor_close(&cursor, &error), PG_OK);
	STATUS(pg_source_entries(source, "a", 0, &cursor,
		&error), PG_CONFLICT);
	STATUS(pg_source_entries(source, "missing", 0, &cursor, &error), PG_OK);
	CHECK(end(cursor) == 0);
	STATUS(pg_entry_cursor_close(&cursor, &error), PG_OK);
	STATUS(pg_tree_close(&tree, &error), PG_OK);
	STATUS(pg_source_close(&base, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static int archive_case(uint32_t format)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_archive_builder *builder = NULL;
	pg_entry_cursor *cursor = NULL;
	pg_file *file = NULL;
	pg_entry_info info;
	pg_error error;

	CHECK(clear_file("archive") == 0);
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_archive_builder_create(context, "archive", format, 0,
		&builder, &error), PG_OK);
	STATUS(pg_archive_builder_write_all(builder, "Dir/Sub/File", payload,
		sizeof(payload), NULL, &error), PG_OK);
	STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	STATUS(pg_source_open(context, "archive", NULL, &source, &error),
		PG_OK);
	STATUS(pg_source_entries(source, NULL, PG_ENTRIES_RECURSIVE,
		&cursor, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_BUSY);
	CHECK(next(cursor, "dir", PG_ENTRY_DIRECTORY, 1) == 0);
	CHECK(next(cursor, "dir/sub", PG_ENTRY_DIRECTORY, 1) == 0);
	STATUS(pg_entry_cursor_next(cursor, &info, &file, &error), PG_OK);
	CHECK(file && !strcmp(info.canonical_name, "dir/sub/file"));
	CHECK(info.attributes & PG_ENTRY_READ_ONLY);
	STATUS(pg_entry_cursor_close(&cursor, &error), PG_OK);
	unsigned char data[sizeof(payload)];
	size_t size;
	STATUS(pg_file_read_all(file, data, sizeof(data), &size, &error),
		PG_OK);
	CHECK(size == sizeof(payload) && !memcmp(data, payload, size));
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static int metadata_case(void)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_tree *tree = NULL;
	pg_entry_cursor *cursor = (pg_entry_cursor *)1;
	pg_file *file = (pg_file *)1;
	pg_entry_info info;
	pg_error error;
	pg_observer observer = { 0 };

	STATUS(pg_source_entries(NULL, NULL, 0, &cursor, &error), PG_INVALID);
	CHECK(cursor == NULL);
	STATUS(pg_tree_entries(NULL, NULL, 0, &cursor, &error), PG_INVALID);
	CHECK(cursor == NULL);
	memset(&info, 0xa5, sizeof(info));
	STATUS(pg_entry_cursor_next(NULL, &info, &file, &error), PG_INVALID);
	CHECK(!info.kind && !info.canonical_name && !file);
	STATUS(pg_entry_cursor_close(NULL, &error), PG_INVALID);
	STATUS(pg_entry_cursor_close(&cursor, &error), PG_OK);
	CHECK(directory("root") == 0);
	CHECK(put_bytes("root/.marked", payload, sizeof(payload)) == 0);
#ifdef _WIN32
	CHECK(SetFileAttributesA("root/.marked", FILE_ATTRIBUTE_HIDDEN |
		FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_READONLY));
	DWORD native_attributes = GetFileAttributesA("root/.marked");

	printf("fixture attributes before Piggle: %lu\n", native_attributes);
	CHECK((native_attributes & (FILE_ATTRIBUTE_HIDDEN |
		FILE_ATTRIBUTE_SYSTEM)) == (FILE_ATTRIBUTE_HIDDEN |
		FILE_ATTRIBUTE_SYSTEM));
#else
	CHECK(chmod("root/.marked", 0444) == 0);
#endif
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_source_open(context, "root", NULL, &source, &error), PG_OK);
	STATUS(pg_tree_create(context, &tree, &error), PG_OK);
	STATUS(pg_tree_attach(tree, source, &error), PG_OK);
	STATUS(pg_tree_request_subtree(tree, NULL, &error), PG_OK);
	STATUS(pg_tree_watch(tree, PG_WATCH_SCAN, &error), PG_OK);
	STATUS(pg_tree_entries(tree, NULL, 0, &cursor, &error), PG_OK);
	STATUS(pg_entry_cursor_next(cursor, NULL, &file, &error), PG_INVALID);
	CHECK(!file);
	STATUS(pg_entry_cursor_next(cursor, &info, NULL, &error), PG_INVALID);
	CHECK(!info.kind);
	STATUS(pg_entry_cursor_next(cursor, &info, &file, &error), PG_OK);
	CHECK(!strcmp(info.canonical_name, ".marked"));
	CHECK(info.kind == PG_ENTRY_FILE && info.size == sizeof(payload));
	printf("entry metadata: mtime=%lld attributes=%u\n",
		(long long)info.mtime, info.attributes);
#ifdef _WIN32
	printf("native attributes: %lu\n", GetFileAttributesA("root/.marked"));
#endif
	CHECK(info.mtime > 0 && (info.attributes & PG_ENTRY_HIDDEN));
	CHECK(info.attributes & PG_ENTRY_READ_ONLY);
#ifdef _WIN32
	CHECK(info.attributes & PG_ENTRY_SYSTEM);
	CHECK(SetFileAttributesA("root/.marked", FILE_ATTRIBUTE_NORMAL));
#else
	CHECK(!(info.attributes & PG_ENTRY_SYSTEM));
	CHECK(chmod("root/.marked", 0644) == 0);
#endif
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_entry_cursor_close(&cursor, &error), PG_OK);
	CHECK(directory("root/empty") == 0);
	STATUS(pg_tree_poll(tree, &observer, &error), PG_OK);
	STATUS(pg_tree_entries(tree, "empty", 0, &cursor, &error), PG_OK);
	CHECK(end(cursor) == 0);
	STATUS(pg_entry_cursor_close(&cursor, &error), PG_OK);
	STATUS(pg_tree_entries(tree, NULL, 0, &cursor, &error), PG_OK);
	CHECK(next(cursor, ".marked", PG_ENTRY_FILE, 0) == 0);
	CHECK(next(cursor, "empty", PG_ENTRY_DIRECTORY, 0) == 0);
	CHECK(end(cursor) == 0);
	STATUS(pg_entry_cursor_close(&cursor, &error), PG_OK);
	CHECK(remove_dir("root/empty") == 0);
	STATUS(pg_tree_poll(tree, &observer, &error), PG_OK);
	STATUS(pg_tree_entries(tree, NULL, 0, &cursor, &error), PG_OK);
	CHECK(next(cursor, ".marked", PG_ENTRY_FILE, 0) == 0);
	CHECK(end(cursor) == 0);
	STATUS(pg_entry_cursor_close(&cursor, &error), PG_OK);
	STATUS(pg_tree_close(&tree, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static int shallow_lookup(void)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_file *file = NULL;
	pg_file_info info;
	pg_error error;

	CHECK(!directory("indexed"));
	CHECK(!directory("indexed/nested"));
	CHECK(!put_bytes("indexed/file", payload, sizeof(payload)));
	CHECK(!put_bytes("indexed/nested/file", payload, sizeof(payload)));
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_source_open(context, "indexed", NULL, &source, &error),
		PG_OK);
	STATUS(pg_source_discover(source, NULL, PG_DISCOVER_CHILDREN,
		&error), PG_OK);
	CHECK(!put_bytes("indexed/file", "x", 1));
	CHECK(!put_bytes("indexed/new", payload, sizeof(payload)));
	STATUS(pg_source_find(source, "file", &file, &error), PG_OK);
	STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	CHECK(info.logical_size == sizeof(payload));
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_source_find(source, "new", &file, &error), PG_NOT_FOUND);
	/* Shallow coverage stops at immediate children, including directories. */
	STATUS(pg_source_find(source, "nested", &file, &error), PG_CONFLICT);
	STATUS(pg_source_find(source, "nested/file", &file, &error), PG_OK);
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_source_discover(source, "nested", PG_DISCOVER_CHILDREN,
		&error), PG_OK);
	CHECK(!put_bytes("indexed/nested/file", "x", 1));
	CHECK(!put_bytes("indexed/nested/new", payload, sizeof(payload)));
	STATUS(pg_source_find(source, "nested/file", &file, &error), PG_OK);
	STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	CHECK(info.logical_size == sizeof(payload));
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_source_find(source, "nested/new", &file, &error),
		PG_NOT_FOUND);
	STATUS(pg_source_discover(source, NULL, PG_DISCOVER_CHILDREN,
		&error), PG_OK);
	STATUS(pg_source_find(source, "file", &file, &error), PG_OK);
	STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	CHECK(info.logical_size == 1);
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_source_find(source, "new", &file, &error), PG_OK);
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	CHECK(remove("indexed/new") == 0);
	CHECK(remove("indexed/nested/new") == 0);
	return 0;
}

static int shallow(void)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_tree *tree = NULL;
	pg_entry_cursor *cursor = NULL;
	pg_error error;

	CHECK(!directory("shallow"));
	CHECK(!directory("shallow/nested"));
	CHECK(!put_bytes("shallow/nested/file", payload, sizeof(payload)));
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_source_open(context, "shallow", NULL, &source, &error),
		PG_OK);
	STATUS(pg_source_discover(source, NULL, PG_DISCOVER_CHILDREN,
		&error), PG_OK);
	STATUS(pg_source_entries(source, NULL, 0, &cursor, &error), PG_OK);
	CHECK(!next(cursor, "nested", PG_ENTRY_DIRECTORY, 0));
	CHECK(!end(cursor));
	STATUS(pg_entry_cursor_close(&cursor, &error), PG_OK);
	STATUS(pg_source_entries(source, NULL, PG_ENTRIES_RECURSIVE,
		&cursor, &error), PG_INVALID);
	STATUS(pg_source_entries(source, "nested", 0, &cursor, &error),
		PG_INVALID);
	STATUS(pg_tree_create(context, &tree, &error), PG_OK);
	STATUS(pg_tree_attach(tree, source, &error), PG_OK);
	STATUS(pg_tree_discover(tree, "nested", PG_DISCOVER_CHILDREN,
		&error), PG_OK);
	STATUS(pg_tree_entries(tree, "nested", 0, &cursor, &error), PG_OK);
	CHECK(!next(cursor, "nested/file", PG_ENTRY_FILE, 0));
	CHECK(!end(cursor));
	STATUS(pg_entry_cursor_close(&cursor, &error), PG_OK);
	STATUS(pg_tree_entries(tree, NULL, 0, &cursor, &error), PG_INVALID);
	STATUS(pg_tree_close(&tree, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return shallow_lookup();
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	if (!strcmp(argv[1], "shallow"))
		return shallow();
	if (!strcmp(argv[1], "metadata"))
		return metadata_case();
	if (!strcmp(argv[1], "loose") || !strcmp(argv[1], "tree"))
		return loose_case(!strcmp(argv[1], "tree"));
	return archive_case(!strcmp(argv[1], "pigg") ? PG_PIGG2 : PG_HOGG10);
}
