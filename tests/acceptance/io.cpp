#include "world.hpp"

namespace
{

World &world()
{
	WorldScope scope;

	return *scope;
}

GIVEN("^\"base\" contains \"a\" and loses to \"patch\"$")
{
	[[maybe_unused]] World &w = world();

	w.loose("base", {{"a", "base"}});
	w.loose("patch", {{"a", "patch"}});
	w.attach("base");
	w.attach("patch");
	w.active = "base";
}

WHEN("^I import a native file into \"a\" in destination source \"ba"
	"se\"$")
{
	[[maybe_unused]] World &w = world();

	write_native("input", "replacement");
	expect_status(pg_reader_open_native(
				w.context, "input", &w.reader, &w.error),
		PG_OK, w.error);
	w.staged("a", 11);
}

WHEN("^only part of the input has been transferred$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.chunk(w.reader, 4), "repl");
	expect_status(w.status, PG_OK, w.error);
	w.write("repl");
}

THEN("^the old base copy and current patch winner remain publish"
	"ed$")
{
	[[maybe_unused]] World &w = world();

	w.select("a", false);
	EXPECT_EQ(w.read(w.file), "base");
	w.select();
	EXPECT_EQ(w.read(w.file), "patch");
}

WHEN("^validation and commitment complete$")
{
	[[maybe_unused]] World &w = world();

	w.write(w.chunk(w.reader, 32));
	expect_status(
		pg_reader_close(&w.reader, &w.error), PG_OK, w.error);
	w.finished();
}

THEN("^only the base copy is replaced$")
{
	[[maybe_unused]] World &w = world();

	w.select("a", false);
	EXPECT_EQ(w.read(w.file), "replacement");
}

THEN("^the patch copy remains visible$")
{
	[[maybe_unused]] World &w = world();

	w.select();
	EXPECT_EQ(w.read(w.file), "patch");
}

GIVEN("^a binary file containing zero bytes and 257 logical bytes"
	"$")
{
	[[maybe_unused]] World &w = world();

	w.loose("loose", {{"a", "old"}});
	for (unsigned i = 0; i < 257; ++i)
		w.expected.push_back(static_cast<char>(i));
	w.staged("a", w.expected.size());
}

WHEN("^I write it in chunks of 1, 7, 64, and the remaining bytes"
	"$")
{
	[[maybe_unused]] World &w = world();

	size_t offset = 0;

	for (size_t size : {1u, 7u, 64u, 185u}) {
		w.write(w.expected.substr(offset, size));
		offset += w.count;
	}
	EXPECT_EQ(offset, w.expected.size());
}

WHEN("^I finish and read it with output capacities of 3 and 11 r"
	"epeatedly$")
{
	[[maybe_unused]] World &w = world();

	w.finished();
	w.select("a", false);
	expect_status(pg_reader_open(w.file, PG_READ_LOGICAL, &w.reader,
				&w.error),
		PG_OK, w.error);
	size_t capacity = 3;

	while (true) {
		auto bytes = w.chunk(w.reader, capacity);

		w.bytes += bytes;
		if (w.status == PG_END)
			break;
		expect_status(w.status, PG_OK, w.error);
		EXPECT_GT(w.count, 0u);
		capacity = capacity == 3 ? 11 : 3;
	}
}

THEN("^the concatenated output equals all 257 input bytes$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.bytes, w.expected);
	EXPECT_EQ(w.bytes.size(), 257u);
}

THEN("^each transfer count advances only by bytes consumed or pr"
	"oduced$")
{
	[[maybe_unused]] World &w = world();

	uint64_t position = 0;

	expect_status(pg_reader_tell(w.reader, &position, &w.error),
		PG_OK, w.error);
	EXPECT_EQ(position, 257u);
	EXPECT_EQ(w.count, 0u);
}

GIVEN("^the destination contains \"old\"$")
{
	[[maybe_unused]] World &w = world();

	w.loose("loose", {{"a", "old"}});
}

WHEN("^I finish a replacement without writing any bytes$")
{
	[[maybe_unused]] World &w = world();

	w.staged("a", 0);
	w.finished();
}

THEN("^the destination is an existing empty regular file$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_TRUE(std::filesystem::is_regular_file(
		w.paths.at("loose") + "/a"));
	EXPECT_EQ(
		std::filesystem::file_size(w.paths.at("loose") + "/a"),
		0u);
	w.select("a", false);
}

THEN("^its reader reaches end with zero bytes$")
{
	[[maybe_unused]] World &w = world();

	expect_status(pg_reader_open(w.file, PG_READ_LOGICAL, &w.reader,
				&w.error),
		PG_OK, w.error);
	EXPECT_EQ(w.chunk(w.reader, 1), "");
	expect_status(w.status, PG_END, w.error);
	EXPECT_EQ(w.count, 0u);
}

GIVEN("^the selected file contains \"new\"$")
{
	[[maybe_unused]] World &w = world();

	w.loose("loose", {{"a", "new"}});
	w.select("a", false);
}

GIVEN("^the native destination is a regular file containing \"old\""
	"$")
{
	[[maybe_unused]] World &w = world();

	write_native("output", "old");
}

WHEN("^I export the selected file with overwrite enabled and com"
	"plete commitment$")
{
	[[maybe_unused]] World &w = world();

	w.status = pg_file_export(
		w.file, "output", PG_OVERWRITE, &w.error);
	expect_status(w.status, PG_OK, w.error);
}

THEN("^the native destination contains exactly \"new\"$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(read_native("output"), "new");
}

GIVEN("^the native replacement target is a ([^\"]*)$")
{
	[[maybe_unused]] World &w = world();
	REGEX_PARAM(std::string, kind);

	w.edit = kind;
	write_native("referent", "unchanged");
	if (kind == "directory") {
		std::filesystem::create_directories("output");
		write_native("output/child", "unchanged");
	} else if (kind == "symbolic link") {
		std::filesystem::create_symlink("referent", "output");
	} else if (kind == "special file") {
#ifndef _WIN32
		ASSERT_EQ(mkfifo("output", 0600), 0);
#endif
	} else {
#ifdef _WIN32
		std::filesystem::create_directories("linked-directory");
		write_native("linked-directory/child", "unchanged");
		ASSERT_EQ(system("cmd /c mklink /J output "
				"linked-directory >NUL"), 0);
#else
		std::filesystem::create_directories("linked-directory");
		write_native("linked-directory/child", "unchanged");
		std::filesystem::create_directory_symlink(
			"linked-directory", "output");
#endif
	}
	w.loose("loose", {{"a", "new"}});
	w.select("a", false);
}

WHEN("^I import, export, or pack a regular file at that target$")
{
	[[maybe_unused]] World &w = world();

	w.status = pg_file_export(
		w.file, "output", PG_OVERWRITE, &w.error);
}

THEN("^the result is a type\\-conflict error$")
{
	[[maybe_unused]] World &w = world();

	expect_status(w.status, PG_CONFLICT, w.error);
}

THEN("^the conflicting object and anything it refers to are unch"
	"anged$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_TRUE(std::filesystem::exists("output"));
	EXPECT_EQ(read_native("referent"), "unchanged");
	if (w.edit == "directory") {
		EXPECT_EQ(read_native("output/child"), "unchanged");
	} else if (w.edit == "Windows junction") {
		EXPECT_EQ(read_native("linked-directory/child"),
			"unchanged");
	}
}

GIVEN("^a destination contains \"old\"$")
{
	[[maybe_unused]] World &w = world();

	w.loose("loose", {{"a", "old"}});
}

GIVEN("^a replacement has staged \"new\" without commitment$")
{
	[[maybe_unused]] World &w = world();

	w.staged("a", 3);
	w.write("new");
}

WHEN("^I close the unfinished writer$")
{
	[[maybe_unused]] World &w = world();

	w.status = pg_writer_close(&w.writer, &w.error);
}

THEN("^the close succeeds$")
{
	[[maybe_unused]] World &w = world();

	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.writer, nullptr);
}

THEN("^the destination still contains \"old\"$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(read_native(w.paths.at("loose") + "/a"), "old");
}

THEN("^no staged copy is visible$")
{
	[[maybe_unused]] World &w = world();

	w.request(false);
	expect_status(w.status, PG_OK, w.error);
	w.list(false);
	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.names, std::vector<std::string>({"a"}));
	EXPECT_EQ(std::distance(std::filesystem::directory_iterator(
					w.paths.at("loose")),
				std::filesystem::directory_iterator()),
		1);
}

GIVEN("^a writer has accepted bytes but has not finished$")
{
	[[maybe_unused]] World &w = world();

	w.loose("loose", {{"a", "old"}});
	w.staged("a", 3);
	w.write("new");
}

WHEN("^I close it and complete cleanup$")
{
	[[maybe_unused]] World &w = world();

	expect_status(
		pg_writer_close(&w.writer, &w.error), PG_OK, w.error);
}

THEN("^the old destination is preserved$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(read_native(w.paths.at("loose") + "/a"), "old");
}

THEN("^no replacement is committed$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.writer, nullptr);
	w.select("a", false);
	EXPECT_EQ(w.read(w.file), "old");
}

GIVEN("^stored input declares compressed logical contents$")
{
	[[maybe_unused]] World &w = world();

	w.loose("loose", {{"a", "old"}});
	w.expected = "not zlib";
}

GIVEN("^its compressed stream is malformed or truncated$")
{
	[[maybe_unused]] World &w = world();

	pg_write_options options;

	pg_write_options_init(&options, 3);
	options.encoding = PG_ZLIB;
	options.input_size = w.expected.size();
	expect_status(pg_writer_open_source(w.source(), "a", &options,
				&w.writer, &w.error),
		PG_OK, w.error);
}

WHEN("^I import it and finish validation$")
{
	[[maybe_unused]] World &w = world();

	w.status = pg_writer_write(w.writer, w.expected.data(),
		w.expected.size(), &w.count, &w.error);
	if (w.status == PG_OK)
		w.status = pg_writer_finish(w.writer, &w.error);
	w.saved_error = w.error;
	expect_status(
		pg_writer_close(&w.writer, &w.error), PG_OK, w.error);
}

THEN("^the result is a corrupt\\-data error$")
{
	[[maybe_unused]] World &w = world();

	expect_status(w.status, PG_CORRUPT, w.saved_error);
}

GIVEN("^compressed input decodes but its expected digest is wrong"
	"$")
{
	[[maybe_unused]] World &w = world();

	w.loose("loose", {{"a", "old"}});
	unsigned char encoded[128];
	size_t size = sizeof(encoded);

	ASSERT_EQ(
		zng_compress2(encoded, &size,
			reinterpret_cast<const unsigned char *>("new"),
			3, 6),
		Z_OK);
	w.expected.assign(reinterpret_cast<char *>(encoded), size);
	pg_write_options options;

	pg_write_options_init(&options, 3);
	options.encoding = PG_ZLIB;
	options.input_size = size;
	options.entry.digest_kind = PG_DIGEST_MD5;
	options.entry.expected_digest[0] = 1;
	expect_status(pg_writer_open_source(w.source(), "a", &options,
				&w.writer, &w.error),
		PG_OK, w.error);
}

WHEN("^I import or export it and complete validation$")
{
	[[maybe_unused]] World &w = world();

	w.status = pg_writer_write(w.writer, w.expected.data(),
		w.expected.size(), &w.count, &w.error);
	if (w.status == PG_OK)
		w.status = pg_writer_finish(w.writer, &w.error);
	w.saved_error = w.error;
	expect_status(
		pg_writer_close(&w.writer, &w.error), PG_OK, w.error);
}

THEN("^the result is a checksum error$")
{
	[[maybe_unused]] World &w = world();

	expect_status(w.status, PG_CHECKSUM, w.saved_error);
}

GIVEN("^a selected archive file has a bad stored checksum$")
{
	[[maybe_unused]] World &w = world();

	raw_pigg("bad.pigg", {{"a", "bad bytes"}}, true);
	w.paths["archive"] = "bad.pigg";
	w.formats["archive"] = PG_PIGG2;
	w.open("archive");
	expect_status(w.status, PG_OK, w.error);
	w.select("a", false);
	expect_status(pg_reader_open(w.file, PG_READ_LOGICAL, &w.reader,
				&w.error),
		PG_OK, w.error);
}

WHEN("^I sequentially read the file$")
{
	[[maybe_unused]] World &w = world();

	while (true) {
		std::string bytes = w.chunk(w.reader, 2);

		w.bytes += bytes;
		if (w.status != PG_OK)
			break;
	}
}

THEN("^a prefix may be delivered before the checksum error$")
{
	[[maybe_unused]] World &w = world();

	expect_status(w.status, PG_CHECKSUM, w.error);
	EXPECT_GT(w.bytes.size(), 0u);
}

THEN("^reported byte counts describe exactly the delivered prefi"
	"x$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.bytes,
		std::string("bad bytes").substr(0, w.bytes.size()));
	uint64_t position;

	expect_status(pg_reader_tell(w.reader, &position, &w.error),
		PG_OK, w.error);
	EXPECT_EQ(position, w.bytes.size());
}

THEN("^successful bytes do not imply successful final verificati"
	"on$")
{
	[[maybe_unused]] World &w = world();

	expect_status(
		pg_file_verify(w.file, &w.error), PG_CHECKSUM, w.error);
	EXPECT_EQ(w.chunk(w.reader, 2), "");
	expect_status(w.status, PG_INVALID, w.error);
}

GIVEN("^an open sequential writer$")
{
	[[maybe_unused]] World &w = world();

	w.loose("loose", {{"a", "old contents"}});
	w.staged("a", 3);
	w.write("new");
}

THEN("^the writer provides no seek, positional I/O, or append$")
{
	[[maybe_unused]] World &w = world();

	// Sequential input cannot accept bytes beyond its declared
	// replacement length.
	w.status = pg_writer_write(
		w.writer, "extra", 5, &w.count, &w.error);
	expect_status(w.status, PG_INVALID, w.error);
	EXPECT_EQ(w.count, 0u);
}

THEN("^a write always describes a complete replacement$")
{
	[[maybe_unused]] World &w = world();

	w.finished();
	w.select("a", false);
	EXPECT_EQ(w.read(w.file), "new");
}

GIVEN("^a tree has visible files \"a\" and \"dir/b\" and losing copie"
	"s$")
{
	[[maybe_unused]] World &w = world();

	w.loose("base", {{"a", "loser"}, {"dir/b", "base-b"}});
	w.loose("patch", {{"a", "winner"}});
	std::filesystem::create_directories(
		w.paths.at("patch") + "/empty");
	w.attach("base");
	w.attach("patch");
	write_native("out/a", "old-a");
	write_native("out/dir/b", "old-b");
}

WHEN("^I pack its visible view and unpack with overwrite enabled"
	"$")
{
	[[maybe_unused]] World &w = world();

	expect_status(pg_tree_pack(w.tree, "packed", PG_PIGG2, nullptr,
				&w.error),
		PG_OK, w.error);
	w.paths["packed"] = "packed";
	w.formats["packed"] = PG_PIGG2;
	w.open("packed");
	expect_status(w.status, PG_OK, w.error);
	expect_status(pg_source_unpack(w.source(), "out", PG_OVERWRITE,
				&w.error),
		PG_OK, w.error);
}

THEN("^the output contains the visible contents at \"a\" and \"dir/"
	"b\"$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(read_native("out/a"), "winner");
	EXPECT_EQ(read_native("out/dir/b"), "base-b");
}

THEN("^losing copies and empty directories are not exported$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_FALSE(std::filesystem::exists("out/empty"));
	w.list(false);
	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.names, std::vector<std::string>({"a", "dir/b"}));
}

THEN("^each unpacked regular file replaces any existing regular "
	"file$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_NE(read_native("out/a"), "old-a");
	EXPECT_NE(read_native("out/dir/b"), "old-b");
}

GIVEN("^unpack has committed \"a\" and is staging \"b\"$")
{
	[[maybe_unused]] World &w = world();

	raw_pigg("partial.pigg", {{"a", "new-a"}, {"b", "bad-b"}});
	auto archive = read_native("partial.pigg");

	archive[16 + 48 + 28] = 1;
	write_native("partial.pigg", archive);
	w.paths["archive"] = "partial.pigg";
	w.formats["archive"] = PG_PIGG2;
	w.open("archive");
	expect_status(w.status, PG_OK, w.error);
	write_native("out/a", "old-a");
	write_native("out/b", "old-b");
}

WHEN("^processing \"b\" fails before publication$")
{
	[[maybe_unused]] World &w = world();

	w.status = pg_source_unpack(
		w.source(), "out", PG_OVERWRITE, &w.error);
}

THEN("^\"a\" remains replaced$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(read_native("out/a"), "new-a");
}

THEN("^the previous \"b\" remains unchanged$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(read_native("out/b"), "old-b");
}

THEN("^the call returns partial with the underlying cause$")
{
	[[maybe_unused]] World &w = world();

	expect_status(w.status, PG_PARTIAL, w.error);
	EXPECT_EQ(w.error.cause, PG_CHECKSUM);
}

GIVEN("^a selected logical file has three bytes including a zero "
	"byte$")
{
	[[maybe_unused]] World &w = world();

	w.expected = std::string("a\0b", 3);
	w.loose("loose", {{"a", w.expected}});
	w.select("a", false);
}

WHEN("^I request an allocated read with a two\\-byte limit$")
{
	[[maybe_unused]] World &w = world();

	w.status =
		pg_file_read_all_alloc(w.file, 2, &w.buffer, &w.error);
}

THEN("^the result is a limit error with an empty buffer$")
{
	[[maybe_unused]] World &w = world();

	expect_status(w.status, PG_LIMIT, w.error);
	EXPECT_EQ(w.buffer.data, nullptr);
	EXPECT_EQ(w.buffer.size, 0u);
}

WHEN("^I read with a three\\-byte limit$")
{
	[[maybe_unused]] World &w = world();

	w.status =
		pg_file_read_all_alloc(w.file, 3, &w.buffer, &w.error);
}

THEN("^the buffer is published only after verification and clean"
	"up$")
{
	[[maybe_unused]] World &w = world();

	expect_status(w.status, PG_OK, w.error);
	ASSERT_EQ(w.buffer.size, 3u);
	EXPECT_EQ(std::string(static_cast<char *>(w.buffer.data),
				w.buffer.size),
		w.expected);
}

THEN("^its three bytes remain owned after context close$")
{
	[[maybe_unused]] World &w = world();

	expect_status(pg_file_close(&w.file, &w.error), PG_OK, w.error);
	expect_status(pg_source_close(&w.sources.at("loose"), &w.error),
		PG_OK, w.error);
	expect_status(
		pg_context_close(&w.context, &w.error), PG_OK, w.error);
	EXPECT_EQ(std::string(static_cast<char *>(w.buffer.data),
				w.buffer.size),
		w.expected);
}

THEN("^freeing the buffer clears its pointer and length$")
{
	[[maybe_unused]] World &w = world();

	pg_buffer_free(&w.buffer);
	EXPECT_EQ(w.buffer.data, nullptr);
	EXPECT_EQ(w.buffer.size, 0u);
}

GIVEN("^an archive builder has staged several entries$")
{
	[[maybe_unused]] World &w = world();

	expect_status(pg_archive_builder_create(w.context, "built",
				PG_PIGG2, 0, &w.builder, &w.error),
		PG_OK, w.error);
	expect_status(pg_archive_builder_write_all(w.builder, "first",
				"first", 5, nullptr, &w.error),
		PG_OK, w.error);
	pg_write_options options;

	pg_write_options_init(&options, 6);
	expect_status(pg_writer_open_archive_builder(w.builder,
				"second", &options, &w.writer, &w.error),
		PG_OK, w.error);
	w.write("second");
}

WHEN("^an entry writer finishes successfully$")
{
	[[maybe_unused]] World &w = world();

	expect_status(
		pg_writer_finish(w.writer, &w.error), PG_OK, w.error);
}

THEN("^no external archive has been published$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_FALSE(std::filesystem::exists("built"));
}

THEN("^a live entry writer prevents builder finish or close$")
{
	[[maybe_unused]] World &w = world();

	pg_archive_builder *owned = w.builder;

	expect_status(pg_archive_builder_finish(w.builder, &w.error),
		PG_BUSY, w.error);
	expect_status(pg_archive_builder_close(&w.builder, &w.error),
		PG_BUSY, w.error);
	EXPECT_EQ(w.builder, owned);
}

WHEN("^I close the entry writer and finish the builder$")
{
	[[maybe_unused]] World &w = world();

	expect_status(
		pg_writer_close(&w.writer, &w.error), PG_OK, w.error);
	expect_status(pg_archive_builder_finish(w.builder, &w.error),
		PG_OK, w.error);
}

THEN("^the complete archive is published once$")
{
	[[maybe_unused]] World &w = world();

	w.paths["archive"] = "built";
	w.formats["archive"] = PG_PIGG2;
	w.open("archive");
	expect_status(w.status, PG_OK, w.error);
	w.list(false);
	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(
		w.names, std::vector<std::string>({"first", "second"}));
	expect_status(pg_source_validate(w.source(), &w.error), PG_OK,
		w.error);
}

THEN("^an accepted builder finish makes the builder close\\-only$")
{
	[[maybe_unused]] World &w = world();

	expect_status(pg_archive_builder_finish(w.builder, &w.error),
		PG_INVALID, w.error);
	expect_status(pg_archive_builder_write_all(w.builder, "late",
				"x", 1, nullptr, &w.error),
		PG_INVALID, w.error);
	expect_status(pg_archive_builder_close(&w.builder, &w.error),
		PG_OK, w.error);
}

GIVEN("^a regular native output already exists$")
{
	[[maybe_unused]] World &w = world();

	w.loose("loose", {{"a", "new"}});
	w.select("a", false);
	write_native("output", "old");
}

WHEN("^I export or create a native writer without overwrite$")
{
	[[maybe_unused]] World &w = world();

	w.status = pg_file_export(w.file, "output", 0, &w.error);
}

THEN("^the result is an exists error and the original is unchang"
	"ed$")
{
	[[maybe_unused]] World &w = world();

	expect_status(w.status, PG_EXISTS, w.error);
	EXPECT_EQ(read_native("output"), "old");
}

WHEN("^I explicitly enable overwrite and finish successfully$")
{
	[[maybe_unused]] World &w = world();

	expect_status(pg_file_export(
				w.file, "output", PG_OVERWRITE, &w.error),
		PG_OK, w.error);
}

THEN("^the regular output is replaced$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(read_native("output"), "new");
}

THEN("^links and special objects remain conflicts$")
{
	[[maybe_unused]] World &w = world();

	std::filesystem::create_directory("directory");
	expect_status(pg_file_export(w.file, "directory", PG_OVERWRITE,
				&w.error),
		PG_CONFLICT, w.error);
#ifndef _WIN32
	std::filesystem::create_symlink("output", "link");
	expect_status(
		pg_file_export(w.file, "link", PG_OVERWRITE, &w.error),
		PG_CONFLICT, w.error);
	ASSERT_EQ(mkfifo("fifo", 0600), 0);
	expect_status(
		pg_file_export(w.file, "fifo", PG_OVERWRITE, &w.error),
		PG_CONFLICT, w.error);
#endif
}

GIVEN("^an archive builder has staged an entry named \"Dir/A\"$")
{
	[[maybe_unused]] World &w = world();

	expect_status(pg_archive_builder_create(w.context, "built",
				PG_PIGG2, 0, &w.builder, &w.error),
		PG_OK, w.error);
	expect_status(pg_archive_builder_write_all(w.builder, "Dir/A",
				"first", 5, nullptr, &w.error),
		PG_OK, w.error);
}

WHEN("^I try to stage another entry named \"dir/a\"$")
{
	[[maybe_unused]] World &w = world();

	w.status = pg_archive_builder_write_all(
		w.builder, "dir/a", "second", 6, nullptr, &w.error);
}

THEN("^the second entry returns an exists error$")
{
	[[maybe_unused]] World &w = world();

	expect_status(w.status, PG_EXISTS, w.error);
}

THEN("^the first staged entry remains available for builder fini"
	"sh$")
{
	[[maybe_unused]] World &w = world();

	expect_status(pg_archive_builder_finish(w.builder, &w.error),
		PG_OK, w.error);
	expect_status(pg_archive_builder_close(&w.builder, &w.error),
		PG_OK, w.error);
	w.paths["archive"] = "built";
	w.formats["archive"] = PG_PIGG2;
	w.open("archive");
	expect_status(w.status, PG_OK, w.error);
	w.select("dir/a", false);
	EXPECT_EQ(w.read(w.file), "first");
}

GIVEN("^independent readers of native, PIGG and HOGG files$")
{
	[[maybe_unused]] World &w = world();

	w.expected = "abcdefgh";
	write_native("input", w.expected);
	pg_reader *input = nullptr;

	expect_status(pg_reader_open_native(
				w.context, "input", &input, &w.error),
		PG_OK, w.error);
	w.readers.push_back(input);
	for (uint32_t format : {PG_PIGG2, PG_HOGG10}) {
		std::string label =
			format == PG_PIGG2 ? "pigg" : "hogg";

		w.archive(label, {{"a", w.expected}}, format);
		w.select("a", false);
		expect_status(pg_reader_open(w.file, PG_READ_LOGICAL,
					&input, &w.error),
			PG_OK, w.error);
		w.readers.push_back(input);
	}
	expect_status(pg_reader_open_native(w.context, "input",
				&w.other_reader, &w.error),
		PG_OK, w.error);
}

WHEN("^I seek forward and backward within each represented size$")
{
	[[maybe_unused]] World &w = world();

	for (auto reader : w.readers) {
		for (uint64_t offset : {6u, 1u, 7u, 0u}) {
			expect_status(pg_reader_seek(
						reader, offset, &w.error),
				PG_OK, w.error);
			uint64_t position = 0;

			expect_status(pg_reader_tell(reader, &position,
						&w.error),
				PG_OK, w.error);
			EXPECT_EQ(position, offset);
			EXPECT_EQ(w.chunk(reader, 1),
				w.expected.substr(offset, 1));
			expect_status(w.status, PG_OK, w.error);
		}
	}
}

THEN("^tell reports the requested offset and reads return its ex"
	"act bytes$")
{
	[[maybe_unused]] World &w = world();

	for (auto reader : w.readers) {
		uint64_t position = 0;

		expect_status(
			pg_reader_tell(reader, &position, &w.error),
			PG_OK, w.error);
		EXPECT_EQ(position, 1u);
		EXPECT_EQ(w.chunk(reader, 3), "bcd");
	}
}

THEN("^seeking one reader does not move another reader$")
{
	[[maybe_unused]] World &w = world();

	uint64_t position = 1;

	expect_status(
		pg_reader_tell(w.other_reader, &position, &w.error),
		PG_OK, w.error);
	EXPECT_EQ(position, 0u);
	EXPECT_EQ(w.chunk(w.other_reader, 3), "abc");
}

THEN("^an out\\-of\\-range seek fails without moving the reader$")
{
	[[maybe_unused]] World &w = world();

	for (auto reader : w.readers) {
		expect_status(pg_reader_seek(reader, 9, &w.error),
			PG_INVALID, w.error);
		uint64_t position = 0;

		expect_status(
			pg_reader_tell(reader, &position, &w.error),
			PG_OK, w.error);
		EXPECT_EQ(position, 4u);
	}
}

GIVEN("^an archive reader with a bad stored checksum$")
{
	[[maybe_unused]] World &w = world();

	raw_pigg("bad.pigg", {{"a", "bad bytes"}}, true);
	w.paths["archive"] = "bad.pigg";
	w.formats["archive"] = PG_PIGG2;
	w.open("archive");
	expect_status(w.status, PG_OK, w.error);
	w.select("a", false);
	expect_status(pg_reader_open(w.file, PG_READ_LOGICAL, &w.reader,
				&w.error),
		PG_OK, w.error);
}

WHEN("^I seek to EOF and perform a positive\\-capacity read$")
{
	[[maybe_unused]] World &w = world();

	expect_status(
		pg_reader_seek(w.reader, 9, &w.error), PG_OK, w.error);
	w.chunk(w.reader, 1);
}

THEN("^the read reports the checksum error$")
{
	[[maybe_unused]] World &w = world();

	expect_status(w.status, PG_CHECKSUM, w.error);
	EXPECT_EQ(w.count, 0u);
}

THEN("^seeking does not make a stale physical selection valid ag"
	"ain$")
{
	[[maybe_unused]] World &w = world();

	expect_status(
		pg_reader_close(&w.reader, &w.error), PG_OK, w.error);
	replace_native(w.paths.at("archive"), "external");
	w.status = pg_reader_open(
		w.file, PG_READ_LOGICAL, &w.reader, &w.error);
	expect_status(w.status, PG_STALE, w.error);
	EXPECT_EQ(w.reader, nullptr);
}

GIVEN("^a compressed logical reader spanning more than 64 MiB$")
{
	[[maybe_unused]] World &w = world();

	w.expected.resize(66 * 1024 * 1024 + 17);
	for (size_t i = 0; i < w.expected.size(); ++i)
		w.expected[i] = static_cast<char>(i % 251);
	expect_status(pg_archive_builder_create(w.context, "large.pigg",
				PG_PIGG2, 0, &w.builder, &w.error),
		PG_OK, w.error);
	pg_entry_options entry = {};

	entry.compression = PG_COMPRESS_FORCE;
	expect_status(pg_archive_builder_write_all(w.builder, "large",
				w.expected.data(), w.expected.size(),
				&entry, &w.error),
		PG_OK, w.error);
	expect_status(pg_archive_builder_finish(w.builder, &w.error),
		PG_OK, w.error);
	expect_status(pg_archive_builder_close(&w.builder, &w.error),
		PG_OK, w.error);
	w.paths["archive"] = "large.pigg";
	w.formats["archive"] = PG_PIGG2;
	w.open("archive");
	expect_status(w.status, PG_OK, w.error);
	w.select("large", false);
	expect_status(pg_reader_open(w.file, PG_READ_LOGICAL, &w.reader,
				&w.error),
		PG_OK, w.error);
}

WHEN("^I alternate backward and forward seeks across checkpoint "
	"boundaries$")
{
	[[maybe_unused]] World &w = world();

	for (uint64_t offset : {65u * 1024 * 1024, 1024u * 1024,
			64u * 1024 * 1024 + 7, 0u, 33u * 1024 * 1024}) {
		expect_status(
			pg_reader_seek(w.reader, offset, &w.error),
			PG_OK, w.error);
		auto bytes = w.chunk(w.reader, 127);

		expect_status(w.status, PG_OK, w.error);
		EXPECT_EQ(
			bytes, w.expected.substr(offset, bytes.size()));
		w.bytes += bytes;
	}
}

THEN("^every returned byte matches the selected logical content$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.bytes.size(), 5u * 127);
}

THEN("^the reader retains at most 64 private decoder checkpoints"
	"$")
{
	[[maybe_unused]] World &w = world();

	// Exercise the documented cache boundary and replay older data
	// through the public API without exposing decoder internals.
	for (unsigned i = 0; i < 66; ++i) {
		uint64_t offset =
			static_cast<uint64_t>(i) * 1024 * 1024;

		expect_status(
			pg_reader_seek(w.reader, offset, &w.error),
			PG_OK, w.error);
		EXPECT_EQ(w.chunk(w.reader, 1),
			w.expected.substr(offset, 1));
	}
	expect_status(
		pg_reader_seek(w.reader, 0, &w.error), PG_OK, w.error);
	EXPECT_EQ(w.chunk(w.reader, 1), w.expected.substr(0, 1));
}

GIVEN("^a HOGG uses the explicit stored checksum profile$")
{
	[[maybe_unused]] World &w = world();

	pg_archive_options create = {PG_HOGG10, 0, PG_CHECKSUM_STORED};
	pg_source_options open = {
		PG_HOGG10, PG_WRITE, PG_CHECKSUM_STORED};
	pg_entry_options entry = {};

	w.expected = "mission payload mission payload mission payload";
	entry.compression = PG_COMPRESS_FORCE;
	expect_status(
		pg_archive_builder_create_options(w.context,
			"mission.hogg", &create, &w.builder, &w.error),
		PG_OK, w.error);
	expect_status(pg_archive_builder_write_all(w.builder, "a",
				w.expected.data(), w.expected.size(),
				&entry, &w.error),
		PG_OK, w.error);
	expect_status(pg_archive_builder_finish(w.builder, &w.error),
		PG_OK, w.error);
	expect_status(pg_archive_builder_close(&w.builder, &w.error),
		PG_OK, w.error);
	w.paths["archive"] = "mission.hogg";
	w.formats["archive"] = PG_HOGG10;
	expect_status(pg_source_open(w.context, "mission.hogg", &open,
				&w.sources["archive"], &w.error),
		PG_OK, w.error);
	w.active = "archive";
	w.select("a", false);
	expect_status(pg_file_inspect(w.file, &w.captured, &w.error),
		PG_OK, w.error);
}

WHEN("^I read logical or stored bytes through verified EOF$")
{
	[[maybe_unused]] World &w = world();

	for (uint32_t mode : {PG_READ_LOGICAL, PG_READ_STORED}) {
		expect_status(pg_reader_open(w.file, mode, &w.reader,
					&w.error),
			PG_OK, w.error);
		std::string bytes;

		while (true) {
			bytes += w.chunk(w.reader, 7);
			if (w.status == PG_END)
				break;
			expect_status(w.status, PG_OK, w.error);
		}
		if (mode == PG_READ_LOGICAL) {
			EXPECT_EQ(bytes, w.expected);
		} else {
			w.previous = bytes;
		}
		expect_status(pg_reader_close(&w.reader, &w.error),
			PG_OK, w.error);
	}
}

THEN("^its stored checksum and logical encoding are verified$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.captured.checksum_domain, PG_CHECKSUM_STORED);
	EXPECT_EQ(w.captured.encoding, PG_ZLIB);
	expect_status(pg_file_verify(w.file, &w.error), PG_OK, w.error);
	EXPECT_LT(w.previous.size(), w.expected.size());
}

THEN("^copying to a logical profile recomputes the destination d"
	"igest$")
{
	[[maybe_unused]] World &w = world();

	expect_status(
		pg_archive_builder_create(w.context, "logical.pigg",
			PG_PIGG2, 0, &w.builder, &w.error),
		PG_OK, w.error);
	expect_status(pg_archive_builder_copy(w.builder, "a", w.file,
				PG_COMPRESS_FORCE, &w.error),
		PG_OK, w.error);
	expect_status(pg_archive_builder_finish(w.builder, &w.error),
		PG_OK, w.error);
	expect_status(pg_archive_builder_close(&w.builder, &w.error),
		PG_OK, w.error);
	pg_source *logical = nullptr;
	pg_file *copy = nullptr;
	pg_file_info info;

	expect_status(pg_source_open(w.context, "logical.pigg", nullptr,
				&logical, &w.error),
		PG_OK, w.error);
	expect_status(pg_source_find(logical, "a", &copy, &w.error),
		PG_OK, w.error);
	expect_status(
		pg_file_inspect(copy, &info, &w.error), PG_OK, w.error);
	EXPECT_EQ(info.checksum_domain, PG_CHECKSUM_LOGICAL);
	EXPECT_NE(memcmp(info.digest, w.captured.digest, 4), 0);
	EXPECT_EQ(w.read(copy), w.expected);
	expect_status(
		pg_source_validate(logical, &w.error), PG_OK, w.error);
}

THEN("^opening with the wrong profile never accepts the alternat"
	"e digest$")
{
	[[maybe_unused]] World &w = world();

	expect_status(pg_file_close(&w.file, &w.error), PG_OK, w.error);
	expect_status(
		pg_source_close(&w.sources.at("archive"), &w.error),
		PG_OK, w.error);
	pg_source *wrong = nullptr;

	expect_status(pg_source_open(w.context, "mission.hogg", nullptr,
				&wrong, &w.error),
		PG_OK, w.error);
	expect_status(pg_source_validate(wrong, &w.error), PG_CHECKSUM,
		w.error);
}

GIVEN("^a reader has captured a file on Windows$")
{
	[[maybe_unused]] World &w = world();

#ifdef _WIN32
	w.loose("loose", {{"a", "old"}});
	set_windows_time(
		w.paths.at("loose") + "/a", 133000000001000000ULL);
	w.select("a", false);
	w.retain_selection();
	expect_status(pg_reader_open(w.held, PG_READ_LOGICAL, &w.reader,
				&w.error),
		PG_OK, w.error);
#endif
}

WHEN("^its bytes change without changing size or whole\\-second m"
	"time$")
{
	[[maybe_unused]] World &w = world();

	write_native(w.paths.at("loose") + "/a", "new");
}

WHEN("^its native subsecond modification time changes$")
{
	[[maybe_unused]] World &w = world();

#ifdef _WIN32
	set_windows_time(
		w.paths.at("loose") + "/a", 133000000002000000ULL);
#endif
}

THEN("^the reader reports stale$")
{
	[[maybe_unused]] World &w = world();

	w.chunk(w.reader, 3);
	expect_status(w.status, PG_STALE, w.error);
	EXPECT_EQ(w.count, 0u);
}

THEN("^a source rescan advances the copy generation$")
{
	[[maybe_unused]] World &w = world();

	expect_status(
		pg_source_rescan(w.source(), &w.error), PG_OK, w.error);
	w.select("a", false);
	pg_file_info info;

	expect_status(pg_file_inspect(w.file, &info, &w.error), PG_OK,
		w.error);
	EXPECT_GT(info.copy_generation, w.captured.copy_generation);
	EXPECT_EQ(info.mtime, w.captured.mtime);
}

THEN("^public timestamps remain whole Unix seconds$")
{
	[[maybe_unused]] World &w = world();

	pg_file_info info;

	expect_status(pg_file_inspect(w.file, &info, &w.error), PG_OK,
		w.error);
	EXPECT_EQ(info.mtime, 1655526400);
}

} // namespace

GIVEN("^I stage malformed stored bytes with a supplied archive digest$")
{
	World &w = world();
	pg_stored_write_options options = {};

	expect_status(pg_archive_builder_create(w.context, "stored.hogg",
		PG_HOGG10, 0, &w.builder, &w.error), PG_OK, w.error);
	options.logical_size = 100;
	options.stored_size = 3;
	options.encoding = PG_ZLIB;
	options.digest_kind = PG_DIGEST_MD5_32;
	options.digest[0] = 42;
	expect_status(pg_writer_open_archive_builder_stored(w.builder, "a",
		&options, &w.writer, &w.error), PG_OK, w.error);
	w.write("bad");
	w.finished();
}

WHEN("^I publish the stored entry and read it unchecked$")
{
	World &w = world();

	expect_status(pg_archive_builder_finish(w.builder, &w.error),
		PG_OK, w.error);
	expect_status(pg_archive_builder_close(&w.builder, &w.error),
		PG_OK, w.error);
	pg_source *source = NULL;

	expect_status(pg_source_open(w.context, "stored.hogg", NULL,
		&source, &w.error), PG_OK, w.error);
	expect_status(pg_source_find(source, "a", &w.file, &w.error),
		PG_OK, w.error);
	expect_status(pg_source_close(&source, &w.error), PG_OK, w.error);
	expect_status(pg_reader_open(w.file, PG_READ_STORED_UNCHECKED,
		&w.reader, &w.error), PG_OK, w.error);
}

THEN("^the stored bytes and supplied metadata are preserved$")
{
	World &w = world();
	pg_file_info info;

	EXPECT_EQ(w.chunk(w.reader, 8), "bad");
	EXPECT_EQ(w.chunk(w.reader, 8), "");
	expect_status(w.status, PG_END, w.error);
	expect_status(pg_file_inspect(w.file, &info, &w.error),
		PG_OK, w.error);
	EXPECT_EQ(info.digest[0], 42);
	EXPECT_EQ(info.logical_size, 100u);
	EXPECT_EQ(info.stored_size, 3u);
}

THEN("^a checked stored reader rejects the malformed payload$")
{
	World &w = world();

	expect_status(pg_reader_close(&w.reader, &w.error), PG_OK, w.error);
	expect_status(pg_reader_open(w.file, PG_READ_STORED,
		&w.reader, &w.error), PG_OK, w.error);
	w.chunk(w.reader, 8);
	expect_status(w.status, PG_CORRUPT, w.error);
}
