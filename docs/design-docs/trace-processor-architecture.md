# Trace Processor Architecture

This document explains how Perfetto's trace processor works, from ingesting raw trace files to providing SQL-queryable data. It covers the key components, data flow, and architectural patterns that enable the trace processor to handle traces from various formats (Proto, JSON, Systrace, etc.) and transform them into a unified analytical database.

## Overview

The trace processor is a system that ingests trace files of various formats, parses their contents, sorts events by timestamp, and stores the data in a columnar SQL database for analysis. It processes traces in chunks to efficiently handle large files.

## Core Data Pipeline

```
Raw Trace → ForwardingTraceParser → Format-Specific ChunkedTraceReader →
TraceSorter → TraceStorage → SQL Query Engine
```

## Format Detection and Delegation

**ForwardingTraceParser** (`src/trace_processor/forwarding_trace_parser.cc:95-134`)
- Detects trace format using `GuessTraceType()` from first bytes
- Creates appropriate reader via **TraceReaderRegistry** (`src/trace_processor/trace_reader_registry.h`)
- All readers implement **ChunkedTraceReader** interface (`src/trace_processor/importers/common/chunked_trace_reader.h`)

**Format Registration** (`src/trace_processor/trace_processor_impl.cc:475-519`)
```cpp
context()->reader_registry->RegisterTraceReader<JsonTraceTokenizer>(kJsonTraceType);
context()->reader_registry->RegisterTraceReader<ProtoTraceReader>(kProtoTraceType);
context()->reader_registry->RegisterTraceReader<SystraceTraceParser>(kSystraceTraceType);
```

## Format-Specific Readers (Diverse Approaches)

### 1. JSON Traces
**JsonTraceTokenizer** (`src/trace_processor/importers/json/json_trace_tokenizer.h:73`)
- **Data Flow**: Raw JSON → Tokenizer → JsonEvent objects → TraceSorter::Stream<JsonEvent>
- **Parser**: JsonTraceParser processes sorted events → TraceStorage
- **Architecture**: Tokenizer/Parser split with JSON-specific state machine

### 2. Proto Traces (Complex Modular System)
**ProtoTraceReader** (`src/trace_processor/importers/proto/proto_trace_reader.h:58`)
- **Data Flow**: Proto bytes → ProtoTraceTokenizer → ProtoImporterModules → TraceSorter::Stream<TracePacketData>
- **Modules**: Register for specific packet field IDs (`src/trace_processor/importers/proto/proto_importer_module.h:110`)
  - Tokenization phase: Early processing before sorting
  - Parsing phase: Post-sorting detailed processing
- **Examples**: FtraceModule, TrackEventModule, AndroidModule (many files in `src/trace_processor/importers/proto/`)

### 3. Systrace (Line-Based Processing)
**SystraceTraceParser** (`src/trace_processor/importers/systrace/systrace_trace_parser.h:34`)
- **Data Flow**: Text lines → SystraceLineTokenizer → SystraceLine objects → TraceSorter::Stream<SystraceLine>
- **Architecture**: State machine for HTML + trace data sections

### 4. Other Formats
- **Perf**: `perf_importer::PerfDataTokenizer` (binary perf.data format)
- **Gecko**: `gecko_importer::GeckoTraceTokenizer` (Firefox traces)
- **Fuchsia**: `FuchsiaTraceTokenizer` (Fuchsia kernel traces)

## Event Sorting and Processing

**TraceSorter** (`src/trace_processor/sorter/trace_sorter.h:43`)
- **Purpose**: Multi-stream timestamp-based merge sorting
- **Architecture**: Per-CPU queues for ftrace, windowed sorting for streaming
- **Streams**: Each format creates typed streams (JsonEvent, TracePacketData, SystraceLine, etc.)
- **Output**: Sorted events to format-specific parsers

## Storage Layer

**TraceStorage** (`src/trace_processor/storage/trace_storage.h`)
- **Architecture**: Columnar storage with specialized table types
- **Tables**: SliceTable, ProcessTable, ThreadTable, CounterTable, etc.
- **Access**: Direct insertion by parsers, SQL queries by engine

## Context and Coordination

**TraceProcessorContext** (`src/trace_processor/types/trace_processor_context.h`)
- **Multi-level state management**:
  - Global state (shared across machines)
  - Per-trace state (specific to each trace file)
  - Per-machine state (unique to each machine)
  - Per-trace-and-machine state (most specific)
- **Coordination**: Central access point for storage, sorter, trackers

## Key Architectural Patterns

### 1. ChunkedTraceReader Interface
All format readers implement same interface but with completely different internal architectures:
- JSON: Incremental JSON parsing with state machine
- Proto: Modular packet processing with field-based routing
- Systrace: Line-by-line text processing
- Archives (ZIP/TAR): Container formats that extract and delegate

### 2. TraceSorter::Stream<T> Pattern
Each format defines its own event types and creates typed streams:
- `Stream<JsonEvent>` for JSON traces
- `Stream<TracePacketData>` for proto events
- `Stream<SystraceLine>` for systrace lines

### 3. Parser vs Tokenizer Split
- **Tokenizer**: Pre-sorting processing, fast timestamp extraction
- **Parser**: Post-sorting detailed processing into storage
- Not all formats use this split (depends on complexity)

## File Path Reference

**Core Infrastructure**:
- `src/trace_processor/forwarding_trace_parser.{h,cc}` - Format detection and delegation
- `src/trace_processor/trace_reader_registry.{h,cc}` - Reader registration
- `src/trace_processor/sorter/trace_sorter.h` - Event sorting
- `src/trace_processor/storage/trace_storage.h` - Columnar storage

**Format Readers** (examples):
- `src/trace_processor/importers/json/json_trace_tokenizer.h` - JSON processing
- `src/trace_processor/importers/proto/proto_trace_reader.h` - Proto entry point
- `src/trace_processor/importers/proto/proto_importer_module.h` - Proto module system
- `src/trace_processor/importers/systrace/systrace_trace_parser.h` - Systrace processing

**Registration**:
- `src/trace_processor/trace_processor_impl.cc:475-519` - Where all readers are registered