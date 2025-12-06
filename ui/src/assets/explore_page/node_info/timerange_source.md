# Time Range Source

**Purpose:** Make a time selection from the timeline and use it as a source node in the graph. Useful for filtering data to specific time windows.

**How to use:**
- **Dynamic mode:** Toggle on to automatically sync with your timeline selection
  - The node updates whenever you change the selection in the timeline
  - Perfect for exploratory analysis

- **Static mode:** Use "Update from Timeline" button to capture current selection
  - Takes a snapshot of the timeline selection
  - Won't change when you modify the timeline selection

- **Manual mode:** Click edit icons to manually enter start time, end time, or duration
  - Useful for precise time ranges

**Data transformation:**
- No transformation - this is a source node
- Produces a single row with three columns:
  - `id`: Always 0 (row identifier)
  - `ts`: Start timestamp (nanoseconds)
  - `dur`: Duration (nanoseconds)

**Common usage patterns:**
1. **With Filter During node:** Connect to the secondary input of Filter During to filter events to the selected time range
2. **With Interval Intersect node:** Find which events overlap with the selected time
3. **With Add Columns node:** Use as a reference time range

**Example workflow:**
1. Select a time range in the timeline (e.g., an interesting spike)
2. Add Time Range Source in dynamic mode
3. Connect to Filter During node to filter your main data to that time window
4. Analyze what happened during that time period

**Dynamic vs Static:**
- **Dynamic**: Live updates, great for exploration ("what's in this selection?")
- **Static**: Snapshot, great for reproducible analysis or comparing multiple time windows

**Note:** If no selection exists in the timeline, the node uses the full trace time range.
