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
import {Button} from '../../../../widgets/button';
import {Checkbox} from '../../../../widgets/checkbox';
import {Chip} from '../../../../widgets/chip';
import {Intent} from '../../../../widgets/common';
import {Icon} from '../../../../widgets/icon';
import {Select} from '../../../../widgets/select';
import {TextInput} from '../../../../widgets/text_input';
import {SqlValue} from '../../../../trace_processor/query_result';
import {ColumnInfo} from '../column_info';
import protos from '../../../../protos';
import {Stack} from '../../../../widgets/stack';
import {Icons} from '../../../../base/semantic_icons';

interface FilterValue {
  readonly column: string;
  readonly op: '=' | '!=' | '<' | '<=' | '>' | '>=' | 'glob';
  readonly value: SqlValue;
  enabled?: boolean; // Default true - controls if filter is active
}

interface FilterNull {
  readonly column: string;
  readonly op: 'is null' | 'is not null';
  enabled?: boolean; // Default true - controls if filter is active
}

export type UIFilter = FilterValue | FilterNull;

/**
 * Represents an OR group of filters.
 * Multiple filters within a group are combined with OR.
 * Groups themselves are combined with AND at the top level.
 */
export interface FilterGroup {
  readonly id: string;
  readonly filters: UIFilter[];
  enabled?: boolean; // Default true - controls if entire group is active
}

/**
 * Attributes for the FilterOperation component.
 */
export interface FilterAttrs {
  readonly sourceCols: ColumnInfo[];
  readonly filters?: ReadonlyArray<UIFilter>;
  readonly filterOperator?: 'AND' | 'OR';
  readonly groups?: ReadonlyArray<FilterGroup>; // OR groups
  readonly onFiltersChanged?: (filters: ReadonlyArray<UIFilter>) => void;
  readonly onFilterOperatorChanged?: (operator: 'AND' | 'OR') => void;
  readonly onGroupsChanged?: (groups: ReadonlyArray<FilterGroup>) => void;
  readonly onchange?: () => void;
}

export class FilterOperation implements m.ClassComponent<FilterAttrs> {
  private error?: string;
  private uiFilters: Partial<UIFilter>[] = [];
  private editingFilter?: Partial<UIFilter>;
  private uiGroups: FilterGroup[] = [];
  private dragOverFilter?: Partial<UIFilter>; // Track which filter is being dragged over
  private draggedFilter?: Partial<UIFilter>; // Store the filter being dragged

  oncreate({attrs}: m.Vnode<FilterAttrs>) {
    this.uiFilters = [...(attrs.filters ?? [])];
    this.uiGroups = [...(attrs.groups ?? [])];
  }

  onbeforeupdate({attrs}: m.Vnode<FilterAttrs>) {
    // If we are not in editing mode, sync with the parent.
    if (this.editingFilter === undefined) {
      this.uiFilters = [...(attrs.filters ?? [])];
    }
    this.uiGroups = [...(attrs.groups ?? [])];
  }

  private setFilters(
    nextFilters: Partial<UIFilter>[],
    attrs: FilterAttrs,
    editing?: Partial<UIFilter>,
  ) {
    this.uiFilters = nextFilters;
    this.editingFilter = editing;

    // Only notify the parent of "stable" changes, i.e. when not editing.
    if (this.editingFilter === undefined) {
      attrs.onFiltersChanged?.(this.uiFilters.filter(isFilterDefinitionValid));
    }
    attrs.onchange?.();
    m.redraw();
  }

  private setGroups(nextGroups: FilterGroup[], attrs: FilterAttrs) {
    this.uiGroups = nextGroups;
    attrs.onGroupsChanged?.(this.uiGroups.filter(isGroupDefinitionValid));
    attrs.onchange?.();
    m.redraw();
  }

  private renderFilterChip(
    filter: Partial<UIFilter>,
    attrs: FilterAttrs,
  ): m.Child {
    const isComplete = isFilterDefinitionValid(filter);
    const isEnabled = filter.enabled !== false; // Default to true
    const label = isComplete
      ? `${filter.column} ${filter.op} ${'value' in filter ? filter.value : ''}`
      : 'New Filter';
    const isDragOver = this.dragOverFilter === filter;

    return m(
      'div',
      {
        className: `pf-filter-chip-draggable ${isDragOver ? 'pf-filter-chip-draggable--drag-over' : ''}`,
        style: {
          display: 'flex',
          alignItems: 'center',
          gap: '4px',
          opacity: isEnabled ? 1 : 0.5,
        },
        draggable: isComplete,
        ondragstart: isComplete
          ? (e: DragEvent) => {
              e.dataTransfer!.effectAllowed = 'move';
              // Store the filter reference instead of serializing it
              this.draggedFilter = filter;
              e.dataTransfer!.setData('text/plain', 'filter');
            }
          : undefined,
        ondragover: isComplete
          ? (e: DragEvent) => {
              e.preventDefault();
              e.stopPropagation();
              this.dragOverFilter = filter;
              m.redraw();
            }
          : undefined,
        ondragleave: () => {
          this.dragOverFilter = undefined;
          m.redraw();
        },
        ondrop: isComplete
          ? (e: DragEvent) => {
              e.preventDefault();
              e.stopPropagation();
              this.handleDropOnFilter(filter, attrs);
            }
          : undefined,
        ondragend: () => {
          this.draggedFilter = undefined;
          this.dragOverFilter = undefined;
          m.redraw();
        },
      },
      isComplete &&
        m(Icon, {
          icon: Icons.DragHandle,
          className: 'pf-filter-drag-handle',
          style: {cursor: 'grab', userSelect: 'none'},
        }),
      isComplete &&
        m(Checkbox, {
          checked: isEnabled,
          onchange: () => {
            // Find if filter is in a group
            const group = findFilterGroup(filter as UIFilter, this.uiGroups);
            if (group !== undefined) {
              const nextGroups = this.uiGroups.map((g) => {
                if (g !== group) return g;
                return {
                  ...g,
                  filters: g.filters?.map((f) =>
                    f === filter ? {...f, enabled: !isEnabled} : f,
                  ),
                };
              });
              this.setGroups(nextGroups, attrs);
            } else {
              const nextFilters = this.uiFilters.map((f) =>
                f === filter ? {...f, enabled: !isEnabled} : f,
              );
              this.setFilters(nextFilters, attrs);
            }
          },
        }),
      m(Chip, {
        label,
        rounded: true,
        removable: isComplete,
        intent: isComplete
          ? isEnabled
            ? Intent.Primary
            : Intent.None
          : Intent.None,
        onclick: () => {
          if (!isComplete) {
            // When we start editing a chip, we remove all other invalid
            // filters from the list.
            const nextFilters = this.uiFilters.filter(
              (f) => f === filter || isFilterDefinitionValid(f),
            );
            this.setFilters(nextFilters, attrs, filter);
          }
        },
        onRemove: isComplete
          ? () => {
              // Use shared utility to handle filter removal with group dissolution
              const result = removeFilterFromGroupsOrFilters(
                filter as UIFilter,
                this.uiFilters as UIFilter[],
                this.uiGroups,
              );
              this.uiFilters = result.filters;
              this.uiGroups = result.groups;
              this.setFilters(this.uiFilters, attrs);
              this.setGroups(this.uiGroups, attrs);
            }
          : undefined,
      }),
    );
  }

  private handleDropOnFilter(
    targetFilter: Partial<UIFilter>,
    attrs: FilterAttrs,
  ) {
    const draggedFilter = this.draggedFilter;

    if (!draggedFilter) {
      this.dragOverFilter = undefined;
      m.redraw();
      return;
    }

    // Don't do anything if dropping on itself
    if (draggedFilter === targetFilter) {
      this.dragOverFilter = undefined;
      m.redraw();
      return;
    }

    // Extract dragged filter from its current location using utility
    const extractResult = extractFilterFromLocation(
      draggedFilter as UIFilter,
      this.uiFilters as UIFilter[],
      this.uiGroups,
    );
    this.uiFilters = extractResult.filters;
    this.uiGroups = extractResult.groups;

    // Add to target's location
    const targetGroup = findFilterGroup(
      targetFilter as UIFilter,
      this.uiGroups,
    );
    if (targetGroup) {
      // Add to existing group using utility
      this.uiGroups = addFilterToGroup(
        draggedFilter as UIFilter,
        targetGroup,
        this.uiGroups,
      );
    } else {
      // Create new group with both filters using utility
      const newGroup = createFilterGroup([
        targetFilter as UIFilter,
        draggedFilter as UIFilter,
      ]);
      this.uiGroups.push(newGroup);
      // Remove target from main list
      this.uiFilters = this.uiFilters.filter((f) => f !== targetFilter);
    }

    this.setFilters(this.uiFilters, attrs);
    this.setGroups(this.uiGroups, attrs);
    this.dragOverFilter = undefined;
    m.redraw();
  }

  private handleDropOutsideGroup(e: DragEvent, attrs: FilterAttrs) {
    e.preventDefault();
    const draggedFilter = this.draggedFilter;

    if (!draggedFilter) {
      this.dragOverFilter = undefined;
      m.redraw();
      return;
    }

    // Check if filter is in a group
    const sourceGroup = findFilterGroup(
      draggedFilter as UIFilter,
      this.uiGroups,
    );
    if (sourceGroup) {
      // Extract from group and add to main list using utility
      const result = extractFilterFromLocation(
        draggedFilter as UIFilter,
        this.uiFilters as UIFilter[],
        this.uiGroups,
      );

      this.uiFilters = [...result.filters, draggedFilter as UIFilter];
      this.uiGroups = result.groups;
      this.setFilters(this.uiFilters, attrs);
      this.setGroups(this.uiGroups, attrs);
    }
    // If it wasn't in a group, it's already in the main list, so nothing to do

    this.dragOverFilter = undefined;
    m.redraw();
  }

  private renderOrGroup(group: FilterGroup, attrs: FilterAttrs): m.Child {
    const isEnabled = group.enabled !== false;
    const filters = group.filters;

    return m(
      '.pf-or-group-container',
      {
        className: !isEnabled ? 'pf-or-group-container--disabled' : '',
      },
      m('.pf-or-group-label', 'OR'),
      m(
        '.pf-or-group-content',
        filters.map((filter) => this.renderFilterChip(filter, attrs)),
      ),
      m(
        '.pf-or-group-controls',
        m(Checkbox, {
          checked: isEnabled,
          title: isEnabled ? 'Disable group' : 'Enable group',
          onchange: () => {
            const nextGroups = this.uiGroups.map((g) =>
              g === group ? {...g, enabled: !isEnabled} : g,
            );
            this.setGroups(nextGroups, attrs);
          },
        }),
      ),
    );
  }

  view({attrs}: m.CVnode<FilterAttrs>) {
    const {sourceCols} = attrs;

    const editor =
      this.editingFilter === undefined
        ? undefined
        : m(FilterEditor, {
            filter: this.editingFilter,
            sourceCols,
            onUpdate: (newFilter) => {
              const index = this.uiFilters.indexOf(this.editingFilter!);
              const nextFilters = this.uiFilters.map((f, i) =>
                i === index ? newFilter : f,
              );
              this.setFilters(nextFilters, attrs, newFilter);
            },
            onRemove: () => {
              const nextFilters = this.uiFilters.filter(
                (f) => f !== this.editingFilter,
              );
              this.setFilters(nextFilters, attrs, undefined);
            },
            onDone: () => {
              this.setFilters(this.uiFilters, attrs, undefined);
            },
          });

    // Build a flat list of all items (filters and groups) to render
    const items: Array<
      | {type: 'filter'; filter: Partial<UIFilter>}
      | {type: 'group'; group: FilterGroup}
    > = [];

    // Add individual filters
    this.uiFilters.forEach((filter) => {
      items.push({type: 'filter', filter});
    });

    // Add groups
    this.uiGroups.forEach((group) => {
      items.push({type: 'group', group});
    });

    return m('.pf-exp-query-operations', [
      // Show hint when there are at least 2 complete filters (draggable items)
      // Count filters in main list plus filters in all groups
      (() => {
        const mainFiltersCount = this.uiFilters.filter(
          isFilterDefinitionValid,
        ).length;
        const groupFiltersCount = this.uiGroups.reduce(
          (sum, g) => sum + g.filters.length,
          0,
        );
        const totalFilters = mainFiltersCount + groupFiltersCount;
        const hasGroups = this.uiGroups.length > 0;

        return (
          totalFilters >= 2 &&
          m(
            '.pf-exp-filter-mode-help',
            hasGroups
              ? 'Drag filters onto each other to create OR groups. Drag filters outside groups to extract them.'
              : 'Tip: Drag filters onto each other to create OR groups.',
          )
        );
      })(),
      m(
        '.pf-exp-filters-header',
        m('label.pf-exp-filters-label', 'Type the filter'),
        m(TextInput, {
          placeholder: 'e.g. ts > 1000',
          onkeydown: (e: KeyboardEvent) => {
            const target = e.target as HTMLInputElement;
            if (e.key === 'Enter') {
              const text = target.value;
              if (text.length > 0) {
                const filter = fromString(text, sourceCols);
                if (!isFilterDefinitionValid(filter)) {
                  if (filter.column === undefined) {
                    this.error = `Column not found in "${text}"`;
                  } else if (filter.op === undefined) {
                    this.error = `Operator not found in "${text}"`;
                  } else {
                    this.error = `Filter value is missing in "${text}"`;
                  }
                  m.redraw();
                  return;
                }
                this.error = undefined;
                this.setFilters([...this.uiFilters, filter], attrs);
                target.value = '';
              }
            }
          },
        }),
      ),
      this.error && m('.pf-exp-error-message', this.error),
      // Filter list with drop zone for extracting from groups
      m(
        '.pf-filters-container',
        {
          ondragover: (e: DragEvent) => {
            e.preventDefault();
          },
          ondrop: (e: DragEvent) => {
            this.handleDropOutsideGroup(e, attrs);
          },
        },
        m(
          Stack,
          {orientation: 'vertical'},
          items.map((item) => {
            if (item.type === 'filter') {
              return this.renderFilterChip(item.filter, attrs);
            } else {
              return this.renderOrGroup(item.group, attrs);
            }
          }),
          m(Button, {
            icon: 'add',
            label: 'Add Filter',
            rounded: true,
            intent: Intent.Primary,
            onclick: () => {
              if (
                this.editingFilter !== undefined &&
                !isFilterDefinitionValid(this.editingFilter)
              ) {
                const nextFilters = this.uiFilters.filter(
                  (f) => f !== this.editingFilter,
                );
                this.setFilters(nextFilters, attrs, undefined);
              } else {
                const newFilter: Partial<UIFilter> = {};
                const nextFilters = [...this.uiFilters, newFilter];
                this.setFilters(nextFilters, attrs, newFilter);
              }
            },
          }),
        ),
      ),
      editor && m('.pf-exp-editor-box', editor),
    ]);
  }
}

interface FilterEditorAttrs {
  readonly filter: Partial<UIFilter>;
  readonly sourceCols: ColumnInfo[];
  readonly onUpdate: (filter: Partial<UIFilter>) => void;
  readonly onRemove: () => void;
  readonly onDone: () => void;
}

// A component which allows the user to edit a single filter.
class FilterEditor implements m.ClassComponent<FilterEditorAttrs> {
  view({attrs}: m.CVnode<FilterEditorAttrs>): m.Children {
    const {filter, sourceCols, onUpdate, onRemove, onDone} = attrs;

    const {column, op} = filter;
    const opObject = ALL_FILTER_OPS.find((o) => o.displayName === op);
    const valueRequired = isValueRequired(opObject);
    const isValid = isFilterDefinitionValid(filter);
    const colOptions = sourceCols
      .filter((c) => c.checked)
      .map(({name}) => {
        return m('option', {value: name, selected: name === column}, name);
      });

    const opOptions = ALL_FILTER_OPS.map((op) => {
      return m(
        'option',
        {
          value: op.key,
          selected: op.displayName === filter.op,
        },
        op.displayName,
      );
    });

    return m(
      '.pf-exp-editor',
      {className: isValid ? 'is-valid' : 'is-invalid'},
      [
        m(
          Select,
          {
            onchange: (e: Event) => {
              const target = e.target as HTMLSelectElement;
              onUpdate({...filter, column: target.value});
            },
          },
          m(
            'option',
            {disabled: true, selected: column === undefined},
            'Column',
          ),
          colOptions,
        ),
        m(
          Select,
          {
            onchange: (e: Event) => {
              const target = e.target as HTMLSelectElement;
              const newOp = ALL_FILTER_OPS.find(
                (op) => op.key === target.value,
              );
              const newFilter: Partial<UIFilter> = {
                ...filter,
                op: newOp?.displayName as UIFilter['op'],
              };
              if (newOp && !isValueRequired(newOp)) {
                delete (newFilter as {value?: SqlValue}).value;
              }
              onUpdate(newFilter);
            },
          },
          m('option', {disabled: true, selected: op === undefined}, 'Operator'),
          opOptions,
        ),
        valueRequired &&
          m(TextInput, {
            placeholder: 'Value',
            value: 'value' in filter ? String(filter.value) : '',
            oninput: (e: Event) => {
              const target = e.target as HTMLInputElement;
              const value = parseFilterValue(target.value);
              if (value !== undefined) {
                onUpdate({...filter, value} as Partial<UIFilter>);
              } else {
                onUpdate(filter);
              }
            },
          }),
        m(Button, {
          className: 'pf-exp-delete-button',
          icon: 'delete',
          onclick: onRemove,
        }),
        m(Button, {
          label: 'Done',
          className: 'is-primary',
          disabled: !isValid,
          onclick: onDone,
        }),
      ],
    );
  }
}

/**
 * Check if a work-in-progress filter is valid and can be converted to a
 * proper Filter.
 * @param filter The filter to check.
 * @returns True if the filter is valid.
 */
export function isFilterDefinitionValid(
  filter: Partial<UIFilter>,
): filter is UIFilter {
  const {column, op} = filter;

  if (column === undefined || op === undefined) {
    return false;
  }

  const opObject = ALL_FILTER_OPS.find((o) => o.displayName === op);

  if (opObject === undefined) {
    return false;
  }

  if (isValueRequired(opObject)) {
    if (!('value' in filter) || filter.value === undefined) {
      return false;
    }
  }

  return true;
}

/**
 * Check if a work-in-progress filter group is valid.
 * @param group The group to check.
 * @returns True if the group is valid.
 */
function isGroupDefinitionValid(
  group: Partial<FilterGroup>,
): group is FilterGroup {
  if (!group.id || !group.filters || group.filters.length === 0) {
    return false;
  }
  // All filters in the group must be valid
  return group.filters.every(isFilterDefinitionValid);
}

// Tries to parse a filter from a raw string. This is a best-effort parser
// for simple filters and does not support complex values with spaces or quotes.
// TODO(mayzner): Improve this parser to handle more complex cases, such as
// quoted strings, escaped characters, or operators within values.
function fromString(text: string, sourceCols: ColumnInfo[]): Partial<UIFilter> {
  // Sort operators by length descending to match "is not null" before "is
  // null".
  const ops = ALL_FILTER_OPS.slice().sort(
    (a, b) => b.displayName.length - a.displayName.length,
  );

  const opRegex = ops
    .map((op) => op.displayName.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'))
    .join('|');

  // A regex to capture the column, operator and value.
  // The value can be a quoted string or a single word.
  const regex = new RegExp(
    `^(\\S+)\\s+(${opRegex})(?:\\s+(".*?"|'.*?'|\\S+))?$`,
    'i',
  );
  const match = text.trim().match(regex);

  if (!match) {
    // If regex doesn't match, maybe it's just a column name.
    const col = sourceCols.find(
      (c) => c.name.toLowerCase() === text.trim().toLowerCase(),
    );
    if (col) {
      return {column: col.name};
    }
    return {};
  }

  const [, colName, opName, valueText] = match;

  const col = sourceCols.find(
    (c) => c.name.toLowerCase() === colName.toLowerCase(),
  );
  if (col === undefined) {
    return {};
  }

  // Find the exact operator object. We need to do a case-insensitive search.
  const op = ALL_FILTER_OPS.find(
    (o) => o.displayName.toLowerCase() === opName.toLowerCase(),
  );

  if (op === undefined) {
    throw new Error('Internal error: operator not found despite regex match');
  }

  const value = isValueRequired(op)
    ? parseFilterValue(valueText || '')
    : undefined;

  if (isValueRequired(op) && value === undefined) {
    // Value is required but not found or empty
    return {
      column: col.name,
      op: op.displayName as UIFilter['op'],
    };
  }

  const result: Partial<UIFilter> = {
    column: col.name,
    op: op.displayName as UIFilter['op'],
  };

  if (value !== undefined) {
    (result as {value: SqlValue}).value = value;
  }

  return result;
}

function op(
  key: string,
  displayName: string,
  proto: protos.PerfettoSqlStructuredQuery.Filter.Operator,
): FilterOp {
  return {
    key,
    displayName,
    proto,
  };
}

/**
 * A "Filter Operation" - i.e. "equals", "less than", "glob", etc.
 * This is a plain object which represents the properties of a filter
 * operation.
 */
export interface FilterOp {
  readonly key: string;
  readonly displayName: string;
  readonly proto: protos.PerfettoSqlStructuredQuery.Filter.Operator;
}

function isValueRequired(op?: FilterOp): boolean {
  return op !== undefined && op.key !== 'IS_NULL' && op.key !== 'IS_NOT_NULL';
}

// Parses a comma-separated string of values into an array of strings or
// numbers.
// If all values can be parsed as numbers, it returns a number array.
// Otherwise, it returns a string array.
function parseFilterValue(text: string): SqlValue | undefined {
  const value = text.trim();
  if (value === '') return undefined;

  // If the value is quoted, remove the quotes.
  if (
    (value.startsWith('"') && value.endsWith('"')) ||
    (value.startsWith("'") && value.endsWith("'"))
  ) {
    return value.slice(1, -1);
  }

  if (value !== '' && !isNaN(Number(value))) {
    return Number(value);
  } else {
    return value;
  }
}

/**
 * All available filter operations.
 */
export const ALL_FILTER_OPS: FilterOp[] = [
  op('EQUAL', '=', protos.PerfettoSqlStructuredQuery.Filter.Operator.EQUAL),
  op(
    'NOT_EQUAL',
    '!=',
    protos.PerfettoSqlStructuredQuery.Filter.Operator.NOT_EQUAL,
  ),
  op(
    'LESS_THAN',
    '<',
    protos.PerfettoSqlStructuredQuery.Filter.Operator.LESS_THAN,
  ),
  op(
    'LESS_THAN_EQUAL',
    '<=',
    protos.PerfettoSqlStructuredQuery.Filter.Operator.LESS_THAN_EQUAL,
  ),
  op(
    'GREATER_THAN',
    '>',
    protos.PerfettoSqlStructuredQuery.Filter.Operator.GREATER_THAN,
  ),
  op(
    'GREATER_THAN_EQUAL',
    '>=',
    protos.PerfettoSqlStructuredQuery.Filter.Operator.GREATER_THAN_EQUAL,
  ),
  op(
    'IS_NULL',
    'is null',
    protos.PerfettoSqlStructuredQuery.Filter.Operator.IS_NULL,
  ),
  op(
    'IS_NOT_NULL',
    'is not null',
    protos.PerfettoSqlStructuredQuery.Filter.Operator.IS_NOT_NULL,
  ),
  op('GLOB', 'glob', protos.PerfettoSqlStructuredQuery.Filter.Operator.GLOB),
];

// ============================================================================
// Filter Group Management Utilities
// ============================================================================

let groupCounter = 0;
function nextGroupId(): string {
  return `group_${groupCounter++}`;
}

/**
 * Result of a filter group operation.
 * @internal
 */
export interface GroupOperationResult {
  filters: UIFilter[];
  groups: FilterGroup[];
}

/**
 * Finds which group contains the given filter.
 * @internal
 */
export function findFilterGroup(
  filter: UIFilter,
  groups: FilterGroup[],
): FilterGroup | undefined {
  return groups.find((g) => g.filters?.includes(filter));
}

/**
 * Removes a filter from a specific group. If the group ends up with fewer
 * than 2 filters, it's dissolved and any remaining filter is moved to the
 * main filters list.
 * @internal
 */
export function removeFilterFromGroup(
  filter: UIFilter,
  group: FilterGroup,
  filters: UIFilter[],
  groups: FilterGroup[],
): GroupOperationResult {
  const newGroupFilters = group.filters.filter((f) => f !== filter);

  // If group now has < 2 filters, dissolve it
  if (newGroupFilters.length < 2) {
    let updatedFilters = [...filters];

    // Move remaining filter back to main list BEFORE removing group
    if (newGroupFilters.length === 1) {
      updatedFilters = [...updatedFilters, newGroupFilters[0]];
    }

    // Remove the group
    const updatedGroups = groups.filter((g) => g !== group);

    return {
      filters: updatedFilters,
      groups: updatedGroups,
    };
  }

  // Update the group with new filters (create new group object)
  const updatedGroups = groups.map((g) =>
    g === group ? {...g, filters: newGroupFilters} : g,
  );

  return {
    filters,
    groups: updatedGroups,
  };
}

/**
 * Removes a filter from whichever group contains it, or from the main
 * filters list if it's not in a group.
 * @internal
 */
export function removeFilterFromGroupsOrFilters(
  filter: UIFilter,
  filters: UIFilter[],
  groups: FilterGroup[],
): GroupOperationResult {
  // Check if filter is in a group
  const group = findFilterGroup(filter, groups);

  if (group) {
    // Remove from group (and potentially dissolve it)
    return removeFilterFromGroup(filter, group, filters, groups);
  }

  // Remove from main filters list
  return {
    filters: filters.filter((f) => f !== filter),
    groups,
  };
}

/**
 * Removes a filter from its current location (group or main list).
 * Does NOT dissolve the group if it becomes a single-filter group - just
 * removes the filter. Use this for drag operations where you'll be adding
 * the filter elsewhere.
 * @internal
 */
export function extractFilterFromLocation(
  filter: UIFilter,
  filters: UIFilter[],
  groups: FilterGroup[],
): GroupOperationResult {
  const group = findFilterGroup(filter, groups);

  if (group) {
    const newGroupFilters = group.filters.filter((f) => f !== filter);

    // If group now has < 2 filters, dissolve it
    if (newGroupFilters.length < 2) {
      let updatedFilters = [...filters];

      // Move remaining filter back to main list
      if (newGroupFilters.length === 1) {
        updatedFilters = [...updatedFilters, newGroupFilters[0]];
      }

      // Remove the group
      const updatedGroups = groups.filter((g) => g !== group);

      return {
        filters: updatedFilters,
        groups: updatedGroups,
      };
    }

    // Update the group with new filters
    const updatedGroups = groups.map((g) =>
      g === group ? {...g, filters: newGroupFilters} : g,
    );

    return {
      filters,
      groups: updatedGroups,
    };
  }

  // Remove from main list
  return {
    filters: filters.filter((f) => f !== filter),
    groups,
  };
}

/**
 * Adds a filter to an existing group.
 * @internal
 */
export function addFilterToGroup(
  filter: UIFilter,
  group: FilterGroup,
  groups: FilterGroup[],
): FilterGroup[] {
  return groups.map((g) =>
    g === group ? {...g, filters: [...g.filters, filter]} : g,
  );
}

/**
 * Creates a new OR group with the given filters.
 * @internal
 */
export function createFilterGroup(filters: UIFilter[]): FilterGroup {
  return {
    id: nextGroupId(),
    filters,
    enabled: true,
  };
}

// ============================================================================
// Proto Generation
// ============================================================================

export function createFiltersProto(
  filters: UIFilter[] | undefined,
  sourceCols: ColumnInfo[],
): protos.PerfettoSqlStructuredQuery.Filter[] | undefined {
  if (filters === undefined || filters.length === 0) {
    return undefined;
  }

  // Filter out disabled filters (enabled defaults to true if not set)
  const enabledFilters = filters.filter((f) => f.enabled !== false);
  if (enabledFilters.length === 0) {
    return undefined;
  }

  const protoFilters: protos.PerfettoSqlStructuredQuery.Filter[] =
    enabledFilters.map(
      (f: UIFilter): protos.PerfettoSqlStructuredQuery.Filter => {
        const result = new protos.PerfettoSqlStructuredQuery.Filter();
        result.columnName = f.column;

        const op = ALL_FILTER_OPS.find((o) => o.displayName === f.op);
        if (op === undefined) {
          // Should be handled by validation before this.
          throw new Error(`Unknown filter operator: ${f.op}`);
        }
        result.op = op.proto;

        if ('value' in f) {
          const value = f.value;
          const col = sourceCols.find((c) => c.name === f.column);
          if (typeof value === 'string') {
            result.stringRhs = [value];
          } else if (typeof value === 'number' || typeof value === 'bigint') {
            if (col && (col.type === 'long' || col.type === 'int')) {
              result.int64Rhs = [Number(value)];
            } else {
              result.doubleRhs = [Number(value)];
            }
          }
          // Not handling Uint8Array here. The original FilterToProto also didn't seem to.
        }
        return result;
      },
    );
  return protoFilters;
}

export function createExperimentalFiltersProto(
  filters: UIFilter[] | undefined,
  sourceCols: ColumnInfo[],
  operator?: 'AND' | 'OR',
  groups?: FilterGroup[],
): protos.PerfettoSqlStructuredQuery.ExperimentalFilterGroup | undefined {
  // If no filters and no groups, return undefined
  if (
    (filters === undefined || filters.length === 0) &&
    (groups === undefined || groups.length === 0)
  ) {
    return undefined;
  }

  // If we have groups, create a nested structure
  if (groups && groups.length > 0) {
    // Filter out disabled groups
    const enabledGroups = groups.filter((g) => g.enabled !== false);

    // Create the root AND group
    const rootGroup =
      new protos.PerfettoSqlStructuredQuery.ExperimentalFilterGroup();
    rootGroup.op =
      protos.PerfettoSqlStructuredQuery.ExperimentalFilterGroup.Operator.AND;

    // Add individual filters at the top level
    if (filters && filters.length > 0) {
      const protoFilters = createFiltersProto(filters, sourceCols);
      if (protoFilters) {
        rootGroup.filters = protoFilters;
      }
    }

    // Add OR groups as nested groups
    rootGroup.groups = enabledGroups
      .map((group) => {
        const groupProtoFilters = createFiltersProto(group.filters, sourceCols);
        if (!groupProtoFilters || groupProtoFilters.length === 0) {
          return undefined;
        }

        const orGroup =
          new protos.PerfettoSqlStructuredQuery.ExperimentalFilterGroup();
        orGroup.op =
          protos.PerfettoSqlStructuredQuery.ExperimentalFilterGroup.Operator.OR;
        orGroup.filters = groupProtoFilters;
        return orGroup;
      })
      .filter(
        (g) => g !== undefined,
      ) as protos.PerfettoSqlStructuredQuery.ExperimentalFilterGroup[];

    // If we have no filters and no valid groups, return undefined
    if (
      (rootGroup.filters === undefined || rootGroup.filters.length === 0) &&
      rootGroup.groups.length === 0
    ) {
      return undefined;
    }

    return rootGroup;
  }

  // Legacy path: no groups, just filters with a single operator
  if (filters === undefined || filters.length === 0) {
    return undefined;
  }

  const protoFilters = createFiltersProto(filters, sourceCols);
  if (!protoFilters) {
    return undefined;
  }

  const filterGroup =
    new protos.PerfettoSqlStructuredQuery.ExperimentalFilterGroup();

  // Use the provided operator, defaulting to AND for backward compatibility
  const op = operator ?? 'AND';
  filterGroup.op =
    op === 'OR'
      ? protos.PerfettoSqlStructuredQuery.ExperimentalFilterGroup.Operator.OR
      : protos.PerfettoSqlStructuredQuery.ExperimentalFilterGroup.Operator.AND;

  filterGroup.filters = protoFilters;

  return filterGroup;
}

/**
 * Helper to render FilterOperation with standard wiring for any node.
 * This avoids repeating the same pattern in every node type.
 */
export function renderFilterOperation(
  filters: UIFilter[] | undefined,
  filterOperator: 'AND' | 'OR' | undefined,
  sourceCols: ColumnInfo[],
  onFiltersChanged: (filters: ReadonlyArray<UIFilter>) => void,
  onFilterOperatorChanged: (operator: 'AND' | 'OR') => void,
  groups?: FilterGroup[],
  onGroupsChanged?: (groups: ReadonlyArray<FilterGroup>) => void,
): m.Child {
  return m(FilterOperation, {
    filters,
    filterOperator,
    sourceCols,
    onFiltersChanged,
    onFilterOperatorChanged,
    groups,
    onGroupsChanged,
  });
}

/**
 * Helper to format a single filter as a readable string.
 */
function formatSingleFilter(filter: UIFilter): string {
  if ('value' in filter) {
    // FilterValue
    const valueStr =
      typeof filter.value === 'string'
        ? `"${filter.value}"`
        : String(filter.value);
    return `${filter.column} ${filter.op} ${valueStr}`;
  } else {
    // FilterNull
    return `${filter.column} ${filter.op}`;
  }
}

/**
 * Helper to create a toggle callback for filter enable/disable in nodeDetails.
 */
function createFilterToggleCallback(state: {
  filters?: UIFilter[];
  onchange?: () => void;
}): (filter: UIFilter) => void {
  return (filter: UIFilter) => {
    if (state.filters) {
      state.filters = state.filters.map((f) =>
        f === filter ? {...f, enabled: f.enabled !== false ? false : true} : f,
      );
      state.onchange?.();
    }
  };
}

/**
 * Helper to format filter details with groups for nodeDetails display.
 * Shows OR groups with visual boundaries and individual filters.
 */
function formatFilterDetailsWithGroups(
  filters: UIFilter[] | undefined,
  groups: FilterGroup[] | undefined,
  state?: {filters?: UIFilter[]; groups?: FilterGroup[]; onchange?: () => void},
  onRemove?: (filter: UIFilter) => void,
  compact?: boolean,
): m.Child {
  const onFilterToggle = state ? createFilterToggleCallback(state) : undefined;
  const onGroupToggle =
    state && state.groups
      ? (group: FilterGroup) => {
          state.groups = state.groups?.map((g) =>
            g === group
              ? {...g, enabled: g.enabled !== false ? false : true}
              : g,
          );
          state.onchange?.();
        }
      : undefined;

  // Helper to render a filter chip
  const renderFilterChip = (filter: UIFilter) => {
    const isEnabled = filter.enabled !== false;
    const label = formatSingleFilter(filter);
    const classNames = [
      'pf-filter-chip-wrapper',
      !isEnabled && 'pf-filter-chip-wrapper--disabled',
      compact && 'pf-filter-chip-wrapper--compact',
    ]
      .filter(Boolean)
      .join(' ');

    return m(
      'span',
      {
        className: classNames,
      },
      m(Chip, {
        label,
        rounded: true,
        removable: !!onRemove,
        intent: isEnabled ? Intent.Primary : Intent.None,
        onclick: onFilterToggle ? () => onFilterToggle(filter) : undefined,
        onRemove: onRemove ? () => onRemove(filter) : undefined,
      }),
    );
  };

  // Helper to render an OR group
  const renderOrGroup = (group: FilterGroup) => {
    const isEnabled = group.enabled !== false;
    const contentClass = compact
      ? '.pf-or-group-content.pf-or-group-content--compact'
      : '.pf-or-group-content';

    return m(
      '.pf-or-group-container',
      {
        className: !isEnabled ? 'pf-or-group-container--disabled' : '',
      },
      m('.pf-or-group-label', 'OR'),
      m(
        contentClass,
        group.filters.map((filter) => renderFilterChip(filter)),
      ),
      // Only show checkbox if we have interactivity
      onGroupToggle &&
        m(
          '.pf-or-group-controls',
          m(Checkbox, {
            checked: isEnabled,
            title: isEnabled ? 'Disable group' : 'Enable group',
            onchange: () => onGroupToggle(group),
          }),
        ),
    );
  };

  const filterChipsClass = compact
    ? '.pf-filter-chips.pf-filter-chips--compact'
    : '.pf-filter-chips';

  return m(
    '.pf-filter-container.pf-filter-container--with-groups',
    // Show individual filters
    filters && filters.length > 0
      ? m(
          filterChipsClass,
          filters.map((filter) => renderFilterChip(filter)),
        )
      : undefined,
    // Show OR groups
    groups && groups.length > 0
      ? groups.map((group) => renderOrGroup(group))
      : undefined,
  );
}

/**
 * Helper to format filter details for nodeDetails display.
 * Returns interactive chips that can be clicked to toggle enabled/disabled state.
 * When using OR operator, shows individual filter chips for visibility.
 *
 * Pass the node state to enable interactive toggling, or omit for read-only display.
 */
export function formatFilterDetails(
  filters: UIFilter[] | undefined,
  filterOperator: 'AND' | 'OR' | undefined,
  groups?: FilterGroup[],
  state?: {filters?: UIFilter[]; groups?: FilterGroup[]; onchange?: () => void},
  onRemove?: (filter: UIFilter) => void,
  compact?: boolean,
): m.Child | undefined {
  const hasFilters = filters && filters.length > 0;
  const hasGroups = groups && groups.length > 0;

  if (!hasFilters && !hasGroups) {
    return undefined;
  }

  // Create default onRemove handler if state is provided but onRemove is not
  const effectiveOnRemove =
    onRemove ??
    (state
      ? (filter: UIFilter) => {
          const result = removeFilterFromGroupsOrFilters(
            filter,
            state.filters ?? [],
            state.groups ?? [],
          );
          state.filters = result.filters;
          state.groups = result.groups;
          state.onchange?.();
        }
      : undefined);

  // If we have groups, show a different layout
  if (hasGroups) {
    return formatFilterDetailsWithGroups(
      filters,
      groups,
      state,
      effectiveOnRemove,
      compact,
    );
  }

  // Legacy single-operator display
  if (!filters || filters.length === 0) {
    return undefined;
  }

  const count = filters.length;
  const enabledCount = filters.filter((f) => f.enabled !== false).length;
  const operator = filterOperator ?? 'AND';
  const onFilterToggle = state ? createFilterToggleCallback(state) : undefined;

  // Helper to render a filter chip
  const renderFilterChip = (filter: UIFilter) => {
    const isEnabled = filter.enabled !== false;
    const label = formatSingleFilter(filter);
    const classNames = [
      'pf-filter-chip-wrapper',
      !isEnabled && 'pf-filter-chip-wrapper--disabled',
      compact && 'pf-filter-chip-wrapper--compact',
    ]
      .filter(Boolean)
      .join(' ');

    return m(
      'span',
      {
        className: classNames,
      },
      m(Chip, {
        label,
        rounded: true,
        removable: !!effectiveOnRemove,
        intent: isEnabled ? Intent.Primary : Intent.None,
        onclick: onFilterToggle ? () => onFilterToggle(filter) : undefined,
        onRemove: effectiveOnRemove
          ? () => effectiveOnRemove(filter)
          : undefined,
      }),
    );
  };

  // For 4 or fewer filters
  if (count <= 4) {
    // For OR filters, show chips in visual boundary without summary text
    if (operator === 'OR') {
      const orChipsClass = compact
        ? '.pf-filter-chips.pf-filter-chips--no-count.pf-filter-chips--compact'
        : '.pf-filter-chips.pf-filter-chips--no-count';

      return m(
        '.pf-filter-or-group',
        m('.pf-filter-or-badge', 'OR'),
        m(
          orChipsClass,
          filters.map((filter) => renderFilterChip(filter)),
        ),
      );
    }
    // For AND filters, just show chips
    const filterChipsClass = compact
      ? '.pf-filter-chips.pf-filter-chips--compact'
      : '.pf-filter-chips';

    return m(
      '.pf-filter-container',
      m(
        filterChipsClass,
        filters.map((filter) => renderFilterChip(filter)),
      ),
    );
  }

  // For more than 4 filters with AND operator, show summary only
  if (operator === 'AND') {
    return m(
      '.pf-filter-container',
      m(
        '.pf-filter-and-header',
        m('.pf-filter-operator-badge', 'AND'),
        enabledCount === count
          ? `${count} filters`
          : `${enabledCount} of ${count} enabled`,
      ),
    );
  }

  // For more than 4 filters with OR operator, show summary only
  return m(
    '.pf-filter-container',
    m(
      '.pf-filter-and-header',
      m('.pf-filter-operator-badge', 'OR'),
      enabledCount === count
        ? `${count} filters`
        : `${enabledCount} of ${count} enabled`,
    ),
  );
}
