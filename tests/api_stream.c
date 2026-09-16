#include <piggle/piggle.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>

static char *test_mkdtemp(char *pattern)
{
	if (!_mktemp(pattern) || _mkdir(pattern))
		return NULL;
	return pattern;
}

#define mkdtemp test_mkdtemp
#define mkdir(path, mode) _mkdir(path)
#define lstat stat
#else
#include <unistd.h>
#endif

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, \
			#condition); \
		return 1; \
	} \
} while (0)
#define STATUS(call, expected) do { \
	pg_status actual = (call); \
	if (actual != (expected) || error.status != actual) { \
		fprintf(stderr, "%s:%d: %s returned %d, cause %d\n", \
			__FILE__, __LINE__, #call, (int)actual, \
			(int)error.cause); \
		return 1; \
	} \
} while (0)

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
	char found[32] = { 0 };
	FILE *stream = fopen(path, "rb");
	if (!stream)
		return 1;
	size_t size = strlen(bytes);
	int failed = fread(found, 1, size, stream) != size ||
		memcmp(found, bytes, size) != 0 ||
		fgetc(stream) != EOF;
	return fclose(stream) != 0 || failed;
}

static int absent(const char *path)
{
	struct stat native;
	return lstat(path, &native) != 0 && errno == ENOENT;
}

static pg_status transfer(pg_reader *reader, pg_writer *writer,
	pg_error *error)
{
	static const size_t choices[] = { 1, 7, 2, 3 };
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
			status = pg_writer_write(writer,
				buffer + i, 1, &written, error);
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

	CHECK(mkdir("root", 0700) == 0);
	CHECK(mkdir("out", 0700) == 0);
	CHECK(mkdir("abort", 0700) == 0);
	CHECK(put_file("root/alpha", "abcdefghi") == 0);
	CHECK(put_file("root/beta", "xyz") == 0);
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_source_open(context, "root", NULL,
		&source, &error), PG_OK);
	STATUS(pg_source_request_subtree(source, NULL, &error), PG_OK);
	STATUS(pg_source_files(source, NULL, &cursor, &error), PG_OK);
	STATUS(pg_archive_builder_create(context, "packed.pigg",
		PG_PIGG2, 0, &builder, &error), PG_OK);
	while (pg_cursor_next(cursor, &file, &error) == PG_OK) {
		STATUS(pg_file_inspect(file, &info, &error), PG_OK);
		STATUS(pg_reader_open(file, PG_READ_LOGICAL,
			&reader, &error), PG_OK);
		pg_write_options options;
		pg_write_options_init(&options, info.logical_size);
		STATUS(pg_writer_open_archive_builder(builder,
			info.canonical_name, &options, &writer,
			&error), PG_OK);
		CHECK(transfer(reader, writer, &error) == PG_OK);
		STATUS(pg_reader_close(&reader, &error), PG_OK);
		STATUS(pg_writer_finish(writer, &error), PG_OK);
		STATUS(pg_writer_close(&writer, &error), PG_OK);
		STATUS(pg_file_close(&file, &error), PG_OK);
	}
	CHECK(error.status == PG_END);
	STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_source_open(context, "packed.pigg", NULL,
		&archive, &error), PG_OK);
	STATUS(pg_source_files(archive, NULL, &cursor, &error), PG_OK);
	STATUS(pg_unpack_target_open(cursor, "abort", 0,
		&target, &error), PG_OK);
	STATUS(pg_cursor_next(cursor, &file, &error), PG_OK);
	STATUS(pg_writer_open_unpack(target, file,
		&writer, &error), PG_OK);
	size_t written = 0;
	STATUS(pg_writer_write(writer, "x", 1, &written,
		&error), PG_OK);
	CHECK(written == 1);
	STATUS(pg_writer_close(&writer, &error), PG_OK);
	CHECK(absent("abort/alpha"));
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_unpack_target_close(&target, &error), PG_OK);
	STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	STATUS(pg_source_files(archive, NULL, &cursor, &error), PG_OK);
	STATUS(pg_unpack_target_open(cursor, "out", 0,
		&target, &error), PG_OK);
	while (pg_cursor_next(cursor, &file, &error) == PG_OK) {
		STATUS(pg_reader_open(file, PG_READ_LOGICAL,
			&reader, &error), PG_OK);
		STATUS(pg_writer_open_unpack(target, file,
			&writer, &error), PG_OK);
		CHECK(transfer(reader, writer, &error) == PG_OK);
		STATUS(pg_reader_close(&reader, &error), PG_OK);
		STATUS(pg_writer_finish(writer, &error), PG_OK);
		STATUS(pg_writer_close(&writer, &error), PG_OK);
		STATUS(pg_file_close(&file, &error), PG_OK);
	}
	CHECK(error.status == PG_END);
	STATUS(pg_unpack_target_close(&target, &error), PG_OK);
	STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	CHECK(has_file("out/alpha", "abcdefghi") == 0);
	CHECK(has_file("out/beta", "xyz") == 0);
	STATUS(pg_source_close(&archive, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static uint32_t get32(const unsigned char bytes[4])
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
		((uint32_t)bytes[2] << 16) |
		((uint32_t)bytes[3] << 24);
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

	CHECK(mkdir("out", 0700) == 0);
	CHECK(mkdir("conflict", 0700) == 0);
	CHECK(mkdir("stable", 0700) == 0);
	CHECK(mkdir("stable2", 0700) == 0);
	CHECK(put_file("conflict/b", "existing") == 0);
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_archive_builder_create(context, "damaged.pigg",
		PG_PIGG2, 0, &builder, &error), PG_OK);
	pg_entry_options entry = { 0 };
	entry.compression = PG_COMPRESS_NEVER;
	STATUS(pg_archive_builder_write_all(builder, "a", "aaa", 3,
		&entry, &error), PG_OK);
	STATUS(pg_archive_builder_write_all(builder, "b", "bbb", 3,
		&entry, &error), PG_OK);
	STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	CHECK(corrupt_second_payload() == 0);
	STATUS(pg_source_open(context, "damaged.pigg", NULL,
		&source, &error), PG_OK);
	STATUS(pg_source_files(source, NULL, &cursor, &error), PG_OK);
	STATUS(pg_unpack_target_open(cursor, "conflict", 0,
		&target, &error), PG_EXISTS);
	CHECK(target == NULL && absent("conflict/a"));
	STATUS(pg_unpack_target_open(cursor, "stable", 0,
		&target, &error), PG_OK);
	CHECK(rename("stable", "moved") == 0);
	CHECK(mkdir("stable", 0700) == 0);
	STATUS(pg_cursor_next(cursor, &file, &error), PG_OK);
	STATUS(pg_writer_open_unpack(target, file,
		&writer, &error), PG_STALE);
	CHECK(writer == NULL);
	STATUS(pg_unpack_target_close(&target, &error), PG_OK);
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	STATUS(pg_source_files(source, NULL, &cursor, &error), PG_OK);
	STATUS(pg_unpack_target_open(cursor, "stable2", 0,
		&target, &error), PG_OK);
	STATUS(pg_cursor_next(cursor, &file, &error), PG_OK);
	STATUS(pg_writer_open_unpack(target, file,
		&writer, &error), PG_OK);
	size_t written = 0;
	STATUS(pg_writer_write(writer, "aaa", 3, &written,
		&error), PG_OK);
	CHECK(rename("stable2", "moved2") == 0);
	CHECK(mkdir("stable2", 0700) == 0);
	STATUS(pg_writer_finish(writer, &error), PG_STALE);
	STATUS(pg_writer_close(&writer, &error), PG_OK);
	CHECK(absent("stable2/a") && absent("moved2/a"));
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_unpack_target_close(&target, &error), PG_OK);
	STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	STATUS(pg_source_unpack(source, "out", 0, &error), PG_PARTIAL);
	CHECK(error.cause == PG_CHECKSUM);
	CHECK(has_file("out/a", "aaa") == 0);
	CHECK(absent("out/b"));
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	char directory[] = "run-XXXXXX";
	if (!mkdtemp(directory) || chdir(directory) != 0)
		return 2;
	if (strcmp(argv[1], "coroutines") == 0)
		return test_coroutines();
	if (strcmp(argv[1], "guards") == 0)
		return test_guards();
	return 2;
}
