#include "../support.hpp"
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

namespace {

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
		PG_CHECK(make_root());
	FILE *stream = fopen(path, "wb");

	PG_CHECK(stream != NULL);
	PG_CHECK(fwrite(bytes, 1, size, stream) == size);
	PG_CHECK(fclose(stream) == 0);
	return 0;
}

static int has_file(const char *path, const char *bytes, size_t size)
{
	FILE *stream = fopen(path, "rb");
	char actual[64];

	PG_CHECK(stream != NULL);
	PG_CHECK(size <= sizeof(actual));
	PG_CHECK(fread(actual, 1, size, stream) == size);
	PG_CHECK(fgetc(stream) == EOF);
	PG_CHECK(fclose(stream) == 0);
	PG_CHECK(memcmp(actual, bytes, size) == 0);
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
	pg_buffer allocated = {NULL, 0};
	pg_error error;
	char bytes[4] = {};
	size_t count = 0;

	PG_CHECK(put_file("root/a", "abc", 3) == 0);
	remove("exported");
	remove("exported_step");
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(
		pg_source_open(context, "root", NULL, &source, &error),
		PG_OK);
	PG_STATUS(pg_tree_create(context, &tree, &error), PG_OK);
	PG_STATUS(pg_tree_attach(tree, source, &error), PG_OK);
	PG_STATUS(pg_tree_find(tree, "A", &file, &error), PG_OK);
	PG_STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	PG_CHECK(strcmp(info.canonical_name, "a") == 0);
	PG_CHECK(info.logical_size == 3);

	PG_STATUS(
		pg_reader_open(file, PG_READ_LOGICAL, &reader, &error),
		PG_OK);
	PG_STATUS(
		pg_reader_inspect(reader, &reader_info, &error), PG_OK);
	PG_CHECK(reader_info.size == 3);
	PG_STATUS(pg_reader_read(reader, bytes, 2, &count, &error),
		PG_OK);
	PG_CHECK(count == 2 && memcmp(bytes, "ab", 2) == 0);
	PG_STATUS(pg_reader_close(&reader, &error), PG_OK);
	PG_STATUS(pg_reader_open_source(source, "a", PG_READ_LOGICAL,
				&reader, &error),
		PG_OK);
	PG_STATUS(pg_reader_read(reader, bytes, 3, &count, &error),
		PG_OK);
	PG_CHECK(count == 3 && memcmp(bytes, "abc", 3) == 0);
	PG_STATUS(pg_reader_close(&reader, &error), PG_OK);
	PG_STATUS(pg_reader_open_tree(
				tree, "a", PG_READ_LOGICAL, &reader, &error),
		PG_OK);
	PG_STATUS(pg_reader_read(reader, bytes, 3, &count, &error),
		PG_OK);
	PG_CHECK(count == 3 && memcmp(bytes, "abc", 3) == 0);
	PG_STATUS(pg_reader_close(&reader, &error), PG_OK);
	PG_STATUS(pg_reader_open_native(
				context, "root/a", &reader, &error),
		PG_OK);
	PG_STATUS(pg_reader_close(&reader, &error), PG_OK);

	PG_STATUS(pg_file_read_all(file, bytes, 2, &count, &error),
		PG_CAPACITY);
	PG_CHECK(count == 0);
	PG_STATUS(pg_file_read_all(
				file, bytes, sizeof(bytes), &count, &error),
		PG_OK);
	PG_CHECK(count == 3 && memcmp(bytes, "abc", 3) == 0);
	PG_STATUS(pg_file_read_all_alloc(file, 2, &allocated, &error),
		PG_LIMIT);
	PG_CHECK(allocated.data == NULL && allocated.size == 0);
	PG_STATUS(pg_file_read_all_alloc(file, 3, &allocated, &error),
		PG_OK);
	PG_CHECK(allocated.size == 3);
	PG_CHECK(memcmp(allocated.data, "abc", 3) == 0);
	pg_buffer_free(&allocated);
	PG_STATUS(pg_file_verify(file, &error), PG_NO_CHECKSUM);
	PG_STATUS(pg_file_verify(file, &error), PG_NO_CHECKSUM);

	PG_STATUS(pg_file_export(file, "exported", 0, &error), PG_OK);

	PG_CHECK(has_file("exported", "abc", 3) == 0);
	PG_STATUS(pg_file_export(file, "exported_step", 0, &error),
		PG_OK);

	PG_CHECK(has_file("exported_step", "abc", 3) == 0);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_tree_detach(tree, source, &error), PG_OK);
	PG_STATUS(pg_tree_close(&tree, &error), PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static int test_writers_and_deletes(void)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_file *file = NULL;
	pg_writer *writer = NULL;
	pg_source_options source_options = {PG_LOOSE, PG_WRITE, 0};
	pg_write_options options;
	pg_error error;
	size_t count = 0;

	PG_CHECK(put_file("root/a", "old", 3) == 0);
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(pg_source_open(context, "root", &source_options,
				&source, &error),
		PG_OK);
	pg_write_options_init(&options, 3);
	PG_STATUS(pg_writer_open_source(
				source, "b", &options, &writer, &error),
		PG_OK);
	PG_STATUS(pg_writer_write(writer, "new", 3, &count, &error),
		PG_OK);
	PG_CHECK(count == 3);
	PG_STATUS(pg_writer_finish(writer, &error), PG_OK);

	PG_STATUS(pg_writer_close(&writer, &error), PG_OK);
	PG_CHECK(has_file("root/b", "new", 3) == 0);
	PG_STATUS(pg_source_find(source, "a", &file, &error), PG_OK);
	PG_STATUS(pg_writer_open_file(file, &options, &writer, &error),
		PG_OK);
	PG_STATUS(pg_writer_write(writer, "one", 3, &count, &error),
		PG_OK);
	PG_STATUS(pg_writer_finish(writer, &error), PG_OK);

	PG_STATUS(pg_writer_close(&writer, &error), PG_OK);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_CHECK(has_file("root/a", "one", 3) == 0);
	PG_STATUS(pg_source_find(source, "a", &file, &error), PG_OK);
	PG_STATUS(
		pg_file_write_all(file, "two", 3, NULL, &error), PG_OK);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_CHECK(has_file("root/a", "two", 3) == 0);
	PG_STATUS(pg_source_find(source, "a", &file, &error), PG_OK);
	PG_STATUS(pg_file_delete(file, &error), PG_OK);

	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_source_find(source, "b", &file, &error), PG_OK);
	PG_STATUS(pg_file_delete(file, &error), PG_OK);

	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
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
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	pg_write_options_init(&options, 3);
	PG_STATUS(pg_writer_open_native(context, "excess.bin", &options,
				0, &writer, &error),
		PG_OK);
	PG_STATUS(pg_writer_write(writer, "abcd", 4, &count, &error),
		PG_INVALID);
	PG_CHECK(count == 0);
	PG_STATUS(pg_writer_close(&writer, &error), PG_OK);
	stream = fopen("excess.bin", "rb");
	PG_CHECK(stream == NULL);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
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

	PG_CHECK(put_file("root/a", "abc", 3) == 0);
	PG_CHECK(put_file("native", "def", 3) == 0);
	remove("built.pigg");
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(
		pg_source_open(context, "root", NULL, &source, &error),
		PG_OK);
	PG_STATUS(pg_source_find(source, "a", &file, &error), PG_OK);
	PG_STATUS(pg_archive_builder_create(context, "built.pigg",
				PG_PIGG2, 0, &builder, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_write_all(
				builder, "zero", NULL, 0, NULL, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_import(builder, "imported",
				"native", PG_COMPRESS_NEVER, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_import(builder, "imported_step",
				"native", PG_COMPRESS_NEVER, &error),
		PG_OK);

	PG_STATUS(pg_archive_builder_copy(builder, "copied", file,
				PG_COMPRESS_NEVER, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_copy(builder, "copied_step", file,
				PG_COMPRESS_NEVER, &error),
		PG_OK);
	pg_write_options_init(&options, 3);
	PG_STATUS(pg_writer_open_archive_builder(builder, "streamed",
				&options, &writer, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_finish(builder, &error), PG_BUSY);
	PG_STATUS(pg_archive_builder_close(&builder, &error), PG_BUSY);
	PG_STATUS(pg_writer_write(writer, "ghi", 3, &count, &error),
		PG_OK);
	PG_CHECK(count == 3);
	PG_STATUS(pg_writer_finish(writer, &error), PG_OK);

	PG_STATUS(pg_writer_close(&writer, &error), PG_OK);
	PG_STATUS(pg_archive_builder_finish(builder, &error), PG_OK);

	PG_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static int test_source_workflows(void)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_file *file = NULL;
	pg_cursor *cursor = NULL;
	pg_source_options options = {PG_LOOSE, PG_WRITE, 0};
	pg_source_info info;
	pg_buffer allocated = {NULL, 0};
	pg_error error;
	char bytes[4] = {};
	size_t count = 0;

	PG_CHECK(put_file("root/a", "abc", 3) == 0);
	PG_CHECK(put_file("native", "def", 3) == 0);
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
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(pg_source_open(
				context, "root", &options, &source, &error),
		PG_OK);
	PG_STATUS(pg_source_inspect(source, &info, &error), PG_OK);
	PG_CHECK(info.id != 0 && info.access == PG_WRITE);
	PG_STATUS(pg_source_read_all(source, "a", bytes, sizeof(bytes),
				&count, &error),
		PG_OK);
	PG_CHECK(count == 3 && memcmp(bytes, "abc", 3) == 0);
	PG_STATUS(pg_source_read_all_alloc(
				source, "a", 3, &allocated, &error),
		PG_OK);
	PG_CHECK(allocated.size == 3);
	pg_buffer_free(&allocated);
	PG_STATUS(pg_source_write_all(
				source, "b", "bbb", 3, NULL, &error),
		PG_OK);

	PG_STATUS(pg_source_import(source, "c", "native",
				PG_COMPRESS_NEVER, &error),
		PG_OK);
	PG_STATUS(pg_source_import(source, "d", "native",
				PG_COMPRESS_NEVER, &error),
		PG_OK);
	PG_STATUS(pg_source_find(source, "a", &file, &error), PG_OK);
	PG_STATUS(pg_source_copy(
				source, "e", file, PG_COMPRESS_NEVER, &error),
		PG_OK);
	PG_STATUS(pg_source_copy(
				source, "f", file, PG_COMPRESS_NEVER, &error),
		PG_OK);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_source_export(source, "a", "exported", 0, &error),
		PG_OK);
	PG_CHECK(has_file("exported", "abc", 3) == 0);
	PG_STATUS(pg_source_export(
				source, "a", "exported_step", 0, &error),
		PG_OK);
	PG_CHECK(has_file("exported_step", "abc", 3) == 0);
	PG_STATUS(
		pg_source_request_subtree(source, NULL, &error), PG_OK);
	PG_STATUS(
		pg_source_files(source, NULL, &cursor, &error), PG_OK);
	PG_STATUS(pg_cursor_next(cursor, &file, &error), PG_OK);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	PG_STATUS(pg_source_rescan(source, &error), PG_OK);
	PG_STATUS(pg_source_validate(source, &error), PG_OK);
	PG_STATUS(pg_source_validate(source, &error), PG_OK);
	PG_STATUS(pg_source_pack(source, "packed.pigg", PG_PIGG2, NULL,
				&error),
		PG_OK);

	PG_STATUS(pg_source_pack(source, "packed_step.pigg", PG_PIGG2,
				NULL, &error),
		PG_OK);

	PG_STATUS(pg_source_unpack(source, "out", 0, &error), PG_OK);
	PG_CHECK(has_file("out/a", "abc", 3) == 0);
	PG_STATUS(pg_source_unpack(source, "out", PG_OVERWRITE, &error),
		PG_OK);
	PG_STATUS(pg_source_recover(context, "missing.hogg", &error),
		PG_NOT_FOUND);
	PG_STATUS(pg_source_recover(context, "missing.hogg", &error),
		PG_NOT_FOUND);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static int test_source_delete(void)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_source_options options = {PG_LOOSE, PG_WRITE, 0};
	pg_error error;

	PG_CHECK(put_file("root/a", "abc", 3) == 0);
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(pg_source_open(
				context, "root", &options, &source, &error),
		PG_OK);
	PG_STATUS(pg_source_delete(source, &error), PG_OK);

	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static int test_source_delete_blocking(void)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_source_options options = {PG_LOOSE, PG_WRITE, 0};
	pg_error error;

	PG_CHECK(put_file("root/a", "abc", 3) == 0);
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(pg_source_open(
				context, "root", &options, &source, &error),
		PG_OK);
	PG_STATUS(pg_source_delete(source, &error), PG_OK);

	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static void PG_CALL collect_change(
	void *user, const pg_visible_change *change)
{
	unsigned int *count = static_cast<unsigned int *>(user);

	if (change->kind == PG_CHANGE_ADD &&
		(strcmp(change->canonical_name, "b") == 0 ||
			strcmp(change->canonical_name, "c") == 0))
		(*count)++;
}

static void PG_CALL collect_loss(
	void *user, const pg_visible_change *change)
{
	unsigned int *count = static_cast<unsigned int *>(user);

	if (change->kind == PG_CHANGE_LOSS && change->scope == NULL)
		(*count)++;
}

static void PG_CALL collect_reader_change(
	void *user, const pg_visible_change *change)
{
	unsigned int *count = static_cast<unsigned int *>(user);

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
	struct watch_close_state *state =
		static_cast<struct watch_close_state *>(user);
	pg_error error;

	state->failed = pg_reader_close(state->reader, &error) != PG_OK;
	return NULL;
}

static void PG_CALL collect_reader_close(
	void *user, const pg_visible_change *change)
{
	struct watch_close_state *state =
		static_cast<struct watch_close_state *>(user);
	pthread_t worker;

	if (change->kind != PG_CHANGE_UPDATE ||
		strcmp(change->canonical_name, "a") != 0)
		return;
	state->changes++;
	if (pthread_create(
			&worker, NULL, close_watched_reader, state) ||
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
	struct watch_close_state close_state = {&reader, 0, 0};
#endif

	PG_CHECK(put_file("root/a", "abc", 3) == 0);
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(
		pg_source_open(context, "root", NULL, &source, &error),
		PG_OK);
	PG_STATUS(pg_tree_create(context, &tree, &error), PG_OK);
	PG_STATUS(pg_tree_attach(tree, source, &error), PG_OK);
	PG_STATUS(pg_tree_watch(tree, PG_WATCH_SCAN, &error), PG_OK);
	PG_STATUS(pg_tree_find(tree, "a", &file, &error), PG_OK);
	PG_STATUS(
		pg_reader_open(file, PG_READ_LOGICAL, &reader, &error),
		PG_OK);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_CHECK(put_file("root/a", "newer", 5) == 0);
	observer.user = &changes;
	observer.visible = collect_reader_change;
#ifndef _WIN32
	observer.user = &close_state;
	observer.visible = collect_reader_close;
#endif
	PG_STATUS(pg_tree_poll(tree, &observer, &error), PG_OK);
#ifndef _WIN32
	changes = close_state.changes;
	PG_CHECK(!close_state.failed);
#endif
	PG_CHECK(changes == 1);
#ifdef _WIN32
	PG_STATUS(pg_reader_close(&reader, &error), PG_OK);
#endif
	PG_CHECK(put_file("root/a", "later!", 6) == 0);
	PG_STATUS(pg_tree_poll(tree, &observer, &error), PG_OK);
	PG_CHECK(changes == 1);
	PG_STATUS(pg_tree_close(&tree, &error), PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static void PG_CALL collect_queued_change(
	void *user, const pg_visible_change *change)
{
	unsigned int *steps = static_cast<unsigned int *>(user);

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
	pg_source_options options = {PG_LOOSE, PG_WRITE, 0};
	pg_observer observer;
	pg_error error;
	unsigned int steps = 0;

	PG_CHECK(put_file("root/a", "abc", 3) == 0);
	remove("root/b");
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(pg_source_open(
				context, "root", &options, &source, &error),
		PG_OK);
	PG_STATUS(pg_tree_create(context, &tree, &error), PG_OK);
	PG_STATUS(pg_tree_attach(tree, source, &error), PG_OK);
	PG_STATUS(pg_tree_request_subtree(tree, NULL, &error), PG_OK);
	PG_STATUS(pg_tree_files(tree, NULL, &cursor, &error), PG_OK);
	PG_STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	PG_STATUS(pg_tree_watch(tree, PG_WATCH_SCAN, &error), PG_OK);
	PG_STATUS(pg_source_write_all(
				source, "b", "new", 3, NULL, &error),
		PG_OK);
	PG_STATUS(pg_source_find(source, "b", &file, &error), PG_OK);
	PG_STATUS(pg_file_delete(file, &error), PG_OK);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	observer.user = &steps;
	observer.visible = collect_queued_change;
	PG_STATUS(pg_tree_poll(tree, &observer, &error), PG_OK);
	PG_CHECK(steps == 2);
	PG_STATUS(pg_tree_close(&tree, &error), PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
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
	PG_CHECK(put_file("root/a", "abc", 3) == 0);
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(
		pg_source_open(context, "root", NULL, &source, &error),
		PG_OK);
	PG_STATUS(pg_tree_create(context, &tree, &error), PG_OK);
	PG_STATUS(pg_tree_attach(tree, source, &error), PG_OK);
	PG_STATUS(pg_tree_watch(tree, PG_WATCH_NATIVE, &error), PG_OK);
#ifdef _WIN32
	PG_CHECK(MoveFileExA("root", "moved_root",
				MOVEFILE_REPLACE_EXISTING) != 0);
#else
	PG_CHECK(rename("root", "moved_root") == 0);
#endif
	observer.user = &losses;
	observer.visible = collect_loss;
	PG_STATUS(pg_tree_poll(tree, &observer, &error), PG_OK);
	PG_CHECK(losses == 1);
	PG_STATUS(pg_tree_poll(tree, &observer, &error), PG_OK);
	PG_CHECK(losses == 1);
	PG_STATUS(pg_tree_unwatch(tree, &error), PG_OK);
	PG_STATUS(pg_tree_detach(tree, source, &error), PG_OK);
	PG_STATUS(pg_tree_close(&tree, &error), PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	PG_CHECK(rename("moved_root", "root") == 0);
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
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(pg_archive_builder_create(context, "retry.pigg",
				PG_PIGG2, 0, &builder, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_write_all(
				builder, "a", "abc", 3, NULL, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	PG_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	PG_STATUS(pg_source_open(
				context, "retry.pigg", NULL, &source, &error),
		PG_OK);
	PG_STATUS(pg_tree_create(context, &tree, &error), PG_OK);
	PG_STATUS(pg_tree_attach(tree, source, &error), PG_OK);
	PG_STATUS(pg_tree_files(tree, NULL, &cursor, &error), PG_OK);
	PG_STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	PG_STATUS(pg_tree_watch(tree, PG_WATCH_NATIVE, &error), PG_OK);
	PG_CHECK(rename("retry.pigg", "moved.pigg") == 0);
	observer.user = &losses;
	observer.visible = collect_loss;
	PG_STATUS(pg_tree_poll(tree, &observer, &error), PG_IO);
	PG_CHECK(rename("moved.pigg", "retry.pigg") == 0);
	PG_STATUS(pg_tree_poll(tree, &observer, &error), PG_OK);
	PG_CHECK(losses == 1);
	PG_STATUS(pg_tree_close(&tree, &error), PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
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
	pg_buffer allocated = {NULL, 0};
	pg_error error;
	pg_observer observer;
	char bytes[4] = {};
	size_t count = 0;
	unsigned int changes = 0;

	PG_CHECK(put_file("root/a", "abc", 3) == 0);
	remove("exported");
	remove("exported_step");
	remove("packed.pigg");
	remove("packed_step.pigg");
	remove("root/b");
	remove("root/c");
	remove("out/a");
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(
		pg_source_open(context, "root", NULL, &source, &error),
		PG_OK);
	PG_STATUS(pg_tree_open(context, NULL, 0, &tree, &error), PG_OK);
	PG_STATUS(pg_tree_attach(tree, source, &error), PG_OK);
	PG_STATUS(pg_tree_inspect(tree, &info, &error), PG_OK);
	PG_CHECK(info.source_count == 1);
	PG_STATUS(pg_tree_source(tree, 0, &reference, &error), PG_OK);
	PG_CHECK(reference != NULL);
	PG_STATUS(pg_source_close(&reference, &error), PG_OK);
	PG_STATUS(pg_tree_find(tree, "A", &file, &error), PG_OK);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_tree_read_all(tree, "a", bytes, sizeof(bytes),
				&count, &error),
		PG_OK);
	PG_CHECK(count == 3 && memcmp(bytes, "abc", 3) == 0);
	PG_STATUS(pg_tree_read_all_alloc(
				tree, "a", 3, &allocated, &error),
		PG_OK);
	PG_CHECK(allocated.size == 3);
	pg_buffer_free(&allocated);
	PG_STATUS(pg_tree_request_subtree(tree, NULL, &error), PG_OK);
	PG_STATUS(pg_tree_files(tree, NULL, &cursor, &error), PG_OK);
	PG_STATUS(pg_cursor_next(cursor, &file, &error), PG_OK);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	PG_STATUS(pg_tree_request_subtree(tree, NULL, &error), PG_OK);
	PG_STATUS(pg_tree_rescan(tree, &error), PG_OK);
	PG_STATUS(pg_tree_export(tree, "a", "exported", 0, &error),
		PG_OK);
	PG_STATUS(pg_tree_export(tree, "a", "exported_step", 0, &error),
		PG_OK);
	PG_CHECK(has_file("exported", "abc", 3) == 0);
	PG_CHECK(has_file("exported_step", "abc", 3) == 0);
	PG_STATUS(pg_tree_pack(
				tree, "packed.pigg", PG_PIGG2, NULL, &error),
		PG_OK);
	PG_STATUS(pg_tree_pack(tree, "packed_step.pigg", PG_PIGG2, NULL,
				&error),
		PG_OK);
	PG_STATUS(pg_tree_unpack(tree, "out", 0, &error), PG_OK);
	PG_CHECK(has_file("out/a", "abc", 3) == 0);
	PG_STATUS(pg_tree_unpack(tree, "out", PG_OVERWRITE, &error),
		PG_OK);
	PG_STATUS(pg_tree_watch(tree, PG_WATCH_SCAN, &error), PG_OK);
	PG_STATUS(pg_tree_request_subtree(tree, NULL, &error), PG_OK);
	PG_CHECK(put_file("root/b", "new", 3) == 0);
	observer.user = &changes;
	observer.visible = collect_change;
	PG_STATUS(pg_tree_poll(tree, &observer, &error), PG_OK);
	PG_CHECK(changes == 1);
	PG_STATUS(pg_tree_unwatch(tree, &error), PG_OK);
	PG_STATUS(pg_tree_watch(tree, PG_WATCH_NATIVE, &error), PG_OK);
	PG_CHECK(put_file("root/c", "new", 3) == 0);
	PG_STATUS(pg_tree_poll(tree, &observer, &error), PG_OK);
	PG_CHECK(changes == 2);
	PG_STATUS(pg_tree_unwatch(tree, &error), PG_OK);
	PG_STATUS(pg_tree_detach(tree, source, &error), PG_OK);
	PG_STATUS(pg_tree_close(&tree, &error), PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

using ApiFlows = piggle_test::ContractTest;

TEST_F(ApiFlows, api_readers_files)
{
	std::filesystem::create_directories("root");
	std::filesystem::create_directories("out");
	ASSERT_EQ(0, test_readers_and_files());
}

TEST_F(ApiFlows, api_writers_deletes)
{
	std::filesystem::create_directories("root");
	std::filesystem::create_directories("out");
	ASSERT_EQ(0, test_writers_and_deletes());
}

TEST_F(ApiFlows, api_writer_excess)
{
	std::filesystem::create_directories("root");
	std::filesystem::create_directories("out");
	ASSERT_EQ(0, test_writer_excess());
}

TEST_F(ApiFlows, api_builder_workflows)
{
	std::filesystem::create_directories("root");
	std::filesystem::create_directories("out");
	ASSERT_EQ(0, test_builder_workflows());
}

TEST_F(ApiFlows, api_source_workflows)
{
	std::filesystem::create_directories("root");
	std::filesystem::create_directories("out");
	ASSERT_EQ(0, test_source_workflows());
}

TEST_F(ApiFlows, api_source_delete)
{
	std::filesystem::create_directories("root");
	std::filesystem::create_directories("out");
	ASSERT_EQ(0, test_source_delete());
}

TEST_F(ApiFlows, api_source_delete_blocking)
{
	std::filesystem::create_directories("root");
	std::filesystem::create_directories("out");
	ASSERT_EQ(0, test_source_delete_blocking());
}

TEST_F(ApiFlows, api_tree_workflows)
{
	std::filesystem::create_directories("root");
	std::filesystem::create_directories("out");
	ASSERT_EQ(0, test_tree_workflows());
}

TEST_F(ApiFlows, api_watch_loss)
{
	std::filesystem::create_directories("root");
	std::filesystem::create_directories("out");
	ASSERT_EQ(0, test_watch_loss());
}

TEST_F(ApiFlows, api_watch_reader)
{
	std::filesystem::create_directories("root");
	std::filesystem::create_directories("out");
	ASSERT_EQ(0, test_watch_reader());
}

TEST_F(ApiFlows, api_watch_queue)
{
	std::filesystem::create_directories("root");
	std::filesystem::create_directories("out");
	ASSERT_EQ(0, test_watch_queue());
}

#ifdef __linux__
TEST_F(ApiFlows, api_watch_retry)
{
	std::filesystem::create_directories("root");
	std::filesystem::create_directories("out");
	ASSERT_EQ(0, test_watch_retry());
}
#endif

} // namespace
