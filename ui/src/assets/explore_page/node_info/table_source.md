# Table Source

**Purpose:** Provides direct access to Perfetto tables.

**Data transformation:**
- No transformation - this is a source node
- Provides raw access to the selected table's data
- All rows and columns from the table are available
- Use downstream nodes (Filter, Modify Columns, etc.) to transform the data

**Example:** Start with the `slice` table to access all trace slices, then add Filter and Aggregation nodes to analyze specific patterns.

**Tip:** Many tables have JOINID columns (like `upid`, `utid`, `track_id`) that can be used with the Add Columns node to enrich your data with related information.
