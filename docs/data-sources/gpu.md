# GPU

![](/docs/images/gpu-counters.png)

## GPU Frequency

GPU frequency can be included in the trace by adding the ftrace category.

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

## GPU Counters

GPU counters can be configured by adding the data source to the trace config as follows:

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

The counter_ids correspond to the ones described in `GpuCounterSpec` in the data source descriptor.

See the full configuration options in [gpu\_counter\_config.proto](/protos/perfetto/config/gpu/gpu_counter_config.proto)

