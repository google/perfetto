# GPU

Perfetto supports tracing GPU activity across a range of use-cases, from
Android mobile graphics to high-end multi-GPU compute workloads.

![](/docs/images/gpu-counters.png)

## Data sources

The following data sources are available for GPU tracing:

| Data Source | Config | Purpose |
|---|---|---|
| `gpu.counters` | [gpu\_counter\_config.proto](/protos/perfetto/config/gpu/gpu_counter_config.proto) | Periodic or instrumented GPU counter sampling |
| `gpu.renderstages` | [gpu\_renderstages\_config.proto](/protos/perfetto/config/gpu/gpu_renderstages_config.proto) | GPU render stage and compute activity timeline |
| `vulkan.memory_tracker` | [vulkan\_memory\_config.proto](/protos/perfetto/config/gpu/vulkan_memory_config.proto) | Vulkan memory allocation and bind tracking |
| `gpu.log` | *(none)* | GPU debug log messages |
| `linux.ftrace` | [ftrace\_config.proto](/protos/perfetto/config/ftrace/ftrace_config.proto) | GPU frequency, memory totals, DRM scheduler events |

GPU producers commonly register data sources with a hardware-specific suffix,
e.g. `gpu.counters.adreno` or `gpu.renderstages.mali`. The tracing service uses
exact name matching, so the trace config must use the same suffixed name. The
trace processor parses GPU data based on proto field types, so all suffixed
variants are handled identically. When targeting a specific GPU vendor's
producer, use the suffixed name in your trace config:

```
data_sources: {
    config {
        name: "gpu.counters"
        gpu_counter_config {
            counter_period_ns: 1000000
            counter_ids: 1
        }
    }
}
```

Traces include a `gpu_id` field to distinguish between GPUs and a `machine_id`
field to distinguish between machines in
[multi-machine setups](/docs/deployment/multi-machine-architecture.md).
GPU hardware metadata (name, vendor, architecture, UUID, PCI BDF) is recorded
via the [GpuInfo](/protos/perfetto/trace/system_info/gpu_info.proto) trace
packet.

## Android

### GPU frequency

GPU frequency is collected via ftrace:

```
data_sources: {
    config {
        name: "linux.ftrace"
        ftrace_config {
            ftrace_events: "power/gpu_frequency"
        }
    }
}
```

### GPU counters

Android GPU producers must use counter descriptor mode 1: the
`GpuCounterDescriptor` is embedded directly in the first `GpuCounterEvent`
packet of the session, and counter IDs are global. This is required for
CDD/CTS compliance.

GPU counters are sampled by specifying device-specific counter IDs. The
available counter IDs are described in `GpuCounterSpec` in the data source
descriptor.

```
data_sources: {
    config {
        name: "gpu.counters"
        gpu_counter_config {
            counter_period_ns: 1000000
            counter_ids: 1
            counter_ids: 3
            counter_ids: 106
            counter_ids: 107
            counter_ids: 109
        }
    }
}
```

`counter_period_ns` sets the desired sampling interval.

Alternatively, counters can be selected by name using `counter_names`. Use one
or the other, not both. Not all producers support this — check
`supports_counter_names` in the `GpuCounterDescriptor` data source descriptor.
Glob patterns may be used in `counter_names` to match multiple counters by
name; check `supports_counter_name_globs` in the descriptor for support.

### GPU memory

Total GPU memory usage per process is collected via ftrace:

```
data_sources: {
    config {
        name: "linux.ftrace"
        ftrace_config {
            ftrace_events: "gpu_mem/gpu_mem_total"
        }
    }
}
```

### GPU render stages

Render stage tracing provides a timeline of GPU activity (graphics and compute
submissions):

```
data_sources: {
    config {
        name: "gpu.renderstages"
    }
}
```

### Vulkan memory

Vulkan memory allocation and bind events can be tracked with:

```
data_sources: {
    config {
        name: "vulkan.memory_tracker"
        vulkan_memory_config {
            track_driver_memory_usage: true
            track_device_memory_usage: true
        }
    }
}
```

### GPU log

GPU debug log messages can be collected by enabling the data source:

```
data_sources: {
    config {
        name: "gpu.log"
    }
}
```

## High-end GPGPU

For high-performance and data-center GPU workloads (CUDA, OpenCL, HIP),
Perfetto supports multi-GPU and multi-machine tracing with instrumented counter
sampling.

### Instrumented counter sampling

Instead of global sampling, counters can be sampled by instrumenting GPU
command buffers. This provides per-submission counter values:

```
data_sources: {
    config {
        name: "gpu.counters"
        gpu_counter_config {
            counter_ids: 1
            counter_ids: 2
            instrumented_sampling: true
        }
    }
}
```

For more control over which GPU activities are instrumented, use
`instrumented_sampling_config` instead of the `instrumented_sampling` bool.
This enables a pipeline of filters applied in the following order:

1. **Activity name filtering**: If `activity_name_filters` is non-empty, the
   activity must match at least one filter. Each filter requires a `name_glob`
   pattern and an optional `name_base` (defaults to `MANGLED_KERNEL_NAME` if
   not specified). If empty, all activities pass this step.

2. **TX range filtering**: If `activity_tx_include_globs` is non-empty, the
   activity must fall within a TX range (e.g. NVTX range for CUDA) matching
   one of the include globs. Activities in TX ranges matching
   `activity_tx_exclude_globs` are excluded (excludes take precedence over
   includes). TX ranges can be nested, and an activity matches if any range
   in its nesting hierarchy matches. If both are empty, all activities pass
   this step.

3. **Range-based sampling**: If `activity_ranges` is non-empty, only
   activities within the specified skip/count ranges are instrumented.
   `skip` defaults to 0 and `count` defaults to UINT32\_MAX (all remaining
   activities) when not specified. If empty, all activities that passed the
   previous steps are instrumented.

Example configuration that instruments only activities with demangled kernel
names matching `"myKernel*"` within TX ranges matching `"training*"`,
skipping the first 10 matching activities and then instrumenting 5:

```
data_sources: {
    config {
        name: "gpu.counters"
        gpu_counter_config {
          counter_names: "sm__cycles_elapsed.avg"
          counter_names: "sm__cycles_active.avg"
          instrumented_sampling_config {
            activity_name_filters {
              name_glob: "myKernel*"
              name_base: DEMANGLED_KERNEL_NAME
            }
            activity_tx_include_globs: "training*"
            activity_ranges {
              skip: 10
              count: 5
            }
          }
        }
    }
}
```

Counter descriptor mode 2 is recommended for GPGPU use-cases: the producer
emits an `InternedGpuCounterDescriptor` referenced by IID, giving each
trusted sequence its own scoped counter IDs. This avoids the global
coordination required by mode 1 and supports multiple producers and GPUs
naturally. See
[gpu\_counter\_event.proto](/protos/perfetto/trace/gpu/gpu_counter_event.proto)
for details on both modes.

Counter names and IDs are advertised by the GPU producer via `GpuCounterSpec` in
the data source descriptor, which includes measurement units and descriptions.

### Counter groups

Counter groups are used by the Perfetto UI to organize counter tracks into
groups. Counters can be assigned to built-in groups (SYSTEM, VERTICES,
FRAGMENTS, PRIMITIVES, MEMORY, COMPUTE, RAY_TRACING) via
`GpuCounterSpec.groups`. Producers can also define custom counter groups
using the `GpuCounterGroupSpec` message in `GpuCounterDescriptor`:

```
message GpuCounterGroupSpec {
    optional uint32 group_id = 1;
    optional string name = 2;
    optional string description = 3;
    repeated uint32 counter_ids = 4;
}
```

Custom groups can also be used to provide display names and descriptions for
the fixed `GpuCounterGroup` enum values (SYSTEM, VERTICES, etc.). To do this,
set `group_id` to the enum value and provide a `name` and/or `description`.

A counter's group membership is the union of groups assigned via
`GpuCounterSpec.groups` (the fixed enum) and `GpuCounterGroupSpec.counter_ids`
(custom groups).

For example, with custom groups "Compute Core" and "L2 Cache":

```
GPU > Counters > Compute Core > Counter A
GPU > Counters > Compute Core > Counter B
GPU > Counters > L2 Cache > Counter C
```


### Multi-GPU

Each GPU in the system is assigned a `gpu_id`. Counter events, render stages,
and other GPU trace data carry this ID so the UI can group tracks per GPU. GPU
hardware details are recorded via the
[GpuInfo](/protos/perfetto/trace/system_info/gpu_info.proto) message, which
includes:

- `name`, `vendor`, `model`, `architecture`
- `uuid` (16-byte identifier)
- `pci_bdf` (PCI bus/device/function)

### Multi-machine

When tracing across multiple machines, each GPU trace event also carries a
`machine_id` to distinguish which machine the GPU belongs to. The Perfetto UI
displays machine labels alongside GPU tracks.

### Render stage event correlation

GPU render stage events can declare dependencies on other render stage events
using the `event_wait_ids` field on `GpuRenderStageEvent`. Each entry is the
`event_id` of another render stage event that this event had to wait on before
it could run. The trace processor uses these to create flow arrows between
the correlated GPU slices.

Example: a matmul kernel that depends on a previous asynchronous memcpy:

```
gpu_render_stage_event {
    event_id: 1
    duration: 50000
    hw_queue_iid: 1
    stage_iid: 2
    context: 0
    name: "Memcpy HtoD"
}

gpu_render_stage_event {
    event_id: 2
    duration: 40000
    hw_queue_iid: 3
    stage_iid: 4
    context: 0
    name: "matmul_kernel"
    event_wait_ids: 1
}
```

This creates a flow from the memcpy event (event\_id 1) to the matmul kernel
(event\_id 2), visualizing the dependency in the Perfetto UI.

### Host-to-GPU correlation

Host-side track events can be correlated with GPU render stage events using
the `GpuCorrelation` TrackEvent extension. This is useful for connecting
host API calls (e.g. `cudaLaunchKernel`, `cudaMemcpyAsync`) with the
corresponding GPU work.

The extension provides two fields:

- `render_stage_submission_event_ids`: event IDs of GPU render stage events
  that this host event submitted.
- `render_stage_wait_event_ids`: event IDs of GPU render stage events that
  this host event waited on to complete.

Example: a host kernel launch correlated with a GPU compute kernel:

```
track_event {
    type: TYPE_SLICE_BEGIN
    name: "cudaLaunchKernel"
    [perfetto.protos.GpuTrackEvent.gpu_correlation] {
        render_stage_submission_event_ids: 1
    }
}

gpu_render_stage_event {
    event_id: 1
    duration: 50000
    hw_queue_iid: 1
    stage_iid: 2
    context: 0
    name: "matmul_kernel"
}
```

## Generic GPU tracks

GPU concepts are not limited to physical hardware queues. Producers often need
to show logical timelines such as schedulers, annotations, or command buffer
state. These can be represented with regular `TrackDescriptor` and
`TrackEvent` packets by adding the `GpuTrackDescriptor` extension defined in
[gpu_track_event.proto](/protos/perfetto/trace/gpu/gpu_track_event.proto).

The presence of the extension marks a descriptor as GPU-associated. The normal
`TrackDescriptor` fields still define the track name, event type, ordering,
sibling merging, and `parent_uuid` hierarchy. Descendants inherit the GPU
association, so only the root of a GPU subtree needs the extension.

```
track_descriptor {
  uuid: 100
  name: "Scheduler"
  [perfetto.protos.GpuTrackDescriptorExtension.gpu_track] {
    gpu_id: 0
  }
}
track_descriptor {
  uuid: 101
  parent_uuid: 100
  name: "Dispatches"
}
```

`gpu_id` identifies the physical GPU that owns the track. A global descriptor
which introduces a concrete `gpu_id` is the placement anchor for that GPU's
existing frequency, memory, counter, and other GPU-specific tracks. There must
be at most one unambiguous placement anchor for each `(machine, gpu_id)`.
Descendants created using `parent_uuid` inherit the anchor's GPU association and
should not repeat `gpu_id`.

Existing GPU tracks become direct children of a valid placement anchor; the UI
does not reproduce its default `Counters -> GPU N` grouping there. A unique
global GPU root without a concrete `gpu_id` similarly receives machine-wide GPU
tracks. Missing or ambiguous anchors leave the corresponding tracks in their
normal default locations. Traces without generic GPU descriptors therefore keep
the existing UI hierarchy unchanged.

Generic GPU tracks support the normal TrackEvent event types, including slices,
instants, counters, and states. Producers can create arbitrary descriptor trees;
Perfetto does not assign meaning to names such as `Device`, `Context`, or
`Stream`.

### Process-scoped GPU tracks

Some GPU concepts are defined by a software API within a process rather than by
machine-wide hardware. For example, CUDA logical devices, contexts, and streams
belong to the process that created them. A track named `Stream #2` identifies a
stream in one process, but has no meaning outside that process and is unrelated
to a track with the same name in another process.

Set the GPU subtree root's `parent_uuid` to the UUID of a process
`TrackDescriptor` to associate the hierarchy with the process that owns those
concepts:

```
track_descriptor {
  uuid: 10
  process { pid: 1234 }
}
track_descriptor {
  uuid: 20
  parent_uuid: 10
  name: "CUDA"
  [perfetto.protos.GpuTrackDescriptorExtension.gpu_track] {
    gpu_id: 0
  }
}
track_descriptor {
  uuid: 21
  parent_uuid: 20
  name: "Device #0"
}
track_descriptor {
  uuid: 22
  parent_uuid: 21
  name: "Stream #2"
}
```

The process-parent edge supplies ownership but is not displayed as part of the
GPU subtree. The `CUDA` track becomes a root in the process's GPU group, while
subsequent `parent_uuid` edges define the producer-authored GPU hierarchy.
Process ownership is inherited by GPU descendants. Use separate roots for
separate processes, and emit concepts which are not process-scoped as a
separate global GPU subtree.

### Render-stage queue tracks

A GPU descriptor can represent either an interned hardware queue or a
process-scoped logical queue. The exact descriptor is both the authored
TrackEvent track and the merge anchor for matching render-stage events.

A global hardware queue binding uses `hw_queue_iid`. The descriptor packet and
render-stage events must use the same trusted packet sequence. The producer
must keep the ID stable and must not reuse it for another queue after an
incremental-state reset:

```
track_descriptor {
  uuid: 100
  name: "Accelerators"
  [perfetto.protos.GpuTrackDescriptorExtension.gpu_track] {}
}
track_descriptor {
  uuid: 101
  parent_uuid: 100
  name: "Physical execution"
  [perfetto.protos.GpuTrackDescriptorExtension.gpu_track] {}
}
track_descriptor {
  uuid: 102
  parent_uuid: 101
  name: "Channel #0"
  [perfetto.protos.GpuTrackDescriptorExtension.gpu_track] {
    gpu_id: 0
    hw_queue_iid: 7
  }
}
```

Annotations can be emitted directly on UUID 102. Render-stage events carrying
`hw_queue_iid: 7` appear as additional lanes on the same visual track.

A process logical queue binding uses `logical_queue_id`:

```
track_descriptor {
  uuid: 22
  parent_uuid: 21
  name: "Stream #2"
  [perfetto.protos.GpuTrackDescriptorExtension.gpu_track] {
    gpu_id: 0
    logical_queue_id: 2
  }
}
gpu_render_stage_event {
  event_id: 1
  duration: 50000
  context: 1
  hw_queue_iid: 7
  stage_iid: 2
  logical_queue_id: 2
  name: "matmul_kernel"
}
```

The event's process is obtained from its graphics context. Matching render-stage
work is projected onto the descriptor's native TrackEvent sibling group while
the canonical GPU slice remains available for analysis and flow correlation.
The default sibling behavior merges by descriptor name. Producers can use
`TrackDescriptor.sibling_merge_behavior` and `sibling_merge_key` to select an
explicit merge identity, or `SIBLING_MERGE_BEHAVIOR_NONE` to keep annotations
and render-stage work as separate sibling tracks.

Queue fields apply only to the exact slice/instant descriptor and are not
inherited. Counters and state tracks cannot bind render-stage queues. Logical
queue ID zero is valid and scoped to the inherited process. A hardware queue
binding requires a concrete inherited `gpu_id`.

## UI plugins

The Perfetto UI ships several plugins that consume GPU trace data. Default
tracks use the standard `GPU` group; producer-authored GPU trees can claim
matching tracks globally or beneath a process while unclaimed sources keep
that default placement.

### dev.perfetto.Gpu

Without authored placement anchors, the base plugin populates the default `GPU`
workspace group exactly as before. With an unambiguous per-GPU anchor,
frequency, memory, counters, and other matching legacy tracks become direct
children of that anchor. A unique machine-wide root receives matching tracks
which have no concrete GPU ID. Unmatched sources retain the default hierarchy.
Only hardware queues with a successful exact `hw_queue_iid` binding use the
authored TrackEvent location and sibling merging; unbound queues remain under
the default `GPU` group.

![](/docs/images/gpu-tracks.png)

### dev.perfetto.GpuByProcess

Without an authored process GPU tree, this plugin uses the default inferred
GPU hierarchy and hardware-queue fallback. Process-associated authored roots
are attached directly to the process. Matching logical queues use their exact
producer-authored TrackEvent locations, while unbound logical queues and events
without a logical queue remain in the inferred fallback hierarchy. Authored and
fallback process GPU roots can therefore coexist.

![](/docs/images/gpu-by-process.png)

### com.meta.GpuCompute

Compute-kernel deep dive. Adds three tabs that are populated whenever a
compute `gpu_render_stage` slice (i.e. `gpu_slice.render_stage_category =
COMPUTE`) is selected:

- **Summary** — table of every kernel launch in the trace, sortable by
  duration, occupancy, and other hardware metrics. Double-click jumps to
  the details view for that kernel.
- **Details** — per-section metric tables (Speed-of-Light, Launch
  Statistics, Occupancy, Workload Analysis), with optional baseline
  comparison between two kernels.
- **Toolbar** — kernel selector, baseline pin, terminology switch
  (CUDA / OpenCL / vendor-supplied), and automatic unit conversion
  (bytes → KB, ns → s, etc.).

The core plugin ships CUDA and AMD support; additional vendors are added
by companion plugins that register terminologies, metric sections,
well-known metric IDs, and analysis providers. See
[com.meta.GpuCompute/README.md](https://github.com/google/perfetto/blob/main/ui/src/plugins/com.meta.GpuCompute/README.md)
for the extension API.

![](/docs/images/gpu-compute-summary.png)

![](/docs/images/gpu-compute-details.png)

## Example queries

### Top 5 longest-running kernels with time-weighted utilization

This query ranks compute kernels by duration and, for each one, computes
the time-weighted average of the GPU `Utilization` counter over the
kernel's execution window. `counter_leading_intervals` turns the sparse
counter samples into `(ts, dur, value)` intervals (each sample's value
holds until the next sample), and `_interval_intersect` clips those
intervals against each kernel's `[ts, ts + dur)` window so the average is
weighted by how long each counter value was actually in effect during the
kernel.

```sql
INCLUDE PERFETTO MODULE counters.intervals;
INCLUDE PERFETTO MODULE intervals.intersect;

WITH
  -- The GPU Utilization counter, expanded into (ts, dur, value) intervals.
  -- Carries ugpu so the intersect can match each kernel to its own GPU.
  utilization AS (
    SELECT u.id, u.ts, u.dur, u.value, gct.ugpu
    FROM counter_leading_intervals!((
      SELECT c.id, c.ts, c.track_id, c.value
      FROM counter c
      JOIN gpu_counter_track gct ON gct.id = c.track_id
      WHERE gct.name = 'Utilization'
    )) u
    JOIN gpu_counter_track gct ON gct.id = u.track_id
  ),
  -- The 5 longest compute kernels (render_stage_category 2 = COMPUTE).
  top_kernels AS (
    SELECT
      s.id, s.ts, s.dur, s.name,
      extract_arg(t.dimension_arg_set_id, 'ugpu') AS ugpu
    FROM gpu_slice s
    JOIN gpu_track t ON s.track_id = t.id
    WHERE s.render_stage_category = 2 AND s.dur > 0
    ORDER BY s.dur DESC
    LIMIT 5
  )
SELECT
  k.name AS kernel,
  g.name AS gpu_name,
  k.dur AS dur_ns,
  -- Time-weighted average: sum(value * overlap_dur) / kernel_dur.
  SUM(u.value * ii.dur) / k.dur AS avg_utilization
FROM top_kernels k
LEFT JOIN gpu g ON g.id = k.ugpu
JOIN _interval_intersect!((top_kernels, utilization), (ugpu)) ii
  ON ii.id_0 = k.id
JOIN utilization u ON u.id = ii.id_1
GROUP BY k.id, k.name, g.name, k.dur
ORDER BY k.dur DESC;
```

Example output (two-GPU training trace):

| kernel | gpu\_name | dur\_ns | avg\_utilization |
|---|---|---|---|
| matmul\_bwd\_kernel | NVIDIA A100-SXM4-80GB #1 | 180000 | 78.27 |
| matmul\_bwd\_kernel | NVIDIA A100-SXM4-80GB #2 | 180000 | 77.25 |
| matmul\_kernel     | NVIDIA A100-SXM4-80GB #1 | 125000 | 78.70 |
| matmul\_kernel     | NVIDIA A100-SXM4-80GB #2 | 125000 | 78.83 |
| softmax\_bwd\_kernel | NVIDIA A100-SXM4-80GB #1 | 110000 | 73.76 |
