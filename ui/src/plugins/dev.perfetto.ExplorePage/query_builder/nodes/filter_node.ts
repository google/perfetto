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
import {
  QueryNode,
  QueryNodeState,
  nextNodeId,
  NodeType,
} from '../../query_node';
import {ColumnInfo} from '../column_info';
import protos from '../../../../protos';
import {
  UIFilter,
  createExperimentalFiltersProto,
  showFilterEditModal,
  formatFilterDetails,
  isFilterDefinitionValid,
  parseFilterFromText,
} from '../operations/filter';
import {StructuredQueryBuilder} from '../structured_query_builder';
import {NodeIssues} from '../node_issues';
import {showModal} from '../../../../widgets/modal';
import {Editor} from '../../../../widgets/editor';
import {TextInput} from '../../../../widgets/text_input';
import {ListItem, EqualWidthRow} from '../widgets';
import {EmptyState} from '../../../../widgets/empty_state';
import {classNames} from '../../../../base/classnames';
import {NodeModifyAttrs} from '../node_explorer_types';
import {Button, ButtonVariant} from '../../../../widgets/button';

// Maximum length for truncated SQL display
const SQL_TRUNCATE_LENGTH = 50;

export interface FilterNodeState extends QueryNodeState {
  filters?: UIFilter[];
  filterOperator?: 'AND' | 'OR';
  filterMode?: 'structured' | 'freeform';
  sqlExpression?: string;
}

export class FilterNode implements QueryNode {
  readonly nodeId: string;
  readonly type = NodeType.kFilter;
  primaryInput?: QueryNode;
  secondaryInputs?: undefined; // FilterNode doesn't support secondary inputs
  nextNodes: QueryNode[];
  readonly state: FilterNodeState;

  constructor(state: FilterNodeState) {
    this.nodeId = nextNodeId();
    this.state = state;
    this.nextNodes = [];
  }

  get sourceCols(): ColumnInfo[] {
    return this.primaryInput?.finalCols ?? [];
  }

  get finalCols(): ColumnInfo[] {
    return this.sourceCols;
  }

  getTitle(): string {
    return 'Filter';
  }

  private setValidationError(message: string): void {
    if (!this.state.issues) {
      this.state.issues = new NodeIssues();
    }
    this.state.issues.queryError = new Error(message);
  }

  private handleFilterEdit(filter: UIFilter): void {
    // Check if there are any columns available
    if (this.sourceCols.length === 0) {
      showModal({
        title: 'Cannot edit filter',
        content: m(
          'div',
          m('p', 'No columns are available to filter on.'),
          m(
            'p',
            'Please select a table or add columns before editing filters.',
          ),
        ),
      });
      return;
    }

    showFilterEditModal(
      filter,
      this.sourceCols,
      (editedFilter) => {
        // Update filter in main filters array
        this.state.filters = (this.state.filters ?? []).map((f) =>
          f === filter ? editedFilter : f,
        );
        this.state.onchange?.();
        m.redraw();
      },
      () => {
        // Delete callback
        this.state.filters = (this.state.filters ?? []).filter(
          (f) => f !== filter,
        );
        this.state.onchange?.();
        m.redraw();
      },
    );
  }

  nodeDetails(): m.Child {
    this.validate();

    const mode = this.state.filterMode ?? 'structured';

    // Freeform SQL mode
    if (mode === 'freeform') {
      const sql = this.state.sqlExpression?.trim();
      if (!sql) {
        return m('.pf-filter-node-details', 'No filter clause');
      }

      if (sql.length < 200) {
        return m('.pf-filter-node-details', m('code', sql));
      } else {
        return m('.pf-filter-node-details', 'Filter clause applied');
      }
    }

    // Structured mode
    if (!this.state.filters || this.state.filters.length === 0) {
      return m('.pf-filter-node-details', 'No filters applied');
    }

    return formatFilterDetails(
      this.state.filters,
      this.state.filterOperator,
      this.state, // Pass state for interactive toggling and removal
      undefined, // onRemove - handled internally by formatFilterDetails
      true, // compact mode for smaller font
      (filter) => this.handleFilterEdit(filter), // onEdit callback for right-click editing
    );
  }

  nodeSpecificModify(): NodeModifyAttrs {
    this.validate();

    const mode = this.state.filterMode ?? 'structured';
    const filters = this.state.filters ?? [];
    const operator = this.state.filterOperator ?? 'AND';

    // Set autoExecute based on mode
    this.state.autoExecute = mode === 'structured';

    // Build bottom buttons
    const bottomLeftButtons: NodeModifyAttrs['bottomLeftButtons'] = [];
    const bottomRightButtons: NodeModifyAttrs['bottomRightButtons'] = [];

    // Show AND/OR switch only when there are 2+ filters in structured mode
    if (mode === 'structured' && filters.length >= 2) {
      bottomLeftButtons.push({
        label: operator === 'OR' ? 'OR' : 'AND',
        icon: operator === 'OR' ? 'alt_route' : 'join',
        onclick: () => {
          this.state.filterOperator = operator === 'OR' ? 'AND' : 'OR';
          this.state.onchange?.();
        },
      });
    }

    // Mode switch button
    bottomRightButtons.push({
      label:
        mode === 'structured' ? 'Switch to WHERE clause' : 'Switch to filters',
      icon: mode === 'structured' ? 'code' : 'filter_alt',
      onclick: () => this.handleModeSwitch(mode),
      compact: true,
    });

    // Build sections
    const sections: NodeModifyAttrs['sections'] = [];

    // Input section with buttons/inputs
    if (mode === 'structured') {
      sections.push({
        content: m(
          EqualWidthRow,
          {separator: '•'},
          m(Button, {
            label: 'Create filter',
            icon: 'add',
            variant: ButtonVariant.Outlined,
            onclick: () => this.showAddFilterModal(),
          }),
          m(TextInput, {
            placeholder: 'Type filter (e.g., dur > 1000)',
            leftIcon: 'filter_alt',
            onkeydown: (e: KeyboardEvent) => {
              if (e.key !== 'Enter') return;
              e.preventDefault();
              const input = e.target as HTMLInputElement;
              const text = input.value.trim();
              if (text === '') return;

              // Parse the text into a structured filter
              const filter = parseFilterFromText(text, this.sourceCols);
              if (!isFilterDefinitionValid(filter)) {
                // Show error to user - filter couldn't be parsed
                showModal({
                  title: 'Invalid filter',
                  content: m(
                    'div',
                    m('p', `Could not parse filter: "${text}"`),
                    m(
                      'p',
                      'Expected format: column operator value (e.g., dur > 1000)',
                    ),
                  ),
                });
                return;
              }

              // Add the parsed filter to the list
              this.state.filters = [...(this.state.filters ?? []), filter];
              this.state.filterMode = 'structured';
              this.state.onchange?.();
              input.value = '';
            },
          }),
        ),
      });
    } else {
      // Freeform mode - show edit button
      sections.push({
        content: m(Button, {
          label: 'Edit WHERE clause',
          icon: 'edit',
          variant: ButtonVariant.Outlined,
          onclick: () => this.showSqlExpressionModal(),
        }),
      });
    }

    // Filters list section
    sections.push({
      content: this.renderFiltersList(),
    });

    return {
      bottomLeftButtons,
      bottomRightButtons,
      sections,
    };
  }

  private renderFiltersList(): m.Child {
    const mode = this.state.filterMode ?? 'structured';
    const filters = this.state.filters ?? [];
    const hasFilters = filters.length > 0;
    const hasSqlExpression =
      this.state.sqlExpression !== undefined &&
      this.state.sqlExpression.trim() !== '';

    if (mode === 'freeform') {
      if (!hasSqlExpression) {
        return m(EmptyState, {
          title: 'No WHERE clause defined.',
        });
      }
      // Show SQL expression as a single list item
      return m(
        '.pf-filters-list',
        m(ListItem, {
          icon: 'code',
          name: 'WHERE clause',
          description: this.truncateSql(this.state.sqlExpression ?? ''),
          actions: [
            {
              label: 'Edit',
              icon: 'edit',
              onclick: () => this.showSqlExpressionModal(),
            },
          ],
          onRemove: () => {
            this.state.sqlExpression = '';
            this.state.filterMode = 'structured';
            this.state.onchange?.();
          },
        }),
      );
    }

    if (!hasFilters) {
      return m(EmptyState, {
        title: 'No filters added yet.',
      });
    }

    const items: m.Child[] = [];

    // Show each filter as a list item
    for (const [index, filter] of filters.entries()) {
      const isEnabled = filter.enabled !== false;
      const filterDescription = this.formatFilterDescription(filter);

      items.push(
        m(ListItem, {
          icon: isEnabled ? 'filter_alt' : 'filter_alt_off',
          name: filter.column,
          description: filterDescription,
          actions: [
            {
              label: 'Edit',
              icon: 'edit',
              onclick: () => this.handleFilterEdit(filter),
            },
          ],
          onRemove: () => this.removeFilter(index),
          className: classNames(!isEnabled && 'pf-filter-disabled'),
          onclick: (e: MouseEvent) => {
            // Do nothing if a button was clicked
            if ((e.target as HTMLElement).closest('button')) {
              return;
            }
            filter.enabled = !isEnabled;
            this.state.onchange?.();
          },
        }),
      );
    }

    return m('.pf-filters-list', items);
  }

  private handleModeSwitch(currentMode: 'structured' | 'freeform'): void {
    if (currentMode === 'structured') {
      // Switching to freeform: if no SQL expression yet, open modal first
      const hasExpression =
        this.state.sqlExpression !== undefined &&
        this.state.sqlExpression.trim() !== '';
      if (hasExpression) {
        this.state.filterMode = 'freeform';
        this.state.onchange?.();
      } else {
        this.showSqlExpressionModal();
      }
    } else {
      // Switching to structured
      this.state.filterMode = 'structured';
      this.state.onchange?.();
    }
  }

  private showAddFilterModal(): void {
    // Check if there are any columns available
    if (this.sourceCols.length === 0) {
      showModal({
        title: 'Cannot add filter',
        content: m(
          'div',
          m('p', 'No columns are available to filter on.'),
          m('p', 'Please select a table or add columns before adding filters.'),
        ),
      });
      return;
    }

    // Start with first column and "is not null" operator
    const defaultColumn = this.sourceCols[0].name;
    const newFilter: Partial<UIFilter> = {
      column: defaultColumn,
      op: 'is not null',
    };

    showFilterEditModal(newFilter, this.sourceCols, (createdFilter) => {
      this.state.filters = [...(this.state.filters ?? []), createdFilter];
      this.state.filterMode = 'structured';
      this.state.onchange?.();
      m.redraw();
    });
  }

  private showSqlExpressionModal(): void {
    let tempExpression = this.state.sqlExpression ?? '';

    showModal({
      title: 'SQL Filter Expression',
      key: 'sql-expression-modal',
      content: () =>
        m(
          '.pf-filter-sql-editor',
          m(Editor, {
            text: tempExpression,
            onUpdate: (text: string) => {
              tempExpression = text;
            },
          }),
        ),
      buttons: [
        {
          text: 'Cancel',
          action: () => {},
        },
        {
          text: 'Apply',
          primary: true,
          action: () => {
            this.state.sqlExpression = tempExpression;
            if (tempExpression.trim() !== '') {
              this.state.filterMode = 'freeform';
            }
            this.state.onchange?.();
          },
        },
      ],
    });
  }

  private removeFilter(index: number): void {
    const filters = this.state.filters;
    if (!filters || index >= filters.length) return;

    const newFilters = [...filters];
    newFilters.splice(index, 1);
    this.state.filters = newFilters;
    this.state.onchange?.();
  }

  private formatFilterDescription(filter: UIFilter): string {
    if ('value' in filter) {
      const valueStr =
        typeof filter.value === 'string'
          ? `"${filter.value}"`
          : String(filter.value);
      return `${filter.op} ${valueStr}`;
    }
    return filter.op;
  }

  private truncateSql(sql: string): string {
    const trimmed = sql.trim();
    if (trimmed.length <= SQL_TRUNCATE_LENGTH) {
      return trimmed;
    }
    return trimmed.substring(0, SQL_TRUNCATE_LENGTH - 1) + '\u2026';
  }

  nodeInfo(): m.Children {
    return m(
      'div',
      m(
        'p',
        'Keep only rows that match conditions you specify. Supports operators like ',
        m('code', '='),
        ', ',
        m('code', '>'),
        ', ',
        m('code', '<'),
        ', ',
        m('code', 'glob'),
        ', and null checks.',
      ),
      m(
        'p',
        'Combine multiple conditions with ',
        m('code', 'AND'),
        ' or ',
        m('code', 'OR'),
        ' logic. To use both AND and OR together, use multiple filter nodes.',
      ),
      m(
        'p',
        m('strong', 'Example:'),
        ' Keep slices where ',
        m('code', 'dur > 1000000'),
        ' AND ',
        m('code', 'name glob "*render*"'),
      ),
    );
  }

  validate(): boolean {
    // Clear any previous errors at the start of validation
    if (this.state.issues) {
      this.state.issues.clear();
    }

    if (this.primaryInput === undefined) {
      this.setValidationError('No input node connected');
      return false;
    }

    // Check if there are columns available from the previous node
    if (this.sourceCols.length === 0) {
      this.setValidationError(
        'No columns available. Please select a table or add columns before filtering.',
      );
      return false;
    }

    return true;
  }

  clone(): QueryNode {
    return new FilterNode(this.state);
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (this.primaryInput === undefined) return undefined;

    const mode = this.state.filterMode ?? 'structured';

    if (mode === 'freeform') {
      // Use SQL expression for freeform filtering
      if (!this.state.sqlExpression || this.state.sqlExpression.trim() === '') {
        return this.primaryInput.getStructuredQuery();
      }

      // Create a filter group with just the SQL expression
      const filterGroup =
        new protos.PerfettoSqlStructuredQuery.ExperimentalFilterGroup({
          op: protos.PerfettoSqlStructuredQuery.ExperimentalFilterGroup.Operator
            .AND,
          sqlExpressions: [this.state.sqlExpression],
        });

      return StructuredQueryBuilder.withFilter(
        this.primaryInput,
        filterGroup,
        this.nodeId,
      );
    }

    // Structured mode
    if (!this.state.filters || this.state.filters.length === 0) {
      return this.primaryInput.getStructuredQuery();
    }

    const filtersProto = createExperimentalFiltersProto(
      this.state.filters,
      this.sourceCols,
      this.state.filterOperator,
    );

    if (!filtersProto) {
      return this.primaryInput.getStructuredQuery();
    }

    return StructuredQueryBuilder.withFilter(
      this.primaryInput,
      filtersProto,
      this.nodeId,
    );
  }

  serializeState(): object {
    return {
      primaryInputId: this.primaryInput?.nodeId,
      filters: this.state.filters?.map((f) => {
        if ('value' in f) {
          return {
            column: f.column,
            op: f.op,
            value: f.value,
            enabled: f.enabled,
          };
        } else {
          return {
            column: f.column,
            op: f.op,
            enabled: f.enabled,
          };
        }
      }),
      filterOperator: this.state.filterOperator,
      filterMode: this.state.filterMode,
      sqlExpression: this.state.sqlExpression,
      comment: this.state.comment,
    };
  }

  /**
   * Deserializes a FilterNodeState from JSON.
   *
   * IMPORTANT: This method returns a state with primaryInput set to undefined.
   * The caller (typically json_handler.ts) is responsible for:
   * 1. Creating all nodes first with undefined primaryInput references
   * 2. Reconnecting the graph by setting primaryInput references based on serialized node IDs
   * 3. Calling validate() on each node after reconnection to ensure graph integrity
   *
   * @param state The serialized state
   * @returns A FilterNodeState ready for construction
   */
  static deserializeState(state: FilterNodeState): FilterNodeState {
    return {...state};
  }
}
