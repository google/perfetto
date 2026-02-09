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

/**
 * Filter During Node - Temporal Interval Filtering
 *
 * This node filters intervals from a primary input to only those that occurred
 * during intervals from a secondary input (called "Filter intervals"). The output
 * preserves all columns from the primary input, with ts and dur values representing
 * the actual overlap period.
 *
 * ## Architecture
 *
 * The node uses ExperimentalFilterToIntervals which:
 * - Automatically merges overlapping intervals in the filter set
 * - Preserves the base query's output schema
 * - Handles clip_to_intervals setting for ts/dur output
 * - Optionally filters out negative duration intervals (unfinished events)
 *
 * ## Required Columns
 *
 * Primary input must have:
 *   - id: Unique identifier for the interval
 *   - ts: Timestamp (start time)
 *   - dur: Duration
 *
 * Filter intervals input must have:
 *   - ts: Timestamp (start time)
 *   - dur: Duration
 *
 * ## Example Use Cases
 *
 * - Filter CPU slices to only those during app startup
 * - Filter memory allocations during specific user interactions
 * - Filter thread states during performance-critical time windows
 *
 * ## Output Behavior
 *
 * Overlapping intervals in the filter set are merged before filtering.
 * If a primary interval overlaps with multiple non-overlapping intervals
 * from the filter set, multiple output rows will be produced (one for each).
 * Each output row shows the actual overlap period (when clipToIntervals is true).
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
import {
  ListItem,
  LabeledControl,
  OutlinedMultiSelect,
  MultiSelectOption,
  MultiSelectDiff,
} from '../widgets';
import {Switch} from '../../../../widgets/switch';
import {NodeModifyAttrs, NodeDetailsAttrs} from '../node_explorer_types';
import {NodeDetailsMessage, ColumnName} from '../node_styling_widgets';
import {notifyNextNodes} from '../graph_utils';
import {getCommonColumns} from '../utils';

export interface FilterDuringNodeState extends QueryNodeState {
  partitionColumns?: string[]; // Columns to partition by during interval intersection
  clipToIntervals?: boolean; // When true (default), use intersected ts/dur; when false, use original ts/dur from primary
}

export class FilterDuringNode implements QueryNode {
  readonly nodeId: string;
  readonly type = NodeType.kFilterDuring;
  primaryInput?: QueryNode;
  secondaryInputs: SecondaryInputSpec;
  nextNodes: QueryNode[];
  readonly state: FilterDuringNodeState;

  constructor(state: FilterDuringNodeState) {
    this.nodeId = nextNodeId();
    this.state = state;
    this.secondaryInputs = {
      connections: new Map(),
      min: 1,
      max: 1,
      portNames: ['Filter intervals'],
    };
    this.nextNodes = [];
    this.state.autoExecute = this.state.autoExecute ?? false;
  }

  // Get the node connected to the secondary input port (the intervals to filter during)
  get secondaryNodes(): QueryNode[] {
    return Array.from(this.secondaryInputs.connections.values());
  }

  get finalCols(): ColumnInfo[] {
    // Return the same columns as the primary input
    // Partition columns are preserved through the interval intersection
    return this.primaryInput?.finalCols ?? [];
  }

  getTitle(): string {
    return 'Filter During';
  }

  nodeDetails(): NodeDetailsAttrs {
    const details: m.Child[] = [
      this.state.clipToIntervals
        ? NodeDetailsMessage(
            'Filters time to intervals that occurred during input intervals.',
          )
        : NodeDetailsMessage(
            'Filters rows to intervals that occurred during input intervals. Retains original timestamps and durations.',
          ),
    ];

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

  private getCommonColumnsForPartition(): string[] {
    if (this.primaryInput === undefined || this.secondaryNodes.length === 0) {
      return [];
    }
    const columnArrays = [
      this.primaryInput.finalCols,
      ...this.secondaryNodes.map((n) => n.finalCols),
    ];
    return getCommonColumns(columnArrays, {
      excludedColumns: new Set(['id', 'ts', 'dur']),
      excludedTypes: new Set(['STRING', 'BYTES']),
    });
  }

  private cleanupPartitionColumns(): void {
    if (
      !this.state.partitionColumns ||
      this.state.partitionColumns.length === 0
    ) {
      return;
    }

    const commonColumns = new Set(this.getCommonColumnsForPartition());

    // Remove partition columns that no longer exist in all inputs
    const validPartitionCols = this.state.partitionColumns.filter((colName) =>
      commonColumns.has(colName),
    );

    if (validPartitionCols.length !== this.state.partitionColumns.length) {
      const removed = this.state.partitionColumns.filter(
        (c) => !validPartitionCols.includes(c),
      );
      console.warn(
        `[FilterDuring] Removing partition columns no longer available in all inputs: ${removed.join(', ')}`,
      );
      this.state.partitionColumns = validPartitionCols;
    }
  }

  private renderPartitionSelector(): m.Child {
    // Initialize partition columns if needed
    if (!this.state.partitionColumns) {
      this.state.partitionColumns = [];
    }

    // Get common columns for partition selection
    const commonColumns = this.getCommonColumnsForPartition();
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

  nodeSpecificModify(): NodeModifyAttrs {
    // Run validation to populate error state
    this.validate();
    const error = this.state.issues?.queryError;

    const secondaryNodes = this.secondaryNodes;

    // If no secondary input connected, show empty state
    if (secondaryNodes.length === 0) {
      return {
        info: 'Filters the primary input to only show intervals that occurred during the intervals from the secondary input.',
        sections: [
          {
            content: m(EmptyState, {
              icon: 'link_off',
              title: 'No filter intervals connected',
              detail:
                'Connect a node to the left port that provides intervals (must have ts, dur columns).',
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

    // Build info text (first section after error)
    const infoText =
      'Filters the primary input to only show intervals that occurred during the intervals from the secondary input.';

    // Get clipToIntervals for use in switch below
    const clipToIntervals = this.state.clipToIntervals ?? true;

    // Add partition selector
    const partitionSelector = this.renderPartitionSelector();
    if (partitionSelector !== null) {
      sections.push({
        content: partitionSelector,
      });
    }

    // Add "Clip to intervals" switch
    sections.push({
      content: m(
        '.pf-filter-during-clip-row',
        m(Switch, {
          checked: clipToIntervals,
          label: clipToIntervals
            ? 'Clip to intervals (use intersected ts/dur)'
            : 'Use original timestamps (from primary input)',
          onchange: () => {
            this.state.clipToIntervals = !clipToIntervals;
            this.state.onchange?.();
          },
        }),
      ),
    });

    // Add primary input section
    sections.push({
      content: m(ListItem, {
        icon: 'input',
        name: 'Primary Input',
        description: '',
        actions: [],
      }),
    });

    // Add filter intervals input section
    sections.push({
      content: m(ListItem, {
        icon: 'input',
        name: 'Filter intervals',
        description: '',
        actions: [],
      }),
    });

    return {
      info: infoText,
      sections,
    };
  }

  nodeInfo(): m.Children {
    return loadNodeDoc('filter_during');
  }

  validate(): boolean {
    // Clear any previous errors at the start of validation
    if (this.state.issues) {
      this.state.issues.clear();
    }

    if (this.primaryInput === undefined) {
      setValidationError(
        this.state,
        'Connect a node to be filtered to the top port',
      );
      return false;
    }

    if (!this.primaryInput.validate()) {
      setValidationError(this.state, 'Node to be filtered is invalid');
      return false;
    }

    const secondaryInput = this.secondaryInputs.connections.get(0);
    if (secondaryInput === undefined) {
      setValidationError(
        this.state,
        'Connect a node with intervals to the port on the left',
      );
      return false;
    }

    // Validate the secondary input
    if (!secondaryInput.validate()) {
      const childError =
        secondaryInput.state.issues?.queryError !== undefined
          ? `: ${secondaryInput.state.issues.queryError.message}`
          : '';
      setValidationError(
        this.state,
        `Filter intervals input is invalid${childError}`,
      );
      return false;
    }

    // Check that primary input has required columns
    const primaryCols = new Set(this.primaryInput.finalCols.map((c) => c.name));
    const requiredCols = ['id', 'ts', 'dur'];
    const missingPrimary = requiredCols.filter((c) => !primaryCols.has(c));
    if (missingPrimary.length > 0) {
      setValidationError(
        this.state,
        `Node to be filtered is missing required columns: ${missingPrimary.join(', ')}`,
      );
      return false;
    }

    // Check that the secondary input has required columns (id is optional)
    const secondaryCols = new Set(secondaryInput.finalCols.map((c) => c.name));
    const requiredSecondaryColumns = ['ts', 'dur'];
    const missingSecondary = requiredSecondaryColumns.filter(
      (c) => !secondaryCols.has(c),
    );
    if (missingSecondary.length > 0) {
      setValidationError(
        this.state,
        `Filter intervals input is missing required columns: ${missingSecondary.join(', ')}`,
      );
      return false;
    }

    return true;
  }

  clone(): QueryNode {
    const stateCopy: FilterDuringNodeState = {
      partitionColumns: this.state.partitionColumns
        ? [...this.state.partitionColumns]
        : undefined,
      clipToIntervals: this.state.clipToIntervals,
      filters: this.state.filters?.map((f) => ({...f})),
      filterOperator: this.state.filterOperator,
      onchange: this.state.onchange,
    };
    return new FilterDuringNode(stateCopy);
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (!this.validate()) return undefined;
    if (this.primaryInput === undefined) return undefined;

    const secondaryInput = this.secondaryInputs.connections.get(0);
    if (secondaryInput === undefined) return undefined;

    // Build selectColumns with proper ordering.
    // When clip_to_intervals is true, we need ts and dur first so the C++ side
    // can prepend clipped ts/dur and append original_ts/original_dur at the end.
    // When clip_to_intervals is false, the order doesn't matter as much, but
    // we still put ts and dur first for consistency.
    const clipToIntervals = this.state.clipToIntervals ?? true;
    const allColumns = this.primaryInput.finalCols.map((c) => c.name);

    let selectColumns: string[];
    if (clipToIntervals) {
      // When clipping: ts and dur must be first, then other columns
      // C++ will output: ii.ts, ii.dur, <other cols>, original_ts, original_dur
      const tsIndex = allColumns.indexOf('ts');
      const durIndex = allColumns.indexOf('dur');
      const otherCols = allColumns.filter((c) => c !== 'ts' && c !== 'dur');

      selectColumns = [];
      if (tsIndex >= 0) selectColumns.push('ts');
      if (durIndex >= 0) selectColumns.push('dur');
      selectColumns.push(...otherCols);
    } else {
      // When not clipping: preserve original order (ts/dur will use base values)
      selectColumns = allColumns;
    }

    return StructuredQueryBuilder.withFilterToIntervals(
      this.primaryInput,
      secondaryInput,
      this.state.partitionColumns,
      this.state.clipToIntervals ?? true,
      this.nodeId,
      selectColumns,
    );
  }

  serializeState(): object {
    return {
      primaryInputId: this.primaryInput?.nodeId,
      secondaryInputNodeIds: Array.from(
        this.secondaryInputs.connections.values(),
      ).map((node) => node.nodeId),
      partitionColumns: this.state.partitionColumns,
      clipToIntervals: this.state.clipToIntervals,
    };
  }

  static deserializeState(
    serializedState: FilterDuringNodeState,
  ): FilterDuringNodeState {
    return {
      partitionColumns: serializedState.partitionColumns,
      clipToIntervals: serializedState.clipToIntervals,
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

  // Called when a node is connected/disconnected to secondary inputs
  onPrevNodesUpdated(): void {
    // Validate and clean up partition columns
    this.cleanupPartitionColumns();

    // Notify next nodes that our columns have changed
    notifyNextNodes(this);
    this.state.onchange?.();
    m.redraw();
  }
}
