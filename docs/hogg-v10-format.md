# HOGG version 10 archive format

This document specifies the HOGG version 10 disk format. It is intended to be
sufficient to write a new archive, enumerate it, read either stored or logical
payloads, verify its checksums, and recover an interrupted in-place update.

The fixed layouts and recovery behavior below are compatibility requirements.
Items called "recommended" are choices that produce a valid deterministic
archive but are not required by the format. The final section lists facts that
remain unknown or intentionally unspecified.

## Conventions

- All multibyte integers are little-endian.
- `u8`, `u16`, `u32`, and `u64` are unsigned integers of the stated width;
  `i32` is a two's-complement signed 32-bit integer.
- All offsets are absolute file offsets unless explicitly described as
  relative to a structure.
- `0xffffffff` is called `NO_VALUE` below.
- Additions and multiplications used to derive ranges must be checked for
  integer overflow before checking them against the file size.
- There is no general alignment rule for payloads. Fixed records have the
  exact strides documented below, including the explicit padding in a file
  record. Readers must not map these records onto host C structures.

This specification covers the desktop Windows layout. An Xbox-specific
header-data union ordering is outside its scope.

## Archive map

The archive consists of the following regions without implicit padding:

| Region | Offset | Size |
|---|---:|---:|
| Archive header | `0` | 24 |
| Operation journal | `24` | `op_journal_size` |
| DataList journal | `24 + op_journal_size` | `dl_journal_size` |
| File table | `table_base` | `file_list_size` |
| Extended-attribute (EA) table | `ea_base` | `ea_list_size` |
| Payload area | at or after `metadata_end` | remainder of file |

where:

```text
table_base   = 24 + op_journal_size + dl_journal_size
ea_base      = table_base + file_list_size
metadata_end = ea_base + ea_list_size
file_count   = file_list_size / 32
ea_count     = ea_list_size / 16
```

Payload offsets are absolute. Payloads may contain gaps, need not occur in file
slot order, and may be relocated by later mutations. A canonical fresh archive
uses `op_journal_size = 1024` and `dl_journal_size = 3096`, making
`table_base = 4144` (`0x1030`). A reader must use the declared sizes rather
than assuming the constants.

### Archive header (`HogHeader`, 24 bytes)

| Offset | Size | Type | Meaning |
|---:|---:|---|---|
| 0 | 4 | u32 | Magic, exactly `0xdeadf00d` |
| 4 | 2 | u16 | Version, exactly `10` |
| 6 | 2 | u16 | Operation-journal reservation in bytes |
| 8 | 4 | u32 | File-table size in bytes |
| 12 | 4 | u32 | EA-table size in bytes |
| 16 | 4 | u32 | Physical file slot containing `?DataList`, or `NO_VALUE` |
| 20 | 2 | u16 | DataList-journal reservation in bytes |
| 22 | 2 | u16 | Unused padding; write zero, ignore when reading |

The operation journal must reserve at least 8 bytes and the DataList journal
at least 12 bytes. For broad compatibility, writers should reserve exactly
1024 and 3096 bytes, respectively.

## File and EA tables

### File record (`HogFileHeader`, 32-byte stride)

| Offset | Size | Type | Meaning |
|---:|---:|---|---|
| 0 | 8 | u64 | Absolute stored-payload offset |
| 8 | 4 | u32 | Stored byte length; `NO_VALUE` marks a free file slot |
| 12 | 4 | i32 | Modification time in UTC seconds from the Unix epoch |
| 16 | 4 | byte[4] | First four bytes of MD5 of the logical payload |
| 20 | 4 | byte[4] | Native ABI padding; write zero, ignore when reading |
| 24 | 8 | byte[8] | Opaque header data or a tagged EA reference |

A live stored size is at most `0xfffffffe`. A zero-sized live file has stored
size zero; fresh writers also use offset zero or the current payload cursor.
For a nonempty live file, the complete `[offset, offset + stored_size)` range
must be inside the archive and begin at or after `metadata_end`.

The checksum bytes are not a numeric MD5 word to be reordered. Compute the
standard 16-byte MD5 digest over the logical, decompressed payload and copy
digest bytes 0 through 3 verbatim. For example, MD5(`"abc"`) begins
`90 01 50 98`, which are the four bytes stored in the record.
There is no separate checksum-present bit. Readers should treat an all-zero
prefix as unavailable. A new writer should always store the computed prefix,
including `d4 1d 8c d9` for an empty logical payload.

The eight bytes at offset 24 form this tagged union on little-endian desktop
archives:

| Relative offset | Size | Type | Meaning when tagged |
|---:|---:|---|---|
| 24 | 2 | u16 | EA tag, exactly `0xfffe` |
| 26 | 2 | u16 | Reserved; writers use zero |
| 28 | 4 | i32 | EA slot, or `-1` (`NO_VALUE`) for no EA |

If the low `u16` is not `0xfffe`, all eight bytes are opaque per-file header
data. This form is used by unnamed records. If it is tagged and the EA ID is
not `-1`, the referenced EA must be in range and live. Writers must not use an
opaque value whose low 16 bits accidentally equal the tag.
With a zero reserved field, a tagged desktop value can be formed as
`0x0000fffe | (u64(u32(ea_id)) << 32)`, or written field by field to avoid
host layout assumptions.

### EA record (`HogEAHeader`, 16-byte stride)

| Offset | Size | Type | Meaning |
|---:|---:|---|---|
| 0 | 4 | u32 | DataList slot containing the pathname, or `NO_VALUE` |
| 4 | 4 | u32 | DataList slot containing cached-header bytes, or `NO_VALUE` |
| 8 | 4 | u32 | Logical size if compressed; zero if stored raw |
| 12 | 4 | u32 | Flags; bit 0 means the EA slot is free |

Only flag bit 0 is defined. A new writer should write zero for a live EA and
`1` for a free EA. A preserving editor should retain unknown flag bits when it
is not replacing the record.

`unpacked_size != 0` is the sole compression marker. Therefore a compressed
entry necessarily has a nonzero logical size, and an empty file is represented
as raw. For raw entries, logical size equals the file record's stored size.
The name and cached-header IDs address the effective DataList described next.
A cached header is an opaque nonempty byte string; it has no HOGG-level schema.

There is no on-disk pathname hash or sorted pathname index. Physical file and
EA slot numbers are stable identifiers across ordinary in-place changes until
the corresponding slots are deleted and reused.

## The `?DataList` metadata file

Pathnames and cached headers live in a special file whose physical slot is
given by `datalist_fileno` in the archive header. Its file and EA records use
the same layouts as any other named file. Its conventional stored pathname is
the ten bytes `?DataList\0`. The physical slot in the archive header, rather
than a pathname search, is authoritative for locating it.

The DataList file may be raw or zlib-compressed according to its EA
`unpacked_size`. Decode it exactly as an ordinary payload before parsing it.
Its file-record checksum is likewise the first four MD5 bytes of the decoded
DataList. The DataList contains its own pathname, so constructing a fresh one
requires assigning that final slot before serializing it.

`datalist_fileno == NO_VALUE` is structurally supported and means that there is
no base DataList payload. Begin with an empty slot array and still apply the
selected DataList journal, which can introduce slots. Without such journal
adds, all name and cached-header IDs must be `NO_VALUE`; EA compression metadata
does not itself require a DataList slot. Fresh interoperable writers normally
always create `?DataList`.

### Base DataList layout

| Offset | Size | Type | Meaning |
|---:|---:|---|---|
| 0 | 4 | u32 | DataList version, exactly `0` |
| 4 | 4 | u32 | Number of slots, `data_count` |
| 8 | variable | slots | `data_count` consecutive slot encodings |

Each slot is encoded without alignment padding:

```text
u32 byte_length
u8  data[byte_length]
```

A zero length marks a free slot. Nonzero slot IDs remain meaningful even when
other slots are free, so a decoder must retain the sparse indices. The last
slot must end exactly at the decoded DataList length; trailing bytes are not
part of version 0.

A pathname slot includes its terminating NUL in `byte_length`, has at least two
bytes, ends in exactly one NUL, and has no earlier NUL. A cached-header slot is
opaque and nonempty. The DataList itself does not distinguish the two uses.

### DataList journal

The DataList journal starts at `24 + op_journal_size`. Its 12-byte header is:

| Offset | Size | Type | Meaning |
|---:|---:|---|---|
| 0 | 4 | u32 | `inuse` publication flag |
| 4 | 4 | u32 | New action-stream length, `size` |
| 8 | 4 | u32 | Previously committed length, `oldsize` |

Select the effective action-stream length as follows:

```text
selected_length = (inuse != 0) ? oldsize : size
```

It must be no greater than `dl_journal_size - 12`. Starting at journal offset
12, parse exactly `selected_length` bytes as packed actions:

| Opcode | Bytes | Meaning |
|---:|---|---|
| 1 | `u8 opcode; i32 slot; u32 length; u8 data[length]` | Add or replace `slot` |
| 2 | `u8 opcode; i32 slot` | Free `slot` |

Slot IDs must be nonnegative. An add/replace length must be nonzero. Applying
opcode 1 implicitly frees any previous value, grows the sparse slot array
through `slot` if needed, and installs the supplied bytes. Opcode 2 frees an
existing slot; an out-of-range free is a no-op. Apply actions in byte order
after decoding the base DataList.

The crash-safe publication order is:

1. Append complete action bytes at journal offset `12 + old selected length`.
2. Write `inuse = 1`.
3. Write the new total `size`.
4. Write `inuse = 0`.
5. Write `oldsize = size`.

Each field write is a four-byte little-endian write. An interruption therefore
selects either the old complete prefix or the new complete prefix. Before the
journal fills, a writer compacts it by writing a new complete base DataList,
publishing the `?DataList` file-record change through the operation journal,
and then using operation-journal action 6 to clear this journal safely.

## Entry interpretation

For each file slot whose stored size is not `NO_VALUE`:

1. Validate its stored payload range.
2. If the header-data tag is `0xfffe` and its EA ID is not `NO_VALUE`, resolve
   that live EA. A tagged `NO_VALUE` ID means no EA. If the tag is absent, the
   eight header-data bytes are opaque. Having no EA does not by itself assign a
   name.
3. Resolve a non-`NO_VALUE` name ID through the effective DataList. Validate
   it as a pathname string. A missing name ID denotes an unnamed entry.
4. Resolve a non-`NO_VALUE` cached-header ID through the effective DataList.
5. If EA `unpacked_size` is zero, expose the stored bytes directly and set the
   logical size equal to the stored size. Otherwise inflate the stored bytes as
   one RFC 1950 zlib stream and require exactly `unpacked_size` output bytes,
   the zlib end marker exactly at `stored_size`, and no trailing stored bytes.
6. To verify the file, MD5 the logical bytes and compare its first four bytes
   to the file-record checksum.

The `?DataList` slot is internal metadata and should normally be hidden from a
user-facing virtual filesystem, though a physical archive enumerator should
still expose it. HOGG stores files only; directory entries are derived from
pathname components by higher-level code.

### Path bytes and lookup

HOGG declares no character encoding for pathname bytes. The original format
uses NUL-terminated narrow strings. Compatible readers preserve their bytes and apply
case folding only to ASCII `A` through `Z` when configured for legacy lookup.

For safe, interoperable lookup, canonicalize a pathname as follows while
retaining the stored spelling for display:

- require a nonempty relative pathname;
- reject an initial slash or backslash and an ASCII drive prefix such as
  `C:`;
- treat slash and backslash as separators;
- collapse repeated separators and remove `.` components;
- reject every `..` component rather than resolving it;
- reject a trailing separator for a file; and
- optionally fold ASCII uppercase to lowercase for legacy case-insensitive
  matching.

Path-based readers should reject duplicate canonical names and reserve
`?DataList` under the selected matching policy. The disk format contains no
field that resolves duplicates, so a general forensic reader may enumerate
them but should not silently choose one for pathname lookup.

## Operation journal and recovery

The operation journal begins at file offset 24. It is a single-frame redo
journal:

| Journal-relative offset | Size | Meaning |
|---:|---:|---|
| 0 | 4 | Frame length `N` |
| 4 | `N` | Windows HFJ frame |
| `4 + N` | 4 | Commit terminator `0xdeabac05` |

The journal is committed only if `N != 0`, `N <= op_journal_size - 8`, and the
terminator matches. Zero length, an out-of-range length, or a missing/torn
terminator means there is no committed operation. A defensive reader still
range-checks before looking for the terminator.

Frames contain ABI padding and ignored process pointers. The following are the
only supported profiles. Frame offsets start at the action field, which is
journal offset 4. All meaningful integers are little-endian. Ignored and
padding bytes must not be dereferenced or interpreted. A writer should zero
all padding and ignored pointer bytes.

### Action 1: DELETE (`N = 12`)

| Frame offset | Size | Type | Meaning |
|---:|---:|---|---|
| 0 | 4 | u32 | Action `1` |
| 4 | 4 | u32 | File slot |
| 8 | 4 | i32 | EA slot, or `-1` |

Replay writes a zeroed 32-byte file record with stored size `NO_VALUE`. If the
EA slot is not `-1`, it writes a zeroed 16-byte EA record with flags `1`.

### Action 2: ADD (`N = 64` or `72`)

`N = 64` is the Windows32 profile and the recommended writer profile. `N = 72` is
the Windows64 profile. Both have identical meaningful fields:

| Frame offset | Size | Type | Meaning |
|---:|---:|---|---|
| 0 | 4 | u32 | Action `2` |
| 4 | 4 | - | ABI padding |
| 8 | 4 | u32 | File slot |
| 12 | 4 | u32 | Stored size |
| 16 | 4 | i32 | Modification time |
| 20 | 4 | - | ABI padding |
| 24 | 8 | byte[8] | File header data / tagged EA reference |
| 32 | 4 | byte[4] | Logical-payload MD5 prefix |
| 36 | 4 | - | ABI padding |
| 40 | 8 | u64 | Absolute payload offset |
| 48 | 4 | u32 | Logical size if compressed, else zero |
| 52 | 4 | i32 | DataList name ID, or `-1` |
| 56 | 4 | i32 | DataList cached-header ID, or `-1` |
| 60 | 4 | - | Win32 ignored pointer; Win64 padding |
| 64 | 8 | - | Win64 ignored pointer; absent from `N = 64` |

Replay constructs and writes the complete file record, with file-record
padding zeroed. If its header data is tagged and its EA ID is not `-1`, replay
also writes a live EA using the logical size, name ID, and header ID in the
frame. No payload is copied during replay; the writer must have written and
flushed it before committing this frame.

### Action 3: UPDATE (`N = 56`)

| Frame offset | Size | Type | Meaning |
|---:|---:|---|---|
| 0 | 4 | u32 | Action `3` |
| 4 | 4 | - | ABI padding |
| 8 | 4 | u32 | File slot |
| 12 | 4 | u32 | Stored size |
| 16 | 4 | i32 | Modification time |
| 20 | 4 | - | ABI padding |
| 24 | 8 | byte[8] | File header data / tagged EA reference |
| 32 | 4 | byte[4] | Logical-payload MD5 prefix |
| 36 | 4 | - | ABI padding |
| 40 | 8 | u64 | Absolute payload offset |
| 48 | 8 | - | Ignored pointer/padding |

Replay replaces the complete file record and does not change its EA record.
This action is used for changes such as timestamps and for replacing an
uncompressed `?DataList` record while preserving its EA.

### Action 4: MOVE (`N = 32`)

| Frame offset | Size | Type | Meaning |
|---:|---:|---|---|
| 0 | 4 | u32 | Action `4` |
| 4 | 4 | - | ABI padding |
| 8 | 4 | u32 | File slot |
| 12 | 4 | u32 | Stored size |
| 16 | 8 | u64 | Old payload offset |
| 24 | 8 | u64 | New payload offset |

Replay writes only the new offset into the file record. Stored size and old
offset describe/validate the copy that the writer must complete and flush
before committing the journal; replay itself does not copy payload bytes.

### Action 5: FILELIST_RESIZE (`N = 28` or `32`)

`N = 28` is the Windows32 profile and the recommended writer profile. `N = 32` is
the Windows64 profile produced despite the original structure's 4-byte packing
exception.

| Frame offset | Size | Type | Meaning |
|---:|---:|---|---|
| 0 | 4 | u32 | Action `5` |
| 4 | 4 | u32 | New file-table size in bytes |
| 8 | 4 | u32 | Old EA-table absolute offset |
| 12 | 4 | u32 | Old EA-table size in bytes |
| 16 | 4 | u32 | New EA-table absolute offset |
| 20 | 4 | u32 | New EA-table size in bytes |
| 24 | 4 | - | Win32 ignored pointer / low half of Win64 pointer |
| 28 | 4 | - | High half of Win64 ignored pointer; absent for `N = 28` |

Before committing this frame, the writer must place the complete new EA table
at `new_ea_offset`. Replay writes free file records, each with stored size
`NO_VALUE`, in 32-byte steps over `[old_ea_offset, new_ea_offset)`. It then
writes the new file-table byte size to archive-header offset 8 and the new
EA-table byte size to archive-header offset 12. This deliberately overwrites
the old EA location as newly added file slots.

During an interrupted resize either four-byte archive-header size field may
contain a bytewise mixture of its old and new value. Recovery must trust the
complete committed frame after validating its ranges; it must not require the
current field to equal one whole value. A compatible recovery implementation
accepts each current byte only when it equals the corresponding old or new
byte.

### Action 6: DATALISTFLUSH (`N = 16`)

| Frame offset | Size | Type | Meaning |
|---:|---:|---|---|
| 0 | 4 | u32 | Action `6` |
| 4 | 4 | - | ABI padding |
| 8 | 8 | u64 | Absolute offset of the DataList-journal header |

Replay clears the DataList journal using its own publication protocol: write
`inuse = 1`, write `size = 0`, write `inuse = 0`, then write `oldsize = 0`.
The offset must equal `24 + op_journal_size`.

### Journal transaction order

To publish any operation safely:

1. Write all new payload bytes and prerequisite metadata, such as the copied
   EA table for a resize, then flush them durably.
2. Write operation-journal length, frame, and terminator, in that order, and
   flush. The operation is now committed and must be replayed after a crash.
3. Apply exactly the replay changes above and flush them.
4. Zero the four-byte terminator first and flush it.
5. Zero the length and frame and flush them.

Replaying a committed frame is idempotent. A read-only implementation should
report that recovery is required rather than exposing possibly partial
metadata. A reader that cannot modify the archive may instead apply the redo
record to a private metadata view, provided it implements the same validation
and action semantics. An unknown committed action or frame length is an
unsupported journal profile, not an invitation to guess a host-native layout.

### Committed-frame validation

Validate the complete frame before replaying any part of it. In addition to
the exact action/length profiles above, enforce these constraints:

- For all actions other than FILELIST_RESIZE, the current file and EA table
  sizes must be exact record multiples and their combined range must fit in the
  file.
- DELETE's file slot must be below the current file count; its non-`-1` EA slot
  must be below the current EA count.
- ADD and UPDATE must name an existing file slot, must not use stored size
  `NO_VALUE`, and must describe a payload range inside the file. A nonempty
  payload must start after the current metadata end. A tagged, non-`-1` EA
  reference must be below the current EA count.
- MOVE must name an existing file slot, must not use size `NO_VALUE`, and its
  new payload range must fit in the file and start after metadata when
  nonempty.
- FILELIST_RESIZE requires `old_ea_offset >= table_base`,
  `(old_ea_offset - table_base) % 32 == 0`, both EA sizes divisible by 16, the
  new file-table size divisible by 32, and
  `new_ea_offset == table_base + new_file_table_size`. The new file table and
  EA table cannot shrink. Both the old and new EA ranges must fit in the file.
  Validate the partially published header-size bytes as described under action
  5, then apply configured entry-count limits to the new sizes.
- DATALISTFLUSH's target must be exactly the declared DataList-journal start.

These checks ensure replay writes only to the archive regions named by a valid
transaction. Implementations may additionally cap counts, payload sizes, and
decoded bytes for resource control.

## Complete reader procedure

1. Read 24 bytes. Require magic `0xdeadf00d` and version 10. Reject shorter
   files.
2. Validate the two journal reservations (`>= 8` and `>= 12`) and their range.
3. Read the operation journal and determine whether it contains a committed
   frame. If so, validate and replay it before trusting ordinary table sizes,
   or construct the equivalent private recovered view. This ordering matters
   for action 5 because the header sizes may be partially published.
4. Require `file_list_size % 32 == 0`, `ea_list_size % 16 == 0`, and
   `metadata_end <= file_size`. Apply implementation limits before allocating.
5. Read both tables. Require `datalist_fileno` to be `NO_VALUE` or a valid live
   file slot. For every live file, validate its payload range and any tagged EA
   reference. A referenced EA with flag bit 0 set is corrupt.
6. If present, read and, when its EA logical size is nonzero, zlib-inflate the
   `?DataList` payload with exact input/output termination checks.
7. Select the DataList-journal length, append those bytes conceptually after
   the decoded base DataList, parse the base, and replay every selected action
   to obtain a sparse effective DataList.
8. Resolve live file names and cached headers. Validate name termination and
   DataList references. Mark the header-declared DataList slot as internal.
9. For pathname lookup, canonicalize names and detect collisions. Physical
   enumeration does not require canonicalization and includes unnamed/internal
   records.
10. On payload access, return stored bytes directly for raw entries or inflate
    the single zlib stream for compressed entries. On verification, compare the
    four-byte logical MD5 prefix.

## Complete fresh-writer procedure

The following deterministic recipe produces a broadly compatible archive.

1. Validate every user entry. Logical and stored sizes must fit the live HOGG
   ranges, timestamps must fit `i32`, unnamed records must be raw and have no
   cached header, and opaque header data must not use the `0xfffe` tag.
2. Validate/canonicalize names for uniqueness and reject the reserved internal
   name. Preserve the desired stored spelling, including its final NUL.
3. For each logical payload, compute MD5. Optionally zlib-compress a nonempty
   named payload using an RFC 1950 stream. When an automatic compression policy
   does not make it smaller, store the raw bytes. For precompressed input,
   inflate once to validate exact termination, logical length, and checksum,
   while preserving the supplied stored bytes.
4. Assign user file slots in insertion order `0 .. user_count - 1`; assign the
   `?DataList` file slot `user_count`. Let
   `capacity = max(user_count + 1, 16)` for the recommended profile. Use
   equal file and EA capacities. Other capacity policies are valid; readers
   cannot depend on a particular amount of unused capacity.
5. Assign DataList IDs in user-entry order. A named entry receives a name ID;
   if it has a cached header, that header receives the immediately following
   ID. Unnamed entries receive neither. Finally assign one ID to the ten bytes
   `?DataList\0` and serialize the version-0 DataList.
6. Set `op_journal_size = 1024`, `dl_journal_size = 3096`, and zero both
   complete reservations. Set table byte sizes to `capacity * 32` and
   `capacity * 16`, and set `datalist_fileno = user_count`.
7. Set `payload_base = 4144 + capacity * 48`. Place user stored payloads
   consecutively from that offset without padding, followed by the raw
   DataList. Other payload placement is valid if every absolute offset is
   correct and lies after the metadata.
8. For every user file, write its assigned absolute payload offset, stored
   length, `i32` timestamp, logical MD5 prefix, and zero file-record padding.
   For a named entry at slot `i`, write a tagged reference to EA slot `i`, then
   write that EA's name ID, optional header ID, logical size when compressed
   (otherwise zero), and zero flags. For an unnamed entry, write its opaque
   eight-byte header data and mark EA slot `i` free.
9. Compute the DataList's MD5 prefix and write the `?DataList` file record with
   its absolute offset, stored length, zero timestamp, checksum, and tagged
   reference to EA slot `user_count`. Its EA names the final DataList ID, has
   cached-header ID `NO_VALUE`, has zero unpacked size for this raw recipe, and
   has zero flags.
10. For every remaining file slot, zero the record and set stored size to
    `NO_VALUE`. For every remaining EA slot, zero the record and set flags to
    `1`. Write zero to all defined padding/reserved bytes.
11. Write and flush a complete temporary archive before atomically publishing
    it. This avoids needing the journals during initial creation.

A writer may choose a different capacity, payload order, slack, or compressed
DataList. Those choices do not change any references or validation rules.

## In-place writer notes

A compatible editor may always rewrite a new archive using the fresh-writer
procedure. An editor that mutates in place must use both journals and the exact
transaction order above.

- Reuse any file slot whose stored size is `NO_VALUE` and any EA slot whose
  flags have bit 0 set. A deterministic writer can choose the lowest free slot
  and keep an existing named entry's file slot, EA slot, and name ID when
  replacing it.
- Append new payloads before publishing ADD or UPDATE. Replacing a named entry
  uses ADD because the action republishes its EA; a metadata-only change can
  use UPDATE.
- A deletion should free both its file slot and tagged EA slot. Corresponding
  DataList name/header IDs can be freed through the DataList journal or left as
  unreachable slots until compaction.
- To grow tables, first relocate every nonempty payload that overlaps the new
  metadata end with a MOVE transaction. Write the new EA table at its new
  location, then publish FILELIST_RESIZE. Growing both capacities by at least
  16 slots is a practical policy; the resize replay layout is fixed.
- When compacting/replacing the DataList, append and flush the new DataList
  payload, publish its file/EA record with ADD or UPDATE, then commit
  DATALISTFLUSH before reusing its journal area.

## Validation and error classification

A robust implementation should distinguish these outcomes:

- **Unsupported:** wrong magic/version for a requested HOGG v10 reader;
  nonzero DataList version; or a committed operation-journal action/ABI length
  outside the explicitly documented profiles.
- **Corrupt:** truncated regions; arithmetic overflow; table sizes not exact
  record multiples; invalid payload, file, EA, or DataList references; a live
  entry referencing a free EA/DataList slot; malformed name termination;
  malformed DataList actions; invalid or trailing zlib data; or decoded length
  disagreement.
- **Recovery required:** a known, valid, committed operation journal exists but
  the implementation opened the archive read-only and does not emulate replay.
- **Checksum mismatch:** structure and decoding succeeded but the computed
  logical MD5 prefix differs.
- **Implementation limit:** structurally valid counts, decoded bytes, or cached
  headers exceed configured memory/work limits. Such limits are not format
  corruption.

The writer procedures in this specification do not create overlapping
payloads, but there is no on-disk ownership table from which a reader can prove
overlap invalid. Readers need only enforce the metadata boundary and
containing-file ranges. Similarly, checksum verification is a separate
content-integrity operation rather than a prerequisite for metadata
enumeration.

## Minimal test vector

This vector contains one raw named entry, `a.txt`, whose payload is `abc` and
whose timestamp is zero. It follows the deterministic fresh-writer procedure
with capacity 16.

```text
file size       = 4947 (0x1353)
SHA-256         = 6f8169005630a0f7f9cc98349304925230937eb1840627b404d303fdca2e5156
table_base      = 0x1030
EA base         = 0x1230
payload_base    = 0x1330
DataList offset = 0x1333
```

The operation journal (`0x0018..0x0417`) and DataList journal
(`0x0418..0x102f`) are all zero. Defined nonzero regions are:

```text
0000: 0d f0 ad de 0a 00 00 04 00 02 00 00 00 01 00 00
0010: 01 00 00 00 18 0c 00 00

1030: 30 13 00 00 00 00 00 00 03 00 00 00 00 00 00 00
1040: 90 01 50 98 00 00 00 00 fe ff 00 00 00 00 00 00
1050: 33 13 00 00 00 00 00 00 20 00 00 00 00 00 00 00
1060: 74 b2 ac aa 00 00 00 00 fe ff 00 00 01 00 00 00

1230: 00 00 00 00 ff ff ff ff 00 00 00 00 00 00 00 00
1240: 01 00 00 00 ff ff ff ff 00 00 00 00 00 00 00 00

1330: 61 62 63
1333: 00 00 00 00 02 00 00 00 06 00 00 00 61 2e 74 78
1343: 74 00 0a 00 00 00 3f 44 61 74 61 4c 69 73 74 00
```

File slots 2 through 15 are otherwise zero with `ff ff ff ff` at record offset
8. EA slots 2 through 15 are otherwise zero with `01 00 00 00` at record
offset 12. The DataList is 32 bytes, has MD5
`74b2acaaf792640d1836feee8f54a3d1`, and encodes:

```text
version 0, count 2
slot 0: length 6,  "a.txt\0"
slot 1: length 10, "?DataList\0"
```

This description fixes every byte in the file and therefore permits an
independent writer to reproduce the stated SHA-256.

## Compatibility boundaries and unresolved details

This specification does not define other HOGG versions, journal layouts from
other compiler ABIs, a pathname character encoding, meanings for EA flag bits
above bit 0, or meanings for the two reserved/padding fields. Writers should
emit the documented zero values, and preserving editors should avoid changing
unknown bits or opaque bytes unnecessarily. The four-byte MD5 prefix is a
format compatibility field and is not collision-resistant authentication.
