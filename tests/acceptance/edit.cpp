#include "world.hpp"

namespace
{

World &world()
{
	WorldScope scope;

	return *scope;
}

GIVEN("^\"patch\" wins \"a\" over \"base\"$")
{
	[[maybe_unused]] World &w = world();

	w.loose("base", {{"a", "base"}});
	w.loose("patch", {{"a", "patch"}});
	w.attach("base");
	w.attach("patch");
	w.select();
	w.retain_selection();
}

WHEN("^I delete the selected patch copy and complete commitment$")
{
	[[maybe_unused]] World &w = world();

	w.status = pg_file_delete(w.held, &w.error);
	expect_status(w.status, PG_OK, w.error);
}

THEN("^\"base\" becomes visible as \"a\"$")
{
	[[maybe_unused]] World &w = world();

	w.select();
	EXPECT_EQ(w.read(w.file), "base");
}

THEN("^only the selected physical copy has been deleted$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_FALSE(
		std::filesystem::exists(w.paths.at("patch") + "/a"));
	EXPECT_EQ(read_native(w.paths.at("base") + "/a"), "base");
}

THEN("^lookup no longer selects the deleted copy$")
{
	[[maybe_unused]] World &w = world();

	pg_file_info info;

	expect_status(pg_file_inspect(w.file, &info, &w.error), PG_OK,
		w.error);
	EXPECT_NE(info.source_id, w.captured.source_id);
}

GIVEN("^one archive contains \"A\" as record 1 and \"a\" as record 2$")
{
	[[maybe_unused]] World &w = world();

	w.archive("archive", {{"A", "first"}, {"a", "second"}},
		PG_PIGG2, true);
}

WHEN("^I select and delete record 2$")
{
	[[maybe_unused]] World &w = world();

	w.select("a", false);
	pg_file_info info;

	expect_status(pg_file_inspect(w.file, &info, &w.error), PG_OK,
		w.error);
	EXPECT_EQ(info.archive_record, 1u);
	expect_status(pg_file_delete(w.file, &w.error), PG_OK, w.error);
}

THEN("^record 1 becomes the selected copy of \"a\"$")
{
	[[maybe_unused]] World &w = world();

	w.select("a", false);
	EXPECT_EQ(w.read(w.file), "first");
}

THEN("^no public cursor exposes both records$")
{
	[[maybe_unused]] World &w = world();

	w.list(false);
	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.names, std::vector<std::string>({"a"}));
}

GIVEN("^an open detached ([^\"]*) source with no active transfers$")
{
	[[maybe_unused]] World &w = world();
	REGEX_PARAM(std::string, kind);

	if (kind == "archive")
		w.archive("archive", {{"a", "old"}});
	else
		w.loose("loose", {{"a", "old"}, {"dir/b", "b"}});
	w.select("a", false);
	w.retain_selection();
}

WHEN("^I explicitly delete that source$")
{
	[[maybe_unused]] World &w = world();

	w.status = pg_source_delete(w.source(), &w.error);
	expect_status(w.status, PG_OK, w.error);
}

THEN("^its ([^\"]*) is removed$")
{
	[[maybe_unused]] World &w = world();
	REGEX_PARAM(std::string, data);

	EXPECT_FALSE(std::filesystem::exists(w.paths.at(w.active)))
		<< data;
}

THEN("^retained metadata handles become stale for content access"
	"$")
{
	[[maybe_unused]] World &w = world();

	pg_file_info info;

	expect_status(pg_file_inspect(w.held, &info, &w.error), PG_OK,
		w.error);
	EXPECT_EQ(info.copy_generation, w.captured.copy_generation);
	w.status = pg_reader_open(
		w.held, PG_READ_LOGICAL, &w.reader, &w.error);
	expect_status(w.status, PG_STALE, w.error);
	EXPECT_EQ(w.reader, nullptr);
}

GIVEN("^a writable HOGG contains an old selected copy$")
{
	[[maybe_unused]] World &w = world();

	w.archive("archive", {{"a", "old"}, {"other", "unchanged"}},
		PG_HOGG10);
	w.select("a", false);
	w.retain_selection();
	w.select("other", false);
	expect_status(pg_file_inspect(w.file, &w.captured, &w.error),
		PG_OK, w.error);
}

WHEN("^I replace it with complete validated content$")
{
	[[maybe_unused]] World &w = world();

	w.status = pg_source_write_all(
		w.source(), "a", "replacement", 11, nullptr, &w.error);
	expect_status(w.status, PG_OK, w.error);
}

THEN("^payload preparation precedes the journal commitment$")
{
	[[maybe_unused]] World &w = world();

	expect_status(pg_source_validate(w.source(), &w.error), PG_OK,
		w.error);
	auto bytes = read_native(w.paths.at("archive"));

	EXPECT_EQ(get_le32(bytes, 24), 0u);
	EXPECT_NE(bytes.find("replacement"), std::string::npos);
}

THEN("^the intended record is updated through HOGG mutation faci"
	"lities$")
{
	[[maybe_unused]] World &w = world();

	w.select("a", false);
	EXPECT_EQ(w.read(w.file), "replacement");
}

THEN("^unrelated records and their metadata are preserved$")
{
	[[maybe_unused]] World &w = world();

	w.select("other", false);
	pg_file_info info;

	expect_status(pg_file_inspect(w.file, &info, &w.error), PG_OK,
		w.error);
	EXPECT_EQ(info.copy_id, w.captured.copy_id);
	EXPECT_EQ(info.copy_generation, w.captured.copy_generation);
	EXPECT_EQ(info.mtime, w.captured.mtime);
	EXPECT_EQ(w.read(w.file), "unchanged");
}

GIVEN("^a HOGG replacement has a durably committed journal frame$")
{
	[[maybe_unused]] World &w = world();

	w.archive("archive", {{"a", "old"}}, PG_HOGG10);
	w.attach("archive");
	w.request();
	expect_status(w.status, PG_OK, w.error);
	w.watch(PG_WATCH_SCAN);
	w.select();
	w.retain_selection();
	w.staged("a", 3);
	w.write("new");
}

WHEN("^applying that frame fails$")
{
	[[maybe_unused]] World &w = world();

#ifdef __linux__
	pg_test_journal_fault(3);
	w.status = pg_writer_finish(w.writer, &w.error);
	w.saved_error = w.error;
	pg_test_journal_fault(0);
#endif
}

THEN("^the call returns recovery\\-required and records the cause"
	"$")
{
	[[maybe_unused]] World &w = world();

	expect_status(w.status, PG_RECOVERY_REQUIRED, w.saved_error);
	EXPECT_EQ(w.saved_error.cause, PG_IO);
	EXPECT_EQ(w.saved_error.native_code, EIO);
}

THEN("^new content access is blocked until recovery completes$")
{
	[[maybe_unused]] World &w = world();

	expect_status(
		pg_writer_close(&w.writer, &w.error), PG_OK, w.error);
	w.status = pg_reader_open(
		w.held, PG_READ_LOGICAL, &w.reader, &w.error);
	expect_status(w.status, PG_RECOVERY_REQUIRED, w.error);
	EXPECT_EQ(w.reader, nullptr);
}

WHEN("^I recover the source$")
{
	[[maybe_unused]] World &w = world();

	w.status = pg_source_recover(
		w.context, w.paths.at("archive").c_str(), &w.error);
	expect_status(w.status, PG_OK, w.error);
}

THEN("^the committed frame is replayed idempotently$")
{
	[[maybe_unused]] World &w = world();

	expect_status(pg_source_recover(w.context,
				w.paths.at("archive").c_str(), &w.error),
		PG_OK, w.error);
	w.select();
	EXPECT_EQ(w.read(w.file), "new");
	expect_status(pg_source_validate(w.source(), &w.error), PG_OK,
		w.error);
}

THEN("^the tree publishes the recovered state before its callbac"
	"ks$")
{
	[[maybe_unused]] World &w = world();

	w.callback_lookup = true;
	w.poll();
	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.reports(PG_CHANGE_UPDATE, "a"), 1u);
	EXPECT_EQ(w.callback_status, PG_OK);
	EXPECT_EQ(w.callback_bytes, "new");
}

GIVEN("^a HOGG has a valid committed but unapplied journal frame$")
{
	[[maybe_unused]] World &w = world();

	w.archive("archive", {{"a", "old"}}, PG_HOGG10);
	w.select("a", false);
	expect_status(pg_file_inspect(w.file, &w.captured, &w.error),
		PG_OK, w.error);
	expect_status(pg_file_close(&w.file, &w.error), PG_OK, w.error);
	expect_status(
		pg_source_close(&w.sources.at("archive"), &w.error),
		PG_OK, w.error);
	commit_timestamp(
		w.paths.at("archive"), w.captured.archive_record, 123);
}

WHEN("^I open it for reading without recovery permission$")
{
	[[maybe_unused]] World &w = world();

	w.open("archive", PG_READ);
}

THEN("^the result is recovery\\-required with no usable source ou"
	"tput$")
{
	[[maybe_unused]] World &w = world();

	expect_status(w.status, PG_RECOVERY_REQUIRED, w.error);
	EXPECT_EQ(w.sources.at("archive"), nullptr);
}

GIVEN("^a PIGG has colliding records and opaque unaffected metada"
	"ta$")
{
	[[maybe_unused]] World &w = world();

	w.archive("archive",
		{{"A", "first"}, {"a", "second"},
			{"other", "unchanged"}},
		PG_PIGG2, true);
	w.previous = read_native(w.paths.at("archive"));
	w.select("other", false);
	w.retain_selection();
}

WHEN("^I request a complete ([^\"]*)$")
{
	[[maybe_unused]] World &w = world();
	REGEX_PARAM(std::string, edit);

	w.edit = edit;
	if (edit == "deletion") {
		w.select("a", false);
		w.status = pg_file_delete(w.file, &w.error);
	} else {
		w.status = pg_source_write_all(w.source(),
			edit == "addition" ? "new" : "a", "changed", 7,
			nullptr, &w.error);
	}
	expect_status(w.status, PG_OK, w.error);
}

THEN("^a clone contains the intended additions, replacements, or"
	" exclusions$")
{
	[[maybe_unused]] World &w = world();

	w.select(w.edit == "addition" ? "new" : "a", false);
	EXPECT_EQ(w.read(w.file),
		w.edit == "deletion" ? "first" : "changed");
}

THEN("^unaffected records, payload representations, and metadata"
	" survive$")
{
	[[maybe_unused]] World &w = world();

	w.select("other", false);
	pg_file_info info;

	expect_status(pg_file_inspect(w.file, &info, &w.error), PG_OK,
		w.error);
	EXPECT_EQ(info.logical_size, w.captured.logical_size);
	EXPECT_EQ(info.encoding, w.captured.encoding);
	EXPECT_EQ(info.mtime, w.captured.mtime);
	EXPECT_EQ(memcmp(info.digest, w.captured.digest, 16), 0);
	EXPECT_EQ(w.read(w.file), "unchanged");
	std::string bytes = read_native(w.paths.at("archive"));
	size_t record = 16 + info.archive_record * 48;

	EXPECT_EQ(get_le32(bytes, record + 20), 0xfeed1234u);
}

THEN("^the clone is fully validated before it replaces the origi"
	"nal$")
{
	[[maybe_unused]] World &w = world();

	expect_status(pg_source_validate(w.source(), &w.error), PG_OK,
		w.error);
	EXPECT_NE(read_native(w.paths.at("archive")), w.previous);
}

GIVEN("^a PIGG edit has constructed a clone$")
{
	[[maybe_unused]] World &w = world();

	raw_pigg("archive.pigg", {{"bad", "corrupt"}, {"a", "old"}},
		true);
	w.paths["archive"] = "archive.pigg";
	w.formats["archive"] = PG_PIGG2;
	w.open("archive");
	expect_status(w.status, PG_OK, w.error);
	w.previous = read_native("archive.pigg");
}

WHEN("^clone validation detects an invalid record or checksum$")
{
	[[maybe_unused]] World &w = world();

	w.status = pg_source_write_all(
		w.source(), "a", "new", 3, nullptr, &w.error);
}

THEN("^the call fails before publication$")
{
	[[maybe_unused]] World &w = world();

	expect_status(w.status, PG_CHECKSUM, w.error);
}

THEN("^the original archive is byte\\-for\\-byte unchanged$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(read_native(w.paths.at("archive")), w.previous);
}

THEN("^cleanup removes the unpublished clone$")
{
	[[maybe_unused]] World &w = world();

	size_t files = 0;

	for (const auto &entry :
		std::filesystem::directory_iterator(".")) {
		EXPECT_EQ(entry.path().filename().string(),
			"archive.pigg");
		++files;
	}
	EXPECT_EQ(files, 1u);
}

GIVEN("^a validated clone has replaced its native archive path$")
{
	[[maybe_unused]] World &w = world();

	w.archive("archive", {{"a", "old"}});
	w.previous = read_native(w.paths.at("archive"));
	w.staged("a", 3);
	w.write("new");
}

WHEN("^a subsequent durability or cleanup operation fails$")
{
	[[maybe_unused]] World &w = world();

#ifdef __linux__
	pg_test_journal_fault(6);
	w.status = pg_writer_finish(w.writer, &w.error);
	w.saved_error = w.error;
	pg_test_journal_fault(0);
#endif
}

THEN("^the call returns committed with the failure in error\\.cau"
	"se$")
{
	[[maybe_unused]] World &w = world();

	expect_status(w.status, PG_COMMITTED, w.saved_error);
	EXPECT_EQ(w.saved_error.cause, PG_IO);
	EXPECT_EQ(w.saved_error.native_code, EIO);
}

THEN("^it does not claim that the old archive was preserved$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_NE(read_native(w.paths.at("archive")), w.previous);
	expect_status(
		pg_writer_close(&w.writer, &w.error), PG_OK, w.error);
	expect_status(
		pg_source_rescan(w.source(), &w.error), PG_OK, w.error);
	w.select("a", false);
	EXPECT_EQ(w.read(w.file), "new");
}

GIVEN("^an import captured the destination generation$")
{
	[[maybe_unused]] World &w = world();

	w.loose("loose", {{"a", "old"}});
	w.staged("a", 3);
	w.write("new");
}

WHEN("^an uncoordinated external tool changes that destination$")
{
	[[maybe_unused]] World &w = world();

	replace_native(w.paths.at("loose") + "/a", "external");
}

WHEN("^the change is detected before commitment$")
{
	[[maybe_unused]] World &w = world();

	w.status = pg_writer_finish(w.writer, &w.error);
}

THEN("^the import fails as stale or retryable$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_TRUE(w.status == PG_STALE || w.status == PG_RETRY);
	EXPECT_EQ(w.error.status, w.status);
}

THEN("^the external edit remains published after reconciliation$")
{
	[[maybe_unused]] World &w = world();

	expect_status(
		pg_writer_close(&w.writer, &w.error), PG_OK, w.error);
	expect_status(
		pg_source_rescan(w.source(), &w.error), PG_OK, w.error);
	w.select("a", false);
	EXPECT_EQ(w.read(w.file), "external");
}

GIVEN("^a HOGG reader is open on an unchanged entry$")
{
	[[maybe_unused]] World &w = world();

	w.archive("archive", {{"a", "old"}}, PG_HOGG10);
	expect_status(pg_reader_open_source(w.source(), "a",
				PG_READ_LOGICAL, &w.reader, &w.error),
		PG_OK, w.error);
	w.previous = read_native(w.paths.at("archive"));
	w.staged("staged", 1);
}

WHEN("^a conflicting mutation is rejected as busy$")
{
	[[maybe_unused]] World &w = world();

	w.status = pg_source_write_all(
		w.source(), "conflicting", "new", 3, nullptr, &w.error);
	expect_status(w.status, PG_BUSY, w.error);
}

THEN("^the archive bytes remain unchanged$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(read_native(w.paths.at("archive")), w.previous);
}

THEN("^the reader remains usable$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.chunk(w.reader, 16), "old");
	expect_status(w.status, PG_OK, w.error);
}

THEN("^releasing the conflicting writer lease permits another wr"
	"ite$")
{
	[[maybe_unused]] World &w = world();

	expect_status(
		pg_writer_close(&w.writer, &w.error), PG_OK, w.error);
	expect_status(pg_source_write_all(w.source(), "conflicting",
				"new", 3, nullptr, &w.error),
		PG_OK, w.error);
}

THEN("^no explicit rescan is required$")
{
	[[maybe_unused]] World &w = world();

	w.select("conflicting", false);
	EXPECT_EQ(w.read(w.file), "new");
	expect_status(
		pg_reader_seek(w.reader, 0, &w.error), PG_OK, w.error);
	EXPECT_EQ(w.chunk(w.reader, 16), "old");
}

GIVEN("^a reader has selected one HOGG entry$")
{
	[[maybe_unused]] World &w = world();

	w.archive("archive", {{"a", "old"}}, PG_HOGG10);
	w.select("a", false);
	w.retain_selection();
	expect_status(pg_reader_open(w.held, PG_READ_LOGICAL, &w.reader,
				&w.error),
		PG_OK, w.error);
}

WHEN("^unrelated entries are added and archive tables grow$")
{
	[[maybe_unused]] World &w = world();

	for (unsigned i = 0; i < 40; ++i) {
		std::string name = "new" + std::to_string(i);

		expect_status(
			pg_source_write_all(w.source(), name.c_str(),
				"new", 3, nullptr, &w.error),
			PG_OK, w.error);
	}
}

THEN("^the reader can still read and seek its original entry$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.chunk(w.reader, 16), "old");
	expect_status(w.status, PG_OK, w.error);
	expect_status(
		pg_reader_seek(w.reader, 1, &w.error), PG_OK, w.error);
	EXPECT_EQ(w.chunk(w.reader, 16), "ld");
	expect_status(w.status, PG_OK, w.error);
}

WHEN("^its selected entry is replaced$")
{
	[[maybe_unused]] World &w = world();

	expect_status(pg_file_write_all(w.held, "replacement", 11,
				nullptr, &w.error),
		PG_OK, w.error);
}

THEN("^the reader reports stale without selecting the new entry$")
{
	[[maybe_unused]] World &w = world();

	w.status = pg_reader_seek(w.reader, 0, &w.error);
	expect_status(w.status, PG_STALE, w.error);
}

GIVEN("^a writable HOGG source retains its OS lease$")
{
	[[maybe_unused]] World &w = world();

	w.archive("archive", {{"a", "old"}}, PG_HOGG10);
	w.previous = read_native(w.paths.at("archive"));
	w.select("a", false);
	w.retain_selection();
	expect_status(pg_reader_open(w.held, PG_READ_LOGICAL, &w.reader,
				&w.error),
		PG_OK, w.error);
}

WHEN("^another context or process requests writable access$")
{
	[[maybe_unused]] World &w = world();

	pg_context *other = nullptr;
	pg_source *blocked = nullptr;
	pg_source_options options = {PG_HOGG10, PG_WRITE, 0};

	expect_status(
		pg_context_open(&other, &w.error), PG_OK, w.error);
	w.status = pg_source_open(other, w.paths.at("archive").c_str(),
		&options, &blocked, &w.error);
	w.saved_error = w.error;
	EXPECT_EQ(blocked, nullptr);
	expect_status(
		pg_context_close(&other, &w.error), PG_OK, w.error);
}

THEN("^that open returns busy without modifying the archive$")
{
	[[maybe_unused]] World &w = world();

	expect_status(w.status, PG_BUSY, w.saved_error);
	EXPECT_EQ(read_native(w.paths.at("archive")), w.previous);
}

THEN("^final reference release permits a subsequent writable ope"
	"n$")
{
	[[maybe_unused]] World &w = world();

	expect_status(
		pg_source_close(&w.sources.at("archive"), &w.error),
		PG_OK, w.error);
	expect_status(pg_file_close(&w.held, &w.error), PG_OK, w.error);
	expect_status(
		pg_reader_close(&w.reader, &w.error), PG_OK, w.error);
	w.open("archive");
	expect_status(w.status, PG_OK, w.error);
}

GIVEN("^a writable captured archive entry with a cached header$")
{
	[[maybe_unused]] World &w = world();

	w.paths["archive"] = "metadata.hogg";
	w.formats["archive"] = PG_HOGG10;
	expect_status(
		pg_archive_builder_create(w.context, "metadata.hogg",
			PG_HOGG10, 0, &w.builder, &w.error),
		PG_OK, w.error);
	pg_entry_options options = {};

	options.compression = PG_COMPRESS_FORCE;
	options.cached_header = "header";
	options.cached_header_size = 6;
	w.expected = std::string(100, 'x');
	expect_status(pg_archive_builder_write_all(w.builder, "a",
				w.expected.data(), w.expected.size(),
				&options, &w.error),
		PG_OK, w.error);
	expect_status(pg_archive_builder_finish(w.builder, &w.error),
		PG_OK, w.error);
	expect_status(pg_archive_builder_close(&w.builder, &w.error),
		PG_OK, w.error);
	w.open("archive");
	expect_status(w.status, PG_OK, w.error);
	w.select("a", false);
	w.retain_selection();
	expect_status(pg_reader_open(w.held, PG_READ_STORED, &w.reader,
				&w.error),
		PG_OK, w.error);
	w.previous = w.chunk(w.reader, 256);
	expect_status(w.status, PG_OK, w.error);
	expect_status(
		pg_reader_close(&w.reader, &w.error), PG_OK, w.error);
	w.offset = std::filesystem::file_size("metadata.hogg");
}

WHEN("^I change only its timestamp and later clear its header$")
{
	[[maybe_unused]] World &w = world();

	pg_metadata_options options = {
		PG_METADATA_MTIME, 123, nullptr, 0};

	expect_status(
		pg_file_update_metadata(w.held, &options, &w.error),
		PG_OK, w.error);
	w.select("a", false);
	pg_file_info before;

	expect_status(pg_file_inspect(w.file, &before, &w.error), PG_OK,
		w.error);
	EXPECT_EQ(before.mtime, 123);
	EXPECT_EQ(before.cached_header_size, 6u);
	w.sequence = before.copy_generation;
	w.required = std::filesystem::file_size("metadata.hogg");
	options.fields = PG_METADATA_HEADER;
	expect_status(
		pg_file_update_metadata(w.file, &options, &w.error),
		PG_OK, w.error);
	w.select("a", false);
}

THEN("^its encoding, payload bytes and checksum remain unchanged"
	"$")
{
	[[maybe_unused]] World &w = world();

	pg_file_info info;

	expect_status(pg_file_inspect(w.file, &info, &w.error), PG_OK,
		w.error);
	EXPECT_EQ(info.encoding, w.captured.encoding);
	EXPECT_EQ(info.stored_size, w.captured.stored_size);
	EXPECT_EQ(memcmp(info.digest, w.captured.digest, 16), 0);
	EXPECT_EQ(info.mtime, 123);
	EXPECT_EQ(info.cached_header_size, 0u);
	expect_status(pg_reader_open(w.file, PG_READ_STORED, &w.reader,
				&w.error),
		PG_OK, w.error);
	EXPECT_EQ(w.chunk(w.reader, 256), w.previous);
	expect_status(w.status, PG_OK, w.error);
}

THEN("^each successful change invalidates the old captured gener"
	"ation$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_GT(w.sequence, w.captured.copy_generation);
	pg_file_info info;

	expect_status(pg_file_inspect(w.file, &info, &w.error), PG_OK,
		w.error);
	EXPECT_GT(info.copy_generation, w.sequence);
	expect_status(
		pg_file_verify(w.held, &w.error), PG_STALE, w.error);
}

THEN("^a HOGG timestamp change does not grow its payload$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.required, w.offset);
}

GIVEN("^a retained HOGG reader and an externally committed journa"
	"l$")
{
	[[maybe_unused]] World &w = world();

	w.archive("archive", {{"a", "old"}}, PG_HOGG10);
	w.select("a", false);
	w.retain_selection();
	expect_status(pg_reader_open(w.held, PG_READ_LOGICAL, &w.reader,
				&w.error),
		PG_OK, w.error);
	commit_timestamp(
		w.paths.at("archive"), w.captured.archive_record, 123);
}

WHEN("^I recover the archive with no staged writer$")
{
	[[maybe_unused]] World &w = world();

	w.status = pg_source_recover(
		w.context, w.paths.at("archive").c_str(), &w.error);
}

THEN("^recovery completes while the reader handle remains open$")
{
	[[maybe_unused]] World &w = world();

	expect_status(w.status, PG_OK, w.error);
	EXPECT_NE(w.reader, nullptr);
}

THEN("^the old reader reports stale for the externally changed a"
	"rchive$")
{
	[[maybe_unused]] World &w = world();

	w.chunk(w.reader, 16);
	expect_status(w.status, PG_STALE, w.error);
	EXPECT_EQ(w.count, 0u);
}

} // namespace
