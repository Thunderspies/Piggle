/* Snapshot enumeration of files and directories. Contracts: docs/api.md. */
#ifndef PIGGLE_ENTRIES_H
#define PIGGLE_ENTRIES_H
#include <piggle/tree.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Owned cursor, released with pg_entry_cursor_close. */
typedef struct pg_entry_cursor pg_entry_cursor;
enum { PG_ENTRY_FILE = 1, PG_ENTRY_DIRECTORY };
/* Zero flags lists immediate children; recursive lists all descendants. */
enum { PG_ENTRIES_RECURSIVE = 1u };
/* Discovery depth; CHILDREN never opens a child directory. */
enum { PG_DISCOVER_CHILDREN = 0, PG_DISCOVER_RECURSIVE };
enum {
	PG_ENTRY_READ_ONLY = 1u,
	PG_ENTRY_HIDDEN = 2u,
	PG_ENTRY_SYSTEM = 4u,
	PG_ENTRY_IMPLIED = 8u
};
/* Immutable metadata; name spans borrow the entry cursor until close.
 * size is logical bytes for files, zero for directories. mtime is Unix
 * seconds, zero for implied archive directories. Attributes describe the
 * captured native entry; archives are READ_ONLY when opened for reading.
 * Implied directories use canonical spelling for original_name.
 * Hidden means the Windows hidden attribute or a Unix dot-prefixed basename.
 * System is Windows-only. Source IDs are context-local.
 */
typedef struct pg_entry_info {
	uint32_t kind;
	uint32_t attributes;
	pg_id source_id;
	const char *canonical_name;
	const char *original_name;
	uint64_t size;
	int64_t mtime;
} pg_entry_info;

/* Discover and retain immediate children or a complete recursive subtree.
 * NULL/empty prefix means root. Repeat to refresh. Same ownership, errors,
 * normalization, rollback and control rules as request_subtree. A shallow
 * request covers only direct children, never descendants or their absence.
 * Tree discovery adds a watched scope at the requested depth. Source-local
 * discovery does not add a tree watch. No initial reports or callbacks.
 */
PG_API pg_status PG_CALL pg_source_discover(pg_source *source,
		const char *prefix, uint32_t depth, pg_error *error);
PG_API pg_status PG_CALL pg_tree_discover(pg_tree *tree,
		const char *prefix, uint32_t depth, pg_error *error);

/* Capture entries below prefix; NULL/empty means root, excluding root itself.
 * Immediate entries require shallow or recursive coverage; recursive entries
 * require recursive coverage. Same normalization, control and errors as
 * source_files/tree_files. A file prefix -> CONFLICT; missing -> empty.
 * No native scan implied. Empty loose directories and archive-implied
 * directories are included. Canonical lexical order, independent of later
 * rescans. Tree directories hide same-name files; later sources choose
 * directory metadata, preferring explicit over implied within one source.
 * Unknown flags -> INVALID. Owned output, NULL on failure.
 */
PG_API pg_status PG_CALL pg_source_entries(pg_source *source,
		const char *prefix, uint32_t flags, pg_entry_cursor **out,
		pg_error *error);
PG_API pg_status PG_CALL pg_tree_entries(pg_tree *tree,
		const char *prefix, uint32_t flags, pg_entry_cursor **out,
		pg_error *error);

/* Next captured entry. out and file required and zero/NULL on error or END.
 * File entry transfers one owned physical selection; close it independently
 * with pg_file_close. Directory yields NULL file. Metadata spans continue
 * borrowing the cursor, even after its selected file is closed. Workers
 * allowed; serialize this cursor. Failed calls do not advance the cursor.
 */
PG_API pg_status PG_CALL pg_entry_cursor_next(pg_entry_cursor *cursor,
		pg_entry_info *out, pg_file **file, pg_error *error);

/* Release snapshot and unconsumed file selections, never publish changes.
 * Same ownership/control rules as cursor_close; NULL *cursor is a no-op.
 * Rejected close preserves ownership; accepted close clears the address.
 */
PG_API pg_status PG_CALL pg_entry_cursor_close(pg_entry_cursor **cursor,
		pg_error *error);

#ifdef __cplusplus
}
#endif
#endif
