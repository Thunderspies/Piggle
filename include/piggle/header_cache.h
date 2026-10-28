/* Standalone resource-header inference and cache-container codecs. */
#ifndef PIGGLE_HEADER_CACHE_H
#define PIGGLE_HEADER_CACHE_H

#include <piggle/file.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Recipe lengths include the complete cached prefix. Length-word recipes
 * read an unsigned word in their named byte order, then add adjustment.
 */
enum {
	PG_HEADER_RECIPE_NONE = 0,
	PG_HEADER_RECIPE_PREFIX,
	PG_HEADER_RECIPE_U32LE,
	PG_HEADER_RECIPE_U32BE
};
typedef struct pg_header_recipe {
	uint32_t mode;
	uint32_t prefix_size;
	int32_t adjustment;
} pg_header_recipe;

/* Resolved automatic recipe and its prefix length; NONE has zero fields. */
typedef struct pg_header_cache_info {
	pg_header_recipe recipe;
	uint64_t size;
} pg_header_cache_info;

/* Classify the exact final extension, ignoring ASCII case. anm/geo/geolm/
 * lgeo use U32LE+4, bgeo U32BE+4, texture/anim U32LE, lm2 PREFIX/29.
 * Every other extension, including wtex/dcache, returns NONE successfully.
 * name must be nonempty; no filesystem, context, or archive is accessed.
 * out required, zero on failure. Inputs borrow through return. Reentrant.
 */
PG_API pg_status PG_CALL pg_header_cache_classify(const char *name,
		pg_header_recipe *out, pg_error *error);

/* Infer from leading logical bytes before compression. prefix_size is the
 * available prefix length, at most logical_size. NULL prefix is allowed
 * only at zero size. Recognized resources shorter than four bytes, or with
 * a zero first word, resolve to NONE. Otherwise four prefix bytes suffice;
 * fewer available bytes return CAPACITY. Lengths exceeding logical_size
 * return CORRUPT. This pure probe imposes no archive policy size limit.
 * out required, zero on failure. No allocation/I/O. Reentrant; all inputs
 * and output borrow through return and must not overlap.
 */
PG_API pg_status PG_CALL pg_header_cache_probe(const char *name,
		const void *prefix, size_t prefix_size, uint64_t logical_size,
		pg_header_cache_info *out, pg_error *error);

/* Infer and copy a header from complete logical data into an owned buffer.
 * NULL data is allowed only at size=0. limit bounds returned bytes, with
 * zero permitting an absent header only. Too large -> LIMIT; allocation
 * failure -> NOMEM. No archive policy cap is imposed. out required and
 * initially empty; success without a header and every failure leave NULL/0.
 * Release with pg_buffer_free. No filesystem/context needed. Reentrant;
 * borrowed inputs/output must not overlap and expire after return.
 */
PG_API pg_status PG_CALL pg_header_cache_extract(const char *name,
		const void *data, size_t size, uint64_t limit, pg_buffer *out,
		pg_error *error);

/* PIGG_POOL is a complete 0x9abc data pool; HOGG_DATALIST is a complete
 * decoded version-0 DataList. These codecs neither compress nor replay
 * journals. DataList slots can also contain opaque names or other bytes.
 */
enum { PG_HEADER_CACHE_PIGG_POOL = 1, PG_HEADER_CACHE_HOGG_DATALIST };

/* Immutable owned decoded container; independent of archives and contexts. */
typedef struct pg_header_cache pg_header_cache;

/* Borrowed opaque bytes; empty slots use NULL/0 and retain their index. */
typedef struct pg_header_cache_span {
	const void *data;
	size_t size;
} pg_header_cache_span;

/* Decode exactly size bytes, retaining a private copy. format must name a
 * codec above. NULL data only at zero size; empty wire input is CORRUPT.
 * Reject invalid magic/version, truncation, inconsistent counts or lengths,
 * and trailing bytes with CORRUPT. Allocation failure -> NOMEM; an
 * unrepresentable allocation -> LIMIT. out required, NULL on failure.
 * Borrowed input expires on return. No filesystem/context. Reentrant.
 */
PG_API pg_status PG_CALL pg_header_cache_decode(uint32_t format,
		const void *data, size_t size, pg_header_cache **out,
		pg_error *error);

/* Count every slot, including empty/free slots. Both arguments required;
 * zero output on failure. No allocation/I/O. Concurrent reads allowed.
 */
PG_API pg_status PG_CALL pg_header_cache_count(const pg_header_cache *cache,
		size_t *out, pg_error *error);

/* Get one zero-based slot; out required, NULL/0 on failure. An index at or
 * beyond count returns END. Returned bytes borrow cache lifetime; they are
 * immutable and remain valid until close. No allocation/I/O. Concurrent
 * reads allowed; close must not overlap reads or borrow these spans.
 */
PG_API pg_status PG_CALL pg_header_cache_get(const pg_header_cache *cache,
		size_t index, pg_header_cache_span *out, pg_error *error);

/* Encode slots in order, retaining empty indices. entries may be NULL only
 * at count=0; each data pointer may be NULL only at size=0. Counts/lengths
 * must fit u32, and PIGG framed elements must together fit u32. Overflow
 * -> LIMIT before output allocation; unknown format -> INVALID.
 * out required and initially empty; every failure leaves NULL/0. Release
 * success with pg_buffer_free. Inputs borrow through return and must not
 * overlap output. No archive/context/filesystem. Reentrant.
 */
PG_API pg_status PG_CALL pg_header_cache_encode(uint32_t format,
		const pg_header_cache_span *entries, size_t count,
		pg_buffer *out, pg_error *error);

/* Free one owned container and clear it. Address required; NULL *cache is
 * successful. No I/O/allocation. Serialize close with reads and borrowed
 * spans; no other handle is retained. No context needed.
 */
PG_API pg_status PG_CALL pg_header_cache_close(pg_header_cache **cache,
		pg_error *error);

#ifdef __cplusplus
}
#endif
#endif
