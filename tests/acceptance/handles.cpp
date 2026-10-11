#include "world.hpp"

namespace
{

World &world()
{
	WorldScope scope;

	return *scope;
}

GIVEN("^I selected the base copy of \"a\"$")
{
	[[maybe_unused]] World &w = world();

	w.loose("base", {{"a", "base"}});
	w.attach("base");
	w.select();
	w.retain_selection();
}

WHEN("^a stronger source containing \"a\" is attached$")
{
	[[maybe_unused]] World &w = world();

	w.loose("patch", {{"a", "patch"}});
	w.attach("patch");
}

THEN("^the held handle still selects the base copy$")
{
	[[maybe_unused]] World &w = world();

	pg_file_info info;

	expect_status(pg_file_inspect(w.held, &info, &w.error), PG_OK,
		w.error);
	EXPECT_EQ(info.source_id, w.captured.source_id);
	EXPECT_EQ(info.copy_id, w.captured.copy_id);
	w.select();
	EXPECT_EQ(w.read(w.file), "patch");
}

THEN("^opening a read on it reads the base copy if unchanged$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.read(w.held), "base");
}

GIVEN("^I selected a copy of \"a\" with contents \"old\"$")
{
	[[maybe_unused]] World &w = world();

	w.ready({{"a", "old"}});
	w.select();
	w.retain_selection();
}

WHEN("^an external tool replaces that copy with contents \"new\"$")
{
	[[maybe_unused]] World &w = world();

	replace_native(w.paths.at("loose") + "/a", "new");
}

WHEN("^reconciliation completes$")
{
	[[maybe_unused]] World &w = world();

	expect_status(pg_tree_rescan(w.tree, &w.error), PG_OK, w.error);
}

THEN("^the old handle retains its original metadata$")
{
	[[maybe_unused]] World &w = world();

	pg_file_info info;

	expect_status(pg_file_inspect(w.held, &info, &w.error), PG_OK,
		w.error);
	EXPECT_EQ(info.copy_id, w.captured.copy_id);
	EXPECT_EQ(info.copy_generation, w.captured.copy_generation);
	EXPECT_EQ(info.logical_size, w.captured.logical_size);
}

THEN("^a new read on that handle returns a stale\\-copy error$")
{
	[[maybe_unused]] World &w = world();

	w.status = pg_reader_open(
		w.held, PG_READ_LOGICAL, &w.reader, &w.error);
	expect_status(w.status, PG_STALE, w.error);
	EXPECT_EQ(w.reader, nullptr);
}

THEN("^it does not read \"new\" through a substituted handle$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.reader, nullptr);
	w.select();
	EXPECT_EQ(w.read(w.file), "new");
}

GIVEN("^a cursor captures visible names \"a, c\"$")
{
	[[maybe_unused]] World &w = world();

	w.ready({{"a", "old"}, {"c", "c"}}, true);
	w.capture_files();
	expect_status(w.status, PG_OK, w.error);
	w.old_cursor = w.cursor;
	w.cursor = nullptr;
	w.select();
	w.retain_selection();
}

WHEN("^\"a\" is removed and \"b\" is added and reconciliation comple"
	"tes$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_TRUE(
		std::filesystem::remove(w.paths.at("loose") + "/a"));
	write_native(w.paths.at("loose") + "/b", "b");
	expect_status(pg_tree_rescan(w.tree, &w.error), PG_OK, w.error);
}

THEN("^that cursor yields \"a\" and \"c\" one entry at a time$")
{
	[[maybe_unused]] World &w = world();

	w.old_names = w.consume(w.old_cursor);
	EXPECT_EQ(w.old_names, std::vector<std::string>({"a", "c"}));
}

THEN("^a new cursor yields \"b\" and \"c\" one entry at a time$")
{
	[[maybe_unused]] World &w = world();

	w.list();
	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.names, std::vector<std::string>({"b", "c"}));
}

THEN("^reading the old \"a\" may return a stale\\-copy error$")
{
	[[maybe_unused]] World &w = world();

	w.status = pg_reader_open(
		w.held, PG_READ_LOGICAL, &w.reader, &w.error);
	expect_status(w.status, PG_STALE, w.error);
	EXPECT_EQ(w.reader, nullptr);
}

THEN("^exhaustion reports end\\-of\\-iteration with no file output"
	"$")
{
	[[maybe_unused]] World &w = world();

	w.status = pg_cursor_next(w.old_cursor, &w.file, &w.error);
	expect_status(w.status, PG_END, w.error);
	EXPECT_EQ(w.file, nullptr);
}

GIVEN("^a cursor captures visible files \"a, c\"$")
{
	[[maybe_unused]] World &w = world();

	w.ready({{"a", "old"}, {"c", "c"}}, true);
	w.capture_files();
	expect_status(w.status, PG_OK, w.error);
	w.old_cursor = w.cursor;
	w.cursor = nullptr;
	w.select();
	w.retain_selection();
}

WHEN("^\"a\" is replaced and \"b\" is added$")
{
	[[maybe_unused]] World &w = world();

	replace_native(w.paths.at("loose") + "/a", "replacement");
	write_native(w.paths.at("loose") + "/b", "b");
	expect_status(pg_tree_rescan(w.tree, &w.error), PG_OK, w.error);
}

THEN("^the cursor still returns \"a, c\" with captured metadata$")
{
	[[maybe_unused]] World &w = world();

	pg_file *first = nullptr;
	pg_file_info info;

	expect_status(pg_cursor_next(w.old_cursor, &first, &w.error),
		PG_OK, w.error);
	expect_status(pg_file_inspect(first, &info, &w.error), PG_OK,
		w.error);
	EXPECT_STREQ(info.canonical_name, "a");
	EXPECT_EQ(info.copy_generation, w.captured.copy_generation);
	EXPECT_EQ(info.logical_size, 3u);
	expect_status(pg_file_close(&first, &w.error), PG_OK, w.error);
	EXPECT_EQ(w.consume(w.old_cursor),
		std::vector<std::string>({"c"}));
}

THEN("^a new cursor returns \"a, b, c\" with current metadata$")
{
	[[maybe_unused]] World &w = world();

	w.list();
	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.names, std::vector<std::string>({"a", "b", "c"}));
	w.select();
	pg_file_info info;

	expect_status(pg_file_inspect(w.file, &info, &w.error), PG_OK,
		w.error);
	EXPECT_GT(info.copy_generation, w.captured.copy_generation);
	EXPECT_EQ(info.logical_size, 11u);
}

GIVEN("^two read transfers refer to the same selected file \"abcd\""
	"$")
{
	[[maybe_unused]] World &w = world();

	w.ready({{"a", "abcd"}});
	w.select();
	expect_status(pg_reader_open(w.file, PG_READ_LOGICAL, &w.reader,
				&w.error),
		PG_OK, w.error);
	expect_status(pg_reader_open(w.file, PG_READ_LOGICAL,
				&w.other_reader, &w.error),
		PG_OK, w.error);
}

WHEN("^the first consumes 3 bytes and the second consumes 1 byte"
	"$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.chunk(w.reader, 3), "abc");
	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.chunk(w.other_reader, 1), "a");
	expect_status(w.status, PG_OK, w.error);
}

THEN("^the first next returns \"d\"$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.chunk(w.reader, 8), "d");
	expect_status(w.status, PG_OK, w.error);
}

THEN("^the second next returns \"bcd\"$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.chunk(w.other_reader, 8), "bcd");
	expect_status(w.status, PG_OK, w.error);
}

GIVEN("^a file and cursor retain a source attached to a tree$")
{
	[[maybe_unused]] World &w = world();

	w.ready({{"a", "retained"}}, true);
	w.select();
	w.retain_selection();
	w.capture_files();
	expect_status(w.status, PG_OK, w.error);
}

WHEN("^I detach that source and close my source reference$")
{
	[[maybe_unused]] World &w = world();

	expect_status(pg_tree_detach(w.tree, w.source(), &w.error),
		PG_OK, w.error);
	expect_status(pg_source_close(&w.sources.at("loose"), &w.error),
		PG_OK, w.error);
}

THEN("^the source data still exists$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(read_native(w.paths.at("loose") + "/a"), "retained");
}

THEN("^the retained file and cursor keep their metadata$")
{
	[[maybe_unused]] World &w = world();

	pg_file_info info;

	expect_status(pg_file_inspect(w.held, &info, &w.error), PG_OK,
		w.error);
	EXPECT_EQ(info.copy_id, w.captured.copy_id);
	EXPECT_EQ(w.consume(w.cursor), std::vector<std::string>({"a"}));
}

THEN("^unchanged retained files can still be read$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.read(w.held), "retained");
}

THEN("^tree lookup no longer selects that detached source$")
{
	[[maybe_unused]] World &w = world();

	w.find("a");
	expect_status(w.status, PG_NOT_FOUND, w.error);
	EXPECT_EQ(w.file, nullptr);
}

GIVEN("^workers share a live tree with its control thread$")
{
	[[maybe_unused]] World &w = world();

	w.ready({{"a", "base"}, {"c", "c"}}, true);
	w.loose("patch", {{"a", "patch"}, {"b", "b"}});
	w.active = "loose";
}

WHEN("^workers look up files and capture independent file and en"
	"try listings$")
{
	[[maybe_unused]] World &w = world();

	w.worker_success = true;
	// Concurrent workers repeatedly capture both cursor types while
	// control changes.
	std::atomic<bool> stop(false);
	std::atomic<bool> valid(true);
	auto traverse = [&]() {
		while (!stop.load()) {
			pg_file *file = nullptr;
			pg_cursor *files = nullptr;
			pg_entry_cursor *entries = nullptr;
			pg_error error;
			bool ok = pg_tree_find(w.tree, "a", &file,
						&error) == PG_OK;
			pg_file_info info;

			ok = pg_file_inspect(file, &info, &error) ==
					PG_OK &&
				ok;
			ok = info.logical_size >= 4 && ok;
			ok = pg_file_close(&file, &error) == PG_OK &&
				ok;
			ok = pg_tree_files(w.tree, nullptr, &files,
					&error) == PG_OK &&
				ok;
			std::string previous;
			while (pg_cursor_next(files, &file, &error) ==
				PG_OK) {
				ok = pg_file_inspect(file, &info,
						&error) == PG_OK &&
					ok;
				ok = previous < info.canonical_name &&
					ok;
				previous = info.canonical_name;
				ok = pg_file_close(&file, &error) ==
						PG_OK &&
					ok;
			}
			ok = error.status == PG_END &&
				file == nullptr && ok;
			ok = pg_cursor_close(&files, &error) == PG_OK &&
				ok;
			ok = pg_tree_entries(w.tree, nullptr,
					PG_ENTRIES_RECURSIVE, &entries,
					&error) == PG_OK &&
				ok;
			pg_entry_info entry;
			previous.clear();
			while (pg_entry_cursor_next(entries, &entry,
					&file, &error) == PG_OK) {
				ok = previous < entry.canonical_name &&
					ok;
				previous = entry.canonical_name;
				ok = pg_file_close(&file, &error) ==
						PG_OK &&
					ok;
			}
			ok = error.status == PG_END && ok;
			ok = pg_entry_cursor_close(&entries, &error) ==
					PG_OK &&
				ok;
			if (!ok)
				valid = false;
		}
	};
	std::thread first(traverse), second(traverse);

	for (unsigned i = 0; i < 20; ++i) {
		// Keep assertions outside worker and callback stacks.
		pg_status attached = pg_tree_attach(
			w.tree, w.sources.at("patch"), &w.error);
		pg_status refreshed = pg_tree_rescan(w.tree, &w.error);
		pg_status detached = pg_tree_detach(
			w.tree, w.sources.at("patch"), &w.error);

		if (attached != PG_OK || refreshed != PG_OK ||
			detached != PG_OK)
			valid = false;
	}
	stop = true;
	first.join();
	second.join();
	w.worker_success = valid.load();
}

WHEN("^the control thread attaches, detaches or refreshes source"
	"s$")
{
	[[maybe_unused]] World &w = world();

	w.attach("patch");
	expect_status(pg_tree_rescan(w.tree, &w.error), PG_OK, w.error);
	expect_status(
		pg_tree_detach(w.tree, w.sources.at("patch"), &w.error),
		PG_OK, w.error);
}

THEN("^each traversal captures a complete synchronized view$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_TRUE(w.worker_success);
	w.list();
	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.names, std::vector<std::string>({"a", "c"}));
}

THEN("^captured cursors retain lexical order and original metada"
	"ta$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_TRUE(w.worker_success);
	w.capture_files();
	expect_status(w.status, PG_OK, w.error);
	w.attach("patch");
	EXPECT_EQ(w.consume(w.cursor),
		std::vector<std::string>({"a", "c"}));
}

THEN("^workers can advance and close their own cursors and selec"
	"ted files$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_TRUE(w.worker_success);
}

THEN("^named reads never switch selected copies on stale content"
	"$")
{
	[[maybe_unused]] World &w = world();

	expect_status(pg_reader_open_tree(w.tree, "a", PG_READ_LOGICAL,
				&w.reader, &w.error),
		PG_OK, w.error);
	replace_native(w.paths.at("patch") + "/a", "new patch");
	w.chunk(w.reader, 16);
	expect_status(w.status, PG_STALE, w.error);
	EXPECT_EQ(w.count, 0u);
}

GIVEN("^a watched tree has queued visible changes$")
{
	[[maybe_unused]] World &w = world();

	w.ready({{"a", "old"}}, true);
	w.watch();
	write_native(w.paths.at("loose") + "/a", "updated");
}

WHEN("^its observer starts and waits for a worker lookup and lis"
	"ting$")
{
	[[maybe_unused]] World &w = world();

	w.callback_worker = true;
	w.poll();
	expect_status(w.status, PG_OK, w.error);
}

THEN("^the worker completes without an internal lock blocking th"
	"e callback$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_TRUE(w.callback_done);
	EXPECT_TRUE(w.worker_success);
	EXPECT_EQ(w.callback_status, PG_OK);
}

THEN("^reports queued during the callback wait for the next poll"
	"$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.reports(PG_CHANGE_ADD, "late"), 0u);
	w.poll();
	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.reports(PG_CHANGE_ADD, "late"), 1u);
}

THEN("^mutation and recursive poll remain restricted$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.recursive_status, PG_REENTRANT);
	EXPECT_EQ(w.mutation_status, PG_REENTRANT);
}

} // namespace
