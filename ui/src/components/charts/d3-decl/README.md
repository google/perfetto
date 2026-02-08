# Declarative Charts (`d3-decl`)

Stateless, Mithril-native chart components with cross-filtering support. Built for the Perfetto UI with clean architecture and zero anti-patterns.

---

## Table of Contents

1. [Quick Start](#quick-start)
2. [Core Concepts](#core-concepts)
3. [Available Charts](#available-charts)
4. [API Reference](#api-reference)
5. [Design Principles](#design-principles)
6. [Architecture](#architecture)
7. [File Structure](#file-structure)
8. [Integration Patterns](#integration-patterns)
9. [Advanced Usage](#advanced-usage)

---

## Quick Start

### Basic Histogram

```typescript
import {Histogram, InMemoryHistogramLoader} from './components/charts/d3-decl';
import {Filter} from './components/widgets/datagrid/model';

class MyComponent {
  private filters: Filter[] = [];
  private loader = new InMemoryHistogramLoader({
    data: [{value: 10}, {value: 20}, {value: 30}],
    valueCol: 'value',
  });
  
  view() {
    const {data} = this.loader.use({
      bucketCount: 20,
      filters: this.filters,
    });
    
    return m(Histogram, {
      data,
      filters: this.filters,
      column: 'value',
      onFiltersChanged: (filters) => {
        this.filters = [...filters];
      },
      height: 200,
      xAxisLabel: 'Value',
    });
  }
}
```

That's it! The histogram will render, support brushing to filter, and automatically update when filters change.

### SQL-Based Histogram

```typescript
import {SQLHistogramLoader} from './components/charts/d3-decl';

class SQLExample {
  private filters: Filter[] = [];
  private loader: SQLHistogramLoader;
  
  constructor(engine: Engine) {
    this.loader = new SQLHistogramLoader({
      engine,
      query: 'SELECT dur FROM slice WHERE dur > 0',
      valueColumn: 'dur',
    });
  }
  
  view() {
    const {data} = this.loader.use({
      bucketCount: 30,
      filters: this.filters,
    });
    
    return m(Histogram, {
      data,
      filters: this.filters,
      column: 'dur',
      onFiltersChanged: (filters) => {
        this.filters = [...filters];
      },
    });
  }
  
  onremove() {
    this.loader.dispose();
  }
}
```

---

## Core Concepts

### 1. Declarative State Flow

Charts are **pure functions** of their input state. State flows down, events flow up:

```
Parent Component
  ├── filters: Filter[] (owns state)
  └── Chart Component
        ├── Receives: data, filters, column
        ├── Renders: Pure function of attrs
        └── Emits: onFiltersChanged(newFilters)
```

**No internal data state** - only UI transients like hover state.

### 2. Complete Filter Array Pattern

Charts always work with the **complete** filter array, not deltas:

```typescript
// ✅ CORRECT: Return complete new array
onFiltersChanged: (filters) => {
  this.allFilters = [...filters];  // Replace entire array
}

// ❌ WRONG: Don't mutate or track deltas
onFiltersChanged: (newFilter) => {
  this.allFilters.push(newFilter);  // Mutation!
}
```

This enables **cross-filtering**: all charts see all filters.

### 3. Loaders Handle Data

Loaders fetch and transform data async, with automatic caching:

```typescript
// Loader persists across renders
private loader = new SQLHistogramLoader({...});

view() {
  // .use() returns cached data when config unchanged
  const {data, isPending} = this.loader.use({
    bucketCount: 20,
    filters: this.filters,  // Changing filters triggers re-fetch
  });
  
  return m(Histogram, {data, ...});
}
```

### 4. Mithril Owns DOM

All SVG rendering uses Mithril's hyperscript. D3 only used for scales and math:

```typescript
// ✅ Mithril vnodes
m('svg', 
  m('rect', {x: 0, y: 0, width: 10, height: 20})
)

// ❌ Not this
d3.select('svg').append('rect').attr('x', 0)...
```

Mithril handles DOM diffing automatically. No manual updates needed.

---

## Available Charts

### Histogram
Distribution visualization with bucket counts.
- **Use case**: Duration distributions, value frequencies
- **Interactions**: Brush to filter range, click bucket for exact range
- **Features**: Linear/log Y-axis, custom formatters

### Bar Charts
Categorical data visualization in three variants:
- **SimpleBarChart**: Single series
- **GroupedBarChart**: Multiple series side-by-side
- **StackedBarChart**: Multiple series stacked vertically

### CDF Chart
Cumulative distribution function visualization.
- **Use case**: Percentile analysis (P50, P90, P95, P99)
- **Features**: Multi-line support, percentile markers, crosshair tooltips

### Scatter Plot
Two-variable correlation visualization.
- **Use case**: Finding relationships between metrics
- **Interactions**: 2D rectangular brush selection
- **Features**: Correlation line, category-based coloring

---

## API Reference

### Chart Component Pattern

All chart components follow this interface:

```typescript
interface ChartAttrs {
  /**
   * Data to display. Undefined while loading.
   */
  readonly data: ChartData | undefined;
  
  /**
   * Complete filter array for cross-filtering.
   */
  readonly filters: readonly Filter[];
  
  /**
   * Column(s) this chart operates on.
   */
  readonly column: string;  // or xColumn, yColumn for scatter
  
  /**
   * Called when user modifies filters.
   * Receives complete new filter array.
   */
  readonly onFiltersChanged?: (filters: readonly Filter[]) => void;
  
  /**
   * Display height in pixels. Defaults to 200.
   */
  readonly height?: number;
  
  /**
   * Axis labels.
   */
  readonly xAxisLabel?: string;
  readonly yAxisLabel?: string;
  
  /**
   * Custom formatters for axes and tooltips.
   */
  readonly formatXValue?: (value: number) => string;
  readonly formatYValue?: (value: number) => string;
  
  /**
   * Styling props.
   */
  readonly fillParent?: boolean;
  readonly className?: string;
}
```

### Histogram Specific Props

```typescript
interface HistogramAttrs extends ChartAttrs {
  readonly data: HistogramData | undefined;
  readonly barColor?: string;
  readonly barHoverColor?: string;
  readonly logScale?: boolean;  // Use log scale for Y-axis
}
```

### Bar Chart Specific Props

```typescript
interface SimpleBarChartAttrs extends ChartAttrs {
  readonly data: SimpleBarData | undefined;
  readonly sort?: {by: 'category' | 'value'; direction: 'asc' | 'desc'};
}

interface GroupedBarChartAttrs extends ChartAttrs {
  readonly data: GroupedBarData | undefined;
  // Grouped data has {category, value, group} structure
}
```

### CDF Chart Specific Props

```typescript
interface CDFChartAttrs extends ChartAttrs {
  readonly data: CDFData | undefined;
  readonly showPercentiles?: boolean;  // Show P50, P90, P95, P99 lines
  readonly onLineClick?: (lineName: string) => void;  // For multi-line CDFs
  readonly title?: string;
}
```

### Scatter Plot Specific Props

```typescript
interface ScatterPlotAttrs {
  readonly data: ScatterData | undefined;
  readonly filters: readonly Filter[];
  readonly xColumn: string;  // Note: two columns
  readonly yColumn: string;
  readonly onFiltersChanged?: (filters: readonly Filter[]) => void;
  readonly pointColor?: string;
  readonly pointSize?: number;
  readonly showCorrelation?: boolean;  // Show regression line
  // ... standard props
}
```

---

## Design Principles

### 1. Single Responsibility
- **One file = one concern** (400-500 lines max)
- Charts only render
- Loaders only fetch data
- Utilities only provide calculations
- Renderers only generate DOM

### 2. Immutable Data Flow
```typescript
// ✅ Create new arrays
const newFilters = [...oldFilters, newFilter];

// ❌ Don't mutate
oldFilters.push(newFilter);
```

### 3. Zero Magic
- No hardcoded values (use constants)
- No implicit behavior (explicit props)
- No side effects in render
- No `any` types

### 4. Composition Over Inheritance
Shared functionality via utility functions, not base classes:
```typescript
// Shared axis rendering
renderLinearAxis({scale, orientation, length, label});

// Shared brush handling
new BrushHandler1D(svg, scale, margin, width, onBrush, onClear);
```

---

## Architecture

### Component Hierarchy

```
ui/src/components/charts/d3-decl/
├── index.ts              # Public API exports
├── chart_utils.ts        # Shared utilities (formatNumber, etc.)
├── filter_utils.ts       # Filter engines (in-memory & SQL)
│
├── renderers/            # Reusable rendering functions
│   ├── axis_renderer.ts  # renderLinearAxis, renderBandAxis
│   ├── legend_renderer.ts
│   └── axis.scss
│
├── interactions/         # Reusable interaction handlers
│   └── brush_handler.ts  # BrushHandler1D, BrushHandler2D
│
├── histogram/            # Chart-specific modules
│   ├── index.ts
│   ├── histogram.ts      # Component
│   ├── histogram_loader.ts  # Data loading
│   └── histogram.scss
│
├── bar/
│   ├── index.ts
│   ├── simple_bar_chart.ts
│   ├── grouped_bar_chart.ts
│   ├── stacked_bar_chart.ts
│   ├── bar_loader.ts
│   ├── bar_types.ts
│   ├── bar_utils.ts
│   └── bar.scss
│
├── cdf/
│   ├── index.ts
│   ├── cdf.ts
│   ├── cdf_loader.ts
│   └── cdf.scss
│
└── scatter/
    ├── index.ts
    ├── scatter.ts
    ├── scatter_loader.ts
    └── scatter.scss
```

### Data Flow

```
1. Parent renders with filters
2. Loader.use({filters}) fetches/computes data
3. Chart receives data + filters in attrs
4. Chart renders SVG (pure function)
5. User interacts (brush, click)
6. Chart computes new filter array
7. Chart calls onFiltersChanged(newFilters)
8. Parent updates filters
9. Mithril auto-redraws → back to step 1
```

### Loader Architecture

Loaders use `QuerySlot` for automatic caching:

```typescript
class SQLHistogramLoader {
  private querySlot = new QuerySlot<HistogramData>(taskQueue);
  
  use(config: HistogramLoaderConfig): HistogramLoaderResult {
    const result = this.querySlot.use({
      key: {
        query: this.baseQuery,
        filters: JSON.stringify(config.filters),
        bucketCount: config.bucketCount,
      },
      queryFn: async () => {
        // Only runs if key changed
        const sql = this.buildSQL(config);
        return await this.engine.query(sql);
      },
    });
    
    return {data: result.data, isPending: result.isPending};
  }
}
```

**Key insight**: Same config = cached result. No redundant queries.

---

## File Structure

### Core Utilities

#### [`chart_utils.ts`](./chart_utils.ts)
Shared math and formatting utilities:
- `formatNumber(value)` - Smart number formatting
- `formatDuration(ns)` - Nanoseconds to human-readable
- `generateTicks(min, max, count)` - Tick value generation
- `calculateCorrelation(xValues, yValues)` - Pearson correlation
- `niceDomain(min, max)` - Round domain bounds
- Constants: `DEFAULT_MARGIN`, `VIEWBOX_WIDTH`

#### [`filter_utils.ts`](./filter_utils.ts)
Filter application engines:
- `InMemoryFilterEngine.apply(data, filters)` - Client-side filtering
- `SQLFilterEngine.toSQL(filters)` - Generate WHERE clauses

### Renderers

#### [`renderers/axis_renderer.ts`](./renderers/axis_renderer.ts)
Pure functions for axis rendering:
- `renderLinearAxis(config)` - Numeric axes
- `renderBandAxis(config)` - Categorical axes
- `renderGridLines(config)` - Background grid

#### [`renderers/legend_renderer.ts`](./renderers/legend_renderer.ts)
Legend rendering for multi-series charts:
- `renderLegend({items, position})`

### Interactions

#### [`interactions/brush_handler.ts`](./interactions/brush_handler.ts)
Stateful brush interaction handlers:
- `BrushHandler1D` - Horizontal brush (histogram, CDF)
- `BrushHandler2D` - Rectangular brush (scatter)
- `BrushHandlerCategorical` - Category selection (bar charts)

**Key methods**:
- `getEventHandlers()` - Returns Mithril event props
- `getCurrentBrush()` - Get in-progress selection
- `updateScale(newScale)` - Update after re-render

### Charts

Each chart follows the same structure:

#### `histogram/`
- **histogram.ts**: Chart component (470 lines)
- **histogram_loader.ts**: Data loading + computation (508 lines)
  - `InMemoryHistogramLoader` - For static data
  - `SQLHistogramLoader` - For SQL queries
  - `computeHistogram()` - Pure histogram computation
- **index.ts**: Public exports

#### `bar/`
- **simple_bar_chart.ts**: Single series bar chart
- **grouped_bar_chart.ts**: Multi-series side-by-side
- **stacked_bar_chart.ts**: Multi-series stacked
- **bar_loader.ts**: Data loading for all variants
- **bar_types.ts**: Type definitions
- **bar_utils.ts**: Shared utilities (sorting, color scales)

#### `cdf/`
- **cdf.ts**: CDF line chart with percentiles
- **cdf_loader.ts**: CDF computation from raw values

#### `scatter/`
- **scatter.ts**: Scatter plot with 2D brush
- **scatter_loader.ts**: Point data + correlation

---

## Integration Patterns

### DataGrid + Charts

Share the same `filters` array for bidirectional filtering:

```typescript
class Dashboard {
  private filters: Filter[] = [];
  
  view() {
    return [
      // DataGrid
      m(DataGrid, {
        data: dataSource,
        filters: this.filters,
        onFiltersChanged: (filters) => {
          this.filters = [...filters];
        },
      }),
      
      // Chart (shares same filters)
      m(Histogram, {
        data: histogramLoader.use({filters: this.filters}),
        filters: this.filters,
        column: 'duration',
        onFiltersChanged: (filters) => {
          this.filters = [...filters];
        },
      }),
    ];
  }
}
```

**Result**: Filter in grid → chart updates. Brush in chart → grid updates.

### Cross-Filtering Multiple Charts

All charts share one filter array:

```typescript
class Dashboard {
  private filters: Filter[] = [];
  
  view() {
    return [
      m(Histogram, {
        data: histLoader.use({filters: this.filters}),
        filters: this.filters,
        column: 'duration',
        onFiltersChanged: (f) => { this.filters = [...f]; },
      }),
      
      m(BarChart, {
        data: barLoader.use({filters: this.filters}),
        filters: this.filters,
        column: 'category',
        onFiltersChanged: (f) => { this.filters = [...f]; },
      }),
      
      m(ScatterPlot, {
        data: scatterLoader.use({filters: this.filters}),
        filters: this.filters,
        xColumn: 'x',
        yColumn: 'y',
        onFiltersChanged: (f) => { this.filters = [...f]; },
      }),
    ];
  }
}
```

**Key insight**: Every chart sees **all** filters, enabling cross-filtering.

---

## Advanced Usage

### Custom Formatters

```typescript
import {formatDuration} from './chart_utils';

m(Histogram, {
  data,
  filters,
  column: 'dur',
  onFiltersChanged: (f) => { this.filters = [...f]; },
  formatXValue: formatDuration,  // Convert ns to "1.2ms"
  formatYValue: (v) => `${v} slices`,
});
```

### Log Scale Histogram

For exponential distributions:

```typescript
m(Histogram, {
  data,
  filters,
  column: 'duration',
  onFiltersChanged: (f) => { this.filters = [...f]; },
  logScale: true,  // Y-axis uses log scale
});
```

### Sorted Bar Chart

```typescript
m(SimpleBarChart, {
  data,
  filters,
  column: 'process',
  onFiltersChanged: (f) => { this.filters = [...f]; },
  sort: {by: 'value', direction: 'desc'},  // Top values first
});
```

### Multi-Line CDF

```typescript
// Loader computes separate CDF per group
const loader = new InMemoryCDFLoader({
  data: myData,
  valueCol: 'latency',
  groupCol: 'region',  // Creates one line per region
});

m(CDFChart, {
  data: loader.use({filters: this.filters}).data,
  filters: this.filters,
  column: 'latency',
  showPercentiles: true,  // Show P50, P90, P95, P99
  onFiltersChanged: (f) => { this.filters = [...f]; },
});
```

### Scatter Plot with Correlation

```typescript
const loader = new InMemoryScatterLoader({
  data: myData,
  xCol: 'duration',
  yCol: 'cpu_time',
  categoryCol: 'priority',  // Optional: color by category
});

m(ScatterPlot, {
  data: loader.use({
    filters: this.filters,
    computeCorrelation: true,  // Compute Pearson r
  }).data,
  filters: this.filters,
  xColumn: 'duration',
  yColumn: 'cpu_time',
  showCorrelation: true,  // Show regression line + r value
  onFiltersChanged: (f) => { this.filters = [...f]; },
});
```

### Conditional Filtering

Only apply filters to some charts:

```typescript
// "Source" chart doesn't filter itself
m(Histogram, {
  data: loader.use({filters: []}),  // Empty filters - always show all data
  filters: this.filters,  // But still shows brush overlay
  column: 'duration',
  onFiltersChanged: (f) => { this.filters = [...f]; },
});

// "Target" chart filters based on source
m(BarChart, {
  data: barLoader.use({filters: this.filters}),  // Filtered
  filters: this.filters,
  column: 'process',
  onFiltersChanged: (f) => { this.filters = [...f]; },
});
```

---

## Examples

### Example 1: Simple Histogram

```typescript
import {Histogram, InMemoryHistogramLoader} from './components/charts/d3-decl';

class HistogramExample {
  private filters = [];
  private loader = new InMemoryHistogramLoader({
    data: Array.from({length: 1000}, () => ({
      value: Math.random() * 100,
    })),
    valueCol: 'value',
  });
  
  view() {
    return m(Histogram, {
      data: this.loader.use({bucketCount: 30, filters: this.filters}).data,
      filters: this.filters,
      column: 'value',
      onFiltersChanged: (f) => { this.filters = [...f]; },
      height: 250,
      xAxisLabel: 'Value',
      yAxisLabel: 'Frequency',
    });
  }
}
```

### Example 2: SQL Bar Chart with Sorting

```typescript
import {SimpleBarChart, SQLBarLoader} from './components/charts/d3-decl';

class TopProcesses {
  private filters = [];
  private loader: SQLBarLoader;
  
  constructor(engine: Engine) {
    this.loader = new SQLBarLoader({
      engine,
      query: `
        SELECT name, COUNT(*) as count 
        FROM slice 
        GROUP BY name 
        ORDER BY count DESC 
        LIMIT 20
      `,
      categoryCol: 'name',
      valueCol: 'count',
    });
  }
  
  view() {
    return m(SimpleBarChart, {
      data: this.loader.use({filters: this.filters}).data,
      filters: this.filters,
      column: 'name',
      onFiltersChanged: (f) => { this.filters = [...f]; },
      sort: {by: 'value', direction: 'desc'},
      yAxisLabel: 'Slice Count',
    });
  }
  
  onremove() {
    this.loader.dispose();
  }
}
```

### Example 3: Cross-Filtering Dashboard

```typescript
class CrossFilterDashboard {
  private filters: Filter[] = [];
  private histLoader: SQLHistogramLoader;
  private barLoader: SQLBarLoader;
  private scatterLoader: SQLScatterLoader;
  
  constructor(engine: Engine) {
    this.histLoader = new SQLHistogramLoader({
      engine,
      query: 'SELECT dur FROM slice WHERE dur > 0',
      valueColumn: 'dur',
    });
    
    this.barLoader = new SQLBarLoader({
      engine,
      query: 'SELECT name, COUNT(*) as count FROM slice GROUP BY name',
      categoryCol: 'name',
      valueCol: 'count',
    });
    
    this.scatterLoader = new SQLScatterLoader({
      engine,
      query: 'SELECT dur, depth FROM slice WHERE dur > 0 LIMIT 1000',
      xCol: 'dur',
      yCol: 'depth',
    });
  }
  
  view() {
    return m('.dashboard', [
      m('h2', 'Cross-Filtering Dashboard'),
      
      // Active filters display
      this.filters.length > 0 && m('.filters', [
        'Active Filters: ',
        this.filters.map(f => `${f.field} ${f.op} ${f.value}`).join(', '),
        m('button', {onclick: () => { this.filters = []; }}, 'Clear All'),
      ]),
      
      // All charts share this.filters
      m('.charts-grid', {
        style: 'display: grid; grid-template-columns: 1fr 1fr; gap: 16px;',
      }, [
        m(Histogram, {
          data: this.histLoader.use({filters: this.filters, bucketCount: 30}).data,
          filters: this.filters,
          column: 'dur',
          onFiltersChanged: (f) => { this.filters = [...f]; },
          xAxisLabel: 'Duration (ns)',
        }),
        
        m(SimpleBarChart, {
          data: this.barLoader.use({filters: this.filters}).data,
          filters: this.filters,
          column: 'name',
          onFiltersChanged: (f) => { this.filters = [...f]; },
          yAxisLabel: 'Count',
        }),
        
        m(ScatterPlot, {
          data: this.scatterLoader.use({filters: this.filters}).data,
          filters: this.filters,
          xColumn: 'dur',
          yColumn: 'depth',
          onFiltersChanged: (f) => { this.filters = [...f]; },
          xAxisLabel: 'Duration',
          yAxisLabel: 'Depth',
        }),
      ]),
    ]);
  }
  
  onremove() {
    this.histLoader.dispose();
    this.barLoader.dispose();
    this.scatterLoader.dispose();
  }
}
```

---

## Best Practices

### DO ✅

1. **Always use readonly for props**
   ```typescript
   interface MyAttrs {
     readonly filters: readonly Filter[];
   }
   ```

2. **Create new arrays when updating**
   ```typescript
   onFiltersChanged: (filters) => {
     this.filters = [...filters];  // New array
   }
   ```

3. **Dispose loaders on component removal**
   ```typescript
   onremove() {
     this.loader.dispose();
   }
   ```

4. **Use constants for magic numbers**
   ```typescript
   const DEFAULT_HEIGHT = 200;
   const DEFAULT_BUCKET_COUNT = 30;
   ```

5. **Provide meaningful axis labels**
   ```typescript
   m(Histogram, {
     xAxisLabel: 'Latency (ms)',
     yAxisLabel: 'Request Count',
   });
   ```

### DON'T ❌

1. **Don't mutate filter arrays**
   ```typescript
   // ❌ BAD
   this.filters.push(newFilter);
   ```

2. **Don't store data in components**
   ```typescript
   // ❌ BAD
   class Histogram {
     private data: HistogramData;  // NO - data comes from attrs
   }
   ```

3. **Don't call m.redraw() after events**
   ```typescript
   // ❌ BAD
   onclick: () => {
     this.update();
     m.redraw();  // Unnecessary - Mithril auto-redraws
   }
   ```

4. **Don't use D3 for DOM**
   ```typescript
   // ❌ BAD
   d3.select('svg').append('rect');
   
   // ✅ GOOD
   m('svg', m('rect', {...}));
   ```

5. **Don't hardcode values**
   ```typescript
   // ❌ BAD
   const margin = {top: 10, left: 50};
   
   // ✅ GOOD
   import {DEFAULT_MARGIN} from './chart_utils';
   ```

---

## Summary

### Key Takeaways

1. **Declarative**: Charts are pure functions of attrs
2. **Stateless**: Parent owns all data state
3. **Complete Arrays**: Always work with full filter array
4. **Composable**: Shared renderers and utilities
5. **Type-Safe**: No any types, all readonly interfaces
6. **Mithril Native**: Zero D3 DOM manipulation

### When to Use This Library

✅ **Good fit**:
- Interactive dashboards with filtering
- Cross-filtering between multiple views
- SQL-backed analytics visualizations
- Perfetto trace analysis UI

❌ **Not ideal for**:
- Static chart images (use D3 directly)
- 3D visualizations
- Real-time streaming (needs optimization)
- Extremely custom chart types (extend carefully)

### Getting Help

- **Examples**: See `DeclarativeChartsPage` plugin
- **Tests**: (To be added)
- **Issues**: File in Perfetto GitHub

---

Built with ❤️ for Perfetto UI
