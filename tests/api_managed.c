#include "api_test.h"
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

struct reports {
	unsigned invalidated, added, updated, removed, directories, losses;
};

static void PG_CALL observe(void *user, const pg_visible_change *change)
{
	struct reports *r = user;

	if (change->kind == PG_CHANGE_LOSS)
		r->losses++;
	if (change->kind == PG_CHANGE_INVALIDATE)
		r->invalidated++;
	if (change->kind == PG_CHANGE_ADD)
		r->added++;
	if (change->kind == PG_CHANGE_UPDATE)
		r->updated++;
	if (change->kind == PG_CHANGE_REMOVE)
		r->removed++;
	if ((change->before_entry &&
	     change->before_entry->kind == PG_ENTRY_DIRECTORY) ||
	    (change->after_entry &&
	     change->after_entry->kind == PG_ENTRY_DIRECTORY))
		r->directories++;
}

static int write_text(const char *path, const char *text)
{
	FILE *stream = fopen(path, "wb");

	if (!stream)
		return 1;
	if (fwrite(text, 1, strlen(text), stream) != strlen(text)) {
		fclose(stream);
		return 1;
	}
	return fclose(stream);
}

/* Allow the native service to deliver its asynchronous notification. */
static pg_status poll_once(pg_tree *tree, const pg_observer *observer,
		pg_error *error)
{
#ifdef _WIN32
	Sleep(30);
#endif
	return pg_tree_poll(tree, observer, error);
}

/* Windows can deliver queued native changes in successive finite cuts. */
static pg_status poll_changes(pg_tree *tree, const pg_observer *observer,
		pg_error *error)
{
#ifdef _WIN32
	for (unsigned i = 0; i < 3; i++) {
		pg_status status = poll_once(tree, observer, error);

		if (status != PG_OK)
			return status;
	}
#endif
	return poll_once(tree, observer, error);
}

struct callback_state {
	struct reports reports;
	pg_tree *tree;
	pg_status status;
	int triggered;
};

static void PG_CALL observe_cut(void *user, const pg_visible_change *change)
{
	struct callback_state *state = user;
	pg_file *file = NULL;

	observe(&state->reports, change);
	if (state->triggered)
		return;
	state->triggered = 1;
	if (rename("root", "moved")) {
		state->status = PG_IO;
		return;
	}
	state->status = pg_tree_find(state->tree, "missing", &file, NULL);
	pg_file_close(&file, NULL);
}

static int finite_cut(void)
{
	pg_context *context = NULL;
	pg_tree *tree = NULL;
	pg_source_spec spec = { "root", { PG_LOOSE, PG_READ, 0 } };
	pg_error error;
	struct callback_state state = { 0 };
	pg_observer observer = { &state, observe_cut };

	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_tree_open(context, &spec, 1, &tree, &error), PG_OK);
	state.tree = tree;
	STATUS(pg_tree_manage(tree, NULL, PG_DISCOVER_RECURSIVE, &error),
		PG_OK);
	STATUS(pg_tree_watch(tree, PG_WATCH_NATIVE, &error), PG_OK);
	CHECK(!write_text("root/a", "first cut"));
	STATUS(poll_once(tree, &observer, &error), PG_OK);
	CHECK(state.triggered && state.status == PG_NOT_FOUND);
	CHECK(state.reports.invalidated == 1);
	CHECK(state.reports.losses == 0);
	STATUS(poll_once(tree, &observer, &error), PG_OK);
	CHECK(state.reports.invalidated == 2);
	CHECK(state.reports.losses == 1);
	STATUS(pg_tree_close(&tree, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static int rename_directory(void)
{
	pg_context *context = NULL;
	pg_tree *tree = NULL;
	pg_source_spec spec = { "root", { PG_LOOSE, PG_READ, 0 } };
	pg_file *file = NULL;
	pg_error error;
	struct reports r = { 0 };
	pg_observer observer = { &r, observe };

	CHECK(!write_text("root/deep/a", "before rename"));
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_tree_open(context, &spec, 1, &tree, &error), PG_OK);
	STATUS(pg_tree_manage(tree, NULL, PG_DISCOVER_RECURSIVE, &error),
		PG_OK);
	STATUS(pg_tree_find(tree, "deep/a", &file, &error), PG_OK);
	STATUS(pg_file_close(&file, &error), PG_OK);
	STATUS(pg_tree_find(tree, "renamed/a", &file, &error), PG_NOT_FOUND);
	STATUS(pg_tree_watch(tree, PG_WATCH_NATIVE, &error), PG_OK);
	CHECK(!rename("root/deep", "root/renamed"));
	STATUS(poll_changes(tree, &observer, &error), PG_OK);
	CHECK(r.removed == 1 && r.added == 1);
	CHECK(!write_text("root/renamed/a", "after rename, changed size"));
	STATUS(poll_changes(tree, &observer, &error), PG_OK);
	CHECK(r.updated == 1);
	STATUS(pg_tree_close(&tree, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

static int atomic_publication(void)
{
	pg_context *context = NULL;
	pg_tree *tree = NULL;
	pg_source_spec spec = {"root", {PG_LOOSE, PG_READ, 0}};
	pg_error error;

	CHECK(!write_text("root/a", "old"));
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_tree_open(context, &spec, 1, &tree, &error), PG_OK);
	STATUS(pg_tree_manage(tree, NULL, PG_DISCOVER_RECURSIVE, &error),
		PG_OK);
	STATUS(pg_tree_discover(tree, NULL, PG_DISCOVER_RECURSIVE, &error),
		PG_OK);
	STATUS(pg_tree_watch(tree, PG_WATCH_NATIVE, &error), PG_OK);
	for (unsigned i = 0; i < 32; i++) {
		pg_file *file = NULL;
		pg_file_info info;
		char text[64];

		memset(text, 'a', i + 4);
		text[i + 4] = 0;
		CHECK(!write_text("root/temporary", text));
#ifdef _WIN32
		CHECK(MoveFileExA(
			"root/temporary", "root/a", MOVEFILE_REPLACE_EXISTING));
		Sleep(30);
#else
		CHECK(!rename("root/temporary", "root/a"));
#endif
		/* First lookup must drain the temporary and publication cuts.
		 */
		STATUS(pg_tree_find(tree, "a", &file, &error), PG_OK);
		STATUS(pg_file_inspect(file, &info, &error), PG_OK);
		CHECK(info.logical_size == i + 4);
		STATUS(pg_file_close(&file, &error), PG_OK);
	}
	STATUS(pg_tree_close(&tree, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "publication"))
		return atomic_publication();
	if (argc > 1 && !strcmp(argv[1], "finite"))
		return finite_cut();
	if (argc > 1 && !strcmp(argv[1], "rename"))
		return rename_directory();
	pg_context *context = NULL;
	pg_tree *tree = NULL;
	pg_source_spec spec = { "root", { PG_LOOSE, PG_READ, 0 } };
	pg_file *file = NULL;
	pg_cursor *cursor = NULL;
	pg_error error;
	struct reports r = { 0 };
	pg_observer observer = { &r, observe };
	unsigned before;
	int scan = argc > 1 && !strcmp(argv[1], "scan");
	int root = argc > 1 && !strcmp(argv[1], "native_root");
	const char *physical = root ? "root/a" : "root/deep/a";
	const char *logical = root ? "a" : "deep/a";
	const char *prefix = root ? NULL : "deep";
	const char *dir_name = root ? "empty" : "deep/empty";
	const char *dir_path = root ? "root/empty" : "root/deep/empty";

	CHECK(!write_text(physical, "old"));
	STATUS(pg_context_open(&context, &error), PG_OK);
	STATUS(pg_tree_open(context, &spec, 1, &tree, &error), PG_OK);
	STATUS(pg_tree_manage(tree, NULL, PG_DISCOVER_RECURSIVE, &error),
		PG_OK);
	STATUS(pg_tree_manage(tree, "", PG_DISCOVER_RECURSIVE, &error), PG_OK);
	STATUS(pg_tree_watch(tree, scan ? PG_WATCH_SCAN : PG_WATCH_NATIVE,
		&error), PG_OK);
	STATUS(pg_tree_files(tree, NULL, &cursor, &error),
		scan ? PG_OK : PG_INVALID);
	STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	CHECK(!write_text(physical, "unknown edit"));
	STATUS(poll_changes(tree, &observer, &error), PG_OK);
	CHECK(scan ? r.updated > 0 : r.invalidated > 0);
	STATUS(pg_tree_find(tree, logical, &file, &error), PG_OK);
	STATUS(pg_file_close(&file, &error), PG_OK);
	before = r.updated;
	CHECK(!write_text(physical, "known edit, different size"));
	STATUS(poll_changes(tree, &observer, &error), PG_OK);
	CHECK(r.updated > before);
	STATUS(pg_tree_find(tree, "new", &file, &error), PG_NOT_FOUND);
	before = r.added;
	CHECK(!write_text("root/new", "created"));
	STATUS(poll_changes(tree, &observer, &error), PG_OK);
	CHECK(r.added > before);
#ifdef _WIN32
	CHECK(!_mkdir(dir_path));
#else
	CHECK(!mkdir(dir_path, 0700));
#endif
	STATUS(poll_changes(tree, &observer, &error), PG_OK);
	STATUS(pg_tree_discover(tree, prefix, PG_DISCOVER_CHILDREN,
		&error), PG_OK);
	before = r.updated;
	unsigned added = r.added;

	CHECK(!write_text(physical, "overlapping scopes update"));
	STATUS(poll_changes(tree, &observer, &error), PG_OK);
	CHECK(r.updated == before + 1);
	CHECK(r.added == added);
	/* Discovering while watched captures existing directories too. */
	STATUS(pg_tree_find(tree, dir_name, &file, &error), PG_CONFLICT);
	before = r.directories;
#ifdef _WIN32
	CHECK(!_rmdir(dir_path));
#else
	CHECK(!rmdir(dir_path));
#endif
	STATUS(poll_changes(tree, &observer, &error), PG_OK);
	CHECK(r.directories > before);
	before = r.directories;
#ifdef _WIN32
	CHECK(!_mkdir(dir_path));
#else
	CHECK(!mkdir(dir_path, 0700));
#endif
	STATUS(poll_changes(tree, &observer, &error), PG_OK);
	CHECK(r.directories > before);
	if (!scan) {
		/* A disappearing root keeps management live until
		 * recreation. */
		CHECK(!rename("root", "moved"));
		STATUS(poll_changes(tree, &observer, &error), PG_OK);
		CHECK(r.losses > 0);
#ifdef _WIN32
		CHECK(!_mkdir("root"));
#else
		CHECK(!mkdir("root", 0700));
#endif
		before = r.invalidated;
		STATUS(poll_changes(tree, &observer, &error), PG_OK);
		CHECK(r.invalidated > before);
		before = root ? r.added : r.invalidated;
		CHECK(!write_text("root/after", "new root"));
		STATUS(poll_changes(tree, &observer, &error), PG_OK);
		CHECK((root ? r.added : r.invalidated) > before);
		STATUS(pg_tree_find(tree, "after", &file, &error), PG_OK);
		STATUS(pg_file_close(&file, &error), PG_OK);
	}
	STATUS(pg_tree_unmanage(tree, NULL, PG_DISCOVER_RECURSIVE,
		&error), PG_OK);
	STATUS(pg_tree_unmanage(tree, NULL, PG_DISCOVER_RECURSIVE,
		&error), PG_NOT_FOUND);
	STATUS(pg_tree_close(&tree, &error), PG_OK);
	STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}
