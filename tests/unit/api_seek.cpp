#include "../api_test.hpp"

namespace {

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

	PG_STATUS(pg_reader_seek(reader, offset, &error), PG_OK);
	PG_STATUS(pg_reader_tell(reader, &position, &error), PG_OK);
	PG_CHECK(position == offset);
	PG_STATUS(pg_reader_read(
				reader, bytes, sizeof(bytes), &count, &error),
		offset == size ? PG_END : PG_OK);
	PG_CHECK(count <= sizeof(bytes) && count <= size - offset);
	PG_CHECK(count || offset == size);
	for (size_t i = 0; i < count; i++)
		PG_CHECK(bytes[i] == sample(offset + i));
	PG_STATUS(pg_reader_tell(reader, &position, &error), PG_OK);
	PG_CHECK(position == offset + count);
	return 0;
}

static int run_case(
	uint32_t format, uint32_t compression, size_t size, int stored)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_archive_builder *builder = NULL;
	pg_reader *reader = NULL, *other = NULL;
	pg_error error;
	pg_entry_options entry = {};
	unsigned char *data =
		size ? (unsigned char *)malloc(size) : NULL;
	pg_reader_info info;
	uint64_t position;

	PG_CHECK(!size || data);
	for (size_t i = 0; i < size; i++)
		data[i] = sample(i);
	PG_CHECK(clear_file("input") == 0);
	PG_CHECK(clear_file("archive") == 0);
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	if (format == PG_LOOSE) {
		PG_CHECK(put_bytes("input", data, size) == 0);
		PG_STATUS(pg_reader_open_native(
					context, "input", &reader, &error),
			PG_OK);
		PG_STATUS(pg_reader_open_native(
					context, "input", &other, &error),
			PG_OK);
	} else {
		entry.compression = compression;
		PG_STATUS(pg_archive_builder_create(context, "archive",
					format, 0, &builder, &error),
			PG_OK);
		PG_STATUS(pg_archive_builder_write_all(builder, "file",
					data, size, &entry, &error),
			PG_OK);
		PG_STATUS(pg_archive_builder_finish(builder, &error),
			PG_OK);
		PG_STATUS(pg_archive_builder_close(&builder, &error),
			PG_OK);
		PG_STATUS(pg_source_open(context, "archive", NULL,
					&source, &error),
			PG_OK);
		PG_STATUS(pg_reader_open_source(source, "file",
					stored ? PG_READ_STORED
						: PG_READ_LOGICAL,
					&reader, &error),
			PG_OK);
		PG_STATUS(pg_reader_open_source(source, "file",
					PG_READ_LOGICAL, &other, &error),
			PG_OK);
	}
	free(data);
	PG_STATUS(pg_reader_inspect(reader, &info, &error), PG_OK);
	PG_STATUS(pg_reader_tell(NULL, &position, &error), PG_INVALID);
	PG_CHECK(position == 0);
	PG_STATUS(pg_reader_tell(reader, NULL, &error), PG_INVALID);
	PG_STATUS(pg_reader_seek(NULL, 0, &error), PG_INVALID);
	PG_STATUS(
		pg_reader_seek(reader, UINT64_MAX, &error), PG_INVALID);
	PG_STATUS(pg_reader_seek(reader, info.size + 1, &error),
		PG_INVALID);
	PG_STATUS(pg_reader_tell(reader, &position, &error), PG_OK);
	PG_CHECK(position == 0);
	if (stored) {
		unsigned char expected[257], found[257];
		size_t count, reread;
		PG_STATUS(pg_reader_read(reader, expected,
					sizeof(expected), &count, &error),
			PG_OK);
		PG_CHECK(count > 4);
		PG_STATUS(pg_reader_seek(reader, info.size, &error),
			PG_OK);
		PG_STATUS(pg_reader_read(reader, found, sizeof(found),
					&reread, &error),
			PG_END);
		PG_STATUS(pg_reader_seek(reader, 3, &error), PG_OK);
		PG_STATUS(pg_reader_read(reader, found, count - 3,
					&reread, &error),
			PG_OK);
		PG_CHECK(reread == count - 3);
		PG_CHECK(memcmp(expected + 3, found, reread) == 0);
	} else {
		PG_CHECK(read_at(reader, size, size) == 0);
		PG_CHECK(read_at(reader, 0, size) == 0);
		if (size) {
			PG_CHECK(read_at(reader, size - 1, size) == 0);
			PG_CHECK(read_at(reader, size / 2, size) == 0);
			PG_CHECK(read_at(reader, 1, size) == 0);
			for (uint64_t i = 1048576; i < size;
				i += 1048576)
				PG_CHECK(read_at(reader, i + 7, size) ==
					0);
			for (uint64_t i = size / 1048576; i > 0; i--)
				PG_CHECK(
					read_at(reader, i * 1048576 - 7,
						size) == 0);
			PG_CHECK(read_at(reader, 11, size) == 0);
		}
	}
	PG_STATUS(pg_reader_tell(other, &position, &error), PG_OK);
	PG_CHECK(position == 0);
	PG_STATUS(pg_reader_close(&other, &error), PG_OK);
	PG_STATUS(pg_reader_close(&reader, &error), PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static int stale_case(void)
{
	pg_context *context = NULL;
	pg_reader *reader = NULL;
	pg_error error;
	uint64_t position;

	PG_CHECK(put_bytes("input", payload, sizeof(payload)) == 0);
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(pg_reader_open_native(
				context, "input", &reader, &error),
		PG_OK);
	PG_CHECK(put_bytes("input", payload, 1) == 0);
	PG_STATUS(pg_reader_seek(reader, 0, &error), PG_STALE);
	PG_STATUS(pg_reader_seek(reader, 0, &error), PG_INVALID);
	PG_STATUS(pg_reader_tell(reader, &position, &error), PG_OK);
	PG_CHECK(position == 0);
	PG_STATUS(pg_reader_close(&reader, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static int checksum_case(void)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_archive_builder *builder = NULL;
	pg_reader *reader = NULL;
	pg_error error;
	unsigned char digest[16] = {1};
	unsigned char byte;
	size_t count;
	FILE *stream;

	PG_CHECK(clear_file("corrupt.pigg") == 0);
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(pg_archive_builder_create(context, "corrupt.pigg",
				PG_PIGG2, 0, &builder, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_write_all(builder, "a", payload,
				sizeof(payload), NULL, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	PG_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	stream = fopen("corrupt.pigg", "r+b");
	PG_CHECK(stream && !fseek(stream, 16 + 28, SEEK_SET));
	PG_CHECK(fwrite(digest, 1, sizeof(digest), stream) ==
		sizeof(digest));
	PG_CHECK(!fclose(stream));
	PG_STATUS(pg_source_open(context, "corrupt.pigg", NULL, &source,
				&error),
		PG_OK);
	PG_STATUS(pg_reader_open_source(source, "a", PG_READ_LOGICAL,
				&reader, &error),
		PG_OK);
	PG_STATUS(
		pg_reader_seek(reader, sizeof(payload), &error), PG_OK);
	PG_STATUS(pg_reader_read(reader, &byte, 0, &count, &error),
		PG_OK);
	PG_STATUS(pg_reader_read(reader, &byte, 1, &count, &error),
		PG_CHECKSUM);
	PG_CHECK(count == 0);
	PG_STATUS(pg_reader_seek(reader, 0, &error), PG_INVALID);
	PG_STATUS(pg_reader_close(&reader, &error), PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

using ApiSeek = piggle_test::ContractTest;

TEST_F(ApiSeek, api_seek_native)
{
	ASSERT_EQ(0, run_case(PG_LOOSE, PG_COMPRESS_NEVER, 131123, 0));
}

TEST_F(ApiSeek, api_seek_empty)
{
	ASSERT_EQ(0, run_case(PG_HOGG10, PG_COMPRESS_NEVER, 0, 0));
}

TEST_F(ApiSeek, api_seek_stale)
{
	ASSERT_EQ(0, stale_case());
}

TEST_F(ApiSeek, api_seek_checksum)
{
	ASSERT_EQ(0, checksum_case());
}

TEST_F(ApiSeek, api_seek_pigg_raw)
{
	ASSERT_EQ(0,
		run_case(PG_PIGG2, PG_COMPRESS_NEVER,
			3u * 1048576u + 1730, 0));
}

TEST_F(ApiSeek, api_seek_hogg_raw)
{
	ASSERT_EQ(0,
		run_case(PG_HOGG10, PG_COMPRESS_NEVER,
			3u * 1048576u + 1730, 0));
}

TEST_F(ApiSeek, api_seek_pigg_zlib)
{
	ASSERT_EQ(0,
		run_case(PG_PIGG2, PG_COMPRESS_FORCE,
			3u * 1048576u + 1730, 0));
}

TEST_F(ApiSeek, api_seek_hogg_zlib)
{
	ASSERT_EQ(0,
		run_case(PG_HOGG10, PG_COMPRESS_FORCE,
			3u * 1048576u + 1730, 0));
}

TEST_F(ApiSeek, api_seek_pigg_stored)
{
	ASSERT_EQ(0,
		run_case(PG_PIGG2, PG_COMPRESS_FORCE,
			3u * 1048576u + 1730, 1));
}

TEST_F(ApiSeek, api_seek_hogg_stored)
{
	ASSERT_EQ(0,
		run_case(PG_HOGG10, PG_COMPRESS_FORCE,
			3u * 1048576u + 1730, 1));
}

TEST_F(ApiSeek, api_seek_pigg_evict)
{
	ASSERT_EQ(0,
		run_case(PG_PIGG2, PG_COMPRESS_FORCE,
			70u * 1048576u + 123, 0));
}

} // namespace
