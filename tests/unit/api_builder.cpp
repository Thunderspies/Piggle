#include "../api_test.hpp"
#include "../../src/internal.hpp"

namespace
{
using ApiBuilder = piggle_test::ContractTest;
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
} // namespace
