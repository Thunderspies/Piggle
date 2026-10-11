#include <piggle/piggle.h>
#include <string.h>

_Static_assert(sizeof(pg_status) == sizeof(int32_t), "status ABI");
_Static_assert(sizeof(pg_id) == sizeof(uint64_t), "identity ABI");

int piggle_c11_contract(void)
{
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
