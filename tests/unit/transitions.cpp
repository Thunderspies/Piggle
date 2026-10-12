#include "../api_test.hpp"

class DeletedArchive : public piggle_test::ContractTest,
			public testing::WithParamInterface<uint32_t>
{
};

TEST_P(DeletedArchive, RetainedSelectionReportsStale)
{
	pg_context *context = nullptr;
	pg_archive_builder *builder = nullptr;
	pg_source *source = nullptr;
	pg_file *file = nullptr;
	pg_reader *reader = nullptr;
	pg_error error;
	pg_source_options options = {GetParam(), PG_WRITE, 0};

	ASSERT_EQ(pg_context_open(&context, &error), PG_OK);
	ASSERT_EQ(pg_archive_builder_create(context, "archive",
				GetParam(), 0, &builder, &error),
		PG_OK);
	ASSERT_EQ(pg_archive_builder_write_all(
				builder, "a", "old", 3, nullptr, &error),
		PG_OK);
	ASSERT_EQ(pg_archive_builder_finish(builder, &error), PG_OK);
	ASSERT_EQ(pg_archive_builder_close(&builder, &error), PG_OK);
	ASSERT_EQ(pg_source_open(context, "archive", &options, &source,
				&error),
		PG_OK);
	ASSERT_EQ(pg_source_find(source, "a", &file, &error), PG_OK);
	ASSERT_EQ(pg_source_delete(source, &error), PG_OK);
	EXPECT_EQ(
		pg_reader_open(file, PG_READ_LOGICAL, &reader, &error),
		PG_STALE);
	EXPECT_EQ(reader, nullptr);
	EXPECT_EQ(error.status, PG_STALE);
}

INSTANTIATE_TEST_SUITE_P(Formats, DeletedArchive,
	testing::Values(static_cast<uint32_t>(PG_PIGG2),
		static_cast<uint32_t>(PG_HOGG10)));

class WatchedSubtree : public piggle_test::ContractTest,
			public testing::WithParamInterface<uint32_t>
{
};

struct Transitions {
	unsigned removed = 0;
	unsigned added = 0;
};

static void PG_CALL transition(
	void *user, const pg_visible_change *change)
{
	auto *counts = static_cast<Transitions *>(user);

	if (change->kind == PG_CHANGE_REMOVE &&
		change->canonical_name &&
		strcmp(change->canonical_name, "a/b") == 0)
		counts->removed++;
	if (change->kind == PG_CHANGE_ADD && change->canonical_name &&
		strcmp(change->canonical_name, "a") == 0)
		counts->added++;
}

TEST_P(WatchedSubtree, DetachingDescendantsRevealsEarlierFile)
{
	ASSERT_EQ(directory("base"), 0);
	ASSERT_EQ(directory("patch"), 0);
	ASSERT_EQ(directory("patch/a"), 0);
	ASSERT_EQ(put_bytes("base/a", "file", 4), 0);
	ASSERT_EQ(put_bytes("patch/a/b", "child", 5), 0);
	pg_context *context = nullptr;
	pg_tree *tree = nullptr;
	pg_source *subtree = nullptr;
	pg_file *file = nullptr;
	pg_error error;
	pg_source_spec inputs[] = {{"base", {PG_LOOSE, PG_READ, 0}},
		{"patch", {PG_LOOSE, PG_READ, 0}}};
	Transitions counts;
	pg_observer observer = {&counts, transition};

	ASSERT_EQ(pg_context_open(&context, &error), PG_OK);
	ASSERT_EQ(
		pg_tree_open(context, inputs, 2, &tree, &error), PG_OK);
	ASSERT_EQ(pg_tree_source(tree, 1, &subtree, &error), PG_OK);
	ASSERT_EQ(pg_tree_request_subtree(tree, "a", &error), PG_OK);
	ASSERT_EQ(pg_tree_watch(tree, GetParam(), &error), PG_OK);
	ASSERT_EQ(pg_tree_detach(tree, subtree, &error), PG_OK);
	ASSERT_EQ(pg_tree_poll(tree, &observer, &error), PG_OK);
	EXPECT_EQ(counts.removed, 1u);
	EXPECT_EQ(counts.added, 1u);
	ASSERT_EQ(pg_tree_find(tree, "a", &file, &error), PG_OK);
	char bytes[8];
	size_t size = 0;

	ASSERT_EQ(pg_file_read_all(
				file, bytes, sizeof(bytes), &size, &error),
		PG_OK);
	EXPECT_EQ(std::string(bytes, size), "file");
}

#if defined(__linux__) || defined(_WIN32)
INSTANTIATE_TEST_SUITE_P(Modes, WatchedSubtree,
	testing::Values(static_cast<uint32_t>(PG_WATCH_SCAN),
		static_cast<uint32_t>(PG_WATCH_NATIVE)));
#else
INSTANTIATE_TEST_SUITE_P(
	Modes, WatchedSubtree,
	testing::Values(static_cast<uint32_t>(PG_WATCH_SCAN)));
#endif
