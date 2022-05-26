# Perfetto UI

[Perfetto UI](https://ui.perfetto.dev) enables you to view and analyze traces in
the browser. It supports several different tracing formats, including the
perfetto proto trace format and the legacy json trace format.

## UI Tips and Tricks

### Debug Slices

Sometimes you may want to insert some fake slices into the timeline to help
with your understanding of the data. You can do so by inserting rows into a
magic `debug_slices` table.

`debug_slices` table has five columns:

* `id` (integer) [optional] If present, Perfetto UI will use it as slice id to
  open the details panel when you click on the slices.
* `name` (string) [optional] The displayed slice title.
* `ts` (integer) [required] Start of the slice, in nanoseconds.
* `dur` (integer) [required] Duration of the slice, in nanoseconds. Determines
  slice width.
* `depth` (integer) [optional] The row at which the slice is drawn. Depth 0 is
  the first row.

You can open the debug track by going to the "Sample queries" menu on the
left, and clicking "Show Debug Track". A debug slice track will become pinned to
the top and will initially be empty. After you insert slices in the
`debug_slices` table, you can click the reload button on the track to refresh
the information shown in that track.

Here is a simple example with random slices to illustrate the use:

```sql
CREATE VIEW rand_slices AS SELECT * FROM SLICE
  ORDER BY RANDOM() LIMIT 2000;

INSERT INTO debug_slices(id, name, ts, dur, depth)
  SELECT id, name, ts, dur, depth FROM RAND_SLICES;
```

After you click the reload button, you should see the slices in the debug track.

![Example of debug slices](/docs/images/debug-slices-random.png)

Once you're done, you can click the X button to hide the track, and you can
clear the `debug_slices` table (`DELETE FROM debug_slices`) to clear the track.

A more interesting example is seeing RAIL modes in chrome traces:

```sql
SELECT RUN_METRIC('chrome/rail_modes.sql');

-- Depth 0 is the unified RAIL Mode
INSERT INTO debug_slices
  SELECT NULL, rail_mode, ts, dur, 0 FROM combined_overall_rail_slices;

-- Depth 2+ are for each Renderer process with depth 1 left blank
INSERT INTO debug_slices
  SELECT NULL, short_name, ts, dur, depth + 1 FROM rail_mode_slices,
    (SELECT track_id, row_number() OVER () AS depth FROM
      (SELECT DISTINCT track_id FROM rail_mode_slices)) depth_map,
    rail_modes
  WHERE depth_map.track_id = rail_mode_slices.track_id
    AND rail_mode=rail_modes.mode;
```

This produces a visualization like this:

![RAIL modes in Debug Track](/docs/images/rail-mode-debug-slices.png)

Note: There is no equivalent debug counters feature yet, but the feature request
is tracked on [b/168886909](http://b/168886909)).

### Pivot Tables

To use pivot tables in the Perfetto UI, you will need to enable the
"Pivot tables" feature flag in the "Flags" tab under "Support" in the Sidebar.
You can pop up a pivot table over the entire trace when clicking "p" on your
keyboard. The "Edit" button opens a pop up window to add/remove and reorder
columns and change the default sorting of aggregations.

![Pivot table editor](/docs/images/pivot-tables/pivot-table-editor.png)

Clicking on "Query" generates a table with the selected columns.
Table cells with the expand icon can be expanded to show the next column values.
The "name (stack)" column displays top level slices that can be expanded to show
their descendants down to the last child.

![Pivot table](/docs/images/pivot-tables/pivot-table.png)

Area selection pops up a pre-filled pivot table restricted over the selected
timestamps and track ids.

![Pivot table area selection](/docs/images/pivot-tables/pivot-table-area-selection.png)

### Disabling metrics

Some metrics execute at trace load time to annotate the trace with
additional tracks and events. You can stop these metrics from
running by disabling them in the 'Flags' page:

![Disable metrics from running at trace load time](/docs/images/perfetto-ui-disable-metrics.png)


