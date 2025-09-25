# Trace Clock Synchronization for Multi-Trace Merging

- **Status**: draft
- **Author**: lalitm

## Overview

This document outlines the design and implementation plan for handling clock
synchronization when merging multiple trace files in ZIP/TAR archives. The goal
is to establish a unified global clock domain that allows accurate temporal
correlation of events across different trace sources, building upon the existing
single-trace clock synchronization infrastructure.

## Key Terminology

This proposal introduces several key concepts that are used throughout the
document:

### Clock Domain Categories

- **Explicit Clocks**: Trace formats that contain definitive clock domain
  information within the trace data itself (e.g., Proto traces with
  `ClockSnapshot` packets). These clocks cannot and should not be overridden by
  external metadata.

- **Semi-explicit Clocks**: Trace formats that have established clock domain
  conventions hardcoded in their parsers, but can be overridden via external
  metadata when necessary (e.g., Systrace defaults to `BUILTIN_CLOCK_BOOTTIME`
  but can be configured to use other clocks).

- **Non-explicit Clocks**: Trace formats that contain no clock domain
  information and require external metadata specification to be properly
  synchronized (e.g., Chrome JSON traces).

### Multi-Trace Concepts

- **Sidecar Metadata**: A JSON file (`clock_metadata.json`) included alongside
  trace files in ZIP/TAR archives that specifies clock domains and
  synchronization information for traces that need it.

- **Primary Trace**: The trace file that defines the global clock domain for the
  entire multi-trace archive. All other traces are synchronized relative to this
  trace's timeline.

- **Global Clock Domain**: The unified time reference established by the primary
  trace, into which all other trace timestamps are converted for display on a
  common timeline.

### Perfetto Architecture Terms

- **ArchiveEntry**: A sorting mechanism used to determine processing order of
  files within ZIP/TAR archives, ensuring proto traces are processed first.

- **ForwardingTraceParser**: The trace processor component that handles
  individual trace files within multi-trace archives, delegating to
  format-specific parsers.

- **ClockSynchronizer**: The existing single-trace clock synchronization
  infrastructure that handles multiple clock domains within individual traces
  via graph-based pathfinding.

### Clock Types and Components

- **BUILTIN_CLOCK_BOOTTIME**: System boot time clock that includes time spent in
  suspend/sleep states, preferred for trace synchronization.

- **BUILTIN_CLOCK_MONOTONIC**: Monotonic clock that excludes suspend time but
  provides steady progression, commonly used as a fallback.

- **ClockSnapshot**: Proto message containing timestamp readings from multiple
  clock domains at the same moment, used to establish relationships between
  different clocks.

### Error Handling Concepts

- **Soft Errors**: Non-fatal errors that allow trace processing to continue
  while dropping problematic data and providing user feedback through
  statistics.

- **Graceful Degradation**: The ability to continue processing traces even when
  some components fail, preserving as much data as possible while clearly
  reporting what was lost.

- **Hard Errors**: Fatal errors that completely stop trace processing, avoided
  in this proposal in favor of soft error approaches.

### Trace Format Terms

- **Proto Traces**: Traces using Perfetto's native protobuf format, which
  contain rich metadata including explicit clock synchronization information.

- **ZIP/TAR Archives**: Compressed archive formats that can contain multiple
  trace files, requiring special handling to process each contained trace
  appropriately.

- **Tokenizer**: The component responsible for parsing raw trace data and
  converting it into structured events that the trace processor can analyze.

## Current State Analysis

### Existing Architecture

Perfetto already has sophisticated clock synchronization for single traces:

- **ClockSynchronizer**: Handles multiple clock domains within a single trace
  via graph-based pathfinding
- **Multi-trace UI Support**: `MultiTraceOpen` plugin allows loading multiple
  trace files
- **Archive Processing**: ZIP/TAR files are extracted and each trace processed
  independently
- **Proto Leadership**: Proto traces are processed first due to their rich clock
  information

**Current Processing Pipeline:**

```
ZIP/TAR Archive → Extract Files → Sort by ArchiveEntry → ForwardingTraceParser per file
```

### Current Problems

1. **Incorrect Clock Domain Assumptions**: All traces are forced into the same
   global clock domain (typically `BUILTIN_CLOCK_BOOTTIME`) regardless of their
   actual clock source. For example, Chrome JSON traces implicitly assume
   `BOOTTIME` even when they may actually use a different clock domain.
2. **Incorrect Temporal Correlation**: Traces with different actual clock
   domains appear on the same timeline with wrong relative timing, making
   cross-trace analysis unreliable or misleading.
3. **Missing Clock Metadata Handling**: No way to specify the actual clock
   domain used by traces that don't explicitly declare it (Category 2 and 3
   traces).
4. **No User Feedback on Clock Issues**: Users have no visibility into potential
   clock domain mismatches or way to correct them.

## Enhancement Goals

### Primary Objectives

1. **Three-Category Clock Framework**: Handle explicit, semi-explicit, and
   non-explicit clock traces appropriately based on their format characteristics
2. **User-Specified Clock Correction**: Allow users to specify correct clock
   domains and offsets for traces via JSON metadata
3. **Graceful Degradation**: Drop timestamped events for traces lacking required
   clock metadata while preserving non-temporal data
4. **UI Integration**: Enable users to configure clock relationships through the
   multi-trace dialog
5. **Selective Backward Compatibility**: Single-trace behavior remains
   unchanged. Multi-trace behavior is preserved for explicit and semi-explicit
   traces, but non-explicit traces (e.g., JSON) will require metadata or drop
   timestamped events.

### Success Criteria

- Multi-trace archives with mixed clock domains display on unified timeline
- Missing clock metadata results in soft errors with clear user feedback
- Proto traces drive clock decisions for other formats in same archive
- Users can configure clock relationships through intuitive UI workflow

## Clock Categorization Framework

Based on analysis of trace format parsers, traces fall into three categories:

### Category 1: Explicit Clocks (no override allowed)

These formats contain explicit clock domain information that must be respected:

- **kProtoTraceType**: Contains `ClockSnapshot` packets with explicit clock
  relationships
- **kPerfDataTraceType**: Has explicit perf clock information
  (`BUILTIN_CLOCK_PERF` when available)
- **kAndroidLogcatTraceType**: Contains explicit Android log timestamp clock
  information
- **kArtMethodTraceType**: Has explicit ART runtime timing clocks
- **kSymbolsTraceType**: No clocks (symbols only), no override needed

### Category 2: Semi-explicit Clocks (convention with override)

These formats have established clock conventions but allow metadata override:

- **kSystraceTraceType**: Defaults to `BUILTIN_CLOCK_BOOTTIME` or specified
  ftrace clock
- **kGeckoTraceType**: Defaults to `BUILTIN_CLOCK_MONOTONIC`
- **kFuchsiaTraceType**: Has Fuchsia-specific clock conventions
- **kNinjaLogTraceType**: Build timing conventions
- **kInstrumentsXmlTraceType**: Apple Instruments conventions
- **kPerfTextTraceType**: Perf tool text output conventions
- **kArtHprofTraceType**: Heap profiling timing conventions
- **kAndroidDumpstateTraceType**: Android dumpstate timing conventions

### Category 3: Non-explicit Clocks (requires sidecar metadata)

These formats lack inherent clock information and require external
specification:

- **kJsonTraceType**: Chrome JSON traces have no explicit clock specification

### Container Types (special handling)

- **kZipFile, kTarTraceType, kCtraceTraceType**: Contain other traces
- **kGzipTraceType**: Compressed single trace
- **kAndroidBugreportTraceType**: Special ZIP handling for Android bug reports

## Implementation Plan

### Phase 1: ClockTracker Architecture Refactoring

**Status: Not Started**

#### Task 1.1: Separate ClockSynchronizer into Pure Algorithm and Coordination Layers

**Files:**

- `src/trace_processor/util/clock_synchronizer.h`
- `src/trace_processor/importers/common/clock_tracker.h`
- `src/trace_processor/importers/common/clock_tracker.cc`

**Priority:** High (foundational architectural change)

**Dependencies:** None

**Priority Field Explanation:** The Priority field indicates the importance and
urgency of completing each task. High priority tasks are foundational or
critical path items that block subsequent work. Medium priority tasks improve
user experience or enable additional features. This prioritization helps
developers understand which tasks should be completed first when resources are
limited.

**Current Architecture Problems:**

The current `ClockSynchronizer` conflates two distinct responsibilities:

1. **Pure clock domain conversion algorithms** (stable, testable graph
   algorithms)
2. **Multi-trace/multi-machine coordination policy** (heuristic-driven,
   evolving)

This makes it difficult to add multi-trace coordination without disrupting the
core algorithms.

**New Two-Layer Architecture:**

```cpp
// Layer 1: Pure clock graph algorithms (renamed ClockSynchronizer)
class ClockGraphEngine {
 public:
  // Core conversion between clock domains via graph pathfinding
  base::StatusOr<int64_t> Convert(ClockId from, int64_t timestamp, ClockId to);

  // Build clock relationships from snapshots
  base::Status AddClockSnapshot(const std::vector<ClockTimestamp>& clocks);

  // All existing ClockSynchronizer logic - BFS, caching, etc.
  // ... (pure algorithmic functions only)
};

// Layer 2: Multi-trace coordination (becomes the new ClockTracker)
class ClockTracker {
 public:
  // Multi-trace coordination - different behavior per category
  base::Status SetTraceTimeClock(TraceFileTable::Id file_id, ClockId clock_domain);
  ClockId GetConfiguredClock(TraceFileTable::Id file_id, ClockId default_clock);

  // Delegates to engine for conversions
  base::StatusOr<int64_t> ToTraceTime(ClockId clock_id, int64_t timestamp);
  base::Status AddClockSnapshot(const std::vector<ClockTimestamp>& clocks);

  // Configuration management
  void SetTraceClockConfig(TraceFileTable::Id file_id, ClockId clock_id);

 private:
  std::unique_ptr<ClockGraphEngine> engine_;
  TraceProcessorContext* context_;
  // Multi-trace configuration state
};
```

**Parser Usage Patterns by Category:**

**Category 1 (Explicit - Proto, Perf with explicit clocks):**

```cpp
// Uses trace's own explicit clock information
context_->clock_tracker->SetTraceTimeClock(file_id, trace_specified_clock);
```

**Category 2 (Semi-explicit - Gecko, Systrace):**

```cpp
// Gets configured clock or falls back to format default
ClockId clock = context_->clock_tracker->GetConfiguredClock(file_id, BUILTIN_CLOCK_MONOTONIC);
context_->clock_tracker->SetTraceTimeClock(file_id, clock);
```

**Category 3 (Non-explicit - JSON):**

```cpp
// Must have configuration or drops events
ClockId clock = context_->clock_tracker->GetConfiguredClock(file_id, std::nullopt);
if (!clock.has_value()) {
  // Drop timestamped events, increment stats
  return DropTimestampedEvents();
}
```

**Implementation Steps:**

- [ ] Extract pure algorithm functions from ClockSynchronizer into
      ClockGraphEngine
- [ ] Create new ClockTracker class with coordination methods
- [ ] Update typedef in clock_tracker.h to use new ClockTracker class
- [ ] Migrate existing ClockSynchronizer usage to delegate through ClockTracker
- [ ] Add multi-trace configuration state management to ClockTracker
- [ ] Test that single-trace behavior is preserved with new architecture
- [ ] **Commit:**
      `tp: refactor ClockSynchronizer into algorithm and coordination layers`

#### Task 1.2: Add Multi-Trace Clock Statistics

**Files:** `src/trace_processor/storage/stats.h` **Priority:** High (essential
for user feedback)

**Dependencies:** Task 1.1

**New Statistics:**

```cpp
F(multi_trace_clock_metadata_missing,   kSingle,  kError,    kTrace,
   "Clock metadata file missing for multi-trace archive with non-explicit clock traces"), \
F(multi_trace_clock_sync_failed,        kSingle,  kError,    kTrace,
   "Failed to synchronize clocks between traces in multi-trace archive"), \
F(multi_trace_events_dropped_no_clock,  kSingle,  kDataLoss, kTrace,
   "Events dropped from traces lacking required clock metadata"), \
```

**Implementation Steps:**

- [ ] Add statistics definitions to `PERFETTO_TP_STATS` macro
- [ ] Update stats documentation and tooling
- [ ] Add increment calls in appropriate error conditions
- [ ] Test statistics collection in multi-trace scenarios
- [ ] **Commit:** `tp: add statistics for multi-trace clock synchronization`

### Phase 2: Parser Integration with New ClockTracker

**Status: Not Started**

#### Task 2.1: Update All Parsers to Use New ClockTracker Interface

**Files:**

- All trace format parsers
- `ForwardingTraceParser`

**Priority:** High (core functionality)

**Dependencies:** Phase 1 complete

**Parser Refactoring by Category:**

**Category 1 Parsers (Explicit Clocks):** Update parsers that currently call
`SetTraceTimeClock()` directly:

```cpp
// Current proto parser:
context_->clock_tracker->SetTraceTimeClock(static_cast<ClockTracker::ClockId>(evt.primary_trace_clock()));

// Updated proto parser:
context_->clock_tracker->SetTraceTimeClock(file_id, static_cast<ClockTracker::ClockId>(evt.primary_trace_clock()));
```

**Category 2 Parsers (Semi-explicit Clocks):** Update parsers to fetch
configured clock or use default:

```cpp
// Current Gecko parser:
context_->clock_tracker->SetTraceTimeClock(protos::pbzero::ClockSnapshot::Clock::MONOTONIC);

// Updated Gecko parser:
ClockId clock = context_->clock_tracker->GetConfiguredClock(file_id, protos::pbzero::ClockSnapshot::Clock::MONOTONIC);
context_->clock_tracker->SetTraceTimeClock(file_id, clock);
```

**Category 3 Parsers (Non-explicit Clocks):** Update parsers to require
configuration or drop events:

```cpp
// Updated JSON parser:
auto clock_result = context_->clock_tracker->GetConfiguredClock(file_id, std::nullopt);
if (!clock_result.has_value()) {
  context_->storage->IncrementStats(stats::multi_trace_events_dropped_no_clock);
  return base::OkStatus(); // Drop event, continue parsing
}
ClockId clock = *clock_result;
// Process event with configured clock
```

**Trace File ID Integration:** Update `ForwardingTraceParser` to pass trace file
ID to parsers:

```cpp
// ForwardingTraceParser provides file ID to individual parsers
class JsonTraceTokenizer {
 public:
  JsonTraceTokenizer(TraceProcessorContext* context, TraceFileTable::Id file_id)
      : context_(context), file_id_(file_id) {}
 private:
  TraceFileTable::Id file_id_;
};
```

**Implementation Steps:**

- [ ] Update all Category 1 parsers to pass file_id to SetTraceTimeClock()
- [ ] Update all Category 2 parsers to use GetConfiguredClock() with fallback
- [ ] Update all Category 3 parsers to require configuration or drop events
- [ ] Modify ForwardingTraceParser to provide file_id to individual parsers
- [ ] Test each parser category with representative traces
- [ ] Verify single-trace behavior remains unchanged
- [ ] **Commit:** `tp: update all parsers to use new ClockTracker interface`

#### Task 2.2: Configure Individual Trace Parsers for Clock Handling

**Files:**

- `src/trace_processor/forwarding_trace_parser.cc`
- Individual tokenizers

**Priority:** High (enables per-trace clock configuration)

**Dependencies:** Task 2.1

**Clock Configuration Strategy:**

- **Category 1 (Explicit)**: No configuration needed, use trace's own clock info
- **Category 2 (Semi-explicit)**: Apply clock override from metadata if present
- **Category 3 (Non-explicit)**: Must have metadata or drop timestamped events

**Per-Tokenizer Modifications:**

```cpp
// Example: JsonTraceTokenizer modification
base::Status JsonTraceTokenizer::ParseEvent(const Json::Value& event) {
  if (event.has_timestamp()) {
    if (!has_clock_configuration_) {
      // Category 3 trace without metadata - drop event
      context_->storage->IncrementStats(stats::multi_trace_events_dropped_no_clock);
      return base::OkStatus();
    }
    // Apply clock configuration and process normally
    int64_t timestamp_ns = ApplyClockConfiguration(event.timestamp());
    // ... continue with normal processing
  }
  // Process non-timestamped data normally
}
```

**Implementation Steps:**

- [ ] Add clock configuration state to ForwardingTraceParser
- [ ] Propagate clock metadata to individual tokenizers
- [ ] Modify Category 2 tokenizers to respect clock overrides
- [ ] Modify Category 3 tokenizers to handle missing clock metadata gracefully
- [ ] Implement clock offset application during timestamp conversion
- [ ] Test clock configuration with representative traces from each category
- [ ] **Commit:**
      `tp: configure individual trace parsers for multi-trace clock handling`

### Phase 3: Sidecar Metadata Implementation

**Status: Not Started**

#### Task 3.1: Define and Implement Sidecar JSON Metadata Schema

**Files:**

- Archive processing
- JSON parsing infrastructure **Priority:** Medium (enables user configuration)

**Dependencies:** Phase 2 complete

**Metadata Schema Definition:**

```json
{
  "version": "1.0",
  "traces": [
    {
      "filename": "primary_trace.perfetto-trace",
      "clock_override": null,
      "is_primary": true
    },
    {
      "filename": "systrace.txt",
      "clock_override": "BUILTIN_CLOCK_MONOTONIC",
      "offset_from_primary_ns": 1500000000
    },
    {
      "filename": "chrome_trace.json",
      "clock_domain": "BUILTIN_CLOCK_BOOTTIME",
      "offset_from_primary_ns": -500000000
    }
  ]
}
```

**Key Schema Elements:**

- **filename**: Exact match with trace file name in archive
- **clock_override**: For Category 2 traces, overrides format default
- **clock_domain**: For Category 3 traces, specifies required clock
- **is_primary**: Designates which trace defines global timeline
- **offset_from_primary_ns**: Time offset relative to primary trace

**Archive Integration:**

```cpp
// In ZipTraceReader::NotifyEndOfFile()
base::Status ZipTraceReader::NotifyEndOfFile() {
  std::vector<util::ZipFile> files = zip_reader_.TakeFiles();

  // 1. Look for metadata file first
  auto metadata_it = std::find_if(files.begin(), files.end(),
    [](const util::ZipFile& file) {
      return file.name() == "clock_metadata.json";
    });

  if (metadata_it != files.end()) {
    ASSIGN_OR_RETURN(auto metadata, ParseClockMetadata(*metadata_it));
    ApplyClockConfiguration(metadata);
    files.erase(metadata_it);
  }

  // 2. Process traces with configuration applied
  // ... existing trace processing logic
}
```

**Implementation Steps:**

- [ ] Define JSON schema specification with validation
- [ ] Add metadata file detection to ZIP/TAR readers
- [ ] Implement JSON parsing with comprehensive error handling
- [ ] Connect parsed metadata to ClockTracker configuration methods
- [ ] Add metadata validation (filename matching, clock compatibility)
- [ ] Test metadata-driven configuration with sample multi-trace archives
- [ ] **Commit:**
      `tp: implement sidecar JSON metadata for multi-trace clock configuration`

#### Task 3.2: Error Reporting and User Guidance

**Files:**

- UI error handling
- trace processing error display

**Priority:** Medium (user experience improvement)

**Dependencies:** Task 3.1

**Error Reporting Enhancements:**

- **Clear Error Messages**: Translate statistics into user-friendly explanations
- **Actionable Guidance**: Suggest specific steps to resolve clock issues
- **Partial Success Display**: Show successfully processed traces even with some
  failures

**Error Message Examples:**

- `multi_trace_clock_metadata_missing` → "Some traces require clock
  configuration. Please specify clock settings for: chrome_trace.json"
- `multi_trace_events_dropped_no_clock` → "Dropped 1,234 events from traces
  lacking clock metadata. Timeline may be incomplete."

**Implementation Steps:**

- [ ] Add error message mapping for new statistics
- [ ] Create user guidance for common clock configuration scenarios
- [ ] Implement partial success display in trace viewer
- [ ] Add help documentation for multi-trace clock configuration
- [ ] Test error reporting with various failure scenarios
- [ ] **Commit:** `ui: improve error reporting for multi-trace clock issues`

### Phase 4: UI Integration and User Experience

**Status: Not Started**

#### Task 4.1: Extend Multi-Trace Dialog for Clock Configuration

**Files:**

- `ui/src/core_plugins/dev.perfetto.MultiTraceOpen/multi_trace_modal.ts`
- `ui/src/core_plugins/dev.perfetto.MultiTraceOpen/multi_trace_controller.ts`

**Priority:** Medium (user-facing functionality)

**Dependencies:** Phase 3 complete (needs JSON schema)

**UI Workflow Enhancement:**

1. **Detection Phase**: Analyze uploaded traces and identify Category 2/3 traces
   needing configuration
2. **Configuration Phase**: Present clock configuration UI for traces requiring
   metadata
3. **Generation Phase**: Generate `clock_metadata.json` from UI settings
4. **Display Phase**: Show generated JSON to user for copy/paste
5. **Processing Phase**: Use generated metadata for trace processing

**Clock Configuration UI Elements:**

- **Trace Analysis Display**: Show detected trace categories and current clock
  assumptions
- **Primary Trace Selection**: Radio buttons to choose which trace defines
  global clock domain
- **Clock Domain Override**: Dropdown for Category 2 traces to override format
  defaults
- **Clock Domain Specification**: Dropdown for Category 3 traces to specify
  required clocks
- **Relative Offset Input**: Numeric input fields for time offsets between
  traces
- **Metadata Preview**: Read-only JSON text area with generated configuration
- **Copy to Clipboard**: Button to copy generated metadata JSON

**TraceAnalyzer Integration:**

```typescript
interface TraceClockInfo {
  filename: string;
  category: "explicit" | "semi-explicit" | "non-explicit";
  defaultClock?: string;
  requiresConfiguration: boolean;
}

class MultiTraceController {
  analyzeTraceClocks(): TraceClockInfo[] {
    // Detect trace categories and clock requirements
  }

  generateClockMetadata(): string {
    // Generate JSON metadata from UI state
  }
}
```

**Implementation Steps:**

- [ ] Add trace clock category detection to MultiTraceController
- [ ] Create clock configuration UI components for each trace category
- [ ] Implement metadata JSON generation from UI configuration state
- [ ] Add JSON display and copy-to-clipboard functionality
- [ ] Integrate generated metadata with trace processing pipeline
- [ ] Test complete UI workflow with various multi-trace scenarios
- [ ] **Commit:** `ui: add clock configuration to multi-trace dialog`

### Phase 5: Testing and Validation

**Status: Not Started**

#### Task 5.1: Comprehensive Multi-Trace Testing

**Files:**

- Test infrastructure
- Example traces

**Priority:** High (ensure correctness)

**Dependencies:** All previous phases

**Testing Scenarios:**

- [ ] **Single trace archives**: Verify no regression in existing behavior
- [ ] **Mixed category archives**: Explicit + Semi-explicit + Non-explicit
      traces
- [ ] **Proto leadership**: Proto traces driving clock decisions for other
      formats
- [ ] **Missing metadata**: Graceful degradation with appropriate statistics
- [ ] **Invalid metadata**: Robust error handling for malformed JSON
- [ ] **Clock offset scenarios**: Positive/negative/zero offsets between traces
- [ ] **UI workflow**: End-to-end testing of dialog-generated metadata
- [ ] **Large archives**: Performance testing with many trace files

#### Task 4.2: Documentation and Examples

**Files:**

- Documentation
- example configurations

**Priority:** Medium (user enablement)

**Dependencies:** Task 4.1

**Documentation Deliverables:**

- [ ] **Clock synchronization user guide**: Step-by-step workflow for
      multi-trace setup
- [ ] **Metadata specification**: Complete JSON schema reference with examples
- [ ] **Trace format clock reference**: Document clock behavior for each
      supported format
- [ ] **Troubleshooting guide**: Common issues and solutions
- [ ] **Example configurations**: Representative metadata files for typical
      scenarios

## Progress Tracking

### Phase 1 Progress: 0/2 tasks complete

- [ ] Task 1.1: Separate ClockSynchronizer into Pure Algorithm and Coordination
      Layers
- [ ] Task 1.2: Add Multi-Trace Clock Statistics

### Phase 2 Progress: 0/2 tasks complete

- [ ] Task 2.1: Update All Parsers to Use New ClockTracker Interface
- [ ] Task 2.2: Configure Individual Trace Parsers for Clock Handling

### Phase 3 Progress: 0/2 tasks complete

- [ ] Task 3.1: Define and Implement Sidecar JSON Metadata Schema
- [ ] Task 3.2: Error Reporting and User Guidance

### Phase 4 Progress: 0/2 tasks complete

- [ ] Task 4.1: Extend Multi-Trace Dialog for Clock Configuration
- [ ] Task 4.2: Documentation and Examples

### Phase 5 Progress: 0/1 tasks complete

- [ ] Task 5.1: Comprehensive Multi-Trace Testing

## Key Implementation Notes

### Clock Selection Priority

**Global Clock Domain Selection:**

1. **Primary trace specification**: Use `is_primary: true` from metadata
2. **First trace fallback**: First trace in archive defines global clock if no
   primary specified
3. **Proto leadership**: Proto traces preferred as primary due to rich clock
   information
4. **Stable behavior**: Once selected, global clock domain should not change
   during processing

### Error Handling Philosophy

**Soft Error Approach:**

- **Continue processing**: Drop problematic events but continue trace analysis
- **User visibility**: Increment statistics and show clear error messages
- **Partial success**: Display successfully processed data alongside error
  reports
- **Actionable feedback**: Provide specific steps to resolve configuration
  issues

### Performance Considerations

**Metadata Processing:**

- **Early parsing**: Process metadata before trace files to avoid redundant work
- **Validation caching**: Cache metadata validation results to avoid repeated
  checks
- **Memory efficiency**: Store only necessary clock configuration in trace
  processor context
- **Minimal overhead**: Ensure single-trace performance remains unchanged

### Backward Compatibility

**Behavior Changes and Compatibility:**

- **Single trace files**: No changes to current processing pipeline
- **Multi-trace explicit traces**: Continue to work as before (proto, perf,
  etc.)
- **Multi-trace semi-explicit traces**: Continue to work with format defaults,
  but can be overridden with metadata
- **Multi-trace non-explicit traces**: **Breaking change** - JSON traces and
  others will drop timestamped events without metadata (previously worked with
  assumed clock domain)
- **API compatibility**: No changes to existing trace processor APIs
- **Statistics available**: New stats provide visibility into dropped events

## Future Enhancements (Out of Scope)

### Potential Follow-up Work

- **Automatic Clock Detection**: Machine learning approaches to infer clock
  relationships
- **Cross-Device Synchronization**: Handle traces from multiple devices with NTP
  sync
- **Interactive Clock Adjustment**: UI tools for manual fine-tuning of clock
  relationships
- **Clock Quality Assessment**: Confidence scoring for automatic clock
  synchronization
- **Distributed Trace Correlation**: Handle traces from microservices
  architectures

---

## Meeting Notes Integration

This proposal directly addresses requirements identified in the September 24,
2025 meeting:

### Critical Requirements Addressed

1. **Global Clock Definition**: Primary trace approach ensures global clock
   established before processing
2. **Proto Format Priority**: Existing ArchiveEntry sorting ensures proto traces
   processed first
3. **Soft Error Preference**: Statistics-based approach allows partial trace
   display with clear errors
4. **Metadata-Driven Configuration**: JSON sidecar provides external clock
   specification
5. **Three-Category Framework**: Handles explicit/semi-explicit/non-explicit
   clock traces appropriately
6. **UI Integration**: Multi-trace dialog generates metadata automatically
7. **Graceful Degradation**: Drop events rather than fail completely for missing
   clock metadata

### Non-Interactive Processing Maintained

- Trace processor remains non-interactive for automated use cases
- Metadata provides all necessary clock configuration externally
- Default behaviors preserve existing automation workflows
- Statistics provide programmatic access to processing results

## Git Workflow and CL Stacking

### Branch Strategy

Use `git new-branch --parent <parent-branch> dev/lalitm/<branch-name>` to create
a proper CL stack:

```bash
# Start from main branch
git checkout main
git new-branch dev/lalitm/multi-trace-clock-metadata-schema

# After completing Task 1.1, commit and create next branch
git add . && git commit -m "tp: add sidecar JSON metadata schema for multi-trace clock sync"
git new-branch --parent dev/lalitm/multi-trace-clock-metadata-schema dev/lalitm/multi-trace-clock-stats

# Continue pattern for each major task...
```

### Commit Points

Each task should result in a separate commit/CL:

- **Task 1.1** →
  `tp: add sidecar JSON metadata schema for multi-trace clock sync`
- **Task 1.2** → `tp: add statistics for multi-trace clock synchronization`
- **Task 2.1** → `tp: integrate clock metadata parsing into archive processing`
- **Task 2.2** →
  `tp: configure individual trace parsers for multi-trace clock handling`
- **Task 3.1** → `ui: add clock configuration to multi-trace dialog`
- **Task 3.2** → `ui: improve error reporting for multi-trace clock issues`

### Testing and Validation

Before each commit:

```bash
# Build and test core functionality
tools/ninja -C out/linux_clang_release -k 10000 trace_processor_shell perfetto_unittests

# Run unit tests for modified components
out/linux_clang_release/perfetto_unittests --gtest_brief=1

# Test archive processing with sample multi-trace files
# Test UI functionality with various trace combinations
```

## Usage Instructions for Incremental Implementation

1. **Start with Phase 1** - Foundational metadata and statistics infrastructure
2. **Create proper CL stack** - Use `git new-branch --parent` for each task
3. **Commit after each task** - Each task should be independently reviewable and
   testable
4. **Update progress checkboxes** as tasks are completed in this document
5. **Validate incrementally** - Each phase should leave the system in a working
   state
6. **Test thoroughly** - Multi-trace scenarios are complex and require
   comprehensive testing
7. **Update documentation** - Keep this design document current as
   implementation reveals new requirements

This proposal provides a comprehensive roadmap for implementing multi-trace
clock synchronization while maintaining backward compatibility and providing
excellent user experience through both programmatic and UI interfaces.
