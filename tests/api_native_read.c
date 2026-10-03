#include "api_test.h"

static int changed_reader(size_t capacity, int empty)
{
	pg_context *context = NULL;
	pg_error error;
	const char original[] = "original";
	STATUS(pg_context_open(&context, &error), PG_OK);
	for (unsigned mutation = 0; mutation < 5; mutation++) {
		pg_reader *reader = NULL;
		unsigned char bytes[32];
		size_t count;
		uint64_t position;
		CHECK(clear_file("moved") == 0);
		CHECK(put_bytes("input", original, empty ? 0 : 8) == 0);
		STATUS(pg_reader_open_native(context, "input", &reader,
			&error), PG_OK);
		switch (mutation) {
		case 0:
			CHECK(put_bytes("input", "replacement grows", 17) == 0);
			break;
		case 1:
			CHECK(put_bytes("input", "x", 1) == 0);
			break;
		case 2:
			CHECK(rename("input", "moved") == 0);
			break;
		case 3:
			CHECK(rename("input", "moved") == 0);
			CHECK(put_bytes("input", original, empty ? 0 : 8) == 0);
			break;
		case 4:
			CHECK(clear_file("input") == 0);
			break;
		}
		memset(bytes, 0xa5, sizeof(bytes));
		STATUS(pg_reader_read(reader, bytes, capacity, &count, &error),
			PG_STALE);
		if (empty) {
			CHECK(count == 0 && bytes[0] == 0xa5);
		} else {
			size_t expected = mutation == 1 ? 1 :
				(capacity < 8 ? capacity : 8);
			CHECK(count == expected);
			CHECK(!memcmp(bytes, mutation == 0 ? "replacement" :
				mutation == 1 ? "x" : original, count));
			CHECK(bytes[count] == 0xa5);
		}
		STATUS(pg_reader_tell(reader, &position, &error), PG_OK);
		CHECK(position == count);
		STATUS(pg_reader_read(reader, bytes, capacity, &count, &error),
			PG_INVALID);
		CHECK(count == 0);
		STATUS(pg_reader_seek(reader, 0, &error), PG_INVALID);
		STATUS(pg_reader_close(&reader, &error), PG_OK);
	}
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	if (!strcmp(argv[1], "partial"))
		return changed_reader(2, 0);
	if (!strcmp(argv[1], "full"))
		return changed_reader(16, 0);
	if (!strcmp(argv[1], "empty"))
		return changed_reader(16, 1);
	return 2;
}
