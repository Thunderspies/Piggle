#include "../api_test.hpp"

namespace {

static int loose_case(void)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_file *file = NULL;
	pg_reader *reader = NULL;
	pg_source_options options = {PG_LOOSE, PG_WRITE, 0};
	pg_metadata_options edit = {PG_METADATA_MTIME, 123, NULL, 0};
	pg_error error;
	pg_file_info info;
	unsigned char bytes[32];
	size_t count;

	PG_CHECK(!directory("root"));
	PG_CHECK(!put_bytes("root/a", payload, sizeof(payload)));
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(pg_source_open(
				context, "root", &options, &source, &error),
		PG_OK);
	PG_STATUS(pg_source_find(source, "a", &file, &error), PG_OK);
	PG_STATUS(
		pg_reader_open(file, PG_READ_LOGICAL, &reader, &error),
		PG_OK);
	PG_STATUS(pg_file_update_metadata(file, &edit, &error), PG_OK);
	PG_STATUS(pg_reader_seek(reader, 0, &error), PG_STALE);
	PG_STATUS(pg_reader_close(&reader, &error), PG_OK);
	PG_STATUS(
		pg_file_update_metadata(file, &edit, &error), PG_STALE);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_source_find(source, "a", &file, &error), PG_OK);
	PG_STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	PG_CHECK(info.mtime == 123);
	PG_STATUS(pg_file_read_all(
				file, bytes, sizeof(bytes), &count, &error),
		PG_OK);
	PG_CHECK(count == sizeof(payload) &&
		!memcmp(bytes, payload, count));
	PG_STATUS(pg_file_update_metadata(file, &edit, &error), PG_OK);
	edit.fields = PG_METADATA_HEADER;
	PG_STATUS(pg_file_update_metadata(file, &edit, &error),
		PG_UNSUPPORTED);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static int archive_metadata(uint32_t format)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_archive_builder *builder = NULL;
	pg_file *file = NULL;
	pg_reader *reader = NULL;
	pg_error error;
	pg_entry_options entry = {};
	pg_metadata_options edit = {PG_METADATA_MTIME, 123, NULL, 0};
	pg_source_options options = {PG_AUTO, PG_WRITE, 0};
	pg_file_info before, after;
	unsigned char digest[16], old[128], current[128];
	struct stat initial, changed;
	size_t size, count;

	PG_CHECK(!clear_file("metadata"));
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(pg_archive_builder_create(context, "metadata", format,
				0, &builder, &error),
		PG_OK);
	entry.compression = PG_COMPRESS_FORCE;
	entry.cached_header = "header";
	entry.cached_header_size = 6;
	PG_STATUS(pg_archive_builder_write_all(builder, "a", payload,
				sizeof(payload), &entry, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	PG_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	PG_STATUS(pg_source_open(context, "metadata", &options, &source,
				&error),
		PG_OK);
	PG_STATUS(pg_source_find(source, "a", &file, &error), PG_OK);
	PG_STATUS(pg_file_inspect(file, &before, &error), PG_OK);
	memcpy(digest, before.digest, sizeof(digest));
	PG_STATUS(pg_reader_open(file, PG_READ_STORED, &reader, &error),
		PG_OK);
	PG_STATUS(
		pg_reader_read(reader, old, sizeof(old), &size, &error),
		PG_OK);
	if (format == PG_PIGG2)
		PG_STATUS(pg_reader_close(&reader, &error), PG_OK);
	PG_STATUS(pg_file_update_metadata(NULL, &edit, &error),
		PG_INVALID);
	PG_STATUS(pg_file_update_metadata(file, NULL, &error),
		PG_INVALID);
	edit.fields = 4;
	PG_STATUS(pg_file_update_metadata(file, &edit, &error),
		PG_INVALID);
	edit.fields = PG_METADATA_MTIME;
	edit.mtime = INT64_MAX;
	PG_STATUS(
		pg_file_update_metadata(file, &edit, &error), PG_LIMIT);
	edit.fields = 0;
	PG_STATUS(pg_file_update_metadata(file, &edit, &error), PG_OK);
	PG_STATUS(pg_file_verify(file, &error), PG_OK);
	edit.fields = PG_METADATA_MTIME;
	edit.mtime = 123;
	PG_CHECK(!stat("metadata", &initial));
	PG_STATUS(pg_file_update_metadata(file, &edit, &error), PG_OK);
	PG_CHECK(!stat("metadata", &changed));
	if (format == PG_HOGG10) {
		PG_CHECK(initial.st_size == changed.st_size);
		PG_STATUS(pg_reader_seek(reader, 0, &error), PG_STALE);
		PG_STATUS(pg_reader_close(&reader, &error), PG_OK);
	}
	PG_STATUS(
		pg_file_update_metadata(file, &edit, &error), PG_STALE);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_source_find(source, "a", &file, &error), PG_OK);
	PG_STATUS(pg_file_inspect(file, &after, &error), PG_OK);
	PG_CHECK(after.mtime == 123 && after.cached_header_size == 6);
	PG_CHECK(!memcmp(digest, after.digest, sizeof(digest)));
	edit.fields = PG_METADATA_HEADER;
	PG_STATUS(pg_file_update_metadata(file, &edit, &error), PG_OK);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_source_find(source, "a", &file, &error), PG_OK);
	PG_STATUS(pg_file_inspect(file, &after, &error), PG_OK);
	PG_CHECK(after.mtime == 123 && after.cached_header_size == 0);
	PG_CHECK(!memcmp(digest, after.digest, sizeof(digest)));
	PG_STATUS(pg_reader_open(file, PG_READ_STORED, &reader, &error),
		PG_OK);
	PG_STATUS(pg_reader_read(reader, current, sizeof(current),
				&count, &error),
		PG_OK);
	PG_CHECK(size == count && !memcmp(old, current, size));
	PG_STATUS(pg_reader_close(&reader, &error), PG_OK);
	PG_STATUS(pg_file_verify(file, &error), PG_OK);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_source_validate(source, &error), PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

using ApiMetadata = piggle_test::ContractTest;

TEST_F(ApiMetadata, api_metadata_pigg)
{
	ASSERT_EQ(0, archive_metadata(PG_PIGG2));
}

TEST_F(ApiMetadata, api_metadata_hogg)
{
	ASSERT_EQ(0, archive_metadata(PG_HOGG10));
}

TEST_F(ApiMetadata, api_metadata_loose)
{
	ASSERT_EQ(0, loose_case());
}

TEST_F(ApiMetadata, ReusedSlotRejectsOldCopyAndPreservesOtherReaders)
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_source *source = NULL;
	pg_file *old = NULL, *keep = NULL, *current = NULL;
	pg_reader *reader = NULL, *rejected = NULL;
	pg_source_options options = {PG_HOGG10, PG_WRITE, 0};

	ASSERT_EQ(PG_OK, pg_context_open(&context, NULL));
	ASSERT_EQ(PG_OK, pg_archive_builder_create(context, "reuse.hogg",
		PG_HOGG10, 0, &builder, NULL));
	for (const char *name : {"old", "keep"})
		ASSERT_EQ(PG_OK, pg_archive_builder_write_all(builder, name,
			"safe", 4, NULL, NULL));
	ASSERT_EQ(PG_OK, pg_archive_builder_finish(builder, NULL));
	ASSERT_EQ(PG_OK, pg_archive_builder_close(&builder, NULL));
	ASSERT_EQ(PG_OK, pg_source_open(context, "reuse.hogg", &options,
		&source, NULL));
	ASSERT_EQ(PG_OK, pg_source_find(source, "old", &old, NULL));
	ASSERT_EQ(PG_OK, pg_source_find(source, "keep", &keep, NULL));
	ASSERT_EQ(PG_OK, pg_reader_open(keep, PG_READ_LOGICAL_PAYLOAD,
		&reader, NULL));
	pg_file_info before = {}, after = {};

	ASSERT_EQ(PG_OK, pg_file_inspect(old, &before, NULL));
	ASSERT_EQ(PG_OK, pg_file_delete(old, NULL));
	ASSERT_EQ(PG_OK, pg_source_write_all(source, "new", "next", 4,
		NULL, NULL));
	ASSERT_EQ(PG_OK, pg_source_find(source, "new", &current, NULL));
	ASSERT_EQ(PG_OK, pg_file_inspect(current, &after, NULL));
	EXPECT_EQ(before.archive_record, after.archive_record);
	EXPECT_NE(before.copy_id, after.copy_id);
	ASSERT_EQ(PG_STALE, pg_reader_open(old, PG_READ_LOGICAL_PAYLOAD,
		&rejected, NULL));
	ASSERT_EQ(PG_OK, pg_source_rescan(source, NULL));
	char bytes[4];
	size_t count = 0;

	ASSERT_EQ(PG_OK, pg_reader_read(reader, bytes, sizeof(bytes),
		&count, NULL));
	EXPECT_EQ(4u, count);
	EXPECT_EQ(0, memcmp(bytes, "safe", 4));
	ASSERT_EQ(PG_OK, pg_reader_close(&reader, NULL));
	for (pg_file **file : {&old, &keep, &current})
		ASSERT_EQ(PG_OK, pg_file_close(file, NULL));
	ASSERT_EQ(PG_OK, pg_source_close(&source, NULL));
	ASSERT_EQ(PG_OK, pg_context_close(&context, NULL));
}

TEST_F(ApiMetadata, CapturedHeadersSurviveReplacementAndRescan)
{
	for (uint32_t format : {PG_PIGG2, PG_HOGG10}) {
		pg_context *context = NULL;
		pg_source *source = NULL;
		pg_archive_builder *builder = NULL;
		pg_cursor *cursor = NULL;
		pg_file *old = NULL, *captured = NULL, *current = NULL;
		pg_error error = {};
		std::vector<unsigned char> first(131073), second(131073);

		for (size_t i = 0; i < first.size(); ++i) {
			first[i] = (unsigned char)(i * 13 + i / 251);
			second[i] = (unsigned char)(first[i] ^ 0xa5);
		}
		pg_entry_options entry = {};
		entry.cached_header = first.data();
		entry.cached_header_size = first.size();
		pg_source_options options = {format, PG_WRITE, 0};
		ASSERT_EQ(0, clear_file("retained-headers"));
		ASSERT_EQ(PG_OK, pg_context_open(&context, &error));
		ASSERT_EQ(PG_OK, pg_archive_builder_create(context,
			"retained-headers", format, 0, &builder, &error));
		ASSERT_EQ(PG_OK, pg_archive_builder_write_all(builder, "a",
			payload, sizeof(payload), &entry, &error));
		ASSERT_EQ(PG_OK, pg_archive_builder_finish(builder, &error));
		ASSERT_EQ(PG_OK, pg_archive_builder_close(&builder, &error));
		ASSERT_EQ(PG_OK, pg_source_open(context, "retained-headers",
			&options, &source, &error));
		ASSERT_EQ(PG_OK, pg_source_find(source, "a", &old, &error));
		ASSERT_EQ(PG_OK, pg_source_files(source, NULL, &cursor,
			&error));
		pg_file_info original = {};
		ASSERT_EQ(PG_OK, pg_file_inspect(old, &original, &error));
		pg_metadata_options edit = {PG_METADATA_HEADER, 0,
			second.data(), second.size()};
		ASSERT_EQ(PG_OK, pg_file_update_metadata(old, &edit, &error));
		ASSERT_EQ(PG_OK, pg_source_rescan(source, &error));
		ASSERT_EQ(PG_OK, pg_source_find(source, "a", &current, &error));
		ASSERT_EQ(PG_OK, pg_cursor_next(cursor, &captured, &error));
		ASSERT_EQ(PG_OK, pg_cursor_close(&cursor, &error));
		ASSERT_EQ(first.size(), original.cached_header_size);
		EXPECT_EQ(0, memcmp(original.cached_header, first.data(),
			first.size()));
		for (pg_file *file : {old, captured, current}) {
			pg_file_info info = {};
			const auto &expected = file == current ? second : first;
			ASSERT_EQ(PG_OK, pg_file_inspect(file, &info, &error));
			ASSERT_EQ(expected.size(), info.cached_header_size);
			EXPECT_EQ(0, memcmp(info.cached_header, expected.data(),
				expected.size()));
		}
		ASSERT_EQ(PG_OK, pg_file_close(&old, &error));
		ASSERT_EQ(PG_OK, pg_file_close(&captured, &error));
		ASSERT_EQ(PG_OK, pg_file_close(&current, &error));
		ASSERT_EQ(PG_OK, pg_source_validate(source, &error));
		ASSERT_EQ(PG_OK, pg_source_close(&source, &error));
		ASSERT_EQ(PG_OK, pg_context_close(&context, &error));
	}
}

TEST_F(ApiMetadata, LargeHoggInventoryRetainsNamesHeadersAndBytes)
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_source *source = NULL;
	pg_cursor *cursor = NULL;
	pg_error error = {};
	pg_source_options options = {PG_HOGG10, PG_READ, 0};
	const unsigned entries = 5000;

	ASSERT_EQ(0, clear_file("large-metadata.hogg"));
	ASSERT_EQ(PG_OK, pg_context_open(&context, &error));
	ASSERT_EQ(PG_OK, pg_archive_builder_create(context,
		"large-metadata.hogg", PG_HOGG10, 0, &builder, &error));
	for (unsigned i = 0; i < entries; ++i) {
		char name[48];
		unsigned char header[13];
		for (unsigned j = 0; j < sizeof(header); ++j)
			header[j] = (unsigned char)(
				(i >> (8 * (j % 4))) ^ j);
		pg_entry_options entry = {};
		entry.compression = PG_COMPRESS_NEVER;
		entry.mtime = i;
		entry.cached_header = header;
		entry.cached_header_size = sizeof(header);
		snprintf(name, sizeof(name), "nested/file_%04u.bin", i);
		ASSERT_EQ(PG_OK, pg_archive_builder_write_all(builder, name,
			payload, sizeof(payload), &entry, &error));
	}
	ASSERT_EQ(PG_OK, pg_archive_builder_finish(builder, &error));
	ASSERT_EQ(PG_OK, pg_archive_builder_close(&builder, &error));
	ASSERT_EQ(PG_OK, pg_source_open(context, "large-metadata.hogg",
		&options, &source, &error)) << error.message;
	ASSERT_EQ(PG_OK, pg_source_files(source, NULL, &cursor, &error));
	for (unsigned i = 0; i < entries; ++i) {
		pg_file *file = NULL;
		pg_file_info info = {};
		char name[48];
		unsigned char header[13];
		for (unsigned j = 0; j < sizeof(header); ++j)
			header[j] = (unsigned char)(
				(i >> (8 * (j % 4))) ^ j);
		snprintf(name, sizeof(name), "nested/file_%04u.bin", i);
		ASSERT_EQ(PG_OK, pg_cursor_next(cursor, &file, &error));
		ASSERT_EQ(PG_OK, pg_file_inspect(file, &info, &error));
		EXPECT_STREQ(name, info.canonical_name);
		EXPECT_EQ(i, info.mtime);
		EXPECT_EQ(sizeof(payload), info.logical_size);
		EXPECT_EQ(sizeof(payload), info.stored_size);
		ASSERT_EQ(sizeof(header), info.cached_header_size);
		EXPECT_EQ(0, memcmp(header, info.cached_header,
			sizeof(header)));
		if (i % 997 == 0 || i + 1 == entries) {
			pg_reader *reader = NULL;
			unsigned char bytes[sizeof(payload)];
			size_t size = 0;
			ASSERT_EQ(PG_OK, pg_reader_open(file,
				PG_READ_LOGICAL_PAYLOAD, &reader, &error));
			ASSERT_EQ(PG_OK, pg_reader_read(reader, bytes,
				sizeof(bytes), &size, &error));
			ASSERT_EQ(PG_OK, pg_reader_close(&reader, &error));
			EXPECT_EQ(sizeof(payload), size);
			EXPECT_EQ(0, memcmp(bytes, payload, size));
		}
		ASSERT_EQ(PG_OK, pg_file_close(&file, &error));
	}
	pg_file *file = NULL;
	EXPECT_EQ(PG_END, pg_cursor_next(cursor, &file, &error));
	EXPECT_EQ(nullptr, file);
	ASSERT_EQ(PG_OK, pg_cursor_close(&cursor, &error));
	ASSERT_EQ(PG_OK, pg_source_validate(source, &error));
	ASSERT_EQ(PG_OK, pg_source_close(&source, &error));
	ASSERT_EQ(PG_OK, pg_context_close(&context, &error));
}

struct header_change {
	const std::vector<unsigned char> *before;
	const std::vector<unsigned char> *after;
	unsigned count;
};

static void expect_header(const pg_file_info *info,
	const std::vector<unsigned char> *header)
{
	if (!header) {
		EXPECT_EQ(nullptr, info);
		return;
	}
	ASSERT_NE(nullptr, info);
	ASSERT_EQ(header->size(), info->cached_header_size);
	if (header->empty())
		EXPECT_EQ(nullptr, info->cached_header);
	else
		EXPECT_EQ(0, memcmp(header->data(), info->cached_header,
			header->size()));
}

static void PG_CALL header_changed(void *user, const pg_visible_change *change)
{
	auto *expected = static_cast<header_change *>(user);

	if (!change->canonical_name || strcmp(change->canonical_name, "a"))
		return;
	expect_header(change->before, expected->before);
	expect_header(change->after, expected->after);
	expected->count++;
}

TEST_F(ApiMetadata, RetainedHeadersSurviveClearDeleteAndObservation)
{
	for (uint32_t format : {PG_PIGG2, PG_HOGG10}) {
		pg_context *context = NULL;
		pg_source *source = NULL;
		pg_archive_builder *builder = NULL;
		pg_tree *tree = NULL;
		pg_cursor *cursor = NULL;
		pg_file *old = NULL, *current = NULL, *captured = NULL;
		pg_error error = {};
		std::vector<unsigned char> first(65537), second(131073), empty;

		for (size_t i = 0; i < first.size(); ++i)
			first[i] = (unsigned char)(i * 17 + i / 251);
		for (size_t i = 0; i < second.size(); ++i)
			second[i] = (unsigned char)(i * 29 + i / 253);
		pg_entry_options entry = {};
		entry.cached_header = first.data();
		entry.cached_header_size = first.size();
		pg_source_options options = {format, PG_WRITE, 0};
		ASSERT_EQ(0, clear_file("observed-headers"));
		ASSERT_EQ(PG_OK, pg_context_open(&context, &error));
		ASSERT_EQ(PG_OK, pg_archive_builder_create(context,
			"observed-headers", format, 0, &builder, &error));
		ASSERT_EQ(PG_OK, pg_archive_builder_write_all(builder, "a",
			payload, sizeof(payload), &entry, &error));
		ASSERT_EQ(PG_OK, pg_archive_builder_finish(builder, &error));
		ASSERT_EQ(PG_OK, pg_archive_builder_close(&builder, &error));
		ASSERT_EQ(PG_OK, pg_source_open(context, "observed-headers",
			&options, &source, &error));
		ASSERT_EQ(PG_OK, pg_tree_create(context, &tree, &error));
		ASSERT_EQ(PG_OK, pg_tree_attach(tree, source, &error));
		ASSERT_EQ(PG_OK, pg_tree_request_subtree(tree, NULL, &error));
		ASSERT_EQ(PG_OK, pg_tree_watch(tree, PG_WATCH_SCAN, &error));
		ASSERT_EQ(PG_OK, pg_tree_find(tree, "a", &old, &error));
		ASSERT_EQ(PG_OK, pg_tree_files(tree, NULL, &cursor, &error));
		pg_file_info original = {};
		ASSERT_EQ(PG_OK, pg_file_inspect(old, &original, &error));
		pg_metadata_options edit = {PG_METADATA_HEADER, 0,
			second.data(), second.size()};
		ASSERT_EQ(PG_OK, pg_file_update_metadata(old, &edit, &error));
		header_change change = {&first, &second, 0};
		pg_observer observer = {&change, header_changed};
		ASSERT_EQ(PG_OK, pg_tree_poll(tree, &observer, &error));
		EXPECT_EQ(1u, change.count);
		ASSERT_EQ(PG_OK, pg_source_find(source, "a", &current, &error));
		pg_file_info replacement = {};
		ASSERT_EQ(PG_OK,
			pg_file_inspect(current, &replacement, &error));
		expect_header(&replacement, &second);
		ASSERT_EQ(PG_OK, pg_tree_rescan(tree, &error));
		edit.cached_header = NULL;
		edit.cached_header_size = 0;
		ASSERT_EQ(PG_OK,
			pg_file_update_metadata(current, &edit, &error));
		change = {&second, &empty, 0};
		ASSERT_EQ(PG_OK, pg_tree_poll(tree, &observer, &error));
		EXPECT_EQ(1u, change.count);
		expect_header(&replacement, &second);
		ASSERT_EQ(PG_OK, pg_file_close(&current, &error));
		ASSERT_EQ(PG_OK, pg_source_find(source, "a", &current, &error));
		pg_file_info cleared = {};
		ASSERT_EQ(PG_OK, pg_file_inspect(current, &cleared, &error));
		expect_header(&cleared, &empty);
		ASSERT_EQ(PG_OK, pg_file_delete(current, &error));
		change = {&empty, NULL, 0};
		ASSERT_EQ(PG_OK, pg_tree_poll(tree, &observer, &error));
		EXPECT_EQ(1u, change.count);
		ASSERT_EQ(PG_OK, pg_file_close(&current, &error));
		ASSERT_EQ(PG_OK, pg_cursor_next(cursor, &captured, &error));
		ASSERT_EQ(PG_OK, pg_cursor_close(&cursor, &error));
		ASSERT_EQ(PG_OK, pg_tree_close(&tree, &error));
		ASSERT_EQ(PG_OK, pg_source_close(&source, &error));
		expect_header(&original, &first);
		for (pg_file *file : {old, captured}) {
			pg_file_info info = {};
			ASSERT_EQ(PG_OK, pg_file_inspect(file, &info, &error));
			expect_header(&info, &first);
		}
		ASSERT_EQ(PG_OK, pg_file_close(&old, &error));
		ASSERT_EQ(PG_OK, pg_file_close(&captured, &error));
		ASSERT_EQ(PG_OK, pg_context_close(&context, &error));
	}
}

TEST_F(ApiMetadata, FailedTreeRefreshPreservesIncrementalHeaderSnapshots)
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_source *healthy = NULL, *broken = NULL;
	pg_file *old = NULL, *current = NULL;
	pg_tree *tree = NULL;
	pg_error error = {};
	std::vector<unsigned char> first(65537, 0x5a), second(131073, 0xa5);
	pg_entry_options entry = {};
	entry.cached_header = first.data();
	entry.cached_header_size = first.size();
	ASSERT_EQ(PG_OK, pg_context_open(&context, &error));
	ASSERT_EQ(PG_OK, pg_archive_builder_create(context, "healthy",
		PG_HOGG10, 0, &builder, &error));
	ASSERT_EQ(PG_OK, pg_archive_builder_write_all(builder, "keep", "old",
		3, &entry, &error));
	ASSERT_EQ(PG_OK, pg_archive_builder_finish(builder, &error));
	ASSERT_EQ(PG_OK, pg_archive_builder_close(&builder, &error));
	ASSERT_EQ(PG_OK, pg_archive_builder_create(context, "broken",
		PG_PIGG2, 0, &builder, &error));
	ASSERT_EQ(PG_OK, pg_archive_builder_finish(builder, &error));
	ASSERT_EQ(PG_OK, pg_archive_builder_close(&builder, &error));
	pg_source_options options = {PG_HOGG10, PG_WRITE, 0};
	ASSERT_EQ(PG_OK, pg_source_open(context, "healthy", &options,
		&healthy, &error));
	ASSERT_EQ(PG_OK, pg_source_find(healthy, "keep", &old, &error));
	entry.cached_header = second.data();
	entry.cached_header_size = second.size();
	ASSERT_EQ(PG_OK, pg_source_write_all(healthy, "keep", "new", 3,
		&entry, &error));
	ASSERT_EQ(PG_OK, pg_source_find(healthy, "keep", &current, &error));
	pg_file_info replacement = {};
	ASSERT_EQ(PG_OK, pg_file_inspect(current, &replacement, &error));
	ASSERT_EQ(PG_OK, pg_source_open(context, "broken", NULL, &broken,
		&error));
	ASSERT_EQ(PG_OK, pg_tree_create(context, &tree, &error));
	ASSERT_EQ(PG_OK, pg_tree_attach(tree, broken, &error));
	ASSERT_EQ(PG_OK, pg_tree_attach(tree, healthy, &error));
	ASSERT_EQ(0, put_bytes("broken", NULL, 0));
	ASSERT_EQ(PG_CORRUPT, pg_tree_rescan(tree, &error));
	pg_file_info original = {};
	ASSERT_EQ(PG_OK, pg_file_inspect(old, &original, &error));
	expect_header(&original, &first);
	expect_header(&replacement, &second);
	ASSERT_EQ(PG_OK, pg_file_close(&current, &error));
	ASSERT_EQ(PG_OK, pg_source_find(healthy, "keep", &current, &error));
	ASSERT_EQ(PG_OK, pg_file_inspect(current, &replacement, &error));
	expect_header(&replacement, &second);
	char bytes[3];
	size_t count = 0;
	pg_reader *reader = NULL;
	ASSERT_EQ(PG_OK, pg_reader_open(current, PG_READ_LOGICAL_PAYLOAD,
		&reader, &error));
	ASSERT_EQ(PG_OK, pg_reader_read(reader, bytes, sizeof(bytes),
		&count, &error));
	ASSERT_EQ(PG_OK, pg_reader_close(&reader, &error));
	EXPECT_EQ(sizeof(bytes), count);
	EXPECT_EQ(0, memcmp(bytes, "new", sizeof(bytes)));
	ASSERT_EQ(PG_OK, pg_tree_close(&tree, &error));
	ASSERT_EQ(PG_OK, pg_source_close(&broken, &error));
	ASSERT_EQ(PG_OK, pg_source_close(&healthy, &error));
	expect_header(&replacement, &second);
	ASSERT_EQ(PG_OK, pg_file_close(&old, &error));
	ASSERT_EQ(PG_OK, pg_file_close(&current, &error));
	ASSERT_EQ(PG_OK, pg_context_close(&context, &error));
}

} // namespace
