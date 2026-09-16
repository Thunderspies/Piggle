#include "api_test.h"

static pg_status open_writer(struct fixture *f, unsigned int kind,
	const pg_write_options *options, pg_writer **writer, pg_error *error)
{
	switch (kind) {
	case 0:
		return pg_writer_open_native(f->context, "input", options,
			PG_OVERWRITE, writer, error);
	case 1:
		return pg_writer_open_source(f->source, "a", options, writer,
			error);
	case 2:
		return pg_writer_open_file(f->file, options, writer, error);
	default:
		return pg_writer_open_archive_builder(f->builder, "a", options,
			writer, error);
	}
}

static int writer_state(unsigned int kind)
{
	struct fixture f;
	pg_writer *writer = NULL;
	pg_writer *other = NULL;
	pg_write_options options;
	pg_error error;
	size_t count;

	CHECK(fixture_open(&f) == 0);
	if (kind == 4) {
		STATUS(pg_archive_builder_close(&f.builder, &error), PG_OK);
		STATUS(pg_archive_builder_create(f.context, "built", PG_HOGG10,
			0, &f.builder, &error), PG_OK);
	}
	/* Abort, short input, bad digest, then successful publication. */
	for (unsigned int outcome = 0; outcome < 4; outcome++) {
		pg_write_options_init(&options, 3);
		options.entry.mtime = 43;
		if (outcome == 2)
			options.entry.digest_kind = PG_DIGEST_MD5;
		STATUS(open_writer(&f, kind, &options, &writer, &error), PG_OK);
		if (kind != 0) {
			STATUS(open_writer(&f, kind, &options, &other, &error),
				PG_BUSY);
			CHECK(other == NULL);
		}
		/* Open must have copied the descriptor. */
		memset(&options, 0xa5, sizeof(options));
		STATUS(pg_writer_write(writer, "N", 1, &count, &error), PG_OK);
		CHECK(count == 1);
		STATUS(pg_writer_write(writer, "ignored", 0, &count, &error),
			PG_OK);
		CHECK(count == 0);
		CHECK(expect_bytes("input", payload, sizeof(payload)) == 0);
		CHECK(expect_bytes("root/a", payload, sizeof(payload)) == 0);
		CHECK(expect_absent("built") == 0);
		if (outcome >= 2) {
			STATUS(pg_writer_write(writer, "EW", 2, &count, &error),
				PG_OK);
			CHECK(count == 2);
		}
		if (outcome) {
			pg_status expected = outcome == 1 ? PG_INVALID :
				outcome == 2 ? PG_CHECKSUM : PG_OK;

			STATUS(pg_writer_finish(writer, &error), expected);
			STATUS(pg_writer_finish(writer, &error), PG_INVALID);
			count = SIZE_MAX;
			STATUS(pg_writer_write(writer, NULL, 0, &count, &error),
				PG_INVALID);
			CHECK(count == 0);
		}
		if (kind >= 3) {
			pg_archive_builder *original = f.builder;

			STATUS(pg_archive_builder_finish(f.builder, &error),
				PG_BUSY);
			STATUS(pg_archive_builder_close(&f.builder, &error),
				PG_BUSY);
			CHECK(f.builder == original);
		}
		STATUS(pg_writer_close(&writer, &error), PG_OK);
		CHECK(writer == NULL);
		if (outcome < 3) {
			CHECK(expect_bytes("input", payload,
				sizeof(payload)) == 0);
			CHECK(expect_bytes("root/a", payload,
				sizeof(payload)) == 0);
		}
	}
	if (kind >= 3) {
		STATUS(pg_archive_builder_write_all(f.builder, "A", NULL, 0,
			NULL, &error), PG_EXISTS);
		STATUS(pg_archive_builder_finish(f.builder, &error), PG_OK);
		STATUS(pg_archive_builder_finish(f.builder, &error),
			PG_INVALID);
		STATUS(pg_archive_builder_write_all(f.builder, "late", NULL, 0,
			NULL, &error), PG_INVALID);
		STATUS(pg_archive_builder_import(f.builder, "late", "input",
			PG_COMPRESS_AUTO, &error), PG_INVALID);
		STATUS(pg_archive_builder_copy(f.builder, "late", f.file,
			PG_COMPRESS_AUTO, &error), PG_INVALID);
		STATUS(pg_archive_builder_close(&f.builder, &error), PG_OK);
		pg_source *source = NULL;
		pg_file *file = NULL;
		pg_file_info info;
		unsigned char bytes[4] = { 0 };

		STATUS(pg_source_open(f.context, "built", NULL, &source,
			&error), PG_OK);
		STATUS(pg_source_find(source, "a", &file, &error), PG_OK);
		STATUS(pg_file_inspect(file, &info, &error), PG_OK);
		CHECK(info.mtime == 43 && info.logical_size == 3);
		STATUS(pg_file_read_all(file, bytes, sizeof(bytes), &count,
			&error), PG_OK);
		CHECK(count == 3 && memcmp(bytes, "NEW", 3) == 0);
		STATUS(pg_file_verify(file, &error), PG_OK);
		STATUS(pg_source_validate(source, &error), PG_OK);
		STATUS(pg_file_close(&file, &error), PG_OK);
		STATUS(pg_source_close(&source, &error), PG_OK);
	} else {
		const char *path = kind == 0 ? "input" : "root/a";
		pg_reader *reader = NULL;
		pg_reader_info info;

		CHECK(expect_bytes(path, "NEW", 3) == 0);
		STATUS(pg_reader_open_native(f.context, path, &reader, &error),
			PG_OK);
		STATUS(pg_reader_inspect(reader, &info, &error), PG_OK);
		CHECK(info.mtime == 43 && info.size == 3);
		STATUS(pg_reader_close(&reader, &error), PG_OK);
	}
	CHECK(fixture_close(&f) == 0);
	return 0;
}

static int builder_metadata(uint32_t format)
{
	static const unsigned char digest[] = {
		0x90, 0x01, 0x50, 0x98, 0x3c, 0xd2, 0x4f, 0xb0,
		0xd6, 0x96, 0x3f, 0x7d, 0x28, 0xe1, 0x7f, 0x72
	};
	struct fixture f;
	pg_writer *writer = NULL;
	pg_write_options options;
	pg_source *source = NULL;
	pg_file *file = NULL;
	pg_reader *reader = NULL;
	pg_file_info info;
	pg_reader_info reader_info;
	pg_source_info source_info;
	pg_error error;
	char name[] = "DiR/A";
	unsigned char header[] = { 1, 0, 2 };
	size_t count;

	CHECK(fixture_open(&f) == 0);
	STATUS(pg_archive_builder_close(&f.builder, &error), PG_OK);
	STATUS(pg_archive_builder_create(f.context, "built", format, 0,
		&f.builder, &error), PG_OK);
	pg_write_options_init(&options, 3);
	options.entry.mtime = 123;
	options.entry.original_name = name;
	options.entry.cached_header = header;
	options.entry.cached_header_size = sizeof(header);
	options.entry.compression = PG_COMPRESS_FORCE;
	options.entry.digest_kind = PG_DIGEST_MD5;
	memcpy(options.entry.expected_digest, digest, sizeof(digest));
	STATUS(pg_writer_open_archive_builder(f.builder, "dir/a", &options,
		&writer, &error), PG_OK);
	memset(name, 'x', sizeof(name));
	memset(header, 0xff, sizeof(header));
	memset(&options, 0xa5, sizeof(options));
	STATUS(pg_writer_write(writer, "abc", 3, &count, &error), PG_OK);
	CHECK(count == 3);
	STATUS(pg_writer_finish(writer, &error), PG_OK);
	STATUS(pg_writer_close(&writer, &error), PG_OK);
	STATUS(pg_archive_builder_write_all(f.builder, "empty", NULL, 0,
		NULL, &error), PG_OK);
	STATUS(pg_archive_builder_import(f.builder, "imported", "input",
		PG_COMPRESS_NEVER, &error), PG_OK);
	STATUS(pg_archive_builder_copy(f.builder, "a", f.file,
		PG_COMPRESS_NEVER, &error), PG_OK);
	STATUS(pg_archive_builder_finish(f.builder, &error), PG_OK);
	STATUS(pg_archive_builder_close(&f.builder, &error), PG_OK);
	STATUS(pg_source_open(f.context, "built", NULL, &source, &error),
		PG_OK);
	STATUS(pg_source_inspect(source, &source_info, &error), PG_OK);
	CHECK(source_info.format == format && source_info.access == PG_READ);
	STATUS(pg_source_find(source, "DIR\\A", &file, &error), PG_OK);
	STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	CHECK(strcmp(info.canonical_name, "dir/a") == 0);
	CHECK(strcmp(info.original_name, "DiR/A") == 0);
	CHECK(info.mtime == 123 && info.logical_size == 3);
	CHECK(info.encoding == PG_ZLIB && info.stored_size > 0);
	CHECK(info.cached_header_size == 3);
	CHECK(memcmp(info.cached_header, "\1\0\2", 3) == 0);
	CHECK(info.digest_kind == (format == PG_PIGG2 ?
		PG_DIGEST_MD5 : PG_DIGEST_MD5_32));
	size_t digest_size = format == PG_PIGG2 ? 16 : 4;

	CHECK(memcmp(info.digest, digest, digest_size) == 0);
	for (size_t i = digest_size; i < sizeof(info.digest); i++)
		CHECK(info.digest[i] == 0);
	STATUS(pg_reader_open(file, PG_READ_STORED, &reader, &error), PG_OK);
	STATUS(pg_reader_inspect(reader, &reader_info, &error), PG_OK);
	CHECK(reader_info.size == info.stored_size);
	CHECK(reader_info.logical_size == 3 && reader_info.encoding == PG_ZLIB);
	CHECK(reader_info.mtime == 123);
	unsigned char bytes[64];
	size_t total = 0;

	while (total < info.stored_size) {
		STATUS(pg_reader_read(reader, bytes, 1, &count, &error), PG_OK);
		CHECK(count == 1);
		total += count;
	}
	STATUS(pg_reader_read(reader, bytes, 1, &count, &error), PG_END);
	CHECK(count == 0);
	STATUS(pg_reader_close(&reader, &error), PG_OK);
	STATUS(pg_file_verify(file, &error), PG_OK);
	STATUS(pg_file_close(&file, &error), PG_OK);
	const char *copied[] = { "a", "imported" };

	for (size_t i = 0; i < ARRAY_SIZE(copied); i++) {
		STATUS(pg_source_read_all(source, copied[i], bytes,
			sizeof(bytes), &count, &error), PG_OK);
		CHECK(count == sizeof(payload));
		CHECK(memcmp(bytes, payload, count) == 0);
	}
	STATUS(pg_reader_open_source(source, "empty", PG_READ_LOGICAL,
		&reader, &error), PG_OK);
	STATUS(pg_reader_read(reader, NULL, 0, &count, &error), PG_OK);
	CHECK(count == 0);
	STATUS(pg_reader_read(reader, bytes, 1, &count, &error), PG_END);
	CHECK(count == 0);
	STATUS(pg_reader_close(&reader, &error), PG_OK);
	STATUS(pg_source_validate(source, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	CHECK(fixture_close(&f) == 0);
	return 0;
}

static int builder_stale(uint32_t format)
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_error error;

	CHECK(put_bytes("output", "old", 3) == 0);
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_archive_builder_create(context, "output", format,
		PG_OVERWRITE, &builder, &error), PG_OK);
	STATUS(pg_archive_builder_write_all(builder, "a", payload,
		sizeof(payload), NULL, &error), PG_OK);
	CHECK(expect_bytes("output", "old", 3) == 0);
	CHECK(put_bytes("output", "external edit", 13) == 0);
	STATUS(pg_archive_builder_finish(builder, &error), PG_STALE);
	STATUS(pg_archive_builder_finish(builder, &error), PG_INVALID);
	STATUS(pg_archive_builder_write_all(builder, "b", NULL, 0, NULL,
		&error), PG_INVALID);
	STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	CHECK(expect_bytes("output", "external edit", 13) == 0);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static int read_only(void)
{
	struct fixture f;
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_file *file = NULL;
	pg_writer *writer = NULL;
	pg_write_options options;
	pg_error error;

	CHECK(fixture_open(&f) == 0);
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_source_open(context, "root", NULL, &source, &error), PG_OK);
	STATUS(pg_source_find(source, "a", &file, &error), PG_OK);
	pg_write_options_init(&options, 0);
	STATUS(pg_writer_open_source(source, "a", &options, &writer,
		&error), PG_READ_ONLY);
	CHECK(writer == NULL);
	STATUS(pg_writer_open_file(file, &options, &writer, &error),
		PG_READ_ONLY);
	CHECK(writer == NULL);
	STATUS(pg_file_write_all(file, NULL, 0, NULL, &error), PG_READ_ONLY);
	STATUS(pg_source_write_all(source, "a", NULL, 0, NULL, &error),
		PG_READ_ONLY);
	STATUS(pg_source_import(source, "a", "input", PG_COMPRESS_AUTO,
		&error), PG_READ_ONLY);
	STATUS(pg_source_copy(source, "b", file, PG_COMPRESS_AUTO, &error),
		PG_READ_ONLY);
	STATUS(pg_file_delete(file, &error), PG_READ_ONLY);
	STATUS(pg_source_delete(source, &error), PG_READ_ONLY);
	CHECK(expect_bytes("root/a", payload, sizeof(payload)) == 0);
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	CHECK(fixture_close(&f) == 0);
	return 0;
}

static int delete_state(void)
{
	struct fixture f;
	pg_reader *reader = NULL;
	pg_file *file = NULL;
	pg_file_info info;
	pg_source_info source_info;
	pg_error error;

	CHECK(fixture_open(&f) == 0);
	STATUS(pg_source_delete(f.source, &error), PG_BUSY);
	STATUS(pg_tree_detach(f.tree, f.source, &error), PG_OK);
	STATUS(pg_reader_open(f.file, PG_READ_LOGICAL, &reader, &error), PG_OK);
	STATUS(pg_source_delete(f.source, &error), PG_BUSY);
	STATUS(pg_reader_close(&reader, &error), PG_OK);
	STATUS(pg_file_delete(f.file, &error), PG_OK);
	CHECK(expect_absent("root/a") == 0);
	STATUS(pg_file_inspect(f.file, &info, &error), PG_OK);
	CHECK(info.logical_size == sizeof(payload));
	STATUS(pg_file_delete(f.file, &error), PG_STALE);
	STATUS(pg_source_find(f.source, "a", &file, &error), PG_NOT_FOUND);
	CHECK(file == NULL);
	STATUS(pg_source_delete(f.source, &error), PG_OK);
	CHECK(expect_absent("root") == 0);
	STATUS(pg_source_inspect(f.source, &source_info, &error), PG_OK);
	CHECK(source_info.id == info.source_id);
	STATUS(pg_source_find(f.source, "empty", &file, &error), PG_STALE);
	CHECK(file == NULL);
	pg_cursor *cursor = NULL;

	STATUS(pg_source_files(f.source, NULL, &cursor, &error), PG_STALE);
	CHECK(cursor == NULL);
	STATUS(pg_source_request_subtree(f.source, NULL, &error), PG_STALE);
	STATUS(pg_source_rescan(f.source, &error), PG_STALE);
	STATUS(pg_source_validate(f.source, &error), PG_STALE);
	STATUS(pg_source_delete(f.source, &error), PG_STALE);
	STATUS(pg_file_inspect(f.file, &info, &error), PG_OK);
	CHECK(strcmp(info.canonical_name, "a") == 0);
	CHECK(fixture_close(&f) == 0);
	return 0;
}

static int unpack_state(void)
{
	struct fixture f;
	pg_unpack_target *target = NULL;
	pg_writer *writer = NULL;
	pg_file *file = NULL;
	pg_error error;
	size_t count;

	CHECK(fixture_open(&f) == 0);
	CHECK(clear_file("out/a") == 0);
	CHECK(clear_file("out/empty") == 0);
	STATUS(pg_unpack_target_open(f.cursor, "missing", 0, &target, &error),
		PG_NOT_FOUND);
	CHECK(target == NULL);
	STATUS(pg_unpack_target_open(f.cursor, "input", 0, &target, &error),
		PG_CONFLICT);
	CHECK(target == NULL);
	STATUS(pg_unpack_target_open(f.cursor, "out", 0, &target, &error),
		PG_OK);
	STATUS(pg_cursor_next(f.cursor, &file, &error), PG_OK);
	STATUS(pg_writer_open_unpack(target, file, &writer, &error), PG_OK);
	pg_unpack_target *original = target;

	STATUS(pg_unpack_target_close(&target, &error), PG_BUSY);
	CHECK(target == original);
	STATUS(pg_writer_write(writer, payload, sizeof(payload), &count,
		&error), PG_OK);
	CHECK(count == sizeof(payload));
	CHECK(expect_absent("out/a") == 0);
	STATUS(pg_writer_close(&writer, &error), PG_OK);
	CHECK(expect_absent("out/a") == 0);
	STATUS(pg_writer_open_unpack(target, file, &writer, &error), PG_OK);
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_cursor_close(&f.cursor, &error), PG_OK);
	STATUS(pg_writer_write(writer, payload, sizeof(payload), &count,
		&error), PG_OK);
	STATUS(pg_writer_finish(writer, &error), PG_OK);
	STATUS(pg_unpack_target_close(&target, &error), PG_BUSY);
	CHECK(target == original);
	STATUS(pg_writer_close(&writer, &error), PG_OK);
	STATUS(pg_unpack_target_close(&target, &error), PG_OK);
	CHECK(target == NULL);
	CHECK(expect_bytes("out/a", payload, sizeof(payload)) == 0);
	CHECK(fixture_close(&f) == 0);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	if (strcmp(argv[1], "native") == 0)
		return writer_state(0);
	if (strcmp(argv[1], "source") == 0)
		return writer_state(1);
	if (strcmp(argv[1], "file") == 0)
		return writer_state(2);
	if (strcmp(argv[1], "pigg") == 0)
		return writer_state(3);
	if (strcmp(argv[1], "hogg") == 0)
		return writer_state(4);
	if (strcmp(argv[1], "metadata_pigg") == 0)
		return builder_metadata(PG_PIGG2);
	if (strcmp(argv[1], "metadata_hogg") == 0)
		return builder_metadata(PG_HOGG10);
	if (strcmp(argv[1], "stale_pigg") == 0)
		return builder_stale(PG_PIGG2);
	if (strcmp(argv[1], "stale_hogg") == 0)
		return builder_stale(PG_HOGG10);
	if (strcmp(argv[1], "read_only") == 0)
		return read_only();
	if (strcmp(argv[1], "delete") == 0)
		return delete_state();
	if (strcmp(argv[1], "unpack") == 0)
		return unpack_state();
	return 2;
}
