# Plan: Migrate to Dataframe Architecture

**Overall Goal:** Migrate the codebase from the legacy table system to the new `dataframe` architecture.

**Underlying Principle:** "Undo" the last big commit while keeping all its changes in your working directory (`git reset HEAD~`). Then, selectively add and commit these changes in logical chunks using `git add -p`.

---

## Proposed Phased Approach:

### Phase 1: Introduce Core Dataframe Primitives & Python Generator Updates
*   **Objective:** Lay the foundational C++ `dataframe` structures and update the Python code generators to support the new `Column` and `Table` definitions.
*   **Key Changes:**
    *   Introduce `SqlAccess`, `CppAccess`, `CppAccessDuration` enums in `python/generators/trace_processor_table/public.py`.
    *   Update `Column` and `Table` dataclasses in `python/generators/trace_processor_table/public.py`.
    *   Initial updates to `python/generators/trace_processor_table/serialize.py` and `python/generators/trace_processor_table/util.py` for these new primitives.
    *   Introduce new C++ `dataframe` helper files:
        *   `src/trace_processor/dataframe/impl/bit_vector.h` (new methods)
        *   `src/trace_processor/dataframe/impl/flex_vector.h` (new methods)
        *   `src/trace_processor/dataframe/impl/slab.h` (modifications)
        *   `src/trace_processor/dataframe/specs.h` (new methods)
        *   The new `src/trace_processor/dataframe/adhoc_dataframe_builder.h`.
    *   Core updates to `src/trace_processor/dataframe/dataframe.h` and `src/trace_processor/dataframe/dataframe.cc`.
    *   Update `src/trace_processor/dataframe/runtime_dataframe_builder.h`.
*   **Rationale:** Establishes the new `dataframe` building blocks.

### Phase 2: Adapt SQL Engine & Core Table Modules to Dataframe
*   **Objective:** Integrate the new `dataframe` system with the SQLite query engine and its module infrastructure.
*   **Key Changes:**
    *   Introduce the new `src/trace_processor/perfetto_sql/engine/static_table_function_module.h` and `.cc`.
    *   Update `src/trace_processor/perfetto_sql/engine/dataframe_module.h` and `.cc`.
    *   Update `src/trace_processor/perfetto_sql/engine/table_pointer_module.h` and `.cc`.
    *   Major updates to `src/trace_processor/perfetto_sql/engine/perfetto_sql_engine.h` and `.cc` (e.g., `RegisterStaticTable` now takes `const dataframe::Dataframe*`).
*   **Rationale:** Connects `dataframe` to the query execution layer.

### Phase 3: Migrate Python Table Definitions & Intrinsic Table Functions
*   **Objective:** Convert all static table definitions (Python) and C++ intrinsic table functions to the new `dataframe` model and `StaticTableFunction::Cursor` API. This is the largest phase and could be broken down further.
*   **Key Changes (can be grouped):**
    *   Update all `*.py` files in `src/trace_processor/tables/` to use new `CppAccess` flags, etc.
    *   Refactor all C++ table functions in `src/trace_processor/perfetto_sql/intrinsics/table_functions/` (e.g., `ancestor.cc`, `connected_flow.cc`, `experimental_flamegraph.cc`, etc.) to the new API.
*   **Rationale:** Migrates the actual data providers and their schemas. Could be split by logical groups of tables/functions.

### Phase 4: Update Importers & Other C++ Utilities
*   **Objective:** Adapt the remaining C++ codebase (importers, various utilities) to use the new `dataframe` and table APIs.
*   **Key Changes:**
    *   Updates across `src/trace_processor/importers/` (e.g., `ArgsTracker` usage changes).
    *   Changes in `src/trace_processor/storage/trace_storage.h` (`FinalizeTables`, `GetString` signature).
    *   Updates to `src/trace_processor/util/profile_builder.cc`.
    *   Miscellaneous C++ files interacting with the old table APIs.
*   **Rationale:** Ensures all data ingestion and processing paths are compatible with the new system.

### Phase 5: Remove Old Table Macro System & Associated Files
*   **Objective:** Clean up the codebase by removing the now-obsolete legacy table system.
*   **Key Changes (Deletions):**
    *   `src/trace_processor/db/typed_column.h`
    *   `src/trace_processor/tables/macros_internal.cc` and `.h`
    *   `src/trace_processor/tables/table_destructors.cc`
    *   Associated build targets in `Android.bp` and `BUILD.gn` files.
*   **Rationale:** Final cleanup of the old infrastructure.

### Phase 6: Finalize Build System Changes & Update Tests
*   **Objective:** Ensure the project builds correctly and all tests are updated for the new system.
*   **Key Changes:**
    *   Remaining updates to `Android.bp`, `BUILD.gn`, and `gn/perfetto_benchmarks.gni`.
    *   Update all affected diff tests in `test/trace_processor/diff_tests/`.
    *   Delete old test files like `src/trace_processor/tables/py_tables_benchmark.cc` and `src/trace_processor/tables/py_tables_unittest.cc`.
*   **Rationale:** Ensures a clean, working state post-refactor.

---

## Visual Plan (Simplified Mermaid Diagram):

```mermaid
graph TD
    A[Phase 1: Core Dataframe & PyGen Primitives] --> B;
    B[Phase 2: SQL Engine & Module Adaptation] --> C;
    C[Phase 3: Migrate Table Defs & Intrinsic Functions] --> D;
    D[Phase 4: Update Importers & Utilities] --> E;
    E[Phase 5: Remove Old Macro System] --> F;
    F[Phase 6: Build System & Test Updates];