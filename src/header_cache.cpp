#include "header_cache_internal.hpp"
#include "internal.hpp"

struct pg_header_cache {
	size_t count;
	pg_header_cache_span *entries;
};

static uint32_t pg_cache_u32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
		((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void pg_cache_put_u32(uint8_t *bytes, uint32_t value)
{
	for (unsigned int i = 0; i < 4; ++i)
		bytes[i] = (uint8_t)(value >> (8 * i));
}

static int pg_cache_format_valid(uint32_t format)
{
	return format == PG_HEADER_CACHE_PIGG_POOL ||
		format == PG_HEADER_CACHE_HOGG_DATALIST;
}

/* Validate header/count, scan bounded lengths and visit slots. */
pg_status pg_cache_frame_parse(uint32_t format, uint32_t magic,
	uint64_t size, pg_cache_frame_read_fn read, void *read_state,
	pg_cache_frame_visit_fn visit, void *visit_state, uint32_t *count)
{
	if (count)
		*count = 0;
	if (!count || !read || !pg_cache_format_valid(format) ||
		(format == PG_HEADER_CACHE_HOGG_DATALIST && magic))
		return PG_INVALID;
	uint8_t header[12];
	size_t header_size = format == PG_HEADER_CACHE_PIGG_POOL ? 12 : 8;

	if (size < header_size)
		return PG_CORRUPT;
	pg_status status = read(read_state, 0, header, header_size);

	if (status != PG_OK)
		return status;
	if (pg_cache_u32(header) != magic)
		return PG_CORRUPT;
	if (format == PG_HEADER_CACHE_PIGG_POOL &&
		pg_cache_u32(header + 8) != size - header_size)
		return PG_CORRUPT;
	uint32_t entries = pg_cache_u32(header + 4);
	uint64_t position = header_size;

	if (entries > (size - position) / 4)
		return PG_CORRUPT;
	for (uint32_t i = 0; i < entries; ++i) {
		uint8_t word[4];

		if (size - position < sizeof(word))
			return PG_CORRUPT;
		status = read(read_state, position, word, sizeof(word));
		if (status != PG_OK)
			return status;
		position += sizeof(word);
		uint32_t length = pg_cache_u32(word);

		if (length > size - position)
			return PG_CORRUPT;
		if (visit) {
			status = visit(visit_state, i, position, length);
			if (status != PG_OK)
				return status;
		}
		position += length;
	}
	if (position != size)
		return PG_CORRUPT;
	*count = entries;
	return PG_OK;
}

/* Check wire representability and sum exact framed length. */
pg_status pg_cache_frame_measure(uint32_t format, uint32_t magic,
	size_t count, pg_cache_frame_span_fn get, void *state, uint64_t *out)
{
	if (out)
		*out = 0;
	if (!out || !pg_cache_format_valid(format) || (count && !get) ||
		(format == PG_HEADER_CACHE_HOGG_DATALIST && magic))
		return PG_INVALID;
	if (count > UINT32_MAX)
		return PG_LIMIT;
	uint64_t header_size = format == PG_HEADER_CACHE_PIGG_POOL ? 12 : 8;
	uint64_t total = header_size;

	for (uint32_t i = 0; i < count; ++i) {
		pg_header_cache_span span = {};
		pg_status status = get(state, i, &span);

		if (status != PG_OK)
			return status;
		if (span.size && !span.data)
			return PG_INVALID;
		if (span.size > UINT32_MAX || total > UINT64_MAX - 4 ||
			span.size > UINT64_MAX - total - 4)
			return PG_LIMIT;
		total += 4 + (uint64_t)span.size;
		if (format == PG_HEADER_CACHE_PIGG_POOL &&
			total - header_size > UINT32_MAX)
			return PG_LIMIT;
	}
	*out = total;
	return PG_OK;
}

/* Measure then stream container header, lengths and borrowed payloads. */
pg_status pg_cache_frame_emit(uint32_t format, uint32_t magic,
	size_t count, pg_cache_frame_span_fn get, void *state,
	pg_cache_frame_write_fn write, void *write_state)
{
	if (!write)
		return PG_INVALID;
	uint64_t total = 0;
	pg_status status = pg_cache_frame_measure(
		format, magic, count, get, state, &total);

	if (status != PG_OK)
		return status;
	uint8_t header[12] = {};
	size_t header_size = format == PG_HEADER_CACHE_PIGG_POOL ? 12 : 8;

	pg_cache_put_u32(header, magic);
	pg_cache_put_u32(header + 4, (uint32_t)count);
	if (format == PG_HEADER_CACHE_PIGG_POOL)
		pg_cache_put_u32(header + 8, (uint32_t)(total - header_size));
	status = write(write_state, header, header_size);
	for (uint32_t i = 0; status == PG_OK && i < count; ++i) {
		pg_header_cache_span span = {};

		status = get(state, i, &span);
		if (status != PG_OK)
			break;
		uint8_t word[4];

		pg_cache_put_u32(word, (uint32_t)span.size);
		status = write(write_state, word, sizeof(word));
		if (status == PG_OK && span.size)
			status = write(write_state, span.data, span.size);
	}
	return status;
}

static int pg_cache_extension_is(const char *left, const char *right)
{
	while (*left && *right) {
		unsigned char value = (unsigned char)*left++;

		if (value >= 'A' && value <= 'Z')
			value = (unsigned char)(value + ('a' - 'A'));
		if (value != (unsigned char)*right++)
			return 0;
	}
	return !*left && !*right;
}

/* Classify the exact case-insensitive resource extension. */
PG_API pg_status PG_CALL pg_header_cache_classify(const char *name,
	pg_header_recipe *out, pg_error *error)
{
	if (out)
		memset(out, 0, sizeof(*out));
	if (!out || !name || !*name)
		return pg_result(PG_INVALID, error);
	const char *extension = strrchr(name, '.');

	if (!extension)
		return pg_result(PG_OK, error);
	if (pg_cache_extension_is(extension, ".anm") ||
		pg_cache_extension_is(extension, ".geo") ||
		pg_cache_extension_is(extension, ".geolm") ||
		pg_cache_extension_is(extension, ".lgeo")) {
		out->mode = PG_HEADER_RECIPE_U32LE;
		out->adjustment = 4;
	} else if (pg_cache_extension_is(extension, ".bgeo")) {
		out->mode = PG_HEADER_RECIPE_U32BE;
		out->adjustment = 4;
	} else if (pg_cache_extension_is(extension, ".texture") ||
		pg_cache_extension_is(extension, ".anim")) {
		out->mode = PG_HEADER_RECIPE_U32LE;
	} else if (pg_cache_extension_is(extension, ".lm2")) {
		out->mode = PG_HEADER_RECIPE_PREFIX;
		out->prefix_size = 29;
	}
	return pg_result(PG_OK, error);
}

/* Validate, classify, gate on four nonzero bytes and bound size. */
PG_API pg_status PG_CALL pg_header_cache_probe(const char *name,
	const void *prefix, size_t prefix_size, uint64_t logical_size,
	pg_header_cache_info *out, pg_error *error)
{
	if (out)
		memset(out, 0, sizeof(*out));
	if (!out || (prefix_size && !prefix) || prefix_size > logical_size)
		return pg_result(PG_INVALID, error);
	pg_header_recipe recipe = {};
	pg_status status = pg_header_cache_classify(name, &recipe, NULL);

	if (status != PG_OK)
		return pg_result(status, error);
	if (recipe.mode == PG_HEADER_RECIPE_NONE || logical_size < 4)
		return pg_result(PG_OK, error);
	if (prefix_size < 4)
		return pg_result(PG_CAPACITY, error);
	const uint8_t *bytes = (const uint8_t *)prefix;
	uint32_t word = pg_cache_u32(bytes);

	if (!word)
		return pg_result(PG_OK, error);
	if (recipe.mode == PG_HEADER_RECIPE_U32BE)
		word = ((uint32_t)bytes[0] << 24) |
			((uint32_t)bytes[1] << 16) |
			((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
	uint64_t size = recipe.mode == PG_HEADER_RECIPE_PREFIX
		? recipe.prefix_size : (uint64_t)word + recipe.adjustment;

	if (size > logical_size)
		return pg_result(PG_CORRUPT, error);
	out->recipe = recipe;
	out->size = size;
	return pg_result(PG_OK, error);
}

/* Probe, enforce caller limit, allocate/copy only inferred bytes. */
PG_API pg_status PG_CALL pg_header_cache_extract(const char *name,
	const void *data, size_t size, uint64_t limit, pg_buffer *out,
	pg_error *error)
{
	if (out)
		memset(out, 0, sizeof(*out));
	if (!out)
		return pg_result(PG_INVALID, error);
	pg_header_cache_info info = {};
	pg_status status = pg_header_cache_probe(
		name, data, size, size, &info, NULL);

	if (status != PG_OK)
		return pg_result(status, error);
	if (info.size > limit)
		return pg_result(PG_LIMIT, error);
	if (!info.size)
		return pg_result(PG_OK, error);
	void *bytes = malloc((size_t)info.size);

	if (!bytes)
		return pg_result(PG_NOMEM, error);
	memcpy(bytes, data, (size_t)info.size);
	out->data = bytes;
	out->size = (size_t)info.size;
	return pg_result(PG_OK, error);
}

struct pg_cache_memory {
	const uint8_t *data;
	size_t size;
};

static pg_status pg_cache_memory_read(void *state, uint64_t offset,
	void *out, size_t size)
{
	pg_cache_memory *memory = (pg_cache_memory *)state;

	if (offset > memory->size || size > memory->size - offset)
		return PG_CORRUPT;
	if (size)
		memcpy(out, memory->data + (size_t)offset, size);
	return PG_OK;
}

struct pg_cache_collect {
	const uint8_t *data;
	pg_header_cache_span *entries;
};

static pg_status pg_cache_collect_slot(void *state, uint32_t index,
	uint64_t offset, uint32_t size)
{
	pg_cache_collect *collect = (pg_cache_collect *)state;

	collect->entries[index].data = size ? collect->data + offset : NULL;
	collect->entries[index].size = size;
	return PG_OK;
}

/* Validate framing; allocate owned bytes/spans; publish on success. */
PG_API pg_status PG_CALL pg_header_cache_decode(uint32_t format,
	const void *data, size_t size, pg_header_cache **out, pg_error *error)
{
	if (out)
		*out = NULL;
	if (!out || (size && !data) || !pg_cache_format_valid(format))
		return pg_result(PG_INVALID, error);
	uint32_t magic = format == PG_HEADER_CACHE_PIGG_POOL ? 0x9abc : 0;
	pg_cache_memory input = {(const uint8_t *)data, size};
	uint32_t count = 0;
	pg_status status = pg_cache_frame_parse(format, magic, size,
		pg_cache_memory_read, &input, NULL, NULL, &count);

	if (status != PG_OK)
		return pg_result(status, error);
	uint64_t metadata = sizeof(pg_header_cache) +
		(uint64_t)count * sizeof(pg_header_cache_span);

	if (metadata > SIZE_MAX || size > SIZE_MAX - (size_t)metadata)
		return pg_result(PG_LIMIT, error);
	pg_header_cache *cache =
		(pg_header_cache *)malloc((size_t)metadata + size);

	if (!cache)
		return pg_result(PG_NOMEM, error);
	cache->count = count;
	cache->entries = (pg_header_cache_span *)(cache + 1);
	uint8_t *bytes = (uint8_t *)(cache->entries + count);

	memcpy(bytes, data, size);
	pg_cache_memory copied = {bytes, size};
	pg_cache_collect collect = {bytes, cache->entries};

	status = pg_cache_frame_parse(format, magic, size,
		pg_cache_memory_read, &copied, pg_cache_collect_slot,
		&collect, &count);
	if (status != PG_OK) {
		free(cache);
		return pg_result(status, error);
	}
	*out = cache;
	return pg_result(PG_OK, error);
}

/* Validate handle/output and copy slot count. */
PG_API pg_status PG_CALL pg_header_cache_count(const pg_header_cache *cache,
	size_t *out, pg_error *error)
{
	if (out)
		*out = 0;
	if (!cache || !out)
		return pg_result(PG_INVALID, error);
	*out = cache->count;
	return pg_result(PG_OK, error);
}

/* Validate handle/index; return immutable borrowed span. */
PG_API pg_status PG_CALL pg_header_cache_get(const pg_header_cache *cache,
	size_t index, pg_header_cache_span *out, pg_error *error)
{
	if (out)
		memset(out, 0, sizeof(*out));
	if (!cache || !out)
		return pg_result(PG_INVALID, error);
	if (index >= cache->count)
		return pg_result(PG_END, error);
	*out = cache->entries[index];
	return pg_result(PG_OK, error);
}

static pg_status pg_cache_array_get(void *state, uint32_t index,
	pg_header_cache_span *out)
{
	const pg_header_cache_span *entries =
		(const pg_header_cache_span *)state;

	*out = entries[index];
	return PG_OK;
}

struct pg_cache_sink {
	uint8_t *data;
	size_t size;
	size_t position;
};

static pg_status pg_cache_memory_write(void *state, const void *data,
	size_t size)
{
	pg_cache_sink *sink = (pg_cache_sink *)state;

	if (size > sink->size - sink->position)
		return PG_LIMIT;
	if (size)
		memcpy(sink->data + sink->position, data, size);
	sink->position += size;
	return PG_OK;
}

/* Measure, allocate, emit to bounded owned output, then publish. */
PG_API pg_status PG_CALL pg_header_cache_encode(uint32_t format,
	const pg_header_cache_span *entries, size_t count, pg_buffer *out,
	pg_error *error)
{
	if (out)
		memset(out, 0, sizeof(*out));
	if (!out || (count && !entries))
		return pg_result(PG_INVALID, error);
	uint32_t magic = format == PG_HEADER_CACHE_PIGG_POOL ? 0x9abc : 0;
	uint64_t size = 0;
	pg_status status = pg_cache_frame_measure(format, magic, count,
		pg_cache_array_get, (void *)entries, &size);

	if (status != PG_OK)
		return pg_result(status, error);
	if (size > SIZE_MAX)
		return pg_result(PG_LIMIT, error);
	void *data = malloc((size_t)size);

	if (!data)
		return pg_result(PG_NOMEM, error);
	pg_cache_sink sink = {(uint8_t *)data, (size_t)size, 0};

	status = pg_cache_frame_emit(format, magic, count, pg_cache_array_get,
		(void *)entries, pg_cache_memory_write, &sink);
	if (status != PG_OK) {
		free(data);
		return pg_result(status, error);
	}
	out->data = data;
	out->size = (size_t)size;
	return pg_result(PG_OK, error);
}

/* Consume one owned allocation; NULL owned handle succeeds. */
PG_API pg_status PG_CALL pg_header_cache_close(pg_header_cache **cache,
	pg_error *error)
{
	if (!cache)
		return pg_result(PG_INVALID, error);
	free(*cache);
	*cache = NULL;
	return pg_result(PG_OK, error);
}
