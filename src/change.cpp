#include <piggle/piggle.h>
#include "internal.hpp"

#ifdef __linux__
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <dirent.h>

static pg_status pg_tree_watch_path(pg_tree *tree, pg_source *source,
		const char *path, const char *relative, uint32_t mask,
		int *native_code)
{
	int wd = inotify_add_watch(tree->native_fd, path, mask | IN_ONLYDIR |
		IN_DONT_FOLLOW);

	if (wd < 0) {
		*native_code = errno;
		return PG_IO;
	}
	pg_native_watch *watch;

	for (watch = tree->watches; watch; watch = watch->next)
		if (watch->wd == wd)
			break;
	if (!watch) {
		watch = (pg_native_watch *)calloc(1, sizeof(*watch));
		if (!watch) {
			inotify_rm_watch(tree->native_fd, wd);
			return PG_NOMEM;
		}
		watch->wd = wd;
		watch->source = source;
		watch->next = tree->watches;
		tree->watches = watch;
	}
	char *copy = strdup(relative);

	if (!copy)
		return PG_NOMEM;
	free(watch->relative);
	watch->relative = copy;
	if (!*relative)
		source->watch_wd = wd;
	return PG_OK;
}

static pg_status pg_tree_watch_directories(pg_tree *tree, pg_source *source,
		const char *path, const char *relative, uint32_t mask,
		int *native_code)
{
	pg_status status = pg_tree_watch_path(tree, source, path, relative,
		mask, native_code);
	DIR *listing;
	struct dirent *item;

	if (status != PG_OK)
		return status;
	int scan = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);

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
		char *child, *name;
		size_t length = strlen(path), prefix = strlen(relative);
		size_t size = strlen(item->d_name);

		if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, ".."))
			continue;
		if (item->d_type != DT_UNKNOWN && item->d_type != DT_DIR)
			continue;
		if (fstatat(scan, item->d_name, &found, AT_SYMLINK_NOFOLLOW) ||
		    !S_ISDIR(found.st_mode)) {
			errno = 0;
			continue;
		}
		if (length > SIZE_MAX - size - 2 ||
			prefix > SIZE_MAX - size - 2) {
			status = PG_LIMIT;
			break;
		}
		child = (char *)malloc(length + size + 2);
		name = (char *)malloc(prefix + size + 2);
		if (!child || !name) {
			free(child);
			free(name);
			status = PG_NOMEM;
			break;
		}
		snprintf(child, length + size + 2, "%s/%s", path, item->d_name);
		snprintf(name, prefix + size + 2, "%s%s%s", relative,
			prefix ? "/" : "", item->d_name);
		status = pg_tree_watch_directories(tree, source, child, name,
			mask, native_code);
		free(child);
		free(name);
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

static pg_status pg_tree_linux_event(pg_tree *tree,
		const struct inotify_event *event, pg_error *error)
{
	pg_status status;

	if (event->mask & IN_Q_OVERFLOW) {
		tree->native_repair = 1;
		tree->loss_pending = 1;
	}
	pg_native_watch *watch;

	for (watch = tree->watches; watch; watch = watch->next)
		if (watch->wd == event->wd)
			break;
	if (watch && (event->mask & IN_IGNORED)) {
		pg_native_watch **at = &tree->watches;

		while (*at != watch)
			at = &(*at)->next;
		*at = watch->next;
		free(watch->relative);
		free(watch);
		return PG_OK;
	}
	if (watch) {
		if (!*watch->relative &&
		    (event->mask & (IN_MOVE_SELF | IN_DELETE_SELF))) {
			tree->native_repair = 1;
			tree->loss_pending = 1;
		}
		if (event->len && event->name[0]) {
			size_t size = strlen(watch->relative) +
				strlen(event->name) + 2;
			char *name = (char *)malloc(size);

			if (!name)
				return pg_result(PG_NOMEM, error);
			snprintf(name, size, "%s%s%s", watch->relative,
				*watch->relative ? "/" : "", event->name);
			status = pg_tree_hint_add(tree, watch->source,
				name, !!(event->mask & IN_ISDIR));
			if (status == PG_OK && (event->mask & IN_ISDIR) &&
			    (event->mask & (IN_CREATE | IN_MOVED_TO))) {
				size_t path_size =
					strlen(watch->source->native_path)
					+ strlen(name) + 2;
				char *path = (char *)malloc(path_size);
				int code = 0;

				if (!path) {
					free(name);
					return pg_result(PG_NOMEM, error);
				}
				snprintf(path, path_size, "%s/%s",
					watch->source->native_path, name);
				uint32_t mask = IN_ATTRIB | IN_CLOSE_WRITE |
					IN_CREATE | IN_DELETE | IN_MODIFY |
					IN_MOVED_FROM | IN_MOVED_TO |
					IN_MOVE_SELF | IN_DELETE_SELF;

				status = pg_tree_watch_directories(tree,
					watch->source,
					path, name, mask, &code);
				free(path);
				if (status == PG_IO && code == ENOENT)
					status = PG_OK;
			}
			free(name);
			if (status != PG_OK)
				return pg_result(status, error);
		}
	} else {
		for (size_t i = 0; i < tree->count; i++) {
			pg_source *source = tree->sources[i];

			if (source->format == PG_LOOSE ||
			    source->watch_wd != event->wd)
				continue;
			if (event->mask & (IN_MOVE_SELF | IN_DELETE_SELF |
			    IN_IGNORED)) {
				tree->native_repair = 1;
				tree->loss_pending = 1;
			}
			status = pg_tree_hint_add(tree, source, NULL, 1);
			if (status != PG_OK)
				return pg_result(status, error);
		}
	}
	return PG_OK;
}

#endif

#ifdef _WIN32
static pg_status pg_tree_windows_subtree(pg_source *source, const char *name,
		int *subtree)
{
	size_t root = strlen(source->native_path), size = strlen(name);
	struct stat state;

	*subtree = 1;
	if (root > SIZE_MAX - size - 2)
		return PG_LIMIT;
	char *path = (char *)malloc(root + size + 2);

	if (!path)
		return PG_NOMEM;
	snprintf(path, root + size + 2, "%s/%s", source->native_path, name);
	int found = !lstat(path, &state);

	free(path);
	if (found) {
		*subtree = S_ISDIR(state.st_mode);
		return PG_OK;
	}
	size_t capacity = size + 1;
	char *canonical = (char *)malloc(capacity);

	if (!canonical)
		return PG_NOMEM;
	if (pg_name_normalize(source->context, name, canonical, capacity,
		&capacity, NULL) == PG_OK) {
		for (pg_source_record *r = source->records; r; r = r->next) {
			if (!strcmp(canonical, r->info.canonical_name)) {
				*subtree = S_ISDIR(r->identity.st_mode);
				break;
			}
		}
	}
	free(canonical);
	return PG_OK;
}

static pg_status pg_tree_windows_hints(pg_tree *tree, pg_source *source,
		DWORD bytes, pg_error *error)
{
	pg_status status;

	size_t offset = 0;

	while (offset + offsetof(FILE_NOTIFY_INFORMATION, FileName) <
	       bytes) {
		FILE_NOTIFY_INFORMATION *notice =
			(FILE_NOTIFY_INFORMATION *)(source->native_buffer +
				offset);
		if (notice->FileNameLength > bytes - offset -
		    offsetof(FILE_NOTIFY_INFORMATION, FileName)) {
			tree->loss_pending = tree->native_repair = 1;
			break;
		}
		int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
			notice->FileName,
				notice->FileNameLength / sizeof(WCHAR),
			NULL, 0, NULL, NULL);
		char *name = size ? (char *)malloc((size_t)size + 1) : NULL;

		if (!name)
			return pg_result(size ? PG_NOMEM : PG_IO, error);
		WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
			notice->FileName,
				notice->FileNameLength / sizeof(WCHAR),
			name, size, NULL, NULL);
		name[size] = 0;
		int subtree = 1;

		status = source->format == PG_LOOSE ?
			pg_tree_windows_subtree(source, name, &subtree) : PG_OK;
		if (status == PG_OK)
			status = pg_tree_hint_add(tree, source,
				source->format == PG_LOOSE ? name : NULL,
				subtree);
		free(name);
		if (status != PG_OK)
			return pg_result(status, error);
		if (!notice->NextEntryOffset)
			break;
		if (notice->NextEntryOffset <
		    offsetof(FILE_NOTIFY_INFORMATION, FileName) +
			notice->FileNameLength ||
		    notice->NextEntryOffset > bytes - offset) {
			tree->loss_pending = tree->native_repair = 1;
			break;
		}
		offset += notice->NextEntryOffset;
	}
	return PG_OK;
}

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
	if (source->format == PG_LOOSE)
		return pg_tree_watch_directories(tree, source,
			source->native_path, "", mask, native_code);
	int wd = inotify_add_watch(tree->native_fd, source->native_path, mask);

	if (wd < 0) {
		*native_code = errno;
		return PG_IO;
	}
	source->watch_wd = wd;
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
	pg_native_watch **at = &tree->watches;

	while (*at) {
		pg_native_watch *watch = *at;

		if (watch->source != source) {
			at = &watch->next;
			continue;
		}
		*at = watch->next;
		if (tree->native_fd >= 0)
			inotify_rm_watch(tree->native_fd, watch->wd);
		free(watch->relative);
		free(watch);
	}
	if (source->format != PG_LOOSE && tree->native_fd >= 0 &&
	    source->watch_wd >= 0)
		inotify_rm_watch(tree->native_fd, source->watch_wd);
#elif defined(_WIN32)
	(void)tree;
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
	pg_tree_hint_discard(tree, source);
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
		before->attributes == after->attributes &&
		S_ISDIR(before->source_identity.st_mode) ==
		S_ISDIR(after->source_identity.st_mode) &&
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
	pg_entry_info entries[2] = {};
	pg_file *files[2] = { before, after };

	for (int i = 0; i < 2; i++) {
		pg_file *file = files[i];

		if (!file)
			continue;
		entries[i].kind = S_ISDIR(file->source_identity.st_mode) ?
			PG_ENTRY_DIRECTORY : PG_ENTRY_FILE;
		entries[i].attributes = file->attributes;
		entries[i].source_id = file->info.source_id;
		entries[i].canonical_name = file->info.canonical_name;
		entries[i].original_name = file->info.original_name;
		entries[i].size = file->info.logical_size;
		entries[i].mtime = file->info.mtime;
	}
	change.before_entry = before ? &entries[0] : NULL;
	change.after_entry = after ? &entries[1] : NULL;
	change.before = before && entries[0].kind == PG_ENTRY_FILE ?
		&before->info : NULL;
	change.after = after && entries[1].kind == PG_ENTRY_FILE ?
		&after->info : NULL;
	if (observer->visible)
		observer->visible(observer->user, &change);
	return PG_OK;
}

static int pg_change_scope_covered(pg_tree *tree,
		pg_tree_scope *scope)
{
	for (pg_tree_scope *other = tree->scopes; other; other = other->next) {
		if (other == scope || other->exact)
			continue;
		if (other->shallow && !scope->exact)
			continue;
		if (scope->exact && other->prefix &&
		    !strcmp(scope->prefix, other->prefix))
			continue;
		if (!scope->prefix) {
			if (!other->prefix && !other->shallow)
				return 1;
			continue;
		}
		if (pg_name_in_scope(scope->prefix, other->prefix,
			!other->shallow))
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
		free(batch->invalid_name);
		free(batch->invalid_scope);
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

		if ((tree->partial_changes && !scope->dirty) ||
		    (scope->exact && !scope->reader_refs && !scope->managed) ||
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
			free(batch->invalid_name);
			free(batch->invalid_scope);
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
	if (batch->invalid_name || batch->invalid_scope) {
		pg_visible_change change = {};

		if (tree->next_sequence == UINT64_MAX)
			return PG_LIMIT;
		change.sequence = tree->next_sequence++;
		change.kind = PG_CHANGE_INVALIDATE;
		change.canonical_name = batch->invalid_name;
		change.scope = batch->invalid_scope;
		if (observer->visible)
			observer->visible(observer->user, &change);
		return PG_OK;
	}

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

static pg_status pg_tree_native_reconcile_roots(pg_tree *tree,
		pg_root_binding **saved, pg_error *error)
{
	pg_status status = PG_OK;

	if (!tree->polling && !tree->reconciling)
		pg_tree_reader_scope_sweep(tree);
	if (tree->watch_mode != PG_WATCH_NATIVE ||
	    tree->reconciling)
		return pg_result(PG_OK, error);
	for (size_t i = 0; i < tree->count; i++) {
		pg_source *source = tree->sources[i];
		int changed = 0;

		if (source->format != PG_LOOSE)
			continue;
		status = pg_source_rebind_root(source, &changed, saved, error);
		if (status != PG_OK)
			return status;
		if (changed) {
			pg_tree_native_remove_source(tree, source);
			tree->native_repair = tree->native_dirty = 1;
			tree->loss_pending = 1;
		}
	}
#ifdef __linux__
	if (tree->native_fd >= 0) {
		union {
			struct inotify_event aligned;
			uint8_t bytes[8192];
		} pending;

		int available = 0;

		if (ioctl(tree->native_fd, FIONREAD, &available))
			return pg_native_result(PG_IO, errno, error);
		while (available > 0) {
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
			available -= (int)count;
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
				status = pg_tree_linux_event(tree, event,
					error);
				if (status != PG_OK)
					return status;
				at += length;
			}
		}
	}
#endif
#ifdef _WIN32
	for (size_t i = 0; i < tree->count; i++) {
		pg_source *source = tree->sources[i];
		struct stat path_state;
		DWORD bytes = 0;
		int native_code = 0;

		if (source->format != PG_LOOSE && !tree->loss_reported &&
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
		/* Atomic publication can complete the first buffer on temporary
		 * creation, leaving the final rename queued for the next arm.
		 * Drain completed cuts before a lookup consults the retained
		 * index. Bound work under continuous writers instead of
		 * starving traversal.
		 */
		unsigned cuts = 0;
		while (WaitForSingleObject(source->native_event, 0) ==
			WAIT_OBJECT_0) {
			if (cuts++ == 64) return pg_result(PG_RETRY, error);
			tree->native_dirty = 1;
			source->native_armed = 0;
			if (!GetOverlappedResult(source->native_watch,
				    &source->native_overlapped, &bytes,
				    FALSE) ||
				!bytes) {
				tree->loss_pending = 1;
				tree->native_repair = 1;
			}
			if (bytes && !tree->native_repair) {
				status = pg_tree_windows_hints(
					tree, source, bytes, error);
				if (status != PG_OK) return status;
			}
			ResetEvent(source->native_event);
			status = pg_tree_windows_arm(source, &native_code);
			if (status != PG_OK)
				return pg_native_result(
					status, native_code, error);
		}
	}
#endif
	if (!tree->native_dirty && !tree->native_repair)
		return pg_result(PG_OK, error);
	tree->reconciling = 1;
	status = pg_tree_hints_reconcile(tree, error);
	if (status == PG_OK && tree->loss_pending == 1) {
		status = pg_tree_invalidate_managed(tree);
		if (status == PG_OK)
			status = pg_tree_rescan_scopes(tree, error);
		if (status == PG_OK)
			status = pg_tree_queue_changes(tree, error);
	}
	if (status == PG_OK && tree->loss_pending == 1)
		tree->loss_pending = 2;
	if (status == PG_OK && tree->native_repair) {
		int native_code = 0;
		int deferred = 0;
		int observed = tree->managed != NULL;

		pg_tree_scope_lock(tree);
		for (pg_tree_scope *scope = tree->scopes; scope;
		     scope = scope->next) {
			if (!scope->exact || scope->reader_refs ||
				scope->managed)
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
			if (tree->sources[i]->format == PG_LOOSE &&
			    tree->sources[i]->fd < 0) {
				deferred = 1;
				continue;
			}
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
		tree->native_dirty = tree->hints != NULL;
	return status;
}

pg_status pg_tree_native_reconcile(pg_tree *tree, pg_error *error)
{
	pg_root_binding *saved = NULL;
	pg_status status = pg_tree_native_reconcile_roots(tree, &saved, error);

	pg_source_rebind_finish(saved, status == PG_OK);
	return status;
}

extern "C" {

/* Enable one watch mode after refreshing requested baselines. */
static pg_status PG_CALL pg_tree_watch_coordinated(pg_tree *tree, uint32_t mode,
		pg_error *error)
{
	pg_tree_scope *scope;
	pg_tree_scope *original_scopes = NULL;
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
	original_scopes = tree->scopes;
	was_reconciling = tree->reconciling;
	status = pg_tree_sources_save(tree, &saved);
	if (status != PG_OK)
		goto watch_failed;
	if (mode == PG_WATCH_SCAN) {
		status = pg_tree_manage_scan(tree, error);
		if (status != PG_OK) {
			if (error)
				native_code = error->native_code;
			goto watch_failed;
		}
	}
	tree->reconciling = 1;
	status = pg_tree_rescan_scopes(tree, error);
	if (status != PG_OK) {
		if (error)
			native_code = error->native_code;
		goto watch_failed;
	}
	for (scope = tree->scopes; scope; scope = scope->next) {
		if (scope->exact && !scope->reader_refs && !scope->managed)
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
			free(batch->invalid_name);
			free(batch->invalid_scope);
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
		while (tree->scopes != original_scopes) {
			pg_tree_scope *added = tree->scopes;

			tree->scopes = added->next;
			pg_cursor_close(&added->baseline, NULL);
			free(added->prefix);
			free(added);
		}
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

PG_API pg_status PG_CALL pg_tree_watch(pg_tree *tree, uint32_t mode,
		pg_error *error)
{
	pg_context *context = tree ? tree->context : NULL;

	pg_context_lock(context);
	pg_status status = pg_tree_watch_coordinated(tree, mode, error);

	pg_context_unlock(context);
	return status;
}

/* Stop observation while retaining attachment and request state. */
static pg_status PG_CALL pg_tree_unwatch_coordinated(
		pg_tree *tree, pg_error *error)
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
	pg_tree_hint_discard(tree, NULL);
	pg_tree_pending_discard(tree);
	pg_tree_reader_scope_sweep(tree);
	return pg_result(PG_OK, error);
}

PG_API pg_status PG_CALL pg_tree_unwatch(pg_tree *tree, pg_error *error)
{
	pg_context *context = tree ? tree->context : NULL;

	pg_context_lock(context);
	pg_status status = pg_tree_unwatch_coordinated(tree, error);

	pg_context_unlock(context);
	return status;
}

/* Compare one finite set of requested scopes and deliver visible changes. */
PG_API pg_status PG_CALL pg_tree_poll(pg_tree *tree,
		const pg_observer *observer, pg_error *error)
{
	pg_status status = PG_OK;
	pg_tree_batch *delivery = NULL, *cut = NULL;
	int loss_cut = 0;

	if (!tree || !observer)
		return pg_result(PG_INVALID, error);
	pg_context_lock(tree->context);
	if (!tree->watch_mode) {
		pg_context_unlock(tree->context);
		return pg_result(PG_INVALID, error);
	}
	status = pg_tree_control_status(tree, 1);
	if (status != PG_OK) {
		pg_context_unlock(tree->context);
		return pg_result(status, error);
	}
	tree->polling = 1;
	pg_result(PG_OK, error);
	status = pg_tree_native_reconcile(tree, error);
	if (status == PG_OK && tree->watch_mode == PG_WATCH_SCAN) {
		tree->reconciling = 1;
		status = pg_tree_rescan_scopes(tree, error);
		tree->reconciling = 0;
	}
	if (status != PG_OK)
		goto poll_done;
	if (tree->watch_mode == PG_WATCH_SCAN)
		status = pg_tree_queue_changes(tree, error);
	if (status != PG_OK)
		goto poll_done;
	/* Own this poll's reports while workers queue later reports
	 * separately. */
	delivery = tree->pending_head;
	cut = tree->pending_tail;
	loss_cut = tree->loss_pending;
	tree->pending_head = tree->pending_tail = NULL;
	tree->loss_pending = 0;
	pg_context_unlock(tree->context);
	while (delivery) {
		pg_tree_batch *batch = delivery;

		status = pg_change_deliver_batch(tree, batch, observer);
		if (status != PG_OK) {
			pg_context_lock(tree->context);
			cut->next = tree->pending_head;
			tree->pending_head = delivery;
			if (!tree->pending_tail)
				tree->pending_tail = cut;
			tree->loss_pending |= loss_cut;
			goto poll_done;
		}
		delivery = batch->next;
		pg_cursor_close(&batch->before, NULL);
		pg_cursor_close(&batch->after, NULL);
		free(batch->invalid_name);
		free(batch->invalid_scope);
		free(batch);
	}
	pg_context_lock(tree->context);
	if (loss_cut) {
		pg_visible_change change = {};

		if (tree->next_sequence == UINT64_MAX) {
			status = PG_LIMIT;
			if (!tree->loss_pending)
				tree->loss_pending = loss_cut;
		} else {
			change.sequence = tree->next_sequence++;
			change.kind = PG_CHANGE_LOSS;
			tree->loss_reported = 1;
			if (observer->visible) {
				pg_context_unlock(tree->context);
				observer->visible(observer->user,
					&change);
				pg_context_lock(tree->context);
			}
		}
	}
poll_done:
	tree->polling = 0;
	pg_tree_reader_scope_sweep(tree);
	pg_context_unlock(tree->context);
	if (status != PG_OK && error && error->status == status)
		return status;
	return pg_result(status, error);
}

} /* extern "C" */
