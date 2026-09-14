# Debug Tracks

Debug tracks display PerfettoSQL query results on the timeline. You can create a
debug track from a result table that can be visualized as slices (e.g. the
[`slice`](sql-tables.autogen#slice) table) or counters (e.g. the
[`counter`](sql-tables.autogen#counter) table).

To visualize a result table, it should include:

1. (For `slice` tracks) a name (the name of the slice) column.
1. A non-null timestamp (the timestamp, in nanoseconds, at the start of the
  slice) column.
1. (For `slice` tracks) a duration (the duration, in nanoseconds, of the slice)
   column.
1. (For `counter` tracks) a value column.
1. (Optionally) the name of a column to pivot

    Note: Pivoting allows you to create a single debug track per distinct value
    in the selected "pivot" column.

## Creating Debug `slice` Tracks

To create `slice` tracks:

1. Run a SQL query, and ensure its results are `slice`-like (as described
  above).
  ![Query for debug slice track](/docs/images/debug-tracks/slice-track-query.png)
1. Navigate to the "Timeline" view, and click on "Add debug track" to set
   up a new debug track. Select "Slice Track" as the Track type.

   Note that the names of the columns in the result table do
   not necessarily have to be `name`, `ts`, or `dur`. Columns which
   _semantically_ match but have a different name can be selected from the
   drop-down selectors.

   ![Create a new debug slice track](/docs/images/debug-tracks/slice-track-create.png)

1. The debug slice track is visible as a pinned track near the top of the
   Timeline view with slices from the table from which the track was created
   (note that slices with no/zero duration will be displayed as instant events).
   Debug tracks may be manually unpinned and will appear on the top of other
   unpinned tracks.
   ![Resultant debug track](/docs/images/debug-tracks/slice-track-result.png)

1. (Optional) Pivoted `slice` tracks are created by selecting a column in the
   "Pivot on" dropdown.

   Note: You can enter queries into the search box directly by typing `:` to
   enter SQL mode.

   ![Creating pivoted debug slice tracks](/docs/images/debug-tracks/pivot-slice-tracks-create.png)

   This creates a debug slice track for each distinct pivot value.

   ![Resultant pivoted debug slice tracks](/docs/images/debug-tracks/pivot-slice-tracks-results.png)

## Creating Debug `counter` Tracks

To create debug `counter` tracks, follow similar steps:

1. Run a SQL query, and ensure its results are `counter`-like (as described
   above).

   ![Query for debug counter track](/docs/images/debug-tracks/counter-tracks-query.png)
1. Navigate to the Timeline view, and click on "Add debug track" to set up a
   new debug track. Select "Counter Track" as the Track type and the
   semantically matching column names of interest.

   ![Create a new debug counter track](/docs/images/debug-tracks/counter-tracks-create.png)

1. The counter track will appear as a pinned track near the top of the Timeline view.

   ![Resultant pivoted debug counter track](/docs/images/debug-tracks/counter-tracks-results.png)

1. (Optional) Pivoted `counter` tracks are created by selecting a column in the
   "Pivot on" dropdown.

   ![Create a new debug counter track](/docs/images/debug-tracks/pivot-counter-tracks-create.png)

   This creates a debug counter track for each distinct pivot value.

   ![Resultant pivoted debug counter track](/docs/images/debug-tracks/pivot-counter-tracks-results.png)
