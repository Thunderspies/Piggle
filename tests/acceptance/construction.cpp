#include "world.hpp"

namespace
{
World &world()
{
	WorldScope scope;
	return *scope;
}

static pg_status PG_CALL capture_inventory(
	void *state, const pg_file_info *entry)
{
	auto *names = static_cast<std::vector<std::string> *>(state);

	names->push_back(entry->original_name);
	return PG_OK;
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
	pg_reader *payload = nullptr;

	expect_status(pg_reader_open(w.file, PG_READ_LOGICAL_PAYLOAD,
			&payload, &w.error),
		PG_OK, w.error);
	expect_status(pg_reader_read(
			payload, bytes, sizeof(bytes), &count, &w.error),
		PG_OK, w.error);
	EXPECT_EQ(std::string(bytes, count), "abc");
	expect_status(pg_reader_close(&payload, &w.error), PG_OK, w.error);
}

GIVEN("^a HOGG contains obsolete payload bytes after replacement$")
{
	World &w = world();

	w.archive("storage", {{"a", "original"}}, PG_HOGG10);
	w.active = "storage";
	expect_status(pg_source_write_all(
			w.source(), "a", "latest", 6, nullptr, &w.error),
		PG_OK, w.error);
	w.previous = read_native(w.paths[w.active]);
}

WHEN("^I inspect its storage and compact it to another archive$")
{
	World &w = world();
	pg_source_storage_info info;

	expect_status(pg_source_storage_inspect(w.source(), &info, &w.error),
		PG_OK, w.error);
	EXPECT_GE(info.unreferenced_bytes, 8u);
	EXPECT_EQ(info.physical_length,
		info.live_payload_bytes + info.metadata_bytes +
			info.unreferenced_bytes);
	expect_status(pg_source_compact(
			w.source(), "compact.hogg", nullptr, &w.error),
		PG_OK, w.error);
	w.paths["compact"] = "compact.hogg";
	expect_status(pg_source_open(w.context, "compact.hogg", nullptr,
			&w.sources["compact"], &w.error),
		PG_OK, w.error);
}

THEN("^the compacted archive matches the visible source content$")
{
	World &w = world();
	char bytes[16];
	size_t count;

	expect_status(pg_source_read_all(w.sources["compact"], "a", bytes,
			sizeof(bytes), &count, &w.error),
		PG_OK, w.error);
	EXPECT_EQ(std::string(bytes, count), "latest");
}

THEN("^the original archive remains byte-for-byte unchanged$")
{
	World &w = world();

	EXPECT_EQ(read_native(w.paths["storage"]), w.previous);
}

GIVEN("^a persistent HOGG builder uses a chosen staging directory$")
{
	World &w = world();

	std::filesystem::create_directory("scratch");
	pg_archive_build_options options = {{PG_HOGG10, 0, 0}, "scratch", 32};

	expect_status(
		pg_archive_builder_create_session(w.context, "published.hogg",
			"session", &options, &w.builder, &w.error),
		PG_OK, w.error);
}

WHEN("^I checkpoint one entry and close after adding another entry$")
{
	World &w = world();

	expect_status(pg_archive_builder_write_all(
			w.builder, "First", "abc", 3, nullptr, &w.error),
		PG_OK, w.error);
	expect_status(pg_archive_builder_checkpoint(w.builder, &w.error), PG_OK,
		w.error);
	expect_status(pg_archive_builder_write_all(
			w.builder, "Ignored", "x", 1, nullptr, &w.error),
		PG_OK, w.error);
	expect_status(
		pg_archive_builder_close(&w.builder, &w.error), PG_OK, w.error);
}

WHEN("^I add two entries and suspend construction$")
{
	World &w = world();

	expect_status(pg_archive_builder_write_all(
			w.builder, "First", "abc", 3, nullptr, &w.error),
		PG_OK, w.error);
	expect_status(pg_archive_builder_write_all(
			w.builder, "Second", "def", 3, nullptr, &w.error),
		PG_OK, w.error);
	expect_status(pg_archive_builder_suspend(&w.builder, &w.error), PG_OK,
		w.error);
}

WHEN("^I resume the construction session$")
{
	World &w = world();

	expect_status(pg_archive_builder_resume(
			w.context, "session", &w.builder, &w.error),
		PG_OK, w.error);
}

THEN("^its inventory contains only the checkpointed entry$")
{
	World &w = world();

	expect_status(pg_archive_builder_visit_inventory(
			w.builder, capture_inventory, &w.names, &w.error),
		PG_OK, w.error);
	EXPECT_EQ(w.names, (std::vector<std::string>{"First"}));
}

THEN("^its inventory retains both entries in insertion order$")
{
	World &w = world();

	expect_status(pg_archive_builder_visit_inventory(
			w.builder, capture_inventory, &w.names, &w.error),
		PG_OK, w.error);
	EXPECT_EQ(w.names, (std::vector<std::string>{"First", "Second"}));
}

THEN("^I can add another entry and explicitly publish the archive$")
{
	World &w = world();

	expect_status(pg_archive_builder_write_all(
			w.builder, "Second", "def", 3, nullptr, &w.error),
		PG_OK, w.error);
	expect_status(
		pg_archive_builder_finish(w.builder, &w.error), PG_OK, w.error);
	expect_status(
		pg_archive_builder_close(&w.builder, &w.error), PG_OK, w.error);
	pg_source *source = nullptr;

	expect_status(pg_source_open(w.context, "published.hogg", nullptr,
			&source, &w.error),
		PG_OK, w.error);
	expect_status(pg_source_validate(source, &w.error), PG_OK, w.error);
	expect_status(pg_source_close(&source, &w.error), PG_OK, w.error);
}

THEN("^I can discard construction without publishing an archive$")
{
	World &w = world();

	expect_status(pg_archive_builder_discard(&w.builder, &w.error), PG_OK,
		w.error);
	EXPECT_FALSE(std::filesystem::exists("published.hogg"));
	EXPECT_FALSE(std::filesystem::exists("session"));
}

WHEN("^I copy its checkpoint into another session directory$")
{
	World &w = world();

	expect_status(pg_archive_builder_write_all(
			w.builder, "First", "abc", 3, nullptr, &w.error),
		PG_OK, w.error);
	expect_status(pg_archive_builder_checkpoint(w.builder, &w.error), PG_OK,
		w.error);
	std::filesystem::create_directory("copy");
	std::filesystem::copy_file("session/checkpoint", "copy/checkpoint");
	write_native("copy/lock", "");
}

THEN("^resuming the copy reports busy without acquiring a builder$")
{
	World &w = world();
	pg_archive_builder *other = nullptr;

	w.status = pg_archive_builder_resume(
		w.context, "copy", &other, &w.error);
	expect_status(w.status, PG_BUSY, w.error);
	EXPECT_EQ(nullptr, other);
	pg_archive_builder_close(&other, nullptr);
}

THEN("^the original builder can publish its checkpointed content$")
{
	World &w = world();

	expect_status(
		pg_archive_builder_finish(w.builder, &w.error), PG_OK, w.error);
	expect_status(pg_archive_builder_discard(&w.builder, &w.error), PG_OK,
		w.error);
	expect_status(pg_source_open(w.context, "published.hogg", nullptr,
			&w.sources["published"], &w.error),
		PG_OK, w.error);
	char bytes[3];
	size_t count;

	expect_status(pg_source_read_all(w.sources["published"], "First",
			bytes, sizeof(bytes), &count, &w.error),
		PG_OK, w.error);
	EXPECT_EQ("abc", std::string(bytes, count));
}

static std::string cached_resource()
{
	return std::string("\4\0\0\0headtailbody", 16);
}

GIVEN("^a \"(PIGG|HOGG)\" builder for a resource with a valid header$")
{
	REGEX_PARAM(std::string, format);
	World &w = world();

	w.bytes = cached_resource();
	w.active = "header-cache";
	w.paths[w.active] = "header.archive";
	expect_status(pg_archive_builder_create(w.context, "header.archive",
			format == "PIGG" ? PG_PIGG2 : PG_HOGG10, 0,
			&w.builder, &w.error),
		PG_OK, w.error);
}

WHEN("^I stage the resource with \"(automatic|suppressed|explicit)\" "
	"header caching$")
{
	REGEX_PARAM(std::string, policy);
	World &w = world();
	pg_entry_options options = {};

	options.compression = PG_COMPRESS_FORCE;
	w.expected = w.bytes.substr(0, 8);
	if (policy == "suppressed") {
		options.cached_header_policy = PG_HEADER_CACHE_NONE;
		w.expected.clear();
	} else if (policy == "explicit") {
		w.expected = "opaque";
		options.cached_header_policy = PG_HEADER_CACHE_EXPLICIT;
		options.cached_header = w.expected.data();
		options.cached_header_size = w.expected.size();
	}
	expect_status(pg_archive_builder_write_all(w.builder, "mesh.geo",
			w.bytes.data(), w.bytes.size(), &options, &w.error),
		PG_OK, w.error);
	expect_status(pg_archive_builder_finish(w.builder, &w.error),
		PG_OK, w.error);
	expect_status(pg_archive_builder_close(&w.builder, &w.error),
		PG_OK, w.error);
	expect_status(pg_source_open(w.context, "header.archive", nullptr,
			&w.sources[w.active], &w.error),
		PG_OK, w.error);
}

THEN("^its archive metadata contains the selected header$")
{
	World &w = world();
	pg_file_info info;

	expect_status(pg_source_find(w.source(), "mesh.geo", &w.file, &w.error),
		PG_OK, w.error);
	expect_status(pg_file_inspect(w.file, &info, &w.error), PG_OK, w.error);
	EXPECT_EQ(info.cached_header_size, w.expected.size());
	if (info.cached_header_size)
		EXPECT_EQ(std::string((const char *)info.cached_header,
			info.cached_header_size), w.expected);
	else
		EXPECT_EQ(info.cached_header, nullptr);
}

THEN("^logical prefix reads use that cached header$")
{
	World &w = world();
	size_t size = w.expected.empty() ? 4 : w.expected.size();

	expect_status(pg_reader_open(w.file, PG_READ_LOGICAL,
			&w.reader, &w.error),
		PG_OK, w.error);
	EXPECT_EQ(w.chunk(w.reader, size),
		w.expected.empty() ? w.bytes.substr(0, size) : w.expected);
	expect_status(w.status, PG_OK, w.error);
	expect_status(pg_reader_close(&w.reader, &w.error), PG_OK, w.error);
}

THEN("^a payload-only reader returns the original resource$")
{
	World &w = world();

	expect_status(pg_reader_open(w.file, PG_READ_LOGICAL_PAYLOAD,
			&w.reader, &w.error),
		PG_OK, w.error);
	EXPECT_EQ(w.chunk(w.reader, w.bytes.size() + 1), w.bytes);
	expect_status(w.status, PG_OK, w.error);
	expect_status(pg_reader_close(&w.reader, &w.error), PG_OK, w.error);
}

GIVEN("^logical resource bytes with a length-prefixed header$")
{
	world().bytes = cached_resource();
}

WHEN("^I classify and extract its header independently$")
{
	World &w = world();
	pg_header_recipe recipe;
	pg_header_cache_info info;

	expect_status(pg_header_cache_classify("mesh.GEO", &recipe, &w.error),
		PG_OK, w.error);
	EXPECT_EQ(recipe.mode, PG_HEADER_RECIPE_U32LE);
	EXPECT_EQ(recipe.adjustment, 4);
	expect_status(pg_header_cache_probe("mesh.geo", w.bytes.data(), 4,
			w.bytes.size(), &info, &w.error),
		PG_OK, w.error);
	EXPECT_EQ(info.size, 8u);
	expect_status(pg_header_cache_extract("mesh.geo", w.bytes.data(),
			w.bytes.size(), 8, &w.buffer, &w.error),
		PG_OK, w.error);
}

THEN("^the independent cache contains its complete header prefix$")
{
	World &w = world();

	ASSERT_EQ(w.buffer.size, 8u);
	EXPECT_EQ(std::string((const char *)w.buffer.data, w.buffer.size),
		w.bytes.substr(0, 8));
}

GIVEN("^independent binary cache slots with empty entries$")
{
	world().payloads = {"alpha", "", std::string("\0\377hdr", 5), ""};
}

WHEN("^I encode and decode the slots as a \"(PIGG pool|HOGG DataList)\"$")
{
	REGEX_PARAM(std::string, container);
	World &w = world();
	std::vector<pg_header_cache_span> spans;
	pg_header_cache *cache = nullptr;
	uint32_t format = container == "PIGG pool" ?
		PG_HEADER_CACHE_PIGG_POOL : PG_HEADER_CACHE_HOGG_DATALIST;

	for (const auto &bytes : w.payloads)
		spans.push_back({bytes.data(), bytes.size()});
	expect_status(pg_header_cache_encode(format, spans.data(), spans.size(),
			&w.buffer, &w.error),
		PG_OK, w.error);
	expect_status(pg_header_cache_decode(format, w.buffer.data,
			w.buffer.size, &cache, &w.error),
		PG_OK, w.error);
	pg_buffer_free(&w.buffer);
	size_t count = 0;

	expect_status(pg_header_cache_count(cache, &count, &w.error),
		PG_OK, w.error);
	for (size_t i = 0; i < count; i++) {
		pg_header_cache_span span;

		expect_status(pg_header_cache_get(cache, i, &span, &w.error),
			PG_OK, w.error);
		w.names.push_back(span.size ?
			std::string((const char *)span.data, span.size) : "");
	}
	expect_status(pg_header_cache_close(&cache, &w.error), PG_OK, w.error);
}

THEN("^every slot retains its index and original bytes$")
{
	World &w = world();

	EXPECT_EQ(w.names, w.payloads);
}

} // namespace
