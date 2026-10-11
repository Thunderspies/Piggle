#include "../api_test.hpp"

namespace {

static int options_boundaries(void)
{
	const uint64_t sizes[] = {0, 1, UINT32_MAX, UINT64_MAX};

	for (size_t i = 0; i < ARRAY_SIZE(sizes); i++) {
		pg_write_options options;

		memset(&options, 0xa5, sizeof(options));
		pg_write_options_init(&options, sizes[i]);
		PG_CHECK(options.logical_size == sizes[i]);
		PG_CHECK(options.input_size == sizes[i]);
		PG_CHECK(options.encoding == PG_LOGICAL);
		PG_CHECK(options.entry.mtime == 0);
		PG_CHECK(options.entry.compression == PG_COMPRESS_AUTO);
		PG_CHECK(options.entry.digest_kind == PG_DIGEST_NONE);
		PG_CHECK(options.entry.original_name == NULL);
		PG_CHECK(options.entry.cached_header == NULL);
		PG_CHECK(options.entry.cached_header_size == 0);
		for (size_t j = 0;
			j < sizeof(options.entry.expected_digest); j++)
			PG_CHECK(options.entry.expected_digest[j] == 0);
	}
	return 0;
}

static int name_boundaries(void)
{
	static const struct {
		const char *input;
		const char *output;
	} valid[] = {{"A", "a"}, {"A//./B/", "a/b"},
		{".\\A\\.\\B\\", "a/b"}, {"A\\//B", "a/b"},
		{".../.hidden/X..", ".../.hidden/x.."},
		{" Space / A B ", " space / a b "},
		{"A\xc3\x89/\xffZ", "a\xc3\x89/\xffz"},
		{"_-$@/0123", "_-$@/0123"}};
	static const char *invalid[] = {"", ".", "././", "..", "a/../b",
		"a\\..\\b", "a/..", "/a", "\\a", "//server/a",
		"\\\\server\\a", "C:/a", "C:a", "a:b", "a/b:c"};
	pg_context *context = NULL;
	pg_error error;
	unsigned char bytes[64];
	size_t required;

	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	for (size_t i = 0; i < ARRAY_SIZE(valid); i++) {
		size_t length = strlen(valid[i].output) + 1;

		for (size_t capacity = 0; capacity <= length + 1;
			capacity++) {
			memset(bytes, 0xa5, sizeof(bytes));
			required = SIZE_MAX;
			pg_status expected =
				capacity < length ? PG_CAPACITY : PG_OK;

			PG_STATUS(pg_name_normalize(context,
						valid[i].input,
						(char *)bytes + 1, capacity,
						&required, &error),
				expected);
			PG_CHECK(
				required == length && bytes[0] == 0xa5);
			if (expected == PG_OK) {
				PG_CHECK(strcmp((char *)bytes + 1,
							valid[i].output) == 0);
			}
			size_t untouched =
				expected == PG_OK ? length + 1 : 1;

			for (size_t j = untouched; j < sizeof(bytes);
				j++)
				PG_CHECK(bytes[j] == 0xa5);
		}
		PG_STATUS(pg_name_normalize(context, valid[i].input,
					NULL, 0, &required, &error),
			PG_CAPACITY);
		PG_CHECK(required == length);
	}
	for (size_t i = 0; i < ARRAY_SIZE(invalid); i++) {
		memset(bytes, 0xa5, sizeof(bytes));
		required = SIZE_MAX;
		PG_STATUS(pg_name_normalize(context, invalid[i],
					(char *)bytes, sizeof(bytes),
					&required, &error),
			PG_INVALID);
		PG_CHECK(required == 0);
		for (size_t j = 0; j < sizeof(bytes); j++)
			PG_CHECK(bytes[j] == 0xa5);
	}
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static int empty_closes(void)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_tree *tree = NULL;
	pg_file *file = NULL;
	pg_cursor *cursor = NULL;
	pg_reader *reader = NULL;
	pg_writer *writer = NULL;
	pg_unpack_target *target = NULL;
	pg_archive_builder *builder = NULL;
	pg_error error;

	for (unsigned int i = 0; i < 2; i++) {
		PG_STATUS(pg_context_close(&context, &error), PG_OK);
		PG_STATUS(pg_source_close(&source, &error), PG_OK);
		PG_STATUS(pg_tree_close(&tree, &error), PG_OK);
		PG_STATUS(pg_file_close(&file, &error), PG_OK);
		PG_STATUS(pg_cursor_close(&cursor, &error), PG_OK);
		PG_STATUS(pg_reader_close(&reader, &error), PG_OK);
		PG_STATUS(pg_writer_close(&writer, &error), PG_OK);
		PG_STATUS(
			pg_unpack_target_close(&target, &error), PG_OK);
		PG_STATUS(pg_archive_builder_close(&builder, &error),
			PG_OK);
		PG_CHECK(!context && !source && !tree && !file &&
			!cursor);
		PG_CHECK(!reader && !writer && !target && !builder);
	}
	PG_CHECK(pg_context_open(&context, NULL) == PG_OK);
	PG_CHECK(pg_context_close(&context, NULL) == PG_OK &&
		context == NULL);
	PG_CHECK(pg_source_close(&source, NULL) == PG_OK);
	PG_CHECK(pg_tree_close(&tree, NULL) == PG_OK);
	PG_CHECK(pg_file_close(&file, NULL) == PG_OK);
	PG_CHECK(pg_cursor_close(&cursor, NULL) == PG_OK);
	PG_CHECK(pg_reader_close(&reader, NULL) == PG_OK);
	PG_CHECK(pg_writer_close(&writer, NULL) == PG_OK);
	PG_CHECK(pg_unpack_target_close(&target, NULL) == PG_OK);
	PG_CHECK(pg_archive_builder_close(&builder, NULL) == PG_OK);
	return 0;
}

static pg_status read_all(struct fixture *f, unsigned int layer,
	int empty, void *bytes, size_t capacity, size_t *count,
	pg_error *error)
{
	if (layer == 0)
		return pg_file_read_all(
			f->file, bytes, capacity, count, error);
	if (layer == 1)
		return pg_source_read_all(f->source,
			empty ? "empty" : "A", bytes, capacity, count,
			error);
	return pg_tree_read_all(f->tree, empty ? "EMPTY" : "a", bytes,
		capacity, count, error);
}

static pg_status read_alloc(struct fixture *f, unsigned int layer,
	int empty, size_t limit, pg_buffer *buffer, pg_error *error)
{
	if (layer == 0)
		return pg_file_read_all_alloc(
			f->file, limit, buffer, error);
	if (layer == 1)
		return pg_source_read_all_alloc(f->source,
			empty ? "empty" : "A", limit, buffer, error);
	return pg_tree_read_all_alloc(
		f->tree, empty ? "EMPTY" : "a", limit, buffer, error);
}

static int read_boundaries(void)
{
	struct fixture f;
	pg_error error;
	unsigned char bytes[sizeof(payload) + 2];
	pg_buffer buffer = {NULL, 0};
	pg_buffer owned = {NULL, 0};

	PG_CHECK(fixture_open(&f) == 0);
	for (unsigned int layer = 0; layer < 3; layer++) {
		for (size_t capacity = 0;
			capacity <= sizeof(payload) + 1; capacity++) {
			memset(bytes, 0xa5, sizeof(bytes));
			size_t count = SIZE_MAX;
			pg_status expected = capacity < sizeof(payload)
				? PG_CAPACITY
				: PG_OK;

			PG_STATUS(read_all(&f, layer, 0, bytes + 1,
						capacity, &count, &error),
				expected);
			PG_CHECK(count ==
				(expected == PG_OK ? sizeof(payload)
							: 0));
			PG_CHECK(bytes[0] == 0xa5);
			if (count)
				PG_CHECK(memcmp(bytes + 1, payload,
							count) == 0);
			for (size_t j = count + 1; j < sizeof(bytes);
				j++)
				PG_CHECK(bytes[j] == 0xa5);
		}
		const size_t limits[] = {0, sizeof(payload) - 1,
			sizeof(payload), SIZE_MAX};

		for (size_t i = 0; i < ARRAY_SIZE(limits); i++) {
			pg_status expected = limits[i] < sizeof(payload)
				? PG_LIMIT
				: PG_OK;

			PG_STATUS(read_alloc(&f, layer, 0, limits[i],
						&buffer, &error),
				expected);
			if (expected == PG_OK) {
				PG_CHECK(
					buffer.size == sizeof(payload));
				PG_CHECK(memcmp(buffer.data, payload,
							buffer.size) == 0);
			} else {
				PG_CHECK(buffer.data == NULL &&
					buffer.size == 0);
			}
			pg_buffer_free(&buffer);
			PG_CHECK(buffer.data == NULL &&
				buffer.size == 0);
		}
	}
	PG_STATUS(pg_file_read_all_alloc(
				f.file, sizeof(payload), &owned, &error),
		PG_OK);
	PG_STATUS(pg_file_close(&f.file, &error), PG_OK);
	PG_STATUS(pg_source_find(f.source, "empty", &f.file, &error),
		PG_OK);
	for (unsigned int layer = 0; layer < 3; layer++) {
		size_t count = SIZE_MAX;

		PG_STATUS(
			read_all(&f, layer, 1, NULL, 0, &count, &error),
			PG_OK);
		PG_CHECK(count == 0);
		memset(bytes, 0xa5, sizeof(bytes));
		PG_STATUS(read_all(&f, layer, 1, bytes, 0, &count,
					&error),
			PG_OK);
		PG_CHECK(count == 0 && bytes[0] == 0xa5);
		PG_STATUS(read_all(&f, layer, 1, bytes, sizeof(bytes),
					&count, &error),
			PG_OK);
		PG_CHECK(count == 0);
		for (size_t i = 0; i < sizeof(bytes); i++)
			PG_CHECK(bytes[i] == 0xa5);
		PG_STATUS(read_alloc(&f, layer, 1, 0, &buffer, &error),
			PG_OK);
		PG_CHECK(buffer.data == NULL && buffer.size == 0);
	}
	PG_CHECK(fixture_close(&f) == 0);
	PG_CHECK(memcmp(owned.data, payload, owned.size) == 0);
	((unsigned char *)owned.data)[0] = 42;
	buffer = owned;
	owned.data = NULL;
	owned.size = 0;
	PG_CHECK(((unsigned char *)buffer.data)[0] == 42);
	memset(&error, 0xa5, sizeof(error));
	pg_error saved;

	memcpy(&saved, &error, sizeof(saved));
	pg_buffer_free(&buffer);
	pg_buffer_free(&buffer);
	pg_buffer_free(&owned);
	PG_CHECK(buffer.data == NULL && buffer.size == 0);
	PG_CHECK(memcmp(&error, &saved, sizeof(error)) == 0);
	return 0;
}

static int reader_lifetime(void)
{
	struct fixture f;
	pg_reader *readers[4] = {NULL, NULL, NULL, NULL};
	pg_error error;
	unsigned char bytes[sizeof(payload) + 1];
	size_t count;

	PG_CHECK(fixture_open(&f) == 0);
	PG_STATUS(pg_reader_open(
				f.file, PG_READ_LOGICAL, &readers[0], &error),
		PG_OK);
	PG_STATUS(pg_reader_open_source(f.source, "A", PG_READ_STORED,
				&readers[1], &error),
		PG_OK);
	PG_STATUS(pg_reader_open_tree(f.tree, "a", PG_READ_LOGICAL,
				&readers[2], &error),
		PG_OK);
	PG_STATUS(pg_reader_open_native(
				f.context, "root/a", &readers[3], &error),
		PG_OK);
	for (size_t i = 0; i < ARRAY_SIZE(readers); i++) {
		pg_reader_info info;

		PG_STATUS(pg_reader_inspect(readers[i], &info, &error),
			PG_OK);
		PG_CHECK(info.size == sizeof(payload));
		PG_CHECK(info.logical_size == sizeof(payload));
		PG_CHECK(info.encoding == PG_LOGICAL);
		PG_STATUS(pg_reader_read(
					readers[i], bytes, 1, &count, &error),
			PG_OK);
		PG_CHECK(count == 1 && bytes[0] == payload[0]);
	}
	PG_STATUS(pg_file_read_all(
				f.file, bytes, sizeof(bytes), &count, &error),
		PG_OK);
	PG_CHECK(count == sizeof(payload));
	PG_STATUS(pg_file_close(&f.file, &error), PG_OK);
	PG_STATUS(pg_cursor_close(&f.cursor, &error), PG_OK);
	PG_STATUS(pg_tree_close(&f.tree, &error), PG_OK);
	PG_STATUS(pg_source_close(&f.source, &error), PG_OK);
	PG_STATUS(pg_archive_builder_close(&f.builder, &error), PG_OK);
	pg_context *original = f.context;

	PG_STATUS(pg_context_close(&f.context, &error), PG_BUSY);
	PG_CHECK(f.context == original);
	for (size_t i = 0; i < ARRAY_SIZE(readers); i++) {
		PG_STATUS(pg_reader_read(
					readers[i], NULL, 0, &count, &error),
			PG_OK);
		PG_CHECK(count == 0);
		PG_STATUS(pg_reader_read(readers[i], bytes,
					sizeof(bytes), &count, &error),
			PG_OK);
		PG_CHECK(count == sizeof(payload) - 1);
		PG_CHECK(memcmp(bytes, payload + 1, count) == 0);
		for (unsigned int j = 0; j < 2; j++) {
			memset(bytes, 0xa5, sizeof(bytes));
			PG_STATUS(
				pg_reader_read(readers[i], bytes,
					sizeof(bytes), &count, &error),
				PG_END);
			PG_CHECK(count == 0 && bytes[0] == 0xa5);
		}
		PG_STATUS(pg_reader_read(
					readers[i], NULL, 0, &count, &error),
			PG_OK);
		PG_CHECK(count == 0);
		PG_STATUS(pg_reader_close(&readers[i], &error), PG_OK);
		PG_CHECK(readers[i] == NULL);
	}
	PG_CHECK(fixture_close(&f) == 0);
	return 0;
}

static int stale_metadata(void)
{
	struct fixture f;
	pg_file_info before, after;
	pg_source_info source_before, source_after;
	pg_file *file = NULL;
	pg_reader *reader = NULL;
	pg_buffer buffer = {NULL, 0};
	pg_error error;

	PG_CHECK(fixture_open(&f) == 0);
	PG_STATUS(pg_file_inspect(f.file, &before, &error), PG_OK);
	PG_STATUS(pg_source_inspect(f.source, &source_before, &error),
		PG_OK);
	PG_CHECK(before.source_id == source_before.id &&
		before.copy_id != 0);
	PG_CHECK(before.archive_record == UINT64_MAX);
	PG_CHECK(before.cached_header == NULL &&
		before.cached_header_size == 0);
	PG_CHECK(before.digest_kind == PG_DIGEST_NONE);
	PG_STATUS(pg_source_rescan(f.source, &error), PG_OK);
	PG_STATUS(pg_source_inspect(f.source, &source_after, &error),
		PG_OK);
	PG_CHECK(source_after.id == source_before.id);
	PG_CHECK(source_after.generation == source_before.generation);
	PG_STATUS(pg_tree_rescan(f.tree, &error), PG_OK);
	PG_STATUS(pg_tree_find(f.tree, "a", &file, &error), PG_OK);
	PG_STATUS(pg_file_inspect(file, &after, &error), PG_OK);
	PG_CHECK(after.copy_id == before.copy_id);
	PG_CHECK(after.copy_generation == before.copy_generation);
	PG_CHECK(after.archive_record == UINT64_MAX);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_CHECK(put_bytes("root/a", "replacement", 11) == 0);
	PG_STATUS(pg_source_rescan(f.source, &error), PG_OK);
	PG_STATUS(pg_file_inspect(f.file, &after, &error), PG_OK);
	PG_CHECK(after.logical_size == before.logical_size);
	PG_CHECK(after.copy_generation == before.copy_generation);
	PG_CHECK(strcmp(after.canonical_name, "a") == 0);
	PG_STATUS(pg_reader_open(
				f.file, PG_READ_LOGICAL, &reader, &error),
		PG_STALE);
	PG_CHECK(reader == NULL);
	PG_STATUS(pg_file_read_all_alloc(
				f.file, SIZE_MAX, &buffer, &error),
		PG_STALE);
	PG_CHECK(buffer.data == NULL && buffer.size == 0);
	PG_STATUS(pg_file_write_all(f.file, "x", 1, NULL, &error),
		PG_STALE);
	PG_STATUS(pg_file_delete(f.file, &error), PG_STALE);
	PG_CHECK(expect_bytes("root/a", "replacement", 11) == 0);
	PG_STATUS(pg_source_find(f.source, "a", &file, &error), PG_OK);
	PG_STATUS(pg_file_inspect(file, &after, &error), PG_OK);
	PG_CHECK(after.logical_size == 11);
	PG_CHECK(after.copy_generation != before.copy_generation);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	/* The cursor retains the old selection after both parents
	 * close. */
	PG_STATUS(pg_tree_close(&f.tree, &error), PG_OK);
	PG_STATUS(pg_source_close(&f.source, &error), PG_OK);
	PG_STATUS(pg_cursor_next(f.cursor, &file, &error), PG_OK);
	PG_STATUS(pg_cursor_close(&f.cursor, &error), PG_OK);
	PG_STATUS(pg_file_inspect(file, &after, &error), PG_OK);
	PG_CHECK(after.copy_id == before.copy_id);
	PG_CHECK(after.copy_generation == before.copy_generation);
	PG_CHECK(after.logical_size == before.logical_size);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_CHECK(fixture_close(&f) == 0);
	return 0;
}

static int tree_rollback(void)
{
	pg_context *context = NULL;
	pg_tree *tree = NULL;
	pg_source *source = NULL;
	pg_source *reference = NULL;
	pg_tree_info info;
	pg_source_info first, second;
	pg_error error;
	pg_source_spec specs[] = {{"first", {PG_AUTO, PG_READ, 0}},
		{"missing", {PG_AUTO, PG_READ, 0}}};

	PG_CHECK(directory("first") == 0);
	PG_CHECK(directory("second") == 0);
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(pg_tree_open(context, specs, ARRAY_SIZE(specs), &tree,
				&error),
		PG_NOT_FOUND);
	PG_CHECK(tree == NULL);
	/* Reopening proves rollback released the first source's alias
	 * lease. */
	PG_STATUS(
		pg_source_open(context, "first", NULL, &source, &error),
		PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	specs[1].native_path = "first";
	PG_STATUS(pg_tree_open(context, specs, ARRAY_SIZE(specs), &tree,
				&error),
		PG_BUSY);
	PG_CHECK(tree == NULL);
	specs[1].native_path = "second";
	PG_STATUS(pg_tree_open(context, specs, ARRAY_SIZE(specs), &tree,
				&error),
		PG_OK);
	PG_STATUS(pg_tree_inspect(tree, &info, &error), PG_OK);
	PG_CHECK(info.source_count == 2 &&
		info.watch_mode == PG_WATCH_OFF);
	PG_STATUS(pg_tree_source(tree, 0, &source, &error), PG_OK);
	PG_STATUS(pg_source_inspect(source, &first, &error), PG_OK);
	PG_STATUS(pg_tree_source(tree, 1, &reference, &error), PG_OK);
	PG_STATUS(pg_source_inspect(reference, &second, &error), PG_OK);
	PG_CHECK(first.id != second.id);
	PG_STATUS(pg_tree_attach(tree, source, &error), PG_BUSY);
	PG_STATUS(pg_tree_detach(tree, source, &error), PG_OK);
	PG_STATUS(
		pg_tree_detach(tree, source, &error), PG_NOT_ATTACHED);
	PG_STATUS(pg_tree_attach(tree, source, &error), PG_OK);
	PG_STATUS(pg_source_close(&reference, &error), PG_OK);
	PG_STATUS(pg_tree_source(tree, 0, &reference, &error), PG_OK);
	PG_STATUS(pg_source_inspect(reference, &first, &error), PG_OK);
	PG_CHECK(first.id == second.id);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_tree_close(&tree, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_BUSY);
	PG_STATUS(pg_source_inspect(reference, &first, &error), PG_OK);
	PG_CHECK(first.id == second.id);
	PG_STATUS(pg_source_close(&reference, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

using ApiContract = piggle_test::ContractTest;

TEST_F(ApiContract, api_contract_options)
{
	ASSERT_EQ(0, options_boundaries());
}

TEST_F(ApiContract, api_contract_names)
{
	ASSERT_EQ(0, name_boundaries());
}

TEST_F(ApiContract, api_contract_closes)
{
	ASSERT_EQ(0, empty_closes());
}

TEST_F(ApiContract, api_contract_reads)
{
	ASSERT_EQ(0, read_boundaries());
}

TEST_F(ApiContract, api_contract_readers)
{
	ASSERT_EQ(0, reader_lifetime());
}

TEST_F(ApiContract, api_contract_metadata)
{
	ASSERT_EQ(0, stale_metadata());
}

TEST_F(ApiContract, api_contract_tree)
{
	ASSERT_EQ(0, tree_rollback());
}

} // namespace
