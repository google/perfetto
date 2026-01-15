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
import {maybeUndefined} from '../../../base/utils';
import {MenuDivider, MenuItem} from '../../../widgets/menu';
import {
  getColumnFilterType,
  isColumnDef,
  isParameterizedColumnDef,
  isSchemaRef,
  SchemaRegistry,
} from './column_schema';
import {AggregationFunction, PivotModel, PivotValue} from './model';
import {Icons} from '../../../base/semantic_icons';

export type OnPivotChanged = (pivot: PivotModel | undefined) => void;

// Numeric-only aggregation functions that don't make sense for strings
const NUMERIC_ONLY_FUNCTIONS: ReadonlySet<AggregationFunction> = new Set([
  'SUM',
  'AVG',
  'MIN',
  'MAX',
]);

// Available aggregation functions
const AGGREGATION_FUNCTIONS: ReadonlyArray<{
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

// Helper to insert a new value entry after a specific column in the values object
function insertValueAfterColumn(
  values: {readonly [key: string]: PivotValue},
  afterColumn: string,
  isAfterColumnAggregate: boolean,
  newAlias: string,
  newValue: PivotValue,
): {readonly [key: string]: PivotValue} {
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
  values: {readonly [key: string]: PivotValue},
  oldAlias: string,
  newAlias: string,
  newValue: PivotValue,
): {readonly [key: string]: PivotValue} {
  const entries = Object.entries(values);
  const newEntries = entries.map(([key, val]) => {
    if (key === oldAlias) {
      return [newAlias, newValue] as [string, PivotValue];
    }
    return [key, val] as [string, PivotValue];
  });
  return Object.fromEntries(newEntries);
}

// Check if an aggregation function is valid for a column based on its filter type
function isAggregationValidForFilterType(
  func: AggregationFunction,
  filterType: 'numeric' | 'string' | undefined,
): boolean {
  // COUNT and ANY are always valid
  if (!NUMERIC_ONLY_FUNCTIONS.has(func)) return true;
  // If column is explicitly marked as string type, don't allow numeric functions
  if (filterType === 'string') return false;
  // Otherwise allow (either numeric or unknown type)
  return true;
}

interface ColumnInfo {
  readonly name: string;
  readonly title: m.Children;
  readonly filterType?: 'numeric' | 'string';
}

/**
 * Builds menu items for adding groupBy columns from a schema.
 * Recursively walks the schema to build nested submenus.
 */
function buildGroupByMenuFromSchema(
  registry: SchemaRegistry,
  schemaName: string,
  pathPrefix: string,
  depth: number,
  currentGroupBy: ReadonlyArray<string>,
  onSelect: (columnPath: string) => void,
  maxDepth: number = 5,
): m.Children[] {
  const schema = maybeUndefined(registry[schemaName]);
  if (!schema) return [];

  // Stop if we've gone too deep (prevents infinite menus for self-referential schemas)
  if (depth > maxDepth) {
    return [m(MenuItem, {label: '(max depth reached)', disabled: true})];
  }

  const menuItems: m.Children[] = [];

  for (const [columnName, entry] of Object.entries(schema)) {
    const fullPath = pathPrefix ? `${pathPrefix}.${columnName}` : columnName;

    if (isColumnDef(entry)) {
      // Leaf column - clicking adds it as a pivot (disabled if already grouped)
      const title = entry.title ?? columnName;
      const isAlreadyGrouped = currentGroupBy.includes(fullPath);
      menuItems.push(
        m(MenuItem, {
          label: title,
          disabled: isAlreadyGrouped,
          onclick: () => onSelect(fullPath),
        }),
      );
    } else if (isSchemaRef(entry)) {
      // Reference to another schema - create a submenu
      const refTitle = entry.title ?? columnName;
      const childMenuItems = buildGroupByMenuFromSchema(
        registry,
        entry.ref,
        fullPath,
        depth + 1,
        currentGroupBy,
        onSelect,
        maxDepth,
      );

      if (childMenuItems.length > 0) {
        menuItems.push(m(MenuItem, {label: refTitle}, childMenuItems));
      }
    } else if (isParameterizedColumnDef(entry)) {
      // For parameterized columns, we can't really pivot on them without knowing keys
      // Could potentially show available keys from data source in the future
      const title = typeof entry.title === 'string' ? entry.title : columnName;
      menuItems.push(
        m(MenuItem, {
          label: `${title}...`,
          disabled: true,
        }),
      );
    }
  }

  return menuItems;
}

/**
 * Builds menu items for adding aggregates from a schema.
 * Recursively walks the schema to build nested submenus with aggregation functions.
 */
function buildAggregateMenuFromSchema(
  registry: SchemaRegistry,
  schemaName: string,
  pathPrefix: string,
  depth: number,
  pivot: PivotModel,
  currentColumn: ColumnInfo,
  isCurrentColumnAggregate: boolean,
  onPivotChanged: OnPivotChanged,
  maxDepth: number = 5,
): m.Children[] {
  const schema = maybeUndefined(registry[schemaName]);
  if (!schema) return [];

  // Stop if we've gone too deep
  if (depth > maxDepth) {
    return [m(MenuItem, {label: '(max depth reached)', disabled: true})];
  }

  const menuItems: m.Children[] = [];
  const currentGroupBy = pivot.groupBy;

  for (const [columnName, entry] of Object.entries(schema)) {
    const fullPath = pathPrefix ? `${pathPrefix}.${columnName}` : columnName;

    if (isColumnDef(entry)) {
      // Leaf column - show aggregation function submenu (skip if already grouped)
      const title = entry.title ?? columnName;
      const isGrouped = currentGroupBy.includes(fullPath);

      if (isGrouped) {
        // Skip columns that are already in groupBy
        continue;
      }

      // Get available aggregation functions for this column
      const availableFunctions = AGGREGATION_FUNCTIONS.filter(
        (agg) => agg.func !== 'COUNT',
      ) // COUNT is separate
        .filter((agg) =>
          isAggregationValidForFilterType(agg.func, entry.filterType),
        );

      if (availableFunctions.length > 0) {
        menuItems.push(
          m(
            MenuItem,
            {label: title},
            availableFunctions.map((agg) => {
              return m(MenuItem, {
                label: agg.label,
                onclick: () => {
                  const alias = `${fullPath}_${agg.func.toLowerCase()}`;
                  const newValues = insertValueAfterColumn(
                    pivot.values ?? {},
                    currentColumn.name,
                    isCurrentColumnAggregate,
                    alias,
                    {col: fullPath, func: agg.func} as PivotValue,
                  );
                  const newPivot: PivotModel = {
                    groupBy: currentGroupBy,
                    values: newValues,
                  };
                  onPivotChanged(newPivot);
                },
              });
            }),
          ),
        );
      }
    } else if (isSchemaRef(entry)) {
      // Reference to another schema - create a submenu
      const refTitle = entry.title ?? columnName;
      const childMenuItems = buildAggregateMenuFromSchema(
        registry,
        entry.ref,
        fullPath,
        depth + 1,
        pivot,
        currentColumn,
        isCurrentColumnAggregate,
        onPivotChanged,
        maxDepth,
      );

      if (childMenuItems.length > 0) {
        menuItems.push(m(MenuItem, {label: refTitle}, childMenuItems));
      }
    } else if (isParameterizedColumnDef(entry)) {
      // For parameterized columns, could show available keys in future
      const title = typeof entry.title === 'string' ? entry.title : columnName;
      menuItems.push(
        m(MenuItem, {
          label: `${title}...`,
          disabled: true,
        }),
      );
    }
  }

  return menuItems;
}

/**
 * Renders pivot menu items for a normal column (when not in pivot mode).
 * Shows "Pivot on this" option to enter pivot mode.
 */
export function renderPivotMenuForNormalColumn(
  currentColumn: ColumnInfo,
  columns: ReadonlyArray<string>,
  onPivotChanged: OnPivotChanged,
): m.Children {
  return m(MenuItem, {
    label: 'Pivot on this',
    icon: 'pivot_table_chart',
    onclick: () => {
      const newGroupBy = [currentColumn.name];
      // Add all other visible columns as 'ANY' aggregates to preserve visibility
      const newValues: {[key: string]: PivotValue} = {};
      for (const col of columns) {
        if (col !== currentColumn.name) {
          newValues[col] = {
            col: col,
            func: 'ANY',
          };
        }
      }
      const newPivot: PivotModel = {
        groupBy: newGroupBy,
        values: newValues,
      };
      onPivotChanged(newPivot);
    },
  });
}

/**
 * Renders pivot menu items for a groupBy column in pivot mode.
 * Shows Remove, Add pivot..., and Add aggregate... options.
 */
export function renderPivotMenuForGroupByColumn(
  schema: SchemaRegistry,
  rootSchema: string,
  pivot: PivotModel,
  currentColumn: ColumnInfo,
  onPivotChanged: OnPivotChanged,
): m.Children {
  const currentGroupBy = pivot.groupBy;

  return [
    // Remove this groupBy column
    m(MenuItem, {
      label: 'Remove pivot',
      icon: Icons.Remove,
      onclick: () => {
        const newGroupBy = currentGroupBy.filter(
          (name) => name !== currentColumn.name,
        );
        if (newGroupBy.length === 0) {
          // No more groupBy columns - exit pivot mode entirely
          onPivotChanged(undefined);
        } else {
          const newPivot: PivotModel = {
            groupBy: newGroupBy,
            values: pivot.values,
          };
          onPivotChanged(newPivot);
        }
      },
    }),

    // Add pivot column after this one
    m(
      MenuItem,
      {
        label: 'Add pivot...',
        icon: 'add_column_right',
      },
      buildGroupByMenuFromSchema(
        schema,
        rootSchema,
        '',
        0,
        currentGroupBy,
        (columnPath) => {
          // Insert after current column
          const currentIndex = currentGroupBy.indexOf(currentColumn.name);
          const insertIndex =
            currentIndex !== -1 ? currentIndex + 1 : currentGroupBy.length;
          const newGroupBy = [
            ...currentGroupBy.slice(0, insertIndex),
            columnPath,
            ...currentGroupBy.slice(insertIndex),
          ];
          const newPivot: PivotModel = {
            groupBy: newGroupBy,
            values: pivot.values,
          };
          onPivotChanged(newPivot);
        },
      ),
    ),
  ];
}

/**
 * Renders pivot menu items for an aggregate column in pivot mode.
 * Shows Remove, Change function, Pivot on this, and Add aggregate... options.
 */
export function renderPivotMenuForAggregateColumn(
  schema: SchemaRegistry,
  rootSchema: string,
  pivot: PivotModel,
  currentColumn: ColumnInfo,
  onPivotChanged: OnPivotChanged,
): m.Children {
  const currentGroupBy = pivot.groupBy;
  const pivotValue = maybeUndefined(pivot.values[currentColumn.name]);

  // Get the base column name - if this is a pivoted value column, use its source
  const baseColumnName =
    pivotValue && 'col' in pivotValue ? pivotValue.col : currentColumn.name;

  const menuItems: m.Children[] = [];

  // Remove this aggregate column
  menuItems.push(
    m(MenuItem, {
      label: 'Remove aggregate',
      icon: Icons.Remove,
      onclick: () => {
        const newValues = {...pivot.values};
        delete newValues[currentColumn.name];
        if (
          currentGroupBy.length === 0 &&
          Object.keys(newValues).length === 0
        ) {
          // No more groupBy and no values - exit pivot mode
          onPivotChanged(undefined);
        } else {
          const newPivot: PivotModel = {
            groupBy: currentGroupBy,
            values: newValues,
          };
          onPivotChanged(newPivot);
        }
      },
    }),
  );

  // Change function - only for non-COUNT aggregates (COUNT loses column info)
  if (pivotValue?.func !== 'COUNT') {
    const sourceFilterType = getColumnFilterType(
      schema,
      rootSchema,
      baseColumnName,
    );

    // Filter to functions that can be switched to
    const availableFunctions = AGGREGATION_FUNCTIONS.filter(
      (f) => f.func !== pivotValue?.func,
    )
      .filter((f) => f.func !== 'COUNT') // COUNT loses the original column info
      .filter((f) => isAggregationValidForFilterType(f.func, sourceFilterType));

    // Always show Change function, but disable if no options available
    menuItems.push(
      m(
        MenuItem,
        {
          label: 'Change function',
          icon: 'swap_horiz',
          disabled: availableFunctions.length === 0,
        },
        availableFunctions.map((agg) => {
          return m(MenuItem, {
            label: agg.label,
            onclick: () => {
              const sourceCol =
                pivotValue && 'col' in pivotValue
                  ? pivotValue.col
                  : baseColumnName;
              const newAlias = `${sourceCol}_${agg.func.toLowerCase()}`;
              const newValue: PivotValue = {
                col: sourceCol,
                func: agg.func,
              };

              // Replace old aggregate with new one, preserving position
              const newValues = replaceValuePreservingPosition(
                pivot.values,
                currentColumn.name,
                newAlias,
                newValue,
              );

              const newPivot: PivotModel = {
                groupBy: currentGroupBy,
                values: newValues,
              };
              onPivotChanged(newPivot);
            },
          });
        }),
      ),
    );

    // Pivot on this - converts aggregate to groupBy (not available for COUNT)
    menuItems.push(
      m(MenuItem, {
        label: 'Pivot on this',
        icon: 'pivot_table_chart',
        onclick: () => {
          // Don't add if already in groupBy
          if (currentGroupBy.includes(baseColumnName)) return;

          const newGroupBy = [...currentGroupBy, baseColumnName];
          const newValues = {...pivot.values};
          // Remove the current aggregate since we're pivoting on its source column
          delete newValues[currentColumn.name];

          const newPivot: PivotModel = {
            groupBy: newGroupBy,
            values: newValues,
          };
          onPivotChanged(newPivot);
        },
      }),
    );
  }

  // Add aggregate after this one
  menuItems.push(
    m(
      MenuItem,
      {
        label: 'Add aggregate...',
        icon: 'add_column_right',
      },
      [
        // COUNT - always available
        m(MenuItem, {
          label: 'COUNT',
          onclick: () => {
            const newValues = insertValueAfterColumn(
              pivot.values,
              currentColumn.name,
              true, // isCurrentColumnAggregate = true
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
        // Schema-based column discovery for aggregates
        ...buildAggregateMenuFromSchema(
          schema,
          rootSchema,
          '',
          0,
          pivot,
          currentColumn,
          true, // isCurrentColumnAggregate = true
          onPivotChanged,
        ),
      ],
    ),
  );

  return menuItems;
}
