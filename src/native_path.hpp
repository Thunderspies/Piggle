#ifndef PIGGLE_NATIVE_PATH_HPP
#define PIGGLE_NATIVE_PATH_HPP

#include "internal.hpp"

#include <fcntl.h>

static inline pg_status pg_native_parent_open(const char *absolute,
		int create, int *parent_fd, char **leaf, int *native_code)
{
#ifdef _WIN32
	const char *basename = pg_native_basename(absolute);
	const char *separator = basename > absolute ? basename - 1 : NULL;
	char *parent_path;
	size_t length;
	int parent;

	*parent_fd = -1;
	*leaf = NULL;
	if (!separator || !separator[1] ||
	    strcmp(separator + 1, ".") == 0 ||
	    strcmp(separator + 1, "..") == 0)
		return PG_INVALID;
	length = (size_t)(separator - absolute);
	if (length == 2 && absolute[1] == ':')
		length++;
	parent_path = (char *)malloc(length + 1);
	if (!parent_path)
		return PG_NOMEM;
	memcpy(parent_path, absolute, length);
	parent_path[length] = '\0';
	for (size_t i = 4; i <= length; i++) {
		char held;

		if (i != length && parent_path[i] != '/' &&
		    parent_path[i] != '\\')
			continue;
		held = parent_path[i];
		parent_path[i] = '\0';
		DWORD attributes = pg_win_attributes(parent_path);

		if (attributes != INVALID_FILE_ATTRIBUTES &&
		    !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
			free(parent_path);
			return PG_CONFLICT;
		}
		if (attributes != INVALID_FILE_ATTRIBUTES &&
		    (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
			char *resolved = pg_win_realpath(parent_path,
				NULL);
			int same = resolved &&
				pg_win_path_same(resolved, parent_path);

			free(resolved);
			if (!same) {
				free(parent_path);
				return PG_CONFLICT;
			}
		}
		if (create &&
		    attributes == INVALID_FILE_ATTRIBUTES &&
		    !pg_win_create_directory(parent_path, NULL) &&
		    GetLastError() != ERROR_ALREADY_EXISTS) {
			*native_code = (int)GetLastError();
			free(parent_path);
			return PG_IO;
		}
		parent_path[i] = held;
	}
	parent = open(parent_path, O_RDONLY | O_DIRECTORY |
		O_NOFOLLOW | O_CLOEXEC);
	free(parent_path);
	if (parent < 0) {
		*native_code = errno;
		return PG_IO;
	}
	*leaf = strdup(separator + 1);
	if (!*leaf) {
		close(parent);
		return PG_NOMEM;
	}
	*parent_fd = parent;
	return PG_OK;
#else
	char *path;
	char *component;
	int parent;
	pg_status status = PG_OK;

	*parent_fd = -1;
	*leaf = NULL;
	if (absolute[0] != '/' || !absolute[1])
		return PG_INVALID;
	path = (char *)malloc(strlen(absolute) + 1);
	if (!path)
		return PG_NOMEM;
	strcpy(path, absolute);
	parent = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (parent < 0) {
		*native_code = errno;
		free(path);
		return PG_IO;
	}
	component = path + 1;
	while (*component) {
		char *separator = strchr(component, '/');
		int next;

		if (!separator)
			break;
		*separator = '\0';
		if (!*component || strcmp(component, ".") == 0) {
			component = separator + 1;
			continue;
		}
		next = openat(parent, component,
			O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (next < 0 && errno == ENOENT && create) {
			if (mkdirat(parent, component, 0777) && errno != EEXIST) {
				*native_code = errno;
				status = PG_IO;
				break;
			}
			next = openat(parent, component,
				O_RDONLY | O_DIRECTORY | O_NOFOLLOW |
				O_CLOEXEC);
		}
		if (next < 0) {
			*native_code = errno;
			status = errno == ELOOP || errno == ENOTDIR ?
				PG_CONFLICT : PG_IO;
			break;
		}
		close(parent);
		parent = next;
		component = separator + 1;
	}
	if (status == PG_OK && (!*component ||
	    strcmp(component, ".") == 0 ||
	    strcmp(component, "..") == 0))
		status = PG_INVALID;
	if (status == PG_OK) {
		*leaf = (char *)malloc(strlen(component) + 1);
		if (!*leaf)
			status = PG_NOMEM;
		else
			strcpy(*leaf, component);
	}
	free(path);
	if (status != PG_OK) {
		close(parent);
		return status;
	}
	*parent_fd = parent;
	return PG_OK;
#endif
}

static inline int pg_native_stat_same(const struct stat *before,
		const struct stat *after)
{
	return before->st_dev == after->st_dev &&
		before->st_ino == after->st_ino &&
		before->st_size == after->st_size &&
		before->st_mtime == after->st_mtime
#if defined(__linux__)
		&& before->st_mtim.tv_nsec == after->st_mtim.tv_nsec
#elif defined(__APPLE__)
		&& before->st_mtimespec.tv_nsec == after->st_mtimespec.tv_nsec
#endif
		;
}

static inline pg_status pg_native_alias(pg_context *context, int parent,
		const struct stat *target, int exists,
		pg_source *allowed_source, int *native_code)
{
	pg_source *source;
	struct stat current;
	int walk = dup(parent);

	if (walk < 0) {
		*native_code = errno;
		return PG_IO;
	}
	for (source = context->sources; source;
	     source = source->next_in_context) {
		if (source == allowed_source)
			continue;
		if (source->format != PG_LOOSE && exists &&
		    source->identity.st_dev == target->st_dev &&
		    source->identity.st_ino == target->st_ino) {
			close(walk);
			return PG_CONFLICT;
		}
	}
	for (;;) {
		int next;
		struct stat ancestor;

		if (fstat(walk, &current)) {
			*native_code = errno;
			close(walk);
			return PG_IO;
		}
		for (source = context->sources; source;
		     source = source->next_in_context) {
			if (source == allowed_source)
				continue;
			if (source->format == PG_LOOSE &&
			    source->identity.st_dev == current.st_dev &&
			    source->identity.st_ino == current.st_ino) {
				close(walk);
				return PG_CONFLICT;
			}
		}
		next = openat(walk, "..", O_RDONLY | O_DIRECTORY |
			O_NOFOLLOW | O_CLOEXEC);
		if (next < 0 || fstat(next, &ancestor)) {
			*native_code = errno;
			if (next >= 0)
				close(next);
			close(walk);
			return PG_IO;
		}
		close(walk);
		if (ancestor.st_dev == current.st_dev &&
		    ancestor.st_ino == current.st_ino) {
			close(next);
			return PG_OK;
		}
		walk = next;
	}
}

#endif
