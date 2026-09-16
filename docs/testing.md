# Testing Piggle

The `features/*.feature` files specify observable behavior with stable scenario
IDs. They are not run by a feature runner. CTest runs compiled C tests of the
public API and a C++ header and linkage test. The C tests provide executable
coverage for the behavior specifications and the [API contract](api.md).

## Run the tests

From the repository root, configure a Debug build, build it, and run the full
CTest suite:

```sh
cmake -S . -B /tmp/piggle-build -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/piggle-build --parallel
ctest --test-dir /tmp/piggle-build --output-on-failure
```

A top-level build enables tests and the `piggle` command by default. Set
`BUILD_TESTING=OFF` to omit the tests or `PIGGLE_BUILD_CLI=OFF` to omit the
command. When Piggle is included as a subdirectory of another CMake project,
both options default to off. The build directory is disposable. Use a fresh
one if files from earlier test runs interfere with a case.

To list registered cases or run a subset, use CTest:

```sh
ctest --test-dir /tmp/piggle-build -N
ctest --test-dir /tmp/piggle-build -L contract --output-on-failure
ctest --test-dir /tmp/piggle-build -L infrastructure --output-on-failure
ctest --test-dir /tmp/piggle-build -R '^api_roundtrip_' --output-on-failure
```

## Test groups

- **Contract** (`contract` label) covers argument validation, handle state,
  names, source and tree discovery, readers and writers, streaming, watching,
  archive building and editing, round trips, and regressions. Most cases run
  in separate working directories under the build tree.
- **Infrastructure** (`infrastructure` label) contains the C++ public header
  and library linkage test. The contract executables compile and link the C
  API.

The registered cases depend on the platform. Linux adds journal fault,
watch retry, and native discovery cases; Windows adds path and discovery cases;
Unix builds add memory cases. When the CLI target is built on Windows, a
Windows path regression also invokes it. There is no `cli` CTest label or
general CLI test group.

## Build variants

Use a separate build directory to test a shared library:

```sh
cmake -S . -B /tmp/piggle-shared -DBUILD_SHARED_LIBS=ON
cmake --build /tmp/piggle-shared --parallel
ctest --test-dir /tmp/piggle-shared --output-on-failure
```

For a multi-configuration generator, pass the same configuration to the build
and test commands, for example `--config Release`.

## New discovery and archive contracts

The entry, managed-scope, seek, checksum, metadata and lease cases cover the
0.2 contracts. Linux regression cases inject journal failures before and
after commitment, including metadata-only updates. The lease test launches
another process and checks hard-link aliases, native replacement, builder
replacement and ownership retained by a reader.

Native Windows tests should also run on a real Windows filesystem. Wine can
miss recursive notices for existing subdirectories and can fail to preserve
hidden/system attributes. The deep managed-scope and attribute tests remain
enabled; a Wine-only pass cannot establish those Windows guarantees.

Address/undefined-behavior sanitizer runs can exclude `api_memory_*` when the
sanitizer shadow mapping conflicts with those tests' address-space limits.
Run those memory-limit cases separately in an ordinary build. Do not treat
that separate run as a sanitizer result.
