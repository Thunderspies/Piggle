#include <piggle/piggle.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <wchar.h>
#endif

static void usage(FILE *stream)
{
	fprintf(stream,
		"Usage:\n"
		"  piggle list ARCHIVE [PREFIX]\n"
		"  piggle create FORMAT ARCHIVE [NAME INPUT]...\n"
		"  piggle extract ARCHIVE DIRECTORY [--overwrite]"
		" [--file NAME]...\n"
		"  piggle replace ARCHIVE NAME INPUT\n"
		"  piggle verify ARCHIVE\n"
		"  piggle --version\n"
		"FORMAT is pigg2 or hogg10. DIRECTORY must exist.\n"
		"Without --file, extract exports every visible file.\n");
}

static int extract_options(int argc, char **argv, uint32_t *flags,
		int *file_count)
{
	*flags = 0;
	*file_count = 0;
	for (int i = 4; i < argc; ++i) {
		if (!strcmp(argv[i], "--overwrite")) {
			if (*flags)
				return 0;
			*flags = PG_OVERWRITE;
		} else if (!strcmp(argv[i], "--file")) {
			if (++i == argc || !argv[i][0] ||
			    !strcmp(argv[i], "--file") ||
			    !strcmp(argv[i], "--overwrite"))
				return 0;
			++*file_count;
		} else {
			return 0;
		}
	}
	return 1;
}

static pg_status extract_file(pg_context *context, pg_source *source,
		const char *directory, const char *name, uint32_t flags,
		pg_error *error)
{
	size_t required = 0;
	pg_status status = pg_name_normalize(context, name, NULL, 0,
		&required, error);
	if (status != PG_CAPACITY)
		return status;
	if (strlen(directory) > SIZE_MAX - 2 ||
	    required > SIZE_MAX - strlen(directory) - 2) {
		memset(error, 0, sizeof(*error));
		error->status = PG_LIMIT;
		error->cause = PG_LIMIT;
		error->offset = UINT64_MAX;
		strcpy(error->message, "output path is too long");
		return PG_LIMIT;
	}
	char *path = (char *)malloc(strlen(directory) + required + 1);
	if (!path) {
		memset(error, 0, sizeof(*error));
		error->status = PG_NOMEM;
		error->cause = PG_NOMEM;
		error->offset = UINT64_MAX;
		strcpy(error->message, "allocating output path failed");
		return PG_NOMEM;
	}
	char *canonical = path + strlen(directory) + 1;
	status = pg_name_normalize(context, name, canonical, required,
		&required, error);
	if (status == PG_OK) {
		size_t length = strlen(directory);
		memcpy(path, directory, length);
		path[length] = '/';
		status = pg_source_export(source, canonical, path,
			flags, error);
	}
	free(path);
	return status;
}

static pg_status extract_files(pg_context *context, pg_source *source,
		int argc, char **argv, uint32_t flags, int file_count,
		pg_error *error)
{
	if (!file_count) {
		return pg_source_unpack(source, argv[3], flags, error);
	}
	for (int i = 4; i < argc; ++i) {
		if (strcmp(argv[i], "--file"))
			continue;
		pg_status status = extract_file(context, source, argv[3],
			argv[++i], flags, error);
		if (status != PG_OK)
			return status;
	}
	return PG_OK;
}

static void report(const char *action, pg_status status,
		const pg_error *error)
{
	if (error->status == status &&
	    error->cause != status) {
		fprintf(stderr,
			"piggle: %s: status %d (cause %d)\n",
			action, (int)status, (int)error->cause);
		return;
	}
	if (error->status == status && error->message[0])
		fprintf(stderr, "piggle: %s: %s (status %d)\n",
			action, error->message, (int)status);
	else
		fprintf(stderr, "piggle: %s: status %d\n", action,
			(int)status);
}

static pg_status list_files(pg_source *source, const char *prefix,
		pg_error *error)
{
	pg_cursor *cursor = NULL;
	pg_status status = pg_source_files(source, prefix, &cursor, error);
	if (status != PG_OK)
		return status;

	for (;;) {
		pg_file *file = NULL;
		pg_file_info info;
		status = pg_cursor_next(cursor, &file, error);
		if (status == PG_END) {
			status = PG_OK;
			break;
		}
		if (status != PG_OK)
			break;
		status = pg_file_inspect(file, &info, error);
		if (status == PG_OK &&
			printf("%s\n", info.canonical_name) < 0) {
			status = PG_IO;
			fprintf(stderr, "piggle: writing listing failed\n");
		}
		pg_error close_error;
		pg_status close_status = pg_file_close(&file, &close_error);
		if (status == PG_OK && close_status != PG_OK) {
			status = close_status;
			*error = close_error;
		}
		if (status != PG_OK)
			break;
	}
	pg_error close_error;
	pg_status close_status = pg_cursor_close(&cursor, &close_error);
	if (status == PG_OK && close_status != PG_OK) {
		status = close_status;
		*error = close_error;
	}
	return status;
}

static pg_status create_archive(pg_context *context, int argc,
		char **argv, pg_error *error)
{
	uint32_t format;
	if (!strcmp(argv[2], "pigg2"))
		format = PG_PIGG2;
	else
		format = PG_HOGG10;

	pg_archive_builder *builder = NULL;
	pg_status status = pg_archive_builder_create(context, argv[3],
		format, 0, &builder, error);
	if (status != PG_OK)
		return status;
	for (int i = 4; i < argc; i += 2) {
		status = pg_archive_builder_import(builder, argv[i],
			argv[i + 1], PG_COMPRESS_AUTO, error);
		if (status != PG_OK)
			break;
	}
	if (status == PG_OK) {
		status = pg_archive_builder_finish(builder, error);
	}
	pg_error close_error;
	pg_status close_status = pg_archive_builder_close(&builder,
		&close_error);
	if (status == PG_OK && close_status != PG_OK) {
		status = close_status;
		*error = close_error;
	}
	return status;
}

static int run(int argc, char **argv)
{
	uint32_t extract_flags = 0;
	int extract_count = 0;
	if (argc == 2 && (!strcmp(argv[1], "--help") ||
		!strcmp(argv[1], "help"))) {
		usage(stdout);
		return EXIT_SUCCESS;
	}
	if (argc == 2 && !strcmp(argv[1], "--version")) {
		printf("piggle %s\n", PIGGLE_VERSION);
		return EXIT_SUCCESS;
	}
	if (argc < 2 ||
		(!strcmp(argv[1], "list") && (argc < 3 || argc > 4)) ||
		(!strcmp(argv[1], "create") &&
			(argc < 4 || ((argc - 4) % 2) != 0 ||
			(strcmp(argv[2], "pigg2") &&
			 strcmp(argv[2], "hogg10")))) ||
		(!strcmp(argv[1], "extract") &&
			(argc < 4 || !extract_options(argc, argv,
				&extract_flags, &extract_count))) ||
		(!strcmp(argv[1], "replace") && argc != 5) ||
		(!strcmp(argv[1], "verify") && argc != 3) ||
		(strcmp(argv[1], "list") &&
		 strcmp(argv[1], "create") &&
		 strcmp(argv[1], "extract") &&
		 strcmp(argv[1], "replace") &&
		 strcmp(argv[1], "verify"))) {
		usage(stderr);
		return 2;
	}

	pg_context *context = NULL;
	pg_source *source = NULL;
	pg_error error;
	pg_status status = pg_context_open(&context, &error);
	if (status != PG_OK) {
		report("opening context", status, &error);
		return EXIT_FAILURE;
	}
	if (!strcmp(argv[1], "create")) {
		status = create_archive(context, argc, argv, &error);
	} else {
		pg_source_options options = { PG_AUTO, PG_READ,
			PG_CHECKSUM_LOGICAL };
		if (!strcmp(argv[1], "replace"))
			options.access = PG_WRITE;
		status = pg_source_open(context, argv[2], &options,
			&source, &error);
		if (status == PG_OK && !strcmp(argv[1], "list"))
			status = list_files(source, argc == 4 ? argv[3] : NULL,
				&error);
		else if (status == PG_OK && !strcmp(argv[1], "extract")) {
			status = extract_files(context, source, argc, argv,
				extract_flags, extract_count, &error);
		} else if (status == PG_OK && !strcmp(argv[1], "replace")) {
			status = pg_source_import(source, argv[3], argv[4],
				PG_COMPRESS_AUTO, &error);
		} else if (status == PG_OK) {
			status = pg_source_validate(source, &error);
		}
	}
	if (status != PG_OK)
		report(argv[1], status, &error);
	pg_error close_error;
	pg_status close_status = pg_source_close(&source, &close_error);
	if (close_status != PG_OK) {
		report("closing source", close_status, &close_error);
		if (status == PG_OK)
			status = close_status;
	}
	close_status = pg_context_close(&context, &close_error);
	if (close_status != PG_OK) {
		report("closing context", close_status, &close_error);
		if (status == PG_OK)
			status = close_status;
	}
	return status == PG_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}

#if defined(_WIN32)
int wmain(int argc, wchar_t **wide_argv)
{
	char **argv = (char **)calloc((size_t)argc, sizeof(*argv));
	if (!argv)
		return EXIT_FAILURE;
	for (int i = 0; i < argc; ++i) {
		int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
			wide_argv[i], -1, NULL, 0, NULL, NULL);
		if (size <= 0 || !(argv[i] = (char *)malloc((size_t)size)) ||
			!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
				wide_argv[i], -1, argv[i], size, NULL, NULL)) {
			fprintf(stderr, "piggle: invalid command argument\n");
			for (int j = 0; j <= i; ++j)
				free(argv[j]);
			free(argv);
			return EXIT_FAILURE;
		}
	}
	int result = run(argc, argv);
	for (int i = 0; i < argc; ++i)
		free(argv[i]);
	free(argv);
	return result;
}
#else
int main(int argc, char **argv)
{
	return run(argc, argv);
}
#endif
