# Visualisation Node

The Visualisation node allows you to visualize data from upstream nodes as interactive charts. Click on chart elements to create filters that flow to downstream nodes.

## Chart Types

The visualization system uses a registry-based architecture for extensibility. Currently supported chart types are:

### Bar Chart
Displays categorical data with bars. Each bar represents a unique value and its aggregated measure.

- Best for: Categorical columns, discrete values
- Supports: Aggregation functions (COUNT, SUM, AVG, MIN, MAX)
- Click behavior: Adds an equality filter (`column = value`)

### Histogram
Displays the distribution of numeric data using binned ranges.

- Best for: Numeric columns (integers, doubles, timestamps, durations)
- Supports: Binning (automatic or custom bin count)
- Click behavior: Adds a range filter (`column >= binStart AND column < binEnd`)

## Adding New Chart Types

The chart system uses a registry pattern for extensibility. To add a new chart type:

1. **Update the type union**: Add your type to `ChartType` in `visualisation_node.ts`
2. **Register the type**: Add an entry to `CHART_TYPES` in `chart_type_registry.ts` with:
   - `type`: The string identifier
   - `label`: Human-readable name for the UI
   - `icon`: Material icon name
   - `supportsAggregation`: Whether the chart can aggregate values
   - `supportsBinning`: Whether the chart bins continuous values
3. **Implement rendering**: Add rendering logic in `chart_view.ts`
4. **Implement data loading**: Add data fetching in `chart_data_loader.ts`

## Configuration

1. **Chart Type**: Select the visualization type (Bar Chart, Histogram)
2. **Column**: Choose which column to visualize
3. **Aggregation** (Bar Chart): Select the aggregation function
4. **Measure Column** (Bar Chart, non-COUNT): Select the column to aggregate
5. **Orientation** (Bar Chart): Choose vertical or horizontal layout
6. **Bin Count** (Histogram): Override the automatic bin calculation

## Filtering

- **Single click**: Replaces existing chart filters with the clicked element
- **Brush selection**: Drag to select multiple elements
- **Clear Filters**: Use the toolbar button to remove all chart filters

## Current Limitations

### Single Measure Per Chart

Currently, each chart supports only a single measure:
- Bar charts can show one aggregation at a time (e.g., COUNT OR SUM(duration), not both)
- Histograms visualize a single numeric column's distribution

Future work may extend this to support multiple measures (e.g., overlaying multiple metrics on the same chart). See the `ChartConfig` interface documentation in `visualisation_node.ts` for implementation guidance.

## Tips

- For large datasets, the bar chart shows the top 100 categories by count
- Histogram bin count is automatically calculated using the Terrell-Scott rule if not specified
- Chart filters are additive - each click adds filters that downstream nodes see
- Toggle individual filters on/off in the node configuration panel
- Drag chart headers to reorder charts
- Use the resize handle to customize chart widths
