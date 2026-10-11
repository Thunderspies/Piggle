#include "../support.hpp"
#include <piggle/piggle.h>

#include <stdio.h>
#include <string.h>

namespace {

#define EXPECT_INVALID(call)                                           \
	do {                                                           \
		pg_status status = (call);                             \
		if (status != PG_INVALID || error.status != status ||  \
			error.cause != status ||                       \
			error.native_code != 0 ||                      \
			error.offset != UINT64_MAX ||                  \
			memchr(error.message, 0,                       \
				sizeof(error.message)) == NULL) {      \
			fprintf(stderr, "%s: got %d, error %d\n",      \
				#call, (int)status,                    \
				(int)error.status);                    \
			failures++;                                    \
		}                                                      \
	} while (0)

static int test_context(void)
{
	pg_error error;
	int failures = 0;

	EXPECT_INVALID(pg_context_open(NULL, &error));
	EXPECT_INVALID(pg_context_close(NULL, &error));
	EXPECT_INVALID(
		pg_name_normalize(NULL, NULL, NULL, 0, NULL, &error));

	return failures != 0;
}

static int test_io(void)
{
	pg_error error;
	int failures = 0;

	EXPECT_INVALID(
		pg_reader_open_source(NULL, NULL, 0, NULL, &error));
	EXPECT_INVALID(
		pg_reader_open_tree(NULL, NULL, 0, NULL, &error));
	EXPECT_INVALID(pg_reader_open(NULL, 0, NULL, &error));
	EXPECT_INVALID(pg_reader_open_native(NULL, NULL, NULL, &error));
	EXPECT_INVALID(pg_reader_inspect(NULL, NULL, &error));
	EXPECT_INVALID(pg_reader_read(NULL, NULL, 0, NULL, &error));
	EXPECT_INVALID(pg_reader_close(NULL, &error));
	EXPECT_INVALID(
		pg_writer_open_source(NULL, NULL, NULL, NULL, &error));
	EXPECT_INVALID(pg_writer_open_file(NULL, NULL, NULL, &error));
	EXPECT_INVALID(pg_writer_open_archive_builder(
		NULL, NULL, NULL, NULL, &error));
	EXPECT_INVALID(pg_writer_open_native(
		NULL, NULL, NULL, 0, NULL, &error));
	EXPECT_INVALID(
		pg_unpack_target_open(NULL, NULL, 0, NULL, &error));
	EXPECT_INVALID(pg_writer_open_unpack(NULL, NULL, NULL, &error));
	EXPECT_INVALID(pg_unpack_target_close(NULL, &error));
	EXPECT_INVALID(pg_writer_write(NULL, NULL, 0, NULL, &error));
	EXPECT_INVALID(pg_writer_finish(NULL, &error));
	EXPECT_INVALID(pg_writer_close(NULL, &error));

	return failures != 0;
}

static int test_file(void)
{
	pg_error error;
	int failures = 0;

	EXPECT_INVALID(pg_file_read_all(NULL, NULL, 0, NULL, &error));
	EXPECT_INVALID(pg_file_read_all_alloc(NULL, 0, NULL, &error));
	EXPECT_INVALID(pg_file_write_all(NULL, NULL, 0, NULL, &error));
	EXPECT_INVALID(pg_file_export(NULL, NULL, 0, &error));
	EXPECT_INVALID(pg_file_inspect(NULL, NULL, &error));
	EXPECT_INVALID(pg_cursor_next(NULL, NULL, &error));
	EXPECT_INVALID(pg_file_verify(NULL, &error));
	EXPECT_INVALID(pg_file_delete(NULL, &error));
	EXPECT_INVALID(pg_file_close(NULL, &error));
	EXPECT_INVALID(pg_cursor_close(NULL, &error));

	return failures != 0;
}

static int test_source(void)
{
	pg_error error;
	int failures = 0;

	EXPECT_INVALID(pg_source_open(NULL, NULL, NULL, NULL, &error));
	EXPECT_INVALID(pg_source_inspect(NULL, NULL, &error));
	EXPECT_INVALID(
		pg_source_read_all(NULL, NULL, NULL, 0, NULL, &error));
	EXPECT_INVALID(
		pg_source_read_all_alloc(NULL, NULL, 0, NULL, &error));
	EXPECT_INVALID(
		pg_source_write_all(NULL, NULL, NULL, 0, NULL, &error));
	EXPECT_INVALID(pg_source_import(NULL, NULL, NULL, 0, &error));
	EXPECT_INVALID(pg_source_copy(NULL, NULL, NULL, 0, &error));
	EXPECT_INVALID(pg_source_export(NULL, NULL, NULL, 0, &error));
	EXPECT_INVALID(pg_source_pack(NULL, NULL, 0, NULL, &error));
	EXPECT_INVALID(pg_source_unpack(NULL, NULL, 0, &error));
	EXPECT_INVALID(pg_source_find(NULL, NULL, NULL, &error));
	EXPECT_INVALID(pg_source_files(NULL, NULL, NULL, &error));
	EXPECT_INVALID(pg_source_request_subtree(NULL, NULL, &error));
	EXPECT_INVALID(pg_source_rescan(NULL, &error));
	EXPECT_INVALID(pg_source_validate(NULL, &error));
	EXPECT_INVALID(pg_source_recover(NULL, NULL, &error));
	EXPECT_INVALID(pg_source_delete(NULL, &error));
	EXPECT_INVALID(pg_source_close(NULL, &error));

	return failures != 0;
}

static int test_tree(void)
{
	pg_error error;
	int failures = 0;

	EXPECT_INVALID(pg_tree_create(NULL, NULL, &error));
	EXPECT_INVALID(pg_tree_inspect(NULL, NULL, &error));
	EXPECT_INVALID(pg_tree_open(NULL, NULL, 0, NULL, &error));
	EXPECT_INVALID(pg_tree_attach(NULL, NULL, &error));
	EXPECT_INVALID(pg_tree_detach(NULL, NULL, &error));
	EXPECT_INVALID(pg_tree_source(NULL, 0, NULL, &error));
	EXPECT_INVALID(
		pg_tree_read_all(NULL, NULL, NULL, 0, NULL, &error));
	EXPECT_INVALID(
		pg_tree_read_all_alloc(NULL, NULL, 0, NULL, &error));
	EXPECT_INVALID(pg_tree_export(NULL, NULL, NULL, 0, &error));
	EXPECT_INVALID(pg_tree_pack(NULL, NULL, 0, NULL, &error));
	EXPECT_INVALID(pg_tree_unpack(NULL, NULL, 0, &error));
	EXPECT_INVALID(pg_tree_find(NULL, NULL, NULL, &error));
	EXPECT_INVALID(pg_tree_files(NULL, NULL, NULL, &error));
	EXPECT_INVALID(pg_tree_request_subtree(NULL, NULL, &error));
	EXPECT_INVALID(pg_tree_rescan(NULL, &error));
	EXPECT_INVALID(pg_tree_close(NULL, &error));

	return failures != 0;
}

static int test_change(void)
{
	pg_error error;
	int failures = 0;

	EXPECT_INVALID(pg_tree_watch(NULL, 0, &error));
	EXPECT_INVALID(pg_tree_unwatch(NULL, &error));
	EXPECT_INVALID(pg_tree_poll(NULL, NULL, &error));

	return failures != 0;
}

static int test_archive_builder(void)
{
	pg_error error;
	int failures = 0;

	EXPECT_INVALID(pg_archive_builder_create(
		NULL, NULL, 0, 0, NULL, &error));
	EXPECT_INVALID(pg_archive_builder_write_all(
		NULL, NULL, NULL, 0, NULL, &error));
	EXPECT_INVALID(
		pg_archive_builder_import(NULL, NULL, NULL, 0, &error));
	EXPECT_INVALID(
		pg_archive_builder_copy(NULL, NULL, NULL, 0, &error));
	EXPECT_INVALID(pg_archive_builder_finish(NULL, &error));
	EXPECT_INVALID(pg_archive_builder_close(NULL, &error));

	return failures != 0;
}

using ApiInvalid = piggle_test::ContractTest;

TEST_F(ApiInvalid, api_invalid_context)
{
	ASSERT_EQ(0, test_context());
}

TEST_F(ApiInvalid, api_invalid_io)
{
	ASSERT_EQ(0, test_io());
}

TEST_F(ApiInvalid, api_invalid_file)
{
	ASSERT_EQ(0, test_file());
}

TEST_F(ApiInvalid, api_invalid_source)
{
	ASSERT_EQ(0, test_source());
}

TEST_F(ApiInvalid, api_invalid_tree)
{
	ASSERT_EQ(0, test_tree());
}

TEST_F(ApiInvalid, api_invalid_change)
{
	ASSERT_EQ(0, test_change());
}

TEST_F(ApiInvalid, api_invalid_archive_builder)
{
	ASSERT_EQ(0, test_archive_builder());
}

} // namespace
