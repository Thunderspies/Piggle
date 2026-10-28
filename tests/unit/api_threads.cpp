#include "../api_test.hpp"

#ifdef _WIN32
#include <windows.h>
typedef HANDLE test_thread;
#else
#include <pthread.h>
typedef pthread_t test_thread;
#endif

namespace {

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
	int archive_header;
};

static int readable(pg_status status)
{
	return status == PG_OK || status == PG_STALE ||
		status == PG_RETRY;
}

static int archive_header(const pg_file_info *info)
{
	PG_CHECK(info->cached_header_size == 65537);
	const auto *header = static_cast<const unsigned char *>(
		info->cached_header);
	for (size_t i = 0; i < info->cached_header_size; ++i)
		PG_CHECK(header[i] == (unsigned char)(i * 17 + i / 251));
	return 0;
}

/* HANDLE-007: independent results have worker-owned cleanup. */
static int traverse(struct work *work)
{
	pg_error error;

	/* Final source release synchronizes the shared context
	 * registry. */
	PG_STATUS(pg_file_close(&work->retired, &error), PG_OK);
	if (work->late) {
		pg_file *file = NULL;
		pg_status status;

		PG_CHECK(!put_bytes("root/late", "old", 3));
		status =
			pg_tree_find(work->tree, "late", &file, &error);
		PG_CHECK(status == PG_OK || status == PG_NOT_FOUND);
		PG_STATUS(pg_file_close(&file, &error), PG_OK);
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
		pg_buffer allocated = {};
		char bytes[8];
		size_t size = 0;
		pg_status status;

		PG_STATUS(
			pg_tree_inspect(work->tree, &tree_info, &error),
			PG_OK);
		PG_CHECK(tree_info.source_count >= 1);
		PG_STATUS(
			pg_tree_source(work->tree, 0, &source, &error),
			PG_OK);
		PG_STATUS(
			pg_source_inspect(source, &source_info, &error),
			PG_OK);
		PG_CHECK(source_info.id && source_info.native_path);
		PG_STATUS(pg_source_close(&source, &error), PG_OK);
		PG_STATUS(pg_tree_find(work->tree, "A", &file, &error),
			PG_OK);
		PG_STATUS(pg_file_inspect(file, &info, &error), PG_OK);
		PG_CHECK(!strcmp(info.canonical_name, "a"));
		if (work->archive_header)
			PG_CHECK(!archive_header(&info));
		status = pg_file_verify(file, &error);
		PG_CHECK(readable(status) || status == PG_NO_CHECKSUM);
		status = pg_reader_open(file, PG_READ_LOGICAL_PAYLOAD,
			&reader, &error);
		if (status == PG_OK)
			status = pg_reader_read(reader, bytes, sizeof(bytes),
				&size, &error);
		PG_CHECK(readable(status));
		if (status == PG_OK)
			PG_CHECK(size == 3 && !memcmp(bytes, "old", 3));
		PG_STATUS(pg_reader_close(&reader, &error), PG_OK);
		PG_STATUS(pg_file_close(&file, &error), PG_OK);
		status = pg_tree_read_all(work->tree, "a", bytes,
			sizeof(bytes), &size, &error);
		PG_CHECK(readable(status));
		status = pg_tree_read_all_alloc(
			work->tree, "a", 8, &allocated, &error);
		PG_CHECK(readable(status));
		if (status == PG_OK)
			PG_CHECK(allocated.size == 3);
		pg_buffer_free(&allocated);
		status = pg_reader_open_tree(work->tree, "a",
			PG_READ_LOGICAL, &reader, &error);
		PG_CHECK(readable(status));
		PG_STATUS(pg_reader_close(&reader, &error), PG_OK);
		PG_STATUS(pg_tree_files(
					work->tree, NULL, &cursor, &error),
			PG_OK);
		PG_STATUS(pg_cursor_next(cursor, &file, &error), PG_OK);
		PG_STATUS(pg_file_inspect(file, &info, &error), PG_OK);
		PG_CHECK(!strcmp(info.canonical_name, "a"));
		PG_STATUS(pg_file_close(&file, &error), PG_OK);
		PG_STATUS(pg_cursor_close(&cursor, &error), PG_OK);
		PG_STATUS(pg_tree_entries(work->tree, NULL, 0, &entries,
					&error),
			PG_OK);
		PG_STATUS(pg_entry_cursor_next(
					entries, &entry, &file, &error),
			PG_OK);
		PG_CHECK(entry.kind == PG_ENTRY_FILE);
		PG_CHECK(!strcmp(entry.canonical_name, "a"));
		PG_STATUS(pg_file_close(&file, &error), PG_OK);
		PG_STATUS(
			pg_entry_cursor_close(&entries, &error), PG_OK);
		PG_STATUS(
			pg_tree_entries(work->tree, NULL,
				PG_ENTRIES_RECURSIVE, &entries, &error),
			PG_OK);
		PG_STATUS(
			pg_entry_cursor_close(&entries, &error), PG_OK);
		PG_STATUS(pg_source_find(
					work->source, "a", &file, &error),
			PG_OK);
		PG_STATUS(pg_file_close(&file, &error), PG_OK);
		PG_STATUS(pg_tree_find(
					work->tree, "absent", &file, &error),
			PG_NOT_FOUND);
		PG_CHECK(!file);
		PG_STATUS(pg_tree_request_subtree(
					work->tree, NULL, &error),
			PG_BUSY);
		PG_STATUS(pg_tree_rescan(work->tree, &error), PG_BUSY);
		PG_STATUS(pg_tree_detach(
					work->tree, work->source, &error),
			PG_BUSY);
		PG_STATUS(pg_tree_export(work->tree, "a",
					"worker-output", 0, &error),
			PG_BUSY);
	}
	if (work->file) {
		pg_file_info info;
		char bytes[8];
		size_t size = 0;

		PG_STATUS(pg_file_inspect(work->file, &info, &error),
			PG_OK);
		PG_CHECK(!strcmp(info.canonical_name, "a"));
		PG_CHECK(info.logical_size == 3);
		if (work->archive_header)
			PG_CHECK(!archive_header(&info));
		PG_STATUS(pg_file_read_all(work->file, bytes,
					sizeof(bytes), &size, &error),
			work->stale ? PG_STALE : PG_OK);
		PG_STATUS(pg_file_close(&work->file, &error), PG_OK);
	}
	if (work->cursor) {
		pg_file *file = NULL;
		char previous[128] = "";

		while (pg_cursor_next(work->cursor, &file, &error) ==
			PG_OK) {
			pg_file_info info;

			PG_STATUS(pg_file_inspect(file, &info, &error),
				PG_OK);
			PG_CHECK(strcmp(previous, info.canonical_name) <
				0);
			PG_CHECK(strlen(info.canonical_name) <
				sizeof(previous));
			strcpy(previous, info.canonical_name);
			PG_STATUS(pg_file_close(&file, &error), PG_OK);
		}
		PG_CHECK(error.status == PG_END && !file);
		PG_STATUS(
			pg_cursor_close(&work->cursor, &error), PG_OK);
	}
	PG_STATUS(pg_entry_cursor_close(&work->entries, &error), PG_OK);
	return 0;
}

#ifdef _WIN32
static DWORD WINAPI worker(void *argument)
#else
static void *worker(void *argument)
#endif
{
	struct work *work = static_cast<struct work *>(argument);

	work->result = traverse(work);
	return 0;
}

static int start(test_thread *thread, struct work *work)
{
#ifdef _WIN32
	*thread = CreateThread(NULL, 0, worker, work, 0, NULL);
	PG_CHECK(*thread);
#else
	PG_CHECK(!pthread_create(thread, NULL, worker, work));
#endif
	return 0;
}

static int join(test_thread thread, struct work *work)
{
#ifdef _WIN32
	PG_CHECK(WaitForSingleObject(thread, 30000) == WAIT_OBJECT_0);
	PG_CHECK(CloseHandle(thread));
#else
	PG_CHECK(!pthread_join(thread, NULL));
#endif
	PG_CHECK(!work->result);
	return 0;
}

struct callback_state {
	struct work work;
	int called;
	int failed;
	int late;
};

/* HANDLE-008: poll must release coordination before calling an
 * observer. */
static void changed(void *argument, const pg_visible_change *change)
{
	struct callback_state *state =
		static_cast<struct callback_state *>(argument);
	test_thread thread;
	pg_error error;
	pg_observer observer = {};

	if (change->canonical_name &&
		!strcmp(change->canonical_name, "late"))
		state->late++;
	state->called++;
	if (pg_tree_poll(state->work.tree, &observer, &error) !=
			PG_REENTRANT ||
		pg_tree_rescan(state->work.tree, &error) !=
			PG_REENTRANT ||
		start(&thread, &state->work) ||
		join(thread, &state->work))
		state->failed = 1;
}

static int create_archive(pg_context *context, const char *path,
	uint32_t format, const char *name)
{
	pg_archive_builder *builder = NULL;
	pg_error error;

	remove(path);
	PG_STATUS(pg_archive_builder_create(
				context, path, format, 0, &builder, &error),
		PG_OK);
	std::vector<unsigned char> header(65537);
	for (size_t i = 0; i < header.size(); ++i)
		header[i] = (unsigned char)(i * 17 + i / 251);
	pg_entry_options entry = {};
	entry.cached_header = header.data();
	entry.cached_header_size = header.size();
	PG_STATUS(pg_archive_builder_write_all(
				builder, name, "old", 3, &entry, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_finish(builder, &error), PG_OK);
	PG_STATUS(pg_archive_builder_close(&builder, &error), PG_OK);
	return 0;
}

static int threads_contract(uint32_t format, uint32_t mode)
{
	pg_context *context = NULL;
	pg_source *source = NULL, *extra = NULL;
	pg_tree *tree = NULL;
	pg_source_options options = {format, PG_WRITE, 0};
	pg_error error;
	struct work work[3] = {};
	test_thread threads[3];
	struct callback_state callback = {};
	pg_observer observer = {};
	const char *path =
		format == PG_LOOSE ? "root" : "source.archive";

	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	if (format == PG_LOOSE) {
		PG_CHECK(!directory("root"));
		PG_CHECK(!clear_file("root/late"));
		PG_CHECK(!put_bytes("root/a", "old", 3));
		PG_CHECK(!directory("root/deep"));
	} else {
		PG_CHECK(!create_archive(context, path, format, "a"));
	}
	PG_CHECK(!create_archive(context, "extra.pigg", PG_PIGG2, "b"));
	PG_STATUS(pg_source_open(
				context, path, &options, &source, &error),
		PG_OK);
	PG_STATUS(pg_source_open(
				context, "extra.pigg", NULL, &extra, &error),
		PG_OK);
	PG_STATUS(pg_tree_create(context, &tree, &error), PG_OK);
	PG_STATUS(pg_tree_attach(tree, source, &error), PG_OK);
	PG_STATUS(pg_tree_request_subtree(tree, NULL, &error), PG_OK);
	if (mode)
		PG_STATUS(pg_tree_watch(tree, mode, &error), PG_OK);
	for (size_t i = 0; i < ARRAY_SIZE(work); i++) {
		char retired_path[32];
		pg_source *retired_source = NULL;

		snprintf(retired_path, sizeof(retired_path),
			"retired-%u.pigg", (unsigned int)i);
		PG_CHECK(!create_archive(
			context, retired_path, PG_PIGG2, "a"));
		PG_STATUS(pg_source_open(context, retired_path, NULL,
					&retired_source, &error),
			PG_OK);
		PG_STATUS(pg_source_find(retired_source, "a",
					&work[i].retired, &error),
			PG_OK);
		PG_STATUS(pg_source_close(&retired_source, &error),
			PG_OK);
		work[i].tree = tree;
		work[i].source = source;
		work[i].iterations = 80;
		work[i].archive_header = format != PG_LOOSE;
		PG_CHECK(!start(&threads[i], &work[i]));
	}
	for (int i = 0; i < 40; i++) {
		PG_STATUS(pg_tree_attach(tree, extra, &error), PG_OK);
		PG_STATUS(pg_tree_detach(tree, extra, &error), PG_OK);
		PG_STATUS(pg_tree_discover(tree, NULL,
					PG_DISCOVER_CHILDREN, &error),
			PG_OK);
		PG_STATUS(pg_tree_request_subtree(tree, NULL, &error),
			PG_OK);
		PG_STATUS(pg_tree_rescan(tree, &error), PG_OK);
		pg_status status = pg_source_write_all(
			source, "c", "old", 3, NULL, &error);

		PG_CHECK(status == PG_OK || status == PG_BUSY);
		if (mode)
			PG_STATUS(pg_tree_poll(tree, &observer, &error),
				PG_OK);
	}
	for (size_t i = 0; i < ARRAY_SIZE(work); i++)
		PG_CHECK(!join(threads[i], &work[i]));
	if (mode) {
		PG_STATUS(pg_tree_poll(tree, &observer, &error), PG_OK);
		callback.work.tree = tree;
		callback.work.source = source;
		callback.work.iterations = 1;
		callback.work.archive_header = format != PG_LOOSE;
		callback.work.late = format == PG_LOOSE;
		observer.user = &callback;
		observer.visible = changed;
		PG_STATUS(pg_tree_attach(tree, extra, &error), PG_OK);
		PG_STATUS(pg_tree_poll(tree, &observer, &error), PG_OK);
		PG_CHECK(callback.called && !callback.failed);
		PG_CHECK(!callback.late);
		if (format == PG_LOOSE) {
			for (int i = 0; i < 50 && !callback.late; i++) {
				PG_STATUS(pg_tree_poll(tree, &observer,
							&error),
					PG_OK);
#ifdef _WIN32
				Sleep(1);
#endif
			}
			PG_CHECK(
				callback.late == 1 && !callback.failed);
		}
	}
	/* Captured results survive tree close and retain worker
	 * cleanup. */
	struct work retained[3] = {};
	test_thread retained_threads[3];

	PG_STATUS(pg_tree_find(tree, "a", &retained[0].file, &error),
		PG_OK);
	PG_STATUS(
		pg_tree_files(tree, NULL, &retained[0].cursor, &error),
		PG_OK);
	PG_STATUS(pg_tree_entries(
				tree, NULL, 0, &retained[0].entries, &error),
		PG_OK);
	PG_STATUS(pg_tree_files(
				tree, "missing", &retained[1].cursor, &error),
		PG_OK);
	PG_STATUS(pg_source_files(
				source, NULL, &retained[2].cursor, &error),
		PG_OK);
	pg_file *file = NULL;

	while (pg_cursor_next(retained[2].cursor, &file, &error) ==
		PG_OK)
		PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_CHECK(error.status == PG_END && !file);
	/* A metadata snapshot stays old when its physical copy changes.
	 */
	PG_STATUS(pg_source_write_all(
				source, "a", "changed", 7, NULL, &error),
		PG_OK);
	PG_STATUS(pg_tree_rescan(tree, &error), PG_OK);
	retained[0].stale = 1;
	retained[0].archive_header = format != PG_LOOSE;
	PG_STATUS(pg_tree_close(&tree, &error), PG_OK);
	PG_STATUS(pg_source_close(&source, &error), PG_OK);
	PG_STATUS(pg_source_close(&extra, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_BUSY);
	for (size_t i = 0; i < ARRAY_SIZE(retained); i++)
		PG_CHECK(!start(&retained_threads[i], &retained[i]));
	for (size_t i = 0; i < ARRAY_SIZE(retained); i++)
		PG_CHECK(!join(retained_threads[i], &retained[i]));
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

using ApiThreads = piggle_test::ContractTest;

TEST_F(ApiThreads, api_threads_loose_off)
{
	ASSERT_EQ(0, threads_contract(PG_LOOSE, PG_WATCH_OFF));
}

TEST_F(ApiThreads, api_threads_loose_scan)
{
	ASSERT_EQ(0, threads_contract(PG_LOOSE, PG_WATCH_SCAN));
}

#if defined(__linux__) || defined(_WIN32)

TEST_F(ApiThreads, api_threads_loose_native)
{
	ASSERT_EQ(0, threads_contract(PG_LOOSE, PG_WATCH_NATIVE));
}
#endif

TEST_F(ApiThreads, api_threads_pigg_off)
{
	ASSERT_EQ(0, threads_contract(PG_PIGG2, PG_WATCH_OFF));
}

TEST_F(ApiThreads, api_threads_pigg_scan)
{
	ASSERT_EQ(0, threads_contract(PG_PIGG2, PG_WATCH_SCAN));
}

#if defined(__linux__) || defined(_WIN32)

TEST_F(ApiThreads, api_threads_pigg_native)
{
	ASSERT_EQ(0, threads_contract(PG_PIGG2, PG_WATCH_NATIVE));
}
#endif

TEST_F(ApiThreads, api_threads_hogg_off)
{
	ASSERT_EQ(0, threads_contract(PG_HOGG10, PG_WATCH_OFF));
}

TEST_F(ApiThreads, api_threads_hogg_scan)
{
	ASSERT_EQ(0, threads_contract(PG_HOGG10, PG_WATCH_SCAN));
}

#if defined(__linux__) || defined(_WIN32)

TEST_F(ApiThreads, api_threads_hogg_native)
{
	ASSERT_EQ(0, threads_contract(PG_HOGG10, PG_WATCH_NATIVE));
}
#endif

} // namespace
