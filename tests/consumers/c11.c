#include <piggle/piggle.h>
#include <string.h>

_Static_assert(sizeof(pg_status) == sizeof(int32_t), "status ABI");
_Static_assert(sizeof(pg_id) == sizeof(uint64_t), "identity ABI");

static int header_cache_contract(void)
{
	const unsigned char data[] = {4, 0, 0, 0, 1, 2, 0, 255};
	pg_header_recipe recipe;
	pg_header_cache_info info;
	pg_buffer header = {NULL, 0};
	pg_error error;
	int failed = pg_header_cache_classify("mesh.GEO", &recipe,
		&error) != PG_OK || recipe.mode != PG_HEADER_RECIPE_U32LE ||
		recipe.adjustment != 4;

	failed |= pg_header_cache_probe("mesh.geo", data, sizeof(data),
		sizeof(data), &info, &error) != PG_OK ||
		info.size != sizeof(data);
	failed |= pg_header_cache_extract("mesh.geo", data, sizeof(data),
		sizeof(data), &header, &error) != PG_OK;
	const uint32_t formats[] = {
		PG_HEADER_CACHE_PIGG_POOL, PG_HEADER_CACHE_HOGG_DATALIST};

	for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
		pg_header_cache_span spans[] = {
			{header.data, header.size}, {NULL, 0}};
		pg_buffer encoded = {NULL, 0};
		pg_header_cache *cache = NULL;
		pg_header_cache_span found;
		size_t count = 0;

		failed |= pg_header_cache_encode(formats[i], spans, 2,
			&encoded, &error) != PG_OK;
		failed |= pg_header_cache_decode(formats[i], encoded.data,
			encoded.size, &cache, &error) != PG_OK;
		pg_buffer_free(&encoded);
		failed |= pg_header_cache_count(cache, &count, &error) !=
			PG_OK ||
			count != 2;
		failed |= pg_header_cache_get(cache, 0, &found, &error) !=
			PG_OK ||
			found.size != sizeof(data);
		if (found.size == sizeof(data))
			failed |= memcmp(found.data, data, sizeof(data)) != 0;
		failed |= pg_header_cache_get(cache, 1, &found, &error) !=
			PG_OK ||
			found.data != NULL || found.size != 0;
		failed |= pg_header_cache_close(&cache, &error) != PG_OK ||
			cache != NULL;
	}
	pg_buffer_free(&header);
	return failed;
}

int piggle_c11_contract(void)
{
	if (header_cache_contract())
		return 1;
	pg_archive_build_options options = {{PG_HOGG10, 0, 0}, NULL, 0};
	pg_archive_builder *builder = NULL;
	pg_error contract_error;
	if (pg_archive_builder_create_config(NULL, "invalid", &options,
		&builder, &contract_error) != PG_INVALID ||
		builder)
		return 1;
	pg_source_storage_info storage;
	if (pg_source_set_staging_directory(NULL, NULL, &contract_error) !=
			PG_INVALID ||
		pg_source_storage_inspect(NULL, &storage, &contract_error) !=
			PG_INVALID ||
		storage.physical_length ||
		pg_source_compact(NULL, "invalid", NULL, &contract_error) !=
			PG_INVALID ||
		pg_archive_builder_create_session(NULL, "invalid", "invalid",
			&options, &builder, &contract_error) != PG_INVALID ||
		builder ||
		pg_archive_builder_resume(NULL, "invalid", &builder,
			&contract_error) != PG_INVALID ||
		builder ||
		pg_archive_builder_checkpoint(NULL, &contract_error) !=
			PG_INVALID ||
		pg_archive_builder_suspend(NULL, &contract_error) !=
			PG_INVALID ||
		pg_archive_builder_visit_inventory(
			NULL, NULL, NULL, &contract_error) != PG_INVALID ||
		pg_archive_builder_discard(NULL, &contract_error) != PG_INVALID)
		return 1;
	pg_stored_write_options stored = {0};
	pg_writer *writer = NULL;

	if (pg_writer_open_archive_builder_stored(NULL, "a", &stored,
		&writer, &contract_error) != PG_INVALID || writer)
		return 1;
	pg_context *context = NULL;
	pg_error error;
	char name[32];
	size_t required = 0;
	pg_status status = pg_context_open(&context, &error);

	if (status != PG_OK || error.status != status || !context)
		return 1;
	status = pg_name_normalize(context, "Dir\\A", name,
		sizeof(name), &required, &error);
	int result = status != PG_OK || strcmp(name, "dir/a") ||
		required != 6;

	status = pg_context_close(&context, &error);
	return result || status != PG_OK || context != NULL;
}
