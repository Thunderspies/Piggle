#include "../api_test.hpp"

namespace {

struct header_read_fixture {
	pg_context *context;
	pg_source *source;
};

static int header_archive(header_read_fixture *fixture, uint32_t format,
	uint32_t compression, const void *data, size_t size,
	const void *header, size_t header_size)
{
	pg_archive_builder *builder = NULL;
	pg_entry_options entry = {};
	pg_error error;

	entry.compression = compression;
	entry.cached_header = header;
	entry.cached_header_size = header_size;
	PG_STATUS(pg_context_open(&fixture->context, &error), PG_OK);
	PG_STATUS(pg_archive_builder_create(fixture->context, "archive",
		format, 0, &builder, &error), PG_OK);
	PG_STATUS(pg_archive_builder_write_all(builder, "file", data, size,
		&entry, &error), PG_OK);
	PG_STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	PG_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	PG_STATUS(pg_source_open(fixture->context, "archive", NULL,
		&fixture->source, &error), PG_OK);
	return 0;
}

static int header_cleanup(header_read_fixture *fixture)
{
	pg_error error;

	PG_STATUS(pg_source_close(&fixture->source, &error), PG_OK);
	PG_STATUS(pg_context_close(&fixture->context, &error), PG_OK);
	PG_CHECK(!clear_file("archive"));
	return 0;
}

using ApiHeaderRead = piggle_test::ContractTest;

TEST_F(ApiHeaderRead, CachedRangesAndPayloadOnlyReads)
{
	for (uint32_t format : {PG_PIGG2, PG_HOGG10}) {
		for (uint32_t compression :
			{PG_COMPRESS_NEVER, PG_COMPRESS_FORCE}) {
			header_read_fixture fixture = {};
			pg_reader *reader = NULL;
			pg_reader *payload_reader = NULL;
			pg_error error;
			char bytes[16];
			size_t count;

			ASSERT_EQ(0, header_archive(&fixture, format,
				compression, "abcdefghijk", 11,
				"HEADER", 6));
			ASSERT_EQ(PG_OK, pg_reader_open_source(fixture.source,
				"file", PG_READ_LOGICAL, &reader, &error));
			ASSERT_EQ(PG_OK, pg_reader_open_source(fixture.source,
				"file", PG_READ_LOGICAL_PAYLOAD,
				&payload_reader, &error));
			ASSERT_EQ(PG_OK, pg_reader_read(reader, bytes, 4,
				&count, &error));
			ASSERT_EQ(4u, count);
			EXPECT_EQ(0, memcmp(bytes, "HEAD", count));
			ASSERT_EQ(PG_OK, pg_reader_read(reader, bytes, 4,
				&count, &error));
			ASSERT_EQ(4u, count);
			EXPECT_EQ(0, memcmp(bytes, "efgh", count));
			ASSERT_EQ(PG_OK, pg_reader_seek(reader, 1, &error));
			ASSERT_EQ(PG_OK, pg_reader_read(reader, bytes, 3,
				&count, &error));
			EXPECT_EQ(0, memcmp(bytes, "EAD", 3));
			ASSERT_EQ(PG_OK, pg_reader_seek(reader, 7, &error));
			ASSERT_EQ(PG_OK, pg_reader_read(reader, bytes,
				sizeof(bytes), &count, &error));
			ASSERT_EQ(4u, count);
			EXPECT_EQ(0, memcmp(bytes, "hijk", count));
			ASSERT_EQ(PG_OK, pg_reader_read(payload_reader, bytes,
				sizeof(bytes), &count, &error));
			ASSERT_EQ(11u, count);
			EXPECT_EQ(0, memcmp(bytes, "abcdefghijk", count));
			ASSERT_EQ(PG_END, pg_reader_read(reader, bytes, 1,
				&count, &error));
			ASSERT_EQ(PG_OK, pg_reader_close(&reader, &error));
			ASSERT_EQ(PG_OK, pg_reader_close(&payload_reader,
				&error));
			ASSERT_EQ(0, header_cleanup(&fixture));
		}
	}
}

TEST_F(ApiHeaderRead, EligibilityUsesCompleteRequestBeforeChunkLimit)
{
	for (uint32_t format : {PG_PIGG2, PG_HOGG10}) {
		for (uint32_t compression :
			{PG_COMPRESS_NEVER, PG_COMPRESS_FORCE}) {
			header_read_fixture fixture = {};
			pg_reader *reader = NULL;
			pg_error error;
			std::vector<unsigned char> data(200000, 'p');
			std::vector<unsigned char> header(100000, 'h');
			std::vector<unsigned char> bytes(150000);
			size_t count;

			ASSERT_EQ(0, header_archive(&fixture, format,
				compression, data.data(), data.size(),
				header.data(), header.size()));
			ASSERT_EQ(PG_OK, pg_reader_open_source(fixture.source,
				"file", PG_READ_LOGICAL, &reader, &error));
			ASSERT_EQ(PG_OK, pg_reader_read(reader, bytes.data(),
				bytes.size(), &count, &error));
			ASSERT_GT(count, 0u);
			EXPECT_TRUE(std::all_of(bytes.begin(),
				bytes.begin() + count,
				[](unsigned char value) {
					return value == 'p';
				}));
			ASSERT_EQ(PG_OK, pg_reader_seek(reader, 0, &error));
			ASSERT_EQ(PG_OK, pg_reader_read(reader, bytes.data(),
				header.size(), &count, &error));
			ASSERT_EQ(header.size(), count);
			EXPECT_EQ(0, memcmp(bytes.data(),
				header.data(), count));
			ASSERT_EQ(PG_OK, pg_reader_close(&reader, &error));
			ASSERT_EQ(0, header_cleanup(&fixture));
		}
	}
}

TEST_F(ApiHeaderRead, ClampsToEofAndRetainsHeaderAfterSelectionClose)
{
	for (uint32_t format : {PG_PIGG2, PG_HOGG10}) {
		header_read_fixture fixture = {};
		pg_file *file = NULL;
		pg_reader *reader = NULL;
		pg_reader_info info;
		pg_error error;
		char bytes[32];
		size_t count;

		ASSERT_EQ(0, header_archive(&fixture, format,
			PG_COMPRESS_FORCE, "payload", 7, "CACHE!!extra", 12));
		ASSERT_EQ(PG_OK, pg_source_find(fixture.source, "file",
			&file, &error));
		ASSERT_EQ(PG_OK, pg_file_read_all(file, bytes, sizeof(bytes),
			&count, &error));
		ASSERT_EQ(7u, count);
		EXPECT_EQ(0, memcmp(bytes, "CACHE!!", count));
		ASSERT_EQ(PG_OK, pg_reader_open(file, PG_READ_LOGICAL,
			&reader, &error));
		ASSERT_EQ(PG_OK, pg_file_close(&file, &error));
		ASSERT_EQ(PG_OK, pg_source_close(&fixture.source, &error));
		ASSERT_EQ(PG_OK, pg_reader_inspect(reader, &info, &error));
		EXPECT_EQ(7u, info.size);
		ASSERT_EQ(PG_OK, pg_reader_seek(reader, 2, &error));
		ASSERT_EQ(PG_OK, pg_reader_read(reader, bytes, sizeof(bytes),
			&count, &error));
		ASSERT_EQ(5u, count);
		EXPECT_EQ(0, memcmp(bytes, "CHE!!", count));
		ASSERT_EQ(PG_END, pg_reader_read(reader, bytes, 1,
			&count, &error));
		ASSERT_EQ(PG_OK, pg_reader_close(&reader, &error));
		ASSERT_EQ(0, header_cleanup(&fixture));
	}
}

TEST_F(ApiHeaderRead, CachedSeekDefersMalformedPayloadUntilFallbackOrEof)
{
	header_read_fixture fixture = {};
	pg_reader *reader = NULL;
	pg_error error;
	FILE *native;
	char bytes[32];
	size_t count;

	ASSERT_EQ(0, header_archive(&fixture, PG_PIGG2, PG_COMPRESS_FORCE,
		"payload with more data", 22, "HEADER", 6));
	ASSERT_EQ(PG_OK, pg_source_close(&fixture.source, &error));
	native = fopen("archive", "r+b");
	ASSERT_NE(nullptr, native);
	unsigned char offset[4];

	ASSERT_EQ(0, fseek(native, 16 + 16, SEEK_SET));
	ASSERT_EQ(sizeof(offset), fread(offset, 1, sizeof(offset), native));
	uint32_t at = (uint32_t)offset[0] | (uint32_t)offset[1] << 8 |
		(uint32_t)offset[2] << 16 | (uint32_t)offset[3] << 24;

	ASSERT_EQ(0, fseek(native, at, SEEK_SET));
	ASSERT_NE(EOF, fputc(0, native));
	ASSERT_EQ(0, fclose(native));
	ASSERT_EQ(PG_OK, pg_source_open(fixture.context, "archive", NULL,
		&fixture.source, &error));
	ASSERT_EQ(PG_OK, pg_reader_open_source(fixture.source, "file",
		PG_READ_LOGICAL, &reader, &error));
	ASSERT_EQ(PG_OK, pg_reader_seek(reader, 3, &error));
	ASSERT_EQ(PG_OK, pg_reader_read(reader, bytes, 3, &count, &error));
	ASSERT_EQ(3u, count);
	EXPECT_EQ(0, memcmp(bytes, "DER", count));
	ASSERT_EQ(PG_CORRUPT, pg_reader_read(reader, bytes, 1,
		&count, &error));
	EXPECT_EQ(0u, count);
	ASSERT_EQ(PG_OK, pg_reader_close(&reader, &error));
	ASSERT_EQ(PG_OK, pg_reader_open_source(fixture.source, "file",
		PG_READ_LOGICAL, &reader, &error));
	ASSERT_EQ(PG_OK, pg_reader_seek(reader, 22, &error));
	ASSERT_EQ(PG_CORRUPT, pg_reader_read(reader, bytes, 1,
		&count, &error));
	ASSERT_EQ(PG_OK, pg_reader_close(&reader, &error));
	ASSERT_EQ(0, header_cleanup(&fixture));
}

TEST_F(ApiHeaderRead, CachedReadThroughEofStillChecksPayloadDigest)
{
	header_read_fixture fixture = {};
	pg_reader *reader = NULL;
	pg_error error;
	unsigned char digest[16] = {1};
	char bytes[16];
	size_t count;

	ASSERT_EQ(0, header_archive(&fixture, PG_PIGG2, PG_COMPRESS_NEVER,
		"payload", 7, "HEADER!", 7));
	ASSERT_EQ(PG_OK, pg_source_close(&fixture.source, &error));
	FILE *native = fopen("archive", "r+b");

	ASSERT_NE(nullptr, native);
	ASSERT_EQ(0, fseek(native, 16 + 28, SEEK_SET));
	ASSERT_EQ(sizeof(digest), fwrite(digest, 1, sizeof(digest), native));
	ASSERT_EQ(0, fclose(native));
	ASSERT_EQ(PG_OK, pg_source_open(fixture.context, "archive", NULL,
		&fixture.source, &error));
	ASSERT_EQ(PG_OK, pg_reader_open_source(fixture.source, "file",
		PG_READ_LOGICAL, &reader, &error));
	ASSERT_EQ(PG_CHECKSUM, pg_reader_read(reader, bytes, sizeof(bytes),
		&count, &error));
	EXPECT_EQ(7u, count);
	EXPECT_EQ(0, memcmp(bytes, "HEADER!", 7));
	ASSERT_EQ(PG_INVALID, pg_reader_seek(reader, 0, &error));
	ASSERT_EQ(PG_OK, pg_reader_close(&reader, &error));
	ASSERT_EQ(0, header_cleanup(&fixture));
}

TEST_F(ApiHeaderRead, DecoderCheckpointsSurviveCachedPositionChanges)
{
	header_read_fixture fixture = {};
	pg_reader *reader = NULL;
	pg_error error;
	std::vector<unsigned char> data(3 * 1048576 + 31);
	std::vector<unsigned char> header(128, 'h');
	unsigned char bytes[37];
	size_t count;

	for (size_t i = 0; i < data.size(); ++i)
		data[i] = (unsigned char)(i * 37 + i / 251);
	ASSERT_EQ(0, header_archive(&fixture, PG_HOGG10, PG_COMPRESS_FORCE,
		data.data(), data.size(), header.data(), header.size()));
	ASSERT_EQ(PG_OK, pg_reader_open_source(fixture.source, "file",
		PG_READ_LOGICAL, &reader, &error));
	for (uint64_t offset : {2097159u, 4u, 1048601u, 12u, 3145713u}) {
		ASSERT_EQ(PG_OK, pg_reader_seek(reader, offset, &error));
		size_t done = 0;

		while (done < sizeof(bytes)) {
			ASSERT_EQ(PG_OK, pg_reader_read(reader, bytes + done,
				sizeof(bytes) - done, &count, &error));
			ASSERT_GT(count, 0u);
			done += count;
		}
		const unsigned char *expected = offset < header.size()
			? header.data() + offset : data.data() + offset;

		EXPECT_EQ(0, memcmp(bytes, expected, sizeof(bytes)));
	}
	ASSERT_EQ(PG_OK, pg_reader_close(&reader, &error));
	ASSERT_EQ(0, header_cleanup(&fixture));
}

} /* namespace */
