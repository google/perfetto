# com.meta.GpuCompute

A Perfetto UI plugin for exploring GPU compute workloads across multiple
vendors and hardware architectures. It adds a dedicated tab for viewing kernel
launches, comparing performance metrics, and inspecting hardware counter data.

## Overview

The core plugin provides:

- **Compute tab** — automatically opens when a compute GPU slice (render stage
  event) is selected, showing detailed metrics for the selected kernel.
- **Summary tab** — lists all compute kernel launches in the trace, sortable by
  duration, occupancy, and other hardware metrics. Double-click a row to jump to
  its detail view.
- **Details tab** — per-kernel metric tables organized into collapsible sections.
  Supports baseline comparison between two kernels.
- **Toolbar** — kernel selector, baseline management, terminology switching, and
  automatic unit conversion (e.g. bytes → KB, nanoseconds → seconds).
- **Analysis** — optional per-section and per-kernel analysis. The core plugin
  provides the analysis UI and caching; the actual analysis backend is supplied
  by an external analysis provider plugin.

The core plugin includes built-in support for **CUDA** (NVIDIA) and **AMD**
hardware, with CUDA terminology as the default. It is designed to be extended
by companion plugins that add support for additional vendors by registering
**terminologies**, **metric sections**, **well-known metrics**, and
**analysis providers**.

## Extension Plugins

Extension plugins declare `com.meta.GpuCompute` as a dependency and call
registration functions exported by the core plugin during `onActivate()`.

### Terminology Plugins

A terminology plugin provides vendor-specific terms for GPU concepts (thread,
warp, block, SM, etc.). The user can switch the display terminology via the
toolbar.

Register a terminology by calling `registerTerminology()` from
`terminology.ts`:

```ts
import {PerfettoPlugin} from '../../public/plugin';
import {createTerminology, registerTerminology} from '../com.meta.GpuCompute/terminology';
import GpuComputePlugin from '../com.meta.GpuCompute';

export default class MyTerminologyPlugin implements PerfettoPlugin {
  static readonly id = 'com.meta.GpuCompute.Terminology.MyVendor';
  static readonly dependencies = [GpuComputePlugin];

  static onActivate(): void {
    registerTerminology('myvendor', createTerminology('MyVendor', { ... }, { ... }));
  }
}
```

Built-in terminologies (registered by the core plugin):

| File | Description |
|---|---|
| `terminology/cuda.ts` | CUDA terminology (thread, warp, block, SM, etc.) — default |
| `terminology/opencl.ts` | OpenCL terminology (work-item, work-group, NDRange, CU, etc.) |

### Section Plugins

A section plugin adds a collapsible group of metric tables to the details tab.
Each section declares which launch metrics (extracted from slice args) and
counter metrics (from `gpu_counter_track`) it needs. The core plugin
automatically includes them in the SQL query and materializes the results into
tables.

Each section contains one or more **tables**. A table is visible when all of
its `required`-importance rows have data available. Tables with no `required`
rows are always visible. A section is automatically visible when at least one
of its tables is visible.

The `Section` interface supports these optional fields:

- **`collapsedByDefault`** — start the section collapsed in the details tab.
- **`analysisPrompt`** — optional context for the analysis provider describing
  what the metrics mean and how to interpret them. The analysis provider may
  use this as a prompt for a language model or ignore it entirely.

Each `TableDecl` supports:

- **`description`** — terminology-aware description shown above the table.
- **`rows`** — metric row declarations with id, label, unit, and importance.
  Rows with `importance: 'required'` control table visibility.

Register a section by calling `registerSection()` from `section.ts`:

```ts
import {PerfettoPlugin} from '../../public/plugin';
import {registerSection} from '../com.meta.GpuCompute/section';
import GpuComputePlugin from '../com.meta.GpuCompute';

export default class MySectionPlugin implements PerfettoPlugin {
  static readonly id = 'com.vendor.GpuCompute.Section.MySection';
  static readonly dependencies = [GpuComputePlugin];

  static onActivate(): void {
    registerSection({
      id: 'my_section',
      title: 'My Section',
      launchMetrics: ['my_launch_metric'],
      counterMetrics: ['my_counter.avg'],
      analysisPrompt:
        'You are analyzing the "My Section" metrics for a GPU compute kernel.\n' +
        'Explain what each metric means and provide actionable recommendations.',
      tables: [{
        description: (t) => `Description using ${t.sm.title} terminology.`,
        rows: [
          {id: 'my_counter.avg', label: (t) => `${t.sm.title} Counter`, unit: () => 'cycle', importance: 'required'},
        ],
      }],
    });
  }
}
```

Built-in sections (in `section/`):

| File | Description |
|---|---|
| `section/speed_of_light.ts` | Compute and memory throughput overview |
| `section/launch_statistics.ts` | Kernel launch configuration |
| `section/occupancy.ts` | Warp occupancy and limiting factors |
| `section/workload_analysis.ts` | Per-pipeline utilization |

### Well-Known Metrics

The toolbar and summary table use "well-known" metric roles (e.g. `duration`,
`cycles`, `frequency`, `compute_throughput`, `memory_throughput`) to display
key values. Different GPU vendors use different counter names for these roles.

Plugins register vendor-specific metric IDs for well-known roles by calling
`registerWellKnownMetric()` from `section/index.ts`. The core plugin resolves
the first available metric at render time, so traces from any registered vendor
display correctly.

```ts
import {PerfettoPlugin} from '../../public/plugin';
import {registerWellKnownMetric} from '../com.meta.GpuCompute/section';
import GpuComputePlugin from '../com.meta.GpuCompute';

export default class MyVendorPlugin implements PerfettoPlugin {
  static readonly id = 'com.vendor.GpuCompute.MyMetrics';
  static readonly dependencies = [GpuComputePlugin];

  static onActivate(): void {
    registerWellKnownMetric('duration', 'my_vendor__duration_ns');
    registerWellKnownMetric('cycles', 'my_vendor__elapsed_cycles');
    registerWellKnownMetric('frequency', 'my_vendor__clock_freq');
  }
}
```

Multiple IDs can be registered per role as an array. Later registrations
append to existing ones. At render time, the first ID that has data in the
kernel's metrics is used.

Built-in roles registered by `section/speed_of_light.ts`:

| Role | CUDA | AMD |
|---|---|---|
| `duration` | `gpu__time_duration.sum` | `GRBM_TIME_DUR_max` |
| `cycles` | `gpc__cycles_elapsed.max` | `GRBM_GUI_ACTIVE_avr` |
| `frequency` | `gpc__cycles_elapsed.avg.per_second` | `GRBM_GUI_ACTIVE_avr_per_second` |
| `compute_throughput` | `sm__throughput.avg.pct_of_peak_sustained_elapsed` | — |
| `memory_throughput` | `gpu__compute_memory_throughput.avg.pct_of_peak_sustained_elapsed` | — |

### Analysis Provider Plugins

An analysis provider plugin supplies the analysis backend. The core plugin
provides the analysis UI scaffolding and caching; the provider implements the
actual analysis logic (e.g. static heuristics, a language model, or a
combination).

Register a provider by calling `registerAnalysisProvider()` from `analysis.ts`:

```ts
import {PerfettoPlugin} from '../../public/plugin';
import {registerAnalysisProvider} from '../com.meta.GpuCompute/analysis';
import GpuComputePlugin from '../com.meta.GpuCompute';

export default class MyAnalysisPlugin implements PerfettoPlugin {
  static readonly id = 'com.vendor.GpuCompute.Analysis.MyProvider';
  static readonly dependencies = [GpuComputePlugin];

  static onActivate(): void {
    registerAnalysisProvider({
      renderAnalysisTab(attrs) { ... },
      renderSectionAnalysis(attrs) { ... },
    });
  }
}
```

## Core Files

| File | Purpose |
|---|---|
| `index.ts` | Plugin entry point — tab registration and lifecycle |
| `details.ts` | SQL query building, data fetching, metric table rendering |
| `summary.ts` | All-kernel summary table |
| `toolbar.ts` | Toolbar controls (kernel selector, baseline, terminology, units) |
| `section/index.ts` | `Section`, `TableDecl` interfaces and registry (`registerSection` / `getSections`) |
| `section/*.ts` | Built-in metric sections (generated by gen_perfettone) |
| `terminology/index.ts` | `Terminology` interface and registry |
| `terminology/cuda.ts` | CUDA terminology definitions (default) |
| `terminology/opencl.ts` | OpenCL terminology definitions |
| `humanize.ts` | Unit conversion and metric value formatting |
| `analysis.ts` | `AnalysisProvider` interface, registry, and cache |
| `styles.scss` | All component styles (BEM under `.pf-gpu-compute`) |
