# SQLite Upgrade Guide

## Overview

Perfetto depends on SQLite internals:
- SQLite grammar processing via the syntaqlite library
- Internal SQLite constants and structures

## Upgrade Procedure

### Prerequisites
Only upgrade when Chrome, Android, and Google3 all support the target SQLite version.

### Steps

1. **Update version references:**
   - `tools/install-build-deps` - update SQLite version/hash
   - `bazel/deps.bzl` - update SQLite version/hash

2. **Regenerate the PerfettoSQL parser:**
   ```bash
   python3 tools/gen_syntaqlite_parser
   ```

3. **Build and test:**
   ```bash
   tools/ninja -C out/linux_clang_release trace_processor_shell perfetto_unittests
   out/linux_clang_release/perfetto_unittests --gtest_filter="*Sql*"
   tools/diff_test_trace_processor.py out/linux_clang_release/trace_processor_shell --quiet
   ```

## Common Issues

### SQLite Internal API Changes
**Error:** Compilation errors in `sqlite_utils.h` or `sqlite/bindings/*.h`

**Fix:** Update bindings for SQLite API changes

## Key Files

### Always Review
- `tools/install-build-deps` - SQLite version/hash
- `bazel/deps.bzl` - SQLite version/hash
- `tools/gen_syntaqlite_parser` - Parser regeneration script

### Generated (Don't Edit)
- `src/trace_processor/perfetto_sql/syntaqlite/syntaqlite_perfetto.c`
- `src/trace_processor/perfetto_sql/syntaqlite/syntaqlite_perfetto.h`

### Grammar Sources (Edit These)
- `src/trace_processor/perfetto_sql/syntaqlite/perfetto.y` - Perfetto dialect grammar
- `src/trace_processor/perfetto_sql/syntaqlite/perfetto.synq` - AST node definitions

## Rollback
1. Revert version changes in `tools/install-build-deps` and `bazel/deps.bzl`
2. Re-run `python3 tools/gen_syntaqlite_parser`
3. Rebuild
