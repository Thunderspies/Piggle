#include <piggle/piggle.h>
#include "internal.hpp"

#include <fcntl.h>
#ifndef _WIN32
#include <sys/file.h>
#endif

struct pg_source_sync {
#ifdef _WIN32
	CRITICAL_SECTION lock;
#else
	pthread_mutex_t lock;
#endif
};

pg_status pg_source_sync_create(pg_source *source)
{
	source->sync = (pg_source_sync *)calloc(1, sizeof(*source->sync));
	if (!source->sync)
		return PG_NOMEM;
#ifdef _WIN32
	InitializeCriticalSection(&source->sync->lock);
#else
	pthread_mutexattr_t attributes;
	int code = pthread_mutexattr_init(&attributes);

	if (!code) {
		code = pthread_mutexattr_settype(&attributes,
			PTHREAD_MUTEX_RECURSIVE);
		if (!code)
			code = pthread_mutex_init(&source->sync->lock,
				&attributes);
		pthread_mutexattr_destroy(&attributes);
	}
	if (code) {
		free(source->sync);
		source->sync = NULL;
		return PG_IO;
	}
#endif
	return PG_OK;
}

void pg_source_sync_destroy(pg_source *source)
{
	if (!source->sync)
		return;
#ifdef _WIN32
	DeleteCriticalSection(&source->sync->lock);
#else
	pthread_mutex_destroy(&source->sync->lock);
#endif
	free(source->sync);
}

void pg_source_lock(pg_source *source)
{
	if (!source || !source->sync)
		return;
#ifdef _WIN32
	EnterCriticalSection(&source->sync->lock);
#else
	pthread_mutex_lock(&source->sync->lock);
#endif
}

void pg_source_unlock(pg_source *source)
{
	if (!source || !source->sync)
		return;
#ifdef _WIN32
	LeaveCriticalSection(&source->sync->lock);
#else
	pthread_mutex_unlock(&source->sync->lock);
#endif
}

/* The descriptor owns the lease; close releases it, including process exit. */
pg_status pg_native_writer_lease(int fd, int *native_code)
{
#ifdef _WIN32
	OVERLAPPED range = {};

	/* No supported archive payload can occupy this byte. */
	range.Offset = 0xffffffffu;
	range.OffsetHigh = 0x7fffffffu;
	if (LockFileEx((HANDLE)_get_osfhandle(fd), LOCKFILE_EXCLUSIVE_LOCK |
		LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &range))
		return PG_OK;
	*native_code = (int)GetLastError();
	return *native_code == ERROR_LOCK_VIOLATION ? PG_BUSY : PG_IO;
#else
	if (!flock(fd, LOCK_EX | LOCK_NB))
		return PG_OK;
	*native_code = errno;
	return errno == EWOULDBLOCK || errno == EAGAIN ? PG_BUSY : PG_IO;
#endif
}

/* Existing native replacements participate in HOGG writer ownership. */
pg_status pg_native_target_lease(const char *path, int exists, int *out,
		int *native_code)
{
	*out = -1;
	if (!exists)
		return PG_OK;
	int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);

	if (fd < 0) {
		*native_code = errno;
		return errno == ENOENT ? PG_STALE : PG_IO;
	}
	pg_status status = pg_native_writer_lease(fd, native_code);

	if (status != PG_OK)
		close(fd);
	else
		*out = fd;
	return status;
}
