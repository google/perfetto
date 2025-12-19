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
import {notifyNextNodes} from '../graph_utils';
import protos from '../../../../protos';
import {ColumnInfo} from '../column_info';
import {PerfettoSqlTypes} from '../../../../trace_processor/perfetto_sql_type';
import {Callout} from '../../../../widgets/callout';
import {EmptyState} from '../../../../widgets/empty_state';
import {NodeIssues} from '../node_issues';
import {StructuredQueryBuilder} from '../structured_query_builder';
import {
  LabeledControl,
  IssueList,
  ListItem,
  OutlinedMultiSelect,
  MultiSelectOption,
  MultiSelectDiff,
} from '../widgets';
import {NodeModifyAttrs, NodeDetailsAttrs} from '../node_explorer_types';
import {NodeTitle} from '../node_styling_widgets';
import {loadNodeDoc} from '../node_doc_loader';

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
    // These have well-defined types: ts is TIMESTAMP, dur is DURATION
    finalCols.push({
      name: 'ts',
      type: 'TIMESTAMP',
      checked: true,
      column: {name: 'ts', type: PerfettoSqlTypes.TIMESTAMP},
    });
    finalCols.push({
      name: 'dur',
      type: 'DURATION',
      checked: true,
      column: {name: 'dur', type: PerfettoSqlTypes.DURATION},
    });
    seenColumns.add('ts');
    seenColumns.add('dur');

    // Add partition columns (without suffix)
    // Partition columns preserve their type from the first input node
    if (this.state.partitionColumns) {
      const firstNode = inputNodes[0];
      for (const colName of this.state.partitionColumns) {
        const sourceCol = firstNode?.finalCols.find((c) => c.name === colName);

        // Validate that partition column types match across all input nodes
        // Skip validation if type is unknown/NA
        const sourceType = sourceCol?.type;
        if (sourceType && sourceType !== 'NA' && sourceType !== 'UNKNOWN') {
          for (let i = 1; i < inputNodes.length; i++) {
            const node = inputNodes[i];
            if (node === undefined) {
              console.warn(
                `[IntervalIntersect] Input node ${i} is undefined, skipping type validation`,
              );
              continue;
            }

            const otherCol = node.finalCols.find((c) => c.name === colName);
            const otherType = otherCol?.type;

            // Only warn if both types are known and different
            if (
              otherType &&
              otherType !== 'NA' &&
              otherType !== 'UNKNOWN' &&
              otherType !== sourceType
            ) {
              console.warn(
                `[IntervalIntersect] Partition column "${colName}" has inconsistent types: ` +
                  `node 0 has "${sourceType}", node ${i} has "${otherType}". ` +
                  `Using type from first node.`,
              );
            }
          }
        }

        // Create column with explicit type handling
        const columnType = sourceCol?.column.type;
        finalCols.push({
          name: colName,
          type: sourceCol?.type ?? 'NA',
          checked: true,
          column: columnType
            ? {name: colName, type: columnType}
            : {name: colName},
        });
        seenColumns.add(colName);
      }
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

    // For each input node, add id_N, ts_N, dur_N
    for (let i = 0; i < inputNodes.length; i++) {
      const node = inputNodes[i];
      if (node === undefined) continue;

      // Find the actual column info for id to get its type
      const nodeCols = node.finalCols;
      const idCol = nodeCols.find((c) => c.name === 'id');

      // Create id_N column with explicit type handling
      const idColumnType = idCol?.column.type;
      finalCols.push({
        name: `id_${i}`,
        type: idCol?.type ?? 'NA',
        checked: true,
        column: idColumnType
          ? {name: `id_${i}`, type: idColumnType}
          : {name: `id_${i}`},
      });
      // ts_N columns are TIMESTAMP type
      finalCols.push({
        name: `ts_${i}`,
        type: 'TIMESTAMP',
        checked: true,
        column: {name: `ts_${i}`, type: PerfettoSqlTypes.TIMESTAMP},
      });
      // dur_N columns are DURATION type
      finalCols.push({
        name: `dur_${i}`,
        type: 'DURATION',
        checked: true,
        column: {name: `dur_${i}`, type: PerfettoSqlTypes.DURATION},
      });
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
      max: 6,
      portNames: (portIndex: number) => `Input ${portIndex}`,
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

    // Validate partition columns exist in all inputs
    if (this.state.partitionColumns && this.state.partitionColumns.length > 0) {
      for (const partitionCol of this.state.partitionColumns) {
        for (let i = 0; i < inputNodes.length; i++) {
          const node = inputNodes[i];
          const cols = new Set(node.finalCols.map((c) => c.name));
          if (!cols.has(partitionCol)) {
            this.setValidationError(
              `Partition column '${partitionCol}' is missing from Input ${i}. Please remove the partitioning or ensure all inputs have this column.`,
            );
            return false;
          }
        }
      }
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
    return loadNodeDoc('interval_intersect');
  }

  private renderPartitionSelector(compact: boolean = false): m.Child {
    // Initialize partition columns if needed
    if (!this.state.partitionColumns) {
      this.state.partitionColumns = [];
    }

    // Get common columns for partition selection
    const commonColumns = this.getCommonColumns();

    // Build options: include both common columns AND currently selected partition columns
    // This ensures we show invalid partition columns so the user can deselect them
    const allPartitionOptions = new Set([
      ...commonColumns,
      ...(this.state.partitionColumns ?? []),
    ]);

    // If there are no options at all (no common columns and no partitions set), don't show
    if (allPartitionOptions.size === 0) {
      return null;
    }

    const partitionOptions: MultiSelectOption[] = Array.from(
      allPartitionOptions,
    ).map((col) => ({
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
      m(OutlinedMultiSelect, {
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
          // Notify downstream nodes about the column change
          notifyNextNodes(this);
          this.state.onchange?.();
        },
      }),
    );
  }

  nodeDetails(): NodeDetailsAttrs {
    return {
      content: [NodeTitle(this.getTitle()), this.renderPartitionSelector(true)],
    };
  }

  private cleanupPartitionColumns(): void {
    if (
      !this.state.partitionColumns ||
      this.state.partitionColumns.length === 0
    ) {
      return;
    }

    const inputNodes = this.inputNodesList;
    if (inputNodes.length === 0) {
      if (this.state.partitionColumns.length > 0) {
        console.warn(
          '[IntervalIntersect] Clearing partition columns - no input nodes available',
        );
        this.state.partitionColumns = [];
      }
      return;
    }

    // Don't automatically remove partition columns that become invalid.
    // Instead, keep them and let validation fail so the user sees the error
    // and can manually remove the partitioning.
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

    // Validate and clean up partition columns
    this.cleanupPartitionColumns();

    // Notify next nodes that our columns have changed
    notifyNextNodes(this);
    this.state.onchange?.();
    m.redraw();
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

  nodeSpecificModify(): NodeModifyAttrs {
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
      return {
        info: 'Finds overlapping time intervals between inputs. Optionally partition the intersection by common columns (e.g., utid). When partitioned, intervals are matched only within the same partition values. Common columns are those that exist in all input tables, excluding id, ts, dur, and string/bytes types.',
        sections: [
          {
            content: m(EmptyState, {
              icon: 'link_off',
              title: 'No inputs connected',
            }),
          },
        ],
      };
    }

    const sections: NodeModifyAttrs['sections'] = [];

    // Add error if present
    if (error) {
      sections.push({
        content: m(Callout, {icon: 'error'}, error.message),
      });
    }

    // Add duplicate warnings if present
    if (duplicateWarnings.length > 0) {
      sections.push({
        content: m(IssueList, {
          icon: 'warning',
          title:
            "Duplicate columns will be excluded from the result. Use '+ -> Columns -> Modify' to rename them:",
          items: duplicateWarnings,
        }),
      });
    }

    // Add partition selector
    const partitionSelector = this.renderPartitionSelector(false);
    if (partitionSelector !== null) {
      sections.push({
        content: partitionSelector,
      });
    }

    // Add input nodes section
    sections.push({
      content: connectedInputs.map(({node, index}) => {
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
    });

    return {
      info: 'Finds overlapping time intervals between inputs. Optionally partition the intersection by common columns (e.g., utid). When partitioned, intervals are matched only within the same partition values. Common columns are those that exist in all input tables, excluding id, ts, dur, and string/bytes types.',
      sections,
    };
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
      filterNegativeDur: this.state.filterNegativeDur,
      partitionColumns: this.state.partitionColumns,
    };
  }

  static deserializeState(
    state: IntervalIntersectSerializedState,
  ): IntervalIntersectNodeState {
    return {
      inputNodes: [],
      filterNegativeDur: state.filterNegativeDur,
      partitionColumns: state.partitionColumns,
    };
  }

  static deserializeConnections(
    nodes: Map<string, QueryNode>,
    state: IntervalIntersectSerializedState,
  ): {inputNodes: QueryNode[]} {
    // Resolve all input nodes from their IDs
    const inputNodes = state.intervalNodes
      .map((id) => nodes.get(id))
      .filter((node): node is QueryNode => node !== undefined);
    return {
      inputNodes,
    };
  }
}
