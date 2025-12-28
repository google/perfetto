# Filter

**Purpose:** Keep only rows that match specified conditions. Use SQL expressions to filter data based on column values.

**How to use:**
- Add filter conditions using SQL expressions (e.g., `dur > 1000`, `name = 'my_function'`)
- Combine multiple filters with AND/OR logic
- Use comparison operators: `=`, `!=`, `<`, `>`, `<=`, `>=`, `LIKE`, `GLOB`
- Reference any column from the input data

**Data transformation:**
- Rows that match the filter conditions pass through unchanged
- Rows that don't match are excluded from the output
- All columns are preserved for rows that pass the filter
- No modification to the data values or column structure

**Example:** Filter to show only slices with duration greater than 1ms: `dur > 1000000`. Or filter by name: `name LIKE '%render%'`.

**Filter operators:**
- `=`, `!=`: Equality/inequality
- `<`, `>`, `<=`, `>=`: Numeric comparisons
- `LIKE`: Pattern matching with `%` wildcard
- `GLOB`: Unix-style pattern matching with `*` and `?`
- `IN`: Check if value is in a list
- `AND`, `OR`, `NOT`: Combine conditions
