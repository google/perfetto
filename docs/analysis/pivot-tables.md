# Pivot Tables

Pivot Tables are a way to summarize and aggregate information about selected
slices in a configurable way. It is available in canary and autopush channels by
default, and can be enabled in stable by toggling a corresponding flag
(Support > Flags > Pivot tables V2).

To create a pivot table, you need to select a timeline area that contains
slices. The table will then show an aggregation of the selected slices.

## Conceptual model

Pivot tables have two types of columns: pivots and aggregations. Conceptually,
the UI requests all the data from columns of both types and then computes
aggregate values for aggregation columns for every distinct tuple of pivot
column values.

Pivots are hierarchical, and the aggregate values are also computed for all the
prefixes of pivot column values. For example, if you select process name,
category, and event name (in that order) as pivots and have duration sum as the
only aggregation columns, the following aggregate values will be computed:

*   Total duration for each process
*   Total duration for each process and category
*   Total duration for each process, category, and event name

The table rows are appropriately nested in the UI, from more general to more
specific. Portions of the table can be collapsed and expanded.

## Working with pivot table

Pivot tables can be configured using dropdown menus in the table header cells.
These can be used to:

*   Add and remove pivots
*   Add and remove aggregations
*   Change aggregation functions
*   Sort by aggregation columns
