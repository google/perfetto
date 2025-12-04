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
import {SqlValue} from '../../../trace_processor/query_result';
import {EmptyState} from '../../../widgets/empty_state';
import {Form} from '../../../widgets/form';
import {Icon} from '../../../widgets/icon';
import {MenuDivider, MenuItem} from '../../../widgets/menu';
import {TextInput} from '../../../widgets/text_input';
import {ColumnDefinition, DataGridFilter, FilterType} from './common';

type OnFilterAdd = (filter: DataGridFilter) => void;

/**
 * Converts a string to a case-insensitive glob pattern.
 * For example: "abc" becomes "*[aA][bB][cC]*"
 */
function toCaseInsensitiveGlob(text: string): string {
  const pattern = text
    .split('')
    .map((char) => {
      const lower = char.toLowerCase();
      const upper = char.toUpperCase();
      // Only create character class for letters
      if (lower !== upper) {
        return `[${lower}${upper}]`;
      }
      // Non-letters remain as-is
      return char;
    })
    .join('');
  return `*${pattern}*`;
}

function formatDistinctValue(value: SqlValue): string {
  if (value === null) {
    return 'NULL';
  }
  if (value instanceof Uint8Array) {
    return `Blob (${value.length} bytes)`;
  }
  return String(value);
}

export function renderFilterSubmenuItems(
  column: ColumnDefinition,
  onFilterAdd: OnFilterAdd,
  distinctValues: readonly SqlValue[] | undefined,
  supportedFilters: ReadonlyArray<FilterType>,
  distinctValuesColumns: Set<string>,
): m.Children {
  return [
    // Null filters
    supportedFilters.includes('is not null') &&
      m(MenuItem, {
        label: 'Filter out nulls',
        onclick: () => {
          onFilterAdd({column: column.name, op: 'is not null'});
        },
      }),
    supportedFilters.includes('is null') &&
      m(MenuItem, {
        label: 'Only show nulls',
        onclick: () => {
          onFilterAdd({column: column.name, op: 'is null'});
        },
      }),
    m(MenuDivider),
    (column.distinctValues ?? true) && [
      supportedFilters.includes('in') &&
        m(
          MenuItem,
          {
            label: 'Equals to...',
            onChange: (isOpen) => {
              if (isOpen === true) {
                distinctValuesColumns.add(column.name);
              } else {
                distinctValuesColumns.delete(column.name);
              }
            },
          },
          m(DistinctValuesSubmenu, {
            columnName: column.name,
            distinctState: distinctValues,
            formatValue: formatDistinctValue,
            onApply: (selectedValues) => {
              onFilterAdd({
                column: column.name,
                op: 'in',
                value: Array.from(selectedValues),
              });
            },
          }),
        ),
      supportedFilters.includes('not in') &&
        m(
          MenuItem,
          {
            label: 'Not equals to...',
            onChange: (isOpen) => {
              if (isOpen === true) {
                distinctValuesColumns.add(column.name);
              } else {
                distinctValuesColumns.delete(column.name);
              }
            },
          },
          m(DistinctValuesSubmenu, {
            columnName: column.name,
            distinctState: distinctValues,
            formatValue: formatDistinctValue,
            onApply: (selectedValues) => {
              onFilterAdd({
                column: column.name,
                op: 'not in',
                value: Array.from(selectedValues),
              });
            },
          }),
        ),
    ],
    m(MenuDivider),
    // Free-text equals/not equals filters for columns without distinct values
    !(column.distinctValues ?? true) && [
      supportedFilters.includes('=') &&
        m(
          MenuItem,
          {
            label: 'Equals to...',
          },
          m(TextFilterSubmenu, {
            columnName: column.name,
            operator: '=',
            onApply: (value) => {
              onFilterAdd({
                column: column.name,
                op: '=',
                value,
              });
            },
          }),
        ),
      !(column.distinctValues ?? true) &&
        supportedFilters.includes('!=') &&
        m(
          MenuItem,
          {
            label: 'Not equals to...',
          },
          m(TextFilterSubmenu, {
            columnName: column.name,
            operator: '!=',
            onApply: (value) => {
              onFilterAdd({
                column: column.name,
                op: '!=',
                value,
              });
            },
          }),
        ),
    ],
    m(MenuDivider),
    // Numeric comparison filters (only for numeric columns)
    column.filterType === 'numeric' && [
      supportedFilters.includes('>') &&
        m(
          MenuItem,
          {
            label: 'Greater than...',
          },
          m(TextFilterSubmenu, {
            columnName: column.name,
            operator: '>',
            onApply: (value) => {
              onFilterAdd({
                column: column.name,
                op: '>',
                value,
              });
            },
          }),
        ),
      supportedFilters.includes('>=') &&
        m(
          MenuItem,
          {
            label: 'Greater than or equal...',
          },
          m(TextFilterSubmenu, {
            columnName: column.name,
            operator: '>=',
            onApply: (value) => {
              onFilterAdd({
                column: column.name,
                op: '>=',
                value,
              });
            },
          }),
        ),
      supportedFilters.includes('<') &&
        m(
          MenuItem,
          {
            label: 'Less than...',
          },
          m(TextFilterSubmenu, {
            columnName: column.name,
            operator: '<',
            onApply: (value) => {
              onFilterAdd({
                column: column.name,
                op: '<',
                value,
              });
            },
          }),
        ),
      supportedFilters.includes('<=') &&
        m(
          MenuItem,
          {
            label: 'Less than or equal...',
          },
          m(TextFilterSubmenu, {
            columnName: column.name,
            operator: '<=',
            onApply: (value) => {
              onFilterAdd({
                column: column.name,
                op: '<=',
                value,
              });
            },
          }),
        ),
    ],
    m(MenuDivider),
    // Text-based filters (only if filterType is not 'numeric')
    column.filterType !== 'numeric' && [
      supportedFilters.includes('glob') &&
        m(
          MenuItem,
          {
            label: 'Contains...',
          },
          m(TextFilterSubmenu, {
            columnName: column.name,
            operator: 'contains',
            onApply: (value) => {
              onFilterAdd({
                column: column.name,
                op: 'glob',
                value: toCaseInsensitiveGlob(String(value)),
              });
            },
          }),
        ),
      supportedFilters.includes('not glob') &&
        m(
          MenuItem,
          {
            label: 'Not contains...',
          },
          m(TextFilterSubmenu, {
            columnName: column.name,
            operator: 'not contains',
            onApply: (value) => {
              onFilterAdd({
                column: column.name,
                op: 'not glob',
                value: toCaseInsensitiveGlob(String(value)),
              });
            },
          }),
        ),
      supportedFilters.includes('glob') &&
        m(
          MenuItem,
          {
            label: 'Glob...',
          },
          m(TextFilterSubmenu, {
            columnName: column.name,
            operator: 'glob',
            onApply: (value) => {
              onFilterAdd({column: column.name, op: 'glob', value});
            },
          }),
        ),
      supportedFilters.includes('not glob') &&
        m(
          MenuItem,
          {
            label: 'Not glob...',
          },
          m(TextFilterSubmenu, {
            columnName: column.name,
            operator: 'not glob',
            onApply: (value) => {
              onFilterAdd({column: column.name, op: 'not glob', value});
            },
          }),
        ),
    ],
  ];
}

// Helper component to manage distinct values selection
interface DistinctValuesSubmenuAttrs {
  readonly columnName: string;
  readonly distinctState: ReadonlyArray<SqlValue> | undefined;
  readonly formatValue: (value: SqlValue) => string;
  readonly onApply: (selectedValues: Set<SqlValue>) => void;
}

class DistinctValuesSubmenu
  implements m.ClassComponent<DistinctValuesSubmenuAttrs>
{
  private selectedValues = new Set<SqlValue>();
  private searchQuery = '';
  private static readonly MAX_VISIBLE_ITEMS = 100;

  view({attrs}: m.Vnode<DistinctValuesSubmenuAttrs>) {
    const {distinctState, formatValue, onApply} = attrs;

    if (distinctState === undefined) {
      return m('.pf-distinct-values-menu', [
        m(MenuItem, {label: 'Loading...', disabled: true}),
      ]);
    }

    // Use fuzzy search to filter and get highlighted segments
    const fuzzyResults = (() => {
      if (this.searchQuery === '') {
        // No search - show all values without highlighting
        return distinctState.map((value) => ({
          value,
          segments: [{matching: false, value: formatValue(value)}],
        }));
      } else {
        // Fuzzy search with highlighting
        const finder = new FuzzyFinder(distinctState, (v) => formatValue(v));
        return finder.find(this.searchQuery).map((result) => ({
          value: result.item,
          segments: result.segments,
        }));
      }
    })();

    // Limit the number of items rendered
    const visibleResults = fuzzyResults.slice(
      0,
      DistinctValuesSubmenu.MAX_VISIBLE_ITEMS,
    );
    const remainingCount =
      fuzzyResults.length - DistinctValuesSubmenu.MAX_VISIBLE_ITEMS;

    return m('.pf-distinct-values-menu', [
      m(
        '.pf-distinct-values-menu__search',
        {
          onclick: (e: MouseEvent) => {
            // Prevent menu from closing when clicking search box
            e.stopPropagation();
          },
        },
        m(TextInput, {
          placeholder: 'Search...',
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
      m(
        '.pf-distinct-values-menu__list',
        fuzzyResults.length > 0
          ? [
              visibleResults.map((result) => {
                const isSelected = this.selectedValues.has(result.value);
                // Render highlighted label
                const labelContent = result.segments.map((segment) => {
                  if (segment.matching) {
                    return m('strong.pf-fuzzy-match', segment.value);
                  } else {
                    return segment.value;
                  }
                });

                // Render custom menu item with highlighted content
                return m(
                  'button.pf-menu-item',
                  {
                    onclick: () => {
                      if (isSelected) {
                        this.selectedValues.delete(result.value);
                      } else {
                        this.selectedValues.add(result.value);
                      }
                    },
                  },
                  m(Icon, {
                    className: 'pf-menu-item__left-icon',
                    icon: isSelected ? Icons.Checkbox : Icons.BlankCheckbox,
                  }),
                  m('.pf-menu-item__label', labelContent),
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
      m('.pf-distinct-values-menu__footer', [
        m(MenuItem, {
          label: 'Apply',
          icon: 'check',
          disabled: this.selectedValues.size === 0,
          onclick: () => {
            if (this.selectedValues.size > 0) {
              onApply(this.selectedValues);
              this.selectedValues.clear();
              this.searchQuery = '';
            }
          },
        }),
        m(MenuItem, {
          label: 'Clear selection',
          icon: 'close',
          disabled: this.selectedValues.size === 0,
          closePopupOnClick: false,
          onclick: () => {
            this.selectedValues.clear();
            m.redraw();
          },
        }),
      ]),
    ]);
  }
}

// Helper component for text-based filter input
interface TextFilterSubmenuAttrs {
  readonly columnName: string;
  readonly operator:
    | 'glob'
    | 'not glob'
    | 'contains'
    | 'not contains'
    | '='
    | '!='
    | '>'
    | '>='
    | '<'
    | '<=';
  readonly onApply: (value: string | number) => void;
}

class TextFilterSubmenu implements m.ClassComponent<TextFilterSubmenuAttrs> {
  private inputValue = '';

  view({attrs}: m.Vnode<TextFilterSubmenuAttrs>) {
    const {operator, onApply} = attrs;

    const placeholder = (() => {
      switch (operator) {
        case 'glob':
          return 'Enter glob pattern (e.g., *text*)...';
        case 'not glob':
          return 'Enter glob pattern to exclude...';
        case 'contains':
          return 'Enter text to include...';
        case 'not contains':
          return 'Enter text to exclude...';
        case '=':
          return 'Enter value to match...';
        case '!=':
          return 'Enter value to exclude...';
        case '>':
          return 'Enter number...';
        case '>=':
          return 'Enter number...';
        case '<':
          return 'Enter number...';
        case '<=':
          return 'Enter number...';
      }
    })();

    // Check if this is a numeric comparison operator
    const isNumericOperator = ['>', '>=', '<', '<='].includes(operator);

    const applyFilter = () => {
      if (this.inputValue.trim().length > 0) {
        let value: string | number = this.inputValue.trim();

        // For numeric operators, try to parse as number
        if (isNumericOperator) {
          const numValue = Number(value);
          if (!isNaN(numValue)) {
            value = numValue;
          }
        }

        onApply(value);
        this.inputValue = '';
      }
    };

    return m(
      Form,
      {
        className: 'pf-data-grid__text-filter-form',
        submitLabel: 'Add Filter',
        submitIcon: 'check',
        onSubmit: (e: Event) => {
          e.preventDefault();
          applyFilter();
        },
        validation: () => this.inputValue.trim().length > 0,
      },
      m(TextInput, {
        placeholder,
        value: this.inputValue,
        autofocus: true,
        oninput: (e: InputEvent) => {
          this.inputValue = (e.target as HTMLInputElement).value;
        },
      }),
    );
  }
}
