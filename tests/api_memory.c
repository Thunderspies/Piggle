#include <piggle/piggle.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#define SIZE (24u * 1024u * 1024u)
#define CHUNK 65536u
#define CHECK(call) do { \
	pg_status status = (call); \
	if (status != PG_OK) { \
		fprintf(stderr, "%s: %d (cause %d)\n", #call, \
			(int)status, (int)error.cause); \
		return 1; \
	} \
} while (0)

static uint32_t next(uint32_t *state)
{
	*state = *state * 1664525u + 1013904223u;
	return *state;
}

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	uint32_t format = strcmp(argv[1], "pigg") == 0 ?
		PG_PIGG2 : PG_HOGG10;
	struct rlimit limit = { 48u * 1024u * 1024u,
		48u * 1024u * 1024u };
	if (setrlimit(RLIMIT_AS, &limit) != 0)
		return 2;
	unlink("large.archive");
	pg_error error;
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_writer *writer = NULL;
	pg_source *source = NULL;
	pg_file *file = NULL;
	pg_reader *reader = NULL;
	CHECK(pg_context_open(&context, &error));
	CHECK(pg_archive_builder_create(context, "large.archive",
		format, 0, &builder, &error));
	pg_write_options options;
	pg_write_options_init(&options, SIZE);
	options.entry.compression = PG_COMPRESS_NEVER;
	CHECK(pg_writer_open_archive_builder(builder, "large.bin",
		&options, &writer, &error));
	uint8_t bytes[CHUNK];
	uint32_t state = 1;
	for (size_t offset = 0; offset < SIZE; offset += CHUNK) {
		for (size_t i = 0; i < CHUNK; i++)
			bytes[i] = (uint8_t)(next(&state) >> 24);
		size_t count = 0;
		CHECK(pg_writer_write(writer, bytes, CHUNK, &count, &error));
		if (count != CHUNK)
			return 1;
	}
	CHECK(pg_writer_finish(writer, &error));
	CHECK(pg_writer_close(&writer, &error));
	CHECK(pg_archive_builder_finish(builder, &error));
	CHECK(pg_archive_builder_close(&builder, &error));
	CHECK(pg_source_open(context, "large.archive", NULL,
		&source, &error));
	CHECK(pg_source_find(source, "large.bin", &file, &error));
	CHECK(pg_reader_open(file, PG_READ_LOGICAL, &reader, &error));
	state = 1;
	for (size_t offset = 0; offset < SIZE; offset += CHUNK) {
		size_t count = 0;
		CHECK(pg_reader_read(reader, bytes, CHUNK, &count, &error));
		if (count != CHUNK)
			return 1;
		for (size_t i = 0; i < CHUNK; i++) {
			if (bytes[i] != (uint8_t)(next(&state) >> 24))
				return 1;
		}
	}
	size_t count = 1;
	if (pg_reader_read(reader, bytes, CHUNK, &count,
		&error) != PG_END || count != 0)
		return 1;
	CHECK(pg_reader_close(&reader, &error));
	CHECK(pg_file_close(&file, &error));
	CHECK(pg_source_validate(source, &error));
	CHECK(pg_source_close(&source, &error));
	CHECK(pg_context_close(&context, &error));
	if (unlink("large.archive") != 0)
		return 1;
	return 0;
}
