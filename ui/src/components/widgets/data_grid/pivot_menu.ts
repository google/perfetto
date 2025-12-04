// Copyright (C) 2025 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import m from 'mithril';
import {MenuDivider, MenuItem} from '../../../widgets/menu';
import {
  AggregationFunction,
  ColumnDefinition,
  PivotModel,
  PivotValue,
} from './common';
import {maybeUndefined} from '../../../base/utils';

type OnPivotChanged = (pivot: PivotModel | undefined) => void;

// Numeric-only aggregation functions that don't make sense for strings
const NUMERIC_ONLY_FUNCTIONS: ReadonlySet<AggregationFunction> = new Set([
  'SUM',
  'AVG',
  'MIN',
  'MAX',
]);

// Helper to insert a new value entry after a specific column in the values object
function insertValueAfterColumn(
  values: Record<string, PivotValue>,
  afterColumn: string,
  isAfterColumnAggregate: boolean,
  newAlias: string,
  newValue: PivotValue,
): Record<string, PivotValue> {
  const entries = Object.entries(values);

  // Find insertion index - after current column if it's an aggregate, otherwise at end
  let insertIndex = entries.length;
  if (isAfterColumnAggregate) {
    const currentIndex = entries.findIndex(([key]) => key === afterColumn);
    if (currentIndex !== -1) {
      insertIndex = currentIndex + 1;
    }
  }

  const newEntries = [
    ...entries.slice(0, insertIndex),
    [newAlias, newValue] as [string, PivotValue],
    ...entries.slice(insertIndex),
  ];
  return Object.fromEntries(newEntries);
}

// Helper to replace a value entry with a new one, preserving its position
function replaceValuePreservingPosition(
  values: Record<string, PivotValue>,
  oldAlias: string,
  newAlias: string,
  newValue: PivotValue,
): Record<string, PivotValue> {
  const entries = Object.entries(values);
  const newEntries = entries.map(([key, val]) => {
    if (key === oldAlias) {
      return [newAlias, newValue] as [string, PivotValue];
    }
    return [key, val] as [string, PivotValue];
  });
  return Object.fromEntries(newEntries);
}

// Check if an aggregation function is valid for a column based on its type
function isAggregationValidForColumn(
  func: AggregationFunction,
  column: ColumnDefinition | undefined,
): boolean {
  // COUNT and ANY are always valid
  if (!NUMERIC_ONLY_FUNCTIONS.has(func)) return true;
  // If column is explicitly marked as string type, don't allow numeric functions
  if (column?.filterType === 'string') return false;
  // Otherwise allow (either numeric or unknown type)
  return true;
}

export function renderAggregateActions(
  currentColumn: ColumnDefinition,
  pivot: PivotModel,
  onPivotChanged: OnPivotChanged,
  currentGroupBy: ReadonlyArray<string>,
  columns: ReadonlyArray<ColumnDefinition>,
): m.Children {
  const pivotValue = maybeUndefined(pivot.values[currentColumn.name]);

  // COUNT columns don't have an underlying source column, so they can't be
  // changed to other functions (which require a column to aggregate)
  if (pivotValue?.func === 'COUNT') {
    return m(MenuItem, {
      label: 'Remove',
      icon: 'delete',
      onclick: () => {
        const newValues = {...pivot.values};
        delete newValues[currentColumn.name];
        if (
          currentGroupBy.length === 0 &&
          Object.keys(newValues).length === 0
        ) {
          onPivotChanged(undefined);
        } else {
          const newPivot: PivotModel = {
            groupBy: [...currentGroupBy],
            values: newValues,
          };
          onPivotChanged(newPivot);
        }
      },
    });
  }

  const baseColumnName =
    pivotValue && 'col' in pivotValue ? pivotValue.col : currentColumn.name;
  const sourceColumn = columns.find((c) => c.name === baseColumnName);

  // Available aggregation functions
  const aggregationFunctions: ReadonlyArray<{
    func: AggregationFunction;
    label: string;
  }> = [
    {func: 'ANY', label: 'ANY'},
    {func: 'COUNT', label: 'COUNT'},
    {func: 'SUM', label: 'SUM'},
    {func: 'AVG', label: 'AVG'},
    {func: 'MIN', label: 'MIN'},
    {func: 'MAX', label: 'MAX'},
  ];

  // Filter to functions that can be switched to (excludes current function, COUNT, and invalid for column type)
  const availableFunctions = aggregationFunctions
    .filter((f) => f.func !== pivotValue?.func)
    .filter((f) => f.func !== 'COUNT') // COUNT loses the original column info
    .filter((f) => isAggregationValidForColumn(f.func, sourceColumn));

  return [
    m(MenuItem, {
      label: 'Remove',
      icon: 'delete',
      onclick: () => {
        const newValues = {...pivot.values};
        delete newValues[currentColumn.name];
        if (
          currentGroupBy.length === 0 &&
          Object.keys(newValues).length === 0
        ) {
          // If no groupBy and no values, clear the pivot entirely
          onPivotChanged(undefined);
        } else {
          const newPivot: PivotModel = {
            groupBy: [...currentGroupBy],
            values: newValues,
          };
          onPivotChanged(newPivot);
        }
      },
    }),
    // Only show "Change function" if there are other valid functions to switch to
    availableFunctions.length > 0 &&
      m(
        MenuItem,
        {
          label: 'Change function',
          icon: 'swap_horiz',
        },
        availableFunctions.map((agg) => {
          return m(MenuItem, {
            label: agg.label,
            onclick: () => {
              let newAlias: string;
              let newValue: PivotValue;

              if (agg.func === 'COUNT') {
                // Changing to COUNT - use 'count' as alias
                newAlias = 'count';
                newValue = {
                  func: 'COUNT',
                } as PivotValue;
              } else if (pivotValue?.func === 'COUNT') {
                // Changing from COUNT to another function
                // Use the current column context
                newAlias = `${baseColumnName}_${agg.func.toLowerCase()}`;
                newValue = {
                  col: baseColumnName,
                  func: agg.func,
                } as PivotValue;
              } else {
                // Normal case - changing between column-based aggregates
                // Keep the same column, change the function
                const sourceCol = pivotValue?.col ?? baseColumnName;
                newAlias = `${sourceCol}_${agg.func.toLowerCase()}`;
                newValue = {
                  col: sourceCol,
                  func: agg.func,
                } as PivotValue;
              }

              // Replace old aggregate with new one, preserving position
              const newValues = replaceValuePreservingPosition(
                pivot.values,
                currentColumn.name,
                newAlias,
                newValue,
              );

              const newPivot: PivotModel = {
                groupBy: [...currentGroupBy],
                values: newValues,
              };
              onPivotChanged(newPivot);
            },
          });
        }),
      ),
  ];
}

export function renderPivotMenu(
  columns: ReadonlyArray<ColumnDefinition>,
  pivot: PivotModel | undefined,
  onPivotChanged: OnPivotChanged,
  currentColumn: ColumnDefinition,
): m.Children {
  const currentGroupBy = pivot?.groupBy ?? [];
  const isCurrentColumnGrouped = currentGroupBy.includes(currentColumn.name);
  const isCurrentColumnAggregate =
    pivot?.values?.[currentColumn.name] !== undefined;
  const availableColumns = columns.filter(
    (col) => !currentGroupBy.includes(col.name),
  );

  // Get the base column name - if this is a pivoted value column, use its source
  const pivotValue = pivot?.values?.[currentColumn.name];
  const baseColumnName =
    pivotValue && 'col' in pivotValue ? pivotValue.col : currentColumn.name;

  // Available aggregation functions for the current column
  const aggregationFunctions: ReadonlyArray<{
    func: AggregationFunction;
    label: string;
  }> = [
    {func: 'ANY', label: 'ANY'},
    {func: 'COUNT', label: 'COUNT'},
    {func: 'SUM', label: 'SUM'},
    {func: 'AVG', label: 'AVG'},
    {func: 'MIN', label: 'MIN'},
    {func: 'MAX', label: 'MAX'},
  ];

  return [
    // If current column is NOT grouped, show "Pivot on this column" option
    // For aggregate columns, use the source column name
    // COUNT columns don't have an underlying source column, so can't pivot on them
    !isCurrentColumnGrouped &&
      pivotValue?.func !== 'COUNT' &&
      m(MenuItem, {
        label: 'Pivot on this column',
        icon: 'pivot_table_chart',
        onclick: () => {
          // For aggregate columns, use the source column; otherwise use the column name
          const columnToPivot = isCurrentColumnAggregate
            ? baseColumnName
            : currentColumn.name;

          // Don't add if already in groupBy
          if (currentGroupBy.includes(columnToPivot)) return;

          const newGroupBy = [...currentGroupBy, columnToPivot];
          let newValues = {...(pivot?.values ?? {})};

          // If this is the first time entering pivot mode, add all other columns
          // as 'ANY' aggregates to preserve their visibility
          if (!pivot) {
            const otherColumns = columns.filter(
              (col) => col.name !== columnToPivot,
            );
            newValues = {};
            for (const col of otherColumns) {
              newValues[col.name] = {
                col: col.name,
                func: 'ANY',
              };
            }
          } else if (isCurrentColumnAggregate) {
            // Remove the current aggregate since we're pivoting on its source column
            delete newValues[currentColumn.name];
          }

          const newPivot: PivotModel = {
            groupBy: newGroupBy,
            values: newValues,
          };
          onPivotChanged(newPivot);
        },
      }),
    // Aggregate this column - for non-pivot, non-aggregate columns
    pivot &&
      !isCurrentColumnGrouped &&
      !isCurrentColumnAggregate &&
      m(
        MenuItem,
        {
          label: 'Aggregate this column...',
          icon: 'functions',
        },
        aggregationFunctions
          .filter((agg) => agg.func !== 'COUNT')
          .filter((agg) => isAggregationValidForColumn(agg.func, currentColumn))
          .map((agg) => {
            return m(MenuItem, {
              label: agg.label,
              onclick: () => {
                const alias = `${baseColumnName}_${agg.func.toLowerCase()}`;
                const newValue: PivotValue = {
                  col: baseColumnName,
                  func: agg.func,
                };

                const newValues = {
                  ...pivot?.values,
                  [alias]: newValue,
                };
                const newPivot: PivotModel = {
                  groupBy: currentGroupBy,
                  values: newValues,
                };
                onPivotChanged(newPivot);
              },
            });
          }),
      ),
    // Aggregate this column - for pivot columns (moves from pivot to aggregate)
    pivot &&
      isCurrentColumnGrouped &&
      m(
        MenuItem,
        {
          label: 'Aggregate this column...',
          icon: 'functions',
        },
        aggregationFunctions
          .filter((agg) => agg.func !== 'COUNT')
          .filter((agg) => isAggregationValidForColumn(agg.func, currentColumn))
          .map((agg) => {
            return m(MenuItem, {
              label: agg.label,
              onclick: () => {
                // Remove from groupBy
                const newGroupBy = currentGroupBy.filter(
                  (name) => name !== currentColumn.name,
                );

                const alias = `${currentColumn.name}_${agg.func.toLowerCase()}`;
                const newValue: PivotValue = {
                  col: currentColumn.name,
                  func: agg.func,
                };

                const newValues = {
                  ...pivot?.values,
                  [alias]: newValue,
                };

                if (
                  newGroupBy.length === 0 &&
                  Object.keys(newValues).length === 0
                ) {
                  onPivotChanged(undefined);
                } else {
                  const newPivot: PivotModel = {
                    groupBy: newGroupBy,
                    values: newValues,
                  };
                  onPivotChanged(newPivot);
                }
              },
            });
          }),
      ),

    // Add pivot column - only show when in pivot mode and not on an aggregate column
    pivot &&
      !isCurrentColumnAggregate &&
      m(
        MenuItem,
        {
          label: 'Add pivot...',
          icon: 'add',
          disabled: availableColumns.length === 0,
        },
        availableColumns.map((col) => {
          const columnLabel =
            col.title !== undefined ? String(col.title) : col.name;
          return m(MenuItem, {
            label: columnLabel,
            onclick: () => {
              // Insert after current column if it's a pivot column, otherwise at end
              let insertIndex = currentGroupBy.length;
              if (isCurrentColumnGrouped) {
                const currentIndex = currentGroupBy.indexOf(currentColumn.name);
                if (currentIndex !== -1) {
                  insertIndex = currentIndex + 1;
                }
              }
              const newGroupBy = [
                ...currentGroupBy.slice(0, insertIndex),
                col.name,
                ...currentGroupBy.slice(insertIndex),
              ];
              const newPivot: PivotModel = {
                groupBy: newGroupBy,
                values: pivot?.values ?? {},
              };
              onPivotChanged(newPivot);
            },
          });
        }),
      ),

    // Add aggregate - only show when in pivot mode and not on a pivot column
    pivot &&
      !isCurrentColumnGrouped &&
      m(
        MenuItem,
        {
          label: 'Add aggregate',
          icon: 'add',
        },
        [
          // COUNT - always available as it doesn't need a specific column
          m(MenuItem, {
            label: 'COUNT',
            onclick: () => {
              const newValues = insertValueAfterColumn(
                pivot?.values ?? {},
                currentColumn.name,
                isCurrentColumnAggregate,
                'count',
                {func: 'COUNT'} as PivotValue,
              );
              const newPivot: PivotModel = {
                groupBy: currentGroupBy,
                values: newValues,
              };
              onPivotChanged(newPivot);
            },
          }),
          m(MenuDivider),
          // Show each available column (non-grouped columns)
          ...availableColumns.map((col) => {
            const columnLabel =
              col.title !== undefined ? String(col.title) : col.name;
            return m(
              MenuItem,
              {
                label: columnLabel,
              },
              // Show aggregation functions for this column (excluding COUNT which is column-independent)
              aggregationFunctions
                .filter((agg) => agg.func !== 'COUNT')
                .filter((agg) => isAggregationValidForColumn(agg.func, col))
                .map((agg) => {
                  return m(MenuItem, {
                    label: agg.label,
                    onclick: () => {
                      const alias = `${col.name}_${agg.func.toLowerCase()}`;
                      const newValues = insertValueAfterColumn(
                        pivot?.values ?? {},
                        currentColumn.name,
                        isCurrentColumnAggregate,
                        alias,
                        {col: col.name, func: agg.func} as PivotValue,
                      );
                      const newPivot: PivotModel = {
                        groupBy: currentGroupBy,
                        values: newValues,
                      };
                      onPivotChanged(newPivot);
                    },
                  });
                }),
            );
          }),
        ],
      ),
  ];
}
