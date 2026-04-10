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
field to distinguish between machines in multi-machine setups. GPU hardware
metadata (name, vendor, architecture, UUID, PCI BDF) is recorded via the
[GpuInfo](/protos/perfetto/trace/system_info/gpu_info.proto) trace packet.

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
Some producers may also support glob patterns in `counter_names` for matching
multiple counters but this is not guaranteed.

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
the data source descriptor. Counters are organized into groups (SYSTEM,
VERTICES, FRAGMENTS, PRIMITIVES, MEMORY, COMPUTE, RAY_TRACING) and include
measurement units and descriptions.

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
