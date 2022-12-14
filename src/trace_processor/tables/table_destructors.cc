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

#include "src/trace_processor/tables/android_tables.h"
#include "src/trace_processor/tables/counter_tables.h"
#include "src/trace_processor/tables/flow_tables.h"
#include "src/trace_processor/tables/memory_tables.h"
#include "src/trace_processor/tables/metadata_tables.h"
#include "src/trace_processor/tables/profiler_tables.h"
#include "src/trace_processor/tables/slice_tables.h"
#include "src/trace_processor/tables/trace_proto_tables.h"
#include "src/trace_processor/tables/track_tables.h"

namespace perfetto {
namespace trace_processor {
namespace macros_internal {
// macros_internal.h
MacroTable::~MacroTable() = default;
}  // namespace macros_internal

namespace tables {
// android_tables.h
AndroidDumpstateTable::~AndroidDumpstateTable() = default;
AndroidGameInterventionListTable::~AndroidGameInterventionListTable() = default;
AndroidLogTable::~AndroidLogTable() = default;

// counter_tables.h
CounterTable::~CounterTable() = default;

// metadata_tables.h
RawTable::~RawTable() = default;
ArgTable::~ArgTable() = default;
ExpMissingChromeProcTable::~ExpMissingChromeProcTable() = default;
MetadataTable::~MetadataTable() = default;
CpuTable::~CpuTable() = default;
CpuFreqTable::~CpuFreqTable() = default;
ThreadTable::~ThreadTable() = default;
ProcessTable::~ProcessTable() = default;
FiledescriptorTable::~FiledescriptorTable() = default;
ClockSnapshotTable::~ClockSnapshotTable() = default;

// profiler_tables.h
StackProfileMappingTable::~StackProfileMappingTable() = default;
StackProfileFrameTable::~StackProfileFrameTable() = default;
StackProfileCallsiteTable::~StackProfileCallsiteTable() = default;
StackSampleTable::~StackSampleTable() = default;
CpuProfileStackSampleTable::~CpuProfileStackSampleTable() = default;
PerfSampleTable::~PerfSampleTable() = default;
SymbolTable::~SymbolTable() = default;
HeapProfileAllocationTable::~HeapProfileAllocationTable() = default;
ExperimentalFlamegraphNodesTable::~ExperimentalFlamegraphNodesTable() = default;
HeapGraphObjectTable::~HeapGraphObjectTable() = default;
HeapGraphClassTable::~HeapGraphClassTable() = default;
HeapGraphReferenceTable::~HeapGraphReferenceTable() = default;
VulkanMemoryAllocationsTable::~VulkanMemoryAllocationsTable() = default;
PackageListTable::~PackageListTable() = default;
ProfilerSmapsTable::~ProfilerSmapsTable() = default;
GpuCounterGroupTable::~GpuCounterGroupTable() = default;

// slice_tables.h
SliceTable::~SliceTable() = default;
FlowTable::~FlowTable() = default;
SchedSliceTable::~SchedSliceTable() = default;
GpuSliceTable::~GpuSliceTable() = default;
GraphicsFrameSliceTable::~GraphicsFrameSliceTable() = default;
ThreadStateTable::~ThreadStateTable() = default;
ExpectedFrameTimelineSliceTable::~ExpectedFrameTimelineSliceTable() = default;
ActualFrameTimelineSliceTable::~ActualFrameTimelineSliceTable() = default;
ExperimentalFlatSliceTable::~ExperimentalFlatSliceTable() = default;

// track_tables.h
TrackTable::~TrackTable() = default;
ProcessTrackTable::~ProcessTrackTable() = default;
ThreadTrackTable::~ThreadTrackTable() = default;
GpuTrackTable::~GpuTrackTable() = default;
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

// trace_proto_tables.h
ExperimentalProtoPathTable::~ExperimentalProtoPathTable() = default;
ExperimentalProtoContentTable::~ExperimentalProtoContentTable() = default;

// memory_tables.h
MemorySnapshotTable::~MemorySnapshotTable() = default;
ProcessMemorySnapshotTable::~ProcessMemorySnapshotTable() = default;
MemorySnapshotNodeTable::~MemorySnapshotNodeTable() = default;
MemorySnapshotEdgeTable::~MemorySnapshotEdgeTable() = default;

}  // namespace tables

}  // namespace trace_processor
}  // namespace perfetto
