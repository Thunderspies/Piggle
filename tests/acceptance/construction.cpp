#include "world.hpp"

namespace
{
World &world()
{
	WorldScope scope;
	return *scope;
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
