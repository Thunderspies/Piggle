# Piggle API design rationale

This document collects the research, influences, and reasoning behind the
[Piggle API contract](api.md). It explains why the public concepts and their
boundaries exist. The contract and public headers remain authoritative for
behavior.

Piggle combines loose directories and two archive formats into an ordered
virtual file tree. Its API must keep physical storage, resolved visibility, retained
file identity, byte transfer, mutation publication, and caller-controlled work
distinct without making ordinary named-file tasks cumbersome.

## Design influences

### PhysicsFS: virtual names over ordered storage

PhysicsFS mounts archives and directories into a search path and opens content
by virtual name. It also selects a write directory separately from lookup. This
establishes a useful distinction between logical asset names and their physical
packaging. Piggle retains that distinction while using explicit context, tree,
source, and destination handles instead of global search and write paths.

PhysicsFS enumeration also demonstrates that delivery, sorting, deduplication,
and error reporting are separate promises. Piggle therefore distinguishes
visible files under a prefix from one exact-name lookup and defines
ordering for each operation.

[PhysicsFS API and enumeration](https://www.icculus.org/physfs/docs/html/physfs_8h.html)

### libarchive: conveniences over one I/O foundation

libarchive's reader accepts sequential entry headers and data through a shared
callback-based foundation. Filename, descriptor, and memory openers adapt that
foundation rather than defining separate archive semantics. Piggle follows the
same layering principle: blocking tasks and named helpers compose the same
reader, writer, selection, ownership, and error rules as caller-sized streams.

libarchive is stream-oriented and does not model Piggle's ordered lookup or
retained visible-copy identity. Those remain separate Piggle concepts.
Its archive-writing interface also exposes construction as a distinct function
family with sequential entry headers and data. Piggle uses a distinct builder
family too, while adding one transaction-wide publication boundary.

[libarchive overview](https://raw.githubusercontent.com/libarchive/libarchive/master/libarchive/libarchive.3),
[reader interface](https://raw.githubusercontent.com/libarchive/libarchive/master/libarchive/archive_read.3),
[filename adapter](https://raw.githubusercontent.com/libarchive/libarchive/master/libarchive/archive_read_open_filename.c),
[writer interface](https://raw.githubusercontent.com/libarchive/libarchive/master/libarchive/archive_write.3)

### libzip: selected entries and staged mutation

libzip distinguishes adding a file by name from replacing a selected entry and
makes input ownership explicit. Piggle similarly supports common name-based
tasks while preserving a `pg_file` selection for one visible physical copy. A
selection includes generation information so external changes cannot silently
redirect it.

libzip's source-writing interface prepares private output before committing it.
That supports Piggle's staged writer model. Piggle keeps `finish` separate from
`close`, however, so resource cleanup never decides whether to publish content.
This also differs from libzip's archive-close contract, where a successful close
writes changes and releases the archive while failure leaves it to the caller.

[Adding and replacing entries](https://libzip.org/documentation/zip_file_add/),
[buffer ownership](https://libzip.org/documentation/zip_source_buffer/),
[named and indexed reads](https://libzip.org/documentation/zip_fopen/),
[begin staged source writing](https://libzip.org/documentation/zip_source_begin_write/),
[commit staged source writing](https://libzip.org/documentation/zip_source_commit_write/),
[archive close](https://libzip.org/documentation/zip_close/)

### Caller-sized streaming

Piggle exposes seekable readers and staged writers. The caller decides each
transfer size and can yield between calls. Blocking helpers compose these same
primitives. Opening, indexing, finishing and publishing may still block within
a call. The unpack target pins a captured cursor and native root, preflights the
batch, and rechecks each path at writer open and finish.

### GLib, GIO, and libuv: allocated results and explicit observation

GLib's whole-file loader returns allocated bytes with an explicit release
operation. Piggle uses the same ownership shape for bounded unknown-size reads:
`pg_buffer` keeps pointer and length together, and `pg_buffer_free` resets both.
Piggle additionally requires a decoded-size limit and returns binary data
without a terminator.

GIO requests file monitoring explicitly. libuv separates native file events
from stat-based polling. Piggle therefore leaves new trees unwatched and lets
the caller choose native or scan observation. Managed scopes, discovered
subtrees and open tree readers define observation coverage. Native management can remain lazy; callers poll for visible changes
or invalidations when prior state is unknown.

[GLib allocated reads](https://docs.gtk.org/glib/func.file_get_contents.html),
[GIO monitoring](https://docs.gtk.org/gio/method.File.monitor.html),
[libuv scanning](https://docs.libuv.org/en/v1.x/fs_poll.html)

## Core decisions and tradeoffs

| Decision | Reason and tradeoff |
| --- | --- |
| Keep source, tree, and file selection distinct | Storage, ordered resolution, and one captured copy have different lifetimes. A selection never changes winners silently. |
| Attach sources in order | Callers choose roots explicitly; later attachments win exact-name conflicts without numeric ranks or timestamp policies. |
| Let directories win | A directory and its descendants remain reachable even when another source has a same-name file. |
| Index archives on open, discover loose paths on request | Archive formats have no pathname index; loose exact lookup can probe native paths, while subtree requests pay for a recursive scan. |
| Require coverage at the listing depth | Shallow discovery visits immediate entries; recursive discovery covers descendants. Snapshots use indexed metadata. INVALID rejects an incomplete view. |
| Retain requested subtrees | Repeated listings and exact reads within a scope reuse known names. Native watching can maintain requested tree scopes; without it, callers explicitly refresh. |
| Expose only visible copies | Common lookup and enumeration have one outcome per name. Existing archive duplicates may be validated but are not public selections. |
| Separate management from discovery | A tree starts OFF. Managed native scopes need no file baseline; requested scopes and tree readers add known observations. SCAN requires discovery. |
| Let callers size transfers | Readers and writers return exact counts; callers can yield between calls, while finish and indexing may block. |
| Keep caller-sized chunks synchronous | Applications control transfer scheduling by choosing chunk sizes. |
| Require an archive builder | Constructing a new archive needs one private staging area and publication boundary; duplicate canonical names fail. |

## Layer composition

Higher-level calls compose lower-level selection, transfer, ownership and
publication behavior. They do not redefine normalization, integrity or error
rules.

| Higher-level task | Composition |
| --- | --- |
| Ordered tree open | Open each source synchronously and attach in list order; publish one output on success. |
| Exact lookup | Use the requested index inside its scope; probe unrequested loose paths, then select the visible winner. |
| Subtree request | Scan and retain a complete loose prefix; repeat to refresh it. |
| Listing | Capture indexed visible names within a requested prefix without a disk scan. |
| Named reader or export | Select once, open or export the retained file, then release the temporary selection. |
| Read all | Inspect captured length, enforce capacity, read through verified EOF, then clean up. |
| Pack and unpack | Enumerate visible files and transfer them through builder or native writers. |
| Watch and poll | Retain native hint paths, refresh affected known names, and invalidate unknown managed names in the visible feed. |

The initial subtree scan should normalize each discovered path once and avoid
an exact path probe for every file. Source selection and tree overlay merging
should use name indexes rather than repeated linear searches or public
`find` calls per result. For N discovered files, initial indexing should use
at most O(N log N) comparison work; a quiet listing should perform no native
directory traversal and at most linear work in its output size. Notifications
may require work proportional to affected scopes. These are implementation
targets, not elapsed-time guarantees. Pack and unpack deliberately refresh
loose roots because they must capture complete current loose views.

Archive payloads remain in descriptor-backed ranges or private staging files.
Active transfer buffers are bounded, while metadata and temporary disk use may
grow with the archive. Synchronous discovery and observation may block in
proportion to the requested scope.

## Deliberate boundaries

Piggle uses explicit C ownership, known-length sequential writes, bounded
allocated reads, binary results without implicit terminators, explicit mutation
commitment, and separate builder publication. It does not provide writer seek,
append, partial in-place file editing, historical event reads, automatic mutation
retries, custom allocators, custom format plugins, or an owning callback/event
framework.

These boundaries keep the library focused on named whole files, exact physical
selections, predictable publication, and caller-controlled scheduling. Adding a
new capability requires defining its identity, lifetime, failure, scheduling,
and cleanup contract at the lowest layer before adding convenience adapters.
