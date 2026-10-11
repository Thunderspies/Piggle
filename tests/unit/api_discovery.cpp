#include "../support.hpp"
#include <piggle/piggle.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace {

static int make_dir(const char *path)
{
#if defined(_WIN32)
	int result = _mkdir(path);
#else
	int result = mkdir(path, 0700);
#endif

	return result == 0 || errno == EEXIST;
}

static int put_file(const char *path, const char *bytes)
{
	FILE *stream = fopen(path, "wb");
	size_t size = strlen(bytes);

	PG_CHECK(stream != NULL);
	PG_CHECK(fwrite(bytes, 1, size, stream) == size);
	PG_CHECK(fclose(stream) == 0);
	return 0;
}

static int has_file(const char *path, const char *bytes)
{
	FILE *stream = fopen(path, "rb");
	char found[64];
	size_t size = strlen(bytes);

	PG_CHECK(stream != NULL);
	PG_CHECK(size < sizeof(found));
	PG_CHECK(fread(found, 1, size, stream) == size);
	PG_CHECK(fgetc(stream) == EOF);
	PG_CHECK(fclose(stream) == 0);
	PG_CHECK(memcmp(found, bytes, size) == 0);
	return 0;
}

static int expect_names(
	pg_cursor *cursor, const char **names, size_t count)
{
	pg_file *file = NULL;
	pg_file_info info;
	pg_error error;
	size_t i;

	for (i = 0; i < count; i++) {
		PG_STATUS(pg_cursor_next(cursor, &file, &error), PG_OK);
		PG_STATUS(pg_file_inspect(file, &info, &error), PG_OK);
		PG_CHECK(strcmp(info.canonical_name, names[i]) == 0);
		PG_STATUS(pg_file_close(&file, &error), PG_OK);
	}
	PG_STATUS(pg_cursor_next(cursor, &file, &error), PG_END);
	PG_CHECK(file == NULL);
	return 0;
}

static int test_source(void)
{
	static const char *initial[] = {"menu/a"};
	static const char *updated[] = {"menu/a", "menu/b"};
	static const char *other[] = {"other/b"};
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_cursor *cursor = NULL;
	pg_cursor *old = NULL;
	pg_file *file = NULL;
	pg_reader *reader = NULL;
	pg_error error;
	char bytes[8] = {};
	size_t count = 0;

	PG_CHECK(make_dir("root"));
	PG_CHECK(make_dir("root/menu"));
	PG_CHECK(make_dir("root/other"));
	PG_CHECK(put_file("root/menu/a", "aaa") == 0);
	PG_CHECK(put_file("root/other/b", "bbb") == 0);
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(
		pg_source_open(context, "root", NULL, &source, &error),
		PG_OK);
	PG_STATUS(pg_source_files(source, "menu", &cursor, &error),
		PG_INVALID);
	PG_CHECK(cursor == NULL);
	PG_STATUS(pg_source_request_subtree(source, "menu", &error),
		PG_OK);
	PG_STATUS(pg_source_files(source, "menu", &old, &error), PG_OK);
	PG_STATUS(pg_source_files(source, NULL, &cursor, &error),
		PG_INVALID);
	PG_CHECK(cursor == NULL);

	PG_CHECK(put_file("root/menu/b", "new") == 0);
	PG_STATUS(pg_source_find(source, "menu/b", &file, &error),
		PG_NOT_FOUND);
	PG_CHECK(file == NULL);
	PG_STATUS(pg_source_find(source, "other/b", &file, &error),
		PG_OK);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_source_files(source, "menu", &cursor, &error),
		PG_OK);
	PG_CHECK(expect_names(cursor, initial, 1) == 0);
	PG_STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	PG_STATUS(pg_source_request_subtree(source, "menu", &error),
		PG_OK);
	PG_STATUS(pg_source_files(source, "menu", &cursor, &error),
		PG_OK);
	PG_CHECK(expect_names(cursor, updated, 2) == 0);
	PG_STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	PG_CHECK(expect_names(old, initial, 1) == 0);
	PG_STATUS(pg_cursor_close(&old, &error), PG_OK);
	PG_STATUS(pg_source_read_all(source, "menu/b", bytes,
				sizeof(bytes), &count, &error),
		PG_OK);
	PG_CHECK(count == 3 && memcmp(bytes, "new", 3) == 0);
	PG_STATUS(pg_source_files(source, "menu/a", &cursor, &error),
		PG_CONFLICT);
	PG_CHECK(cursor == NULL);
	PG_STATUS(pg_source_request_subtree(source, "missing", &error),
		PG_OK);
	PG_STATUS(pg_source_files(source, "missing", &cursor, &error),
		PG_OK);
	PG_CHECK(expect_names(cursor, NULL, 0) == 0);
	PG_STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	PG_STATUS(
		pg_source_request_subtree(source, NULL, &error), PG_OK);
	PG_STATUS(pg_source_files(source, "other", &cursor, &error),
		PG_OK);
	PG_CHECK(expect_names(cursor, other, 1) == 0);
	PG_STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	PG_CHECK(remove("root/menu/a") == 0);
	PG_STATUS(
		pg_source_find(source, "menu/a", &file, &error), PG_OK);
	PG_STATUS(
		pg_reader_open(file, PG_READ_LOGICAL, &reader, &error),
		PG_STALE);
	PG_CHECK(reader == NULL);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static int test_tree(void)
{
	static const char *initial[] = {
		"menu/a", "menu/arch", "menu/loose"};
	static const char *updated[] = {
		"menu/a", "menu/arch", "menu/loose", "menu/new"};
	static const char *polled[] = {"menu/a", "menu/arch",
		"menu/loose", "menu/new", "menu/polled"};
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_source *archive = NULL;
	pg_source *loose = NULL;
	pg_tree *tree = NULL;
	pg_cursor *cursor = NULL;
	pg_cursor *old = NULL;
	pg_file *file = NULL;
	pg_observer observer = {NULL, NULL};
	pg_error error;
	char bytes[8] = {};
	size_t count = 0;

	PG_CHECK(make_dir("root"));
	PG_CHECK(make_dir("root/menu"));
	PG_CHECK(put_file("root/menu/a", "loose") == 0);
	PG_CHECK(put_file("root/menu/loose", "only") == 0);
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(pg_archive_builder_create(context, "base.pigg",
				PG_PIGG2, 0, &builder, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_write_all(builder, "menu/a",
				"archive", 7, NULL, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_write_all(builder, "menu/arch",
				"base", 4, NULL, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	PG_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	PG_STATUS(pg_source_open(
				context, "base.pigg", NULL, &archive, &error),
		PG_OK);
	PG_STATUS(pg_source_open(context, "root", NULL, &loose, &error),
		PG_OK);
	PG_STATUS(pg_tree_create(context, &tree, &error), PG_OK);
	PG_STATUS(pg_tree_attach(tree, archive, &error), PG_OK);
	PG_STATUS(pg_tree_files(tree, "menu", &cursor, &error), PG_OK);
	PG_STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	PG_STATUS(pg_tree_attach(tree, loose, &error), PG_OK);
	PG_STATUS(pg_tree_files(tree, "menu", &cursor, &error),
		PG_INVALID);
	PG_CHECK(cursor == NULL);
	PG_STATUS(pg_source_request_subtree(loose, "menu", &error),
		PG_OK);
	PG_STATUS(pg_tree_files(tree, "menu", &cursor, &error),
		PG_INVALID);
	PG_CHECK(cursor == NULL);
	PG_STATUS(pg_tree_request_subtree(tree, "menu", &error), PG_OK);
	PG_STATUS(pg_tree_files(tree, "menu", &old, &error), PG_OK);
	PG_STATUS(pg_tree_read_all(tree, "menu/a", bytes, sizeof(bytes),
				&count, &error),
		PG_OK);
	PG_CHECK(count == 5 && memcmp(bytes, "loose", 5) == 0);
	PG_CHECK(put_file("root/menu/new", "new") == 0);
	PG_STATUS(pg_tree_find(tree, "menu/new", &file, &error),
		PG_NOT_FOUND);
	PG_CHECK(file == NULL);
	PG_STATUS(pg_tree_files(tree, "menu", &cursor, &error), PG_OK);
	PG_CHECK(expect_names(cursor, initial, 3) == 0);
	PG_STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	PG_STATUS(pg_tree_request_subtree(tree, "menu", &error), PG_OK);
	PG_STATUS(pg_tree_files(tree, "menu", &cursor, &error), PG_OK);
	PG_CHECK(expect_names(cursor, updated, 4) == 0);
	PG_STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	PG_CHECK(expect_names(old, initial, 3) == 0);
	PG_STATUS(pg_cursor_close(&old, &error), PG_OK);
	PG_STATUS(pg_tree_watch(tree, PG_WATCH_SCAN, &error), PG_OK);
	PG_CHECK(put_file("root/menu/polled", "new") == 0);
	PG_STATUS(pg_tree_files(tree, "menu", &cursor, &error), PG_OK);
	PG_CHECK(expect_names(cursor, updated, 4) == 0);
	PG_STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	PG_STATUS(pg_tree_poll(tree, &observer, &error), PG_OK);
	PG_STATUS(pg_tree_files(tree, "menu", &cursor, &error), PG_OK);
	PG_CHECK(expect_names(cursor, polled, 5) == 0);
	PG_STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	PG_STATUS(pg_tree_close(&tree, &error), PG_OK);
	PG_STATUS(pg_source_close(&loose, &error), PG_OK);
	PG_STATUS(pg_source_close(&archive, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static int test_scoped_rollback(void)
{
	static const char *nested[] = {"menu/a", "menu/sub/deep"};
	static const char *leaf[] = {"leaf/a"};
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_tree *tree = NULL;
	pg_cursor *cursor = NULL;
	pg_file *file = NULL;
	pg_file_info info;
	pg_error error;

	PG_CHECK(make_dir("root"));
	PG_CHECK(make_dir("root/menu"));
	PG_CHECK(make_dir("root/menu/sub"));
	PG_CHECK(make_dir("root/other"));
	PG_CHECK(make_dir("root/leaf"));
	PG_CHECK(put_file("root/leaf/a", "leaf") == 0);
	PG_CHECK(put_file("root/menu/a", "first") == 0);
	PG_CHECK(put_file("root/menu/sub/deep", "deep") == 0);
	PG_CHECK(put_file("root/other/a", "other") == 0);
	PG_CHECK(put_file("root/target", "old") == 0);
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(
		pg_source_open(context, "root", NULL, &source, &error),
		PG_OK);
	PG_STATUS(pg_tree_create(context, &tree, &error), PG_OK);
	PG_STATUS(pg_tree_attach(tree, source, &error), PG_OK);
	PG_STATUS(pg_tree_discover(
				tree, NULL, PG_DISCOVER_RECURSIVE, &error),
		PG_OK);

	/* A failed recursive discovery restores only the affected copy.
	 */
	PG_CHECK(put_file("root/target", "changed") == 0);
	PG_STATUS(pg_tree_discover(tree, "target",
				PG_DISCOVER_RECURSIVE, &error),
		PG_CONFLICT);
	PG_STATUS(
		pg_source_find(source, "target", &file, &error), PG_OK);
	PG_STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	PG_CHECK(info.logical_size == 3);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);

	/* Shallow rollback preserves unrelated cached descendants. */
	PG_CHECK(rename("root/leaf", "saved_leaf") == 0);
	PG_CHECK(put_file("root/leaf", "replacement") == 0);
	PG_STATUS(pg_tree_discover(
				tree, "leaf", PG_DISCOVER_CHILDREN, &error),
		PG_CONFLICT);
	PG_STATUS(pg_tree_files(tree, "leaf", &cursor, &error), PG_OK);
	PG_CHECK(expect_names(cursor, leaf, 1) == 0);
	PG_STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	PG_STATUS(pg_tree_files(tree, "menu", &cursor, &error), PG_OK);
	PG_CHECK(expect_names(cursor, nested, 2) == 0);
	PG_STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	PG_STATUS(pg_tree_discover(
				tree, "other", PG_DISCOVER_RECURSIVE, &error),
		PG_OK);
	PG_STATUS(pg_tree_files(tree, "menu", &cursor, &error), PG_OK);
	PG_CHECK(expect_names(cursor, nested, 2) == 0);
	PG_STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	PG_STATUS(pg_tree_close(&tree, &error), PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static int test_bulk(void)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_source *packed = NULL;
	pg_tree *tree = NULL;
	pg_cursor *cursor = NULL;
	pg_file *file = NULL;
	pg_error error;

	PG_CHECK(make_dir("root"));
	PG_CHECK(make_dir("out_source"));
	PG_CHECK(make_dir("out_tree"));
	PG_CHECK(put_file("root/a", "one") == 0);
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(
		pg_source_open(context, "root", NULL, &source, &error),
		PG_OK);
	PG_STATUS(pg_source_pack(source, "source.pigg", PG_PIGG2, NULL,
				&error),
		PG_OK);
	PG_STATUS(pg_source_open(context, "source.pigg", NULL, &packed,
				&error),
		PG_OK);
	PG_STATUS(
		pg_source_files(packed, NULL, &cursor, &error), PG_OK);
	PG_STATUS(pg_cursor_next(cursor, &file, &error), PG_OK);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	PG_STATUS(pg_source_unpack(source, "out_source", 0, &error),
		PG_OK);
	PG_CHECK(has_file("out_source/a", "one") == 0);
	PG_STATUS(pg_tree_create(context, &tree, &error), PG_OK);
	PG_STATUS(pg_tree_attach(tree, source, &error), PG_OK);
	PG_STATUS(
		pg_tree_pack(tree, "tree.pigg", PG_PIGG2, NULL, &error),
		PG_OK);
	PG_STATUS(pg_tree_unpack(tree, "out_tree", 0, &error), PG_OK);
	PG_CHECK(has_file("out_tree/a", "one") == 0);
	PG_STATUS(pg_tree_close(&tree, &error), PG_OK);
	PG_STATUS(pg_source_close(&packed, &error), PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static void PG_CALL count_add(
	void *user, const pg_visible_change *change)
{
	unsigned int *count = static_cast<unsigned int *>(user);

	if (change->kind == PG_CHANGE_ADD && change->canonical_name &&
		strcmp(change->canonical_name, "menu/new") == 0)
		(*count)++;
}

static int test_source_scope(void)
{
	static const char *initial[] = {"menu/a"};
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_tree *tree = NULL;
	pg_cursor *cursor = NULL;
	pg_observer observer;
	pg_error error;
	unsigned int additions = 0;

	PG_CHECK(make_dir("root"));
	PG_CHECK(make_dir("root/menu"));
	PG_CHECK(put_file("root/menu/a", "old") == 0);
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(
		pg_source_open(context, "root", NULL, &source, &error),
		PG_OK);
	PG_STATUS(pg_tree_create(context, &tree, &error), PG_OK);
	PG_STATUS(pg_tree_attach(tree, source, &error), PG_OK);
	PG_STATUS(pg_tree_watch(tree, PG_WATCH_SCAN, &error), PG_OK);
	PG_STATUS(pg_source_request_subtree(source, "menu", &error),
		PG_OK);
	PG_CHECK(put_file("root/menu/new", "new") == 0);
	observer.user = &additions;
	observer.visible = count_add;
	PG_STATUS(pg_tree_poll(tree, &observer, &error), PG_OK);
	PG_CHECK(additions == 0);
	PG_STATUS(pg_source_files(source, "menu", &cursor, &error),
		PG_OK);
	PG_CHECK(expect_names(cursor, initial, 1) == 0);
	PG_STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	PG_STATUS(pg_tree_files(tree, "menu", &cursor, &error),
		PG_INVALID);
	PG_CHECK(cursor == NULL);
	PG_STATUS(pg_source_request_subtree(source, "menu", &error),
		PG_OK);
	PG_STATUS(pg_tree_close(&tree, &error), PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

#if defined(__linux__)
static int test_writer_guard(void)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_file *file = NULL;
	pg_source_options options = {PG_LOOSE, PG_WRITE, 0};
	pg_file_info info;
	pg_error error;
	FILE *unexpected;

	PG_CHECK(make_dir("root"));
	PG_CHECK(make_dir("root/menu"));
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(pg_source_open(
				context, "root", &options, &source, &error),
		PG_OK);
	PG_STATUS(pg_source_request_subtree(source, "menu", &error),
		PG_OK);
	PG_CHECK(put_file("root/menu/NEW", "external") == 0);
	PG_STATUS(pg_source_write_all(source, "menu/new", "replacement",
				11, NULL, &error),
		PG_OK);
	PG_CHECK(has_file("root/menu/NEW", "replacement") == 0);
	unexpected = fopen("root/menu/new", "rb");
	PG_CHECK(unexpected == NULL && errno == ENOENT);
	PG_STATUS(pg_source_find(source, "menu/new", &file, &error),
		PG_OK);
	PG_STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	PG_CHECK(strcmp(info.original_name, "menu/NEW") == 0);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static int test_native_query(void)
{
	static const char *updated[] = {"menu/a", "menu/new"};
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_tree *tree = NULL;
	pg_cursor *cursor = NULL;
	pg_observer observer;
	pg_error error;
	unsigned int additions = 0;
	char bytes[8] = {};
	size_t count = 0;

	PG_CHECK(make_dir("root"));
	PG_CHECK(make_dir("root/menu"));
	PG_CHECK(put_file("root/menu/a", "old") == 0);
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(
		pg_source_open(context, "root", NULL, &source, &error),
		PG_OK);
	PG_STATUS(pg_tree_create(context, &tree, &error), PG_OK);
	PG_STATUS(pg_tree_attach(tree, source, &error), PG_OK);
	PG_STATUS(pg_tree_request_subtree(tree, "menu", &error), PG_OK);
	PG_STATUS(pg_tree_watch(tree, PG_WATCH_NATIVE, &error), PG_OK);
	PG_CHECK(put_file("root/menu/new", "new") == 0);
	PG_STATUS(pg_tree_files(tree, "menu", &cursor, &error), PG_OK);
	PG_CHECK(expect_names(cursor, updated, 2) == 0);
	PG_STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	PG_STATUS(pg_tree_read_all(tree, "menu/new", bytes,
				sizeof(bytes), &count, &error),
		PG_OK);
	PG_CHECK(count == 3 && memcmp(bytes, "new", 3) == 0);
	observer.user = &additions;
	observer.visible = count_add;
	PG_STATUS(pg_tree_poll(tree, &observer, &error), PG_OK);
	PG_CHECK(additions == 1);
	PG_STATUS(pg_tree_close(&tree, &error), PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}
#endif

#if defined(_WIN32)
static int test_windows_scan(void)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_cursor *cursor = NULL;
	pg_file *old = NULL, *fresh = NULL;
	pg_reader *reader = NULL;
	pg_file_info before, after;
	pg_error error;
	BY_HANDLE_FILE_INFORMATION native;
	HANDLE handle;
	DWORD attributes;
	FILETIME modified;
	WIN32_FIND_DATAA item;
	char path[64], bytes[8] = {};
	size_t count, files = 0;
	int i, collision = 0, link = 0;
	int upper = 0, lower = 0;

	PG_CHECK(make_dir("root"));
	PG_CHECK(make_dir("root/window"));
	PG_CHECK(make_dir("root/window/nested"));
	DeleteFileA("root/prior");
	PG_CHECK(put_file("root/window/a", "old") == 0);
	PG_CHECK(put_file("root/window/nested/b", "nested") == 0);
	for (i = 0; i < 128; i++) {
		snprintf(path, sizeof(path), "root/window/item%03d", i);
		PG_CHECK(put_file(path, "x") == 0);
	}
	handle = CreateFileA("root/window/a", FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL, OPEN_EXISTING, 0, NULL);
	PG_CHECK(handle != INVALID_HANDLE_VALUE);
	PG_CHECK(GetFileInformationByHandle(handle, &native));
	modified = native.ftLastWriteTime;
	PG_CHECK(CloseHandle(handle));
	handle = CreateFileA("root/window/Dup", GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL, CREATE_NEW, FILE_FLAG_POSIX_SEMANTICS, NULL);
	if (handle != INVALID_HANDLE_VALUE) {
		PG_CHECK(CloseHandle(handle));
		handle = CreateFileA("root/window/dup", GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE |
				FILE_SHARE_DELETE,
			NULL, CREATE_NEW, FILE_FLAG_POSIX_SEMANTICS,
			NULL);
		if (handle != INVALID_HANDLE_VALUE) {
			PG_CHECK(CloseHandle(handle));
		}
	}
	handle = FindFirstFileA("root/window/*", &item);
	PG_CHECK(handle != INVALID_HANDLE_VALUE);
	do {
		upper |= strcmp(item.cFileName, "Dup") == 0;
		lower |= strcmp(item.cFileName, "dup") == 0;
	} while (FindNextFileA(handle, &item));
	PG_CHECK(FindClose(handle));
	collision = upper && lower;
	CreateSymbolicLinkA("root/window/link", "a", 0);
	attributes = GetFileAttributesA("root/window/link");
	link = attributes != INVALID_FILE_ATTRIBUTES &&
		(attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#ifdef PIGGLE_TEST_WINE_CASES
	PG_CHECK(collision);
#endif
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(
		pg_source_open(context, "root", NULL, &source, &error),
		PG_OK);
	PG_STATUS(pg_source_request_subtree(source, "window", &error),
		PG_OK);
	PG_STATUS(pg_source_files(source, "window", &cursor, &error),
		PG_OK);
	for (;;) {
		pg_file *entry = NULL;
		pg_status result =
			pg_cursor_next(cursor, &entry, &error);

		if (result == PG_END)
			break;
		PG_CHECK(result == PG_OK);
		files++;
		PG_STATUS(pg_file_close(&entry, &error), PG_OK);
	}
	PG_CHECK(files == 131);
	PG_STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	PG_STATUS(pg_source_find(source, "window/a", &old, &error),
		PG_OK);
	PG_STATUS(pg_file_inspect(old, &before, &error), PG_OK);
	PG_CHECK(before.logical_size == 3 && before.stored_size == 3);
	PG_CHECK(before.mtime ==
		(int64_t)((((uint64_t)modified.dwHighDateTime << 32) |
					modified.dwLowDateTime) /
			10000000ULL) -
			11644473600LL);
	if (collision) {
		PG_STATUS(pg_source_find(
					source, "window/dup", &fresh, &error),
			PG_OK);
		PG_STATUS(
			pg_file_inspect(fresh, &after, &error), PG_OK);
		PG_CHECK(
			strcmp(after.original_name, "window/dup") == 0);
		PG_STATUS(pg_file_close(&fresh, &error), PG_OK);
	}
	if (link) {
		PG_STATUS(pg_source_find(source, "window/link", &fresh,
					&error),
			PG_NOT_FOUND);
		PG_CHECK(fresh == NULL);
	}
	PG_STATUS(pg_source_request_subtree(source, "window/a", &error),
		PG_CONFLICT);
	PG_STATUS(pg_source_find(source, "window/a", &fresh, &error),
		PG_OK);
	PG_STATUS(pg_file_close(&fresh, &error), PG_OK);
	PG_CHECK(MoveFileExA("root/window/a", "root/prior", 0));
	PG_CHECK(put_file("root/window/a", "new") == 0);
	handle = CreateFileA("root/window/a", FILE_WRITE_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL, OPEN_EXISTING, 0, NULL);
	PG_CHECK(handle != INVALID_HANDLE_VALUE);
	PG_CHECK(SetFileTime(handle, NULL, NULL, &modified));
	PG_CHECK(CloseHandle(handle));
	PG_STATUS(pg_reader_open(old, PG_READ_LOGICAL, &reader, &error),
		PG_STALE);
	PG_CHECK(reader == NULL);
	PG_STATUS(pg_source_rescan(source, &error), PG_OK);
	PG_STATUS(pg_source_find(source, "window/a", &fresh, &error),
		PG_OK);
	PG_STATUS(pg_file_inspect(fresh, &after, &error), PG_OK);
	PG_CHECK(after.copy_id == before.copy_id);
	PG_CHECK(after.copy_generation == before.copy_generation + 1);
	PG_STATUS(
		pg_reader_open(fresh, PG_READ_LOGICAL, &reader, &error),
		PG_OK);
	PG_STATUS(pg_reader_read(
				reader, bytes, sizeof(bytes), &count, &error),
		PG_OK);
	PG_CHECK(count == 3 && memcmp(bytes, "new", 3) == 0);
	PG_STATUS(pg_reader_close(&reader, &error), PG_OK);
	PG_STATUS(pg_file_close(&fresh, &error), PG_OK);
	PG_STATUS(pg_file_close(&old, &error), PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}
#endif

using ApiDiscovery = piggle_test::ContractTest;

TEST_F(ApiDiscovery, api_discovery_source)
{
	ASSERT_EQ(0, test_source());
}

TEST_F(ApiDiscovery, api_discovery_tree)
{
	ASSERT_EQ(0, test_tree());
}

TEST_F(ApiDiscovery, api_discovery_bulk)
{
	ASSERT_EQ(0, test_bulk());
}

TEST_F(ApiDiscovery, api_discovery_source_scope)
{
	ASSERT_EQ(0, test_source_scope());
}

TEST_F(ApiDiscovery, api_discovery_scoped_rollback)
{
	ASSERT_EQ(0, test_scoped_rollback());
}

#if defined(__linux__)

TEST_F(ApiDiscovery, api_discovery_writer_guard)
{
	ASSERT_EQ(0, test_writer_guard());
}
#endif

#if defined(__linux__)

TEST_F(ApiDiscovery, api_discovery_native_query)
{
	ASSERT_EQ(0, test_native_query());
}
#endif

#if defined(_WIN32)

TEST_F(ApiDiscovery, api_discovery_windows_scan)
{
	ASSERT_EQ(0, test_windows_scan());
}
#endif

} // namespace
