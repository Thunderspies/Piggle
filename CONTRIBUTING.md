Contributions will be rejected unless they meet the following criteria.

# Code Style

Write library sources in C++11, with a C11-compatible public C header and
a procedural C++ header. Use free functions, plain data structs, opaque
handles, and explicit state machines. Do not introduce methods, inheritance,
polymorphism, templates (including STL containers or smart pointers), or
exception-based error handling. Keep callbacks as explicit function pointers
with caller state, and report errors through the public status contract.

Follow the [Linux kernel coding style][style] with these extra restrictions.

- Keep line width 80 characters or under
- Declare local variables close to where they're first used
- Only indent with tabs and only use tabs for indenting
- Any number of spaces may be used for alignment/readability in these cases:
  - Never at the start of a line
  - Never for indenting

Since this is inherited code, only new code needs to follow this style guide.

[style]: https://docs.kernel.org/process/coding-style.html

# Commits

Keep changes in commits as complete and concise as possible. Do not attempt to
fix two things at once, but don't itemize tasks and goals into individual
commits that are so small that any one commit would be pointless in isolation.
A commit should be treated as a product just as much as the code, because it
will be used by a maintainer to apply patches to other forks or revert changes
that introduce errors.

Commits before the current tagged release are considered immutable, so a large
commit to add a new feature or subsystem is valid and encouraged within a
release, but follow-up work after the release where it was introduced should be
itemized. This is because a maintainer would apply patches to the latest
release.

## Titles

A maximum of 72 characters is allowed for commit titles. Use imperative-tense
for titles so that the following statement makes sense when the title is used
to fill in the blank:

> The purpose of this commit is to BLANK

For instance, a commit title of "fix a segfault in the text formatter" still
makes sense when used in the sentence "The purpose of this commit is to *fix a
segfault in the text formatter*".

## Descriptions

The rest of the commit message should have a body explaining the following:

- The purpose of the commit
- High level implementation details
- Any known issues, limitations, or follow-up work introduced by this commit

## Fixups

If a commit is intended to fix an issue or complete follow-up work in another
commit *since the last tagged release*, then submit it as a fixup commit using
`git commit --fixup` on the other commit. A maintainer will later squash all
fixup commits into one concise commit before tagging the release.

# Dependencies

This project uses CPM in CMake to pull dependencies. If a dependency is needed,
add it with `CPMAddPackage`. Dependencies may be patched only so that they can
build with CMake and CPM. Use the `PATCHES` parameter in `CPMAddPackage` to
apply patches. Do not patch a dependency to port it to a specific compiler,
link with another library, fix errors, or add/remove/change features for
instance.

Introducing a new dependeny that cannot be included as a standard CPM package
requires project maintainer approval.

# Testing

All automated tests must pass. Changes to the API declared in public headers
necessitates revision or additions to the tests. This project follows the
Behavioral Driven Development process, so changes to the intended usage and
behavior of this software should be reflected in the included feature files.
Where applicable, this software should be tested for interoperability with its
previous release.
