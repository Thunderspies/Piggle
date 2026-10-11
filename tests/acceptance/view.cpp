#include "world.hpp"

namespace
{

World &world()
{
	WorldScope scope;

	return *scope;
}

GIVEN("^an archive and a loose source both contain \"a\"$")
{
	[[maybe_unused]] World &w = world();

	w.archive("archive", {{"a", "archive"}});
	w.loose("loose", {{"a", "loose"}});
}

WHEN("^I attach the archive and then the loose source$")
{
	[[maybe_unused]] World &w = world();

	w.attach("archive");
	w.attach("loose");
}

THEN("^the selected source for \"a\" is the loose source$")
{
	[[maybe_unused]] World &w = world();

	w.select();
	EXPECT_EQ(w.read(w.file), "loose");
}

THEN("^changing timestamps does not change that priority$")
{
	[[maybe_unused]] World &w = world();

	auto path = std::filesystem::path(w.paths.at("loose")) / "a";
	std::filesystem::last_write_time(path,
		std::filesystem::file_time_type::clock::now() -
			std::chrono::hours(24));
	expect_status(pg_tree_rescan(w.tree, &w.error), PG_OK, w.error);
	w.select();
	EXPECT_EQ(w.read(w.file), "loose");
}

GIVEN("^\"base\" and \"patch\" both contain \"a\"$")
{
	[[maybe_unused]] World &w = world();

	w.loose("base", {{"a", "base"}});
	w.loose("patch", {{"a", "patch"}});
}

GIVEN("^\"patch\" was attached after \"base\"$")
{
	[[maybe_unused]] World &w = world();

	w.attach("base");
	w.attach("patch");
}

WHEN("^I detach and reattach \"base\"$")
{
	[[maybe_unused]] World &w = world();

	expect_status(
		pg_tree_detach(w.tree, w.sources.at("base"), &w.error),
		PG_OK, w.error);
	w.attach("base");
}

THEN("^a new lookup selects \"base\"$")
{
	[[maybe_unused]] World &w = world();

	w.select();
	EXPECT_EQ(w.read(w.file), "base");
}

GIVEN("^\"base\", \"patch\", and \"loose\" each contain \"a\"$")
{
	[[maybe_unused]] World &w = world();

	w.archive("base", {{"a", "base"}});
	w.archive("patch", {{"a", "patch"}});
	w.loose("loose", {{"a", "loose"}});
	for (auto &entry : w.sources)
		expect_status(pg_source_close(&entry.second, &w.error),
			PG_OK, w.error);
}

WHEN("^I open a tree from the ordered list \"base, patch, loose\"$")
{
	[[maybe_unused]] World &w = world();

	pg_source_spec inputs[] = {
		{w.paths.at("base").c_str(), {PG_PIGG2, PG_READ, 0}},
		{w.paths.at("patch").c_str(), {PG_PIGG2, PG_READ, 0}},
		{w.paths.at("loose").c_str(), {PG_LOOSE, PG_READ, 0}}};

	w.status =
		pg_tree_open(w.context, inputs, 3, &w.tree, &w.error);
	expect_status(w.status, PG_OK, w.error);
}

THEN("^the selected source for \"a\" is \"loose\"$")
{
	[[maybe_unused]] World &w = world();

	w.select();
	EXPECT_EQ(w.read(w.file), "loose");
}

GIVEN("^\"base\" loses \"a\" to \"patch\"$")
{
	[[maybe_unused]] World &w = world();

	w.loose("base", {{"a", "base"}});
	w.loose("patch", {{"a", "patch"}});
	w.attach("base");
	w.attach("patch");
}

GIVEN("^I hold the selected \"patch\" file$")
{
	[[maybe_unused]] World &w = world();

	w.select();
	w.retain_selection();
}

THEN("^the held file still identifies the \"patch\" copy$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.read(w.held), "patch");
	pg_file_info info;

	expect_status(pg_file_inspect(w.held, &info, &w.error), PG_OK,
		w.error);
	EXPECT_EQ(info.copy_id, w.captured.copy_id);
}

GIVEN("^an existing archive contains \"A\" as record 1 and \"a\" as r"
	"ecord 2$")
{
	[[maybe_unused]] World &w = world();

	w.archive("records", {{"A", "first"}, {"a", "second"}},
		PG_PIGG2, true);
}

WHEN("^I resolve \"a\" in that source$")
{
	[[maybe_unused]] World &w = world();

	w.find("a", false);
}

THEN("^the selected contents are from record 2$")
{
	[[maybe_unused]] World &w = world();

	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.read(w.file), "second");
	pg_file_info info;

	expect_status(pg_file_inspect(w.file, &info, &w.error), PG_OK,
		w.error);
	EXPECT_EQ(info.archive_record, 1u);
}

THEN("^no API cursor exposes record 1 as a losing copy$")
{
	[[maybe_unused]] World &w = world();

	w.list(false);
	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.names, std::vector<std::string>({"a"}));
}

GIVEN("^a case\\-sensitive loose source contains \"Dir/A\" and \"dir/"
	"a\"$")
{
	[[maybe_unused]] World &w = world();

	w.loose("loose", {{"Dir/A", "first"}, {"dir/a", "second"}});
	w.attach("loose");
}

WHEN("^I resolve \"dir/a\"$")
{
	[[maybe_unused]] World &w = world();

	w.find("dir/a", false);
}

THEN("^the original relative path is \"dir/a\"$")
{
	[[maybe_unused]] World &w = world();

	expect_status(w.status, PG_OK, w.error);
	pg_file_info info;

	expect_status(pg_file_inspect(w.file, &info, &w.error), PG_OK,
		w.error);
#ifndef _WIN32
	EXPECT_STREQ(info.original_name, "dir/a");
#else
	// NTFS normally folds case; the original surviving spelling is
	// retained.
	EXPECT_EQ(w.read(w.file), "second");
#endif
}

WHEN("^I request the loose root and enumerate it$")
{
	[[maybe_unused]] World &w = world();

	w.request(false);
	expect_status(w.status, PG_OK, w.error);
	w.list(false);
}

THEN("^enumeration returns one visible file for that name$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.names, std::vector<std::string>({"dir/a"}));
}

GIVEN("^two attached sources and an existing archive contain coll"
	"iding \"a\"$")
{
	[[maybe_unused]] World &w = world();

	raw_pigg("records.archive", {{"A", "bad"}, {"a", "second"}},
		true);
	w.paths["records"] = "records.archive";
	w.formats["records"] = PG_PIGG2;
	w.open("records");
	expect_status(w.status, PG_OK, w.error);
	w.loose("loose", {{"a", "winner"}});
	w.attach("records");
	w.attach("loose");
}

WHEN("^I request the root, enumerate visible files and resolve \""
	"a\"$")
{
	[[maybe_unused]] World &w = world();

	w.request();
	expect_status(w.status, PG_OK, w.error);
	w.list();
	w.select();
}

THEN("^enumeration returns one \"a\" and lookup selects its winner"
	"$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.names, std::vector<std::string>({"a"}));
	EXPECT_EQ(w.read(w.file), "winner");
}

THEN("^physical validation still checks the losing archive recor"
	"d$")
{
	[[maybe_unused]] World &w = world();

	w.status =
		pg_source_validate(w.sources.at("records"), &w.error);
	expect_status(w.status, PG_CHECKSUM, w.error);
}

GIVEN("^source \"([^\"]*)\" contains \"([^\"]*)\"$")
{
	[[maybe_unused]] World &w = world();
	REGEX_PARAM(std::string, label);
	REGEX_PARAM(std::string, name);

	w.loose(label, {{name, label}});
	w.attach(label);
}

GIVEN("^source \"base\" contains \"a/b\" and \"a/c/d\"$")
{
	[[maybe_unused]] World &w = world();

	w.loose("base", {{"a/b", "b"}, {"a/c/d", "d"}});
	w.attach("base");
}

GIVEN("^source \"patch\" containing \"a\" is attached later$")
{
	[[maybe_unused]] World &w = world();

	w.loose("patch", {{"a", "hidden"}});
	w.attach("patch");
}

WHEN("^I request the root and enumerate visible files$")
{
	[[maybe_unused]] World &w = world();

	w.request();
	expect_status(w.status, PG_OK, w.error);
	w.list();
	expect_status(w.status, PG_OK, w.error);
}

THEN("^\"a/b\" and \"a/c/d\" are returned$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.names, std::vector<std::string>({"a/b", "a/c/d"}));
}

THEN("^resolving \"a\" reports a directory conflict$")
{
	[[maybe_unused]] World &w = world();

	w.find("a");
	expect_status(w.status, PG_CONFLICT, w.error);
	EXPECT_EQ(w.file, nullptr);
}

WHEN("^I resolve \"a\"$")
{
	[[maybe_unused]] World &w = world();

	w.find("a");
}

THEN("^the result is a directory conflict$")
{
	[[maybe_unused]] World &w = world();

	expect_status(w.status, PG_CONFLICT, w.error);
	EXPECT_EQ(w.file, nullptr);
}

THEN("^only \"a/b\" is returned$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.names, std::vector<std::string>({"a/b"}));
}

THEN("^the names are \"a/b, a/c\"$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.names, std::vector<std::string>({"a/b", "a/c"}));
}

THEN("^\"a\" is a directory conflict$")
{
	[[maybe_unused]] World &w = world();

	w.find("a");
	expect_status(w.status, PG_CONFLICT, w.error);
}

GIVEN("^one existing archive contains \"a\" and \"a/b\"$")
{
	[[maybe_unused]] World &w = world();

	w.archive("archive", {{"a", "hidden"}, {"a/b", "b"}}, PG_PIGG2,
		true);
	w.attach("archive");
}

GIVEN("^source \"subtree\" contains \"a/b\" and \"a/c\"$")
{
	[[maybe_unused]] World &w = world();

	w.loose("subtree", {{"a/b", "b"}, {"a/c", "c"}});
	w.attach("subtree");
}

GIVEN("^the subtree \"a\" has been requested while watching$")
{
	[[maybe_unused]] World &w = world();

	w.request(true, "a");
	expect_status(w.status, PG_OK, w.error);
	w.watch(PG_WATCH_SCAN);
}

WHEN("^I detach \"subtree\" and poll$")
{
	[[maybe_unused]] World &w = world();

	expect_status(pg_tree_detach(w.tree, w.sources.at("subtree"),
				&w.error),
		PG_OK, w.error);
	w.poll();
	expect_status(w.status, PG_OK, w.error);
}

THEN("^a new lookup selects the file \"a\"$")
{
	[[maybe_unused]] World &w = world();

	w.select();
	EXPECT_EQ(w.read(w.file), "file");
}

THEN("^a visible callback reports removal of the descendants$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.reports(PG_CHANGE_REMOVE, "a/b"), 1u);
	EXPECT_EQ(w.reports(PG_CHANGE_REMOVE, "a/c"), 1u);
}

THEN("^a visible callback reports addition of \"a\"$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.reports(PG_CHANGE_ADD, "a"), 1u);
}

} // namespace
