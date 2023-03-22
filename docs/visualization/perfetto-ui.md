# Perfetto UI

[Perfetto UI](https://ui.perfetto.dev) enables you to view and analyze traces in
the browser. It supports several different tracing formats, including the
perfetto proto trace format and the legacy json trace format.

## UI Tips and Tricks

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


