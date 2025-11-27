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
  SecondaryInputSpec,
} from '../../query_node';
import protos from '../../../../protos';
import {ColumnInfo, columnInfoFromName} from '../column_info';
import {Callout} from '../../../../widgets/callout';
import {EmptyState} from '../../../../widgets/empty_state';
import {NodeIssues} from '../node_issues';
import {
  PopupMultiSelect,
  MultiSelectOption,
  MultiSelectDiff,
} from '../../../../widgets/multiselect';
import {StructuredQueryBuilder} from '../structured_query_builder';
import {LabeledControl, IssueList, ListItem} from '../widgets';

export interface IntervalIntersectSerializedState {
  intervalNodes: string[];
  comment?: string;
  filterNegativeDur?: boolean[]; // Per-input filter to exclude negative durations
  partitionColumns?: string[]; // Columns to partition by during interval intersection
}

export interface IntervalIntersectNodeState extends QueryNodeState {
  inputNodes: QueryNode[];
  filterNegativeDur?: boolean[]; // Per-input filter to exclude negative durations
  partitionColumns?: string[]; // Columns to partition by during interval intersection
}

export class IntervalIntersectNode implements QueryNode {
  readonly nodeId: string;
  readonly type = NodeType.kIntervalIntersect;
  secondaryInputs: SecondaryInputSpec;
  nextNodes: QueryNode[];
  readonly state: IntervalIntersectNodeState;

  get inputNodesList(): QueryNode[] {
    return [...this.secondaryInputs.connections.entries()]
      .sort(([a], [b]) => a - b)
      .map(([, node]) => node);
  }

  get finalCols(): ColumnInfo[] {
    const inputNodes = this.inputNodesList;
    if (inputNodes.length === 0) {
      return [];
    }

    const finalCols: ColumnInfo[] = [];
    const seenColumns = new Set<string>();

    // Add ts and dur from the intersection (without suffix)
    finalCols.push(columnInfoFromName('ts', true));
    finalCols.push(columnInfoFromName('dur', true));
    seenColumns.add('ts');
    seenColumns.add('dur');

    // Add partition columns (without suffix)
    if (this.state.partitionColumns) {
      for (const col of this.state.partitionColumns) {
        finalCols.push(columnInfoFromName(col, true));
        seenColumns.add(col);
      }
    }

    // For each input node, add id_N, ts_N, dur_N
    for (let i = 0; i < inputNodes.length; i++) {
      const node = inputNodes[i];
      if (node === undefined) continue;

      // Find the actual column info for id, ts, dur to get their types
      const nodeCols = node.finalCols;
      const idCol = nodeCols.find((c) => c.name === 'id');
      const tsCol = nodeCols.find((c) => c.name === 'ts');
      const durCol = nodeCols.find((c) => c.name === 'dur');

      finalCols.push({
        ...idCol,
        name: `id_${i}`,
        type: idCol?.type ?? 'NA',
        checked: true,
        column: {name: `id_${i}`},
      });
      finalCols.push({
        ...tsCol,
        name: `ts_${i}`,
        type: tsCol?.type ?? 'NA',
        checked: true,
        column: {name: `ts_${i}`},
      });
      finalCols.push({
        ...durCol,
        name: `dur_${i}`,
        type: durCol?.type ?? 'NA',
        checked: true,
        column: {name: `dur_${i}`},
      });
    }

    // First, identify which columns are duplicated across inputs
    const columnCounts = new Map<string, number>();
    for (const node of inputNodes) {
      if (node === undefined) continue;

      for (const col of node.finalCols) {
        if (
          col.name !== 'id' &&
          col.name !== 'ts' &&
          col.name !== 'dur' &&
          !seenColumns.has(col.name)
        ) {
          columnCounts.set(col.name, (columnCounts.get(col.name) ?? 0) + 1);
        }
      }
    }

    // Add only non-duplicated columns (columns that appear in exactly one input)
    for (const node of inputNodes) {
      if (node === undefined) continue;

      for (const col of node.finalCols) {
        if (
          col.name !== 'id' &&
          col.name !== 'ts' &&
          col.name !== 'dur' &&
          !seenColumns.has(col.name) &&
          columnCounts.get(col.name) === 1
        ) {
          finalCols.push({...col, checked: true});
          seenColumns.add(col.name);
        }
      }
    }

    return finalCols;
  }

  constructor(state: IntervalIntersectNodeState) {
    this.nodeId = nextNodeId();

    // Initialize filterNegativeDur array with true for each inputNode if not provided
    const filterNegativeDur = state.filterNegativeDur ?? [];
    // Fill missing indices with true (default to filtering enabled)
    for (let i = 0; i < state.inputNodes.length; i++) {
      if (filterNegativeDur[i] === undefined) {
        filterNegativeDur[i] = true;
      }
    }

    this.state = {
      ...state,
      autoExecute: state.autoExecute ?? false,
      filterNegativeDur,
    };
    this.secondaryInputs = {
      connections: new Map(),
      min: 2,
      max: 'unbounded',
    };
    // Initialize connections from state.inputNodes
    for (let i = 0; i < state.inputNodes.length; i++) {
      this.secondaryInputs.connections.set(i, state.inputNodes[i]);
    }
    this.nextNodes = [];
  }

  validate(): boolean {
    // Clear any previous errors at the start of validation
    if (this.state.issues) {
      this.state.issues.clear();
    }

    const inputNodes = this.inputNodesList;

    if (inputNodes.length < 2) {
      this.setValidationError(
        'Interval intersect node requires at least two inputs.',
      );
      return false;
    }

    for (const inputNode of inputNodes) {
      if (!inputNode.validate()) {
        this.setValidationError(
          inputNode.state.issues?.queryError?.message ??
            `Input node '${inputNode.getTitle()}' is invalid`,
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

    for (const inputNode of inputNodes) {
      if (!checkColumns(inputNode, ['id', 'ts', 'dur'])) return false;
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

  nodeInfo(): m.Children {
    return m(
      'div',
      m(
        'p',
        'Find intervals that overlap across all connected sources. All inputs are treated equally - returns intervals that exist in all sources simultaneously.',
      ),
      m(
        'p',
        m('strong', 'Required columns:'),
        ' All inputs must have ',
        m('code', 'id'),
        ', ',
        m('code', 'ts'),
        ', and ',
        m('code', 'dur'),
        ' columns.',
      ),
      m(
        'p',
        m('strong', 'Partition:'),
        ' Optionally partition the intersection by common columns (e.g., ',
        m('code', 'utid'),
        '). When partitioned, intervals are matched only within the same partition values.',
      ),
      m(
        'p',
        m('strong', 'Duplicate columns:'),
        " Columns that appear in multiple inputs will be excluded from the result. Use '+ -> Columns -> Modify' to rename conflicting columns before connecting.",
      ),
      m(
        'p',
        m('strong', 'Filter unfinished intervals:'),
        " Enable per-input to exclude intervals that haven't completed yet.",
      ),
      m(
        'p',
        m('strong', 'Example:'),
        ' Find CPU slices that occur during both a user gesture AND a network request.',
      ),
    );
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
      LabeledControl,
      {label: 'Partition by:'},
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
    // Initialize filterNegativeDur if it doesn't exist
    if (!this.state.filterNegativeDur) {
      this.state.filterNegativeDur = [];
    }

    // Compact filterNegativeDur array to match inputNodes length
    // When nodes are removed, inputNodes is compacted, so we need to match that
    if (this.state.filterNegativeDur.length > this.inputNodesList.length) {
      this.state.filterNegativeDur = this.state.filterNegativeDur.slice(
        0,
        this.inputNodesList.length,
      );
    }

    // Initialize missing indices with true (default to filtering enabled)
    for (let i = 0; i < this.inputNodesList.length; i++) {
      if (this.state.filterNegativeDur[i] === undefined) {
        this.state.filterNegativeDur[i] = true;
      }
    }
  }

  private checkDuplicateColumns(): string[] {
    const EXCLUDED_COLUMNS = new Set(['id', 'ts', 'dur']);
    const columnToInputs = new Map<string, number[]>();

    // Also exclude columns used for partitioning (they're intentionally in all inputs)
    const partitionColumns = new Set(this.state.partitionColumns ?? []);

    // Track which inputs each column appears in
    for (let i = 0; i < this.inputNodesList.length; i++) {
      const node = this.inputNodesList[i];
      const columns = node.finalCols.map((c) => c.name);
      for (const col of columns) {
        if (!EXCLUDED_COLUMNS.has(col) && !partitionColumns.has(col)) {
          const inputs = columnToInputs.get(col) ?? [];
          inputs.push(i);
          columnToInputs.set(col, inputs);
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

    if (this.inputNodesList.length === 0) return [];

    // Start with columns from the first input
    const firstNode = this.inputNodesList[0];
    const commonColumns = new Set(
      firstNode.finalCols
        .filter(
          (c) => !EXCLUDED_COLUMNS.has(c.name) && !EXCLUDED_TYPES.has(c.type),
        )
        .map((c) => c.name),
    );

    // Intersect with columns from remaining inputs
    for (let i = 1; i < this.inputNodesList.length; i++) {
      const node = this.inputNodesList[i];
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

    // Map inputNodes to UI elements with their indices
    const connectedInputs: Array<{node: QueryNode; index: number}> =
      this.inputNodesList.map((node, index) => ({node, index}));

    // If no inputs connected, show empty state
    if (connectedInputs.length === 0) {
      return m(
        '.pf-exp-query-operations',
        m(EmptyState, {
          icon: 'link_off',
          title: 'No inputs connected',
        }),
      );
    }

    return m(
      '.pf-exp-query-operations',
      error && m(Callout, {icon: 'error'}, error.message),
      m(IssueList, {
        icon: 'warning',
        title:
          "Duplicate columns will be excluded from the result. Use '+ -> Columns -> Modify' to rename them:",
        items: duplicateWarnings,
      }),
      this.renderPartitionSelector(false),
      connectedInputs.map(({node, index}) => {
        const filterEnabled = this.state.filterNegativeDur?.[index] ?? true;

        return m(ListItem, {
          key: node.nodeId,
          icon: 'input',
          name: `Input ${index}`,
          description: filterEnabled
            ? 'Filtering unfinished intervals'
            : 'Including all intervals',
          actions: [
            {
              icon: filterEnabled ? 'check_box' : 'check_box_outline_blank',
              title: 'Filter out intervals with negative duration',
              onclick: () => {
                if (!this.state.filterNegativeDur) {
                  this.state.filterNegativeDur = [];
                }
                this.state.filterNegativeDur[index] = !filterEnabled;
                this.state.onchange?.();
              },
            },
            {
              icon: 'view_column',
              title: 'Pick columns',
              onclick: () => {
                if (this.state.actions?.onInsertModifyColumnsNode) {
                  this.state.actions.onInsertModifyColumnsNode(index);
                }
              },
            },
          ],
        });
      }),
    );
  }

  clone(): QueryNode {
    const stateCopy: IntervalIntersectNodeState = {
      inputNodes: [...this.state.inputNodes],
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

    const sq = StructuredQueryBuilder.withIntervalIntersect(
      this.inputNodesList[0],
      this.inputNodesList.slice(1),
      this.state.partitionColumns,
      this.state.filterNegativeDur,
      this.nodeId,
    );

    if (sq === undefined) return undefined;

    // Add select_columns to explicitly specify which columns to return
    // This ensures we only expose the clean, well-defined columns from finalCols
    sq.selectColumns = this.finalCols.map((col) => {
      const selectCol = new protos.PerfettoSqlStructuredQuery.SelectColumn();
      selectCol.columnNameOrExpression = col.name;
      return selectCol;
    });

    return sq;
  }

  serializeState(): IntervalIntersectSerializedState {
    return {
      // Store ALL input node IDs (not just slice(1)) for reliable deserialization
      intervalNodes: this.inputNodesList
        .filter((n): n is QueryNode => n !== undefined)
        .map((n) => n.nodeId),
      comment: this.state.comment,
      filterNegativeDur: this.state.filterNegativeDur,
      partitionColumns: this.state.partitionColumns,
    };
  }

  static deserializeState(
    nodes: Map<string, QueryNode>,
    state: IntervalIntersectSerializedState,
  ): {
    inputNodes: QueryNode[];
    filterNegativeDur?: boolean[];
    partitionColumns?: string[];
  } {
    // Resolve all input nodes from their IDs
    const inputNodes = state.intervalNodes
      .map((id) => nodes.get(id))
      .filter((node): node is QueryNode => node !== undefined);
    return {
      inputNodes,
      filterNegativeDur: state.filterNegativeDur,
      partitionColumns: state.partitionColumns,
    };
  }
}
