/* Exact imports; no wrapper implementation or additional ABI. */
#ifndef PIGGLE_ARCHIVE_BUILDER_HPP
#define PIGGLE_ARCHIVE_BUILDER_HPP
#include <piggle/archive_builder.h>
#include <piggle/io.hpp>

namespace piggle
{
using ::pg_archive_builder_create;
using ::pg_archive_builder_create_options;
using ::pg_archive_builder_write_all;
using ::pg_archive_builder_import;
using ::pg_archive_builder_copy;
using ::pg_archive_builder_finish;
using ::pg_archive_builder_close;
} /* namespace piggle */
#endif
