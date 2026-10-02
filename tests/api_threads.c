#include "api_test.h"

#ifdef _WIN32
#include <windows.h>
typedef HANDLE test_thread;
#else
#include <pthread.h>
typedef pthread_t test_thread;
#endif

struct work {
	pg_tree *tree;
	pg_source *source;
	pg_cursor *cursor;
	pg_entry_cursor *entries;
	pg_file *file;
	pg_file *retired;
	int iterations;
	int result;
	int stale;
	int late;
};

static int readable(pg_status status)
{
	return status == PG_OK || status == PG_STALE || status == PG_RETRY;
}

/* HANDLE-007: independent results have worker-owned cleanup. */
static int traverse(struct work *work)
{
	pg_error error;

	/* Final source release synchronizes the shared context registry. */
	STATUS(pg_file_close(&work->retired, &error), PG_OK);
	if (work->late) {
		pg_file *file = NULL;
		pg_status status;

		CHECK(!put_bytes("root/late", "old", 3));
		status = pg_tree_find(work->tree, "late", &file, &error);
		CHECK(status == PG_OK || status == PG_NOT_FOUND);
		STATUS(pg_file_close(&file, &error), PG_OK);
		work->late = 0;
	}
	for (int i = 0; i < work->iterations; i++) {
		pg_file *file = NULL;
		pg_cursor *cursor = NULL;
		pg_entry_cursor *entries = NULL;
		pg_reader *reader = NULL;
		pg_source *source = NULL;
		pg_source_info source_info;
		pg_tree_info tree_info;
		pg_entry_info entry;
		pg_file_info info;
		pg_buffer allocated = { 0 };
		char bytes[8];
		size_t size;
		pg_status status;

		STATUS(pg_tree_inspect(work->tree, &tree_info, &error), PG_OK);
		CHECK(tree_info.source_count >= 1);
		STATUS(pg_tree_source(work->tree, 0, &source, &error), PG_OK);
		STATUS(pg_source_inspect(source, &source_info, &error), PG_OK);
		CHECK(source_info.id && source_info.native_path);
		STATUS(pg_source_close(&source, &error), PG_OK);
		STATUS(pg_tree_find(work->tree, "A", &file, &error), PG_OK);
		STATUS(pg_file_inspect(file, &info, &error), PG_OK);
		CHECK(!strcmp(info.canonical_name, "a"));
		status = pg_file_verify(file, &error);
		CHECK(readable(status) || status == PG_NO_CHECKSUM);
		status = pg_file_read_all(file, bytes, sizeof(bytes), &size,
			&error);
		CHECK(readable(status));
		if (status == PG_OK)
			CHECK(size == 3 && !memcmp(bytes, "old", 3));
		STATUS(pg_file_close(&file, &error), PG_OK);
		status = pg_tree_read_all(work->tree, "a", bytes,
			sizeof(bytes), &size, &error);
		CHECK(readable(status));
		status = pg_tree_read_all_alloc(work->tree, "a", 8,
			&allocated, &error);
		CHECK(readable(status));
		if (status == PG_OK)
			CHECK(allocated.size == 3);
		pg_buffer_free(&allocated);
		status = pg_reader_open_tree(work->tree, "a", PG_READ_LOGICAL,
			&reader, &error);
		CHECK(readable(status));
		STATUS(pg_reader_close(&reader, &error), PG_OK);
		STATUS(pg_tree_files(work->tree, NULL, &cursor, &error), PG_OK);
		STATUS(pg_cursor_next(cursor, &file, &error), PG_OK);
		STATUS(pg_file_inspect(file, &info, &error), PG_OK);
		CHECK(!strcmp(info.canonical_name, "a"));
		STATUS(pg_file_close(&file, &error), PG_OK);
		STATUS(pg_cursor_close(&cursor, &error), PG_OK);
		STATUS(pg_tree_entries(work->tree, NULL, 0, &entries, &error),
			PG_OK);
		STATUS(pg_entry_cursor_next(entries, &entry, &file, &error),
			PG_OK);
		CHECK(entry.kind == PG_ENTRY_FILE);
		CHECK(!strcmp(entry.canonical_name, "a"));
		STATUS(pg_file_close(&file, &error), PG_OK);
		STATUS(pg_entry_cursor_close(&entries, &error), PG_OK);
		STATUS(pg_tree_entries(work->tree, NULL, PG_ENTRIES_RECURSIVE,
			&entries, &error), PG_OK);
		STATUS(pg_entry_cursor_close(&entries, &error), PG_OK);
		STATUS(pg_source_find(work->source, "a", &file, &error), PG_OK);
		STATUS(pg_file_close(&file, &error), PG_OK);
		STATUS(pg_tree_find(work->tree, "absent", &file, &error),
			PG_NOT_FOUND);
		CHECK(!file);
		STATUS(pg_tree_request_subtree(work->tree, NULL, &error),
			PG_BUSY);
		STATUS(pg_tree_rescan(work->tree, &error), PG_BUSY);
		STATUS(pg_tree_detach(work->tree, work->source, &error),
			PG_BUSY);
		STATUS(pg_tree_export(work->tree, "a", "worker-output", 0,
			&error), PG_BUSY);
	}
	if (work->file) {
		pg_file_info info;
		char bytes[8];
		size_t size;

		STATUS(pg_file_inspect(work->file, &info, &error), PG_OK);
		CHECK(!strcmp(info.canonical_name, "a"));
		CHECK(info.logical_size == 3);
		STATUS(pg_file_read_all(work->file, bytes, sizeof(bytes), &size,
			&error), work->stale ? PG_STALE : PG_OK);
		STATUS(pg_file_close(&work->file, &error), PG_OK);
	}
	if (work->cursor) {
		pg_file *file = NULL;
		char previous[128] = "";

		while (pg_cursor_next(work->cursor, &file, &error) == PG_OK) {
			pg_file_info info;

			STATUS(pg_file_inspect(file, &info, &error), PG_OK);
			CHECK(strcmp(previous, info.canonical_name) < 0);
			CHECK(strlen(info.canonical_name) < sizeof(previous));
			strcpy(previous, info.canonical_name);
			STATUS(pg_file_close(&file, &error), PG_OK);
		}
		CHECK(error.status == PG_END && !file);
		STATUS(pg_cursor_close(&work->cursor, &error), PG_OK);
	}
	STATUS(pg_entry_cursor_close(&work->entries, &error), PG_OK);
	return 0;
}

#ifdef _WIN32
static DWORD WINAPI worker(void *argument)
#else
static void *worker(void *argument)
#endif
{
	struct work *work = argument;

	work->result = traverse(work);
	return 0;
}

static int start(test_thread *thread, struct work *work)
{
#ifdef _WIN32
	*thread = CreateThread(NULL, 0, worker, work, 0, NULL);
	CHECK(*thread);
#else
	CHECK(!pthread_create(thread, NULL, worker, work));
#endif
	return 0;
}

static int join(test_thread thread, struct work *work)
{
#ifdef _WIN32
	CHECK(WaitForSingleObject(thread, 30000) == WAIT_OBJECT_0);
	CHECK(CloseHandle(thread));
#else
	CHECK(!pthread_join(thread, NULL));
#endif
	CHECK(!work->result);
	return 0;
}

struct callback_state {
	struct work work;
	int called;
	int failed;
	int late;
};

/* HANDLE-008: poll must release coordination before calling an observer. */
static void changed(void *argument, const pg_visible_change *change)
{
	struct callback_state *state = argument;
	test_thread thread;
	pg_error error;
	pg_observer observer = { 0 };

	if (change->canonical_name &&
	    !strcmp(change->canonical_name, "late"))
		state->late++;
	state->called++;
	if (pg_tree_poll(state->work.tree, &observer, &error) != PG_REENTRANT ||
	    pg_tree_rescan(state->work.tree, &error) != PG_REENTRANT ||
	    start(&thread, &state->work) || join(thread, &state->work))
		state->failed = 1;
}

static int create_archive(pg_context *context, const char *path,
		uint32_t format, const char *name)
{
	pg_archive_builder *builder = NULL;
	pg_error error;

	remove(path);
	STATUS(pg_archive_builder_create(context, path, format, 0,
		&builder, &error), PG_OK);
	STATUS(pg_archive_builder_write_all(builder, name, "old", 3, NULL,
		&error), PG_OK);
	STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	return 0;
}

static int exercise(uint32_t format, uint32_t mode)
{
	pg_context *context = NULL;
	pg_source *source = NULL, *extra = NULL;
	pg_tree *tree = NULL;
	pg_source_options options = { format, PG_WRITE };
	pg_error error;
	struct work work[3] = { 0 };
	test_thread threads[3];
	struct callback_state callback = { 0 };
	pg_observer observer = { 0 };
	const char *path = format == PG_LOOSE ? "root" : "source.archive";

	STATUS(pg_context_open(&context, &error), PG_OK);
	if (format == PG_LOOSE) {
		CHECK(!directory("root"));
		CHECK(!clear_file("root/late"));
		CHECK(!put_bytes("root/a", "old", 3));
		CHECK(!directory("root/deep"));
	} else {
		CHECK(!create_archive(context, path, format, "a"));
	}
	CHECK(!create_archive(context, "extra.pigg", PG_PIGG2, "b"));
	STATUS(pg_source_open(context, path, &options, &source, &error), PG_OK);
	STATUS(pg_source_open(context, "extra.pigg", NULL, &extra, &error),
		PG_OK);
	STATUS(pg_tree_create(context, &tree, &error), PG_OK);
	STATUS(pg_tree_attach(tree, source, &error), PG_OK);
	STATUS(pg_tree_request_subtree(tree, NULL, &error), PG_OK);
	if (mode)
		STATUS(pg_tree_watch(tree, mode, &error), PG_OK);
	for (size_t i = 0; i < ARRAY_SIZE(work); i++) {
		char retired_path[32];
		pg_source *retired_source = NULL;

		snprintf(retired_path, sizeof(retired_path), "retired-%u.pigg",
			(unsigned int)i);
		CHECK(!create_archive(context, retired_path, PG_PIGG2, "a"));
		STATUS(pg_source_open(context, retired_path, NULL,
			&retired_source, &error), PG_OK);
		STATUS(pg_source_find(retired_source, "a", &work[i].retired,
			&error), PG_OK);
		STATUS(pg_source_close(&retired_source, &error), PG_OK);
		work[i].tree = tree;
		work[i].source = source;
		work[i].iterations = 80;
		CHECK(!start(&threads[i], &work[i]));
	}
	for (int i = 0; i < 40; i++) {
		STATUS(pg_tree_attach(tree, extra, &error), PG_OK);
		STATUS(pg_tree_detach(tree, extra, &error), PG_OK);
		STATUS(pg_tree_discover(tree, NULL, PG_DISCOVER_CHILDREN,
			&error), PG_OK);
		STATUS(pg_tree_request_subtree(tree, NULL, &error), PG_OK);
		STATUS(pg_tree_rescan(tree, &error), PG_OK);
		pg_status status = pg_source_write_all(source, "c", "old", 3,
			NULL, &error);

		CHECK(status == PG_OK || status == PG_BUSY);
		if (mode)
			STATUS(pg_tree_poll(tree, &observer, &error), PG_OK);
	}
	for (size_t i = 0; i < ARRAY_SIZE(work); i++)
		CHECK(!join(threads[i], &work[i]));
	if (mode) {
		STATUS(pg_tree_poll(tree, &observer, &error), PG_OK);
		callback.work.tree = tree;
		callback.work.source = source;
		callback.work.iterations = 1;
		callback.work.late = format == PG_LOOSE;
		observer.user = &callback;
		observer.visible = changed;
		STATUS(pg_tree_attach(tree, extra, &error), PG_OK);
		STATUS(pg_tree_poll(tree, &observer, &error), PG_OK);
		CHECK(callback.called && !callback.failed);
		CHECK(!callback.late);
		if (format == PG_LOOSE) {
			for (int i = 0; i < 50 && !callback.late; i++) {
				STATUS(pg_tree_poll(tree, &observer, &error),
					PG_OK);
#ifdef _WIN32
				Sleep(1);
#endif
			}
			CHECK(callback.late == 1 && !callback.failed);
		}
	}
	/* Captured results survive tree close and retain worker cleanup. */
	struct work retained[3] = { 0 };
	test_thread retained_threads[3];

	STATUS(pg_tree_find(tree, "a", &retained[0].file, &error), PG_OK);
	STATUS(pg_tree_files(tree, NULL, &retained[0].cursor, &error), PG_OK);
	STATUS(pg_tree_entries(tree, NULL, 0, &retained[0].entries, &error),
		PG_OK);
	STATUS(pg_tree_files(tree, "missing", &retained[1].cursor, &error),
		PG_OK);
	STATUS(pg_source_files(source, NULL, &retained[2].cursor, &error),
		PG_OK);
	pg_file *file = NULL;

	while (pg_cursor_next(retained[2].cursor, &file, &error) == PG_OK)
		STATUS(pg_file_close(&file, &error), PG_OK);
	CHECK(error.status == PG_END && !file);
	/* A metadata snapshot stays old when its physical copy changes. */
	STATUS(pg_source_write_all(source, "a", "changed", 7, NULL, &error),
		PG_OK);
	STATUS(pg_tree_rescan(tree, &error), PG_OK);
	retained[0].stale = 1;
	STATUS(pg_tree_close(&tree, &error), PG_OK);
	STATUS(pg_source_close(&source, &error), PG_OK);
	STATUS(pg_source_close(&extra, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_BUSY);
	for (size_t i = 0; i < ARRAY_SIZE(retained); i++)
		CHECK(!start(&retained_threads[i], &retained[i]));
	for (size_t i = 0; i < ARRAY_SIZE(retained); i++)
		CHECK(!join(retained_threads[i], &retained[i]));
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

int main(int argc, char **argv)
{
	CHECK(argc == 3);
	uint32_t format = !strcmp(argv[1], "loose") ? PG_LOOSE :
		!strcmp(argv[1], "pigg") ? PG_PIGG2 : PG_HOGG10;
	uint32_t mode = !strcmp(argv[2], "off") ? PG_WATCH_OFF :
		!strcmp(argv[2], "scan") ? PG_WATCH_SCAN : PG_WATCH_NATIVE;

	return exercise(format, mode);
}
