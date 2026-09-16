# Piggle API contract

This document defines the proposed public API contract. Implementation work
follows this contract draft. See the project [README](../README.md) for the
quickstart and the [design rationale](design.md) for background.

## Purpose and boundaries

Piggle manages named whole files in loose directories, PIGG v2 and HOGG v10
archives. A source represents storage; a tree resolves ordered sources; a
file selects one physical copy. Readers have independent seekable positions.
Writers stage complete replacements. An archive builder stages several entries
and publishes one new archive. Callers schedule transfers by choosing each reader and writer chunk size.

The C11 interface is implemented in procedural C++11. Headers use opaque
handles, plain structs, free functions and status values. C++ headers import
the same `pg_*` functions into `piggle` and offer shorter type aliases; they
add no classes, overloads, templates, ownership conversions or ABI. Include
`piggle/piggle.h` or `piggle/piggle.hpp` for the complete surface.

No writer seek, append, partial payload editing, revision history, asset
interpretation, game-directory discovery or cross-source transaction is
promised. Reading exact stored bytes is supported, with validation. Archive
construction is a transaction on one native destination; editing an existing
source commits one selected file at a time. Arbitrary custom I/O, allocators
and format plugins are outside this API.

## Find the right level

| Task | Start here | Foundation |
| --- | --- | --- |
| Open an archive or loose root | `pg_source_open` | Archive names index now; loose names are discovered on request |
| Combine sources | `pg_tree_open` | `pg_tree_create`, attach, detach; later attachment wins |
| Find one file | `pg_source_find` / `pg_tree_find` | Indexed inside a requested subtree; exact loose probe outside it |
| Cache a subtree | `pg_source_request_subtree` / `pg_tree_request_subtree` | Recursive loose scan; repeat to refresh |
| List visible files | `pg_source_files` / `pg_tree_files` | Captured cursor from a requested index |
| List files and directories | `pg_source_entries` / `pg_tree_entries` | Immediate children or recursive entry snapshots |
| Read a named file | Source/tree `read_all` or `read_all_alloc` | Select once, verify, close |
| Stream a named file | `pg_reader_open_source` / `pg_reader_open_tree` | Independent reader position |
| Reposition a reader | `pg_reader_seek` / `pg_reader_tell` | Absolute offsets in the selected representation |
| Refresh known paths | Source/tree `rescan` | Indexed archives and requested loose scopes only |
| Observe changes | `pg_tree_manage`, `pg_tree_watch`, `pg_tree_poll` | Managed scopes, discovery and open tree readers define coverage |
| Write or export content | Source/file writer or export helpers | Caller-sized chunks or blocking helpers |
| Pack or unpack | Source/tree helpers or reader/writer composition | One archive publication or per-file unpack publication |
| Release a reference | `pg_*_close(&handle, error)` | Synchronous cleanup |

All calls are synchronous. Archive opening, indexing, listing, writer finish,
publication, recovery, and whole-file helpers may block for arbitrarily long.
Callers seeking control between transfer calls open a reader and writer, choose
each `pg_reader_read` capacity and `pg_writer_write` size, and finish explicitly.
There is no operation handle, step protocol, or cancellation request. Closing an
unfinished writer or builder aborts its unpublished work.

## Common contract

These rules apply to every declaration, together with its header comment.

- Every pointer is required unless specifically nullable. `pg_error *` is
  always optional. NULL input handles are invalid except an owned pointer
  containing NULL passed to a typed close. The pointer address itself is
  required. Arbitrary invalid pointers, expired handles, and undersized
  storage are caller programming errors, not detectable statuses.
- Strings are NUL-terminated byte strings; binary inputs have explicit sizes.
  For a byte span, zero size/capacity accepts either NULL or a non-NULL pointer
  and never accesses that storage. Positive size/capacity requires valid
  storage. This also applies to input cached headers and empty source-spec
  arrays; returned empty spans use their documented canonical NULL/0 form.
  No input/output overlap, including descriptors and borrowed metadata.
  Output handle slots must not contain an unreleased owned reference. Allocated
  buffer outputs must be initialized to `{NULL, 0}` and own no storage.
- Every handle in a request must belong to the same context; otherwise
  INVALID. Close of a NULL owned pointer has no object context to compare.
- Status-returning functions initialize writable outputs on failure: handle
  outputs NULL, structs/counts zero. Exceptions are in/out close pointers,
  normalizer capacity and partial transfer counts.
- `error.status` always equals the returned status, including OK and END.
  Native code is zero if absent; offset is UINT64_MAX if absent;
  message is always terminated. For effect-aware statuses, `error.cause` retains the underlying failure.
- Options are ordinary fixed-layout structs. Initialize every field; zero
  initialization selects documented defaults. `pg_write_options_init`
  derives both lengths once for logical input. Unknown values or flags are
  INVALID. There is no `struct_size` field or promise that differently sized
  descriptors interoperate. Header and binary versions must match.
- Calls borrow input storage through return. Writer open copies metadata,
  original name and cached header before returning. No caller buffer remains
  borrowed merely because a writer remains open.
- Native paths relative to process cwd require cwd to remain stable through
  return. Already-open objects are unaffected by later chdir.
- No process-global current tree, destination or error slot exists. No
  function is signal-safe. Callbacks must return normally and cannot throw
  or longjmp across the API.

Allocating calls may return NOMEM/LIMIT while preserving outputs. Source/native work
also returns NOT_FOUND, EXISTS, CONFLICT, UNSUPPORTED, IO, STALE, RETRY,
CORRUPT or RECOVERY_REQUIRED as appropriate. Writes add READ_ONLY and
INDETERMINATE. Header comments narrow these common outcomes. Diagnostic text
is not a control-flow interface.

## Ownership and release

`pg_buffer` is a plain owning pair of data and length, produced only by
`read_all_alloc`. Initialize it to `{NULL, 0}` before an output call and release
it with `pg_buffer_free(&buffer)`. This immediate, infallible function resets
both fields, does no native I/O, and leaves diagnostics unchanged. An empty
buffer is a valid no-op; the buffer address itself is required. Do not call
free/realloc on its data, edit its ownership fields, or create a second owner
by shallow copying. To transfer ownership, copy the pair and reset the original
to empty. The bytes are writable and outlive the source, file and
context; freeing needs no context. Independent buffers may be used concurrently;
access to one buffer must be serialized. Keep returned bytes alive through any
later call borrowing them as input. No application free
is needed after a failed allocated read: it releases all private bytes itself.

Every returned handle is one owned reference, closed exactly once. A context
accounts for its dependent references and cannot close while any exist.
Trees retain attached sources. Files/cursors retain captured metadata and
sources. Readers retain their file or native input; writers retain their
source/selected copy/builder. Parent release does not invalidate retained
children. An archive builder is the exception to early parent release: close
or finish returns BUSY while an entry writer is still live, including a
finished writer that has not been closed.

Every typed close takes `pg_type **`. Rejected close leaves `*handle` intact;
accepted close clears it immediately, even if later cleanup reports IO.
The pointer being non-NULL is the unambiguous indication that the caller
still owns it. Accepted close never returns ownership. NULL `*handle` is a
successful no-op. Cleanup completes before the synchronous call returns.
No close commits an unfinished writer or builder, deletes source data, or
recursively consumes other caller-owned references.

Accepting close requires no allocation, and cleanup may block. Backend cleanup
must not depend on new dynamic allocations. OS cleanup can fail; release all
library resources, report IO,
and leave any undeletable staging artifacts unindexed. Finished entry-writer
release has no fallible native work: those resources already belong to the
builder or were closed before entry acceptance.

A close can be rejected for INVALID arguments/state, BUSY due to a conflicting reference or wrong control thread, or REENTRANT. Correctly serialized cleanup
of an otherwise releasable handle is always accepted. Failed cleanup does
not mean the reference remains owned. Internal adapters reserve their cleanup
capacity before acquiring resources and never lose an owned temporary.

`pg_context_close` is immediate and allocation-free. It returns BUSY while
children remain. An unpack target retains its cursor snapshot and pinned root;
close returns BUSY while its staged writers remain live.

## Caller-controlled streaming

Writers transfer sequentially; readers also support absolute seeks. The
caller chooses each read capacity and write size. A reader reports exact bytes,
verifies available profile-selected hashes at EOF, and may be closed early
without a verification claim.
Reader seek accepts an absolute offset from zero through the represented size;
tell reports that offset without I/O. Invalid offsets leave position intact.
Seeking retains the selected physical copy and checks its native identity;
stale or operational failures make the reader close-only. Seeking alone makes
no verification claim, including at EOF. The next positive-capacity read at
EOF verifies the complete archive payload before returning END, even when
earlier bytes were skipped. Backward seek permits reading after EOF.

Native, uncompressed and stored-representation readers reposition directly.
Compressed logical readers start retaining decompressor checkpoints when seek
is first used. Checkpoints are spaced at 1 MiB boundaries, with up to 64 retained
per reader and least-recently-used eviction. A seek restores the nearest
preceding checkpoint and decodes the remainder; the initial state remains
available without a checkpoint. Cache allocation is optional: inability to
retain a checkpoint falls back to decoding without changing the public result.
Independent reader caches are released on close. Writer positions remain
sequential and cannot be changed.

A writer requires declared input and logical lengths. `pg_writer_finish`
checks lengths, codec and digest, then publishes or privately stages its result.
Closing before finish aborts unpublished content. A failed accepted write or
finish makes a writer close-only. Finish may block while assembling data.

For pack, request the source or tree root, capture its cursor, create a
builder, then for each file
open a reader and builder writer and transfer chosen chunks. Close each entry
writer before opening the next. Finish the builder to publish one archive.
For unpack, open an unpack target over the captured cursor and an existing
native directory, then open and finish one writer per captured file. Target
open preflights every path before publication, pins the native root and
snapshot, and rechecks paths at writer open and finish. Close the target after
all writers. Blocking pack and unpack helpers compose these same primitives.

## Threads and callbacks

A tree has one control thread, selected by create or ordered open. Source
and tree lookup, enumeration, subtree request, attachment, rescan, watch,
poll and tree cleanup use that thread. An attached source's control calls
use the tree thread. Standalone source control and builder access require
serialization. Independent readers can be read and closed on workers with
per-handle serialization; writer and selected-file access follow their
respective ownership rules. Calls sharing a handle are never concurrent.

Callbacks execute synchronously during `pg_tree_poll`, after reconciliation.
They may inspect metadata, find/read named files, open readers, advance
independent cursors, and close independent file/cursor/reader references.
Poll holds no internal lock while invoking an observer. A callback lookup
may reconcile newer hints or a reader open may register another watch; any
new reports wait for a later poll. Recursive poll, source/tree control
mutation and closing the polling caller's handle return REENTRANT. Callback
spans expire on return. Copy a name before scheduling work that outlives it.

## Names and resolution

All name-taking APIs use the same virtual spelling. Normalization converts
backslashes to `/`, folds ASCII A-Z, removes empty and `.` components and
trailing separators. It rejects rooted names, colons, any `..` component and
an empty result. Non-ASCII bytes are unchanged. Root subtree requests and
enumeration accept NULL/empty prefix; `.` is not a root alias. Native paths
remain separate and retain OS spelling.

Source open indexes all archive pathname metadata synchronously. Loose source
open records the root without recursively scanning it.
`pg_source_request_subtree` and `pg_tree_request_subtree` scan and retain a
complete loose subtree;
NULL/empty requests the root. A parent request covers descendant prefixes.
A missing prefix is retained as empty. Repeating the request refreshes that
prefix, including when watching is OFF. A failed or racing refresh preserves
the prior observation and returns an error; RETRY denotes an unstable scan.
These calls can block in proportion to the requested subtree size.

`pg_source_files` requires a covering source request for a loose source.
`pg_tree_files` requires a covering tree request if any loose source is
attached; source-local requests do not satisfy this tree precondition.
Without coverage, `files` returns INVALID and no cursor, even if some names
are indexed. Archive-only sources and trees, and empty trees, need no request.
Within coverage, listing captures the indexed visible files below the prefix
in unsigned-byte canonical lexical order. It does not force a subtree scan or
register a new watched prefix; NATIVE hints may be reconciled first. An exact
visible file -> CONFLICT; a missing directory -> empty cursor. The cursor
owns its captured selections and remains stable when the index changes.

Entry cursors follow the same coverage and snapshot rules. Zero enumeration
flags selects immediate children; `PG_ENTRIES_RECURSIVE` selects all descendants.
They include explicit loose directories, including empty ones, and implied
directories from archive paths. The requested root is excluded. Each next call
returns `pg_entry_info` and, for a file, transfers an owned `pg_file`. Directory
entries return no file handle. Entry names borrow the entry cursor until close;
file handles have independent ownership. Closing the entry cursor releases
unconsumed selections and metadata without modifying sources.

Entries use canonical lexical order. Directory metadata follows source order;
within one source an explicit directory takes precedence over an implied one.
Implied directories have zero size and timestamp and canonical original names.
Captured attributes expose read-only, hidden, system and implied flags. Hidden
uses Windows attributes or a dot-prefixed basename on Unix; system applies only
on Windows. File-only cursors and archive pack/unpack continue to omit directory
records. Requested empty directories participate in rescan and native-watch
reconciliation and are reported through entry metadata in the visible feed.

Exact source lookup uses the indexed result, including indexed absence, inside
a source-requested loose subtree. Tree lookup uses the overlay index inside a
tree-requested subtree. Outside those scopes, exact lookup probes relevant
loose paths and ancestors synchronously and retains the exact observation;
one exact probe does not request the surrounding subtree or start a watch.
Named read, reader-open, and export helpers use the same find semantics.
Cached metadata may identify a copy that has changed on disk; content access
revalidates the physical copy and reports STALE for an observably changed
selection. Writers independently probe and revalidate native targets and
hierarchy rather than trusting a cached read result.

With NATIVE watching active, tree listing and lookup reconcile pending hints
for tree-requested scopes before answering. Native hints are processed only
when a caller enters the tree API; no worker refreshes the index. SCAN mode
refreshes requested scopes at `pg_tree_poll`, not at listing or lookup.
With watching OFF, external edits inside a requested scope remain unobserved
until a repeat request or source/tree `rescan`. A source-local request on an
attached source does not create a tree watch scope; its indexed view requires
an explicit source refresh unless a covering tree request refreshes it.
Successful Piggle mutations and tree attachment changes update affected
requested indexes before returning, independent of watch mode.

The tree keeps attached sources in order. A later attachment wins an exact
file-name conflict, regardless of storage kind or timestamp. Detach and
reattach moves a source to the end. There is no precedence setting or
automatic game-directory discovery. Within an archive, the last active record
of one canonical name wins. Loose collisions choose the unsigned-bytewise
greatest original relative path. Losing records are not selectable through
the public API. Physical archive validation still examines every record.

An exact file loses to a directory at the same canonical path, including an
implied directory contributed by any source. Thus if `a` and `a/b` both
exist, `find("a")` returns CONFLICT and `a/b` may be visible. An exact file
at a proper ancestor never hides the subtree. A missing file returns
NOT_FOUND; a missing directory prefix yields an empty cursor. The same
hierarchy rule applies inside a standalone source. Empty directories are not
returned by file enumeration.

Named source writes use source-local visibility. A target that is a directory
or has a visible file at a proper ancestor returns CONFLICT; otherwise the visible
same-name copy is replaced or a new file is added. Writer finish rechecks the
target and hierarchy before publication. All nonzero source-writer flags are
INVALID. A retained selection can still target its captured physical copy
if a later attachment hides it. Builders reject duplicate canonical names,
including spellings that normalize to the same name; they may stage `a` and
`a/b`, with the directory winning when reopened.

## Copy identity and stable metadata

Source/copy IDs are context-local, nonzero and never reused. They are not
persistence keys. An in-place edit may retain a copy ID but changes its copy
generation. Remove/recreate or replacement invalidates the former identity.
Never bind an old handle to a different record reusing an offset or path.

A file captures one visible physical copy and its metadata, including original
name, lengths, mtime, digest and cached header. Later attachment changes never
retarget it. Cursor next returns a new owned file and preserves captured
order and metadata; NOMEM leaves position unchanged. Cursor snapshots do not
freeze native bytes. Metadata stays inspectable after edits, detachment or
parent release. Content access to an observably changed copy reports STALE.
Unchanged retained copies remain readable after detachment. Identity, size,
time and available digests detect observable races, not every transient rewrite.

## Readers, writers and native output

`pg_reader_open_source(source, name, representation, ...)` and
`pg_reader_open_tree(tree, name, representation, ...)` compose one find,
selected-file reader open, and temporary file close synchronously.
They publish only the owned reader, after all fallible setup/selection cleanup.
Failure closes private resources, leaves the output NULL and preserves the
first error. The captured identity, available digest verification, stored/logical
representation and stale behavior are identical to the selected-file opener.
There is no retry or second lookup. The name and output slot borrow through
return; they need not survive the returned reader. Named open follows its
source/tree control thread. After open, reader read/close can run on a worker
with per-handle serialization, just like an explicitly selected reader.


`pg_reader_open(file, representation, ...)` fixes one physical copy and starts
at zero. Logical mode decodes; stored mode returns the exact stored payload,
with reader metadata describing its encoding and exposed length. Stored EOF
still validates decoding, logical length and the available profile-selected
digest. There is no unchecked extraction mode. An independent reader does not move another
reader. Native readers capture one regular file's identity, size and timestamp.

Positive reads return up to capacity. The final nonempty read validates before
OK; completed OK with positive capacity always delivers at least one byte.
Subsequent positive reads return END/0. Empty files validate and return END/0
on the first positive read. A zero-capacity read is OK/0 without verification.
Reader calls complete before returning. Errors may
follow delivered bytes; counts remain exact and the reader becomes close-only.
Closing early does not imply verification. Explicit `pg_file_verify` reports
NO_CHECKSUM when none is stored; ordinary complete reads permit its absence.
Earlier chunks can precede a later verification failure. A streaming consumer
keeps its effects reversible until verified EOF; whole-file helpers enforce
their own documented output/publication boundary around that same reader.

Writers target a source-local name, selected copy, builder entry or native
path. All require complete input and logical lengths. Logical input requires
equal lengths; encoded input must be exactly one RFC 1950 stream, with no
truncation or trailing data. Chunk requests are sequential, with exact counts.
Streams requiring an external preset dictionary return UNSUPPORTED; the API
does not accept dictionaries. A private guard byte may detect excess decoder
output without exposing more than the declared logical size to the caller.
Only submit a new chunk after the previous call returns. Validation,
decoding, hashing, compression and publication are shared stages.

Entry options default to epoch timestamp, AUTO compression, no expected digest,
no cached header and no original-name override. The API never reads the clock
implicitly. The initializer derives
both lengths for streaming logical input; memory helpers derive them from
size. Output hashing remains required by archive formats even without an
expected digest. PIGG stores MD5, HOGG its four-byte prefix: these validate
format integrity, not authenticity. Unused expected digest bytes must be zero.
Lengths and timestamps must fit the selected wire/native format; conversion
overflow is LIMIT before publication, never truncation or wraparound.

Additions use canonical original names unless an explicit original_name
normalizes to the same identity. Replacements preserve their existing original
name and reject an override. Writer open owns copies of name/header/options
before return. Native/loose output rejects FORCE compression, nonempty cached
headers or
an original-name override as UNSUPPORTED; AUTO/NEVER produce ordinary bytes.

An open writer accepts zero or more writes, followed by one finish. Empty
files may finish without a write call. A failed accepted write or any accepted
finish leaves the writer CLOSE_ONLY. Finish is one-shot even on failure.
Rejected begin leaves writer state unchanged. Closing without successful
finish aborts staging. Source/native finish rechecks target identity, validates
the complete replacement, publishes once, then reports commitment independently
of later errors. Source generation and any tree visibility update precede
queuing notifications when the tree is watching.

Native file/archive creation is exclusive by default: an existing regular
file returns EXISTS. PG_OVERWRITE explicitly permits replacement; links,
directories and special objects remain CONFLICT. Capture target identity and
recheck at commitment; observed intervening edits are STALE/RETRY. Reject output
aliases into retained source data, including contained paths of loose sources.
Create missing parents through anchored native traversal; reject substituted
ancestors or links. Created directories may remain after abort. Cross-context
or external changes are detected when observable. Writable HOGG sources
also enforce an exclusive OS lease across cooperating contexts/processes.

Import/copy rejects an input that aliases its own replacement target. Native
path spelling and native input timestamps are preserved. Explicit copying from
one selected archive entry to a different entry of the same source is allowed;
finish input reads before publishing the replacement. Source-copy helpers
preserve mtime and archive cached headers, omit cached headers for loose
output, use canonical names for additions, and keep existing original names
for replacements. Archive-copy helpers can preserve input original spelling
when it normalizes to the target name.

## Composed whole-file tasks

`pg_source_export` and `pg_tree_export` find one named visible selection,
export through `pg_file_export`, then close the temporary selection. They retain the same logical bytes/mtime, normalization, native
conflict checks, flags and commitment as selected-file export. No private
handle escapes; the first error survives cleanup.
Post-publication cleanup failure returns PG_COMMITTED with IO as cause. They follow
the originating control thread throughout and are not callback-safe, just as
selected-file export is not. Explicit file export remains for retained identity.

`pg_source_read_all` and `pg_tree_read_all` select once through the corresponding
find call, use file read-all, and close the private selection. No file
reference escapes to the caller. They inherit lookup errors, captured identity,
logical decoding, verified empty-file handling, size limits and capacity rules.
CAPACITY leaves the buffer untouched with bytes zero. Other failures preserve
the exact delivered byte count; callers discard that prefix. No returned
allocation or implicit NUL terminator exists. Private cleanup completes before
return; the first work error wins, or cleanup IO if work succeeded. They do not
retry or change winners on STALE. All inputs borrow through return. Whole-buffer
reads, writes and allocated reads are synchronous. Callers needing transfer
scheduling use caller-sized reader and writer chunks; writer finish may block while publishing. Entry writes still stage only.

The `read_all_alloc` family adds a bounded allocation around the same read.
File variants use the captured selection. Source/tree variants find once and
retain that same selection through size inspection, reading and cleanup. The
required `size_t max_bytes` bounds decoded logical content: zero permits only
an empty file; there is no unlimited sentinel. A logical size exceeding the
limit or SIZE_MAX returns LIMIT before allocating or reading payload bytes.
The cap is not a total working-memory budget; indexing, metadata and codec
scratch have their own costs. Allocate the captured logical size only, not the
maximum. Allocation failure is NOMEM. No NUL terminator is appended.

Allocated bytes stay private. Only after successful verification and
all fallible cleanup, including the temporary selection's release, does the
caller receive the buffer. Any failure frees private bytes and
leaves the output `{NULL, 0}`. A verified empty file also returns `{NULL, 0}`,
with PG_OK; status distinguishes it from failure. Do not reselect or silently
resize/retry if the physical content changes. The caller uses `pg_buffer_free`
to release successful output. No partial allocated buffer is returned. These read operations never publish content.

Source/tree pack captures the corresponding files cursor, creates a private
builder, copies each selected file with the requested compression, and closes
all input selections before builder finish. The format is explicit PIGG2 or
HOGG10. NULL `pg_pack_options` selects AUTO compression and exclusive creation;
its flags may be zero or PG_OVERWRITE. Preserve original names, timestamps and
archive headers. Hidden copies and empty directories are omitted. Publication
is one archive transaction, including a valid empty archive. Before publication,
failure leaves the target unchanged; later errors use ordinary commitment rules.

Source/tree unpack captures the corresponding files cursor and preflights
canonical-name mappings and native collisions before exporting under an existing
directory. Flags are zero or PG_OVERWRITE. Each file export preserves mtime and
publishes independently; earlier completed files survive a later failure. Created parent directories may remain. Aliases into retained
source data, links and unsafe native substitutions are CONFLICT. Unpack is not
a cross-file transaction. Pack and unpack both report STALE/RETRY on observable
input changes and retain the first failure through cleanup. A failed unpack
reports PG_PARTIAL after any file publication. Empty unpack publishes nothing.

Source variants use source-local visibility without constructing or attaching
a tree. Tree variants use overlay visibility. Both use the same cursor and
reader/writer/builder contracts; there is no temporary change to source order
or attachment. A source form can operate on an attached source without detaching
it, on its control thread. These compound tasks obey the originating source or
tree control rules throughout execution; native output tasks are not permitted
from observer callbacks. Ordinary named source mutations compose a source writer
without adding separate hierarchy rules in the adapter.

## Archive construction

`pg_archive_builder_create` prepares private staging for an explicit PIGG2/HOGG10
native destination, retaining original target identity. It publishes nothing.
A `pg_archive_builder` has one live entry writer; close it before another entry,
builder finish or builder close. Separate builders can be used concurrently
subject to native target conflicts.

An entry writer's finish validates the complete entry, closes fallible native
resources and installs the prepared entry as its final non-failing transition.
It publishes no external archive.
A failed finish never installs that entry. Releasing a successfully finished
entry writer has no fallible native cleanup. Compound write-all/import/copy helpers
finish and close input readers before installing the entry. Thus a failed
staging helper cannot leave an ambiguously accepted entry; earlier entries
remain intact. These ordering requirements are part of the API contract.

Names colliding after normalization always return EXISTS. Existing archives
may contain colliding records; the last active one wins. Hierarchy conflicts are
representable and obey the normal file/directory rules. Internal format
records are produced by Piggle, never supplied by callers.

Builder finish validates all records and metadata and publishes the whole
archive once. Empty archives are valid. Before publication, failure
preserves any old native target. An accepted finish makes the builder close-only
on every outcome; do not retry it. PG_OVERWRITE is honored only against the
captured unchanged regular target. Closing an unfinished builder aborts all
entries without publishing. No source is implicitly opened after creation;
open one explicitly if subsequent lookup or mutation is needed.

## Publication outcomes

Ordinary failure statuses mean no user-file publication by the call.
`PG_PARTIAL` means some files in a batch were published before a later
failure. `PG_COMMITTED` means all intended publications occurred before a
later failure, such as durability or cleanup. `PG_INDETERMINATE` means the
outcome cannot be established. `PG_RECOVERY_REQUIRED` means a committed HOGG
journal must be replayed. For these effect-aware statuses, `pg_error.cause`
retains the underlying failure. `pg_error.status` always matches the return
value. An empty successful archive still counts as published; an empty unpack
publishes nothing. No count or step payload ledger is exposed.

Loose/native replacement, new archive, and PIGG edits publish a validated
staged native file. HOGG edits commit a durable journal frame and replay
forward. Unpack publishes each file independently; a later failure reports
`PG_PARTIAL`. Builder entry finish stages privately and has no disk effect.
A failed writer can be closed to abort work that has not been published.

HOGG edits use the operation journal and DataList facilities of the documented
format. Prepare and validate payload/metadata before committing their frame.
Preserve unaffected semantics. An unapplied committed frame makes the source
recovery-required and blocks new content operations; captured metadata survives.
Recovery validates known profiles, replays idempotently, flushes/clears the
journal, rescans and publishes. Unknown/corrupt profiles are rejected without guessed repair. Committed
maintenance finishes forward.

PIGG edits validate a complete clone before native replacement. Preserve
unaffected compressed bytes, original names, timestamps, cached headers,
supported opaque fields and relative record order, including collisions.
Offsets/pool layout may change. Unsupported opaque semantics return UNSUPPORTED
before replacement; invalid clones never replace originals. None of these
format stages are caller-managed compression/hash tasks.

Whole-source deletion requires a detached writable source without live readers
or writers. It removes the archive or recursively unlinks the loose root,
without following encountered links. It is distinct from file deletion and
release. After deletion the source can be inspected/closed; other source
operations return STALE. Retained file metadata survives, content is stale.

## Polling and change feeds

Trees start with `PG_WATCH_OFF`. `pg_tree_watch(tree, PG_WATCH_NATIVE)` uses inotify
on Linux or ReadDirectoryChangesW on Windows; unsupported capability returns
UNSUPPORTED and setup failure returns IO. `PG_WATCH_SCAN` requires no OS
watcher. Both modes are driven by caller calls; neither starts a worker.
Watch and unwatch are synchronous. Starting the same mode is a no-op;
switching modes requires unwatch first. OFF is not a watch input.

Starting a watch establishes a stable baseline for already requested tree
subtrees and open tree readers without initial events. Subsequently,
`pg_tree_request_subtree` adds persistent watched prefixes, including missing
prefixes. A source-local request on an attached source does not add a tree
watch scope. `pg_reader_open_tree` and `pg_reader_open` on a tree-selected
file add a watched exact name until that reader closes. The latter does so
only while its originating tree is live. Tree close stops monitoring but
never invalidates a retained reader's content reference. A failed native
watch setup fails the call that would add the scope, without publishing its
output or a partial cache. A reader close stops its watch after the close is
accepted; events already queued remain pending. Inside managed coverage, `find`
retains a baseline, including known absence.
Outside it, `find`, cursor advancement and standalone-source readers do not
add watched scopes. Management is independent of discovery completeness.

NATIVE tracks loose directories and archive paths and retains hint names.
An exact lookup reconciles hints affecting that name; poll reconciles the
remaining cut. Directory watch setup does not index files. SCAN eagerly
discovers managed scopes and polls them along with requested scopes and
active exact names. Synchronous tree lookup and listing also process
pending native hints for requested tree scopes before answering, but never
call observers. Without watching, exact lookup probes an unrequested loose
path again; a requested path is answered from its index. Subtree requests
refresh their prefix; listings never refresh it. `pg_tree_rescan`
refreshes indexed archives and requested loose paths/prefixes, including
when watching is OFF; it does not enumerate unrelated loose paths.
`pg_source_rescan` applies the same rule to one source. Failed or racing
refresh preserves prior cached observations; RETRY reports an unstable scan.
Existing file and cursor metadata never refresh in place.

Source/tree pack and unpack helpers explicitly refresh the loose root before
enumerating, even if it was requested before. They therefore include all
currently discoverable loose files without requiring a prior caller request.
Archive content follows its indexed view unless the caller rescans it.

`pg_tree_poll` processes one finite observation cut and delivers visible
reports synchronously on the tree control thread. The observer has one
optional callback; NULL discards delivered reports. `before_entry` and
`after_entry` describe file or directory sides; `before` and `after` expose
additional metadata only for file sides. ADD has no before side, REMOVE no
after side, and UPDATE includes both, including a changed winner with equal
content. A known hidden-only edit produces no visible transition. Unknown
managed history produces INVALIDATE without claiming a transition. Events are
limited to watched names and prefixes. They may coalesce external intermediate
edits; no revision history is promised. Attachment and detachment invalidate
managed scopes and compare known visible names. Library mutations queue changes for known watched names.
Publication precedes callbacks, so callback lookup sees the latest reconciled
state. Reports queued before a reader closes remain deliverable. Unwatch or
tree close discards undelivered reports.

Native notification loss queues `PG_CHANGE_LOSS` with a known watched scope
when available, or NULL scope when provenance is unknown. LOSS invalidates
application assumptions; it is not a removal. Poll invalidates managed scopes
and refreshes discovered coverage and exact observations before considering
loss repaired. It does not index unknown managed descendants. A missing loose root remains managed; when it reappears,
Piggle rebinds it, invalidates the scope and restores native monitoring.
Retained selections keep their captured identity and become stale when the
corresponding native path changes. A failed reconciliation retains queued
reports and prior cached state. No ordinary report allocation is required
merely to remember loss.
Callbacks already delivered are never replayed. Callback names and metadata
borrow until return; copy them before retaining or scheduling work.

## Resource use and scheduling

Archive and requested loose indexes retain pathname metadata. Archive sources
also retain a descriptor for payload ranges. Readers
keep decoder state and use bounded transfer buffers. Writers stage content in
private temporary storage, hash and compress incrementally, and assemble an
archive by streaming staged or borrowed ranges into a temporary archive.
Memory may scale with entry metadata and active fixed-size buffers; temporary
disk use may scale with payload size. Compression uses zlib-ng. Callers control
transfer scheduling with read and write chunk sizes. Opening, finishing,
indexing, and blocking helpers may still block within one call.

## Related documentation

See [testing](testing.md) for test commands. The [PIGG v2](pigg-v2-format.md) and
[HOGG v10](hogg-v10-format.md) specifications define the on-disk formats.

## Checksum profiles

Source options select LOGICAL (zero/default) or STORED checksums. STORED is
supported for HOGG user records only. The four wire bytes have no domain tag;
opening with the wrong profile produces CHECKSUM when verification fails.
Internal DataList records always retain their logical checksum convention.
Both reader representations verify exact encoding and decoded length as well
as the selected digest. There is no accept-either fallback.

`pg_archive_builder_create_options` accepts an explicit archive profile;
`pg_archive_builder_create` retains logical defaults. Pack options select the
destination profile. Captured file/source metadata includes the domain.
Expected-digest options independently select logical bytes or final stored
output bytes. Copy and export verify the source profile before publication
and compute the destination profile; a stored input hash is never reused as
a logical expectation. Per-entry source profiles are not supported.

## Shallow discovery

`pg_source_discover` and `pg_tree_discover` accept CHILDREN or RECURSIVE.
CHILDREN enumerates one directory without opening its child directories;
applications prune or stop by deciding which children to discover next.
The existing request_subtree calls select RECURSIVE. Directory-capable entry
cursors accept shallow coverage for immediate listings, while recursive
listings require recursive coverage. Discovery and enumeration remain separate:
a snapshot never silently scans an incompletely discovered subtree.

## HOGG write ownership and readers

A writable HOGG source holds an exclusive OS lease until its last retained
reference closes. Competing writable opens, recovery or native replacement
through Piggle return BUSY. Read-only opens remain possible; independent
contexts retain external-change detection rather than sharing cached indexes.
The lease coordinates Piggle users, not legacy Cryptic mutex protocols.

HOGG reads, seeks, verification, recovery and publication serialize for the
duration of each operation. An idle reader does not exclude writes. Known internal
mutations preserve unchanged copies, including their relocation during table
growth. Changed or deleted selected entries return STALE on subsequent content
access, including metadata-only changes. Retained metadata never changes.
External changes remain conservatively stale. PIGG reader exclusion is unchanged.

## Metadata-only mutation

`pg_file_update_metadata` updates a captured writable copy. MTIME and HEADER
flags select the fields; other fields, original spelling, payload encoding,
lengths and checksum remain unchanged. An empty selected header clears it.
No-op values do not advance generations. HOGG timestamp edits use UPDATE without
payload growth; header edits retain the payload range through DataList/journal
publication. PIGG edits clone encoded ranges without recompression. Loose files
support MTIME and reject HEADER. Old selections remain inspectable but become
stale for content after a change. Ordinary publication outcome rules apply.

## Managed scopes and unknown state

`pg_tree_manage(tree, prefix, depth, error)` registers observation coverage
without claiming discovery completeness. Use one tree per physical source
when the application owns winner selection. In native mode, unknown names
produce `PG_CHANGE_INVALIDATE` in the existing visible feed, with no invented
before/after state. Lookups (including known absence) and explicit discovery
establish baselines; subsequent visible transitions use ADD/UPDATE/REMOVE.
Directory transitions expose `before_entry` and `after_entry`; the original
file metadata pointers are populated only for file sides.

Linux watch setup visits directories but does not index files. Windows uses
recursive native monitoring. Ordinary hints retain their relative paths and
refresh affected known names/subtrees. SCAN management establishes an eager
baseline and refreshes it at poll. Native loss invalidates managed scopes;
only previously discovered scopes are rescanned. Applications choose when to
discover unknown subtrees. Directory rename invalidates unknown descendants.

Registrations form a union and persist through unwatch. Unmanage removes only
the matching prefix/depth registration, preserving discovery, queued reports,
and independent reader watches. Detaching a source removes its native watches.
Lookup/list operations queue reports, never invoke observers. Reports queued
inside an observer wait for a later poll.

The 0.2 API extends plain option, inspection and change structs. Rebuild
clients against matching headers and library; mixing 0.1 structs with a 0.2
binary is unsupported. Existing archive wire formats are unchanged.
