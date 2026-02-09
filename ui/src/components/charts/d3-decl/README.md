# Declarative Charts (`d3-decl`)

Stateless, Mithril-native chart components with cross-filtering support.

## Quick Start

```typescript
import {Histogram, InMemoryHistogramLoader} from './components/charts/d3-decl';

class MyComponent {
  private filters: Filter[] = [];
  private loader = new InMemoryHistogramLoader({
    data: [{value: 10}, {value: 20}, {value: 30}],
    valueCol: 'value',
  });
  
  view() {
    const {data} = this.loader.use({bucketCount: 20, filters: this.filters});
    
    return m(Histogram, {
      data,
      filters: this.filters,
      column: 'value',
      onFiltersChanged: (filters) => { this.filters = [...filters]; },
      height: 200,
      xAxisLabel: 'Value',
    });
  }
}
```

## Core Concepts

### Declarative State Flow
Charts are pure functions of input state. Parent owns filters array, chart emits new array on interaction.

```
Parent → filters → Chart → onFiltersChanged(newFilters) → Parent
```

### Complete Filter Array Pattern
Always work with the **complete** filter array, not deltas:

```typescript
// ✅ CORRECT
onFiltersChanged: (filters) => { this.allFilters = [...filters]; }

// ❌ WRONG
onFiltersChanged: (newFilter) => { this.allFilters.push(newFilter); }
```

### Loader Pattern
Loaders fetch and transform data async, with automatic caching:

```typescript
private loader = new SQLHistogramLoader({...});

view() {
  const {data, isPending} = this.loader.use({filters: this.filters});
  return m(Histogram, {data, ...});
}
```

## Available Charts

| Chart | Use Case | Interactions |
|-------|----------|--------------|
| **Histogram** | Value distributions | Brush to filter range |
| **SimpleBarChart** | Single series categorical | Brush categories |
| **GroupedBarChart** | Multi-series side-by-side | Brush categories |
| **StackedBarChart** | Multi-series stacked | Brush categories |
| **CDFChart** | Percentile analysis | Brush range |
| **ScatterPlot** | Two-variable correlation | 2D brush selection |

## API Reference

### Common Props

All chart components follow this interface:

```typescript
interface ChartAttrs {
  readonly data: ChartData | undefined;         // undefined = loading
  readonly filters: readonly Filter[];          // Complete filter array
  readonly column: string;                      // Column this chart operates on
  readonly onFiltersChanged?: (filters: readonly Filter[]) => void;
  readonly height?: number;                     // Default: 200
  readonly xAxisLabel?: string;
  readonly yAxisLabel?: string;
  readonly formatXValue?: (value: unknown) => string;
  readonly formatYValue?: (value: number) => string;
  readonly fillParent?: boolean;
  readonly className?: string;
}
```

### Chart-Specific Props

**Histogram**:
```typescript
{barColor?: string, barHoverColor?: string, logScale?: boolean}
```

**SimpleBarChart**:
```typescript
{sort?: {by: 'category' | 'value', direction: 'asc' | 'desc'}}
```

**Grouped/StackedBarChart**:
```typescript
{colors?: readonly string[], showLegend?: boolean}
```

**CDFChart**:
```typescript
{showPercentiles?: boolean, onLineClick?: (lineName: string) => void}
```

**ScatterPlot**:
```typescript
{
  xColumn: string,  // Note: two columns
  yColumn: string,
  showCorrelation?: boolean,
  pointColor?: string,
  pointSize?: number
}
```

## Architecture

### Component Hierarchy
```
d3-decl/
├── index.ts              # Public API
├── chart_utils.ts        # Shared utilities (formatNumber, etc.)
├── filter_utils.ts       # Filter engines (in-memory & SQL)
├── renderers/            # Reusable rendering functions
│   ├── axis_renderer.ts  # renderLinearAxis, renderBandAxis
│   └── legend_renderer.ts
├── interactions/         # Reusable interaction handlers
│   └── brush_handler.ts  # BrushHandler1D, BrushHandler2D, BrushHandlerCategorical
└── [chart]/              # Chart-specific modules
    ├── index.ts
    ├── [chart].ts        # Component
    ├── [chart]_loader.ts # Data loading
    └── [chart].scss
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

## Integration Patterns

### Cross-Filtering Dashboard

```typescript
class Dashboard {
  private filters: Filter[] = [];
  
  view() {
    return [
      m(DataGrid, {
        data: dataSource,
        filters: this.filters,
        onFiltersChanged: (f) => { this.filters = [...f]; },
      }),
      
      m(Histogram, {
        data: histLoader.use({filters: this.filters}),
        filters: this.filters,
        column: 'duration',
        onFiltersChanged: (f) => { this.filters = [...f]; },
      }),
      
      m(BarChart, {
        data: barLoader.use({filters: this.filters}),
        filters: this.filters,
        column: 'status',
        onFiltersChanged: (f) => { this.filters = [...f]; },
      }),
    ];
  }
}
```

**Result**: Filter in any component → all others update automatically.

## Design Principles

1. **Single Responsibility** - One file = one concern (400-500 lines max)
2. **Immutable Data Flow** - Create new arrays, never mutate
3. **Zero Magic** - No hardcoded values, no implicit behavior, no `any` types
4. **Composition Over Inheritance** - Shared functionality via utility functions

## Best Practices

```typescript
// Always readonly
interface MyAttrs {
  readonly filters: readonly Filter[];
}

// Create new arrays
onFiltersChanged: (filters) => { this.filters = [...filters]; }

// Dispose loaders
onremove() { this.loader.dispose(); }

// Use constants
const DEFAULT_HEIGHT = 200;
```

## Examples

See [`d3_decl_charts_demo.ts`](../../plugins/dev.perfetto.WidgetsPage/demos/d3_decl_charts_demo.ts) for:
- Cross-filtering dashboard
- DataGrid + Charts integration
- Custom formatters
- Sort configurations
- Multi-series charts

## Summary

- **Declarative**: Charts are pure functions of attrs
- **Stateless**: Parent owns all data state
- **Complete Arrays**: Always work with full filter array
- **Composable**: Shared renderers and utilities
- **Type-Safe**: No `any` types, all readonly interfaces
- **Mithril Native**: Zero D3 DOM manipulation

---

Built for Perfetto UI
