/* Optional, synchronous observation of requested tree paths.
 * Common contracts: docs/api.md.
 */
#ifndef PIGGLE_CHANGE_H
#define PIGGLE_CHANGE_H
#include <piggle/tree.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Select one mode for the tree. OFF is the initial state, not an input.
 * NATIVE uses OS notices; SCAN compares tracked scopes during each poll.
 * Start from a stable baseline of tree-requested subtrees and open tree
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
	PG_CHANGE_LOSS
};
/* One visible-name transition. Before is NULL for addition; after is NULL
 * for removal. UPDATE includes winner changes even if content is equal.
 * LOSS has NULL name/before/after and identifies an affected watched scope;
 * NULL scope means the affected scope is unknown. Other events have NULL
 * scope. All spans borrow until the callback returns. Sequence increases
 * within one tree and never wraps.
 */
typedef struct pg_visible_change {
	uint64_t sequence;
	uint32_t kind;
	const char *canonical_name;
	const char *scope;
	const pg_file_info *before;
	const pg_file_info *after;
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
 * Native loss reports LOSS and reconciles the affected tracked scopes.
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
