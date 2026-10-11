#include "internal.hpp"

#ifdef _WIN32

static char *pg_windows_absolute(const char *path, pg_status *status,
		int *native_code)
{
	/* Device paths already specify their namespace and must bypass
	 * ordinary Win32 normalization, including trailing-dot removal.
	 */
	if (strlen(path) >= 4 && path[0] == '\\' && path[1] == '\\' &&
		(path[2] == '?' || path[2] == '.') && path[3] == '\\') {
		char *result = (char *)malloc(strlen(path) + 1);

		if (!result)
			*status = PG_NOMEM;
		else
			strcpy(result, path);
		return result;
	}
	WCHAR *wide = pg_win_utf16(path);

	if (!wide) {
		*native_code = errno;
		*status = errno == ENOMEM ? PG_NOMEM : PG_IO;
		return NULL;
	}
	DWORD capacity = GetFullPathNameW(wide, 0, NULL, NULL);
	WCHAR *absolute = NULL;
	char *result = NULL;

	while (capacity) {
		if (capacity > SIZE_MAX / sizeof(*absolute)) {
			*status = PG_LIMIT;
			free(wide);
			return NULL;
		}
		absolute = (WCHAR *)malloc((size_t)capacity *
			sizeof(*absolute));
		if (!absolute) {
			*status = PG_NOMEM;
			free(wide);
			return NULL;
		}
		DWORD length = GetFullPathNameW(wide, capacity, absolute,
			NULL);

		if (length && length < capacity) {
			result = pg_win_utf8(absolute);
			if (!result) {
				*native_code = errno;
				*status = errno == ENOMEM ? PG_NOMEM : PG_IO;
			}
			free(absolute);
			free(wide);
			return result;
		}
		DWORD code = GetLastError();

		free(absolute);
		if (!length) {
			pg_win_errno(code);
			break;
		}
		capacity = length;
	}
	if (!capacity)
		pg_win_errno(GetLastError());
	*native_code = errno;
	*status = PG_IO;
	free(wide);
	return NULL;
}

#endif

char *pg_native_absolute(const char *path, pg_status *status,
		int *native_code)
{
#ifdef _WIN32
	return pg_windows_absolute(path, status, native_code);
#else
	char *result;
	size_t length = strlen(path);

	if (path[0] == '/') {
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
#endif
}
