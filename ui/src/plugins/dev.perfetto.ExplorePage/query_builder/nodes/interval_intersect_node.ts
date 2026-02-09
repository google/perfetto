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
  OutlinedField,
  MultiSelectOption,
  MultiSelectDiff,
} from '../widgets';
import {NodeModifyAttrs, NodeDetailsAttrs} from '../node_explorer_types';
import {NodeTitle, ColumnName} from '../node_styling_widgets';
import {loadNodeDoc} from '../node_doc_loader';
import {getCommonColumns} from '../utils';

export interface IntervalIntersectSerializedState {
  intervalNodes: string[];
  comment?: string;
  partitionColumns?: string[]; // Columns to partition by during interval intersection
  tsDurSource?: 'intersection' | number; // Source for ts/dur: 'intersection' or input index
}

export interface IntervalIntersectNodeState extends QueryNodeState {
  inputNodes: QueryNode[];
  partitionColumns?: string[]; // Columns to partition by during interval intersection
  tsDurSource?: 'intersection' | number; // Source for ts/dur: 'intersection' or input index
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

    // Add id column only when tsDurSource is a specific input (not intersection)
    // Intersection doesn't have a single id, but a specific input does
    const tsDurSource = this.state.tsDurSource ?? 'intersection';
    if (typeof tsDurSource === 'number') {
      // Get the id type from the selected input
      const sourceNode = inputNodes[tsDurSource];
      const sourceIdCol =
        sourceNode !== undefined
          ? this.getEffectiveCols(sourceNode).find((c) => c.name === 'id')
          : undefined;
      const idColumnType = sourceIdCol?.column.type;

      finalCols.push({
        name: 'id',
        type: sourceIdCol?.type ?? 'NA',
        checked: true,
        column: idColumnType ? {name: 'id', type: idColumnType} : {name: 'id'},
      });
      seenColumns.add('id');
    }

    // Add ts and dur columns - always present with same names,
    // but aliased from different sources based on tsDurSource
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
      const firstNodeCols =
        firstNode !== undefined ? this.getEffectiveCols(firstNode) : [];
      for (const colName of this.state.partitionColumns) {
        const sourceCol = firstNodeCols.find((c) => c.name === colName);

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

            const nodeCols = this.getEffectiveCols(node);
            const otherCol = nodeCols.find((c) => c.name === colName);
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

      const nodeCols = this.getEffectiveCols(node);
      for (const col of nodeCols) {
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

      const nodeCols = this.getEffectiveCols(node);
      for (const col of nodeCols) {
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
      const nodeCols = this.getEffectiveCols(node);
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

    this.state = {
      ...state,
      autoExecute: state.autoExecute ?? false,
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
      // Require standard interval columns (id, ts, dur)
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

  private renderPartitionSelector(): m.Child {
    // Initialize partition columns if needed
    if (!this.state.partitionColumns) {
      this.state.partitionColumns = [];
    }

    // Get common columns for partition selection
    const commonColumns = this.getCommonColumnsForPartition();

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

  private renderTsDurSourceSelector(): m.Child {
    const inputNodes = this.inputNodesList;
    if (inputNodes.length === 0) {
      return null;
    }

    const currentSource = this.state.tsDurSource ?? 'intersection';
    const currentValue =
      currentSource === 'intersection' ? 'intersection' : String(currentSource);

    // Build options: "Intersection" + one per input
    const options: m.Children = [
      m('option', {value: 'intersection'}, 'Intersection (no id)'),
    ];

    for (let i = 0; i < inputNodes.length; i++) {
      options.push(m('option', {value: String(i)}, `Input ${i}`));
    }

    return m(
      OutlinedField,
      {
        label: 'Return id, ts, dur from',
        value: currentValue,
        onchange: (e: Event) => {
          const target = e.target as HTMLSelectElement;
          const value = target.value;
          if (value === 'intersection') {
            this.state.tsDurSource = 'intersection';
          } else {
            this.state.tsDurSource = parseInt(value, 10);
          }
          notifyNextNodes(this);
          this.state.onchange?.();
        },
      },
      options,
    );
  }

  nodeDetails(): NodeDetailsAttrs {
    const details: m.Child[] = [NodeTitle(this.getTitle())];

    // Display id/ts/dur source (read-only)
    const tsDurSource = this.state.tsDurSource ?? 'intersection';
    const tsDurSourceLabel =
      tsDurSource === 'intersection' ? 'Intersection' : `Input ${tsDurSource}`;
    if (tsDurSource !== 'intersection') {
      details.push(m('div', `Return id, ts, dur from: ${tsDurSourceLabel}`));
    }

    // Display partition columns (read-only)
    if (this.state.partitionColumns && this.state.partitionColumns.length > 0) {
      details.push(
        m(
          'div',
          'Partition by: ',
          this.state.partitionColumns.map((col, index) => [
            ColumnName(col),
            index < this.state.partitionColumns!.length - 1 ? ', ' : '',
          ]),
        ),
      );
    }

    return {
      content: details,
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

  private getCommonColumnsForPartition(): string[] {
    if (this.inputNodesList.length === 0) return [];
    return getCommonColumns(
      this.inputNodesList.map((n) => n.finalCols),
      {
        excludedColumns: new Set(['id', 'ts', 'dur']),
        excludedTypes: new Set(['STRING', 'BYTES']),
      },
    );
  }

  nodeSpecificModify(): NodeModifyAttrs {
    this.validate();
    const error = this.state.issues?.queryError;
    const duplicateWarnings = this.checkDuplicateColumns();

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
    const partitionSelector = this.renderPartitionSelector();
    if (partitionSelector !== null) {
      sections.push({
        content: partitionSelector,
      });
    }

    // Add ts/dur source selector
    const tsDurSourceSelector = this.renderTsDurSourceSelector();
    if (tsDurSourceSelector !== null) {
      sections.push({
        content: tsDurSourceSelector,
      });
    }

    // Add input nodes section
    sections.push({
      content: connectedInputs.map(({node, index}) => {
        const isCounter = this.isCounterNode(node);

        // Build description based on state
        const description = isCounter
          ? 'Missing dur column - click below to convert to intervals'
          : '';

        // Build actions array
        const actions = [];

        // Add convert to intervals action if this is counter data
        if (isCounter) {
          actions.push({
            label: 'Convert to Intervals',
            onclick: () => {
              if (this.state.actions?.onInsertCounterToIntervalsNode) {
                this.state.actions.onInsertCounterToIntervalsNode(index);
              }
            },
          });
        }

        // Add pick columns action
        actions.push({
          icon: 'view_column',
          title: 'Pick columns',
          onclick: () => {
            if (this.state.actions?.onInsertModifyColumnsNode) {
              this.state.actions.onInsertModifyColumnsNode(index);
            }
          },
        });

        return m(ListItem, {
          key: node.nodeId,
          icon: 'input',
          name: `Input ${index}`,
          description,
          actions,
        });
      }),
    });

    return {
      info: 'Finds overlapping time intervals between inputs. Optionally partition the intersection by common columns (e.g., utid). When partitioned, intervals are matched only within the same partition values. Common columns are those that exist in all input tables, excluding id, ts, dur, and string/bytes types.',
      sections,
    };
  }

  // Check if a node is counter data (has id, ts, track_id, value but NOT dur)
  private isCounterNode(node: QueryNode): boolean {
    const cols = new Set(node.finalCols.map((c) => c.name));
    return (
      !cols.has('dur') &&
      cols.has('id') &&
      cols.has('ts') &&
      cols.has('track_id') &&
      cols.has('value')
    );
  }

  // Get the finalCols for a node
  private getEffectiveCols(node: QueryNode): ColumnInfo[] {
    return node.finalCols;
  }

  clone(): QueryNode {
    const stateCopy: IntervalIntersectNodeState = {
      inputNodes: [...this.state.inputNodes],
      partitionColumns: this.state.partitionColumns
        ? [...this.state.partitionColumns]
        : undefined,
      tsDurSource: this.state.tsDurSource,
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
      this.nodeId,
    );

    if (sq === undefined) return undefined;

    const tsDurSource = this.state.tsDurSource ?? 'intersection';

    // Build select_columns with aliasing based on tsDurSource
    sq.selectColumns = this.buildSelectColumns(tsDurSource);

    return sq;
  }

  /**
   * Build the select columns with appropriate aliasing based on tsDurSource.
   * When tsDurSource is 'intersection': ts/dur come from the intersection (no id)
   * When tsDurSource is a number: id/ts/dur come from that input (id_N/ts_N/dur_N)
   */
  private buildSelectColumns(
    tsDurSource: 'intersection' | number,
  ): protos.PerfettoSqlStructuredQuery.SelectColumn[] {
    const selectColumns: protos.PerfettoSqlStructuredQuery.SelectColumn[] = [];

    for (const col of this.finalCols) {
      const selectCol = new protos.PerfettoSqlStructuredQuery.SelectColumn();

      if (typeof tsDurSource === 'number' && col.name === 'id') {
        // Main id comes from the selected input
        selectCol.columnNameOrExpression = `id_${tsDurSource}`;
        selectCol.alias = 'id';
      } else if (typeof tsDurSource === 'number' && col.name === 'ts') {
        // Main ts comes from the selected input
        selectCol.columnNameOrExpression = `ts_${tsDurSource}`;
        selectCol.alias = 'ts';
      } else if (typeof tsDurSource === 'number' && col.name === 'dur') {
        // Main dur comes from the selected input
        selectCol.columnNameOrExpression = `dur_${tsDurSource}`;
        selectCol.alias = 'dur';
      } else {
        // All other columns pass through unchanged
        selectCol.columnNameOrExpression = col.name;
      }

      selectColumns.push(selectCol);
    }

    return selectColumns;
  }

  serializeState(): IntervalIntersectSerializedState {
    return {
      // Store ALL input node IDs (not just slice(1)) for reliable deserialization
      intervalNodes: this.inputNodesList
        .filter((n): n is QueryNode => n !== undefined)
        .map((n) => n.nodeId),
      partitionColumns: this.state.partitionColumns,
      tsDurSource: this.state.tsDurSource,
    };
  }

  static deserializeState(
    state: IntervalIntersectSerializedState,
  ): IntervalIntersectNodeState {
    return {
      inputNodes: [],
      partitionColumns: state.partitionColumns,
      tsDurSource: state.tsDurSource,
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
