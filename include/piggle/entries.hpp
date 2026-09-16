/* Exact imports; no wrapper implementation or additional ABI. */
#ifndef PIGGLE_ENTRIES_HPP
#define PIGGLE_ENTRIES_HPP
#include <piggle/entries.h>
#include <piggle/tree.hpp>
namespace piggle
{
using ::PG_DISCOVER_CHILDREN;
using ::PG_DISCOVER_RECURSIVE;
using ::pg_source_discover;
using ::pg_tree_discover;
using ::pg_entry_cursor;
using entry_cursor = ::pg_entry_cursor;
using ::pg_entry_info;
using entry_info = ::pg_entry_info;
using ::PG_ENTRY_FILE;
using ::PG_ENTRY_DIRECTORY;
using ::PG_ENTRIES_RECURSIVE;
using ::PG_ENTRY_READ_ONLY;
using ::PG_ENTRY_HIDDEN;
using ::PG_ENTRY_SYSTEM;
using ::PG_ENTRY_IMPLIED;
using ::pg_source_entries;
using ::pg_tree_entries;
using ::pg_entry_cursor_next;
using ::pg_entry_cursor_close;
}
#endif
