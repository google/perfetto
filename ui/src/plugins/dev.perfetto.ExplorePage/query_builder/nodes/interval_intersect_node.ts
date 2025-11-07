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
  MultiSourceNode,
} from '../../query_node';
import protos from '../../../../protos';
import {ColumnInfo, newColumnInfoList} from '../column_info';
import {Button} from '../../../../widgets/button';
import {Callout} from '../../../../widgets/callout';
import {NodeIssues} from '../node_issues';
import {UIFilter} from '../operations/filter';
import {
  PopupMultiSelect,
  MultiSelectOption,
  MultiSelectDiff,
} from '../../../../widgets/multiselect';
import {StructuredQueryBuilder} from '../structured_query_builder';

export interface IntervalIntersectSerializedState {
  intervalNodes: string[];
  filters?: UIFilter[];
  comment?: string;
  filterNegativeDur?: boolean[]; // Per-input filter to exclude negative durations
  partitionColumns?: string[]; // Columns to partition by during interval intersection
}

export interface IntervalIntersectNodeState extends QueryNodeState {
  readonly prevNodes: QueryNode[];
  filterNegativeDur?: boolean[]; // Per-input filter to exclude negative durations
  partitionColumns?: string[]; // Columns to partition by during interval intersection
}

export class IntervalIntersectNode implements MultiSourceNode {
  readonly nodeId: string;
  readonly type = NodeType.kIntervalIntersect;
  readonly prevNodes: QueryNode[];
  nextNodes: QueryNode[];
  readonly state: IntervalIntersectNodeState;

  get finalCols(): ColumnInfo[] {
    return newColumnInfoList(this.prevNodes[0]?.finalCols ?? [], true);
  }

  constructor(state: IntervalIntersectNodeState) {
    this.nodeId = nextNodeId();
    this.state = {
      ...state,
      autoExecute: state.autoExecute ?? false,
    };
    this.prevNodes = state.prevNodes;
    this.nextNodes = [];
  }

  validate(): boolean {
    // Clear any previous errors at the start of validation
    if (this.state.issues) {
      this.state.issues.clear();
    }

    // Check for undefined entries (disconnected inputs)
    const validPrevNodes = this.prevNodes.filter(
      (node): node is QueryNode => node !== undefined,
    );

    if (validPrevNodes.length < this.prevNodes.length) {
      this.setValidationError(
        'Interval intersect node has disconnected inputs. Please connect all inputs or remove this node.',
      );
      return false;
    }

    if (this.prevNodes.length < 2) {
      this.setValidationError(
        'Interval intersect node requires at least two inputs.',
      );
      return false;
    }

    for (const prevNode of this.prevNodes) {
      // Skip undefined entries (already handled above)
      if (prevNode === undefined) continue;

      if (!prevNode.validate()) {
        this.setValidationError(
          prevNode.state.issues?.queryError?.message ??
            `Previous node '${prevNode.getTitle()}' is invalid`,
        );
        return false;
      }
    }

    const checkColumns = (node: QueryNode, required: string[]) => {
      const cols = new Set(node.finalCols.map((c) => c.name));
      const missing = required.filter((r) => !cols.has(r));
      if (missing.length > 0) {
        this.setValidationError(
          `Node '${node.getTitle()}' is missing required columns: ${missing.join(
            ', ',
          )}`,
        );
        return false;
      }
      return true;
    };

    for (const prevNode of this.prevNodes) {
      if (!checkColumns(prevNode, ['id', 'ts', 'dur'])) return false;
    }

    return true;
  }

  private setValidationError(message: string): void {
    if (!this.state.issues) {
      this.state.issues = new NodeIssues();
    }
    this.state.issues.queryError = new Error(message);
  }

  getTitle(): string {
    return 'Interval Intersect';
  }

  private renderPartitionSelector(compact: boolean = false): m.Child {
    // Initialize partition columns if needed
    if (!this.state.partitionColumns) {
      this.state.partitionColumns = [];
    }

    // Get common columns for partition selection
    const commonColumns = this.getCommonColumns();
    if (commonColumns.length === 0) {
      return null;
    }

    const partitionOptions: MultiSelectOption[] = commonColumns.map((col) => ({
      id: col,
      name: col,
      checked: this.state.partitionColumns?.includes(col) ?? false,
    }));

    const label =
      this.state.partitionColumns.length > 0
        ? this.state.partitionColumns.join(', ')
        : 'None';

    return m(
      '.pf-exp-partition-columns',
      {
        style: compact
          ? {marginTop: '4px', marginBottom: '4px'}
          : {marginTop: '8px', marginBottom: '8px'},
      },
      m(
        'label',
        {
          style: compact
            ? {marginRight: '8px', fontSize: '12px'}
            : {marginRight: '8px'},
        },
        'Partition by:',
      ),
      m(PopupMultiSelect, {
        label,
        options: partitionOptions,
        showNumSelected: false,
        compact,
        onChange: (diffs: MultiSelectDiff[]) => {
          if (!this.state.partitionColumns) {
            this.state.partitionColumns = [];
          }
          for (const diff of diffs) {
            if (diff.checked) {
              if (!this.state.partitionColumns.includes(diff.id)) {
                this.state.partitionColumns.push(diff.id);
              }
            } else {
              const index = this.state.partitionColumns.indexOf(diff.id);
              if (index !== -1) {
                this.state.partitionColumns.splice(index, 1);
              }
            }
          }
          this.state.onchange?.();
        },
      }),
    );
  }

  nodeDetails(): m.Child {
    return this.renderPartitionSelector(true);
  }

  onPrevNodesUpdated(): void {
    // Compact filterNegativeDur array to match prevNodes length
    // When nodes are removed, prevNodes is compacted, so we need to match that
    if (
      this.state.filterNegativeDur &&
      this.state.filterNegativeDur.length > this.prevNodes.length
    ) {
      this.state.filterNegativeDur = this.state.filterNegativeDur.slice(
        0,
        this.prevNodes.length,
      );
    }
  }

  private checkDuplicateColumns(): string[] {
    const EXCLUDED_COLUMNS = new Set(['id', 'ts', 'dur']);
    const columnToInputs = new Map<string, number[]>();

    // Also exclude columns used for partitioning (they're intentionally in all inputs)
    const partitionColumns = new Set(this.state.partitionColumns ?? []);

    // Track which inputs each column appears in
    for (let i = 0; i < this.prevNodes.length; i++) {
      const node = this.prevNodes[i];
      const columns = node.finalCols.map((c) => c.name);
      for (const col of columns) {
        if (!EXCLUDED_COLUMNS.has(col) && !partitionColumns.has(col)) {
          if (!columnToInputs.has(col)) {
            columnToInputs.set(col, []);
          }
          columnToInputs.get(col)!.push(i + 1); // +1 for user-friendly "Input 1, Input 2" labels
        }
      }
    }

    // Build warning message for columns that appear in multiple inputs
    const warnings: string[] = [];
    for (const [col, inputs] of columnToInputs.entries()) {
      if (inputs.length > 1) {
        const inputLabels = inputs.map((i) => `Input ${i}`).join(', ');
        warnings.push(`'${col}' in ${inputLabels}`);
      }
    }

    return warnings;
  }

  private getCommonColumns(): string[] {
    const EXCLUDED_COLUMNS = new Set(['id', 'ts', 'dur']);
    const EXCLUDED_TYPES = new Set(['STRING', 'BYTES']);

    if (this.prevNodes.length === 0) return [];

    // Start with columns from the first input
    const firstNode = this.prevNodes[0];
    const commonColumns = new Set(
      firstNode.finalCols
        .filter(
          (c) => !EXCLUDED_COLUMNS.has(c.name) && !EXCLUDED_TYPES.has(c.type),
        )
        .map((c) => c.name),
    );

    // Intersect with columns from remaining inputs
    for (let i = 1; i < this.prevNodes.length; i++) {
      const node = this.prevNodes[i];
      const nodeColumns = new Map(node.finalCols.map((c) => [c.name, c.type]));
      // Keep only columns that exist in this node too with a non-excluded type
      for (const col of commonColumns) {
        const colType = nodeColumns.get(col);
        if (colType === undefined || EXCLUDED_TYPES.has(colType)) {
          commonColumns.delete(col);
        }
      }
    }

    return Array.from(commonColumns).sort();
  }

  nodeSpecificModify(): m.Child {
    this.validate();
    const error = this.state.issues?.queryError;
    const duplicateWarnings = this.checkDuplicateColumns();

    // Initialize filterNegativeDur array if needed
    if (!this.state.filterNegativeDur) {
      this.state.filterNegativeDur = [];
    }

    // Map prevNodes to UI elements with their indices
    const connectedInputs: Array<{node: QueryNode; index: number}> =
      this.prevNodes.map((node, index) => ({node, index}));

    return m(
      '.pf-exp-query-operations',
      error && m(Callout, {icon: 'error'}, error.message),
      duplicateWarnings.length > 0 &&
        m(
          Callout,
          {icon: 'warning'},
          m('div', 'Duplicate columns found:'),
          m(
            'ul',
            {style: {marginTop: '4px', marginBottom: '0', paddingLeft: '20px'}},
            duplicateWarnings.map((warning) => m('li', warning)),
          ),
        ),
      this.renderPartitionSelector(false),
      m(
        '.pf-exp-section',
        m(
          '.pf-exp-operations-container',
          connectedInputs.map(({node, index}) => {
            const label = `Input ${index + 1}`;
            const filterEnabled =
              this.state.filterNegativeDur?.[index] ?? false;

            return m(
              '.pf-exp-interval-node',
              {
                key: node.nodeId,
                style: {
                  display: 'flex',
                  gap: '8px',
                  alignItems: 'center',
                  marginBottom: '8px',
                },
              },
              m('span', {style: {flex: 1}}, `${label}: ${node.getTitle()}`),
              m(Button, {
                label: 'Filter unfinished intervals',
                icon: filterEnabled ? 'check_box' : 'check_box_outline_blank',
                title: 'Filter out intervals with negative duration',
                onclick: () => {
                  if (!this.state.filterNegativeDur) {
                    this.state.filterNegativeDur = [];
                  }
                  this.state.filterNegativeDur[index] = !filterEnabled;
                  this.state.onchange?.();
                },
              }),
            );
          }),
        ),
      ),
    );
  }

  clone(): QueryNode {
    const stateCopy: IntervalIntersectNodeState = {
      prevNodes: [...this.state.prevNodes],
      filters: this.state.filters ? [...this.state.filters] : undefined,
      filterNegativeDur: this.state.filterNegativeDur
        ? [...this.state.filterNegativeDur]
        : undefined,
      partitionColumns: this.state.partitionColumns
        ? [...this.state.partitionColumns]
        : undefined,
      onchange: this.state.onchange,
    };
    return new IntervalIntersectNode(stateCopy);
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (!this.validate()) return;

    return StructuredQueryBuilder.withIntervalIntersect(
      this.prevNodes[0],
      this.prevNodes.slice(1),
      this.state.partitionColumns,
      this.state.filterNegativeDur,
      this.nodeId,
    );
  }

  serializeState(): IntervalIntersectSerializedState {
    return {
      intervalNodes: this.prevNodes
        .slice(1)
        .filter((n): n is QueryNode => n !== undefined)
        .map((n) => n.nodeId),
      filters: this.state.filters?.map((f) => {
        // Explicitly extract only serializable fields to avoid circular references
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
      comment: this.state.comment,
      filterNegativeDur: this.state.filterNegativeDur,
      partitionColumns: this.state.partitionColumns,
    };
  }

  static deserializeState(
    nodes: Map<string, QueryNode>,
    state: IntervalIntersectSerializedState,
    baseNode: QueryNode,
  ): {
    prevNodes: QueryNode[];
    filterNegativeDur?: boolean[];
    partitionColumns?: string[];
  } {
    const intervalNodes = state.intervalNodes
      .map((id) => nodes.get(id))
      .filter((node): node is QueryNode => node !== undefined);
    return {
      prevNodes: [baseNode, ...intervalNodes],
      filterNegativeDur: state.filterNegativeDur,
      partitionColumns: state.partitionColumns,
    };
  }
}
