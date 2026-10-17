# Piggle

Piggle is a standalone implementation of the pseudo-filesystem, file watcher,
and archive manager used by the Cryptic engine. It combines loose directories,
PIGG v2 archives, and HOGG v10 archives into an ordered virtual file tree.

The library exists to make those storage formats usable through one explicit,
portable interface. It supports whole-file and streaming I/O, archive creation,
ordered overlays, and caller-polled change reporting through a C11 API with a
procedural C++17 interface.

## Quickstart

Piggle requires CMake 3.24 or newer, C11 and C++17 compilers, and a build tool
supported by CMake. CPM fetches the compression dependency during the first
configure.

From the repository root, configure and build Piggle:

```sh
cmake --preset default
cmake --build --preset default --parallel
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
cmake --preset default
cmake --build --preset default --parallel
ctest --preset default
```

The default suite uses Google Test. The acceptance suite uses Ruby Cucumber
and a native Cucumber-Cpp wire server:

```sh
bundle install
cmake --preset acceptance
cmake --build --preset acceptance --parallel
ctest --preset acceptance
```

Use `release` for optimized unit and acceptance tests, `shared` for a shared
library, or `no-tests` to omit all test infrastructure. The
[testing guide](docs/testing.md) describes the suite switches, prerequisites,
and ignored `CMakeUserPresets.json` customization. `PIGGLE_BUILD_CLI` controls
the command-line tool; it defaults to off when included as a subdirectory.

## Documentation

- [API contract](docs/api.md)
- [API design rationale and influences](docs/design.md)
- [Testing](docs/testing.md)
- [PIGG v2 format](docs/pigg-v2-format.md)
- [HOGG v10 format](docs/hogg-v10-format.md)

See [CONTRIBUTING.md](CONTRIBUTING.md) for code style, commit guidelines, and
dependency policy.

Accepted pre-encoded objects can be transferred with
`PG_READ_STORED_UNCHECKED` and `pg_writer_open_archive_builder_stored`.
These explicit operations preserve stored bytes and supplied archive checksums
without payload decoding or hashing. See [the API contract](docs/api.md).
