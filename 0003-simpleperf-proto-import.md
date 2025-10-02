# Simpleperf Proto Import Support

**Authors:** @lalitm

**Status:** Draft

## Problem

Perfetto currently lacks support for importing profiling data from Android's Simpleperf tool in its protobuf format. Simpleperf is a widely-used native profiling tool on Android that can capture CPU profiles with call stacks. The tool can export data in a custom protobuf format (via `simpleperf report-sample --protobuf`), but Perfetto cannot currently parse this format.

Without this capability, users who want to analyze Simpleperf profiles in Perfetto's UI or query them using SQL need to manually convert the data or use separate tools, creating friction in the profiling workflow.

## Decision

Implement a new importer in Trace Processor to natively support the Simpleperf protobuf format. This will allow users to directly load Simpleperf profiles into Perfetto for analysis.

## Design

### File Format

The Simpleperf protobuf format follows this structure:

```
char magic[10] = "SIMPLEPERF";
LittleEndian16(version) = 1;
LittleEndian32(record_size_0)
message Record(record_0) (having record_size_0 bytes)
LittleEndian32(record_size_1)
message Record(record_1) (having record_size_1 bytes)
...
LittleEndian32(record_size_N)
message Record(record_N) (having record_size_N bytes)
LittleEndian32(0)  // End marker
```

Each `Record` message is defined in `cmd_report_sample.proto` and can contain:

- **Sample**: CPU samples with timestamps, thread IDs, event counts, and call chains
- **Thread**: Thread metadata (TID, PID, thread name)
- **File**: ELF file paths and symbol tables
- **MetaInfo**: Global metadata (event types, app package name, build info)
- **LostSituation**: Information about dropped samples
- **ContextSwitch**: Thread scheduling events (when `--trace-offcpu` is used)

A key advantage of this format is that Simpleperf performs symbolization before export, so symbol names are already resolved in the File records. Note that obfuscated symbols are not deobfuscated - that remains the responsibility of post-processing tools.

### Implementation Components

#### 1. Proto Definitions

Import the Simpleperf proto definition (`cmd_report_sample.proto`) into `protos/third_party/simpleperf/` with namespace `perfetto.third_party.simpleperf.proto` to avoid conflicts with other proto definitions.

The BUILD.gn configuration generates:
- Zero-copy proto parsers for efficient parsing
- C++ bindings for any helpers needed
- A descriptor file (`simpleperf.descriptor`) for diff test infrastructure

#### 2. Tokenizer (`simpleperf_proto_tokenizer.{cc,h}`)

The tokenizer is responsible for parsing the binary file format and extracting individual records. It uses `TraceBlobViewReader` for efficient streaming parsing without copying data unnecessarily.

**State Machine:**

- `kExpectingMagic`: Read and validate "SIMPLEPERF" magic (10 bytes)
- `kExpectingVersion`: Read and validate version number (2 bytes, must be 1)
- `kExpectingRecordSize`: Read 32-bit little-endian record size
- `kExpectingRecord`: Read the protobuf record data of the specified size
- `kFinished`: End marker (size=0) encountered, no more records

**Record Processing:**

Records are handled differently based on their type:

**Metadata Records (File, MetaInfo, LostSituation):**
- Processed **directly in the tokenizer** during `ParseRecord()`
- Not sent through the trace sorter
- Ensures metadata is available before any timestamped events that reference it

Example for File records:
```cpp
if (record.has_file()) {
  File::Decoder file(record.file());
  std::string path(file.path().data, file.path().size);
  context_->mapping_tracker->CreateDummyMapping(path);

  // Store symbol table for later resolution
  for (auto symbol : file.symbol()) {
    StoreSymbol(file.id(), symbol_index++, symbol);
  }

  // Don't push to sorter - already processed
  return base::OkStatus();
}
```

**Thread Records:**
- Special case: while threads don't have explicit timestamps, they need to respect PID/TID reuse
- Use the timestamp of the **most recently seen timestamped event** (Sample or ContextSwitch)
- Wrapped in `SimpleperfProtoEvent` and pushed to trace sorter with this timestamp
- Ensures thread metadata updates happen at the correct point in time

```cpp
if (record.has_thread()) {
  SimpleperfProtoEvent event;
  event.ts = last_seen_timestamp_;  // From most recent Sample/ContextSwitch
  event.record_data = std::move(record_data);
  stream_->Push(event.ts, std::move(event));
  return base::OkStatus();
}
```

**Timestamped Records (Sample, ContextSwitch):**
- Extract timestamp from the record
- Track as `last_seen_timestamp_` for subsequent Thread records
- Wrapped in `SimpleperfProtoEvent` and pushed to trace sorter
- Processed later by the parser in timestamp order

```cpp
if (record.has_sample()) {
  Sample::Decoder sample(record.sample());
  int64_t ts = sample.time();
  last_seen_timestamp_ = ts;  // Track for Thread records

  SimpleperfProtoEvent event;
  event.ts = ts;
  event.record_data = std::move(record_data);
  stream_->Push(ts, std::move(event));
  return base::OkStatus();
}
```

This design ensures:
1. File/metadata available immediately
2. Thread updates happen at the correct timestamp to handle PID/TID reuse
3. Samples and context switches properly sorted by timestamp
4. Thread metadata appears before/at the samples that reference it

#### 3. Parser (`simpleperf_proto_parser.{cc,h}`)

The parser receives sorted timestamped events (Sample, ContextSwitch, and Thread records) from the trace sorter. File/MetaInfo/LostSituation records have already been processed by the tokenizer.

**Thread Records:**

Thread records are processed in timestamp order to handle PID/TID reuse correctly:

```cpp
if (record.has_thread()) {
  Thread::Decoder thread(record.thread());
  uint32_t tid = thread.thread_id();
  uint32_t pid = thread.process_id();

  context_->process_tracker->UpdateThread(tid, pid);

  if (thread.has_thread_name()) {
    StringId name_id = context_->storage->InternString(thread.thread_name());
    context_->process_tracker->UpdateThreadName(
        tid, name_id, ThreadNamePriority::kOther);
  }
  return;
}
```

**Sample Records:**

Sample records contain the actual profiling data:

```cpp
Sample::Decoder sample(record.sample());

int64_t ts = sample.time();
uint32_t tid = sample.thread_id();
uint32_t event_type_id = sample.event_type_id();
uint64_t event_count = sample.event_count();

// Process callchain
std::vector<CallsiteId> callsites;
CallsiteId parent_callsite = std::nullopt;

for (auto entry : sample.callchain()) {
  CallChainEntry::Decoder call(entry);
  uint64_t vaddr = call.vaddr_in_file();
  uint32_t file_id = call.file_id();
  int32_t symbol_id = call.symbol_id();

  // Resolve symbol name from file's symbol table (populated by tokenizer)
  std::optional<StringId> symbol_name = ResolveSymbol(file_id, symbol_id);

  // Create frame and callsite
  FrameId frame = CreateFrame(vaddr, file_id, symbol_name);
  CallsiteId callsite = CreateCallsite(frame, parent_callsite);

  parent_callsite = callsite;
}

// Insert into perf_sample table
context_->storage->mutable_perf_sample_table()->Insert({
  ts, tid, parent_callsite, event_count
});
```

**ContextSwitch Records:**

When `--trace-offcpu` is used, Simpleperf records thread scheduling events:

```cpp
ContextSwitch::Decoder cs(record.context_switch());

int64_t ts = cs.time();
uint32_t tid = cs.thread_id();
bool switch_on = cs.switch_on();

// Import into sched_slice table or separate off-cpu tracking
```

#### 4. Diff Test Infrastructure

Add `SimpleperfProto` class to `python/generators/diff_tests/testing.py`:

```python
@dataclass
class SimpleperfProto:
  """Represents a simpleperf_proto binary file with inline generation."""
  records: List[str]  # List of textproto strings for Record messages
```

The trace generator in `trace_generator.py` serializes this into binary format:

```python
def serialize_simpleperf_proto_trace(self, simpleperf_trace, out_stream):
  # Write header
  out_stream.write(b"SIMPLEPERF")
  out_stream.write(struct.pack('<H', 1))  # version

  # Write each record
  for record_textproto in simpleperf_trace.records:
    record = parse_textproto(record_textproto)
    record_bytes = record.SerializeToString()

    out_stream.write(struct.pack('<I', len(record_bytes)))
    out_stream.write(record_bytes)

  # End marker
  out_stream.write(struct.pack('<I', 0))
```

Tests can be written as:

```python
def test_simpleperf_basic(self):
  return DiffTestBlueprint(
    trace=SimpleperfProto(records=[
      """
      thread {
        thread_id: 1234
        process_id: 5678
        thread_name: "MyThread"
      }
      """,
      """
      file {
        id: 1
        path: "/system/lib64/libc.so"
        symbol: "malloc"
        symbol: "free"
        symbol: "memcpy"
      }
      """,
      """
      sample {
        time: 1000000000
        thread_id: 1234
        event_count: 100
        callchain {
          vaddr_in_file: 0x1000
          file_id: 1
          symbol_id: 0  # malloc
        }
      }
      """
    ]),
    query="SELECT ts, tid FROM perf_sample",
    out=Csv("""
    "ts","tid"
    1000000000,1234
    """)
  )
```

#### 5. Trace Type Detection

Add `kSimpleperfProtoTraceType` to the `TraceType` enum and register detection logic:

```cpp
// In trace_reader_registry.cc
bool RequiresMagicDetection(TraceType t) {
  return t == kSimpleperfProtoTraceType || ...;
}

std::optional<TraceType> GuessTraceType(const uint8_t* data, size_t size) {
  if (size >= 10 && memcmp(data, "SIMPLEPERF", 10) == 0) {
    return kSimpleperfProtoTraceType;
  }
  // ... other type detection
}
```

Register the reader in `TraceProcessorImpl`:

```cpp
context()->reader_registry->RegisterTraceReader<
    simpleperf_proto_importer::SimpleperfProtoTokenizer>(
    kSimpleperfProtoTraceType);
```

### Data Flow

```
Simpleperf Binary File
        ↓
SimpleperfProtoTokenizer
  - Validates magic/version
  - Processes metadata records immediately:
    * File → mapping_tracker + symbol tables
    * MetaInfo → metadata
    * LostSituation → stats
  - Extracts timestamped records (Sample, ContextSwitch, Thread)
  - Thread records use timestamp of most recent Sample/ContextSwitch
  - Assigns timestamps and pushes to sorter
        ↓
TraceSorter
  - Orders Sample, ContextSwitch, and Thread records by timestamp
        ↓
SimpleperfProtoParser
  - Thread records → process_tracker (timestamp-aware for PID/TID reuse)
  - Sample records → perf_sample table
  - ContextSwitch → sched_slice table
        ↓
TraceStorage
  - Query via SQL
  - Visualize in UI
```

### Phased Implementation

**Phase 1 (Current):**
- File format parsing and validation
- Thread/process metadata import
- Basic mapping creation for ELF files
- Infrastructure for diff testing

**Phase 2:**
- Full sample parsing with call stacks
- Symbol table support and resolution
- Event type metadata handling

**Phase 3:**
- Context switch event import
- Lost sample tracking and statistics
- Integration with Perfetto UI flamegraph

## Open questions

- How should we handle symbol deduplication across multiple File records with the same path?
- Should context switches from Simpleperf be merged with ftrace sched events or kept separate?
- What priority level should Simpleperf thread names have compared to other sources (currently using `kOther`)?
- Should we validate that symbol_id references are within bounds of the File's symbol table?
