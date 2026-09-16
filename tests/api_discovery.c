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



#define CHECK(expr) do { \
	if (!(expr)) { \
		fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
		return 1; \
	} \
} while (0)

#define STATUS(call, expected) do { \
	pg_status result = (call); \
	if (result != (expected) || error.status != result) { \
		fprintf(stderr, "%s:%d: %s returned %d, error %d\n", \
			__FILE__, __LINE__, #call, (int)result, \
			(int)error.status); \
		return 1; \
	} \
} while (0)

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

	CHECK(stream != NULL);
	CHECK(fwrite(bytes, 1, size, stream) == size);
	CHECK(fclose(stream) == 0);
	return 0;
}

static int has_file(const char *path, const char *bytes)
{
	FILE *stream = fopen(path, "rb");
	char found[64];
	size_t size = strlen(bytes);

	CHECK(stream != NULL);
	CHECK(size < sizeof(found));
	CHECK(fread(found, 1, size, stream) == size);
	CHECK(fgetc(stream) == EOF);
	CHECK(fclose(stream) == 0);
	CHECK(memcmp(found, bytes, size) == 0);
	return 0;
}

static int expect_names(pg_cursor *cursor, const char **names, size_t count)
{
	pg_file *file = NULL;
	pg_file_info info;
	pg_error error;
	size_t i;

	for (i = 0; i < count; i++) {
		STATUS(pg_cursor_next(cursor, &file, &error), PG_OK);
		STATUS(pg_file_inspect(file, &info, &error), PG_OK);
		CHECK(strcmp(info.canonical_name, names[i]) == 0);
		STATUS(pg_file_close(&file, &error), PG_OK);
	}
	STATUS(pg_cursor_next(cursor, &file, &error), PG_END);
	CHECK(file == NULL);
	return 0;
}

static int test_source(void)
{
	static const char *initial[] = { "menu/a" };
	static const char *updated[] = { "menu/a", "menu/b" };
	static const char *other[] = { "other/b" };
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_cursor *cursor = NULL;
	pg_cursor *old = NULL;
	pg_file *file = NULL;
	pg_reader *reader = NULL;
	pg_error error;
	char bytes[8] = { 0 };
	size_t count = 0;

	CHECK(make_dir("root"));
	CHECK(make_dir("root/menu"));
	CHECK(make_dir("root/other"));
	CHECK(put_file("root/menu/a", "aaa") == 0);
	CHECK(put_file("root/other/b", "bbb") == 0);
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_source_open(context, "root", NULL, &source, &error),
		PG_OK);
	STATUS(pg_source_files(source, "menu", &cursor, &error), PG_INVALID);
	CHECK(cursor == NULL);
	STATUS(pg_source_request_subtree(source, "menu", &error), PG_OK);
	STATUS(pg_source_files(source, "menu", &old, &error), PG_OK);
	STATUS(pg_source_files(source, NULL, &cursor, &error), PG_INVALID);
	CHECK(cursor == NULL);

	CHECK(put_file("root/menu/b", "new") == 0);
	STATUS(pg_source_find(source, "menu/b", &file, &error),
		PG_NOT_FOUND);
	CHECK(file == NULL);
	STATUS(pg_source_find(source, "other/b", &file, &error), PG_OK);
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_source_files(source, "menu", &cursor, &error), PG_OK);
	CHECK(expect_names(cursor, initial, 1) == 0);
	STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	STATUS(pg_source_request_subtree(source, "menu", &error), PG_OK);
	STATUS(pg_source_files(source, "menu", &cursor, &error), PG_OK);
	CHECK(expect_names(cursor, updated, 2) == 0);
	STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	CHECK(expect_names(old, initial, 1) == 0);
	STATUS(pg_cursor_close(&old, &error), PG_OK);
	STATUS(pg_source_read_all(source, "menu/b", bytes,
		sizeof(bytes), &count, &error), PG_OK);
	CHECK(count == 3 && memcmp(bytes, "new", 3) == 0);
	STATUS(pg_source_files(source, "menu/a", &cursor, &error),
		PG_CONFLICT);
	CHECK(cursor == NULL);
	STATUS(pg_source_request_subtree(source, "missing", &error), PG_OK);
	STATUS(pg_source_files(source, "missing", &cursor, &error), PG_OK);
	CHECK(expect_names(cursor, NULL, 0) == 0);
	STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	STATUS(pg_source_request_subtree(source, NULL, &error), PG_OK);
	STATUS(pg_source_files(source, "other", &cursor, &error), PG_OK);
	CHECK(expect_names(cursor, other, 1) == 0);
	STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	CHECK(remove("root/menu/a") == 0);
	STATUS(pg_source_find(source, "menu/a", &file, &error), PG_OK);
	STATUS(pg_reader_open(file, PG_READ_LOGICAL, &reader,
		&error), PG_STALE);
	CHECK(reader == NULL);
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static int test_tree(void)
{
	static const char *initial[] = {
		"menu/a", "menu/arch", "menu/loose"
	};
	static const char *updated[] = {
		"menu/a", "menu/arch", "menu/loose", "menu/new"
	};
	static const char *polled[] = {
		"menu/a", "menu/arch", "menu/loose", "menu/new",
		"menu/polled"
	};
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_source *archive = NULL;
	pg_source *loose = NULL;
	pg_tree *tree = NULL;
	pg_cursor *cursor = NULL;
	pg_cursor *old = NULL;
	pg_file *file = NULL;
	pg_observer observer = { NULL, NULL };
	pg_error error;
	char bytes[8] = { 0 };
	size_t count = 0;

	CHECK(make_dir("root"));
	CHECK(make_dir("root/menu"));
	CHECK(put_file("root/menu/a", "loose") == 0);
	CHECK(put_file("root/menu/loose", "only") == 0);
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_archive_builder_create(context, "base.pigg", PG_PIGG2,
		0, &builder, &error), PG_OK);
	STATUS(pg_archive_builder_write_all(builder, "menu/a", "archive",
		7, NULL, &error), PG_OK);
	STATUS(pg_archive_builder_write_all(builder, "menu/arch", "base",
		4, NULL, &error), PG_OK);
	STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	STATUS(pg_source_open(context, "base.pigg", NULL, &archive,
		&error), PG_OK);
	STATUS(pg_source_open(context, "root", NULL, &loose, &error),
		PG_OK);
	STATUS(pg_tree_create(context, &tree, &error), PG_OK);
	STATUS(pg_tree_attach(tree, archive, &error), PG_OK);
	STATUS(pg_tree_files(tree, "menu", &cursor, &error), PG_OK);
	STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	STATUS(pg_tree_attach(tree, loose, &error), PG_OK);
	STATUS(pg_tree_files(tree, "menu", &cursor, &error), PG_INVALID);
	CHECK(cursor == NULL);
	STATUS(pg_source_request_subtree(loose, "menu", &error), PG_OK);
	STATUS(pg_tree_files(tree, "menu", &cursor, &error), PG_INVALID);
	CHECK(cursor == NULL);
	STATUS(pg_tree_request_subtree(tree, "menu", &error), PG_OK);
	STATUS(pg_tree_files(tree, "menu", &old, &error), PG_OK);
	STATUS(pg_tree_read_all(tree, "menu/a", bytes, sizeof(bytes),
		&count, &error), PG_OK);
	CHECK(count == 5 && memcmp(bytes, "loose", 5) == 0);
	CHECK(put_file("root/menu/new", "new") == 0);
	STATUS(pg_tree_find(tree, "menu/new", &file, &error),
		PG_NOT_FOUND);
	CHECK(file == NULL);
	STATUS(pg_tree_files(tree, "menu", &cursor, &error), PG_OK);
	CHECK(expect_names(cursor, initial, 3) == 0);
	STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	STATUS(pg_tree_request_subtree(tree, "menu", &error), PG_OK);
	STATUS(pg_tree_files(tree, "menu", &cursor, &error), PG_OK);
	CHECK(expect_names(cursor, updated, 4) == 0);
	STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	CHECK(expect_names(old, initial, 3) == 0);
	STATUS(pg_cursor_close(&old, &error), PG_OK);
	STATUS(pg_tree_watch(tree, PG_WATCH_SCAN, &error), PG_OK);
	CHECK(put_file("root/menu/polled", "new") == 0);
	STATUS(pg_tree_files(tree, "menu", &cursor, &error), PG_OK);
	CHECK(expect_names(cursor, updated, 4) == 0);
	STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	STATUS(pg_tree_poll(tree, &observer, &error), PG_OK);
	STATUS(pg_tree_files(tree, "menu", &cursor, &error), PG_OK);
	CHECK(expect_names(cursor, polled, 5) == 0);
	STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	STATUS(pg_tree_close(&tree, &error), PG_OK);
	STATUS(pg_source_close(&loose, &error), PG_OK);
	STATUS(pg_source_close(&archive, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
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

	CHECK(make_dir("root"));
	CHECK(make_dir("out_source"));
	CHECK(make_dir("out_tree"));
	CHECK(put_file("root/a", "one") == 0);
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_source_open(context, "root", NULL, &source, &error),
		PG_OK);
	STATUS(pg_source_pack(source, "source.pigg", PG_PIGG2, NULL,
		&error), PG_OK);
	STATUS(pg_source_open(context, "source.pigg", NULL, &packed,
		&error), PG_OK);
	STATUS(pg_source_files(packed, NULL, &cursor, &error), PG_OK);
	STATUS(pg_cursor_next(cursor, &file, &error), PG_OK);
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	STATUS(pg_source_unpack(source, "out_source", 0, &error), PG_OK);
	CHECK(has_file("out_source/a", "one") == 0);
	STATUS(pg_tree_create(context, &tree, &error), PG_OK);
	STATUS(pg_tree_attach(tree, source, &error), PG_OK);
	STATUS(pg_tree_pack(tree, "tree.pigg", PG_PIGG2, NULL,
		&error), PG_OK);
	STATUS(pg_tree_unpack(tree, "out_tree", 0, &error), PG_OK);
	CHECK(has_file("out_tree/a", "one") == 0);
	STATUS(pg_tree_close(&tree, &error), PG_OK);
	STATUS(pg_source_close(&packed, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static void PG_CALL count_add(void *user,
		const pg_visible_change *change)
{
	unsigned int *count = user;

	if (change->kind == PG_CHANGE_ADD && change->canonical_name &&
	    strcmp(change->canonical_name, "menu/new") == 0)
		(*count)++;
}

static int test_source_scope(void)
{
	static const char *initial[] = { "menu/a" };
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_tree *tree = NULL;
	pg_cursor *cursor = NULL;
	pg_observer observer;
	pg_error error;
	unsigned int additions = 0;

	CHECK(make_dir("root"));
	CHECK(make_dir("root/menu"));
	CHECK(put_file("root/menu/a", "old") == 0);
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_source_open(context, "root", NULL, &source, &error),
		PG_OK);
	STATUS(pg_tree_create(context, &tree, &error), PG_OK);
	STATUS(pg_tree_attach(tree, source, &error), PG_OK);
	STATUS(pg_tree_watch(tree, PG_WATCH_SCAN, &error), PG_OK);
	STATUS(pg_source_request_subtree(source, "menu", &error), PG_OK);
	CHECK(put_file("root/menu/new", "new") == 0);
	observer.user = &additions;
	observer.visible = count_add;
	STATUS(pg_tree_poll(tree, &observer, &error), PG_OK);
	CHECK(additions == 0);
	STATUS(pg_source_files(source, "menu", &cursor, &error), PG_OK);
	CHECK(expect_names(cursor, initial, 1) == 0);
	STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	STATUS(pg_tree_files(tree, "menu", &cursor, &error), PG_INVALID);
	CHECK(cursor == NULL);
	STATUS(pg_source_request_subtree(source, "menu", &error), PG_OK);
	STATUS(pg_tree_close(&tree, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

#if defined(__linux__)
static int test_writer_guard(void)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_file *file = NULL;
	pg_source_options options = { PG_LOOSE, PG_WRITE };
	pg_file_info info;
	pg_error error;
	FILE *unexpected;

	CHECK(make_dir("root"));
	CHECK(make_dir("root/menu"));
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_source_open(context, "root", &options, &source,
		&error), PG_OK);
	STATUS(pg_source_request_subtree(source, "menu", &error), PG_OK);
	CHECK(put_file("root/menu/NEW", "external") == 0);
	STATUS(pg_source_write_all(source, "menu/new", "replacement", 11,
		NULL, &error), PG_OK);
	CHECK(has_file("root/menu/NEW", "replacement") == 0);
	unexpected = fopen("root/menu/new", "rb");
	CHECK(unexpected == NULL && errno == ENOENT);
	STATUS(pg_source_find(source, "menu/new", &file, &error), PG_OK);
	STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	CHECK(strcmp(info.original_name, "menu/NEW") == 0);
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static int test_native_query(void)
{
	static const char *updated[] = { "menu/a", "menu/new" };
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_tree *tree = NULL;
	pg_cursor *cursor = NULL;
	pg_observer observer;
	pg_error error;
	unsigned int additions = 0;
	char bytes[8] = { 0 };
	size_t count = 0;

	CHECK(make_dir("root"));
	CHECK(make_dir("root/menu"));
	CHECK(put_file("root/menu/a", "old") == 0);
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_source_open(context, "root", NULL, &source, &error),
		PG_OK);
	STATUS(pg_tree_create(context, &tree, &error), PG_OK);
	STATUS(pg_tree_attach(tree, source, &error), PG_OK);
	STATUS(pg_tree_request_subtree(tree, "menu", &error), PG_OK);
	STATUS(pg_tree_watch(tree, PG_WATCH_NATIVE, &error), PG_OK);
	CHECK(put_file("root/menu/new", "new") == 0);
	STATUS(pg_tree_files(tree, "menu", &cursor, &error), PG_OK);
	CHECK(expect_names(cursor, updated, 2) == 0);
	STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	STATUS(pg_tree_read_all(tree, "menu/new", bytes,
		sizeof(bytes), &count, &error), PG_OK);
	CHECK(count == 3 && memcmp(bytes, "new", 3) == 0);
	observer.user = &additions;
	observer.visible = count_add;
	STATUS(pg_tree_poll(tree, &observer, &error), PG_OK);
	CHECK(additions == 1);
	STATUS(pg_tree_close(&tree, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
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
	char path[64], bytes[8] = { 0 };
	size_t count, files = 0;
	int i, collision = 0, link = 0;
	int upper = 0, lower = 0;

	CHECK(make_dir("root"));
	CHECK(make_dir("root/window"));
	CHECK(make_dir("root/window/nested"));
	DeleteFileA("root/prior");
	CHECK(put_file("root/window/a", "old") == 0);
	CHECK(put_file("root/window/nested/b", "nested") == 0);
	for (i = 0; i < 128; i++) {
		snprintf(path, sizeof(path), "root/window/item%03d", i);
		CHECK(put_file(path, "x") == 0);
	}
	handle = CreateFileA("root/window/a", FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL, OPEN_EXISTING, 0, NULL);
	CHECK(handle != INVALID_HANDLE_VALUE);
	CHECK(GetFileInformationByHandle(handle, &native));
	modified = native.ftLastWriteTime;
	CHECK(CloseHandle(handle));
	handle = CreateFileA("root/window/Dup", GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL, CREATE_NEW, FILE_FLAG_POSIX_SEMANTICS, NULL);
	if (handle != INVALID_HANDLE_VALUE) {
		CHECK(CloseHandle(handle));
		handle = CreateFileA("root/window/dup", GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE |
			FILE_SHARE_DELETE, NULL, CREATE_NEW,
			FILE_FLAG_POSIX_SEMANTICS, NULL);
		if (handle != INVALID_HANDLE_VALUE) {
			CHECK(CloseHandle(handle));
		}
	}
	handle = FindFirstFileA("root/window/*", &item);
	CHECK(handle != INVALID_HANDLE_VALUE);
	do {
		upper |= strcmp(item.cFileName, "Dup") == 0;
		lower |= strcmp(item.cFileName, "dup") == 0;
	} while (FindNextFileA(handle, &item));
	CHECK(FindClose(handle));
	collision = upper && lower;
	CreateSymbolicLinkA("root/window/link", "a", 0);
	attributes = GetFileAttributesA("root/window/link");
	link = attributes != INVALID_FILE_ATTRIBUTES &&
		(attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#ifdef PIGGLE_TEST_WINE_CASES
	CHECK(collision);
#endif
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_source_open(context, "root", NULL, &source, &error),
		PG_OK);
	STATUS(pg_source_request_subtree(source, "window", &error), PG_OK);
	STATUS(pg_source_files(source, "window", &cursor, &error), PG_OK);
	for (;;) {
		pg_file *entry = NULL;
		pg_status result = pg_cursor_next(cursor, &entry, &error);

		if (result == PG_END)
			break;
		CHECK(result == PG_OK);
		files++;
		STATUS(pg_file_close(&entry, &error), PG_OK);
	}
	CHECK(files == 131);
	STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	STATUS(pg_source_find(source, "window/a", &old, &error), PG_OK);
	STATUS(pg_file_inspect(old, &before, &error), PG_OK);
	CHECK(before.logical_size == 3 && before.stored_size == 3);
	CHECK(before.mtime ==
		(int64_t)((((uint64_t)modified.dwHighDateTime << 32) |
		modified.dwLowDateTime) / 10000000ULL) - 11644473600LL);
	if (collision) {
		STATUS(pg_source_find(source, "window/dup", &fresh,
			&error), PG_OK);
		STATUS(pg_file_inspect(fresh, &after, &error), PG_OK);
		CHECK(strcmp(after.original_name, "window/dup") == 0);
		STATUS(pg_file_close(&fresh, &error), PG_OK);
	}
	if (link) {
		STATUS(pg_source_find(source, "window/link", &fresh,
			&error), PG_NOT_FOUND);
		CHECK(fresh == NULL);
	}
	STATUS(pg_source_request_subtree(source, "window/a", &error),
		PG_CONFLICT);
	STATUS(pg_source_find(source, "window/a", &fresh, &error),
		PG_OK);
	STATUS(pg_file_close(&fresh, &error), PG_OK);
	CHECK(MoveFileExA("root/window/a", "root/prior", 0));
	CHECK(put_file("root/window/a", "new") == 0);
	handle = CreateFileA("root/window/a", FILE_WRITE_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL, OPEN_EXISTING, 0, NULL);
	CHECK(handle != INVALID_HANDLE_VALUE);
	CHECK(SetFileTime(handle, NULL, NULL, &modified));
	CHECK(CloseHandle(handle));
	STATUS(pg_reader_open(old, PG_READ_LOGICAL, &reader, &error),
		PG_STALE);
	CHECK(reader == NULL);
	STATUS(pg_source_rescan(source, &error), PG_OK);
	STATUS(pg_source_find(source, "window/a", &fresh, &error), PG_OK);
	STATUS(pg_file_inspect(fresh, &after, &error), PG_OK);
	CHECK(after.copy_id == before.copy_id);
	CHECK(after.copy_generation == before.copy_generation + 1);
	STATUS(pg_reader_open(fresh, PG_READ_LOGICAL, &reader, &error),
		PG_OK);
	STATUS(pg_reader_read(reader, bytes, sizeof(bytes), &count,
		&error), PG_OK);
	CHECK(count == 3 && memcmp(bytes, "new", 3) == 0);
	STATUS(pg_reader_close(&reader, &error), PG_OK);
	STATUS(pg_file_close(&fresh, &error), PG_OK);
	STATUS(pg_file_close(&old, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}
#endif

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	if (strcmp(argv[1], "source") == 0)
		return test_source();
	if (strcmp(argv[1], "tree") == 0)
		return test_tree();
	if (strcmp(argv[1], "bulk") == 0)
		return test_bulk();
	if (strcmp(argv[1], "source_scope") == 0)
		return test_source_scope();
#if defined(_WIN32)
	if (strcmp(argv[1], "windows_scan") == 0)
		return test_windows_scan();
#endif
#if defined(__linux__)
	if (strcmp(argv[1], "writer_guard") == 0)
		return test_writer_guard();
	if (strcmp(argv[1], "native_query") == 0)
		return test_native_query();
#endif
	return 2;
}
