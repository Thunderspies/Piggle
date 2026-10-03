#include "api_test.h"
#include <windows.h>
#include <wchar.h>

int main(void)
{
	pg_context *context = NULL;
	pg_reader *reader = NULL;
	pg_reader_info info;
	pg_error error;
	WCHAR cwd[1200], directory[1600], path[1800], component[221];
	char utf8[7200], bytes[4];
	size_t count;
	CHECK(GetCurrentDirectoryW(ARRAY_SIZE(cwd), cwd));
	for (size_t i = 0; i < ARRAY_SIZE(component) - 1; i++)
		component[i] = L'x';
	component[ARRAY_SIZE(component) - 1] = 0;
	swprintf(directory, ARRAY_SIZE(directory), L"\\\\?\\%ls\\%ls",
		cwd, component);
	CHECK(wcslen(directory) > MAX_PATH);
	CHECK(CreateDirectoryW(directory, NULL));
	swprintf(path, ARRAY_SIZE(path), L"%ls\\\x732b.bin", directory);
	HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
		FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD written;
	CHECK(file != INVALID_HANDLE_VALUE);
	CHECK(WriteFile(file, "abc", 3, &written, NULL) && written == 3);
	CHECK(CloseHandle(file));
	CHECK(WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8,
		sizeof(utf8), NULL, NULL));
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_reader_open_native(context, utf8, &reader, &error), PG_OK);
	STATUS(pg_reader_inspect(reader, &info, &error), PG_OK);
	CHECK(info.size == 3 && info.logical_size == 3);
	STATUS(pg_reader_read(reader, bytes, sizeof(bytes), &count, &error),
		PG_OK);
	CHECK(count == 3 && !memcmp(bytes, "abc", count));
	STATUS(pg_reader_read(reader, bytes, sizeof(bytes), &count, &error),
		PG_END);
	CHECK(count == 0);
	STATUS(pg_reader_close(&reader, &error), PG_OK);
	CHECK(DeleteFileW(path));
	STATUS(pg_reader_open_native(context, utf8, &reader, &error),
		PG_NOT_FOUND);
	CHECK(reader == NULL);
	CHECK(RemoveDirectoryW(directory));
	STATUS(pg_reader_open_native(context, "\xc3(", &reader, &error), PG_IO);
	CHECK(reader == NULL && error.native_code == EIO);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}
