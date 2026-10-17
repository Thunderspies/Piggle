#include "../api_test.hpp"

namespace {

static const unsigned char encoded[] = {0x78, 0x9c, 0xcb, 0xcd, 0x2c,
	0x2e, 0xce, 0xcc, 0xcf, 0x53, 0x28, 0x48, 0xac, 0xcc, 0xc9,
	0x4f, 0x4c, 0x51, 0xc8, 0xc5, 0xcf, 0x07, 0x00, 0xbb, 0x39,
	0x12, 0x65};
static const unsigned char stored_digest[] = {0xb2, 0x34, 0x88, 0xc3};

static int checksum_contract()
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_source *source = NULL, *wrong = NULL;
	pg_file *file = NULL;
	pg_writer *writer = NULL;
	pg_reader *reader = NULL;
	pg_archive_options create = {PG_HOGG10, 0, PG_CHECKSUM_STORED};
	pg_source_options open = {
		PG_HOGG10, PG_WRITE, PG_CHECKSUM_STORED};
	pg_write_options write;
	pg_file_info info;
	pg_error error;
	unsigned char bytes[128];
	size_t count;

	PG_CHECK(!clear_file("mission.hogg"));
	PG_CHECK(!clear_file("logical.pigg"));
	PG_CHECK(!clear_file("exported"));
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(pg_archive_builder_create_options(context,
				"mission.hogg", &create, &builder, &error),
		PG_OK);
	pg_write_options_init(&write, 47);
	write.encoding = PG_ZLIB;
	write.input_size = sizeof(encoded);
	write.entry.digest_kind = PG_DIGEST_MD5_32;
	write.entry.expected_digest_domain = PG_CHECKSUM_STORED;
	memcpy(write.entry.expected_digest, stored_digest, 4);
	PG_STATUS(pg_writer_open_archive_builder(
				builder, "arc", &write, &writer, &error),
		PG_OK);
	PG_STATUS(pg_writer_write(writer, encoded, sizeof(encoded),
				&count, &error),
		PG_OK);
	PG_STATUS(pg_writer_finish(writer, &error), PG_OK);
	PG_STATUS(pg_writer_close(&writer, &error), PG_OK);
	PG_STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	PG_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	PG_STATUS(pg_source_open(context, "mission.hogg", &open,
				&source, &error),
		PG_OK);
	PG_STATUS(pg_source_validate(source, &error), PG_OK);
	PG_STATUS(pg_source_find(source, "arc", &file, &error), PG_OK);
	PG_STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	PG_CHECK(info.checksum_domain == PG_CHECKSUM_STORED);
	PG_CHECK(!memcmp(info.digest, stored_digest, 4));
	for (uint32_t mode = PG_READ_LOGICAL; mode <= PG_READ_STORED;
		mode++) {
		PG_STATUS(pg_reader_open(file, mode, &reader, &error),
			PG_OK);
		PG_STATUS(pg_reader_read(reader, bytes, sizeof(bytes),
					&count, &error),
			PG_OK);
		PG_CHECK(count ==
			(mode == PG_READ_LOGICAL ? 47
							: sizeof(encoded)));
		PG_STATUS(pg_reader_read(reader, bytes, sizeof(bytes),
					&count, &error),
			PG_END);
		PG_STATUS(pg_reader_close(&reader, &error), PG_OK);
	}
	PG_STATUS(pg_file_export(file, "exported", 0, &error), PG_OK);
	PG_CHECK(!expect_bytes("exported",
		"mission payload mission payload "
		"mission payload",
		47));
	PG_STATUS(pg_archive_builder_create(context, "logical.pigg",
				PG_PIGG2, 0, &builder, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_copy(builder, "arc", file,
				PG_COMPRESS_FORCE, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	PG_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	pg_source_options logical_write = {PG_PIGG2, PG_WRITE, 0};
	pg_pack_options pack = {
		PG_COMPRESS_FORCE, 0, PG_CHECKSUM_STORED};

	PG_STATUS(pg_source_open(context, "logical.pigg",
				&logical_write, &wrong, &error),
		PG_OK);
	PG_STATUS(pg_source_copy(wrong, "copy", file, PG_COMPRESS_FORCE,
				&error),
		PG_OK);
	PG_STATUS(pg_source_validate(wrong, &error), PG_OK);
	PG_STATUS(pg_source_close(&wrong, &error), PG_OK);
	PG_CHECK(!clear_file("packed.hogg"));
	PG_STATUS(pg_source_pack(source, "packed.hogg", PG_HOGG10,
				&pack, &error),
		PG_OK);
	PG_STATUS(pg_source_open(context, "packed.hogg", &open, &wrong,
				&error),
		PG_OK);
	PG_STATUS(pg_source_validate(wrong, &error), PG_OK);
	PG_STATUS(pg_source_close(&wrong, &error), PG_OK);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_source_write_all(
				source, "new", "new", 3, NULL, &error),
		PG_OK);
	PG_STATUS(pg_source_validate(source, &error), PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_source_open(context, "mission.hogg", NULL, &wrong,
				&error),
		PG_OK);
	PG_STATUS(pg_source_validate(wrong, &error), PG_CHECKSUM);
	PG_STATUS(pg_source_close(&wrong, &error), PG_OK);
	PG_STATUS(pg_source_open(context, "logical.pigg", NULL, &source,
				&error),
		PG_OK);
	PG_STATUS(pg_source_validate(source, &error), PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	open.checksum_domain = 2;
	PG_STATUS(pg_source_open(context, "logical.pigg", &open,
				&source, &error),
		PG_INVALID);
	create.format = PG_PIGG2;
	PG_STATUS(pg_archive_builder_create_options(context, "invalid",
				&create, &builder, &error),
		PG_UNSUPPORTED);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

using ApiChecksum = piggle_test::ContractTest;

TEST_F(ApiChecksum, api_checksum)
{
	ASSERT_EQ(0, checksum_contract());
}

} // namespace

TEST_F(ApiChecksum, StoredTransferSkipsPayloadValidation)
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_source *source = NULL;
	pg_reader *reader = NULL;
	pg_writer *writer = NULL;
	pg_error error;
	pg_stored_write_options options = {};

	ASSERT_EQ(pg_context_open(&context, &error), PG_OK);
	ASSERT_EQ(pg_archive_builder_create(context, "unchecked.hogg",
		PG_HOGG10, 0, &builder, &error), PG_OK);
	options.logical_size = 100;
	options.stored_size = 3;
	options.encoding = PG_ZLIB;
	options.digest_kind = PG_DIGEST_MD5_32;
	options.digest[0] = 42;
	ASSERT_EQ(pg_writer_open_archive_builder_stored(builder, "a",
		&options, &writer, &error), PG_OK);
	size_t count = 0;

	EXPECT_EQ(pg_writer_write(writer, "bad!", 4, &count, &error),
		PG_INVALID);
	EXPECT_EQ(count, 0u);
	ASSERT_EQ(pg_writer_write(writer, "bad", 3, &count, &error), PG_OK);
	ASSERT_EQ(pg_writer_finish(writer, &error), PG_OK);
	ASSERT_EQ(pg_writer_close(&writer, &error), PG_OK);
	ASSERT_EQ(pg_archive_builder_finish(builder, &error), PG_OK);
	ASSERT_EQ(pg_archive_builder_close(&builder, &error), PG_OK);
	ASSERT_EQ(pg_source_open(context, "unchecked.hogg", NULL,
		&source, &error), PG_OK);
	ASSERT_EQ(pg_reader_open_source(source, "a", PG_READ_STORED_UNCHECKED,
		&reader, &error), PG_OK);
	pg_reader_info info = {};
	char bytes[8] = {};

	ASSERT_EQ(pg_reader_inspect(reader, &info, &error), PG_OK);
	EXPECT_EQ(info.size, 3u);
	EXPECT_EQ(info.encoding, PG_ZLIB);
	ASSERT_EQ(pg_reader_read(reader, bytes, sizeof(bytes), &count,
		&error), PG_OK);
	EXPECT_EQ(count, 3u);
	EXPECT_EQ(memcmp(bytes, "bad", 3), 0);
	ASSERT_EQ(pg_reader_read(reader, bytes, sizeof(bytes), &count,
		&error), PG_END);
	ASSERT_EQ(pg_reader_seek(reader, 1, &error), PG_OK);
	ASSERT_EQ(pg_reader_read(reader, bytes, sizeof(bytes), &count,
		&error), PG_OK);
	EXPECT_EQ(count, 2u);
	ASSERT_EQ(pg_reader_close(&reader, &error), PG_OK);
	ASSERT_EQ(pg_reader_open_source(source, "a", PG_READ_STORED,
		&reader, &error), PG_OK);
	EXPECT_EQ(pg_reader_read(reader, bytes, sizeof(bytes), &count,
		&error), PG_CORRUPT);
	ASSERT_EQ(pg_reader_close(&reader, &error), PG_OK);
	ASSERT_EQ(pg_source_close(&source, &error), PG_OK);
	ASSERT_EQ(pg_context_close(&context, &error), PG_OK);
}

TEST_F(ApiChecksum, StoredWriterValidatesMetadataAndExactLength)
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_writer *writer = NULL;
	pg_error error;
	pg_stored_write_options options = {};

	ASSERT_EQ(pg_context_open(&context, &error), PG_OK);
	ASSERT_EQ(pg_archive_builder_create(context, "stored.pigg",
		PG_PIGG2, 0, &builder, &error), PG_OK);
	options.logical_size = options.stored_size = 3;
	options.digest_kind = PG_DIGEST_MD5_32;
	EXPECT_EQ(pg_writer_open_archive_builder_stored(builder, "a",
		&options, &writer, &error), PG_INVALID);
	options.digest_kind = PG_DIGEST_MD5;
	options.checksum_domain = PG_CHECKSUM_STORED;
	EXPECT_EQ(pg_writer_open_archive_builder_stored(builder, "a",
		&options, &writer, &error), PG_INVALID);
	options.checksum_domain = PG_CHECKSUM_LOGICAL;
	options.stored_size = 2;
	EXPECT_EQ(pg_writer_open_archive_builder_stored(builder, "a",
		&options, &writer, &error), PG_INVALID);
	options.encoding = PG_ZLIB;
	options.stored_size = 0;
	EXPECT_EQ(pg_writer_open_archive_builder_stored(builder, "a",
		&options, &writer, &error), PG_INVALID);
	options.encoding = PG_LOGICAL;
	options.stored_size = 3;
	ASSERT_EQ(pg_writer_open_archive_builder_stored(builder, "a",
		&options, &writer, &error), PG_OK);
	EXPECT_EQ(pg_writer_finish(writer, &error), PG_INVALID);
	ASSERT_EQ(pg_writer_close(&writer, &error), PG_OK);
	ASSERT_EQ(pg_archive_builder_close(&builder, &error), PG_OK);
	ASSERT_EQ(pg_context_close(&context, &error), PG_OK);
}
