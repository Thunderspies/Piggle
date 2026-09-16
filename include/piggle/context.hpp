/* Exact imports; no wrapper implementation or additional ABI. */
#ifndef PIGGLE_CONTEXT_HPP
#define PIGGLE_CONTEXT_HPP
#include <piggle/context.h>
#include <piggle/types.hpp>

namespace piggle
{
using ::pg_context_open;
using ::pg_context_close;
using ::pg_name_normalize;
} /* namespace piggle */
#endif
