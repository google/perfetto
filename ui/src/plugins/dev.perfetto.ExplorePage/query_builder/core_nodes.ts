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
// WITHOUT WARRANTIES OR CONDITIONS OF ANY, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import {nodeRegistry} from './node_registry';
import {SlicesSourceNode} from './nodes/sources/slices_source';
import {
  modalForTableSelection,
  TableSourceNode,
  TableSourceState,
} from './nodes/sources/table_source';
import {SqlSourceNode, SqlSourceState} from './nodes/sources/sql_source';
import {
  TimeRangeSourceNode,
  TimeRangeSourceState,
} from './nodes/sources/timerange_source';
import {AggregationNode, AggregationNodeState} from './nodes/aggregation_node';
import {
  ModifyColumnsNode,
  ModifyColumnsState,
} from './nodes/modify_columns_node';
import {AddColumnsNode, AddColumnsNodeState} from './nodes/add_columns_node';
import {
  FilterDuringNode,
  FilterDuringNodeState,
} from './nodes/filter_during_node';
import {
  IntervalIntersectNode,
  IntervalIntersectNodeState,
} from './nodes/interval_intersect_node';
import {JoinNode, JoinNodeState} from './nodes/join_node';
import {
  CreateSlicesNode,
  CreateSlicesNodeState,
} from './nodes/create_slices_node';
import {SortNode, SortNodeState} from './nodes/sort_node';
import {FilterNode, FilterNodeState} from './nodes/filter_node';
import {UnionNode, UnionNodeState} from './nodes/union_node';
import {
  LimitAndOffsetNode,
  LimitAndOffsetNodeState,
} from './nodes/limit_and_offset_node';
import {Icons} from '../../../base/semantic_icons';

export function registerCoreNodes() {
  nodeRegistry.register('slice', {
    name: 'Slices',
    description: 'Explore all the slices from your trace.',
    icon: 'bar_chart',
    hotkey: 's',
    type: 'source',
    showOnLandingPage: true,
    factory: (state) => new SlicesSourceNode(state),
  });

  nodeRegistry.register('table', {
    name: 'Table',
    description: 'Query and explore data from any table in your trace.',
    icon: 'table_chart',
    hotkey: 't',
    type: 'source',
    showOnLandingPage: true,
    preCreate: async ({sqlModules}) => {
      const selections = await modalForTableSelection(sqlModules);
      if (selections && selections.length > 0) {
        // Return an array of states, one for each selected table
        return selections.map((selection) => ({
          sqlTable: selection.sqlTable,
          sqlModules,
        }));
      }
      return null;
    },
    factory: (state) => new TableSourceNode(state as TableSourceState),
  });

  nodeRegistry.register('sql', {
    name: 'Query',
    description:
      'Start with a custom SQL query to act as a source for further exploration.',
    icon: 'code',
    hotkey: 'q',
    type: 'source',
    showOnLandingPage: true,
    factory: (state) => new SqlSourceNode(state as SqlSourceState),
  });

  nodeRegistry.register('timerange', {
    name: 'Time Range',
    description:
      'Use timeline selection as a source node. Can be dynamic (syncs with timeline) or static (snapshot).',
    icon: 'schedule',
    type: 'source',
    showOnLandingPage: false, // Available in menus but not on landing page
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
  });

  nodeRegistry.register('add_columns', {
    name: 'Add Columns',
    description:
      'Add columns from another node via LEFT JOIN. Connect a node to the left-side port.',
    icon: 'add_box',
    type: 'modification',
    factory: (state) => {
      const fullState: AddColumnsNodeState = {
        ...state,
        selectedColumns: (state as AddColumnsNodeState).selectedColumns ?? [],
        leftColumn: (state as AddColumnsNodeState).leftColumn ?? 'id',
        rightColumn: (state as AddColumnsNodeState).rightColumn ?? 'id',
      };
      return new AddColumnsNode(fullState);
    },
  });

  nodeRegistry.register('modify_columns', {
    name: 'Modify Columns',
    description: 'Select, rename, and add new columns to the data.',
    icon: 'edit',
    type: 'modification',
    factory: (state) => new ModifyColumnsNode(state as ModifyColumnsState),
  });

  nodeRegistry.register('aggregation', {
    name: 'Aggregation',
    description: 'Group and aggregate data from the source node.',
    icon: 'functions',
    type: 'modification',
    factory: (state) => new AggregationNode(state as AggregationNodeState),
  });

  nodeRegistry.register('filter_node', {
    name: 'Filter',
    description: 'Filter rows based on column values.',
    icon: Icons.Filter,
    type: 'modification',
    factory: (state) => new FilterNode(state as FilterNodeState),
  });

  nodeRegistry.register('filter_during', {
    name: 'Filter During',
    description:
      'Filter to only show intervals that occurred during intervals from another source.',
    icon: Icons.Filter,
    type: 'multisource',
    category: 'Time',
    factory: (state) => {
      const fullState: FilterDuringNodeState = {
        ...state,
        filterNegativeDurPrimary:
          (state as FilterDuringNodeState).filterNegativeDurPrimary ?? true,
        filterNegativeDurSecondary:
          (state as FilterDuringNodeState).filterNegativeDurSecondary ?? true,
      };
      return new FilterDuringNode(fullState);
    },
  });

  nodeRegistry.register('interval_intersect', {
    name: 'Interval Intersect',
    description: 'Intersect the intervals with another table.',
    icon: 'timeline',
    type: 'multisource',
    category: 'Time',
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
  });

  nodeRegistry.register('join', {
    name: 'Join',
    description:
      'Join two tables using equality columns or custom SQL condition.',
    icon: 'merge',
    type: 'multisource',
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
  });

  nodeRegistry.register('create_slices', {
    name: 'Create Slices',
    description:
      'Create slices by pairing start and end timestamps from two sources.',
    icon: 'add_circle',
    type: 'multisource',
    category: 'Time',
    factory: (state) => {
      const fullState: CreateSlicesNodeState = {
        ...state,
        startsTsColumn: 'ts',
        endsTsColumn: 'ts',
      };
      return new CreateSlicesNode(fullState);
    },
  });

  nodeRegistry.register('sort_node', {
    name: 'Sort',
    description: 'Sort rows by one or more columns.',
    icon: 'sort',
    type: 'modification',
    factory: (state) => new SortNode(state as SortNodeState),
  });

  nodeRegistry.register('union_node', {
    name: 'Union',
    description: 'Combine rows from multiple sources.',
    icon: 'merge_type',
    type: 'multisource',
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
  });

  nodeRegistry.register('limit_and_offset_node', {
    name: 'Limit and Offset',
    description: 'Limit the number of rows returned and optionally skip rows.',
    icon: Icons.Filter,
    type: 'modification',
    factory: (state) =>
      new LimitAndOffsetNode(state as LimitAndOffsetNodeState),
  });
}
