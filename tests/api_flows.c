#include <piggle/piggle.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#else
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define CHECK(expr) do { \
	if (!(expr)) { \
		fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
		return 1; \
	} \
} while (0)

#define STATUS(call, expected) do { \
	pg_status status = (call); \
	if (status != (expected) || error.status != status) { \
		fprintf(stderr, "%s:%d: %s returned %d, error %d\n", \
			__FILE__, __LINE__, #call, (int)status, \
			(int)error.status); \
		return 1; \
	} \
} while (0)

static int make_root(void)
{
#if defined(_WIN32)
	int status = _mkdir("root");
#else
	int status = mkdir("root", 0700);
#endif

	return status == 0 || errno == EEXIST;
}

static int put_file(const char *path, const char *bytes, size_t size)
{
	if (strncmp(path, "root/", 5) == 0)
		CHECK(make_root());
	FILE *stream = fopen(path, "wb");

	CHECK(stream != NULL);
	CHECK(fwrite(bytes, 1, size, stream) == size);
	CHECK(fclose(stream) == 0);
	return 0;
}

static int has_file(const char *path, const char *bytes, size_t size)
{
	FILE *stream = fopen(path, "rb");
	char actual[64];

	CHECK(stream != NULL);
	CHECK(size <= sizeof(actual));
	CHECK(fread(actual, 1, size, stream) == size);
	CHECK(fgetc(stream) == EOF);
	CHECK(fclose(stream) == 0);
	CHECK(memcmp(actual, bytes, size) == 0);
	return 0;
}

static int test_readers_and_files(void)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_tree *tree = NULL;
	pg_file *file = NULL;
	pg_reader *reader = NULL;
	pg_file_info info;
	pg_reader_info reader_info;
	pg_buffer allocated = { NULL, 0 };
	pg_error error;
	char bytes[4] = { 0 };
	size_t count = 0;

	CHECK(put_file("root/a", "abc", 3) == 0);
	remove("exported");
	remove("exported_step");
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_source_open(context, "root", NULL, &source, &error),
		PG_OK);
	STATUS(pg_tree_create(context, &tree, &error), PG_OK);
	STATUS(pg_tree_attach(tree, source, &error), PG_OK);
	STATUS(pg_tree_find(tree, "A", &file, &error), PG_OK);
	STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	CHECK(strcmp(info.canonical_name, "a") == 0);
	CHECK(info.logical_size == 3);

	STATUS(pg_reader_open(file, PG_READ_LOGICAL, &reader, &error),
		PG_OK);
	STATUS(pg_reader_inspect(reader, &reader_info, &error), PG_OK);
	CHECK(reader_info.size == 3);
	STATUS(pg_reader_read(reader, bytes, 2, &count, &error), PG_OK);
	CHECK(count == 2 && memcmp(bytes, "ab", 2) == 0);
	STATUS(pg_reader_close(&reader, &error), PG_OK);
	STATUS(pg_reader_open_source(source, "a", PG_READ_LOGICAL,
		&reader, &error), PG_OK);
	STATUS(pg_reader_read(reader, bytes, 3, &count, &error), PG_OK);
	CHECK(count == 3 && memcmp(bytes, "abc", 3) == 0);
	STATUS(pg_reader_close(&reader, &error), PG_OK);
	STATUS(pg_reader_open_tree(tree, "a", PG_READ_LOGICAL,
		&reader, &error), PG_OK);
	STATUS(pg_reader_read(reader, bytes, 3, &count, &error), PG_OK);
	CHECK(count == 3 && memcmp(bytes, "abc", 3) == 0);
	STATUS(pg_reader_close(&reader, &error), PG_OK);
	STATUS(pg_reader_open_native(context, "root/a", &reader,
		&error), PG_OK);
	STATUS(pg_reader_close(&reader, &error), PG_OK);

	STATUS(pg_file_read_all(file, bytes, 2, &count, &error),
		PG_CAPACITY);
	CHECK(count == 0);
	STATUS(pg_file_read_all(file, bytes, sizeof(bytes), &count,
		&error), PG_OK);
	CHECK(count == 3 && memcmp(bytes, "abc", 3) == 0);
	STATUS(pg_file_read_all_alloc(file, 2, &allocated, &error),
		PG_LIMIT);
	CHECK(allocated.data == NULL && allocated.size == 0);
	STATUS(pg_file_read_all_alloc(file, 3, &allocated, &error),
		PG_OK);
	CHECK(allocated.size == 3);
	CHECK(memcmp(allocated.data, "abc", 3) == 0);
	pg_buffer_free(&allocated);
	STATUS(pg_file_verify(file, &error), PG_NO_CHECKSUM);
	STATUS(pg_file_verify(file, &error), PG_NO_CHECKSUM);

	STATUS(pg_file_export(file, "exported", 0, &error),
		PG_OK);

	CHECK(has_file("exported", "abc", 3) == 0);
	STATUS(pg_file_export(file, "exported_step", 0, &error), PG_OK);

	CHECK(has_file("exported_step", "abc", 3) == 0);
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_tree_detach(tree, source, &error), PG_OK);
	STATUS(pg_tree_close(&tree, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static int test_writers_and_deletes(void)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_file *file = NULL;
	pg_writer *writer = NULL;
	pg_source_options source_options = { PG_LOOSE, PG_WRITE };
	pg_write_options options;
	pg_error error;
	size_t count = 0;

	CHECK(put_file("root/a", "old", 3) == 0);
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_source_open(context, "root", &source_options, &source,
		&error), PG_OK);
	pg_write_options_init(&options, 3);
	STATUS(pg_writer_open_source(source, "b", &options, &writer,
		&error), PG_OK);
	STATUS(pg_writer_write(writer, "new", 3, &count, &error), PG_OK);
	CHECK(count == 3);
	STATUS(pg_writer_finish(writer, &error), PG_OK);

	STATUS(pg_writer_close(&writer, &error), PG_OK);
	CHECK(has_file("root/b", "new", 3) == 0);
	STATUS(pg_source_find(source, "a", &file, &error), PG_OK);
	STATUS(pg_writer_open_file(file, &options, &writer, &error), PG_OK);
	STATUS(pg_writer_write(writer, "one", 3, &count, &error), PG_OK);
	STATUS(pg_writer_finish(writer, &error), PG_OK);

	STATUS(pg_writer_close(&writer, &error), PG_OK);
	STATUS(pg_file_close(&file, &error), PG_OK);
	CHECK(has_file("root/a", "one", 3) == 0);
	STATUS(pg_source_find(source, "a", &file, &error), PG_OK);
	STATUS(pg_file_write_all(file, "two", 3, NULL, &error), PG_OK);
	STATUS(pg_file_close(&file, &error), PG_OK);
	CHECK(has_file("root/a", "two", 3) == 0);
	STATUS(pg_source_find(source, "a", &file, &error), PG_OK);
	STATUS(pg_file_delete(file, &error), PG_OK);

	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_source_find(source, "b", &file, &error), PG_OK);
	STATUS(pg_file_delete(file, &error), PG_OK);

	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}















static int test_writer_excess(void)
{
	pg_context *context = NULL;
	pg_writer *writer = NULL;
	pg_write_options options;
	pg_error error;
	size_t count = 99;
	FILE *stream;

	remove("excess.bin");
	STATUS(pg_context_open(&context, &error), PG_OK);
	pg_write_options_init(&options, 3);
	STATUS(pg_writer_open_native(context, "excess.bin", &options, 0,
		&writer, &error), PG_OK);
	STATUS(pg_writer_write(writer, "abcd", 4, &count, &error),
		PG_INVALID);
	CHECK(count == 0);
	STATUS(pg_writer_close(&writer, &error), PG_OK);
	stream = fopen("excess.bin", "rb");
	CHECK(stream == NULL);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static int test_builder_workflows(void)
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_source *source = NULL;
	pg_file *file = NULL;
	pg_writer *writer = NULL;
	pg_write_options options;
	pg_error error;
	size_t count = 0;

	CHECK(put_file("root/a", "abc", 3) == 0);
	CHECK(put_file("native", "def", 3) == 0);
	remove("built.pigg");
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_source_open(context, "root", NULL, &source, &error),
		PG_OK);
	STATUS(pg_source_find(source, "a", &file, &error), PG_OK);
	STATUS(pg_archive_builder_create(context, "built.pigg", PG_PIGG2,
		0, &builder, &error), PG_OK);
	STATUS(pg_archive_builder_write_all(builder, "zero", NULL, 0,
		NULL, &error), PG_OK);
	STATUS(pg_archive_builder_import(builder, "imported", "native",
		PG_COMPRESS_NEVER, &error), PG_OK);
	STATUS(pg_archive_builder_import(builder, "imported_step",
		"native", PG_COMPRESS_NEVER, &error), PG_OK);

	STATUS(pg_archive_builder_copy(builder, "copied", file,
		PG_COMPRESS_NEVER, &error), PG_OK);
	STATUS(pg_archive_builder_copy(builder, "copied_step", file,
		PG_COMPRESS_NEVER, &error), PG_OK);
	pg_write_options_init(&options, 3);
	STATUS(pg_writer_open_archive_builder(builder, "streamed", &options,
		&writer, &error), PG_OK);
	STATUS(pg_archive_builder_finish(builder, &error), PG_BUSY);
	STATUS(pg_archive_builder_close(&builder, &error), PG_BUSY);
	STATUS(pg_writer_write(writer, "ghi", 3, &count, &error), PG_OK);
	CHECK(count == 3);
	STATUS(pg_writer_finish(writer, &error), PG_OK);

	STATUS(pg_writer_close(&writer, &error), PG_OK);
	STATUS(pg_archive_builder_finish(builder, &error), PG_OK);


	STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static int test_source_workflows(void)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_file *file = NULL;
	pg_cursor *cursor = NULL;
	pg_source_options options = { PG_LOOSE, PG_WRITE };
	pg_source_info info;
	pg_buffer allocated = { NULL, 0 };
	pg_error error;
	char bytes[4] = { 0 };
	size_t count = 0;

	CHECK(put_file("root/a", "abc", 3) == 0);
	CHECK(put_file("native", "def", 3) == 0);
	remove("exported");
	remove("exported_step");
	remove("packed.pigg");
	remove("packed_step.pigg");
	remove("out/a");
	remove("out/b");
	remove("out/c");
	remove("out/d");
	remove("out/e");
	remove("out/f");
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_source_open(context, "root", &options, &source, &error),
		PG_OK);
	STATUS(pg_source_inspect(source, &info, &error), PG_OK);
	CHECK(info.id != 0 && info.access == PG_WRITE);
	STATUS(pg_source_read_all(source, "a", bytes, sizeof(bytes),
		&count, &error), PG_OK);
	CHECK(count == 3 && memcmp(bytes, "abc", 3) == 0);
	STATUS(pg_source_read_all_alloc(source, "a", 3, &allocated,
		&error), PG_OK);
	CHECK(allocated.size == 3);
	pg_buffer_free(&allocated);
	STATUS(pg_source_write_all(source, "b", "bbb", 3, NULL, &error), PG_OK);

	STATUS(pg_source_import(source, "c", "native",
		PG_COMPRESS_NEVER, &error), PG_OK);
	STATUS(pg_source_import(source, "d", "native",
		PG_COMPRESS_NEVER, &error), PG_OK);
	STATUS(pg_source_find(source, "a", &file, &error), PG_OK);
	STATUS(pg_source_copy(source, "e", file, PG_COMPRESS_NEVER, &error), PG_OK);
	STATUS(pg_source_copy(source, "f", file,
		PG_COMPRESS_NEVER, &error), PG_OK);
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_source_export(source, "a", "exported", 0, &error), PG_OK);
	CHECK(has_file("exported", "abc", 3) == 0);
	STATUS(pg_source_export(source, "a", "exported_step", 0, &error), PG_OK);
	CHECK(has_file("exported_step", "abc", 3) == 0);
	STATUS(pg_source_request_subtree(source, NULL, &error), PG_OK);
	STATUS(pg_source_files(source, NULL, &cursor, &error), PG_OK);
	STATUS(pg_cursor_next(cursor, &file, &error), PG_OK);
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	STATUS(pg_source_rescan(source, &error), PG_OK);
	STATUS(pg_source_validate(source, &error), PG_OK);
	STATUS(pg_source_validate(source, &error), PG_OK);
	STATUS(pg_source_pack(source, "packed.pigg", PG_PIGG2,
		NULL, &error), PG_OK);

	STATUS(pg_source_pack(source, "packed_step.pigg", PG_PIGG2,
		NULL, &error), PG_OK);

	STATUS(pg_source_unpack(source, "out", 0, &error),
		PG_OK);
	CHECK(has_file("out/a", "abc", 3) == 0);
	STATUS(pg_source_unpack(source, "out", PG_OVERWRITE, &error), PG_OK);
	STATUS(pg_source_recover(context, "missing.hogg", &error), PG_NOT_FOUND);
	STATUS(pg_source_recover(context, "missing.hogg", &error), PG_NOT_FOUND);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}



static int test_source_delete(void)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_source_options options = { PG_LOOSE, PG_WRITE };
	pg_error error;

	CHECK(put_file("root/a", "abc", 3) == 0);
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_source_open(context, "root", &options, &source, &error),
		PG_OK);
	STATUS(pg_source_delete(source, &error), PG_OK);

	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}



static int test_source_delete_blocking(void)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_source_options options = { PG_LOOSE, PG_WRITE };
	pg_error error;

	CHECK(put_file("root/a", "abc", 3) == 0);
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_source_open(context, "root", &options, &source, &error),
		PG_OK);
	STATUS(pg_source_delete(source, &error), PG_OK);

	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static void PG_CALL collect_change(void *user,
	const pg_visible_change *change)
{
	unsigned int *count = user;

	if (change->kind == PG_CHANGE_ADD &&
	    (strcmp(change->canonical_name, "b") == 0 ||
	     strcmp(change->canonical_name, "c") == 0))
		(*count)++;
}

static void PG_CALL collect_loss(void *user,
	const pg_visible_change *change)
{
	unsigned int *count = user;

	if (change->kind == PG_CHANGE_LOSS &&
	    change->scope == NULL)
		(*count)++;
}

static void PG_CALL collect_reader_change(void *user,
	const pg_visible_change *change)
{
	unsigned int *count = user;

	if (change->kind == PG_CHANGE_UPDATE &&
	    strcmp(change->canonical_name, "a") == 0)
		(*count)++;
}

#ifndef _WIN32
struct watch_close_state {
	pg_reader **reader;
	unsigned int changes;
	int failed;
};

static void *close_watched_reader(void *user)
{
	struct watch_close_state *state = user;
	pg_error error;

	state->failed = pg_reader_close(state->reader, &error) != PG_OK;
	return NULL;
}

static void PG_CALL collect_reader_close(void *user,
	const pg_visible_change *change)
{
	struct watch_close_state *state = user;
	pthread_t worker;

	if (change->kind != PG_CHANGE_UPDATE ||
	    strcmp(change->canonical_name, "a") != 0)
		return;
	state->changes++;
	if (pthread_create(&worker, NULL, close_watched_reader, state) ||
	    pthread_join(worker, NULL))
		state->failed = 1;
}
#endif

static int test_watch_reader(void)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_tree *tree = NULL;
	pg_file *file = NULL;
	pg_reader *reader = NULL;
	pg_observer observer;
	pg_error error;
	unsigned int changes = 0;
#ifndef _WIN32
	struct watch_close_state close_state = { &reader, 0, 0 };
#endif

	CHECK(put_file("root/a", "abc", 3) == 0);
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_source_open(context, "root", NULL, &source,
		&error), PG_OK);
	STATUS(pg_tree_create(context, &tree, &error), PG_OK);
	STATUS(pg_tree_attach(tree, source, &error), PG_OK);
	STATUS(pg_tree_watch(tree, PG_WATCH_SCAN, &error), PG_OK);
	STATUS(pg_tree_find(tree, "a", &file, &error), PG_OK);
	STATUS(pg_reader_open(file, PG_READ_LOGICAL, &reader,
		&error), PG_OK);
	STATUS(pg_file_close(&file, &error), PG_OK);
	CHECK(put_file("root/a", "newer", 5) == 0);
	observer.user = &changes;
	observer.visible = collect_reader_change;
#ifndef _WIN32
	observer.user = &close_state;
	observer.visible = collect_reader_close;
#endif
	STATUS(pg_tree_poll(tree, &observer, &error), PG_OK);
#ifndef _WIN32
	changes = close_state.changes;
	CHECK(!close_state.failed);
#endif
	CHECK(changes == 1);
#ifdef _WIN32
	STATUS(pg_reader_close(&reader, &error), PG_OK);
#endif
	CHECK(put_file("root/a", "later!", 6) == 0);
	STATUS(pg_tree_poll(tree, &observer, &error), PG_OK);
	CHECK(changes == 1);
	STATUS(pg_tree_close(&tree, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static void PG_CALL collect_queued_change(void *user,
	const pg_visible_change *change)
{
	unsigned int *steps = user;

	if (strcmp(change->canonical_name, "b") != 0)
		return;
	if (change->kind == PG_CHANGE_ADD && *steps == 0)
		*steps = 1;
	else if (change->kind == PG_CHANGE_REMOVE && *steps == 1)
		*steps = 2;
}

static int test_watch_queue(void)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_tree *tree = NULL;
	pg_cursor *cursor = NULL;
	pg_file *file = NULL;
	pg_source_options options = { PG_LOOSE, PG_WRITE };
	pg_observer observer;
	pg_error error;
	unsigned int steps = 0;

	CHECK(put_file("root/a", "abc", 3) == 0);
	remove("root/b");
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_source_open(context, "root", &options,
		&source, &error), PG_OK);
	STATUS(pg_tree_create(context, &tree, &error), PG_OK);
	STATUS(pg_tree_attach(tree, source, &error), PG_OK);
	STATUS(pg_tree_request_subtree(tree, NULL, &error), PG_OK);
	STATUS(pg_tree_files(tree, NULL, &cursor, &error), PG_OK);
	STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	STATUS(pg_tree_watch(tree, PG_WATCH_SCAN, &error), PG_OK);
	STATUS(pg_source_write_all(source, "b", "new", 3,
		NULL, &error), PG_OK);
	STATUS(pg_source_find(source, "b", &file, &error), PG_OK);
	STATUS(pg_file_delete(file, &error), PG_OK);
	STATUS(pg_file_close(&file, &error), PG_OK);
	observer.user = &steps;
	observer.visible = collect_queued_change;
	STATUS(pg_tree_poll(tree, &observer, &error), PG_OK);
	CHECK(steps == 2);
	STATUS(pg_tree_close(&tree, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static int test_watch_loss(void)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_tree *tree = NULL;
	pg_observer observer;
	pg_error error;
	unsigned int losses = 0;

	remove("moved_root/a");
#ifdef _WIN32
	_rmdir("moved_root");
#else
	rmdir("moved_root");
#endif
	CHECK(put_file("root/a", "abc", 3) == 0);
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_source_open(context, "root", NULL, &source,
		&error), PG_OK);
	STATUS(pg_tree_create(context, &tree, &error), PG_OK);
	STATUS(pg_tree_attach(tree, source, &error), PG_OK);
	STATUS(pg_tree_watch(tree, PG_WATCH_NATIVE, &error), PG_OK);
#ifdef _WIN32
	CHECK(MoveFileExA("root", "moved_root",
		MOVEFILE_REPLACE_EXISTING) != 0);
#else
	CHECK(rename("root", "moved_root") == 0);
#endif
	observer.user = &losses;
	observer.visible = collect_loss;
	STATUS(pg_tree_poll(tree, &observer, &error), PG_OK);
	CHECK(losses == 1);
	STATUS(pg_tree_poll(tree, &observer, &error), PG_OK);
	CHECK(losses == 1);
	STATUS(pg_tree_unwatch(tree, &error), PG_OK);
	STATUS(pg_tree_detach(tree, source, &error), PG_OK);
	STATUS(pg_tree_close(&tree, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	CHECK(rename("moved_root", "root") == 0);
	return 0;
}

#ifdef __linux__
static int test_watch_retry(void)
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_source *source = NULL;
	pg_tree *tree = NULL;
	pg_cursor *cursor = NULL;
	pg_observer observer;
	pg_error error;
	unsigned int losses = 0;

	remove("retry.pigg");
	remove("moved.pigg");
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_archive_builder_create(context, "retry.pigg", PG_PIGG2,
		0, &builder, &error), PG_OK);
	STATUS(pg_archive_builder_write_all(builder, "a", "abc", 3,
		NULL, &error), PG_OK);
	STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	STATUS(pg_source_open(context, "retry.pigg", NULL, &source,
		&error), PG_OK);
	STATUS(pg_tree_create(context, &tree, &error), PG_OK);
	STATUS(pg_tree_attach(tree, source, &error), PG_OK);
	STATUS(pg_tree_files(tree, NULL, &cursor, &error), PG_OK);
	STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	STATUS(pg_tree_watch(tree, PG_WATCH_NATIVE, &error), PG_OK);
	CHECK(rename("retry.pigg", "moved.pigg") == 0);
	observer.user = &losses;
	observer.visible = collect_loss;
	STATUS(pg_tree_poll(tree, &observer, &error), PG_IO);
	CHECK(rename("moved.pigg", "retry.pigg") == 0);
	STATUS(pg_tree_poll(tree, &observer, &error), PG_OK);
	CHECK(losses == 1);
	STATUS(pg_tree_close(&tree, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}
#endif

static int test_tree_workflows(void)
{
	pg_context *context = NULL;
	pg_tree *tree = NULL;
	pg_source *source = NULL;
	pg_source *reference = NULL;
	pg_file *file = NULL;
	pg_cursor *cursor = NULL;
	pg_tree_info info;
	pg_buffer allocated = { NULL, 0 };
	pg_error error;
	pg_observer observer;
	char bytes[4] = { 0 };
	size_t count = 0;
	unsigned int changes = 0;

	CHECK(put_file("root/a", "abc", 3) == 0);
	remove("exported");
	remove("exported_step");
	remove("packed.pigg");
	remove("packed_step.pigg");
	remove("root/b");
	remove("root/c");
	remove("out/a");
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_source_open(context, "root", NULL, &source, &error),
		PG_OK);
	STATUS(pg_tree_open(context, NULL, 0, &tree, &error), PG_OK);
	STATUS(pg_tree_attach(tree, source, &error), PG_OK);
	STATUS(pg_tree_inspect(tree, &info, &error), PG_OK);
	CHECK(info.source_count == 1);
	STATUS(pg_tree_source(tree, 0, &reference, &error), PG_OK);
	CHECK(reference != NULL);
	STATUS(pg_source_close(&reference, &error), PG_OK);
	STATUS(pg_tree_find(tree, "A", &file, &error), PG_OK);
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_tree_read_all(tree, "a", bytes, sizeof(bytes),
		&count, &error), PG_OK);
	CHECK(count == 3 && memcmp(bytes, "abc", 3) == 0);
	STATUS(pg_tree_read_all_alloc(tree, "a", 3, &allocated,
		&error), PG_OK);
	CHECK(allocated.size == 3);
	pg_buffer_free(&allocated);
	STATUS(pg_tree_request_subtree(tree, NULL, &error), PG_OK);
	STATUS(pg_tree_files(tree, NULL, &cursor, &error), PG_OK);
	STATUS(pg_cursor_next(cursor, &file, &error), PG_OK);
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	STATUS(pg_tree_request_subtree(tree, NULL, &error), PG_OK);
	STATUS(pg_tree_rescan(tree, &error), PG_OK);
	STATUS(pg_tree_export(tree, "a", "exported", 0, &error), PG_OK);
	STATUS(pg_tree_export(tree, "a", "exported_step", 0, &error), PG_OK);
	CHECK(has_file("exported", "abc", 3) == 0);
	CHECK(has_file("exported_step", "abc", 3) == 0);
	STATUS(pg_tree_pack(tree, "packed.pigg", PG_PIGG2, NULL, &error), PG_OK);
	STATUS(pg_tree_pack(tree, "packed_step.pigg", PG_PIGG2,
		NULL, &error), PG_OK);
	STATUS(pg_tree_unpack(tree, "out", 0, &error), PG_OK);
	CHECK(has_file("out/a", "abc", 3) == 0);
	STATUS(pg_tree_unpack(tree, "out", PG_OVERWRITE, &error), PG_OK);
	STATUS(pg_tree_watch(tree, PG_WATCH_SCAN, &error), PG_OK);
	STATUS(pg_tree_request_subtree(tree, NULL, &error), PG_OK);
	CHECK(put_file("root/b", "new", 3) == 0);
	observer.user = &changes;
	observer.visible = collect_change;
	STATUS(pg_tree_poll(tree, &observer, &error), PG_OK);
	CHECK(changes == 1);
	STATUS(pg_tree_unwatch(tree, &error), PG_OK);
	STATUS(pg_tree_watch(tree, PG_WATCH_NATIVE, &error), PG_OK);
	CHECK(put_file("root/c", "new", 3) == 0);
	STATUS(pg_tree_poll(tree, &observer, &error), PG_OK);
	CHECK(changes == 2);
	STATUS(pg_tree_unwatch(tree, &error), PG_OK);
	STATUS(pg_tree_detach(tree, source, &error), PG_OK);
	STATUS(pg_tree_close(&tree, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	if (strcmp(argv[1], "readers_files") == 0)
		return test_readers_and_files();
	if (strcmp(argv[1], "writers_deletes") == 0)
		return test_writers_and_deletes();
	if (strcmp(argv[1], "writer_excess") == 0)
		return test_writer_excess();
	if (strcmp(argv[1], "builder_workflows") == 0)
		return test_builder_workflows();
	if (strcmp(argv[1], "source_workflows") == 0)
		return test_source_workflows();
	if (strcmp(argv[1], "source_delete") == 0)
		return test_source_delete();
	if (strcmp(argv[1], "source_delete_blocking") == 0)
		return test_source_delete_blocking();
	if (strcmp(argv[1], "tree_workflows") == 0)
		return test_tree_workflows();
	if (strcmp(argv[1], "watch_loss") == 0)
		return test_watch_loss();
#ifdef __linux__
	if (strcmp(argv[1], "watch_retry") == 0)
		return test_watch_retry();
#endif
	if (strcmp(argv[1], "watch_reader") == 0)
		return test_watch_reader();
	if (strcmp(argv[1], "watch_queue") == 0)
		return test_watch_queue();
	return 2;
}
