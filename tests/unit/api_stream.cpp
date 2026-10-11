#include "../support.hpp"
#include <piggle/piggle.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

namespace {

#ifdef _WIN32
#define mkdir(path, mode) _mkdir(path)
#define lstat stat
#endif

static int put_file(const char *path, const char *bytes)
{
	FILE *stream = fopen(path, "wb");
	if (!stream)
		return 1;
	size_t size = strlen(bytes);
	int failed = fwrite(bytes, 1, size, stream) != size;
	return fclose(stream) != 0 || failed;
}

static int has_file(const char *path, const char *bytes)
{
	char found[32] = {};
	FILE *stream = fopen(path, "rb");
	if (!stream)
		return 1;
	size_t size = strlen(bytes);
	int failed = fread(found, 1, size, stream) != size ||
		memcmp(found, bytes, size) != 0 || fgetc(stream) != EOF;
	return fclose(stream) != 0 || failed;
}

static int absent(const char *path)
{
	struct stat native;
	return lstat(path, &native) != 0 && errno == ENOENT;
}

static pg_status transfer(
	pg_reader *reader, pg_writer *writer, pg_error *error)
{
	static const size_t choices[] = {1, 7, 2, 3};
	char buffer[8];
	for (size_t step = 0;; step++) {
		size_t count = 0;
		pg_status status = pg_reader_read(reader, buffer,
			choices[step % 4], &count, error);
		if (status == PG_END)
			return PG_OK;
		if (status != PG_OK)
			return status;
		for (size_t i = 0; i < count; i++) {
			size_t written = 0;
			status = pg_writer_write(
				writer, buffer + i, 1, &written, error);
			if (status != PG_OK || written != 1)
				return status == PG_OK ? PG_IO : status;
		}
	}
}

static int test_coroutines(void)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_source *archive = NULL;
	pg_archive_builder *builder = NULL;
	pg_cursor *cursor = NULL;
	pg_file *file = NULL;
	pg_reader *reader = NULL;
	pg_writer *writer = NULL;
	pg_unpack_target *target = NULL;
	pg_error error;
	pg_file_info info;

	PG_CHECK(mkdir("root", 0700) == 0);
	PG_CHECK(mkdir("out", 0700) == 0);
	PG_CHECK(mkdir("abort", 0700) == 0);
	PG_CHECK(put_file("root/alpha", "abcdefghi") == 0);
	PG_CHECK(put_file("root/beta", "xyz") == 0);
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(
		pg_source_open(context, "root", NULL, &source, &error),
		PG_OK);
	PG_STATUS(
		pg_source_request_subtree(source, NULL, &error), PG_OK);
	PG_STATUS(
		pg_source_files(source, NULL, &cursor, &error), PG_OK);
	PG_STATUS(pg_archive_builder_create(context, "packed.pigg",
				PG_PIGG2, 0, &builder, &error),
		PG_OK);
	while (pg_cursor_next(cursor, &file, &error) == PG_OK) {
		PG_STATUS(pg_file_inspect(file, &info, &error), PG_OK);
		PG_STATUS(pg_reader_open(file, PG_READ_LOGICAL, &reader,
					&error),
			PG_OK);
		pg_write_options options;
		pg_write_options_init(&options, info.logical_size);
		PG_STATUS(pg_writer_open_archive_builder(builder,
					info.canonical_name, &options,
					&writer, &error),
			PG_OK);
		PG_CHECK(transfer(reader, writer, &error) == PG_OK);
		PG_STATUS(pg_reader_close(&reader, &error), PG_OK);
		PG_STATUS(pg_writer_finish(writer, &error), PG_OK);
		PG_STATUS(pg_writer_close(&writer, &error), PG_OK);
		PG_STATUS(pg_file_close(&file, &error), PG_OK);
	}
	PG_CHECK(error.status == PG_END);
	PG_STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	PG_STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	PG_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_source_open(context, "packed.pigg", NULL, &archive,
				&error),
		PG_OK);
	PG_STATUS(
		pg_source_files(archive, NULL, &cursor, &error), PG_OK);
	PG_STATUS(pg_unpack_target_open(
				cursor, "abort", 0, &target, &error),
		PG_OK);
	PG_STATUS(pg_cursor_next(cursor, &file, &error), PG_OK);
	PG_STATUS(pg_writer_open_unpack(target, file, &writer, &error),
		PG_OK);
	size_t written = 0;
	PG_STATUS(pg_writer_write(writer, "x", 1, &written, &error),
		PG_OK);
	PG_CHECK(written == 1);
	PG_STATUS(pg_writer_close(&writer, &error), PG_OK);
	PG_CHECK(absent("abort/alpha"));
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_unpack_target_close(&target, &error), PG_OK);
	PG_STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	PG_STATUS(
		pg_source_files(archive, NULL, &cursor, &error), PG_OK);
	PG_STATUS(pg_unpack_target_open(
				cursor, "out", 0, &target, &error),
		PG_OK);
	while (pg_cursor_next(cursor, &file, &error) == PG_OK) {
		PG_STATUS(pg_reader_open(file, PG_READ_LOGICAL, &reader,
					&error),
			PG_OK);
		PG_STATUS(pg_writer_open_unpack(
					target, file, &writer, &error),
			PG_OK);
		PG_CHECK(transfer(reader, writer, &error) == PG_OK);
		PG_STATUS(pg_reader_close(&reader, &error), PG_OK);
		PG_STATUS(pg_writer_finish(writer, &error), PG_OK);
		PG_STATUS(pg_writer_close(&writer, &error), PG_OK);
		PG_STATUS(pg_file_close(&file, &error), PG_OK);
	}
	PG_CHECK(error.status == PG_END);
	PG_STATUS(pg_unpack_target_close(&target, &error), PG_OK);
	PG_STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	PG_CHECK(has_file("out/alpha", "abcdefghi") == 0);
	PG_CHECK(has_file("out/beta", "xyz") == 0);
	PG_STATUS(pg_source_close(&archive, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static uint32_t get32(const unsigned char bytes[4])
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
		((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static int corrupt_second_payload(void)
{
	FILE *stream = fopen("damaged.pigg", "r+b");
	if (!stream)
		return 1;
	unsigned char offset[4];
	int failed = fseek(stream, 80, SEEK_SET) != 0 ||
		fread(offset, 1, 4, stream) != 4 ||
		fseek(stream, (long)get32(offset), SEEK_SET) != 0 ||
		fputc('!', stream) == EOF;
	return fclose(stream) != 0 || failed;
}

static int test_guards(void)
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_source *source = NULL;
	pg_cursor *cursor = NULL;
	pg_file *file = NULL;
	pg_unpack_target *target = NULL;
	pg_writer *writer = NULL;
	pg_error error;

	PG_CHECK(mkdir("out", 0700) == 0);
	PG_CHECK(mkdir("conflict", 0700) == 0);
	PG_CHECK(mkdir("stable", 0700) == 0);
	PG_CHECK(mkdir("stable2", 0700) == 0);
	PG_CHECK(put_file("conflict/b", "existing") == 0);
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(pg_archive_builder_create(context, "damaged.pigg",
				PG_PIGG2, 0, &builder, &error),
		PG_OK);
	pg_entry_options entry = {};
	entry.compression = PG_COMPRESS_NEVER;
	PG_STATUS(pg_archive_builder_write_all(
				builder, "a", "aaa", 3, &entry, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_write_all(
				builder, "b", "bbb", 3, &entry, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	PG_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	PG_CHECK(corrupt_second_payload() == 0);
	PG_STATUS(pg_source_open(context, "damaged.pigg", NULL, &source,
				&error),
		PG_OK);
	PG_STATUS(
		pg_source_files(source, NULL, &cursor, &error), PG_OK);
	PG_STATUS(pg_unpack_target_open(
				cursor, "conflict", 0, &target, &error),
		PG_EXISTS);
	PG_CHECK(target == NULL && absent("conflict/a"));
	PG_STATUS(pg_unpack_target_open(
				cursor, "stable", 0, &target, &error),
		PG_OK);
	PG_CHECK(rename("stable", "moved") == 0);
	PG_CHECK(mkdir("stable", 0700) == 0);
	PG_STATUS(pg_cursor_next(cursor, &file, &error), PG_OK);
	PG_STATUS(pg_writer_open_unpack(target, file, &writer, &error),
		PG_STALE);
	PG_CHECK(writer == NULL);
	PG_STATUS(pg_unpack_target_close(&target, &error), PG_OK);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	PG_STATUS(
		pg_source_files(source, NULL, &cursor, &error), PG_OK);
	PG_STATUS(pg_unpack_target_open(
				cursor, "stable2", 0, &target, &error),
		PG_OK);
	PG_STATUS(pg_cursor_next(cursor, &file, &error), PG_OK);
	PG_STATUS(pg_writer_open_unpack(target, file, &writer, &error),
		PG_OK);
	size_t written = 0;
	PG_STATUS(pg_writer_write(writer, "aaa", 3, &written, &error),
		PG_OK);
	PG_CHECK(rename("stable2", "moved2") == 0);
	PG_CHECK(mkdir("stable2", 0700) == 0);
	PG_STATUS(pg_writer_finish(writer, &error), PG_STALE);
	PG_STATUS(pg_writer_close(&writer, &error), PG_OK);
	PG_CHECK(absent("stable2/a") && absent("moved2/a"));
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_unpack_target_close(&target, &error), PG_OK);
	PG_STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	PG_STATUS(
		pg_source_unpack(source, "out", 0, &error), PG_PARTIAL);
	PG_CHECK(error.cause == PG_CHECKSUM);
	PG_CHECK(has_file("out/a", "aaa") == 0);
	PG_CHECK(absent("out/b"));
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

using ApiStream = piggle_test::ContractTest;

TEST_F(ApiStream, api_stream_coroutines)
{
	ASSERT_EQ(0, test_coroutines());
}

TEST_F(ApiStream, api_stream_guards)
{
	ASSERT_EQ(0, test_guards());
}

} // namespace
