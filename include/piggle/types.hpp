/* Exact imports; no wrapper implementation or additional ABI. */
#ifndef PIGGLE_TYPES_HPP
#define PIGGLE_TYPES_HPP
#include <piggle/types.h>

namespace piggle
{
using ::PG_CHECKSUM_LOGICAL;
using ::PG_CHECKSUM_STORED;
using ::pg_archive_options;
using archive_options = ::pg_archive_options;
using ::pg_archive_builder;
using archive_builder = ::pg_archive_builder;
using ::pg_buffer;
using buffer = ::pg_buffer;
using ::pg_context;
using context = ::pg_context;
using ::pg_cursor;
using cursor = ::pg_cursor;
using ::pg_entry_options;
using entry_options = ::pg_entry_options;
using ::pg_error;
using error = ::pg_error;
using ::pg_file;
using file = ::pg_file;
using ::pg_file_info;
using file_info = ::pg_file_info;
using ::pg_id;
using id = ::pg_id;
using ::pg_pack_options;
using pack_options = ::pg_pack_options;
using ::pg_reader;
using reader = ::pg_reader;
using ::pg_reader_info;
using reader_info = ::pg_reader_info;
using ::pg_source;
using source = ::pg_source;
using ::pg_source_info;
using source_info = ::pg_source_info;
using ::pg_source_options;
using source_options = ::pg_source_options;
using ::pg_status;
using status = ::pg_status;
using ::pg_unpack_target;
using unpack_target = ::pg_unpack_target;
using ::pg_tree;
using tree = ::pg_tree;
using ::pg_write_options;
using write_options = ::pg_write_options;
using ::pg_writer;
using writer = ::pg_writer;
using ::PG_AUTO;
using ::PG_BUSY;
using ::PG_CAPACITY;
using ::PG_CHECKSUM;
using ::PG_COMMITTED;
using ::PG_COMPRESS_AUTO;
using ::PG_COMPRESS_FORCE;
using ::PG_COMPRESS_NEVER;
using ::PG_CONFLICT;
using ::PG_CORRUPT;
using ::PG_DIGEST_MD5;
using ::PG_DIGEST_MD5_32;
using ::PG_DIGEST_NONE;
using ::PG_END;
using ::PG_EXISTS;
using ::PG_HOGG10;
using ::PG_INDETERMINATE;
using ::PG_PARTIAL;
using ::PG_INVALID;
using ::PG_IO;
using ::PG_LIMIT;
using ::PG_LOGICAL;
using ::PG_LOOSE;
using ::PG_NOMEM;
using ::PG_NOT_ATTACHED;
using ::PG_NOT_FOUND;
using ::PG_NO_CHECKSUM;
using ::PG_OK;
using ::PG_OVERWRITE;
using ::PG_PIGG2;
using ::PG_READ;
using ::PG_READ_LOGICAL;
using ::PG_READ_ONLY;
using ::PG_READ_STORED;
using ::PG_RECOVERY_REQUIRED;
using ::PG_REENTRANT;
using ::PG_RETRY;
using ::PG_STALE;
using ::PG_UNSUPPORTED;
using ::PG_WRITE;
using ::PG_ZLIB;
using ::pg_write_options_init;
} /* namespace piggle */
#endif
