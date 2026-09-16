#include <piggle/piggle.h>
#include "internal.hpp"

int pg_tree_manages(pg_tree *tree, const char *name)
{
	for (pg_managed_scope *scope = tree->managed; scope;
	     scope = scope->next)
		if (pg_name_in_scope(name, scope->prefix,
			scope->depth == PG_DISCOVER_RECURSIVE))
			return 1;
	return 0;
}

static pg_status pg_manage_name(pg_tree *tree, const char *prefix,
		uint32_t depth, char **out, pg_error *error)
{
	size_t size = 0;
	pg_status status;

	*out = NULL;
	if (!tree || depth > PG_DISCOVER_RECURSIVE)
		return pg_result(PG_INVALID, error);
	status = pg_tree_control_status(tree, 1);
	if (status != PG_OK)
		return pg_result(status, error);
	if (!prefix || !*prefix)
		return PG_OK;
	status = pg_name_normalize(tree->context, prefix, NULL, 0, &size, NULL);
	if (status != PG_CAPACITY)
		return pg_result(status, error);
	*out = (char *)malloc(size);
	if (!*out)
		return pg_result(PG_NOMEM, error);
	status = pg_name_normalize(tree->context, prefix, *out, size, &size,
		error);
	if (status != PG_OK) {
		free(*out);
		*out = NULL;
	}
	return status;
}

pg_status pg_tree_manage(pg_tree *tree, const char *prefix, uint32_t depth,
		pg_error *error)
{
	char *canonical = NULL;
	pg_status status = pg_manage_name(tree, prefix, depth, &canonical,
		error);

	if (status != PG_OK)
		return status;
	for (pg_managed_scope *s = tree->managed; s; s = s->next) {
		if (s->depth == depth && !strcmp(s->prefix ? s->prefix : "",
			canonical ? canonical : "")) {
			free(canonical);
			return pg_result(PG_OK, error);
		}
	}
	pg_managed_scope *scope = (pg_managed_scope *)calloc(1, sizeof(*scope));

	if (!scope) {
		free(canonical);
		return pg_result(PG_NOMEM, error);
	}
	scope->prefix = canonical;
	scope->depth = depth;
	if (tree->watch_mode == PG_WATCH_SCAN)
		status = pg_tree_discover(tree, canonical, depth, error);
	if (status != PG_OK) {
		free(canonical);
		free(scope);
		return status;
	}
	scope->next = tree->managed;
	tree->managed = scope;
	return pg_result(PG_OK, error);
}

pg_status pg_tree_unmanage(pg_tree *tree, const char *prefix, uint32_t depth,
		pg_error *error)
{
	char *canonical = NULL;
	pg_status status = pg_manage_name(tree, prefix, depth, &canonical,
		error);

	if (status != PG_OK)
		return status;
	pg_managed_scope **at = &tree->managed;

	while (*at && ((*at)->depth != depth ||
	       strcmp((*at)->prefix ? (*at)->prefix : "",
		canonical ? canonical : "")))
		at = &(*at)->next;
	free(canonical);
	if (!*at)
		return pg_result(PG_NOT_FOUND, error);
	pg_managed_scope *scope = *at;

	*at = scope->next;
	free(scope->prefix);
	free(scope);
	for (pg_tree_scope *s = tree->scopes; s; s = s->next)
		if (s->managed && !pg_tree_manages(tree, s->prefix))
			s->managed = 0;
	pg_tree_reader_scope_sweep(tree);
	return pg_result(PG_OK, error);
}

pg_status pg_tree_manage_scan(pg_tree *tree, pg_error *error)
{
	for (pg_managed_scope *s = tree->managed; s; s = s->next) {
		pg_status status = pg_tree_discover(tree, s->prefix, s->depth,
			error);

		if (status != PG_OK)
			return status;
	}
	return PG_OK;
}

void pg_tree_manage_discard(pg_tree *tree)
{
	while (tree->managed) {
		pg_managed_scope *scope = tree->managed;

		tree->managed = scope->next;
		free(scope->prefix);
		free(scope);
	}
}

pg_status pg_tree_observe_name(pg_tree *tree, const char *name,
		pg_error *error)
{
	for (pg_tree_scope *s = tree->scopes; s; s = s->next) {
		if (s->exact && !strcmp(s->prefix, name)) {
			s->managed = 1;
			return PG_OK;
		}
		if (!s->exact && pg_name_in_scope(name, s->prefix, !s->shallow))
			return PG_OK;
	}
	pg_tree_scope *scope = (pg_tree_scope *)calloc(1, sizeof(*scope));

	if (!scope)
		return pg_result(PG_NOMEM, error);
	scope->prefix = strdup(name);
	if (!scope->prefix) {
		free(scope);
		return pg_result(PG_NOMEM, error);
	}
	for (size_t i = 0; i < tree->count; i++) {
		if (tree->sources[i]->format != PG_LOOSE)
			continue;
		pg_status status = pg_source_refresh_name(tree->sources[i],
			name,
			0, error);

		if (status != PG_OK) {
			free(scope->prefix);
			free(scope);
			return status;
		}
	}
	scope->exact = scope->managed = 1;
	int reconciling = tree->reconciling;

	tree->reconciling = 1;
	pg_status status = pg_tree_scope_snapshot(tree, scope,
		&scope->baseline, error);

	tree->reconciling = reconciling;
	if (status != PG_OK) {
		free(scope->prefix);
		free(scope);
		return status;
	}
	scope->next = tree->scopes;
	tree->scopes = scope;
	return PG_OK;
}

static pg_status pg_tree_invalidate(pg_tree *tree, const char *name,
		const char *scope)
{
	pg_tree_batch *batch = (pg_tree_batch *)calloc(1, sizeof(*batch));

	if (!batch)
		return PG_NOMEM;
	batch->invalid_name = name ? strdup(name) : NULL;
	batch->invalid_scope = scope ? strdup(scope) : NULL;
	if ((name && !batch->invalid_name) || (scope &&
		!batch->invalid_scope)) {
		free(batch->invalid_name);
		free(batch->invalid_scope);
		free(batch);
		return PG_NOMEM;
	}
	if (tree->pending_tail)
		tree->pending_tail->next = batch;
	else
		tree->pending_head = batch;
	tree->pending_tail = batch;
	return PG_OK;
}

pg_status pg_tree_invalidate_managed(pg_tree *tree)
{
	for (pg_managed_scope *s = tree->managed; s; s = s->next) {
		pg_status status = pg_tree_invalidate(tree, NULL,
			s->prefix ? s->prefix : "");

		if (status != PG_OK)
			return status;
	}
	return PG_OK;
}

pg_status pg_tree_hint_add(pg_tree *tree, pg_source *source,
		const char *name, int subtree)
{
	size_t size = 0;
	pg_status status;
	char *canonical = NULL;

	if (name && *name) {
		status = pg_name_normalize(tree->context, name, NULL, 0,
			&size, NULL);
		if (status != PG_CAPACITY)
			return status == PG_INVALID ? PG_OK : status;
		canonical = (char *)malloc(size);
		if (!canonical)
			return PG_NOMEM;
		status = pg_name_normalize(tree->context, name, canonical,
			size, &size, NULL);
		if (status != PG_OK) {
			free(canonical);
			return status;
		}
	}
	for (pg_native_hint *h = tree->hints; h; h = h->next) {
		if (h->source == source && !strcmp(h->name ? h->name : "",
			canonical ? canonical : "")) {
			h->subtree |= subtree;
			free(canonical);
			return PG_OK;
		}
	}
	pg_native_hint *hint = (pg_native_hint *)calloc(1, sizeof(*hint));

	if (!hint) {
		free(canonical);
		return PG_NOMEM;
	}
	hint->source = source;
	hint->name = canonical;
	hint->subtree = subtree;
	hint->next = tree->hints;
	tree->hints = hint;
	return PG_OK;
}

void pg_tree_hint_discard(pg_tree *tree, pg_source *source)
{
	pg_native_hint **at = &tree->hints;

	while (*at) {
		pg_native_hint *hint = *at;

		if (source && source != hint->source) {
			at = &hint->next;
			continue;
		}
		*at = hint->next;
		free(hint->name);
		free(hint);
	}
}

static int pg_hint_scope_intersects(pg_native_hint *hint, pg_tree_scope *scope)
{
	if (scope->exact)
		return (scope->reader_refs || scope->managed) &&
			(!strcmp(scope->prefix, hint->name) ||
			 (hint->subtree && pg_name_in_scope(scope->prefix,
				hint->name, 1)));
	return pg_name_in_scope(hint->name, scope->prefix, !scope->shallow) ||
		(hint->subtree && scope->prefix &&
		 pg_name_in_scope(scope->prefix, hint->name, 1));
}

static pg_status pg_hint_refresh_descendants(pg_tree *tree,
		pg_native_hint *h, pg_error *error)
{
	pg_status status = PG_OK;
	for (pg_tree_scope *s = tree->scopes; s;
	     s = s->next) {
		if (!s->prefix || !pg_hint_scope_intersects(h, s) ||
		    !pg_name_in_scope(s->prefix, h->name, 1))
			continue;
		status = s->exact ? pg_source_refresh_name(h->source,
			s->prefix, 0, error) :
			pg_source_discover_tree(h->source, s->prefix,
				s->shallow ? PG_DISCOVER_CHILDREN :
				PG_DISCOVER_RECURSIVE, error);
		if (status != PG_OK)
			break;
	}
	return status;
}

pg_status pg_tree_hints_reconcile(pg_tree *tree, pg_error *error)
{
	int changed = 0;
	pg_status status = PG_OK;
	pg_tree_source_state *saved = NULL;
	pg_tree_batch *previous = tree->pending_tail;

	for (pg_native_hint *h = tree->hints; h; h = h->next) {
		int known = 0, recursive = 0;

		if (tree->query_name && h->name &&
		    strcmp(tree->query_name, h->name) &&
		    !(h->subtree && pg_name_in_scope(tree->query_name, h->name,
			1)))
			continue;
		h->processed = 1;
		if (h->name) {
			for (pg_tree_scope *s = tree->scopes; s; s = s->next) {
				if (!pg_hint_scope_intersects(h, s))
					continue;
				known = 1;
				s->dirty = 1;
				recursive |= !s->exact && !s->shallow &&
					pg_name_in_scope(h->name, s->prefix, 1);
			}
		} else if (h->source->format != PG_LOOSE) {
			known = 1;
			for (pg_tree_scope *s = tree->scopes; s; s = s->next)
				s->dirty = 1;
		}
		if (known && !saved) {
			status = pg_tree_sources_save(tree, &saved);
			if (status != PG_OK)
				break;
		}
		if (h->source->format != PG_LOOSE) {
			status = pg_source_rescan(h->source, error);
			changed = 1;
		} else if (h->name) {
			if (known) {
				status = pg_source_refresh_name(h->source,
					h->name,
					h->subtree && recursive, error);
				if (status == PG_OK && h->subtree && !recursive)
					status = pg_hint_refresh_descendants(
						tree, h, error);
				changed = 1;
			}
			if (status == PG_OK && pg_tree_manages(tree, h->name) &&
			    (!known || (h->subtree && !recursive)))
				status = pg_tree_invalidate(tree, h->name,
					NULL);
		}
		if (status != PG_OK)
			break;
	}
	if (status == PG_OK && changed) {
		tree->partial_changes = 1;
		status = pg_tree_queue_changes(tree, error);
		tree->partial_changes = 0;
	}
	if (status != PG_OK) {
		if (saved)
			pg_tree_sources_restore(tree, saved);
		pg_tree_batch *added =
			previous ? previous->next : tree->pending_head;

		if (previous)
			previous->next = NULL;
		else
			tree->pending_head = NULL;
		tree->pending_tail = previous;
		while (added) {
			pg_tree_batch *next = added->next;

			free(added->invalid_name);
			free(added->invalid_scope);
			pg_cursor_close(&added->before, NULL);
			pg_cursor_close(&added->after, NULL);
			free(added);
			added = next;
		}
	}
	pg_tree_sources_discard(tree, saved);
	pg_native_hint **at = &tree->hints;

	while (*at) {
		pg_native_hint *hint = *at;

		if (!hint->processed || status != PG_OK) {
			hint->processed = 0;
			at = &hint->next;
			continue;
		}
		*at = hint->next;
		free(hint->name);
		free(hint);
	}
	for (pg_tree_scope *s = tree->scopes; s; s = s->next)
		s->dirty = 0;
	return pg_result(status, error);
}

pg_status pg_tree_queue_topology(pg_tree *tree, pg_error *error)
{
	if (!tree->watch_mode)
		return pg_result(PG_OK, error);
	pg_tree_batch *previous = tree->pending_tail;
	pg_status status = pg_tree_invalidate_managed(tree);

	if (status == PG_OK)
		status = pg_tree_queue_changes(tree, error);
	if (status != PG_OK) {
		pg_tree_batch *added = previous ? previous->next :
			tree->pending_head;

		if (previous)
			previous->next = NULL;
		else
			tree->pending_head = NULL;
		tree->pending_tail = previous;
		while (added) {
			pg_tree_batch *next = added->next;

			free(added->invalid_name);
			free(added->invalid_scope);
			free(added);
			added = next;
		}
	}
	return pg_result(status, error);
}
