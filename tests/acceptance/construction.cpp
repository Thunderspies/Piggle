#include "world.hpp"

namespace
{
World &world()
{
	WorldScope scope;
	return *scope;
}

GIVEN("^a HOGG was created with room for 31 user entries$")
{
	World &w = world();
	pg_archive_build_options options = {{PG_HOGG10, 0, 0}, nullptr, 31};

	expect_status(pg_archive_builder_create_config(w.context,
			"reserved.hogg", &options, &w.builder, &w.error),
		PG_OK, w.error);
	expect_status(
		pg_archive_builder_finish(w.builder, &w.error), PG_OK, w.error);
	expect_status(
		pg_archive_builder_close(&w.builder, &w.error), PG_OK, w.error);
	pg_source_options writable = {PG_HOGG10, PG_WRITE, 0};

	w.active = "reserve";
	w.paths[w.active] = "reserved.hogg";
	expect_status(pg_source_open(w.context, "reserved.hogg", &writable,
			&w.sources[w.active], &w.error),
		PG_OK, w.error);
}

WHEN("^I add 40 named files to the existing archive$")
{
	World &w = world();

	for (unsigned i = 0; i < 40; i++) {
		std::string name = "file/" + std::to_string(i);

		expect_status(
			pg_source_write_all(w.source(), name.c_str(),
				name.data(), name.size(), nullptr, &w.error),
			PG_OK, w.error);
		w.names.push_back(name);
	}
}

THEN("^every addition is readable after reopening the archive$")
{
	World &w = world();

	expect_status(pg_source_close(&w.sources[w.active], &w.error), PG_OK,
		w.error);
	expect_status(pg_source_open(w.context, w.paths[w.active].c_str(),
			nullptr, &w.sources[w.active], &w.error),
		PG_OK, w.error);
	for (const auto &name : w.names) {
		char bytes[64];
		size_t count;

		expect_status(pg_source_read_all(w.source(), name.c_str(),
				bytes, sizeof(bytes), &count, &w.error),
			PG_OK, w.error);
		EXPECT_EQ(std::string(bytes, count), name);
	}
}

GIVEN("^a HOGG has an unchanged reader and an editable entry$")
{
	World &w = world();

	w.archive("headers", {{"keep", "safe"}, {"edit", "abc"}}, PG_HOGG10);
	w.active = "headers";
	expect_status(pg_reader_open_source(w.source(), "keep", PG_READ_LOGICAL,
			&w.reader, &w.error),
		PG_OK, w.error);
}

WHEN("^I install a 64 KiB cached header on the editable entry$")
{
	World &w = world();

	expect_status(pg_source_find(w.source(), "edit", &w.file, &w.error),
		PG_OK, w.error);
	w.expected.assign(65536, 'H');
	pg_metadata_options options = {
		PG_METADATA_HEADER, 0, w.expected.data(), w.expected.size()};

	expect_status(pg_file_update_metadata(w.file, &options, &w.error),
		PG_OK, w.error);
	expect_status(pg_file_close(&w.file, &w.error), PG_OK, w.error);
}

THEN("^the unchanged reader still returns its original bytes$")
{
	World &w = world();

	EXPECT_EQ(w.chunk(w.reader, 16), "safe");
	expect_status(w.status, PG_OK, w.error);
}

THEN("^the edited entry retains its payload and the complete cached header$")
{
	World &w = world();
	pg_file_info info;

	expect_status(pg_source_find(w.source(), "edit", &w.file, &w.error),
		PG_OK, w.error);
	expect_status(pg_file_inspect(w.file, &info, &w.error), PG_OK, w.error);
	ASSERT_EQ(info.cached_header_size, w.expected.size());
	EXPECT_EQ(0,
		memcmp(info.cached_header, w.expected.data(),
			w.expected.size()));
	char bytes[3];
	size_t count;

	expect_status(pg_file_read_all(
			w.file, bytes, sizeof(bytes), &count, &w.error),
		PG_OK, w.error);
	EXPECT_EQ(std::string(bytes, count), "abc");
}

} // namespace
