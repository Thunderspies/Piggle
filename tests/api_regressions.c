#include <piggle/piggle.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <zlib-ng.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define make_dir(path) _mkdir(path)
#else
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#define make_dir(path) mkdir(path, 0777)
#endif

#define CHECK(expr) do { \
	if (!(expr)) { \
		fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
		return 1; \
	} \
} while (0)

#define STATUS(call, expected) do { \
	pg_status actual = (call); \
	if (actual != (expected) || error.status != actual) { \
		fprintf(stderr, "%s:%d: %s: got %d, expected %d\n", \
			__FILE__, __LINE__, #call, actual, (expected)); \
		return 1; \
	} \
} while (0)

static int put_file(const char *path, const char *data)
{
	FILE *stream = fopen(path, "wb");

	CHECK(stream);
	CHECK(fwrite(data, 1, strlen(data), stream) == strlen(data));
	CHECK(!fclose(stream));
	return 0;
}

static int archive(pg_context *context, const char *path, uint32_t format,
		const char *const *names, size_t count)
{
	pg_archive_builder *builder = NULL;
	pg_error error;

	remove(path);
	STATUS(pg_archive_builder_create(context, path, format, 0,
		&builder, &error), PG_OK);
	for (size_t i = 0; i < count; i++)
		STATUS(pg_archive_builder_write_all(builder, names[i],
			"old", 3, NULL, &error), PG_OK);
	STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	return 0;
}

/* HANDLE-003 / HANDLE-006: empty cursors still retain their context. */
static int cursor_lifetime(int exhausted)
{
	pg_context *context = NULL;
	pg_tree *tree = NULL;
	pg_source *source = NULL;
	pg_cursor *cursor = NULL;
	pg_file *file = NULL;
	pg_error error;
	const char *names[] = { "a" };

	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_tree_create(context, &tree, &error), PG_OK);
	if (exhausted) {
		CHECK(!archive(context, "cursor.pigg", PG_PIGG2, names, 1));
		STATUS(pg_source_open(context, "cursor.pigg", NULL,
			&source, &error), PG_OK);
		STATUS(pg_tree_attach(tree, source, &error), PG_OK);
	}
	STATUS(pg_tree_files(tree, NULL, &cursor, &error), PG_OK);
	if (exhausted) {
		STATUS(pg_cursor_next(cursor, &file, &error), PG_OK);
		STATUS(pg_file_close(&file, &error), PG_OK);
	}
	STATUS(pg_tree_close(&tree, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_BUSY);
	CHECK(context && cursor);
	STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

/* HANDLE-006 / IO-005: loose readers retain sources and alias protection. */
static int loose_reader(int alias)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_reader *reader = NULL;
	pg_writer *writer = NULL;
	pg_write_options options;
	pg_source_options writable = { PG_LOOSE, PG_WRITE };
	pg_error error;
	char bytes[4];
	size_t size;

	make_dir("root");
	CHECK(!put_file("root/a", "old"));
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_source_open(context, "root", &writable,
		&source, &error), PG_OK);
	STATUS(pg_reader_open_source(source, "a", PG_READ_LOGICAL,
		&reader, &error), PG_OK);
	if (alias) {
		STATUS(pg_source_close(&source, &error), PG_OK);
		pg_write_options_init(&options, 0);
		STATUS(pg_writer_open_native(context, "root/a", &options,
			PG_OVERWRITE, &writer, &error), PG_CONFLICT);
	} else {
		STATUS(pg_source_delete(source, &error), PG_BUSY);
	}
	STATUS(pg_reader_read(reader, bytes, sizeof(bytes), &size,
		&error), PG_OK);
	CHECK(size == 3 && !memcmp(bytes, "old", 3));
	STATUS(pg_reader_read(reader, bytes, sizeof(bytes), &size,
		&error), PG_END);
	STATUS(pg_reader_close(&reader, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

struct worker_close {
	pg_reader *reader;
	pg_cursor *cursor;
	pg_status status;
};

#ifdef _WIN32
static DWORD WINAPI close_worker(void *argument)
#else
static void *close_worker(void *argument)
#endif
{
	struct worker_close *work = argument;

	work->status = work->reader ? pg_reader_close(&work->reader, NULL) :
		pg_cursor_close(&work->cursor, NULL);
	return 0;
}

static int run_worker(struct worker_close *work)
{
#ifdef _WIN32
	HANDLE thread = CreateThread(NULL, 0, close_worker, work, 0, NULL);

	CHECK(thread);
	CHECK(WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0);
	CHECK(CloseHandle(thread));
#else
	pthread_t thread;

	CHECK(!pthread_create(&thread, NULL, close_worker, work));
	CHECK(!pthread_join(thread, NULL));
#endif
	return 0;
}

/* HANDLE-006 / CHANGE-003: worker cleanup defers control-thread references. */
static int worker_cleanup(int cursor_case)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_tree *tree = NULL;
	pg_error error;
	struct worker_close work = { 0 };
	const char *names[] = { "a" };

	STATUS(pg_context_open(&context, &error), PG_OK);
	CHECK(!archive(context, "worker.pigg", PG_PIGG2, names, 1));
	STATUS(pg_source_open(context, "worker.pigg", NULL,
		&source, &error), PG_OK);
	STATUS(pg_tree_create(context, &tree, &error), PG_OK);
	STATUS(pg_tree_attach(tree, source, &error), PG_OK);
	if (cursor_case)
		STATUS(pg_tree_files(tree, NULL, &work.cursor, &error), PG_OK);
	else
		STATUS(pg_reader_open_tree(tree, "a", PG_READ_LOGICAL,
			&work.reader, &error), PG_OK);
	CHECK(!run_worker(&work));
	CHECK(work.status == PG_OK);
	if (cursor_case) {
		CHECK(!work.cursor);
		STATUS(pg_cursor_close(&work.cursor, &error), PG_OK);
	}
	STATUS(pg_tree_close(&tree, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

/* EDIT-010: rescan must not retarget an already-open writer. */
static int writer_stale(uint32_t format, int selected)
{
	pg_context *context = NULL, *external = NULL;
	pg_source *source = NULL, *other = NULL;
	pg_file *file = NULL;
	pg_writer *writer = NULL;
	pg_write_options options;
	pg_source_options writable = { format, PG_WRITE };
	pg_error error;
	const char *names[] = { "a" };
	size_t bytes;
	char data[8];

	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_context_open(&external, &error), PG_OK);
	CHECK(!archive(context, "target", format, names, 1));
	STATUS(pg_source_open(context, "target", &writable,
		&source, &error), PG_OK);
	pg_write_options_init(&options, 3);
	if (selected) {
		STATUS(pg_source_find(source, "a", &file, &error), PG_OK);
		STATUS(pg_writer_open_file(file, &options, &writer,
			&error), PG_OK);
	} else {
		STATUS(pg_writer_open_source(source, "a", &options,
			&writer, &error), PG_OK);
	}
	STATUS(pg_writer_write(writer, "bad", 3, &bytes, &error), PG_OK);
	const char *external_path = "external";

	if (format == PG_HOGG10)
		STATUS(pg_source_open(external, "target", &writable,
			&other, &error), PG_BUSY);
	CHECK(!archive(external, external_path, format, names, 1));
	STATUS(pg_source_open(external, external_path, &writable,
		&other, &error), PG_OK);
	STATUS(pg_source_write_all(other, "a", "external", 8, NULL,
		&error), PG_OK);
	STATUS(pg_source_close(&other, &error), PG_OK);
	STATUS(pg_context_close(&external, &error), PG_OK);
	{
		FILE *input = fopen("external", "rb");
		FILE *output = fopen("target", "wb");
		int byte;

		CHECK(input && output);
		while ((byte = fgetc(input)) != EOF)
			CHECK(fputc(byte, output) != EOF);
		CHECK(!fclose(input) && !fclose(output));
	}
	STATUS(pg_source_rescan(source, &error), PG_OK);
	STATUS(pg_writer_finish(writer, &error), PG_STALE);
	STATUS(pg_writer_close(&writer, &error), PG_OK);
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_source_read_all(source, "a", data, sizeof(data), &bytes,
		&error), PG_OK);
	CHECK(bytes == 8 && !memcmp(data, "external", 8));
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

/* EDIT-010 / IO-001: unchanged targets and distinct source copies work. */
static int writer_controls(uint32_t format)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_file *file = NULL;
	pg_writer *writer = NULL;
	pg_write_options options;
	pg_source_options writable = { format, PG_WRITE };
	pg_error error;
	const char *names[] = { "a", "b" };
	size_t count;

	STATUS(pg_context_open(&context, &error), PG_OK);
	CHECK(!archive(context, "controls", format, names, 2));
	STATUS(pg_source_open(context, "controls", &writable, &source,
		&error), PG_OK);
	STATUS(pg_source_find(source, "a", &file, &error), PG_OK);
	pg_write_options_init(&options, 3);
	STATUS(pg_writer_open_file(file, &options, &writer, &error), PG_OK);
	STATUS(pg_source_rescan(source, &error), PG_OK);
	STATUS(pg_writer_write(writer, "new", 3, &count, &error), PG_OK);
	STATUS(pg_writer_finish(writer, &error), PG_OK);
	STATUS(pg_writer_close(&writer, &error), PG_OK);
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_source_find(source, "a", &file, &error), PG_OK);
	STATUS(pg_source_copy(source, "b", file, PG_COMPRESS_NEVER,
		&error), PG_OK);
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_source_validate(source, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

/* Named writes must reject visible ancestor files after relaxed lookup. */
static int loose_writer_ancestor(void)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_source_options writable = { PG_LOOSE, PG_WRITE };
	pg_error error;

	make_dir("root");
	CHECK(!put_file("root/A", "file"));
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_source_open(context, "root", &writable, &source,
		&error), PG_OK);
	STATUS(pg_source_write_all(source, "a/b", "bad", 3, NULL, &error),
		PG_CONFLICT);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

/* TREE-001..004: intervening sort keys cannot make files visible. */
static int hierarchy(int overlay, int prefix)
{
	pg_context *context = NULL;
	pg_source *source = NULL, *patch = NULL;
	pg_tree *tree = NULL;
	pg_cursor *cursor = NULL;
	pg_file *file = NULL;
	pg_file_info info;
	pg_error error;
	const char *names[] = { "a", "a-b", "a/b" };
	const char *wanted = prefix ? "a" : NULL;

	STATUS(pg_context_open(&context, &error), PG_OK);
	CHECK(!archive(context, "base", PG_PIGG2,
		names + (overlay ? 1 : 0), overlay ? 2 : 3));
	STATUS(pg_source_open(context, "base", NULL, &source, &error),
		PG_OK);
	if (overlay) {
		CHECK(!archive(context, "patch", PG_PIGG2, names, 1));
		STATUS(pg_source_open(context, "patch", NULL, &patch,
			&error), PG_OK);
		STATUS(pg_tree_create(context, &tree, &error), PG_OK);
		STATUS(pg_tree_attach(tree, source, &error), PG_OK);
		STATUS(pg_tree_attach(tree, patch, &error), PG_OK);
		STATUS(pg_tree_request_subtree(tree, wanted, &error), PG_OK);
		STATUS(pg_tree_files(tree, wanted, &cursor, &error), PG_OK);
	} else {
		STATUS(pg_source_request_subtree(source, wanted, &error),
			PG_OK);
		STATUS(pg_source_files(source, wanted, &cursor, &error), PG_OK);
	}
	if (!prefix) {
		STATUS(pg_cursor_next(cursor, &file, &error), PG_OK);
		STATUS(pg_file_inspect(file, &info, &error), PG_OK);
		CHECK(!strcmp(info.canonical_name, "a-b"));
		STATUS(pg_file_close(&file, &error), PG_OK);
	}
	STATUS(pg_cursor_next(cursor, &file, &error), PG_OK);
	STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	CHECK(!strcmp(info.canonical_name, "a/b"));
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_cursor_next(cursor, &file, &error), PG_END);
	STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	make_dir("out");
	if (overlay)
		STATUS(pg_tree_unpack(tree, "out", PG_OVERWRITE, &error),
			PG_OK);
	else
		STATUS(pg_source_unpack(source, "out", PG_OVERWRITE, &error),
			PG_OK);
	FILE *unpacked = fopen("out/a/b", "rb");

	CHECK(unpacked && !fclose(unpacked));
	STATUS(pg_tree_close(&tree, &error), PG_OK);
	STATUS(pg_source_close(&patch, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static int writer_hierarchy(uint32_t format)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_source_options writable = { format, PG_WRITE };
	pg_error error;
	const char *names[] = { "a/b", "x" };

	STATUS(pg_context_open(&context, &error), PG_OK);
	CHECK(!archive(context, "hierarchy", format, names, 2));
	STATUS(pg_source_open(context, "hierarchy", &writable,
		&source, &error), PG_OK);
	STATUS(pg_source_write_all(source, "a", "bad", 3, NULL, &error),
		PG_CONFLICT);
	STATUS(pg_source_write_all(source, "x/y", "bad", 3, NULL, &error),
		PG_CONFLICT);
	STATUS(pg_source_write_all(source, "a/c", "ok", 2, NULL, &error),
		PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

/* IO-003 / IO-009: absent checksums allow export; real mismatches do not. */
static int export_digest(void)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_archive_builder *builder = NULL;
	pg_error error;
	FILE *stream;
	unsigned char digest[16] = { 0 };

	STATUS(pg_context_open(&context, &error), PG_OK);
	for (size_t size = 0; size <= 4; size += 4) {
		remove("digest.pigg");
		remove("output");
		STATUS(pg_archive_builder_create(context, "digest.pigg",
			PG_PIGG2, 0, &builder, &error), PG_OK);
		STATUS(pg_archive_builder_write_all(builder, "a", "data", size,
			NULL, &error), PG_OK);
		STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
		STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
		for (int mismatch = 0; mismatch < 2; mismatch++) {
			digest[0] = (unsigned char)mismatch;
			stream = fopen("digest.pigg", "r+b");
			CHECK(stream && !fseek(stream, 16 + 28, SEEK_SET));
			CHECK(fwrite(digest, 1, sizeof(digest), stream) ==
				sizeof(digest));
			CHECK(!fclose(stream));
			STATUS(pg_source_open(context, "digest.pigg", NULL,
				&source, &error), PG_OK);
			STATUS(pg_source_export(source, "a", "output", 0,
				&error), mismatch ? PG_CHECKSUM : PG_OK);
			STATUS(pg_source_close(&source, &error), PG_OK);
			if (!mismatch)
				CHECK(!remove("output"));
		}
	}
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

/* DISCOVERY-005 / NAME-002 / TREE-001: normalize and merge observations. */
static int loose_resolution(int normalized)
{
	pg_context *context = NULL;
	pg_source *loose = NULL, *base = NULL;
	pg_tree *tree = NULL;
	pg_file *file = NULL;
	pg_file_info info;
	pg_error error;
	const char *names[] = { "a/b" };

	make_dir("root");
	make_dir("root/menu");
	CHECK(!put_file("root/menu/a", "old"));
	CHECK(!put_file("root/a", "obstacle"));
	STATUS(pg_context_open(&context, &error), PG_OK);
	CHECK(!archive(context, "base", PG_PIGG2, names, 1));
	STATUS(pg_source_open(context, "base", NULL, &base, &error), PG_OK);
	STATUS(pg_source_open(context, "root", NULL, &loose, &error), PG_OK);
	STATUS(pg_tree_create(context, &tree, &error), PG_OK);
	STATUS(pg_tree_attach(tree, base, &error), PG_OK);
	STATUS(pg_tree_attach(tree, loose, &error), PG_OK);
	if (normalized) {
		STATUS(pg_tree_request_subtree(tree, "menu", &error), PG_OK);
		CHECK(!put_file("root/menu/a", "changed"));
		STATUS(pg_tree_find(tree, "MENU\\A", &file, &error), PG_OK);
		STATUS(pg_file_inspect(file, &info, &error), PG_OK);
		CHECK(info.logical_size == 3);
		STATUS(pg_file_close(&file, &error), PG_OK);
	} else {
		for (int requested = 0; requested < 2; requested++) {
			if (requested)
				STATUS(pg_tree_request_subtree(tree, "a",
					&error), PG_OK);
			STATUS(pg_tree_find(tree, "a/b", &file, &error), PG_OK);
			STATUS(pg_file_close(&file, &error), PG_OK);
			STATUS(pg_tree_find(tree, "a", &file, &error),
				PG_CONFLICT);
		}
	}
	STATUS(pg_tree_close(&tree, &error), PG_OK);
	STATUS(pg_source_close(&loose, &error), PG_OK);
	STATUS(pg_source_close(&base, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

#ifndef _WIN32
/* RANK-006: choose the greatest complete path, across all matching branches. */
static int loose_collisions(void)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_file *file = NULL;
	pg_file_info info;
	pg_error error;

	make_dir("root");
	make_dir("root/Dir");
	make_dir("root/dir");
	make_dir("root/DIR");
	make_dir("root/DIR/child");
	CHECK(!put_file("root/Dir/a", "low"));
	CHECK(!put_file("root/dir/A", "high"));
	CHECK(!put_file("root/Dir/b", "only"));
	CHECK(!put_file("root/dir/child", "hidden"));
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_source_open(context, "root", NULL, &source, &error), PG_OK);
	STATUS(pg_source_find(source, "dir/b", &file, &error), PG_OK);
	STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	CHECK(!strcmp(info.original_name, "Dir/b"));
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_source_find(source, "dir/a", &file, &error), PG_OK);
	STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	CHECK(!strcmp(info.original_name, "dir/A"));
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_source_find(source, "dir/child", &file, &error), PG_CONFLICT);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}
#endif

static uint32_t get32(const unsigned char *bytes)
{
	return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
		(uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static void set32(unsigned char *bytes, uint32_t value)
{
	for (int i = 0; i < 4; i++)
		bytes[i] = (unsigned char)(value >> (8 * i));
}

/* Encode the existing DataList, retaining its committed journal. */
static int compress_datalist(const char *path, size_t short_size)
{
	FILE *stream = fopen(path, "r+b");
	unsigned char header[24], record[32], ea[16];
	unsigned char *logical, *encoded;
	long table, record_at, ea_at, payload;
	uint32_t size;
	size_t encoded_size;

	CHECK(stream);
	CHECK(fread(header, 1, sizeof(header), stream) == sizeof(header));
	table = 24 + (header[6] | header[7] << 8) +
		(header[20] | header[21] << 8);
	record_at = table + 32 * get32(header + 16);
	CHECK(!fseek(stream, record_at, SEEK_SET));
	CHECK(fread(record, 1, sizeof(record), stream) == sizeof(record));
	ea_at = table + get32(header + 8) + 16 * get32(record + 28);
	CHECK(!fseek(stream, ea_at, SEEK_SET));
	CHECK(fread(ea, 1, sizeof(ea), stream) == sizeof(ea));
	size = short_size ? (uint32_t)short_size : get32(record + 8);
	logical = calloc(size, 1);
	CHECK(logical);
	if (!short_size) {
		CHECK(!fseek(stream, (long)get32(record), SEEK_SET));
		CHECK(fread(logical, 1, size, stream) == size);
	}
	encoded_size = zng_compressBound(size);
	encoded = malloc(encoded_size);
	CHECK(encoded);
	CHECK(zng_compress(encoded, &encoded_size, logical, size) == Z_OK);
	CHECK(!fseek(stream, 0, SEEK_END));
	payload = ftell(stream);
	CHECK(payload >= 0);
	CHECK(fwrite(encoded, 1, encoded_size, stream) == encoded_size);
	set32(record, (uint32_t)payload);
	set32(record + 8, (uint32_t)encoded_size);
	if (short_size)
		memset(record + 16, 0, 4);
	set32(ea + 8, size);
	CHECK(!fseek(stream, record_at, SEEK_SET));
	CHECK(fwrite(record, 1, sizeof(record), stream) == sizeof(record));
	CHECK(!fseek(stream, ea_at, SEEK_SET));
	CHECK(fwrite(ea, 1, sizeof(ea), stream) == sizeof(ea));
	CHECK(!fclose(stream));
	free(encoded);
	free(logical);
	return 0;
}

/* EDIT-004 / EDIT-006: decoded header bounds and logical DataList IDs. */
static int datalist(int short_data)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_file *file = NULL;
	pg_file_info info;
	pg_source_options writable = { PG_HOGG10, PG_WRITE };
	pg_entry_options entry = { 0 };
	pg_error error;
	const char *names[] = { "a" };

	STATUS(pg_context_open(&context, &error), PG_OK);
	CHECK(!archive(context, "data.hogg", PG_HOGG10, names, 1));
	if (short_data) {
		for (size_t size = 1; size < 8; size++) {
			CHECK(!compress_datalist("data.hogg", size));
			STATUS(pg_source_open(context, "data.hogg", NULL,
				&source, &error), PG_CORRUPT);
		}
	} else {
		STATUS(pg_source_open(context, "data.hogg", &writable,
			&source, &error), PG_OK);
		STATUS(pg_source_write_all(source, "journal", "kept", 4,
			NULL, &error), PG_OK);
		STATUS(pg_source_close(&source, &error), PG_OK);
		CHECK(!compress_datalist("data.hogg", 0));
		STATUS(pg_source_open(context, "data.hogg", &writable,
			&source, &error), PG_OK);
		entry.cached_header = "header";
		entry.cached_header_size = 6;
		STATUS(pg_source_write_all(source, "new", "new", 3,
			&entry, &error), PG_OK);
		STATUS(pg_source_validate(source, &error), PG_OK);
		STATUS(pg_source_close(&source, &error), PG_OK);
		STATUS(pg_source_open(context, "data.hogg", NULL,
			&source, &error), PG_OK);
		STATUS(pg_source_validate(source, &error), PG_OK);
		STATUS(pg_source_find(source, "journal", &file, &error), PG_OK);
		STATUS(pg_file_close(&file, &error), PG_OK);
		STATUS(pg_source_find(source, "new", &file, &error), PG_OK);
		STATUS(pg_file_inspect(file, &info, &error), PG_OK);
		CHECK(info.cached_header_size == 6 &&
			!memcmp(info.cached_header, "header", 6));
		STATUS(pg_file_close(&file, &error), PG_OK);
		STATUS(pg_source_close(&source, &error), PG_OK);
	}
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

#ifdef __linux__
void pg_test_journal_fault(int mode);

/* EDIT-005 / EDIT-009: publication outcomes and recovery gates. */
static int journal_fault(int mode, int metadata)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_file *file = NULL;
	pg_reader *reader = NULL;
	pg_writer *writer = NULL;
	pg_write_options options;
	pg_source_options writable = { PG_HOGG10, PG_WRITE };
	pg_error error;
	const char *names[] = { "a" };
	pg_status expected[] = { PG_OK, PG_IO, PG_INDETERMINATE,
		PG_RECOVERY_REQUIRED, PG_COMMITTED };
	char bytes[4];
	size_t count;

	STATUS(pg_context_open(&context, &error), PG_OK);
	CHECK(!archive(context, "fault.hogg", PG_HOGG10, names, 1));
	STATUS(pg_source_open(context, "fault.hogg", &writable,
		&source, &error), PG_OK);
	STATUS(pg_source_find(source, "a", &file, &error), PG_OK);
	if (mode == 1)
		STATUS(pg_reader_open(file, PG_READ_LOGICAL, &reader,
			&error), PG_OK);
	pg_test_journal_fault(mode);
	if (metadata) {
		pg_metadata_options edit = { PG_METADATA_MTIME, 123, NULL, 0 };

		STATUS(pg_file_update_metadata(file, &edit, &error),
			expected[mode]);
	} else {
		STATUS(pg_source_write_all(source, "a", "new", 3, NULL,
			&error), expected[mode]);
	}
	CHECK(error.cause == PG_IO && error.native_code == EIO);
	pg_test_journal_fault(0);
	if (mode == 1) {
		STATUS(pg_reader_read(reader, bytes, sizeof(bytes), &count,
			&error), PG_OK);
		CHECK(count == 3 && !memcmp(bytes, "old", 3));
		STATUS(pg_reader_close(&reader, &error), PG_OK);
	}
	if (mode > 1) {
		STATUS(pg_reader_open(file, PG_READ_LOGICAL, &reader,
			&error), PG_RECOVERY_REQUIRED);
		STATUS(pg_file_verify(file, &error), PG_RECOVERY_REQUIRED);
		pg_write_options_init(&options, 0);
		STATUS(pg_writer_open_source(source, "another", &options,
			&writer, &error), PG_RECOVERY_REQUIRED);
	}
	STATUS(pg_source_recover(context, "fault.hogg", &error), PG_OK);
	STATUS(pg_source_rescan(source, &error), PG_OK);
	STATUS(pg_source_read_all(source, "a", bytes, sizeof(bytes), &count,
		&error), PG_OK);
	CHECK(count == 3 && !memcmp(bytes, (mode == 1 ||
		metadata) ? "old" : "new", 3));
	STATUS(pg_source_validate(source, &error), PG_OK);
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_source_open(context, "fault.hogg", NULL, &source,
		&error), PG_OK);
	STATUS(pg_source_validate(source, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}
#endif

struct reports {
	unsigned int changes;
	unsigned int losses;
};

static void PG_CALL observe(void *user, const pg_visible_change *change)
{
	struct reports *reports = user;

	if (change->kind == PG_CHANGE_LOSS)
		reports->losses++;
	else
		reports->changes++;
}

/* CHANGE-011: refresh requested and reader observations at watch start. */
static int watch_baseline(uint32_t mode)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_tree *tree = NULL;
	pg_reader *reader = NULL;
	pg_file *file = NULL;
	pg_file_info info;
	pg_error error;
	struct reports reports = { 0 };
	pg_observer observer = { &reports, observe };

	make_dir("root");
	CHECK(!put_file("root/a", "old"));
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_source_open(context, "root", NULL, &source, &error), PG_OK);
	STATUS(pg_tree_create(context, &tree, &error), PG_OK);
	STATUS(pg_tree_attach(tree, source, &error), PG_OK);
	STATUS(pg_tree_request_subtree(tree, NULL, &error), PG_OK);
	STATUS(pg_reader_open_tree(tree, "a", PG_READ_LOGICAL, &reader,
		&error), PG_OK);
	CHECK(!put_file("root/a", "baseline"));
	STATUS(pg_tree_watch(tree, mode, &error), PG_OK);
	STATUS(pg_tree_find(tree, "a", &file, &error), PG_OK);
	STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	CHECK(info.logical_size == 8);
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_tree_poll(tree, &observer, &error), PG_OK);
	CHECK(reports.changes == 0);
	CHECK(!put_file("root/a", "later"));
	STATUS(pg_tree_poll(tree, &observer, &error), PG_OK);
	CHECK(reports.changes == 1);
	STATUS(pg_reader_close(&reader, &error), PG_OK);
	STATUS(pg_tree_close(&tree, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

/* CHANGE-007 / CHANGE-013: replacement must preserve native observation. */
static int watch_repair(void)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_tree *tree = NULL;
	pg_file *file = NULL;
	pg_file_info info;
	pg_source_options writable = { PG_PIGG2, PG_WRITE };
	pg_error error;
	struct reports reports = { 0 };
	pg_observer observer = { &reports, observe };
	const char *names[] = { "a" };
	FILE *stream;
	unsigned char mtime[4] = { 42, 0, 0, 0 };

	STATUS(pg_context_open(&context, &error), PG_OK);
	CHECK(!archive(context, "watch.pigg", PG_PIGG2, names, 1));
	STATUS(pg_source_open(context, "watch.pigg", &writable,
		&source, &error), PG_OK);
	STATUS(pg_tree_create(context, &tree, &error), PG_OK);
	STATUS(pg_tree_attach(tree, source, &error), PG_OK);
	STATUS(pg_tree_request_subtree(tree, NULL, &error), PG_OK);
	STATUS(pg_tree_watch(tree, PG_WATCH_NATIVE, &error), PG_OK);
	STATUS(pg_source_write_all(source, "a", "new", 3, NULL, &error), PG_OK);
	STATUS(pg_tree_poll(tree, &observer, &error), PG_OK);
	CHECK(reports.changes == 1);
	stream = fopen("watch.pigg", "r+b");
	CHECK(stream && !fseek(stream, 16 + 12, SEEK_SET));
	CHECK(fwrite(mtime, 1, sizeof(mtime), stream) == sizeof(mtime));
	CHECK(!fclose(stream));
	STATUS(pg_tree_find(tree, "a", &file, &error), PG_OK);
	STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	CHECK(info.mtime == 42);
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_tree_poll(tree, &observer, &error), PG_OK);
	CHECK(reports.changes == 2);
	STATUS(pg_tree_close(&tree, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

/* CHANGE-011: failed setup preserves prior observations and watching OFF. */
static int watch_rollback(uint32_t mode)
{
	pg_context *context = NULL;
	pg_source *source = NULL, *broken = NULL;
	pg_tree *tree = NULL;
	pg_file *file = NULL;
	pg_file_info info;
	pg_error error;
	pg_observer observer = { 0 };
	const char *names[] = { "b" };

	make_dir("root");
	CHECK(!put_file("root/a", "old"));
	STATUS(pg_context_open(&context, &error), PG_OK);
	CHECK(!archive(context, "broken", PG_PIGG2, names, 1));
	STATUS(pg_source_open(context, "root", NULL, &source, &error), PG_OK);
	STATUS(pg_source_open(context, "broken", NULL, &broken, &error), PG_OK);
	STATUS(pg_tree_create(context, &tree, &error), PG_OK);
	STATUS(pg_tree_attach(tree, source, &error), PG_OK);
	STATUS(pg_tree_attach(tree, broken, &error), PG_OK);
	STATUS(pg_tree_request_subtree(tree, NULL, &error), PG_OK);
	STATUS(pg_tree_manage(tree, NULL, PG_DISCOVER_RECURSIVE, &error),
		PG_OK);
	CHECK(!put_file("root/a", "changed"));
	CHECK(!put_file("broken", "bad"));
	STATUS(pg_tree_watch(tree, mode, &error), PG_CORRUPT);
	STATUS(pg_tree_poll(tree, &observer, &error), PG_INVALID);
	STATUS(pg_tree_find(tree, "a", &file, &error), PG_OK);
	STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	CHECK(info.logical_size == 3);
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_tree_close(&tree, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_source_close(&broken, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static int root_rollback(void)
{
	pg_context *context = NULL;
	pg_tree *tree = NULL;
	pg_file *file = NULL;
	pg_file_info info;
	pg_error error;
	pg_observer observer = { 0 };
	pg_source_spec sources[] = {
		{ "root", { PG_LOOSE, PG_READ, 0 } },
		{ "broken", { PG_PIGG2, PG_READ, 0 } }
	};
	const char *names[] = { "b" };

	make_dir("root");
	CHECK(!put_file("root/a", "old"));
	STATUS(pg_context_open(&context, &error), PG_OK);
	CHECK(!archive(context, "broken", PG_PIGG2, names, 1));
	STATUS(pg_tree_open(context, sources, 2, &tree, &error), PG_OK);
	STATUS(pg_tree_request_subtree(tree, NULL, &error), PG_OK);
	STATUS(pg_tree_manage(tree, NULL, PG_DISCOVER_RECURSIVE, &error),
		PG_OK);
	STATUS(pg_tree_watch(tree, PG_WATCH_NATIVE, &error), PG_OK);
	CHECK(!rename("root", "moved"));
	make_dir("root");
	CHECK(!put_file("root/a", "replacement"));
	CHECK(!put_file("broken", "bad"));
	STATUS(pg_tree_poll(tree, &observer, &error), PG_CORRUPT);
	STATUS(pg_tree_unwatch(tree, &error), PG_OK);
	STATUS(pg_tree_find(tree, "a", &file, &error), PG_OK);
	STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	CHECK(info.logical_size == 3);
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_tree_close(&tree, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

/* CHANGE-003: queued reports survive close; new observation stops. */
static int watch_worker(void)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_tree *tree = NULL;
	pg_source_options writable = { PG_PIGG2, PG_WRITE };
	pg_error error;
	struct worker_close work = { 0 };
	struct reports reports = { 0 };
	pg_observer observer = { &reports, observe };
	const char *names[] = { "a" };

	STATUS(pg_context_open(&context, &error), PG_OK);
	CHECK(!archive(context, "worker-watch", PG_PIGG2, names, 1));
	STATUS(pg_source_open(context, "worker-watch", &writable,
		&source, &error), PG_OK);
	STATUS(pg_tree_create(context, &tree, &error), PG_OK);
	STATUS(pg_tree_attach(tree, source, &error), PG_OK);
	STATUS(pg_reader_open_tree(tree, "a", PG_READ_LOGICAL,
		&work.reader, &error), PG_OK);
	STATUS(pg_tree_watch(tree, PG_WATCH_SCAN, &error), PG_OK);
	/* Attachment queues a report while the reader still owns its scope. */
	STATUS(pg_tree_detach(tree, source, &error), PG_OK);
	CHECK(!run_worker(&work));
	CHECK(work.status == PG_OK);
	STATUS(pg_tree_attach(tree, source, &error), PG_OK);
	STATUS(pg_tree_poll(tree, &observer, &error), PG_OK);
	CHECK(reports.changes == 1);
	STATUS(pg_tree_close(&tree, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

#ifdef _WIN32
static int set_native_time(const char *path, uint64_t ticks)
{
	HANDLE file = CreateFileA(path, FILE_WRITE_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	FILETIME time = { (DWORD)ticks, (DWORD)(ticks >> 32) };

	CHECK(file != INVALID_HANDLE_VALUE);
	CHECK(SetFileTime(file, NULL, NULL, &time));
	CHECK(CloseHandle(file));
	return 0;
}

static int windows_subsecond(void)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_file *file = NULL;
	pg_reader *reader = NULL, *native = NULL;
	pg_file_info before, after;
	pg_error error;
	char bytes[3];
	size_t count;
	uint64_t ticks = 133000000000000000ULL;

	make_dir("root");
	CHECK(!put_file("root/a", "old"));
	CHECK(!set_native_time("root/a", ticks + 1000000));
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_source_open(context, "root", NULL, &source, &error), PG_OK);
	STATUS(pg_source_request_subtree(source, NULL, &error), PG_OK);
	STATUS(pg_source_find(source, "a", &file, &error), PG_OK);
	STATUS(pg_file_inspect(file, &before, &error), PG_OK);
	STATUS(pg_reader_open(file, PG_READ_LOGICAL, &reader, &error), PG_OK);
	STATUS(pg_reader_open_native(context, "root/a", &native, &error),
		PG_OK);
	STATUS(pg_file_close(&file, &error), PG_OK);
	CHECK(!put_file("root/a", "new"));
	CHECK(!set_native_time("root/a", ticks + 2000000));
	STATUS(pg_reader_read(reader, bytes, sizeof(bytes), &count, &error),
		PG_STALE);
	CHECK(!count);
	STATUS(pg_reader_seek(native, 0, &error), PG_STALE);
	STATUS(pg_source_rescan(source, &error), PG_OK);
	STATUS(pg_source_find(source, "a", &file, &error), PG_OK);
	STATUS(pg_file_inspect(file, &after, &error), PG_OK);
	CHECK(before.mtime == after.mtime);
	CHECK(before.copy_generation != after.copy_generation);
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_reader_close(&reader, &error), PG_OK);
	STATUS(pg_reader_close(&native, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

/* NAME-005: native UTF-8 paths are independent of the ANSI code page. */
static int windows_native(int cli)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_tree *tree = NULL;
	pg_cursor *cursor = NULL;
	pg_file *file = NULL;
	pg_reader *reader = NULL;
	pg_writer *writer = NULL;
	pg_archive_builder *builder = NULL;
	pg_write_options options;
	pg_source_options writable = { PG_LOOSE, PG_WRITE };
	pg_error error;
	WCHAR cwd[1024], root[1200], output[1200], native_file[1200];
	char root_utf8[4800], output_utf8[4800], native_utf8[4800];
	const char *name = "\xe7\x8c\xab";
	char bytes[4];
	size_t count;

	CHECK(GetACP() != CP_UTF8);
	CHECK(GetCurrentDirectoryW(1024, cwd));
	swprintf(root, 1200, L"%ls\\\x732b-root", cwd);
	swprintf(output, 1200, L"%ls\\\x732b-out", cwd);
	CHECK(CreateDirectoryW(root, NULL));
	CHECK(CreateDirectoryW(output, NULL));
	CHECK(WideCharToMultiByte(CP_UTF8, 0, root, -1, root_utf8,
		sizeof(root_utf8), NULL, NULL));
	CHECK(WideCharToMultiByte(CP_UTF8, 0, output, -1, output_utf8,
		sizeof(output_utf8), NULL, NULL));
	CHECK(SetCurrentDirectoryW(root));
	STATUS(pg_context_open(&context, &error), PG_OK);
	pg_write_options_init(&options, 3);
	STATUS(pg_writer_open_native(context, name, &options, 0, &writer,
		&error), PG_OK);
	STATUS(pg_writer_write(writer, "new", 3, &count, &error), PG_OK);
	STATUS(pg_writer_finish(writer, &error), PG_OK);
	STATUS(pg_writer_close(&writer, &error), PG_OK);
	swprintf(native_file, 1200, L"%ls\\\x732b", root);
	CHECK(GetFileAttributesW(native_file) != INVALID_FILE_ATTRIBUTES);
	STATUS(pg_reader_open_native(context, name, &reader, &error), PG_OK);
	STATUS(pg_reader_read(reader, bytes, sizeof(bytes), &count,
		&error), PG_OK);
	CHECK(count == 3 && !memcmp(bytes, "new", 3));
	STATUS(pg_reader_close(&reader, &error), PG_OK);
	STATUS(pg_archive_builder_create(context, "archive", PG_PIGG2, 0,
		&builder, &error), PG_OK);
	STATUS(pg_archive_builder_write_all(builder, name, "new", 3, NULL,
		&error), PG_OK);
	STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	CHECK(SetCurrentDirectoryW(cwd));
	STATUS(pg_source_open(context, root_utf8, &writable, &source,
		&error), PG_OK);
	STATUS(pg_source_find(source, name, &file, &error), PG_OK);
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_tree_create(context, &tree, &error), PG_OK);
	STATUS(pg_tree_attach(tree, source, &error), PG_OK);
	STATUS(pg_tree_request_subtree(tree, NULL, &error), PG_OK);
	STATUS(pg_tree_watch(tree, PG_WATCH_NATIVE, &error), PG_OK);
	STATUS(pg_tree_find(tree, name, &file, &error), PG_OK);
	STATUS(pg_file_read_all(file, bytes, sizeof(bytes), &count,
		&error), PG_OK);
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_tree_files(tree, NULL, &cursor, &error), PG_OK);
	STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	STATUS(pg_tree_close(&tree, &error), PG_OK);
#ifdef PIGGLE_TEST_CLI_PATH
	if (cli) {
		WCHAR executable[1200], command[4800];
		STARTUPINFOW startup = { 0 };
		PROCESS_INFORMATION process = { 0 };
		DWORD result;

		CHECK(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
			PIGGLE_TEST_CLI_PATH, -1, executable, 1200));
		swprintf(command, 4800, L"\"%ls\" extract \"%ls\\archive\" "
			L"\"%ls\"", executable, root, output);
		startup.cb = sizeof(startup);
		CHECK(CreateProcessW(executable, command, NULL, NULL, FALSE,
			0, NULL, NULL, &startup, &process));
		CHECK(WaitForSingleObject(process.hProcess, 30000) ==
			WAIT_OBJECT_0);
		CHECK(GetExitCodeProcess(process.hProcess, &result));
		CHECK(!result);
		CHECK(CloseHandle(process.hThread));
		CHECK(CloseHandle(process.hProcess));
	} else
#else
	CHECK(!cli);
#endif
	{
		STATUS(pg_source_unpack(source, output_utf8, 0, &error), PG_OK);
	}
	swprintf(native_file, 1200, L"%ls\\\x732b", output);
	CHECK(GetFileAttributesW(native_file) != INVALID_FILE_ATTRIBUTES);
	CHECK(WideCharToMultiByte(CP_UTF8, 0, native_file, -1, native_utf8,
		sizeof(native_utf8), NULL, NULL));
	STATUS(pg_reader_open_native(context, native_utf8, &reader,
		&error), PG_OK);
	STATUS(pg_reader_read(reader, bytes, sizeof(bytes), &count,
		&error), PG_OK);
	CHECK(count == 3 && !memcmp(bytes, "new", 3));
	STATUS(pg_reader_close(&reader, &error), PG_OK);
	STATUS(pg_source_delete(source, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	CHECK(GetFileAttributesW(root) == INVALID_FILE_ATTRIBUTES);
	STATUS(pg_source_open(context, output_utf8, &writable, &source,
		&error), PG_OK);
	STATUS(pg_source_delete(source, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static int windows_paths(int unicode)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_archive_builder *builder = NULL;
	pg_source_options writable = { PG_PIGG2, PG_WRITE };
	pg_error error;
	WCHAR directory[1024], target[1200];
	char path[4800];
	const WCHAR *leaf = unicode ? L"\\\x732b.pigg" : L"\\absolute.pigg";
	const char *name = unicode ? "\xe7\x8c\xab" : "a";
	pg_file *file = NULL;
	pg_file_info info;

	CHECK(GetACP() != CP_UTF8);
	CHECK(GetCurrentDirectoryW(1024, directory));
	wcscpy(target, directory);
	wcscat(target, leaf);
	DeleteFileW(target);
	CHECK(WideCharToMultiByte(CP_UTF8, 0, target, -1, path,
		sizeof(path), NULL, NULL));
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_archive_builder_create(context, path, PG_PIGG2, 0,
		&builder, &error), PG_OK);
	STATUS(pg_archive_builder_write_all(builder, name, "old", 3,
		NULL, &error), PG_OK);
	STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	CHECK(GetFileAttributesW(target) != INVALID_FILE_ATTRIBUTES);
	STATUS(pg_source_open(context, path, &writable, &source,
		&error), PG_OK);
	STATUS(pg_source_write_all(source, name, "new", 3, NULL, &error),
		PG_OK);
	STATUS(pg_source_validate(source, &error), PG_OK);
	STATUS(pg_source_find(source, name, &file, &error), PG_OK);
	STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	CHECK(!strcmp(info.canonical_name, name));
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	CHECK(DeleteFileW(target));
	/* Mixed separators still select the same parent and basename. */
	for (char *at = path; *at; at++) {
		if (*at == '\\') {
			*at = '/';
			break;
		}
	}
	STATUS(pg_archive_builder_create(context, path, PG_PIGG2, 0,
		&builder, &error), PG_OK);
	STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	CHECK(DeleteFileW(target));
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}
#endif

/* HANDLE-006 / IO-012: a cursor survives release of its originating tree. */
static int retained_unpack(void)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_tree *tree = NULL;
	pg_cursor *cursor = NULL;
	pg_file *file = NULL;
	pg_unpack_target *target = NULL;
	pg_writer *writer = NULL;
	pg_error error;
	const char *names[] = { "a" };
	char bytes[4];
	size_t count, written;

	make_dir("out");
	remove("out/a");
	STATUS(pg_context_open(&context, &error), PG_OK);
	CHECK(!archive(context, "unpack", PG_PIGG2, names, 1));
	STATUS(pg_source_open(context, "unpack", NULL, &source, &error), PG_OK);
	STATUS(pg_tree_create(context, &tree, &error), PG_OK);
	STATUS(pg_tree_attach(tree, source, &error), PG_OK);
	STATUS(pg_tree_files(tree, NULL, &cursor, &error), PG_OK);
	STATUS(pg_tree_close(&tree, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_unpack_target_open(cursor, "out", 0, &target, &error), PG_OK);
	STATUS(pg_cursor_next(cursor, &file, &error), PG_OK);
	STATUS(pg_file_read_all(file, bytes, sizeof(bytes), &count,
		&error), PG_OK);
	STATUS(pg_writer_open_unpack(target, file, &writer, &error), PG_OK);
	STATUS(pg_writer_write(writer, bytes, count, &written, &error), PG_OK);
	CHECK(written == count);
	STATUS(pg_writer_finish(writer, &error), PG_OK);
	STATUS(pg_writer_close(&writer, &error), PG_OK);
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_unpack_target_close(&target, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_BUSY);
	STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

#ifndef _WIN32
/* NAME-005: a POSIX backslash stays part of the native basename. */
static int posix_basename(void)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_source_options writable = { PG_PIGG2, PG_WRITE };
	pg_error error;
	const char *names[] = { "a" };

	STATUS(pg_context_open(&context, &error), PG_OK);
	CHECK(!archive(context, "native\\archive.pigg", PG_PIGG2, names, 1));
	STATUS(pg_source_open(context, "native\\archive.pigg", &writable,
		&source, &error), PG_OK);
	STATUS(pg_source_write_all(source, "a", "new", 3, NULL, &error), PG_OK);
	STATUS(pg_source_validate(source, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	CHECK(!remove("native\\archive.pigg"));
	return 0;
}
#endif

/* EDIT-011: rejection must not invalidate an unrelated reader. */
static int hogg_busy(void)
{
	pg_context *context = NULL;
	pg_source *source = NULL, *second = NULL;
	pg_context *other = NULL;
	pg_reader *reader = NULL;
	pg_file *file = NULL;
	pg_source_options options = { PG_HOGG10, PG_WRITE };
	const char *names[] = { "a" };
	struct stat before, after;
	unsigned char bytes[32];
	size_t count;
	pg_error error;

	STATUS(pg_context_open(&context, &error), PG_OK);
	CHECK(!archive(context, "busy.hogg", PG_HOGG10, names, 1));
	STATUS(pg_source_open(context, "busy.hogg", &options, &source,
		&error), PG_OK);
	STATUS(pg_reader_open_source(source, "a", PG_READ_LOGICAL,
		&reader, &error), PG_OK);
	STATUS(pg_source_find(source, "a", &file, &error), PG_OK);
	CHECK(!stat("busy.hogg", &before));
	STATUS(pg_context_open(&other, &error), PG_OK);
	STATUS(pg_source_open(other, "busy.hogg", &options, &second,
		&error), PG_BUSY);
	CHECK(!stat("busy.hogg", &after));
	CHECK(before.st_size == after.st_size);
	STATUS(pg_reader_read(reader, bytes, sizeof(bytes), &count,
		&error), PG_OK);
	CHECK(count == 3);
	CHECK(!memcmp(bytes, "old", count));
	for (unsigned i = 0; i < 40; i++) {
		char name[32];

		snprintf(name, sizeof(name), "new%u", i);
		STATUS(pg_source_write_all(source, name, "new", 3, NULL,
			&error), PG_OK);
		STATUS(pg_reader_seek(reader, 0, &error), PG_OK);
		STATUS(pg_reader_read(reader, bytes, sizeof(bytes), &count,
			&error), PG_OK);
		CHECK(count == 3 && !memcmp(bytes, "old", 3));
	}
	STATUS(pg_file_verify(file, &error), PG_OK);
	STATUS(pg_file_write_all(file, "replacement", 11,
		NULL, &error), PG_OK);
	STATUS(pg_reader_seek(reader, 0, &error), PG_STALE);
	STATUS(pg_reader_close(&reader, &error), PG_OK);
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_source_write_all(source, "b", "new", 3, NULL, &error),
		PG_OK);
	STATUS(pg_source_validate(source, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_source_open(other, "busy.hogg", &options, &second,
		&error), PG_OK);
	STATUS(pg_source_close(&second, &error), PG_OK);
	STATUS(pg_context_close(&other, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	if (!strcmp(argv[1], "hogg_busy"))
		return hogg_busy();
	if (!strcmp(argv[1], "retained_unpack"))
		return retained_unpack();
#ifndef _WIN32
	if (!strcmp(argv[1], "posix_basename"))
		return posix_basename();
#endif
	if (!strcmp(argv[1], "pigg_controls"))
		return writer_controls(PG_PIGG2);
	if (!strcmp(argv[1], "hogg_controls"))
		return writer_controls(PG_HOGG10);
	if (!strcmp(argv[1], "loose_writer_ancestor"))
		return loose_writer_ancestor();
#ifdef _WIN32
	if (!strcmp(argv[1], "windows_subsecond"))
		return windows_subsecond();
	if (!strcmp(argv[1], "windows_native"))
		return windows_native(0);
	if (!strcmp(argv[1], "windows_cli"))
		return windows_native(1);
	if (!strcmp(argv[1], "windows_absolute"))
		return windows_paths(0);
	if (!strcmp(argv[1], "windows_unicode"))
		return windows_paths(1);
#endif
	if (!strcmp(argv[1], "scan_baseline"))
		return watch_baseline(PG_WATCH_SCAN);
	if (!strcmp(argv[1], "native_baseline"))
		return watch_baseline(PG_WATCH_NATIVE);
	if (!strcmp(argv[1], "scan_rollback"))
		return watch_rollback(PG_WATCH_SCAN);
	if (!strcmp(argv[1], "native_rollback"))
		return watch_rollback(PG_WATCH_NATIVE);
	if (!strcmp(argv[1], "root_rollback"))
		return root_rollback();
	if (!strcmp(argv[1], "watch_repair"))
		return watch_repair();
	if (!strcmp(argv[1], "watch_worker"))
		return watch_worker();
	if (!strcmp(argv[1], "short_datalist"))
		return datalist(1);
	if (!strcmp(argv[1], "compressed_datalist"))
		return datalist(0);
#ifdef __linux__
	if (!strncmp(argv[1], "journal_", 8))
		return journal_fault(atoi(argv[1] + 8), 0);
	if (!strncmp(argv[1], "metadata_fault_", 15))
		return journal_fault(atoi(argv[1] + 15), 1);
#endif
	if (!strcmp(argv[1], "normalized"))
		return loose_resolution(1);
	if (!strcmp(argv[1], "ancestor"))
		return loose_resolution(0);
#ifndef _WIN32
	if (!strcmp(argv[1], "loose_collisions"))
		return loose_collisions();
#endif
	if (!strcmp(argv[1], "empty_cursor"))
		return cursor_lifetime(0);
	if (!strcmp(argv[1], "exhausted_cursor"))
		return cursor_lifetime(1);
	if (!strcmp(argv[1], "loose_lease"))
		return loose_reader(0);
	if (!strcmp(argv[1], "loose_alias"))
		return loose_reader(1);
	if (!strcmp(argv[1], "worker_reader"))
		return worker_cleanup(0);
	if (!strcmp(argv[1], "worker_cursor"))
		return worker_cleanup(1);
	if (!strcmp(argv[1], "pigg_writer"))
		return writer_stale(PG_PIGG2, 0);
	if (!strcmp(argv[1], "pigg_selected"))
		return writer_stale(PG_PIGG2, 1);
	if (!strcmp(argv[1], "hogg_writer"))
		return writer_stale(PG_HOGG10, 0);
	if (!strcmp(argv[1], "hogg_selected"))
		return writer_stale(PG_HOGG10, 1);
	if (!strcmp(argv[1], "source_hierarchy"))
		return hierarchy(0, 0);
	if (!strcmp(argv[1], "source_prefix"))
		return hierarchy(0, 1);
	if (!strcmp(argv[1], "tree_hierarchy"))
		return hierarchy(1, 0);
	if (!strcmp(argv[1], "tree_prefix"))
		return hierarchy(1, 1);
	if (!strcmp(argv[1], "pigg_hierarchy"))
		return writer_hierarchy(PG_PIGG2);
	if (!strcmp(argv[1], "hogg_hierarchy"))
		return writer_hierarchy(PG_HOGG10);
	if (!strcmp(argv[1], "export_digest"))
		return export_digest();
	return 1;
}
