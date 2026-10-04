#include "api_test.h"
#include <windows.h>

static int test_case(unsigned method, unsigned mutation, int empty)
{
	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_tree *tree = NULL;
	pg_file *file = NULL;
	pg_reader *readers[2] = {NULL, NULL};
	pg_error error;
	const char *original = "original";
	size_t size = empty ? 0 : 8;
	char bytes[32];
	size_t count;
	uint64_t position;

	DeleteFileA("root/input");
	DeleteFileA("root/moved");
	CHECK(put_bytes("root/input", original, size) == 0);
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_source_open(context, "root", NULL, &source, &error), PG_OK);
	STATUS(pg_tree_create(context, &tree, &error), PG_OK);
	STATUS(pg_tree_attach(tree, source, &error), PG_OK);
	STATUS(pg_source_find(source, "input", &file, &error), PG_OK);
	for (unsigned i = 0; i < 2; i++) {
		switch (method) {
		case 0:
			STATUS(pg_reader_open_native(context, "root/input",
						     &readers[i], &error),
			       PG_OK);
			break;
		case 1:
			STATUS(pg_reader_open(file, PG_READ_LOGICAL,
					      &readers[i], &error),
			       PG_OK);
			break;
		case 2:
			STATUS(pg_reader_open_source(source, "input",
						     PG_READ_LOGICAL,
						     &readers[i], &error),
			       PG_OK);
			break;
		case 3:
			STATUS(pg_reader_open_tree(tree, "input",
						   PG_READ_LOGICAL, &readers[i],
						   &error),
			       PG_OK);
			break;
		}
	}
	if (!empty) {
		STATUS(pg_reader_read(readers[0], bytes, 2, &count, &error),
		       PG_OK);
		CHECK(count == 2 && !memcmp(bytes, original, 2));
	}
	if (mutation < 3) {
		if (mutation == 1) {
			CHECK(DeleteFileA("root/input"));
		} else {
			CHECK(MoveFileExA("root/input", "root/moved", 0));
			if (mutation == 2)
				CHECK(put_bytes("root/input", "new copy", 8) ==
				      0);
		}
		for (unsigned i = 0; i < 2; i++) {
			STATUS(pg_reader_read(readers[i], bytes, sizeof(bytes),
					      &count, &error),
			       empty ? PG_END : PG_OK);
			CHECK(count == (empty ? 0 : i ? 8 : 6));
			CHECK(!count ||
			      !memcmp(bytes, original + (i ? 0 : 2), count));
			STATUS(pg_reader_read(readers[i], bytes, sizeof(bytes),
					      &count, &error),
			       PG_END);
			CHECK(count == 0);
			STATUS(pg_reader_seek(readers[i], 0, &error), PG_OK);
			STATUS(pg_reader_tell(readers[i], &position, &error),
			       PG_OK);
			CHECK(position == 0);
			STATUS(pg_reader_read(readers[i], bytes, sizeof(bytes),
					      &count, &error),
			       empty ? PG_END : PG_OK);
			CHECK(count == size &&
			      (!size || !memcmp(bytes, original, size)));
		}
		pg_reader *fresh = NULL;
		STATUS(pg_reader_open_native(context, "root/input", &fresh,
					     &error),
		       mutation == 2 ? PG_OK : PG_NOT_FOUND);
		if (fresh) {
			STATUS(pg_reader_read(fresh, bytes, sizeof(bytes),
					      &count, &error),
			       PG_OK);
			CHECK(count == 8 && !memcmp(bytes, "new copy", 8));
			STATUS(pg_reader_close(&fresh, &error), PG_OK);
		}
		STATUS(pg_reader_open(file, PG_READ_LOGICAL, &fresh, &error),
		       PG_STALE);
		CHECK(fresh == NULL);
	} else {
		if (mutation == 3)
			CHECK(put_bytes("root/input", "modified", size) == 0);
		if (mutation == 4)
			CHECK(put_bytes("root/input", "growing input", 13) ==
			      0);
		if (mutation == 5)
			CHECK(put_bytes("root/input", "x", empty ? 0 : 1) == 0);
		HANDLE handle = CreateFileA("root/input", FILE_WRITE_ATTRIBUTES,
					    FILE_SHARE_READ | FILE_SHARE_WRITE |
						FILE_SHARE_DELETE,
					    NULL, OPEN_EXISTING, 0, NULL);
		CHECK(handle != INVALID_HANDLE_VALUE);
		FILETIME timestamp;
		GetSystemTimeAsFileTime(&timestamp);
		timestamp.dwLowDateTime ^= 0x00100001;
		CHECK(SetFileTime(handle, NULL, NULL, &timestamp));
		CHECK(CloseHandle(handle));
		STATUS(pg_reader_read(readers[0], bytes, sizeof(bytes), &count,
				      &error),
		       PG_STALE);
		STATUS(pg_reader_seek(readers[1], 0, &error), PG_STALE);
	}
	for (unsigned i = 0; i < 2; i++)
		STATUS(pg_reader_close(&readers[i], &error), PG_OK);
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_tree_close(&tree, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

int main(void)
{
	CreateDirectoryA("root", NULL);
	for (unsigned method = 0; method < 4; method++)
		for (unsigned mutation = 0; mutation < 7; mutation++)
			for (int empty = 0; empty < 2; empty++)
				if (test_case(method, mutation, empty)) {
					fprintf(
					    stderr,
					    "method=%u mutation=%u empty=%d\n",
					    method, mutation, empty);
					return 1;
				}
	puts("56 opened-object cases passed across native/file/source/tree "
	     "openers");
	return 0;
}
