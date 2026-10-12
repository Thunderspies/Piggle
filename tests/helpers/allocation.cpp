#include <piggle/piggle.h>
#include "../../src/internal.hpp"
#include <stdio.h>

static int fail_entry;
extern "C" void *__real_calloc(size_t count, size_t size);
extern "C" void *__wrap_calloc(size_t count, size_t size)
{
	if (fail_entry && count == 1 && size == sizeof(pg_builder_entry)) {
		fail_entry = 0;
		return NULL;
	}
	return __real_calloc(count, size);
}

int main()
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_writer *writer = NULL;
	pg_error error;
	if (pg_context_open(&context, &error) ||
		pg_archive_builder_create(context, "allocation.hogg", PG_HOGG10,
			0, &builder, &error) ||
		pg_archive_builder_write_all(
			builder, "keep", "safe", 4, NULL, &error))
		return 1;
	pg_write_options options;
	pg_write_options_init(&options, 1);
	fail_entry = 1;
	pg_status status = pg_writer_open_archive_builder(
		builder, "retry", &options, &writer, &error);
	fail_entry = 0;
	if (status != PG_NOMEM || writer || builder->entry_count != 1 ||
		pg_archive_builder_write_all(
			builder, "retry", "x", 1, NULL, &error) ||
		pg_archive_builder_finish(builder, &error) ||
		pg_archive_builder_close(&builder, &error))
		return 1;
	pg_source *source = NULL;
	char bytes[4];
	size_t count;
	if (pg_source_open(context, "allocation.hogg", NULL, &source, &error) ||
		pg_source_read_all(
			source, "keep", bytes, sizeof(bytes), &count, &error) ||
		count != 4 || memcmp(bytes, "safe", 4) ||
		pg_source_close(&source, &error) ||
		pg_context_close(&context, &error))
		return 1;
	return remove("allocation.hogg") != 0;
}
