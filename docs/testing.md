# Testing Piggle

Piggle follows the build and test conventions of
[native-repo-template](https://github.com/pgsocks/native-repo-template), adopted
from commit `ef70a32ac47a03806b5db2e011c6f083586ab4a3`. Library code is
procedural C++17; public C headers remain C11-compatible. Google Test checks
public contracts, boundaries, and regressions. Ruby Cucumber runs the stable
scenarios in `features/` against native Cucumber-Cpp steps.

## Prerequisites

Use CMake 3.24 or newer, a C11/C++17 toolchain, and a CMake build tool. CPM
fetches pinned dependencies at configure time. Acceptance builds additionally
need Ruby, the Bundler version in `Gemfile.lock`, and `patch` on `PATH` for the
test dependencies' CMake integration patches. Ruby 3.3 is used in CI.

```sh
gem install bundler -v 2.6.9
bundle install
```

The locked gems are Cucumber 7.1.0 and cucumber-wire 6.2.1. Configure checks
Ruby, Bundler, and those gems when acceptance is enabled. Unit-only and
disabled-suite builds do not require Ruby or acceptance gems.

## Shared presets

Configure, build, and test with matching presets:

```sh
cmake --preset default
cmake --build --preset default --parallel
ctest --preset default

cmake --preset acceptance
cmake --build --preset acceptance --parallel
ctest --preset acceptance -L infrastructure
ctest --preset acceptance -L '^(unit|acceptance)$'
```

| Preset | Library | Configuration | Product suites |
| --- | --- | --- | --- |
| `default` | Static | Debug | Unit |
| `acceptance` | Static | Debug | Unit and acceptance |
| `acceptance-only` | Static | Debug | Acceptance |
| `no-suites` | Static | Debug | Neither suite |
| `no-tests` | Static | Debug | Test infrastructure disabled |
| `release` | Static | Release | Unit and acceptance |
| `shared` | Shared | Debug | Unit and acceptance |

`BUILD_TESTING` is the master switch. `BUILD_UNIT_TESTS` and
`BUILD_ACCEPTANCE_TESTS` independently select product suites. Infrastructure
regressions run when acceptance is enabled. An enabled unit suite with no
registered cases, or acceptance run selecting no scenarios, fails. The
`no-suites` and `no-tests` presets intentionally allow zero CTest tests.
Acceptance dependencies and runners are absent when acceptance is disabled;
all test dependencies are absent when testing or both suites are disabled.

The CLI defaults to on for top-level builds. `BUILD_TESTING` and
`PIGGLE_BUILD_CLI` default to off when Piggle is used as a subdirectory.
The installed package exports `piggle::piggle` with no test dependencies.
Shared presets specify matching build/test configurations for generators
that support multiple configurations.

## Selecting tests and scenarios

```sh
ctest --preset acceptance -N
ctest --preset acceptance -L unit
ctest --preset acceptance -R 'ApiRoundtrip\.'
```

The acceptance runner accepts Cucumber arguments after the native executable:

```sh
ruby tests/acceptance/run.rb build/acceptance/tests/acceptance/acceptance_steps
ruby tests/acceptance/run.rb build/acceptance/tests/acceptance/acceptance_steps \
  features/names.feature --tags @NAME-001
ruby tests/acceptance/run.rb build/acceptance/tests/acceptance/acceptance_steps \
  features/names.feature:5
```

On Windows add `.exe`; with a multi-configuration generator the executable
is under its configuration directory, such as `Debug/acceptance_steps.exe`.
The runner starts the native server on an ephemeral loopback port, waits for
readiness, and invokes locked Cucumber with `--require features --strict`.
It propagates server and Cucumber failures and cleans up both process groups,
including timeouts and interruptions. Parallel runs use independent ports
and scenario workspaces. Ruby code supplies protocol support; product steps
and assertions live in C++.

Every existing scenario ID is retained. `@windows` selects Windows native
identity and junction cases. `@posix` selects case-sensitive collisions and
POSIX special files. `@linux` selects syscall fault injection and deterministic
inotify queue overflow. The runner excludes inapplicable tags automatically;
an explicit filter that leaves no scenarios still fails. Common archive,
stream, watch, and ownership behavior runs on all supported CI platforms.
Fixtures use temporary native paths on the available volume; Windows does
not require a pre-existing `C:\Game` directory.

## Test organization and isolation

- `tests/unit/` retains the original contract and regression expectations as
  discovered Google Test cases, with parameterized transition regressions.
- `tests/consumers/c11.c` compiles and links a C11 consumer; the C++ header
  case checks the procedural aliases and linkage.
- `tests/acceptance/` owns one context and workspace per scenario. Steps use
  real public API calls, independent malformed archive fixtures, and native
  edits. Assertions never escape across observer callbacks.
- `tests/helpers/` isolates memory limits and cross-process writable leases.
  Linux fault injection checks partial deletion, journal recovery, cleanup,
  and failures after publication without production test hooks.
- `tests/infrastructure/` contains separate runner fixtures for discovery,
  missing executables/gems, strict failures, empty selection, readiness,
  cleanup, interruption, and concurrent runs. These are not product coverage.

Owned library handles are tracked for cleanup after failed assertions. Each
case uses a unique directory under the preset's build tree; directories are
removed after the case. Worker tests serialize each owned handle. Unix
memory-limit tests launch a small separate executable so framework allocations
do not count toward the limit. Sanitizer builds should run those cases
separately in an ordinary build because shadow mappings can conflict with
their address-space limits.

## Local customization

Keep machine-specific paths and experiments in ignored `CMakeUserPresets.json`.
Inherit shared presets and keep configure, build, and test presets together:

```json
{
  "version": 5,
  "configurePresets": [{
    "name": "local-release",
    "inherits": "release",
    "generator": "Ninja",
    "binaryDir": "${sourceDir}/build/local-release",
    "cacheVariables": {
      "CMAKE_INSTALL_PREFIX": "${sourceDir}/build/install",
      "CMAKE_C_COMPILER": "clang",
      "CMAKE_CXX_COMPILER": "clang++"
    }
  }],
  "buildPresets": [{
    "name": "local-release",
    "inherits": "release",
    "configurePreset": "local-release"
  }],
  "testPresets": [{
    "name": "local-release",
    "inherits": "release",
    "configurePreset": "local-release"
  }]
}
```

Use separate build directories for toolchain or library variants. CI follows
the same convention, creating ignored presets that inherit `acceptance` or
`release` and choose GCC, Clang, or MSVC. Native Windows checks require a real
Windows filesystem: Wine alone cannot establish recursive notification and
native attribute guarantees.

## Archive construction and maintenance checks

The unit suite covers canonical collisions and insertion order, failed entry
metadata allocation, reserve exhaustion and relocation, known mutation cache
reconciliation and tree rollback, large cached headers, compressed DataLists,
encoded input and interruption at all three DataList fold publication steps,
unioned hidden/unnamed/shared ranges, compaction and aliases.
Session checks cover exclusive ownership, checkpoint selection, suspend/close,
metadata and payload corruption, cross-volume publication and process exit at
checkpoint/publication boundaries. Linux syscall faults include ENOSPC and
existing journal/cleanup failures. Both archive formats reject unwritable
staging directories; pack reserves its captured inventory before transfer. A
bounded child file-size limit checks retries after partial staging writes across
volumes. A separate-volume case explicitly reports
when the test machine has no second available filesystem.

The `BUILD-001` through `BUILD-005` acceptance scenarios in
`features/construction.feature` exercise reserve growth, a 64 KiB cached header
with an unrelated reader, compaction, checkpoint-only resume and suspension.
C11 and procedural C++ consumer fixtures cover each public addition.

Header-cache unit cases cover exact extension recipes, prefix inference and
bounds, automatic/explicit/suppressed writes, logical and encoded input, cache
reads and payload-only reads, seeks, EOF payload verification, and independent
PIGG-pool/HOGG-DataList codecs with binary bytes and empty slots.
`features/header_cache.feature` exercises automatic caching in both archive
formats, caller overrides, standalone extraction and independent container
roundtrips. Original-implementation interoperability checks are one-off external
verification and are deliberately excluded from automated repository tests.

## Scaling benchmark

```sh
cmake --preset benchmark
cmake --build --preset benchmark --parallel
ctest --preset benchmark -L benchmark
```

The benchmark uses 10,000/50,000/100,000 named one-byte raw entries. It reports
construction and finish time, canonical comparisons, allocation calls/bytes,
process peak memory, payload copying, temporary archive length, growth events
and relocated bytes. The mutation phase reports warmed replacement time,
archive metadata read calls and allocations separately. GNU static builds count
allocation and `pread64` calls with linker wrappers. Those counters are
unavailable in other configurations; zero counter fields there mean unmeasured.
The benchmark always checks builder comparison bounds and zero full
reconciliations for known replacements. `reserve` as its third argument reserves
the complete inventory and avoids table-overlap relocation.
