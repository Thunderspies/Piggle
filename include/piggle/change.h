/* Optional, synchronous observation of requested tree paths.
 * Common contracts: docs/api.md.
 */
#ifndef PIGGLE_CHANGE_H
#define PIGGLE_CHANGE_H
#include <piggle/entries.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Manage a normalized directory prefix independently of discovery.
 * NULL/empty means root; depth is PG_DISCOVER_CHILDREN or RECURSIVE.
 * Repeating the same registration is a no-op; overlapping scopes form a
 * union. Native mode retains no initial file baseline: Linux walks only
 * directories to install watches; Windows arms a recursive root watch.
 * SCAN mode discovers the scope eagerly when enabled or registered.
 * Tree lookups and discovery establish known state within managed scopes.
 * Control thread, no callbacks. Failure preserves the registration set.
 * Management persists through unwatch; manage does not enable watching.
 */
PG_API pg_status PG_CALL pg_tree_manage(pg_tree *tree, const char *prefix,
		uint32_t depth, pg_error *error);

/* Remove exactly one normalized registration. Missing -> NOT_FOUND.
 * Preserve cached discovery and already queued reports. Other managed,
 * discovered or reader scopes continue observing their covered names.
 * Same input, control and callback rules as manage; no disk commitment.
 */
PG_API pg_status PG_CALL pg_tree_unmanage(pg_tree *tree, const char *prefix,
		uint32_t depth, pg_error *error);

/* Select one mode for the tree. OFF is the initial state, not an input.
 * NATIVE uses OS notices; SCAN compares tracked scopes during each poll.
 * Managed native scopes need no file scan. Start from a stable baseline
 * of tree-requested subtrees and open tree
 * readers. Later tree subtree requests and reader opens add scopes without
 * initial events. Source-local requests do not add tree watch scopes.
 * No source-wide scan is implied. Same mode is a no-op; switching modes
 * requires unwatch first. Native facility absence -> UNSUPPORTED.
 * Control thread only; no callbacks or disk commitment.
 */
PG_API pg_status PG_CALL pg_tree_watch(pg_tree *tree, uint32_t mode,
		pg_error *error);

/* Stop tracking, discard undelivered reports, and release native watches.
 * Keep cached metadata and attached sources. OFF is a no-op. Control thread;
 * no callbacks or disk commitment. Cleanup completes before return.
 */
PG_API pg_status PG_CALL pg_tree_unwatch(pg_tree *tree, pg_error *error);

enum {
	PG_CHANGE_ADD = 1,
	PG_CHANGE_UPDATE,
	PG_CHANGE_REMOVE,
	PG_CHANGE_LOSS,
	PG_CHANGE_INVALIDATE
};
/* One visible-name transition. Before is NULL for addition; after is NULL
 * for removal. UPDATE includes winner changes even if content is equal.
 * LOSS has NULL name/before/after and identifies an affected watched scope;
 * NULL scope means the affected scope is unknown. INVALIDATE reports a
 * managed name with unknown prior state, or a scope whose history was lost;
 * it has NULL before/after and entry metadata. A named invalidation may
 * cover a directory and its descendants. Other events have NULL scope.
 * before_entry/after_entry describe files or directories; before/after are
 * available only for file sides. Native hints may coalesce intermediate
 * states. Directory metadata changes are reported even without file edits.
 * All spans borrow until the callback returns. Sequence increases
 * within one tree and never wraps.
 */
typedef struct pg_visible_change {
	uint64_t sequence;
	uint32_t kind;
	const char *canonical_name;
	const char *scope;
	const pg_file_info *before;
	const pg_file_info *after;
	const pg_entry_info *before_entry;
	const pg_entry_info *after_entry;
} pg_visible_change;
typedef void (PG_CALL *pg_visible_fn)(void *user,
		const pg_visible_change *change);
typedef struct pg_observer {
	void *user;
	pg_visible_fn visible; /* NULL discards delivered reports. */
} pg_observer;

/* Process one finite cut of native hints or scan tracked scopes, reconcile
 * visible names, then deliver queued reports on the control thread.
 * OFF -> INVALID. observer required; NULL callback discards reports.
 * Tree lookups and listings may reconcile native hints and queue changes but
 * never call observers. SCAN refreshes only during poll or explicit refresh.
 * A reader watch ends on reader close; reports already queued remain until
 * poll. Tree-requested subtrees remain watched until tree close or unwatch.
 * Native loss reports LOSS plus managed-scope invalidations. It refreshes
 * discovered scopes, never recursively indexes an undiscovered managed root.
 * Failed refresh preserves prior observations and undelivered reports;
 * races -> RETRY.
 * Callback lookup sees reconciled state. Reader opens are allowed and may
 * add watches; newly queued reports wait for a later poll. Poll holds no
 * internal lock while calling observers. Recursive poll/control mutation
 * returns REENTRANT. No worker invokes callbacks.
 */
PG_API pg_status PG_CALL pg_tree_poll(pg_tree *tree,
		const pg_observer *observer, pg_error *error);

#ifdef __cplusplus
}
#endif
#endif
