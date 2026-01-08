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
import {isEmptyVnodes} from '../../../base/mithril_utils';
import {Icons} from '../../../base/semantic_icons';
import {SqlValue} from '../../../trace_processor/query_result';
import {EmptyState} from '../../../widgets/empty_state';
import {Form} from '../../../widgets/form';
import {Icon} from '../../../widgets/icon';
import {MenuDivider, MenuItem} from '../../../widgets/menu';
import {TextInput} from '../../../widgets/text_input';
import {ColumnType} from './datagrid_schema';
import {FilterOpAndValue} from './model';

// Helper to convert search text to case-insensitive glob pattern
export function toCaseInsensitiveGlob(text: string): string {
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

// Helper component to manage distinct values selection
interface DistinctValuesSubmenuAttrs {
  readonly distinctValues: readonly SqlValue[] | undefined;
  readonly valueFormatter: (value: SqlValue) => string;
  readonly onApply: (selectedValues: Set<SqlValue>) => void;
}

export class DistinctValuesSubmenu
  implements m.ClassComponent<DistinctValuesSubmenuAttrs>
{
  private selectedValues = new Set<SqlValue>();
  private searchQuery = '';
  private static readonly MAX_VISIBLE_ITEMS = 100;

  view({attrs}: m.Vnode<DistinctValuesSubmenuAttrs>) {
    const {distinctValues, valueFormatter, onApply} = attrs;

    if (distinctValues === undefined) {
      return m('.pf-distinct-values-menu', [
        m(MenuItem, {label: 'Loading...', disabled: true}),
      ]);
    }

    // Use fuzzy search to filter and get highlighted segments
    const fuzzyResults = (() => {
      if (this.searchQuery === '') {
        // No search - show all values without highlighting
        return distinctValues.map((value) => ({
          value,
          segments: [{matching: false, value: valueFormatter(value)}],
        }));
      } else {
        // Fuzzy search with highlighting
        const finder = new FuzzyFinder(distinctValues, (v) =>
          valueFormatter(v),
        );
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
  readonly placeholder?: string;
  readonly inputType: 'text' | 'number';
  readonly onApply: (value: string | number) => void;
}

export class TextFilterSubmenu
  implements m.ClassComponent<TextFilterSubmenuAttrs>
{
  private inputValue = '';

  view({attrs}: m.Vnode<TextFilterSubmenuAttrs>) {
    const {placeholder = 'Enter value...', inputType, onApply} = attrs;

    const applyFilter = () => {
      if (this.inputValue.trim().length > 0) {
        let value: string | number = this.inputValue.trim();

        if (inputType === 'number') {
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
        onInput: (value) => {
          this.inputValue = value;
        },
      }),
    );
  }
}

export interface FilterMenuAttrs {
  readonly distinctValues: readonly SqlValue[] | undefined;
  readonly columnType: ColumnType | undefined;
  readonly structuredQueryCompatMode: boolean;
  readonly valueFormatter: (value: SqlValue) => string;
  readonly onFilterAdd: (filter: FilterOpAndValue) => void;
  readonly onRequestDistinctValues: () => void;
  readonly onDismissDistinctValues: () => void;
}

/**
 * Renders the complete filter menu group for a column header.
 * Returns the "Add filter..." menu item with all filter options as a submenu.
 */
export class FilterMenu implements m.ClassComponent<FilterMenuAttrs> {
  view({attrs}: m.Vnode<FilterMenuAttrs>): m.Children {
    const filterSubmenuItems = renderFilterMenuItems(attrs);

    if (isEmptyVnodes(filterSubmenuItems)) {
      return undefined;
    }

    return m(
      MenuItem,
      {label: 'Add filter', icon: Icons.Filter},
      filterSubmenuItems,
    );
  }
}

/**
 * Renders numeric comparison filter menu items (>, >=, <, <=).
 */
function renderNumericComparisonMenuItems(
  onFilterAdd: (filter: FilterOpAndValue) => void,
): m.ChildArray {
  return [
    m(
      MenuItem,
      {label: 'Greater than'},
      m(TextFilterSubmenu, {
        placeholder: 'Enter number...',
        inputType: 'number',
        onApply: (value) => onFilterAdd({op: '>', value}),
      }),
    ),
    m(
      MenuItem,
      {label: 'Greater than or equals'},
      m(TextFilterSubmenu, {
        placeholder: 'Enter number...',
        inputType: 'number',
        onApply: (value) => onFilterAdd({op: '>=', value}),
      }),
    ),
    m(
      MenuItem,
      {label: 'Less than'},
      m(TextFilterSubmenu, {
        placeholder: 'Enter number...',
        inputType: 'number',
        onApply: (value) => onFilterAdd({op: '<', value}),
      }),
    ),
    m(
      MenuItem,
      {label: 'Less than or equals'},
      m(TextFilterSubmenu, {
        placeholder: 'Enter number...',
        inputType: 'number',
        onApply: (value) => onFilterAdd({op: '<=', value}),
      }),
    ),
  ];
}

/**
 * Renders contains filter menu items (Contains, Not contains).
 */
function renderContainsFilterMenuItems(
  onFilterAdd: (filter: FilterOpAndValue) => void,
  includeNotContains: boolean,
): m.ChildArray {
  return [
    m(
      MenuItem,
      {label: 'Contains'},
      m(TextFilterSubmenu, {
        placeholder: 'Enter text to search...',
        inputType: 'text',
        onApply: (value) =>
          onFilterAdd({
            op: 'glob',
            value: toCaseInsensitiveGlob(String(value)),
          }),
      }),
    ),
    // Not contains - hidden in structuredQueryCompatMode
    includeNotContains &&
      m(
        MenuItem,
        {label: 'Not contains'},
        m(TextFilterSubmenu, {
          placeholder: 'Enter text to exclude...',
          inputType: 'text',
          onApply: (value) =>
            onFilterAdd({
              op: 'not glob',
              value: toCaseInsensitiveGlob(String(value)),
            }),
        }),
      ),
  ];
}

/**
 * Renders glob filter menu items (Glob, Not glob).
 */
function renderGlobFilterMenuItems(
  onFilterAdd: (filter: FilterOpAndValue) => void,
  includNotGlob: boolean,
): m.ChildArray {
  return [
    m(
      MenuItem,
      {label: 'Glob'},
      m(TextFilterSubmenu, {
        placeholder: 'Enter glob pattern (e.g., *text*)...',
        inputType: 'text',
        onApply: (value) => onFilterAdd({op: 'glob', value}),
      }),
    ),
    // Not glob - hidden in structuredQueryCompatMode
    includNotGlob &&
      m(
        MenuItem,
        {label: 'Not glob'},
        m(TextFilterSubmenu, {
          placeholder: 'Enter glob pattern to exclude...',
          inputType: 'text',
          onApply: (value) => onFilterAdd({op: 'not glob', value}),
        }),
      ),
  ];
}

/**
 * Renders free text equals filter menu items (Equals, Not equals) for quantitative columns.
 */
function renderFreeTextEqualsFilterMenuItems(
  onFilterAdd: (filter: FilterOpAndValue) => void,
): m.ChildArray {
  return [
    m(
      MenuItem,
      {label: 'Equals'},
      m(TextFilterSubmenu, {
        placeholder: 'Enter number to match...',
        inputType: 'number',
        onApply: (value) => onFilterAdd({op: '=', value}),
      }),
    ),
    m(
      MenuItem,
      {label: 'Not equals'},
      m(TextFilterSubmenu, {
        placeholder: 'Enter number to exclude...',
        inputType: 'number',
        onApply: (value) => onFilterAdd({op: '!=', value}),
      }),
    ),
  ];
}

/**
 * Renders null filter menu items (Is null, Is not null).
 */
function renderNullFilterMenuItems(
  onFilterAdd: (filter: FilterOpAndValue) => void,
): m.ChildArray {
  return [
    m(MenuItem, {
      label: 'Is null',
      onclick: () => {
        onFilterAdd({op: 'is null'});
      },
    }),
    m(MenuItem, {
      label: 'Is not null',
      onclick: () => {
        onFilterAdd({op: 'is not null'});
      },
    }),
  ];
}

/**
 * Renders distinct value picker menu items (Equals, Not equals).
 */
function renderDistinctValueFilterMenuItems(config: {
  readonly distinctValues: readonly SqlValue[] | undefined;
  readonly valueFormatter: (value: SqlValue) => string;
  readonly onFilterAdd: (filter: FilterOpAndValue) => void;
  readonly onRequestDistinctValues: () => void;
  readonly onDismissDistinctValues: () => void;
}): m.ChildArray {
  const {
    distinctValues,
    valueFormatter,
    onFilterAdd,
    onRequestDistinctValues,
    onDismissDistinctValues,
  } = config;

  return [
    m(
      MenuItem,
      {
        label: 'Equals',
        onChange: (isOpen) => {
          if (isOpen === true) {
            onRequestDistinctValues();
          } else {
            onDismissDistinctValues();
          }
        },
      },
      m(DistinctValuesSubmenu, {
        // Filter out null - use "is null" filter instead (SQL IN doesn't match NULL)
        distinctValues: distinctValues?.filter((v: SqlValue) => v !== null),
        valueFormatter,
        onApply: (selectedValues) => {
          onFilterAdd({
            op: 'in',
            value: Array.from(selectedValues),
          });
        },
      }),
    ),
    m(
      MenuItem,
      {
        label: 'Not equals',
        onChange: (isOpen) => {
          if (isOpen === true) {
            onRequestDistinctValues();
          } else {
            onDismissDistinctValues();
          }
        },
      },
      m(DistinctValuesSubmenu, {
        // Filter out null - use "is not null" filter instead (SQL NOT IN doesn't exclude NULL)
        distinctValues: distinctValues?.filter((v: SqlValue) => v !== null),
        valueFormatter,
        onApply: (selectedValues) => {
          onFilterAdd({
            op: 'not in',
            value: Array.from(selectedValues),
          });
        },
      }),
    ),
  ];
}

/**
 * Renders filter menu items for text columns.
 * Includes: distinct value picker (equals/not equals), contains, glob, null filters.
 */
function renderTextFilterMenuItems(config: FilterMenuAttrs): m.ChildArray {
  const {
    structuredQueryCompatMode,
    distinctValues,
    valueFormatter,
    onFilterAdd,
    onRequestDistinctValues,
    onDismissDistinctValues,
  } = config;

  return [
    renderDistinctValueFilterMenuItems({
      distinctValues,
      valueFormatter,
      onFilterAdd,
      onRequestDistinctValues,
      onDismissDistinctValues,
    }),
    m(MenuDivider),
    renderContainsFilterMenuItems(onFilterAdd, !structuredQueryCompatMode),
    renderGlobFilterMenuItems(onFilterAdd, !structuredQueryCompatMode),
    m(MenuDivider),
    renderNullFilterMenuItems(onFilterAdd),
  ];
}

/**
 * Renders filter menu items for quantitative columns.
 * Includes: free text equals/not equals, numeric comparisons, null filters.
 */
function renderQuantitativeFilterMenuItems(
  onFilterAdd: (filter: FilterOpAndValue) => void,
): m.ChildArray {
  return [
    renderFreeTextEqualsFilterMenuItems(onFilterAdd),
    renderNumericComparisonMenuItems(onFilterAdd),
    m(MenuDivider),
    renderNullFilterMenuItems(onFilterAdd),
  ];
}

/**
 * Renders filter menu items for identifier columns.
 * Includes: distinct value picker (equals/not equals), numeric comparisons, null filters.
 */
function renderIdentifierFilterMenuItems(
  config: FilterMenuAttrs,
): m.ChildArray {
  const {
    distinctValues,
    valueFormatter,
    onFilterAdd,
    onRequestDistinctValues,
    onDismissDistinctValues,
  } = config;

  return [
    renderDistinctValueFilterMenuItems({
      distinctValues,
      valueFormatter,
      onFilterAdd,
      onRequestDistinctValues,
      onDismissDistinctValues,
    }),
    renderNumericComparisonMenuItems(onFilterAdd),
    m(MenuDivider),
    renderNullFilterMenuItems(onFilterAdd),
  ];
}

/**
 * Renders filter menu items when columnType is undefined.
 * Shows all filter options except distinct value picker.
 */
function renderUnknownTypeFilterMenuItems(
  config: FilterMenuAttrs,
): m.ChildArray {
  const {structuredQueryCompatMode, onFilterAdd} = config;

  return [
    renderFreeTextEqualsFilterMenuItems(onFilterAdd),
    renderNumericComparisonMenuItems(onFilterAdd),
    m(MenuDivider),
    renderContainsFilterMenuItems(onFilterAdd, !structuredQueryCompatMode),
    renderGlobFilterMenuItems(onFilterAdd, !structuredQueryCompatMode),
    m(MenuDivider),
    renderNullFilterMenuItems(onFilterAdd),
  ];
}

/**
 * Renders the filter submenu items for a column header context menu.
 * Dispatches to type-specific renderers based on columnType.
 */
function renderFilterMenuItems(config: FilterMenuAttrs): m.ChildArray {
  switch (config.columnType) {
    case 'text':
      return renderTextFilterMenuItems(config);
    case 'quantitative':
      return renderQuantitativeFilterMenuItems(config.onFilterAdd);
    case 'identifier':
      return renderIdentifierFilterMenuItems(config);
    default:
      // When columnType is undefined, show all filters except distinct values
      return renderUnknownTypeFilterMenuItems(config);
  }
}
