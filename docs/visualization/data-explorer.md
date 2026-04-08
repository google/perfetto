# Data Explorer

The Data Explorer is a visual query builder in the Perfetto UI that lets you
construct complex SQL queries by connecting nodes in a graph. Instead of writing
raw SQL, you build analysis pipelines by linking data sources and operations
together, and the system generates and executes the SQL for you.

## Opening the Data Explorer

After loading a trace, click **Data Explorer** in the left sidebar (under the
"Current Trace" section). This opens the explorer at the `#!/explore` route.

The sidebar entry appears only when a trace is loaded, since the Data Explorer
queries trace data.

## Core Concepts

### Graphs and Nodes

A Data Explorer graph is a directed acyclic graph (DAG) where each node
represents either a **data source** or an **operation**. Data flows from source
nodes at the top, through operation nodes, to results at the bottom.

- **Source nodes** provide initial data (a SQL table, all slices, custom SQL, or
  a time range selection).
- **Operation nodes** transform data (filter, aggregate, join, sort, etc.).
- **Export nodes** push results to dashboards, metrics, or trace summaries.

Nodes are connected by dragging from an output port on one node to an input port
on another. Each connection represents data flowing from the parent node to the
child node.

### Tabs

The Data Explorer supports multiple tabs, each containing an independent graph.
You can create, rename, reorder, and close tabs using the tab bar at the top.
Each tab also has sub-tabs for the graph view and any associated dashboards.

### Dashboards

Dashboards are canvas-based visualization pages associated with a graph. You can
create charts from data exported by graph nodes, position and resize them freely,
and add text labels for annotations. Clicking on chart elements creates brush
filters that filter the underlying data.

## Getting Started

1. Open a trace in the Perfetto UI.
2. Click **Data Explorer** in the left sidebar.
3. The explorer opens with a default graph containing commonly used tables.
4. Click a table node to select it and see its data in the results panel.
5. Add operation nodes (filter, aggregation, etc.) to transform the data.

You can also load one of the built-in examples from the **Examples** button to
see a working analysis pipeline.

## Node Types

### Source Nodes

These nodes provide the starting data for your analysis. They have no input
ports.

| Node | Hotkey | Description |
|------|--------|-------------|
| **Table** | `T` | Query any table available in the trace. Opens a table picker on creation. |
| **Slices** | `L` | Pre-configured source for all trace slices. |
| **Query** | `Q` | Write custom SQL as a data source. Requires manual execution. |
| **Time Range** | — | Generates time intervals from the current timeline selection. |

### Single-Input Operation Nodes

These nodes take one input and produce a transformed output. They are added by
selecting a parent node and choosing the operation from the menu.

| Node | Description |
|------|-------------|
| **Filter** | Adds WHERE conditions to filter rows. |
| **Aggregation** | Groups rows (GROUP BY) and applies aggregate functions (SUM, AVG, COUNT, etc.). |
| **Modify Columns** | Renames, removes, or reorders columns. |
| **Add Columns** | Adds columns from a secondary source via LEFT JOIN, or adds computed expressions. |
| **Sort** | Orders results by one or more columns. |
| **Limit and Offset** | Limits the number of rows returned, with optional offset for pagination. |
| **Counter to Intervals** | Converts counter events into time intervals. |
| **Visualisation** | Renders results as a chart (bar chart, histogram, etc.). |

### Multi-Input Operation Nodes

These nodes accept two or more inputs, connected via primary and secondary input
ports.

| Node | Description |
|------|-------------|
| **Join** | Combines columns from two sources via INNER, LEFT, or CROSS JOIN. |
| **Union** | Combines rows from multiple sources (UNION ALL). Requires manual execution. |
| **Interval Intersect** | Finds overlapping time intervals between two sources. Requires manual execution. |
| **Filter During** | Filters rows from the primary input using time intervals from the secondary input. Requires manual execution. |
| **Filter In** | Filters rows where a column value exists in a secondary source (IN subquery). |
| **Create Slices** | Pairs start/end events from two sources into slices. Requires manual execution. |

### Export Nodes

These nodes export results to other features in the Perfetto UI.

| Node | Description |
|------|-------------|
| **Export to Dashboard** | Makes the node's data available as a dashboard data source. |
| **Metrics** | Defines a trace metric from the node's output. |
| **Trace Summary** | Bundles multiple metrics into a trace summary specification. |

## Building a Graph

### Creating Nodes

- **Source nodes**: Use keyboard shortcuts (`T` for Table, `L` for Slices, `Q`
  for Query) or click the buttons in the sidebar.
- **Operation nodes**: Select a node, then choose an operation from the node's
  action menu (the `+` button or right-click context menu).

### Connecting Nodes

Drag from a node's output port (bottom) to another node's input port (top). For
multi-input operations like Join, connect secondary inputs to the numbered side
ports.

### Inserting Nodes

When you add a single-input operation node to a node that already has children,
the new node is inserted between the parent and its children, preserving the
existing data flow.

### Deleting Nodes

Select a node and press `Delete` or `Backspace`. When a node in the middle of a
chain is deleted, the system automatically reconnects the parent to the children,
preserving the data flow.

## Query Execution

### Auto-Execute vs Manual Execute

Most nodes auto-execute: the query runs automatically whenever you change the
node's configuration. Some nodes (Query, Union, Interval Intersect, Filter
During, Create Slices) require manual execution — click the **Run Query** button
or press `Ctrl+Enter` (`Cmd+Enter` on macOS).

### Two-Phase Execution

1. **Analysis**: The system validates your query structure and generates SQL
   without executing it. You can preview the generated SQL in the SQL tab.
2. **Execution**: The query runs against the trace processor, materializing
   results into a temporary table for efficient pagination and further
   operations.

### Viewing Results

Select a node to see its results in the bottom panel. The results panel supports:

- Paginated data grid with sortable columns.
- **SQL** tab showing the generated SQL query.
- **Proto** tab showing the structured query protobuf.

## Saving and Sharing

### Automatic Persistence

Your graphs are automatically saved to the browser's localStorage as you work.
When you reopen Perfetto with the same trace, your graphs are restored.

### Import and Export

- **Export** (`E`): Downloads the current graph as a JSON file.
- **Import** (`I`): Loads a graph from a JSON file.

### Recent Graphs

The sidebar shows your recent graphs (up to 10). You can star graphs to keep
them, rename them, or delete them. Click a recent graph card to load it.

### Permalinks

When you share a trace via permalink, the current Data Explorer state (all tabs
and dashboards) is included in the shared link.

## Keyboard Shortcuts

### Node Creation

| Key | Action |
|-----|--------|
| `L` | Create Slices source node |
| `T` | Create Table source node |
| `Q` | Create Query source node |

### Graph Editing

| Key | Action |
|-----|--------|
| `Delete` / `Backspace` | Delete selected nodes |
| `Ctrl+C` / `Cmd+C` | Copy selected nodes |
| `Ctrl+V` / `Cmd+V` | Paste nodes |
| `Ctrl+Z` / `Cmd+Z` | Undo |
| `Ctrl+Shift+Z` / `Cmd+Shift+Z` | Redo |
| `Ctrl+Y` / `Cmd+Y` | Redo (alternative) |

### Execution

| Key | Action |
|-----|--------|
| `Ctrl+Enter` / `Cmd+Enter` | Execute selected node |

### Import / Export

| Key | Action |
|-----|--------|
| `I` | Import graph from JSON file |
| `E` | Export graph to JSON file |

### Navigation

| Key | Action |
|-----|--------|
| `W` / `A` / `S` / `D` or Scroll | Pan the graph canvas |
| `Ctrl+Scroll` / `Cmd+Scroll` | Zoom in/out |

### Selection

| Action | Effect |
|--------|--------|
| Click | Select a single node |
| `Ctrl+Click` / `Cmd+Click` | Toggle node in selection |
| `Shift+Click` | Add node to selection |
| `Shift+Drag` | Rectangle select |

### Other

| Key | Action |
|-----|--------|
| `?` | Show help modal |

## Examples

The Data Explorer includes built-in example graphs accessible from the
**Examples** button:

- **Learning**: An interactive tutorial that walks through node docking,
  filtering, adding operation nodes, and working with multi-child workflows.
- **Slice Analysis Pipeline**: A practical example that demonstrates finding the
  total duration of specific process slices, showcasing a complete analysis
  workflow from data source to aggregated results.

## Dashboards

### Creating a Dashboard

1. Add an **Export to Dashboard** node to your graph and connect it to the node
   whose data you want to visualize.
2. Execute the graph to materialize the data.
3. Click the dashboard sub-tab (e.g., **Dashboard 1**) to switch to the
   dashboard view.
4. Click **Add Chart** to create a chart from the exported data.

### Configuring Charts

Each chart card can be configured with:

- **Chart type**: Bar chart, histogram, line chart, etc.
- **Columns**: Choose which columns to use for axes and series.
- **Dimensions**: Group data by specific columns.

### Interacting with Dashboards

- **Drag** chart cards to reposition them on the canvas.
- **Resize** chart cards by dragging the resize handles.
- **Click** on chart elements to create brush filters that filter the underlying
  data.
- **Add labels** to annotate your dashboard with text.
- Create multiple dashboards per graph using the `+` button on the sub-tab bar.
