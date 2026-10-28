#include "../api_test.hpp"
#ifdef _WIN32
#include <windows.h>
#define remove_dir(path) _rmdir(path)
#else
#include <unistd.h>
#define remove_dir(path) rmdir(path)
#endif

namespace {

static int next(pg_entry_cursor *cursor, const char *name,
	uint32_t kind, int implied)
{
	pg_entry_info info;
	pg_file *file = NULL;
	pg_error error;

	PG_STATUS(pg_entry_cursor_next(cursor, &info, &file, &error),
		PG_OK);
	PG_CHECK(info.kind == kind &&
		strcmp(info.canonical_name, name) == 0);
	PG_CHECK(info.source_id != 0 && info.original_name != NULL);
	PG_CHECK(!!(info.attributes & PG_ENTRY_IMPLIED) == implied);
	if (kind == PG_ENTRY_FILE) {
		unsigned char data[16];
		size_t size;
		PG_CHECK(file != NULL && info.size == sizeof(payload));
		PG_STATUS(pg_file_read_all(file, data, sizeof(data),
					&size, &error),
			PG_OK);
		PG_CHECK(size == sizeof(payload) &&
			!memcmp(data, payload, size));
		PG_STATUS(pg_file_close(&file, &error), PG_OK);
	} else {
		PG_CHECK(file == NULL && info.size == 0);
		if (implied)
			PG_CHECK(info.mtime == 0);
	}
	return 0;
}

static int end(pg_entry_cursor *cursor)
{
	pg_entry_info info;
	pg_file *file = (pg_file *)1;
	pg_error error;

	memset(&info, 0xa5, sizeof(info));
	PG_STATUS(pg_entry_cursor_next(cursor, &info, &file, &error),
		PG_END);
	PG_CHECK(!file && !info.kind && !info.canonical_name);
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

	PG_CHECK(directory("root") == 0);
	PG_CHECK(directory("root/Empty") == 0);
	PG_CHECK(directory("root/Nested") == 0);
	PG_CHECK(directory("root/Nested/Void") == 0);
	PG_CHECK(put_bytes("root/Nested/File", payload,
				sizeof(payload)) == 0);
	PG_CHECK(put_bytes("root/a", payload, sizeof(payload)) == 0);
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(
		pg_source_open(context, "root", NULL, &source, &error),
		PG_OK);
	PG_STATUS(pg_source_entries(source, NULL, 0, &cursor, &error),
		PG_INVALID);
	PG_CHECK(cursor == NULL);
	PG_STATUS(
		pg_source_request_subtree(source, NULL, &error), PG_OK);
	PG_STATUS(pg_source_entries(source, NULL, 2, &cursor, &error),
		PG_INVALID);
	if (tree_mode) {
		PG_CHECK(directory("base") == 0);
		PG_CHECK(put_bytes("base/empty", payload,
					sizeof(payload)) == 0);
		PG_STATUS(pg_source_open(
					context, "base", NULL, &base, &error),
			PG_OK);
		PG_STATUS(
			pg_tree_create(context, &tree, &error), PG_OK);
		PG_STATUS(pg_tree_attach(tree, base, &error), PG_OK);
		PG_STATUS(pg_tree_attach(tree, source, &error), PG_OK);
		PG_STATUS(
			pg_tree_entries(tree, NULL, 0, &cursor, &error),
			PG_INVALID);
		PG_STATUS(pg_tree_request_subtree(tree, NULL, &error),
			PG_OK);
		PG_STATUS(
			pg_tree_entries(tree, NULL, 0, &cursor, &error),
			PG_OK);
		PG_STATUS(pg_tree_find(tree, "empty", &file, &error),
			PG_CONFLICT);
		PG_STATUS(pg_tree_files(tree, NULL, &files, &error),
			PG_OK);
	} else {
		PG_STATUS(pg_source_entries(
					source, NULL, 0, &cursor, &error),
			PG_OK);
		PG_STATUS(
			pg_source_find(source, "empty", &file, &error),
			PG_CONFLICT);
		PG_STATUS(pg_source_files(source, NULL, &files, &error),
			PG_OK);
	}
	PG_CHECK(next(cursor, "a", PG_ENTRY_FILE, 0) == 0);
	PG_CHECK(next(cursor, "empty", PG_ENTRY_DIRECTORY, 0) == 0);
	PG_CHECK(next(cursor, "nested", PG_ENTRY_DIRECTORY, 0) == 0);
	PG_CHECK(end(cursor) == 0);
	PG_STATUS(pg_entry_cursor_close(&cursor, &error), PG_OK);
	for (int i = 0; i < 2; i++) {
		PG_STATUS(pg_cursor_next(files, &file, &error), PG_OK);
		PG_STATUS(pg_file_close(&file, &error), PG_OK);
	}
	PG_STATUS(pg_cursor_next(files, &file, &error), PG_END);
	PG_STATUS(pg_cursor_close(&files, &error), PG_OK);
	PG_STATUS(pg_source_entries(source, "nested",
				PG_ENTRIES_RECURSIVE, &saved, &error),
		PG_OK);
	PG_CHECK(remove_dir("root/Nested/Void") == 0);
	PG_STATUS(pg_source_rescan(source, &error), PG_OK);
	PG_CHECK(next(saved, "nested/file", PG_ENTRY_FILE, 0) == 0);
	PG_CHECK(
		next(saved, "nested/void", PG_ENTRY_DIRECTORY, 0) == 0);
	PG_CHECK(end(saved) == 0);
	PG_STATUS(pg_entry_cursor_close(&saved, &error), PG_OK);
	PG_STATUS(
		pg_source_entries(source, "nested", 0, &cursor, &error),
		PG_OK);
	PG_CHECK(next(cursor, "nested/file", PG_ENTRY_FILE, 0) == 0);
	PG_CHECK(end(cursor) == 0);
	PG_STATUS(pg_entry_cursor_close(&cursor, &error), PG_OK);
	PG_STATUS(pg_source_entries(source, "a", 0, &cursor, &error),
		PG_CONFLICT);
	PG_STATUS(pg_source_entries(
				source, "missing", 0, &cursor, &error),
		PG_OK);
	PG_CHECK(end(cursor) == 0);
	PG_STATUS(pg_entry_cursor_close(&cursor, &error), PG_OK);
	PG_STATUS(pg_tree_close(&tree, &error), PG_OK);
	PG_STATUS(pg_source_close(&base, &error), PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
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

	PG_CHECK(clear_file("archive") == 0);
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(pg_archive_builder_create(context, "archive", format,
				0, &builder, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_write_all(builder, "Dir/Sub/File",
				payload, sizeof(payload), NULL, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	PG_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	PG_STATUS(pg_source_open(
				context, "archive", NULL, &source, &error),
		PG_OK);
	PG_STATUS(pg_source_entries(source, NULL, PG_ENTRIES_RECURSIVE,
				&cursor, &error),
		PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_BUSY);
	PG_CHECK(next(cursor, "dir", PG_ENTRY_DIRECTORY, 1) == 0);
	PG_CHECK(next(cursor, "dir/sub", PG_ENTRY_DIRECTORY, 1) == 0);
	PG_STATUS(pg_entry_cursor_next(cursor, &info, &file, &error),
		PG_OK);
	PG_CHECK(file && !strcmp(info.canonical_name, "dir/sub/file"));
	PG_CHECK(info.attributes & PG_ENTRY_READ_ONLY);
	PG_STATUS(pg_entry_cursor_close(&cursor, &error), PG_OK);
	unsigned char data[sizeof(payload)];
	size_t size;
	PG_STATUS(pg_file_read_all(
				file, data, sizeof(data), &size, &error),
		PG_OK);
	PG_CHECK(size == sizeof(payload) &&
		!memcmp(data, payload, size));
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
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
	pg_observer observer = {};

	PG_STATUS(pg_source_entries(NULL, NULL, 0, &cursor, &error),
		PG_INVALID);
	PG_CHECK(cursor == NULL);
	PG_STATUS(pg_tree_entries(NULL, NULL, 0, &cursor, &error),
		PG_INVALID);
	PG_CHECK(cursor == NULL);
	memset(&info, 0xa5, sizeof(info));
	PG_STATUS(pg_entry_cursor_next(NULL, &info, &file, &error),
		PG_INVALID);
	PG_CHECK(!info.kind && !info.canonical_name && !file);
	PG_STATUS(pg_entry_cursor_close(NULL, &error), PG_INVALID);
	PG_STATUS(pg_entry_cursor_close(&cursor, &error), PG_OK);
	PG_CHECK(directory("root") == 0);
	PG_CHECK(put_bytes("root/.marked", payload, sizeof(payload)) ==
		0);
#ifdef _WIN32
	PG_CHECK(SetFileAttributesA("root/.marked",
		FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM |
			FILE_ATTRIBUTE_READONLY));
	DWORD native_attributes = GetFileAttributesA("root/.marked");

	printf("fixture attributes before Piggle: %lu\n",
		native_attributes);
	PG_CHECK((native_attributes &
				(FILE_ATTRIBUTE_HIDDEN |
					FILE_ATTRIBUTE_SYSTEM)) ==
		(FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM));
#else
	PG_CHECK(chmod("root/.marked", 0444) == 0);
#endif
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(
		pg_source_open(context, "root", NULL, &source, &error),
		PG_OK);
	PG_STATUS(pg_tree_create(context, &tree, &error), PG_OK);
	PG_STATUS(pg_tree_attach(tree, source, &error), PG_OK);
	PG_STATUS(pg_tree_request_subtree(tree, NULL, &error), PG_OK);
	PG_STATUS(pg_tree_watch(tree, PG_WATCH_SCAN, &error), PG_OK);
	PG_STATUS(
		pg_tree_entries(tree, NULL, 0, &cursor, &error), PG_OK);
	PG_STATUS(pg_entry_cursor_next(cursor, NULL, &file, &error),
		PG_INVALID);
	PG_CHECK(!file);
	PG_STATUS(pg_entry_cursor_next(cursor, &info, NULL, &error),
		PG_INVALID);
	PG_CHECK(!info.kind);
	PG_STATUS(pg_entry_cursor_next(cursor, &info, &file, &error),
		PG_OK);
	PG_CHECK(!strcmp(info.canonical_name, ".marked"));
	PG_CHECK(info.kind == PG_ENTRY_FILE &&
		info.size == sizeof(payload));
	printf("entry metadata: mtime=%lld attributes=%u\n",
		(long long)info.mtime, info.attributes);
#ifdef _WIN32
	printf("native attributes: %lu\n",
		GetFileAttributesA("root/.marked"));
#endif
	PG_CHECK(info.mtime > 0 && (info.attributes & PG_ENTRY_HIDDEN));
	PG_CHECK(info.attributes & PG_ENTRY_READ_ONLY);
#ifdef _WIN32
	PG_CHECK(info.attributes & PG_ENTRY_SYSTEM);
	PG_CHECK(SetFileAttributesA(
		"root/.marked", FILE_ATTRIBUTE_NORMAL));
#else
	PG_CHECK(!(info.attributes & PG_ENTRY_SYSTEM));
	PG_CHECK(chmod("root/.marked", 0644) == 0);
#endif
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_entry_cursor_close(&cursor, &error), PG_OK);
	PG_CHECK(directory("root/empty") == 0);
	PG_STATUS(pg_tree_poll(tree, &observer, &error), PG_OK);
	PG_STATUS(pg_tree_entries(tree, "empty", 0, &cursor, &error),
		PG_OK);
	PG_CHECK(end(cursor) == 0);
	PG_STATUS(pg_entry_cursor_close(&cursor, &error), PG_OK);
	PG_STATUS(
		pg_tree_entries(tree, NULL, 0, &cursor, &error), PG_OK);
	PG_CHECK(next(cursor, ".marked", PG_ENTRY_FILE, 0) == 0);
	PG_CHECK(next(cursor, "empty", PG_ENTRY_DIRECTORY, 0) == 0);
	PG_CHECK(end(cursor) == 0);
	PG_STATUS(pg_entry_cursor_close(&cursor, &error), PG_OK);
	PG_CHECK(remove_dir("root/empty") == 0);
	PG_STATUS(pg_tree_poll(tree, &observer, &error), PG_OK);
	PG_STATUS(
		pg_tree_entries(tree, NULL, 0, &cursor, &error), PG_OK);
	PG_CHECK(next(cursor, ".marked", PG_ENTRY_FILE, 0) == 0);
	PG_CHECK(end(cursor) == 0);
	PG_STATUS(pg_entry_cursor_close(&cursor, &error), PG_OK);
	PG_STATUS(pg_tree_close(&tree, &error), PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static int shallow_lookup(void)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_file *file = NULL;
	pg_file_info info;
	pg_error error;

	PG_CHECK(!directory("indexed"));
	PG_CHECK(!directory("indexed/nested"));
	PG_CHECK(!put_bytes("indexed/file", payload, sizeof(payload)));
	PG_CHECK(!put_bytes(
		"indexed/nested/file", payload, sizeof(payload)));
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(pg_source_open(
				context, "indexed", NULL, &source, &error),
		PG_OK);
	PG_STATUS(pg_source_discover(
				source, NULL, PG_DISCOVER_CHILDREN, &error),
		PG_OK);
	PG_CHECK(!put_bytes("indexed/file", "x", 1));
	PG_CHECK(!put_bytes("indexed/new", payload, sizeof(payload)));
	PG_STATUS(pg_source_find(source, "file", &file, &error), PG_OK);
	PG_STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	PG_CHECK(info.logical_size == sizeof(payload));
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_source_find(source, "new", &file, &error),
		PG_NOT_FOUND);
	/* Shallow coverage stops at immediate children, including
	 * directories.
	 */
	PG_STATUS(pg_source_find(source, "nested", &file, &error),
		PG_CONFLICT);
	PG_STATUS(pg_source_find(source, "nested/file", &file, &error),
		PG_OK);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_source_discover(source, "nested",
				PG_DISCOVER_CHILDREN, &error),
		PG_OK);
	PG_CHECK(!put_bytes("indexed/nested/file", "x", 1));
	PG_CHECK(!put_bytes(
		"indexed/nested/new", payload, sizeof(payload)));
	PG_STATUS(pg_source_find(source, "nested/file", &file, &error),
		PG_OK);
	PG_STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	PG_CHECK(info.logical_size == sizeof(payload));
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_source_find(source, "nested/new", &file, &error),
		PG_NOT_FOUND);
	PG_STATUS(pg_source_discover(
				source, NULL, PG_DISCOVER_CHILDREN, &error),
		PG_OK);
	PG_STATUS(pg_source_find(source, "file", &file, &error), PG_OK);
	PG_STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	PG_CHECK(info.logical_size == 1);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_source_find(source, "new", &file, &error), PG_OK);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	PG_CHECK(remove("indexed/new") == 0);
	PG_CHECK(remove("indexed/nested/new") == 0);
	return 0;
}

static int shallow(void)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_tree *tree = NULL;
	pg_entry_cursor *cursor = NULL;
	pg_error error;

	PG_CHECK(!directory("shallow"));
	PG_CHECK(!directory("shallow/nested"));
	PG_CHECK(!put_bytes(
		"shallow/nested/file", payload, sizeof(payload)));
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(pg_source_open(
				context, "shallow", NULL, &source, &error),
		PG_OK);
	PG_STATUS(pg_source_discover(
				source, NULL, PG_DISCOVER_CHILDREN, &error),
		PG_OK);
	PG_STATUS(pg_source_entries(source, NULL, 0, &cursor, &error),
		PG_OK);
	PG_CHECK(!next(cursor, "nested", PG_ENTRY_DIRECTORY, 0));
	PG_CHECK(!end(cursor));
	PG_STATUS(pg_entry_cursor_close(&cursor, &error), PG_OK);
	PG_STATUS(pg_source_entries(source, NULL, PG_ENTRIES_RECURSIVE,
				&cursor, &error),
		PG_INVALID);
	PG_STATUS(
		pg_source_entries(source, "nested", 0, &cursor, &error),
		PG_INVALID);
	PG_STATUS(pg_tree_create(context, &tree, &error), PG_OK);
	PG_STATUS(pg_tree_attach(tree, source, &error), PG_OK);
	PG_STATUS(pg_tree_discover(
				tree, "nested", PG_DISCOVER_CHILDREN, &error),
		PG_OK);
	PG_STATUS(pg_tree_entries(tree, "nested", 0, &cursor, &error),
		PG_OK);
	PG_CHECK(!next(cursor, "nested/file", PG_ENTRY_FILE, 0));
	PG_CHECK(!end(cursor));
	PG_STATUS(pg_entry_cursor_close(&cursor, &error), PG_OK);
	PG_STATUS(pg_tree_entries(tree, NULL, 0, &cursor, &error),
		PG_INVALID);
	PG_STATUS(pg_tree_close(&tree, &error), PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return shallow_lookup();
}

using ApiEntries = piggle_test::ContractTest;

TEST_F(ApiEntries, ShallowOverlayKeepsPunctuationAndDirectoryPrecedence)
{
	pg_context *context = NULL;
	pg_tree *tree = NULL;
	pg_source *sources[2] = {};
	pg_archive_builder *builder = NULL;
	pg_entry_cursor *cursor = NULL;

	ASSERT_EQ(PG_OK, pg_context_open(&context, NULL));
	ASSERT_EQ(PG_OK, pg_tree_create(context, &tree, NULL));
	for (int rank = 0; rank < 2; rank++) {
		const char *path = rank ? "upper.hogg" : "lower.hogg";

		ASSERT_EQ(PG_OK, pg_archive_builder_create(context, path,
			PG_HOGG10, 0, &builder, NULL));
		const char *names[] = {"a", "a!", "a-", "a.", "a/deep/leaf"};

		for (size_t i = 0; i < (rank ? 4u : 5u); i++)
			ASSERT_EQ(PG_OK, pg_archive_builder_write_all(builder,
				names[i], payload, sizeof(payload), NULL,
				NULL));
		ASSERT_EQ(PG_OK, pg_archive_builder_finish(builder, NULL));
		ASSERT_EQ(PG_OK, pg_archive_builder_close(&builder, NULL));
		pg_source_options options = {PG_HOGG10, PG_WRITE, 0};

		ASSERT_EQ(PG_OK, pg_source_open(context, path, &options,
			&sources[rank], NULL));
		ASSERT_EQ(PG_OK, pg_tree_attach(tree, sources[rank], NULL));
	}
	ASSERT_EQ(PG_OK, pg_tree_entries(tree, NULL, 0, &cursor, NULL));
	ASSERT_EQ(0, next(cursor, "a", PG_ENTRY_DIRECTORY, 1));
	for (const char *name : {"a!", "a-", "a."})
		ASSERT_EQ(0, next(cursor, name, PG_ENTRY_FILE, 0));
	ASSERT_EQ(0, end(cursor));
	ASSERT_EQ(PG_OK, pg_entry_cursor_close(&cursor, NULL));
	pg_file *leaf = NULL;

	ASSERT_EQ(PG_OK, pg_source_find(sources[0], "a/deep/leaf", &leaf,
		NULL));
	ASSERT_EQ(PG_OK, pg_file_delete(leaf, NULL));
	ASSERT_EQ(PG_OK, pg_file_close(&leaf, NULL));
	ASSERT_EQ(PG_OK, pg_tree_entries(tree, NULL, 0, &cursor, NULL));
	for (const char *name : {"a", "a!", "a-", "a."})
		ASSERT_EQ(0, next(cursor, name, PG_ENTRY_FILE, 0));
	ASSERT_EQ(0, end(cursor));
	ASSERT_EQ(PG_OK, pg_entry_cursor_close(&cursor, NULL));
	ASSERT_EQ(PG_OK, pg_source_find(sources[0], "a", &leaf, NULL));
	ASSERT_EQ(PG_OK, pg_file_delete(leaf, NULL));
	ASSERT_EQ(PG_OK, pg_file_close(&leaf, NULL));
	ASSERT_EQ(PG_OK, pg_source_write_all(sources[0], "a/new", payload,
		sizeof(payload), NULL, NULL));
	ASSERT_EQ(PG_OK, pg_source_rescan(sources[0], NULL));
	ASSERT_EQ(PG_OK, pg_tree_entries(tree, "a", 0, &cursor, NULL));
	ASSERT_EQ(0, next(cursor, "a/new", PG_ENTRY_FILE, 0));
	ASSERT_EQ(0, end(cursor));
	ASSERT_EQ(PG_OK, pg_entry_cursor_close(&cursor, NULL));
	ASSERT_EQ(PG_OK, pg_tree_close(&tree, NULL));
	for (pg_source *&source : sources)
		ASSERT_EQ(PG_OK, pg_source_close(&source, NULL));
	ASSERT_EQ(PG_OK, pg_context_close(&context, NULL));
}

TEST_F(ApiEntries, api_entries_loose)
{
	ASSERT_EQ(0, loose_case(0));
}

TEST_F(ApiEntries, api_entries_tree)
{
	ASSERT_EQ(0, loose_case(1));
}

TEST_F(ApiEntries, api_entries_pigg)
{
	ASSERT_EQ(0, archive_case(PG_PIGG2));
}

TEST_F(ApiEntries, api_entries_hogg)
{
	ASSERT_EQ(0, archive_case(PG_HOGG10));
}

TEST_F(ApiEntries, api_entries_metadata)
{
	ASSERT_EQ(0, metadata_case());
}

TEST_F(ApiEntries, api_entries_shallow)
{
	ASSERT_EQ(0, shallow());
}

} // namespace
