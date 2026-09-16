#include "api_test.h"

static const unsigned char encoded[] = {
	0x78, 0x9c, 0xcb, 0xcd, 0x2c, 0x2e, 0xce, 0xcc, 0xcf, 0x53,
	0x28, 0x48, 0xac, 0xcc, 0xc9, 0x4f, 0x4c, 0x51, 0xc8, 0xc5,
	0xcf, 0x07, 0x00, 0xbb, 0x39, 0x12, 0x65
};
static const unsigned char stored_digest[] = { 0xb2, 0x34, 0x88, 0xc3 };

int main(void)
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_source *source = NULL, *wrong = NULL;
	pg_file *file = NULL;
	pg_writer *writer = NULL;
	pg_reader *reader = NULL;
	pg_archive_options create = { PG_HOGG10, 0, PG_CHECKSUM_STORED };
	pg_source_options open = { PG_HOGG10, PG_WRITE, PG_CHECKSUM_STORED };
	pg_write_options write;
	pg_file_info info;
	pg_error error;
	unsigned char bytes[128];
	size_t count;

	CHECK(!clear_file("mission.hogg"));
	CHECK(!clear_file("logical.pigg"));
	CHECK(!clear_file("exported"));
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_archive_builder_create_options(context, "mission.hogg",
		&create, &builder, &error), PG_OK);
	pg_write_options_init(&write, 47);
	write.encoding = PG_ZLIB;
	write.input_size = sizeof(encoded);
	write.entry.digest_kind = PG_DIGEST_MD5_32;
	write.entry.expected_digest_domain = PG_CHECKSUM_STORED;
	memcpy(write.entry.expected_digest, stored_digest, 4);
	STATUS(pg_writer_open_archive_builder(builder, "arc", &write,
		&writer, &error), PG_OK);
	STATUS(pg_writer_write(writer, encoded, sizeof(encoded), &count,
		&error), PG_OK);
	STATUS(pg_writer_finish(writer, &error), PG_OK);
	STATUS(pg_writer_close(&writer, &error), PG_OK);
	STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	STATUS(pg_source_open(context, "mission.hogg", &open, &source,
		&error), PG_OK);
	STATUS(pg_source_validate(source, &error), PG_OK);
	STATUS(pg_source_find(source, "arc", &file, &error), PG_OK);
	STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	CHECK(info.checksum_domain == PG_CHECKSUM_STORED);
	CHECK(!memcmp(info.digest, stored_digest, 4));
	for (uint32_t mode = PG_READ_LOGICAL; mode <= PG_READ_STORED; mode++) {
		STATUS(pg_reader_open(file, mode, &reader, &error), PG_OK);
		STATUS(pg_reader_read(reader, bytes, sizeof(bytes), &count,
			&error), PG_OK);
		CHECK(count == (mode == PG_READ_LOGICAL ? 47 :
			sizeof(encoded)));
		STATUS(pg_reader_read(reader, bytes, sizeof(bytes), &count,
			&error), PG_END);
		STATUS(pg_reader_close(&reader, &error), PG_OK);
	}
	STATUS(pg_file_export(file, "exported", 0, &error), PG_OK);
	CHECK(!expect_bytes("exported", "mission payload mission payload "
		"mission payload", 47));
	STATUS(pg_archive_builder_create(context, "logical.pigg", PG_PIGG2,
		0, &builder, &error), PG_OK);
	STATUS(pg_archive_builder_copy(builder, "arc", file,
		PG_COMPRESS_FORCE, &error), PG_OK);
	STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	pg_source_options logical_write = { PG_PIGG2, PG_WRITE, 0 };
	pg_pack_options pack = { PG_COMPRESS_FORCE, 0, PG_CHECKSUM_STORED };

	STATUS(pg_source_open(context, "logical.pigg", &logical_write, &wrong,
		&error), PG_OK);
	STATUS(pg_source_copy(wrong, "copy", file, PG_COMPRESS_FORCE, &error),
		PG_OK);
	STATUS(pg_source_validate(wrong, &error), PG_OK);
	STATUS(pg_source_close(&wrong, &error), PG_OK);
	CHECK(!clear_file("packed.hogg"));
	STATUS(pg_source_pack(source, "packed.hogg", PG_HOGG10, &pack,
		&error), PG_OK);
	STATUS(pg_source_open(context, "packed.hogg", &open, &wrong,
		&error), PG_OK);
	STATUS(pg_source_validate(wrong, &error), PG_OK);
	STATUS(pg_source_close(&wrong, &error), PG_OK);
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_source_write_all(source, "new", "new", 3, NULL, &error),
		PG_OK);
	STATUS(pg_source_validate(source, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_source_open(context, "mission.hogg", NULL, &wrong,
		&error), PG_OK);
	STATUS(pg_source_validate(wrong, &error), PG_CHECKSUM);
	STATUS(pg_source_close(&wrong, &error), PG_OK);
	STATUS(pg_source_open(context, "logical.pigg", NULL, &source,
		&error), PG_OK);
	STATUS(pg_source_validate(source, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	open.checksum_domain = 2;
	STATUS(pg_source_open(context, "logical.pigg", &open, &source,
		&error), PG_INVALID);
	create.format = PG_PIGG2;
	STATUS(pg_archive_builder_create_options(context, "invalid", &create,
		&builder, &error), PG_UNSUPPORTED);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}
