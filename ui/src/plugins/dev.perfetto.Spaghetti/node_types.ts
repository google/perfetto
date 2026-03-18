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

import {Connection, Label, NodePort} from '../../widgets/nodegraph';
import {FromNodeData} from './from';
import {SelectNodeData} from './select';
import {FilterNodeData} from './filter';
import {SortNodeData} from './sort';
import {LimitNodeData} from './limit';
import {GroupByNodeData} from './groupby';
import {IntervalIntersectNodeData} from './interval_intersect';
import {SelectionNodeData} from './selection';
import {UnionAllNodeData} from './union_all';
import {ExtendNodeData} from './extend';
import {ExtractArgNodeData} from './extract_arg';

export {FromNodeData} from './from';
export {SelectNodeData} from './select';
export {FilterNodeData} from './filter';
export {SortNodeData} from './sort';
export {LimitNodeData} from './limit';
export {GroupByNodeData} from './groupby';
export {IntervalIntersectNodeData} from './interval_intersect';
export {SelectionNodeData} from './selection';
export {UnionAllNodeData} from './union_all';
export {ExtendNodeData} from './extend';
export {ExtractArgNodeData} from './extract_arg';

export interface BaseNodeData {
  readonly id: string;
  x: number;
  y: number;
  nextId?: string;
  collapsed?: boolean;
}

export type NodeData =
  | FromNodeData
  | SelectNodeData
  | FilterNodeData
  | SortNodeData
  | LimitNodeData
  | GroupByNodeData
  | IntervalIntersectNodeData
  | SelectionNodeData
  | UnionAllNodeData
  | ExtendNodeData
  | ExtractArgNodeData;

export interface NodeQueryBuilderStore {
  readonly nodes: Map<string, NodeData>;
  readonly connections: Connection[];
  readonly labels: Label[];
}

export interface NodeConfig {
  readonly title: string;
  readonly icon?: string;
  readonly inputs?: ReadonlyArray<NodePort>;
  readonly outputs?: ReadonlyArray<NodePort>;
  readonly canDockTop?: boolean;
  readonly canDockBottom?: boolean;
  readonly hue: number;
}

export const NODE_CONFIGS: Record<NodeData['type'], NodeConfig> = {
  from: {
    title: 'From',
    icon: 'table_chart',
    outputs: [{content: 'Output', direction: 'right'}],
    canDockBottom: true,
    hue: 210,
  },
  select: {
    title: 'Select',
    icon: 'view_column',
    inputs: [{content: 'Input', direction: 'left'}],
    outputs: [{content: 'Output', direction: 'right'}],
    canDockTop: true,
    canDockBottom: true,
    hue: 145,
  },
  filter: {
    title: 'Filter',
    icon: 'filter_alt',
    inputs: [{content: 'Input', direction: 'left'}],
    outputs: [{content: 'Output', direction: 'right'}],
    canDockTop: true,
    canDockBottom: true,
    hue: 35,
  },
  sort: {
    title: 'Sort',
    icon: 'sort',
    inputs: [{content: 'Input', direction: 'left'}],
    outputs: [{content: 'Output', direction: 'right'}],
    canDockTop: true,
    canDockBottom: true,
    hue: 178,
  },
  limit: {
    title: 'Limit',
    icon: 'horizontal_rule',
    inputs: [{content: 'Input', direction: 'left'}],
    outputs: [{content: 'Output', direction: 'right'}],
    canDockTop: true,
    canDockBottom: true,
    hue: 60,
  },
  groupby: {
    title: 'Group By',
    icon: 'workspaces',
    inputs: [{content: 'Input', direction: 'left'}],
    outputs: [{content: 'Output', direction: 'right'}],
    canDockTop: true,
    canDockBottom: true,
    hue: 275,
  },
  interval_intersect: {
    title: 'Interval Intersect',
    icon: 'compare_arrows',
    inputs: [
      {content: 'Input 1', direction: 'left'},
      {content: 'Input 2', direction: 'left'},
    ],
    outputs: [{content: 'Output', direction: 'right'}],
    canDockTop: true,
    canDockBottom: true,
    hue: 340,
  },
  selection: {
    title: 'Time Range',
    icon: 'highlight_alt',
    outputs: [{content: 'Output', direction: 'right'}],
    canDockBottom: true,
    hue: 15,
  },
  union_all: {
    title: 'Union',
    icon: 'merge',
    inputs: [
      {content: 'Input 1', direction: 'left'},
      {content: 'Input 2', direction: 'left'},
    ],
    outputs: [{content: 'Output', direction: 'right'}],
    canDockTop: true,
    canDockBottom: true,
    hue: 242,
  },
  extract_arg: {
    title: 'Extract Arg',
    icon: 'data_object',
    inputs: [{content: 'Input', direction: 'left'}],
    outputs: [{content: 'Output', direction: 'right'}],
    canDockTop: true,
    canDockBottom: true,
    hue: 95,
  },
  extend: {
    title: 'Join',
    icon: 'add_circle',
    inputs: [
      {content: 'Left', direction: 'left'},
      {content: 'Right', direction: 'left'},
    ],
    outputs: [{content: 'Output', direction: 'right'}],
    canDockTop: true,
    canDockBottom: true,
    hue: 308,
  },
};
