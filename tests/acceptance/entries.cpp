#include "world.hpp"

namespace
{

World &world()
{
	WorldScope scope;

	return *scope;
}

GIVEN("^a requested loose root has files and nested empty directo"
	"ries$")
{
	[[maybe_unused]] World &w = world();

	w.ready({{"a", "a"}, {"nested/b", "b"}}, true);
	std::filesystem::create_directories(
		w.paths.at("loose") + "/empty/deep");
	w.request();
	expect_status(w.status, PG_OK, w.error);
}

WHEN("^I enumerate its immediate entries$")
{
	[[maybe_unused]] World &w = world();

	w.capture_entries();
	expect_status(w.status, PG_OK, w.error);
	w.entries = w.consume_entries(w.entry_cursor);
}

THEN("^each direct child appears once in canonical order$")
{
	[[maybe_unused]] World &w = world();

	ASSERT_EQ(w.entries.size(), 3u);
	EXPECT_EQ(w.entries[0].name, "a");
	EXPECT_EQ(w.entries[1].name, "empty");
	EXPECT_EQ(w.entries[2].name, "nested");
}

THEN("^only file entries transfer owned file selections$")
{
	[[maybe_unused]] World &w = world();

	for (const auto &entry : w.entries)
		EXPECT_EQ(entry.file, entry.kind == PG_ENTRY_FILE);
}

THEN("^recursive enumeration includes every descendant directory"
	"$")
{
	[[maybe_unused]] World &w = world();

	w.capture_entries(true, PG_ENTRIES_RECURSIVE);
	expect_status(w.status, PG_OK, w.error);
	w.entries = w.consume_entries(w.entry_cursor);
	ASSERT_EQ(w.entries.size(), 5u);
	EXPECT_EQ(w.entries[2].name, "empty/deep");
	EXPECT_EQ(w.entries[2].kind, PG_ENTRY_DIRECTORY);
}

GIVEN("^a PIGG or HOGG contains \"a/b/file\"$")
{
	[[maybe_unused]] World &w = world();

	w.archive("archive", {{"a/b/file", "payload"}});
	w.attach("archive");
}

WHEN("^I enumerate its recursive entries$")
{
	[[maybe_unused]] World &w = world();

	w.capture_entries(true, PG_ENTRIES_RECURSIVE);
	expect_status(w.status, PG_OK, w.error);
	w.entries = w.consume_entries(w.entry_cursor);
}

THEN("^directories \"a\" and \"a/b\" have implied metadata$")
{
	[[maybe_unused]] World &w = world();

	ASSERT_EQ(w.entries.size(), 3u);
	for (size_t i = 0; i < 2; ++i) {
		EXPECT_EQ(w.entries[i].kind, PG_ENTRY_DIRECTORY);
		EXPECT_TRUE(w.entries[i].attributes & PG_ENTRY_IMPLIED);
		EXPECT_FALSE(w.entries[i].file);
	}
	EXPECT_EQ(w.entries[0].name, "a");
	EXPECT_EQ(w.entries[1].name, "a/b");
}

THEN("^its file\\-only enumeration still contains only \"a/b/file\""
	"$")
{
	[[maybe_unused]] World &w = world();

	w.list();
	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.names, std::vector<std::string>({"a/b/file"}));
}

GIVEN("^a cursor captures files and directories$")
{
	[[maybe_unused]] World &w = world();

	w.ready({{"a", "a"}}, false);
	std::filesystem::create_directories(
		w.paths.at("loose") + "/empty");
	w.request();
	expect_status(w.status, PG_OK, w.error);
	w.capture_entries(true, PG_ENTRIES_RECURSIVE);
	expect_status(w.status, PG_OK, w.error);
	w.old_entries = w.entry_cursor;
	w.entry_cursor = nullptr;
}

WHEN("^the source is rescanned after a directory is removed$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_TRUE(std::filesystem::remove(
		w.paths.at("loose") + "/empty"));
	expect_status(
		pg_source_rescan(w.source(), &w.error), PG_OK, w.error);
}

THEN("^the old cursor retains its captured entries$")
{
	[[maybe_unused]] World &w = world();

	w.entries = w.consume_entries(w.old_entries);
	ASSERT_EQ(w.entries.size(), 2u);
	EXPECT_EQ(w.entries[1].name, "empty");
	EXPECT_EQ(w.entries[1].kind, PG_ENTRY_DIRECTORY);
}

THEN("^a new cursor reflects the removal$")
{
	[[maybe_unused]] World &w = world();

	w.capture_entries(true, PG_ENTRIES_RECURSIVE);
	expect_status(w.status, PG_OK, w.error);
	w.entries = w.consume_entries(w.entry_cursor);
	ASSERT_EQ(w.entries.size(), 1u);
	EXPECT_EQ(w.entries[0].name, "a");
}

GIVEN("^one source has file \"empty\" and another has directory \"em"
	"pty\"$")
{
	[[maybe_unused]] World &w = world();

	w.loose("file", {{"empty", "hidden"}});
	w.loose("directory", {});
	std::filesystem::create_directories(
		w.paths.at("directory") + "/empty");
	w.attach("file");
	w.attach("directory");
	w.request();
	expect_status(w.status, PG_OK, w.error);
}

WHEN("^I enumerate the requested tree$")
{
	[[maybe_unused]] World &w = world();

	w.capture_entries();
	expect_status(w.status, PG_OK, w.error);
	w.entries = w.consume_entries(w.entry_cursor);
}

THEN("^\"empty\" appears as a directory$")
{
	[[maybe_unused]] World &w = world();

	ASSERT_EQ(w.entries.size(), 1u);
	EXPECT_EQ(w.entries[0].name, "empty");
	EXPECT_EQ(w.entries[0].kind, PG_ENTRY_DIRECTORY);
}

THEN("^file\\-only lookup and listing do not expose the hidden fi"
	"le$")
{
	[[maybe_unused]] World &w = world();

	w.find("empty");
	expect_status(w.status, PG_CONFLICT, w.error);
	w.list();
	expect_status(w.status, PG_OK, w.error);
	EXPECT_TRUE(w.names.empty());
}

} // namespace
