#include "api_test.h"

static int context_arguments(struct fixture *f)
{
	pg_error error;
	size_t required = 123;
	char bytes[8] = "guard";

	STATUS(pg_name_normalize(f->context, NULL, bytes, sizeof(bytes),
		&required, &error), PG_INVALID);
	CHECK(required == 0 && strcmp(bytes, "guard") == 0);
	STATUS(pg_name_normalize(f->context, "a", NULL, 1, &required,
		&error), PG_INVALID);
	CHECK(required == 0);
	STATUS(pg_name_normalize(f->context, "a", bytes, sizeof(bytes),
		NULL, &error), PG_INVALID);
	CHECK(strcmp(bytes, "guard") == 0);
	STATUS(pg_name_normalize(NULL, "a", bytes, sizeof(bytes),
		&required, &error), PG_INVALID);
	CHECK(required == 0);
	CHECK(pg_name_normalize(f->context, "A", bytes, sizeof(bytes),
		&required, NULL) == PG_OK);
	CHECK(required == 2 && strcmp(bytes, "a") == 0);
	return 0;
}

static int source_arguments(struct fixture *f)
{
	pg_source *source = NULL;
	pg_file *file = NULL;
	pg_cursor *cursor = NULL;
	pg_source_info info;
	pg_source_options options = { UINT32_MAX, PG_READ };
	pg_error error;

	STATUS(pg_source_open(f->context, "root", &options, &source,
		&error), PG_INVALID);
	CHECK(source == NULL);
	options.format = PG_AUTO;
	options.access = UINT32_MAX;
	STATUS(pg_source_open(f->context, "root", &options, &source,
		&error), PG_INVALID);
	CHECK(source == NULL);
	STATUS(pg_source_open(f->context, NULL, NULL, &source, &error),
		PG_INVALID);
	STATUS(pg_source_open(f->context, "", NULL, &source, &error),
		PG_INVALID);
	STATUS(pg_source_open(NULL, "root", NULL, &source, &error),
		PG_INVALID);
	CHECK(source == NULL);
	STATUS(pg_source_open(f->context, "root", NULL, NULL, &error),
		PG_INVALID);
	STATUS(pg_source_open(f->context, "root", NULL, &source, &error),
		PG_BUSY);
	CHECK(source == NULL);
	STATUS(pg_source_inspect(f->source, NULL, &error), PG_INVALID);
	memset(&info, 0xa5, sizeof(info));
	STATUS(pg_source_inspect(NULL, &info, &error), PG_INVALID);
	CHECK(info.id == 0 && info.generation == 0 && info.native_path == NULL);
	CHECK(info.format == 0 && info.access == 0);
	STATUS(pg_source_find(f->source, NULL, &file, &error), PG_INVALID);
	CHECK(file == NULL);
	STATUS(pg_source_find(f->source, "a", NULL, &error), PG_INVALID);
	STATUS(pg_source_find(f->source, "../a", &file, &error), PG_INVALID);
	CHECK(file == NULL);
	STATUS(pg_source_files(f->source, NULL, NULL, &error), PG_INVALID);
	STATUS(pg_source_files(f->source, ".", &cursor, &error), PG_INVALID);
	CHECK(cursor == NULL);
	STATUS(pg_source_request_subtree(f->source, "..", &error),
		PG_INVALID);
	STATUS(pg_source_recover(f->context, NULL, &error), PG_INVALID);
	STATUS(pg_source_recover(f->context, "", &error), PG_INVALID);
	return 0;
}

static int tree_arguments(struct fixture *f)
{
	pg_tree *tree = NULL;
	pg_source *source = NULL;
	pg_file *file = NULL;
	pg_cursor *cursor = NULL;
	pg_tree_info info;
	pg_error error;

	STATUS(pg_tree_create(f->context, NULL, &error), PG_INVALID);
	STATUS(pg_tree_create(NULL, &tree, &error), PG_INVALID);
	CHECK(tree == NULL);
	STATUS(pg_tree_open(f->context, NULL, 1, &tree, &error), PG_INVALID);
	CHECK(tree == NULL);
	STATUS(pg_tree_open(f->context, NULL, 0, NULL, &error), PG_INVALID);
	STATUS(pg_tree_inspect(f->tree, NULL, &error), PG_INVALID);
	memset(&info, 0xa5, sizeof(info));
	STATUS(pg_tree_inspect(NULL, &info, &error), PG_INVALID);
	CHECK(info.source_count == 0 && info.watch_mode == 0);
	STATUS(pg_tree_attach(f->tree, NULL, &error), PG_INVALID);
	STATUS(pg_tree_detach(f->tree, NULL, &error), PG_INVALID);
	STATUS(pg_tree_source(f->tree, 0, NULL, &error), PG_INVALID);
	STATUS(pg_tree_source(f->tree, SIZE_MAX, &source, &error), PG_END);
	CHECK(source == NULL);
	STATUS(pg_tree_find(f->tree, NULL, &file, &error), PG_INVALID);
	STATUS(pg_tree_find(f->tree, "a", NULL, &error), PG_INVALID);
	STATUS(pg_tree_find(f->tree, "a:b", &file, &error), PG_INVALID);
	CHECK(file == NULL);
	STATUS(pg_tree_files(f->tree, NULL, NULL, &error), PG_INVALID);
	STATUS(pg_tree_files(f->tree, ".", &cursor, &error), PG_INVALID);
	CHECK(cursor == NULL);
	STATUS(pg_tree_request_subtree(f->tree, "/", &error), PG_INVALID);
	STATUS(pg_tree_watch(f->tree, PG_WATCH_OFF, &error), PG_INVALID);
	STATUS(pg_tree_watch(f->tree, UINT32_MAX, &error), PG_INVALID);
	STATUS(pg_tree_poll(f->tree, NULL, &error), PG_INVALID);
	STATUS(pg_tree_inspect(f->tree, &info, &error), PG_OK);
	CHECK(info.watch_mode == PG_WATCH_OFF && info.source_count == 1);
	return 0;
}

static int file_arguments(struct fixture *f)
{
	pg_file_info info;
	pg_file *file = NULL;
	pg_buffer allocated = { NULL, 0 };
	pg_error error;
	unsigned char bytes[16];
	size_t count = 123;

	memset(bytes, 0xa5, sizeof(bytes));
	STATUS(pg_file_read_all(f->file, NULL, 1, &count, &error), PG_INVALID);
	CHECK(count == 0);
	STATUS(pg_file_read_all(f->file, bytes, sizeof(bytes), NULL, &error),
		PG_INVALID);
	STATUS(pg_file_read_all_alloc(f->file, 10, NULL, &error), PG_INVALID);
	STATUS(pg_file_read_all_alloc(NULL, 10, &allocated, &error),
		PG_INVALID);
	CHECK(allocated.data == NULL && allocated.size == 0);
	STATUS(pg_file_write_all(f->file, NULL, 1, NULL, &error), PG_INVALID);
	STATUS(pg_file_export(f->file, NULL, 0, &error), PG_INVALID);
	STATUS(pg_file_export(f->file, "", 0, &error), PG_INVALID);
	STATUS(pg_file_export(f->file, "output", 2, &error), PG_INVALID);
	STATUS(pg_file_inspect(f->file, NULL, &error), PG_INVALID);
	memset(&info, 0xa5, sizeof(info));
	STATUS(pg_file_inspect(NULL, &info, &error), PG_INVALID);
	CHECK(info.source_id == 0 && info.copy_id == 0);
	CHECK(info.source_generation == 0 && info.copy_generation == 0);
	CHECK(info.canonical_name == NULL && info.original_name == NULL);
	CHECK(info.archive_record == 0 && info.mtime == 0);
	CHECK(info.logical_size == 0 && info.stored_size == 0);
	CHECK(info.encoding == 0 && info.digest_kind == 0);
	CHECK(info.cached_header == NULL && info.cached_header_size == 0);
	for (size_t i = 0; i < sizeof(info.digest); i++)
		CHECK(info.digest[i] == 0);
	STATUS(pg_cursor_next(f->cursor, NULL, &error), PG_INVALID);
	STATUS(pg_cursor_next(f->cursor, &file, &error), PG_OK);
	STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	CHECK(strcmp(info.canonical_name, "a") == 0);
	STATUS(pg_file_close(&file, &error), PG_OK);
	for (size_t i = 0; i < sizeof(bytes); i++)
		CHECK(bytes[i] == 0xa5);
	return 0;
}

static int reader_arguments(struct fixture *f)
{
	pg_reader *reader = NULL;
	pg_reader_info info;
	pg_error error;
	size_t count = 123;
	unsigned char bytes[sizeof(payload)];

	STATUS(pg_reader_open(f->file, UINT32_MAX, &reader, &error),
		PG_INVALID);
	CHECK(reader == NULL);
	STATUS(pg_reader_open(f->file, PG_READ_LOGICAL, NULL, &error),
		PG_INVALID);
	STATUS(pg_reader_open_source(f->source, NULL, PG_READ_LOGICAL,
		&reader, &error), PG_INVALID);
	STATUS(pg_reader_open_source(f->source, "a", UINT32_MAX,
		&reader, &error), PG_INVALID);
	STATUS(pg_reader_open_source(f->source, "a", PG_READ_LOGICAL,
		NULL, &error), PG_INVALID);
	STATUS(pg_reader_open_tree(f->tree, NULL, PG_READ_LOGICAL,
		&reader, &error), PG_INVALID);
	STATUS(pg_reader_open_tree(f->tree, "a", UINT32_MAX,
		&reader, &error), PG_INVALID);
	STATUS(pg_reader_open_tree(f->tree, "a", PG_READ_LOGICAL,
		NULL, &error), PG_INVALID);
	STATUS(pg_reader_open_native(f->context, NULL, &reader, &error),
		PG_INVALID);
	STATUS(pg_reader_open_native(f->context, "", &reader, &error),
		PG_INVALID);
	STATUS(pg_reader_open_native(f->context, "input", NULL, &error),
		PG_INVALID);
	CHECK(reader == NULL);
	memset(&info, 0xa5, sizeof(info));
	STATUS(pg_reader_inspect(NULL, &info, &error), PG_INVALID);
	CHECK(info.size == 0 && info.logical_size == 0);
	CHECK(info.mtime == 0 && info.encoding == 0);
	STATUS(pg_reader_open(f->file, PG_READ_LOGICAL, &reader, &error),
		PG_OK);
	STATUS(pg_reader_inspect(reader, NULL, &error), PG_INVALID);
	STATUS(pg_reader_read(reader, NULL, 1, &count, &error), PG_INVALID);
	CHECK(count == 0);
	STATUS(pg_reader_read(reader, bytes, sizeof(bytes), NULL, &error),
		PG_INVALID);
	STATUS(pg_reader_read(reader, NULL, 0, &count, &error), PG_OK);
	CHECK(count == 0);
	STATUS(pg_reader_read(reader, bytes, sizeof(bytes), &count, &error),
		PG_OK);
	CHECK(count == sizeof(payload));
	CHECK(memcmp(bytes, payload, count) == 0);
	STATUS(pg_reader_close(&reader, &error), PG_OK);
	CHECK(reader == NULL);
	return 0;
}

static int writer_arguments(struct fixture *f)
{
	pg_writer *writer = NULL;
	pg_write_options options;
	pg_error error;
	size_t count = 123;

	pg_write_options_init(&options, sizeof(payload));
	STATUS(pg_writer_open_source(f->source, "a", NULL, &writer,
		&error), PG_INVALID);
	STATUS(pg_writer_open_source(f->source, NULL, &options, &writer,
		&error), PG_INVALID);
	STATUS(pg_writer_open_source(f->source, "a", &options, NULL,
		&error), PG_INVALID);
	STATUS(pg_writer_open_file(f->file, NULL, &writer, &error), PG_INVALID);
	STATUS(pg_writer_open_file(f->file, &options, NULL, &error),
		PG_INVALID);
	STATUS(pg_writer_open_archive_builder(f->builder, "a", NULL,
		&writer, &error), PG_INVALID);
	STATUS(pg_writer_open_archive_builder(f->builder, NULL, &options,
		&writer, &error), PG_INVALID);
	STATUS(pg_writer_open_archive_builder(f->builder, "a", &options,
		NULL, &error), PG_INVALID);
	STATUS(pg_writer_open_native(f->context, NULL, &options, 0,
		&writer, &error), PG_INVALID);
	STATUS(pg_writer_open_native(f->context, "", &options, 0,
		&writer, &error), PG_INVALID);
	STATUS(pg_writer_open_native(f->context, "output", NULL, 0,
		&writer, &error), PG_INVALID);
	STATUS(pg_writer_open_native(f->context, "output", &options, 2,
		&writer, &error), PG_INVALID);
	STATUS(pg_writer_open_native(f->context, "output", &options, 0,
		NULL, &error), PG_INVALID);
	CHECK(writer == NULL);
	STATUS(pg_writer_open_archive_builder(f->builder, "a", &options,
		&writer, &error), PG_OK);
	STATUS(pg_writer_write(writer, NULL, 1, &count, &error), PG_INVALID);
	CHECK(count == 0);
	STATUS(pg_writer_write(writer, payload, sizeof(payload), NULL,
		&error), PG_INVALID);
	STATUS(pg_writer_write(writer, NULL, 0, &count, &error), PG_OK);
	CHECK(count == 0);
	STATUS(pg_writer_write(writer, payload, sizeof(payload), &count,
		&error), PG_OK);
	CHECK(count == sizeof(payload));
	STATUS(pg_writer_finish(writer, &error), PG_OK);
	STATUS(pg_writer_close(&writer, &error), PG_OK);
	CHECK(writer == NULL);
	return 0;
}

static int descriptor_arguments(struct fixture *f)
{
	pg_write_options options;
	pg_writer *writer = NULL;
	pg_error error;

	for (unsigned int i = 0; i < 5; i++) {
		pg_write_options_init(&options, 1);
		switch (i) {
		case 0:
			options.encoding = UINT32_MAX;
			break;
		case 1:
			options.input_size = 2;
			break;
		case 2:
			options.entry.compression = UINT32_MAX;
			break;
		case 3:
			options.entry.digest_kind = UINT32_MAX;
			break;
		case 4:
			options.entry.cached_header_size = 1;
			break;
		}
		/* A nonempty header is unsupported on loose/native output. */
		if (i == 4) {
			STATUS(pg_writer_open_archive_builder(f->builder, "a",
				&options, &writer, &error), PG_INVALID);
			CHECK(writer == NULL);
			continue;
		}
		STATUS(pg_writer_open_source(f->source, "a", &options,
			&writer, &error), PG_INVALID);
		CHECK(writer == NULL);
		STATUS(pg_writer_open_file(f->file, &options, &writer,
			&error), PG_INVALID);
		CHECK(writer == NULL);
		STATUS(pg_writer_open_archive_builder(f->builder, "a",
			&options, &writer, &error), PG_INVALID);
		CHECK(writer == NULL);
		STATUS(pg_writer_open_native(f->context, "output", &options,
			0, &writer, &error), PG_INVALID);
		CHECK(writer == NULL);
	}
	pg_write_options_init(&options, 1);
	options.entry.original_name = "different";
	STATUS(pg_writer_open_archive_builder(f->builder, "a", &options,
		&writer, &error), PG_INVALID);
	STATUS(pg_writer_open_file(f->file, &options, &writer, &error),
		PG_INVALID);
	CHECK(writer == NULL);
	return 0;
}

static int named_arguments(struct fixture *f)
{
	pg_error error;
	pg_buffer allocated = { NULL, 0 };
	unsigned char bytes[16];
	size_t count = 123;

	STATUS(pg_source_read_all(f->source, NULL, bytes, sizeof(bytes),
		&count, &error), PG_INVALID);
	CHECK(count == 0);
	count = 123;
	STATUS(pg_source_read_all(f->source, "a", NULL, 1, &count,
		&error), PG_INVALID);
	CHECK(count == 0);
	STATUS(pg_source_read_all(f->source, "a", bytes, sizeof(bytes),
		NULL, &error), PG_INVALID);
	STATUS(pg_source_read_all_alloc(f->source, "a", 10, NULL, &error),
		PG_INVALID);
	STATUS(pg_source_read_all_alloc(f->source, NULL, 10, &allocated,
		&error), PG_INVALID);
	CHECK(allocated.data == NULL && allocated.size == 0);
	count = 123;
	STATUS(pg_tree_read_all(f->tree, NULL, bytes, sizeof(bytes),
		&count, &error), PG_INVALID);
	CHECK(count == 0);
	count = 123;
	STATUS(pg_tree_read_all(f->tree, "a", NULL, 1, &count, &error),
		PG_INVALID);
	CHECK(count == 0);
	STATUS(pg_tree_read_all(f->tree, "a", bytes, sizeof(bytes), NULL,
		&error), PG_INVALID);
	STATUS(pg_tree_read_all_alloc(f->tree, "a", 10, NULL, &error),
		PG_INVALID);
	STATUS(pg_tree_read_all_alloc(f->tree, NULL, 10, &allocated, &error),
		PG_INVALID);
	CHECK(allocated.data == NULL && allocated.size == 0);
	return 0;
}

static int transfer_arguments(struct fixture *f)
{
	pg_error error;

	STATUS(pg_source_write_all(f->source, NULL, payload, sizeof(payload),
		NULL, &error), PG_INVALID);
	STATUS(pg_source_write_all(f->source, "a", NULL, 1, NULL, &error),
		PG_INVALID);
	STATUS(pg_source_import(f->source, NULL, "input", PG_COMPRESS_AUTO,
		&error), PG_INVALID);
	STATUS(pg_source_import(f->source, "a", NULL, PG_COMPRESS_AUTO,
		&error), PG_INVALID);
	STATUS(pg_source_import(f->source, "a", "input", UINT32_MAX,
		&error), PG_INVALID);
	STATUS(pg_source_copy(f->source, NULL, f->file, PG_COMPRESS_AUTO,
		&error), PG_INVALID);
	STATUS(pg_source_copy(f->source, "a", NULL, PG_COMPRESS_AUTO,
		&error), PG_INVALID);
	STATUS(pg_source_copy(f->source, "b", f->file, UINT32_MAX, &error),
		PG_INVALID);
	STATUS(pg_source_export(f->source, NULL, "output", 0, &error),
		PG_INVALID);
	STATUS(pg_source_export(f->source, "a", NULL, 0, &error), PG_INVALID);
	STATUS(pg_source_export(f->source, "a", "output", 2, &error),
		PG_INVALID);
	STATUS(pg_tree_export(f->tree, NULL, "output", 0, &error), PG_INVALID);
	STATUS(pg_tree_export(f->tree, "a", NULL, 0, &error), PG_INVALID);
	STATUS(pg_tree_export(f->tree, "a", "output", 2, &error), PG_INVALID);
	CHECK(expect_bytes("root/a", payload, sizeof(payload)) == 0);
	return 0;
}

static int pack_arguments(struct fixture *f)
{
	pg_pack_options options = { UINT32_MAX, 0 };
	pg_error error;

	STATUS(pg_source_pack(f->source, NULL, PG_PIGG2, NULL, &error),
		PG_INVALID);
	STATUS(pg_tree_pack(f->tree, NULL, PG_PIGG2, NULL, &error), PG_INVALID);
	STATUS(pg_source_pack(f->source, "packed", PG_AUTO, NULL, &error),
		PG_INVALID);
	STATUS(pg_tree_pack(f->tree, "packed", PG_LOOSE, NULL, &error),
		PG_INVALID);
	STATUS(pg_source_pack(f->source, "packed", PG_PIGG2, &options,
		&error), PG_INVALID);
	STATUS(pg_tree_pack(f->tree, "packed", PG_HOGG10, &options,
		&error), PG_INVALID);
	options.compression = PG_COMPRESS_AUTO;
	options.flags = 2;
	STATUS(pg_source_pack(f->source, "packed", PG_PIGG2, &options,
		&error), PG_INVALID);
	STATUS(pg_tree_pack(f->tree, "packed", PG_HOGG10, &options,
		&error), PG_INVALID);
	STATUS(pg_source_unpack(f->source, NULL, 0, &error), PG_INVALID);
	STATUS(pg_source_unpack(f->source, "out", 2, &error), PG_INVALID);
	STATUS(pg_tree_unpack(f->tree, NULL, 0, &error), PG_INVALID);
	STATUS(pg_tree_unpack(f->tree, "out", 2, &error), PG_INVALID);
	return 0;
}

static int builder_arguments(struct fixture *f)
{
	pg_archive_builder *builder = NULL;
	pg_error error;
	const uint32_t formats[] = { PG_AUTO, PG_LOOSE, UINT32_MAX };

	for (size_t i = 0; i < ARRAY_SIZE(formats); i++) {
		STATUS(pg_archive_builder_create(f->context, "output",
			formats[i], 0, &builder, &error), PG_INVALID);
		CHECK(builder == NULL);
	}
	STATUS(pg_archive_builder_create(f->context, NULL, PG_PIGG2, 0,
		&builder, &error), PG_INVALID);
	STATUS(pg_archive_builder_create(f->context, "", PG_PIGG2, 0,
		&builder, &error), PG_INVALID);
	STATUS(pg_archive_builder_create(f->context, "output", PG_PIGG2,
		2, &builder, &error), PG_INVALID);
	STATUS(pg_archive_builder_create(f->context, "output", PG_PIGG2,
		0, NULL, &error), PG_INVALID);
	CHECK(builder == NULL);
	STATUS(pg_archive_builder_write_all(f->builder, NULL, NULL, 0,
		NULL, &error), PG_INVALID);
	STATUS(pg_archive_builder_write_all(f->builder, "a", NULL, 1,
		NULL, &error), PG_INVALID);
	STATUS(pg_archive_builder_import(f->builder, NULL, "input", 0,
		&error), PG_INVALID);
	STATUS(pg_archive_builder_import(f->builder, "a", NULL, 0,
		&error), PG_INVALID);
	STATUS(pg_archive_builder_import(f->builder, "a", "input",
		UINT32_MAX, &error), PG_INVALID);
	STATUS(pg_archive_builder_copy(f->builder, NULL, f->file, 0,
		&error), PG_INVALID);
	STATUS(pg_archive_builder_copy(f->builder, "a", NULL, 0, &error),
		PG_INVALID);
	STATUS(pg_archive_builder_copy(f->builder, "a", f->file,
		UINT32_MAX, &error), PG_INVALID);
	STATUS(pg_archive_builder_write_all(f->builder, "a", NULL, 0,
		NULL, &error), PG_OK);
	CHECK(expect_absent("built") == 0);
	return 0;
}

static int unpack_arguments(struct fixture *f)
{
	pg_unpack_target *target = NULL;
	pg_writer *writer = NULL;
	pg_error error;

	STATUS(pg_unpack_target_open(f->cursor, NULL, 0, &target, &error),
		PG_INVALID);
	STATUS(pg_unpack_target_open(f->cursor, "", 0, &target, &error),
		PG_INVALID);
	STATUS(pg_unpack_target_open(f->cursor, "out", 2, &target, &error),
		PG_INVALID);
	STATUS(pg_unpack_target_open(f->cursor, "out", 0, NULL, &error),
		PG_INVALID);
	CHECK(target == NULL);
	STATUS(pg_unpack_target_open(f->cursor, "out", 0, &target, &error),
		PG_OK);
	STATUS(pg_writer_open_unpack(target, NULL, &writer, &error),
		PG_INVALID);
	CHECK(writer == NULL);
	STATUS(pg_writer_open_unpack(target, f->file, NULL, &error),
		PG_INVALID);
	STATUS(pg_unpack_target_close(&target, &error), PG_OK);
	CHECK(target == NULL);
	return 0;
}

static int foreign_arguments(struct fixture *f)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_file *file = NULL;
	pg_tree *tree = NULL;
	pg_unpack_target *target = NULL;
	pg_writer *writer = NULL;
	pg_error error;

	CHECK(clear_file("root/foreign") == 0);
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_source_open(context, "root", NULL, &source, &error), PG_OK);
	STATUS(pg_source_find(source, "a", &file, &error), PG_OK);
	STATUS(pg_tree_create(context, &tree, &error), PG_OK);
	STATUS(pg_tree_attach(f->tree, source, &error), PG_INVALID);
	STATUS(pg_tree_detach(f->tree, source, &error), PG_INVALID);
	STATUS(pg_tree_attach(tree, f->source, &error), PG_INVALID);
	STATUS(pg_archive_builder_copy(f->builder, "foreign", file,
		PG_COMPRESS_AUTO, &error), PG_INVALID);
	STATUS(pg_archive_builder_write_all(f->builder, "foreign", NULL, 0,
		NULL, &error), PG_OK);
	STATUS(pg_source_copy(f->source, "foreign", file, PG_COMPRESS_AUTO,
		&error), PG_INVALID);
	CHECK(expect_absent("root/foreign") == 0);
	STATUS(pg_unpack_target_open(f->cursor, "out", 0, &target, &error),
		PG_OK);
	STATUS(pg_writer_open_unpack(target, file, &writer, &error),
		PG_INVALID);
	CHECK(writer == NULL);
	STATUS(pg_unpack_target_close(&target, &error), PG_OK);
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_tree_close(&tree, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

int main(int argc, char **argv)
{
	static const struct {
		const char *name;
		int (*run)(struct fixture *);
	} cases[] = {
		{ "context", context_arguments },
		{ "source", source_arguments },
		{ "tree", tree_arguments },
		{ "file", file_arguments },
		{ "reader", reader_arguments },
		{ "writer", writer_arguments },
		{ "descriptor", descriptor_arguments },
		{ "named", named_arguments },
		{ "transfer", transfer_arguments },
		{ "pack", pack_arguments },
		{ "builder", builder_arguments },
		{ "unpack", unpack_arguments },
		{ "foreign", foreign_arguments }
	};

	if (argc != 2)
		return 2;
	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		if (strcmp(argv[1], cases[i].name) != 0)
			continue;
		struct fixture fixture;

		CHECK(fixture_open(&fixture) == 0);
		int result = cases[i].run(&fixture);
		int closed = fixture_close(&fixture);

		return result || closed;
	}
	return 2;
}
