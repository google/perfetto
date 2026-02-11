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
  TableSourceState,
  TableSourceSerializedState,
} from './nodes/sources/table_source';
import {
  SqlSourceNode,
  SqlSourceState,
  SqlSourceSerializedState,
} from './nodes/sources/sql_source';
import {
  TimeRangeSourceNode,
  TimeRangeSourceState,
  TimeRangeSourceSerializedState,
} from './nodes/sources/timerange_source';
import {
  AggregationNode,
  AggregationNodeState,
  AggregationSerializedState,
} from './nodes/aggregation_node';
import {
  ModifyColumnsNode,
  ModifyColumnsState,
  ModifyColumnsSerializedState,
} from './nodes/modify_columns_node';
import {AddColumnsNode, AddColumnsNodeState} from './nodes/add_columns_node';
import {
  FilterDuringNode,
  FilterDuringNodeState,
} from './nodes/filter_during_node';
import {FilterInNode, FilterInNodeState} from './nodes/filter_in_node';
import {
  IntervalIntersectNode,
  IntervalIntersectNodeState,
  IntervalIntersectSerializedState,
} from './nodes/interval_intersect_node';
import {JoinNode, JoinNodeState, JoinSerializedState} from './nodes/join_node';
import {
  CreateSlicesNode,
  CreateSlicesNodeState,
  CreateSlicesSerializedState,
} from './nodes/create_slices_node';
import {SortNode, SortNodeState} from './nodes/sort_node';
import {FilterNode, FilterNodeState} from './nodes/filter_node';
import {
  UnionNode,
  UnionNodeState,
  UnionSerializedState,
} from './nodes/union_node';
import {
  LimitAndOffsetNode,
  LimitAndOffsetNodeState,
} from './nodes/limit_and_offset_node';
import {
  CounterToIntervalsNode,
  CounterToIntervalsNodeState,
} from './nodes/counter_to_intervals_node';
import {Icons} from '../../../base/semantic_icons';
import {NodeType} from '../query_node';

export function registerCoreNodes() {
  nodeRegistry.register('slice', {
    name: 'Slices',
    description: 'Explore all the slices from your trace.',
    icon: 'bar_chart',
    hotkey: 'l',
    type: 'source',
    showOnLandingPage: true,
    nodeType: NodeType.kSimpleSlices,
    factory: (state) => new SlicesSourceNode(state),
    deserialize: (_state, trace, sqlModules) =>
      new SlicesSourceNode({trace, sqlModules}),
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
          sqlTable: selection.sqlTable,
        }));
      }
      return null;
    },
    factory: (state) => new TableSourceNode(state as TableSourceState),
    deserialize: (state, trace, sqlModules) =>
      new TableSourceNode(
        TableSourceNode.deserializeState(
          trace,
          sqlModules,
          state as TableSourceSerializedState,
        ),
      ),
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
    factory: (state) => new SqlSourceNode(state as SqlSourceState),
    deserialize: (state, trace) =>
      new SqlSourceNode({
        ...(state as SqlSourceSerializedState),
        trace,
      }),
    deserializeConnections: (node, state, allNodes) => {
      const sqlSourceNode = node as SqlSourceNode;
      const conns = SqlSourceNode.deserializeConnections(
        allNodes,
        state as SqlSourceSerializedState,
      );
      sqlSourceNode.secondaryInputs.connections.clear();
      for (let i = 0; i < conns.inputNodes.length; i++) {
        sqlSourceNode.secondaryInputs.connections.set(i, conns.inputNodes[i]);
      }
    },
  });

  nodeRegistry.register('timerange', {
    name: 'Time Range',
    description:
      'Use timeline selection as a source node. Can be dynamic (syncs with timeline) or static (snapshot).',
    icon: 'schedule',
    type: 'source',
    showOnLandingPage: false, // Available in menus but not on landing page
    nodeType: NodeType.kTimeRangeSource,
    factory: (state) => {
      // If start/end are already set, this is being restored from serialization
      // or created programmatically - use those values
      if (
        'start' in state &&
        state.start !== undefined &&
        'end' in state &&
        state.end !== undefined
      ) {
        if (!state.trace) {
          throw new Error('TimeRange node requires a trace instance');
        }
        return new TimeRangeSourceNode({
          ...state,
          trace: state.trace,
          isDynamic:
            'isDynamic' in state && state.isDynamic === true ? true : false,
        } as TimeRangeSourceState);
      }

      // New node - initialize from current selection
      if (!state.trace) {
        throw new Error('TimeRange node requires a trace instance');
      }

      const timeRange = state.trace.selection.getTimeSpanOfSelection();
      // Note: If there's no selection, start/end will be undefined and the node
      // will be in an invalid state (validate() will return false and show error).
      // This is intentional - the user can fix it by clicking "Update from Selection"
      // or by entering times manually.
      const fullState: TimeRangeSourceState = {
        ...state,
        start: timeRange?.start,
        end: timeRange?.end,
        isDynamic: false, // Default to static mode
        trace: state.trace,
      };
      return new TimeRangeSourceNode(fullState);
    },
    deserialize: (state, trace) =>
      new TimeRangeSourceNode(
        TimeRangeSourceNode.deserializeState(
          trace,
          state as TimeRangeSourceSerializedState,
        ),
      ),
  });

  nodeRegistry.register('add_columns', {
    name: 'Add Columns',
    description:
      'Add columns from another node via LEFT JOIN. Connect a node to the left-side port.',
    icon: 'add_box',
    type: 'modification',
    nodeType: NodeType.kAddColumns,
    factory: (state) => {
      const fullState: AddColumnsNodeState = {
        ...state,
        selectedColumns: (state as AddColumnsNodeState).selectedColumns ?? [],
        leftColumn: (state as AddColumnsNodeState).leftColumn ?? 'id',
        rightColumn: (state as AddColumnsNodeState).rightColumn ?? 'id',
      };
      return new AddColumnsNode(fullState);
    },
    deserialize: (state, _trace, sqlModules) =>
      new AddColumnsNode(
        AddColumnsNode.deserializeState(
          sqlModules,
          state as AddColumnsNodeState,
        ),
      ),
    deserializeConnections: (node, state, allNodes) => {
      const addColumnsNode = node as AddColumnsNode;
      const s = state as {secondaryInputNodeId?: string};
      if (s.secondaryInputNodeId) {
        const secondaryInputNode = allNodes.get(s.secondaryInputNodeId);
        if (secondaryInputNode) {
          addColumnsNode.secondaryInputs.connections.set(0, secondaryInputNode);
        }
      }
    },
  });

  nodeRegistry.register('modify_columns', {
    name: 'Modify Columns',
    description: 'Select, rename, and add new columns to the data.',
    icon: 'edit',
    type: 'modification',
    nodeType: NodeType.kModifyColumns,
    factory: (state) => new ModifyColumnsNode(state as ModifyColumnsState),
    deserialize: (state, _trace, sqlModules) =>
      new ModifyColumnsNode(
        ModifyColumnsNode.deserializeState(
          sqlModules,
          state as ModifyColumnsSerializedState,
        ),
      ),
    postDeserialize: (node) => (node as ModifyColumnsNode).resolveColumns(),
  });

  nodeRegistry.register('aggregation', {
    name: 'Aggregation',
    description: 'Group and aggregate data from the source node.',
    icon: 'functions',
    type: 'modification',
    nodeType: NodeType.kAggregation,
    factory: (state) => new AggregationNode(state as AggregationNodeState),
    deserialize: (state, _trace, sqlModules) =>
      new AggregationNode({
        ...AggregationNode.deserializeState(
          state as AggregationSerializedState,
        ),
        sqlModules,
      }),
    postDeserialize: (node) => (node as AggregationNode).resolveColumns(),
  });

  nodeRegistry.register('filter_node', {
    name: 'Filter',
    description: 'Filter rows based on column values.',
    icon: Icons.Filter,
    type: 'modification',
    nodeType: NodeType.kFilter,
    factory: (state) => new FilterNode(state as FilterNodeState),
    deserialize: (state, _trace, sqlModules) =>
      new FilterNode({
        ...FilterNode.deserializeState(state as FilterNodeState),
        sqlModules,
      }),
  });

  nodeRegistry.register('filter_during', {
    name: 'Filter During',
    description:
      'Filter to only show intervals that occurred during intervals from another source.',
    icon: Icons.Filter,
    type: 'multisource',
    category: 'Time',
    nodeType: NodeType.kFilterDuring,
    // Override: multisource nodes default to no primary input, but
    // FilterDuring has both a primary input and secondary inputs.
    hasPrimaryInput: true,
    factory: (state) => {
      return new FilterDuringNode(state as FilterDuringNodeState);
    },
    deserialize: (state, _trace, sqlModules) =>
      new FilterDuringNode({
        ...FilterDuringNode.deserializeState(state as FilterDuringNodeState),
        sqlModules,
      }),
    deserializeConnections: (node, state, allNodes) => {
      const filterDuringNode = node as FilterDuringNode;
      const conns = FilterDuringNode.deserializeConnections(
        allNodes,
        state as {secondaryInputNodeIds?: string[]},
      );
      filterDuringNode.secondaryInputs.connections.clear();
      for (let i = 0; i < conns.secondaryInputNodes.length; i++) {
        filterDuringNode.secondaryInputs.connections.set(
          i,
          conns.secondaryInputNodes[i],
        );
      }
    },
  });

  nodeRegistry.register('filter_in', {
    name: 'Filter In',
    description:
      'Filter rows to only those where a column value exists in another query result.',
    icon: Icons.Filter,
    type: 'multisource',
    category: 'Filtering',
    nodeType: NodeType.kFilterIn,
    // Override: multisource nodes default to no primary input, but
    // FilterIn has both a primary input and secondary inputs.
    hasPrimaryInput: true,
    factory: (state) => {
      return new FilterInNode(state as FilterInNodeState);
    },
    deserialize: (state) =>
      new FilterInNode(
        FilterInNode.deserializeState(state as FilterInNodeState),
      ),
    deserializeConnections: (node, state, allNodes) => {
      const filterInNode = node as FilterInNode;
      const conns = FilterInNode.deserializeConnections(
        allNodes,
        state as {secondaryInputNodeIds?: string[]},
      );
      filterInNode.secondaryInputs.connections.clear();
      for (let i = 0; i < conns.secondaryInputNodes.length; i++) {
        filterInNode.secondaryInputs.connections.set(
          i,
          conns.secondaryInputNodes[i],
        );
      }
    },
  });

  nodeRegistry.register('counter_to_intervals', {
    name: 'Counter to Intervals',
    description:
      'Convert counter data (with ts but no dur) to interval data (with ts and dur).',
    icon: 'show_chart',
    type: 'modification',
    nodeType: NodeType.kCounterToIntervals,
    factory: (state) =>
      new CounterToIntervalsNode(state as CounterToIntervalsNodeState),
    deserialize: (state, _trace, sqlModules) =>
      new CounterToIntervalsNode({
        ...CounterToIntervalsNode.deserializeState(
          state as CounterToIntervalsNodeState,
        ),
        sqlModules,
      }),
  });

  nodeRegistry.register('interval_intersect', {
    name: 'Interval Intersect',
    description: 'Intersect the intervals with another table.',
    icon: 'timeline',
    type: 'multisource',
    category: 'Time',
    nodeType: NodeType.kIntervalIntersect,
    factory: (state, context) => {
      if (!context) {
        throw new Error(
          'NodeFactoryContext is required for IntervalIntersectNode',
        );
      }
      const fullState: IntervalIntersectNodeState = {
        ...state,
        inputNodes: [],
      };
      return new IntervalIntersectNode(fullState);
    },
    deserialize: (state, _trace, sqlModules) =>
      new IntervalIntersectNode({
        ...IntervalIntersectNode.deserializeState(
          state as IntervalIntersectSerializedState,
        ),
        sqlModules,
      }),
    deserializeConnections: (node, state, allNodes) => {
      const intervalNode = node as IntervalIntersectNode;
      const conns = IntervalIntersectNode.deserializeConnections(
        allNodes,
        state as IntervalIntersectSerializedState,
      );
      intervalNode.secondaryInputs.connections.clear();
      for (let i = 0; i < conns.inputNodes.length; i++) {
        intervalNode.secondaryInputs.connections.set(i, conns.inputNodes[i]);
      }
    },
  });

  nodeRegistry.register('join', {
    name: 'Join',
    description:
      'Join two tables using equality columns or custom SQL condition.',
    icon: 'merge',
    type: 'multisource',
    nodeType: NodeType.kJoin,
    factory: (state) => {
      const fullState: JoinNodeState = {
        ...state,
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
      return new JoinNode(fullState);
    },
    deserialize: (state, _trace, sqlModules) =>
      new JoinNode({
        ...JoinNode.deserializeState(state as JoinSerializedState),
        sqlModules,
      }),
    deserializeConnections: (node, state, allNodes) => {
      const joinNode = node as JoinNode;
      const conns = JoinNode.deserializeConnections(
        allNodes,
        state as JoinSerializedState,
      );
      if (conns.leftNode) {
        joinNode.secondaryInputs.connections.set(0, conns.leftNode);
      }
      if (conns.rightNode) {
        joinNode.secondaryInputs.connections.set(1, conns.rightNode);
      }
    },
    postDeserializeLate: (node) => (node as JoinNode).onPrevNodesUpdated(),
  });

  nodeRegistry.register('create_slices', {
    name: 'Create Slices',
    description:
      'Create slices by pairing start and end timestamps from two sources.',
    icon: 'add_circle',
    type: 'multisource',
    category: 'Time',
    nodeType: NodeType.kCreateSlices,
    factory: (state) => {
      const fullState: CreateSlicesNodeState = {
        ...state,
        startsTsColumn: 'ts',
        endsTsColumn: 'ts',
      };
      return new CreateSlicesNode(fullState);
    },
    deserialize: (state, _trace, sqlModules) =>
      new CreateSlicesNode({
        ...CreateSlicesNode.deserializeState(
          state as CreateSlicesSerializedState,
        ),
        sqlModules,
      }),
    deserializeConnections: (node, state, allNodes) => {
      const createSlicesNode = node as CreateSlicesNode;
      const conns = CreateSlicesNode.deserializeConnections(
        allNodes,
        state as CreateSlicesSerializedState,
      );
      if (conns.startsNode) {
        createSlicesNode.secondaryInputs.connections.set(0, conns.startsNode);
      }
      if (conns.endsNode) {
        createSlicesNode.secondaryInputs.connections.set(1, conns.endsNode);
      }
    },
  });

  nodeRegistry.register('sort_node', {
    name: 'Sort',
    description: 'Sort rows by one or more columns.',
    icon: 'sort',
    type: 'modification',
    nodeType: NodeType.kSort,
    factory: (state) => new SortNode(state as SortNodeState),
    deserialize: (state, _trace, sqlModules) =>
      new SortNode({
        ...SortNode.deserializeState(state as SortNodeState),
        sqlModules,
      }),
  });

  nodeRegistry.register('union_node', {
    name: 'Union',
    description: 'Combine rows from multiple sources.',
    icon: 'merge_type',
    type: 'multisource',
    nodeType: NodeType.kUnion,
    factory: (state) => {
      const fullState: UnionNodeState = {
        ...state,
        inputNodes: [],
        selectedColumns: [],
      };
      const node = new UnionNode(fullState);
      node.onPrevNodesUpdated();
      return node;
    },
    deserialize: (state, _trace, sqlModules) =>
      new UnionNode({
        ...UnionNode.deserializeState(state as UnionSerializedState),
        sqlModules,
      }),
    deserializeConnections: (node, state, allNodes) => {
      const unionNode = node as UnionNode;
      const conns = UnionNode.deserializeConnections(
        allNodes,
        state as UnionSerializedState,
      );
      unionNode.secondaryInputs.connections.clear();
      for (let i = 0; i < conns.inputNodes.length; i++) {
        unionNode.secondaryInputs.connections.set(i, conns.inputNodes[i]);
      }
    },
  });

  nodeRegistry.register('limit_and_offset_node', {
    name: 'Limit and Offset',
    description: 'Limit the number of rows returned and optionally skip rows.',
    icon: Icons.Filter,
    type: 'modification',
    nodeType: NodeType.kLimitAndOffset,
    factory: (state) =>
      new LimitAndOffsetNode(state as LimitAndOffsetNodeState),
    deserialize: (state, _trace, sqlModules) =>
      new LimitAndOffsetNode({
        ...LimitAndOffsetNode.deserializeState(
          state as LimitAndOffsetNodeState,
        ),
        sqlModules,
      }),
  });
}
