#include <piggle/piggle.h>
#include "internal.hpp"

extern "C" {

/* Clear every option field and set the default entry metadata. */
/* Select logical input and copy logical_size to both lengths. */
PG_API void PG_CALL pg_write_options_init(pg_write_options *out,
		uint64_t logical_size)
{
	memset(out, 0, sizeof(*out));
	out->logical_size = logical_size;
	out->input_size = logical_size;
	out->encoding = PG_LOGICAL;
}

} /* extern "C" */
