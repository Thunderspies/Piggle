/* Context and virtual-name utilities. Common contracts: docs/api.md. */
#ifndef PIGGLE_CONTEXT_H
#define PIGGLE_CONTEXT_H
#include <piggle/types.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Owned context; close requires all child handles to be closed. */
PG_API pg_status PG_CALL pg_context_open(pg_context **out,
		pg_error *error);
PG_API pg_status PG_CALL pg_context_close(pg_context **context,
		pg_error *error);

/* Normalize a virtual name. CAPACITY sets required including NUL and leaves
 * buffer untouched. Other failures set required to zero. */
PG_API pg_status PG_CALL pg_name_normalize(pg_context *context,
		const char *name, char *buffer, size_t capacity,
		size_t *required, pg_error *error);

#ifdef __cplusplus
}
#endif
#endif
