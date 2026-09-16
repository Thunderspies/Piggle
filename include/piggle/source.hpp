/* Exact imports; no wrapper implementation or additional ABI. */
#ifndef PIGGLE_SOURCE_HPP
#define PIGGLE_SOURCE_HPP
#include <piggle/source.h>
#include <piggle/file.hpp>

namespace piggle
{
using ::pg_source_open;
using ::pg_source_inspect;
using ::pg_source_read_all;
using ::pg_source_read_all_alloc;
using ::pg_source_write_all;
using ::pg_source_import;
using ::pg_source_copy;
using ::pg_source_export;
using ::pg_source_pack;
using ::pg_source_unpack;
using ::pg_source_find;
using ::pg_source_files;
using ::pg_source_request_subtree;
using ::pg_source_rescan;
using ::pg_source_validate;
using ::pg_source_recover;
using ::pg_source_delete;
using ::pg_source_close;
} /* namespace piggle */
#endif
