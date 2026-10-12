#ifndef PIGGLE_ACCEPTANCE_WORLD_HPP
#define PIGGLE_ACCEPTANCE_WORLD_HPP

#include "../support.hpp"
#ifdef _WIN32
#include <windows.h>
#endif
#include <cucumber-cpp/autodetect.hpp>
#include <fstream>
#include <map>
#include <string>
#include <set>
#include <thread>
#include <zlib-ng.h>

using WorldFiles = std::vector<std::pair<std::string, std::string>>;

#ifdef __linux__
extern "C" void pg_test_journal_fault(int mode);
#endif

inline void expect_status(
	pg_status actual, pg_status expected, const pg_error &error)
{
	EXPECT_TRUE(
		piggle_test::status_result(actual, expected, error));
}

inline void write_native(
	const std::string &path, const std::string &bytes)
{
	auto parent = std::filesystem::path(path).parent_path();

	if (!parent.empty())
		std::filesystem::create_directories(parent);
	std::ofstream stream(path, std::ios::binary | std::ios::trunc);

	EXPECT_TRUE(stream.is_open()) << path;
	stream.write(bytes.data(), bytes.size());
	stream.close();
	EXPECT_TRUE(stream.good()) << path;
}

inline std::string read_native(const std::string &path)
{
	std::ifstream stream(path, std::ios::binary);

	EXPECT_TRUE(stream.is_open()) << path;
	return {std::istreambuf_iterator<char>(stream),
		std::istreambuf_iterator<char>()};
}

inline void replace_native(
	const std::string &path, const std::string &bytes)
{
	write_native(path + ".replacement", bytes);
#ifdef _WIN32
	EXPECT_TRUE(MoveFileExA((path + ".replacement").c_str(),
		path.c_str(), MOVEFILE_REPLACE_EXISTING));
#else
	EXPECT_EQ(0,
		rename((path + ".replacement").c_str(), path.c_str()));
#endif
}

inline uint32_t get_le32(const std::string &bytes, size_t offset)
{
	uint32_t value = 0;

	for (unsigned i = 0; i < 4; ++i)
		value |= static_cast<uint32_t>(
					static_cast<unsigned char>(
						bytes.at(offset + i)))
			<< (8 * i);
	return value;
}

inline void set_le32(std::string &bytes, size_t offset, uint64_t value)
{
	ASSERT_LE(value, UINT32_MAX);
	for (unsigned i = 0; i < 4; ++i)
		bytes.at(offset + i) =
			static_cast<char>(value >> (8 * i));
}

/* Independent PIGG fixtures admit colliding and hierarchical file
 * records. */
inline void raw_pigg(const std::string &path, const WorldFiles &files,
	bool bad_loser = false)
{
	std::string bytes(16 + 48 * files.size(), '\0');

	set_le32(bytes, 0, 0x123);
	set_le32(bytes, 4, 0x00020002);
	set_le32(bytes, 8, 0x00300010);
	set_le32(bytes, 12, files.size());
	size_t pool = bytes.size();

	bytes.resize(pool + 12);
	set_le32(bytes, pool, 0x6789);
	set_le32(bytes, pool + 4, files.size());
	for (const auto &file : files) {
		size_t element = bytes.size();

		bytes.resize(element + 4);
		set_le32(bytes, element, file.first.size() + 1);
		bytes.append(file.first);
		bytes.push_back('\0');
	}
	set_le32(bytes, pool + 8, bytes.size() - pool - 12);
	size_t headers = bytes.size();

	bytes.resize(headers + 12);
	set_le32(bytes, headers, 0x9abc);
	for (size_t i = 0; i < files.size(); ++i) {
		size_t record = 16 + 48 * i;

		set_le32(bytes, record, 0x3456);
		set_le32(bytes, record + 4, i);
		set_le32(bytes, record + 8, files[i].second.size());
		set_le32(bytes, record + 12, 10);
		set_le32(bytes, record + 16, bytes.size());
		set_le32(bytes, record + 20, 0xfeed1234);
		set_le32(bytes, record + 24, UINT32_MAX);
		if (bad_loser && !i)
			bytes.at(record + 28) = 1;
		bytes.append(files[i].second);
	}
	write_native(path, bytes);
}

/* Independent UPDATE frame with unchanged payload identity and a new
 * mtime. */
inline void commit_timestamp(
	const std::string &path, uint64_t slot, uint32_t mtime)
{
	ASSERT_LE(slot, UINT32_MAX);
	std::string archive = read_native(path);
	uint32_t op_size = get_le32(archive, 4) >> 16;
	uint32_t dl_size = get_le32(archive, 20) & 0xffff;
	size_t record = 24 + op_size + dl_size + 32 * static_cast<size_t>(slot);
	std::string frame(56, '\0');

	set_le32(frame, 0, 3);
	set_le32(frame, 8, slot);
	set_le32(frame, 12, get_le32(archive, record + 8));
	set_le32(frame, 16, mtime);
	frame.replace(24, 8, archive.substr(record + 24, 8));
	frame.replace(32, 4, archive.substr(record + 16, 4));
	frame.replace(40, 8, archive.substr(record, 8));
	set_le32(archive, 24, frame.size());
	archive.replace(28, frame.size(), frame);
	set_le32(archive, 28 + frame.size(), 0xdeabac05);
	write_native(path, archive);
}

#ifdef _WIN32
inline void set_windows_time(const std::string &path, uint64_t ticks)
{
	HANDLE file = CreateFileA(path.c_str(), FILE_WRITE_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, 0, nullptr);
	FILETIME time = {static_cast<DWORD>(ticks),
		static_cast<DWORD>(ticks >> 32)};

	ASSERT_NE(file, INVALID_HANDLE_VALUE);
	EXPECT_TRUE(SetFileTime(file, nullptr, nullptr, &time));
	EXPECT_TRUE(CloseHandle(file));
}
#endif

struct WorldEvent {
	uint32_t kind;
	std::string name;
	std::string scope;
	bool before;
	bool after;
	uint64_t before_size;
	uint64_t after_size;
	bool directory;
};

struct WorldEntry {
	std::string name;
	uint32_t kind;
	uint32_t attributes;
	bool file;
};

struct World {
	piggle_test::Workspace workspace;
	pg_context *context = nullptr;
	pg_tree *tree = nullptr;
	std::map<std::string, pg_source *> sources;
	std::map<std::string, std::string> paths;
	std::map<std::string, uint32_t> formats;
	std::string active;
	pg_error error = {};
	pg_status status = PG_OK;
	pg_file *file = nullptr;
	pg_file *held = nullptr;
	pg_reader *reader = nullptr;
	pg_reader *other_reader = nullptr;
	pg_writer *writer = nullptr;
	pg_archive_builder *builder = nullptr;
	pg_cursor *cursor = nullptr;
	pg_cursor *old_cursor = nullptr;
	pg_entry_cursor *entry_cursor = nullptr;
	pg_entry_cursor *old_entries = nullptr;
	pg_unpack_target *target = nullptr;
	pg_buffer buffer = {};
	pg_file_info captured = {};
	std::vector<std::string> names;
	std::vector<std::string> old_names;
	std::vector<WorldEntry> entries;
	std::vector<WorldEvent> events;
	std::string bytes;
	std::string previous;
	std::string normalized;
	std::string expected;
	std::string native_name;
	std::string edit;
	std::vector<pg_reader *> readers;
	std::vector<std::string> payloads;
	pg_error saved_error = {};
	pg_status saved_status = PG_OK;
	uint64_t offset = 0;
	uint64_t sequence = 0;
	std::vector<uint64_t> sequences;
	size_t count = 0;
	size_t required = 0;
	uint32_t mode = PG_WATCH_NATIVE;
	bool callback_lookup = false;
	bool use_tree = true;
	bool callback_worker = false;
	bool callback_replace_root = false;
	bool callback_done = false;
	bool worker_success = false;
	pg_status recursive_status = PG_OK;
	pg_status mutation_status = PG_OK;
	pg_status callback_status = PG_OK;
	std::string callback_bytes;

	void ready(const WorldFiles &files = {{"a", "old"},
				{"b", "old"}, {"menu/a", "old"}},
		bool discover = false)
	{
		loose("loose", files);
		attach("loose");
		if (discover) {
			request();
			expect_status(status, PG_OK, error);
		}
	}

	void manage(uint32_t depth = PG_DISCOVER_RECURSIVE)
	{
		expect_status(
			pg_tree_manage(tree, nullptr, depth, &error),
			PG_OK, error);
	}

	void select(
		const std::string &name = "a", bool from_tree = true)
	{
		find(name, from_tree);
		expect_status(status, PG_OK, error);
	}

	void retain_selection()
	{
		held = file;
		file = nullptr;
		expect_status(pg_file_inspect(held, &captured, &error),
			PG_OK, error);
	}

	void staged(const std::string &name, uint64_t size)
	{
		pg_write_options options;

		pg_write_options_init(&options, size);
		expect_status(
			pg_writer_open_source(source(), name.c_str(),
				&options, &writer, &error),
			PG_OK, error);
	}

	void write(const std::string &value)
	{
		expect_status(pg_writer_write(writer, value.data(),
					value.size(), &count, &error),
			PG_OK, error);
		EXPECT_EQ(count, value.size());
	}

	void finished()
	{
		expect_status(
			pg_writer_finish(writer, &error), PG_OK, error);
		expect_status(
			pg_writer_close(&writer, &error), PG_OK, error);
	}

	std::string chunk(pg_reader *input, size_t capacity)
	{
		std::string result(capacity, '\0');

		status = pg_reader_read(
			input, result.data(), capacity, &count, &error);
		EXPECT_LE(count, capacity);
		result.resize(count);
		return result;
	}

	World()
	{
		expect_status(pg_context_open(&context, &error), PG_OK,
			error);
	}

	pg_source *source() const
	{
		return sources.at(active);
	}

	void ensure_tree()
	{
		if (!tree)
			expect_status(
				pg_tree_create(context, &tree, &error),
				PG_OK, error);
	}

	void open(const std::string &label, uint32_t access = PG_WRITE)
	{
		pg_source_options options = {
			formats.at(label), access, 0};
		pg_source *opened = nullptr;

		status =
			pg_source_open(context, paths.at(label).c_str(),
				&options, &opened, &error);
		if (status == PG_OK) {
			sources[label] = opened;
			active = label;
		}
	}

	void loose(const std::string &label, const WorldFiles &files)
	{
		paths[label] = "storage-" + label;
		formats[label] = PG_LOOSE;
		std::filesystem::create_directories(paths[label]);
		for (const auto &entry : files)
			write_native(paths[label] + "/" + entry.first,
				entry.second);
		open(label);
		expect_status(status, PG_OK, error);
	}

	void archive(const std::string &label, const WorldFiles &files,
		uint32_t format = PG_PIGG2, bool raw = false)
	{
		paths[label] = label + ".archive";
		formats[label] = format;
		if (raw) {
			raw_pigg(paths[label], files);
		} else {
			pg_archive_builder *output = nullptr;

			expect_status(
				pg_archive_builder_create(context,
					paths[label].c_str(), format, 0,
					&output, &error),
				PG_OK, error);
			for (const auto &entry : files)
				expect_status(
					pg_archive_builder_write_all(
						output,
						entry.first.c_str(),
						entry.second.data(),
						entry.second.size(),
						nullptr, &error),
					PG_OK, error);
			expect_status(pg_archive_builder_finish(
						output, &error),
				PG_OK, error);
			expect_status(pg_archive_builder_close(
						&output, &error),
				PG_OK, error);
		}
		open(label);
		expect_status(status, PG_OK, error);
	}

	void attach(const std::string &label)
	{
		ensure_tree();
		expect_status(
			pg_tree_attach(tree, sources.at(label), &error),
			PG_OK, error);
	}

	void request(
		bool from_tree = true, const char *prefix = nullptr)
	{
		status = from_tree
			? pg_tree_request_subtree(tree, prefix, &error)
			: pg_source_request_subtree(
					source(), prefix, &error);
	}

	void find(const std::string &name, bool from_tree = true)
	{
		expect_status(
			pg_file_close(&file, &error), PG_OK, error);
		status = from_tree
			? pg_tree_find(
					tree, name.c_str(), &file, &error)
			: pg_source_find(source(), name.c_str(), &file,
					&error);
	}

	std::string read(pg_file *selection)
	{
		pg_buffer result = {};
		pg_error diagnostic;

		expect_status(
			pg_file_read_all_alloc(selection, 1024 * 1024,
				&result, &diagnostic),
			PG_OK, diagnostic);
		std::string value;

		if (result.size)
			value.assign(
				static_cast<const char *>(result.data),
				result.size);

		pg_buffer_free(&result);
		return value;
	}

	void capture_files(
		bool from_tree = true, const char *prefix = nullptr)
	{
		expect_status(
			pg_cursor_close(&cursor, &error), PG_OK, error);
		status = from_tree
			? pg_tree_files(tree, prefix, &cursor, &error)
			: pg_source_files(
					source(), prefix, &cursor, &error);
	}

	std::vector<std::string> consume(pg_cursor *snapshot)
	{
		std::vector<std::string> result;
		pg_file *selection = nullptr;
		pg_error diagnostic;

		while (true) {
			pg_status next = pg_cursor_next(
				snapshot, &selection, &diagnostic);

			if (next == PG_END) {
				expect_status(next, PG_END, diagnostic);
				EXPECT_EQ(selection, nullptr);
				break;
			}
			expect_status(next, PG_OK, diagnostic);
			pg_file_info info;

			expect_status(pg_file_inspect(selection, &info,
						&diagnostic),
				PG_OK, diagnostic);
			result.emplace_back(info.canonical_name);
			expect_status(
				pg_file_close(&selection, &diagnostic),
				PG_OK, diagnostic);
		}
		EXPECT_TRUE(
			std::is_sorted(result.begin(), result.end()));
		return result;
	}

	void list(bool from_tree = true, const char *prefix = nullptr)
	{
		capture_files(from_tree, prefix);
		names.clear();
		if (status == PG_OK)
			names = consume(cursor);
	}

	void capture_entries(bool from_tree = true, uint32_t flags = 0)
	{
		expect_status(
			pg_entry_cursor_close(&entry_cursor, &error),
			PG_OK, error);
		status = from_tree
			? pg_tree_entries(tree, nullptr, flags,
					&entry_cursor, &error)
			: pg_source_entries(source(), nullptr, flags,
					&entry_cursor, &error);
	}

	std::vector<WorldEntry> consume_entries(
		pg_entry_cursor *snapshot)
	{
		std::vector<WorldEntry> result;
		pg_error diagnostic;

		while (true) {
			pg_entry_info info;
			pg_file *selection = nullptr;
			pg_status next = pg_entry_cursor_next(snapshot,
				&info, &selection, &diagnostic);

			if (next == PG_END) {
				expect_status(next, PG_END, diagnostic);
				EXPECT_EQ(selection, nullptr);
				break;
			}
			expect_status(next, PG_OK, diagnostic);
			result.push_back({info.canonical_name,
				info.kind, info.attributes,
				selection != nullptr});
			expect_status(
				pg_file_close(&selection, &diagnostic),
				PG_OK, diagnostic);
		}
		return result;
	}

	void watch(uint32_t selected = PG_WATCH_NATIVE)
	{
		mode = selected;
		status = pg_tree_watch(tree, mode, &error);
		expect_status(status, PG_OK, error);
	}

	static void PG_CALL observe(
		void *user, const pg_visible_change *change);

	void poll()
	{
#ifdef _WIN32
		std::this_thread::sleep_for(
			std::chrono::milliseconds(30));
#endif
		pg_observer observer = {this, observe};

		status = pg_tree_poll(tree, &observer, &error);
	}

	size_t reports(
		uint32_t kind, const std::string &name = "") const
	{
		return std::count_if(events.begin(), events.end(),
			[&](const auto &event) {
				return event.kind == kind &&
					(name.empty() ||
						event.name == name);
			});
	}
};

using WorldScope = cucumber::ScenarioScope<World>;

#endif
