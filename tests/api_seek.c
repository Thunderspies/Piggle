#include "api_test.h"

static unsigned char sample(uint64_t offset)
{
	return (unsigned char)((offset * 17) ^ (offset >> 8) ^
		(offset >> 19));
}

static int read_at(pg_reader *reader, uint64_t offset, uint64_t size)
{
	unsigned char bytes[4096];
	uint64_t position = UINT64_MAX;
	size_t count = 0;
	pg_error error;

	STATUS(pg_reader_seek(reader, offset, &error), PG_OK);
	STATUS(pg_reader_tell(reader, &position, &error), PG_OK);
	CHECK(position == offset);
	STATUS(pg_reader_read(reader, bytes, sizeof(bytes), &count, &error),
		offset == size ? PG_END : PG_OK);
	CHECK(count <= sizeof(bytes) && count <= size - offset);
	CHECK(count || offset == size);
	for (size_t i = 0; i < count; i++)
		CHECK(bytes[i] == sample(offset + i));
	STATUS(pg_reader_tell(reader, &position, &error), PG_OK);
	CHECK(position == offset + count);
	return 0;
}

static int run_case(uint32_t format, uint32_t compression, size_t size,
		int stored)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_archive_builder *builder = NULL;
	pg_reader *reader = NULL, *other = NULL;
	pg_error error;
	pg_entry_options entry = { 0 };
	unsigned char *data = size ? malloc(size) : NULL;
	pg_reader_info info;
	uint64_t position;

	CHECK(!size || data);
	for (size_t i = 0; i < size; i++)
		data[i] = sample(i);
	CHECK(clear_file("input") == 0);
	CHECK(clear_file("archive") == 0);
	STATUS(pg_context_open(&context, &error), PG_OK);
	if (format == PG_LOOSE) {
		CHECK(put_bytes("input", data, size) == 0);
		STATUS(pg_reader_open_native(context, "input", &reader,
			&error), PG_OK);
		STATUS(pg_reader_open_native(context, "input", &other,
			&error), PG_OK);
	} else {
		entry.compression = compression;
		STATUS(pg_archive_builder_create(context, "archive", format,
			0, &builder, &error), PG_OK);
		STATUS(pg_archive_builder_write_all(builder, "file", data,
			size, &entry, &error), PG_OK);
		STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
		STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
		STATUS(pg_source_open(context, "archive", NULL, &source,
			&error), PG_OK);
		STATUS(pg_reader_open_source(source, "file", stored ?
			PG_READ_STORED : PG_READ_LOGICAL, &reader, &error),
				PG_OK);
		STATUS(pg_reader_open_source(source, "file", PG_READ_LOGICAL,
			&other, &error), PG_OK);
	}
	free(data);
	STATUS(pg_reader_inspect(reader, &info, &error), PG_OK);
	STATUS(pg_reader_tell(NULL, &position, &error), PG_INVALID);
	CHECK(position == 0);
	STATUS(pg_reader_tell(reader, NULL, &error), PG_INVALID);
	STATUS(pg_reader_seek(NULL, 0, &error), PG_INVALID);
	STATUS(pg_reader_seek(reader, UINT64_MAX, &error), PG_INVALID);
	STATUS(pg_reader_seek(reader, info.size + 1, &error), PG_INVALID);
	STATUS(pg_reader_tell(reader, &position, &error), PG_OK);
	CHECK(position == 0);
	if (stored) {
		unsigned char expected[257], found[257];
		size_t count, reread;
		STATUS(pg_reader_read(reader, expected, sizeof(expected),
			&count, &error), PG_OK);
		CHECK(count > 4);
		STATUS(pg_reader_seek(reader, info.size, &error), PG_OK);
		STATUS(pg_reader_read(reader, found, sizeof(found),
			&reread, &error), PG_END);
		STATUS(pg_reader_seek(reader, 3, &error), PG_OK);
		STATUS(pg_reader_read(reader, found, count - 3,
			&reread, &error), PG_OK);
		CHECK(reread == count - 3);
		CHECK(memcmp(expected + 3, found, reread) == 0);
	} else {
		CHECK(read_at(reader, size, size) == 0);
		CHECK(read_at(reader, 0, size) == 0);
		if (size) {
			CHECK(read_at(reader, size - 1, size) == 0);
			CHECK(read_at(reader, size / 2, size) == 0);
			CHECK(read_at(reader, 1, size) == 0);
			for (uint64_t i = 1048576; i < size; i += 1048576)
				CHECK(read_at(reader, i + 7, size) == 0);
			for (uint64_t i = size / 1048576; i > 0; i--)
				CHECK(read_at(reader, i * 1048576 - 7,
					size) == 0);
			CHECK(read_at(reader, 11, size) == 0);
		}
	}
	STATUS(pg_reader_tell(other, &position, &error), PG_OK);
	CHECK(position == 0);
	STATUS(pg_reader_close(&other, &error), PG_OK);
	STATUS(pg_reader_close(&reader, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static int stale_case(void)
{
	pg_context *context = NULL;
	pg_reader *reader = NULL;
	pg_error error;
	uint64_t position;

	CHECK(put_bytes("input", payload, sizeof(payload)) == 0);
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_reader_open_native(context, "input", &reader, &error), PG_OK);
	CHECK(put_bytes("input", payload, 1) == 0);
	STATUS(pg_reader_seek(reader, 0, &error), PG_STALE);
	STATUS(pg_reader_seek(reader, 0, &error), PG_INVALID);
	STATUS(pg_reader_tell(reader, &position, &error), PG_OK);
	CHECK(position == 0);
	STATUS(pg_reader_close(&reader, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static int checksum_case(void)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_archive_builder *builder = NULL;
	pg_reader *reader = NULL;
	pg_error error;
	unsigned char digest[16] = { 1 };
	unsigned char byte;
	size_t count;
	FILE *stream;

	CHECK(clear_file("corrupt.pigg") == 0);
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_archive_builder_create(context, "corrupt.pigg", PG_PIGG2,
		0, &builder, &error), PG_OK);
	STATUS(pg_archive_builder_write_all(builder, "a", payload,
		sizeof(payload), NULL, &error), PG_OK);
	STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	stream = fopen("corrupt.pigg", "r+b");
	CHECK(stream && !fseek(stream, 16 + 28, SEEK_SET));
	CHECK(fwrite(digest, 1, sizeof(digest), stream) == sizeof(digest));
	CHECK(!fclose(stream));
	STATUS(pg_source_open(context, "corrupt.pigg", NULL, &source,
		&error), PG_OK);
	STATUS(pg_reader_open_source(source, "a", PG_READ_LOGICAL,
		&reader, &error), PG_OK);
	STATUS(pg_reader_seek(reader, sizeof(payload), &error), PG_OK);
	STATUS(pg_reader_read(reader, &byte, 0, &count, &error), PG_OK);
	STATUS(pg_reader_read(reader, &byte, 1, &count, &error), PG_CHECKSUM);
	CHECK(count == 0);
	STATUS(pg_reader_seek(reader, 0, &error), PG_INVALID);
	STATUS(pg_reader_close(&reader, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	if (!strcmp(argv[1], "native"))
		return run_case(PG_LOOSE, PG_COMPRESS_NEVER, 131123, 0);
	if (!strcmp(argv[1], "empty"))
		return run_case(PG_HOGG10, PG_COMPRESS_NEVER, 0, 0);
	if (!strcmp(argv[1], "stale"))
		return stale_case();
	if (!strcmp(argv[1], "checksum"))
		return checksum_case();
	uint32_t format = strstr(argv[1], "hogg") ? PG_HOGG10 : PG_PIGG2;
	uint32_t compression = strstr(argv[1], "raw") ?
		PG_COMPRESS_NEVER : PG_COMPRESS_FORCE;
	size_t size = strstr(argv[1], "evict") ?
		70u * 1048576u + 123 : 3u * 1048576u + 1730;

	return run_case(format, compression, size,
		strstr(argv[1], "stored") != NULL);
}
