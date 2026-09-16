#include <piggle/piggle.h>
#include "internal.hpp"

extern "C" {

/* Validate output; allocate and initialize context state. */
/* Initialize nonzero identity sequence and ownership counts. */
/* Publish only after setup; release partial state on failure. */
PG_API pg_status PG_CALL pg_context_open(pg_context **out,
		pg_error *error)
{
	if (!out)
		return pg_result(PG_INVALID, error);
	*out = NULL;
	pg_context *context = (pg_context *)calloc(1, sizeof(*context));
	if (!context)
		return pg_result(PG_NOMEM, error);
	context->next_id = 1;
	*out = context;
	return pg_result(PG_OK, error);
}

/* Accept a null owned handle as a no-op; reject live dependents. */
/* Release context state, clear the owned pointer and report cleanup. */
PG_API pg_status PG_CALL pg_context_close(pg_context **context,
		pg_error *error)
{
	if (!context)
		return pg_result(PG_INVALID, error);
	if (!*context)
		return pg_result(PG_OK, error);
	if (pg_context_children(*context))
		return pg_result(PG_BUSY, error);
	free(*context);
	*context = NULL;
	return pg_result(PG_OK, error);
}

static pg_status pg_name_measure(const char *name, size_t *length)
{
	const unsigned char *at = (const unsigned char *)name;
	size_t size = 0;

	if (!*at || *at == '/' || *at == '\\')
		return PG_INVALID;
	while (*at) {
		const unsigned char *start;
		size_t component;

		while (*at == '/' || *at == '\\')
			at++;
		start = at;
		while (*at && *at != '/' && *at != '\\') {
			if (*at == ':')
				return PG_INVALID;
			at++;
		}
		component = (size_t)(at - start);
		if (component == 2 && start[0] == '.' && start[1] == '.')
			return PG_INVALID;
		if (!component || (component == 1 && start[0] == '.'))
			continue;
		if (size)
			size++;
		if (component > SIZE_MAX - size - 1)
			return PG_LIMIT;
		size += component;
	}
	if (!size)
		return PG_INVALID;
	*length = size;
	return PG_OK;
}

/* Validate the virtual path, separators and canonical components. */
/* Measure required capacity without writing a short buffer. */
/* Fold ASCII case into caller storage and report required size. */
PG_API pg_status PG_CALL pg_name_normalize(pg_context *context,
		const char *name, char *buffer, size_t capacity,
		size_t *required, pg_error *error)
{
	const unsigned char *at;
	size_t length = 0;
	size_t used = 0;
	pg_status status;

	if (required)
		*required = 0;
	if (!context || !name || !required || (capacity && !buffer))
		return pg_result(PG_INVALID, error);
	status = pg_name_measure(name, &length);
	if (status != PG_OK)
		return pg_result(status, error);
	*required = length + 1;
	if (capacity < length + 1)
		return pg_result(PG_CAPACITY, error);
	at = (const unsigned char *)name;
	while (*at) {
		const unsigned char *start;
		size_t component;

		while (*at == '/' || *at == '\\')
			at++;
		start = at;
		while (*at && *at != '/' && *at != '\\')
			at++;
		component = (size_t)(at - start);
		if (!component || (component == 1 && start[0] == '.'))
			continue;
		if (used)
			buffer[used++] = '/';
		while (start != at) {
			unsigned char ch = *start++;

			if (ch >= 'A' && ch <= 'Z')
				ch += 'a' - 'A';
			buffer[used++] = (char)ch;
		}
	}
	buffer[used] = '\0';
	return pg_result(PG_OK, error);
}

} /* extern "C" */
