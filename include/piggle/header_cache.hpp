/* Exact imports; no wrapper implementation or additional ABI. */
#ifndef PIGGLE_HEADER_CACHE_HPP
#define PIGGLE_HEADER_CACHE_HPP
#include <piggle/header_cache.h>
#include <piggle/file.hpp>

namespace piggle
{
using ::pg_header_recipe;
using header_recipe = ::pg_header_recipe;
using ::pg_header_cache_info;
using header_cache_info = ::pg_header_cache_info;
using ::pg_header_cache;
using header_cache = ::pg_header_cache;
using ::pg_header_cache_span;
using header_cache_span = ::pg_header_cache_span;
using ::PG_HEADER_RECIPE_NONE;
using ::PG_HEADER_RECIPE_PREFIX;
using ::PG_HEADER_RECIPE_U32LE;
using ::PG_HEADER_RECIPE_U32BE;
using ::PG_HEADER_CACHE_PIGG_POOL;
using ::PG_HEADER_CACHE_HOGG_DATALIST;
using ::pg_header_cache_classify;
using ::pg_header_cache_probe;
using ::pg_header_cache_extract;
using ::pg_header_cache_decode;
using ::pg_header_cache_count;
using ::pg_header_cache_get;
using ::pg_header_cache_encode;
using ::pg_header_cache_close;
} /* namespace piggle */
#endif
