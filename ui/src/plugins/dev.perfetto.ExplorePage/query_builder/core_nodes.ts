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
import {AggregationNode, AggregationNodeState} from './nodes/aggregation_node';
import {
  ModifyColumnsNode,
  ModifyColumnsState,
} from './nodes/modify_columns_node';
import {
  IntervalIntersectNode,
  IntervalIntersectNodeState,
} from './nodes/interval_intersect_node';

export function registerCoreNodes() {
  nodeRegistry.register('slice', {
    name: 'Slices',
    description: 'Explore all the slices from your trace.',
    icon: 'bar_chart',
    hotkey: 's',
    type: 'source',
    factory: (state) => new SlicesSourceNode(state),
  });

  nodeRegistry.register('table', {
    name: 'Perfetto Table',
    description:
      'Query and explore data from any table in the Perfetto standard library.',
    icon: 'table_chart',
    hotkey: 't',
    type: 'source',
    preCreate: async ({sqlModules}) => {
      const selection = await modalForTableSelection(sqlModules);
      if (selection) {
        return {
          sqlTable: selection.sqlTable,
          sqlModules,
        };
      }
      return null;
    },
    factory: (state) => new TableSourceNode(state as TableSourceState),
  });

  nodeRegistry.register('sql', {
    name: 'Query Node',
    description:
      'Start with a custom SQL query to act as a source for further exploration.',
    icon: 'code',
    hotkey: 'q',
    type: 'source',
    factory: (state) => new SqlSourceNode(state as SqlSourceState),
  });

  nodeRegistry.register('aggregation', {
    name: 'Aggregation',
    description: 'Group and aggregate data from the source node.',
    icon: 'functions',
    type: 'modification',
    factory: (state) => new AggregationNode(state as AggregationNodeState),
  });

  nodeRegistry.register('modify_columns', {
    name: 'Modify Columns',
    description: 'Select, rename, and add new columns to the data.',
    icon: 'edit',
    type: 'modification',
    factory: (state) => new ModifyColumnsNode(state as ModifyColumnsState),
  });

  nodeRegistry.register('interval_intersect', {
    name: 'Interval Intersect',
    description: 'Intersect the intervals with another table.',
    icon: 'timeline',
    type: 'multisource',
    factory: (state, context) => {
      if (!context) {
        throw new Error(
          'NodeFactoryContext is required for IntervalIntersectNode',
        );
      }
      const fullState: IntervalIntersectNodeState = {
        ...state,
        prevNodes: state.prevNodes ?? [],
        allNodes: context.allNodes,
      };
      return new IntervalIntersectNode(fullState);
    },
  });
}
