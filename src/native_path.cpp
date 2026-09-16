#include "internal.hpp"

char *pg_native_absolute(const char *path, pg_status *status,
		int *native_code)
{
	char *result;
	size_t length = strlen(path);

	if (path[0] == '/'
#ifdef _WIN32
	    || (length >= 3 && path[1] == ':' &&
		(path[2] == '/' || path[2] == '\\')) ||
	    (length >= 2 && path[0] == '\\' && path[1] == '\\')
#endif
	    ) {
		result = (char *)malloc(length + 1);
		if (!result)
			*status = PG_NOMEM;
		if (result)
			memcpy(result, path, length + 1);
		return result;
	}
	char *cwd = getcwd(NULL, 0);
	if (!cwd) {
		*native_code = errno;
		*status = errno == ENOMEM ? PG_NOMEM : PG_IO;
		return NULL;
	}
	size_t prefix = strlen(cwd);

	if (length > SIZE_MAX - prefix - 2) {
		free(cwd);
		*status = PG_LIMIT;
		return NULL;
	}
	result = (char *)malloc(prefix + length + 2);
	if (!result)
		*status = PG_NOMEM;
	if (result) {
		memcpy(result, cwd, prefix);
		result[prefix] = '/';
		memcpy(result + prefix + 1, path, length + 1);
	}
	free(cwd);
	return result;
}
