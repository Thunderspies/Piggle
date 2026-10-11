#include "../support.hpp"

namespace {

#ifndef _WIN32
static int memory_probe(const char *format)
{
	pid_t process = fork();
	int status;

	PG_CHECK(process >= 0);
	if (!process) {
		execl(PIGGLE_TEST_MEMORY_PATH, PIGGLE_TEST_MEMORY_PATH,
			format, static_cast<char *>(nullptr));
		_exit(2);
	}
	PG_CHECK(waitpid(process, &status, 0) == process);
	PG_CHECK(WIFEXITED(status));
	return WEXITSTATUS(status);
}

using ApiMemory = piggle_test::ContractTest;

TEST_F(ApiMemory, api_memory_pigg)
{
	ASSERT_EQ(0, memory_probe("pigg"));
}

TEST_F(ApiMemory, api_memory_hogg)
{
	ASSERT_EQ(0, memory_probe("hogg"));
}
#endif

} // namespace
