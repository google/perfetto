# D3 Interactive Charting

Interactive charting library for Perfetto UI and BigTrace. 9 chart types, brush selection, cross-filtering, SQL/HTTP data sources.

## Architecture

Four layers:

```
┌─────────────────────────────────────────────────────────────┐
│  Applications (BigTrace)                                    │
│  bigtrace/index.ts                                          │
└─────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────┐
│  Plugins (D3ChartsPage, Explore Page)                       │
│  plugins/dev.perfetto.D3ChartsPage/                         │
│  plugins/dev.perfetto.ExplorePage/                          │
└─────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────┐
│  Components (Data Sources)                                  │
│  components/d3/sql_data_source.ts - local trace queries     │
│  components/d3/http_data_source.ts - Brush backend          │
└─────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────┐
│  Widgets (Core Library)                                     │
│  widgets/charts/d3/ - Chart, BaseRenderer, FilterStore      │
└─────────────────────────────────────────────────────────────┘
```

### Widgets Layer

Core primitives:
- [`Chart`](charts/chart.ts) - manages data lifecycle, subscribes to FilterStore
- [`BaseRenderer`](charts/base_renderer.ts) - D3 rendering shared by all chart types
- [`FilterStore`](data/filter_store.ts) - coordinates cross-chart filtering
- [`MemorySource`](data/memory_source.ts) - client-side filtering/aggregation

```typescript
const source = new MemorySource([{dur: 100}, {dur: 200}]);
const filterStore = new FilterStore();
const chart = new Chart({type: 'histogram', x: 'dur'}, source, filterStore);
```

### Components Layer

Perfetto data sources implementing [`DataSource`](data/source.ts):

**SqlDataSource** - queries local trace
```typescript
const source = new SqlDataSource(engine, 'SELECT name, dur FROM slice');
```

**HttpDataSource** - queries Brush backend
```typescript
const source = new HttpDataSource(
  'SELECT anr_type, COUNT(*) FROM android_anr GROUP BY anr_type',
  'android_telemetry.field_trace_summaries_prod.last30days',
  10000
);
```

### Plugins Layer

**D3ChartsPage** ([`plugins/dev.perfetto.D3ChartsPage/`](../../plugins/dev.perfetto.D3ChartsPage/))
Full chart creator:
- SQL editor (Ctrl+Enter execution)
- Auto-creates histogram + bar on query
- Sidebar for manual chart/table creation
- Toggle filter mode (source chart updates itself or not)

```typescript
m(D3ChartsPage, {
  useBrushBackend: true,
  initialQuery: 'SELECT * FROM slice',
});
```

**Explore Page** ([`plugins/dev.perfetto.ExplorePage/`](../../plugins/dev.perfetto.ExplorePage/))
Embed charts in trace analysis:
```typescript
const chart = new Chart(spec, new SqlDataSource(engine, query), filterStore);
m(ChartWidget, {chart});
```

### Applications Layer

**BigTrace** ([`bigtrace/index.ts`](../../bigtrace/index.ts))
Full app using D3ChartsPage + HttpDataSource for remote trace queries.

## Chart Types

### Histogram
```typescript
{type: 'histogram', x: 'dur', bins: 20}
```
Brush: range selection → `dur >= min AND dur <= max`

### CDF
```typescript
{type: 'cdf', x: 'dur', colorBy: 'name'}
```
Percentile distribution. Brush: percentile range.

### Bar
```typescript
{
  type: 'bar',
  x: 'name',
  y: 'dur',
  aggregation: 'sum',  // sum | avg | count | min | max
  groupBy: 'state',    // optional
  mode: 'stacked',     // grouped | stacked
  sort: {by: 'y', direction: 'desc'}
}
```
Brush: click or drag across bars.

SQL example:
```sql
SELECT name, state, SUM(dur) as dur FROM slice GROUP BY name, state
```

### Scatter
```typescript
{
  type: 'scatter',
  x: 'dur',
  y: 'ts',
  colorBy: 'name',
  showCorrelation: true
}
```
Brush: rectangular selection.

### Line
```typescript
{
  type: 'line',
  x: 'ts',
  y: 'value',
  aggregation: 'avg',
  colorBy: 'cpu',
  sort: {by: 'x', direction: 'asc'}
}
```
Brush: vertical range. Crosshair on hover.

SQL example:
```sql
SELECT ts, cpu, AVG(freq) as value
FROM counter WHERE name = 'cpufreq'
GROUP BY ts, cpu ORDER BY ts
```

### Boxplot
```typescript
{type: 'boxplot', x: 'name', y: 'dur'}
```
Shows min, Q1, median, Q3, max per category.

### Violin
```typescript
{type: 'violin', x: 'name', y: 'dur'}
```
Boxplot + KDE density.

### Heatmap
```typescript
{
  type: 'heatmap',
  x: 'hour',
  y: 'day',
  value: 'count',
  aggregation: 'sum'
}
```
2D categorical aggregation with color scale.

### Donut
```typescript
{
  type: 'donut',
  category: 'name',
  value: 'dur',
  aggregation: 'sum'
}
```
Click slice to filter.

## Cross-Chart Filtering

Two modes controlled by `filterStore.setUpdateSourceChart(bool)`:

**Filter Mode (default)** - source chart reloads, filtered data disappears:
```
Brush Chart A → Chart A reloads (filtered) + Chart B/C reload (filtered)
```

**Opacity Mode** - source chart dims non-selected at 20%:
```
Brush Chart A → Chart A dims unselected + Chart B/C reload (filtered)
```

Implementation via SelectionStrategy pattern (FilterSelectionStrategy vs OpacitySelectionStrategy).

## Multiple Chart Groups

Separate FilterStores = independent cross-filtering. Charts can share DataSource.

```typescript
const source = new SqlDataSource(engine, 'SELECT ts, dur, name FROM slice');

// Group 1: cross-filters independently
const store1 = new FilterStore();
const chart1a = new Chart({type: 'histogram', x: 'dur'}, source, store1);
const chart1b = new Chart({type: 'bar', x: 'name', y: 'dur', aggregation: 'sum'}, source, store1);

// Group 2: cross-filters independently
const store2 = new FilterStore();
const chart2a = new Chart({type: 'histogram', x: 'dur'}, source, store2);
const chart2b = new Chart({type: 'bar', x: 'name', y: 'dur', aggregation: 'sum'}, source, store2);
```

Brush chart1a → chart1b updates. chart2a/chart2b unaffected.

**Example: Compare thread states**
```typescript
const source = new SqlDataSource(engine, 'SELECT state, dur, utid FROM thread_state');

const runnableStore = new FilterStore();
runnableStore.setFilterGroup({
  id: 'base',
  filters: [{col: 'state', op: '=', val: 'R'}]
}, 'init');

const sleepingStore = new FilterStore();
sleepingStore.setFilterGroup({
  id: 'base',
  filters: [{col: 'state', op: '=', val: 'S'}]
}, 'init');

// Create charts with respective stores
const runnableHist = new Chart({type: 'histogram', x: 'dur'}, source, runnableStore);
const sleepingHist = new Chart({type: 'histogram', x: 'dur'}, source, sleepingStore);
```

**Partial sync between groups:**
```typescript
groupA.subscribe((notification) => {
  const nameFilters = notification.filters.filter(f => f.col === 'name');
  if (nameFilters.length > 0) {
    groupB.setFilterGroup({id: 'synced', filters: nameFilters}, 'sync');
  }
});
```

**Note:** Charts are bound to FilterStore at construction. To change group membership, recreate the Chart instance with a different FilterStore.

| Scenario | Pattern |
|----------|---------|
| Compare processes/threads | Shared DataSource + separate FilterStores |
| Compare tables (slice vs thread_state) | Separate DataSources + separate FilterStores |
| All charts filter together | Shared DataSource + single FilterStore |
| Sync specific columns only | Separate FilterStores + subscription bridge |

## Chart-Table Integration

Charts and tables use shared FilterStore:

```typescript
const filterStore = new FilterStore();

// Chart
const chart = new Chart(spec, sqlSource, filterStore);

// Table
filterStore.subscribe((notification) => {
  const query = buildQueryWithFilters(baseQuery, notification.filters);
  updateTable(engine.query(query));
});

// Table row click
onRowClick: (row) => {
  filterStore.setFilterGroup({
    id: 'table',
    filters: [{col: 'name', op: '=', val: row.name}]
  }, 'table');
}
```

## File Structure

```
d3/
├── data/
│   ├── types.ts          # Filter, ChartSpec, Row
│   ├── source.ts         # DataSource interface
│   ├── memory_source.ts  # Client-side filtering
│   └── filter_store.ts   # Observable filter coordination
├── charts/
│   ├── base_renderer.ts  # Shared D3 rendering
│   ├── chart.ts          # Data lifecycle
│   ├── registry.ts       # Type → renderer map
│   ├── bar.ts, histogram.ts, cdf.ts, scatter.ts, line.ts
│   ├── boxplot.ts, violin.ts, heatmap.ts, donut.ts
│   ├── selection/        # Strategy pattern for filter/opacity modes
│   └── brushing/         # Brush behavior delegates
├── chart_widget.ts       # Mithril wrapper
├── d3_types.ts           # D3 selection helpers
├── tooltip.ts            # Singleton tooltip
└── index.ts              # Public API
```

## Adding Chart Types

1. Create renderer extending [`BaseRenderer`](charts/base_renderer.ts):
```typescript
export class MyChartRenderer extends BaseRenderer {
  render(svg: SVGElement, data: Row[], spec: ChartSpec) {
    if (spec.type !== 'my_chart') return;
    this.clear(svg);
    const g = this.createGroup(svg);
    // Use this.createLinearScale(), this.drawAxes(), this.setupTooltip()
  }
}
```

2. Add type to [`data/types.ts`](data/types.ts):
```typescript
export type ChartSpec = ... | {type: 'my_chart'; field: string};
```

3. Register in [`charts/registry.ts`](charts/registry.ts):
```typescript
export const RENDERERS = {
  my_chart: () => new MyChartRenderer(),
  ...
};
```

## Testing

```typescript
// Renderer
const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
new HistogramRenderer().render(svg, mockData, mockSpec);

// Chart
class MockSource implements DataSource {
  async query() { return mockData; }
}
const chart = new Chart(spec, new MockSource(), filterStore);
```

## BigTrace Integration

```typescript
// bigtrace/index.ts
m(D3ChartsPage, {
  useBrushBackend: true,
  initialQuery: `
    SELECT anr_type, process_name, COUNT(*) as count
    FROM android_anr
    WHERE timestamp > DATE_SUB(CURRENT_DATE(), INTERVAL 7 DAY)
    GROUP BY anr_type, process_name
  `,
});
```

Workflow:
1. Execute SQL → auto-creates histogram + bar
2. Brush histogram → bar filters
3. Click bar → histogram filters to that slice
4. Add charts via sidebar → participate in cross-filtering

## API

### Chart
```typescript
constructor(spec: ChartSpec, source: DataSource, filterStore: FilterStore)
getData(): Row[]
isLoading(): boolean
hasActiveFilters(): boolean
destroy(): void
```

### FilterStore
```typescript
setFilterGroup(group: FilterGroup, sourceChartId: string): void
clearFilterGroup(id: string, sourceChartId: string): void
clearAll(): void
getFilters(): Filter[]
subscribe(cb: (notification) => void): () => void
setUpdateSourceChart(value: boolean): void
```

### DataSource
```typescript
interface DataSource {
  query(filters: Filter[], spec: ChartSpec): Promise<Row[]>;
}
```

Implementations: [`MemorySource`](data/memory_source.ts), [`SqlDataSource`](../../components/d3/sql_data_source.ts), [`HttpDataSource`](../../components/d3/http_data_source.ts)

### BaseRenderer
```typescript
render(svg: SVGElement, data: Row[], spec: ChartSpec): void
setSelectionStrategy(strategy: SelectionStrategy): void
onFilterRequest?: (filters: Filter[]) => void
```

Helpers: `createLinearScale`, `createBandScale`, `drawAxes`, `renderLegend`, `setup2DBrush`, `setupTooltip`

```