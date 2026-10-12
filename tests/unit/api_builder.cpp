#include "../api_test.hpp"
#include "../../src/internal.hpp"
#include <fstream>
#include <zlib-ng.h>

namespace
{
using ApiBuilder = piggle_test::ContractTest;
#define EXPECT_STATUS(call, expected) ASSERT_EQ(expected, call)
static int ordered_builder(uint32_t format)
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_source *source = NULL;
	pg_file *file = NULL;
	pg_file_info info;
	pg_error error;
	pg_entry_options options = {};
	options.compression = PG_COMPRESS_NEVER;

	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(pg_archive_builder_create(
			context, "ordered", format, 0, &builder, &error),
		PG_OK);
	for (unsigned i = 0; i < 4096; i++) {
		char name[40];
		snprintf(name, sizeof(name), "Dir/%08u", 4095 - i);
		PG_STATUS(pg_archive_builder_write_all(builder, name, &i,
				sizeof(i), &options, &error),
			PG_OK);
	}
	PG_STATUS(pg_archive_builder_write_all(
			builder, "dir\\00000000", NULL, 0, NULL, &error),
		PG_EXISTS);
	options.digest_kind = PG_DIGEST_MD5;
	PG_STATUS(pg_archive_builder_write_all(
			builder, "retry", "x", 1, &options, &error),
		PG_CHECKSUM);
	options.digest_kind = PG_DIGEST_NONE;
	PG_STATUS(pg_archive_builder_write_all(
			builder, "retry", "x", 1, &options, &error),
		PG_OK);
	PG_CHECK(builder->entry_count == 4097);
	PG_CHECK(builder->name_comparisons < 4097 * 32);
	PG_CHECK(builder->name_index->height < 20);
	PG_STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	PG_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	PG_STATUS(pg_source_open(context, "ordered", NULL, &source, &error),
		PG_OK);
	PG_STATUS(pg_source_find(source, "dir/00004095", &file, &error), PG_OK);
	PG_STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	PG_CHECK(info.archive_record == 0);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_source_find(source, "dir/00000000", &file, &error), PG_OK);
	PG_STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	PG_CHECK(info.archive_record == 4095);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}
TEST_F(ApiBuilder, OrderedHogg)
{
	ASSERT_EQ(0, ordered_builder(PG_HOGG10));
}

TEST_F(ApiBuilder, OrderedPigg)
{
	ASSERT_EQ(0, ordered_builder(PG_PIGG2));
}

#ifdef PIGGLE_TEST_ALLOCATION_PATH
TEST_F(ApiBuilder, MetadataAllocationBeforeEntryAcceptance)
{
	pid_t child = fork();
	ASSERT_GE(child, 0);
	if (!child) {
		execl(PIGGLE_TEST_ALLOCATION_PATH, PIGGLE_TEST_ALLOCATION_PATH,
			(char *)NULL);
		_exit(2);
	}
	int status;
	ASSERT_EQ(child, waitpid(child, &status, 0));
	ASSERT_TRUE(WIFEXITED(status));
	EXPECT_EQ(0, WEXITSTATUS(status));
}

#endif

TEST_F(ApiBuilder, LargeCachedHeaders)
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_source *source = NULL;
	pg_file *file = NULL;
	pg_error error;
	pg_source_options writable = {PG_HOGG10, PG_WRITE, 0};
	EXPECT_STATUS(pg_context_open(&context, &error), PG_OK);
	EXPECT_STATUS(pg_archive_builder_create(context, "headers", PG_HOGG10,
			0, &builder, &error),
		PG_OK);
	EXPECT_STATUS(pg_archive_builder_write_all(
			builder, "keep", "safe", 4, NULL, &error),
		PG_OK);
	EXPECT_STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	EXPECT_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	EXPECT_STATUS(
		pg_source_open(context, "headers", &writable, &source, &error),
		PG_OK);
	for (size_t size : {3074u, 3075u, 3076u, 65536u}) {
		std::string header(size, 'H');
		pg_entry_options entry = {};
		entry.cached_header = header.data();
		entry.cached_header_size = header.size();
		entry.compression = PG_COMPRESS_FORCE;
		EXPECT_STATUS(pg_source_write_all(source, "new/long-name",
				"abc", 3, &entry, &error),
			PG_OK);
		EXPECT_STATUS(
			pg_source_find(source, "new/long-name", &file, &error),
			PG_OK);
		pg_file_info info;
		EXPECT_STATUS(pg_file_inspect(file, &info, &error), PG_OK);
		EXPECT_EQ(header.size(), info.cached_header_size);
		EXPECT_EQ(0,
			memcmp(header.data(), info.cached_header,
				header.size()));
		EXPECT_STATUS(pg_file_close(&file, &error), PG_OK);
		EXPECT_STATUS(
			pg_source_find(source, "keep", &file, &error), PG_OK);
		char bytes[4];
		size_t count;
		EXPECT_STATUS(pg_file_read_all(file, bytes, sizeof(bytes),
				&count, &error),
			PG_OK);
		EXPECT_EQ(0, memcmp(bytes, "safe", 4));
		EXPECT_STATUS(pg_file_close(&file, &error), PG_OK);
	}
	EXPECT_STATUS(
		pg_source_find(source, "new/long-name", &file, &error), PG_OK);
	pg_metadata_options metadata = {PG_METADATA_HEADER, 0, NULL, 0};
	EXPECT_STATUS(pg_file_update_metadata(file, &metadata, &error), PG_OK);
	EXPECT_STATUS(pg_file_close(&file, &error), PG_OK);
	EXPECT_STATUS(pg_source_close(&source, &error), PG_OK);
	EXPECT_STATUS(pg_source_open(context, "headers", NULL, &source, &error),
		PG_OK);
	EXPECT_STATUS(pg_source_validate(source, &error), PG_OK);
	EXPECT_STATUS(pg_source_close(&source, &error), PG_OK);
	EXPECT_STATUS(pg_context_close(&context, &error), PG_OK);
}

TEST_F(ApiBuilder, CompressedDataListAndEncodedLargeHeader)
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_source *source = NULL;
	pg_writer *writer = NULL;
	pg_error error;
	EXPECT_STATUS(pg_context_open(&context, &error), PG_OK);
	EXPECT_STATUS(pg_archive_builder_create(context, "encoded.hogg",
			PG_HOGG10, 0, &builder, &error),
		PG_OK);
	EXPECT_STATUS(pg_archive_builder_write_all(
			builder, "keep", "safe", 4, NULL, &error),
		PG_OK);
	EXPECT_STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	EXPECT_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	std::ifstream input("encoded.hogg", std::ios::binary);
	std::string archive((std::istreambuf_iterator<char>(input)),
		std::istreambuf_iterator<char>());
	input.close();
	auto load = [&](size_t at, unsigned width) {
		uint64_t value = 0;
		for (unsigned i = 0; i < width; i++)
			value |= (uint64_t)(uint8_t)archive[at + i] << (8 * i);
		return value;
	};
	auto save = [&](size_t at, uint64_t value, unsigned width) {
		for (unsigned i = 0; i < width; i++)
			archive[at + i] = (char)(value >> (8 * i));
	};
	uint32_t dl = (uint32_t)load(16, 4);
	size_t record = 4144 + 32 * dl;
	size_t ea =
		4144 + (size_t)load(8, 4) + 16 * (size_t)load(record + 28, 4);
	size_t dl_size = (size_t)load(record + 8, 4);
	size_t offset = (size_t)load(record, 8);
	std::vector<uint8_t> encoded(zng_compressBound(dl_size));
	size_t encoded_size = encoded.size();
	ASSERT_EQ(Z_OK,
		zng_compress(encoded.data(), &encoded_size,
			(const uint8_t *)archive.data() + offset, dl_size));
	save(record, archive.size(), 8);
	save(record + 8, encoded_size, 4);
	save(ea + 8, dl_size, 4);
	archive.append((const char *)encoded.data(), encoded_size);
	ASSERT_EQ(0, put_bytes("encoded.hogg", archive.data(), archive.size()));
	pg_source_options writable = {PG_HOGG10, PG_WRITE, 0};
	EXPECT_STATUS(pg_source_open(context, "encoded.hogg", &writable,
			&source, &error),
		PG_OK);
	std::string header(65536, 'H');
	uint8_t bytes[64];
	encoded_size = sizeof(bytes);
	ASSERT_EQ(Z_OK,
		zng_compress(bytes, &encoded_size, (const uint8_t *)"abc", 3));
	pg_write_options options;
	pg_write_options_init(&options, 3);
	options.input_size = encoded_size;
	options.encoding = PG_ZLIB;
	options.entry.cached_header = header.data();
	options.entry.cached_header_size = header.size();
	EXPECT_STATUS(pg_writer_open_source(
			source, "encoded", &options, &writer, &error),
		PG_OK);
	size_t count;
	EXPECT_STATUS(
		pg_writer_write(writer, bytes, encoded_size, &count, &error),
		PG_OK);
	EXPECT_STATUS(pg_writer_finish(writer, &error), PG_OK);
	EXPECT_STATUS(pg_writer_close(&writer, &error), PG_OK);
	EXPECT_STATUS(pg_source_validate(source, &error), PG_OK);
	EXPECT_STATUS(pg_source_close(&source, &error), PG_OK);
	EXPECT_STATUS(
		pg_source_open(context, "encoded.hogg", NULL, &source, &error),
		PG_OK);
	pg_file *file = NULL;
	pg_file_info info;
	EXPECT_STATUS(pg_source_find(source, "encoded", &file, &error), PG_OK);
	EXPECT_STATUS(pg_file_inspect(file, &info, &error), PG_OK);
	EXPECT_EQ(PG_ZLIB, info.encoding);
	EXPECT_EQ(65536u, info.cached_header_size);
	EXPECT_STATUS(pg_file_close(&file, &error), PG_OK);
	EXPECT_STATUS(pg_source_close(&source, &error), PG_OK);
	EXPECT_STATUS(pg_context_close(&context, &error), PG_OK);
}

#ifdef __linux__
extern "C" void pg_test_journal_fault(int mode);

TEST_F(ApiBuilder, InterruptedLargeHeaderFold)
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_source *source = NULL;
	pg_error error;
	EXPECT_STATUS(pg_context_open(&context, &error), PG_OK);
	EXPECT_STATUS(pg_archive_builder_create(context, "base", PG_HOGG10,
			0, &builder, &error), PG_OK);
	EXPECT_STATUS(pg_archive_builder_write_all(
			builder, "keep", "safe", 4, NULL, &error), PG_OK);
	EXPECT_STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	EXPECT_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	pg_source_options writable = {PG_HOGG10, PG_WRITE, 0};
	EXPECT_STATUS(pg_source_open(context, "base", &writable, &source,
			&error), PG_OK);
	EXPECT_STATUS(pg_source_write_all(source, "journal", "live", 4,
			NULL, &error), PG_OK);
	EXPECT_STATUS(pg_source_close(&source, &error), PG_OK);
	for (int mode = 13; mode <= 15; mode++) {
		std::filesystem::copy_file("base", "fold",
			std::filesystem::copy_options::overwrite_existing);
		pid_t pid = fork();
		ASSERT_GE(pid, 0);
		if (!pid) {
			pg_source *child_source = NULL;
			if (pg_source_open(context, "fold", &writable,
					&child_source, &error) != PG_OK)
				_exit(2);
			std::string header(65536, 'H');
			pg_entry_options entry = {};
			entry.cached_header = header.data();
			entry.cached_header_size = header.size();
			pg_test_journal_fault(mode);
			pg_source_write_all(child_source, "new", "abc", 3,
				&entry, &error);
			_exit(3);
		}
		int status;
		ASSERT_EQ(pid, waitpid(pid, &status, 0));
		ASSERT_TRUE(WIFEXITED(status));
		ASSERT_EQ(0, WEXITSTATUS(status));
		EXPECT_STATUS(pg_source_recover(context, "fold", &error), PG_OK);
		EXPECT_STATUS(pg_source_open(context, "fold", NULL, &source,
				&error), PG_OK);
		EXPECT_STATUS(pg_source_validate(source, &error), PG_OK);
		for (const char *name : {"keep", "journal"}) {
			char bytes[4];
			size_t count;
			EXPECT_STATUS(pg_source_read_all(source, name, bytes,
					sizeof(bytes), &count, &error), PG_OK);
			EXPECT_EQ(0, memcmp(bytes,
				!strcmp(name, "keep") ? "safe" : "live", 4));
		}
		pg_file *file = NULL;
		EXPECT_STATUS(pg_source_find(source, "new", &file, &error),
			mode == 15 ? PG_OK : PG_NOT_FOUND);
		if (file) {
			pg_file_info info;
			EXPECT_STATUS(pg_file_inspect(file, &info, &error), PG_OK);
			EXPECT_EQ(65536u, info.cached_header_size);
			EXPECT_STATUS(pg_file_close(&file, &error), PG_OK);
		}
		EXPECT_STATUS(pg_source_close(&source, &error), PG_OK);
	}
	EXPECT_STATUS(pg_context_close(&context, &error), PG_OK);
}

#endif

TEST_F(ApiBuilder, KnownMutationAvoidsReconciliation)
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_source *source = NULL;
	pg_error error;
	EXPECT_STATUS(pg_context_open(&context, &error), PG_OK);
	EXPECT_STATUS(pg_archive_builder_create(context, "indexed", PG_HOGG10,
			0, &builder, &error),
		PG_OK);
	EXPECT_STATUS(pg_archive_builder_write_all(
			builder, "keep", "safe", 4, NULL, &error),
		PG_OK);
	EXPECT_STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	EXPECT_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	pg_source_options writable = {PG_HOGG10, PG_WRITE, 0};
	EXPECT_STATUS(
		pg_source_open(context, "indexed", &writable, &source, &error),
		PG_OK);
	for (unsigned i = 0; i < 40; i++) {
		char name[30];
		snprintf(name, sizeof(name), "new/%08u", i);
		EXPECT_STATUS(
			pg_source_write_all(source, name, "x", 1, NULL, &error),
			PG_OK);
	}
	EXPECT_EQ(0u, source->full_reconciliations);
	EXPECT_STATUS(pg_source_close(&source, &error), PG_OK);
	EXPECT_STATUS(pg_context_close(&context, &error), PG_OK);
}

TEST_F(ApiBuilder, FailedTreeRefreshDropsMutationCache)
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_source *healthy = NULL, *broken = NULL;
	pg_tree *tree = NULL;
	pg_error error;
	EXPECT_STATUS(pg_context_open(&context, &error), PG_OK);
	EXPECT_STATUS(pg_archive_builder_create(context, "healthy", PG_HOGG10,
			0, &builder, &error),
		PG_OK);
	EXPECT_STATUS(pg_archive_builder_write_all(
			builder, "keep", "old", 3, NULL, &error),
		PG_OK);
	EXPECT_STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	EXPECT_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	EXPECT_STATUS(pg_archive_builder_create(
			context, "broken", PG_PIGG2, 0, &builder, &error),
		PG_OK);
	EXPECT_STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	EXPECT_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	pg_source_options writable = {PG_HOGG10, PG_WRITE, 0};
	EXPECT_STATUS(
		pg_source_open(context, "healthy", &writable, &healthy, &error),
		PG_OK);
	EXPECT_STATUS(
		pg_source_write_all(healthy, "keep", "new", 3, NULL, &error),
		PG_OK);
	ASSERT_NE(nullptr, healthy->hogg_cache);
	EXPECT_STATUS(pg_source_open(context, "broken", NULL, &broken, &error),
		PG_OK);
	EXPECT_STATUS(pg_tree_create(context, &tree, &error), PG_OK);
	EXPECT_STATUS(pg_tree_attach(tree, broken, &error), PG_OK);
	EXPECT_STATUS(pg_tree_attach(tree, healthy, &error), PG_OK);
	ASSERT_EQ(0, put_bytes("broken", NULL, 0));
	EXPECT_STATUS(pg_tree_rescan(tree, &error), PG_CORRUPT);
	EXPECT_EQ(nullptr, healthy->hogg_cache);
	char bytes[3];
	size_t count;
	EXPECT_STATUS(pg_source_read_all(healthy, "keep", bytes, sizeof(bytes),
			&count, &error),
		PG_OK);
	EXPECT_EQ(0, memcmp(bytes, "new", 3));
	EXPECT_STATUS(pg_tree_close(&tree, &error), PG_OK);
	EXPECT_STATUS(pg_source_close(&broken, &error), PG_OK);
	EXPECT_STATUS(pg_source_close(&healthy, &error), PG_OK);
	EXPECT_STATUS(pg_context_close(&context, &error), PG_OK);
}


TEST_F(ApiBuilder, Capacity)
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_source *source = NULL;
	pg_error error;
	pg_archive_build_options options = {
		{PG_HOGG10, 0, PG_CHECKSUM_LOGICAL}, NULL, 31};

	EXPECT_STATUS(pg_context_open(&context, &error), PG_OK);
	EXPECT_STATUS(pg_archive_builder_create_config(
			context, "reserved", &options, &builder, &error),
		PG_OK);
	EXPECT_STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	EXPECT_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	FILE *disk = fopen("reserved", "rb");
	ASSERT_NE(nullptr, disk);
	unsigned char bytes[24];
	ASSERT_EQ(sizeof(bytes), fread(bytes, 1, sizeof(bytes), disk));
	EXPECT_EQ(0, fclose(disk));
	EXPECT_EQ(0, bytes[8]);
	EXPECT_EQ(4, bytes[9]);
	EXPECT_EQ(0, bytes[12]);
	EXPECT_EQ(2, bytes[13]);
	pg_source_options writable = {PG_HOGG10, PG_WRITE, PG_CHECKSUM_LOGICAL};
	EXPECT_STATUS(
		pg_source_open(context, "reserved", &writable, &source, &error),
		PG_OK);
	for (unsigned i = 0; i < 40; i++) {
		char name[32];
		snprintf(name, sizeof(name), "file%u", i);
		EXPECT_STATUS(
			pg_source_write_all(source, name, "x", 1, NULL, &error),
			PG_OK);
	}
	EXPECT_STATUS(pg_source_close(&source, &error), PG_OK);
	EXPECT_STATUS(
		pg_source_open(context, "reserved", NULL, &source, &error),
		PG_OK);
	EXPECT_STATUS(pg_source_validate(source, &error), PG_OK);
	EXPECT_STATUS(pg_source_close(&source, &error), PG_OK);
	options.archive.format = PG_PIGG2;
	EXPECT_STATUS(pg_archive_builder_create_config(
			context, "pigg", &options, &builder, &error),
		PG_UNSUPPORTED);
	EXPECT_EQ(nullptr, builder);
	options.archive.format = PG_HOGG10;
	options.hogg_user_capacity = UINT64_MAX;
	EXPECT_STATUS(pg_archive_builder_create_config(
			context, "limit", &options, &builder, &error),
		PG_LIMIT);
	EXPECT_EQ(nullptr, builder);
	EXPECT_STATUS(pg_context_close(&context, &error), PG_OK);
}

TEST_F(ApiBuilder, StagingPolicy)
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_error error;
	ASSERT_EQ(0, directory("scratch"));
	char scratch[] = "scratch";
	pg_archive_build_options config = {{PG_HOGG10, 0, 0}, scratch, 0};
	EXPECT_STATUS(pg_context_open(&context, &error), PG_OK);
	EXPECT_STATUS(pg_archive_builder_create_config(
			context, "staged", &config, &builder, &error),
		PG_OK);
	scratch[0] = 'X';
	EXPECT_STATUS(pg_archive_builder_write_all(
			builder, "a", "abc", 3, NULL, &error),
		PG_OK);
	EXPECT_STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	EXPECT_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	EXPECT_STATUS(pg_context_close(&context, &error), PG_OK);
}

} // namespace
