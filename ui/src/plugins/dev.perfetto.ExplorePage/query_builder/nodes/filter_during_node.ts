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
 * The node uses a multi-step SQL query generation approach:
 *
 * 1. **Wrap Secondary for Column Selection**:
 *    - Select only id, ts, dur columns from the filter intervals input
 *    - Avoids column name conflicts in the interval intersection
 *
 * 2. **Interval Intersection**:
 *    - Use StructuredQueryBuilder.withIntervalIntersect()
 *    - Computes overlaps between primary and filter intervals
 *    - Optionally filters out negative duration intervals (unfinished events)
 *
 * 3. **Column Reshaping**:
 *    - Maps the intersection output back to primary input's schema
 *    - id comes from id_0 (primary's id)
 *    - ts and dur are the intersected values
 *    - All other primary columns are preserved as-is
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
 *   - id: (optional) If not present, a dummy id of 0 will be used
 *
 * ## Example Use Cases
 *
 * - Filter CPU slices to only those during app startup
 * - Filter memory allocations during specific user interactions
 * - Filter thread states during performance-critical time windows
 *
 * ## Output Behavior
 *
 * If a primary interval overlaps with multiple interval rows from the filter
 * intervals input, multiple output rows will be produced (one for each overlap).
 * Each output row shows the actual overlap period (intersected ts/dur values).
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
import {StructuredQueryBuilder, ColumnSpec} from '../structured_query_builder';
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
import {NodeDetailsMessage} from '../node_styling_widgets';
import {notifyNextNodes} from '../graph_utils';

export interface FilterDuringNodeState extends QueryNodeState {
  filterNegativeDurPrimary?: boolean; // Filter negative durations in primary input
  filterNegativeDurSecondary?: boolean; // Filter negative durations in secondary input
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
    this.state.filterNegativeDurPrimary =
      this.state.filterNegativeDurPrimary ?? true;
    this.state.filterNegativeDurSecondary =
      this.state.filterNegativeDurSecondary ?? true;
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
    return {
      content: [
        NodeDetailsMessage(
          'Filters only to intervals that occurred during input.',
        ),
        this.renderPartitionSelector(true),
      ],
    };
  }

  private getCommonColumns(): string[] {
    const EXCLUDED_COLUMNS = new Set(['id', 'ts', 'dur']);
    const EXCLUDED_TYPES = new Set(['STRING', 'BYTES']);

    // Need both primary input and at least one secondary input
    if (this.primaryInput === undefined || this.secondaryNodes.length === 0) {
      return [];
    }

    // Start with columns from the primary input
    const commonColumns = new Set(
      this.primaryInput.finalCols
        .filter(
          (c) => !EXCLUDED_COLUMNS.has(c.name) && !EXCLUDED_TYPES.has(c.type),
        )
        .map((c) => c.name),
    );

    // Intersect with columns from all secondary inputs
    for (const node of this.secondaryNodes) {
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

  private cleanupPartitionColumns(): void {
    if (
      !this.state.partitionColumns ||
      this.state.partitionColumns.length === 0
    ) {
      return;
    }

    const commonColumns = new Set(this.getCommonColumns());

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
    const partitionSelector = this.renderPartitionSelector(false);
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

    // Add filter toggle for primary input
    const primaryFilterEnabled = this.state.filterNegativeDurPrimary ?? true;
    sections.push({
      content: m(ListItem, {
        icon: 'input',
        name: 'Primary Input',
        description: primaryFilterEnabled
          ? 'Filtering unfinished intervals'
          : 'Including all intervals',
        actions: [
          {
            icon: primaryFilterEnabled
              ? 'check_box'
              : 'check_box_outline_blank',
            title: 'Filter out intervals with negative duration',
            onclick: () => {
              this.state.filterNegativeDurPrimary = !primaryFilterEnabled;
              this.state.onchange?.();
            },
          },
        ],
      }),
    });

    // Add filter intervals input section
    const secondaryFilterEnabled =
      this.state.filterNegativeDurSecondary ?? true;
    sections.push({
      content: m(ListItem, {
        icon: 'input',
        name: 'Filter intervals',
        description: secondaryFilterEnabled
          ? 'Filtering unfinished intervals'
          : 'Including all intervals',
        actions: [
          {
            icon: secondaryFilterEnabled
              ? 'check_box'
              : 'check_box_outline_blank',
            title: 'Filter out intervals with negative duration',
            onclick: () => {
              this.state.filterNegativeDurSecondary = !secondaryFilterEnabled;
              this.state.onchange?.();
            },
          },
        ],
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
      filterNegativeDurPrimary: this.state.filterNegativeDurPrimary,
      filterNegativeDurSecondary: this.state.filterNegativeDurSecondary,
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

    // Step 1: Get the secondary input's query
    const secondaryQuery = secondaryInput.getStructuredQuery();
    if (secondaryQuery === undefined) return undefined;

    // Step 2: Wrap the secondary to select id, ts, dur, and partition columns
    // This avoids column conflicts in the interval intersection while preserving partition columns
    // If secondary input doesn't have an id column, add a dummy id of 0
    const secondaryHasId = secondaryInput.finalCols.some(
      (c) => c.name === 'id',
    );
    const secondaryColumnsOnly: ColumnSpec[] = [
      secondaryHasId
        ? {columnNameOrExpression: 'id'}
        : {columnNameOrExpression: '0', alias: 'id'},
      {columnNameOrExpression: 'ts'},
      {columnNameOrExpression: 'dur'},
      // Add partition columns so they're available for interval intersect
      ...(this.state.partitionColumns ?? []).map((col) => ({
        columnNameOrExpression: col,
      })),
    ];

    // Create a temporary QueryNode wrapper for the secondary query
    const secondaryNodeWrapper: QueryNode = {
      nodeId: `${this.nodeId}_secondary`,
      type: NodeType.kSqlSource,
      nextNodes: [],
      state: this.state,
      finalCols: [
        {
          name: 'id',
          type: 'NA',
          checked: true,
          column: {name: 'id'},
        },
        {
          name: 'ts',
          type: 'TIMESTAMP',
          checked: true,
          column: {name: 'ts'},
        },
        {
          name: 'dur',
          type: 'DURATION',
          checked: true,
          column: {name: 'dur'},
        },
      ],
      getTitle: () => 'Filter Intervals',
      validate: () => true,
      clone: () => secondaryNodeWrapper,
      getStructuredQuery: () => secondaryQuery,
      nodeInfo: () => null,
      nodeDetails: () => ({content: null}),
      nodeSpecificModify: () => ({sections: []}),
      serializeState: () => ({}),
    };

    const wrappedSecondary = StructuredQueryBuilder.withSelectColumns(
      secondaryNodeWrapper,
      secondaryColumnsOnly,
      undefined,
      `${this.nodeId}_secondary_wrap`,
    );

    if (wrappedSecondary === undefined) return undefined;

    // Create a temporary QueryNode wrapper for the wrapped secondary query
    // This is needed because withIntervalIntersect expects QuerySource (QueryNode | undefined)
    const wrappedSecondaryNode: QueryNode = {
      nodeId: `${this.nodeId}_secondary_temp`,
      type: NodeType.kSqlSource, // Doesn't matter, just needs to be a valid type
      nextNodes: [],
      state: this.state,
      finalCols: [
        {
          name: 'id',
          type: 'NA',
          checked: true,
          column: {name: 'id'},
        },
        {
          name: 'ts',
          type: 'TIMESTAMP',
          checked: true,
          column: {name: 'ts'},
        },
        {
          name: 'dur',
          type: 'DURATION',
          checked: true,
          column: {name: 'dur'},
        },
        // Add partition columns
        ...(this.state.partitionColumns ?? []).map((col) => ({
          name: col,
          type: 'NA' as const,
          checked: true,
          column: {name: col},
        })),
      ],
      getTitle: () => 'Wrapped Secondary',
      validate: () => true,
      clone: () => wrappedSecondaryNode,
      getStructuredQuery: () => wrappedSecondary,
      nodeInfo: () => null,
      nodeDetails: () => ({content: null}),
      nodeSpecificModify: () => ({sections: []}),
      serializeState: () => ({}),
    };

    // Step 3: Build interval intersect with filterNegativeDur and partition columns
    const filterNegativeDur = [
      this.state.filterNegativeDurPrimary ?? true,
      this.state.filterNegativeDurSecondary ?? true,
    ];

    const intervalIntersectQuery = StructuredQueryBuilder.withIntervalIntersect(
      this.primaryInput,
      [wrappedSecondaryNode],
      this.state.partitionColumns, // Partition columns from state
      filterNegativeDur,
      `${this.nodeId}_intersect`,
    );

    if (intervalIntersectQuery === undefined) return undefined;

    // Step 4: Select columns to match primary input's schema
    // IntervalIntersect returns: ts, dur (intersected), id_0, ts_0, dur_0, id_1, ts_1, dur_1, plus other primary columns
    // Depending on clipToIntervals setting:
    //   - true (default): Use intersected ts/dur
    //   - false: Use original ts_0/dur_0 from primary
    const clipToIntervals = this.state.clipToIntervals ?? true;
    const selectColumns: ColumnSpec[] = this.primaryInput.finalCols.map(
      (col) => {
        if (col.name === 'id') {
          // Use id_0 (from primary) and alias it back to 'id'
          return {columnNameOrExpression: 'id_0', alias: 'id'};
        } else if (col.name === 'ts') {
          // Use intersected ts or original ts_0 based on clipToIntervals setting
          return clipToIntervals
            ? {columnNameOrExpression: 'ts'}
            : {columnNameOrExpression: 'ts_0', alias: 'ts'};
        } else if (col.name === 'dur') {
          // Use intersected dur or original dur_0 based on clipToIntervals setting
          return clipToIntervals
            ? {columnNameOrExpression: 'dur'}
            : {columnNameOrExpression: 'dur_0', alias: 'dur'};
        } else {
          // Use the column as-is (IntervalIntersect preserves unique columns from primary)
          return {columnNameOrExpression: col.name};
        }
      },
    );

    // Create a temporary QueryNode wrapper for the interval intersect query
    const intervalIntersectNode: QueryNode = {
      nodeId: `${this.nodeId}_intersect_temp`,
      type: NodeType.kIntervalIntersect,
      nextNodes: [],
      state: this.state,
      finalCols: [], // Not needed for this temporary node
      getTitle: () => 'Interval Intersect',
      validate: () => true,
      clone: () => intervalIntersectNode,
      getStructuredQuery: () => intervalIntersectQuery,
      nodeInfo: () => null,
      nodeDetails: () => ({content: null}),
      nodeSpecificModify: () => ({sections: []}),
      serializeState: () => ({}),
    };

    // Step 5: Wrap with SELECT to reshape columns
    return StructuredQueryBuilder.withSelectColumns(
      intervalIntersectNode,
      selectColumns,
      undefined,
      this.nodeId,
    );
  }

  serializeState(): object {
    return {
      primaryInputId: this.primaryInput?.nodeId,
      secondaryInputNodeIds: Array.from(
        this.secondaryInputs.connections.values(),
      ).map((node) => node.nodeId),
      filterNegativeDurPrimary: this.state.filterNegativeDurPrimary,
      filterNegativeDurSecondary: this.state.filterNegativeDurSecondary,
      partitionColumns: this.state.partitionColumns,
      clipToIntervals: this.state.clipToIntervals,
    };
  }

  static deserializeState(
    serializedState: FilterDuringNodeState,
  ): FilterDuringNodeState {
    return {
      filterNegativeDurPrimary: serializedState.filterNegativeDurPrimary,
      filterNegativeDurSecondary: serializedState.filterNegativeDurSecondary,
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
