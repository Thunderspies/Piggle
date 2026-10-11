#include "../api_test.hpp"
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

struct reports {
	unsigned invalidated, added, updated, removed, directories,
		losses;
};

static void PG_CALL observe(void *user, const pg_visible_change *change)
{
	struct reports *r = static_cast<struct reports *>(user);

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
			change->after_entry->kind ==
				PG_ENTRY_DIRECTORY))
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
static pg_status poll_once(
	pg_tree *tree, const pg_observer *observer, pg_error *error)
{
#ifdef _WIN32
	Sleep(30);
#endif
	return pg_tree_poll(tree, observer, error);
}

/* Windows can deliver queued native changes in successive finite cuts.
 */
static pg_status poll_changes(
	pg_tree *tree, const pg_observer *observer, pg_error *error)
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

static void PG_CALL observe_cut(
	void *user, const pg_visible_change *change)
{
	struct callback_state *state =
		static_cast<struct callback_state *>(user);
	pg_file *file = NULL;

	observe(&state->reports, change);
	if (state->triggered)
		return;
	state->triggered = 1;
	if (rename("root", "moved")) {
		state->status = PG_IO;
		return;
	}
	state->status =
		pg_tree_find(state->tree, "missing", &file, NULL);
	pg_file_close(&file, NULL);
}

static int finite_cut(void)
{
	pg_context *context = NULL;
	pg_tree *tree = NULL;
	pg_source_spec spec = {"root", {PG_LOOSE, PG_READ, 0}};
	pg_error error;
	struct callback_state state = {};
	pg_observer observer = {&state, observe_cut};

	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(
		pg_tree_open(context, &spec, 1, &tree, &error), PG_OK);
	state.tree = tree;
	PG_STATUS(pg_tree_manage(
				tree, NULL, PG_DISCOVER_RECURSIVE, &error),
		PG_OK);
	PG_STATUS(pg_tree_watch(tree, PG_WATCH_NATIVE, &error), PG_OK);
	PG_CHECK(!write_text("root/a", "first cut"));
	PG_STATUS(poll_once(tree, &observer, &error), PG_OK);
	PG_CHECK(state.triggered && state.status == PG_NOT_FOUND);
	PG_CHECK(state.reports.invalidated == 1);
	PG_CHECK(state.reports.losses == 0);
	PG_STATUS(poll_once(tree, &observer, &error), PG_OK);
	PG_CHECK(state.reports.invalidated == 2);
	PG_CHECK(state.reports.losses == 1);
	PG_STATUS(pg_tree_close(&tree, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

#ifdef __linux__
static int rename_directory(void)
{
	pg_context *context = NULL;
	pg_tree *tree = NULL;
	pg_source_spec spec = {"root", {PG_LOOSE, PG_READ, 0}};
	pg_file *file = NULL;
	pg_error error;
	struct reports r = {};
	pg_observer observer = {&r, observe};

	PG_CHECK(!write_text("root/deep/a", "before rename"));
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(
		pg_tree_open(context, &spec, 1, &tree, &error), PG_OK);
	PG_STATUS(pg_tree_manage(
				tree, NULL, PG_DISCOVER_RECURSIVE, &error),
		PG_OK);
	PG_STATUS(pg_tree_find(tree, "deep/a", &file, &error), PG_OK);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	PG_STATUS(pg_tree_find(tree, "renamed/a", &file, &error),
		PG_NOT_FOUND);
	PG_STATUS(pg_tree_watch(tree, PG_WATCH_NATIVE, &error), PG_OK);
	PG_CHECK(!rename("root/deep", "root/renamed"));
	PG_STATUS(poll_changes(tree, &observer, &error), PG_OK);
	PG_CHECK(r.removed == 1 && r.added == 1);
	PG_CHECK(!write_text(
		"root/renamed/a", "after rename, changed size"));
	PG_STATUS(poll_changes(tree, &observer, &error), PG_OK);
	PG_CHECK(r.updated == 1);
	PG_STATUS(pg_tree_close(&tree, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

#endif

static int managed_contract(bool scan, bool root)
{
	pg_context *context = NULL;
	pg_tree *tree = NULL;
	pg_source_spec spec = {"root", {PG_LOOSE, PG_READ, 0}};
	pg_file *file = NULL;
	pg_cursor *cursor = NULL;
	pg_error error;
	struct reports r = {};
	pg_observer observer = {&r, observe};
	unsigned before;
	const char *physical = root ? "root/a" : "root/deep/a";
	const char *logical = root ? "a" : "deep/a";
	const char *prefix = root ? NULL : "deep";
	const char *dir_name = root ? "empty" : "deep/empty";
	const char *dir_path = root ? "root/empty" : "root/deep/empty";

	PG_CHECK(!write_text(physical, "old"));
	PG_STATUS(pg_context_open(&context, &error), PG_OK);
	PG_STATUS(
		pg_tree_open(context, &spec, 1, &tree, &error), PG_OK);
	PG_STATUS(pg_tree_manage(
				tree, NULL, PG_DISCOVER_RECURSIVE, &error),
		PG_OK);
	PG_STATUS(
		pg_tree_manage(tree, "", PG_DISCOVER_RECURSIVE, &error),
		PG_OK);
	PG_STATUS(
		pg_tree_watch(tree,
			scan ? PG_WATCH_SCAN : PG_WATCH_NATIVE, &error),
		PG_OK);
	PG_STATUS(pg_tree_files(tree, NULL, &cursor, &error),
		scan ? PG_OK : PG_INVALID);
	PG_STATUS(pg_cursor_close(&cursor, &error), PG_OK);
	PG_CHECK(!write_text(physical, "unknown edit"));
	PG_STATUS(poll_changes(tree, &observer, &error), PG_OK);
	PG_CHECK(scan ? r.updated > 0 : r.invalidated > 0);
	PG_STATUS(pg_tree_find(tree, logical, &file, &error), PG_OK);
	PG_STATUS(pg_file_close(&file, &error), PG_OK);
	before = r.updated;
	PG_CHECK(!write_text(physical, "known edit, different size"));
	PG_STATUS(poll_changes(tree, &observer, &error), PG_OK);
	PG_CHECK(r.updated > before);
	PG_STATUS(
		pg_tree_find(tree, "new", &file, &error), PG_NOT_FOUND);
	before = r.added;
	PG_CHECK(!write_text("root/new", "created"));
	PG_STATUS(poll_changes(tree, &observer, &error), PG_OK);
	PG_CHECK(r.added > before);
#ifdef _WIN32
	PG_CHECK(!_mkdir(dir_path));
#else
	PG_CHECK(!mkdir(dir_path, 0700));
#endif
	PG_STATUS(poll_changes(tree, &observer, &error), PG_OK);
	PG_STATUS(pg_tree_discover(
				tree, prefix, PG_DISCOVER_CHILDREN, &error),
		PG_OK);
	before = r.updated;
	unsigned added = r.added;

	PG_CHECK(!write_text(physical, "overlapping scopes update"));
	PG_STATUS(poll_changes(tree, &observer, &error), PG_OK);
	PG_CHECK(r.updated == before + 1);
	PG_CHECK(r.added == added);
	/* Discovering while watched captures existing directories too.
	 */
	PG_STATUS(pg_tree_find(tree, dir_name, &file, &error),
		PG_CONFLICT);
	before = r.directories;
#ifdef _WIN32
	PG_CHECK(!_rmdir(dir_path));
#else
	PG_CHECK(!rmdir(dir_path));
#endif
	PG_STATUS(poll_changes(tree, &observer, &error), PG_OK);
	PG_CHECK(r.directories > before);
	before = r.directories;
#ifdef _WIN32
	PG_CHECK(!_mkdir(dir_path));
#else
	PG_CHECK(!mkdir(dir_path, 0700));
#endif
	PG_STATUS(poll_changes(tree, &observer, &error), PG_OK);
	PG_CHECK(r.directories > before);
	if (!scan) {
		/* A disappearing root keeps management live until
		 * recreation. */
		PG_CHECK(!rename("root", "moved"));
		PG_STATUS(poll_changes(tree, &observer, &error), PG_OK);
		PG_CHECK(r.losses > 0);
#ifdef _WIN32
		PG_CHECK(!_mkdir("root"));
#else
		PG_CHECK(!mkdir("root", 0700));
#endif
		before = r.invalidated;
		PG_STATUS(poll_changes(tree, &observer, &error), PG_OK);
		PG_CHECK(r.invalidated > before);
		before = root ? r.added : r.invalidated;
		PG_CHECK(!write_text("root/after", "new root"));
		PG_STATUS(poll_changes(tree, &observer, &error), PG_OK);
		PG_CHECK((root ? r.added : r.invalidated) > before);
		PG_STATUS(pg_tree_find(tree, "after", &file, &error),
			PG_OK);
		PG_STATUS(pg_file_close(&file, &error), PG_OK);
	}
	PG_STATUS(pg_tree_unmanage(
				tree, NULL, PG_DISCOVER_RECURSIVE, &error),
		PG_OK);
	PG_STATUS(pg_tree_unmanage(
				tree, NULL, PG_DISCOVER_RECURSIVE, &error),
		PG_NOT_FOUND);
	PG_STATUS(pg_tree_close(&tree, &error), PG_OK);
	PG_STATUS(pg_context_close(&context, &error), PG_OK);
	return 0;
}

using ApiManaged = piggle_test::ContractTest;

TEST_F(ApiManaged, api_managed_native)
{
	std::filesystem::create_directories("root/deep");
	ASSERT_EQ(0, managed_contract(false, false));
}

TEST_F(ApiManaged, api_managed_native_root)
{
	std::filesystem::create_directories("root/deep");
	ASSERT_EQ(0, managed_contract(false, true));
}

TEST_F(ApiManaged, api_managed_scan)
{
	std::filesystem::create_directories("root/deep");
	ASSERT_EQ(0, managed_contract(true, false));
}

TEST_F(ApiManaged, api_managed_finite)
{
	std::filesystem::create_directories("root/deep");
	ASSERT_EQ(0, finite_cut());
}

#if defined(__linux__)

TEST_F(ApiManaged, api_managed_rename)
{
	std::filesystem::create_directories("root/deep");
	ASSERT_EQ(0, rename_directory());
}
#endif

} // namespace
