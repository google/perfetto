# Debug Tracks

Debug Tracks are a way to display tabular results from running a PerfettoSQL
query as a so-called "debug" track. Specifically, if the resultant table can
be visualised in a slice format (ex: the
[`slice`](sql-tables.autogen#slice) table) or counter format
(ex: the [`counter`](sql-tables.autogen#counter) table),
a debug track can be created from it.

For a result table to be visualised, it should
include:

1. A name (the name of the slice) column.
1. A non-null timestamp (the timestamp, in nanoseconds, at the start of the
  slice) column.
1. (For `slice` tracks) a duration (the duration, in nanoseconds, of the slice)
   column.
1. (Optionally) the name of a column to pivot

    Note: Pivoting means allows you to create a single debug track per distinct
    value in the selected "pivot" column.

## Creating Debug `slice` Tracks

To create `slice` tracks:

1. Run a SQL query, and ensure its results are `slice`-like (as described
  above).
  ![Query for debug slice track](/docs/images/debug-tracks/slice-track-query.png)
1. Navigate to the "Show Timeline" view, and click on "Show debug track" to set
   up a new debug track. Select "slice" from the Track type dropdown.

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

1. (Optional) Pivoted `slice` tracks are created by selecting a value from the
   "pivot" column.

   Note: You can enter queries into the search box directly by typing `:` to
   enter SQL mode.

   ![Creating pivoted debug slice tracks](/docs/images/debug-tracks/pivot-slice-tracks-create.png)

   This will result in a debug slice track created for each distinct pivot
   value.

   ![Resultant pivoted debug slice tracks](/docs/images/debug-tracks/pivot-slice-tracks-results.png)

## Creating Debug `counter` Tracks

You can create new debug `counter` tracks by following similar steps to the ones
mentioned above:

1. Run a SQL query, and ensure its results are `counter`-like (as described
   above).

   ![Query for debug counter track](/docs/images/debug-tracks/counter-tracks-query.png)
1. Navigate to the Timeline view, and click on "Show debug track" to set up a
   new debug track. Select "counter" from the Track type dropdown and the
   semantically matching column names of interest.

   ![Create a new debug counter track](/docs/images/debug-tracks/counter-tracks-create.png)

1. The counter track will appear as a pinned track near the top of the Timeline view.

   ![Resultant pivoted debug counter track](/docs/images/debug-tracks/counter-tracks-results.png)

1. (Optional) Pivoted `counter` tracks are created by selecting a value from the
   "pivot" column.

   ![Create a new debug counter track](/docs/images/debug-tracks/pivot-counter-tracks-create.png)

   This will result in a debug counter track created for each distinct pivot
   value.

   ![Resultant pivoted debug counter track](/docs/images/debug-tracks/pivot-counter-tracks-results.png)
