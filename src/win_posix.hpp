#ifndef PIGGLE_WIN_POSIX_HPP
#define PIGGLE_WIN_POSIX_HPP

#ifdef _WIN32

#include <windows.h>
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <limits.h>
#include <process.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#ifdef _MSC_VER
typedef intptr_t ssize_t;
#define off_t int64_t
#define fseeko _fseeki64
#define ftello _ftelli64
#define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#endif

#ifndef O_DIRECTORY
#define O_DIRECTORY 0x10000000
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0x20000000
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x100
#endif
#ifndef AT_REMOVEDIR
#define AT_REMOVEDIR 0x200
#endif

struct pg_win_stat_type {
	uint64_t st_dev;
	uint64_t st_ino;
	int64_t st_size;
	int64_t st_atime;
	int64_t st_mtime;
	int64_t st_ctime;
	unsigned int st_mode;
	unsigned int st_nlink;
};

static inline void pg_win_errno(DWORD code)
{
	switch (code) {
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
		errno = ENOENT;
		break;
	case ERROR_ALREADY_EXISTS:
	case ERROR_FILE_EXISTS:
		errno = EEXIST;
		break;
	case ERROR_ACCESS_DENIED:
		errno = EACCES;
		break;
	case ERROR_DIRECTORY:
		errno = ENOTDIR;
		break;
	default:
		errno = EIO;
		break;
	}
}

/* Native paths are UTF-8 at library boundaries and UTF-16 at Win32 calls. */
static inline WCHAR *pg_win_utf16(const char *text)
{
	int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
		text, -1, NULL, 0);

	if (!count) {
		errno = EINVAL;
		return NULL;
	}
	WCHAR *wide = (WCHAR *)malloc((size_t)count * sizeof(*wide));

	if (!wide) {
		errno = ENOMEM;
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
		text, -1, wide, count)) {
		free(wide);
		errno = EINVAL;
		return NULL;
	}
	return wide;
}

static inline char *pg_win_utf8(const WCHAR *wide)
{
	int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
		wide, -1, NULL, 0, NULL, NULL);

	if (!count) {
		errno = EINVAL;
		return NULL;
	}
	char *text = (char *)malloc((size_t)count);

	if (!text) {
		errno = ENOMEM;
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
		wide, -1, text, count, NULL, NULL)) {
		free(text);
		errno = EINVAL;
		return NULL;
	}
	return text;
}

static inline HANDLE pg_win_create_file(const char *path, DWORD access,
		DWORD sharing, LPSECURITY_ATTRIBUTES security, DWORD creation,
		DWORD flags, HANDLE template_file)
{
	WCHAR *wide = pg_win_utf16(path);

	if (!wide)
		return INVALID_HANDLE_VALUE;
	HANDLE result = CreateFileW(wide, access, sharing, security, creation,
		flags, template_file);
	DWORD code = GetLastError();

	free(wide);
	SetLastError(code);
	return result;
}

static inline DWORD pg_win_attributes(const char *path)
{
	WCHAR *wide = pg_win_utf16(path);

	if (!wide)
		return INVALID_FILE_ATTRIBUTES;
	DWORD result = GetFileAttributesW(wide);
	DWORD code = GetLastError();

	free(wide);
	SetLastError(code);
	return result;
}

static inline BOOL pg_win_create_directory(const char *path,
		LPSECURITY_ATTRIBUTES security)
{
	WCHAR *wide = pg_win_utf16(path);

	if (!wide)
		return FALSE;
	BOOL result = CreateDirectoryW(wide, security);
	DWORD code = GetLastError();

	free(wide);
	SetLastError(code);
	return result;
}

static inline char *pg_win_handle_path(HANDLE handle)
{
	DWORD size = GetFinalPathNameByHandleW(handle, NULL, 0,
		FILE_NAME_NORMALIZED);

	if (!size) {
		pg_win_errno(GetLastError());
		return NULL;
	}
	WCHAR *wide = (WCHAR *)malloc(((size_t)size + 1) * sizeof(*wide));

	if (!wide) {
		errno = ENOMEM;
		return NULL;
	}
	DWORD written = GetFinalPathNameByHandleW(handle, wide, size + 1,
		FILE_NAME_NORMALIZED);

	if (!written || written > size) {
		pg_win_errno(GetLastError());
		free(wide);
		return NULL;
	}
	char *path = pg_win_utf8(wide);

	free(wide);
	if (path && strncmp(path, "\\\\?\\UNC\\", 8) == 0) {
		memmove(path + 2, path + 8, strlen(path + 8) + 1);
		path[0] = path[1] = '\\';
	} else if (path && strncmp(path, "\\\\?\\", 4) == 0) {
		memmove(path, path + 4, strlen(path + 4) + 1);
	}
	return path;
}

static inline char *pg_win_fd_path(int fd)
{
	HANDLE handle = (HANDLE)_get_osfhandle(fd);

	if (handle == INVALID_HANDLE_VALUE) {
		errno = EBADF;
		return NULL;
	}
	return pg_win_handle_path(handle);
}

static inline char *pg_win_realpath(const char *path, char *resolved)
{
	HANDLE handle = pg_win_create_file(path, FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);

	if (handle == INVALID_HANDLE_VALUE) {
		pg_win_errno(GetLastError());
		return NULL;
	}
	char *result = pg_win_handle_path(handle);

	CloseHandle(handle);
	if (result && resolved) {
		strcpy(resolved, result);
		free(result);
		return resolved;
	}
	return result;
}

static inline char *pg_win_getcwd(char *buffer, size_t size)
{
	WCHAR *wide = _wgetcwd(NULL, 0);

	if (!wide)
		return NULL;
	char *text = pg_win_utf8(wide);

	free(wide);
	if (!text || !buffer)
		return text;
	if (strlen(text) >= size) {
		free(text);
		errno = ERANGE;
		return NULL;
	}
	strcpy(buffer, text);
	free(text);
	return buffer;
}

static inline int pg_win_path_same(const char *left, const char *right)
{
	WCHAR *a = pg_win_utf16(left);
	WCHAR *b = pg_win_utf16(right);
	int same = 0;

	if (a && b) {
		for (WCHAR *at = a; *at; at++)
			if (*at == L'/')
				*at = L'\\';
		for (WCHAR *at = b; *at; at++)
			if (*at == L'/')
				*at = L'\\';
		same = CompareStringOrdinal(a, -1, b, -1, TRUE) == CSTR_EQUAL;
	}
	free(a);
	free(b);
	return same;
}

static inline char *pg_win_at_path(int parent, const char *leaf)
{
	char *root = pg_win_fd_path(parent);
	char *path;
	size_t length, suffix;

	if (!root)
		return NULL;
	length = strlen(root);
	suffix = strlen(leaf);
	path = (char *)malloc(length + suffix + 2);
	if (!path) {
		free(root);
		errno = ENOMEM;
		return NULL;
	}
	memcpy(path, root, length);
	path[length] = '\\';
	memcpy(path + length + 1, leaf, suffix + 1);
	free(root);
	return path;
}

static inline int pg_win_open(const char *path, int flags,
		int mode = 0666)
{
	DWORD access = flags & O_RDWR ? GENERIC_READ | GENERIC_WRITE :
		flags & O_WRONLY ? GENERIC_WRITE : GENERIC_READ;
	DWORD creation = flags & O_CREAT ?
		flags & O_EXCL ? CREATE_NEW :
		flags & O_TRUNC ? CREATE_ALWAYS : OPEN_ALWAYS :
		flags & O_TRUNC ? TRUNCATE_EXISTING : OPEN_EXISTING;
	DWORD attributes = FILE_FLAG_BACKUP_SEMANTICS;
	DWORD found;
	BY_HANDLE_FILE_INFORMATION info;
	HANDLE handle;
	int fd;

	(void)mode;
	if (flags & O_NOFOLLOW)
		attributes |= FILE_FLAG_OPEN_REPARSE_POINT;
	if (flags & O_DIRECTORY)
		access = FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY;
	handle = pg_win_create_file(path, access,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL, creation, attributes, NULL);
	if (handle == INVALID_HANDLE_VALUE) {
		pg_win_errno(GetLastError());
		return -1;
	}
	if (!GetFileInformationByHandle(handle, &info)) {
		pg_win_errno(GetLastError());
		CloseHandle(handle);
		return -1;
	}
	found = info.dwFileAttributes;
	if ((flags & O_NOFOLLOW) &&
	    (found & FILE_ATTRIBUTE_REPARSE_POINT)) {
		CloseHandle(handle);
		errno = ELOOP;
		return -1;
	}
	if ((flags & O_DIRECTORY) &&
	    !(found & FILE_ATTRIBUTE_DIRECTORY)) {
		CloseHandle(handle);
		errno = ENOTDIR;
		return -1;
	}
	fd = _open_osfhandle((intptr_t)handle,
		(flags & (O_RDONLY | O_WRONLY | O_RDWR | O_APPEND)) |
		_O_BINARY);
	if (fd < 0)
		CloseHandle(handle);
	return fd;
}

static inline int pg_win_openat(int parent, const char *leaf,
		int flags, int mode = 0666)
{
	char *path = pg_win_at_path(parent, leaf);
	int fd;

	if (!path)
		return -1;
	if (strcmp(leaf, ".") == 0 || strcmp(leaf, "..") == 0)
		flags &= ~O_NOFOLLOW;
	fd = pg_win_open(path, flags, mode);
	free(path);
	return fd;
}

static inline int pg_win_mkstemp(char *pattern)
{
	size_t length = strlen(pattern);
	static unsigned long counter;

	if (length < 6 || strcmp(pattern + length - 6,
		"XXXXXX") != 0) {
		errno = EINVAL;
		return -1;
	}
	for (unsigned int attempt = 0; attempt < 1000; attempt++) {
		unsigned long token = (unsigned long)GetTickCount64() ^
			GetCurrentProcessId() ^ ++counter;
		static const char hex[] = "0123456789abcdef";
		int fd;

		for (size_t i = 0; i < 6; i++) {
			pattern[length - 6 + i] = hex[token & 15];
			token >>= 4;
		}
		fd = pg_win_open(pattern, O_CREAT | O_EXCL |
			O_RDWR, 0600);
		if (fd >= 0)
			return fd;
		if (errno != EEXIST)
			return -1;
	}
	errno = EEXIST;
	return -1;
}

static inline int pg_win_fill_identity(HANDLE handle,
		struct pg_win_stat_type *state)
{
	BY_HANDLE_FILE_INFORMATION info;
	uint64_t modified;

	if (!GetFileInformationByHandle(handle, &info)) {
		pg_win_errno(GetLastError());
		return -1;
	}
	state->st_dev = info.dwVolumeSerialNumber;
	state->st_ino =
		((uint64_t)info.nFileIndexHigh << 32) |
		info.nFileIndexLow;
	state->st_size = info.dwFileAttributes &
		FILE_ATTRIBUTE_DIRECTORY ? 0 :
		((uint64_t)info.nFileSizeHigh << 32) |
		info.nFileSizeLow;
	modified = ((uint64_t)info.ftLastWriteTime.dwHighDateTime <<
		32) | info.ftLastWriteTime.dwLowDateTime;
	state->st_mtime = (int64_t)(modified / 10000000ULL) -
		11644473600LL;
	state->st_atime = state->st_mtime;
	state->st_ctime = state->st_mtime;
	state->st_mode = info.dwFileAttributes &
		FILE_ATTRIBUTE_DIRECTORY ? _S_IFDIR : _S_IFREG;
	if (!(info.dwFileAttributes & FILE_ATTRIBUTE_READONLY))
		state->st_mode |= _S_IWRITE;
	state->st_mode |= _S_IREAD;
	if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
		state->st_mode = 0;
	state->st_nlink = info.nNumberOfLinks;
	return 0;
}

static inline int pg_win_fstat(int fd,
		struct pg_win_stat_type *state)
{
	HANDLE handle = (HANDLE)_get_osfhandle(fd);

	if (handle == INVALID_HANDLE_VALUE)
		return -1;
	return pg_win_fill_identity(handle, state);
}

static inline int pg_win_lstat(const char *path,
		struct pg_win_stat_type *state)
{
	HANDLE handle = pg_win_create_file(path, FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS |
		FILE_FLAG_OPEN_REPARSE_POINT, NULL);
	int status;

	if (handle == INVALID_HANDLE_VALUE) {
		pg_win_errno(GetLastError());
		return -1;
	}
	status = pg_win_fill_identity(handle, state);
	CloseHandle(handle);
	return status;
}

static inline int pg_win_fstatat(int parent, const char *leaf,
		struct pg_win_stat_type *state, int flags)
{
	char *path = pg_win_at_path(parent, leaf);
	int status;

	(void)flags;
	if (!path)
		return -1;
	status = pg_win_lstat(path, state);
	free(path);
	return status;
}

struct pg_win_dirent {
	char d_name[4 * MAX_PATH + 1];
};

struct pg_win_dir {
	HANDLE search;
	WIN32_FIND_DATAW found;
	pg_win_dirent item;
	int first;
	int ended;
};

static inline pg_win_dir *pg_win_fdopendir(int fd)
{
	char *path = pg_win_at_path(fd, "*");

	if (!path)
		return NULL;
	WCHAR *wide = pg_win_utf16(path);

	free(path);
	if (!wide)
		return NULL;
	pg_win_dir *listing = (pg_win_dir *)calloc(1, sizeof(*listing));

	if (!listing) {
		free(wide);
		errno = ENOMEM;
		return NULL;
	}
	listing->search = FindFirstFileW(wide, &listing->found);
	DWORD code = GetLastError();

	free(wide);
	if (listing->search == INVALID_HANDLE_VALUE &&
	    code != ERROR_FILE_NOT_FOUND) {
		free(listing);
		pg_win_errno(code);
		return NULL;
	}
	listing->first = 1;
	listing->ended = listing->search == INVALID_HANDLE_VALUE;
	close(fd);
	return listing;
}

static inline pg_win_dirent *pg_win_readdir(pg_win_dir *listing)
{
	if (listing->ended) {
		errno = 0;
		return NULL;
	}
	if (!listing->first &&
	    !FindNextFileW(listing->search, &listing->found)) {
		DWORD code = GetLastError();

		listing->ended = 1;
		if (code == ERROR_NO_MORE_FILES)
			errno = 0;
		else
			pg_win_errno(code);
		return NULL;
	}
	listing->first = 0;
	if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
		listing->found.cFileName, -1, listing->item.d_name,
		sizeof(listing->item.d_name), NULL, NULL)) {
		pg_win_errno(GetLastError());
		return NULL;
	}
	return &listing->item;
}

static inline int pg_win_closedir(pg_win_dir *listing)
{
	int result = 0;

	if (listing->search != INVALID_HANDLE_VALUE &&
	    !FindClose(listing->search)) {
		pg_win_errno(GetLastError());
		result = -1;
	}
	free(listing);
	return result;
}

static inline ssize_t pg_win_read(int fd, void *bytes, size_t count)
{
	unsigned int amount = count > INT_MAX ? INT_MAX : (unsigned int)count;

	return _read(fd, bytes, amount);
}

static inline ssize_t pg_win_write(int fd, const void *bytes, size_t count)
{
	unsigned int amount = count > INT_MAX ? INT_MAX : (unsigned int)count;

	return _write(fd, bytes, amount);
}

static inline ssize_t pg_win_pread(int fd, void *bytes,
		size_t count, off_t offset)
{
	if (_lseeki64(fd, offset, SEEK_SET) < 0)
		return -1;
	return pg_win_read(fd, bytes, count);
}

static inline ssize_t pg_win_pwrite(int fd, const void *bytes,
		size_t count, off_t offset)
{
	if (_lseeki64(fd, offset, SEEK_SET) < 0)
		return -1;
	return pg_win_write(fd, bytes, count);
}

static inline int pg_win_fsync(int fd)
{
	BY_HANDLE_FILE_INFORMATION info;
	HANDLE handle = (HANDLE)_get_osfhandle(fd);

	if (GetFileInformationByHandle(handle, &info) &&
	    (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
		return 0;
	return _commit(fd);
}

static inline int pg_win_futimens(int fd,
		const struct timespec times[2])
{
	uint64_t ticks = (uint64_t)(times[1].tv_sec +
		11644473600LL) * 10000000ULL +
		(uint64_t)times[1].tv_nsec / 100;
	FILETIME modified;

	modified.dwLowDateTime = (DWORD)ticks;
	modified.dwHighDateTime = (DWORD)(ticks >> 32);
	if (!SetFileTime((HANDLE)_get_osfhandle(fd), NULL,
		NULL, &modified)) {
		pg_win_errno(GetLastError());
		return -1;
	}
	return 0;
}

static inline int pg_win_mkdirat(int parent, const char *leaf,
		int mode)
{
	char *path = pg_win_at_path(parent, leaf);
	int status;

	(void)mode;
	if (!path)
		return -1;
	status = pg_win_create_directory(path, NULL) ? 0 : -1;
	if (status)
		pg_win_errno(GetLastError());
	free(path);
	return status;
}

static inline int pg_win_remove(const char *path, int directory)
{
	WCHAR *wide = pg_win_utf16(path);

	if (!wide)
		return -1;
	BOOL removed = directory ? RemoveDirectoryW(wide) : DeleteFileW(wide);
	DWORD code = GetLastError();

	free(wide);
	if (!removed)
		pg_win_errno(code);
	return removed ? 0 : -1;
}

static inline int pg_win_unlink(const char *path)
{
	return pg_win_remove(path, 0);
}

static inline int pg_win_unlinkat(int parent, const char *leaf, int flags)
{
	char *path = pg_win_at_path(parent, leaf);

	if (!path)
		return -1;
	int status = pg_win_remove(path, flags & AT_REMOVEDIR);

	free(path);
	return status;
}

static inline int pg_win_renameat(int old_parent,
		const char *old_leaf, int new_parent,
		const char *new_leaf)
{
	char *old_path = pg_win_at_path(old_parent, old_leaf);
	char *new_path = pg_win_at_path(new_parent, new_leaf);
	int status;

	if (!old_path || !new_path) {
		free(old_path);
		free(new_path);
		return -1;
	}
	WCHAR *old_wide = pg_win_utf16(old_path);
	WCHAR *new_wide = pg_win_utf16(new_path);

	free(old_path);
	free(new_path);
	if (!old_wide || !new_wide) {
		free(old_wide);
		free(new_wide);
		return -1;
	}
	status = MoveFileExW(old_wide, new_wide,
		MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ?
		0 : -1;
	if (status && GetLastError() == ERROR_ACCESS_DENIED)
		status = ReplaceFileW(new_wide, old_wide, NULL,
			REPLACEFILE_IGNORE_MERGE_ERRORS, NULL, NULL) ?
			0 : -1;
	if (status)
		pg_win_errno(GetLastError());
	free(old_wide);
	free(new_wide);
	return status;
}

static inline int pg_win_linkat(int old_parent,
		const char *old_leaf, int new_parent,
		const char *new_leaf, int flags)
{
	char *old_path = pg_win_at_path(old_parent, old_leaf);
	char *new_path = pg_win_at_path(new_parent, new_leaf);
	int status;

	(void)flags;
	if (!old_path || !new_path) {
		free(old_path);
		free(new_path);
		return -1;
	}
	WCHAR *old_wide = pg_win_utf16(old_path);
	WCHAR *new_wide = pg_win_utf16(new_path);

	free(old_path);
	free(new_path);
	if (!old_wide || !new_wide) {
		free(old_wide);
		free(new_wide);
		return -1;
	}
	status = CreateHardLinkW(new_wide, old_wide, NULL) ? 0 : -1;
	if (status)
		pg_win_errno(GetLastError());
	free(old_wide);
	free(new_wide);
	return status;
}

#define DIR pg_win_dir
#define dirent pg_win_dirent
#define readdir pg_win_readdir
#define closedir pg_win_closedir
#define getcwd pg_win_getcwd
#define unlink pg_win_unlink
#define open pg_win_open
#define openat pg_win_openat
#define fstat pg_win_fstat
#define lstat pg_win_lstat
#define fstatat pg_win_fstatat
#define fdopendir pg_win_fdopendir
#define read pg_win_read
#define write pg_win_write
#define pread pg_win_pread
#define pwrite pg_win_pwrite
#define fsync pg_win_fsync
#define futimens pg_win_futimens
#define mkdirat pg_win_mkdirat
#define unlinkat pg_win_unlinkat
#define renameat pg_win_renameat
#define linkat pg_win_linkat
#define realpath pg_win_realpath
#define mkstemp pg_win_mkstemp
#define stat pg_win_stat_type

#endif

#endif
