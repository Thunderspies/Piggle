#ifndef PIGGLE_HEADER_CACHE_INTERNAL_HPP
#define PIGGLE_HEADER_CACHE_INTERNAL_HPP
#include <piggle/header_cache.h>

/* Exact read-at and write callbacks; non-OK aborts immediately. Offsets and
 * lengths are relative to one complete uncompressed cache container.
 */
typedef pg_status (*pg_cache_frame_read_fn)(void *, uint64_t, void *,
		size_t);
typedef pg_status (*pg_cache_frame_visit_fn)(void *, uint32_t, uint64_t,
		uint32_t);
typedef pg_status (*pg_cache_frame_span_fn)(void *, uint32_t,
		pg_header_cache_span *);
typedef pg_status (*pg_cache_frame_write_fn)(void *, const void *, size_t);

/* Validate framing with bounded reads and no allocation. magic selects a
 * PIGG pool's expected magic or the HOGG version (zero). Visits can occur
 * before a later malformed element is detected; keep effects private.
 * count required, zero on failure; visit may be NULL for validation only.
 */
pg_status pg_cache_frame_parse(uint32_t format, uint32_t magic,
		uint64_t size, pg_cache_frame_read_fn read, void *read_state,
		pg_cache_frame_visit_fn visit, void *visit_state,
		uint32_t *count);

/* No allocation. get is called once per slot in ascending index order,
 * must return valid spans and be repeatable while callers measure then
 * emit. Empty slots are retained.
 */
pg_status pg_cache_frame_measure(uint32_t format, uint32_t magic,
		size_t count, pg_cache_frame_span_fn get, void *state,
		uint64_t *out);

/* Validate/measure first, then stream header, lengths and borrowed payloads.
 * get sees two ascending passes, each starting at zero. Sink failure can
 * leave a partial private output; no rollback is supplied.
 */
pg_status pg_cache_frame_emit(uint32_t format, uint32_t magic,
		size_t count, pg_cache_frame_span_fn get, void *state,
		pg_cache_frame_write_fn write, void *write_state);
#endif
