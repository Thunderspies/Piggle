#include "../api_test.hpp"

namespace {

static pg_status export_file(struct fixture *f, unsigned int layer,
	uint32_t flags, pg_error *error)
{
	if (layer == 0)
		return pg_file_export(
			f->file, "exported", flags, error);
	if (layer == 1)
		return pg_source_export(
			f->source, "a", "exported", flags, error);
	return pg_tree_export(f->tree, "a", "exported", flags, error);
}

static int roundtrip(uint32_t format, int overlay)
{
	static const unsigned char patch_bytes[] = {0, 42};
	struct fixture f;
	pg_source *patch = NULL;
	pg_source *archive = NULL;
	pg_file *file = NULL;
	pg_cursor *cursor = NULL;
	pg_reader *reader = NULL;
	pg_reader_info native_info;
	pg_file_info info;
	pg_source_options writable = {PG_AUTO, PG_WRITE, 0};
	pg_pack_options options = {PG_COMPRESS_FORCE, 0, 0};
	pg_error error;
	unsigned char bytes[32];
	size_t count;

	PG_CHECK(fixture_open(&f) == 0);
	PG_CHECK(clear_file("root/copied") == 0);
	PG_CHECK(clear_file("root/imported") == 0);
	PG_STATUS(pg_source_request_subtree(f.source, NULL, &error),
		PG_OK);
	PG_STATUS(pg_reader_open_native(
				f.context, "input", &reader, &error),
		PG_OK);
	PG_STATUS(
		pg_reader_inspect(reader, &native_info, &error), PG_OK);
	PG_STATUS(pg_reader_close(&reader, &error), PG_OK);
	PG_STATUS(pg_source_import(f.source, "Imported", "input",
				PG_COMPRESS_NEVER, &error),
		PG_OK);
	PG_STATUS(pg_source_find(f.source, "imported", &file, &error),
		PG_OK);
	PG_STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	PG_CHECK(info.mtime == native_info.mtime);
	PG_STATUS(pg_source_copy(f.source, "Copied", file,
				PG_COMPRESS_AUTO, &error),
		PG_OK);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_CHECK(expect_bytes(
				"root/copied", payload, sizeof(payload)) == 0);
	PG_CHECK(expect_bytes("root/imported", payload,
				sizeof(payload)) == 0);
	PG_STATUS(pg_source_copy(f.source, "A", f.file,
				PG_COMPRESS_AUTO, &error),
		PG_CONFLICT);
	PG_STATUS(pg_source_import(f.source, "a", "root/a",
				PG_COMPRESS_AUTO, &error),
		PG_CONFLICT);
	for (unsigned int layer = 0; layer < 3; layer++) {
		PG_CHECK(clear_file("exported") == 0);
		PG_STATUS(export_file(&f, layer, 0, &error), PG_OK);
		PG_CHECK(expect_bytes("exported", payload,
					sizeof(payload)) == 0);
		PG_CHECK(put_bytes("exported", "old", 3) == 0);
		PG_STATUS(export_file(&f, layer, 0, &error), PG_EXISTS);
		PG_CHECK(expect_bytes("exported", "old", 3) == 0);
		PG_STATUS(export_file(&f, layer, PG_OVERWRITE, &error),
			PG_OK);
		PG_CHECK(expect_bytes("exported", payload,
					sizeof(payload)) == 0);
	}
	if (overlay) {
		PG_CHECK(directory("patch") == 0);
		PG_CHECK(put_bytes("patch/a", patch_bytes,
					sizeof(patch_bytes)) == 0);
		PG_STATUS(pg_source_open(f.context, "patch", NULL,
					&patch, &error),
			PG_OK);
		PG_STATUS(pg_tree_attach(f.tree, patch, &error), PG_OK);
	}
	PG_CHECK(clear_file("packed") == 0);
	if (overlay) {
		PG_STATUS(pg_tree_pack(f.tree, "packed", format,
					&options, &error),
			PG_OK);
		PG_STATUS(pg_tree_pack(f.tree, "packed", format,
					&options, &error),
			PG_EXISTS);
	} else {
		PG_STATUS(pg_source_pack(f.source, "packed", format,
					&options, &error),
			PG_OK);
		PG_STATUS(pg_source_pack(f.source, "packed", format,
					&options, &error),
			PG_EXISTS);
	}
	PG_STATUS(pg_source_open(f.context, "packed", &writable,
				&archive, &error),
		PG_OK);
	PG_STATUS(pg_source_validate(archive, &error), PG_OK);
	const char *names[] = {"a", "copied", "empty", "imported"};

	PG_STATUS(
		pg_source_files(archive, NULL, &cursor, &error), PG_OK);
	for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
		PG_STATUS(pg_cursor_next(cursor, &file, &error), PG_OK);
		PG_STATUS(pg_file_inspect(file, &info, &error), PG_OK);
		PG_CHECK(strcmp(info.canonical_name, names[i]) == 0);
		const unsigned char *expected =
			overlay && i == 0 ? patch_bytes : payload;
		size_t size = i == 2	    ? 0
			: overlay && i == 0 ? sizeof(patch_bytes)
						: sizeof(payload);

		PG_STATUS(pg_file_read_all(file, bytes, sizeof(bytes),
					&count, &error),
			PG_OK);
		PG_CHECK(count == size &&
			memcmp(bytes, expected, size) == 0);
		PG_STATUS(pg_file_verify(file, &error), PG_OK);
		PG_STATUS(pg_file_close(&file, &error), PG_OK);
	}
	PG_STATUS(pg_cursor_next(cursor, &file, &error), PG_END);
	PG_CHECK(file == NULL);
	PG_STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
		char path[32];

		PG_CHECK(snprintf(path, sizeof(path), "out/%s",
					names[i]) > 0);
		PG_CHECK(clear_file(path) == 0);
	}
	if (overlay) {
		PG_STATUS(pg_tree_unpack(f.tree, "out", 0, &error),
			PG_OK);
		PG_STATUS(pg_tree_unpack(f.tree, "out", 0, &error),
			PG_EXISTS);
		PG_STATUS(pg_tree_unpack(
					f.tree, "out", PG_OVERWRITE, &error),
			PG_OK);
	} else {
		PG_STATUS(pg_source_unpack(archive, "out", 0, &error),
			PG_OK);
		PG_STATUS(pg_source_unpack(archive, "out", 0, &error),
			PG_EXISTS);
		PG_STATUS(pg_source_unpack(
					archive, "out", PG_OVERWRITE, &error),
			PG_OK);
	}
	PG_CHECK(expect_bytes("out/a", overlay ? patch_bytes : payload,
				overlay ? sizeof(patch_bytes)
					: sizeof(payload)) == 0);
	PG_CHECK(expect_bytes("out/copied", payload, sizeof(payload)) ==
		0);
	PG_CHECK(expect_bytes("out/imported", payload,
				sizeof(payload)) == 0);
	PG_CHECK(expect_bytes("out/empty", NULL, 0) == 0);
	PG_STATUS(pg_source_find(archive, "a", &file, &error), PG_OK);
	PG_STATUS(pg_source_delete(archive, &error), PG_OK);
	PG_CHECK(expect_absent("packed") == 0);
	PG_STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	PG_CHECK(strcmp(info.canonical_name, "a") == 0);
	PG_STATUS(pg_source_files(archive, NULL, &cursor, &error),
		PG_STALE);
	PG_CHECK(cursor == NULL);
	PG_STATUS(pg_source_request_subtree(archive, NULL, &error),
		PG_STALE);
	PG_STATUS(pg_source_rescan(archive, &error), PG_STALE);
	PG_STATUS(pg_source_validate(archive, &error), PG_STALE);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_source_close(&archive, &error), PG_OK);
	PG_STATUS(pg_source_close(&patch, &error), PG_OK);
	PG_CHECK(fixture_close(&f) == 0);
	return 0;
}

static int empty_archive(uint32_t format)
{
	pg_context *context = NULL;
	pg_tree *tree = NULL;
	pg_source *source = NULL;
	pg_cursor *cursor = NULL;
	pg_file *file = NULL;
	pg_error error;

	PG_CHECK(clear_file("empty_archive") == 0);
	PG_CHECK(directory("out") == 0);
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	/* A non-NULL zero-length source array is also a valid empty
	 * tree. */
	pg_source_spec unused = {NULL, {UINT32_MAX, UINT32_MAX, 0}};

	PG_STATUS(pg_tree_open(context, &unused, 0, &tree, &error),
		PG_OK);
	PG_STATUS(pg_tree_pack(
				tree, "empty_archive", format, NULL, &error),
		PG_OK);
	PG_STATUS(pg_tree_unpack(tree, "out", 0, &error), PG_OK);
	PG_STATUS(pg_tree_close(&tree, &error), PG_OK);
	PG_STATUS(pg_source_open(context, "empty_archive", NULL,
				&source, &error),
		PG_OK);
	PG_STATUS(pg_source_validate(source, &error), PG_OK);
	PG_STATUS(pg_source_files(source, "", &cursor, &error), PG_OK);
	PG_STATUS(pg_cursor_next(cursor, &file, &error), PG_END);
	PG_CHECK(file == NULL);
	PG_STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	PG_STATUS(pg_source_unpack(source, "out", 0, &error), PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	if (format == PG_HOGG10) {
		PG_STATUS(pg_source_recover(
					context, "empty_archive", &error),
			PG_OK);
		PG_STATUS(pg_source_recover(
					context, "empty_archive", &error),
			PG_OK);
	} else {
		PG_STATUS(pg_source_recover(
					context, "empty_archive", &error),
			PG_UNSUPPORTED);
	}
	PG_STATUS(pg_source_open(context, "empty_archive", NULL,
				&source, &error),
		PG_OK);
	PG_STATUS(pg_source_validate(source, &error), PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

using ApiRoundtrip = piggle_test::ContractTest;

TEST_F(ApiRoundtrip, api_roundtrip_source_pigg)
{
	ASSERT_EQ(0, roundtrip(PG_PIGG2, 0));
}

TEST_F(ApiRoundtrip, api_roundtrip_source_hogg)
{
	ASSERT_EQ(0, roundtrip(PG_HOGG10, 0));
}

TEST_F(ApiRoundtrip, api_roundtrip_tree_pigg)
{
	ASSERT_EQ(0, roundtrip(PG_PIGG2, 1));
}

TEST_F(ApiRoundtrip, api_roundtrip_tree_hogg)
{
	ASSERT_EQ(0, roundtrip(PG_HOGG10, 1));
}

TEST_F(ApiRoundtrip, api_roundtrip_empty_pigg)
{
	ASSERT_EQ(0, empty_archive(PG_PIGG2));
}

TEST_F(ApiRoundtrip, api_roundtrip_empty_hogg)
{
	ASSERT_EQ(0, empty_archive(PG_HOGG10));
}

} // namespace
