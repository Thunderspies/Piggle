/* Exact imports; no wrapper implementation or additional ABI. */
#ifndef PIGGLE_CHANGE_HPP
#define PIGGLE_CHANGE_HPP
#include <piggle/change.h>
#include <piggle/entries.hpp>

namespace piggle
{
using ::PG_CHANGE_ADD;
using ::PG_CHANGE_UPDATE;
using ::PG_CHANGE_REMOVE;
using ::PG_CHANGE_LOSS;
using ::PG_CHANGE_INVALIDATE;
using ::pg_tree_manage;
using ::pg_tree_unmanage;
using ::pg_visible_change;
using visible_change = ::pg_visible_change;
using ::pg_observer;
using observer = ::pg_observer;
using ::pg_visible_fn;
using visible_fn = ::pg_visible_fn;
using ::pg_tree_watch;
using ::pg_tree_unwatch;
using ::pg_tree_poll;
} /* namespace piggle */
#endif
