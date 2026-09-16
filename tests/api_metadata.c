#include "api_test.h"

static int loose_case(void)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_file *file = NULL;
	pg_reader *reader = NULL;
	pg_source_options options = { PG_LOOSE, PG_WRITE, 0 };
	pg_metadata_options edit = { PG_METADATA_MTIME, 123, NULL, 0 };
	pg_error error;
	pg_file_info info;
	unsigned char bytes[32];
	size_t count;

	CHECK(!directory("root"));
	CHECK(!put_bytes("root/a", payload, sizeof(payload)));
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_source_open(context, "root", &options, &source,
		&error), PG_OK);
	STATUS(pg_source_find(source, "a", &file, &error), PG_OK);
	STATUS(pg_reader_open(file, PG_READ_LOGICAL, &reader, &error), PG_OK);
	STATUS(pg_file_update_metadata(file, &edit, &error), PG_OK);
	STATUS(pg_reader_seek(reader, 0, &error), PG_STALE);
	STATUS(pg_reader_close(&reader, &error), PG_OK);
	STATUS(pg_file_update_metadata(file, &edit, &error), PG_STALE);
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_source_find(source, "a", &file, &error), PG_OK);
	STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	CHECK(info.mtime == 123);
	STATUS(pg_file_read_all(file, bytes, sizeof(bytes), &count,
		&error), PG_OK);
	CHECK(count == sizeof(payload) && !memcmp(bytes, payload, count));
	STATUS(pg_file_update_metadata(file, &edit, &error), PG_OK);
	edit.fields = PG_METADATA_HEADER;
	STATUS(pg_file_update_metadata(file, &edit, &error), PG_UNSUPPORTED);
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

int main(int argc, char **argv)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_archive_builder *builder = NULL;
	pg_file *file = NULL;
	pg_reader *reader = NULL;
	pg_error error;
	pg_entry_options entry = { 0 };
	pg_metadata_options edit = { PG_METADATA_MTIME, 123, NULL, 0 };
	pg_source_options options = { PG_AUTO, PG_WRITE, 0 };
	pg_file_info before, after;
	unsigned char digest[16], old[128], current[128];
	struct stat initial, changed;
	size_t size, count;
	uint32_t format;

	CHECK(argc == 2);
	if (!strcmp(argv[1], "loose"))
		return loose_case();
	format = !strcmp(argv[1], "hogg") ? PG_HOGG10 : PG_PIGG2;
	CHECK(!clear_file("metadata"));
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_archive_builder_create(context, "metadata", format, 0,
		&builder, &error), PG_OK);
	entry.compression = PG_COMPRESS_FORCE;
	entry.cached_header = "header";
	entry.cached_header_size = 6;
	STATUS(pg_archive_builder_write_all(builder, "a", payload,
		sizeof(payload), &entry, &error), PG_OK);
	STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	STATUS(pg_source_open(context, "metadata", &options, &source,
		&error), PG_OK);
	STATUS(pg_source_find(source, "a", &file, &error), PG_OK);
	STATUS(pg_file_inspect(file, &before, &error), PG_OK);
	memcpy(digest, before.digest, sizeof(digest));
	STATUS(pg_reader_open(file, PG_READ_STORED, &reader, &error), PG_OK);
	STATUS(pg_reader_read(reader, old, sizeof(old), &size, &error), PG_OK);
	if (format == PG_PIGG2)
		STATUS(pg_reader_close(&reader, &error), PG_OK);
	STATUS(pg_file_update_metadata(NULL, &edit, &error), PG_INVALID);
	STATUS(pg_file_update_metadata(file, NULL, &error), PG_INVALID);
	edit.fields = 4;
	STATUS(pg_file_update_metadata(file, &edit, &error), PG_INVALID);
	edit.fields = PG_METADATA_MTIME;
	edit.mtime = INT64_MAX;
	STATUS(pg_file_update_metadata(file, &edit, &error), PG_LIMIT);
	edit.fields = 0;
	STATUS(pg_file_update_metadata(file, &edit, &error), PG_OK);
	STATUS(pg_file_verify(file, &error), PG_OK);
	edit.fields = PG_METADATA_MTIME;
	edit.mtime = 123;
	CHECK(!stat("metadata", &initial));
	STATUS(pg_file_update_metadata(file, &edit, &error), PG_OK);
	CHECK(!stat("metadata", &changed));
	if (format == PG_HOGG10) {
		CHECK(initial.st_size == changed.st_size);
		STATUS(pg_reader_seek(reader, 0, &error), PG_STALE);
		STATUS(pg_reader_close(&reader, &error), PG_OK);
	}
	STATUS(pg_file_update_metadata(file, &edit, &error), PG_STALE);
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_source_find(source, "a", &file, &error), PG_OK);
	STATUS(pg_file_inspect(file, &after, &error), PG_OK);
	CHECK(after.mtime == 123 && after.cached_header_size == 6);
	CHECK(!memcmp(digest, after.digest, sizeof(digest)));
	edit.fields = PG_METADATA_HEADER;
	STATUS(pg_file_update_metadata(file, &edit, &error), PG_OK);
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_source_find(source, "a", &file, &error), PG_OK);
	STATUS(pg_file_inspect(file, &after, &error), PG_OK);
	CHECK(after.mtime == 123 && after.cached_header_size == 0);
	CHECK(!memcmp(digest, after.digest, sizeof(digest)));
	STATUS(pg_reader_open(file, PG_READ_STORED, &reader, &error), PG_OK);
	STATUS(pg_reader_read(reader, current, sizeof(current), &count,
		&error), PG_OK);
	CHECK(size == count && !memcmp(old, current, size));
	STATUS(pg_reader_close(&reader, &error), PG_OK);
	STATUS(pg_file_verify(file, &error), PG_OK);
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_source_validate(source, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}
