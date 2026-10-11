#include "world.hpp"

namespace
{

World &world()
{
	WorldScope scope;

	return *scope;
}

WHEN("^I normalize the name \"Textures\\\\\u00c9T\u00c9\\.DDS\"$")
{
	[[maybe_unused]] World &w = world();

	char output[128];
	w.status = pg_name_normalize(w.context, "Textures\\ÉTÉ.DDS",
		output, sizeof(output), &w.required, &w.error);
	if (w.status == PG_OK)
		w.normalized = output;
}

THEN("^the canonical name is \"([^\"]*)\"$")
{
	[[maybe_unused]] World &w = world();
	REGEX_PARAM(std::string, name);

	expect_status(w.status, PG_OK, w.error);
	EXPECT_EQ(w.normalized, name);
}

THEN("^the non\\-ASCII bytes have not changed$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.normalized.substr(9, 5), "ÉtÉ");
}

GIVEN("^a visible file named \"textures/a\\.dds\"$")
{
	[[maybe_unused]] World &w = world();

	w.ready({{"textures/a.dds", "texture"}});
}

WHEN("^I find \"Textures\\\\A\\.DDS\" through the source API$")
{
	[[maybe_unused]] World &w = world();

	w.find("Textures\\A.DDS", false);
}

THEN("^I select the copy visible as \"textures/a\\.dds\"$")
{
	[[maybe_unused]] World &w = world();

	expect_status(w.status, PG_OK, w.error);
	if (w.file) {
		pg_file_info info;

		expect_status(pg_file_inspect(w.file, &info, &w.error),
			PG_OK, w.error);
		EXPECT_STREQ(info.canonical_name, "textures/a.dds");
		EXPECT_EQ(w.read(w.file), "texture");
	} else {
		EXPECT_EQ(w.chunk(w.reader, 32), "texture");
	}
}

WHEN("^I open \"Textures\\\\A\\.DDS\" as a named reader$")
{
	[[maybe_unused]] World &w = world();

	expect_status(pg_file_close(&w.file, &w.error), PG_OK, w.error);
	w.status = pg_reader_open_tree(w.tree, "Textures\\A.DDS",
		PG_READ_LOGICAL, &w.reader, &w.error);
}

WHEN("^I normalize \"([^\"]*)\"$")
{
	[[maybe_unused]] World &w = world();
	REGEX_PARAM(std::string, name);

	char output[128];

	memset(output, '!', sizeof(output));
	w.previous.assign(output, sizeof(output));
	w.status = pg_name_normalize(w.context, name.c_str(), output,
		sizeof(output), &w.required, &w.error);
	w.bytes.assign(output, sizeof(output));
	if (w.status == PG_OK)
		w.normalized = output;
}

THEN("^the result is an invalid\\-name error$")
{
	[[maybe_unused]] World &w = world();

	expect_status(w.status, PG_INVALID, w.error);
}

THEN("^no destination is changed$")
{
	[[maybe_unused]] World &w = world();

	EXPECT_EQ(w.bytes, w.previous);
}

THEN("^synchronous lookup accepts the equivalent spelling \"a//b\""
	"$")
{
	[[maybe_unused]] World &w = world();

	w.ready({{"a/b", "equivalent"}});
	w.select("a//b");
	EXPECT_EQ(w.read(w.file), "equivalent");
}

GIVEN("^the source has native path \"C:\\\\Game\\\\Data\\.PIGG\"$")
{
	[[maybe_unused]] World &w = world();

// A Windows spelling is tested as a native POSIX filename here.
// Windows uses a native temporary absolute path on its existing volume.
#ifdef _WIN32
	w.native_name = (std::filesystem::current_path() / "Data.PIGG")
				.string();
#else
	w.native_name = "C:\\Game\\Data.PIGG";
#endif
	raw_pigg(w.native_name, {{"textures/a.dds", "texture"}});
	w.paths["native"] = w.native_name;
	w.formats["native"] = PG_PIGG2;
	w.open("native");
	expect_status(w.status, PG_OK, w.error);
}

GIVEN("^it contains the canonical name \"textures/a\\.dds\"$")
{
	[[maybe_unused]] World &w = world();

	w.select("textures/a.dds", false);
	EXPECT_EQ(w.read(w.file), "texture");
}

WHEN("^I find \"Textures\\\\A\\.DDS\" through that source handle$")
{
	[[maybe_unused]] World &w = world();

	w.find("Textures\\A.DDS", false);
}

THEN("^I select that source's copy of \"textures/a\\.dds\"$")
{
	[[maybe_unused]] World &w = world();

	expect_status(w.status, PG_OK, w.error);
	pg_file_info info;
	pg_source_info source;

	expect_status(pg_file_inspect(w.file, &info, &w.error), PG_OK,
		w.error);
	expect_status(pg_source_inspect(w.source(), &source, &w.error),
		PG_OK, w.error);
	EXPECT_EQ(info.source_id, source.id);
	EXPECT_STREQ(info.canonical_name, "textures/a.dds");
}

THEN("^the native source path remains \"C:\\\\Game\\\\Data\\.PIGG\"$")
{
	[[maybe_unused]] World &w = world();

	pg_source_info info;

	expect_status(pg_source_inspect(w.source(), &info, &w.error),
		PG_OK, w.error);
	EXPECT_EQ(std::filesystem::path(info.native_path),
		std::filesystem::absolute(w.native_name));
}

WHEN("^I use \"C:\\\\Game\\\\Data\\.PIGG:textures/a\\.dds\" as a "
	"virtual"
	" name$")
{
	[[maybe_unused]] World &w = world();

	w.find("C:\\Game\\Data.PIGG:textures/a.dds", false);
}

GIVEN("^\"Extra\\.PIGG\" exists but is not attached$")
{
	[[maybe_unused]] World &w = world();

	w.ensure_tree();
	raw_pigg("Extra.PIGG", {{"data/a", "standalone"}});
	w.paths["extra"] = "Extra.PIGG";
	w.formats["extra"] = PG_PIGG2;
}

WHEN("^I explicitly open \"Extra\\.PIGG\" as a source$")
{
	[[maybe_unused]] World &w = world();

	w.open("extra");
	expect_status(w.status, PG_OK, w.error);
}

THEN("^I can find \"data/a\" through that source handle$")
{
	[[maybe_unused]] World &w = world();

	w.select("data/a", false);
	EXPECT_EQ(w.read(w.file), "standalone");
}

THEN("^the source is still not attached to the tree$")
{
	[[maybe_unused]] World &w = world();

	pg_tree_info info;

	expect_status(pg_tree_inspect(w.tree, &info, &w.error), PG_OK,
		w.error);
	EXPECT_EQ(info.source_count, 0u);
	w.find("data/a");
	expect_status(w.status, PG_NOT_FOUND, w.error);
}

WHEN("^I open that archive through a dot-containing native path$")
{
	World &w = world();

	w.paths["extra"] = ".\\Extra.PIGG";
	w.open("extra");
	expect_status(w.status, PG_OK, w.error);
}

THEN("^its reported path matches the full native archive path$")
{
	World &w = world();
	pg_source_info info;

	expect_status(pg_source_inspect(w.source(), &info, &w.error),
		PG_OK, w.error);
	EXPECT_EQ(std::filesystem::absolute("Extra.PIGG").u8string(),
		info.native_path);
}

} // namespace
