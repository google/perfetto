# Create Slices

**Purpose:** Create time slices by pairing start and end timestamps from two separate data sources. Constructs intervals from independent start/end event streams.

**How to use:**
- Connect two data sources:
  - **Starts input:** Data source containing start timestamps
  - **Ends input:** Data source containing end timestamps
- Configure each input:
  - **Mode:** Choose between "Timestamp" (single column) or "Timestamp End" (for events that already have durations)
  - **Timestamp Column:** Select the column containing the timestamp
  - **Duration Column:** (Optional, only for "Timestamp End" mode) Select the column containing the duration
- The node pairs each start event with its corresponding end event to create slices

**Data transformation:**
- Combines rows from both inputs based on temporal ordering
- Creates one output slice for each start-end pair
- Output columns:
  - `ts`: Start timestamp (from starts input)
  - `dur`: Duration (calculated as end timestamp - start timestamp)
- Only successfully paired events appear in the output

**Example 1 - Lock acquisition/release:** Track how long locks are held:
- Starts input: Lock acquisition events with `acquire_ts` column
- Ends input: Lock release events with `release_ts` column
- Both in "Timestamp" mode
- Result: Slices representing lock hold duration

**Example 2 - Resource allocation:** Track resource lifetime:
- Starts input: Allocation events with `alloc_time` column
- Ends input: Deallocation events with `free_time` column
- Result: Slices showing how long each resource was allocated

**Example 3 - State transitions:** Track duration of specific states:
- Starts input: Events where state becomes ACTIVE (filter: `state = 'ACTIVE'`)
- Ends input: Events where state becomes INACTIVE (filter: `state = 'INACTIVE'`)
- Result: Slices representing active periods

**Modes:**
- **Timestamp:** Use a single timestamp column from each input
  - Start time = timestamp from starts input
  - End time = timestamp from ends input

- **Timestamp End:** Use both timestamp and duration columns to compute the end timestamp
  - Useful when events already contain duration information
  - End time is computed as: timestamp + duration

**Pairing behavior:**
- The node uses a first-in-first-out (FIFO) pairing strategy
- Each start event is paired with the next available end event that occurs after it
- Unpaired start or end events are discarded
- Negative durations (end before start) are possible if data is not well-ordered

**SQL equivalent:** Uses the experimental `CREATE_SLICES()` table function to pair timestamps from two queries.

**Use cases:**
- Tracking lock acquisition and release times
- Measuring resource allocation lifetimes
- Computing state transition durations
- Analyzing paired event patterns (begin/end, open/close, enter/exit)
