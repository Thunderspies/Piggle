/* Create a new archive: create, stage entries, finish, close.
 * Open/read/edit existing archives with source.h. Contracts: docs/api.md.
 */
#ifndef PIGGLE_ARCHIVE_BUILDER_H
#define PIGGLE_ARCHIVE_BUILDER_H
#include <piggle/io.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Create private archive builder; explicit PIGG2 or HOGG10, no AUTO/LOOSE.
 * OVERWRITE permits an existing regular destination; other flags INVALID.
 * Duplicate canonical entry names always return EXISTS, including
 * differently spelled names.
 * No publication before finish. Capture destination identity and recheck
 * before publish. Retained-source aliases/links -> CONFLICT.
 * Owned builder with serialized access, no fixed control thread.
 */
PG_API pg_status PG_CALL pg_archive_builder_create(
		pg_context *context,
		const char *native_path,
		uint32_t format,
		uint32_t flags,
		pg_archive_builder **out,
		pg_error *error);

/* Create with an explicit checksum profile. options is required, copied
 * before return. Same ownership, publication and control rules as create.
 * Unknown values -> INVALID; STORED with PIGG2 -> UNSUPPORTED. NULL output
 * on failure. The original create function selects LOGICAL checksums.
 */
PG_API pg_status PG_CALL pg_archive_builder_create_options(pg_context *context,
	const char *native_path, const pg_archive_options *options,
	pg_archive_builder **out, pg_error *error);

/* Create with construction policy; options and out are required. Copies
 * all options and resolves staging_directory before returning. Same format,
 * publication, identity, ownership and error rules as create_options.
 * Capacity arithmetic overflow -> LIMIT; no handle on failure.
 */
PG_API pg_status PG_CALL pg_archive_builder_create_config(pg_context *context,
	const char *native_path, const pg_archive_build_options *options,
	pg_archive_builder **out, pg_error *error);

/* Persistent construction sessions support HOGG10 only. native_session is
 * a new, exclusively owned directory for versioned checkpoints and a lease;
 * payloads follow options.staging_directory. Inputs are copied/resolved.
 * create_session commits an empty checkpoint before returning an owned
 * builder. Existing session -> EXISTS; another owner -> BUSY. Ordinary
 * create/create_options/create_config remain nonpersistent.
 */
PG_API pg_status PG_CALL pg_archive_builder_create_session(pg_context *context,
	const char *native_path, const char *native_session,
	const pg_archive_build_options *options, pg_archive_builder **out,
	pg_error *error);

/* Acquire an exclusive session and restore its last committed checkpoint.
 * Validate metadata and reread/decode completed payloads before returning.
 * Ignore unfinished/uncheckpointed entries. Never publish automatically.
 * Missing session -> NOT_FOUND; malformed/changed session -> CORRUPT/STALE.
 * Corrupt completed payloads can return CHECKSUM. An already published output
 * matching saved publication identity restores a close-only builder. out is
 * required and NULL on failure.
 */
PG_API pg_status PG_CALL pg_archive_builder_resume(pg_context *context,
	const char *native_session, pg_archive_builder **out, pg_error *error);

/* Commit all completed entries plus a complete metadata image. Requires a
 * persistent active builder. Live writer -> BUSY; finished/ordinary -> INVALID.
 * Flush payloads and the next image before atomically selecting the checkpoint.
 * A failed selection retains the previous checkpoint. Checkpoints may traverse
 * metadata and payloads. Process interruption is covered; this is no guarantee
 * against storage hardware ignoring durability requests. Publication outcome
 * statuses apply to checkpoint selection, independently of archive publication.
 */
PG_API pg_status PG_CALL pg_archive_builder_checkpoint(
	pg_archive_builder *builder, pg_error *error);

/* Checkpoint and close a persistent builder. Required owned pointer; BUSY or
 * failure before selecting a checkpoint leaves it owned. NULL *builder is OK.
 * Ordinary close of a session retains only its last committed checkpoint;
 * suspend includes all completed entries. Ordinary builders still abort on
 * close.
 */
PG_API pg_status PG_CALL pg_archive_builder_suspend(
	pg_archive_builder **builder, pg_error *error);

/* Completed inventory, insertion order. Metadata spans borrow through each
 * callback only; source/copy IDs and generations are zero, archive_record is
 * the insertion ordinal. Callback receives explicit state and returns PG_OK
 * to continue; another status stops visitation and is returned. Caller must
 * serialize access. Reentrant builder mutation/close returns REENTRANT.
 */
typedef pg_status(PG_CALL *pg_archive_inventory_fn)(
	void *state, const pg_file_info *entry);
PG_API pg_status PG_CALL pg_archive_builder_visit_inventory(
	pg_archive_builder *builder, pg_archive_inventory_fn visit, void *state,
	pg_error *error);

/* Discard a persistent session and consume its builder. Live writer -> BUSY.
 * Remove checkpoint/private files, never published output. Cleanup failure
 * consumes ownership and reports IO/PARTIAL; NULL *builder is a no-op.
 * A replaced session directory -> STALE without consuming the builder.
 */
PG_API pg_status PG_CALL pg_archive_builder_discard(
	pg_archive_builder **builder, pg_error *error);

/* Stage complete entries; use io.h for chunked or encoded input. */
/* Stage one complete logical entry; no external commitment until builder
 * finish. NULL entry -> defaults; NULL buffer allowed only at size=0. Empty
 * spans are not accessed. Inputs borrow through return; owns its writer.
 * Staging has no external disk commitment and may block.
 * Failure never stages this entry, invalidates earlier entries, or publishes
 * output.
 * Stage hierarchy conflicts literally; duplicate canonical names fail.
 */
PG_API pg_status PG_CALL pg_archive_builder_write_all(
		pg_archive_builder *builder,
		const char *name, const void *buffer, size_t size,
		const pg_entry_options *entry, pg_error *error);

/* Native reader -> archive entry writer. Preserve mtime; stage only.
 * Same entry completion and duplicate rules as archive_builder_write_all.
 * Complete cleanup before return, no external disk publication.
 */
PG_API pg_status PG_CALL pg_archive_builder_import(
		pg_archive_builder *builder,
		const char *name,
		const char *native_input,
		uint32_t compression,
		pg_error *error);

/* Selected reader -> staged archive entry. Preserve timestamp/header and
 * original spelling if it normalizes to target; otherwise canonical name.
 * Validate content; no external commitment. Same duplicate policy.
 */
PG_API pg_status PG_CALL pg_archive_builder_copy(
		pg_archive_builder *builder,
		const char *name,
		pg_file *input,
		uint32_t compression,
		pg_error *error);

/* Validate complete archive and atomically publish native destination.
 * Empty archive valid. Live entry writer -> BUSY without starting. Accepted
 * finish is one-shot; builder becomes close-only on any outcome. Preserves
 * completed entry metadata and encodings; no view/source is implicitly
 * opened. A published empty archive is valid. A later cleanup or
 * durability failure returns PG_COMMITTED with its cause in pg_error.
 */
PG_API pg_status PG_CALL pg_archive_builder_finish(
		pg_archive_builder *builder,
		pg_error *error);

/* Release one owned reference; never commit or delete backing data.
 * Live entry writer -> BUSY, including a finished writer not yet closed.
 * Otherwise an ordinary unfinished builder aborts all private entries; a
 * persistent builder retains its last checkpoint. Address
 * required; NULL *handle succeeds. Accepted close clears it; rejection
 * leaves it owned. Cleanup may block and report IO; ownership stays consumed.
 */
PG_API pg_status PG_CALL pg_archive_builder_close(
		pg_archive_builder **builder,
		pg_error *error);

#ifdef __cplusplus
}
#endif
#endif
