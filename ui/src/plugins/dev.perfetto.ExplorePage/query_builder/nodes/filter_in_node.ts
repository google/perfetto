// Copyright (C) 2026 The Android Open Source Project
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

/**
 * Filter In Node - Semi-Join Filtering
 *
 * This node filters rows from a primary input to only those where a specified
 * column's values exist in another column from a secondary input (the "match
 * values"). This is essentially a semi-join operation.
 *
 * ## Architecture
 *
 * The node uses ExperimentalFilterIn which:
 * - Filters rows where base_column IN (SELECT match_column FROM match_values)
 * - Preserves all columns from the base query
 * - Does not add any columns from the match values query
 *
 * ## Required Columns
 *
 * Primary input must have:
 *   - A column matching the specified base_column
 *
 * Input node must have:
 *   - A column matching the specified match_column
 *
 * ## Example Use Cases
 *
 * - Filter slices to only those belonging to specific threads
 * - Filter counter data to specific track IDs
 * - Filter events to only those from specific processes
 *
 * ## Output Behavior
 *
 * The output preserves exactly the same columns as the primary input,
 * filtered to only rows where base_column value exists in match_values.
 */

import {
  QueryNode,
  QueryNodeState,
  nextNodeId,
  NodeType,
  SecondaryInputSpec,
} from '../../query_node';
import {ColumnInfo} from '../column_info';
import protos from '../../../../protos';
import m from 'mithril';
import {StructuredQueryBuilder} from '../structured_query_builder';
import {setValidationError} from '../node_issues';
import {EmptyState} from '../../../../widgets/empty_state';
import {Callout} from '../../../../widgets/callout';
import {loadNodeDoc} from '../node_doc_loader';
import {ListItem, OutlinedField} from '../widgets';
import {NodeModifyAttrs, NodeDetailsAttrs} from '../node_explorer_types';
import {NodeDetailsMessage, ColumnName} from '../node_styling_widgets';
import {notifyNextNodes} from '../graph_utils';
import {getCommonColumns} from '../utils';

export interface FilterInNodeState extends QueryNodeState {
  baseColumn?: string; // Column name in the primary input to filter on
  matchColumn?: string; // Column name in the match values input to match against
}

export class FilterInNode implements QueryNode {
  readonly nodeId: string;
  readonly type = NodeType.kFilterIn;
  primaryInput?: QueryNode;
  secondaryInputs: SecondaryInputSpec;
  nextNodes: QueryNode[];
  readonly state: FilterInNodeState;

  constructor(state: FilterInNodeState) {
    this.nodeId = nextNodeId();
    this.state = state;
    this.secondaryInputs = {
      connections: new Map(),
      min: 1,
      max: 1,
      portNames: ['Input'],
    };
    this.nextNodes = [];
    this.state.autoExecute = this.state.autoExecute ?? false;
  }

  // Get the node connected to the secondary input port (the match values)
  get matchValuesNode(): QueryNode | undefined {
    return this.secondaryInputs.connections.get(0);
  }

  get finalCols(): ColumnInfo[] {
    // Return the same columns as the primary input
    return this.primaryInput?.finalCols ?? [];
  }

  // Auto-suggest columns if there's only one common column
  private autoSuggestColumns(): void {
    if (this.primaryInput === undefined || this.matchValuesNode === undefined) {
      return;
    }
    const commonCols = getCommonColumns([
      this.primaryInput.finalCols,
      this.matchValuesNode.finalCols,
    ]);
    if (commonCols.length === 1) {
      const commonCol = commonCols[0];
      if (!this.state.baseColumn) {
        this.state.baseColumn = commonCol;
      }
      if (!this.state.matchColumn) {
        this.state.matchColumn = commonCol;
      }
    }
  }

  getTitle(): string {
    return 'Filter In';
  }

  nodeDetails(): NodeDetailsAttrs {
    const baseCol = this.state.baseColumn;
    const matchCol = this.state.matchColumn;

    if (baseCol && matchCol) {
      if (baseCol === matchCol) {
        // Same column name - simpler message
        return {
          content: m(
            'span',
            NodeDetailsMessage('Keep rows where '),
            ColumnName(baseCol),
            NodeDetailsMessage(' in input'),
          ),
        };
      } else {
        // Different column names - show both
        return {
          content: m(
            'span',
            NodeDetailsMessage('Keep rows where '),
            ColumnName(baseCol),
            NodeDetailsMessage(' in input.'),
            ColumnName(matchCol),
          ),
        };
      }
    }

    return {
      content: NodeDetailsMessage(
        'Filters rows to only those where a column value exists in match set.',
      ),
    };
  }

  nodeSpecificModify(): NodeModifyAttrs {
    // Run validation to populate error state
    this.validate();
    const error = this.state.issues?.queryError;

    const matchValuesNode = this.matchValuesNode;

    // If no secondary input connected, show empty state
    if (matchValuesNode === undefined) {
      return {
        info: 'Filters rows to only those where a column value exists in the match values from the secondary input.',
        sections: [
          {
            content: m(EmptyState, {
              icon: 'link_off',
              title: 'No match values connected',
              detail:
                'Connect a node to the left port that provides values to match against.',
            }),
          },
        ],
      };
    }

    const sections: NodeModifyAttrs['sections'] = [];

    // Add error callout if present
    if (error) {
      sections.push({
        content: m(Callout, {icon: 'error'}, error.message),
      });
    }

    // Get available columns for dropdowns
    const primaryCols = this.primaryInput?.finalCols ?? [];
    const matchCols = matchValuesNode.finalCols ?? [];

    // Column selectors side by side
    sections.push({
      content: m('.pf-filter-in-column-selectors', [
        m(
          OutlinedField,
          {
            label: 'Base column',
            value: this.state.baseColumn ?? '',
            onchange: (e: Event) => {
              this.state.baseColumn = (e.target as HTMLSelectElement).value;
              this.state.onchange?.();
            },
          },
          [
            m('option', {value: ''}, '-- Select column --'),
            ...primaryCols.map((col) =>
              m('option', {value: col.name}, col.name),
            ),
          ],
        ),
        m(
          OutlinedField,
          {
            label: 'Match column',
            value: this.state.matchColumn ?? '',
            onchange: (e: Event) => {
              this.state.matchColumn = (e.target as HTMLSelectElement).value;
              this.state.onchange?.();
            },
          },
          [
            m('option', {value: ''}, '-- Select column --'),
            ...matchCols.map((col) => m('option', {value: col.name}, col.name)),
          ],
        ),
      ]),
    });

    // Add primary input section
    sections.push({
      content: m(ListItem, {
        icon: 'input',
        name: 'Primary Input',
        description: 'Rows to filter',
        actions: [],
      }),
    });

    // Add match values input section
    sections.push({
      content: m(ListItem, {
        icon: 'input',
        name: 'Input',
        description: 'Values to match against',
        actions: [],
      }),
    });

    return {
      info: 'Filters the primary input to only rows where the base column value exists in the match values.',
      sections,
    };
  }

  nodeInfo(): m.Children {
    return loadNodeDoc('filter_in');
  }

  validate(): boolean {
    // Clear any previous errors at the start of validation
    if (this.state.issues) {
      this.state.issues.clear();
    }

    if (this.primaryInput === undefined) {
      setValidationError(
        this.state,
        'Connect a node with rows to filter to the top port',
      );
      return false;
    }

    if (!this.primaryInput.validate()) {
      setValidationError(this.state, 'Primary input is invalid');
      return false;
    }

    const matchValuesNode = this.matchValuesNode;
    if (matchValuesNode === undefined) {
      setValidationError(
        this.state,
        'Connect a node with match values to the port on the left',
      );
      return false;
    }

    // Validate the secondary input
    if (!matchValuesNode.validate()) {
      const childError =
        matchValuesNode.state.issues?.queryError !== undefined
          ? `: ${matchValuesNode.state.issues.queryError.message}`
          : '';
      setValidationError(this.state, `Input node is invalid${childError}`);
      return false;
    }

    // Check that base column is specified
    if (!this.state.baseColumn) {
      setValidationError(this.state, 'Select a base column to filter on');
      return false;
    }

    // Check that match column is specified
    if (!this.state.matchColumn) {
      setValidationError(this.state, 'Select a match column');
      return false;
    }

    // Check that base column exists in primary input
    const primaryCols = new Set(this.primaryInput.finalCols.map((c) => c.name));
    if (!primaryCols.has(this.state.baseColumn)) {
      setValidationError(
        this.state,
        `Primary input is missing column: ${this.state.baseColumn}`,
      );
      return false;
    }

    // Check that match column exists in match values input
    const matchCols = new Set(matchValuesNode.finalCols.map((c) => c.name));
    if (!matchCols.has(this.state.matchColumn)) {
      setValidationError(
        this.state,
        `Input node is missing column: ${this.state.matchColumn}`,
      );
      return false;
    }

    return true;
  }

  clone(): QueryNode {
    const stateCopy: FilterInNodeState = {
      baseColumn: this.state.baseColumn,
      matchColumn: this.state.matchColumn,
      filters: this.state.filters?.map((f) => ({...f})),
      filterOperator: this.state.filterOperator,
      onchange: this.state.onchange,
    };
    return new FilterInNode(stateCopy);
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (!this.validate()) return undefined;
    if (this.primaryInput === undefined) return undefined;

    const matchValuesNode = this.matchValuesNode;
    if (matchValuesNode === undefined) return undefined;

    const baseColumn = this.state.baseColumn;
    const matchColumn = this.state.matchColumn;
    if (!baseColumn || !matchColumn) return undefined;

    return StructuredQueryBuilder.withFilterIn(
      this.primaryInput,
      matchValuesNode,
      baseColumn,
      matchColumn,
      this.nodeId,
    );
  }

  serializeState(): object {
    return {
      primaryInputId: this.primaryInput?.nodeId,
      secondaryInputNodeIds: Array.from(
        this.secondaryInputs.connections.values(),
      ).map((node) => node.nodeId),
      baseColumn: this.state.baseColumn,
      matchColumn: this.state.matchColumn,
    };
  }

  static deserializeState(
    serializedState: FilterInNodeState,
  ): FilterInNodeState {
    return {
      baseColumn: serializedState.baseColumn,
      matchColumn: serializedState.matchColumn,
    };
  }

  static deserializeConnections(
    nodes: Map<string, QueryNode>,
    serializedState: {secondaryInputNodeIds?: string[]},
  ): {secondaryInputNodes: QueryNode[]} {
    const secondaryInputNodes: QueryNode[] = [];
    if (serializedState.secondaryInputNodeIds) {
      for (const nodeId of serializedState.secondaryInputNodeIds) {
        const node = nodes.get(nodeId);
        if (node) {
          secondaryInputNodes.push(node);
        }
      }
    }
    return {
      secondaryInputNodes,
    };
  }

  private cleanupStaleColumns(): void {
    if (this.primaryInput !== undefined && this.state.baseColumn) {
      const primaryCols = new Set(
        this.primaryInput.finalCols.map((c) => c.name),
      );
      if (!primaryCols.has(this.state.baseColumn)) {
        this.state.baseColumn = undefined;
      }
    }

    const matchNode = this.matchValuesNode;
    if (matchNode !== undefined && this.state.matchColumn) {
      const matchCols = new Set(matchNode.finalCols.map((c) => c.name));
      if (!matchCols.has(this.state.matchColumn)) {
        this.state.matchColumn = undefined;
      }
    }
  }

  // Called when a node is connected/disconnected to secondary inputs
  onPrevNodesUpdated(): void {
    // Clean up columns that no longer exist in the inputs
    this.cleanupStaleColumns();

    // Auto-suggest columns if there's only one common column
    this.autoSuggestColumns();

    // Notify next nodes that our columns may have changed
    notifyNextNodes(this);
    this.state.onchange?.();
    m.redraw();
  }
}
