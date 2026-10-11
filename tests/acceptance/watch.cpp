#include "world.hpp"

namespace
{

World &world()
{
	WorldScope scope;

	return *scope;
}

GIVEN("^native watching is active$")
{
	[[maybe_unused]] World &w = world();

	w.ready();
	w.watch();
}

WHEN("^I open a tree reader for \"a\"$")
{
	[[maybe_unused]] World &w = world();

	expect_status(pg_reader_open_tree(w.tree, "a", PG_READ_LOGICAL,
				&w.reader, &w.error),
		PG_OK, w.error);
}

WHEN("^an editor replaces \"a\" before the reader closes$")
{
	[[maybe_unused]] World &w = world();

	replace_native(w.paths.at("loose") + "/a", "updated");
}

WHEN("^I poll$")
{
	[[maybe_unused]] World &w = world();

	w.poll();
	expect_status(w.status, PG_OK, w.error);
}

THEN("^one visible update for \"a\" is delivered$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.reports(PG_CHANGE_UPDATE, "a"), 1u);
}

GIVEN("^no subtree has been requested$")
{
	[[maybe_unused]] World &w = world();

	w.list();
	expect_status(w.status, PG_INVALID, w.error);
	EXPECT_EQ(w.cursor, nullptr);
}

WHEN("^an editor changes an unopened file \"b\"$")
{
	[[maybe_unused]] World &w = world();

	write_native(w.paths.at("loose") + "/b", "unopened edit");
}

THEN("^no visible event for \"b\" is delivered$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(std::count_if(w.events.begin(), w.events.end(),
				[](const auto &entry) {
					return entry.name == "b";
				}),
		0);
}

THEN("^a later exact lookup probes the current loose file$")
{
	[[maybe_unused]] World &w = world();

	w.select("b");
	EXPECT_EQ(w.read(w.file), "unopened edit");
}

GIVEN("^a tree reader for \"a\" has been opened$")
{
	[[maybe_unused]] World &w = world();

	expect_status(pg_reader_open_tree(w.tree, "a", PG_READ_LOGICAL,
				&w.reader, &w.error),
		PG_OK, w.error);
}

WHEN("^I close the reader and then an editor changes \"a\"$")
{
	[[maybe_unused]] World &w = world();

	expect_status(
		pg_reader_close(&w.reader, &w.error), PG_OK, w.error);
	write_native(w.paths.at("loose") + "/a", "after close");
}

THEN("^no new event for \"a\" is delivered$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_TRUE(w.events.empty());
}

THEN("^reports queued before close remain deliverable$")
{
	[[maybe_unused]] World &w = world();

	expect_status(pg_reader_open_tree(w.tree, "a", PG_READ_LOGICAL,
				&w.reader, &w.error),
		PG_OK, w.error);
	expect_status(pg_source_write_all(w.source(), "a", "queued", 6,
				nullptr, &w.error),
		PG_OK, w.error);
	expect_status(
		pg_reader_close(&w.reader, &w.error), PG_OK, w.error);
	w.poll();
	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.reports(PG_CHANGE_UPDATE, "a"), 1u);
}

WHEN("^an editor adds \"menu/new\"$")
{
	[[maybe_unused]] World &w = world();

	write_native(w.paths.at("loose") + "/menu/new", "new");
}

THEN("^a visible addition for \"menu/new\" is delivered$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.reports(PG_CHANGE_ADD, "menu/new"), 1u);
}

GIVEN("^a watched subtree contains \"a\" and \"b\"$")
{
	[[maybe_unused]] World &w = world();

	w.ready({{"a", "old"}, {"b", "unchanged"}}, true);
	w.watch(PG_WATCH_SCAN);
}

WHEN("^I attach and then detach a source that replaces only \"a\"$")
{
	[[maybe_unused]] World &w = world();

	w.loose("patch", {{"a", "patch"}});
	w.attach("patch");
	w.poll();
	expect_status(w.status, PG_OK, w.error);
	expect_status(
		pg_tree_detach(w.tree, w.sources.at("patch"), &w.error),
		PG_OK, w.error);
	w.poll();
	expect_status(w.status, PG_OK, w.error);
}

WHEN("^I poll after each change$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.reports(PG_CHANGE_UPDATE, "a"), 2u);
}

THEN("^visible winner changes for \"a\" are delivered$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.reports(PG_CHANGE_UPDATE, "a"), 2u);
	for (const auto &entry : w.events) {
		EXPECT_TRUE(entry.before);
		EXPECT_TRUE(entry.after);
	}
}

THEN("^no event for unchanged \"b\" is delivered$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(std::count_if(w.events.begin(), w.events.end(),
				[](const auto &entry) {
					return entry.name == "b";
				}),
		0);
}

GIVEN("^a visible update for watched \"a\" is queued$")
{
	[[maybe_unused]] World &w = world();

	w.ready({{"a", "old"}}, true);
	w.watch(PG_WATCH_SCAN);
	expect_status(pg_source_write_all(w.source(), "a", "updated", 7,
				nullptr, &w.error),
		PG_OK, w.error);
	w.callback_lookup = true;
}

WHEN("^I poll on the tree control thread$")
{
	[[maybe_unused]] World &w = world();

	w.poll();
	expect_status(w.status, PG_OK, w.error);
}

THEN("^callback lookup sees the updated \"a\"$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.callback_status, PG_OK);
	EXPECT_EQ(w.callback_bytes, "updated");
}

THEN("^recursive polling reports a reentrancy error$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.recursive_status, PG_REENTRANT);
}

GIVEN("^an open reader tracks \"a\" while native watching is active"
	"$")
{
	[[maybe_unused]] World &w = world();

	w.ready({{"a", "old"}});
	w.watch();
	expect_status(pg_reader_open_tree(w.tree, "a", PG_READ_LOGICAL,
				&w.reader, &w.error),
		PG_OK, w.error);
}

WHEN("^Piggle replaces \"a\" and its native echo arrives$")
{
	[[maybe_unused]] World &w = world();

	expect_status(pg_source_write_all(w.source(), "a", "updated", 7,
				nullptr, &w.error),
		PG_OK, w.error);
}

THEN("^exactly one visible update for \"a\" is delivered$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.reports(PG_CHANGE_UPDATE, "a"), 1u);
	w.poll();
	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.reports(PG_CHANGE_UPDATE, "a"), 1u);
}

GIVEN("^\"a\" is watched$")
{
	[[maybe_unused]] World &w = world();

	w.ready({{"a", "old"}}, true);
	w.watch();
}

WHEN("^an editor completes three replacements before one poll$")
{
	[[maybe_unused]] World &w = world();

	for (const auto &value : {"first", "second", "latest"})
		replace_native(w.paths.at("loose") + "/a", value);
	w.poll();
	expect_status(w.status, PG_OK, w.error);
}

THEN("^the poll may report one transition to the latest stable s"
	"tate$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_GE(w.reports(PG_CHANGE_UPDATE, "a"), 1u);
	w.select();
	EXPECT_EQ(w.read(w.file), "latest");
}

THEN("^it does not promise revision history$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_LE(w.reports(PG_CHANGE_UPDATE, "a"), 3u);
	EXPECT_EQ(w.events.back().after_size, 6u);
}

GIVEN("^native watching is active for subtree \"menu\"$")
{
	[[maybe_unused]] World &w = world();

	w.ready({{"menu/a", "old"}, {"unknown/file", "untouched"}});
	w.request(true, "menu");
	expect_status(w.status, PG_OK, w.error);
	w.watch();
}

WHEN("^the native notification queue loses events$")
{
	[[maybe_unused]] World &w = world();

	// Overflow one finite native queue using distinct names,
	// avoiding coalescing.
	for (unsigned i = 0; i < 20000; ++i) {
		std::string path = w.paths.at("loose") +
			"/menu/overflow-" + std::to_string(i);

		write_native(path, "x");
		EXPECT_TRUE(std::filesystem::remove(path));
	}
	write_native(w.paths.at("loose") + "/menu/new", "after loss");
}

WHEN("^I poll synchronously$")
{
	[[maybe_unused]] World &w = world();

	w.poll();
	expect_status(w.status, PG_OK, w.error);
}

THEN("^a loss event identifies \"menu\" when provenance is known$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_GT(w.reports(PG_CHANGE_LOSS), 0u);
	for (const auto &event : w.events) {
		if (event.kind == PG_CHANGE_LOSS) {
			EXPECT_TRUE(event.scope.empty() ||
				event.scope == "menu");
		}
	}
}

THEN("^reconciliation refreshes only tracked scopes$")
{
	[[maybe_unused]] World &w = world();

	w.list(true, "menu");
	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.names,
		std::vector<std::string>({"menu/a", "menu/new"}));
	w.list(true, "unknown");
	expect_status(w.status, PG_INVALID, w.error);
}

GIVEN("^an external edit was missed in requested subtree \"menu\"$")
{
	[[maybe_unused]] World &w = world();

	w.ready({{"menu/a", "old"}});
	w.request(true, "menu");
	expect_status(w.status, PG_OK, w.error);
	w.watch(PG_WATCH_SCAN);
	write_native(w.paths.at("loose") + "/menu/a", "stable edit");
}

WHEN("^I synchronously rescan and then poll$")
{
	[[maybe_unused]] World &w = world();

	expect_status(pg_tree_rescan(w.tree, &w.error), PG_OK, w.error);
	w.poll();
	expect_status(w.status, PG_OK, w.error);
}

THEN("^the cached subtree reflects the stable edit$")
{
	[[maybe_unused]] World &w = world();

	w.select("menu/a");
	EXPECT_EQ(w.read(w.file), "stable edit");
}

THEN("^its visible change is delivered if watching is active$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.reports(PG_CHANGE_UPDATE, "menu/a"), 1u);
}

GIVEN("^a new tree starts with watching off$")
{
	[[maybe_unused]] World &w = world();

	w.ready({{"a", "old"}}, true);
	pg_tree_info info;

	expect_status(pg_tree_inspect(w.tree, &info, &w.error), PG_OK,
		w.error);
	EXPECT_EQ(info.watch_mode, PG_WATCH_OFF);
}

THEN("^polling reports invalid state$")
{
	[[maybe_unused]] World &w = world();

	w.poll();
	expect_status(w.status, PG_INVALID, w.error);
}

WHEN("^I enable scan watching$")
{
	[[maybe_unused]] World &w = world();

	write_native(w.paths.at("loose") + "/a", "baseline");
	w.watch(PG_WATCH_SCAN);
}

THEN("^active scopes gain baselines without initial reports$")
{
	[[maybe_unused]] World &w = world();

	w.poll();
	expect_status(w.status, PG_OK, w.error);
	EXPECT_TRUE(w.events.empty());
	w.select();
	EXPECT_EQ(w.read(w.file), "baseline");
}

THEN("^enabling the same mode preserves queued changes$")
{
	[[maybe_unused]] World &w = world();

	expect_status(pg_source_write_all(w.source(), "a", "queued", 6,
				nullptr, &w.error),
		PG_OK, w.error);
	w.watch(PG_WATCH_SCAN);
	w.poll();
	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.reports(PG_CHANGE_UPDATE, "a"), 1u);
}

WHEN("^I unwatch$")
{
	[[maybe_unused]] World &w = world();

	expect_status(pg_source_write_all(w.source(), "a", "discard", 7,
				nullptr, &w.error),
		PG_OK, w.error);
	expect_status(
		pg_tree_unwatch(w.tree, &w.error), PG_OK, w.error);
	w.events.clear();
}

THEN("^monitoring stops and queued reports are discarded before "
	"return$")
{
	[[maybe_unused]] World &w = world();

	w.poll();
	expect_status(w.status, PG_INVALID, w.error);
	w.watch(PG_WATCH_SCAN);
	w.poll();
	expect_status(w.status, PG_OK, w.error);
	EXPECT_TRUE(w.events.empty());
}

GIVEN("^watching is off and a cursor captured names \"a, c\"$")
{
	[[maybe_unused]] World &w = world();

	w.ready({{"a", "a"}, {"c", "c"}}, true);
	w.capture_files();
	expect_status(w.status, PG_OK, w.error);
	w.old_cursor = w.cursor;
	w.cursor = nullptr;
}

WHEN("^an editor removes \"a\" and adds \"b\"$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_TRUE(
		std::filesystem::remove(w.paths.at("loose") + "/a"));
	write_native(w.paths.at("loose") + "/b", "b");
}

THEN("^another cursor still yields \"a, c\"$")
{
	[[maybe_unused]] World &w = world();

	w.list();
	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.names, std::vector<std::string>({"a", "c"}));
}

WHEN("^I request the same subtree again$")
{
	[[maybe_unused]] World &w = world();

	w.request();
	expect_status(w.status, PG_OK, w.error);
}

THEN("^a new cursor yields \"b, c\"$")
{
	[[maybe_unused]] World &w = world();

	w.list();
	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.names, std::vector<std::string>({"b", "c"}));
}

THEN("^the old cursor still yields \"a, c\"$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.consume(w.old_cursor),
		std::vector<std::string>({"a", "c"}));
}

GIVEN("^native watching is active for requested subtree \"menu\"$")
{
	[[maybe_unused]] World &w = world();

	w.ready({{"menu/a", "old"}});
	w.request(true, "menu");
	expect_status(w.status, PG_OK, w.error);
	w.watch();
}

WHEN("^I list \"menu\" before polling$")
{
	[[maybe_unused]] World &w = world();

	w.list(true, "menu");
	expect_status(w.status, PG_OK, w.error);
}

THEN("^the cursor contains \"menu/new\" without a full quiet\\-stat"
	"e rescan$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.names,
		std::vector<std::string>({"menu/a", "menu/new"}));
}

THEN("^a named read of \"menu/new\" succeeds$")
{
	[[maybe_unused]] World &w = world();

	pg_buffer result = {};

	expect_status(pg_tree_read_all_alloc(w.tree, "menu/new", 32,
				&result, &w.error),
		PG_OK, w.error);
	EXPECT_EQ(std::string(static_cast<char *>(result.data),
				result.size),
		"new");
	pg_buffer_free(&result);
}

THEN("^the visible addition remains queued for a later poll$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_TRUE(w.events.empty());
	w.poll();
	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.reports(PG_CHANGE_ADD, "menu/new"), 1u);
}

GIVEN("^scan watching is active for requested subtree \"menu\"$")
{
	[[maybe_unused]] World &w = world();

	w.ready({{"menu/a", "old"}});
	w.request(true, "menu");
	expect_status(w.status, PG_OK, w.error);
	w.watch(PG_WATCH_SCAN);
}

THEN("^listing \"menu\" still uses the previous index$")
{
	[[maybe_unused]] World &w = world();

	w.list(true, "menu");
	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.names, std::vector<std::string>({"menu/a"}));
}

WHEN("^I poll and list \"menu\" again$")
{
	[[maybe_unused]] World &w = world();

	w.poll();
	expect_status(w.status, PG_OK, w.error);
	w.list(true, "menu");
	expect_status(w.status, PG_OK, w.error);
}

THEN("^the cursor contains \"menu/new\"$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.names,
		std::vector<std::string>({"menu/a", "menu/new"}));
}

GIVEN("^native watching is active on a tree with one loose source"
	"$")
{
	[[maybe_unused]] World &w = world();

	w.ready({{"menu/a", "old"}});
	w.watch();
}

WHEN("^I request \"menu\" only on the attached source$")
{
	[[maybe_unused]] World &w = world();

	w.request(false, "menu");
	expect_status(w.status, PG_OK, w.error);
}

WHEN("^an editor adds \"menu/new\" and I poll$")
{
	[[maybe_unused]] World &w = world();

	write_native(w.paths.at("loose") + "/menu/new", "new");
	w.poll();
	expect_status(w.status, PG_OK, w.error);
}

THEN("^no visible event for \"menu/new\" is delivered$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_TRUE(w.events.empty());
}

THEN("^source\\-local listing still uses its previous index$")
{
	[[maybe_unused]] World &w = world();

	w.list(false, "menu");
	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.names, std::vector<std::string>({"menu/a"}));
}

THEN("^tree listing contains \"menu/new\"$")
{
	[[maybe_unused]] World &w = world();

	w.list(true, "menu");
	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.names,
		std::vector<std::string>({"menu/a", "menu/new"}));
}

} // namespace
