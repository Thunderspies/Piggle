/* Exact imports; no wrapper implementation or additional ABI. */
#ifndef PIGGLE_FILE_HPP
#define PIGGLE_FILE_HPP
#include <piggle/file.h>
#include <piggle/context.hpp>

namespace piggle
{
using ::pg_metadata_options;
using metadata_options = ::pg_metadata_options;
using ::PG_METADATA_MTIME;
using ::PG_METADATA_HEADER;
using ::pg_file_update_metadata;
using ::pg_buffer_free;
using ::pg_file_read_all;
using ::pg_file_read_all_alloc;
using ::pg_file_write_all;
using ::pg_file_export;
using ::pg_file_inspect;
using ::pg_cursor_next;
using ::pg_file_verify;
using ::pg_file_delete;
using ::pg_file_close;
using ::pg_cursor_close;
} /* namespace piggle */
#endif
