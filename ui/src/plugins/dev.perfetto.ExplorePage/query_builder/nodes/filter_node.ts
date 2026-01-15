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
  formatFilterDetails,
  isFilterDefinitionValid,
  ALL_FILTER_OPS,
  isValueRequired,
  parseFilterValue,
} from '../operations/filter';
import {StructuredQueryBuilder} from '../structured_query_builder';
import {NodeIssues} from '../node_issues';
import {showModal} from '../../../../widgets/modal';
import {Editor} from '../../../../widgets/editor';
import {ListItem, OutlinedField, InlineEditList} from '../widgets';
import {EmptyState} from '../../../../widgets/empty_state';
import {NodeModifyAttrs, NodeDetailsAttrs} from '../node_explorer_types';
import {Button, ButtonVariant} from '../../../../widgets/button';
import {NodeDetailsMessage} from '../node_styling_widgets';
import {Icons} from '../../../../base/semantic_icons';
import {loadNodeDoc} from '../node_doc_loader';

// Maximum length for truncated SQL display
const SQL_TRUNCATE_LENGTH = 50;

export interface FilterNodeState extends QueryNodeState {
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
    this.state = {
      ...state,
      filters: state.filters ?? [],
    };
    this.nextNodes = [];
  }

  get sourceCols(): ColumnInfo[] {
    return this.primaryInput?.finalCols ?? [];
  }

  get finalCols(): ColumnInfo[] {
    return this.sourceCols;
  }

  /**
   * Check if a filter is valid for this node.
   * A filter is valid if:
   * 1. Its definition is structurally valid (has column, op, value etc)
   * 2. The column it references actually exists in sourceCols
   */
  private isFilterValid(filter: Partial<UIFilter>): filter is UIFilter {
    // First check if the filter structure is valid
    if (!isFilterDefinitionValid(filter)) {
      return false;
    }

    // Then check if the column exists in sourceCols
    const columnExists = this.sourceCols.some(
      (col) => col.name === filter.column,
    );
    return columnExists;
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

  nodeDetails(): NodeDetailsAttrs {
    this.validate();

    const mode = this.state.filterMode ?? 'structured';

    // Freeform SQL mode
    if (mode === 'freeform') {
      const sql = this.state.sqlExpression?.trim();
      if (!sql) {
        return {
          content: NodeDetailsMessage('No filter clause'),
        };
      }

      if (sql.length < 200) {
        return {
          content: m('code', sql),
        };
      } else {
        return {
          content: NodeDetailsMessage('Filter clause applied'),
        };
      }
    }

    // Structured mode - only show valid filters in nodeDetails
    const validFilters =
      this.state.filters?.filter((f) => this.isFilterValid(f)) ?? [];

    if (validFilters.length === 0) {
      return {
        content: NodeDetailsMessage('No filters applied'),
      };
    }

    return {
      content: formatFilterDetails(
        validFilters,
        this.state.filterOperator,
        this.state, // Pass state for interactive toggling and removal
        undefined, // onRemove - handled internally by formatFilterDetails
        true, // compact mode for smaller font
        undefined, // No edit callback - editing happens in nodeSpecificModify
      ),
    };
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
      icon: mode === 'structured' ? 'code' : Icons.Filter,
      onclick: () => this.handleModeSwitch(mode),
      compact: true,
    });

    // Build sections
    const sections: NodeModifyAttrs['sections'] = [];

    // Input section with buttons/inputs - only for freeform mode
    if (mode === 'freeform') {
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

    // Info text explaining nested filters (only shown in structured mode)
    const info =
      mode === 'structured'
        ? 'To combine AND and OR logic (nested filters), use multiple filter nodes. Each filter node can use either AND or OR to combine its conditions.'
        : 'Use a custom WHERE clause to filter rows. This mode allows for complex SQL expressions that structured filters cannot express.';

    return {
      info,
      bottomLeftButtons,
      bottomRightButtons,
      sections,
    };
  }

  private renderFiltersList(): m.Child {
    const mode = this.state.filterMode ?? 'structured';
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

    // Structured mode - use InlineEditList widget
    return m(InlineEditList<Partial<UIFilter>>, {
      items: this.state.filters ?? [],
      validate: (filter) => this.isFilterValid(filter),
      renderControls: (filter, _index, onUpdate) =>
        this.renderFilterFormControls(filter, onUpdate),
      onUpdate: (filters) => {
        this.state.filters = filters;
      },
      onValidChange: () => {
        this.state.onchange?.();
      },
      addButtonLabel: 'Add filter',
      addButtonIcon: 'add',
      emptyItem: () => ({enabled: true}),
    });
  }

  private renderFilterFormControls(
    filter: Partial<UIFilter>,
    onUpdate: (updated: Partial<UIFilter>) => void,
  ): m.Children {
    const opObject = ALL_FILTER_OPS.find((o) => o.displayName === filter.op);
    const valueRequired = isValueRequired(opObject);

    return [
      // Column selector with outlined style
      m(
        OutlinedField,
        {
          label: 'Column',
          value: filter.column ?? '',
          onchange: (e: Event) => {
            const target = e.target as HTMLSelectElement;
            onUpdate({...filter, column: target.value});
          },
        },
        [
          m('option', {value: '', disabled: true}, 'Select column...'),
          ...this.sourceCols.map((col) =>
            m('option', {value: col.name}, col.name),
          ),
        ],
      ),
      // Operator selector with outlined style
      m(
        OutlinedField,
        {
          label: 'Operator',
          value: opObject?.key ?? '',
          onchange: (e: Event) => {
            const target = e.target as HTMLSelectElement;
            const newOp = ALL_FILTER_OPS.find((op) => op.key === target.value);
            if (newOp) {
              const updated: Partial<UIFilter> = {
                column: filter.column,
                op: newOp.displayName as UIFilter['op'],
                enabled: filter.enabled,
              };
              // Add value if required
              if (isValueRequired(newOp)) {
                (updated as {value: string}).value =
                  'value' in filter ? String(filter.value) : '';
              }
              onUpdate(updated);
            }
          },
        },
        [
          m('option', {value: '', disabled: true}, 'Select operator...'),
          ...ALL_FILTER_OPS.map((op) =>
            m('option', {value: op.key}, op.displayName),
          ),
        ],
      ),
      // Value input with outlined style (always show, disabled when not required)
      m(OutlinedField, {
        label: 'Value',
        value: 'value' in filter ? String(filter.value) : '',
        disabled: !valueRequired,
        placeholder: 'Enter value...',
        oninput: (e: Event) => {
          const target = e.target as HTMLInputElement;
          const parsed = parseFilterValue(target.value);
          onUpdate({
            ...filter,
            value: parsed ?? target.value,
          } as Partial<UIFilter>);
        },
      }),
    ];
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

  private truncateSql(sql: string): string {
    const trimmed = sql.trim();
    if (trimmed.length <= SQL_TRUNCATE_LENGTH) {
      return trimmed;
    }
    return trimmed.substring(0, SQL_TRUNCATE_LENGTH - 1) + '\u2026';
  }

  nodeInfo(): m.Children {
    return loadNodeDoc('filter');
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

    if (!this.primaryInput.validate()) {
      this.setValidationError('Previous node is invalid');
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
    const stateCopy: FilterNodeState = {
      filterMode: this.state.filterMode,
      sqlExpression: this.state.sqlExpression,
      filters: this.state.filters?.map((f) => ({...f})),
      filterOperator: this.state.filterOperator,
      onchange: this.state.onchange,
    };
    return new FilterNode(stateCopy);
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

    // Structured mode - only use valid filters for query building
    const validFilters =
      this.state.filters?.filter((f) => this.isFilterValid(f)) ?? [];

    if (validFilters.length === 0) {
      return this.primaryInput.getStructuredQuery();
    }

    const filtersProto = createExperimentalFiltersProto(
      validFilters,
      this.sourceCols,
      this.state.filterOperator,
    );

    if (filtersProto === undefined) {
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
