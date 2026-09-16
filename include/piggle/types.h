/* C11 ABI. Normative contracts: docs/api.md. */
#ifndef PIGGLE_TYPES_H
#define PIGGLE_TYPES_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(PIGGLE_SHARED)
#if defined(PIGGLE_BUILD)
#define PG_API __declspec(dllexport)
#else
#define PG_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define PG_API __attribute__((visibility("default")))
#else
#define PG_API
#endif
#if defined(_WIN32)
#define PG_CALL __cdecl
#else
#define PG_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed-width results. No exceptions or errno contract. See docs/api.md. */
typedef int32_t pg_status;
enum {
	PG_OK = 0, /* Success. */
	PG_END, /* Cursor exhaustion or verified reader EOF. */
	PG_INVALID, /* Invalid arguments, flags, descriptor or state. */
	PG_CAPACITY, /* Caller buffer too small; no output bytes changed. */
	PG_NOT_FOUND, /* Missing source, file or selected copy. */
	PG_NOT_ATTACHED, /* Source is not attached to the specified tree. */
	PG_EXISTS, /* Exclusive native creation or duplicate builder name. */
	PG_CONFLICT, /* Hierarchy, native alias, link or special object. */
	PG_NOMEM, /* Allocation failed; never a close-acceptance failure. */
	PG_LIMIT, /* Size, ID, count or generation cannot be represented. */
	PG_UNSUPPORTED, /* Format, profile or native facility unsupported. */
	PG_CORRUPT, /* Malformed structure or encoded payload. */
	PG_CHECKSUM, /* Available or expected logical digest mismatch. */
	PG_NO_CHECKSUM, /* Explicit verification has no stored digest. */
	PG_STALE, /* Captured physical identity/generation changed. */
	PG_RETRY, /* Could not obtain a stable external observation. */
	PG_RECOVERY_REQUIRED, /* Committed HOGG journal needs replay. */
	PG_IO, /* Native, durability or cleanup failure. */
	PG_READ_ONLY, /* Destination source was opened read-only. */
	PG_BUSY, /* Conflicting live reference or incorrect thread. */
	PG_REENTRANT, /* Callback attempted disallowed control. */
	PG_COMMITTED, /* Failure after all intended publications. */
	PG_INDETERMINATE, /* Mutation outcome cannot be established. */
	PG_PARTIAL /* Some files in a batch were published. */
};

/* Optional diagnostic output; reset on EVERY status-returning call. */
typedef struct pg_error {
	pg_status status; /* Always equals the call's return value. */
	pg_status cause; /* Underlying failure when status reports an effect. */
	int32_t native_code; /* Zero if absent. */
	uint64_t offset; /* UINT64_MAX if absent. */
	char message[192]; /* NUL-terminated; diagnostic, not a parsing API. */
} pg_error;

/* Owned binary result of read_all_alloc. Initialize to {NULL, 0} before use.
 * data is writable, size is the logical byte length, with no NUL terminator.
 * Empty success and every failure yield {NULL, 0}; status distinguishes them.
 * Never free/realloc data directly. Fields are unchanged except ownership
 * transfer: copy the pair and reset the original to empty; never duplicate it.
 * Release with pg_buffer_free (file.h). Lifetime is independent of context,
 * source or selection. Output slot borrows through return.
 */
typedef struct pg_buffer {
	void *data;
	size_t size;
} pg_buffer;

/* Owned opaque references. No global current source, tree or destination. */
typedef struct pg_context pg_context;
typedef struct pg_source pg_source;
typedef struct pg_tree pg_tree;
typedef struct pg_file pg_file;
typedef struct pg_cursor pg_cursor;
typedef struct pg_reader pg_reader;
typedef struct pg_writer pg_writer;
typedef struct pg_unpack_target pg_unpack_target;
/* Private archive construction; an existing readable archive is a source. */
typedef struct pg_archive_builder pg_archive_builder;
typedef uint64_t pg_id; /* Nonzero context-local identity; never reused. */

/* Detection accepts AUTO; explicit archive creation requires PIGG2/HOGG10. */
enum { PG_AUTO = 0, PG_PIGG2, PG_HOGG10, PG_LOOSE };
enum { PG_READ = 0, PG_WRITE }; /* Source access, not creation. */
enum { PG_COMPRESS_AUTO = 0, PG_COMPRESS_NEVER, PG_COMPRESS_FORCE };
enum { PG_LOGICAL = 0, PG_ZLIB }; /* Write input / stored representation. */
enum { PG_DIGEST_NONE = 0, PG_DIGEST_MD5, PG_DIGEST_MD5_32 };
enum { PG_READ_LOGICAL = 0, PG_READ_STORED }; /* Reader representation. */
/* Native creation is exclusive by default. Never overwrite links/specials. */
enum { PG_OVERWRITE = 1u };

/* Plain options have no size/version field. Zero-init sets valid defaults;
 * headers and library must match. NULL source options means AUTO/READ.
 */
typedef struct pg_source_options {
	uint32_t format;
	uint32_t access;
} pg_source_options;
typedef struct pg_source_info {
	pg_id id;
	uint64_t generation;
	const char *native_path; /* Borrowed until source close. */
	uint32_t format;
	uint32_t access;
} pg_source_info;
/* Immutable captured metadata. Spans live until the owned file is closed. */
typedef struct pg_file_info {
	pg_id source_id;
	pg_id copy_id;
	uint64_t source_generation;
	uint64_t copy_generation;
	const char *canonical_name;
	const char *original_name;
	uint64_t archive_record; /* UINT64_MAX for loose files. */
	uint64_t logical_size;
	uint64_t stored_size;
	int64_t mtime; /* Signed Unix seconds. */
	uint32_t encoding;
	uint32_t digest_kind;
	uint8_t digest[16]; /* Unused bytes are zero. */
	const void *cached_header; /* NULL when size is zero. */
	size_t cached_header_size;
} pg_file_info;

/* Metadata/policy shared by memory and streaming writes. Zero-init means
 * epoch mtime, AUTO compression, no expected digest/header/name override.
 * original_name, if provided, must normalize to the target virtual name.
 * Existing copies preserve original_name; an override there is INVALID.
 * cached_header may be NULL only at size=0; empty spans are never accessed.
 * Input spans copied before writer-open returns; empty headers are NULL/0.
 */
typedef struct pg_entry_options {
	int64_t mtime;
	uint32_t compression;
	uint32_t digest_kind;
	uint8_t expected_digest[16];
	const char *original_name;
	const void *cached_header;
	size_t cached_header_size;
} pg_entry_options;
/* Complete lengths required. LOGICAL requires input_size==logical_size.
 * ZLIB means one RFC 1950 stream with no trailing data. Use the initializer
 * for logical input; change fields explicitly for pre-encoded input.
 */
typedef struct pg_write_options {
	pg_entry_options entry;
	uint64_t logical_size;
	uint64_t input_size;
	uint32_t encoding;
} pg_write_options;
/* Immediate, no allocation/I/O/failure; out must point to writable storage.
 * Writes every field: default entry, LOGICAL, both lengths=logical_size.
 */
PG_API void PG_CALL pg_write_options_init(pg_write_options *out,
		uint64_t logical_size);
/* Pack customization. NULL -> AUTO compression, exclusive native output. */
typedef struct pg_pack_options {
	uint32_t compression;
	uint32_t flags; /* Zero or OVERWRITE only. */
} pg_pack_options;

/* Reader metadata: size counts bytes exposed by this representation.
 * Native readers are LOGICAL, with size==logical_size and native mtime.
 */
typedef struct pg_reader_info {
	uint64_t size;
	uint64_t logical_size;
	int64_t mtime;
	uint32_t encoding; /* LOGICAL in logical mode; else stored codec. */
} pg_reader_info;

#ifdef __cplusplus
}
#endif
#endif
