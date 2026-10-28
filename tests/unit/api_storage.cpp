#include "../api_test.hpp"
#include <fstream>

namespace
{
using ApiStorage = piggle_test::ContractTest;
#define EXPECT_STATUS(call, expected) ASSERT_EQ(expected, call)

TEST_F(ApiStorage, CompactVisibleWinners)
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_source *source = NULL, *compacted = NULL;
	pg_file *file = NULL;
	pg_error error;
	EXPECT_STATUS(pg_context_open(&context, &error), PG_OK);
	EXPECT_STATUS(pg_archive_builder_create(context, "original", PG_HOGG10,
			0, &builder, &error),
		PG_OK);
	std::vector<unsigned char> header(131073);
	for (size_t i = 0; i < header.size(); ++i)
		header[i] = (unsigned char)(i * 37 + i / 251);
	pg_entry_options entry = {};
	entry.cached_header = header.data();
	entry.cached_header_size = header.size();
	EXPECT_STATUS(pg_archive_builder_write_all(
			builder, "Keep/Spelling", "abc", 3, &entry, &error),
		PG_OK);
	EXPECT_STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	EXPECT_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	pg_source_options writable = {PG_HOGG10, PG_WRITE, 0};
	EXPECT_STATUS(
		pg_source_open(context, "original", &writable, &source, &error),
		PG_OK);
	EXPECT_STATUS(pg_source_write_all(
			source, "keep/spelling", "new", 3, &entry, &error),
		PG_OK);
	pg_source_storage_info storage;
	EXPECT_STATUS(
		pg_source_storage_inspect(source, &storage, &error), PG_OK);
	EXPECT_EQ(storage.physical_length,
		storage.live_payload_bytes + storage.metadata_bytes +
			storage.unreferenced_bytes);
	EXPECT_EQ(3u, storage.live_payload_bytes);
	EXPECT_GE(storage.unreferenced_bytes, 3u);
	EXPECT_GT(storage.reclaimable_bytes, 0u);
	EXPECT_STATUS(pg_source_compact(source, "original", NULL, &error),
		PG_CONFLICT);
	EXPECT_STATUS(
		pg_source_compact(source, "compacted", NULL, &error), PG_OK);
	EXPECT_STATUS(
		pg_source_open(context, "compacted", NULL, &compacted, &error),
		PG_OK);
	pg_source_storage_info after;
	EXPECT_STATUS(
		pg_source_storage_inspect(compacted, &after, &error), PG_OK);
	EXPECT_EQ(storage.compacted_size, after.physical_length);
	EXPECT_EQ(0u, after.unreferenced_bytes);
	EXPECT_STATUS(pg_source_find(compacted, "keep/spelling", &file, &error),
		PG_OK);
	pg_file_info info;
	EXPECT_STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	EXPECT_STREQ("Keep/Spelling", info.original_name);
	ASSERT_EQ(header.size(), info.cached_header_size);
	EXPECT_EQ(0, memcmp(header.data(), info.cached_header, header.size()));
	char bytes[3];
	size_t count;
	pg_reader *reader = NULL;

	EXPECT_STATUS(pg_reader_open(file, PG_READ_LOGICAL_PAYLOAD, &reader,
		&error), PG_OK);
	EXPECT_STATUS(
		pg_reader_read(reader, bytes, sizeof(bytes), &count, &error),
		PG_OK);
	EXPECT_EQ(0, memcmp(bytes, "new", 3));
	EXPECT_STATUS(pg_reader_close(&reader, &error), PG_OK);
	EXPECT_STATUS(pg_file_close(&file, &error), PG_OK);
	EXPECT_STATUS(pg_source_close(&compacted, &error), PG_OK);
	EXPECT_STATUS(
		pg_source_find(source, "keep/spelling", &file, &error), PG_OK);
	EXPECT_STATUS(pg_file_close(&file, &error), PG_OK);
	EXPECT_STATUS(pg_source_close(&source, &error), PG_OK);
	EXPECT_STATUS(pg_context_close(&context, &error), PG_OK);
}
TEST_F(ApiStorage, SharedHiddenAndUnnamedRanges)
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_source *source = NULL;
	pg_error error;
	EXPECT_STATUS(pg_context_open(&context, &error), PG_OK);
	EXPECT_STATUS(pg_archive_builder_create(context, "shared", PG_HOGG10, 0,
			&builder, &error),
		PG_OK);
	EXPECT_STATUS(pg_archive_builder_write_all(
			builder, "Firstx", "old-old", 7, NULL, &error),
		PG_OK);
	EXPECT_STATUS(pg_archive_builder_write_all(
			builder, "second", "new-new", 7, NULL, &error),
		PG_OK);
	EXPECT_STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	EXPECT_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	std::ifstream input("shared", std::ios::binary);
	std::string archive((std::istreambuf_iterator<char>(input)),
		std::istreambuf_iterator<char>());
	input.close();
	size_t name = archive.find("second");
	ASSERT_NE(std::string::npos, name);
	archive.replace(name, 6, "FIRSTX");
	archive.replace(4144 + 32 * 3, 32, archive.substr(4144, 32));
	archive.replace(4144 + 32 * 3 + 24, 8, 8, '\0');
	archive.replace(4144 + 32 * 2 + 16, 4, 4, '\0');
	ASSERT_EQ(0, put_bytes("shared", archive.data(), archive.size()));
	EXPECT_STATUS(pg_source_open(context, "shared", NULL, &source, &error),
		PG_OK);
	pg_source_storage_info info;
	EXPECT_STATUS(pg_source_storage_inspect(source, &info, &error), PG_OK);
	EXPECT_EQ(14u, info.live_payload_bytes);
	EXPECT_EQ(0u, info.unreferenced_bytes);
	EXPECT_STATUS(pg_source_compact(source, "compact-shared", NULL, &error),
		PG_OK);
	EXPECT_STATUS(pg_source_close(&source, &error), PG_OK);
	EXPECT_STATUS(pg_source_open(
			context, "compact-shared", NULL, &source, &error),
		PG_OK);
	pg_source_storage_info compacted;
	EXPECT_STATUS(
		pg_source_storage_inspect(source, &compacted, &error), PG_OK);
	EXPECT_EQ(7u, compacted.live_payload_bytes);
	EXPECT_EQ(info.compacted_size, compacted.physical_length);
	pg_file *file = NULL;
	pg_file_info metadata;
	EXPECT_STATUS(pg_source_find(source, "firstx", &file, &error), PG_OK);
	EXPECT_STATUS(pg_file_inspect(file, &metadata, &error), PG_OK);
	EXPECT_STREQ("FIRSTX", metadata.original_name);
	EXPECT_STATUS(pg_file_close(&file, &error), PG_OK);
	EXPECT_STATUS(pg_source_close(&source, &error), PG_OK);
	EXPECT_STATUS(pg_context_close(&context, &error), PG_OK);
}
} // namespace
