#include <piggle/piggle.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif
#ifdef __linux__
extern "C" void pg_test_journal_fault(int mode);
#endif

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_error error;
	pg_archive_build_options options = {
		{PG_HOGG10, PG_OVERWRITE, 0},
		getenv("PIGGLE_SESSION_TEST_STAGE"), 0};

	if (pg_context_open(&context, &error))
		return 1;
	if (!strcmp(argv[1], "busy") || !strcmp(argv[1], "busy-copy")) {
		pg_status status = pg_archive_builder_resume(
			context, !strcmp(argv[1], "busy") ? "session" : "copy",
			&builder, &error);

		int result = status == PG_BUSY && !builder ? 0 : 1;

		pg_archive_builder_close(&builder, NULL);
		pg_context_close(&context, NULL);
		return result;
	}
	if (pg_archive_builder_create_session(
		context, "output", "session", &options, &builder, &error) ||
		pg_archive_builder_write_all(
			builder, "First", "abc", 3, NULL, &error) ||
		pg_archive_builder_checkpoint(builder, &error))
		return 1;
	if (pg_archive_builder_write_all(
		builder, "Second", "def", 3, NULL, &error))
		return 1;
#ifdef __linux__
	if (!strcmp(argv[1], "before-checkpoint")) {
		pg_test_journal_fault(9);
		pg_archive_builder_checkpoint(builder, &error);
		return 1;
	}
	if (!strcmp(argv[1], "after-checkpoint")) {
		pg_test_journal_fault(10);
		pg_archive_builder_checkpoint(builder, &error);
		return 1;
	}
	if (!strcmp(argv[1], "after-publication")) {
		pg_test_journal_fault(8);
		pg_archive_builder_finish(builder, &error);
		return 1;
	}
#endif
	if (!strcmp(argv[1], "publish") &&
		pg_archive_builder_finish(builder, &error))
		return 1;
	/* Simulate process termination without any handle cleanup. */
	_exit(0);
}
