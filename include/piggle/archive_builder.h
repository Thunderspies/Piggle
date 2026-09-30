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
PG_API pg_status PG_CALL pg_archive_builder_create_options(
		pg_context *context, const char *native_path,
		const pg_archive_options *options, pg_archive_builder **out,
		pg_error *error);

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
 * Otherwise an unfinished builder aborts all private entries. Address
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
