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

// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

import {Selection} from 'd3';
import {ZeroDurationNode, DurationNode, MarkNode, InstanceNode} from './node';
import {TTraceEvent} from '../types';

export interface IChartContainer {
  node: HTMLDivElement;
  selection: Selection<HTMLDivElement, unknown, null, undefined>;
}
export type TBaseNodeData = {
  group: string | number;
  thread?: string | number;
  name: string;
  ts: number;
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  _internal: Record<string, any> & {
    raw: TTraceEvent[];
  };
};

export type TInstanceNodeData = TBaseNodeData;
export type TMarkNodeData = TBaseNodeData;
export type TZeroDurationNodeData = TBaseNodeData;
export type TDurationNodeData = TBaseNodeData & {
  dur: number;
};

export type TNodeData =
  | TInstanceNodeData
  | TMarkNodeData
  | TZeroDurationNodeData
  | TDurationNodeData;

export type TNode = ZeroDurationNode | InstanceNode | DurationNode | MarkNode;

export enum ENodeType {
  ZeroDuration = 'zeroDuration',
  Instance = 'instance',
  Duration = 'duration',
  Mark = 'mark',
}
