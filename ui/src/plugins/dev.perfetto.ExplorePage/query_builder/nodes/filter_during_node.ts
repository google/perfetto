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
 * during intervals from one or more secondary inputs. The output preserves all
 * columns from the primary input, with ts and dur values representing the
 * actual overlap period.
 *
 * ## Architecture
 *
 * The node uses a multi-step SQL query generation approach:
 *
 * 1. **Union Secondary Inputs** (if multiple):
 *    - If only 1 secondary input: use it directly
 *    - If 2+ secondary inputs: combine them via UNION ALL
 *    - This creates a single set of intervals from all secondary sources
 *
 * 2. **Wrap Secondary for Column Selection**:
 *    - Select only id, ts, dur columns from the combined secondary
 *    - Avoids column name conflicts in the interval intersection
 *
 * 3. **Interval Intersection**:
 *    - Use StructuredQueryBuilder.withIntervalIntersect()
 *    - Computes overlaps between primary and secondary intervals
 *    - Optionally filters out negative duration intervals (unfinished events)
 *
 * 4. **Column Reshaping**:
 *    - Maps the intersection output back to primary input's schema
 *    - id comes from id_0 (primary's id)
 *    - ts and dur are the intersected values
 *    - All other primary columns are preserved as-is
 *
 * ## Multiple Secondary Inputs
 *
 * When multiple secondary inputs are connected, they are combined via UNION ALL
 * before performing the interval intersection. This means an interval from the
 * primary input is kept if it overlaps with ANY interval from ANY of the
 * secondary sources.
 *
 * Example:
 *   Secondary Input 1: App startup intervals
 *   Secondary Input 2: User interaction intervals
 *   Result: Primary intervals that occurred during either startup OR interactions
 *
 * ## Required Columns
 *
 * All inputs (primary and all secondaries) must have:
 *   - id: Unique identifier for the interval
 *   - ts: Timestamp (start time)
 *   - dur: Duration
 *
 * ## Example Use Cases
 *
 * - Filter CPU slices to only those during app startup
 * - Filter memory allocations during specific user interactions
 * - Filter thread states during multiple performance-critical time windows
 * - Combine multiple time ranges (e.g., all frame drops) and filter events
 *
 * ## Output Behavior
 *
 * If a primary interval overlaps with multiple secondary intervals, multiple
 * output rows will be produced (one for each overlap). Each output row shows
 * the actual overlap period (intersected ts/dur values).
 */

import {
  QueryNode,
  QueryNodeState,
  nextNodeId,
  NodeType,
} from '../../query_node';
import {ColumnInfo} from '../column_info';
import protos from '../../../../protos';
import m from 'mithril';
import {StructuredQueryBuilder, ColumnSpec} from '../structured_query_builder';
import {setValidationError} from '../node_issues';
import {EmptyState} from '../../../../widgets/empty_state';
import {Callout} from '../../../../widgets/callout';
import {ListItem, InfoBox} from '../widgets';
import {NodeModifyAttrs, NodeDetailsAttrs} from '../node_explorer_types';

export interface FilterDuringNodeState extends QueryNodeState {
  filterNegativeDurPrimary?: boolean; // Filter negative durations in primary input
  filterNegativeDurSecondary?: boolean; // Filter negative durations in secondary input
}

export class FilterDuringNode implements QueryNode {
  readonly nodeId: string;
  readonly type = NodeType.kFilterDuring;
  primaryInput?: QueryNode;
  secondaryInputs: {
    connections: Map<number, QueryNode>;
    min: 1;
    max: -1;
  };
  nextNodes: QueryNode[];
  readonly state: FilterDuringNodeState;

  constructor(state: FilterDuringNodeState) {
    this.nodeId = nextNodeId();
    this.state = state;
    this.secondaryInputs = {
      connections: new Map(),
      min: 1,
      max: -1,
    };
    this.nextNodes = [];
    this.state.autoExecute = this.state.autoExecute ?? false;
    this.state.filterNegativeDurPrimary =
      this.state.filterNegativeDurPrimary ?? true;
    this.state.filterNegativeDurSecondary =
      this.state.filterNegativeDurSecondary ?? true;
  }

  // Get all nodes connected to secondary input ports (the intervals to filter during)
  get secondaryNodes(): QueryNode[] {
    return Array.from(this.secondaryInputs.connections.values());
  }

  get finalCols(): ColumnInfo[] {
    // Return the same columns as the primary input
    return this.primaryInput?.finalCols ?? [];
  }

  getTitle(): string {
    return 'Filter During';
  }

  nodeDetails(): NodeDetailsAttrs {
    const count = this.secondaryNodes.length;
    const message =
      count === 0
        ? 'No interval sources'
        : count === 1
          ? 'Filter during intervals'
          : `Filter during ${count} interval sources`;
    return {
      content: m('.pf-exp-node-details-message', message),
    };
  }

  nodeSpecificModify(): NodeModifyAttrs {
    // Run validation to populate error state
    this.validate();
    const error = this.state.issues?.queryError;

    const secondaryNodes = this.secondaryNodes;

    // If no secondary inputs connected, show empty state
    if (secondaryNodes.length === 0) {
      return {
        sections: [
          {
            content: m(EmptyState, {
              icon: 'link_off',
              title: 'No interval sources connected',
              detail:
                'Connect one or more nodes to the left port that provide intervals (must have id, ts, dur columns).',
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

    // Add info about the operation (first section after error)
    const infoText =
      secondaryNodes.length === 1
        ? 'Filters the primary input to only show intervals that occurred during the intervals from the secondary input. Output ts/dur values represent the actual overlap.'
        : `Filters the primary input to only show intervals that occurred during intervals from any of the ${secondaryNodes.length} secondary inputs (combined via UNION ALL). Output ts/dur values represent the actual overlap.`;
    sections.push({
      content: m(InfoBox, infoText),
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

    // Add all secondary inputs in one section
    const secondaryFilterEnabled =
      this.state.filterNegativeDurSecondary ?? true;
    const secondaryInputItems: m.Children = [];
    for (let i = 0; i < secondaryNodes.length; i++) {
      const inputName =
        secondaryNodes.length === 1
          ? 'Secondary Input'
          : `Secondary Input ${i + 1}`;
      secondaryInputItems.push(
        m(ListItem, {
          icon: 'input',
          name: inputName,
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
      );
    }
    sections.push({
      content: m('.pf-exp-secondary-inputs', secondaryInputItems),
    });

    return {
      sections,
    };
  }

  nodeInfo(): m.Children {
    return m(
      'div',
      m(
        'p',
        'Filter intervals to only those that occurred during intervals from one or more sources. The output preserves all columns from the primary input, with ts and dur values representing the actual overlap period.',
      ),
      m(
        'p',
        m('strong', 'Multiple sources:'),
        ' When multiple secondary inputs are connected, they are combined via UNION ALL before filtering, meaning intervals are kept if they overlap with ANY of the secondary sources.',
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
        m('strong', 'Example:'),
        ' Filter CPU slices to only those that occurred during app startup, or filter memory allocations during multiple user interactions.',
      ),
      m(
        'p',
        m('strong', 'Note:'),
        ' If a primary interval overlaps with multiple secondary intervals, multiple output rows will be produced (one for each overlap).',
      ),
    );
  }

  validate(): boolean {
    // Clear any previous errors at the start of validation
    if (this.state.issues) {
      this.state.issues.clear();
    }

    if (this.primaryInput === undefined) {
      setValidationError(this.state, 'No primary input connected');
      return false;
    }

    if (!this.primaryInput.validate()) {
      setValidationError(this.state, 'Primary input is invalid');
      return false;
    }

    const secondaryNodes = this.secondaryNodes;
    if (secondaryNodes.length === 0) {
      setValidationError(this.state, 'No interval sources connected');
      return false;
    }

    // Validate all secondary inputs
    for (let i = 0; i < secondaryNodes.length; i++) {
      const node = secondaryNodes[i];
      if (!node.validate()) {
        setValidationError(this.state, `Interval source ${i + 1} is invalid`);
        return false;
      }
    }

    // Check that primary input has required columns
    const primaryCols = new Set(this.primaryInput.finalCols.map((c) => c.name));
    const requiredCols = ['id', 'ts', 'dur'];
    const missingPrimary = requiredCols.filter((c) => !primaryCols.has(c));
    if (missingPrimary.length > 0) {
      setValidationError(
        this.state,
        `Primary input is missing required columns: ${missingPrimary.join(', ')}`,
      );
      return false;
    }

    // Check that all secondary inputs have required columns
    for (let i = 0; i < secondaryNodes.length; i++) {
      const node = secondaryNodes[i];
      const secondaryCols = new Set(node.finalCols.map((c) => c.name));
      const missingSecondary = requiredCols.filter(
        (c) => !secondaryCols.has(c),
      );
      if (missingSecondary.length > 0) {
        setValidationError(
          this.state,
          `Interval source ${i + 1} is missing required columns: ${missingSecondary.join(', ')}`,
        );
        return false;
      }
    }

    return true;
  }

  clone(): QueryNode {
    const stateCopy: FilterDuringNodeState = {
      filterNegativeDurPrimary: this.state.filterNegativeDurPrimary,
      filterNegativeDurSecondary: this.state.filterNegativeDurSecondary,
      filters: this.state.filters?.map((f) => ({...f})),
      filterOperator: this.state.filterOperator,
      onchange: this.state.onchange,
    };
    return new FilterDuringNode(stateCopy);
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (!this.validate()) return undefined;
    if (this.primaryInput === undefined) return undefined;

    const secondaryNodes = this.secondaryNodes;
    if (secondaryNodes.length === 0) return undefined;

    // Step 1: Union all secondary inputs if there are multiple
    // If only one, use it directly
    let combinedSecondaryQuery: protos.PerfettoSqlStructuredQuery | undefined;
    if (secondaryNodes.length === 1) {
      combinedSecondaryQuery = secondaryNodes[0].getStructuredQuery();
    } else {
      // Multiple inputs - union them all using UNION ALL
      combinedSecondaryQuery = StructuredQueryBuilder.withUnion(
        secondaryNodes,
        true, // Use UNION ALL to keep all intervals
        `${this.nodeId}_secondary_union`,
      );
    }

    if (combinedSecondaryQuery === undefined) return undefined;

    // Step 2: Wrap the combined secondary to only select id, ts, dur
    // This avoids column conflicts in the interval intersection
    const secondaryColumnsOnly: ColumnSpec[] = [
      {columnNameOrExpression: 'id'},
      {columnNameOrExpression: 'ts'},
      {columnNameOrExpression: 'dur'},
    ];

    // Create a temporary QueryNode wrapper for the combined secondary query
    const combinedSecondaryNode: QueryNode = {
      nodeId: `${this.nodeId}_combined_secondary`,
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
      getTitle: () => 'Combined Secondary',
      validate: () => true,
      clone: () => combinedSecondaryNode,
      getStructuredQuery: () => combinedSecondaryQuery,
      nodeInfo: () => null,
      nodeDetails: () => ({content: null}),
      nodeSpecificModify: () => ({sections: []}),
      serializeState: () => ({}),
    };

    const wrappedSecondary = StructuredQueryBuilder.withSelectColumns(
      combinedSecondaryNode,
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

    // Step 3: Build interval intersect with filterNegativeDur
    const filterNegativeDur = [
      this.state.filterNegativeDurPrimary ?? true,
      this.state.filterNegativeDurSecondary ?? true,
    ];

    const intervalIntersectQuery = StructuredQueryBuilder.withIntervalIntersect(
      this.primaryInput,
      [wrappedSecondaryNode],
      undefined, // No partition columns
      filterNegativeDur,
      `${this.nodeId}_intersect`,
    );

    if (intervalIntersectQuery === undefined) return undefined;

    // Step 4: Select columns to match primary input's schema
    // IntervalIntersect returns: ts, dur (intersected), id_0, ts_0, dur_0, id_1, ts_1, dur_1, plus other primary columns
    // We want to return: all primary columns in their original order, with ts/dur being intersected values
    const selectColumns: ColumnSpec[] = this.primaryInput.finalCols.map(
      (col) => {
        if (col.name === 'id') {
          // Use id_0 (from primary) and alias it back to 'id'
          return {columnNameOrExpression: 'id_0', alias: 'id'};
        } else if (col.name === 'ts') {
          // Use intersected ts (no suffix)
          return {columnNameOrExpression: 'ts'};
        } else if (col.name === 'dur') {
          // Use intersected dur (no suffix)
          return {columnNameOrExpression: 'dur'};
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
    // Get all secondary input node IDs
    const secondaryInputNodeIds = this.secondaryNodes.map(
      (node) => node.nodeId,
    );

    return {
      primaryInputId: this.primaryInput?.nodeId,
      secondaryInputNodeIds,
      filterNegativeDurPrimary: this.state.filterNegativeDurPrimary,
      filterNegativeDurSecondary: this.state.filterNegativeDurSecondary,
    };
  }

  static deserializeState(
    serializedState: FilterDuringNodeState,
  ): FilterDuringNodeState {
    return {
      filterNegativeDurPrimary: serializedState.filterNegativeDurPrimary,
      filterNegativeDurSecondary: serializedState.filterNegativeDurSecondary,
    };
  }

  static deserializeConnections(
    nodes: Map<string, QueryNode>,
    serializedState: {secondaryInputNodeIds?: string[]},
  ): {secondaryInputNodes: QueryNode[]} {
    const secondaryInputNodes = (serializedState.secondaryInputNodeIds ?? [])
      .map((id) => nodes.get(id))
      .filter((node): node is QueryNode => node !== undefined);
    return {
      secondaryInputNodes,
    };
  }

  // Called when a node is connected/disconnected to secondary inputs
  onPrevNodesUpdated(): void {
    this.state.onchange?.();
  }
}
