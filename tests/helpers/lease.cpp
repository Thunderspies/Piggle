#include "../api_test.hpp"
static int child(const char *path, int busy)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_source_options options = {PG_HOGG10, PG_WRITE, 0};
	pg_error error;

	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(pg_source_open(
				context, path, &options, &source, &error),
		busy ? PG_BUSY : PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

int main(int argc, char **argv)
{
	PG_CHECK(argc == 3);
	return child(argv[1], !strcmp(argv[2], "busy"));
}
