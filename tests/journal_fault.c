#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

static int fault_mode;
static int marker_written;
static int marker_cleared;
static off_t marker_offset;

void pg_test_journal_fault(int mode)
{
	fault_mode = mode;
	marker_written = 0;
	marker_cleared = 0;
	marker_offset = 0;
}

ssize_t pwrite64(int fd, const void *bytes, size_t size, off64_t offset)
{
	const unsigned char *data = bytes;
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
		} else if (marker_written && offset == marker_offset) {
			marker_cleared = 1;
		}
	}
	return result;
}

ssize_t pwrite(int fd, const void *bytes, size_t size, off_t offset)
{
	return pwrite64(fd, bytes, size, offset);
}

int fsync(int fd)
{
	if ((fault_mode == 2 && marker_written) ||
	    (fault_mode == 4 && marker_cleared)) {
		fault_mode = 0;
		errno = EIO;
		return -1;
	}
	return (int)syscall(SYS_fsync, fd);
}
