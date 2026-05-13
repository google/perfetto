# Counter to Intervals

**Purpose:** Convert counter-style data (individual timestamped values) into interval-style data (with duration). Each counter sample becomes an interval lasting until the next sample on the same track.

**How to use:**
- Connect an input that contains counter data with the required columns: `id`, `ts`, `track_id`, and `value`
- The input must **not** already have a `dur` column (it should be raw counter data, not interval data)
- The node automatically converts each counter sample into an interval

**Data transformation:**
- Each counter sample becomes an interval spanning from its timestamp to the next sample's timestamp on the same track
- All original columns are preserved
- Three new columns are added:
  - `dur`: Duration until the next counter value on the same track
  - `next_value`: The value of the next counter sample
  - `delta_value`: The change in value (`next_value - value`)
- The last sample on each track gets a duration extending to the end of the trace

**Example 1 - CPU frequency analysis:** Convert raw CPU frequency counter samples into intervals:
- Input: Table Source with `counter` table, filtered to CPU frequency tracks
- Result: Each frequency reading becomes an interval showing how long that frequency was held

**Example 2 - Memory usage over time:** Convert memory counter snapshots into intervals:
- Input: Counter data with periodic memory usage samples
- Result: Intervals showing memory usage between each measurement, with `delta_value` showing changes

**Required input columns:**
- `id`: Unique identifier for each counter sample
- `ts`: Timestamp of the counter sample
- `track_id`: Identifies which counter track the sample belongs to
- `value`: The counter value at this timestamp

**SQL equivalent:** Uses the `counter_leading_intervals!()` PerfettoSQL macro to convert counter data into intervals.
