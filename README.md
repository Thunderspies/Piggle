# Piggle

Piggle is a standalone implementation of the pseudo-filesystem, file watcher,
and archive manager used by the Cryptic engine. It combines loose directories,
PIGG v2 archives, and HOGG v10 archives into an ordered virtual file tree.

The library exists to make those storage formats usable through one explicit,
portable interface. It supports whole-file and streaming I/O, archive creation,
ordered overlays, and caller-polled change reporting through a C11 API with a
procedural C++11 interface.

## Quickstart

Piggle requires CMake 3.20 or newer, C11 and C++11 compilers, and a build tool
supported by CMake. CPM fetches the compression dependency during the first
configure.

From the repository root, configure and build Piggle:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```

To use Piggle from another CMake project, add it as a subdirectory and link the
exported target:

```cmake
add_subdirectory(path/to/piggle)
target_link_libraries(my_app PRIVATE piggle::piggle)
```

Include `<piggle/piggle.h>` from C or `<piggle/piggle.hpp>` from C++. A basic C
workflow opens one context and source, reads a named logical file, then releases
owned handles through their addresses:

```c
pg_context *context = NULL;
pg_source *source = NULL;
unsigned char bytes[4096];
size_t size = 0;

pg_context_open(&context, error);
pg_source_open(context, "assets.pigg", NULL, &source, error);
pg_source_read_all(source, "textures/icon.dds",
		bytes, sizeof(bytes), &size, error);
pg_source_close(&source, error);
pg_context_close(&context, error);
```

This excerpt shows the ownership sequence only. Applications should check every
returned `pg_status`, preserve the primary diagnostic while cleaning up, and use
`pg_source_read_all_alloc` with an explicit size limit when the content size is
unknown.

## Build and test

The default top-level build includes the command-line tool and tests. Run the
complete test suite from the repository root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Set `BUILD_SHARED_LIBS=ON` for a shared library,
`PIGGLE_BUILD_CLI=OFF` to omit the CLI, or `BUILD_TESTING=OFF` to omit tests.

## Documentation

- [API contract](docs/api.md)
- [API design rationale and influences](docs/design.md)
- [Testing](docs/testing.md)
- [PIGG v2 format](docs/pigg-v2-format.md)
- [HOGG v10 format](docs/hogg-v10-format.md)

See [CONTRIBUTING.md](CONTRIBUTING.md) for code style, commit guidelines, and
dependency policy.
