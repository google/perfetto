# Perfetto Project Development Guidelines

This document provides essential instructions and best practices for developing
in the Perfetto codebase. Adhere to these guidelines to ensure consistency and
quality.

## 1. Building the Project

Use the following commands to build the project for different configurations.
All commands should be run from the root of the repository.

### Standard Release Build

To build the standard release version:

```sh
tools/ninja -C out/linux_clang_release -k 10000 trace_processor_shell perfetto_unittests
```

## 2. Running Tests

Use the following commands to run unit tests for the corresponding build
configurations.

### Standard Release Tests

```sh
out/linux_clang_release/perfetto_unittests --gtest_brief=1 --gtest_filter="<TestSuiteName.*>"
```

### Trace Processor Diff Tests

Trace Processor Diff Tests (or diff tests for short) are executed by running the
following command:

```sh
tools/diff_test_trace_processor.py out/linux_clang_release/trace_processor_shell --keep-input --quiet --name-filter="<regex of test names>"
```

**Note:** These tests can also be run with ASan or MSan builds by changing the
path from `out/linux_clang_release/` to `out/linux_asan/` or `out/linux_msan/`
respectively. **Note:** The `--name-filter` argument is optional. **Note:** When
using the `--name-filter` flag, do not include `test_` in the filter. The test
runner automatically drops this prefix. For example, to run `test_my_cool_test`,
use the filter `MyTestSuite.my_cool_test`.

### Integration Tests

Integration tests are executed by running the `perfetto_integrationtests`
binary. For example:

```sh
out/linux_clang_release/perfetto_integrationtests --gtest_filter="<TestSuiteName.*>"
```

### Test Guidelines

- **Prefer test suites over individual tests.** When using the `--gtest_filter`
  flag, specify a whole test suite (e.g., `"MyTestSuite.*"`) instead of a single
  test case (e.g., `"MyTestSuite.MySpecificTest"`). This ensures broader test
  coverage.
- **Do not test unstable IDs.** When writing diff tests, do not include columns
  that contain unstable IDs (e.g. `upid`, `utid`, `id`, etc) in the output. These
  IDs can change between different runs of the same test, which will cause the
  test to fail.
- **Remove `test_` prefix for diff tests.** When using the `--name-filter` flag
  for diff tests, do not include `test_` in the filter. The test
  runner automatically drops this prefix. For example, to run `test_my_cool_test`,
  use the filter `MyTestSuite.my_cool_test`.

## 3. Core Software Engineering Principles

Follow these principles when writing and modifying code.

### Principle 1: Don't Repeat Yourself (DRY)

- **Avoid code duplication.** Before writing a new function, search the codebase
  for existing functions that provide similar functionality.
- **Reuse and refactor.** If a suitable function exists, reuse it. If it's close
  but not an exact match, consider refactoring the existing function to
  accommodate the new use case instead of creating a copy.
- **Consult if unsure.** If you are considering duplicating a function or a
  significant block of code, consult with the user first.

## 4. Getting Diffs

When asked to "get a diff" or "read the current diff", run the following
command:

```sh
git diff $(git config branch.$(git rev-parse --abbrev-ref HEAD).parent)
```

## 5. Fixing GN Dependencies

When asked to fix GN dependencies, run the following command and fix any errors
that are reported:

```sh
tools/gn check out/linux_clang_release/
```

**Note:** When fixing include errors, do not add dependencies to `public_deps`
unless explicitly instructed to by the user. Instead, add a direct dependency to
the target that requires it.

## 6. Other Configurations

### ASan (AddressSanitizer) Build

To build with ASan for memory error detection:

```sh
tools/ninja -C out/linux_asan -k 10000 trace_processor_shell perfetto_unittests
```

### MSan (MemorySanitizer) Build

To build with MSan for uninitialized read detection:

```sh
tools/ninja -C out/linux_msan -k 10000 trace_processor_shell perfetto_unittests
```

### ASan (AddressSanitizer) Tests

**Note:** Ensure the `ASAN_SYMBOLIZER_PATH` is set correctly.

```sh
ASAN_SYMBOLIZER_PATH="$(pwd)/buildtools/linux64/clang/bin/llvm-symbolizer" \
out/linux_asan/perfetto_unittests --gtest_brief=1 --gtest_filter="<TestSuiteName.*>"
```

### MSan (MemorySanitizer) Tests

**Note:** Ensure the `MSAN_SYMBOLIZER_PATH` is set correctly.

```sh
MSAN_SYMBOLIZER_PATH="$(pwd)/buildtools/linux64/clang/bin/llvm-symbolizer" \
out/linux_msan/perfetto_unittests --gtest_brief=1 --gtest_filter="<TestSuiteName.*>"

## 7. Creating Pull Requests

When creating a pull request, follow these steps:

1.  **Create a new branch:**
    Use the command `git new-branch dev/lalitm/<name-of-branch>` to create a new branch for your pull request.

2.  **Create a stacked/dependent pull request:**
    To create a pull request that depends on another, use the command `git new-branch --parent <name-of-parent-branch> dev/lalitm/<name-of-branch>`.

**Note:** The `git new-branch` command only creates and switches to a new
branch. The normal `git add` and `git commit` workflow should be used to add
changes to the branch.

## 8. Commit Messages

When writing commit messages, follow these guidelines:

- **Prefix your commits.** Prefix changes to Trace Processor code with `tp:`,
  UI code with `ui:`, and general Perfetto changes with `perfetto:`.
- **Keep it concise.** A short one-line summary followed by a paragraph
  describing the change is the best commit message.
