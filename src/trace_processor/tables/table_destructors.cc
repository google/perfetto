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
#include "src/trace_processor/tables/macros_internal.h"
#include "src/trace_processor/tables/memory_tables_py.h"
#include "src/trace_processor/tables/metadata_tables_py.h"
#include "src/trace_processor/tables/profiler_tables_py.h"
#include "src/trace_processor/tables/sched_tables_py.h"
#include "src/trace_processor/tables/slice_tables_py.h"
#include "src/trace_processor/tables/trace_proto_tables_py.h"
#include "src/trace_processor/tables/track_tables_py.h"
#include "src/trace_processor/tables/winscope_tables_py.h"

namespace perfetto::trace_processor {
namespace macros_internal {
// macros_internal.h
MacroTable::~MacroTable() = default;
}  // namespace macros_internal

namespace tables {
// android_tables_py.h
AndroidKeyEventsTable::~AndroidKeyEventsTable() = default;
AndroidMotionEventsTable::~AndroidMotionEventsTable() = default;
AndroidInputEventDispatchTable::~AndroidInputEventDispatchTable() = default;

// counter_tables_py.h
CounterTable::~CounterTable() = default;

// metadata_tables_py.h
ChromeRawTable::~ChromeRawTable() = default;
FtraceEventTable::~FtraceEventTable() = default;
ArgTable::~ArgTable() = default;
MetadataTable::~MetadataTable() = default;
CpuTable::~CpuTable() = default;
ThreadTable::~ThreadTable() = default;
ProcessTable::~ProcessTable() = default;

// profiler_tables_py.h
StackProfileMappingTable::~StackProfileMappingTable() = default;
StackProfileFrameTable::~StackProfileFrameTable() = default;
StackProfileCallsiteTable::~StackProfileCallsiteTable() = default;
PerfSampleTable::~PerfSampleTable() = default;
HeapProfileAllocationTable::~HeapProfileAllocationTable() = default;
ExperimentalFlamegraphTable::~ExperimentalFlamegraphTable() = default;
HeapGraphObjectTable::~HeapGraphObjectTable() = default;
HeapGraphClassTable::~HeapGraphClassTable() = default;
HeapGraphReferenceTable::~HeapGraphReferenceTable() = default;
VulkanMemoryAllocationsTable::~VulkanMemoryAllocationsTable() = default;

// sched_tables_py.h
SchedSliceTable::~SchedSliceTable() = default;
ThreadStateTable::~ThreadStateTable() = default;

// slice_tables_py.h
SliceTable::~SliceTable() = default;
FlowTable::~FlowTable() = default;
ExperimentalFlatSliceTable::~ExperimentalFlatSliceTable() = default;
AndroidNetworkPacketsTable::~AndroidNetworkPacketsTable() = default;

// track_tables_py.h
TrackTable::~TrackTable() = default;

// trace_proto_tables_py.h
ExperimentalProtoPathTable::~ExperimentalProtoPathTable() = default;

// memory_tables_py.h
MemorySnapshotNodeTable::~MemorySnapshotNodeTable() = default;

// winscope_tables_py.h
InputMethodClientsTable::~InputMethodClientsTable() = default;
InputMethodManagerServiceTable::~InputMethodManagerServiceTable() = default;
InputMethodServiceTable::~InputMethodServiceTable() = default;
SurfaceFlingerLayersSnapshotTable::~SurfaceFlingerLayersSnapshotTable() =
    default;
SurfaceFlingerLayerTable::~SurfaceFlingerLayerTable() = default;
SurfaceFlingerTransactionsTable::~SurfaceFlingerTransactionsTable() = default;
SurfaceFlingerTransactionTable::~SurfaceFlingerTransactionTable() = default;
ViewCaptureTable::~ViewCaptureTable() = default;
ViewCaptureViewTable::~ViewCaptureViewTable() = default;
ViewCaptureInternedDataTable::~ViewCaptureInternedDataTable() = default;
WindowManagerTable::~WindowManagerTable() = default;
WindowManagerShellTransitionsTable::~WindowManagerShellTransitionsTable() =
    default;
WindowManagerShellTransitionProtosTable::
    ~WindowManagerShellTransitionProtosTable() = default;

}  // namespace tables

}  // namespace perfetto::trace_processor
