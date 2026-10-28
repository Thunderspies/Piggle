#include "../api_test.hpp"
#include "../../src/digest.hpp"
#include <zlib-ng.h>

namespace
{
using ApiHeaderWrite = piggle_test::ContractTest;

static std::vector<unsigned char> asset(uint32_t word, size_t size,
	bool big = false)
{
	std::vector<unsigned char> data(size, 'a');

	for (size_t i = 0; i < 4 && i < size; ++i)
		data[i] = (unsigned char)(word >> (8 * (big ? 3 - i : i)));
	return data;
}

static void expect_header(pg_source *source, const char *name,
	const std::vector<unsigned char> &data, size_t expected)
{
	pg_file *file = NULL;
	pg_file_info info = {};
	pg_error error;

	ASSERT_EQ(PG_OK, pg_source_find(source, name, &file, &error));
	ASSERT_EQ(PG_OK, pg_file_inspect(file, &info, &error));
	ASSERT_EQ(expected, info.cached_header_size);
	if (expected)
		EXPECT_EQ(0, memcmp(data.data(), info.cached_header, expected));
	else
		EXPECT_EQ(nullptr, info.cached_header);
	ASSERT_EQ(PG_OK, pg_file_verify(file, &error));
	ASSERT_EQ(PG_OK, pg_file_close(&file, &error));
}

TEST_F(ApiHeaderWrite, CompletedInventoryContainsInferredPrefix)
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_error error;
	auto data = asset(8, 32);
	size_t size = 0;
	auto inspect = [](void *state, const pg_file_info *info) -> pg_status {
		*(size_t *)state = info->cached_header_size;
		return PG_OK;
	};

	ASSERT_EQ(PG_OK, pg_context_open(&context, &error));
	ASSERT_EQ(PG_OK, pg_archive_builder_create(context, "archive",
		PG_PIGG2, 0, &builder, &error));
	ASSERT_EQ(PG_OK, pg_archive_builder_write_all(builder, "file.geo",
		data.data(), data.size(), NULL, &error));
	ASSERT_EQ(PG_OK, pg_archive_builder_visit_inventory(builder, inspect,
		&size, &error));
	EXPECT_EQ(12u, size);
	ASSERT_EQ(PG_OK, pg_archive_builder_close(&builder, &error));
	ASSERT_EQ(PG_OK, pg_context_close(&context, &error));
}

TEST_F(ApiHeaderWrite, AutomaticRulesForBothFormatsAndEncodings)
{
	const char *names[] = {"a.anm", "b.GEO", "c.geolm", "d.lgeo",
		"e.bgeo", "f.texture", "g.anim", "h.lm2"};
	const size_t lengths[] = {12, 12, 12, 12, 12, 8, 8, 29};
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_source *source = NULL;
	pg_error error;

	ASSERT_EQ(PG_OK, pg_context_open(&context, &error));
	for (uint32_t format : {PG_PIGG2, PG_HOGG10}) {
		for (uint32_t compression : {PG_COMPRESS_NEVER,
			PG_COMPRESS_FORCE}) {
			ASSERT_EQ(0, clear_file("archive"));
			ASSERT_EQ(PG_OK, pg_archive_builder_create(context,
				"archive", format, 0, &builder, &error));
			pg_entry_options options = {};

			options.compression = compression;
			for (size_t i = 0; i < ARRAY_SIZE(names); ++i) {
				auto data = asset(8, 256, i == 4);

				ASSERT_EQ(PG_OK, pg_archive_builder_write_all(
					builder, names[i], data.data(),
					data.size(),
					&options, &error));
			}
			ASSERT_EQ(PG_OK,
				pg_archive_builder_finish(builder, &error));
			ASSERT_EQ(PG_OK,
				pg_archive_builder_close(&builder, &error));
			ASSERT_EQ(PG_OK, pg_source_open(context, "archive",
				NULL, &source, &error));
			for (size_t i = 0; i < ARRAY_SIZE(names); ++i)
				expect_header(source, names[i],
					asset(8, 256, i == 4), lengths[i]);
			ASSERT_EQ(PG_OK, pg_source_close(&source, &error));
		}
	}
	ASSERT_EQ(PG_OK, pg_context_close(&context, &error));
}

TEST_F(ApiHeaderWrite, EncodedStreamingUsesLogicalPrefix)
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_writer *writer = NULL;
	pg_source *source = NULL;
	pg_error error;
	auto data = asset(70000, 150000);
	std::vector<unsigned char> encoded(zng_compressBound(data.size()));
	size_t accepted = 0;
	size_t size = encoded.size();

	ASSERT_EQ(Z_OK, zng_compress(encoded.data(), &size, data.data(),
		data.size()));
	encoded.resize(size);
	ASSERT_EQ(PG_OK, pg_context_open(&context, &error));
	for (uint32_t format : {PG_PIGG2, PG_HOGG10}) {
		ASSERT_EQ(0, clear_file("archive"));
		ASSERT_EQ(PG_OK, pg_archive_builder_create(context, "archive",
			format, 0, &builder, &error));
		pg_write_options options;

		pg_write_options_init(&options, data.size());
		options.encoding = PG_ZLIB;
		options.input_size = encoded.size();
		ASSERT_EQ(PG_OK, pg_writer_open_archive_builder(builder,
			"stream.geo", &options, &writer, &error));
		for (unsigned char byte : encoded) {
			ASSERT_EQ(PG_OK, pg_writer_write(writer, &byte, 1,
				&accepted, &error));
			ASSERT_EQ(1u, accepted);
		}
		ASSERT_EQ(PG_OK, pg_writer_finish(writer, &error));
		ASSERT_EQ(PG_OK, pg_writer_close(&writer, &error));
		ASSERT_EQ(PG_OK, pg_archive_builder_finish(builder, &error));
		ASSERT_EQ(PG_OK, pg_archive_builder_close(&builder, &error));
		ASSERT_EQ(PG_OK, pg_source_open(context, "archive", NULL,
			&source, &error));
		expect_header(source, "stream.geo", data, 70004);
		ASSERT_EQ(PG_OK, pg_source_close(&source, &error));
	}
	ASSERT_EQ(PG_OK, pg_context_close(&context, &error));
}

TEST_F(ApiHeaderWrite, PoliciesOverrideInferenceAndRejectUnknownValues)
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_source *source = NULL;
	pg_error error;
	auto data = asset(8, 32);
	pg_entry_options options = {};

	ASSERT_EQ(PG_OK, pg_context_open(&context, &error));
	ASSERT_EQ(PG_OK, pg_archive_builder_create(context, "archive",
		PG_HOGG10, 0, &builder, &error));
	options.cached_header_policy = PG_HEADER_CACHE_NONE;
	ASSERT_EQ(PG_OK, pg_archive_builder_write_all(builder, "none.geo",
		data.data(), data.size(), &options, &error));
	options.cached_header_policy = PG_HEADER_CACHE_EXPLICIT;
	ASSERT_EQ(PG_OK, pg_archive_builder_write_all(builder, "empty.geo",
		data.data(), data.size(), &options, &error));
	options.cached_header_policy = PG_HEADER_CACHE_AUTO;
	options.cached_header = "opaque";
	options.cached_header_size = 6;
	ASSERT_EQ(PG_OK, pg_archive_builder_write_all(builder, "override.geo",
		data.data(), data.size(), &options, &error));
	options.cached_header_policy = PG_HEADER_CACHE_EXPLICIT;
	ASSERT_EQ(PG_OK, pg_archive_builder_write_all(builder, "explicit.geo",
		data.data(), data.size(), &options, &error));
	options.cached_header_policy = PG_HEADER_CACHE_NONE;
	ASSERT_EQ(PG_INVALID, pg_archive_builder_write_all(builder, "bad.geo",
		data.data(), data.size(), &options, &error));
	options.cached_header_policy = 99;
	ASSERT_EQ(PG_INVALID, pg_archive_builder_write_all(builder, "bad.geo",
		data.data(), data.size(), &options, &error));
	ASSERT_EQ(PG_OK, pg_archive_builder_finish(builder, &error));
	ASSERT_EQ(PG_OK, pg_archive_builder_close(&builder, &error));
	ASSERT_EQ(PG_OK, pg_source_open(context, "archive", NULL, &source,
		&error));
	expect_header(source, "none.geo", data, 0);
	expect_header(source, "empty.geo", data, 0);
	std::vector<unsigned char> opaque{'o', 'p', 'a', 'q', 'u', 'e'};

	expect_header(source, "override.geo", opaque, opaque.size());
	expect_header(source, "explicit.geo", opaque, opaque.size());
	ASSERT_EQ(PG_OK, pg_source_close(&source, &error));
	ASSERT_EQ(PG_OK, pg_context_close(&context, &error));
}

TEST_F(ApiHeaderWrite, InvalidInferredHeadersDoNotStageEntries)
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_source *source = NULL;
	pg_error error;
	constexpr uint32_t maximum = 2 * 1024 * 1024;
	auto oversized = asset(maximum + 1, maximum + 5);
	auto boundary = asset(maximum, maximum + 4);
	auto truncated = asset(40, 8);

	ASSERT_EQ(PG_OK, pg_context_open(&context, &error));
	for (uint32_t format : {PG_PIGG2, PG_HOGG10}) {
		ASSERT_EQ(0, clear_file("archive"));
		ASSERT_EQ(PG_OK, pg_archive_builder_create(context, "archive",
			format, 0, &builder, &error));
		ASSERT_EQ(PG_CORRUPT, pg_archive_builder_write_all(builder,
			"bad.geo", truncated.data(), truncated.size(), NULL,
			&error));
		ASSERT_EQ(PG_CORRUPT, pg_archive_builder_write_all(builder,
			"bad.lm2", truncated.data(), truncated.size(), NULL,
			&error));
		ASSERT_EQ(PG_LIMIT, pg_archive_builder_write_all(builder,
			"large.geo", oversized.data(), oversized.size(), NULL,
			&error));
		ASSERT_EQ(PG_OK, pg_archive_builder_write_all(builder,
			"boundary.geo", boundary.data(), boundary.size(), NULL,
			&error));
		pg_entry_options none = {};

		none.cached_header_policy = PG_HEADER_CACHE_NONE;
		ASSERT_EQ(PG_OK, pg_archive_builder_write_all(builder,
			"bad.geo", truncated.data(), truncated.size(), &none,
			&error));
		ASSERT_EQ(PG_OK, pg_archive_builder_finish(builder, &error));
		ASSERT_EQ(PG_OK, pg_archive_builder_close(&builder, &error));
		ASSERT_EQ(PG_OK, pg_source_open(context, "archive", NULL,
			&source, &error));
		expect_header(source, "bad.geo", truncated, 0);
		expect_header(source, "boundary.geo", boundary,
			boundary.size());
		ASSERT_EQ(PG_OK, pg_source_close(&source, &error));
	}
	ASSERT_EQ(PG_OK, pg_context_close(&context, &error));
}

TEST_F(ApiHeaderWrite, UncachedFormatsShortInputsAndZeroWords)
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_source *source = NULL;
	pg_error error;
	auto data = asset(8, 32);
	auto zero = asset(0, 32);
	const char *names[] = {"file.wtex", "file.dcache", "file.an",
		"file.txt", "directory.geo/file"};

	ASSERT_EQ(PG_OK, pg_context_open(&context, &error));
	ASSERT_EQ(PG_OK, pg_archive_builder_create(context, "archive",
		PG_PIGG2, 0, &builder, &error));
	for (const char *name : names)
		ASSERT_EQ(PG_OK, pg_archive_builder_write_all(builder, name,
			data.data(), data.size(), NULL, &error));
	ASSERT_EQ(PG_OK, pg_archive_builder_write_all(builder, "zero.lm2",
		zero.data(), zero.size(), NULL, &error));
	ASSERT_EQ(PG_OK, pg_archive_builder_write_all(builder, "short.geo",
		data.data(), 3, NULL, &error));
	ASSERT_EQ(PG_OK, pg_archive_builder_write_all(builder, "empty.geo",
		NULL, 0, NULL, &error));
	ASSERT_EQ(PG_OK, pg_archive_builder_finish(builder, &error));
	ASSERT_EQ(PG_OK, pg_archive_builder_close(&builder, &error));
	ASSERT_EQ(PG_OK, pg_source_open(context, "archive", NULL, &source,
		&error));
	for (const char *name : names)
		expect_header(source, name, data, 0);
	expect_header(source, "zero.lm2", zero, 0);
	expect_header(source, "short.geo", data, 0);
	expect_header(source, "empty.geo", data, 0);
	ASSERT_EQ(PG_OK, pg_source_close(&source, &error));
	ASSERT_EQ(PG_OK, pg_context_close(&context, &error));
}

TEST_F(ApiHeaderWrite, ImportsPackingAndReplacementsInferHeaders)
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_source *source = NULL, *loose = NULL, *packed = NULL;
	pg_error error;
	auto first = asset(8, 64);
	auto second = asset(12, 64);

	ASSERT_EQ(0, directory("root"));
	ASSERT_EQ(0, put_bytes("root/file.geo", first.data(), first.size()));
	ASSERT_EQ(PG_OK, pg_context_open(&context, &error));
	ASSERT_EQ(PG_OK, pg_source_open(context, "root", NULL, &loose,
		&error));
	ASSERT_EQ(PG_OK, pg_source_request_subtree(loose, NULL, &error));
	for (uint32_t format : {PG_PIGG2, PG_HOGG10}) {
		ASSERT_EQ(0, clear_file("archive"));
		ASSERT_EQ(0, clear_file("packed"));
		ASSERT_EQ(PG_OK, pg_archive_builder_create(context, "archive",
			format, 0, &builder, &error));
		ASSERT_EQ(PG_OK, pg_archive_builder_import(builder, "file.geo",
			"root/file.geo", PG_COMPRESS_AUTO, &error));
		ASSERT_EQ(PG_OK, pg_archive_builder_finish(builder, &error));
		ASSERT_EQ(PG_OK, pg_archive_builder_close(&builder, &error));
		pg_source_options options = {format, PG_WRITE, 0};

		ASSERT_EQ(PG_OK, pg_source_open(context, "archive", &options,
			&source, &error));
		expect_header(source, "file.geo", first, 12);
		ASSERT_EQ(PG_OK, pg_source_write_all(source, "file.geo",
			second.data(), second.size(), NULL, &error));
		expect_header(source, "file.geo", second, 16);
		ASSERT_EQ(PG_OK, pg_source_import(source, "import.geo",
			"root/file.geo", PG_COMPRESS_AUTO, &error));
		expect_header(source, "import.geo", first, 12);
		ASSERT_EQ(PG_OK, pg_source_pack(loose, "packed", format, NULL,
			&error));
		ASSERT_EQ(PG_OK, pg_source_open(context, "packed", NULL,
			&packed, &error));
		expect_header(packed, "file.geo", first, 12);
		ASSERT_EQ(PG_OK, pg_source_close(&packed, &error));
		ASSERT_EQ(PG_OK, pg_source_close(&source, &error));
	}
	ASSERT_EQ(PG_OK, pg_source_close(&loose, &error));
	ASSERT_EQ(PG_OK, pg_context_close(&context, &error));
}

TEST_F(ApiHeaderWrite, SessionRestoresAutomaticHeaders)
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_source *source = NULL;
	pg_error error;
	pg_archive_build_options options = {{PG_HOGG10, 0, 0}, NULL, 4};
	auto data = asset(8, 64);

	ASSERT_EQ(PG_OK, pg_context_open(&context, &error));
	ASSERT_EQ(PG_OK, pg_archive_builder_create_session(context, "archive",
		"session", &options, &builder, &error));
	ASSERT_EQ(PG_OK, pg_archive_builder_write_all(builder, "file.geo",
		data.data(), data.size(), NULL, &error));
	ASSERT_EQ(PG_OK, pg_archive_builder_suspend(&builder, &error));
	ASSERT_EQ(PG_OK, pg_archive_builder_resume(context, "session", &builder,
		&error));
	ASSERT_EQ(PG_OK, pg_archive_builder_finish(builder, &error));
	ASSERT_EQ(PG_OK, pg_archive_builder_discard(&builder, &error));
	ASSERT_EQ(PG_OK, pg_source_open(context, "archive", NULL, &source,
		&error));
	expect_header(source, "file.geo", data, 12);
	ASSERT_EQ(PG_OK, pg_source_close(&source, &error));
	ASSERT_EQ(PG_OK, pg_context_close(&context, &error));
}

TEST_F(ApiHeaderWrite, CopiesPreserveCacheAbsenceAndNeverCopyCachedPayload)
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_source *source = NULL, *copied = NULL;
	pg_file *file = NULL;
	pg_reader *reader = NULL;
	pg_error error;
	auto data = asset(8, 32);
	std::vector<unsigned char> opaque(40, 'x');
	pg_entry_options entry = {};

	ASSERT_EQ(PG_OK, pg_context_open(&context, &error));
	for (uint32_t format : {PG_PIGG2, PG_HOGG10}) {
		ASSERT_EQ(0, clear_file("original"));
		ASSERT_EQ(0, clear_file("copied"));
		ASSERT_EQ(0, clear_file("exported"));
		ASSERT_EQ(PG_OK, pg_archive_builder_create(context, "original",
			format, 0, &builder, &error));
		entry.compression = PG_COMPRESS_NEVER;
		entry.cached_header_policy = PG_HEADER_CACHE_NONE;
		ASSERT_EQ(PG_OK, pg_archive_builder_write_all(builder,
			"none.geo", data.data(), data.size(), &entry, &error));
		entry.cached_header_policy = PG_HEADER_CACHE_EXPLICIT;
		entry.cached_header = opaque.data();
		entry.cached_header_size = opaque.size();
		ASSERT_EQ(PG_OK, pg_archive_builder_write_all(builder,
			"opaque.geo", data.data(), data.size(), &entry,
			&error));
		entry.cached_header = NULL;
		entry.cached_header_size = 0;
		ASSERT_EQ(PG_OK, pg_archive_builder_finish(builder, &error));
		ASSERT_EQ(PG_OK, pg_archive_builder_close(&builder, &error));
		ASSERT_EQ(PG_OK, pg_source_open(context, "original", NULL,
			&source, &error));
		ASSERT_EQ(PG_OK, pg_source_pack(source, "copied", format, NULL,
			&error));
		ASSERT_EQ(PG_OK, pg_source_open(context, "copied", NULL,
			&copied, &error));
		expect_header(copied, "none.geo", data, 0);
		expect_header(copied, "opaque.geo", opaque, opaque.size());
		ASSERT_EQ(PG_OK, pg_source_find(copied, "opaque.geo", &file,
			&error));
		ASSERT_EQ(PG_OK, pg_reader_open(file, PG_READ_LOGICAL_PAYLOAD,
			&reader, &error));
		std::vector<unsigned char> actual(data.size());
		size_t count = 0;

		ASSERT_EQ(PG_OK, pg_reader_read(reader, actual.data(),
			actual.size(), &count, &error));
		EXPECT_EQ(data, actual);
		EXPECT_EQ(data.size(), count);
		ASSERT_EQ(PG_OK, pg_reader_close(&reader, &error));
		ASSERT_EQ(PG_OK, pg_file_export(file, "exported", 0, &error));
		ASSERT_EQ(0, expect_bytes("exported", data.data(),
			data.size()));
		ASSERT_EQ(PG_OK, pg_file_close(&file, &error));
		ASSERT_EQ(PG_OK, pg_source_close(&copied, &error));
		ASSERT_EQ(PG_OK, pg_source_close(&source, &error));
	}
	ASSERT_EQ(PG_OK, pg_context_close(&context, &error));
}

TEST_F(ApiHeaderWrite, AcceptedStoredTransfersDoNotInferHeaders)
{
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_writer *writer = NULL;
	pg_source *source = NULL;
	pg_error error;
	/* Decoded data declares an impossible header, but caller acceptance
	 * bypasses asset interpretation as well as payload validation.
	 */
	auto data = asset(40, 8);
	pg_md5 hash;
	pg_stored_write_options options = {};

	options.logical_size = data.size();
	options.stored_size = data.size();
	options.digest_kind = PG_DIGEST_MD5;
	pg_md5_init(&hash);
	pg_md5_update(&hash, data.data(), data.size());
	pg_md5_finish(&hash, options.digest);
	ASSERT_EQ(PG_OK, pg_context_open(&context, &error));
	for (uint32_t format : {PG_PIGG2, PG_HOGG10}) {
		ASSERT_EQ(0, clear_file("archive"));
		ASSERT_EQ(PG_OK, pg_archive_builder_create(context, "archive",
			format, 0, &builder, &error));
		ASSERT_EQ(PG_OK, pg_writer_open_archive_builder_stored(builder,
			"file.geo", &options, &writer, &error));
		size_t count = 0;

		ASSERT_EQ(PG_OK, pg_writer_write(writer, data.data(),
			data.size(), &count, &error));
		ASSERT_EQ(data.size(), count);
		ASSERT_EQ(PG_OK, pg_writer_finish(writer, &error));
		ASSERT_EQ(PG_OK, pg_writer_close(&writer, &error));
		ASSERT_EQ(PG_OK, pg_archive_builder_finish(builder, &error));
		ASSERT_EQ(PG_OK, pg_archive_builder_close(&builder, &error));
		ASSERT_EQ(PG_OK, pg_source_open(context, "archive", NULL,
			&source, &error));
		expect_header(source, "file.geo", data, 0);
		ASSERT_EQ(PG_OK, pg_source_close(&source, &error));
	}
	ASSERT_EQ(PG_OK, pg_context_close(&context, &error));
}
} /* namespace */
