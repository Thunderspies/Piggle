#include "../support.hpp"
#include <piggle/piggle.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib-ng.h>

namespace {

static int write_fixture(
	const char *name, const char *data, size_t size)
{
	FILE *stream = fopen(name, "wb");

	PG_CHECK(stream != NULL);
	PG_CHECK(fwrite(data, 1, size, stream) == size);
	PG_CHECK(fclose(stream) == 0);
	return 0;
}

static int test_options(void)
{
	pg_write_options options;
	size_t i;

	memset(&options, 0xa5, sizeof(options));
	pg_write_options_init(&options, 123);
	PG_CHECK(options.logical_size == 123);
	PG_CHECK(options.input_size == 123);
	PG_CHECK(options.encoding == PG_LOGICAL);
	PG_CHECK(options.entry.mtime == 0);
	PG_CHECK(options.entry.compression == PG_COMPRESS_AUTO);
	PG_CHECK(options.entry.digest_kind == PG_DIGEST_NONE);
	PG_CHECK(options.entry.original_name == NULL);
	PG_CHECK(options.entry.cached_header == NULL);
	PG_CHECK(options.entry.cached_header_size == 0);
	for (i = 0; i < sizeof(options.entry.expected_digest); i++)
		PG_CHECK(options.entry.expected_digest[i] == 0);
	return 0;
}

static int test_buffer(void)
{
	pg_buffer buffer = {NULL, 0};

	pg_buffer_free(&buffer);
	PG_CHECK(buffer.data == NULL && buffer.size == 0);
	return 0;
}

static int test_context(void)
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_writer *writer = NULL;
	pg_write_options options;
	pg_entry_options entry;
	pg_error error;
	size_t accepted = 0;

	remove("context.pigg");
	remove("context-empty.pigg");
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_CHECK(context != NULL);
	PG_STATUS(pg_archive_builder_create(context, "context.pigg",
				PG_PIGG2, 0, &builder, &error),
		PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_BUSY);
	PG_CHECK(context != NULL);
	pg_write_options_init(&options, 3);
	PG_STATUS(pg_writer_open_archive_builder(builder, "abort.txt",
				&options, &writer, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_finish(builder, &error), PG_BUSY);
	PG_STATUS(pg_archive_builder_close(&builder, &error), PG_BUSY);
	PG_CHECK(builder != NULL);
	PG_STATUS(pg_writer_write(writer, "abc", 3, &accepted, &error),
		PG_OK);
	PG_CHECK(accepted == 3);
	PG_STATUS(pg_writer_close(&writer, &error), PG_OK);
	PG_STATUS(pg_writer_open_archive_builder(builder, "abort.txt",
				&options, &writer, &error),
		PG_OK);
	PG_STATUS(pg_writer_close(&writer, &error), PG_OK);
	memset(&entry, 0, sizeof(entry));
	entry.digest_kind = PG_DIGEST_MD5;
	PG_STATUS(pg_archive_builder_write_all(builder, "digest.txt",
				"abc", 3, &entry, &error),
		PG_CHECKSUM);
	PG_STATUS(pg_archive_builder_write_all(builder, "digest.txt",
				"abc", 3, NULL, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_write_all(builder, "DIGEST.TXT",
				"abc", 3, NULL, &error),
		PG_EXISTS);
	PG_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	PG_CHECK(write_fixture("context-existing.pigg", "old", 3) == 0);
	PG_STATUS(pg_archive_builder_create(context,
				"context-existing.pigg", PG_PIGG2, 0,
				&builder, &error),
		PG_EXISTS);
	PG_CHECK(builder == NULL);
	PG_STATUS(pg_archive_builder_create(context,
				"context-existing.pigg", PG_PIGG2,
				PG_OVERWRITE, &builder, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	{
		char bytes[3];
		FILE *stream = fopen("context-existing.pigg", "rb");

		PG_CHECK(stream != NULL);
		PG_CHECK(fread(bytes, 1, sizeof(bytes), stream) ==
			sizeof(bytes));
		PG_CHECK(memcmp(bytes, "old", sizeof(bytes)) == 0);
		PG_CHECK(fclose(stream) == 0);
	}
	PG_CHECK(remove("context-existing.pigg") == 0);
	PG_STATUS(
		pg_archive_builder_create(context, "context-empty.pigg",
			PG_PIGG2, 0, &builder, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	PG_STATUS(
		pg_archive_builder_finish(builder, &error), PG_INVALID);
	PG_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	{
		unsigned char magic[4];
		FILE *stream = fopen("context-empty.pigg", "rb");

		PG_CHECK(stream != NULL);
		PG_CHECK(fread(magic, 1, sizeof(magic), stream) ==
			sizeof(magic));
		PG_CHECK(memcmp(magic, "\x23\x01\x00\x00",
					sizeof(magic)) == 0);
		PG_CHECK(fclose(stream) == 0);
	}
	PG_CHECK(remove("context-empty.pigg") == 0);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	PG_CHECK(context == NULL);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static int test_names(void)
{
	pg_context *context = NULL;
	pg_error error;
	char buffer[32];
	size_t required = 0;

	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	memset(buffer, 'x', sizeof(buffer));
	PG_STATUS(pg_name_normalize(context, "A//./B/", buffer, 3,
				&required, &error),
		PG_CAPACITY);
	PG_CHECK(required == 4);
	PG_CHECK(buffer[0] == 'x' && buffer[2] == 'x');
	PG_STATUS(pg_name_normalize(context, "A//./B/", buffer,
				sizeof(buffer), &required, &error),
		PG_OK);
	PG_CHECK(required == 4 && strcmp(buffer, "a/b") == 0);
	memset(buffer, 'x', sizeof(buffer));
	PG_STATUS(pg_name_normalize(context, "a/../b", buffer,
				sizeof(buffer), &required, &error),
		PG_INVALID);
	PG_CHECK(required == 0 && buffer[0] == 'x');
	PG_STATUS(pg_name_normalize(context, "a:b", buffer,
				sizeof(buffer), &required, &error),
		PG_INVALID);
	PG_CHECK(required == 0 && buffer[0] == 'x');
	PG_STATUS(pg_name_normalize(context,
				"Textures\\\xc3\x89T\xc3\x89.DDS", buffer,
				sizeof(buffer), &required, &error),
		PG_OK);
	PG_CHECK(strcmp(buffer, "textures/\xc3\x89t\xc3\x89.dds") == 0);
	PG_STATUS(pg_name_normalize(context, "/root/file", buffer,
				sizeof(buffer), &required, &error),
		PG_INVALID);
	PG_STATUS(pg_name_normalize(context, "", buffer, sizeof(buffer),
				&required, &error),
		PG_INVALID);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static int test_native_reader(void)
{
	pg_context *context = NULL;
	pg_reader *reader = NULL;
	pg_reader_info info;
	pg_error error;
	char buffer[5] = {};
	size_t bytes = 99;

	PG_CHECK(write_fixture("input.bin", "abcd", 4) == 0);
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(pg_reader_open_native(
				context, "input.bin", &reader, &error),
		PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_BUSY);
	PG_CHECK(context != NULL);
	PG_STATUS(pg_reader_inspect(reader, &info, &error), PG_OK);
	PG_CHECK(info.size == 4 && info.logical_size == 4);
	PG_CHECK(info.encoding == PG_LOGICAL);
	PG_STATUS(pg_reader_read(reader, buffer, 0, &bytes, &error),
		PG_OK);
	PG_CHECK(bytes == 0);
	PG_STATUS(pg_reader_read(reader, buffer, 2, &bytes, &error),
		PG_OK);
	PG_CHECK(bytes == 2 && memcmp(buffer, "ab", 2) == 0);
	PG_STATUS(pg_reader_read(reader, buffer + 2, 2, &bytes, &error),
		PG_OK);
	PG_CHECK(bytes == 2 && memcmp(buffer, "abcd", 4) == 0);
	PG_STATUS(pg_reader_read(reader, buffer, 1, &bytes, &error),
		PG_END);
	PG_CHECK(bytes == 0);
	PG_STATUS(pg_reader_close(&reader, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static int test_native_writer(void)
{
	static const unsigned char compressed[] = {0x78, 0x9c, 0x4b,
		0x4c, 0x4a, 0x06, 0x00, 0x02, 0x4d, 0x01, 0x27};
	static const unsigned char digest[] = {0x90, 0x01, 0x50, 0x98,
		0x3c, 0xd2, 0x4f, 0xb0, 0xd6, 0x96, 0x3f, 0x7d, 0x28,
		0xe1, 0x7f, 0x72};
	pg_context *context = NULL;
	pg_writer *writer = NULL;
	pg_reader *reader = NULL;
	pg_write_options options;
	pg_error error;
	char buffer[4] = {};
	size_t bytes = 99;

	remove("output.bin");
	remove("encoded.bin");
	remove("bad.bin");
	remove("trailing.bin");
	remove("short.bin");
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	pg_write_options_init(&options, 3);
	PG_STATUS(pg_writer_open_native(context, "output.bin", &options,
				0, &writer, &error),
		PG_OK);
	PG_STATUS(pg_writer_write(writer, "ab", 2, &bytes, &error),
		PG_OK);
	PG_CHECK(bytes == 2);
	PG_STATUS(
		pg_writer_write(writer, "c", 1, &bytes, &error), PG_OK);
	PG_CHECK(bytes == 1);
	PG_STATUS(pg_writer_finish(writer, &error), PG_OK);

	PG_STATUS(pg_writer_close(&writer, &error), PG_OK);
	PG_STATUS(pg_reader_open_native(
				context, "output.bin", &reader, &error),
		PG_OK);
	pg_reader_info reader_info;

	PG_STATUS(
		pg_reader_inspect(reader, &reader_info, &error), PG_OK);
	PG_CHECK(reader_info.mtime == 0);
	PG_STATUS(pg_reader_read(reader, buffer, 3, &bytes, &error),
		PG_OK);
	PG_CHECK(bytes == 3 && memcmp(buffer, "abc", 3) == 0);
	PG_STATUS(pg_reader_close(&reader, &error), PG_OK);
	PG_STATUS(pg_writer_open_native(context, "output.bin", &options,
				0, &writer, &error),
		PG_EXISTS);
	PG_CHECK(writer == NULL);
	PG_STATUS(pg_writer_open_native(context, "nested/output.bin",
				&options, 0, &writer, &error),
		PG_OK);
	PG_STATUS(pg_writer_close(&writer, &error), PG_OK);
	PG_STATUS(pg_reader_open_native(context, "nested/output.bin",
				&reader, &error),
		PG_NOT_FOUND);
	options.entry.mtime = 19;
	PG_STATUS(pg_writer_open_native(context, "output.bin", &options,
				PG_OVERWRITE, &writer, &error),
		PG_OK);
	PG_STATUS(pg_writer_write(writer, "xyz", 3, &bytes, &error),
		PG_OK);
	PG_STATUS(pg_writer_finish(writer, &error), PG_OK);
	PG_STATUS(pg_writer_close(&writer, &error), PG_OK);
	PG_STATUS(pg_reader_open_native(
				context, "output.bin", &reader, &error),
		PG_OK);
	PG_STATUS(
		pg_reader_inspect(reader, &reader_info, &error), PG_OK);
	PG_CHECK(reader_info.mtime == 19);
	PG_STATUS(pg_reader_read(reader, buffer, 3, &bytes, &error),
		PG_OK);
	PG_CHECK(bytes == 3 && memcmp(buffer, "xyz", 3) == 0);
	PG_STATUS(pg_reader_close(&reader, &error), PG_OK);
	pg_write_options_init(&options, 3);
	options.encoding = PG_ZLIB;
	options.input_size = sizeof(compressed);
	options.entry.digest_kind = PG_DIGEST_MD5;
	memcpy(options.entry.expected_digest, digest, sizeof(digest));
	PG_STATUS(pg_writer_open_native(context, "encoded.bin",
				&options, 0, &writer, &error),
		PG_OK);
	PG_STATUS(pg_writer_write(writer, compressed,
				sizeof(compressed), &bytes, &error),
		PG_OK);
	PG_STATUS(pg_writer_finish(writer, &error), PG_OK);
	PG_STATUS(pg_writer_close(&writer, &error), PG_OK);
	PG_STATUS(pg_reader_open_native(
				context, "encoded.bin", &reader, &error),
		PG_OK);
	PG_STATUS(pg_reader_read(reader, buffer, 3, &bytes, &error),
		PG_OK);
	PG_CHECK(bytes == 3 && memcmp(buffer, "abc", 3) == 0);
	PG_STATUS(pg_reader_close(&reader, &error), PG_OK);
	options.entry.expected_digest[0] ^= 1;
	PG_STATUS(pg_writer_open_native(context, "bad.bin", &options, 0,
				&writer, &error),
		PG_OK);
	PG_STATUS(pg_writer_write(writer, compressed,
				sizeof(compressed), &bytes, &error),
		PG_OK);
	PG_STATUS(pg_writer_finish(writer, &error), PG_CHECKSUM);

	PG_STATUS(pg_writer_close(&writer, &error), PG_OK);
	options.entry.expected_digest[0] ^= 1;
	options.input_size++;
	PG_STATUS(pg_writer_open_native(context, "trailing.bin",
				&options, 0, &writer, &error),
		PG_OK);
	PG_STATUS(pg_writer_write(writer, compressed,
				sizeof(compressed), &bytes, &error),
		PG_OK);
	PG_STATUS(
		pg_writer_write(writer, "x", 1, &bytes, &error), PG_OK);
	PG_STATUS(pg_writer_finish(writer, &error), PG_CORRUPT);
	PG_STATUS(pg_writer_close(&writer, &error), PG_OK);
	options.input_size = sizeof(compressed) - 1;
	PG_STATUS(pg_writer_open_native(context, "short.bin", &options,
				0, &writer, &error),
		PG_OK);
	PG_STATUS(pg_writer_write(writer, compressed,
				options.input_size, &bytes, &error),
		PG_OK);
	PG_STATUS(pg_writer_finish(writer, &error), PG_CORRUPT);
	PG_STATUS(pg_writer_close(&writer, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static int test_source(void)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_file *file = NULL;
	pg_file *again = NULL;
	pg_writer *writer = NULL;
	pg_write_options options;
	pg_cursor *cursor = NULL;
	pg_file_info info;
	pg_buffer buffer = {NULL, 0};
	pg_error error;
	char bytes[4] = {};
	size_t count = 99;

	PG_CHECK(write_fixture("data.bin", "abc", 3) == 0);
	PG_CHECK(write_fixture("empty", "", 0) == 0);
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(pg_source_open(context, ".", NULL, &source, &error),
		PG_OK);
	pg_write_options_init(&options, 3);
	PG_STATUS(pg_writer_open_native(context, "alias.bin", &options,
				0, &writer, &error),
		PG_CONFLICT);
	PG_CHECK(writer == NULL);
	PG_STATUS(pg_source_find(source, "DATA.BIN", &file, &error),
		PG_OK);
	PG_STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	PG_CHECK(strcmp(info.canonical_name, "data.bin") == 0);
	PG_CHECK(info.logical_size == 3);
	pg_id copy_id = info.copy_id;
	PG_STATUS(pg_source_find(source, "data.bin", &again, &error),
		PG_OK);
	PG_STATUS(pg_file_inspect(again, &info, &error), PG_OK);
	PG_CHECK(info.copy_id == copy_id);
	PG_STATUS(pg_file_close(&again, &error), PG_OK);
	PG_STATUS(pg_source_read_all(
				source, "DATA.BIN", bytes, 2, &count, &error),
		PG_CAPACITY);
	PG_CHECK(count == 0 && bytes[0] == 0);
	PG_STATUS(pg_source_read_all(source, "DATA.BIN", bytes,
				sizeof(bytes), &count, &error),
		PG_OK);
	PG_CHECK(count == 3 && memcmp(bytes, "abc", 3) == 0);
	PG_STATUS(pg_source_read_all_alloc(
				source, "DATA.BIN", 2, &buffer, &error),
		PG_LIMIT);
	PG_CHECK(buffer.data == NULL && buffer.size == 0);
	PG_STATUS(pg_source_read_all_alloc(
				source, "DATA.BIN", 3, &buffer, &error),
		PG_OK);
	PG_CHECK(
		buffer.size == 3 && memcmp(buffer.data, "abc", 3) == 0);
	pg_buffer_free(&buffer);
	PG_CHECK(buffer.data == NULL && buffer.size == 0);
	PG_STATUS(pg_source_read_all_alloc(
				source, "empty", 0, &buffer, &error),
		PG_OK);
	PG_CHECK(buffer.data == NULL && buffer.size == 0);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(
		pg_source_request_subtree(source, NULL, &error), PG_OK);
	PG_STATUS(
		pg_source_files(source, NULL, &cursor, &error), PG_OK);
	PG_STATUS(pg_cursor_next(cursor, &file, &error), PG_OK);
	PG_STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	PG_CHECK(strcmp(info.canonical_name, "data.bin") == 0);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_cursor_next(cursor, &file, &error), PG_OK);
	PG_STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	PG_CHECK(strcmp(info.canonical_name, "empty") == 0);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_cursor_next(cursor, &file, &error), PG_END);
	PG_CHECK(file == NULL);
	PG_STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	PG_STATUS(pg_source_files(source, "data.bin", &cursor, &error),
		PG_CONFLICT);
	PG_CHECK(cursor == NULL);
	PG_STATUS(pg_source_files(source, "missing", &cursor, &error),
		PG_OK);
	PG_STATUS(pg_cursor_next(cursor, &file, &error), PG_END);
	PG_STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	PG_CHECK(write_fixture("data.bin", "xyz123", 6) == 0);
	PG_STATUS(
		pg_source_request_subtree(source, NULL, &error), PG_OK);
	PG_STATUS(pg_source_find(source, "data.bin", &file, &error),
		PG_OK);
	PG_STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	PG_CHECK(info.copy_id == copy_id);
	PG_CHECK(info.copy_generation == 2);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static int test_tree(void)
{
	pg_context *context = NULL;
	pg_tree *tree = NULL;
	pg_source *tree_source = NULL;
	pg_tree_info info;
	pg_file *file = NULL;
	pg_error error;

	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(pg_tree_create(context, &tree, &error), PG_OK);
	PG_STATUS(pg_tree_inspect(tree, &info, &error), PG_OK);
	PG_CHECK(info.source_count == 0 &&
		info.watch_mode == PG_WATCH_OFF);
	PG_STATUS(
		pg_tree_source(tree, 0, &tree_source, &error), PG_END);
	PG_CHECK(tree_source == NULL);
	PG_STATUS(pg_tree_find(tree, "missing", &file, &error),
		PG_NOT_FOUND);
	PG_CHECK(file == NULL);
	PG_STATUS(pg_tree_watch(tree, PG_WATCH_SCAN, &error), PG_OK);
	PG_STATUS(pg_tree_watch(tree, PG_WATCH_SCAN, &error), PG_OK);
	PG_STATUS(pg_tree_inspect(tree, &info, &error), PG_OK);
	PG_CHECK(info.watch_mode == PG_WATCH_SCAN);
	PG_STATUS(pg_tree_unwatch(tree, &error), PG_OK);
	PG_STATUS(pg_tree_watch(tree, PG_WATCH_NATIVE, &error), PG_OK);
	PG_STATUS(pg_tree_inspect(tree, &info, &error), PG_OK);
	PG_CHECK(info.watch_mode == PG_WATCH_NATIVE);
	PG_STATUS(pg_tree_unwatch(tree, &error), PG_OK);
	{
		pg_observer observer = {NULL, NULL};

		PG_STATUS(pg_tree_poll(tree, &observer, &error),
			PG_INVALID);
	}
	PG_STATUS(pg_tree_close(&tree, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static int test_builder(void)
{
	static char large[131073];
	static const unsigned char compressed[] = {0x78, 0x9c, 0x4b,
		0x4c, 0x4a, 0x06, 0x00, 0x02, 0x4d, 0x01, 0x27};
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_writer *writer = NULL;
	pg_source *source = NULL;
	pg_file *file = NULL;
	pg_file *again = NULL;
	pg_reader *reader = NULL;
	pg_file_info info;
	pg_write_options options;
	pg_entry_options entry;
	pg_error error;
	char bytes[4] = {};
	char long_bytes[16] = {};
	size_t count = 0;
	size_t written = 0;

	remove("test.pigg");
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(pg_archive_builder_create(context, "test.pigg",
				PG_PIGG2, 0, &builder, &error),
		PG_OK);
	{
		FILE *stream = fopen("test.pigg", "rb");

		PG_CHECK(stream == NULL);
	}
	PG_STATUS(pg_archive_builder_write_all(
				builder, "A.TXT", "abc", 3, NULL, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_write_all(
				builder, "a.txt", "abc", 3, NULL, &error),
		PG_EXISTS);
	pg_write_options_init(&options, 3);
	options.encoding = PG_ZLIB;
	options.input_size = sizeof(compressed);
	PG_STATUS(pg_writer_open_archive_builder(
				builder, "b.txt", &options, &writer, &error),
		PG_OK);
	PG_STATUS(pg_writer_write(writer, compressed,
				sizeof(compressed), &written, &error),
		PG_OK);
	PG_STATUS(pg_writer_finish(writer, &error), PG_OK);
	PG_STATUS(pg_writer_close(&writer, &error), PG_OK);
	memset(large, 'x', sizeof(large));
	PG_STATUS(pg_archive_builder_write_all(builder, "large.bin",
				large, sizeof(large), NULL, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_finish(builder, &error), PG_OK);

	PG_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	PG_STATUS(pg_source_open(
				context, "test.pigg", NULL, &source, &error),
		PG_OK);
	pg_write_options_init(&options, 3);
	PG_STATUS(pg_writer_open_native(context, "test.pigg", &options,
				PG_OVERWRITE, &writer, &error),
		PG_CONFLICT);
	PG_CHECK(writer == NULL);
	PG_STATUS(pg_source_read_all(source, "a.txt", bytes,
				sizeof(bytes), &count, &error),
		PG_OK);
	PG_CHECK(count == 3 && memcmp(bytes, "abc", 3) == 0);
	PG_STATUS(pg_source_read_all(source, "b.txt", bytes,
				sizeof(bytes), &count, &error),
		PG_OK);
	PG_CHECK(count == 3 && memcmp(bytes, "abc", 3) == 0);
	PG_STATUS(pg_source_find(source, "large.bin", &file, &error),
		PG_OK);
	PG_STATUS(pg_file_verify(file, &error), PG_OK);
	PG_STATUS(pg_source_validate(source, &error), PG_OK);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(
		pg_source_find(source, "a.txt", &again, &error), PG_OK);
	PG_STATUS(pg_file_inspect(again, &info, &error), PG_OK);
	pg_id old_id = info.copy_id;
	{
		const unsigned char timestamp[] = {7, 0, 0, 0};
		FILE *stream = fopen("test.pigg", "r+b");

		PG_CHECK(stream != NULL);
		PG_CHECK(fseek(stream, 28, SEEK_SET) == 0);
		PG_CHECK(fwrite(timestamp, 1, sizeof(timestamp),
					stream) == sizeof(timestamp));
		PG_CHECK(fclose(stream) == 0);
	}
	PG_STATUS(pg_source_rescan(source, &error), PG_OK);
	PG_STATUS(
		pg_reader_open(again, PG_READ_LOGICAL, &reader, &error),
		PG_STALE);
	PG_CHECK(reader == NULL);
	PG_STATUS(pg_file_inspect(again, &info, &error), PG_OK);
	PG_CHECK(info.mtime == 0);
	PG_STATUS(
		pg_source_find(source, "a.txt", &file, &error), PG_OK);
	PG_STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	PG_CHECK(info.mtime == 7);
	PG_CHECK(info.copy_id == old_id);
	PG_CHECK(info.copy_generation == 2);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_file_close(&again, &error), PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	remove("test.hogg");
	PG_STATUS(pg_archive_builder_create(context, "test.hogg",
				PG_HOGG10, 0, &builder, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_write_all(
				builder, "A.TXT", "abc", 3, NULL, &error),
		PG_OK);
	memset(&entry, 0, sizeof(entry));
	entry.compression = PG_COMPRESS_FORCE;
	entry.cached_header = "head";
	entry.cached_header_size = 4;
	PG_STATUS(pg_archive_builder_write_all(builder, "b.txt",
				"aaaaaaaaaaaaaaaa", 16, &entry, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_finish(builder, &error), PG_OK);

	PG_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	{
		unsigned char header[24];
		FILE *stream = fopen("test.hogg", "rb");

		PG_CHECK(stream != NULL);
		PG_CHECK(fread(header, 1, sizeof(header), stream) ==
			sizeof(header));
		PG_CHECK(fclose(stream) == 0);
		PG_CHECK(header[0] == 0x0d && header[1] == 0xf0);
		PG_CHECK(header[2] == 0xad && header[3] == 0xde);
		PG_CHECK(header[4] == 10 && header[5] == 0);
		PG_CHECK(header[6] == 0 && header[7] == 4);
		PG_CHECK(header[16] == 2 && header[17] == 0);
	}
	PG_STATUS(pg_source_open(
				context, "test.hogg", NULL, &source, &error),
		PG_OK);
	PG_STATUS(pg_source_read_all(source, "a.txt", bytes,
				sizeof(bytes), &count, &error),
		PG_OK);
	PG_CHECK(count == 3 && memcmp(bytes, "abc", 3) == 0);
	PG_STATUS(
		pg_source_find(source, "b.txt", &file, &error), PG_OK);
	PG_STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	PG_CHECK(info.encoding == PG_ZLIB);
	pg_id archived_id = info.copy_id;
	PG_STATUS(
		pg_source_find(source, "b.txt", &again, &error), PG_OK);
	PG_STATUS(pg_file_inspect(again, &info, &error), PG_OK);
	PG_CHECK(info.copy_id == archived_id);
	PG_STATUS(pg_file_close(&again, &error), PG_OK);
	PG_STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	PG_CHECK(info.digest_kind == PG_DIGEST_MD5_32);
	PG_CHECK(info.cached_header_size == 4);
	PG_CHECK(memcmp(info.cached_header, "head", 4) == 0);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_source_read_all(source, "b.txt", long_bytes,
				sizeof(long_bytes), &count, &error),
		PG_OK);
	PG_CHECK(count == 16);
	PG_CHECK(memcmp(long_bytes, "aaaaaaaaaaaaaaaa", 16) == 0);
	PG_STATUS(pg_source_validate(source, &error), PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(
		pg_source_recover(context, "test.hogg", &error), PG_OK);

	{
		static const unsigned char deletion[] = {12, 0, 0, 0, 1,
			0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0x05, 0xac,
			0xab, 0xde};
		FILE *stream = fopen("test.hogg", "r+b");

		PG_CHECK(stream != NULL);
		PG_CHECK(fseek(stream, 24, SEEK_SET) == 0);
		PG_CHECK(fwrite(deletion, 1, sizeof(deletion),
					stream) == sizeof(deletion));
		PG_CHECK(fclose(stream) == 0);
	}
	PG_STATUS(pg_source_open(
				context, "test.hogg", NULL, &source, &error),
		PG_RECOVERY_REQUIRED);
	PG_CHECK(source == NULL);
	PG_STATUS(
		pg_source_recover(context, "test.hogg", &error), PG_OK);

	PG_STATUS(pg_source_open(
				context, "test.hogg", NULL, &source, &error),
		PG_OK);
	PG_STATUS(pg_source_find(source, "b.txt", &file, &error),
		PG_NOT_FOUND);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	{
		unsigned char record[32];
		unsigned char update[64] = {};
		FILE *stream = fopen("test.hogg", "r+b");

		PG_CHECK(stream != NULL);
		PG_CHECK(fseek(stream, 4144, SEEK_SET) == 0);
		PG_CHECK(fread(record, 1, sizeof(record), stream) ==
			sizeof(record));
		update[0] = 56;
		update[4] = 3;
		update[16] = 3;
		update[20] = 42;
		memcpy(update + 28, record + 24, 8);
		memcpy(update + 36, record + 16, 4);
		memcpy(update + 44, record, 8);
		update[60] = 0x05;
		update[61] = 0xac;
		update[62] = 0xab;
		update[63] = 0xde;
		PG_CHECK(fseek(stream, 24, SEEK_SET) == 0);
		PG_CHECK(fwrite(update, 1, sizeof(update), stream) ==
			sizeof(update));
		PG_CHECK(fclose(stream) == 0);
	}
	PG_STATUS(
		pg_source_recover(context, "test.hogg", &error), PG_OK);

	PG_STATUS(pg_source_open(
				context, "test.hogg", NULL, &source, &error),
		PG_OK);
	PG_STATUS(
		pg_source_find(source, "a.txt", &file, &error), PG_OK);
	PG_STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	PG_CHECK(info.mtime == 42);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_source_open(
				context, "test.hogg", NULL, &source, &error),
		PG_OK);
	PG_STATUS(
		pg_source_find(source, "a.txt", &file, &error), PG_OK);
	PG_STATUS(
		pg_reader_open(file, PG_READ_LOGICAL, &reader, &error),
		PG_OK);
	{
		unsigned char flush[24] = {};
		FILE *stream = fopen("test.hogg", "r+b");

		PG_CHECK(stream != NULL);
		flush[0] = 16;
		flush[4] = 6;
		flush[12] = 0x18;
		flush[13] = 0x04;
		flush[20] = 0x05;
		flush[21] = 0xac;
		flush[22] = 0xab;
		flush[23] = 0xde;
		PG_CHECK(fseek(stream, 24, SEEK_SET) == 0);
		PG_CHECK(fwrite(flush, 1, sizeof(flush), stream) ==
			sizeof(flush));
		PG_CHECK(fclose(stream) == 0);
	}
	PG_STATUS(
		pg_source_recover(context, "test.hogg", &error), PG_OK);
	PG_STATUS(pg_reader_seek(reader, 0, &error), PG_STALE);

	PG_STATUS(pg_reader_close(&reader, &error), PG_OK);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(
		pg_source_recover(context, "test.hogg", &error), PG_OK);

	PG_STATUS(
		pg_source_find(source, "a.txt", &file, &error), PG_OK);
	PG_STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	PG_CHECK(info.mtime == 42);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static int test_pigg_edit(void)
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_source *source = NULL;
	pg_file *old = NULL;
	pg_file *file = NULL;
	pg_writer *writer = NULL;
	pg_reader *reader = NULL;
	pg_error error;
	pg_file_info info;
	pg_source_options source_options = {PG_PIGG2, PG_WRITE, 0};
	pg_write_options options;
	pg_entry_options entry;
	char data[8] = {};
	char stored_before[64] = {};
	char stored_after[64] = {};
	size_t stored_size = 0;
	size_t count = 0;
	size_t written = 0;

	remove("edit.pigg");
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(pg_archive_builder_create(context, "edit.pigg",
				PG_PIGG2, 0, &builder, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_write_all(
				builder, "A.TXT", "abc", 3, NULL, &error),
		PG_OK);
	memset(&entry, 0, sizeof(entry));
	entry.compression = PG_COMPRESS_FORCE;
	entry.mtime = 19;
	entry.cached_header = "head";
	entry.cached_header_size = 4;
	PG_STATUS(pg_archive_builder_write_all(
				builder, "b.txt", "bbb", 3, &entry, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	PG_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	PG_STATUS(pg_source_open(context, "edit.pigg", &source_options,
				&source, &error),
		PG_OK);
	PG_STATUS(pg_source_find(source, "a.txt", &old, &error), PG_OK);
	PG_STATUS(
		pg_source_find(source, "b.txt", &file, &error), PG_OK);
	PG_STATUS(pg_reader_open(file, PG_READ_STORED, &reader, &error),
		PG_OK);
	PG_STATUS(pg_reader_read(reader, stored_before,
				sizeof(stored_before), &stored_size, &error),
		PG_OK);
	PG_STATUS(pg_reader_close(&reader, &error), PG_OK);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_source_write_all(
				source, "a.txt", "new", 3, NULL, &error),
		PG_OK);

	PG_STATUS(pg_file_inspect(old, &info, &error), PG_OK);
	PG_CHECK(strcmp(info.original_name, "A.TXT") == 0);
	PG_STATUS(pg_reader_open(old, PG_READ_LOGICAL, &reader, &error),
		PG_STALE);
	PG_CHECK(reader == NULL);
	PG_STATUS(
		pg_source_find(source, "a.txt", &file, &error), PG_OK);
	PG_STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	PG_CHECK(strcmp(info.original_name, "A.TXT") == 0 &&
		info.archive_record == 0);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_source_read_all(source, "a.txt", data,
				sizeof(data), &count, &error),
		PG_OK);
	PG_CHECK(count == 3 && memcmp(data, "new", 3) == 0);
	memset(&entry, 0, sizeof(entry));
	entry.digest_kind = PG_DIGEST_MD5;
	PG_STATUS(pg_source_write_all(
				source, "a.txt", "bad", 3, &entry, &error),
		PG_CHECKSUM);

	PG_STATUS(pg_source_read_all(source, "a.txt", data,
				sizeof(data), &count, &error),
		PG_OK);
	PG_CHECK(count == 3 && memcmp(data, "new", 3) == 0);
	PG_STATUS(pg_reader_open_source(source, "b.txt",
		PG_READ_LOGICAL_PAYLOAD, &reader, &error), PG_OK);
	PG_STATUS(pg_reader_read(reader, data, sizeof(data), &count, &error),
		PG_OK);
	PG_STATUS(pg_reader_close(&reader, &error), PG_OK);
	PG_CHECK(count == 3 && memcmp(data, "bbb", 3) == 0);
	PG_STATUS(
		pg_source_find(source, "b.txt", &file, &error), PG_OK);
	PG_STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	PG_CHECK(info.encoding == PG_ZLIB && info.mtime == 19 &&
		info.archive_record == 1 &&
		info.cached_header_size == 4 &&
		memcmp(info.cached_header, "head", 4) == 0);
	PG_STATUS(pg_reader_open(file, PG_READ_STORED, &reader, &error),
		PG_OK);
	PG_STATUS(pg_reader_read(reader, stored_after,
				sizeof(stored_after), &count, &error),
		PG_OK);
	PG_CHECK(count == stored_size &&
		memcmp(stored_before, stored_after, count) == 0);
	PG_STATUS(pg_reader_close(&reader, &error), PG_OK);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	pg_write_options_init(&options, 3);
	PG_STATUS(pg_writer_open_source(
				source, "a.txt", &options, &writer, &error),
		PG_OK);
	PG_STATUS(pg_writer_write(writer, "bad", 3, &written, &error),
		PG_OK);
	PG_STATUS(pg_writer_close(&writer, &error), PG_OK);
	PG_STATUS(pg_source_read_all(source, "a.txt", data,
				sizeof(data), &count, &error),
		PG_OK);
	PG_CHECK(count == 3 && memcmp(data, "new", 3) == 0);
	pg_write_options_init(&options, 3);
	PG_STATUS(pg_writer_open_source(
				source, "c.txt", &options, &writer, &error),
		PG_OK);
	PG_STATUS(pg_writer_write(writer, "ccc", 3, &written, &error),
		PG_OK);
	PG_CHECK(written == 3);
	PG_STATUS(pg_writer_finish(writer, &error), PG_OK);
	PG_STATUS(pg_writer_close(&writer, &error), PG_OK);
	PG_STATUS(
		pg_source_find(source, "b.txt", &file, &error), PG_OK);
	PG_STATUS(pg_file_delete(file, &error), PG_OK);

	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_source_find(source, "b.txt", &file, &error),
		PG_NOT_FOUND);
	PG_STATUS(pg_source_read_all(source, "c.txt", data,
				sizeof(data), &count, &error),
		PG_OK);
	PG_CHECK(count == 3 && memcmp(data, "ccc", 3) == 0);
	PG_STATUS(pg_file_close(&old, &error), PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static int test_hogg_delete(void)
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_source *source = NULL;
	pg_file *file = NULL;
	pg_error error;
	pg_source_options options = {PG_HOGG10, PG_WRITE, 0};
	char data[8] = {};
	size_t count = 0;

	remove("delete.hogg");
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(pg_archive_builder_create(context, "delete.hogg",
				PG_HOGG10, 0, &builder, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_write_all(
				builder, "a.txt", "aaa", 3, NULL, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_write_all(
				builder, "b.txt", "bbb", 3, NULL, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	PG_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	PG_STATUS(pg_source_open(context, "delete.hogg", &options,
				&source, &error),
		PG_OK);
	PG_STATUS(
		pg_source_find(source, "a.txt", &file, &error), PG_OK);
	PG_STATUS(pg_file_delete(file, &error), PG_OK);

	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_source_find(source, "a.txt", &file, &error),
		PG_NOT_FOUND);
	PG_STATUS(pg_source_recover(context, "delete.hogg", &error),
		PG_OK);

	PG_STATUS(pg_source_read_all(source, "b.txt", data,
				sizeof(data), &count, &error),
		PG_OK);
	PG_CHECK(count == 3 && memcmp(data, "bbb", 3) == 0);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_source_open(context, "delete.hogg", NULL, &source,
				&error),
		PG_OK);
	PG_STATUS(pg_source_find(source, "a.txt", &file, &error),
		PG_NOT_FOUND);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static int test_hogg_update(void)
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_source *source = NULL;
	pg_file *old = NULL;
	pg_file *file = NULL;
	pg_reader *reader = NULL;
	pg_error error;
	pg_file_info info;
	pg_source_options options = {PG_HOGG10, PG_WRITE, 0};
	pg_entry_options entry;
	char data[8] = {};
	size_t count = 0;

	remove("update.hogg");
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(pg_archive_builder_create(context, "update.hogg",
				PG_HOGG10, 0, &builder, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_write_all(
				builder, "A.TXT", "aaa", 3, NULL, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_write_all(
				builder, "b.txt", "bbb", 3, NULL, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	PG_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	PG_STATUS(pg_source_open(context, "update.hogg", &options,
				&source, &error),
		PG_OK);
	PG_STATUS(pg_source_find(source, "a.txt", &old, &error), PG_OK);
	PG_STATUS(pg_source_write_all(
				source, "a.txt", "new", 3, NULL, &error),
		PG_OK);

	PG_STATUS(pg_reader_open(old, PG_READ_LOGICAL, &reader, &error),
		PG_STALE);
	PG_CHECK(reader == NULL);
	PG_STATUS(
		pg_source_find(source, "a.txt", &file, &error), PG_OK);
	PG_STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	PG_CHECK(strcmp(info.original_name, "A.TXT") == 0 &&
		info.archive_record == 0);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_source_read_all(source, "a.txt", data,
				sizeof(data), &count, &error),
		PG_OK);
	PG_CHECK(count == 3 && memcmp(data, "new", 3) == 0);
	memset(&entry, 0, sizeof(entry));
	entry.digest_kind = PG_DIGEST_MD5;
	PG_STATUS(pg_source_write_all(
				source, "a.txt", "bad", 3, &entry, &error),
		PG_CHECKSUM);

	PG_STATUS(pg_source_read_all(source, "a.txt", data,
				sizeof(data), &count, &error),
		PG_OK);
	PG_CHECK(count == 3 && memcmp(data, "new", 3) == 0);
	PG_STATUS(pg_source_read_all(source, "b.txt", data,
				sizeof(data), &count, &error),
		PG_OK);
	PG_CHECK(count == 3 && memcmp(data, "bbb", 3) == 0);
	memset(&entry, 0, sizeof(entry));
	entry.cached_header = "head";
	entry.cached_header_size = 4;
	entry.compression = PG_COMPRESS_FORCE;
	PG_STATUS(pg_source_write_all(
				source, "c.txt", "ccc", 3, &entry, &error),
		PG_OK);

	PG_STATUS(
		pg_source_find(source, "c.txt", &file, &error), PG_OK);
	PG_STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	PG_CHECK(info.encoding == PG_ZLIB &&
		info.cached_header_size == 4 &&
		memcmp(info.cached_header, "head", 4) == 0);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_reader_open_source(source, "c.txt",
		PG_READ_LOGICAL_PAYLOAD, &reader, &error), PG_OK);
	PG_STATUS(pg_reader_read(reader, data, sizeof(data), &count, &error),
		PG_OK);
	PG_STATUS(pg_reader_close(&reader, &error), PG_OK);
	PG_CHECK(count == 3 && memcmp(data, "ccc", 3) == 0);
	entry.cached_header = "next";
	PG_STATUS(pg_source_write_all(
				source, "c.txt", "cee", 3, &entry, &error),
		PG_OK);
	PG_STATUS(
		pg_source_find(source, "c.txt", &file, &error), PG_OK);
	PG_STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	PG_CHECK(info.cached_header_size == 4 &&
		memcmp(info.cached_header, "next", 4) == 0);
	PG_STATUS(pg_file_delete(file, &error), PG_OK);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_source_write_all(
				source, "d.txt", "ddd", 3, NULL, &error),
		PG_OK);
	PG_STATUS(pg_source_read_all(source, "d.txt", data,
				sizeof(data), &count, &error),
		PG_OK);
	PG_CHECK(count == 3 && memcmp(data, "ddd", 3) == 0);
	PG_STATUS(pg_file_close(&old, &error), PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	{
		const unsigned char inuse[] = {1, 0, 0, 0};
		FILE *stream = fopen("update.hogg", "r+b");
		PG_CHECK(stream != NULL);
		PG_CHECK(fseek(stream, 1048, SEEK_SET) == 0);
		PG_CHECK(fwrite(inuse, 1, sizeof(inuse), stream) ==
			sizeof(inuse));
		PG_CHECK(fclose(stream) == 0);
	}
	PG_STATUS(pg_source_recover(context, "update.hogg", &error),
		PG_OK);

	PG_STATUS(pg_source_open(context, "update.hogg", NULL, &source,
				&error),
		PG_OK);
	PG_STATUS(pg_source_read_all(source, "d.txt", data,
				sizeof(data), &count, &error),
		PG_OK);
	PG_CHECK(count == 3 && memcmp(data, "ddd", 3) == 0);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static int test_hogg_flush(void)
{
	static char header[2000];
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_source *source = NULL;
	pg_file *file = NULL;
	pg_reader *reader = NULL;
	pg_file_info info;
	pg_error error;
	pg_entry_options entry;
	pg_source_options options = {PG_HOGG10, PG_WRITE, 0};
	char data[8] = {};
	size_t count = 0;

	remove("flush.hogg");
	memset(header, 'h', sizeof(header));
	memset(&entry, 0, sizeof(entry));
	entry.cached_header = header;
	entry.cached_header_size = sizeof(header);
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(pg_archive_builder_create(context, "flush.hogg",
				PG_HOGG10, 0, &builder, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_write_all(
				builder, "a.txt", "aaa", 3, NULL, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	PG_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	PG_STATUS(pg_source_open(context, "flush.hogg", &options,
				&source, &error),
		PG_OK);
	PG_STATUS(pg_source_write_all(
				source, "a.txt", "one", 3, &entry, &error),
		PG_OK);
	PG_STATUS(pg_source_write_all(
				source, "a.txt", "two", 3, &entry, &error),
		PG_OK);

	PG_STATUS(
		pg_source_find(source, "a.txt", &file, &error), PG_OK);
	PG_STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	PG_CHECK(info.cached_header_size == sizeof(header) &&
		memcmp(info.cached_header, header, sizeof(header)) ==
			0);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_reader_open_source(source, "a.txt",
		PG_READ_LOGICAL_PAYLOAD, &reader, &error), PG_OK);
	PG_STATUS(pg_reader_read(reader, data, sizeof(data), &count, &error),
		PG_OK);
	PG_STATUS(pg_reader_close(&reader, &error), PG_OK);
	PG_CHECK(count == 3 && memcmp(data, "two", 3) == 0);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static int test_hogg_resize(void)
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_source *source = NULL;
	pg_error error;
	pg_source_options options = {PG_HOGG10, PG_WRITE, 0};
	char name[16];
	char data[4] = {};
	size_t count = 0;

	remove("resize.hogg");
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(pg_archive_builder_create(context, "resize.hogg",
				PG_HOGG10, 0, &builder, &error),
		PG_OK);
	for (unsigned int i = 0; i < 15; i++) {
		PG_CHECK(snprintf(name, sizeof(name), "f%02u", i) > 0);
		PG_STATUS(pg_archive_builder_write_all(builder, name,
					"abc", 3, NULL, &error),
			PG_OK);
	}
	PG_STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	PG_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	PG_STATUS(pg_source_open(context, "resize.hogg", &options,
				&source, &error),
		PG_OK);
	for (unsigned int i = 15; i < 40; i++) {
		PG_CHECK(snprintf(name, sizeof(name), "f%02u", i) > 0);
		PG_STATUS(pg_source_write_all(
					source, name, "new", 3, NULL, &error),
			PG_OK);
	}
	for (unsigned int i = 0; i < 40; i++) {
		PG_CHECK(snprintf(name, sizeof(name), "f%02u", i) > 0);
		PG_STATUS(pg_source_read_all(source, name, data,
					sizeof(data), &count, &error),
			PG_OK);
		PG_CHECK(count == 3 &&
			memcmp(data, i < 15 ? "abc" : "new", 3) == 0);
	}
	PG_STATUS(pg_source_validate(source, &error), PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_source_open(context, "resize.hogg", NULL, &source,
				&error),
		PG_OK);
	PG_STATUS(pg_source_read_all(source, "f39", data, sizeof(data),
				&count, &error),
		PG_OK);
	PG_CHECK(count == 3 && memcmp(data, "new", 3) == 0);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static int test_archive_copy(void)
{
	static const char payload[] =
		"repeated repeated repeated repeated repeated repeated";
	const uint32_t formats[] = {PG_PIGG2, PG_HOGG10};
	const char *paths[] = {"copy.pigg", "copy.hogg"};
	size_t i;

	for (i = 0; i < 2; i++) {
		pg_context *context = NULL;
		pg_archive_builder *builder = NULL;
		pg_source *source = NULL;
		pg_file *input = NULL, *copy = NULL;
		pg_reader *reader = NULL;
		pg_source_options source_options = {
			formats[i], PG_WRITE, 0};
		pg_entry_options entry = {};
		pg_file_info info;
		pg_error error;
		char stored[128], copied[128], logical[128];
		size_t stored_size = 0, copied_size = 0;
		size_t logical_size = 0;

		remove(paths[i]);
		entry.compression = PG_COMPRESS_FORCE;
		entry.cached_header = "meta";
		entry.cached_header_size = 4;
		entry.mtime = 42;
		PG_STATUS(pg_context_open(&context, &error), PG_OK);
		PG_STATUS(pg_archive_builder_create(context, paths[i],
					formats[i], 0, &builder, &error),
			PG_OK);
		PG_STATUS(pg_archive_builder_write_all(builder, "A.TXT",
					payload, sizeof(payload) - 1, &entry,
					&error),
			PG_OK);
		PG_STATUS(pg_archive_builder_finish(builder, &error),
			PG_OK);
		PG_STATUS(pg_archive_builder_close(&builder, &error),
			PG_OK);
		PG_STATUS(pg_source_open(context, paths[i],
					&source_options, &source, &error),
			PG_OK);
		PG_STATUS(
			pg_source_find(source, "a.txt", &input, &error),
			PG_OK);
		PG_STATUS(pg_reader_open(input, PG_READ_STORED, &reader,
					&error),
			PG_OK);
		PG_STATUS(pg_reader_read(reader, stored, sizeof(stored),
					&stored_size, &error),
			PG_OK);
		PG_STATUS(pg_reader_close(&reader, &error), PG_OK);
		PG_STATUS(pg_source_copy(source, "a.txt", input,
					PG_COMPRESS_AUTO, &error),
			PG_CONFLICT);
		PG_STATUS(pg_source_copy(source, "copy.txt", input,
					PG_COMPRESS_AUTO, &error),
			PG_OK);
		PG_STATUS(pg_file_close(&input, &error), PG_OK);
		PG_STATUS(pg_source_find(
					source, "copy.txt", &copy, &error),
			PG_OK);
		PG_STATUS(pg_file_inspect(copy, &info, &error), PG_OK);
		PG_CHECK(info.encoding == PG_ZLIB && info.mtime == 42 &&
			info.cached_header_size == 4 &&
			memcmp(info.cached_header, "meta", 4) == 0);
		PG_STATUS(pg_reader_open(copy, PG_READ_STORED, &reader,
					&error),
			PG_OK);
		PG_STATUS(pg_reader_read(reader, copied, sizeof(copied),
					&copied_size, &error),
			PG_OK);
		PG_CHECK(copied_size == stored_size &&
			memcmp(copied, stored, stored_size) == 0);
		PG_STATUS(pg_reader_close(&reader, &error), PG_OK);
		PG_STATUS(pg_file_close(&copy, &error), PG_OK);
		PG_STATUS(
			pg_source_read_all(source, "copy.txt", logical,
				sizeof(logical), &logical_size, &error),
			PG_OK);
		PG_CHECK(logical_size == sizeof(payload) - 1 &&
			memcmp(logical, payload, logical_size) == 0);
		PG_STATUS(pg_source_validate(source, &error), PG_OK);
		PG_STATUS(pg_source_close(&source, &error), PG_OK);
		PG_STATUS(pg_context_close(&context, &error), PG_OK);
	}
	return 0;
}

static int test_native_encoded_streaming(void)
{
	static unsigned char payload[131073];
	unsigned char output[70000];
	pg_context *context = NULL;
	pg_writer *writer = NULL;
	pg_reader *reader = NULL;
	pg_write_options options;
	pg_error error;
	uint32_t random = 987654321;
	size_t written = 0;
	size_t count = 0;
	size_t offset = 0;

	for (size_t i = 0; i < sizeof(payload); i++) {
		random ^= random << 13;
		random ^= random >> 17;
		random ^= random << 5;
		payload[i] = (unsigned char)random;
	}
	size_t compressed_size = zng_compressBound(sizeof(payload));
	unsigned char *compressed =
		(unsigned char *)malloc(compressed_size);
	PG_CHECK(compressed != NULL);
	PG_CHECK(zng_compress(compressed, &compressed_size, payload,
				sizeof(payload)) == Z_OK);
	PG_CHECK(compressed_size > 65536);
	remove("encoded_large.bin");
	remove("encoded_large_once.bin");
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	pg_write_options_init(&options, sizeof(payload));
	options.encoding = PG_ZLIB;
	options.input_size = compressed_size;
	PG_STATUS(pg_writer_open_native(context, "encoded_large.bin",
				&options, 0, &writer, &error),
		PG_OK);
	for (offset = 0; offset < compressed_size; offset += written) {
		size_t chunk = compressed_size - offset;
		if (chunk > 17000)
			chunk = 17000;
		PG_STATUS(pg_writer_write(writer, compressed + offset,
					chunk, &written, &error),
			PG_OK);
		PG_CHECK(written == chunk);
	}
	PG_STATUS(pg_writer_finish(writer, &error), PG_OK);

	PG_STATUS(pg_writer_close(&writer, &error), PG_OK);
	PG_STATUS(
		pg_writer_open_native(context, "encoded_large_once.bin",
			&options, 0, &writer, &error),
		PG_OK);
	PG_STATUS(pg_writer_write(writer, compressed, compressed_size,
				&written, &error),
		PG_OK);
	PG_CHECK(written == compressed_size);
	PG_STATUS(pg_writer_finish(writer, &error), PG_OK);
	PG_STATUS(pg_writer_close(&writer, &error), PG_OK);
	for (unsigned int variant = 0; variant < 2; variant++) {
		const char *path = variant ? "encoded_large_once.bin"
						: "encoded_large.bin";
		PG_STATUS(pg_reader_open_native(
					context, path, &reader, &error),
			PG_OK);
		for (offset = 0; offset < sizeof(payload);
			offset += count) {
			PG_STATUS(
				pg_reader_read(reader, output,
					sizeof(output), &count, &error),
				PG_OK);
			PG_CHECK(count > 0 && count <= sizeof(output) &&
				memcmp(output, payload + offset,
					count) == 0);
		}
		PG_STATUS(pg_reader_read(
					reader, output, 1, &count, &error),
			PG_END);
		PG_STATUS(pg_reader_close(&reader, &error), PG_OK);
	}
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	free(compressed);
	return 0;
}

static int test_archive_streaming(void)
{
	static unsigned char payload[131073];
	unsigned char buffer[70000];
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_source *source = NULL;
	pg_file *file = NULL;
	pg_reader *reader = NULL;
	pg_entry_options entry;
	pg_file_info info;
	pg_error error;
	size_t count = 0;
	size_t offset = 0;
	uint32_t random = 123456789;

	for (size_t i = 0; i < sizeof(payload); i++) {
		random ^= random << 13;
		random ^= random >> 17;
		random ^= random << 5;
		payload[i] = (unsigned char)random;
	}
	remove("streaming.pigg");
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(pg_archive_builder_create(context, "streaming.pigg",
				PG_PIGG2, 0, &builder, &error),
		PG_OK);
	memset(&entry, 0, sizeof(entry));
	entry.compression = PG_COMPRESS_FORCE;
	PG_STATUS(pg_archive_builder_write_all(builder, "large",
				payload, sizeof(payload), &entry, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_write_all(
				builder, "empty", NULL, 0, &entry, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	PG_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	PG_STATUS(pg_source_open(context, "streaming.pigg", NULL,
				&source, &error),
		PG_OK);
	PG_STATUS(
		pg_source_find(source, "large", &file, &error), PG_OK);
	PG_STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	PG_CHECK(info.encoding == PG_ZLIB &&
		info.logical_size == sizeof(payload) &&
		info.stored_size > 65536);
	PG_STATUS(
		pg_reader_open(file, PG_READ_LOGICAL, &reader, &error),
		PG_OK);
	for (offset = 0; offset < sizeof(payload); offset += count) {
		PG_STATUS(pg_reader_read(reader, buffer, sizeof(buffer),
					&count, &error),
			PG_OK);
		PG_CHECK(count > 0 && count <= 65536 &&
			memcmp(buffer, payload + offset, count) == 0);
	}
	PG_STATUS(pg_reader_read(reader, buffer, 1, &count, &error),
		PG_END);
	PG_CHECK(count == 0);
	PG_STATUS(pg_reader_close(&reader, &error), PG_OK);
	PG_STATUS(pg_reader_open(file, PG_READ_STORED, &reader, &error),
		PG_OK);
	offset = 0;
	while (offset < info.stored_size) {
		PG_STATUS(pg_reader_read(reader, buffer, sizeof(buffer),
					&count, &error),
			PG_OK);
		PG_CHECK(count > 0 && count <= 65536);
		offset += count;
	}
	PG_CHECK(offset == info.stored_size);
	PG_STATUS(pg_reader_read(reader, buffer, 1, &count, &error),
		PG_END);
	PG_STATUS(pg_reader_close(&reader, &error), PG_OK);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(
		pg_source_find(source, "empty", &file, &error), PG_OK);
	PG_STATUS(
		pg_reader_open(file, PG_READ_LOGICAL, &reader, &error),
		PG_OK);
	PG_STATUS(pg_reader_read(reader, buffer, 1, &count, &error),
		PG_END);
	PG_CHECK(count == 0);
	PG_STATUS(pg_reader_close(&reader, &error), PG_OK);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	{
		FILE *stream = fopen("streaming.pigg", "r+b");
		unsigned char field[4];
		uint32_t payload_offset;
		uint32_t stored_size;
		int trailer;

		PG_CHECK(stream != NULL);
		PG_CHECK(fseek(stream, 16 + 16, SEEK_SET) == 0);
		PG_CHECK(fread(field, 1, 4, stream) == 4);
		payload_offset = (uint32_t)field[0] |
			((uint32_t)field[1] << 8) |
			((uint32_t)field[2] << 16) |
			((uint32_t)field[3] << 24);
		PG_CHECK(fseek(stream, 16 + 44, SEEK_SET) == 0);
		PG_CHECK(fread(field, 1, 4, stream) == 4);
		stored_size = (uint32_t)field[0] |
			((uint32_t)field[1] << 8) |
			((uint32_t)field[2] << 16) |
			((uint32_t)field[3] << 24);
		PG_CHECK(stored_size > 4);
		PG_CHECK(fseek(stream, payload_offset + stored_size - 1,
					SEEK_SET) == 0);
		trailer = fgetc(stream);
		PG_CHECK(trailer != EOF);
		PG_CHECK(fseek(stream, -1, SEEK_CUR) == 0);
		PG_CHECK(fputc(trailer ^ 1, stream) != EOF);
		PG_CHECK(fclose(stream) == 0);
	}
	PG_STATUS(pg_source_open(context, "streaming.pigg", NULL,
				&source, &error),
		PG_OK);
	PG_STATUS(
		pg_source_find(source, "large", &file, &error), PG_OK);
	for (uint32_t mode = PG_READ_LOGICAL; mode <= PG_READ_STORED;
		mode++) {
		PG_STATUS(pg_reader_open(file, mode, &reader, &error),
			PG_OK);
		pg_status status = PG_OK;
		offset = 0;
		while (status == PG_OK) {
			status = pg_reader_read(reader, buffer,
				sizeof(buffer), &count, &error);
			PG_CHECK(error.status == status);
			offset += count;
		}
		PG_CHECK(status == PG_CORRUPT);
		PG_CHECK(offset ==
			(mode == PG_READ_LOGICAL ? sizeof(payload)
							: info.stored_size));
		PG_STATUS(pg_reader_close(&reader, &error), PG_OK);
	}
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static int test_archive_read(void)
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_source *source = NULL;
	pg_file *file = NULL;
	pg_reader *reader = NULL;
	pg_reader_info reader_info;
	pg_buffer allocated = {NULL, 0};
	pg_entry_options entry = {};
	pg_error error;
	char bytes[3] = {'z', 'z', 'z'};
	size_t count = 99;
	uint32_t format;

	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	entry.compression = PG_COMPRESS_FORCE;
	for (format = PG_PIGG2; format <= PG_HOGG10; format++) {
		remove("whole.archive");
		PG_STATUS(pg_archive_builder_create(context,
					"whole.archive", format, 0, &builder,
					&error),
			PG_OK);
		PG_STATUS(pg_archive_builder_write_all(builder, "Data",
					"abc", 3, &entry, &error),
			PG_OK);
		PG_STATUS(pg_archive_builder_write_all(builder, "empty",
					NULL, 0, NULL, &error),
			PG_OK);
		PG_STATUS(pg_archive_builder_finish(builder, &error),
			PG_OK);
		PG_STATUS(pg_archive_builder_close(&builder, &error),
			PG_OK);
		PG_STATUS(pg_source_open(context, "whole.archive", NULL,
					&source, &error),
			PG_OK);
		PG_STATUS(pg_source_find(source, "DATA", &file, &error),
			PG_OK);
		PG_STATUS(pg_reader_open_source(source, "data",
					PG_READ_STORED, &reader, &error),
			PG_OK);
		PG_STATUS(
			pg_reader_inspect(reader, &reader_info, &error),
			PG_OK);
		PG_CHECK(reader_info.encoding == PG_ZLIB);
		{
			char stored[64];
			size_t stored_count = 0;

			PG_CHECK(reader_info.size < sizeof(stored));
			PG_STATUS(pg_reader_read(reader, stored,
						(size_t)reader_info.size,
						&stored_count, &error),
				PG_OK);
			PG_CHECK(stored_count == reader_info.size);
			PG_STATUS(pg_reader_read(reader, stored, 1,
						&stored_count, &error),
				PG_END);
		}
		PG_STATUS(pg_reader_close(&reader, &error), PG_OK);
		bytes[0] = 'z';
		PG_STATUS(pg_file_read_all(
					file, bytes, 2, &count, &error),
			PG_CAPACITY);
		PG_CHECK(count == 0 && bytes[0] == 'z');
		PG_STATUS(pg_file_read_all(file, bytes, sizeof(bytes),
					&count, &error),
			PG_OK);
		PG_CHECK(count == 3 && memcmp(bytes, "abc", 3) == 0);
		PG_STATUS(pg_file_read_all_alloc(
					file, 2, &allocated, &error),
			PG_LIMIT);
		PG_STATUS(pg_file_read_all_alloc(
					file, 3, &allocated, &error),
			PG_OK);
		PG_CHECK(allocated.size == 3 &&
			memcmp(allocated.data, "abc", 3) == 0);
		pg_buffer_free(&allocated);
		PG_STATUS(pg_file_verify(file, &error), PG_OK);
		PG_STATUS(pg_file_close(&file, &error), PG_OK);
		PG_STATUS(pg_source_read_all(source, "data", bytes,
					sizeof(bytes), &count, &error),
			PG_OK);
		PG_CHECK(count == 3 && memcmp(bytes, "abc", 3) == 0);
		PG_STATUS(pg_source_read_all_alloc(source, "empty", 0,
					&allocated, &error),
			PG_OK);
		PG_CHECK(allocated.data == NULL && allocated.size == 0);
		PG_STATUS(pg_source_close(&source, &error), PG_OK);
		PG_CHECK(remove("whole.archive") == 0);
	}
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

using ApiUnit = piggle_test::ContractTest;

TEST_F(ApiUnit, api_options)
{
	ASSERT_EQ(0, test_options());
}

TEST_F(ApiUnit, api_buffer)
{
	ASSERT_EQ(0, test_buffer());
}

TEST_F(ApiUnit, api_context)
{
	ASSERT_EQ(0, test_context());
}

TEST_F(ApiUnit, api_names)
{
	ASSERT_EQ(0, test_names());
}

TEST_F(ApiUnit, api_native_reader)
{
	ASSERT_EQ(0, test_native_reader());
}

TEST_F(ApiUnit, api_archive_read)
{
	ASSERT_EQ(0, test_archive_read());
}

TEST_F(ApiUnit, api_native_writer)
{
	ASSERT_EQ(0, test_native_writer());
}

TEST_F(ApiUnit, api_source)
{
	ASSERT_EQ(0, test_source());
}

TEST_F(ApiUnit, api_tree)
{
	ASSERT_EQ(0, test_tree());
}

TEST_F(ApiUnit, api_builder)
{
	ASSERT_EQ(0, test_builder());
}

TEST_F(ApiUnit, api_pigg_edit)
{
	ASSERT_EQ(0, test_pigg_edit());
}

TEST_F(ApiUnit, api_hogg_delete)
{
	ASSERT_EQ(0, test_hogg_delete());
}

TEST_F(ApiUnit, api_hogg_update)
{
	ASSERT_EQ(0, test_hogg_update());
}

TEST_F(ApiUnit, api_hogg_flush)
{
	ASSERT_EQ(0, test_hogg_flush());
}

TEST_F(ApiUnit, api_hogg_resize)
{
	ASSERT_EQ(0, test_hogg_resize());
}

TEST_F(ApiUnit, api_archive_copy)
{
	ASSERT_EQ(0, test_archive_copy());
}

TEST_F(ApiUnit, api_archive_streaming)
{
	ASSERT_EQ(0, test_archive_streaming());
}

TEST_F(ApiUnit, api_native_encoded_streaming)
{
	ASSERT_EQ(0, test_native_encoded_streaming());
}

} // namespace
