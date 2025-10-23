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
import {
  AddColumnsNode,
  AddColumnsNodeState,
} from './nodes/dev/add_columns_node';
import {modalForTableSelection} from './nodes/sources/table_source';
import {TestNode} from './nodes/dev/test_node';
import {
  LimitAndOffsetNode,
  LimitAndOffsetNodeState,
} from './nodes/dev/limit_and_offset_node';
import {SortNode, SortNodeState} from './nodes/dev/sort_node';
import {UnionNode, UnionNodeState} from './nodes/dev/union_node';

export function registerDevNodes() {
  nodeRegistry.register('test_source', {
    name: 'Test Source',
    description: 'A source for testing purposes.',
    icon: 'bug_report',
    type: 'source',
    factory: (state) => new TestNode(state),
    devOnly: true,
  });

  nodeRegistry.register('add_columns_node', {
    name: 'Add Columns',
    description: 'Adds new columns.',
    icon: 'add_box',
    type: 'modification',
    factory: (state) => new AddColumnsNode(state as AddColumnsNodeState),
    preCreate: async ({sqlModules}) => {
      const table = await modalForTableSelection(sqlModules);
      if (table === undefined) {
        return null;
      }
      return {
        sqlTable: table.sqlTable,
      };
    },
    devOnly: true,
  });

  nodeRegistry.register('limit_and_offset_node', {
    name: 'Limit and Offset',
    description: 'Limits number of rows and offsets them.',
    icon: 'filter_list',
    type: 'modification',
    factory: (state) =>
      new LimitAndOffsetNode(state as LimitAndOffsetNodeState),
    devOnly: true,
  });

  nodeRegistry.register('sort_node', {
    name: 'Sort',
    description: 'Sorts by a column.',
    icon: 'sort',
    type: 'modification',
    factory: (state) => new SortNode(state as SortNodeState),
    devOnly: true,
  });

  nodeRegistry.register('union_node', {
    name: 'Union',
    description: 'Union multiple sources.',
    icon: 'merge_type',
    type: 'multisource',
    factory: (state) => {
      const fullState: UnionNodeState = {
        ...state,
        prevNodes: state.prevNodes ?? [],
        selectedColumns: [],
      };
      const node = new UnionNode(fullState);
      node.onPrevNodesUpdated();
      return node;
    },
    devOnly: true,
  });
}
