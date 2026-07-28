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
import {fuzzySearch} from '../../../base/fuzzy';
import {Icons} from '../../../base/semantic_icons';
import {EmptyState} from '../../../widgets/empty_state';
import {Icon} from '../../../widgets/icon';
import {MenuItem} from '../../../widgets/menu';
import {TextInput} from '../../../widgets/text_input';
import type {DataSource} from './data_source';
import type {ColumnSchema} from './datagrid_schema';
import type {AggregateColumn, AggregateFunction} from './model';

interface AddColumnSchemaMenuItemAttrs {
  // Label shown on this submenu's own menu item.
  readonly label: m.Children;
  // Optional icon shown alongside the label.
  readonly icon?: string;
  // The schema whose columns are listed in this submenu.
  readonly schema: ColumnSchema;
  // Path prefix for columns at this level (e.g. 'parent' or 'thread.process').
  readonly pathPrefix: string;
  // Currently visible columns, used to disable already-added entries.
  readonly existingColumns: readonly string[];
  // Data source used to discover keys for parameterized columns.
  readonly datasource: DataSource;
  // Callback invoked with the full column path when a column is selected.
  readonly onSelect: (columnPath: string) => void;
}

/**
 * A menu item that expands into a submenu of the columns available in a schema.
 * Schema references recurse through nested instances of this component, so each
 * level is only built when its submenu is opened - no depth limit needed.
 */
const AddColumnSchemaMenuItem: m.Component<AddColumnSchemaMenuItemAttrs> = {
  view({attrs}) {
    const {
      label,
      icon,
      schema,
      pathPrefix,
      existingColumns,
      onSelect,
      datasource,
    } = attrs;
    const menuItems: m.Children[] = [];

    for (const [columnName, entry] of Object.entries(schema)) {
      const fullPath = pathPrefix ? `${pathPrefix}.${columnName}` : columnName;
      const title = entry.title ?? columnName;

      if ('parameterized' in entry) {
        // Parameterized column - show available keys from datasource
        menuItems.push(
          m(AddColumnParamMenuItem, {
            label: `${title}...`,
            pathPrefix: fullPath,
            existingColumns,
            datasource,
            onSelect,
          }),
        );
      } else if ('schema' in entry) {
        menuItems.push(
          m(AddColumnSchemaMenuItem, {
            label: title,
            schema: entry.schema,
            pathPrefix: fullPath,
            existingColumns,
            onSelect,
            datasource,
          }),
        );
      } else {
        menuItems.push(
          m(MenuItem, {
            label: title,
            disabled: existingColumns.includes(fullPath),
            onclick: () => onSelect(fullPath),
          }),
        );
      }
    }

    return m(
      MenuItem,
      {label, icon, disabled: menuItems.length === 0},
      menuItems,
    );
  },
};

export interface AddColumnParamMenuItemAttrs {
  // Label shown on this submenu's own menu item.
  readonly label: m.Children;
  // Optional icon shown alongside the label.
  readonly icon?: string;
  // Path prefix of the parameterized column (the key is appended to this).
  readonly pathPrefix: string;
  // Currently visible columns, used to disable already-added entries.
  readonly existingColumns: ReadonlyArray<string>;
  // Data source used to discover the available parameter keys.
  readonly datasource: DataSource;
  // Callback invoked with the full column path when a key is selected.
  readonly onSelect: (columnPath: string) => void;
}

/**
 * A menu item for a parameterized column that expands into a searchable list of
 * the keys available in the data source. Keys are only fetched when the submenu
 * is opened.
 */
export class AddColumnParamMenuItem implements m.ClassComponent<AddColumnParamMenuItemAttrs> {
  view({attrs}: m.Vnode<AddColumnParamMenuItemAttrs>) {
    const {pathPrefix, existingColumns, datasource, onSelect, label, icon} =
      attrs;
    return m(MenuItem, {label, icon}, [
      m(RecordPopup, {pathPrefix, existingColumns, datasource, onSelect}),
    ]);
  }
}

interface RecordPopupAttrs {
  // Path prefix of the parameterized column (the key is appended to this).
  readonly pathPrefix: string;
  // Currently visible columns, used to disable already-added entries.
  readonly existingColumns: ReadonlyArray<string>;
  // Data source used to discover the available parameter keys.
  readonly datasource: DataSource;
  // Callback invoked with the full column path when a key is selected.
  readonly onSelect: (columnPath: string) => void;
}

class RecordPopup implements m.ClassComponent<RecordPopupAttrs> {
  private readonly MAX_VISIBLE_ITEMS = 100;
  private searchQuery = '';

  view({attrs}: m.Vnode<RecordPopupAttrs>): m.Children {
    const {pathPrefix, existingColumns, datasource, onSelect} = attrs;

    // Fetch available keys - this is only called when the submenu is visible
    const {data: availableKeys, isPending} =
      datasource.useParameterKeys(pathPrefix);

    // Show loading state while fetching
    if (isPending || availableKeys === undefined) {
      return m('.pf-distinct-values-menu', [
        m(MenuItem, {label: 'Loading...', disabled: true}),
      ]);
    }

    // Use fuzzy search to filter and get highlighted segments
    const fuzzyResults = (() => {
      if (this.searchQuery === '') {
        // No search - show all keys without highlighting
        return availableKeys.map((key: string) => ({
          key,
          segments: [{matching: false, value: key}],
        }));
      } else {
        // Fuzzy search with highlighting
        return fuzzySearch(
          availableKeys as string[],
          (k: string) => k,
          this.searchQuery,
        ).map((result) => ({
          key: result.item,
          segments: result.segments,
        }));
      }
    })();

    // Limit the number of items rendered
    const visibleResults = fuzzyResults.slice(0, this.MAX_VISIBLE_ITEMS);
    const remainingCount = fuzzyResults.length - this.MAX_VISIBLE_ITEMS;

    // Check if search query could be used as a custom key
    const customKeyPath =
      this.searchQuery.trim().length > 0
        ? `${pathPrefix}.${this.searchQuery.trim()}`
        : '';
    const isCustomKeyAlreadyVisible =
      customKeyPath !== '' && existingColumns.includes(customKeyPath);
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
              visibleResults.map(
                (result: {
                  key: string;
                  segments: readonly {matching: boolean; value: string}[];
                }) => {
                  const keyPath = `${pathPrefix}.${result.key}`;
                  const isKeyAlreadyVisible = existingColumns.includes(keyPath);

                  // Render highlighted label
                  const labelContent = result.segments.map(
                    (segment: {matching: boolean; value: string}) => {
                      if (segment.matching) {
                        return m('strong.pf-fuzzy-match', segment.value);
                      } else {
                        return segment.value;
                      }
                    },
                  );

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
                },
              ),
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
  readonly schema: ColumnSchema;
  readonly visibleColumns: ReadonlyArray<string>;
  readonly onAddColumn: (field: string) => void;
  readonly datasource: DataSource;

  // Optional add column control - defaults to true
  readonly canAdd?: boolean;

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
      canAdd = true,
      canRemove,
      onRemove,
      schema,
      visibleColumns,
      onAddColumn,
      datasource,
      removeLabel = 'Remove column',
      addLabel = 'Add column',
    } = attrs;

    return [
      canAdd &&
        m(AddColumnSchemaMenuItem, {
          label: addLabel,
          icon: Icons.AddColumnRight,
          schema,
          pathPrefix: '',
          existingColumns: visibleColumns,
          onSelect: onAddColumn,
          datasource,
        }),
      onRemove &&
        m(MenuItem, {
          label: removeLabel,
          disabled: !canRemove,
          icon: Icons.Remove,
          onclick: onRemove,
        }),
    ];
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
 * When the column type is unknown (undefined), all aggregates are allowed.
 */
export function getAggregateFunctionsForColumnType(
  columnType: 'text' | 'quantitative' | 'identifier' | undefined,
): AggregateFunction[] {
  // For unknown types (undefined), allow all aggregates since we can't
  // determine restrictions
  if (
    columnType === undefined ||
    columnType === 'quantitative' ||
    columnType === 'identifier'
  ) {
    return [...NUMERIC_AGGREGATE_FUNCTIONS, ...TEXT_SAFE_AGGREGATE_FUNCTIONS];
  }
  // For 'text' columns, only text-safe aggregates
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
 * Builds the aggregate-function menu items (SUM, MIN, MAX, ...) for a single
 * column, based on its type.
 */
function buildAggFuncItems(
  columnType: 'text' | 'quantitative' | 'identifier' | undefined,
  field: string,
  existingAggregates: readonly AggregateColumn[] | undefined,
  onSelect: (func: AggregateFunction, field: string) => void,
): m.Children[] {
  return getAggregateFunctionsForColumnType(columnType).map((func) => {
    const exists = isAggregateExists(existingAggregates, func, field);
    return m(MenuItem, {
      label: func,
      disabled: exists,
      onclick: exists ? undefined : () => onSelect(func, field),
    });
  });
}

interface AggregateSchemaMenuItemAttrs {
  // Label shown on this submenu's own menu item.
  readonly label: m.Children;
  // Optional icon shown alongside the label.
  readonly icon?: string;
  // The schema whose columns are listed in this submenu.
  readonly schema: ColumnSchema;
  // Path prefix for columns at this level (e.g. 'parent' or 'thread.process').
  readonly pathPrefix: string;
  // Callback invoked with the chosen aggregate function and full column path.
  readonly onSelect: (func: AggregateFunction, field: string) => void;
  // Existing aggregates, used to disable already-added function/column pairs.
  readonly existingAggregates: readonly AggregateColumn[] | undefined;
  // Extra items rendered before the column entries (e.g. the COUNT option at
  // the root of the menu).
  readonly leadingItems?: m.Children;
}

/**
 * A menu item that expands into a submenu of columns, each exposing the
 * aggregate functions valid for its type. Schema references recurse through
 * nested instances of this component, so each level is only built when its
 * submenu is opened - no depth limit needed.
 */
const AggregateSchemaMenuItem: m.Component<AggregateSchemaMenuItemAttrs> = {
  view({attrs}) {
    const {
      label,
      icon,
      schema,
      pathPrefix,
      onSelect,
      existingAggregates,
      leadingItems,
    } = attrs;
    const menuItems: m.Children[] = [];

    if (leadingItems !== undefined) {
      menuItems.push(leadingItems);
    }

    for (const [columnName, entry] of Object.entries(schema)) {
      const fullPath = pathPrefix ? `${pathPrefix}.${columnName}` : columnName;

      if ('parameterized' in entry) {
        // For parameterized columns, aggregate over the base column.
        const title =
          typeof entry.title === 'string' ? entry.title : columnName;
        menuItems.push(
          m(
            MenuItem,
            {label: `${title} (base)`},
            buildAggFuncItems(
              entry.filterType,
              fullPath,
              existingAggregates,
              onSelect,
            ),
          ),
        );
      } else if ('schema' in entry) {
        menuItems.push(
          m(AggregateSchemaMenuItem, {
            label: entry.title ?? columnName,
            schema: entry.schema,
            pathPrefix: fullPath,
            onSelect,
            existingAggregates,
          }),
        );
      } else {
        menuItems.push(
          m(
            MenuItem,
            {label: entry.title ?? columnName},
            buildAggFuncItems(
              entry.columnType,
              fullPath,
              existingAggregates,
              onSelect,
            ),
          ),
        );
      }
    }

    return m(
      MenuItem,
      {label, icon, disabled: menuItems.length === 0},
      menuItems,
    );
  },
};

interface AggregateMenuAttrs {
  readonly schema: ColumnSchema;
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
      onAddAggregate,
      existingAggregates,
      label = 'Add column',
    } = attrs;

    const countExists = isAggregateExists(
      existingAggregates,
      'COUNT',
      undefined,
    );

    return m(AggregateSchemaMenuItem, {
      label,
      icon: Icons.AddColumnRight,
      schema,
      pathPrefix: '',
      onSelect: (func, field) => onAddAggregate(func, field),
      existingAggregates,
      // COUNT option - doesn't need a field
      leadingItems: m(MenuItem, {
        label: 'COUNT',
        disabled: countExists,
        onclick: countExists
          ? undefined
          : () => onAddAggregate('COUNT', undefined),
      }),
    } satisfies AggregateSchemaMenuItemAttrs);
  }
}
