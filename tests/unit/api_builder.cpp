#include "../api_test.hpp"
#include "../../src/internal.hpp"
#include <fstream>
#include <zlib-ng.h>
#ifdef __linux__
#include <signal.h>
#include <sys/resource.h>
#endif

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

static int retry_staging(const char *scratch)
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_writer *writer = NULL;
	pg_source *source = NULL;
	pg_error error;
	pg_archive_build_options config = {{PG_HOGG10, 0, 0}, scratch, 0};
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(pg_archive_builder_create_config(
			context, "retry.hogg", &config, &builder, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_write_all(
			builder, "keep", "safe", 4, NULL, &error),
		PG_OK);
	std::string payload(65536, 'x');
	pg_write_options options;
	pg_write_options_init(&options, payload.size());
	options.entry.compression = PG_COMPRESS_NEVER;
	PG_STATUS(pg_writer_open_archive_builder(
			builder, "retry", &options, &writer, &error),
		PG_OK);
	size_t count;
	PG_STATUS(pg_writer_write(writer, payload.data(), payload.size(),
			&count, &error),
		PG_OK);
	struct stat staged;
	struct rlimit before, limited;
	PG_CHECK(!fstat(fileno(builder->staging), &staged));
	PG_CHECK(!getrlimit(RLIMIT_FSIZE, &before));
	limited = before;
	limited.rlim_cur = (rlim_t)staged.st_size + 8;
	PG_CHECK(signal(SIGXFSZ, SIG_IGN) != SIG_ERR);
	PG_CHECK(!setrlimit(RLIMIT_FSIZE, &limited));
	pg_status failed = pg_writer_finish(writer, &error);
	PG_CHECK(!setrlimit(RLIMIT_FSIZE, &before));
	PG_CHECK(failed == PG_IO);
	PG_STATUS(pg_writer_close(&writer, &error), PG_OK);
	PG_STATUS(pg_archive_builder_write_all(
			builder, "retry", "x", 1, NULL, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	PG_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	PG_STATUS(pg_source_open(context, "retry.hogg", NULL, &source, &error),
		PG_OK);
	PG_STATUS(pg_source_validate(source, &error), PG_OK);
	char bytes[4];
	PG_STATUS(pg_source_read_all(
			source, "keep", bytes, sizeof(bytes), &count, &error),
		PG_OK);
	PG_CHECK(count == 4 && !memcmp(bytes, "safe", 4));
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

TEST_F(ApiBuilder, RetryAfterPartialStagingWriteAcrossVolumes)
{
	std::filesystem::path scratch = std::filesystem::temp_directory_path() /
		("piggle-retry-" +
			std::filesystem::current_path().filename().string());
	std::filesystem::create_directory(scratch);
	struct stat here, there;
	ASSERT_EQ(0, stat(".", &here));
	ASSERT_EQ(0, stat(scratch.string().c_str(), &there));
	if (here.st_dev == there.st_dev) {
		std::filesystem::remove(scratch);
		GTEST_SKIP() << "A separate staging volume is unavailable";
	}
	pid_t pid = fork();
	ASSERT_GE(pid, 0);
	if (!pid)
		_exit(retry_staging(scratch.string().c_str()));
	int status;
	ASSERT_EQ(pid, waitpid(pid, &status, 0));
	std::filesystem::remove_all(scratch);
	ASSERT_TRUE(WIFEXITED(status));
	EXPECT_EQ(0, WEXITSTATUS(status));
}

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
		EXPECT_STATUS(
			pg_source_recover(context, "fold", &error), PG_OK);
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
			EXPECT_STATUS(
				pg_file_inspect(file, &info, &error), PG_OK);
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
	pg_source *source = NULL;
	pg_writer *writer = NULL;
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
	pg_source_options writable = {PG_HOGG10, PG_WRITE, 0};
	EXPECT_STATUS(
		pg_source_open(context, "staged", &writable, &source, &error),
		PG_OK);
	EXPECT_STATUS(
		pg_source_set_staging_directory(source, "scratch", &error),
		PG_OK);
	pg_write_options options;
	pg_write_options_init(&options, 3);
	EXPECT_STATUS(
		pg_writer_open_source(source, "b", &options, &writer, &error),
		PG_OK);
	EXPECT_STATUS(
		pg_source_set_staging_directory(source, NULL, &error), PG_BUSY);
	EXPECT_STATUS(pg_writer_close(&writer, &error), PG_OK);
	EXPECT_STATUS(pg_source_set_staging_directory(source, "", &error),
		PG_INVALID);
	EXPECT_STATUS(
		pg_source_set_staging_directory(source, NULL, &error), PG_OK);
	EXPECT_STATUS(pg_source_close(&source, &error), PG_OK);
	EXPECT_STATUS(pg_context_close(&context, &error), PG_OK);
}


TEST_F(ApiBuilder, ReservedPrivatePublication)
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_error error;
	pg_archive_build_options config = {{PG_HOGG10, 0, 0}, NULL, 31};
	EXPECT_STATUS(pg_context_open(&context, &error), PG_OK);
	EXPECT_STATUS(pg_archive_builder_create_config(context, "private.hogg",
			&config, &builder, &error),
		PG_OK);
	std::string bytes(1048576, 'x');
	pg_entry_options entry = {};
	entry.compression = PG_COMPRESS_NEVER;
	EXPECT_STATUS(pg_archive_builder_write_all(builder, "payload",
			bytes.data(), bytes.size(), &entry, &error),
		PG_OK);
	struct stat private_identity, published;
	ASSERT_EQ(0, fstat(fileno(builder->staging), &private_identity));
	EXPECT_STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	ASSERT_EQ(0, lstat("private.hogg", &published));
	EXPECT_EQ(private_identity.st_dev, published.st_dev);
	EXPECT_EQ(private_identity.st_ino, published.st_ino);
	EXPECT_EQ(bytes.size(), builder->payload_copied_bytes);
	EXPECT_EQ(0u, builder->whole_archive_copied_bytes);
	EXPECT_EQ(0u, builder->relocated_bytes);
	EXPECT_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	EXPECT_STATUS(pg_context_close(&context, &error), PG_OK);
}

TEST_F(ApiBuilder, PackReservesCapturedInventory)
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_source *source = NULL;
	pg_error error;
	pg_archive_build_options config = {{PG_HOGG10, 0, 0}, NULL, 40};
	pg_entry_options entry = {};
	entry.compression = PG_COMPRESS_NEVER;
	char content[256] = {};
	EXPECT_STATUS(pg_context_open(&context, &error), PG_OK);
	EXPECT_STATUS(pg_archive_builder_create_config(context, "input.hogg",
			&config, &builder, &error), PG_OK);
	for (unsigned i = 0; i < 40; i++) {
		char name[32];
		snprintf(name, sizeof(name), "file/%08u", i);
		EXPECT_STATUS(pg_archive_builder_write_all(builder, name,
				content, sizeof(content), &entry, &error),
			PG_OK);
	}
	EXPECT_STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	EXPECT_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	EXPECT_STATUS(pg_source_open(context, "input.hogg", NULL, &source,
			&error), PG_OK);
	pg_pack_options options = {PG_COMPRESS_NEVER, 0, 0};
	EXPECT_STATUS(pg_source_pack(source, "output.hogg", PG_HOGG10,
			&options, &error), PG_OK);
	struct stat input, output;
	ASSERT_EQ(0, lstat("input.hogg", &input));
	ASSERT_EQ(0, lstat("output.hogg", &output));
	EXPECT_EQ(input.st_size, output.st_size);
	EXPECT_STATUS(pg_source_close(&source, &error), PG_OK);
	EXPECT_STATUS(pg_context_close(&context, &error), PG_OK);
}

#ifndef _WIN32
TEST_F(ApiBuilder, CrossVolumePublication)
{
	std::filesystem::path scratch = std::filesystem::temp_directory_path() /
		("piggle-staging-" +
			std::filesystem::current_path().filename().string());
	std::filesystem::create_directory(scratch);
	struct stat here, there;
	ASSERT_EQ(0, stat(".", &here));
	ASSERT_EQ(0, stat(scratch.string().c_str(), &there));
	if (here.st_dev == there.st_dev) {
		std::filesystem::remove(scratch);
		GTEST_SKIP() << "A separate staging volume is unavailable";
	}
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_error error;
	std::string path = scratch.string();
	pg_archive_build_options config = {{PG_HOGG10, 0, 0}, path.c_str(), 0};
	EXPECT_STATUS(pg_context_open(&context, &error), PG_OK);
	EXPECT_STATUS(pg_archive_builder_create_config(
			context, "cross.hogg", &config, &builder, &error),
		PG_OK);
	EXPECT_STATUS(pg_archive_builder_write_all(
			builder, "a", "abc", 3, NULL, &error),
		PG_OK);
	EXPECT_STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	struct stat output;
	ASSERT_EQ(0, stat("cross.hogg", &output));
	EXPECT_EQ(
		(uint64_t)output.st_size, builder->whole_archive_copied_bytes);
	EXPECT_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	EXPECT_STATUS(pg_context_close(&context, &error), PG_OK);
	EXPECT_TRUE(std::filesystem::is_empty(scratch));
	std::filesystem::remove(scratch);
}

#endif
#ifdef __linux__
static int exdev_publication(uint32_t flags, bool existing, bool session,
	bool fail_copy = false)
{
	PG_CHECK(!directory("scratch"));
	if (existing)
		PG_CHECK(!put_bytes("output", "old", 3));
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_source *source = NULL;
	pg_error error;
	pg_archive_build_options config = {
		{PG_HOGG10, flags, 0}, "scratch", 0};
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	if (session)
		PG_STATUS(pg_archive_builder_create_session(context, "output",
				"session", &config, &builder, &error), PG_OK);
	else
		PG_STATUS(pg_archive_builder_create_config(context, "output",
				&config, &builder, &error), PG_OK);
	std::string bytes(131073, 'x');
	pg_entry_options entry = {};
	entry.compression = PG_COMPRESS_NEVER;
	PG_STATUS(pg_archive_builder_write_all(builder, "payload", bytes.data(),
			bytes.size(), &entry, &error), PG_OK);
	struct stat staged, destination;
	PG_CHECK(!fstat(fileno(builder->staging), &staged));
	PG_CHECK(!fstat(builder->parent_fd, &destination));
	PG_CHECK(staged.st_dev == destination.st_dev);
	pg_test_journal_fault(fail_copy ? 18 : 17);
	pg_status status = pg_archive_builder_finish(builder, &error);
	pg_test_journal_fault(0);
	if (fail_copy) {
		PG_CHECK(status == PG_IO && error.native_code == EIO);
		PG_CHECK(!expect_bytes("output", "old", 3));
	} else {
		PG_CHECK(status == PG_OK && error.native_code == 0);
		PG_CHECK(!stat("output", &destination));
		PG_CHECK(staged.st_ino != destination.st_ino);
		PG_CHECK((uint64_t)destination.st_size ==
			builder->whole_archive_copied_bytes);
	}
	PG_CHECK(std::filesystem::exists(builder->staging_path));
	PG_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	if (session) {
		PG_STATUS(pg_archive_builder_resume(
				context, "session", &builder, &error), PG_OK);
		PG_STATUS(pg_archive_builder_finish(builder, &error),
			fail_copy ? PG_OK : PG_INVALID);
		PG_STATUS(pg_archive_builder_discard(&builder, &error), PG_OK);
	}
	PG_STATUS(pg_source_open(context, "output", NULL, &source, &error),
		PG_OK);
	PG_STATUS(pg_source_validate(source, &error), PG_OK);
	std::string read(bytes.size(), '\0');
	size_t count;
	PG_STATUS(pg_source_read_all(source, "payload", read.data(),
			read.size(), &count, &error), PG_OK);
	PG_CHECK(count == bytes.size() && read == bytes);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	PG_CHECK(std::filesystem::is_empty("scratch"));
	for (const auto &item : std::filesystem::directory_iterator(".")) {
		std::string name = item.path().filename().string();

		PG_CHECK(name.find("output.piggle-") == std::string::npos);
	}
	return 0;
}

TEST_F(ApiBuilder, ExdevLinkPublication)
{
	ASSERT_EQ(0, exdev_publication(0, false, false));
}

TEST_F(ApiBuilder, ExdevRenamePublication)
{
	ASSERT_EQ(0, exdev_publication(PG_OVERWRITE, true, false));
}

TEST_F(ApiBuilder, ExdevRenameWithoutExistingDestination)
{
	ASSERT_EQ(0, exdev_publication(PG_OVERWRITE, false, false));
}

TEST_F(ApiBuilder, ExdevLinkSessionPublication)
{
	ASSERT_EQ(0, exdev_publication(0, false, true));
}

TEST_F(ApiBuilder, ExdevRenameSessionPublication)
{
	ASSERT_EQ(0, exdev_publication(PG_OVERWRITE, true, true));
}

TEST_F(ApiBuilder, ExdevCopyFailurePreservesCheckpoint)
{
	ASSERT_EQ(0, exdev_publication(PG_OVERWRITE, true, true, true));
}

TEST_F(ApiBuilder, LowSpaceLeavesNamesAvailable)
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_source *source = NULL;
	pg_error error;
	EXPECT_STATUS(pg_context_open(&context, &error), PG_OK);
	EXPECT_STATUS(pg_archive_builder_create(context, "space.hogg",
			PG_HOGG10, 0, &builder, &error),
		PG_OK);
	EXPECT_STATUS(pg_archive_builder_write_all(
			builder, "keep", "safe", 4, NULL, &error),
		PG_OK);
	EXPECT_STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	EXPECT_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	pg_source_options writable = {PG_HOGG10, PG_WRITE, 0};
	EXPECT_STATUS(pg_source_open(context, "space.hogg", &writable, &source,
			&error),
		PG_OK);
	pg_test_journal_fault(12);
	pg_status status =
		pg_source_write_all(source, "retry", "x", 1, NULL, &error);
	pg_test_journal_fault(0);
	EXPECT_EQ(PG_IO, status);
	EXPECT_EQ(ENOSPC, error.native_code);
	EXPECT_STATUS(
		pg_source_write_all(source, "retry", "x", 1, NULL, &error),
		PG_OK);
	char bytes[4];
	size_t count;
	EXPECT_STATUS(pg_source_read_all(source, "keep", bytes, sizeof(bytes),
			&count, &error),
		PG_OK);
	EXPECT_EQ(0, memcmp(bytes, "safe", 4));
	EXPECT_STATUS(pg_source_close(&source, &error), PG_OK);
	EXPECT_STATUS(pg_context_close(&context, &error), PG_OK);
}

#endif
#ifndef _WIN32
TEST_F(ApiBuilder, UnwritableStagingDirectory)
{
	if (!geteuid())
		GTEST_SKIP()
			<< "Permission denial requires an unprivileged user";
	ASSERT_EQ(0, directory("blocked"));
	ASSERT_EQ(0, chmod("blocked", 0000));
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_error error;
	EXPECT_STATUS(pg_context_open(&context, &error), PG_OK);
	for (uint32_t format : {PG_PIGG2, PG_HOGG10}) {
		pg_archive_build_options config = {
			{format, 0, 0}, "blocked", 0};
		EXPECT_STATUS(pg_archive_builder_create_config(context,
				"archive", &config, &builder, &error),
			PG_IO);
		EXPECT_EQ(nullptr, builder);
	}
	EXPECT_STATUS(pg_context_close(&context, &error), PG_OK);
	ASSERT_EQ(0, chmod("blocked", 0700));
}

#endif
} // namespace
