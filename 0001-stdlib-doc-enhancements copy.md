# PerfettoSQL Standard Library Documentation Enhancement

**Authors:** @LalitMaganti

**Status:** Draft

## Overview

This document outlines the comprehensive enhancement plan for PerfettoSQL
standard library documentation generation. The goal is to transform the current
basic documentation into rich, navigable documentation with visual diagrams,
proper module organization, and better structure.

## Current State Analysis

### Existing Scripts

- `tools/gen_stdlib_docs_json.py` - Parses SQL files and generates JSON docs
  using `python.generators.sql_processing.docs_parse`
- `infra/perfetto.dev/src/gen_stdlib_docs_md.py` - Converts JSON to markdown
  with basic table formatting
- `infra/perfetto.dev/src/gen_sql_tables_reference.js` - Contains mermaid
  diagram generation for table relationships (reference for our implementation)

### Current Problems

1. **Table relationship visualization missing** - No visual representation of
   how stdlib tables connect via `JOINID(table.column)` relationships
2. **Prelude organization poor** - 40+ tables/views dumped into flat sections
   without logical grouping
3. **No module-level documentation** - Missing module descriptions, purposes,
   and relationships
4. **Poor navigation** - Hard to find related functionality, no cross-references
5. **Monolithic prelude structure** - `prelude/after_eof/tables_views.sql`
   contains 32 tables in one file

## Enhancement Goals

### Primary Objectives

1. **Table Relationship Diagrams** - Visual mermaid diagrams showing how stdlib
   tables connect through joins
2. **Organized Prelude** - Break up monolithic prelude into logical, documented
   modules
3. **Module-Level Documentation** - Support for module descriptions and metadata
4. **Better Formatting** - Improved navigation, layout, and cross-references

### Success Criteria

- Stdlib docs show visual table relationships like the SQL tables reference
- Prelude is organized into logical modules with clear purposes
- Users can easily navigate between related functionality
- Module documentation provides context and usage guidance

## Implementation Plan

### Phase 1: Foundation & Refactoring (Dependencies First)

**Status: Not Started**

#### Task 1.1: Reorganize Prelude Module Structure

**Files:** `src/trace_processor/perfetto_sql/stdlib/prelude/after_eof/`
**Priority:** High (blocks other improvements) **Dependencies:** None

Break up the monolithic `tables_views.sql` (32 tables) into focused modules:

1. **`core.sql`** - Fundamental trace concepts

   - `trace_metrics`, `trace_bounds`

2. **`cpu_scheduling.sql`** - CPU and scheduling data

   - `cpu`, `cpu_available_frequencies`, `sched_slice`, `sched`, `thread_state`,
     `cpu_track`

3. **`tracks.sql`** - Track infrastructure

   - `track`, `thread_track`, `process_track`, `gpu_track` (deprecated)

4. **`counters.sql`** - Performance counters and tracks

   - `counter_track`, `counters`, all `*_counter_track` variants

5. **`events.sql`** - Event data and slices

   - `ftrace_event`, `frame_slice`, `gpu_slice`, timeline slices, `raw`
     (deprecated)

6. **`memory.sql`** - Memory and heap analysis
   - `heap_graph_*` tables, `memory_snapshot*` tables

**Implementation Steps:**

- [ ] Create new module files with proper `-- @module` documentation
- [ ] Move table definitions from `tables_views.sql` to appropriate modules
- [ ] Update `BUILD.gn` dependencies
- [ ] Update include chain in `tables_views.sql` to include new modules
- [ ] Test that all tables remain accessible in prelude
- [ ] **Commit:**
      `git add . && git commit -m "tp: reorganize prelude into focused modules"`

#### Task 1.2: Add Module Documentation Support to Parser

**Files:** `tools/gen_stdlib_docs_json.py`,
`python/generators/sql_processing/docs_parse.py` **Priority:** High (enables
module organization) **Dependencies:** Task 1.1

Extend SQL parsing to extract module-level documentation:

**Module Documentation Syntax:**

````sql
-- @module package.module.name
-- Module description here. Can be multiple lines.
--
-- ## Usage Examples
-- ```sql
-- SELECT * FROM my_table;
-- ```
--
-- ## Related Modules
-- - package.other.module - for related functionality
````

**Implementation Steps:**

- [ ] Extend `docs_parse.py` to recognize `-- @module` comments
- [ ] Extract module name, description, and metadata
- [ ] Add module information to JSON schema output
- [ ] Update `gen_stdlib_docs_json.py` to include module metadata in output
- [ ] Test with sample module documentation
- [ ] **Commit:**
      `git add . && git commit -m "tp: add module documentation support to stdlib parser"`

### Phase 2: Mermaid Diagram Integration

**Status: Not Started**

#### Task 2.1: Port Table Relationship Logic to Python

**Files:** `tools/stdlib_mermaid_generator.py` (new),
`infra/perfetto.dev/src/gen_stdlib_docs_md.py` **Priority:** High (core visual
enhancement) **Dependencies:** Phase 1 complete

Port the mermaid diagram generation from `gen_sql_tables_reference.js` to
Python:

**Reference Implementation:**
`infra/perfetto.dev/src/gen_sql_tables_reference.js:113-149`

**Key Components:**

- Parse `JOINID(table.column)` relationships from JSON data (already extracted
  by `_long_type_to_table()`)
- Generate mermaid graph syntax for table connections
- Support both per-module and per-package diagrams
- Handle click-through links to table documentation

**Implementation Steps:**

- [ ] Create `tools/stdlib_mermaid_generator.py` with relationship parsing
- [ ] Implement mermaid graph generation for stdlib table connections
- [ ] Add support for per-module diagrams (show internal table relationships)
- [ ] Add support for per-package diagrams (show cross-module connections)
- [ ] Test diagram generation with sample stdlib modules
- [ ] **Commit:**
      `git add . && git commit -m "tp: add mermaid diagram generation for stdlib tables"`

#### Task 2.2: Integrate Mermaid Generation into Markdown Output

**Files:** `infra/perfetto.dev/src/gen_stdlib_docs_md.py` **Priority:** High
(user-facing improvement) **Dependencies:** Task 2.1

Enhance markdown generation to include mermaid diagrams:

**Diagram Types:**

1. **Module Relationship Diagrams** - Show how tables within a module connect
2. **Package Architecture Diagrams** - Show inter-module connections
3. **Cross-Package Diagrams** - Show major connections between packages

**Implementation Steps:**

- [ ] Integrate mermaid generator into `ModuleMd` class
- [ ] Add diagram sections to module documentation
- [ ] Enhance `PackageMd` to include package-level diagrams
- [ ] Add click-through navigation in diagrams
- [ ] Test diagram rendering in documentation output
- [ ] **Commit:**
      `git add . && git commit -m "tp: integrate table relationship diagrams into docs"`

### Phase 3: Enhanced Documentation Structure

**Status: Not Started**

#### Task 3.1: Improve Prelude Organization in Docs

**Files:** `infra/perfetto.dev/src/gen_stdlib_docs_md.py` **Priority:** Medium
(improves user experience) **Dependencies:** Phase 1 and 2 complete

Transform prelude from flat lists to organized modules:

**Current Prelude Output:**

```markdown
## Package: prelude

#### Views/Tables

- trace_metrics. Lists all metrics...
- trace_bounds. Time bounds of...
- track. Tracks are a fundamental... [... 40+ more items in flat list]
```

**Enhanced Prelude Output:**

```markdown
## Package: prelude

### prelude.after_eof.core

_Fundamental trace concepts and infrastructure_

[Mermaid diagram showing core table relationships]

#### Views/Tables

- **trace_bounds**. Time bounds of the entire trace
- **track**. Fundamental timeline concept for events

### prelude.after_eof.cpu_scheduling

_CPU scheduling and thread state analysis_

[Mermaid diagram showing scheduling table relationships]

#### Views/Tables

- **cpu**. Information about device CPUs
- **sched_slice**. CPU scheduling time slices
```

**Implementation Steps:**

- [ ] Modify `PackageMd.get_prelude_description()` to organize by modules
      instead of flat lists
- [ ] Add module descriptions and mermaid diagrams to prelude sections
- [ ] Include "Getting Started" guidance for core concepts
- [ ] Add cross-references between related modules
- [ ] Test prelude organization with real documentation output
- [ ] **Commit:**
      `git add . && git commit -m "tp: improve prelude organization in generated docs"`

#### Task 3.2: Enhanced Formatting and Navigation

**Files:** `infra/perfetto.dev/src/gen_stdlib_docs_md.py` **Priority:** Medium
(polish and usability) **Dependencies:** Phases 1-2 complete

Improve overall documentation formatting and navigation:

**Enhancements:**

- Better typography and spacing in markdown output
- Improved table formatting with proper column alignment
- Enhanced collapsible sections for better scanning
- Cross-reference links between related tables/functions
- Module-level table of contents
- Search-friendly metadata

**Implementation Steps:**

- [ ] Enhance markdown table formatting in `_md_table_header()`
- [ ] Improve collapsible section styling in `_md_rolldown()`
- [ ] Add cross-reference link generation for related tables
- [ ] Include module-level navigation aids
- [ ] Add metadata for search optimization
- [ ] Test formatting improvements across different modules
- [ ] **Commit:**
      `git add . && git commit -m "tp: enhance stdlib docs formatting and navigation"`

### Phase 4: Testing and Validation

**Status: Not Started**

#### Task 4.1: Comprehensive Testing

**Files:** All modified files **Priority:** High (ensure quality)
**Dependencies:** All previous phases

**Testing Areas:**

- [ ] Verify all existing tables/functions remain accessible after prelude
      reorganization
- [ ] Test mermaid diagram generation with various stdlib modules
- [ ] Validate JSON schema compatibility with existing tooling
- [ ] Check markdown output rendering in documentation site
- [ ] Performance testing with full stdlib (ensure acceptable generation time)
- [ ] Cross-browser testing of mermaid diagram rendering

#### Task 4.2: Documentation and Examples

**Files:** Documentation, examples **Priority:** Medium (maintenance and
adoption) **Dependencies:** Task 4.1

**Deliverables:**

- [ ] Update module documentation guidelines for stdlib contributors
- [ ] Create examples of well-documented modules
- [ ] Document mermaid diagram syntax and best practices
- [ ] Update build documentation for new files and dependencies

## Progress Tracking

### Phase 1 Progress: 0/2 tasks complete

- [x] Task 1.1: Reorganize Prelude Module Structure
- [x] Task 1.2: Add Module Documentation Support to Parser

### Phase 2 Progress: 0/2 tasks complete

- [ ] Task 2.1: Port Table Relationship Logic to Python
- [ ] Task 2.2: Integrate Mermaid Generation into Markdown Output

### Phase 3 Progress: 0/2 tasks complete

- [ ] Task 3.1: Improve Prelude Organization in Docs
- [ ] Task 3.2: Enhanced Formatting and Navigation

### Phase 4 Progress: 0/2 tasks complete

- [ ] Task 4.1: Comprehensive Testing
- [ ] Task 4.2: Documentation and Examples

## Key Implementation Notes

### Maintaining Backward Compatibility

- All existing tables/views must remain accessible in prelude after
  reorganization
- JSON schema changes should be additive only (add module metadata, don't change
  existing fields)
- Documentation URLs should not break (consider redirects if needed)

### Performance Considerations

- Mermaid diagram generation should not significantly slow documentation build
- Consider caching diagram generation for large modules
- Optimize JSON parsing for module metadata extraction

### Code Quality Standards

- Follow existing code style in modified files
- Add comprehensive docstrings for new functions
- Include type hints for new Python code
- Maintain test coverage for modified functionality

## Future Enhancements (Out of Scope)

### Potential Follow-up Work

- Interactive diagram exploration in documentation site
- Automated module dependency analysis and validation
- Enhanced search functionality with module-level filtering
- Integration with trace analysis tutorials and examples
- Performance metrics tracking for documentation generation

---

## Git Workflow and CL Stacking

### Branch Strategy

Use `git new-branch --current-parent <branch-name>` to create a proper CL stack
for review:

```bash
# Start from main branch
git checkout main
git new-branch dev/lalitm/stdlib-docs-prelude-refactor

# After completing Task 1.1, commit and create next branch
git add . && git commit -m "tp: reorganize prelude into focused modules"
git new-branch --current-parent dev/lalitm/stdlib-docs-module-parsing

# Continue pattern for each major task...
```

### Commit Points

Each task should result in a separate commit/CL:

- **Task 1.1** → `tp: reorganize prelude into focused modules`
- **Task 1.2** → `tp: add module documentation support to stdlib parser`
- **Task 2.1** → `tp: add mermaid diagram generation for stdlib tables`
- **Task 2.2** → `tp: integrate table relationship diagrams into docs`
- **Task 3.1** → `tp: improve prelude organization in generated docs`
- **Task 3.2** → `tp: enhance stdlib docs formatting and navigation`

### Testing and Validation

Before each commit:

```bash
# Build and test documentation generation
tools/ninja -C out/linux_clang_release -k 10000 trace_processor_shell
# Run stdlib docs generation to verify no breakage
# Test that all prelude tables remain accessible
```

## Usage Instructions for Incremental Implementation

1. **Start with Phase 1** - These are foundational changes that other phases
   depend on
2. **Create proper CL stack** - Use `git new-branch --current-parent` for each
   task
3. **Commit after each task** - Each task should be a reviewable, testable unit
4. **Update progress checkboxes** as tasks are completed
5. **Validate each task** before moving to dependent tasks
6. **Test incrementally** - each phase should leave the system in a working
   state
7. **Update this document** if implementation reveals new requirements or
   blockers

This design document should be treated as a living specification that evolves
with implementation learnings.