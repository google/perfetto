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

1. **Single-page documentation unwieldy** - All stdlib documentation (40+
   prelude tables, 50+ android tables, etc.) is crammed into one massive page,
   making it difficult to navigate and slow to load. Need Rust-style
   documentation with global list + per-module pages.
2. **Large package diagrams unreadable** - Packages like `android` with >50
   tables produce tiny, illegible mermaid diagrams. Diagrams need to be scoped
   to individual modules or broken into focused subgraphs.
3. **Cross-package relationships missing** - No visualization of how tables in
   different packages connect (e.g., Android tables linking to prelude's
   process/thread tables via JOINID).
4. **Table relationship visualization missing** - No visual representation of
   how stdlib tables connect via `JOINID(table.column)` relationships (within a
   module).
5. **Prelude organization poor** - 40+ tables/views dumped into flat sections
   without logical grouping
6. **No module-level documentation** - Missing module descriptions, purposes,
   and relationships
7. **Poor navigation** - Hard to find related functionality, no cross-references
8. **Monolithic prelude structure** - `prelude/after_eof/tables_views.sql`
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

**IMPORTANT:** Phase 2 (Multi-page Documentation) must be completed BEFORE Phase
3 (Mermaid Diagrams) to avoid creating unusable single-page diagrams.

### Phase 1: Foundation & Refactoring (Dependencies First)

**Status: Completed**

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

- [x] Create new module files with proper `-- @module` documentation
- [x] Move table definitions from `tables_views.sql` to appropriate modules
- [x] Update `BUILD.gn` dependencies
- [x] Update include chain in `tables_views.sql` to include new modules
- [x] Test that all tables remain accessible in prelude
- [x] **Commit:** `9df0833e9e - tp: reorganize prelude into focused modules`

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

- [x] Extend `docs_parse.py` to recognize `-- @module` comments
- [x] Extract module name, description, and metadata
- [x] Add module information to JSON schema output
- [x] Update `gen_stdlib_docs_json.py` to include module metadata in output
- [x] Test with sample module documentation
- [x] **Commit:**
      `c324ab46cb - tp: add module documentation support to stdlib parser`

### Phase 2: Multi-Page Documentation Structure

**Status: Not Started** **Priority: CRITICAL** - Must complete before Phase 3
(Mermaid Diagrams)

#### Rationale

The single-page documentation approach has several critical issues:

1. **Performance**: Single page with 100+ tables is slow to load and navigate
2. **Diagram readability**: Package-level diagrams with >50 tables are illegible
3. **Navigation**: Users can't efficiently find specific modules or tables
4. **Maintenance**: One massive file is hard to update and version control

#### Task 2.1: Design Multi-Page Documentation Structure

**Files:** Design document, `infra/perfetto.dev/BUILD.gn` (new targets)
**Priority:** Critical (blocks diagram work) **Dependencies:** Phase 1 complete

**Proposed Structure (Rust-style):**

```
/docs/analysis/sql-tables/
  ├── index.md                    # Overview + global table list
  ├── prelude/
  │   ├── index.md               # Prelude package overview
  │   ├── core.md                # prelude.after_eof.core module
  │   ├── tracks.md              # prelude.after_eof.tracks module
  │   ├── cpu_scheduling.md      # prelude.after_eof.cpu_scheduling module
  │   └── ...
  ├── android/
  │   ├── index.md               # Android package overview
  │   ├── startup.md             # android.startup module
  │   ├── memory.md              # android.memory module
  │   └── ...
  └── ...
```

**Implementation Steps:**

- [ ] Design URL structure and navigation hierarchy
- [ ] Create build targets for per-module markdown generation
- [ ] Design global index page with package/module summary
- [ ] Plan cross-page linking and navigation
- [ ] Update build system to generate multiple output files
- [ ] **Design Review** before implementation

#### Task 2.2: Implement Multi-Page Documentation Generator

**Files:** `infra/perfetto.dev/src/gen_stdlib_docs_md.py`,
`infra/perfetto.dev/BUILD.gn` **Priority:** Critical **Dependencies:** Task 2.1

**Implementation Steps:**

- [ ] Refactor `gen_stdlib_docs_md.py` to support multi-page output
- [ ] Implement global index page generation
- [ ] Implement per-package index generation
- [ ] Implement per-module page generation
- [ ] Add navigation breadcrumbs and cross-links
- [ ] Update build targets to generate all pages
- [ ] Test navigation and linking
- [ ] **Commit:**
      `git add . && git commit -m "tp: implement multi-page stdlib documentation"`

#### Task 2.3: Update Documentation Site Integration

**Files:** `infra/perfetto.dev/` site templates and configuration **Priority:**
High **Dependencies:** Task 2.2

**Implementation Steps:**

- [ ] Update site navigation to include stdlib module pages
- [ ] Implement search indexing for multi-page docs
- [ ] Add breadcrumb navigation in site templates
- [ ] Configure URL routing for new page structure
- [ ] Test site integration and navigation
- [ ] **Commit:**
      `git add . && git commit -m "tp: integrate multi-page stdlib docs into site"`

### Phase 3: Mermaid Diagram Integration

**Status: Blocked** (waiting for Phase 2 completion)

#### Task 3.1: Port Table Relationship Logic to Python

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

---

## Implementation Status and Findings (as of 2025-10-09)

### Completed Work

**Branch:** `dev/lalitm/stdlib-docs-mermaid-generation`

**Completed Commits:**

1. `9df0833e9e` - tp: reorganize prelude into focused modules
2. `c324ab46cb` - tp: add module documentation support to stdlib parser
3. `e9ca2a4121` - tp: add mermaid diagram generation for stdlib tables
4. `d98f1b670f` - tp: integrate table relationship diagrams into docs

### Critical Findings from Initial Implementation

#### 1. Single-Page Documentation is Unusable

The current approach generates one massive markdown file with all stdlib
documentation:

- **40+ prelude tables**
- **50+ android tables**
- **Total: 100+ tables** on one page

**Problems:**

- Page is slow to load
- Difficult to navigate
- Poor user experience
- Hard to maintain

**Solution Required:** Rust-style multi-page documentation with:

- Global index page listing all packages/modules
- Per-package index pages
- Per-module detail pages

#### 2. Package-Level Diagrams are Illegible

Packages like `android` with 50+ tables produce mermaid diagrams that are:

- Tiny and unreadable
- Provide no useful information
- Cluttering the page

**Example:** The `android` package diagram attempts to show all 50+ tables and
their relationships in one graph, resulting in microscopic text and overlapping
edges.

**Solution Required:**

- **Remove package-level diagrams** for packages with >20 tables
- **Keep module-level diagrams** (scoped to 5-15 tables each)
- **Add focused subgraphs** for large modules

#### 3. Cross-Package Relationships Not Visualized

Many stdlib tables reference tables in other packages via JOINID:

- Android tables → prelude's `process`/`thread` tables
- Memory tables → `heap_graph` tables
- But these relationships are **not shown** in the diagrams

**Current limitation:** Mermaid generator only shows intra-module or
intra-package relationships, missing the important cross-package connections.

**Solution Required:**

- Text-based cross-reference links (not diagrams)
- "Related Tables" sections listing cross-package references
- Consider separate "Architecture Overview" diagram showing major package
  relationships

### Revised Implementation Plan

**Critical Change:** Multi-page documentation (Phase 2) must be implemented
**BEFORE** continuing with mermaid diagrams. The current single-page approach
makes diagrams unusable.

**Recommended Next Steps:**

1. **HOLD diagram work** - Current implementation on branch can serve as
   prototype
2. **Implement Phase 2** - Multi-page documentation structure
3. **Resume diagram work** - Once pages are scoped appropriately, add diagrams
   to individual module pages

**Rationale:** Diagrams are only useful when scoped to individual module pages.
Putting 50-table diagrams on a single page provides no value and creates a poor
user experience.

### Lessons Learned

1. **Start with page structure, not features** - Should have implemented
   multi-page docs first
2. **Diagram scope matters** - Module-level diagrams (5-15 tables) are useful;
   package-level diagrams (50+ tables) are not
3. **Cross-package visualization needs different approach** - Can't use same
   technique as intra-module relationships
4. **Test with real data** - Implementing against `android` package (50+ tables)
   revealed problems that weren't apparent with smaller modules

### Updated Progress Tracking

**Phase 1: Foundation & Refactoring** - ✅ COMPLETE

- [x] Task 1.1: Reorganize Prelude Module Structure
- [x] Task 1.2: Add Module Documentation Support to Parser

**Phase 2: Multi-Page Documentation Structure** - ⏸️ NOT STARTED (CRITICAL)

- [ ] Task 2.1: Design Multi-Page Documentation Structure
- [ ] Task 2.2: Implement Multi-Page Documentation Generator
- [ ] Task 2.3: Update Documentation Site Integration

**Phase 3: Mermaid Diagram Integration** - ⚠️ PARTIAL/ON HOLD

- [x] Task 3.1: Port Table Relationship Logic to Python (done, but needs
      refinement)
- [x] Task 3.2: Integrate Mermaid Generation (done, but blocked on multi-page
      docs)
- **Status:** Prototype complete but unusable without multi-page structure

**Next Action:** Focus on Phase 2 (multi-page docs) before continuing diagram
work.
