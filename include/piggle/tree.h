/* Ordered sources and named reads. Streaming: io.h; changes: change.h.
 * Common contracts and outputs: docs/api.md.
 */
#ifndef PIGGLE_TREE_H
#define PIGGLE_TREE_H
#include <piggle/source.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Immediate empty tree creation, no I/O. Owned output; context retained.
 * Caller becomes the control thread. Use tree_open for an ordered list.
 * Later attachments win exact-file ties. Watching starts OFF.
 */
PG_API pg_status PG_CALL pg_tree_create(pg_context *context, pg_tree **out,
		pg_error *error);

/* Watch mode is independent of source access. OFF retains no reports; pass
 * NATIVE or SCAN to tree_watch (change.h). SCAN polls tracked scopes only.
 */
enum { PG_WATCH_OFF = 0, PG_WATCH_NATIVE, PG_WATCH_SCAN };
typedef struct pg_tree_info {
	size_t source_count;
	uint32_t watch_mode;
} pg_tree_info;
/* Immediate snapshot, no I/O. Required out, zero on error. Control thread;
 * allowed in observer callbacks. Reports committed attachments and watch mode.
 */
PG_API pg_status PG_CALL pg_tree_inspect(pg_tree *tree, pg_tree_info *out,
		pg_error *error);

/* Existing native inputs; zero options -> AUTO/READ. */
typedef struct pg_source_spec {
	const char *native_path;
	pg_source_options options;
} pg_source_spec;

/* Compose empty tree and open/attach sources in list order. Later entries
 * win exact-file ties. Archive names are indexed; loose roots are not
 * recursively scanned. Publish out only when all succeed. Watching starts
 * OFF. NULL array allowed only at count=0.
 * Input array/strings borrowed through return. Caller is control thread.
 */
PG_API pg_status PG_CALL pg_tree_open(
		pg_context *context,
		const pg_source_spec *sources,
		size_t count,
		pg_tree **out,
		pg_error *error);
/* Attach same-context source at the end; tree retains it, caller keeps it.
 * Source may attach to one tree; duplicate/other attachment -> BUSY.
 * Overlapping loose roots/contained archives -> CONFLICT. Reconcile tracked
 * scopes and queue visible changes while watching. Native watch setup failure
 * leaves attachment unchanged. Scan only tracked prefixes, possibly root.
 */
PG_API pg_status PG_CALL pg_tree_attach(
		pg_tree *tree,
		pg_source *source,
		pg_error *error);

/* Remove attachment, reconcile tracked scopes and queue watched changes.
 * Unattached -> NOT_ATTACHED. Retained files/readers survive unchanged.
 * Reattachment places the source at the end. Never delete source data.
 */
PG_API pg_status PG_CALL pg_tree_detach(
		pg_tree *tree,
		pg_source *source,
		pg_error *error);
/* Immediate source at attachment-order index, with owned reference.
 * END clears out. out required.
 * Enumeration is live, not a snapshot: serialize with tree control.
 */
PG_API pg_status PG_CALL pg_tree_source(pg_tree *tree, size_t index,
		pg_source **out, pg_error *error);

/* Common tasks on the resolved overlay. Named streaming uses
 * pg_reader_open_tree in io.h; watch/poll/unwatch live in change.h.
 */
/* Read a named visible logical file into caller storage. Compose
 * tree_find, file_read_all and file_close; no caller-owned temporary.
 * Same normalization/selection/errors as tree_find. Capture one selection;
 * never retry lookup or switch copies if content becomes STALE.
 * bytes required, initially zero. NULL allowed only at capacity=0; empty
 * spans are never accessed. CAPACITY leaves buffer untouched and bytes=0.
 * Discard any prefix from other failures. Size>SIZE_MAX -> LIMIT.
 * No returned allocation or NUL terminator;
 * empty files still verified.
 * Name/buffer/bytes borrow through return. Close all private handles; retain the
 * first error, report cleanup failure only if work succeeded. Serialize with
 * tree control; allowed during a read-only observer callback.
 */
PG_API pg_status PG_CALL pg_tree_read_all(pg_tree *tree,
		const char *name, void *buffer, size_t capacity, size_t *bytes,
		pg_error *error);

/* Named overlay file -> owned buffer. Compose tree_find, file_read_all_alloc
 * and file_close, retaining one selection through sizing, reading and cleanup.
 * Same lookup errors; never retry/switch winners. max_bytes bounds logical
 * content (zero permits empty only), not total working memory. Too large ->
 * LIMIT before payload allocation/read. out required and initially NULL/0.
 * Publish out only on success after all private cleanup; failure frees private
 * bytes and leaves out empty. No NUL terminator. Empty success is OK/NULL/0;
 * release with pg_buffer_free. Name and output borrow through return. Same
 * tree control and callback rules as tree_read_all.
 */
PG_API pg_status PG_CALL pg_tree_read_all_alloc(pg_tree *tree,
		const char *name, size_t max_bytes, pg_buffer *out,
		pg_error *error);

/* Export one named visible overlay file: find once, file_export, close.
 * Same logical bytes, mtime, lookup, native alias and publication contracts
 * as source_export. flags=0 exclusive or OVERWRITE. No retry/reselection.
 * Borrow inputs through return; tree control thread, not callbacks.
 * Preserve the first failure; a post-publication cleanup error returns
 * PG_COMMITTED with the underlying cause in pg_error.
 */
PG_API pg_status PG_CALL pg_tree_export(pg_tree *tree, const char *name,
		const char *native_output, uint32_t flags,
		pg_error *error);

/* Refresh the loose root, capture visible tree files, create private builder,
 * copy entries, finish once. No prior subtree request is needed.
 * Explicit PIGG2/HOGG10. Preserve selected original names, timestamps
 * and headers. Omit hidden copies/empty directories. No publication on
 * pre-commit failure; a valid empty archive is published. Changes during
 * reads -> STALE/RETRY.
 */
PG_API pg_status PG_CALL pg_tree_pack(
		pg_tree *tree,
		const char *native_archive,
		uint32_t format,
		const pg_pack_options *options,
		pg_error *error);

/* Refresh the loose root and capture visible tree files without a prior
 * subtree request. Preflight native name collisions, then export
 * each under existing native root with canonical names. Zero/OVERWRITE
 * flags; preserve timestamps. Root/ancestors anchored against substitution
 * and links. Not batch-atomic: each file publishes separately. Between-file
 * failure preserves earlier items and returns PG_PARTIAL. Created parent
 * directories may remain on failure. Aliasing source data -> CONFLICT.
 */
PG_API pg_status PG_CALL pg_tree_unpack(
		pg_tree *tree,
		const char *native_directory,
		uint32_t flags,
		pg_error *error);

/* Normalize name and select the visible physical copy. Inside a requested
 * tree subtree, use its indexed overlay, including indexed absence; outside
 * it, probe the exact loose path. NATIVE watching reconciles pending hints
 * for requested tree scopes before selection. No implicit subtree request.
 * A directory at the exact name -> CONFLICT; missing -> NOT_FOUND.
 * Owned file, NULL on failure. Use source_find for a specific source;
 * native-path colon qualification is not part of this API.
 */
PG_API pg_status PG_CALL pg_tree_find(
		pg_tree *tree,
		const char *name,
		pg_file **out,
		pg_error *error);

/* Capture indexed visible files recursively below a normalized directory
 * prefix without a forced subtree scan. NULL/empty means root. If
 * any loose source is attached, a covering tree_request_subtree is required;
 * otherwise INVALID and NULL out. Source-local requests do not suffice.
 * Archive-only/empty trees need no request. Exact file -> CONFLICT; missing
 * directory -> empty cursor. Canonical lexical order; owned cursor retains
 * captured metadata. NATIVE watching reconciles pending tree hints first.
 * Listing does not create a watched scope.
 */
PG_API pg_status PG_CALL pg_tree_files(
		pg_tree *tree,
		const char *prefix,
		pg_cursor **out,
		pg_error *error);

/* Discover and retain a normalized subtree without creating a cursor.
 * NULL/empty means root; exact file -> CONFLICT. A missing subtree is cached
 * as empty so later additions can be observed while watching. Repeating the
 * call refreshes the prefix; a parent request covers descendant listings.
 * May block for an arbitrarily large subtree.
 * Watching monitors it until tree close; no initial change is reported.
 */
PG_API pg_status PG_CALL pg_tree_request_subtree(pg_tree *tree,
		const char *prefix, pg_error *error);

/* Refresh indexed archives and all previously requested loose paths and
 * subtrees, including when watching is OFF. Unrequested loose paths are not
 * scanned. Failure preserves the prior cached observations; races -> RETRY.
 * Queue changes only while watching; poll delivers them. No callbacks or
 * disk commitment. Retained selections never refresh in place.
 */
PG_API pg_status PG_CALL pg_tree_rescan(pg_tree *tree, pg_error *error);

/* Release one owned reference; never commit or delete backing data.
 * Address required; NULL *handle succeeds. Accepted close clears it;
 * rejection leaves it owned. Cleanup may block and report IO, but the
 * reference stays consumed.
 * Final release stops watching and discards queued reports without callbacks.
 */
PG_API pg_status PG_CALL pg_tree_close(
		pg_tree **tree,
		pg_error *error);

#ifdef __cplusplus
}
#endif
#endif
