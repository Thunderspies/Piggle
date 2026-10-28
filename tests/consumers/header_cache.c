#include <piggle/header_cache.h>

/* The standalone header provides its result's complete cleanup contract. */
void piggle_header_cache_c11_include(void)
{
	pg_buffer buffer = {0};

	pg_buffer_free(&buffer);
}
