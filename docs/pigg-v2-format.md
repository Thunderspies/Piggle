# PIGG version 2 archive format

This document specifies the PIGG v2 wire format. It is intended to be
sufficient to build an independent reader and writer. Unless a rule is marked
**canonical writer**, it describes compatible reader behavior. Descriptive
field names in this document do not imply that those were the names or
meanings used by the original format authors.

PIGG v2 is a little-endian, unaligned archive. It consists of an archive
header, a fixed-stride file table, a pathname pool, a cached-header pool, and
file payloads.

```text
0
+-----------------------------+
| archive header              | archive_stride bytes
+-----------------------------+
| file record 0               | file_stride bytes
| ...                         |
| file record file_count - 1  |
+-----------------------------+  pathname_pool_offset
| pathname pool header        | 12 bytes
| length + pathname           | repeated pathname_count times
+-----------------------------+  cached_header_pool_offset
| cached-header pool header   | 12 bytes
| length + cached header      | repeated header_count times
+-----------------------------+  metadata_end / canonical payload_base
| stored payload bytes        | canonical writer: in file-table order
+-----------------------------+
```

There is no required alignment or padding between any sections or payloads.
All multibyte integers are unsigned little-endian integers unless stated
otherwise. Offsets are absolute from the beginning of the archive.

## Archive header

The reader first needs 16 bytes. The declared `archive_stride` can extend the
header; an extensible reader must ignore bytes from offset 16 through
`archive_stride - 1`.

| Offset | Size | Field | Required value or meaning |
|---:|---:|---|---|
| 0 | 4 | `magic` | `0x00000123` (bytes `23 01 00 00`) |
| 4 | 2 | `version_a` | `2` |
| 6 | 2 | `version_b` | `2` |
| 8 | 2 | `archive_stride` | At least 16; canonical value 16 |
| 10 | 2 | `file_stride` | At least 48; canonical value 48 |
| 12 | 4 | `file_count` | Number of physical file records |
| 16 | `archive_stride - 16` | extension | Ignored by the reader; canonical writer emits none |

Both version fields are exact format/version discriminators. Their historical
names and separate meanings are undocumented, so `version_a` and `version_b`
are neutral names.

The file table begins at `archive_stride`, contains `file_count` records, and
ends at:

```text
pathname_pool_offset = archive_stride + file_count * file_stride
```

Perform this and every later offset/size calculation with checked arithmetic.

## File records

Record `i` begins at `archive_stride + i * file_stride`. The first 48 bytes
have this layout. If `file_stride` exceeds 48, the reader ignores the trailing
`file_stride - 48` bytes in every record.

| Record offset | Size | Field | Meaning |
|---:|---:|---|---|
| 0 | 4 | `record_magic` | Must be `0x00003456` (bytes `56 34 00 00`) |
| 4 | 4 | `pathname_id` | Zero-based pathname-pool element index |
| 8 | 4 | `logical_size` | Size after decompression |
| 12 | 4 | `mtime_bits` | Timestamp bits; see below |
| 16 | 4 | `payload_offset` | Absolute file offset of stored payload |
| 20 | 4 | unknown/reserved | Ignored by the reader; canonical writer writes zero |
| 24 | 4 | `cached_header_id` | Pool index, or `0xffffffff` for no cached header |
| 28 | 16 | `logical_md5` | MD5 of the logical, decompressed payload |
| 44 | 4 | `compressed_size` | Compression discriminator and stored byte count |
| 48 | `file_stride - 48` | extension | Ignored by the reader; canonical writer emits none |

`pathname_id` must be less than the pathname pool's count.
`cached_header_id` must either be `0xffffffff` or be less than the cached-header
pool's count.

Do not reinterpret the reserved word at offset 20 as the high half of the
payload offset. The implemented reader ignores that word and uses only the
32-bit field at offset 16. The legacy declarations summarized in the design
notes describe the pool IDs as signed; their wire representation is still the
32-bit bit pattern shown here, including `-1`/`0xffffffff` for an absent cached
header. There is no absent-pathname sentinel.

### Stored-size sentinel

The `compressed_size` field alone selects the representation:

| `compressed_size` | Representation | Stored byte count |
|---:|---|---:|
| 0 | Uncompressed | `logical_size` |
| Nonzero | RFC 1950 zlib stream | `compressed_size` |

Thus zero is a sentinel for “uncompressed,” not a stored length. In particular,
an uncompressed nonempty file still has `compressed_size == 0`, and an empty
file is represented with both sizes zero. The format cannot mark a zero-byte
stored representation as compressed.

For either representation, the range
`[payload_offset, payload_offset + stored_size)` must be within the archive and
`payload_offset` must be at least `metadata_end`. Compatible readers do not
require payload ranges to be ordered, disjoint, or to cover the remainder of
the archive. It also accepts unreferenced trailing archive bytes. A canonical
writer packs payloads contiguously in record order.

### Timestamp

The wire field is 32 raw bits. The reference writer accepts timestamps from 0
through `UINT32_MAX` and writes the low 32 bits. The reference reader casts the
word to a signed 32-bit integer before exposing it as a 64-bit timestamp. Values
from `0x80000000` through `0xffffffff` therefore read back as negative values.
This read/write asymmetry is part of the compatibility behavior. An independent
lossless implementation should retain the raw 32-bit word.

### Digest

`logical_md5` is the ordinary 16-byte MD5 digest of the entire logical
payload, stored in standard MD5 byte order. For compressed entries it covers
the decompressed bytes, not the zlib stream.

MD5 here is legacy corruption-detection data. It does not authenticate an
archive against deliberate modification.

The canonical writer stores 16 zero bytes for an empty file instead of the MD5
of the empty string (`d41d8cd98f00b204e9800998ecf8427e`). Readers must accept
that zero digest as valid for an empty PIGG entry. A zero digest on a nonempty
file means that no checksum is available for verification, although it remains
structurally valid metadata.

## Data pools

Both pools use the same framing. The pathname pool immediately follows the
file table, and the cached-header pool immediately follows the complete
pathname pool. Neither pool is aligned.

### Pool header

| Pool offset | Size | Field | Meaning |
|---:|---:|---|---|
| 0 | 4 | `pool_magic` | `0x00006789` for pathnames; `0x00009abc` for cached headers |
| 4 | 4 | `element_count` | Number of elements |
| 8 | 4 | `elements_bytes` | Total bytes occupied by all framed elements, excluding this 12-byte header |

The pool occupies exactly `12 + elements_bytes` bytes. Its elements must
consume exactly `elements_bytes`; unused tail bytes inside a pool are invalid.
As an early sanity check, `element_count <= elements_bytes / 4`.

### Pool element

| Element offset | Size | Field | Meaning |
|---:|---:|---|---|
| 0 | 4 | `byte_count` | Number of following data bytes |
| 4 | `byte_count` | `data` | Element bytes |

There is no terminator or padding supplied by the framing itself.

Pathname elements must have `byte_count >= 1`, must end in exactly one NUL
byte included in `byte_count`, and must contain no earlier NUL. The bytes before
the terminator are the archive pathname. The reader does not validate UTF-8;
bytes at or above `0x80` are opaque.

Cached-header elements are arbitrary byte strings. They may contain NULs. The
reader even accepts a zero-length cached-header element, though the canonical
writer never emits one and represents a missing or empty header with
`cached_header_id == 0xffffffff`.

The canonical writer emits exactly one pathname element per file record, in
record order, so `pathname_id == record index`. It emits only nonempty cached
headers, also in record order, and assigns their dense zero-based indices to
the corresponding records.

## Pathname semantics

The stored pathname is the display spelling. Lookup uses a separately derived
canonical key; no pathname hash or lookup index is stored in a PIGG archive.

Compatible implementations apply these rules when reading:

1. Reject an empty pathname, a leading `/` or `\`, and an ASCII drive prefix
   of the form `A:` through `Z:` (case-insensitive).
2. Reject a final `/` or `\` because every PIGG record denotes a file.
3. Treat both `/` and `\` as component separators.
4. Collapse repeated separators and remove `.` components.
5. Reject every `..` component.
6. Join the remaining components with `/`.
7. Under ASCII-fold lookup, map only bytes `A` through `Z` to `a` through `z`.
   Under exact lookup, preserve every byte.
8. Reject an archive if two records produce the same canonical key under the
   selected lookup policy.

The canonical writer performs steps 1–6 before storage, stores `/` separators,
and preserves letter case and all other component bytes. It separately applies
step 7 to detect duplicate names. Consequently, an input such as
`Dir\\./File` is stored as `Dir/File`; `../File`, `/File`, `C:\\File`, and a
path that normalizes to empty are rejected.

Directory entries are not represented on the PIGG wire. Higher-level readers
may synthesize directories from pathname components.

## Compression and payload validation

Compressed payloads are complete RFC 1950 zlib streams. The canonical
writer uses zlib's default compression level and default wrapper/window
settings. Exact compressed bytes are therefore not a portable canonical form:
different compatible zlib implementations or versions may produce a different
valid stream for the same data.

A strict logical reader must require all of the following:

- the decompressor reaches its end-of-stream marker;
- it consumes exactly `compressed_size` input bytes, with no bytes following
  the stream inside that payload range;
- it produces exactly `logical_size` bytes; and
- it never exposes more than `logical_size` bytes to the caller. A private
  guard byte may detect excess decoder output without returning that byte.

Metadata parsing alone does not inflate payloads or verify MD5. A reader reports
bad deflate data, a wrong decoded length, truncation, or trailing compressed
bytes only when the logical payload is read through EOF or explicitly
verified. MD5 mismatch is likewise a payload-verification failure, not a
metadata-structure failure.

For writing logical input, the canonical writer computes MD5 while reading the
logical bytes. Its compression choices are:

- **never:** store the original bytes and write `compressed_size = 0`;
- **force:** zlib-compress every nonempty input and write its nonzero size;
- **auto:** zlib-compress a nonempty input, but use the original bytes and the
  zero sentinel when the compressed result is not smaller.

For already stored compressed input, it validates the zlib stream and decoded
length, computes MD5 over the decoded data, then preserves the supplied zlib
bytes. Empty input is always written uncompressed.

## Canonical write procedure

Given `N` entries in insertion order:

1. Validate every field fits its 32-bit wire field. For broad compatibility,
   require `logical_size <= UINT32_MAX`, stored/compressed size
   `<= UINT32_MAX`, cached-header size `<= UINT32_MAX`, and timestamp in
   `0..UINT32_MAX`.
2. Normalize each pathname as above and reject canonical duplicates under the
   chosen name policy.
3. Consume and, if requested, compress each payload. Verify declared lengths,
   zlib termination for precompressed input, and any caller-supplied MD5.
   Compute the logical MD5, using the all-zero empty-file convention.
4. Let `path_len[i]` include the terminating NUL. Compute:

   ```text
   names_bytes   = sum(4 + path_len[i]) for i = 0..N-1
   headers_bytes = sum(4 + header_len[i]) for entries with header_len[i] > 0

   pathname_pool_offset = 16 + 48*N
   header_pool_offset   = pathname_pool_offset + 12 + names_bytes
   payload_base         = header_pool_offset + 12 + headers_bytes

   stored_len[i] = compressed_len[i] if compressed, else logical_size[i]
   payload_offset[i] = payload_base + sum(stored_len[j]) for j < i
   archive_size = payload_base + sum(stored_len[i])
   ```

   Reject any value that does not fit its wire field. In particular,
   `payload_base`, every `payload_offset`, and the final archive size must fit
   the format's 32-bit offset limit.
5. Write the 16-byte canonical archive header and `N` 48-byte file records.
   Use record-order pathname indices and dense header indices. Zero the record
   word at offset 20.
6. Write the pathname pool header and its `u32 length + bytes` elements.
7. Write the cached-header pool header and its nonempty elements.
8. Write stored payloads contiguously in record order. No padding is inserted.

A writer may use larger header/record strides for extensions, but it must set
the declared strides and ensure baseline readers can ignore every added byte.
No semantics are documented for such extension bytes.

PIGG has no documented in-place mutation protocol. Replacing, adding, or
deleting entries is done by constructing and publishing a complete new archive.

## Reader procedure and validation order

An independent safe reader can use this sequence:

1. Require at least 16 bytes and decode the fixed archive header.
2. Check `magic`, both version words, `archive_stride >= 16`, and
   `file_stride >= 48`. Unsupported magic/version is a format/version error;
   an invalid stride is structural corruption.
3. Compute the file-table range with checked arithmetic and require room for
   the 12-byte pathname-pool header after it.
4. Parse the pathname pool header. Check its magic, range, count sanity, exact
   element exhaustion, pathname termination, and lack of embedded NULs.
5. Parse the immediately following cached-header pool in the same way, using
   its own magic. Its end is `metadata_end`.
6. For every fixed-stride record, check its record magic and pool indices.
   Derive `stored_size` from the compression sentinel. Require the payload
   range to be inside the archive and at or after `metadata_end`.
7. Validate and canonicalize each pathname, then reject duplicate canonical
   keys under the reader's selected exact or ASCII-fold policy.
8. Expose metadata only after all structural checks succeed. Preserve unknown
   extension bytes only if the application needs byte-for-byte rewriting;
   they have no known meaning here.
9. On logical payload access, copy `logical_size` bytes for an uncompressed
   entry or perform the strict zlib checks above. On verification, compare MD5
   over the logical bytes, with the empty/all-zero exception.

Implementations should additionally apply configurable resource limits to
entry count, metadata allocation, cached-header bytes, and inflated bytes.
Those are safeguards rather than wire-format constants.

## Deterministic test vector

This canonical archive contains one uncompressed file:

```text
path          = "a"
logical bytes = 78                         # ASCII "x"
mtime_bits    = 00000000
cached header = absent
MD5("x")      = 9dd4e461268c8034f5c8564e155c67a6
```

Its pathname pool has one 2-byte string (`61 00`) framed by a 4-byte length,
so `names_bytes = 6`. The cached-header pool is empty. Therefore
`payload_base = 16 + 48 + 12 + 6 + 12 = 94 (0x5e)`, and the complete archive
is 95 bytes.

```text
00000000  23 01 00 00 02 00 02 00 10 00 30 00 01 00 00 00
00000010  56 34 00 00 00 00 00 00 01 00 00 00 00 00 00 00
00000020  5e 00 00 00 00 00 00 00 ff ff ff ff 9d d4 e4 61
00000030  26 8c 80 34 f5 c8 56 4e 15 5c 67 a6 00 00 00 00
00000040  89 67 00 00 01 00 00 00 06 00 00 00 02 00 00 00
00000050  61 00 bc 9a 00 00 00 00 00 00 00 00 00 00 78
```

Useful assertions are:

```text
file_count                         == 1
record[0].pathname_id              == 0
record[0].logical_size             == 1
record[0].payload_offset           == 94
record[0].cached_header_id         == 0xffffffff
record[0].compressed_size          == 0
pathname[0]                        == "a"
stored bytes at [94, 95)           == 78
MD5(logical bytes at [94, 95))     == record[0].logical_md5
```

## Known unknowns and compatibility boundaries

- The separate historical meanings of the two 16-bit value-2 version fields,
  the file-record word at offset 20, and any stride-extension bytes are unknown.
- The reader deliberately ignores extension bytes and accepts noncanonical
  payload placement, zero-length cached-header elements, and trailing data.
  Writers seeking broad interoperability should emit the compact canonical
  layout described above.
- PIGG contains no global checksum, central pathname index, explicit directory
  records, encoding declaration, archive ID, or generation number.
