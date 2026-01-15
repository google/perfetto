# Table Source

**Purpose:** Provides direct access to trace data tables like slices, processes, threads, counters, and more. This is the starting point for most queries.

**How to use:**
- Click the node to open the table selection modal
- Browse or search for a table by name
- View table descriptions and available columns
- Select a table to use it as your data source
- Ctrl+click to select multiple tables at once (creates multiple nodes)

**Data transformation:**
- No transformation - this is a source node
- Provides raw access to the selected table's data
- All rows and columns from the table are available
- Use downstream nodes (Filter, Modify Columns, etc.) to transform the data

**Common tables:**
- `slice`: All slices in the trace (use `Slices with details` source for richer data)
- `thread_slice`: Slices associated with threads
- `process`: Process information
- `thread`: Thread information
- `counter`: Counter tracks (numeric values over time)
- `thread_track`, `process_track`: Track metadata
- `sched_slice`: CPU scheduling information
- `args`: Key-value arguments associated with events

**Example:** Start with the `slice` table to access all trace slices, then add Filter and Aggregation nodes to analyze specific patterns.

**Tip:** Many tables have JOINID columns (like `upid`, `utid`, `track_id`) that can be used with the Add Columns node to enrich your data with related information.
