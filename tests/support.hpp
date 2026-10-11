#ifndef PIGGLE_TEST_SUPPORT_HPP
#define PIGGLE_TEST_SUPPORT_HPP

#include <gtest/gtest.h>
#include <piggle/piggle.h>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <memory>
#include <vector>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

namespace piggle_test
{

struct Resource {
	void *identity;
	std::function<bool()> close;
};
inline std::mutex resource_mutex;
inline std::vector<Resource> resources;

inline void forget(void *identity)
{
	if (!identity)
		return;
	std::lock_guard<std::mutex> lock(resource_mutex);
	auto found = std::find_if(resources.begin(), resources.end(),
		[identity](const auto &entry) {
			return entry.identity == identity;
		});

	if (found != resources.end())
		resources.erase(found);
}

template <class T>
inline void retain(
	T *identity, pg_status(PG_CALL *close)(T **, pg_error *))
{
	std::lock_guard<std::mutex> lock(resource_mutex);

	resources.push_back({identity, [identity, close]() {
					T *owned = identity;

					close(&owned, nullptr);
					return owned == nullptr;
				}});
}

inline void retain_buffer(pg_buffer buffer)
{
	std::lock_guard<std::mutex> lock(resource_mutex);

	resources.push_back({buffer.data, [buffer]() mutable {
					::pg_buffer_free(&buffer);
					return true;
				}});
}

inline void release_resources()
{
	std::lock_guard<std::mutex> lock(resource_mutex);

	while (!resources.empty()) {
		bool progress = false;

		for (size_t i = resources.size(); i; --i) {
			if (resources[i - 1].close()) {
				resources.erase(
					resources.begin() + i - 1);
				progress = true;
			}
		}
		if (!progress) {
			ADD_FAILURE() << "Unreleased test handles: "
					<< resources.size();
			break;
		}
	}
	resources.clear();
}

class Workspace
{
	std::filesystem::path previous;
	std::filesystem::path directory;

	public:
	Workspace()
	{
		static std::atomic<unsigned> next{0};
#ifdef _WIN32
		int process = _getpid();
#else
		int process = getpid();
#endif
		previous = std::filesystem::current_path();
		directory = std::filesystem::path(PIGGLE_TEST_ROOT) /
			(std::to_string(process) + "-" +
				std::to_string(next.fetch_add(1)));
		std::filesystem::create_directories(directory);
		std::filesystem::current_path(directory);
	}
	~Workspace()
	{
		release_resources();
		std::error_code error;

		std::filesystem::current_path(previous, error);
		EXPECT_FALSE(error) << error.message();
		std::filesystem::remove_all(directory, error);
		EXPECT_FALSE(error) << error.message();
	}
};

class ContractTest : public testing::Test
{
	std::unique_ptr<Workspace> workspace;

	protected:
	void SetUp() override
	{
		workspace = std::make_unique<Workspace>();
	}
	void TearDown() override
	{
		workspace.reset();
	}
};

inline testing::AssertionResult status_result(
	pg_status actual, pg_status expected, const pg_error &error)
{
	if (actual != expected || error.status != actual ||
		!memchr(error.message, 0, sizeof(error.message)))
		return testing::AssertionFailure()
			<< "expected " << expected << ", got " << actual
			<< ", error " << error.status << ", cause "
			<< error.cause << ", native "
			<< error.native_code;
	if (actual == PG_OK || actual == PG_END ||
		actual == PG_INVALID) {
		if (error.cause != actual || error.native_code != 0 ||
			error.offset != UINT64_MAX)
			return testing::AssertionFailure()
				<< "Diagnostic does not describe the "
					"result";
	}
	return testing::AssertionSuccess();
}

inline pg_status PG_CALL tracked_pg_context_open(
	pg_context **out, pg_error *error)
{
	auto status = ::pg_context_open(out, error);
	if (status == PG_OK && out && *out)
		retain(*out, ::pg_context_close);
	return status;
}

inline pg_status PG_CALL tracked_pg_context_close(
	pg_context **context, pg_error *error)
{
	auto before = context ? *context : nullptr;
	auto status = ::pg_context_close(context, error);
	if (context && !*context)
		forget(before);
	return status;
}

inline pg_status PG_CALL tracked_pg_tree_create(
	pg_context *context, pg_tree **out, pg_error *error)
{
	auto status = ::pg_tree_create(context, out, error);
	if (status == PG_OK && out && *out)
		retain(*out, ::pg_tree_close);
	return status;
}

inline pg_status PG_CALL tracked_pg_tree_open(pg_context *context,
	const pg_source_spec *sources, size_t count, pg_tree **out,
	pg_error *error)
{
	auto status =
		::pg_tree_open(context, sources, count, out, error);
	if (status == PG_OK && out && *out)
		retain(*out, ::pg_tree_close);
	return status;
}

inline pg_status PG_CALL tracked_pg_tree_source(
	pg_tree *tree, size_t index, pg_source **out, pg_error *error)
{
	auto status = ::pg_tree_source(tree, index, out, error);
	if (status == PG_OK && out && *out)
		retain(*out, ::pg_source_close);
	return status;
}

inline pg_status PG_CALL tracked_pg_tree_read_all_alloc(pg_tree *tree,
	const char *name, size_t max_bytes, pg_buffer *out,
	pg_error *error)
{
	auto status = ::pg_tree_read_all_alloc(
		tree, name, max_bytes, out, error);
	if (status == PG_OK && out && out->data)
		retain_buffer(*out);
	return status;
}

inline pg_status PG_CALL tracked_pg_tree_find(
	pg_tree *tree, const char *name, pg_file **out, pg_error *error)
{
	auto status = ::pg_tree_find(tree, name, out, error);
	if (status == PG_OK && out && *out)
		retain(*out, ::pg_file_close);
	return status;
}

inline pg_status PG_CALL tracked_pg_tree_files(pg_tree *tree,
	const char *prefix, pg_cursor **out, pg_error *error)
{
	auto status = ::pg_tree_files(tree, prefix, out, error);
	if (status == PG_OK && out && *out)
		retain(*out, ::pg_cursor_close);
	return status;
}

inline pg_status PG_CALL tracked_pg_tree_close(
	pg_tree **tree, pg_error *error)
{
	auto before = tree ? *tree : nullptr;
	auto status = ::pg_tree_close(tree, error);
	if (tree && !*tree)
		forget(before);
	return status;
}

inline pg_status PG_CALL tracked_pg_source_open(pg_context *context,
	const char *native_path, const pg_source_options *options,
	pg_source **out, pg_error *error)
{
	auto status = ::pg_source_open(
		context, native_path, options, out, error);
	if (status == PG_OK && out && *out)
		retain(*out, ::pg_source_close);
	return status;
}

inline pg_status PG_CALL tracked_pg_source_read_all_alloc(
	pg_source *source, const char *name, size_t max_bytes,
	pg_buffer *out, pg_error *error)
{
	auto status = ::pg_source_read_all_alloc(
		source, name, max_bytes, out, error);
	if (status == PG_OK && out && out->data)
		retain_buffer(*out);
	return status;
}

inline pg_status PG_CALL tracked_pg_source_find(pg_source *source,
	const char *name, pg_file **out, pg_error *error)
{
	auto status = ::pg_source_find(source, name, out, error);
	if (status == PG_OK && out && *out)
		retain(*out, ::pg_file_close);
	return status;
}

inline pg_status PG_CALL tracked_pg_source_files(pg_source *source,
	const char *prefix, pg_cursor **out, pg_error *error)
{
	auto status = ::pg_source_files(source, prefix, out, error);
	if (status == PG_OK && out && *out)
		retain(*out, ::pg_cursor_close);
	return status;
}

inline pg_status PG_CALL tracked_pg_source_close(
	pg_source **source, pg_error *error)
{
	auto before = source ? *source : nullptr;
	auto status = ::pg_source_close(source, error);
	if (source && !*source)
		forget(before);
	return status;
}

inline void PG_CALL tracked_pg_buffer_free(pg_buffer *buffer)
{
	void *before = buffer->data;
	::pg_buffer_free(buffer);
	forget(before);
}

inline pg_status PG_CALL tracked_pg_file_read_all_alloc(pg_file *file,
	size_t max_bytes, pg_buffer *out, pg_error *error)
{
	auto status =
		::pg_file_read_all_alloc(file, max_bytes, out, error);
	if (status == PG_OK && out && out->data)
		retain_buffer(*out);
	return status;
}

inline pg_status PG_CALL tracked_pg_cursor_next(
	pg_cursor *cursor, pg_file **out, pg_error *error)
{
	auto status = ::pg_cursor_next(cursor, out, error);
	if (status == PG_OK && out && *out)
		retain(*out, ::pg_file_close);
	return status;
}

inline pg_status PG_CALL tracked_pg_file_close(
	pg_file **file, pg_error *error)
{
	auto before = file ? *file : nullptr;
	auto status = ::pg_file_close(file, error);
	if (file && !*file)
		forget(before);
	return status;
}

inline pg_status PG_CALL tracked_pg_cursor_close(
	pg_cursor **cursor, pg_error *error)
{
	auto before = cursor ? *cursor : nullptr;
	auto status = ::pg_cursor_close(cursor, error);
	if (cursor && !*cursor)
		forget(before);
	return status;
}

inline pg_status PG_CALL tracked_pg_source_entries(pg_source *source,
	const char *prefix, uint32_t flags, pg_entry_cursor **out,
	pg_error *error)
{
	auto status =
		::pg_source_entries(source, prefix, flags, out, error);
	if (status == PG_OK && out && *out)
		retain(*out, ::pg_entry_cursor_close);
	return status;
}

inline pg_status PG_CALL tracked_pg_tree_entries(pg_tree *tree,
	const char *prefix, uint32_t flags, pg_entry_cursor **out,
	pg_error *error)
{
	auto status =
		::pg_tree_entries(tree, prefix, flags, out, error);
	if (status == PG_OK && out && *out)
		retain(*out, ::pg_entry_cursor_close);
	return status;
}

inline pg_status PG_CALL tracked_pg_entry_cursor_next(
	pg_entry_cursor *cursor, pg_entry_info *out, pg_file **file,
	pg_error *error)
{
	auto status = ::pg_entry_cursor_next(cursor, out, file, error);
	if (status == PG_OK && file && *file)
		retain(*file, ::pg_file_close);
	return status;
}

inline pg_status PG_CALL tracked_pg_entry_cursor_close(
	pg_entry_cursor **cursor, pg_error *error)
{
	auto before = cursor ? *cursor : nullptr;
	auto status = ::pg_entry_cursor_close(cursor, error);
	if (cursor && !*cursor)
		forget(before);
	return status;
}

inline pg_status PG_CALL tracked_pg_archive_builder_create(
	pg_context *context, const char *native_path, uint32_t format,
	uint32_t flags, pg_archive_builder **out, pg_error *error)
{
	auto status = ::pg_archive_builder_create(
		context, native_path, format, flags, out, error);
	if (status == PG_OK && out && *out)
		retain(*out, ::pg_archive_builder_close);
	return status;
}

inline pg_status PG_CALL tracked_pg_archive_builder_create_options(
	pg_context *context, const char *native_path,
	const pg_archive_options *options, pg_archive_builder **out,
	pg_error *error)
{
	auto status = ::pg_archive_builder_create_options(
		context, native_path, options, out, error);
	if (status == PG_OK && out && *out)
		retain(*out, ::pg_archive_builder_close);
	return status;
}

inline pg_status PG_CALL tracked_pg_archive_builder_close(
	pg_archive_builder **builder, pg_error *error)
{
	auto before = builder ? *builder : nullptr;
	auto status = ::pg_archive_builder_close(builder, error);
	if (builder && !*builder)
		forget(before);
	return status;
}

inline pg_status PG_CALL tracked_pg_reader_open_source(
	pg_source *source, const char *name, uint32_t representation,
	pg_reader **out, pg_error *error)
{
	auto status = ::pg_reader_open_source(
		source, name, representation, out, error);
	if (status == PG_OK && out && *out)
		retain(*out, ::pg_reader_close);
	return status;
}

inline pg_status PG_CALL tracked_pg_reader_open_tree(pg_tree *tree,
	const char *name, uint32_t representation, pg_reader **out,
	pg_error *error)
{
	auto status = ::pg_reader_open_tree(
		tree, name, representation, out, error);
	if (status == PG_OK && out && *out)
		retain(*out, ::pg_reader_close);
	return status;
}

inline pg_status PG_CALL tracked_pg_reader_open(pg_file *file,
	uint32_t representation, pg_reader **out, pg_error *error)
{
	auto status =
		::pg_reader_open(file, representation, out, error);
	if (status == PG_OK && out && *out)
		retain(*out, ::pg_reader_close);
	return status;
}

inline pg_status PG_CALL tracked_pg_reader_open_native(
	pg_context *context, const char *native_path, pg_reader **out,
	pg_error *error)
{
	auto status = ::pg_reader_open_native(
		context, native_path, out, error);
	if (status == PG_OK && out && *out)
		retain(*out, ::pg_reader_close);
	return status;
}

inline pg_status PG_CALL tracked_pg_reader_close(
	pg_reader **reader, pg_error *error)
{
	auto before = reader ? *reader : nullptr;
	auto status = ::pg_reader_close(reader, error);
	if (reader && !*reader)
		forget(before);
	return status;
}

inline pg_status PG_CALL tracked_pg_writer_open_source(
	pg_source *destination, const char *name,
	const pg_write_options *options, pg_writer **out,
	pg_error *error)
{
	auto status = ::pg_writer_open_source(
		destination, name, options, out, error);
	if (status == PG_OK && out && *out)
		retain(*out, ::pg_writer_close);
	return status;
}

inline pg_status PG_CALL tracked_pg_writer_open_file(pg_file *file,
	const pg_write_options *options, pg_writer **out,
	pg_error *error)
{
	auto status = ::pg_writer_open_file(file, options, out, error);
	if (status == PG_OK && out && *out)
		retain(*out, ::pg_writer_close);
	return status;
}

inline pg_status PG_CALL tracked_pg_writer_open_archive_builder(
	pg_archive_builder *builder, const char *name,
	const pg_write_options *options, pg_writer **out,
	pg_error *error)
{
	auto status = ::pg_writer_open_archive_builder(
		builder, name, options, out, error);
	if (status == PG_OK && out && *out)
		retain(*out, ::pg_writer_close);
	return status;
}

inline pg_status PG_CALL tracked_pg_writer_open_native(
	pg_context *context, const char *native_path,
	const pg_write_options *options, uint32_t flags,
	pg_writer **out, pg_error *error)
{
	auto status = ::pg_writer_open_native(
		context, native_path, options, flags, out, error);
	if (status == PG_OK && out && *out)
		retain(*out, ::pg_writer_close);
	return status;
}

inline pg_status PG_CALL tracked_pg_unpack_target_open(
	pg_cursor *cursor, const char *directory, uint32_t flags,
	pg_unpack_target **out, pg_error *error)
{
	auto status = ::pg_unpack_target_open(
		cursor, directory, flags, out, error);
	if (status == PG_OK && out && *out)
		retain(*out, ::pg_unpack_target_close);
	return status;
}

inline pg_status PG_CALL tracked_pg_writer_open_unpack(
	pg_unpack_target *target, pg_file *file, pg_writer **out,
	pg_error *error)
{
	auto status = ::pg_writer_open_unpack(target, file, out, error);
	if (status == PG_OK && out && *out)
		retain(*out, ::pg_writer_close);
	return status;
}

inline pg_status PG_CALL tracked_pg_unpack_target_close(
	pg_unpack_target **target, pg_error *error)
{
	auto before = target ? *target : nullptr;
	auto status = ::pg_unpack_target_close(target, error);
	if (target && !*target)
		forget(before);
	return status;
}

inline pg_status PG_CALL tracked_pg_writer_close(
	pg_writer **writer, pg_error *error)
{
	auto before = writer ? *writer : nullptr;
	auto status = ::pg_writer_close(writer, error);
	if (writer && !*writer)
		forget(before);
	return status;
}

} // namespace piggle_test

#ifndef PIGGLE_TEST_NO_TRACKING
#define pg_context_open piggle_test::tracked_pg_context_open
#define pg_context_close piggle_test::tracked_pg_context_close
#define pg_tree_create piggle_test::tracked_pg_tree_create
#define pg_tree_open piggle_test::tracked_pg_tree_open
#define pg_tree_source piggle_test::tracked_pg_tree_source
#define pg_tree_read_all_alloc                                         \
	piggle_test::tracked_pg_tree_read_all_alloc
#define pg_tree_find piggle_test::tracked_pg_tree_find
#define pg_tree_files piggle_test::tracked_pg_tree_files
#define pg_tree_close piggle_test::tracked_pg_tree_close
#define pg_source_open piggle_test::tracked_pg_source_open
#define pg_source_read_all_alloc                                       \
	piggle_test::tracked_pg_source_read_all_alloc
#define pg_source_find piggle_test::tracked_pg_source_find
#define pg_source_files piggle_test::tracked_pg_source_files
#define pg_source_close piggle_test::tracked_pg_source_close
#define pg_buffer_free piggle_test::tracked_pg_buffer_free
#define pg_file_read_all_alloc                                         \
	piggle_test::tracked_pg_file_read_all_alloc
#define pg_cursor_next piggle_test::tracked_pg_cursor_next
#define pg_file_close piggle_test::tracked_pg_file_close
#define pg_cursor_close piggle_test::tracked_pg_cursor_close
#define pg_source_entries piggle_test::tracked_pg_source_entries
#define pg_tree_entries piggle_test::tracked_pg_tree_entries
#define pg_entry_cursor_next piggle_test::tracked_pg_entry_cursor_next
#define pg_entry_cursor_close piggle_test::tracked_pg_entry_cursor_close
#define pg_archive_builder_create                                      \
	piggle_test::tracked_pg_archive_builder_create
#define pg_archive_builder_create_options                              \
	piggle_test::tracked_pg_archive_builder_create_options
#define pg_archive_builder_close                                       \
	piggle_test::tracked_pg_archive_builder_close
#define pg_reader_open_source piggle_test::tracked_pg_reader_open_source
#define pg_reader_open_tree piggle_test::tracked_pg_reader_open_tree
#define pg_reader_open piggle_test::tracked_pg_reader_open
#define pg_reader_open_native piggle_test::tracked_pg_reader_open_native
#define pg_reader_close piggle_test::tracked_pg_reader_close
#define pg_writer_open_source piggle_test::tracked_pg_writer_open_source
#define pg_writer_open_file piggle_test::tracked_pg_writer_open_file
#define pg_writer_open_archive_builder                                 \
	piggle_test::tracked_pg_writer_open_archive_builder
#define pg_writer_open_native piggle_test::tracked_pg_writer_open_native
#define pg_unpack_target_open piggle_test::tracked_pg_unpack_target_open
#define pg_writer_open_unpack piggle_test::tracked_pg_writer_open_unpack
#define pg_unpack_target_close                                         \
	piggle_test::tracked_pg_unpack_target_close
#define pg_writer_close piggle_test::tracked_pg_writer_close

#endif

/* Helpers return early while GoogleTest retains the original assertion.
 */
#define PG_CHECK(expr)                                                 \
	do {                                                           \
		bool pg_checked = static_cast<bool>(expr);             \
		EXPECT_TRUE(pg_checked) << #expr;                      \
		if (!pg_checked)                                       \
			return 1;                                      \
	} while (0)

/* Poison diagnostics so success cannot accidentally reuse an old error.
 */
#define PG_STATUS(call, expected)                                      \
	do {                                                           \
		memset(&error, 0xa5, sizeof(error));                   \
		pg_status pg_actual = (call);                          \
		auto pg_checked = piggle_test::status_result(          \
			pg_actual, expected, error);                   \
		EXPECT_TRUE(pg_checked) << #call;                      \
		if (!pg_checked)                                       \
			return 1;                                      \
	} while (0)

#endif
