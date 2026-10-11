Documentation committed to this repository serves both as information for users
and as a contract for the project's intent. Agents must follow this
documentation and must not modify repository documentation unless explicitly
instructed to. Agent notes must not be committed to this repository or treated
as authoritative.

All changes must comply with CONTRIBUTING.md before they are presented for
review. During development, agents may temporarily deviate from its
requirements if those deviations can be fully corrected before review.
Examples include failing tests, nonconforming code, temporary architecture
or dependency experiments, and unfinished commits.

Required approvals must be obtained before taking the actions they govern.
Commits that CONTRIBUTING.md treats as immutable must remain unchanged.
Permission to defer requirements does not authorize documentation changes.

Use the shared CMake presets for supported build configurations. Use the
ignored `CMakeUserPresets.json` for special local options and one-off tests.
Inherit from shared presets, give local configurations separate build
directories, and do not commit user presets. Keep configure, build and test
presets together; multi-configuration generators need matching configuration
values in build and test presets. Keep temporary notes and tools outside
tracked repository files.

Before presenting changes for review, resolve all temporary deviations and
complete the checks required by CONTRIBUTING.md.
