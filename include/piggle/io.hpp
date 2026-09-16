/* Exact imports; no wrapper implementation or additional ABI. */
#ifndef PIGGLE_IO_HPP
#define PIGGLE_IO_HPP
#include <piggle/io.h>
#include <piggle/file.hpp>
#include <piggle/source.hpp>
#include <piggle/tree.hpp>

namespace piggle
{
using ::pg_reader_open_source;
using ::pg_reader_open_tree;
using ::pg_reader_open;
using ::pg_reader_open_native;
using ::pg_reader_inspect;
using ::pg_reader_seek;
using ::pg_reader_tell;
using ::pg_reader_read;
using ::pg_reader_close;
using ::pg_writer_open_source;
using ::pg_writer_open_file;
using ::pg_writer_open_archive_builder;
using ::pg_writer_open_native;
using ::pg_unpack_target_open;
using ::pg_writer_open_unpack;
using ::pg_unpack_target_close;
using ::pg_writer_write;
using ::pg_writer_finish;
using ::pg_writer_close;
} /* namespace piggle */
#endif
