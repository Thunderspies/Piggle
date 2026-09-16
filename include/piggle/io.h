/* Public API. Common contracts and outputs: docs/api.md. */
#ifndef PIGGLE_IO_H
#define PIGGLE_IO_H
#include <piggle/file.h>
#include <piggle/source.h>
#include <piggle/tree.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Open named visible content with one owned reader. Compose source_find,
 * reader_open and file_close; retain one selection, never retry/retarget.
 * representation is READ_LOGICAL (usual) or READ_STORED. Same lookup,
 * staleness, metadata and verification rules as the selected-file reader.
 * out required, NULL until successful return after temporary cleanup.
 * Failure closes private reader/selection and preserves the first error.
 * Name/out borrow through return; control follows source. Standalone-source
 * callback use allowed, but callback spans expire on callback return.
 * Returned reader retains its dependencies; name may expire after open.
 * Subsequent read/close may move to a worker with per-reader serialization.
 */
PG_API pg_status PG_CALL pg_reader_open_source(pg_source *source,
		const char *name, uint32_t representation, pg_reader **out,
		pg_error *error);

/* Same named-reader contract, selecting the visible overlay winner once.
 * Open runs on the tree control thread; later attachments never retarget.
 * While tree watching is active, this reader tracks its virtual name until
 * close; no initial event. Opening during an observer callback is allowed.
 * Read/close obey reader serialization.
 */
PG_API pg_status PG_CALL pg_reader_open_tree(pg_tree *tree,
		const char *name, uint32_t representation, pg_reader **out,
		pg_error *error);

/* Open independent sequential reader at zero for captured physical copy.
 * READ_LOGICAL decodes; READ_STORED exposes its exact stored bytes. Both
 * validate available logical digests at EOF; stored reads still decode for
 * validation. Retains file/source. Changed identity -> STALE. Owned reader.
 * A tree-selected file starts name tracking while its reader is open if that
 * tree is still live and watching; tree close ends tracking, not reading.
 * Opening during an observer callback is allowed and registers the watch.
 */
PG_API pg_status PG_CALL pg_reader_open(
		pg_file *file,
		uint32_t representation,
		pg_reader **out,
		pg_error *error);

/* Open one existing regular native file as a logical reader. Capture size,
 * mtime and native identity; no source/tree needed. Links/specials ->
 * CONFLICT. Detect observable changes as STALE/RETRY. Owned reader.
 */
PG_API pg_status PG_CALL pg_reader_open_native(
		pg_context *context,
		const char *native_path,
		pg_reader **out,
		pg_error *error);

/* Immediate fixed metadata copy; out required, zero on failure. */
PG_API pg_status PG_CALL pg_reader_inspect(pg_reader *reader,
		pg_reader_info *out, pg_error *error);

/* Read at most capacity bytes in the reader representation. NULL buffer
 * allowed only at capacity=0; empty spans are never accessed. bytes required,
 * zero initially and exact on failure. Buffer/count borrow through return.
 * Zero capacity -> OK/0, no verification.
 * A completed positive-capacity read returns OK only with nonzero bytes.
 * Last nonempty read verifies before OK; next positive read -> END/0.
 * Empty files verify and return END/0 on the first positive read. Early
 * close makes no verification claim. Earlier chunks are provisional until
 * verified EOF: keep consumer effects reversible. Failed reads make
 * the reader close-only. Invalid calls leave it unchanged.
 */
PG_API pg_status PG_CALL pg_reader_read(
		pg_reader *reader,
		void *buffer,
		size_t capacity,
		size_t *bytes,
		pg_error *error);

/* Release one owned reference; never commit or delete backing data.
 * Address required; NULL *handle succeeds. Accepted close clears it;
 * rejection leaves it owned. Cleanup may block and report IO, but the
 * reference stays consumed. No implicit finish.
 */
PG_API pg_status PG_CALL pg_reader_close(
		pg_reader **reader,
		pg_error *error);

/* Open writer replacing the source-visible same-name winner, or adding.
 * Normalize name; options required; WRITE source, one writer lease. Open
 * rejects a source-local implied directory at the target or a visible file
 * at any proper ancestor with CONFLICT, even if no target record exists.
 * Ignore other sources' order. Probe the native target and hierarchy even
 * when a requested subtree has a cached read view. Validate at open; recheck
 * target and hierarchy before finish publication. Observed changes ->
 * STALE/RETRY, never retarget.
 * Native conflicts are always rejected.
 * No publication until finish. Copy name/header/options before open returns;
 * caller storage may then expire. Preserve existing original name and leave
 * other same-name physical copies intact. Whole-buffer defaults are in
 * source.h.
 */
PG_API pg_status PG_CALL pg_writer_open_source(
		pg_source *destination,
		const char *name,
		const pg_write_options *options,
		pg_writer **out,
		pg_error *error);

/* Open writer for exactly this captured copy, even if later hidden. Name and
 * source come from file; no redundant name/replace arguments. Options
 * required; original_name override is INVALID. No virtual hierarchy check:
 * the caller selected the exact physical copy. Same lease/ownership and
 * publication rules. Changed copy -> STALE, never retarget.
 */
PG_API pg_status PG_CALL pg_writer_open_file(
		pg_file *file,
		const pg_write_options *options,
		pg_writer **out,
		pg_error *error);

/* Open sequential entry writer; normalize name, options required. One live
 * entry writer per builder; close it before next entry or builder finish.
 * Copy borrowed metadata before open returns. Finish stages entry privately;
 * writer_close without finish aborts only that entry. Earlier entries remain.
 * Entry finish installs only after all fallible work and input cleanup;
 * failure never installs an entry, and finished writer close cannot fail IO.
 * Duplicate canonical name -> EXISTS, including different spelling.
 */
PG_API pg_status PG_CALL pg_writer_open_archive_builder(
		pg_archive_builder *builder,
		const char *name,
		const pg_write_options *options,
		pg_writer **out,
		pg_error *error);

/* Stage one complete native regular file replacement; flags=0 is exclusive,
 * OVERWRITE permits regular-file replacement at finish. Other flags INVALID.
 * Options required; FORCE compression/nonempty header/original_name
 * unsupported. An empty cached-header span is ignored.
 * Create missing parents without following links; they may survive abort.
 * Reject aliases into retained source data. No publication until finish.
 */
PG_API pg_status PG_CALL pg_writer_open_native(
		pg_context *context,
		const char *native_path,
		const pg_write_options *options,
		uint32_t flags,
		pg_writer **out,
		pg_error *error);

/* Pin a captured cursor and an existing native directory. Preflight every
 * captured path before any file is published. Close after staged writers.
 */
PG_API pg_status PG_CALL pg_unpack_target_open(
		pg_cursor *cursor, const char *directory, uint32_t flags,
		pg_unpack_target **out, pg_error *error);
/* Open one staged writer for a file captured by this target. The root and
 * path are checked at open and finish; writes use the pinned directory.
 */
PG_API pg_status PG_CALL pg_writer_open_unpack(
		pg_unpack_target *target, pg_file *file,
		pg_writer **out, pg_error *error);
PG_API pg_status PG_CALL pg_unpack_target_close(
		pg_unpack_target **target, pg_error *error);

/* Stage all input bytes sequentially; no append/seek/in-place edit. NULL
 * buffer allowed only at size=0; empty spans are never accessed. bytes
 * required, exact even on failure; OK means all input was accepted.
 * Buffer/count borrowed through return. Excess declared input -> INVALID. A failed
 * accepted write makes writer abort-only.
 * No publication; only submit new input after this call returns.
 */
PG_API pg_status PG_CALL pg_writer_write(
		pg_writer *writer,
		const void *buffer,
		size_t size,
		size_t *bytes,
		pg_error *error);

/* Require exact lengths, validate/decode/hash/encode and finish the entry.
 * Source/native writer: publish replacement. Archive entry writer: install
 * one private staged entry without publishing an archive.
 * Empty files valid. Accepted finish is one-shot; any outcome makes writer
 * close-only. Failure before publication preserves old logical content.
 * Keep writer until close; inspect status and error on failure.
 */
PG_API pg_status PG_CALL pg_writer_finish(
		pg_writer *writer,
		pg_error *error);

/* Release one owned reference; never commit or delete backing data.
 * Address required; NULL *handle succeeds. Accepted close clears it;
 * rejection leaves it owned. Cleanup may block and report IO, but the
 * reference stays consumed. No implicit finish.
 */
PG_API pg_status PG_CALL pg_writer_close(
		pg_writer **writer,
		pg_error *error);

#ifdef __cplusplus
}
#endif
#endif
