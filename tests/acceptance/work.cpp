#include "world.hpp"

namespace
{

World &world()
{
	WorldScope scope;

	return *scope;
}

GIVEN("^an archive has many entries and long names$")
{
	[[maybe_unused]] World &w = world();

	WorldFiles files;

	for (unsigned i = 0; i < 300; ++i)
		files.push_back({"long/" + std::string(100, 'x') + "/" +
				std::to_string(i),
			"payload"});
	raw_pigg("many.pigg", files);
	w.paths["archive"] = "many.pigg";
	w.formats["archive"] = PG_PIGG2;
}

WHEN("^I open it as a source$")
{
	[[maybe_unused]] World &w = world();

	w.open("archive");
}

THEN("^all archive pathname metadata is indexed before return$")
{
	[[maybe_unused]] World &w = world();

	expect_status(w.status, PG_OK, w.error);
	w.list(false);
	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.names.size(), 300u);
}

THEN("^the source output stays empty if indexing fails$")
{
	[[maybe_unused]] World &w = world();

	raw_pigg("invalid.pigg", {{"../escape", "bad"}});
	pg_source *invalid = nullptr;

	expect_status(pg_source_open(w.context, "invalid.pigg", nullptr,
				&invalid, &w.error),
		PG_CORRUPT, w.error);
	EXPECT_EQ(invalid, nullptr);
}

THEN("^no loose subtree is recursively scanned at source open$")
{
	[[maybe_unused]] World &w = world();

	w.loose("loose", {{"deep/a", "a"}});
	w.list(false);
	expect_status(w.status, PG_INVALID, w.error);
	EXPECT_EQ(w.cursor, nullptr);
}

GIVEN("^an independent reader and an unfinished writer$")
{
	[[maybe_unused]] World &w = world();

	w.ready({{"a", "abcdefgh"}});
	expect_status(pg_reader_open_tree(w.tree, "a", PG_READ_LOGICAL,
				&w.reader, &w.error),
		PG_OK, w.error);
	w.staged("output", 8);
}

WHEN("^I read and write chunks of sizes I choose$")
{
	[[maybe_unused]] World &w = world();

	for (size_t capacity : {1u, 2u, 5u}) {
		std::string chunk = w.chunk(w.reader, capacity);

		expect_status(w.status, PG_OK, w.error);
		EXPECT_EQ(chunk.size(), capacity);
		w.bytes += chunk;
		w.write(chunk);
	}
}

THEN("^each call returns an exact byte count before another chun"
	"k begins$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.bytes, "abcdefgh");
	EXPECT_EQ(w.count, 5u);
	w.find("output");
	expect_status(w.status, PG_NOT_FOUND, w.error);
}

THEN("^I can yield my coroutine between chunk calls$")
{
	[[maybe_unused]] World &w = world();

	std::this_thread::yield();
	EXPECT_EQ(w.chunk(w.reader, 1), "");
	expect_status(w.status, PG_END, w.error);
}

THEN("^finishing the writer may block until publication complete"
	"s$")
{
	[[maybe_unused]] World &w = world();

	w.finished();
	w.select("output");
	EXPECT_EQ(w.read(w.file), "abcdefgh");
}

GIVEN("^a builder with a partly written entry$")
{
	[[maybe_unused]] World &w = world();

	expect_status(pg_archive_builder_create(w.context, "built",
				PG_PIGG2, 0, &w.builder, &w.error),
		PG_OK, w.error);
	pg_write_options options;

	pg_write_options_init(&options, 6);
	expect_status(pg_writer_open_archive_builder(w.builder, "a",
				&options, &w.writer, &w.error),
		PG_OK, w.error);
	w.write("part");
}

WHEN("^I close that entry writer and then close the builder$")
{
	[[maybe_unused]] World &w = world();

	expect_status(
		pg_writer_close(&w.writer, &w.error), PG_OK, w.error);
	expect_status(pg_archive_builder_close(&w.builder, &w.error),
		PG_OK, w.error);
}

THEN("^neither the entry nor the archive is published$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_FALSE(std::filesystem::exists("built"));
	EXPECT_EQ(w.writer, nullptr);
	EXPECT_EQ(w.builder, nullptr);
}

GIVEN("^many visible changes are pending in watched scopes$")
{
	[[maybe_unused]] World &w = world();

	w.ready({}, true);
	w.watch(PG_WATCH_SCAN);
	for (unsigned i = 0; i < 100; ++i) {
		std::string name = "file-" + std::to_string(i);

		expect_status(
			pg_source_write_all(w.source(), name.c_str(),
				"new", 3, nullptr, &w.error),
			PG_OK, w.error);
	}
}

THEN("^all callbacks in the finite observation cut complete befo"
	"re return$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.events.size(), 100u);
	EXPECT_EQ(w.reports(PG_CHANGE_ADD), 100u);
}

THEN("^no callback in that cut is delivered twice$")
{
	[[maybe_unused]] World &w = world();

	std::set<uint64_t> unique(
		w.sequences.begin(), w.sequences.end());

	EXPECT_EQ(unique.size(), 100u);
	w.poll();
	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.events.size(), 100u);
}

GIVEN("^a loose source directory contains many files$")
{
	[[maybe_unused]] World &w = world();

	WorldFiles files;

	for (unsigned i = 0; i < 20; ++i)
		files.push_back({"file-" + std::to_string(i), "old"});
	w.loose("loose", files);
}

WHEN("^its explicit deletion unlinks one file and then encounter"
	"s an I/O error$")
{
	[[maybe_unused]] World &w = world();

#ifdef __linux__
	pg_test_journal_fault(5);
	w.status = pg_source_delete(w.source(), &w.error);
	w.saved_error = w.error;
	pg_test_journal_fault(0);
#endif
}

THEN("^the call returns partial$")
{
	[[maybe_unused]] World &w = world();

	expect_status(w.status, PG_PARTIAL, w.error);
}

THEN("^the error cause identifies the underlying failure$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.saved_error.cause, PG_IO);
	EXPECT_EQ(w.saved_error.native_code, EIO);
}

THEN("^remaining files are not described as deleted$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(std::distance(std::filesystem::directory_iterator(
					w.paths.at("loose")),
				std::filesystem::directory_iterator()),
		19);
	w.find("file-19", false);
	expect_status(w.status, PG_STALE, w.error);
}

GIVEN("^valid output storage and a context$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_NE(w.context, nullptr);
}

WHEN("^a call fails before producing a handle$")
{
	[[maybe_unused]] World &w = world();

	w.status = pg_source_open(w.context, "missing", nullptr,
		&w.sources["missing"], &w.error);
}

THEN("^the output handle is null$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.sources.at("missing"), nullptr);
}

THEN("^the diagnostic identifies the failure$")
{
	[[maybe_unused]] World &w = world();

	expect_status(w.status, PG_NOT_FOUND, w.error);
	EXPECT_EQ(w.error.status, w.status);
}

THEN("^closing a context with outstanding children returns busy$")
{
	[[maybe_unused]] World &w = world();

	w.loose("loose", {});
	pg_context *owned = w.context;

	expect_status(pg_context_close(&w.context, &w.error), PG_BUSY,
		w.error);
	EXPECT_EQ(w.context, owned);
}

GIVEN("^one control thread owns a tree$")
{
	[[maybe_unused]] World &w = world();

	w.ready({{"a", "abcdefgh"}});
}

GIVEN("^two independent readers refer to that tree's sources$")
{
	[[maybe_unused]] World &w = world();

	expect_status(pg_reader_open_tree(w.tree, "a", PG_READ_LOGICAL,
				&w.reader, &w.error),
		PG_OK, w.error);
	expect_status(pg_reader_open_tree(w.tree, "a", PG_READ_LOGICAL,
				&w.other_reader, &w.error),
		PG_OK, w.error);
}

WHEN("^workers read caller\\-sized chunks with serialized access "
	"per handle$")
{
	[[maybe_unused]] World &w = world();

	auto consume = [](pg_reader *reader) {
		std::string bytes;
		pg_error error;
		char buffer[3];
		size_t count = 0;
		pg_status status;

		while ((status = pg_reader_read(reader, buffer,
				sizeof(buffer), &count, &error)) ==
			PG_OK)
			bytes.append(buffer, count);
		return status == PG_END ? bytes : std::string("failed");
	};
	std::thread first([&]() { w.bytes = consume(w.reader); });
	std::thread second(
		[&]() { w.previous = consume(w.other_reader); });

	first.join();
	second.join();
}

THEN("^reader work may proceed concurrently$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.bytes, "abcdefgh");
	EXPECT_EQ(w.previous, "abcdefgh");
}

THEN("^attached\\-source publication remains on the tree's contro"
	"l thread$")
{
	[[maybe_unused]] World &w = world();

	std::thread worker([&]() {
		pg_error error;

		w.mutation_status = pg_source_write_all(
			w.source(), "a", "worker", 6, nullptr, &error);
	});

	worker.join();
	EXPECT_EQ(w.mutation_status, PG_BUSY);
	expect_status(pg_source_write_all(w.source(), "a", "control", 7,
				nullptr, &w.error),
		PG_OK, w.error);
}

GIVEN("^identical inputs to a whole\\-file archive export and a st"
	"reamed export$")
{
	[[maybe_unused]] World &w = world();

	w.archive("archive", {{"a", "export bytes"}});
	w.select("a", false);
	write_native("whole", "old");
	write_native("streamed", "old");
}

WHEN("^both workflows complete successfully$")
{
	[[maybe_unused]] World &w = world();

	expect_status(
		pg_file_export(w.file, "whole", PG_OVERWRITE, &w.error),
		PG_OK, w.error);
	pg_write_options options;

	pg_write_options_init(&options, 12);
	expect_status(pg_reader_open(w.file, PG_READ_LOGICAL, &w.reader,
				&w.error),
		PG_OK, w.error);
	expect_status(
		pg_writer_open_native(w.context, "streamed", &options,
			PG_OVERWRITE, &w.writer, &w.error),
		PG_OK, w.error);
	while (true) {
		auto bytes = w.chunk(w.reader, 3);

		if (w.status == PG_END)
			break;
		expect_status(w.status, PG_OK, w.error);
		w.write(bytes);
	}
	expect_status(
		pg_reader_close(&w.reader, &w.error), PG_OK, w.error);
	w.finished();
}

THEN("^they choose identical visible files and produce identical"
	" contents$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(read_native("whole"), "export bytes");
	EXPECT_EQ(read_native("streamed"), read_native("whole"));
}

THEN("^they obey identical publication and ownership rules$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.reader, nullptr);
	EXPECT_EQ(w.writer, nullptr);
	pg_reader *check = nullptr;

	expect_status(pg_reader_open_native(
				w.context, "whole", &check, &w.error),
		PG_OK, w.error);
	expect_status(
		pg_reader_close(&check, &w.error), PG_OK, w.error);
	expect_status(pg_reader_open_native(
				w.context, "streamed", &check, &w.error),
		PG_OK, w.error);
	expect_status(
		pg_reader_close(&check, &w.error), PG_OK, w.error);
}

GIVEN("^a cursor capturing multiple archive files and an existing"
	" directory$")
{
	[[maybe_unused]] World &w = world();

	w.archive("archive", {{"a", "a"}, {"dir/b", "b"}});
	w.capture_files(false);
	expect_status(w.status, PG_OK, w.error);
	std::filesystem::create_directories("out");
}

WHEN("^I open an unpack target$")
{
	[[maybe_unused]] World &w = world();

	w.status = pg_unpack_target_open(
		w.cursor, "out", PG_OVERWRITE, &w.target, &w.error);
	expect_status(w.status, PG_OK, w.error);
}

THEN("^all captured paths are preflighted before publication$")
{
	[[maybe_unused]] World &w = world();

	for (const auto &entry :
		std::filesystem::recursive_directory_iterator("out")) {
		EXPECT_FALSE(entry.is_regular_file());
	}
	EXPECT_NE(w.target, nullptr);
}

THEN("^substituting the root before writer open or finish fails "
	"stale$")
{
	[[maybe_unused]] World &w = world();

	expect_status(pg_cursor_next(w.cursor, &w.file, &w.error),
		PG_OK, w.error);
	std::filesystem::rename("out", "old-out");
	std::filesystem::create_directories("out");
	w.status = pg_writer_open_unpack(
		w.target, w.file, &w.writer, &w.error);
	expect_status(w.status, PG_STALE, w.error);
	EXPECT_EQ(w.writer, nullptr);
	for (const auto &entry :
		std::filesystem::recursive_directory_iterator("out")) {
		EXPECT_FALSE(entry.is_regular_file());
	}
	for (const auto &entry :
		std::filesystem::recursive_directory_iterator(
			"old-out")) {
		EXPECT_FALSE(entry.is_regular_file());
	}
}

GIVEN("^a reader with no pending work$")
{
	[[maybe_unused]] World &w = world();

	write_native("input", "reader");
	expect_status(pg_reader_open_native(
				w.context, "input", &w.reader, &w.error),
		PG_OK, w.error);
}

WHEN("^I close it through its owned pointer address$")
{
	[[maybe_unused]] World &w = world();

	w.status = pg_reader_close(&w.reader, &w.error);
}

THEN("^the owned reader pointer is null when the call returns$")
{
	[[maybe_unused]] World &w = world();

	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.reader, nullptr);
}

THEN("^cleanup failure is reported without restoring ownership$")
{
	[[maybe_unused]] World &w = world();

#ifdef __linux__
	expect_status(pg_reader_open_native(
				w.context, "input", &w.reader, &w.error),
		PG_OK, w.error);
	pg_test_journal_fault(7);
	w.status = pg_reader_close(&w.reader, &w.error);
	pg_test_journal_fault(0);
	expect_status(w.status, PG_IO, w.error);
	EXPECT_EQ(w.reader, nullptr);
#else
	// Null closes remain synchronous no-ops on platforms without
	// fault injection.
	expect_status(
		pg_reader_close(&w.reader, &w.error), PG_OK, w.error);
	EXPECT_EQ(w.reader, nullptr);
#endif
}

GIVEN("^a loose source contains files but no subtree has been req"
	"uested$")
{
	[[maybe_unused]] World &w = world();

	w.loose("loose", {{"a", "a"}, {"dir/b", "b"}});
	std::filesystem::create_directories("out");
}

WHEN("^I pack that source and unpack it into an empty native dir"
	"ectory$")
{
	[[maybe_unused]] World &w = world();

	expect_status(pg_source_pack(w.source(), "packed", PG_PIGG2,
				nullptr, &w.error),
		PG_OK, w.error);
	expect_status(pg_source_unpack(w.source(), "out", 0, &w.error),
		PG_OK, w.error);
	w.paths["packed"] = "packed";
	w.formats["packed"] = PG_PIGG2;
	w.open("packed");
	expect_status(w.status, PG_OK, w.error);
}

THEN("^both operations include every currently discoverable loos"
	"e file$")
{
	[[maybe_unused]] World &w = world();

	w.list(false);
	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.names, std::vector<std::string>({"a", "dir/b"}));
	EXPECT_EQ(read_native("out/a"), "a");
	EXPECT_EQ(read_native("out/dir/b"), "b");
}

THEN("^neither operation requires the caller to request the root"
	" first$")
{
	[[maybe_unused]] World &w = world();

	w.active = "loose";
	write_native(w.paths.at("loose") + "/late", "late");
	expect_status(pg_source_pack(w.source(), "second", PG_PIGG2,
				nullptr, &w.error),
		PG_OK, w.error);
	expect_status(pg_source_unpack(w.source(), "out", PG_OVERWRITE,
				&w.error),
		PG_OK, w.error);
	EXPECT_EQ(read_native("out/late"), "late");
}

GIVEN("^a requested loose subtree has no file \"menu/new\"$")
{
	[[maybe_unused]] World &w = world();

	w.loose("loose", {{"menu/a", "a"}});
	w.request(false, "menu");
	expect_status(w.status, PG_OK, w.error);
	w.find("menu/new", false);
	expect_status(w.status, PG_NOT_FOUND, w.error);
}

WHEN("^an external editor creates \"menu/NEW\"$")
{
	[[maybe_unused]] World &w = world();

	write_native(w.paths.at("loose") + "/menu/NEW", "external");
}

WHEN("^I write \"menu/new\" through the source API$")
{
	[[maybe_unused]] World &w = world();

	w.status = pg_source_write_all(w.source(), "menu/new",
		"replacement", 11, nullptr, &w.error);
	expect_status(w.status, PG_OK, w.error);
}

THEN("^the writer replaces the current native winner \"menu/NEW\"$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(read_native(w.paths.at("loose") + "/menu/NEW"),
		"replacement");
}

THEN("^it does not create a second physical file \"menu/new\"$")
{
	[[maybe_unused]] World &w = world();

	std::vector<std::string> names;

	for (const auto &entry : std::filesystem::directory_iterator(
			w.paths.at("loose") + "/menu"))
		names.push_back(entry.path().filename().string());
	EXPECT_EQ(names.size(), 2u);
	EXPECT_EQ(std::count(names.begin(), names.end(), "NEW"), 1);
}

} // namespace
