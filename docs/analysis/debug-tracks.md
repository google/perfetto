# Debug Tracks

Debug Tracks are a way to display tabular results from running a PerfettoSQL
query as a new so-called "debug" track. Specifically, if the resultant table can
be visualised in a slice format (for example, the
[`slice`](sql-tables.autogen#slice) table), a debug track can be created from
it.

For a result table to be able to be visualised in a slice format, it should
include:

1.  A name (the name of the slice) column.
1.  A non-null timestamp (the timestamp, in nanoseconds, at the start of the
    slice) column.
1.  (Optionally) a duration (the duration, in nanoseconds, of the slice) column.

To create a new debug track:

1.  Run a SQL query, and ensure its results are `slice`-like (as described
    above).
    ![Query for debug track](/docs/images/debug-tracks/debug-tracks-query.png)
1.  Navigate to the Timeline view, and click on "Show debug track" to set up a
    new debug track. Note that the names of the columns in the result table do
    not necessarily have to be `name`, `ts`, or `dur`. Columns which
    *semantically* match but have a different name can be selected from the
    drop-down selectors.
    ![Create a new debug track](/docs/images/debug-tracks/debug-tracks-create.png)
1.  The debug track is visible as a pinned track near the top of the Timeline
    view with slices from the table from which the track was created (note that
    slices with no/zero duration will be displayed as instant events). The debug
    track may be manually unpinned and then it should appear on the top of other
    unpinned tracks.
    ![Resultant debug track](/docs/images/debug-tracks/debug-tracks-result.png)
