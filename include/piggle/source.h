/* Existing archive/directory: open, read/write by name, pack/unpack, query.
 * Source reads/writes use source-local visibility, independent of a tree.
 * New archive construction: archive_builder.h. Contracts: docs/api.md.
 */
#ifndef PIGGLE_SOURCE_H
#define PIGGLE_SOURCE_H
#include <piggle/file.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Open an existing archive or loose directory; no attachment. Archive names
 * are indexed synchronously; a loose root is not recursively scanned.
 * NULL options -> AUTO/READ. Detect content, not suffix. No implicit
 * creation/recovery. Pending HOGG redo -> RECOVERY_REQUIRED and NULL out.
 * Same-context native alias already open -> BUSY. Writable HOGGs also hold
 * an exclusive OS lease through final reference release; contention -> BUSY.
 * Owned source on success.
 * Standalone source control calls serialize; attached controls use tree
 * thread. Relative paths require stable cwd until completion.
 */
PG_API pg_status PG_CALL pg_source_open(
		pg_context *context,
		const char *native_path,
		const pg_source_options *options,
		pg_source **out,
		pg_error *error);

/* Immediate snapshot; out required, initialized even on error. native_path
 * borrows source lifetime. No native I/O. Serialize with source control.
 */
PG_API pg_status PG_CALL pg_source_inspect(pg_source *source,
		pg_source_info *out, pg_error *error);

/* Common tasks; named streaming uses pg_reader_open_source in io.h. */
/* Read a named visible logical file into caller storage. Compose
 * source_find, file_read_all and file_close; no caller-owned temporary.
 * Same normalization/selection/errors as source_find. Capture one selection;
 * never retry lookup or switch copies if content becomes STALE.
 * bytes required, initially zero. NULL allowed only at capacity=0; empty
 * spans are never accessed. CAPACITY leaves buffer untouched and bytes=0.
 * Discard any prefix from other failures. Size>SIZE_MAX -> LIMIT.
 * No returned allocation or NUL terminator;
 * empty files still verified.
 * Name/buffer/bytes borrow through return. Close all private handles; retain the
 * first error, report cleanup failure only if work succeeded. Serialize with
 * source control; allowed during a read-only observer callback.
 */
PG_API pg_status PG_CALL pg_source_read_all(pg_source *source,
		const char *name, void *buffer, size_t capacity, size_t *bytes,
		pg_error *error);

/* Named visible file -> owned buffer. Compose source_find, file_read_all_alloc
 * and file_close, retaining one selection through sizing, reading and cleanup.
 * Same lookup errors; never retry/switch winners. max_bytes bounds logical
 * content (zero permits empty only), not total working memory. Too large ->
 * LIMIT before payload allocation/read. out required and initially NULL/0.
 * Publish out only on success after all private cleanup; failure frees private
 * bytes and leaves out empty. No NUL terminator. Empty success is OK/NULL/0;
 * release with pg_buffer_free. Name and output borrow through return. Same
 * source control and callback rules as source_read_all.
 */
PG_API pg_status PG_CALL pg_source_read_all_alloc(pg_source *source,
		const char *name, size_t max_bytes, pg_buffer *out,
		pg_error *error);

/* Complete logical buffer, including empty, into source-visible winner/add.
 * Implied directory or visible file ancestor -> CONFLICT.
 * Uses writer_open_source, including its revalidation.
 * NULL entry -> zero defaults. No size descriptor, expected lengths derived.
 * buffer may be NULL only at size=0; empty spans are never accessed. Result
 * required and exact on failure. Inputs borrow through return. Owns writer,
 * including finish and close; this call may block during publication.
 */
PG_API pg_status PG_CALL pg_source_write_all(pg_source *destination,
		const char *name, const void *buffer, size_t size,
		const pg_entry_options *entry,
		pg_error *error);

/* Native reader -> source writer -> validated finish. Preserve native mtime;
 * normalize virtual name, keep native spelling. Explicit destination;
 * input aliasing replacement target -> CONFLICT. No cached header.
 * Same hierarchy checks as source_write_all; no hidden mutation.
 * All caller storage is borrowed through return.
 */
PG_API pg_status PG_CALL pg_source_import(
		pg_source *destination,
		const char *name,
		const char *native_input,
		uint32_t compression,
		pg_error *error);

/* Selected logical reader -> source writer. Preserve timestamp; retain
 * cached header for archives, omit it for loose output. Use canonical name
 * for additions and preserve existing original name for replacements.
 * Same hierarchy checks as source_write_all; no hidden mutation.
 * Reject input aliasing replacement copy; validate before publication.
 */
PG_API pg_status PG_CALL pg_source_copy(
		pg_source *destination,
		const char *name,
		pg_file *input,
		uint32_t compression,
		pg_error *error);

/* Export one named visible file: find once, file_export, close selection.
 * Logical bytes and mtime; flags=0 exclusive or OVERWRITE. Same lookup,
 * verification, native alias and publication rules as file_export. No retry
 * or reselection on STALE. Inputs borrow through return. Close all private
 * handles; preserve the first error. Post-publication cleanup failure
 * returns PG_COMMITTED with the underlying error in pg_error.cause.
 * Source control rules throughout; not permitted in observer callbacks.
 */
PG_API pg_status PG_CALL pg_source_export(pg_source *source,
		const char *name, const char *native_output, uint32_t flags, pg_error *error);

/* Refresh the loose root and capture visible source files, create private
 * builder, copy entries, finish once. No prior subtree request is needed.
 * No tree or attachment is created. Same source-local visibility as
 * source_files. Explicit PIGG2/HOGG10. Preserve original names,
 * timestamps and headers. Omit hidden copies/empty directories. No publication
 * on pre-commit failure; a valid empty archive is published. Changes during
 * reads -> STALE/RETRY.
 */
PG_API pg_status PG_CALL pg_source_pack(
		pg_source *source,
		const char *native_archive,
		uint32_t format,
		const pg_pack_options *options,
		pg_error *error);

/* Refresh the loose root and capture source_files visibility without a prior
 * subtree request, tree or attachment. Preflight
 * native name collisions, then export each under an existing native root
 * with canonical names. Zero/OVERWRITE flags; preserve timestamps.
 * Root/ancestors anchored against substitution
 * and links. Not batch-atomic: each file publishes separately. Between-file
 * failure preserves earlier items and returns PG_PARTIAL. Created parent
 * directories may remain on failure. Aliasing source data -> CONFLICT.
 */
PG_API pg_status PG_CALL pg_source_unpack(
		pg_source *source,
		const char *native_directory,
		uint32_t flags,
		pg_error *error);

/* Normalize and select the source-visible physical winner; no tree needed.
 * Within a requested loose subtree, use its indexed result, including
 * absence. Outside it, synchronously probe the exact loose path and retain
 * its metadata for later rescan. No subtree request or watch is implied.
 * Directory at the exact name -> CONFLICT; absent file -> NOT_FOUND.
 * Owned file, NULL on failure. Selection never changes to another winner.
 */
PG_API pg_status PG_CALL pg_source_find(
		pg_source *source,
		const char *name,
		pg_file **out,
		pg_error *error);

/* Capture indexed source-visible files recursively below a prefix, without
 * native directory I/O or refresh. NULL/empty prefix means root. Loose
 * sources require a covering source_request_subtree call; otherwise INVALID
 * and NULL out. Archives need no request. Exact visible file -> CONFLICT;
 * missing directory -> empty cursor. Canonical lexical order; owned cursor
 * retains captured metadata despite later changes. NULL out on failure.
 */
PG_API pg_status PG_CALL pg_source_files(
		pg_source *source,
		const char *prefix,
		pg_cursor **out,
		pg_error *error);

/* Discover and retain a complete loose subtree without creating a cursor.
 * NULL/empty prefix means root; a parent request covers descendants.
 * Exact file -> CONFLICT; missing directory is retained as empty. Repeating
 * a request refreshes it. Archives use their open-time index without I/O.
 * Failed or racing scans preserve the prior observation; races -> RETRY.
 * An attached source uses the tree control thread, but this source-local
 * request does not create a tree watch scope. No callbacks or publication.
 */
PG_API pg_status PG_CALL pg_source_request_subtree(pg_source *source,
		const char *prefix, pg_error *error);

/* Refresh the archive index or previously requested loose paths/subtrees.
 * Unrequested loose paths are not scanned. Unchanged scans preserve copy
 * generations. Queue watched tree events if attached. Races -> RETRY with
 * prior cached observations preserved. No callbacks or disk commitment.
 */
PG_API pg_status PG_CALL pg_source_rescan(
		pg_source *source,
		pg_error *error);

/* Validate every physical user record, including hidden copies, all format
 * structure, codecs and available hashes. Absent digest permits readable
 * content. No rescan/publication or changes to existing reader positions.
 */
PG_API pg_status PG_CALL pg_source_validate(
		pg_source *source,
		pg_error *error);

/* Explicit HOGG redo recovery, including after open failed. Requires native
 * write access. Validate profile, replay idempotently, flush/clear journal,
 * rescan and publish an already-open source. Attached source control thread
 * required; an active staged writer -> BUSY. Idle readers may remain open.
 * Recovery serializes with read operations. Unknown profile
 * -> UNSUPPORTED, malformed -> CORRUPT, no guessed repair. Result required
 * for the blocking call.
 */
PG_API pg_status PG_CALL pg_source_recover(
		pg_context *context,
		const char *native_path,
		pg_error *error);

/* Delete backing archive or recursively unlink loose root. Must be WRITE,
 * detached, without live readers/writers; else READ_ONLY/BUSY. Never follow
 * links. Errors after an unlink return PG_PARTIAL or PG_INDETERMINATE.
 * Handle remains inspectable and must still be closed.
 */
PG_API pg_status PG_CALL pg_source_delete(
		pg_source *source,
		pg_error *error);

/* Release one owned reference; never commit or delete backing data.
 * Address required; NULL *handle succeeds. Accepted close clears it;
 * rejection leaves it owned. Cleanup may block and report IO, but the
 * reference stays consumed.
 */
PG_API pg_status PG_CALL pg_source_close(
		pg_source **source,
		pg_error *error);

#ifdef __cplusplus
}
#endif
#endif
