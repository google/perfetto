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
  ModificationNode,
} from '../../query_node';
import {ColumnInfo} from '../column_info';
import protos from '../../../../protos';
import {
  UIFilter,
  renderFilterOperation,
  createExperimentalFiltersProto,
  formatFilterDetails,
  showFilterEditModal,
} from '../operations/filter';
import {StructuredQueryBuilder} from '../structured_query_builder';
import {NodeIssues} from '../node_issues';
import {showModal} from '../../../../widgets/modal';
import {TabStrip} from '../../../../widgets/tabs';
import {Editor} from '../../../../widgets/editor';
import {Switch} from '../../../../widgets/switch';

export interface FilterNodeState extends QueryNodeState {
  prevNode: QueryNode;
  filters?: UIFilter[];
  filterOperator?: 'AND' | 'OR';
  filterMode?: 'structured' | 'freeform';
  sqlExpression?: string;
}

export class FilterNode implements ModificationNode {
  readonly nodeId: string;
  readonly type = NodeType.kFilter;
  readonly prevNode: QueryNode;
  nextNodes: QueryNode[];
  readonly state: FilterNodeState;

  constructor(state: FilterNodeState) {
    this.nodeId = nextNodeId();
    this.state = state;
    this.prevNode = state.prevNode;
    this.nextNodes = [];
  }

  get sourceCols(): ColumnInfo[] {
    return this.prevNode?.finalCols ?? [];
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

  nodeSpecificModify(): m.Child {
    this.validate();

    const mode = this.state.filterMode ?? 'structured';
    const operator = this.state.filterOperator ?? 'AND';

    // Set autoExecute based on mode
    this.state.autoExecute = mode === 'structured';

    return m('.pf-exp-query-operations', [
      // Tab strip
      m(
        'div',
        m(TabStrip, {
          tabs: [
            {key: 'structured', title: 'Structured'},
            {key: 'freeform', title: 'Freeform SQL'},
          ],
          currentTabKey: mode,
          onTabChange: (key: string) => {
            this.state.filterMode = key as 'structured' | 'freeform';
            this.state.onchange?.();
          },
        }),
        m('hr', {
          style: {margin: '0', borderTop: '1px solid var(--separator-color)'},
        }),
      ),

      // AND/OR Switch (only for structured mode)
      mode === 'structured' &&
        m(
          '.pf-exp-filter-mode-top',
          m(Switch, {
            labelLeft: 'AND',
            label: 'OR',
            checked: operator === 'OR',
            onchange: (e: Event) => {
              const target = e.target as HTMLInputElement;
              const newOperator = target.checked ? 'OR' : 'AND';
              this.state.filterOperator = newOperator;
              this.state.onchange?.();
            },
          }),
        ),

      // Tab content
      mode === 'structured'
        ? m(
            'div',
            {style: {paddingTop: '10px'}},
            renderFilterOperation(
              this.state.filters,
              this.state.filterOperator,
              this.sourceCols,
              (newFilters) => {
                this.state.filters = [...newFilters];
                this.state.onchange?.();
              },
              (operator) => {
                this.state.filterOperator = operator;
                this.state.onchange?.();
              },
              (filter) => this.handleFilterEdit(filter),
            ),
          )
        : m(
            'div',
            {
              style: {
                minHeight: '400px',
                backgroundColor: '#282c34',
                position: 'relative',
              },
            },
            m(Editor, {
              text: this.state.sqlExpression ?? '',
              onUpdate: (text: string) => {
                this.state.sqlExpression = text;
                this.state.onchange?.();
              },
            }),
          ),
    ]);
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

    if (this.prevNode === undefined) {
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
    if (this.prevNode === undefined) return undefined;

    const mode = this.state.filterMode ?? 'structured';

    if (mode === 'freeform') {
      // Use SQL expression for freeform filtering
      if (!this.state.sqlExpression || this.state.sqlExpression.trim() === '') {
        return this.prevNode.getStructuredQuery();
      }

      // Create a filter group with just the SQL expression
      const filterGroup =
        new protos.PerfettoSqlStructuredQuery.ExperimentalFilterGroup({
          op: protos.PerfettoSqlStructuredQuery.ExperimentalFilterGroup.Operator
            .AND,
          sqlExpressions: [this.state.sqlExpression],
        });

      return StructuredQueryBuilder.withFilter(
        this.prevNode,
        filterGroup,
        this.nodeId,
      );
    }

    // Structured mode
    if (!this.state.filters || this.state.filters.length === 0) {
      return this.prevNode.getStructuredQuery();
    }

    const filtersProto = createExperimentalFiltersProto(
      this.state.filters,
      this.sourceCols,
      this.state.filterOperator,
    );

    if (!filtersProto) {
      return this.prevNode.getStructuredQuery();
    }

    return StructuredQueryBuilder.withFilter(
      this.prevNode,
      filtersProto,
      this.nodeId,
    );
  }

  serializeState(): object {
    return {
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
   * IMPORTANT: This method returns a state with prevNode set to undefined.
   * The caller (typically json_handler.ts) is responsible for:
   * 1. Creating all nodes first with undefined prevNode references
   * 2. Reconnecting the graph by setting prevNode references based on serialized node IDs
   * 3. Calling validate() on each node after reconnection to ensure graph integrity
   *
   * @param state The serialized state (prevNode will be ignored)
   * @returns A FilterNodeState with prevNode set to undefined (to be set by caller)
   */
  static deserializeState(state: FilterNodeState): FilterNodeState {
    return {
      ...state,
      prevNode: undefined as unknown as QueryNode,
    };
  }
}
