/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/trace_processor/tables/android_tables_py.h"
#include "src/trace_processor/tables/counter_tables_py.h"
#include "src/trace_processor/tables/flow_tables_py.h"
#include "src/trace_processor/tables/jit_tables_py.h"
#include "src/trace_processor/tables/memory_tables_py.h"
#include "src/trace_processor/tables/metadata_tables_py.h"
#include "src/trace_processor/tables/perf_tables_py.h"
#include "src/trace_processor/tables/profiler_tables_py.h"
#include "src/trace_processor/tables/sched_tables_py.h"
#include "src/trace_processor/tables/slice_tables_py.h"
#include "src/trace_processor/tables/trace_proto_tables_py.h"
#include "src/trace_processor/tables/track_tables_py.h"
#include "src/trace_processor/tables/v8_tables_py.h"
#include "src/trace_processor/tables/winscope_tables_py.h"

namespace perfetto {
namespace trace_processor {
namespace macros_internal {
// macros_internal.h
MacroTable::~MacroTable() = default;
}  // namespace macros_internal

namespace tables {
// android_tables_py.h
AndroidDumpstateTable::~AndroidDumpstateTable() = default;
AndroidGameInterventionListTable::~AndroidGameInterventionListTable() = default;
AndroidLogTable::~AndroidLogTable() = default;
AndroidKeyEventsTable::~AndroidKeyEventsTable() = default;
AndroidMotionEventsTable::~AndroidMotionEventsTable() = default;
AndroidInputEventDispatchTable::~AndroidInputEventDispatchTable() = default;

// counter_tables_py.h
CounterTable::~CounterTable() = default;

// jit_tables.py
JitCodeTable::~JitCodeTable() = default;
JitFrameTable::~JitFrameTable() = default;

// metadata_tables_py.h
RawTable::~RawTable() = default;
FtraceEventTable::~FtraceEventTable() = default;
ArgTable::~ArgTable() = default;
ExpMissingChromeProcTable::~ExpMissingChromeProcTable() = default;
MetadataTable::~MetadataTable() = default;
CpuTable::~CpuTable() = default;
CpuFreqTable::~CpuFreqTable() = default;
ThreadTable::~ThreadTable() = default;
ProcessTable::~ProcessTable() = default;
FiledescriptorTable::~FiledescriptorTable() = default;
ClockSnapshotTable::~ClockSnapshotTable() = default;
MachineTable::~MachineTable() = default;
TraceFileTable::~TraceFileTable() = default;

// perf_tables.py
SpeRecordTable::~SpeRecordTable() = default;

// profiler_tables_py.h
StackProfileMappingTable::~StackProfileMappingTable() = default;
StackProfileFrameTable::~StackProfileFrameTable() = default;
StackProfileCallsiteTable::~StackProfileCallsiteTable() = default;
CpuProfileStackSampleTable::~CpuProfileStackSampleTable() = default;
PerfSessionTable::~PerfSessionTable() = default;
PerfSampleTable::~PerfSampleTable() = default;
InstrumentsSampleTable::~InstrumentsSampleTable() = default;
SymbolTable::~SymbolTable() = default;
HeapProfileAllocationTable::~HeapProfileAllocationTable() = default;
ExperimentalFlamegraphTable::~ExperimentalFlamegraphTable() = default;
HeapGraphObjectTable::~HeapGraphObjectTable() = default;
HeapGraphClassTable::~HeapGraphClassTable() = default;
HeapGraphReferenceTable::~HeapGraphReferenceTable() = default;
VulkanMemoryAllocationsTable::~VulkanMemoryAllocationsTable() = default;
PackageListTable::~PackageListTable() = default;
ProfilerSmapsTable::~ProfilerSmapsTable() = default;
GpuCounterGroupTable::~GpuCounterGroupTable() = default;

// sched_tables_py.h
SchedSliceTable::~SchedSliceTable() = default;
SpuriousSchedWakeupTable::~SpuriousSchedWakeupTable() = default;
ThreadStateTable::~ThreadStateTable() = default;

// slice_tables_py.h
SliceTable::~SliceTable() = default;
FlowTable::~FlowTable() = default;
GpuSliceTable::~GpuSliceTable() = default;
GraphicsFrameSliceTable::~GraphicsFrameSliceTable() = default;
ExpectedFrameTimelineSliceTable::~ExpectedFrameTimelineSliceTable() = default;
ActualFrameTimelineSliceTable::~ActualFrameTimelineSliceTable() = default;
ExperimentalFlatSliceTable::~ExperimentalFlatSliceTable() = default;
AndroidNetworkPacketsTable::~AndroidNetworkPacketsTable() = default;

// track_tables_py.h
TrackTable::~TrackTable() = default;
ProcessTrackTable::~ProcessTrackTable() = default;
ThreadTrackTable::~ThreadTrackTable() = default;
CpuTrackTable::~CpuTrackTable() = default;
GpuTrackTable::~GpuTrackTable() = default;
UidTrackTable::~UidTrackTable() = default;
GpuWorkPeriodTrackTable::~GpuWorkPeriodTrackTable() = default;
CounterTrackTable::~CounterTrackTable() = default;
ThreadCounterTrackTable::~ThreadCounterTrackTable() = default;
ProcessCounterTrackTable::~ProcessCounterTrackTable() = default;
CpuCounterTrackTable::~CpuCounterTrackTable() = default;
IrqCounterTrackTable::~IrqCounterTrackTable() = default;
SoftirqCounterTrackTable::~SoftirqCounterTrackTable() = default;
GpuCounterTrackTable::~GpuCounterTrackTable() = default;
PerfCounterTrackTable::~PerfCounterTrackTable() = default;
EnergyCounterTrackTable::~EnergyCounterTrackTable() = default;
UidCounterTrackTable::~UidCounterTrackTable() = default;
EnergyPerUidCounterTrackTable::~EnergyPerUidCounterTrackTable() = default;
LinuxDeviceTrackTable::~LinuxDeviceTrackTable() = default;

// trace_proto_tables_py.h
ExperimentalProtoPathTable::~ExperimentalProtoPathTable() = default;
ExperimentalProtoContentTable::~ExperimentalProtoContentTable() = default;

// memory_tables_py.h
MemorySnapshotTable::~MemorySnapshotTable() = default;
ProcessMemorySnapshotTable::~ProcessMemorySnapshotTable() = default;
MemorySnapshotNodeTable::~MemorySnapshotNodeTable() = default;
MemorySnapshotEdgeTable::~MemorySnapshotEdgeTable() = default;

// v8_tables_py.h
V8IsolateTable::~V8IsolateTable() = default;
V8JsScriptTable::~V8JsScriptTable() = default;
V8WasmScriptTable::~V8WasmScriptTable() = default;
V8JsFunctionTable::~V8JsFunctionTable() = default;
V8JsCodeTable::~V8JsCodeTable() = default;
V8InternalCodeTable::~V8InternalCodeTable() = default;
V8WasmCodeTable::~V8WasmCodeTable() = default;
V8RegexpCodeTable::~V8RegexpCodeTable() = default;

// winscope_tables_py.h
InputMethodClientsTable::~InputMethodClientsTable() = default;
InputMethodManagerServiceTable::~InputMethodManagerServiceTable() = default;
InputMethodServiceTable::~InputMethodServiceTable() = default;
ProtoLogTable::~ProtoLogTable() = default;
SurfaceFlingerLayersSnapshotTable::~SurfaceFlingerLayersSnapshotTable() =
    default;
SurfaceFlingerLayerTable::~SurfaceFlingerLayerTable() = default;
SurfaceFlingerTransactionsTable::~SurfaceFlingerTransactionsTable() = default;
ViewCaptureTable::~ViewCaptureTable() = default;
WindowManagerTable::~WindowManagerTable() = default;
WindowManagerShellTransitionsTable::~WindowManagerShellTransitionsTable() =
    default;
WindowManagerShellTransitionHandlersTable::
    ~WindowManagerShellTransitionHandlersTable() = default;

}  // namespace tables

}  // namespace trace_processor
}  // namespace perfetto
