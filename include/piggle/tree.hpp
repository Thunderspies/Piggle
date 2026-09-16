/* Exact imports; no wrapper implementation or additional ABI. */
#ifndef PIGGLE_TREE_HPP
#define PIGGLE_TREE_HPP
#include <piggle/tree.h>
#include <piggle/source.hpp>

namespace piggle
{
using ::pg_source_spec;
using source_spec = ::pg_source_spec;
using ::pg_tree_info;
using tree_info = ::pg_tree_info;
using ::PG_WATCH_OFF;
using ::PG_WATCH_NATIVE;
using ::PG_WATCH_SCAN;
using ::pg_tree_create;
using ::pg_tree_inspect;
using ::pg_tree_open;
using ::pg_tree_attach;
using ::pg_tree_detach;
using ::pg_tree_source;
using ::pg_tree_read_all;
using ::pg_tree_read_all_alloc;
using ::pg_tree_export;
using ::pg_tree_pack;
using ::pg_tree_unpack;
using ::pg_tree_find;
using ::pg_tree_files;
using ::pg_tree_request_subtree;
using ::pg_tree_rescan;
using ::pg_tree_close;
} /* namespace piggle */
#endif
