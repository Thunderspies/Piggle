#include "../api_test.hpp"
#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

namespace {

static int process(const char *executable, const char *path, int busy)
{
#ifdef _WIN32
	return (int)_spawnl(_P_WAIT, executable, executable, path,
		busy ? "busy" : "free", NULL);
#else
	pid_t pid = fork();
	int status;

	if (pid < 0)
		return 1;
	if (!pid) {
		execl(executable, executable, path,
			busy ? "busy" : "free", (char *)NULL);
		_exit(2);
	}
	if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
		return 1;
	return WEXITSTATUS(status);
#endif
}

static int lease_contract()
{
	pg_context *context = NULL, *other = NULL;
	pg_source *source = NULL;
	pg_file *file = NULL;
	pg_reader *reader = NULL;
	pg_archive_builder *builder = NULL;
	pg_writer *writer = NULL;
	pg_source_options options = {PG_HOGG10, PG_WRITE, 0};
	pg_write_options write;
	pg_error error;
	struct stat before, after;
	size_t count;
	unsigned char bytes[16];

	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(pg_context_open(&other, &error), PG_OK);
	PG_STATUS(pg_archive_builder_create(context, "lease", PG_HOGG10,
				0, &builder, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_write_all(builder, "a", payload,
				sizeof(payload), NULL, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	PG_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
#ifdef _WIN32
	PG_CHECK(CreateHardLinkA("alias", "lease", NULL));
#else
	PG_CHECK(!link("lease", "alias"));
#endif
	PG_STATUS(pg_source_open(
				context, "lease", &options, &source, &error),
		PG_OK);
	PG_CHECK(!process(PIGGLE_TEST_LEASE_PATH, "lease", 1));
	PG_CHECK(!process(PIGGLE_TEST_LEASE_PATH, "alias", 1));
	PG_STATUS(pg_source_find(source, "a", &file, &error), PG_OK);
	PG_STATUS(
		pg_reader_open(file, PG_READ_LOGICAL, &reader, &error),
		PG_OK);
	PG_CHECK(!stat("lease", &before));
	PG_STATUS(pg_source_recover(other, "alias", &error), PG_BUSY);
	PG_STATUS(pg_archive_builder_create(other, "lease", PG_HOGG10,
				PG_OVERWRITE, &builder, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_finish(builder, &error), PG_BUSY);
	PG_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	pg_write_options_init(&write, 1);
	PG_STATUS(pg_writer_open_native(other, "alias", &write,
				PG_OVERWRITE, &writer, &error),
		PG_OK);
	PG_STATUS(
		pg_writer_write(writer, "x", 1, &count, &error), PG_OK);
	PG_STATUS(pg_writer_finish(writer, &error), PG_BUSY);
	PG_STATUS(pg_writer_close(&writer, &error), PG_OK);
	PG_CHECK(!stat("lease", &after) &&
		before.st_size == after.st_size);
	PG_STATUS(pg_reader_read(
				reader, bytes, sizeof(bytes), &count, &error),
		PG_OK);
	PG_CHECK(count == sizeof(payload) &&
		!memcmp(bytes, payload, count));
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_CHECK(!process(PIGGLE_TEST_LEASE_PATH, "alias", 1));
	PG_STATUS(pg_reader_close(&reader, &error), PG_OK);
	PG_CHECK(!process(PIGGLE_TEST_LEASE_PATH, "alias", 0));
	PG_STATUS(pg_context_close(&other, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

using ApiLease = piggle_test::ContractTest;

TEST_F(ApiLease, api_lease)
{
	ASSERT_EQ(0, lease_contract());
}

} // namespace
