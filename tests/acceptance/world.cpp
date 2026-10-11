#include "world.hpp"

#ifdef __linux__
extern "C" void pg_test_journal_fault(int mode);
#endif

void PG_CALL World::observe(void *user, const pg_visible_change *change)
{
	auto *world = static_cast<World *>(user);
	const pg_entry_info *before = change->before_entry;
	const pg_entry_info *after = change->after_entry;

	world->events.push_back({change->kind,
		change->canonical_name ? change->canonical_name : "",
		change->scope ? change->scope : "", before != nullptr,
		after != nullptr, before ? before->size : 0,
		after ? after->size : 0,
		(before && before->kind == PG_ENTRY_DIRECTORY) ||
			(after && after->kind == PG_ENTRY_DIRECTORY)});
	world->sequences.push_back(change->sequence);
	if (world->callback_done)
		return;
	world->callback_done = true;
	pg_error error;
	pg_observer observer = {};

	if (world->callback_lookup) {
		char bytes[64];
		size_t size = 0;

		world->callback_status = pg_tree_read_all(world->tree,
			"a", bytes, sizeof(bytes), &size, &error);
		if (world->callback_status == PG_OK)
			world->callback_bytes.assign(bytes, size);
		world->recursive_status =
			pg_tree_poll(world->tree, &observer, &error);
		world->mutation_status =
			pg_source_write_all(world->source(), "a",
				"disallowed", 10, nullptr, &error);
	}
	if (world->callback_replace_root) {
		pg_file *selection = nullptr;

		if (rename(world->paths.at(world->active).c_str(),
				"moved-root")) {
			world->callback_status = PG_IO;
			return;
		}
		world->callback_status = pg_tree_find(
			world->tree, "missing", &selection, &error);
		pg_file_close(&selection, &error);
	}
	if (world->callback_worker) {
		std::thread worker([world]() {
			pg_error diagnostic;
			pg_file *selection = nullptr;
			pg_cursor *cursor = nullptr;
			bool success = pg_tree_find(world->tree, "a",
						&selection,
						&diagnostic) == PG_OK;

			success = pg_file_close(&selection,
						&diagnostic) == PG_OK &&
				success;
			success = pg_tree_files(world->tree, nullptr,
						&cursor,
						&diagnostic) == PG_OK &&
				success;
			success = pg_cursor_close(&cursor,
						&diagnostic) == PG_OK &&
				success;
			world->worker_success = success;
		});

		worker.join();
		world->recursive_status =
			pg_tree_poll(world->tree, &observer, &error);
		world->mutation_status =
			pg_source_write_all(world->source(), "a",
				"disallowed", 10, nullptr, &error);
		std::ofstream output(
			world->paths.at(world->active) + "/late");

		output << "late";
		output.close();
		world->callback_status = output.fail() ? PG_IO : PG_OK;
	}
}

BEFORE()
{
	WorldScope world;
}

AFTER()
{
#ifdef __linux__
	pg_test_journal_fault(0);
#endif
	piggle_test::release_resources();
}
