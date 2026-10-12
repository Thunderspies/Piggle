#include <piggle/piggle.h>
#include "../../src/internal.hpp"
#include <chrono>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <psapi.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

static uint64_t allocations, allocated_bytes, archive_reads;
static int measuring;

#ifdef PIGGLE_MEASURE_ALLOCATIONS
extern "C" void *__real_malloc(size_t size);
extern "C" void *__real_calloc(size_t count, size_t size);
extern "C" void *__real_realloc(void *memory, size_t size);
extern "C" ssize_t __real_pread64(int fd, void *bytes, size_t size, off64_t at);

extern "C" void *__wrap_malloc(size_t size)
{
	void *memory = __real_malloc(size);

	if (memory && measuring) {
		allocations++;
		allocated_bytes += size;
	}
	return memory;
}

extern "C" void *__wrap_calloc(size_t count, size_t size)
{
	void *memory = __real_calloc(count, size);

	if (memory && measuring) {
		allocations++;
		allocated_bytes += count * size;
	}
	return memory;
}

extern "C" void *__wrap_realloc(void *memory, size_t size)
{
	void *result = __real_realloc(memory, size);

	if (result && measuring) {
		allocations++;
		allocated_bytes += size;
	}
	return result;
}

extern "C" ssize_t __wrap_pread64(int fd, void *bytes, size_t size, off64_t at)
{
	if (measuring)
		archive_reads++;
	return __real_pread64(fd, bytes, size, at);
}
#endif

static uint64_t peak_memory()
{
#ifdef _WIN32
	PROCESS_MEMORY_COUNTERS memory = {};

	return GetProcessMemoryInfo(
		GetCurrentProcess(), &memory, sizeof(memory))
		? (uint64_t)memory.PeakWorkingSetSize
		: 0;
#else
	struct rusage memory;

	return getrusage(RUSAGE_SELF, &memory)
		? 0
		: (uint64_t)memory.ru_maxrss * 1024;
#endif
}

static int check(pg_status status, const pg_error *error)
{
	if (status == PG_OK)
		return 0;
	fprintf(stderr, "status=%d cause=%d native=%d\n", status, error->cause,
		error->native_code);
	return 1;
}

int main(int argc, char **argv)
{
	if (argc < 3 || argc > 4)
		return 2;
	uint64_t count = strtoull(argv[1], NULL, 10);
	int reserve = argc == 4 && !strcmp(argv[3], "reserve");
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_source *source = NULL;
	pg_error error;
	pg_entry_options entry = {};

	entry.compression = PG_COMPRESS_NEVER;
	if (!count || count > 1000000 ||
		check(pg_context_open(&context, &error), &error))
		return 1;
#ifdef PIGGLE_BASELINE
	if (check(pg_archive_builder_create(
			context, argv[2], PG_HOGG10, 0, &builder, &error),
		&error))
		return 1;
	(void)reserve;
#else
	pg_archive_build_options config = {
		{PG_HOGG10, 0, 0}, NULL, reserve ? count : 0};

	if (check(pg_archive_builder_create_config(
			context, argv[2], &config, &builder, &error),
		&error))
		return 1;
#endif
	auto start = std::chrono::steady_clock::now();
	measuring = 1;
	for (uint64_t i = 0; i < count; i++) {
		char name[40];
		snprintf(name, sizeof(name), "entry/%09llu",
			(unsigned long long)i);
		if (check(pg_archive_builder_write_all(
				builder, name, "x", 1, &entry, &error),
			&error))
			return 1;
	}
	auto staged = std::chrono::steady_clock::now();
	if (check(pg_archive_builder_finish(builder, &error), &error))
		return 1;
	auto finished = std::chrono::steady_clock::now();
	measuring = 0;
#ifdef PIGGLE_BASELINE
	uint64_t comparisons = count * (count - 1) / 2;
	uint64_t copies = count * 2, growth = 0, relocated = 0;
#else
	uint64_t comparisons = builder->name_comparisons;
	uint64_t copies = builder->payload_copied_bytes;
	uint64_t growth = builder->growth_events,
		relocated = builder->relocated_bytes;
	if (comparisons > count * 42)
		return 1;
#endif
	struct stat physical;
	if (stat(argv[2], &physical))
		return 1;
	printf("builder entries=%llu reserve=%d stage_seconds=%.6f "
	"finish_seconds=%.6f "
	"comparisons=%llu allocations=%llu allocated_bytes=%llu "
	"peak_bytes=%llu "
	"payload_copied_bytes=%llu temporary_bytes=%llu "
	"growth_events=%llu "
	"relocated_bytes=%llu archive_reads=%llu\n",
		(unsigned long long)count, reserve,
		std::chrono::duration<double>(staged - start).count(),
		std::chrono::duration<double>(finished - staged).count(),
		(unsigned long long)comparisons,
		(unsigned long long)allocations,
		(unsigned long long)allocated_bytes,
		(unsigned long long)peak_memory(), (unsigned long long)copies,
		(unsigned long long)physical.st_size,
		(unsigned long long)growth, (unsigned long long)relocated,
		(unsigned long long)archive_reads);
	if (check(pg_archive_builder_close(&builder, &error), &error))
		return 1;
	pg_source_options writable = {PG_HOGG10, PG_WRITE, 0};

	if (check(pg_source_open(context, argv[2], &writable, &source, &error),
		&error))
		return 1;
	allocations = allocated_bytes = archive_reads = 0;
	measuring = 1;
	auto mutation_start = std::chrono::steady_clock::now();
	if (check(pg_source_write_all(
			source, "entry/000000000", "y", 1, &entry, &error),
		&error))
		return 1;
	auto mutation_end = std::chrono::steady_clock::now();
	measuring = 0;
#ifndef PIGGLE_BASELINE
	allocations = allocated_bytes = archive_reads = 0;
	measuring = 1;
	mutation_start = std::chrono::steady_clock::now();
	for (unsigned i = 0; i < 100; i++)
		if (check(pg_source_write_all(source, "entry/000000000", "z", 1,
				&entry, &error),
			&error))
			return 1;
	mutation_end = std::chrono::steady_clock::now();
	measuring = 0;
	if (source->full_reconciliations || archive_reads > 3000)
		return 1;
#endif
	printf("mutation entries=%llu replacements=%u seconds=%.6f "
	"archive_reads=%llu "
	"allocations=%llu allocated_bytes=%llu peak_bytes=%llu\n",
		(unsigned long long)count,
#ifdef PIGGLE_BASELINE
		1u,
#else
		100u,
#endif
		std::chrono::duration<double>(mutation_end - mutation_start)
			.count(),
		(unsigned long long)archive_reads,
		(unsigned long long)allocations,
		(unsigned long long)allocated_bytes,
		(unsigned long long)peak_memory());
	if (check(pg_source_close(&source, &error), &error) ||
		check(pg_context_close(&context, &error), &error))
		return 1;
	return remove(argv[2]) ? 1 : 0;
}
