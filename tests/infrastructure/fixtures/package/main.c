#include <piggle/piggle.h>

int main(void)
{
	pg_context *context = NULL;
	pg_error error;
	pg_status status = pg_context_open(&context, &error);

	if (status != PG_OK)
		return 1;
	return pg_context_close(&context, &error) != PG_OK;
}
