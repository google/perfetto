# Slices with Details

**Purpose:** Provides slice data from your trace with rich context. Slices represent time intervals tracking spans of execution like function calls, scheduling periods, or GPU work.

**How to use:**
- Add this node to start with slice data
- Automatically includes process and thread context
- No configuration needed - data is immediately available
- Use downstream nodes to filter or analyze the slices

**Data transformation:**
- No transformation - this is a source node
- Provides pre-enriched slice data with context columns
- More convenient than raw `slice` table as it includes common joins

**Available columns:**
- `id`: Unique slice identifier
- `ts`: Start timestamp (nanoseconds)
- `dur`: Duration (nanoseconds)
- `name`: Slice name/label
- `track_id`: Associated track identifier
- `depth`: Nesting depth in the track
- `parent_id`: Parent slice identifier (for nested slices)
- `category`: Slice category/type
- **Context columns:**
  - `process_name`: Name of the process
  - `upid`: Process identifier (joinable with process table)
  - `thread_name`: Name of the thread
  - `utid`: Thread identifier (joinable with thread table)

**Data source:** Uses the `thread_or_process_slice` table from the `slices.with_context` module, which combines slice data with process and thread information.

**Example use cases:**
- Find all slices for a specific function: Add Filter node with `name = 'my_function'`
- Analyze slice durations by process: Add Aggregation with GROUP BY `process_name`
- Find longest slices: Add Sort by `dur DESC` and Limit to 10

**Comparison with Table Source (slice):**
- **Slices with Details**: Pre-joined with process/thread data, easier to use
- **Table Source (slice)**: Raw slice table, more columns, requires joins for context
