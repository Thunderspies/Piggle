#include <piggle/piggle.h>
#include "internal.hpp"

#ifdef __linux__
#include <sys/inotify.h>
#include <fcntl.h>
#include <dirent.h>

static pg_status pg_tree_watch_directories(int fd, const char *path,
		uint32_t mask, int *native_code)
{
	DIR *listing;
	struct dirent *item;
	int scan = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC |
		O_NOFOLLOW);
	pg_status status = PG_OK;

	if (scan < 0) {
		*native_code = errno;
		return PG_IO;
	}
	listing = fdopendir(scan);
	if (!listing) {
		*native_code = errno;
		close(scan);
		return PG_IO;
	}
	errno = 0;
	while ((item = readdir(listing))) {
		struct stat found;
		char *child;
		size_t length, name_length;

		if (strcmp(item->d_name, ".") == 0 ||
		    strcmp(item->d_name, "..") == 0)
			continue;
		if (fstatat(scan, item->d_name, &found,
			AT_SYMLINK_NOFOLLOW) || !S_ISDIR(found.st_mode)) {
			errno = 0;
			continue;
		}
		length = strlen(path);
		name_length = strlen(item->d_name);
		if (length > SIZE_MAX - name_length - 2) {
			status = PG_LIMIT;
			break;
		}
		child = (char *)malloc(length + name_length + 2);
		if (!child) {
			status = PG_NOMEM;
			break;
		}
		memcpy(child, path, length);
		child[length] = '/';
		memcpy(child + length + 1, item->d_name,
			name_length + 1);
		if (inotify_add_watch(fd, child, mask) < 0) {
			*native_code = errno;
			status = PG_IO;
		} else {
			status = pg_tree_watch_directories(fd, child,
				mask, native_code);
		}
		free(child);
		if (status != PG_OK)
			break;
		errno = 0;
	}
	if (!item && errno && status == PG_OK) {
		*native_code = errno;
		status = PG_IO;
	}
	closedir(listing);
	return status;
}
#endif

#ifdef _WIN32
static pg_status pg_tree_windows_arm(pg_source *source,
		int *native_code)
{
	DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME |
		FILE_NOTIFY_CHANGE_DIR_NAME |
		FILE_NOTIFY_CHANGE_SIZE |
		FILE_NOTIFY_CHANGE_LAST_WRITE |
		FILE_NOTIFY_CHANGE_ATTRIBUTES;

	source->native_armed = 0;
	memset(&source->native_overlapped, 0,
		sizeof(source->native_overlapped));
	source->native_overlapped.hEvent = source->native_event;
	if (!ReadDirectoryChangesW(source->native_watch,
		source->native_buffer, sizeof(source->native_buffer),
		source->format == PG_LOOSE, filter, NULL,
		&source->native_overlapped, NULL)) {
		*native_code = (int)GetLastError();
		return PG_IO;
	}
	source->native_armed = 1;
	return PG_OK;
}
#endif

pg_status pg_tree_native_add_source(pg_tree *tree, pg_source *source,
		int *native_code)
{
#ifdef __linux__
	uint32_t mask = IN_ATTRIB | IN_CLOSE_WRITE | IN_CREATE |
		IN_DELETE | IN_MODIFY | IN_MOVED_FROM | IN_MOVED_TO |
		IN_MOVE_SELF | IN_DELETE_SELF;
	int wd = inotify_add_watch(tree->native_fd, source->native_path,
		mask);

	if (wd < 0) {
		*native_code = errno;
		return PG_IO;
	}
	source->watch_wd = wd;
	if (source->format == PG_LOOSE)
		return pg_tree_watch_directories(tree->native_fd,
			source->native_path, mask, native_code);
	return PG_OK;
#elif defined(_WIN32)
	char *parent = NULL;
	const char *path = source->native_path;
	const char *basename = pg_native_basename(path);
	const char *separator = basename > path ? basename - 1 : NULL;
	pg_status status;

	if (source->format != PG_LOOSE) {
		size_t length = separator ?
			(size_t)(separator - path) : 0;

		if (length == 2 && path[1] == ':')
			length++;

		parent = (char *)malloc(length + 2);
		if (!parent)
			return PG_NOMEM;
		if (length)
			memcpy(parent, path, length);
		parent[length] = length ? '\0' : '.';
		parent[length ? length : 1] = '\0';
		path = parent;
	}
	source->native_watch = pg_win_create_file(path, FILE_LIST_DIRECTORY,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS |
		FILE_FLAG_OVERLAPPED, NULL);
	free(parent);
	if (source->native_watch == INVALID_HANDLE_VALUE) {
		*native_code = (int)GetLastError();
		source->native_watch = NULL;
		return PG_IO;
	}
	source->native_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!source->native_event) {
		*native_code = (int)GetLastError();
		CloseHandle(source->native_watch);
		source->native_watch = NULL;
		return PG_IO;
	}
	status = pg_tree_windows_arm(source, native_code);
	if (status != PG_OK)
		pg_tree_native_remove_source(tree, source);
	return status;
#else
	(void)tree;
	(void)source;
	(void)native_code;
	return PG_UNSUPPORTED;
#endif
}

void pg_tree_native_remove_source(pg_tree *tree, pg_source *source)
{
#ifdef __linux__
	if (tree->native_fd >= 0 && source->watch_wd >= 0)
		inotify_rm_watch(tree->native_fd, source->watch_wd);
#elif defined(_WIN32)
	if (source->native_watch) {
		CancelIoEx(source->native_watch, NULL);
		CloseHandle(source->native_watch);
		source->native_watch = NULL;
	}
	if (source->native_event) {
		CloseHandle(source->native_event);
		source->native_event = NULL;
	}
	source->native_armed = 0;
#else
	(void)tree;
#endif
	source->watch_wd = -1;
}

static int pg_change_same(const pg_file *before, const pg_file *after)
{
	const pg_file_info *a = &before->info;
	const pg_file_info *b = &after->info;

	return a->source_id == b->source_id &&
		a->copy_id == b->copy_id &&
		a->copy_generation == b->copy_generation &&
		a->logical_size == b->logical_size &&
		a->stored_size == b->stored_size &&
		a->mtime == b->mtime &&
		memcmp(a->digest, b->digest, sizeof(a->digest)) == 0;
}

static pg_status pg_change_emit(pg_tree *tree,
		const pg_observer *observer, pg_file *before,
		pg_file *after)
{
	pg_visible_change change = {};

	if (tree->next_sequence == UINT64_MAX)
		return PG_LIMIT;
	change.sequence = tree->next_sequence++;
	change.kind = before ? after ? PG_CHANGE_UPDATE :
		PG_CHANGE_REMOVE : PG_CHANGE_ADD;
	change.canonical_name = after ? after->info.canonical_name :
		before->info.canonical_name;
	change.before = before ? &before->info : NULL;
	change.after = after ? &after->info : NULL;
	if (observer->visible)
		observer->visible(observer->user, &change);
	return PG_OK;
}

static int pg_change_scope_covered(pg_tree *tree,
		pg_tree_scope *scope)
{
	pg_tree_scope *other;

	for (other = tree->scopes; other; other = other->next) {
		size_t length;

		if (other == scope || other->exact)
			continue;
		if (!other->prefix)
			return 1;
		if (!scope->prefix)
			continue;
		length = strlen(other->prefix);
		if (strncmp(other->prefix, scope->prefix,
			length) == 0 && scope->prefix[length] == '/')
			return 1;
	}
	return 0;
}

void pg_tree_pending_discard(pg_tree *tree)
{
	while (tree->pending_head) {
		pg_tree_batch *batch = tree->pending_head;

		tree->pending_head = batch->next;
		pg_cursor_close(&batch->before, NULL);
		pg_cursor_close(&batch->after, NULL);
		free(batch);
	}
	tree->pending_tail = NULL;
}

pg_status pg_tree_queue_changes(pg_tree *tree, pg_error *error)
{
	pg_tree_scope *scope;
	pg_tree_batch *head = NULL, *tail = NULL;
	pg_tree_source_state *saved = NULL;
	pg_status status = PG_OK;
	int reconciling;

	if (!tree || tree->closed || !tree->watch_mode)
		return pg_result(PG_OK, error);
	pg_tree_scope_lock(tree);
	status = pg_tree_sources_save(tree, &saved);
	if (status != PG_OK) {
		pg_tree_scope_unlock(tree);
		return pg_result(status, error);
	}
	reconciling = tree->reconciling;
	tree->reconciling = 1;
	for (scope = tree->scopes; scope; scope = scope->next) {
		pg_tree_batch *batch;

		if ((scope->exact && !scope->reader_refs) ||
		    pg_change_scope_covered(tree, scope))
			continue;
		batch = (pg_tree_batch *)calloc(1, sizeof(*batch));
		if (!batch) {
			status = PG_NOMEM;
			break;
		}
		status = pg_tree_scope_snapshot(tree, scope,
			&batch->after, error);
		if (status == PG_OK)
			status = pg_tree_cursor_clone(batch->after,
				&batch->replacement);
		if (status != PG_OK) {
			pg_cursor_close(&batch->after, NULL);
			free(batch);
			break;
		}
		batch->scope = scope;
		if (tail)
			tail->next = batch;
		else
			head = batch;
		tail = batch;
	}
	if (status != PG_OK) {
		tree->reconciling = reconciling;
		pg_tree_sources_restore(tree, saved);
		while (head) {
			pg_tree_batch *next = head->next;

			pg_cursor_close(&head->after, NULL);
			pg_cursor_close(&head->replacement, NULL);
			free(head);
			head = next;
		}
		if (!error || error->status != status)
			pg_result(status, error);
		pg_tree_scope_unlock(tree);
		pg_tree_sources_discard(tree, saved);
		return status;
	}
	while (head) {
		pg_tree_batch *next = head->next;

		head->before = head->scope->baseline;
		head->scope->baseline = head->replacement;
		head->replacement = NULL;
		head->next = NULL;
		if (tree->pending_tail)
			tree->pending_tail->next = head;
		else
			tree->pending_head = head;
		tree->pending_tail = head;
		head = next;
	}
	tree->reconciling = reconciling;
	pg_tree_scope_unlock(tree);
	pg_tree_sources_discard(tree, saved);
	return pg_result(PG_OK, error);
}

static pg_status pg_change_deliver_batch(pg_tree *tree,
		pg_tree_batch *batch, const pg_observer *observer)
{
	while (batch->before_index < batch->before->count ||
	       batch->after_index < batch->after->count) {
		pg_file *before = batch->before_index <
			batch->before->count ?
			batch->before->files[batch->before_index] : NULL;
		pg_file *after = batch->after_index <
			batch->after->count ?
			batch->after->files[batch->after_index] : NULL;
		int compare = !before ? 1 : !after ? -1 :
			strcmp(before->info.canonical_name,
				after->info.canonical_name);
		pg_status status = PG_OK;

		if (compare < 0) {
			status = pg_change_emit(tree, observer,
				before, NULL);
			if (status != PG_OK)
				return status;
			batch->before_index++;
		} else if (compare > 0) {
			status = pg_change_emit(tree, observer,
				NULL, after);
			if (status != PG_OK)
				return status;
			batch->after_index++;
		} else {
			if (!pg_change_same(before, after))
				status = pg_change_emit(tree,
					observer, before, after);
			if (status != PG_OK)
				return status;
			batch->before_index++;
			batch->after_index++;
		}
	}
	return PG_OK;
}

pg_status pg_tree_native_reconcile(pg_tree *tree, pg_error *error)
{
	pg_status status = PG_OK;

	if (!tree->polling && !tree->reconciling)
		pg_tree_reader_scope_sweep(tree);
	if (tree->watch_mode != PG_WATCH_NATIVE ||
	    tree->reconciling)
		return pg_result(PG_OK, error);
#ifdef __linux__
	if (tree->native_fd >= 0) {
		union {
			struct inotify_event aligned;
			uint8_t bytes[8192];
		} pending;

		for (;;) {
			ssize_t count = read(tree->native_fd,
				pending.bytes, sizeof(pending.bytes));
			size_t at = 0;

			if (count < 0 && errno == EINTR)
				continue;
			if (count < 0 && errno == EAGAIN)
				break;
			if (count < 0)
				return pg_native_result(PG_IO, errno,
					error);
			if (!count)
				break;
			tree->native_dirty = 1;
			while (at + sizeof(struct inotify_event) <=
			       (size_t)count) {
				struct inotify_event *event =
					(struct inotify_event *)(
						pending.bytes + at);
				size_t length = sizeof(*event) +
					event->len;

				if (length > (size_t)count - at)
					break;
				if (event->mask & (IN_MOVE_SELF |
				    IN_DELETE_SELF | IN_IGNORED |
				    IN_Q_OVERFLOW)) {
					tree->native_repair = 1;
					if (!tree->loss_reported)
						tree->loss_pending = 1;
				}
				if ((event->mask & IN_ISDIR) &&
				    (event->mask & (IN_CREATE | IN_MOVED_TO)))
					tree->native_repair = 1;
				at += length;
			}
		}
	}
#endif
#ifdef _WIN32
	for (size_t i = 0; i < tree->count; i++) {
		pg_source *source = tree->sources[i];
		struct stat path_state;
		DWORD bytes;
		int native_code = 0;

		if (!tree->loss_reported &&
		    (lstat(source->native_path, &path_state) ||
		     path_state.st_dev != source->identity.st_dev ||
		     path_state.st_ino != source->identity.st_ino)) {
			tree->loss_pending = 1;
			tree->native_dirty = 1;
			tree->native_repair = 1;
		}

		if (!source->native_watch ||
		    !source->native_armed) {
			if (source->native_watch) {
				tree->loss_pending = 1;
				tree->native_dirty = 1;
				tree->native_repair = 1;
				status = pg_tree_windows_arm(source,
					&native_code);
				if (status != PG_OK)
					return pg_native_result(status,
						native_code, error);
			}
			continue;
		}
		if (WaitForSingleObject(source->native_event, 0) !=
			WAIT_OBJECT_0)
			continue;
		tree->native_dirty = 1;
		source->native_armed = 0;
		if (!GetOverlappedResult(source->native_watch,
			&source->native_overlapped, &bytes, FALSE) ||
		    !bytes) {
			tree->loss_pending = 1;
			tree->native_repair = 1;
		}
		ResetEvent(source->native_event);
		status = pg_tree_windows_arm(source, &native_code);
		if (status != PG_OK)
			return pg_native_result(status, native_code,
				error);
	}
#endif
	if (!tree->native_dirty && !tree->native_repair)
		return pg_result(PG_OK, error);
	tree->reconciling = 1;
	status = pg_tree_rescan_scopes(tree, error);
	if (status == PG_OK && tree->native_repair) {
		int native_code = 0;
		int deferred = 0;
		int observed = 0;

		pg_tree_scope_lock(tree);
		for (pg_tree_scope *scope = tree->scopes; scope;
		     scope = scope->next) {
			if (!scope->exact || scope->reader_refs)
				observed = 1;
		}
		pg_tree_scope_unlock(tree);

		for (size_t i = 0; i < tree->count; i++) {
			if (!observed && tree->sources[i]->format == PG_LOOSE) {
				deferred = 1;
				continue;
			}
#ifdef _WIN32
			pg_tree_native_remove_source(tree, tree->sources[i]);
#endif
			status = pg_tree_native_add_source(tree,
				tree->sources[i], &native_code);
			if (status != PG_OK) {
				status = pg_native_result(status,
					native_code, error);
				break;
			}
		}
		if (status == PG_OK && !deferred)
			tree->native_repair = 0;
	}
	tree->reconciling = 0;
	if (status == PG_OK)
		tree->native_dirty = 0;
	return status;
}

extern "C" {

/* Enable one watch mode after refreshing requested baselines. */
PG_API pg_status PG_CALL pg_tree_watch(pg_tree *tree, uint32_t mode,
		pg_error *error)
{
	pg_tree_scope *scope;
	pg_tree_batch *head = NULL, *tail = NULL;
	pg_tree_source_state *saved = NULL;
	pg_status status;
	int native_code = 0;
	int scopes_locked = 0;
	int was_reconciling = 0;
	size_t i;

	if (!tree || mode < PG_WATCH_NATIVE || mode > PG_WATCH_SCAN)
		return pg_result(PG_INVALID, error);
	status = pg_tree_control_status(tree, 1);
	if (status != PG_OK)
		return pg_result(status, error);
	if (tree->watch_mode && tree->watch_mode != mode)
		return pg_result(PG_INVALID, error);
	if (tree->watch_mode == mode)
		return pg_result(PG_OK, error);
	if (mode == PG_WATCH_NATIVE) {
#ifdef __linux__
		tree->native_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
		if (tree->native_fd < 0) {
			native_code = errno;
			return pg_native_result(PG_IO, native_code,
				error);
		}
		for (i = 0; i < tree->count; i++) {
			status = pg_tree_native_add_source(tree,
				tree->sources[i], &native_code);
			if (status != PG_OK)
				goto watch_failed;
		}
#elif defined(_WIN32)
		for (i = 0; i < tree->count; i++) {
			status = pg_tree_native_add_source(tree,
				tree->sources[i], &native_code);
			if (status != PG_OK)
				goto watch_failed;
		}
#else
		return pg_result(PG_UNSUPPORTED, error);
#endif
	}
	pg_tree_scope_lock(tree);
	scopes_locked = 1;
	was_reconciling = tree->reconciling;
	status = pg_tree_sources_save(tree, &saved);
	if (status != PG_OK)
		goto watch_failed;
	tree->reconciling = 1;
	status = pg_tree_rescan_scopes(tree, error);
	if (status != PG_OK) {
		if (error)
			native_code = error->native_code;
		goto watch_failed;
	}
	for (scope = tree->scopes; scope; scope = scope->next) {
		if (scope->exact && !scope->reader_refs)
			continue;
		pg_tree_batch *batch = (pg_tree_batch *)calloc(1,
			sizeof(*batch));

		if (!batch) {
			status = PG_NOMEM;
			goto watch_failed;
		}
		status = pg_tree_scope_snapshot(tree, scope, &batch->after,
			error);
		if (status != PG_OK) {
			if (error)
				native_code = error->native_code;
			free(batch);
			goto watch_failed;
		}
		batch->scope = scope;
		if (tail)
			tail->next = batch;
		else
			head = batch;
		tail = batch;
	}
	while (head) {
		pg_tree_batch *next = head->next;

		pg_cursor_close(&head->scope->baseline, NULL);
		head->scope->baseline = head->after;
		free(head);
		head = next;
	}
	pg_tree_scope_unlock(tree);
	scopes_locked = 0;
	tree->reconciling = was_reconciling;
	pg_tree_sources_discard(tree, saved);
	tree->watch_mode = mode;
	tree->loss_reported = 0;
	tree->loss_pending = 0;
	tree->native_dirty = 0;
	tree->native_repair = 0;
	return pg_result(PG_OK, error);

watch_failed:
	while (head) {
		pg_tree_batch *next = head->next;

		pg_cursor_close(&head->after, NULL);
		free(head);
		head = next;
	}
	if (scopes_locked) {
		if (saved)
			pg_tree_sources_restore(tree, saved);
		tree->reconciling = was_reconciling;
		pg_tree_scope_unlock(tree);
	}
	pg_tree_sources_discard(tree, saved);
	if (tree->native_fd >= 0) {
		for (i = 0; i < tree->count; i++)
			pg_tree_native_remove_source(tree,
				tree->sources[i]);
		close(tree->native_fd);
		tree->native_fd = -1;
	}
#ifdef _WIN32
	for (i = 0; i < tree->count; i++)
		pg_tree_native_remove_source(tree, tree->sources[i]);
#endif
	return pg_native_result(status, native_code, error);
}

/* Stop observation while retaining attachment and request state. */
PG_API pg_status PG_CALL pg_tree_unwatch(pg_tree *tree, pg_error *error)
{
	if (!tree)
		return pg_result(PG_INVALID, error);
	pg_status status = pg_tree_control_status(tree, 1);

	if (status != PG_OK)
		return pg_result(status, error);
	if (tree->native_fd >= 0) {
		for (size_t i = 0; i < tree->count; i++)
			pg_tree_native_remove_source(tree,
				tree->sources[i]);
		close(tree->native_fd);
		tree->native_fd = -1;
	}
#ifdef _WIN32
	for (size_t i = 0; i < tree->count; i++)
		pg_tree_native_remove_source(tree, tree->sources[i]);
#endif
	tree->watch_mode = PG_WATCH_OFF;
	tree->loss_reported = 0;
	tree->loss_pending = 0;
	tree->native_dirty = 0;
	tree->native_repair = 0;
	pg_tree_pending_discard(tree);
	pg_tree_reader_scope_sweep(tree);
	return pg_result(PG_OK, error);
}

/* Compare one finite set of requested scopes and deliver visible changes. */
PG_API pg_status PG_CALL pg_tree_poll(pg_tree *tree,
		const pg_observer *observer, pg_error *error)
{
	pg_status status = PG_OK;

	if (!tree || !observer || !tree->watch_mode)
		return pg_result(PG_INVALID, error);
	status = pg_tree_control_status(tree, 0);
	if (status != PG_OK)
		return pg_result(status, error);
	if (tree->polling)
		return pg_result(PG_REENTRANT, error);
	pg_tree_scope_lock(tree);
	tree->polling = 1;
	pg_tree_scope_unlock(tree);
	status = pg_tree_native_reconcile(tree, error);
	if (status == PG_OK && tree->watch_mode == PG_WATCH_SCAN) {
		tree->reconciling = 1;
		status = pg_tree_rescan_scopes(tree, error);
		tree->reconciling = 0;
	}
	if (status != PG_OK) {
		pg_tree_scope_lock(tree);
		tree->polling = 0;
		pg_tree_scope_unlock(tree);
		pg_tree_reader_scope_sweep(tree);
		return pg_result(status, error);
	}
	status = pg_tree_queue_changes(tree, error);
	if (status != PG_OK) {
		pg_tree_scope_lock(tree);
		tree->polling = 0;
		pg_tree_scope_unlock(tree);
		pg_tree_reader_scope_sweep(tree);
		return status;
	}
	while (tree->pending_head) {
		pg_tree_batch *batch = tree->pending_head;

		status = pg_change_deliver_batch(tree, batch, observer);
		if (status != PG_OK) {
			pg_tree_scope_lock(tree);
			tree->polling = 0;
			pg_tree_scope_unlock(tree);
			pg_tree_reader_scope_sweep(tree);
			return pg_result(status, error);
		}
		tree->pending_head = batch->next;
		if (!tree->pending_head)
			tree->pending_tail = NULL;
		pg_cursor_close(&batch->before, NULL);
		pg_cursor_close(&batch->after, NULL);
		free(batch);
	}
	if (tree->loss_pending) {
		pg_visible_change change = {};

		if (tree->next_sequence == UINT64_MAX) {
			status = PG_LIMIT;
		} else {
			change.sequence = tree->next_sequence++;
			change.kind = PG_CHANGE_LOSS;
			tree->loss_pending = 0;
			tree->loss_reported = 1;
			if (observer->visible)
				observer->visible(observer->user,
					&change);
		}
	}
	pg_tree_scope_lock(tree);
	tree->polling = 0;
	pg_tree_scope_unlock(tree);
	pg_tree_reader_scope_sweep(tree);
	if (status != PG_OK)
		return pg_result(status, error);
	return pg_result(PG_OK, error);
}

} /* extern "C" */
