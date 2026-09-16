/* Public API. Common contracts and outputs: docs/api.md. */
#ifndef PIGGLE_FILE_H
#define PIGGLE_FILE_H
#include <piggle/context.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Whole-file tasks. Use io.h for independent readers and writers. */
/* Release library-allocated buffer storage and reset both fields to zero.
 * buffer required. Immediate, no allocation/I/O/failure; no context required.
 * Does not change diagnostics. Empty release is a no-op. No pending operation
 * may borrow this output slot or its bytes. Serialize access to this buffer.
 */
PG_API void PG_CALL pg_buffer_free(pg_buffer *buffer);

/* Read entire selected logical file from zero into caller storage, verified
 * through EOF even when empty. bytes required; NULL allowed only at capacity=0.
 * Zero capacity also accepts non-NULL storage without accessing it.
 * Size>SIZE_MAX -> LIMIT; too small -> CAPACITY, untouched buffer/bytes=0.
 * Other failures may deliver a prefix: discard it. No NUL terminator or
 * returned allocation; independent of every existing reader's position.
 * Buffer/bytes borrow through return.
 * Owns reader cleanup; preserves the first error and exact count.
 */
PG_API pg_status PG_CALL pg_file_read_all(pg_file *file, void *buffer,
		size_t capacity, size_t *bytes, pg_error *error);

/* Read the complete logical selection into a new owned pg_buffer.
 * out required and initially {NULL, 0}. Inspect captured size once; exceeding
 * max_bytes or SIZE_MAX -> LIMIT before payload allocation/read. max_bytes=0
 * permits only empty files; it is not unlimited. Allocation failure -> NOMEM.
 * The limit bounds returned content, not decoder/index/scratch storage.
 * Allocate only the captured logical length; no NUL terminator. Use the same
 * verified reader/cleanup as read_all, with no implicit refresh or retry.
 * Publish out only after verification and all fallible cleanup succeed.
 * Empty success -> OK with NULL/0. Failure, including cleanup IO,
 * leaves NULL/0 and frees private bytes. Release success with pg_buffer_free.
 * Output slot borrows through return; partial bytes stay private.
 * No partial buffer is returned if later verification fails.
 */
PG_API pg_status PG_CALL pg_file_read_all_alloc(pg_file *file,
		size_t max_bytes, pg_buffer *out, pg_error *error);

/* Replace this exact physical copy with a complete logical buffer, even if
 * later hidden. Derive source/name from the selection; changed copy -> STALE.
 * Source must be WRITE. NULL buffer allowed only at size=0; an empty span is
 * never accessed. NULL entry -> zero defaults; original_name override INVALID.
 * Empty replacement is valid. Inputs borrow through return. Publication
 * may block.
 * Owns writer-open/write/finish/close. No retargeting or implicit refresh.
 */
PG_API pg_status PG_CALL pg_file_write_all(pg_file *file,
		const void *buffer, size_t size, const pg_entry_options *entry, pg_error *error);

/* Selected logical reader -> native writer -> validated finish. Preserve
 * mtime; flags=0 exclusive, OVERWRITE permits replacing regular files.
 * Reject aliases into retained source data, links and native races.
 * Checksum failure never publishes destination.
 */
PG_API pg_status PG_CALL pg_file_export(
		pg_file *file,
		const char *native_output,
		uint32_t flags,
		pg_error *error);

/* Immediate captured metadata, no native I/O. out required, zero on error.
 * Spans borrow file lifetime. Metadata survives source edits/deletion/detach;
 * later content access may report STALE. Serialize access to this handle.
 */
PG_API pg_status PG_CALL pg_file_inspect(pg_file *file, pg_file_info *out,
		pg_error *error);
/* Immediate next owned file from captured order; END clears out. NOMEM
 * leaves cursor position unchanged. Returned file outlives cursor close.
 */
PG_API pg_status PG_CALL pg_cursor_next(pg_cursor *cursor, pg_file **out,
		pg_error *error);

/* Verify captured copy by decoding and comparing its stored logical digest.
 * No stored digest -> NO_CHECKSUM. No reader position or view changes.
 * Changed copy -> STALE; malformed stream -> CORRUPT; mismatch -> CHECKSUM.
 */
PG_API pg_status PG_CALL pg_file_verify(
		pg_file *file,
		pg_error *error);

/* Delete exactly the selected physical copy, even if later hidden.
 * Source must be WRITE; attached publication uses control thread. Changed
 * selection -> STALE. Metadata handle survives and still needs close.
 * Never deletes all copies or directories. Check status on failure.
 */
PG_API pg_status PG_CALL pg_file_delete(
		pg_file *file,
		pg_error *error);

/* Release one owned reference; never commit or delete backing data.
 * Address required; NULL *handle succeeds. Accepted close clears it;
 * rejection leaves it owned. Cleanup may block and report IO, but the
 * reference stays consumed.
 */
PG_API pg_status PG_CALL pg_file_close(
		pg_file **file,
		pg_error *error);

/* Release one owned reference; never commit or delete backing data.
 * Address required; NULL *handle succeeds. Accepted close clears it;
 * rejection leaves it owned. Cleanup may block and report IO, but the
 * reference stays consumed.
 */
PG_API pg_status PG_CALL pg_cursor_close(
		pg_cursor **cursor,
		pg_error *error);

#ifdef __cplusplus
}
#endif
#endif
