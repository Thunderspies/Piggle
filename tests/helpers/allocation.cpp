#include <piggle/piggle.h>
#include "../../src/internal.hpp"
#include <stdio.h>
#include <initializer_list>

static int fail_after;
static int faulted;

static int write_native(const char *path, const char *bytes)
{
	FILE *stream = fopen(path, "wb");

	if (!stream)
		return 1;
	int failed = fwrite(bytes, 1, strlen(bytes), stream) != strlen(bytes);

	return fclose(stream) || failed;
}

struct watch_observation {
	size_t count;
	uint64_t sequence;
	int failed;
};

static void PG_CALL observe_watch(void *user, const pg_visible_change *change)
{
	watch_observation *observed = (watch_observation *)user;

	observed->failed |= change->sequence <= observed->sequence ||
			    !strcmp(change->canonical_name, "hidden");
	observed->sequence = change->sequence;
	observed->count++;
}

static int watch_allocation_failures()
{
	for (int allocation = 1; allocation <= 128; allocation++) {
		pg_context *context = NULL;
		pg_tree *tree = NULL;
		pg_source *lower = NULL, *upper = NULL;
		pg_reader *reader = NULL;

		if (mkdir("fault-lower", 0700) || mkdir("fault-upper", 0700) ||
		    write_native("fault-lower/hidden", "old") ||
		    write_native("fault-lower/reveal", "base") ||
		    write_native("fault-upper/hidden", "winner") ||
		    write_native("fault-upper/reveal", "upper") ||
		    write_native("fault-upper/edit", "old") ||
		    write_native("fault-upper/remove", "gone") ||
		    write_native("fault-upper/queued", "old") ||
		    pg_context_open(&context, NULL) ||
		    pg_source_open(context, "fault-lower", NULL, &lower,
				   NULL) ||
		    pg_source_open(context, "fault-upper", NULL, &upper,
				   NULL) ||
		    pg_tree_create(context, &tree, NULL) ||
		    pg_tree_attach(tree, lower, NULL) ||
		    pg_tree_attach(tree, upper, NULL) ||
		    pg_tree_request_subtree(tree, NULL, NULL) ||
		    pg_tree_watch(tree, PG_WATCH_SCAN, NULL) ||
		    pg_reader_open_tree(tree, "edit", PG_READ_LOGICAL, &reader,
					NULL))
			return 1;
		tree->reconciling = 1;
		if (write_native("fault-upper/queued", "pending") ||
		    pg_tree_hint_add(tree, upper, "queued", 0) ||
		    pg_tree_hints_reconcile(tree, NULL))
			return 2;
		pg_tree_batch *pending = tree->pending_tail;
		pg_source_record *old = NULL;

		if (!pending || pg_source_record_find(upper, "edit", &old) ||
		    !old || write_native("fault-upper/edit", "replacement") ||
		    write_native("fault-upper/add", "new") ||
		    write_native("fault-lower/hidden", "invisible edit") ||
		    remove("fault-upper/remove") ||
		    remove("fault-upper/reveal"))
			return 3;
		for (const char *name : {"edit", "add", "remove", "reveal"})
			if (pg_tree_hint_add(tree, upper, name, 0))
				return 4;
		if (pg_tree_hint_add(tree, lower, "HIDDEN", 0) ||
		    pg_tree_hint_add(tree, upper, "EDIT", 0))
			return 4;
		uint64_t generation = old->info.copy_generation;
		pg_tree_batch *before = tree->pending_head;

		faulted = 0;
		fail_after = allocation;
		pg_status status = pg_tree_hints_reconcile(tree, NULL);

		fail_after = 0;
		int injected = faulted;

		if (injected) {
			if (status != PG_NOMEM || old->info.logical_size != 3 ||
			    old->info.copy_generation != generation ||
			    tree->pending_tail != pending || pending->next ||
			    tree->pending_head != before || !tree->hints)
				return 5;
			if (pg_tree_hints_reconcile(tree, NULL))
				return 6;
		} else if (status != PG_OK) {
			return 7;
		}
		pg_source_record *current = NULL;

		if (pg_source_record_find(upper, "edit", &current) ||
		    current != old || old->info.logical_size != 11 ||
		    old->info.copy_generation != generation + 1 || tree->hints)
			return 8;
		tree->reconciling = 0;
		watch_observation observed = {};
		pg_observer observer = {&observed, observe_watch};

		if (pg_tree_poll(tree, &observer, NULL) || observed.failed ||
		    observed.count != 5 ||
		    pg_tree_poll(tree, &observer, NULL) ||
		    observed.count != 5 || pg_reader_close(&reader, NULL) ||
		    pg_tree_close(&tree, NULL) ||
		    pg_source_close(&lower, NULL) ||
		    pg_source_close(&upper, NULL) ||
		    pg_context_close(&context, NULL))
			return 9;
		for (const char *path :
		     {"fault-lower/hidden", "fault-lower/reveal",
		      "fault-upper/hidden", "fault-upper/edit",
		      "fault-upper/add", "fault-upper/queued"})
			if (remove(path))
				return 10;
		if (rmdir("fault-lower") || rmdir("fault-upper"))
			return 10;
		if (!injected)
			return allocation > 20 ? 0 : 11;
	}
	return 12;
}

static int fail_entry;
extern "C" void *__real_calloc(size_t count, size_t size);
extern "C" void *__wrap_calloc(size_t count, size_t size)
{
	if (fail_after && !--fail_after) {
		faulted = 1;
		return NULL;
	}
	if (fail_entry && count == 1 && size == sizeof(pg_builder_entry)) {
		fail_entry = 0;
		return NULL;
	}
	return __real_calloc(count, size);
}

int main(int argc, char **)
{
	if (argc > 1)
		return watch_allocation_failures();
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_writer *writer = NULL;
	pg_error error;
	if (pg_context_open(&context, &error) ||
		pg_archive_builder_create(context, "allocation.hogg", PG_HOGG10,
			0, &builder, &error) ||
		pg_archive_builder_write_all(
			builder, "keep", "safe", 4, NULL, &error))
		return 1;
	pg_write_options options;
	pg_write_options_init(&options, 1);
	fail_entry = 1;
	pg_status status = pg_writer_open_archive_builder(
		builder, "retry", &options, &writer, &error);
	fail_entry = 0;
	if (status != PG_NOMEM || writer || builder->entry_count != 1 ||
		pg_archive_builder_write_all(
			builder, "retry", "x", 1, NULL, &error) ||
		pg_archive_builder_finish(builder, &error) ||
		pg_archive_builder_close(&builder, &error))
		return 1;
	pg_source *source = NULL;
	char bytes[4];
	size_t count;
	if (pg_source_open(context, "allocation.hogg", NULL, &source, &error) ||
		pg_source_read_all(
			source, "keep", bytes, sizeof(bytes), &count, &error) ||
		count != 4 || memcmp(bytes, "safe", 4) ||
		pg_source_close(&source, &error) ||
		pg_context_close(&context, &error))
		return 1;
	return remove("allocation.hogg") != 0;
}
