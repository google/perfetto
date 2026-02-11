# Filter In

**Purpose:** Filter rows from the primary input to only those where a specified column's values exist in a column from the "Input" source. This is a semi-join operation useful for restricting data to a specific set of values.

**How to use:**
- Connect your main data source to the primary input (top port)
- Connect a source containing the allowed values to the "Input" port (left side)
- Select the "Base column" from your primary input to filter on
- Select the "Match column" from the Input source to match against
- If there is only one column in common between both inputs, it will be auto-selected

**Data transformation:**
- Rows are kept only if the base column value exists in the set of match column values
- All columns from the primary input are preserved unchanged
- No columns from the Input source are added to the output
- This is equivalent to `WHERE base_column IN (SELECT match_column FROM input)`

**Example:** Filter slices to only those belonging to specific threads by matching `utid` against a list of thread IDs. Or filter counter data to specific track IDs from another query.

**Tip:** The base column and match column do not need to have the same name, but they should contain comparable values.
