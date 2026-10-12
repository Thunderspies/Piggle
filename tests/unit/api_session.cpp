#include "../api_test.hpp"
#include <fstream>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

namespace
{
using ApiSession = piggle_test::ContractTest;
#define EXPECT_STATUS(call, expected) ASSERT_EQ(expected, call)
#ifdef __linux__
extern "C" void pg_test_journal_fault(int mode);
#endif

static int child(const char *mode)
{
#ifdef _WIN32
	return (int)_spawnl(_P_WAIT, PIGGLE_TEST_SESSION_PATH,
		PIGGLE_TEST_SESSION_PATH, mode, NULL);
#else
	pid_t pid = fork();
	int status;

	if (pid < 0)
		return 1;
	if (!pid) {
		execl(PIGGLE_TEST_SESSION_PATH, PIGGLE_TEST_SESSION_PATH, mode,
			(char *)NULL);
		_exit(2);
	}
	if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
		return 1;
	return WEXITSTATUS(status);
#endif
}

static pg_status PG_CALL inventory(void *state, const pg_file_info *entry)
{
	auto *names = static_cast<std::vector<std::string> *>(state);

	names->push_back(entry->original_name);
	return PG_OK;
}

TEST_F(ApiSession, CheckpointSuspendResumeAndDiscard)
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL, *other = NULL;
	pg_writer *writer = NULL;
	pg_error error;
	pg_archive_build_options options = {{PG_HOGG10, 0, 0}, NULL, 32};
	EXPECT_STATUS(pg_context_open(&context, &error), PG_OK);
	EXPECT_STATUS(pg_archive_builder_create_session(context, "output",
			"session", &options, &builder, &error),
		PG_OK);
	EXPECT_STATUS(pg_archive_builder_write_all(
			builder, "First", "abc", 3, NULL, &error),
		PG_OK);
	EXPECT_STATUS(pg_archive_builder_checkpoint(builder, &error), PG_OK);
	EXPECT_STATUS(
		pg_archive_builder_resume(context, "session", &other, &error),
		PG_BUSY);
	EXPECT_EQ(nullptr, other);
	EXPECT_STATUS(pg_archive_builder_write_all(
			builder, "Ignored", "x", 1, NULL, &error),
		PG_OK);
	EXPECT_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	EXPECT_STATUS(
		pg_archive_builder_resume(context, "session", &builder, &error),
		PG_OK);
	std::vector<std::string> names;
	EXPECT_STATUS(pg_archive_builder_visit_inventory(
			builder, inventory, &names, &error),
		PG_OK);
	EXPECT_EQ((std::vector<std::string>{"First"}), names);
	pg_write_options write;
	pg_write_options_init(&write, 3);
	EXPECT_STATUS(pg_writer_open_archive_builder(
			builder, "Second", &write, &writer, &error),
		PG_OK);
	EXPECT_STATUS(pg_archive_builder_checkpoint(builder, &error), PG_BUSY);
	EXPECT_STATUS(pg_archive_builder_suspend(&builder, &error), PG_BUSY);
	EXPECT_STATUS(pg_writer_close(&writer, &error), PG_OK);
	EXPECT_STATUS(pg_archive_builder_write_all(
			builder, "Second", "def", 3, NULL, &error),
		PG_OK);
	EXPECT_STATUS(pg_archive_builder_suspend(&builder, &error), PG_OK);
	EXPECT_EQ(nullptr, builder);
	EXPECT_STATUS(
		pg_archive_builder_resume(context, "session", &builder, &error),
		PG_OK);
	EXPECT_STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	EXPECT_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	EXPECT_STATUS(
		pg_archive_builder_resume(context, "session", &builder, &error),
		PG_OK);
	EXPECT_STATUS(pg_archive_builder_finish(builder, &error), PG_INVALID);
	EXPECT_STATUS(pg_archive_builder_discard(&builder, &error), PG_OK);
	EXPECT_EQ(nullptr, builder);
	pg_source *source = NULL;
	EXPECT_STATUS(pg_source_open(context, "output", NULL, &source, &error),
		PG_OK);
	EXPECT_STATUS(pg_source_validate(source, &error), PG_OK);
	EXPECT_STATUS(pg_source_close(&source, &error), PG_OK);
	EXPECT_STATUS(pg_context_close(&context, &error), PG_OK);
}

TEST_F(ApiSession, CopiedCheckpointCannotOwnLivePayload)
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL, *other = NULL;
	pg_error error;
	pg_archive_build_options options = {{PG_HOGG10, 0, 0}, NULL, 0};
	EXPECT_STATUS(pg_context_open(&context, &error), PG_OK);
	EXPECT_STATUS(pg_archive_builder_create_session(context, "output",
			"session", &options, &builder, &error),
		PG_OK);
	EXPECT_STATUS(pg_archive_builder_write_all(
			builder, "First", "abc", 3, NULL, &error),
		PG_OK);
	EXPECT_STATUS(pg_archive_builder_checkpoint(builder, &error), PG_OK);
	std::filesystem::create_directory("copy");
	std::filesystem::copy_file("session/checkpoint", "copy/checkpoint");
	ASSERT_EQ(0, put_bytes("copy/lock", NULL, 0));
	EXPECT_STATUS(
		pg_archive_builder_resume(context, "copy", &other, &error),
		PG_BUSY);
	EXPECT_EQ(nullptr, other);
	EXPECT_EQ(0, child("busy-copy"));
	EXPECT_STATUS(pg_archive_builder_write_all(
			builder, "Second", "def", 3, NULL, &error),
		PG_OK);
	EXPECT_STATUS(pg_archive_builder_suspend(&builder, &error), PG_OK);
	EXPECT_STATUS(
		pg_archive_builder_resume(context, "session", &builder, &error),
		PG_OK);
	EXPECT_STATUS(
		pg_archive_builder_resume(context, "copy", &other, &error),
		PG_BUSY);
	EXPECT_EQ(nullptr, other);
	EXPECT_STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	EXPECT_STATUS(pg_archive_builder_discard(&builder, &error), PG_OK);
	pg_source *source = NULL;
	EXPECT_STATUS(pg_source_open(context, "output", NULL, &source, &error),
		PG_OK);
	EXPECT_STATUS(pg_source_validate(source, &error), PG_OK);
	char data[3];
	size_t count;
	EXPECT_STATUS(pg_source_read_all(
			source, "Second", data, sizeof(data), &count, &error),
		PG_OK);
	EXPECT_EQ(3u, count);
	EXPECT_EQ(0, memcmp(data, "def", 3));
	EXPECT_STATUS(pg_source_close(&source, &error), PG_OK);
	EXPECT_STATUS(pg_context_close(&context, &error), PG_OK);
}

#ifdef __linux__
static int intent_flush_failure(bool existing, bool report_error)
{
	if (existing)
		PG_CHECK(!put_bytes("output", "old", 3));
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_error error;
	pg_archive_build_options options = {
		{PG_HOGG10, PG_OVERWRITE, 0}, NULL, 0};
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(pg_archive_builder_create_session(context, "output",
			"session", &options, &builder, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_write_all(
			builder, "First", "abc", 3, NULL, &error),
		PG_OK);
	pg_test_journal_fault(16);
	pg_status status = pg_archive_builder_finish(
		builder, report_error ? &error : NULL);
	pg_test_journal_fault(0);
	PG_CHECK(status == PG_IO);
	if (report_error)
		PG_CHECK(error.status == PG_IO && error.cause == PG_IO &&
			error.native_code == EIO);
	PG_CHECK(existing ? !expect_bytes("output", "old", 3)
			 : !expect_absent("output"));
	PG_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	PG_STATUS(
		pg_archive_builder_resume(context, "session", &builder, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	PG_STATUS(pg_archive_builder_discard(&builder, &error), PG_OK);
	pg_source *source = NULL;
	PG_STATUS(pg_source_open(context, "output", NULL, &source, &error),
		PG_OK);
	PG_STATUS(pg_source_validate(source, &error), PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

TEST_F(ApiSession, IntentFlushFailureDoesNotCommitOutput)
{
	ASSERT_EQ(0, intent_flush_failure(false, true));
}

TEST_F(ApiSession, IntentFlushFailurePreservesExistingOutput)
{
	ASSERT_EQ(0, intent_flush_failure(true, true));
}

TEST_F(ApiSession, IntentFlushFailureWithoutErrorOutput)
{
	ASSERT_EQ(0, intent_flush_failure(false, false));
}
#endif
static int interrupted(const char *mode, size_t completed, bool published)
{
	PG_CHECK(child(mode) == 0);
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_error error;
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(
		pg_archive_builder_resume(context, "session", &builder, &error),
		PG_OK);
	std::vector<std::string> names;
	PG_STATUS(pg_archive_builder_visit_inventory(
			builder, inventory, &names, &error),
		PG_OK);
	PG_CHECK(names.size() == completed && names[0] == "First");
	if (published)
		PG_STATUS(
			pg_archive_builder_finish(builder, &error), PG_INVALID);
	PG_STATUS(pg_archive_builder_discard(&builder, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

TEST_F(ApiSession, ProcessInterruption)
{
	ASSERT_EQ(0, interrupted("create", 1, false));
}

#ifdef __linux__
TEST_F(ApiSession, BeforeCheckpointSelection)
{
	ASSERT_EQ(0, interrupted("before-checkpoint", 1, false));
}

TEST_F(ApiSession, AfterCheckpointSelection)
{
	ASSERT_EQ(0, interrupted("after-checkpoint", 2, false));
}

TEST_F(ApiSession, AfterPublication)
{
	ASSERT_EQ(0, interrupted("after-publication", 2, true));
}
#endif

TEST_F(ApiSession, CorruptCheckpoint)
{
	ASSERT_EQ(0, child("create"));
	FILE *image = fopen("session/checkpoint", "r+b");
	ASSERT_NE(nullptr, image);
	ASSERT_EQ(0, fseek(image, 12, SEEK_SET));
	ASSERT_NE(EOF, fputc(0x7f, image));
	ASSERT_EQ(0, fclose(image));
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_error error;
	EXPECT_STATUS(pg_context_open(&context, &error), PG_OK);
	EXPECT_STATUS(
		pg_archive_builder_resume(context, "session", &builder, &error),
		PG_CORRUPT);
	EXPECT_EQ(nullptr, builder);
	EXPECT_STATUS(pg_context_close(&context, &error), PG_OK);
}
TEST_F(ApiSession, CorruptPayload)
{
	ASSERT_EQ(0, child("create"));
	std::filesystem::path staging;
	for (const auto &item : std::filesystem::directory_iterator("."))
		if (item.path().filename().string().find(".piggle-stage-") == 0)
			staging = item.path();
	ASSERT_FALSE(staging.empty());
	std::ifstream input(staging, std::ios::binary);
	std::string data((std::istreambuf_iterator<char>(input)),
		std::istreambuf_iterator<char>());
	input.close();
	size_t offset = data.find("abc");
	ASSERT_NE(std::string::npos, offset);
	FILE *stored = fopen(staging.string().c_str(), "r+b");
	ASSERT_NE(nullptr, stored);
	ASSERT_EQ(0, fseek(stored, (long)offset, SEEK_SET));
	ASSERT_NE(EOF, fputc('X', stored));
	ASSERT_EQ(0, fclose(stored));
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_error error;
	EXPECT_STATUS(pg_context_open(&context, &error), PG_OK);
	EXPECT_STATUS(
		pg_archive_builder_resume(context, "session", &builder, &error),
		PG_CHECKSUM);
	EXPECT_EQ(nullptr, builder);
	EXPECT_STATUS(pg_context_close(&context, &error), PG_OK);
}

struct visit_state {
	pg_archive_builder **builder;
	pg_status write_status;
	pg_status close_status;
};

static pg_status PG_CALL reentrant_inventory(void *state, const pg_file_info *)
{
	auto *visit = static_cast<visit_state *>(state);
	pg_error error;

	visit->write_status = pg_archive_builder_write_all(
		*visit->builder, "reentrant", NULL, 0, NULL, &error);
	visit->close_status = pg_archive_builder_close(visit->builder, &error);
	return PG_END;
}

TEST_F(ApiSession, InventoryRejectsReentrantMutation)
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_error error;
	EXPECT_STATUS(pg_context_open(&context, &error), PG_OK);
	EXPECT_STATUS(pg_archive_builder_create(context, "inventory", PG_HOGG10,
			0, &builder, &error),
		PG_OK);
	EXPECT_STATUS(pg_archive_builder_write_all(
			builder, "a", NULL, 0, NULL, &error),
		PG_OK);
	visit_state state = {&builder, PG_OK, PG_OK};
	EXPECT_STATUS(pg_archive_builder_visit_inventory(
			builder, reentrant_inventory, &state, &error),
		PG_END);
	EXPECT_EQ(PG_REENTRANT, state.write_status);
	EXPECT_EQ(PG_REENTRANT, state.close_status);
	ASSERT_NE(nullptr, builder);
	EXPECT_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	EXPECT_STATUS(pg_context_close(&context, &error), PG_OK);
}
#ifndef _WIN32
TEST_F(ApiSession, FailedCrossVolumeFinishRemovesDestinationCopy)
{
	std::filesystem::path scratch = std::filesystem::temp_directory_path() /
		("piggle-session-" +
			std::filesystem::current_path().filename().string());
	std::filesystem::create_directory(scratch);
	struct stat here, there;
	ASSERT_EQ(0, stat(".", &here));
	ASSERT_EQ(0, stat(scratch.string().c_str(), &there));
	if (here.st_dev == there.st_dev) {
		std::filesystem::remove(scratch);
		GTEST_SKIP() << "A separate staging volume is unavailable";
	}
	ASSERT_EQ(0, put_bytes("output", "old", 3));
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_error error;
	std::string stage = scratch.string();
	pg_archive_build_options options = {
		{PG_HOGG10, PG_OVERWRITE, 0}, stage.c_str(), 0};
	EXPECT_STATUS(pg_context_open(&context, &error), PG_OK);
	EXPECT_STATUS(pg_archive_builder_create_session(context, "output",
			"session", &options, &builder, &error),
		PG_OK);
	EXPECT_STATUS(pg_archive_builder_write_all(
			builder, "First", "abc", 3, NULL, &error),
		PG_OK);
	ASSERT_EQ(0, put_bytes("output", "changed", 7));
	EXPECT_STATUS(pg_archive_builder_finish(builder, &error), PG_STALE);
	EXPECT_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	EXPECT_STATUS(
		pg_archive_builder_resume(context, "session", &builder, &error),
		PG_OK);
	std::vector<std::string> names;
	EXPECT_STATUS(pg_archive_builder_visit_inventory(
			builder, inventory, &names, &error),
		PG_OK);
	EXPECT_EQ((std::vector<std::string>{"First"}), names);
	EXPECT_STATUS(pg_archive_builder_discard(&builder, &error), PG_OK);
	EXPECT_STATUS(pg_context_close(&context, &error), PG_OK);
	EXPECT_EQ(0, expect_bytes("output", "changed", 7));
	EXPECT_TRUE(std::filesystem::is_empty(scratch));
	std::filesystem::remove(scratch);
	for (const auto &item : std::filesystem::directory_iterator("."))
		EXPECT_EQ(std::string::npos,
			item.path().filename().string().find("output.piggle-"));
}

TEST_F(ApiSession, PublishedSessionAcrossVolumes)
{
	std::filesystem::path scratch = std::filesystem::temp_directory_path() /
		("piggle-session-" +
			std::filesystem::current_path().filename().string());
	std::filesystem::create_directory(scratch);
	struct stat here, there;
	ASSERT_EQ(0, stat(".", &here));
	ASSERT_EQ(0, stat(scratch.string().c_str(), &there));
	if (here.st_dev == there.st_dev) {
		std::filesystem::remove(scratch);
		GTEST_SKIP() << "A separate staging volume is unavailable";
	}
	ASSERT_EQ(0,
		setenv("PIGGLE_SESSION_TEST_STAGE", scratch.string().c_str(),
			1));
	int result = child("publish");
	ASSERT_EQ(0, unsetenv("PIGGLE_SESSION_TEST_STAGE"));
	ASSERT_EQ(0, result);
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_error error;
	EXPECT_STATUS(pg_context_open(&context, &error), PG_OK);
	EXPECT_STATUS(
		pg_archive_builder_resume(context, "session", &builder, &error),
		PG_OK);
	EXPECT_STATUS(pg_archive_builder_finish(builder, &error), PG_INVALID);
	EXPECT_STATUS(pg_archive_builder_discard(&builder, &error), PG_OK);
	EXPECT_STATUS(pg_context_close(&context, &error), PG_OK);
	EXPECT_TRUE(std::filesystem::is_empty(scratch));
	std::filesystem::remove(scratch);
}
#endif
} // namespace
