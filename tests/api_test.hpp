#ifndef PIGGLE_API_TEST_H
#define PIGGLE_API_TEST_H

#include "support.hpp"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define make_dir(path) _mkdir(path)
#else
#define make_dir(path) mkdir(path, 0777)
#endif

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

static const unsigned char payload[] = {0, 255, 'A', 'z', 0, 127, 128};

static inline int put_bytes(
	const char *path, const void *data, size_t size)
{
	FILE *stream = fopen(path, "wb");

	PG_CHECK(stream != NULL);
	PG_CHECK(!size || fwrite(data, 1, size, stream) == size);
	PG_CHECK(fclose(stream) == 0);
	return 0;
}

static inline int expect_bytes(
	const char *path, const void *data, size_t size)
{
	unsigned char bytes[512];
	FILE *stream = fopen(path, "rb");

	PG_CHECK(stream != NULL);
	PG_CHECK(size <= sizeof(bytes));
	PG_CHECK(fread(bytes, 1, sizeof(bytes), stream) == size);
	PG_CHECK(!size || memcmp(bytes, data, size) == 0);
	PG_CHECK(feof(stream) && !ferror(stream));
	PG_CHECK(fclose(stream) == 0);
	return 0;
}

static inline int expect_absent(const char *path)
{
	struct stat info;

	PG_CHECK(stat(path, &info) == -1 && errno == ENOENT);
	return 0;
}

static inline int clear_file(const char *path)
{
	PG_CHECK(remove(path) == 0 || errno == ENOENT);
	return 0;
}

static inline int directory(const char *path)
{
	PG_CHECK(make_dir(path) == 0 || errno == EEXIST);
	return 0;
}

struct fixture {
	pg_context *context;
	pg_source *source;
	pg_tree *tree;
	pg_file *file;
	pg_cursor *cursor;
	pg_archive_builder *builder;
};

static inline int fixture_open(struct fixture *f)
{
	pg_source_options options = {PG_LOOSE, PG_WRITE, 0};
	pg_error error;

	memset(f, 0, sizeof(*f));
	PG_CHECK(directory("root") == 0);
	PG_CHECK(directory("out") == 0);
	PG_CHECK(put_bytes("root/a", payload, sizeof(payload)) == 0);
	PG_CHECK(put_bytes("root/empty", NULL, 0) == 0);
	PG_CHECK(put_bytes("input", payload, sizeof(payload)) == 0);
	PG_CHECK(clear_file("built") == 0);
	PG_STATUS(pg_context_open(&f->context, &error), PG_OK);
	PG_STATUS(pg_source_open(f->context, "root", &options,
				&f->source, &error),
		PG_OK);
	PG_STATUS(pg_tree_create(f->context, &f->tree, &error), PG_OK);
	PG_STATUS(pg_tree_attach(f->tree, f->source, &error), PG_OK);
	PG_STATUS(pg_source_find(f->source, "a", &f->file, &error),
		PG_OK);
	PG_STATUS(pg_source_request_subtree(f->source, NULL, &error),
		PG_OK);
	PG_STATUS(
		pg_tree_request_subtree(f->tree, NULL, &error), PG_OK);
	PG_STATUS(pg_source_files(f->source, NULL, &f->cursor, &error),
		PG_OK);
	PG_STATUS(pg_archive_builder_create(f->context, "built",
				PG_PIGG2, 0, &f->builder, &error),
		PG_OK);
	return 0;
}

static inline int fixture_close(struct fixture *f)
{
	pg_error error;

	PG_STATUS(pg_archive_builder_close(&f->builder, &error), PG_OK);
	PG_STATUS(pg_cursor_close(&f->cursor, &error), PG_OK);
	PG_STATUS(pg_file_close(&f->file, &error), PG_OK);
	PG_STATUS(pg_tree_close(&f->tree, &error), PG_OK);
	PG_STATUS(pg_source_close(&f->source, &error), PG_OK);
	PG_STATUS(pg_context_close(&f->context, &error), PG_OK);
	return 0;
}

#endif
