#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

static int fault_mode;
static int marker_written;
static int marker_cleared;
static off_t marker_offset;
static int unlinked;
static int replaced;
static unsigned marker_count;

extern "C" void pg_test_journal_fault(int mode)
{
	fault_mode = mode;
	marker_written = 0;
	marker_cleared = 0;
	marker_offset = 0;
	unlinked = 0;
	replaced = 0;
	marker_count = 0;
}

extern "C" ssize_t pwrite64(
	int fd, const void *bytes, size_t size, off64_t offset)
{
	const unsigned char *data =
		static_cast<const unsigned char *>(bytes);
	if (fault_mode == 12) {
		fault_mode = 0;
		errno = ENOSPC;
		return -1;
	}
	int marker = size == 4 && data[0] == 5 && data[1] == 172 &&
		data[2] == 171 && data[3] == 222;

	if ((fault_mode == 1 && marker) ||
		(fault_mode == 3 && marker_written)) {
		fault_mode = 0;
		errno = EIO;
		return -1;
	}
	ssize_t result = syscall(SYS_pwrite64, fd, bytes, size, offset);

	if (fault_mode && result == (ssize_t)size) {
		if (marker) {
			marker_written = 1;
			marker_offset = offset;
			marker_count++;
		} else if (marker_written && offset == marker_offset) {
			marker_cleared = 1;
		}
	}
	return result;
}

extern "C" ssize_t pwrite(
	int fd, const void *bytes, size_t size, off_t offset)
{
	return pwrite64(fd, bytes, size, offset);
}

extern "C" int fsync(int fd)
{
	if (fault_mode >= 13 && fault_mode <= 15 && marker_written &&
		marker_count == (unsigned)(fault_mode - 12)) {
		if (syscall(SYS_fsync, fd))
			_exit(2);
		_exit(0);
	}

	if ((fault_mode == 2 && marker_written) ||
		(fault_mode == 4 && marker_cleared) ||
		(fault_mode == 18 && replaced) ||
		(fault_mode == 6 && replaced)) {
		fault_mode = 0;
		errno = EIO;
		return -1;
	}
	return (int)syscall(SYS_fsync, fd);
}

/* Inject a deletion failure only after one irreversible unlink
 * succeeds. */
extern "C" int unlinkat(int fd, const char *path, int flags)
{
	if (fault_mode == 5 && unlinked) {
		fault_mode = 0;
		errno = EIO;
		return -1;
	}
	int result = (int)syscall(SYS_unlinkat, fd, path, flags);

	if (fault_mode == 5 && result == 0)
		unlinked = 1;
	return result;
}

extern "C" int renameat(
	int from, const char *old_path, int to, const char *new_path)
{
	if ((fault_mode == 17 || fault_mode == 18) && from != to &&
		!strcmp(new_path, "output")) {
		replaced = 1;
		errno = EXDEV;
		return -1;
	}
	int result = (int)syscall(
		SYS_renameat, from, old_path, to, new_path);

	if (fault_mode == 6 && result == 0)
		replaced = 1;
	return result;
}

extern "C" int linkat(int from, const char *old_path, int to,
	const char *new_path, int flags)
{
	if ((fault_mode == 17 || fault_mode == 18) && from != to &&
		!strcmp(new_path, "output")) {
		replaced = 1;
		errno = EXDEV;
		return -1;
	}
	return (int)syscall(SYS_linkat, from, old_path, to, new_path, flags);
}

/* Close still consumes the actual descriptor when cleanup reports
 * failure. */
extern "C" int close(int fd)
{
	int result = (int)syscall(SYS_close, fd);

	if (fault_mode == 7 && result == 0) {
		fault_mode = 0;
		errno = EIO;
		return -1;
	}
	return result;
}
