#include "internal.hpp"
#include "native_path.hpp"

pg_status pg_staging_resolve(
	const char *directory, char **out, int *native_code)
{
	*out = NULL;
	if (!directory || !*directory)
		return PG_INVALID;
	pg_status status = PG_OK;
	char *path = pg_native_absolute(directory, &status, native_code);

	if (!path)
		return status;
	int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);

	if (fd < 0) {
		*native_code = errno;
		free(path);
		return PG_IO;
	}
	if (close(fd)) {
		*native_code = errno;
		free(path);
		return PG_IO;
	}
	*out = path;
	return PG_OK;
}

FILE *pg_staging_open(const char *directory, char **named_path)
{
	if (named_path)
		*named_path = NULL;
	if (!directory)
		return tmpfile();
	size_t length = strlen(directory);
	static const char suffix[] = "/.piggle-stage-XXXXXX";

	if (length > SIZE_MAX - sizeof(suffix)) {
		errno = EOVERFLOW;
		return NULL;
	}
	char *path = (char *)malloc(length + sizeof(suffix));

	if (!path) {
		errno = ENOMEM;
		return NULL;
	}
	memcpy(path, directory, length);
	memcpy(path + length, suffix, sizeof(suffix));
	int fd = mkstemp(path);

	if (fd < 0) {
		free(path);
		return NULL;
	}
#ifndef _WIN32
	if (fcntl(fd, F_SETFD, FD_CLOEXEC)) {
		int code = errno;

		close(fd);
		unlink(path);
		free(path);
		errno = code;
		return NULL;
	}
#endif
	FILE *stream = fdopen(fd, "w+b");

	if (!stream || (!named_path && unlink(path))) {
		int code = errno;

		if (stream)
			fclose(stream);
		else
			close(fd);
		unlink(path);
		free(path);
		errno = code;
		return NULL;
	}
	if (named_path)
		*named_path = path;
	else
		free(path);
	return stream;
}
