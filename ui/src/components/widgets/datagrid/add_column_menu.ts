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
import {FuzzyFinder} from '../../../base/fuzzy';
import {Icons} from '../../../base/semantic_icons';
import {maybeUndefined} from '../../../base/utils';
import {EmptyState} from '../../../widgets/empty_state';
import {Icon} from '../../../widgets/icon';
import {MenuItem} from '../../../widgets/menu';
import {TextInput} from '../../../widgets/text_input';
import {DataSource} from './data_source';
import {
  SchemaRegistry,
  isColumnDef,
  isParameterizedColumnDef,
  isSchemaRef,
} from './datagrid_schema';
import {AggregateColumn, AggregateFunction} from './model';

interface AddColumnMenuContext {
  readonly dataSource: DataSource;
  readonly parameterKeyColumns: Set<string>;
}

/**
 * Builds menu items for adding columns from a schema.
 * Recursively builds submenus for schema references.
 *
 * @param registry The schema registry
 * @param schemaName The name of the current schema to build from
 * @param pathPrefix The current path prefix (e.g., 'parent' or 'thread.process')
 * @param depth Current recursion depth (to prevent infinite menus)
 * @param columns Currently visible columns (to disable duplicates)
 * @param onSelect Callback when a column is selected
 * @param context Context containing dataSource and parameterKeyColumns for key discovery
 * @param maxDepth Maximum recursion depth (default 5)
 */
function buildAddColumnMenuFromSchema(
  registry: SchemaRegistry,
  schemaName: string,
  pathPrefix: string,
  depth: number,
  columns: ReadonlyArray<string>,
  onSelect: (columnPath: string) => void,
  context: AddColumnMenuContext,
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
      // Leaf column - clicking adds it (disabled if already visible)
      const title = entry.title ?? columnName;
      const isAlreadyVisible = columns.includes(fullPath);
      menuItems.push(
        m(MenuItem, {
          label: title,
          disabled: isAlreadyVisible,
          onclick: () => onSelect(fullPath),
        }),
      );
    } else if (isSchemaRef(entry)) {
      // Reference to another schema - create a submenu
      const refTitle = entry.title ?? columnName;
      const childMenuItems = buildAddColumnMenuFromSchema(
        registry,
        entry.ref,
        fullPath,
        depth + 1,
        columns,
        onSelect,
        context,
        maxDepth,
      );

      if (childMenuItems.length > 0) {
        menuItems.push(m(MenuItem, {label: refTitle}, childMenuItems));
      }
    } else if (isParameterizedColumnDef(entry)) {
      // Parameterized column - show available keys from datasource
      const title = typeof entry.title === 'string' ? entry.title : columnName;
      const availableKeys = context.dataSource.parameterKeys?.get(fullPath);
      menuItems.push(
        m(
          MenuItem,
          {
            label: `${title}...`,
            onChange: (isOpen) => {
              if (isOpen === true) {
                context.parameterKeyColumns.add(fullPath);
              } else {
                context.parameterKeyColumns.delete(fullPath);
              }
            },
          },
          m(ParameterizedColumnSubmenu, {
            pathPrefix: fullPath,
            columns,
            availableKeys,
            onSelect,
          }),
        ),
      );
    }
  }

  return menuItems;
}

// Helper component for parameterized column input
export interface ParameterizedColumnSubmenuAttrs {
  readonly pathPrefix: string;
  readonly columns: ReadonlyArray<string>;
  readonly availableKeys: ReadonlyArray<string> | undefined;
  readonly onSelect: (columnPath: string) => void;
}

export class ParameterizedColumnSubmenu
  implements m.ClassComponent<ParameterizedColumnSubmenuAttrs>
{
  private searchQuery = '';
  private static readonly MAX_VISIBLE_ITEMS = 100;

  view({attrs}: m.Vnode<ParameterizedColumnSubmenuAttrs>) {
    const {pathPrefix, columns, availableKeys, onSelect} = attrs;

    // Show loading state if availableKeys is undefined
    if (availableKeys === undefined) {
      return m('.pf-distinct-values-menu', [
        m(MenuItem, {label: 'Loading...', disabled: true}),
      ]);
    }

    // Use fuzzy search to filter and get highlighted segments
    const fuzzyResults = (() => {
      if (this.searchQuery === '') {
        // No search - show all keys without highlighting
        return availableKeys.map((key) => ({
          key,
          segments: [{matching: false, value: key}],
        }));
      } else {
        // Fuzzy search with highlighting
        const finder = new FuzzyFinder(availableKeys, (k) => k);
        return finder.find(this.searchQuery).map((result) => ({
          key: result.item,
          segments: result.segments,
        }));
      }
    })();

    // Limit the number of items rendered
    const visibleResults = fuzzyResults.slice(
      0,
      ParameterizedColumnSubmenu.MAX_VISIBLE_ITEMS,
    );
    const remainingCount =
      fuzzyResults.length - ParameterizedColumnSubmenu.MAX_VISIBLE_ITEMS;

    // Check if search query could be used as a custom key
    const customKeyPath =
      this.searchQuery.trim().length > 0
        ? `${pathPrefix}.${this.searchQuery.trim()}`
        : '';
    const isCustomKeyAlreadyVisible =
      customKeyPath !== '' && columns.includes(customKeyPath);
    const isCustomKeyInResults =
      this.searchQuery.trim().length > 0 &&
      availableKeys.includes(this.searchQuery.trim());

    return m('.pf-distinct-values-menu', [
      // Search input
      m(
        '.pf-distinct-values-menu__search',
        {
          onclick: (e: MouseEvent) => {
            // Prevent menu from closing when clicking search box
            e.stopPropagation();
          },
        },
        m(TextInput, {
          placeholder: 'Search or enter key name...',
          value: this.searchQuery,
          oninput: (e: InputEvent) => {
            this.searchQuery = (e.target as HTMLInputElement).value;
          },
          onkeydown: (e: KeyboardEvent) => {
            if (this.searchQuery !== '' && e.key === 'Escape') {
              this.searchQuery = '';
              e.stopPropagation(); // Prevent menu from closing
            }
          },
        }),
      ),
      // List of available keys
      m(
        '.pf-distinct-values-menu__list',
        fuzzyResults.length > 0
          ? [
              visibleResults.map((result) => {
                const keyPath = `${pathPrefix}.${result.key}`;
                const isKeyAlreadyVisible = columns.includes(keyPath);

                // Render highlighted label
                const labelContent = result.segments.map((segment) => {
                  if (segment.matching) {
                    return m('strong.pf-fuzzy-match', segment.value);
                  } else {
                    return segment.value;
                  }
                });

                return m(
                  'button.pf-menu-item' +
                    (isKeyAlreadyVisible ? '[disabled]' : ''),
                  {
                    onclick: () => {
                      if (!isKeyAlreadyVisible) {
                        onSelect(keyPath);
                        this.searchQuery = '';
                      }
                    },
                  },
                  m('.pf-menu-item__label', labelContent),
                  isKeyAlreadyVisible &&
                    m(Icon, {
                      className: 'pf-menu-item__right-icon',
                      icon: 'check',
                    }),
                );
              }),
              remainingCount > 0 &&
                m(MenuItem, {
                  label: `...and ${remainingCount} more`,
                  disabled: true,
                }),
            ]
          : m(EmptyState, {
              title: 'No matches',
            }),
      ),
      // Footer with "Add custom" option when search query doesn't match existing keys
      this.searchQuery.trim().length > 0 &&
        !isCustomKeyInResults &&
        m('.pf-distinct-values-menu__footer', [
          m(MenuItem, {
            label: `Add "${this.searchQuery.trim()}"`,
            icon: 'add',
            disabled: isCustomKeyAlreadyVisible,
            onclick: () => {
              if (!isCustomKeyAlreadyVisible) {
                onSelect(customKeyPath);
                this.searchQuery = '';
              }
            },
          }),
        ]),
    ]);
  }
}

interface ColumnMenuAttrs {
  readonly schema: SchemaRegistry;
  readonly rootSchema: string;
  readonly visibleColumns: ReadonlyArray<string>;
  readonly onAddColumn: (field: string) => void;
  readonly dataSource: DataSource;
  readonly parameterKeyColumns: Set<string>;

  // Optional remove button - if not provided, only "Add" is shown
  readonly canRemove?: boolean;
  readonly onRemove?: () => void;

  // Custom labels (defaults: "Remove column", "Add column")
  readonly removeLabel?: string;
  readonly addLabel?: string;
}

/**
 * Renders column management menu items.
 * Can show "Remove" and "Add" buttons with configurable labels.
 * If onRemove is not provided, only the "Add" button is shown.
 */
export class ColumnMenu implements m.ClassComponent<ColumnMenuAttrs> {
  view({attrs}: m.Vnode<ColumnMenuAttrs>): m.Children {
    const {
      canRemove,
      onRemove,
      schema,
      rootSchema,
      visibleColumns,
      onAddColumn,
      dataSource,
      parameterKeyColumns,
      removeLabel = 'Remove column',
      addLabel = 'Add column',
    } = attrs;

    const addColumnSubmenu = buildAddColumnMenuFromSchema(
      schema,
      rootSchema,
      '',
      0,
      visibleColumns,
      onAddColumn,
      {dataSource, parameterKeyColumns},
    );

    const items: m.Children[] = [];

    return [
      m(
        MenuItem,
        {label: addLabel, icon: Icons.AddColumnRight},
        addColumnSubmenu,
      ),
      onRemove &&
        m(MenuItem, {
          label: removeLabel,
          disabled: !canRemove,
          icon: Icons.Remove,
          onclick: onRemove,
        }),
    ];

    return items;
  }
}

// Numeric aggregate functions - only valid for quantitative/identifier columns
const NUMERIC_AGGREGATE_FUNCTIONS: AggregateFunction[] = [
  'SUM',
  'AVG',
  'MIN',
  'MAX',
];

// Text-safe aggregate functions - valid for all column types
const TEXT_SAFE_AGGREGATE_FUNCTIONS: AggregateFunction[] = ['ANY'];

/**
 * Returns the available aggregate functions for a column based on its type.
 * Numeric aggregates (SUM, AVG, MIN, MAX) are only available for quantitative
 * and identifier columns. ANY is available for all column types.
 */
export function getAggregateFunctionsForColumnType(
  columnType: 'text' | 'quantitative' | 'identifier' | undefined,
): AggregateFunction[] {
  if (columnType === 'quantitative' || columnType === 'identifier') {
    return [...NUMERIC_AGGREGATE_FUNCTIONS, ...TEXT_SAFE_AGGREGATE_FUNCTIONS];
  }
  // For 'text' columns or undefined, only text-safe aggregates
  return TEXT_SAFE_AGGREGATE_FUNCTIONS;
}

/**
 * Checks if an aggregate with the given function and field already exists.
 */
function isAggregateExists(
  existingAggregates: readonly AggregateColumn[] | undefined,
  func: AggregateFunction | 'COUNT',
  field: string | undefined,
): boolean {
  if (!existingAggregates) return false;
  return existingAggregates.some((agg) => {
    if (agg.function !== func) return false;
    const aggField = 'field' in agg ? agg.field : undefined;
    return aggField === field;
  });
}

/**
 * Builds menu items for adding aggregate columns from a schema.
 * Each column shows a submenu with aggregate function options.
 */
function buildAggregateColumnMenuFromSchema(
  registry: SchemaRegistry,
  schemaName: string,
  pathPrefix: string,
  depth: number,
  onSelect: (func: AggregateFunction, field: string) => void,
  existingAggregates: readonly AggregateColumn[] | undefined,
  maxDepth: number = 5,
): m.Children[] {
  const schema = maybeUndefined(registry[schemaName]);
  if (!schema) return [];

  if (depth > maxDepth) {
    return [m(MenuItem, {label: '(max depth reached)', disabled: true})];
  }

  const menuItems: m.Children[] = [];

  for (const [columnName, entry] of Object.entries(schema)) {
    const fullPath = pathPrefix ? `${pathPrefix}.${columnName}` : columnName;

    if (isColumnDef(entry)) {
      const title = entry.title ?? columnName;
      // Get available aggregate functions based on column type
      const availableFuncs = getAggregateFunctionsForColumnType(
        entry.columnType,
      );
      const aggFuncItems = availableFuncs.map((func) => {
        const exists = isAggregateExists(existingAggregates, func, fullPath);
        return m(MenuItem, {
          label: func,
          disabled: exists,
          onclick: exists ? undefined : () => onSelect(func, fullPath),
        });
      });

      menuItems.push(m(MenuItem, {label: title}, aggFuncItems));
    } else if (isSchemaRef(entry)) {
      const refTitle = entry.title ?? columnName;
      const childMenuItems = buildAggregateColumnMenuFromSchema(
        registry,
        entry.ref,
        fullPath,
        depth + 1,
        onSelect,
        existingAggregates,
        maxDepth,
      );

      if (childMenuItems.length > 0) {
        menuItems.push(m(MenuItem, {label: refTitle}, childMenuItems));
      }
    } else if (isParameterizedColumnDef(entry)) {
      // For parameterized columns, show just the base name with a note
      const title = typeof entry.title === 'string' ? entry.title : columnName;
      // Get available aggregate functions based on filter type
      const availableFuncs = getAggregateFunctionsForColumnType(
        entry.filterType,
      );
      const aggFuncItems = availableFuncs.map((func) => {
        const exists = isAggregateExists(existingAggregates, func, fullPath);
        return m(MenuItem, {
          label: func,
          disabled: exists,
          onclick: exists ? undefined : () => onSelect(func, fullPath),
        });
      });

      menuItems.push(m(MenuItem, {label: `${title} (base)`}, aggFuncItems));
    }
  }

  return menuItems;
}

interface AggregateMenuAttrs {
  readonly schema: SchemaRegistry;
  readonly rootSchema: string;
  readonly onAddAggregate: (
    func: AggregateFunction | 'COUNT',
    field: string | undefined,
  ) => void;

  // Existing aggregates to gray out in the menu
  readonly existingAggregates?: readonly AggregateColumn[];

  // Custom label (default: "Add aggregate")
  readonly label?: string;
}

/**
 * Renders an "Add aggregate" menu item with:
 * - COUNT option (no field needed)
 * - Column picker submenu where each column has aggregate function options
 */
export class AggregateMenu implements m.ClassComponent<AggregateMenuAttrs> {
  view({attrs}: m.Vnode<AggregateMenuAttrs>): m.Children {
    const {
      schema,
      rootSchema,
      onAddAggregate,
      existingAggregates,
      label = 'Add column',
    } = attrs;

    const columnAggSubmenu = buildAggregateColumnMenuFromSchema(
      schema,
      rootSchema,
      '',
      0,
      (func, field) => onAddAggregate(func, field),
      existingAggregates,
    );

    const countExists = isAggregateExists(
      existingAggregates,
      'COUNT',
      undefined,
    );

    return m(
      MenuItem,
      {label, icon: Icons.AddColumnRight},
      // COUNT option - doesn't need a field
      m(MenuItem, {
        label: 'COUNT',
        disabled: countExists,
        onclick: countExists
          ? undefined
          : () => onAddAggregate('COUNT', undefined),
      }),
      // Column-based aggregates
      ...columnAggSubmenu,
    );
  }
}
