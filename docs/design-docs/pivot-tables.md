# Pivot Tables

_**Project Plan**: [Perfetto: Pivot tables for slices](https://docs.google.com/document/d/1RuEGQKLgOA8YWjZJHD6CTA3ghRRg6o5Phg3_rFCJEDE/)_  
_**How to Use**: [Pivot Table Usage](/docs/visualization/perfetto-ui#pivot-tables)_  
_**For Googlers**: [Perfetto: Pivot Table Use Cases](https://docs.google.com/document/d/1_iR-JjD7m19Q9GQtMk1_5NLSYXFicB_gg4S9D-6Q8lU/)_  

## Objective
Pivot tables give a simplified aggregated view of more complex data. They are
made up of a number of pivots and aggregations that are grouped around these
pivots. You can add more columns/aggregations and drag and drop the columns to
explore the underlying data.

## Motivation
Pivot tables are useful in debugging hangs, stalls, and digging into traces
which usually have too much data to clearly see the problems.
The pivot table allows users to create custom tables to view specific
information about traces in a summarized and less complex way.

## Main Components

![Pivot table design](/docs/images/pivot-tables/pivot-table-design.png)

### Details Panel (Frontend)
The [DetailsPanel](https://cs.android.com/android/_/android/platform/external/perfetto/+/0ae7c36fd528824ee9fdea6cfd4494e9f05183b5:ui/src/frontend/details_panel.ts)
searches for active PivotTables to display on screen. It also syncs the
PivotTableHelper with data from the State. (PivotTableHelper only syncs when the
PivotTableEditor modal is not open).


### Pivot Table (Frontend)
The [PivotTable](https://cs.android.com/android/_/android/platform/external/perfetto/+/0ae7c36fd528824ee9fdea6cfd4494e9f05183b5:ui/src/frontend/pivot_table.ts) builds
the pivot table tab and the table. It also handles user requests (like opening
the pivot table editor, drag and drop columns, expand, etc) by calling the
PivotTableHelper and updating the table.


### PivotTableEditor (Frontend)
The [PivotTableEditor](https://cs.android.com/android/_/android/platform/external/perfetto/+/0ae7c36fd528824ee9fdea6cfd4494e9f05183b5:ui/src/frontend/pivot_table_editor.ts)
consists of ColumnPicker and ColumnDisplay classes.
ColumnPicker allows the user to select column type, name and aggregation. Edits
made through the ColumnPicker are saved temporarily in the PivotTableHelper
without updating the state.
ColumnDisplay displays the selected column from the ColumnPicker, it also allows
users to manipulate the columns after selection (like delete, reorder, change
the default sorting, etc...).
In this stage the user is able to query the selected columns and update the
table or discard the changes made and the PivotTableHelper will resync with the
data in state.


### PivotTableHelper (Frontend)
The [PivotTableHelper](https://cs.android.com/android/_/android/platform/external/perfetto/+/0ae7c36fd528824ee9fdea6cfd4494e9f05183b5:ui/src/frontend/pivot_table_helper.ts)
is created by every PivotTableController for every PivotTableId. It stores a
copy of the selectedPivots and selectedAggregations from state. It also holds
the logic for manipulating the data locally, which are used by the PivotTable
and PivotTableEditor.
It also replaces the data in the State with the changes upon request.
The PivotTableHelper also checks for special “stack” columns, called stackPivots
(`name (stack)` for [slice table](/docs/analysis/sql-tables.autogen#slice) is
currently the only special column), as it sets the column attributes which are
then used to identify them by other components.


### State (Common)
[PivotTableState](https://cs.android.com/android/_/android/platform/external/perfetto/+/0ae7c36fd528824ee9fdea6cfd4494e9f05183b5:ui/src/common/state.ts;l=303) holds the
information that needs to be transferred to and from the frontend and the
controller for each pivot table instance (PivotTableId). It also includes the
global PivotTableConfigs (like the availableColumns and availableAggregations).


### PivotTableController (Controller)
A new [PivotTableController](https://cs.android.com/android/_/android/platform/external/perfetto/+/0ae7c36fd528824ee9fdea6cfd4494e9f05183b5:ui/src/controller/pivot_table_controller.ts)
is created for every PivotTableId.
The PivotTableController handles the setup of the pivot table once added, it
queries for the columns for all tables and sets the PivotTableConfig. It also
creates and initializes a PivotTableHelper for every PivotTableId and publishes
it to the frontend.
Additionally, the PivotTableController handles the collection and the
computation of all data needed by the PivotTableQueryGenerator.
It constantly checks if a request has been set in the PivotTableState and acts
on it if so.
It decides what columns to query, what whereFilters and tables to include and
how to reformat the query result into a PivotTableQueryResponse based on the
request type.

There are four types of requests implemented in the controller:

**_QUERY:_**
Queries the first pivot of the selectedPivots and all the aggregations,
including any global or table-wide whereFilters (Like the start and end
timestamp and selected track_ids that are set by the pivot table generated
through area selection).
It also adds a whereFilter (Filter in the where clause of the query) if the
pivot is a stackPivot to restrict the result to the top level slices only, since
descendants can be generated by expanding the cell and issuing the DESCENDANTS
request, and returns the result as a PivotTableQueryResponse.

![Pivot table query](/docs/images/pivot-tables/pivot-table-query.png)

Returned PivotTableQueryResponse:

```typescript
pivotTableQueryResponse = {
  columns: ['slice type', 'slice category', 'slice name'];
  rows: [
    {
      row: 'internal_slice',
      expandableColumns: ['slice type'],
      expandedRows = [],
    }, {
      row: 'thread_slice',
      expandableColumns: ['slice type'],
      expandedRows = [],
    };
  ]
}
```

**_EXPAND:_**
The [PivotTableBody](https://cs.android.com/android/_/android/platform/external/perfetto/+/a9118d769009349da7f264abb392f4207e66602b:ui/src/frontend/pivot_table.ts;l=235;drc=0bc8ff07f372a58ca4d0399d88567a66ef5b591b) generates the nested structure by
recursively displaying the rows and checking if the row contains any expanded
rows with the isExpanded flag set to true. As it goes through the nested rows,
it passes the row index that it's about to expand, along with the column it's
expanding for till it reaches a [PivotTableRow](https://cs.android.com/android/_/android/platform/external/perfetto/+/0ae7c36fd528824ee9fdea6cfd4494e9f05183b5:ui/src/frontend/pivot_table.ts;l=192).
The PivotTableRow creates a cell for each column. If the cell is at a column
that can be expanded, it is created as an [ExpandableCell](https://cs.android.com/android/_/android/platform/external/perfetto/+/0ae7c36fd528824ee9fdea6cfd4494e9f05183b5:ui/src/frontend/pivot_table.ts;l=121).
When an 'EXPAND' request is issued on an ExpandableCell, it sets the
requestedAction in the PivotTableState and provides it with the SubQueryAttrs.

Given a columnIdx, value, and an array of rowIndicies (SubQueryAttrs) from
the requestedAction in the PivotTableState, it finds the exact row that called
this request from the main PivotTableQueryResponse, and finds the next pivot
to query. It then generates the query similarly to the ‘QUERY’ request, but
includes the whereFilter of the previous column (column name = column value).
The rows of the query result are then nested into the caller row’s expandedRows,
to build a tree view structure while expanding.

![Pivot table expanded cell](/docs/images/pivot-tables/pivot-table-expanded-cell.png)

Passed value:

```typescript
subQueryAttrs = {
  rowIndices: [0, 3],
  columnIdx: 1,
  value: 'blink,benchmark',
  expandedRowColumns: ['slice category'],
}
```

Returned expanded rows:

```typescript
rows = [
  {
    row: 'LocalFrameView::RunAccessibilityLifecyclePhase',
    expandableColumns: [],
    expandedRows: []
  },
  {
    row: 'LocalFrameView::RunCompositingInputsLifecyclePhase',
    expandableColumns: [],
    expandedRows: []
  },
  {
    row: 'LocalFrameView::RunStyleAndLayoutLifecyclePhases',
    expandableColumns: [],
    expandedRows: []
  },
  ...
]
```

The returned rows are saved inside the caller row expandedRows map.

```typescript
rows = {
  row: 'blink,benchmark',
  expandableColumns: ['slice category'],
  expandedRows: [
    'slice name' => {
        isExpanded: true,
        rows
      }
  ]
}
```

**_UNEXPAND:_**
Sets the caller row’s isExpanded flag to false, to hide it from the display but
also keeping its expandedRows saved so as to not have to query them again if
requested.

**_DESCENDANTS:_**
Should only be called for stackPivots, generates a query containing the
stackPivot and the next pivot, if it exists, and all the aggregations. It also
requests the PivotTableQueryGenerator to order by depth first, which is then
used to refactor the resulting rows into the PivotTableQueryResponse tree view
structure.
The returned format is similar to the EXPAND request.

### PivotTableQueryGenerator (Common)
[PivotTableQueryGenerator](https://cs.android.com/android/_/android/platform/external/perfetto/+/0ae7c36fd528824ee9fdea6cfd4494e9f05183b5:ui/src/common/pivot_table_query_generator.ts)
generates an sql query based on the given data, along with any hidden columns
that may need to be added. It also creates an alias for each pivot and
aggregation that is used to identify the resulting cells in the rows.
