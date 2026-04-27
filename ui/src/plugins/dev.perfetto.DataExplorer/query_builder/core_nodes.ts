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

import {nodeRegistry} from './node_registry';
import {SlicesSourceNode} from './nodes/sources/slices_source';
import {
  modalForTableSelection,
  TableSourceNode,
  TableSourceNodeAttrs,
} from './nodes/sources/table_source';
import {SqlSourceNode, SqlSourceNodeAttrs} from './nodes/sources/sql_source';
import {
  TimeRangeSourceNode,
  TimeRangeSourceNodeAttrs,
} from './nodes/sources/timerange_source';
import {AggregationNode, AggregationNodeAttrs} from './nodes/aggregation_node';
import {
  ModifyColumnsNode,
  ModifyColumnsNodeAttrs,
} from './nodes/modify_columns_node';
import {AddColumnsNode, AddColumnsNodeAttrs} from './nodes/add_columns_node';
import {
  FilterDuringNode,
  FilterDuringNodeAttrs,
} from './nodes/filter_during_node';
import {FilterInNode, FilterInNodeAttrs} from './nodes/filter_in_node';
import {
  IntervalIntersectNode,
  IntervalIntersectNodeAttrs,
} from './nodes/interval_intersect_node';
import {JoinNode, JoinNodeAttrs} from './nodes/join_node';
import {
  CreateSlicesNode,
  CreateSlicesNodeAttrs,
} from './nodes/create_slices_node';
import {SortNode, SortNodeAttrs} from './nodes/sort_node';
import {FilterNode, FilterNodeAttrs} from './nodes/filter_node';
import {UnionNode, UnionNodeAttrs} from './nodes/union_node';
import {
  LimitAndOffsetNode,
  LimitAndOffsetNodeAttrs,
} from './nodes/limit_and_offset_node';
import {
  CounterToIntervalsNode,
  CounterToIntervalsNodeAttrs,
} from './nodes/counter_to_intervals_node';
import {MetricsNode, MetricsNodeAttrs} from './nodes/metrics_node';
import {
  TraceSummaryNode,
  TraceSummaryNodeAttrs,
} from './nodes/trace_summary_node';
import {
  VisualisationNode,
  VisualisationNodeAttrs,
} from './nodes/visualisation_node';
import {DashboardNode, DashboardNodeAttrs} from './nodes/dashboard_node';
import {Icons} from '../../../base/semantic_icons';
import {NodeType, QueryNode} from '../query_node';
import {GroupNode} from './nodes/group_node';

// After JoinNode.onPrevNodesUpdated() defaults all columns to unchecked on
// first initialization, check the columns that the downstream ModifyColumns
// node needs. For JSON import this is a no-op because columns are restored
// from serialized _attrs (already have checked values).
function applyJoinColumnDefaults(joinNode: JoinNode): void {
  const leftCols = joinNode.attrs.leftColumns ?? [];
  const rightCols = joinNode.attrs.rightColumns ?? [];

  // If any columns are already checked, the serialized _attrs was restored
  // correctly (JSON import path) — nothing to do.
  if (leftCols.some((c) => c.checked) || rightCols.some((c) => c.checked)) {
    return;
  }

  // No columns checked — first initialization (e.g. pbtxt import).
  // Check only columns needed by the downstream ModifyColumns node.
  const mc = joinNode.nextNodes.find((n) => n.type === NodeType.kModifyColumns);
  if (mc !== undefined) {
    const mcState = (mc as {attrs?: object}).attrs as {
      selectedColumns?: Array<{name: string; checked: boolean}>;
    };
    const needed = new Set(
      mcState.selectedColumns?.filter((c) => c.checked).map((c) => c.name) ??
        [],
    );
    const checked = new Set<string>();
    for (const col of leftCols) {
      if (needed.has(col.name) && !checked.has(col.name)) {
        col.checked = true;
        checked.add(col.name);
      }
    }
    for (const col of rightCols) {
      if (needed.has(col.name) && !checked.has(col.name)) {
        col.checked = true;
        checked.add(col.name);
      }
    }
  } else {
    // No downstream ModifyColumns — check all columns.
    for (const col of leftCols) col.checked = true;
    for (const col of rightCols) col.checked = true;
  }
}

export function registerCoreNodes() {
  nodeRegistry.register('slice', {
    name: 'Slices',
    description: 'Explore all the slices from your trace.',
    icon: 'bar_chart',
    hotkey: 'l',
    type: 'source',
    showOnLandingPage: true,
    nodeType: NodeType.kSimpleSlices,
    factory: (_attrs, factoryCtx) =>
      new SlicesSourceNode({}, factoryCtx?.context ?? {}),
    deserialize: (_state, trace, sqlModules) =>
      new SlicesSourceNode({}, {trace, sqlModules}),
  });

  nodeRegistry.register('table', {
    name: 'Table',
    description: 'Query and explore data from any table in your trace.',
    icon: 'table_chart',
    hotkey: 't',
    type: 'source',
    showOnLandingPage: true,
    nodeType: NodeType.kTable,
    preCreate: async ({sqlModules}) => {
      const selections = await modalForTableSelection(sqlModules);
      if (selections && selections.length > 0) {
        // Return an array of states, one for each selected table
        return selections.map((selection) => ({
          sqlTable: selection.sqlTable.name,
        }));
      }
      return null;
    },
    factory: (_attrs) =>
      new TableSourceNode(_attrs as TableSourceNodeAttrs, {}),
    deserialize: (_attrs, trace, sqlModules) =>
      new TableSourceNode(_attrs as TableSourceNodeAttrs, {trace, sqlModules}),
  });

  nodeRegistry.register('sql', {
    name: 'Query',
    description:
      'Start with a custom SQL query to act as a source for further exploration.',
    icon: 'code',
    hotkey: 'q',
    type: 'source',
    showOnLandingPage: true,
    nodeType: NodeType.kSqlSource,
    factory: (_attrs, factoryCtx) =>
      new SqlSourceNode(
        _attrs as SqlSourceNodeAttrs,
        factoryCtx?.context ?? {},
      ),
    deserialize: (_attrs, trace) =>
      new SqlSourceNode(_attrs as SqlSourceNodeAttrs, {trace}),
  });

  nodeRegistry.register('timerange', {
    name: 'Time Range',
    description:
      'Use timeline selection as a source node. Can be dynamic (syncs with timeline) or static (snapshot).',
    icon: 'schedule',
    type: 'source',
    showOnLandingPage: false, // Available in menus but not on landing page
    nodeType: NodeType.kTimeRangeSource,
    factory: (_attrs, factoryCtx) => {
      // If start/end are already set, this is being restored from serialization
      // or created programmatically - use those values
      if (
        'start' in _attrs &&
        _attrs.start !== undefined &&
        'end' in _attrs &&
        _attrs.end !== undefined
      ) {
        if (!factoryCtx?.context?.trace) {
          throw new Error('TimeRange node requires a trace instance');
        }
        const attrs: TimeRangeSourceNodeAttrs = {
          start: String(_attrs.start),
          end: String(_attrs.end),
          isDynamic:
            'isDynamic' in _attrs && _attrs.isDynamic === true ? true : false,
        };
        return new TimeRangeSourceNode(attrs, factoryCtx?.context ?? {});
      }

      // New node - initialize from current selection
      if (!factoryCtx?.context?.trace) {
        throw new Error('TimeRange node requires a trace instance');
      }

      const timeRange =
        factoryCtx!.context!.trace!.selection.getTimeSpanOfSelection();
      // Note: If there's no selection, start/end will be undefined and the node
      // will be in an invalid _attrs (validate() will return false and show error).
      // This is intentional - the user can fix it by clicking "Update from Selection"
      // or by entering times manually.
      const attrs: TimeRangeSourceNodeAttrs = {
        start: timeRange?.start?.toString(),
        end: timeRange?.end?.toString(),
        isDynamic: false, // Default to static mode
      };
      return new TimeRangeSourceNode(attrs, factoryCtx?.context ?? {});
    },
    deserialize: (_attrs, trace) =>
      new TimeRangeSourceNode(_attrs as TimeRangeSourceNodeAttrs, {trace}),
  });

  nodeRegistry.register('add_columns', {
    name: 'Add Columns',
    description:
      'Add columns from another node via LEFT JOIN. Connect a node to the left-side port.',
    icon: 'add_box',
    type: 'modification',
    category: 'Columns',
    nodeType: NodeType.kAddColumns,
    factory: (_attrs, factoryCtx) =>
      new AddColumnsNode(
        _attrs as AddColumnsNodeAttrs,
        factoryCtx?.context ?? {},
      ),
    deserialize: (_attrs, trace, sqlModules) =>
      new AddColumnsNode(
        AddColumnsNode.deserializeState(_attrs as AddColumnsNodeAttrs),
        {trace, sqlModules},
      ),
  });

  nodeRegistry.register('modify_columns', {
    name: 'Modify Columns',
    description: 'Select, rename, and add new columns to the data.',
    icon: 'edit',
    type: 'modification',
    category: 'Columns',
    nodeType: NodeType.kModifyColumns,
    factory: (_attrs) =>
      new ModifyColumnsNode(_attrs as unknown as ModifyColumnsNodeAttrs, {}),
    deserialize: (_attrs, _trace, _sqlModules) =>
      new ModifyColumnsNode(
        ModifyColumnsNode.deserializeState(_attrs as ModifyColumnsNodeAttrs),
        {},
      ),
  });

  nodeRegistry.register('aggregation', {
    name: 'Aggregation',
    description: 'Group and aggregate data from the source node.',
    icon: 'functions',
    type: 'modification',
    nodeType: NodeType.kAggregation,
    factory: (_attrs) =>
      new AggregationNode(_attrs as unknown as AggregationNodeAttrs, {}),
    deserialize: (_attrs, _trace, sqlModules) =>
      new AggregationNode(
        AggregationNode.deserializeState(_attrs as AggregationNodeAttrs),
        {sqlModules},
      ),
  });

  nodeRegistry.register('filter_node', {
    name: 'Filter',
    description: 'Filter rows based on column values.',
    icon: Icons.Filter,
    type: 'modification',
    category: 'Filter',
    nodeType: NodeType.kFilter,
    factory: (_attrs, factoryCtx) =>
      new FilterNode(_attrs as FilterNodeAttrs, factoryCtx?.context ?? {}),
    deserialize: (_attrs, _trace, sqlModules) =>
      new FilterNode(_attrs as FilterNodeAttrs, {sqlModules}),
  });

  nodeRegistry.register('filter_during', {
    name: 'Filter During',
    description:
      'Filter to only show intervals that occurred during intervals from another source.',
    icon: Icons.Filter,
    type: 'modification',
    category: 'Filter',
    nodeType: NodeType.kFilterDuring,
    factory: (_attrs) =>
      new FilterDuringNode(_attrs as FilterDuringNodeAttrs, {}),
    deserialize: (_attrs, _trace, sqlModules) =>
      new FilterDuringNode(_attrs as FilterDuringNodeAttrs, {sqlModules}),
  });

  nodeRegistry.register('filter_in', {
    name: 'Filter In',
    description:
      'Filter rows to only those where a column value exists in another query result.',
    icon: Icons.Filter,
    type: 'modification',
    category: 'Filter',
    nodeType: NodeType.kFilterIn,
    factory: (_attrs, factoryCtx) =>
      new FilterInNode(_attrs as FilterInNodeAttrs, factoryCtx?.context ?? {}),
    deserialize: (_attrs, _trace, sqlModules) =>
      new FilterInNode(_attrs as FilterInNodeAttrs, {sqlModules}),
  });

  nodeRegistry.register('interval_intersect', {
    name: 'Interval Intersect',
    description: 'Intersect the intervals with another table.',
    icon: 'timeline',
    type: 'multisource',
    category: 'Time',
    nodeType: NodeType.kIntervalIntersect,
    factory: (_attrs) =>
      new IntervalIntersectNode(_attrs as IntervalIntersectNodeAttrs, {}),
    deserialize: (_attrs, _trace, sqlModules) =>
      new IntervalIntersectNode(_attrs as IntervalIntersectNodeAttrs, {
        sqlModules,
      }),
  });

  nodeRegistry.register('join', {
    name: 'Join',
    description:
      'Join two tables using equality columns or custom SQL condition.',
    icon: 'merge',
    type: 'multisource',
    nodeType: NodeType.kJoin,
    factory: (_attrs, factoryCtx) => {
      const attrs: JoinNodeAttrs = {
        leftQueryAlias: 'left',
        rightQueryAlias: 'right',
        conditionType: 'equality',
        joinType: 'INNER',
        leftColumn: '',
        rightColumn: '',
        sqlExpression: '',
        leftColumns: undefined,
        rightColumns: undefined,
      };
      return new JoinNode(attrs, factoryCtx?.context ?? {});
    },
    deserialize: (_attrs, _trace, sqlModules) =>
      new JoinNode(JoinNode.deserializeState(_attrs as JoinNodeAttrs), {
        sqlModules,
      }),
    postDeserializeLate: (node) => {
      const joinNode = node as JoinNode;
      joinNode.onPrevNodesUpdated();
      // After updateColumnArrays defaults all to unchecked on first init,
      // check the columns that the downstream ModifyColumns needs.
      applyJoinColumnDefaults(joinNode);
    },
  });

  nodeRegistry.register('create_slices', {
    name: 'Create Slices',
    description:
      'Create slices by pairing start and end timestamps from two sources.',
    icon: 'add_circle',
    type: 'multisource',
    category: 'Time',
    nodeType: NodeType.kCreateSlices,
    factory: (_attrs) =>
      new CreateSlicesNode(
        {
          startsTsColumn: 'ts',
          endsTsColumn: 'ts',
          ..._attrs,
        } as CreateSlicesNodeAttrs,
        _attrs,
      ),
    deserialize: (_attrs, _trace, sqlModules) =>
      new CreateSlicesNode(_attrs as CreateSlicesNodeAttrs, {sqlModules}),
  });

  nodeRegistry.register('sort_node', {
    name: 'Sort',
    description: 'Sort rows by one or more columns.',
    icon: 'sort',
    type: 'modification',
    nodeType: NodeType.kSort,
    factory: (_attrs, factoryCtx) =>
      new SortNode(_attrs as SortNodeAttrs, factoryCtx?.context ?? {}),
    deserialize: (_attrs, _trace, sqlModules) =>
      new SortNode(_attrs as SortNodeAttrs, {sqlModules}),
  });

  nodeRegistry.register('union_node', {
    name: 'Union',
    description: 'Combine rows from multiple sources.',
    icon: 'merge_type',
    type: 'multisource',
    nodeType: NodeType.kUnion,
    factory: (_attrs, factoryCtx) => {
      const node = new UnionNode(
        {selectedColumns: [], ..._attrs} as UnionNodeAttrs,
        factoryCtx?.context ?? {},
      );
      node.onPrevNodesUpdated();
      return node;
    },
    deserialize: (_attrs, _trace, sqlModules) =>
      new UnionNode(_attrs as UnionNodeAttrs, {sqlModules}),
  });

  nodeRegistry.register('limit_and_offset_node', {
    name: 'Limit and Offset',
    description: 'Limit the number of rows returned and optionally skip rows.',
    icon: Icons.Filter,
    type: 'modification',
    nodeType: NodeType.kLimitAndOffset,
    factory: (_attrs) =>
      new LimitAndOffsetNode(_attrs as LimitAndOffsetNodeAttrs, {}),
    deserialize: (_attrs, _trace, sqlModules) =>
      new LimitAndOffsetNode(_attrs as LimitAndOffsetNodeAttrs, {sqlModules}),
  });

  nodeRegistry.register('metrics', {
    name: 'Metrics',
    description:
      'Define a trace-based metric with value column and dimensions.',
    icon: 'analytics',
    type: 'export',
    nodeType: NodeType.kMetrics,
    allowedChildren: ['trace_summary'],
    factory: (_attrs, factoryCtx) =>
      new MetricsNode(
        _attrs as unknown as MetricsNodeAttrs,
        factoryCtx?.context ?? {},
      ),
    deserialize: (_attrs, trace, sqlModules) =>
      new MetricsNode(
        MetricsNode.deserializeState(_attrs as MetricsNodeAttrs),
        {trace, sqlModules},
      ),
    postDeserializeLate: (node) => (node as MetricsNode).onPrevNodesUpdated(),
  });

  nodeRegistry.register('visualisation', {
    name: 'Charts',
    description:
      'Visualize data with bar charts or histograms. Click to filter.',
    icon: 'bar_chart',
    type: 'modification',
    nodeType: NodeType.kVisualisation,
    factory: (_attrs) =>
      new VisualisationNode(_attrs as unknown as VisualisationNodeAttrs, {}),
    deserialize: (_attrs, _trace, sqlModules) =>
      new VisualisationNode(_attrs as unknown as VisualisationNodeAttrs, {
        sqlModules,
      }),
  });

  nodeRegistry.register('counter_to_intervals', {
    name: 'Counter to Intervals',
    description:
      'Convert counter data (with ts but no dur) to interval data (with ts and dur).',
    icon: 'show_chart',
    type: 'modification',
    category: 'Advanced',
    nodeType: NodeType.kCounterToIntervals,
    factory: (_attrs) =>
      new CounterToIntervalsNode(_attrs as CounterToIntervalsNodeAttrs, {}),
    deserialize: (_attrs, _trace, sqlModules) =>
      new CounterToIntervalsNode(_attrs as CounterToIntervalsNodeAttrs, {
        sqlModules,
      }),
  });

  nodeRegistry.register('dashboard', {
    name: 'Export to Dashboard',
    description: 'Export this data source so it can be used on dashboards.',
    icon: 'dashboard',
    type: 'export',
    nodeType: NodeType.kDashboard,
    allowedChildren: [],
    factory: (_attrs, factoryCtx) =>
      new DashboardNode(
        _attrs as DashboardNodeAttrs,
        factoryCtx?.context ?? {},
      ),
    deserialize: (_attrs) =>
      new DashboardNode(_attrs as DashboardNodeAttrs, {}),
    postDeserializeLate: (node) => (node as DashboardNode).onPrevNodesUpdated(),
  });

  nodeRegistry.register('trace_summary', {
    name: 'Trace Summary',
    description:
      'Bundle multiple metrics into a single trace summary specification.',
    icon: 'summarize',
    type: 'export',
    nodeType: NodeType.kTraceSummary,
    allowedChildren: [],
    factory: (_attrs) =>
      new TraceSummaryNode(_attrs as TraceSummaryNodeAttrs, {}),
    deserialize: (_attrs, trace) =>
      new TraceSummaryNode(_attrs as TraceSummaryNodeAttrs, {trace}),
  });

  // Groups use type 'source' because they are root-level nodes in the outer
  // graph (not docked children). They don't appear on the landing page and
  // are only created programmatically via the "Group" action.
  nodeRegistry.register('group', {
    name: 'Group',
    description: 'A group of nodes collapsed into a single unit.',
    icon: 'group_work',
    type: 'source',
    showOnLandingPage: false,
    nodeType: NodeType.kGroup,
    factory: () => new GroupNode({name: 'Group'}, {}),
    deserialize: (_attrs) => {
      const s = _attrs as {name?: string};
      // Create a placeholder GroupNode; inner nodes are restored in
      // deserializeConnections once all nodes are available.
      return new GroupNode({name: s.name ?? 'Group'}, {});
    },
    deserializeConnections: (node, _attrs, allNodes, innerNodeIds) => {
      if (!(node instanceof GroupNode)) return;

      // Prefer graph-level innerNodeIds (new format); fall back to the old
      // format where they were stored inside the node's state blob.
      const s = _attrs as {innerNodeIds?: string[]};
      const ids =
        innerNodeIds ?? (Array.isArray(s.innerNodeIds) ? s.innerNodeIds : []);
      const innerNodes: QueryNode[] = [];
      for (const id of ids) {
        const n = allNodes.get(id);
        if (n !== undefined) {
          innerNodes.push(n);
        }
      }
      node.innerNodes = innerNodes;

      // End node is the inner node with no successors inside the group.
      const innerSet = new Set(innerNodes.map((n) => n.nodeId));
      const endNode = innerNodes.find((n) =>
        n.nextNodes.every((next) => !innerSet.has(next.nodeId)),
      );
      if (endNode !== undefined) {
        node.endNode = endNode;
      }

      // External connections are restored in postDeserialize, after all
      // inner nodes have had their primaryInput/secondaryInputs set.
    },
    postDeserialize: (node) => {
      if (!(node instanceof GroupNode)) return;
      const innerNodes = node.innerNodes;
      const innerSet = new Set(innerNodes.map((n) => n.nodeId));

      // Restore external connections and secondary inputs: external
      // sources are nodes outside the group that feed into inner nodes.
      node.secondaryInputs.connections.clear();
      node.externalConnections = [];
      let port = 0;
      for (const inner of innerNodes) {
        if (inner.primaryInput && !innerSet.has(inner.primaryInput.nodeId)) {
          node.secondaryInputs.connections.set(port, inner.primaryInput);
          node.externalConnections.push({
            sourceNode: inner.primaryInput,
            innerTargetNode: inner,
            innerTargetPort: undefined,
            groupPort: port,
          });
          port++;
        }
        if (inner.secondaryInputs) {
          for (const [innerPort, src] of inner.secondaryInputs.connections) {
            if (src !== undefined && !innerSet.has(src.nodeId)) {
              node.secondaryInputs.connections.set(port, src);
              node.externalConnections.push({
                sourceNode: src,
                innerTargetNode: inner,
                innerTargetPort: innerPort,
                groupPort: port,
              });
              port++;
            }
          }
        }
      }

      // Rebuild secondaryInputs with the correct max so no extra empty
      // ports are shown (group nodes don't accept new connections).
      node.secondaryInputs = {
        ...node.secondaryInputs,
        max: port,
      };
    },
  });

  // Set the default allowed children for all nodes.
  // This is the full set of modification + multisource nodes, matching the
  // current behavior. Individual node registrations can override this by
  // setting allowedChildren on their descriptor.
  nodeRegistry.setDefaultAllowedChildren([
    // Modification nodes
    'add_columns',
    'modify_columns',
    'aggregation',
    'filter_node',
    'counter_to_intervals',
    'sort_node',
    'limit_and_offset_node',
    'visualisation',
    // Export nodes
    'metrics',
    // Multisource nodes
    'filter_during',
    'filter_in',
    'interval_intersect',
    'join',
    'create_slices',
    'union_node',
    'dashboard',
    'group',
    // Source nodes that accept secondary inputs (SQL can reference them as $input_N)
    'sql',
  ]);

  // Validate that all allowedChildren references point to registered nodes.
  nodeRegistry.validateAllowedChildren();
}
