#include "api_test.h"

struct observation {
	struct fixture *fixture;
	uint32_t kind;
	uint64_t before_size;
	uint64_t after_size;
	uint64_t sequence;
	unsigned int count;
	int failed;
};

static int check_change(struct observation *state,
	const pg_visible_change *change)
{
	struct fixture *f = state->fixture;
	pg_error error;
	pg_tree_info info;
	pg_observer discard = { NULL, NULL };
	pg_tree *tree = f->tree;
	pg_source *source = f->source;
	pg_file *file = NULL;
	pg_reader *reader = NULL;

	CHECK(change->kind == state->kind);
	CHECK(change->scope == NULL);
	CHECK(change->canonical_name != NULL);
	CHECK(strcmp(change->canonical_name, "changed") == 0);
	CHECK(change->sequence > state->sequence);
	state->sequence = change->sequence;
	if (change->kind == PG_CHANGE_ADD) {
		CHECK(change->before == NULL);
	} else {
		CHECK(change->before != NULL);
		CHECK(change->before->logical_size == state->before_size);
		CHECK(strcmp(change->before->canonical_name, "changed") == 0);
	}
	if (change->kind == PG_CHANGE_REMOVE) {
		CHECK(change->after == NULL);
		STATUS(pg_tree_find(tree, "changed", &file, &error),
			PG_NOT_FOUND);
		CHECK(file == NULL);
	} else {
		CHECK(change->after != NULL);
		CHECK(change->after->logical_size == state->after_size);
		STATUS(pg_tree_find(tree, "changed", &file, &error), PG_OK);
		pg_file_info selected;

		STATUS(pg_file_inspect(file, &selected, &error), PG_OK);
		CHECK(selected.copy_id == change->after->copy_id);
		CHECK(selected.copy_generation ==
			change->after->copy_generation);
		STATUS(pg_file_close(&file, &error), PG_OK);
		STATUS(pg_reader_open_tree(tree, "changed", PG_READ_LOGICAL,
			&reader, &error), PG_OK);
		STATUS(pg_reader_close(&reader, &error), PG_OK);
	}
	STATUS(pg_tree_inspect(tree, &info, &error), PG_OK);
	CHECK(info.watch_mode == PG_WATCH_SCAN && info.source_count == 1);
	STATUS(pg_tree_poll(tree, &discard, &error), PG_REENTRANT);
	STATUS(pg_tree_watch(tree, PG_WATCH_SCAN, &error), PG_REENTRANT);
	STATUS(pg_tree_unwatch(tree, &error), PG_REENTRANT);
	STATUS(pg_tree_rescan(tree, &error), PG_REENTRANT);
	STATUS(pg_tree_request_subtree(tree, NULL, &error), PG_REENTRANT);
	STATUS(pg_tree_detach(tree, source, &error), PG_REENTRANT);
	STATUS(pg_tree_close(&tree, &error), PG_REENTRANT);
	CHECK(tree == f->tree);
	STATUS(pg_source_request_subtree(source, NULL, &error), PG_REENTRANT);
	STATUS(pg_source_rescan(source, &error), PG_REENTRANT);
	STATUS(pg_source_write_all(source, "forbidden", NULL, 0, NULL,
		&error), PG_REENTRANT);
	STATUS(pg_source_close(&source, &error), PG_REENTRANT);
	CHECK(source == f->source);
	return 0;
}

static void PG_CALL observe(void *user, const pg_visible_change *change)
{
	struct observation *state = user;

	state->count++;
	if (check_change(state, change))
		state->failed = 1;
}

int main(void)
{
	struct fixture f;
	struct observation state = { 0 };
	pg_observer observer = { &state, observe };
	pg_observer discard = { NULL, NULL };
	pg_tree_info info;
	pg_error error;

	CHECK(clear_file("root/changed") == 0);
	CHECK(fixture_open(&f) == 0);
	state.fixture = &f;
	STATUS(pg_tree_unwatch(f.tree, &error), PG_OK);
	STATUS(pg_tree_poll(f.tree, &observer, &error), PG_INVALID);
	STATUS(pg_tree_watch(f.tree, PG_WATCH_SCAN, &error), PG_OK);
	STATUS(pg_tree_watch(f.tree, PG_WATCH_SCAN, &error), PG_OK);
	STATUS(pg_tree_watch(f.tree, PG_WATCH_NATIVE, &error), PG_INVALID);
	STATUS(pg_tree_poll(f.tree, &observer, &error), PG_OK);
	CHECK(state.count == 0);
	STATUS(pg_source_write_all(f.source, "changed", "one", 3, NULL,
		&error), PG_OK);
	CHECK(state.count == 0);
	state.kind = PG_CHANGE_ADD;
	state.after_size = 3;
	STATUS(pg_tree_poll(f.tree, &observer, &error), PG_OK);
	CHECK(state.count == 1 && !state.failed);
	STATUS(pg_source_write_all(f.source, "changed", "two-two", 7, NULL,
		&error), PG_OK);
	state.kind = PG_CHANGE_UPDATE;
	state.before_size = 3;
	state.after_size = 7;
	STATUS(pg_tree_poll(f.tree, &observer, &error), PG_OK);
	CHECK(state.count == 2 && !state.failed);
	pg_file *file = NULL;

	STATUS(pg_source_find(f.source, "changed", &file, &error), PG_OK);
	STATUS(pg_file_delete(file, &error), PG_OK);
	STATUS(pg_file_close(&file, &error), PG_OK);
	state.kind = PG_CHANGE_REMOVE;
	state.before_size = 7;
	STATUS(pg_tree_poll(f.tree, &observer, &error), PG_OK);
	CHECK(state.count == 3 && !state.failed);
	STATUS(pg_tree_poll(f.tree, &observer, &error), PG_OK);
	CHECK(state.count == 3);
	/* A null callback consumes the finite cut rather than deferring it. */
	STATUS(pg_source_write_all(f.source, "changed", NULL, 0, NULL,
		&error), PG_OK);
	STATUS(pg_tree_poll(f.tree, &discard, &error), PG_OK);
	STATUS(pg_tree_poll(f.tree, &observer, &error), PG_OK);
	CHECK(state.count == 3);
	STATUS(pg_source_write_all(f.source, "changed", "queued", 6, NULL,
		&error), PG_OK);
	STATUS(pg_tree_unwatch(f.tree, &error), PG_OK);
	STATUS(pg_tree_unwatch(f.tree, &error), PG_OK);
	STATUS(pg_tree_inspect(f.tree, &info, &error), PG_OK);
	CHECK(info.watch_mode == PG_WATCH_OFF && info.source_count == 1);
	STATUS(pg_tree_watch(f.tree, PG_WATCH_SCAN, &error), PG_OK);
	STATUS(pg_tree_poll(f.tree, &observer, &error), PG_OK);
	CHECK(state.count == 3);
	STATUS(pg_tree_unwatch(f.tree, &error), PG_OK);
	CHECK(fixture_close(&f) == 0);
	return 0;
}
