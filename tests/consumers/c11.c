#include <piggle/piggle.h>
#include <string.h>

_Static_assert(sizeof(pg_status) == sizeof(int32_t), "status ABI");
_Static_assert(sizeof(pg_id) == sizeof(uint64_t), "identity ABI");

int piggle_c11_contract(void)
{
	pg_archive_build_options options = {{PG_HOGG10, 0, 0}, NULL, 0};
	pg_archive_builder *builder = NULL;
	pg_error contract_error;
	if (pg_archive_builder_create_config(NULL, "invalid", &options,
		&builder, &contract_error) != PG_INVALID ||
		builder)
		return 1;
	pg_source_storage_info storage;
	if (pg_source_set_staging_directory(NULL, NULL, &contract_error) !=
			PG_INVALID ||
		pg_source_storage_inspect(NULL, &storage, &contract_error) !=
			PG_INVALID ||
		storage.physical_length ||
		pg_source_compact(NULL, "invalid", NULL, &contract_error) !=
			PG_INVALID)
		return 1;
	pg_context *context = NULL;
	pg_error error;
	char name[32];
	size_t required = 0;
	pg_status status = pg_context_open(&context, &error);

	if (status != PG_OK || error.status != status || !context)
		return 1;
	status = pg_name_normalize(context, "Dir\\A", name,
		sizeof(name), &required, &error);
	int result = status != PG_OK || strcmp(name, "dir/a") ||
		required != 6;

	status = pg_context_close(&context, &error);
	return result || status != PG_OK || context != NULL;
}
