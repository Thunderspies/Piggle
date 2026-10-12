#include "world.hpp"

namespace
{

World &world()
{
	WorldScope scope;

	return *scope;
}

GIVEN("^a loose root contains a readable file \"a/b\" and an unread"
	"able \"z\"$")
{
	[[maybe_unused]] World &w = world();

	w.paths["loose"] = "root";
	w.formats["loose"] = PG_LOOSE;
	write_native("root/a/b", "readable");
	write_native("root/z", "unreadable");
#ifndef _WIN32
	EXPECT_EQ(chmod("root/z", 0), 0);
#else
	// An inaccessible descendant is represented by a dangling
	// native link.
	EXPECT_TRUE(DeleteFileA("root/z"));
	ASSERT_TRUE(CreateSymbolicLinkA("root/z", "missing", 2));
#endif
}

WHEN("^I open that root as a source$")
{
	[[maybe_unused]] World &w = world();

	w.open("loose");
	w.use_tree = false;
}

THEN("^opening succeeds without visiting \"z\"$")
{
	[[maybe_unused]] World &w = world();

	expect_status(w.status, PG_OK, w.error);
	w.list(false);
	expect_status(w.status, PG_INVALID, w.error);
	EXPECT_EQ(w.cursor, nullptr);
}

WHEN("^I find \"a/b\"$")
{
	[[maybe_unused]] World &w = world();

	w.find("a/b", false);
}

THEN("^only the requested path and its ancestors are probed$")
{
	[[maybe_unused]] World &w = world();

	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.read(w.file), "readable");
	w.list(false);
	expect_status(w.status, PG_INVALID, w.error);
}

GIVEN("^an ordered tree has a loose source containing \"menu/a\"$")
{
	[[maybe_unused]] World &w = world();

	w.ready({{"menu/a", "a"}});
}

WHEN("^I request subtree \"menu\"$")
{
	[[maybe_unused]] World &w = world();

	w.request(true, "menu");
	expect_status(w.status, PG_OK, w.error);
}

THEN("^its loose names are cached before the call returns$")
{
	[[maybe_unused]] World &w = world();

	w.list(true, "menu");
	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.names, std::vector<std::string>({"menu/a"}));
}

THEN("^I capture a cursor for \"menu\"$")
{
	[[maybe_unused]] World &w = world();

	w.capture_files(true, "menu");
	expect_status(w.status, PG_OK, w.error);
	w.old_cursor = w.cursor;
	w.cursor = nullptr;
}

WHEN("^an editor adds \"menu/b\" while watching is off$")
{
	[[maybe_unused]] World &w = world();

	write_native(w.paths.at("loose") + "/menu/b", "b");
}

THEN("^another cursor still yields only \"menu/a\"$")
{
	[[maybe_unused]] World &w = world();

	w.list(true, "menu");
	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.names, std::vector<std::string>({"menu/a"}));
}

WHEN("^I request \"menu\" again and capture a new cursor$")
{
	[[maybe_unused]] World &w = world();

	w.request(true, "menu");
	expect_status(w.status, PG_OK, w.error);
	w.list(true, "menu");
}

THEN("^the new cursor yields \"menu/a, menu/b\"$")
{
	[[maybe_unused]] World &w = world();

	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.names,
		std::vector<std::string>({"menu/a", "menu/b"}));
}

THEN("^an earlier cursor retains its captured order and metadata"
	"$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.consume(w.old_cursor),
		std::vector<std::string>({"menu/a"}));
}

GIVEN("^an archive has many file records$")
{
	[[maybe_unused]] World &w = world();

	WorldFiles files;

	for (unsigned i = 0; i < 300; ++i)
		files.push_back(
			{"records/" + std::to_string(i), "payload"});
	raw_pigg("many.pigg", files);
	w.paths["archive"] = "many.pigg";
	w.formats["archive"] = PG_PIGG2;
}

WHEN("^I open the archive as a source$")
{
	[[maybe_unused]] World &w = world();

	w.open("archive");
}

THEN("^pathname indexing completes before the call returns$")
{
	[[maybe_unused]] World &w = world();

	expect_status(w.status, PG_OK, w.error);
	w.list(false);
	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.names.size(), 300u);
}

THEN("^invalid archive pathname metadata fails that open$")
{
	[[maybe_unused]] World &w = world();

	raw_pigg("invalid.pigg", {{"../escape", "invalid"}});
	pg_source *invalid = nullptr;

	w.status = pg_source_open(
		w.context, "invalid.pigg", nullptr, &invalid, &w.error);
	expect_status(w.status, PG_CORRUPT, w.error);
	EXPECT_EQ(invalid, nullptr);
}

GIVEN("^a loose source contains \"menu/a\" and \"other/b\"$")
{
	[[maybe_unused]] World &w = world();

	w.loose("loose", {{"menu/a", "a"}, {"other/b", "b"}});
	w.use_tree = false;
}

WHEN("^I enumerate \"menu\" before requesting it$")
{
	[[maybe_unused]] World &w = world();

	w.capture_files(w.use_tree, "menu");
}

THEN("^listing reports an invalid state and no cursor$")
{
	[[maybe_unused]] World &w = world();

	expect_status(w.status, PG_INVALID, w.error);
	EXPECT_EQ(w.cursor, nullptr);
}

WHEN("^I request \"menu\" on the source$")
{
	[[maybe_unused]] World &w = world();

	w.request(false, "menu");
	expect_status(w.status, PG_OK, w.error);
}

THEN("^listing \"menu\" returns a complete captured cursor$")
{
	[[maybe_unused]] World &w = world();

	w.list(false, "menu");
	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.names, std::vector<std::string>({"menu/a"}));
}

THEN("^listing the source root still reports an invalid state$")
{
	[[maybe_unused]] World &w = world();

	w.list(false);
	expect_status(w.status, PG_INVALID, w.error);
	EXPECT_EQ(w.cursor, nullptr);
}

GIVEN("^a loose source contains \"menu/a\" and watching is off$")
{
	[[maybe_unused]] World &w = world();

	w.loose("loose", {{"menu/a", "a"}});
	w.use_tree = false;
}

WHEN("^I request the root and then an editor adds \"menu/b\"$")
{
	[[maybe_unused]] World &w = world();

	w.request(false);
	expect_status(w.status, PG_OK, w.error);
	write_native(w.paths.at("loose") + "/menu/b", "b");
}

THEN("^listing \"menu\" yields only \"menu/a\" without rescanning$")
{
	[[maybe_unused]] World &w = world();

	w.list(false, "menu");
	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.names, std::vector<std::string>({"menu/a"}));
}

THEN("^finding \"menu/b\" reports not found from the index$")
{
	[[maybe_unused]] World &w = world();

	w.find("menu/b", false);
	expect_status(w.status, PG_NOT_FOUND, w.error);
	EXPECT_EQ(w.file, nullptr);
}

WHEN("^I request \"menu\" again$")
{
	[[maybe_unused]] World &w = world();

	w.request(false, "menu");
	expect_status(w.status, PG_OK, w.error);
}

THEN("^listing \"menu\" yields \"menu/a, menu/b\"$")
{
	[[maybe_unused]] World &w = world();

	w.list(false, "menu");
	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.names,
		std::vector<std::string>({"menu/a", "menu/b"}));
}

THEN("^finding \"menu/b\" succeeds$")
{
	[[maybe_unused]] World &w = world();

	w.select("menu/b", false);
	EXPECT_EQ(w.read(w.file), "b");
}

GIVEN("^a loose source has not requested \"menu\"$")
{
	[[maybe_unused]] World &w = world();

	w.loose("loose", {});
	std::filesystem::create_directories(
		w.paths.at("loose") + "/menu");
}

WHEN("^an editor adds \"menu/new\" and I find \"menu/new\"$")
{
	[[maybe_unused]] World &w = world();

	write_native(w.paths.at("loose") + "/menu/new", "new");
	w.find("menu/new", false);
}

THEN("^exact lookup selects the current loose copy$")
{
	[[maybe_unused]] World &w = world();

	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.read(w.file), "new");
}

THEN("^listing \"menu\" still reports an invalid state$")
{
	[[maybe_unused]] World &w = world();

	w.list(false, "menu");
	expect_status(w.status, PG_INVALID, w.error);
	EXPECT_EQ(w.cursor, nullptr);
}

GIVEN("^an archive source contains \"menu/a\"$")
{
	[[maybe_unused]] World &w = world();

	w.archive("archive", {{"menu/a", "a"}});
	w.use_tree = false;
}

WHEN("^I enumerate \"menu\" without requesting it$")
{
	[[maybe_unused]] World &w = world();

	w.list(false, "menu");
}

THEN("^a complete cursor containing \"menu/a\" is returned$")
{
	[[maybe_unused]] World &w = world();

	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.names, std::vector<std::string>({"menu/a"}));
}

GIVEN("^a tree has an archive and a loose source under \"menu\"$")
{
	[[maybe_unused]] World &w = world();

	w.archive("archive",
		{{"menu/a", "loser"}, {"menu/arch", "arch"}});
	w.loose("loose",
		{{"menu/a", "winner"}, {"menu/loose", "loose"}});
	w.attach("archive");
	w.attach("loose");
}

WHEN("^I enumerate \"menu\" before a tree request$")
{
	[[maybe_unused]] World &w = world();

	w.capture_files(true, "menu");
}

THEN("^listing reports an invalid state and no partial archive c"
	"ursor$")
{
	[[maybe_unused]] World &w = world();

	expect_status(w.status, PG_INVALID, w.error);
	EXPECT_EQ(w.cursor, nullptr);
}

WHEN("^I request \"menu\" on the tree$")
{
	[[maybe_unused]] World &w = world();

	w.request(true, "menu");
	expect_status(w.status, PG_OK, w.error);
}

THEN("^enumeration returns the complete overlay in canonical ord"
	"er$")
{
	[[maybe_unused]] World &w = world();

	w.list(true, "menu");
	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.names,
		std::vector<std::string>(
			{"menu/a", "menu/arch", "menu/loose"}));
	w.select("menu/a");
	EXPECT_EQ(w.read(w.file), "winner");
}

GIVEN("^a loose subtree contains many files$")
{
	[[maybe_unused]] World &w = world();

	WorldFiles files;

	for (unsigned i = 0; i < 300; ++i)
		files.push_back(
			{"many/" + std::to_string(i), "payload"});
	w.loose("loose", files);
}

WHEN("^I request that subtree$")
{
	[[maybe_unused]] World &w = world();

	w.request(false, "many");
	expect_status(w.status, PG_OK, w.error);
}

THEN("^its index is complete when the call returns$")
{
	[[maybe_unused]] World &w = world();

	w.list(false, "many");
	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.names.size(), 300u);
}

THEN("^a later listing needs no operation handle or directory sc"
	"an$")
{
	[[maybe_unused]] World &w = world();

	write_native(w.paths.at("loose") + "/many/late", "late");
	w.list(false, "many");
	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.names.size(), 300u);
	EXPECT_EQ(
		std::count(w.names.begin(), w.names.end(), "many/late"),
		0);
}

GIVEN("^a requested loose subtree contains \"menu/a\" while watchin"
	"g is off$")
{
	[[maybe_unused]] World &w = world();

	w.loose("loose", {{"menu/a", "cached"}});
	w.request(false, "menu");
	expect_status(w.status, PG_OK, w.error);
	w.select("menu/a", false);
	w.retain_selection();
}

WHEN("^an editor removes \"menu/a\" and I find it again$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_TRUE(std::filesystem::remove(
		w.paths.at("loose") + "/menu/a"));
	w.find("menu/a", false);
}

THEN("^find returns the cached selection and metadata$")
{
	[[maybe_unused]] World &w = world();

	expect_status(w.status, PG_OK, w.error);
	pg_file_info info;

	expect_status(pg_file_inspect(w.file, &info, &w.error), PG_OK,
		w.error);
	EXPECT_EQ(info.copy_generation, w.captured.copy_generation);
	EXPECT_EQ(info.logical_size, w.captured.logical_size);
}

THEN("^opening that physical copy reports stale$")
{
	[[maybe_unused]] World &w = world();

	w.status = pg_reader_open(
		w.file, PG_READ_LOGICAL, &w.reader, &w.error);
	expect_status(w.status, PG_STALE, w.error);
	EXPECT_EQ(w.reader, nullptr);
}

GIVEN("^a loose root contains an empty directory and a nested fil"
	"e$")
{
	[[maybe_unused]] World &w = world();

	w.loose("loose", {{"nested/file", "nested"}, {"a", "direct"}});
	std::filesystem::create_directories(
		w.paths.at("loose") + "/empty");
}

WHEN("^I discover only its immediate children$")
{
	[[maybe_unused]] World &w = world();

	w.status = pg_source_discover(
		w.source(), nullptr, PG_DISCOVER_CHILDREN, &w.error);
	expect_status(w.status, PG_OK, w.error);
}

THEN("^I can list its files and child directories$")
{
	[[maybe_unused]] World &w = world();

	w.capture_entries(false);
	expect_status(w.status, PG_OK, w.error);
	w.entries = w.consume_entries(w.entry_cursor);
	ASSERT_EQ(w.entries.size(), 3u);
	EXPECT_EQ(w.entries[0].name, "a");
	EXPECT_EQ(w.entries[1].name, "empty");
	EXPECT_EQ(w.entries[2].name, "nested");
}

THEN("^recursive listing still reports incomplete coverage$")
{
	[[maybe_unused]] World &w = world();

	w.capture_entries(false, PG_ENTRIES_RECURSIVE);
	expect_status(w.status, PG_INVALID, w.error);
	EXPECT_EQ(w.entry_cursor, nullptr);
}

THEN("^child directories are never opened until requested$")
{
	[[maybe_unused]] World &w = world();

// Inaccessible children do not prevent a shallow refresh.
#ifndef _WIN32
	EXPECT_EQ(
		chmod((w.paths.at("loose") + "/nested").c_str(), 0), 0);
#endif
	expect_status(pg_source_discover(w.source(), nullptr,
				PG_DISCOVER_CHILDREN, &w.error),
		PG_OK, w.error);
#ifndef _WIN32
	EXPECT_EQ(
		chmod((w.paths.at("loose") + "/nested").c_str(), 0700),
		0);
#endif
}

GIVEN("^a tree with a loose source and an unqueried nested file$")
{
	[[maybe_unused]] World &w = world();

	w.ready({{"deep/a", "old"}});
}

WHEN("^I manage the root recursively and enable native observati"
	"on$")
{
	[[maybe_unused]] World &w = world();

	w.manage();
	w.watch();
}

THEN("^recursive listing still requires discovery$")
{
	[[maybe_unused]] World &w = world();

	w.list();
	expect_status(w.status, PG_INVALID, w.error);
	EXPECT_EQ(w.cursor, nullptr);
}

THEN("^editing the unqueried file reports an invalidation$")
{
	[[maybe_unused]] World &w = world();

	write_native(w.paths.at("loose") + "/deep/a", "unknown edit");
	w.poll();
	expect_status(w.status, PG_OK, w.error);
	EXPECT_GT(w.reports(PG_CHANGE_INVALIDATE), 0u);
}

WHEN("^I query the file and edit it again$")
{
	[[maybe_unused]] World &w = world();

	w.select("deep/a");
	w.events.clear();
	write_native(w.paths.at("loose") + "/deep/a",
		"known edit, larger size");
	w.poll();
	expect_status(w.status, PG_OK, w.error);
}

THEN("^the next report includes its before and after metadata$")
{
	[[maybe_unused]] World &w = world();

	ASSERT_EQ(w.reports(PG_CHANGE_UPDATE, "deep/a"), 1u);
	auto event = std::find_if(w.events.begin(), w.events.end(),
		[](const auto &entry) {
			return entry.kind == PG_CHANGE_UPDATE &&
				entry.name == "deep/a";
		});

	ASSERT_NE(event, w.events.end());
	EXPECT_TRUE(event->before);
	EXPECT_TRUE(event->after);
	EXPECT_NE(event->before_size, event->after_size);
}

GIVEN("^native management of a loose root$")
{
	[[maybe_unused]] World &w = world();

	w.ready({});
	w.manage();
	w.watch();
}

GIVEN("^a lookup has established that a managed name is absent$")
{
	[[maybe_unused]] World &w = world();

	w.find("new");
	expect_status(w.status, PG_NOT_FOUND, w.error);
}

WHEN("^that name is created$")
{
	[[maybe_unused]] World &w = world();

	write_native(w.paths.at("loose") + "/new", "created");
	w.poll();
	expect_status(w.status, PG_OK, w.error);
}

THEN("^an addition is reported$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.reports(PG_CHANGE_ADD, "new"), 1u);
}

THEN("^directory changes are observable without file changes$")
{
	[[maybe_unused]] World &w = world();

	std::filesystem::create_directories(
		w.paths.at("loose") + "/empty");
	w.poll();
	expect_status(w.status, PG_OK, w.error);
	w.request();
	expect_status(w.status, PG_OK, w.error);
	w.events.clear();
	EXPECT_TRUE(std::filesystem::remove(
		w.paths.at("loose") + "/empty"));
	w.poll();
	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.reports(PG_CHANGE_REMOVE, "empty"), 1u);
	ASSERT_FALSE(w.events.empty());
	EXPECT_TRUE(w.events.back().directory);
}

GIVEN("^a managed loose scope with no prior discovery$")
{
	[[maybe_unused]] World &w = world();

	w.ready({{"deep/a", "old"}});
	w.manage();
}

WHEN("^I enable scan observation$")
{
	[[maybe_unused]] World &w = world();

	w.watch(PG_WATCH_SCAN);
}

THEN("^the scope is discovered at the registered depth$")
{
	[[maybe_unused]] World &w = world();

	w.list();
	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.names, std::vector<std::string>({"deep/a"}));
}

THEN("^subsequent polls compare its visible entries$")
{
	[[maybe_unused]] World &w = world();

	w.poll();
	expect_status(w.status, PG_OK, w.error);
	EXPECT_TRUE(w.events.empty());
	write_native(
		w.paths.at("loose") + "/deep/a", "changed contents");
	w.poll();
	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.reports(PG_CHANGE_UPDATE, "deep/a"), 1u);
}

GIVEN("^a managed loose root with discovered and unknown paths$")
{
	[[maybe_unused]] World &w = world();

	w.ready({{"a", "old"}, {"deep/unknown", "unknown"}});
	w.manage();
	w.select();
	w.retain_selection();
	w.watch();
}

WHEN("^the root is moved away$")
{
	[[maybe_unused]] World &w = world();

	std::filesystem::rename(w.paths.at("loose"), "moved-root");
	w.poll();
	expect_status(w.status, PG_OK, w.error);
}

THEN("^polling reports loss and scope invalidation$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_GT(w.reports(PG_CHANGE_LOSS), 0u);
	EXPECT_GT(w.reports(PG_CHANGE_INVALIDATE), 0u);
}

THEN("^it does not recursively discover unknown paths$")
{
	[[maybe_unused]] World &w = world();

	w.list();
	expect_status(w.status, PG_INVALID, w.error);
	EXPECT_EQ(w.cursor, nullptr);
}

WHEN("^a directory is created at the original root path$")
{
	[[maybe_unused]] World &w = world();

	std::filesystem::create_directories(w.paths.at("loose"));
	w.events.clear();
	w.poll();
	expect_status(w.status, PG_OK, w.error);
}

THEN("^native monitoring resumes there$")
{
	[[maybe_unused]] World &w = world();

	write_native(w.paths.at("loose") + "/after", "new root");
	w.poll();
	expect_status(w.status, PG_OK, w.error);
	EXPECT_GT(w.reports(PG_CHANGE_INVALIDATE) +
			w.reports(PG_CHANGE_ADD),
		0u);
	w.select("after");
	EXPECT_EQ(w.read(w.file), "new root");
}

THEN("^previous selections cannot silently retarget to the new r"
	"oot$")
{
	[[maybe_unused]] World &w = world();

	write_native(w.paths.at("loose") + "/a", "replacement");
	w.status = pg_reader_open(
		w.held, PG_READ_LOGICAL, &w.reader, &w.error);
	expect_status(w.status, PG_STALE, w.error);
	EXPECT_EQ(w.reader, nullptr);
}

GIVEN("^native observation of a managed scope with an empty direc"
	"tory$")
{
	[[maybe_unused]] World &w = world();

	w.ready({{"a", "old"}});
	std::filesystem::create_directories(
		w.paths.at("loose") + "/empty");
	w.manage();
	w.watch();
}

WHEN("^I discover the scope and a file is edited$")
{
	[[maybe_unused]] World &w = world();

	w.request();
	expect_status(w.status, PG_OK, w.error);
	write_native(w.paths.at("loose") + "/a", "changed contents");
	w.poll();
	expect_status(w.status, PG_OK, w.error);
}

THEN("^only the file transition is reported$")
{
	[[maybe_unused]] World &w = world();

	ASSERT_EQ(w.events.size(), 1u);
	EXPECT_EQ(w.events.front().name, "a");
	EXPECT_EQ(w.events.front().kind, PG_CHANGE_UPDATE);
}

THEN("^the existing directory does not produce an initial additi"
	"on$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.reports(PG_CHANGE_ADD, "empty"), 0u);
}

GIVEN("^cached metadata in a managed loose scope and an attached "
	"archive$")
{
	[[maybe_unused]] World &w = world();

	w.ready({{"a", "old"}}, true);
	w.manage();
	w.select();
	w.retain_selection();
	w.archive("archive", {{"b", "archive"}});
	w.attach("archive");
	w.active = "loose";
}

WHEN("^watch setup or root replacement refresh encounters a corr"
	"upt archive$")
{
	[[maybe_unused]] World &w = world();

	write_native(w.paths.at("loose") + "/a", "changed contents");
	write_native(w.paths.at("archive"), "bad");
	w.status = pg_tree_watch(w.tree, PG_WATCH_NATIVE, &w.error);
}

THEN("^the refresh fails without replacing prior cached observat"
	"ions$")
{
	[[maybe_unused]] World &w = world();

	expect_status(w.status, PG_CORRUPT, w.error);
	w.select();
	pg_file_info info;

	expect_status(pg_file_inspect(w.file, &info, &w.error), PG_OK,
		w.error);
	EXPECT_EQ(info.logical_size, w.captured.logical_size);
	EXPECT_EQ(info.copy_generation, w.captured.copy_generation);
	w.poll();
	expect_status(w.status, PG_INVALID, w.error);
}

GIVEN("^a visible report is being delivered$")
{
	[[maybe_unused]] World &w = world();

	w.ready({{"a", "old"}});
	w.manage();
	w.watch();
	write_native(w.paths.at("loose") + "/a", "first cut");
}

WHEN("^callback lookup detects a root replacement$")
{
	[[maybe_unused]] World &w = world();

	w.callback_replace_root = true;
	w.poll();
	expect_status(w.status, PG_OK, w.error);
	EXPECT_TRUE(w.callback_done);
	EXPECT_EQ(w.callback_status, PG_NOT_FOUND);
}

THEN("^its loss and scope invalidation wait for the next poll$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.reports(PG_CHANGE_LOSS), 0u);
	w.events.clear();
	w.poll();
	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.reports(PG_CHANGE_LOSS), 1u);
	EXPECT_GT(w.reports(PG_CHANGE_INVALIDATE), 0u);
}

GIVEN("^native management with a known file and a known absent de"
	"stination$")
{
	[[maybe_unused]] World &w = world();

	w.ready({{"deep/a", "before rename"}});
	w.manage();
	w.select("deep/a");
	w.find("renamed/a");
	expect_status(w.status, PG_NOT_FOUND, w.error);
	w.watch();
}

WHEN("^its parent directory is renamed inside the root$")
{
	[[maybe_unused]] World &w = world();

	std::filesystem::rename(w.paths.at("loose") + "/deep",
		w.paths.at("loose") + "/renamed");
	w.poll();
	expect_status(w.status, PG_OK, w.error);
}

THEN("^the old name is removed and the new name is added$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.reports(PG_CHANGE_REMOVE, "deep/a"), 1u);
	EXPECT_EQ(w.reports(PG_CHANGE_ADD, "renamed/a"), 1u);
}

THEN("^subsequent file edits are reported under the new name$")
{
	[[maybe_unused]] World &w = world();

	w.events.clear();
	write_native(w.paths.at("loose") + "/renamed/a",
		"changed after rename");
	w.poll();
	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.reports(PG_CHANGE_UPDATE, "renamed/a"), 1u);
}

} // namespace
