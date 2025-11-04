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

import {IChartConfig} from './types';
import {
  ENodeType,
  TDurationNodeData,
  TInstanceNodeData,
  TMarkNodeData,
  TZeroDurationNodeData,
} from '../chart/types';
import {formatTime} from '../utils';
export * from './types';

export const defaultConfig: IChartConfig = {
  basis: 0,
  label: {
    padding: 5,
    fontSize: 12,
    fontFamily: 'arial,sans-serif',
  },
  scale: [1, 10000],
  node: {
    height: 20,
    margin: 2,
  },
  xAxis: {
    valueFormatter: (val, _idx) => val.toString(),
    style: (_text) => {},
  },
  tooltip: {
    nodeFormatter: (val, _config) => {
      const {nodeData, nodeType} = val;
      let argsTip = '';
      if (nodeData._internal && nodeData._internal.raw && nodeData._internal.raw.length > 0) {
        const args = nodeData._internal.raw[0].args;
        for(const [key,value] of Object.entries(args ?? {})) {
          argsTip += `<br>${key}: ${value}`;
        }
      }

      if (nodeType === ENodeType.ZeroDuration) {
        const {name, ts: start} = nodeData as TZeroDurationNodeData;
        return `<b style="white-space:pre-line;word-wrap:break-word;">${name}</b><br>start: ${formatTime(
          start,
        )} ms<br>duration: 0 ms${argsTip}`;
      } else if (
        nodeType === ENodeType.Instance ||
        nodeType === ENodeType.Mark
      ) {
        const {name, ts} = nodeData as TInstanceNodeData | TMarkNodeData;
        return `<b style="white-space:pre-line;word-wrap:break-word;">${name}</b><br>timing: ${formatTime(
          ts,
        )} ms${argsTip}`;
      } else if (nodeType === ENodeType.Duration) {
        const {name, ts: start, dur} = nodeData as TDurationNodeData;
        return `<b style="white-space:pre-line;word-wrap:break-word;">${name}</b><br>start: ${formatTime(
          start,
        )} ms<br>duration: ${formatTime(dur)} ms${argsTip}`;
      }
      return `${JSON.stringify(nodeData, null, 4)}`;
    },
  },
};

export function getFontContent(config: IChartConfig) {
  return `${config.label?.fontSize}px ${config.label?.fontFamily}`;
}
