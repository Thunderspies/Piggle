#include <gtest/gtest.h>
#include <piggle/piggle.hpp>

#include <stdint.h>
#include <string.h>
#include <vector>

namespace {

TEST(HeaderCache, ClassifiesExactResourceExtensionsWithoutArchiveState)
{
	struct example {
		const char *name;
		uint32_t mode;
		uint32_t prefix;
		int32_t adjustment;
	};
	const example examples[] = {
		{"model.anm", PG_HEADER_RECIPE_U32LE, 0, 4},
		{"model.geo", PG_HEADER_RECIPE_U32LE, 0, 4},
		{"model.geolm", PG_HEADER_RECIPE_U32LE, 0, 4},
		{"model.lgeo", PG_HEADER_RECIPE_U32LE, 0, 4},
		{"model.bgeo", PG_HEADER_RECIPE_U32BE, 0, 4},
		{"image.texture", PG_HEADER_RECIPE_U32LE, 0, 0},
		{"motion.anim", PG_HEADER_RECIPE_U32LE, 0, 0},
		{"map.lm2", PG_HEADER_RECIPE_PREFIX, 29, 0},
		{"Folder/IMAGE.TeXtUrE", PG_HEADER_RECIPE_U32LE, 0, 0},
		{"image.wtex", PG_HEADER_RECIPE_NONE, 0, 0},
		{"model.dcache", PG_HEADER_RECIPE_NONE, 0, 0},
		{"model.ge", PG_HEADER_RECIPE_NONE, 0, 0},
		{"image.texture.backup", PG_HEADER_RECIPE_NONE, 0, 0},
		{"folder.geo/file", PG_HEADER_RECIPE_NONE, 0, 0},
		{"noextension", PG_HEADER_RECIPE_NONE, 0, 0}
	};
	for (const auto &example : examples) {
		SCOPED_TRACE(example.name);
		pg_header_recipe recipe = {};
		pg_error error;

		ASSERT_EQ(PG_OK, pg_header_cache_classify(
			example.name, &recipe, &error));
		EXPECT_EQ(PG_OK, error.status);
		EXPECT_EQ(example.mode, recipe.mode);
		EXPECT_EQ(example.prefix, recipe.prefix_size);
		EXPECT_EQ(example.adjustment, recipe.adjustment);
	}
}

TEST(HeaderCache, ProbeUsesLogicalPrefixAndReturnsPublisherRecipe)
{
	const uint8_t little[] = {8, 0, 0, 0};
	const uint8_t big[] = {0, 0, 0, 8};
	pg_header_cache_info info = {};

	ASSERT_EQ(PG_OK, pg_header_cache_probe(
		"model.anm", little, 4, 64, &info, nullptr));
	EXPECT_EQ(12u, info.size);
	EXPECT_EQ(PG_HEADER_RECIPE_U32LE, info.recipe.mode);
	EXPECT_EQ(4, info.recipe.adjustment);
	ASSERT_EQ(PG_OK, pg_header_cache_probe(
		"model.BGEO", big, 4, 64, &info, nullptr));
	EXPECT_EQ(12u, info.size);
	EXPECT_EQ(PG_HEADER_RECIPE_U32BE, info.recipe.mode);
	ASSERT_EQ(PG_OK, pg_header_cache_probe(
		"image.texture", little, 4, 64, &info, nullptr));
	EXPECT_EQ(8u, info.size);
	EXPECT_EQ(0, info.recipe.adjustment);
	ASSERT_EQ(PG_OK, pg_header_cache_probe(
		"map.lm2", little, 4, 64, &info, nullptr));
	EXPECT_EQ(29u, info.size);
	EXPECT_EQ(PG_HEADER_RECIPE_PREFIX, info.recipe.mode);
	EXPECT_EQ(29u, info.recipe.prefix_size);
}

TEST(HeaderCache, ShortOrZeroLogicalHeadersResolveToNone)
{
	const uint8_t zero[4] = {};
	for (const char *name : {"image.texture", "map.lm2"}) {
		for (size_t size : {0u, 3u, 4u}) {
			pg_header_cache_info info = {};

			ASSERT_EQ(PG_OK, pg_header_cache_probe(name,
				size ? zero : nullptr, size, size,
				&info, nullptr));
			EXPECT_EQ(PG_HEADER_RECIPE_NONE, info.recipe.mode);
			EXPECT_EQ(0u, info.size);
		}
	}
	pg_header_cache_info info = {};

	ASSERT_EQ(PG_OK, pg_header_cache_probe(
		"unknown.bin", nullptr, 0, 64, &info, nullptr));
	EXPECT_EQ(0u, info.size);
}

TEST(HeaderCache, ProbeChecksAvailabilityAndLogicalBounds)
{
	const uint8_t little[] = {8, 0, 0, 0};
	pg_header_cache_info info;
	pg_error error;

	memset(&info, 0xff, sizeof(info));
	EXPECT_EQ(PG_CAPACITY, pg_header_cache_probe(
		"image.texture", little, 3, 64, &info, &error));
	EXPECT_EQ(PG_CAPACITY, error.status);
	EXPECT_EQ(0u, info.size);
	EXPECT_EQ(PG_HEADER_RECIPE_NONE, info.recipe.mode);
	EXPECT_EQ(PG_CORRUPT, pg_header_cache_probe(
		"image.texture", little, 4, 7, &info, nullptr));
	EXPECT_EQ(PG_CORRUPT, pg_header_cache_probe(
		"model.geo", little, 4, 11, &info, nullptr));
	EXPECT_EQ(PG_CORRUPT, pg_header_cache_probe(
		"map.lm2", little, 4, 28, &info, nullptr));
	EXPECT_EQ(PG_INVALID, pg_header_cache_probe(
		"image.texture", little, 4, 3, &info, nullptr));
	EXPECT_EQ(PG_INVALID, pg_header_cache_probe(
		"image.texture", nullptr, 4, 64, &info, nullptr));
}

TEST(HeaderCache, PureProbeDoesNotImposeArchivePolicyLimit)
{
	const uint8_t large[] = {1, 0, 32, 0};
	pg_header_cache_info info = {};

	ASSERT_EQ(PG_OK, pg_header_cache_probe("model.geo", large, 4,
		UINT64_C(16) * 1024 * 1024, &info, nullptr));
	EXPECT_EQ(UINT64_C(2) * 1024 * 1024 + 5, info.size);
	const uint8_t maximum[] = {255, 255, 255, 255};

	ASSERT_EQ(PG_OK, pg_header_cache_probe("model.geo", maximum, 4,
		UINT64_C(1) << 33, &info, nullptr));
	EXPECT_EQ((uint64_t)UINT32_MAX + 4, info.size);
}

TEST(HeaderCache, ExtractReturnsOnlyTheInferredPrefixAndHonorsLimit)
{
	const uint8_t logical[] = {4, 0, 0, 0, 'h', 0, 'd', 'r', 'x'};
	pg_buffer header = {};

	ASSERT_EQ(PG_OK, pg_header_cache_extract("model.geo", logical,
		sizeof(logical), 8, &header, nullptr));
	ASSERT_EQ(8u, header.size);
	EXPECT_EQ(0, memcmp(header.data, logical, header.size));
	pg_buffer_free(&header);
	EXPECT_EQ(PG_LIMIT, pg_header_cache_extract("model.geo", logical,
		sizeof(logical), 7, &header, nullptr));
	EXPECT_EQ(nullptr, header.data);
	EXPECT_EQ(0u, header.size);
	ASSERT_EQ(PG_OK, pg_header_cache_extract("model.bin", logical,
		sizeof(logical), 0, &header, nullptr));
	EXPECT_EQ(nullptr, header.data);
	ASSERT_EQ(PG_OK, pg_header_cache_extract(
		"image.texture", nullptr, 0, 0, &header, nullptr));
	EXPECT_EQ(nullptr, header.data);
}

TEST(HeaderCache, StandaloneExtractionSupportsHeadersAboveArchivePolicy)
{
	const size_t header_size = 2 * 1024 * 1024 + 5;
	std::vector<uint8_t> logical(header_size + 1, 'x');

	logical[0] = 1;
	logical[1] = 0;
	logical[2] = 32;
	logical[3] = 0;
	pg_buffer header = {};

	ASSERT_EQ(PG_OK, pg_header_cache_extract("model.geo", logical.data(),
		logical.size(), header_size, &header, nullptr));
	ASSERT_EQ(header_size, header.size);
	EXPECT_EQ(0, memcmp(header.data, logical.data(), header.size));
	pg_buffer_free(&header);
	const uint8_t unaligned[] = {'x', 8, 0, 0, 0};
	pg_header_cache_info info = {};

	ASSERT_EQ(PG_OK, pg_header_cache_probe("model.geo", unaligned + 1,
		4, 64, &info, nullptr));
	EXPECT_EQ(12u, info.size);
}

static std::vector<uint8_t> cache_wire(uint32_t format)
{
	std::vector<uint8_t> wire;
	if (format == PG_HEADER_CACHE_PIGG_POOL)
		wire = {0xbc, 0x9a, 0, 0, 3, 0, 0, 0, 17, 0, 0, 0};
	else
		wire = {0, 0, 0, 0, 3, 0, 0, 0};
	const uint8_t elements[] = {
		3, 0, 0, 0, 'a', 0, 'b',
		0, 0, 0, 0,
		2, 0, 0, 0, 0xff, 0x01
	};
	wire.insert(wire.end(), elements, elements + sizeof(elements));
	return wire;
}

TEST(HeaderCache, CodecsPreserveOpaqueBytesAndSparseSlotIndices)
{
	for (uint32_t format : {PG_HEADER_CACHE_PIGG_POOL,
		PG_HEADER_CACHE_HOGG_DATALIST}) {
		SCOPED_TRACE(format);
		auto wire = cache_wire(format);
		pg_header_cache *cache = nullptr;
		pg_error error;

		ASSERT_EQ(PG_OK, pg_header_cache_decode(format, wire.data(),
			wire.size(), &cache, &error));
		memset(wire.data(), 0, wire.size());
		size_t count = 0;

		ASSERT_EQ(PG_OK,
			pg_header_cache_count(cache, &count, &error));
		ASSERT_EQ(3u, count);
		pg_header_cache_span spans[3] = {};

		for (size_t i = 0; i < count; ++i)
			ASSERT_EQ(PG_OK, pg_header_cache_get(
				cache, i, &spans[i], &error));
		ASSERT_EQ(3u, spans[0].size);
		EXPECT_EQ(0, memcmp(spans[0].data, "a\0b", 3));
		EXPECT_EQ(0u, spans[1].size);
		EXPECT_EQ(nullptr, spans[1].data);
		ASSERT_EQ(2u, spans[2].size);
		EXPECT_EQ(0xff, ((const uint8_t *)spans[2].data)[0]);
		pg_buffer encoded = {};

		ASSERT_EQ(PG_OK, pg_header_cache_encode(
			format, spans, count, &encoded, &error));
		auto expected = cache_wire(format);

		ASSERT_EQ(expected.size(), encoded.size);
		EXPECT_EQ(0,
			memcmp(encoded.data, expected.data(), encoded.size));
		pg_buffer_free(&encoded);
		pg_header_cache_span end = {(const void *)1, 9};

		EXPECT_EQ(PG_END,
			pg_header_cache_get(cache, count, &end, &error));
		EXPECT_EQ(nullptr, end.data);
		EXPECT_EQ(0u, end.size);
		EXPECT_EQ(PG_OK, pg_header_cache_close(&cache, &error));
		EXPECT_EQ(nullptr, cache);
		EXPECT_EQ(PG_OK, pg_header_cache_close(&cache, &error));
	}
}

TEST(HeaderCache, EmptyContainersAreValidAndRetainTheirWireHeaders)
{
	for (uint32_t format : {PG_HEADER_CACHE_PIGG_POOL,
		PG_HEADER_CACHE_HOGG_DATALIST}) {
		pg_buffer wire = {};

		ASSERT_EQ(PG_OK, pg_header_cache_encode(
			format, nullptr, 0, &wire, nullptr));
		EXPECT_EQ(format == PG_HEADER_CACHE_PIGG_POOL ? 12u : 8u,
			wire.size);
		pg_header_cache *cache = nullptr;

		ASSERT_EQ(PG_OK, pg_header_cache_decode(format, wire.data,
			wire.size, &cache, nullptr));
		size_t count = 7;

		EXPECT_EQ(PG_OK, pg_header_cache_count(cache, &count, nullptr));
		EXPECT_EQ(0u, count);
		pg_header_cache_span span = {};

		EXPECT_EQ(PG_END,
			pg_header_cache_get(cache, 0, &span, nullptr));
		pg_buffer_free(&wire);
		EXPECT_EQ(PG_OK, pg_header_cache_close(&cache, nullptr));
	}
}

TEST(HeaderCache, DecodeRejectsMalformedFramingWithoutPublishingHandle)
{
	for (uint32_t format : {PG_HEADER_CACHE_PIGG_POOL,
		PG_HEADER_CACHE_HOGG_DATALIST}) {
		auto valid = cache_wire(format);
		pg_header_cache *cache = nullptr;

		for (size_t size = 0; size < valid.size(); ++size) {
			SCOPED_TRACE(size);
			EXPECT_EQ(PG_CORRUPT, pg_header_cache_decode(format,
				valid.data(), size, &cache, nullptr));
			EXPECT_EQ(nullptr, cache);
		}
		auto changed = valid;

		changed[0] ^= 1;
		EXPECT_EQ(PG_CORRUPT, pg_header_cache_decode(format,
			changed.data(), changed.size(), &cache, nullptr));
		changed = valid;
		changed[4] = 4;
		EXPECT_EQ(PG_CORRUPT, pg_header_cache_decode(format,
			changed.data(), changed.size(), &cache, nullptr));
		changed = valid;
		changed.push_back(0);
		EXPECT_EQ(PG_CORRUPT, pg_header_cache_decode(format,
			changed.data(), changed.size(), &cache, nullptr));
		changed = valid;
		changed[format == PG_HEADER_CACHE_PIGG_POOL ? 12 : 8] = 255;
		EXPECT_EQ(PG_CORRUPT, pg_header_cache_decode(format,
			changed.data(), changed.size(), &cache, nullptr));
		EXPECT_EQ(nullptr, cache);
	}
}

TEST(HeaderCache, EncodeChecksRepresentabilityBeforeReadingPayload)
{
	const pg_header_cache_span huge = {(const void *)1, UINT32_MAX};
	pg_buffer wire = {};

	EXPECT_EQ(PG_LIMIT, pg_header_cache_encode(
		PG_HEADER_CACHE_PIGG_POOL, &huge, 1, &wire, nullptr));
	EXPECT_EQ(nullptr, wire.data);
	if (SIZE_MAX > UINT32_MAX) {
		const pg_header_cache_span excessive = {
			(const void *)1, (size_t)UINT32_MAX + 1
		};

		EXPECT_EQ(PG_LIMIT, pg_header_cache_encode(
			PG_HEADER_CACHE_HOGG_DATALIST, &excessive, 1,
			&wire, nullptr));
		EXPECT_EQ(PG_LIMIT, pg_header_cache_encode(
			PG_HEADER_CACHE_HOGG_DATALIST, &huge,
			(size_t)UINT32_MAX + 1, &wire, nullptr));
	}
	EXPECT_EQ(0u, wire.size);
}

TEST(HeaderCache, InvalidArgumentsInitializeOutputsAndDiagnostics)
{
	pg_header_recipe recipe;
	pg_header_cache_info info;
	pg_header_cache *cache = nullptr;
	pg_header_cache_span span;
	pg_buffer buffer = {};
	pg_error error;
	size_t count = 9;

	memset(&recipe, 0xff, sizeof(recipe));
	EXPECT_EQ(PG_INVALID,
		pg_header_cache_classify(nullptr, &recipe, &error));
	EXPECT_EQ(PG_INVALID, error.status);
	EXPECT_EQ(0u, recipe.mode);
	EXPECT_EQ(PG_INVALID,
		pg_header_cache_classify("", &recipe, nullptr));
	EXPECT_EQ(PG_INVALID,
		pg_header_cache_classify("file.geo", nullptr, nullptr));
	EXPECT_EQ(PG_INVALID, pg_header_cache_probe(
		nullptr, nullptr, 0, 0, &info, nullptr));
	EXPECT_EQ(PG_INVALID, pg_header_cache_extract(
		"file.geo", nullptr, 1, 1, &buffer, nullptr));
	EXPECT_EQ(PG_INVALID, pg_header_cache_decode(
		999, nullptr, 0, &cache, nullptr));
	EXPECT_EQ(PG_INVALID, pg_header_cache_decode(
		PG_HEADER_CACHE_PIGG_POOL, nullptr, 1, &cache, nullptr));
	EXPECT_EQ(PG_INVALID,
		pg_header_cache_count(nullptr, &count, nullptr));
	EXPECT_EQ(0u, count);
	EXPECT_EQ(PG_INVALID,
		pg_header_cache_get(nullptr, 0, &span, nullptr));
	EXPECT_EQ(nullptr, span.data);
	EXPECT_EQ(0u, span.size);
	EXPECT_EQ(PG_INVALID,
		pg_header_cache_encode(999, nullptr, 0, &buffer, nullptr));
	EXPECT_EQ(PG_INVALID, pg_header_cache_encode(
		PG_HEADER_CACHE_PIGG_POOL, nullptr, 1, &buffer, nullptr));
	const pg_header_cache_span missing = {nullptr, 1};

	EXPECT_EQ(PG_INVALID, pg_header_cache_encode(
		PG_HEADER_CACHE_PIGG_POOL, &missing, 1, &buffer, nullptr));
	EXPECT_EQ(PG_INVALID, pg_header_cache_close(nullptr, nullptr));
}

} /* namespace */
